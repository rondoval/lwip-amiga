/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev TX path: pbuf chains submitted to the driver as scatter-gather
 * descriptors with L4-checksum offload preparation, the L2 header cache
 * that lets the IP output hot path bypass etharp/ethernet_output, and the
 * TX-done reclaim. linkoutput runs in whatever task called into lwIP,
 * always under the netstack core lock.
 */

#include "netstack_sys.h"

#include <lwip/etharp.h>
#include <lwip/prot/ip.h>
#include <lwip/snmp.h>

#include "netdev_priv.h"
#include "netstack.h"
#include "nsprof.h"

/* scatter-list bound; pathological chains are coalesced down to it */
#define NDIF_MAX_STACK_SEGS 16

void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count)
{
    (void)stackctx;

    PERF_T0(t_done);
    netstack_lock();
    for (ULONG i = 0; i < count; i++)
        pbuf_free((struct pbuf *)cookies[i]);
    PERF_ADD(&ns_perf, NSP_TX_DONE, t_done);
    netstack_unlock();
}

/* --------------------------------------------------- L2 header cache --- */

/* Fast hits between slow-path revalidations of a header-cache entry: caps
 * both the residual etharp cost (once per N frames) and the recovery time
 * after an ARP/MAC or DHCP netmask/gateway change the cache cannot see
 * (at most N frames to a stale MAC, ~3 ms at line rate, then re-primed). */
#define NDIF_HH_REVALIDATE 64u

/* Snoop-prime: called from ndif_linkoutput while ndi_HhPrimeDst is set —
 * i.e. exactly for frames built by the slow etharp_output path. Whatever
 * L2 header ethernet_output actually emitted for this destination (VLAN
 * tag, gateway MAC substitution and multicast mapping included) is copied
 * into the cache verbatim; ARP requests and ARP-queue flushes for other
 * destinations are filtered by the ethertype/embedded-destination checks.
 * Runs under the core lock, like every linkoutput. */
static void ndif_hh_prime(struct NetdevIf *ndi, const struct pbuf *p)
{
    const UBYTE *frame = p->payload;
    ULONG l3 = ndif_ip_offset(frame, p->len);
    if (l3 == 0)
        return; /* not IPv4 (an ARP request, say) — nothing to cache */

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    ULONG dst = ip4_addr_get_u32(&ip->dest);
    if (dst != ndi->ndi_HhPrimeDst)
        return; /* some other frame (ARP-queue flush) — keep waiting */

    struct NdHhEntry *e = &ndi->ndi_Hh[dst & (NDIF_HH_ENTRIES - 1)];
    e->nhh_DstIp = dst;
    e->nhh_Len = (UWORD)l3; /* the L2 header is everything before IP */
    e->nhh_Left = NDIF_HH_REVALIDATE;
    for (ULONG i = 0; i < l3; i++)
        e->nhh_Hdr[i] = frame[i];
    ndi->ndi_HhPrimeDst = 0;
}

/* ---------------------------------------------------- checksum offload --- */

/* TX offload preparation: seed the L4 checksum field with the pseudo-header
 * sum (CHECKSUM_PARTIAL style) and compute the offload offsets. Returns the
 * L4 protocol (IP_PROTO_TCP/UDP), or 0xFFFF when the frame is not
 * offloadable (non-IP, fragments, other protocols). */
static ULONG ndif_l4_offsets(struct pbuf *p, UWORD *csum_start, UWORD *csum_offset)
{
    /* headers (incl. any VLAN tag) must be contiguous in the first pbuf */
    ULONG l3 = ndif_ip_offset(p->payload, p->len);
    if (l3 == 0)
        return 0xFFFF; /* not IPv4 (or non-contiguous): not offloadable */

    struct ip_hdr *ip = (struct ip_hdr *)((UBYTE *)p->payload + l3);
    if ((IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0)
        return 0xFFFF; /* fragment: no L4 header / partial coverage */

    ULONG ihl = (ULONG)IPH_HL(ip) * 4;
    ULONG l4_start = l3 + ihl;
    ULONG proto = IPH_PROTO(ip);
    ULONG field;

    switch (proto)
    {
    case IP_PROTO_TCP:
        field = 16; /* offsetof tcp checksum */
        break;
    case IP_PROTO_UDP:
        field = 6;
        break;
    default:
        return 0xFFFF;
    }

    UWORD *csum_field = (UWORD *)((UBYTE *)p->payload + l4_start + field);
    *csum_field = ndif_pseudo_sum(ip, 0);

    *csum_start = (UWORD)l4_start;
    *csum_offset = (UWORD)(l4_start + field);
    return proto;
}

/* ------------------------------------------------------------- submit --- */

err_t ndif_linkoutput(struct netif *nif, struct pbuf *p)
{
    struct NetdevIf *ndi = nif->state;
    PERF_T0(t_out);

    /* a slow-path ndif_ip4_output send is in flight: capture its header */
    if (ndi->ndi_HhPrimeDst != 0)
        ndif_hh_prime(ndi, p);

    /* Bound the scatter list; coalesce pathological chains. */
    ULONG max_segs = ndi->ndi_Caps.ndc_TxMaxSegs;
    if (max_segs > NDIF_MAX_STACK_SEGS)
        max_segs = NDIF_MAX_STACK_SEGS;

    if (pbuf_clen(p) > max_segs)
    {
        p = pbuf_coalesce(p, PBUF_RAW);
        if (pbuf_clen(p) > max_segs)
        {
            ndi->ndi_TxOversize++;
            return ERR_IF;
        }
    }

    /* Zero-copy aliasing guard. A ref beyond the owner's single hold means a
     * previous transmission of this very pbuf is still armed in the TX ring
     * (our in-flight ref is the extra one). lwIP rewrites TCP headers of
     * queued segments IN PLACE on retransmit (ackno/wnd, plus our checksum
     * seed below) — submitting the same memory again would let the NIC
     * DMA-read a torn header and checksum it into wire-validity, which the
     * peer answers with RST. Transmit a private copy instead; only the rare
     * retransmit-while-armed pays it (Linux clones on retransmit for the
     * same reason). */
    struct pbuf *frame = p;
    BOOL cloned = FALSE;
    if (p->ref > 1)
    {
        frame = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (frame == NULL)
            return ERR_MEM; /* retry later; the armed copy stays intact */
        cloned = TRUE;
    }

    struct NetDevSg segs[NDIF_MAX_STACK_SEGS];
    UWORD nsegs = 0;
    for (struct pbuf *q = frame; q != NULL; q = q->next)
    {
        segs[nsegs].nsg_Data = q->payload;
        segs[nsegs].nsg_Len = q->len;
        nsegs++;
    }

    struct NetDevTxDesc desc;
    desc.ntd_Segs = segs;
    desc.ntd_NumSegs = nsegs;
    desc.ntd_Flags = 0;
    desc.ntd_CsumStart = 0;
    desc.ntd_CsumOffset = 0;
    desc.ntd_Cookie = frame;

    if (ndi->ndi_Caps.ndc_Features & NDCF_TX_L4CSUM)
    {
        ULONG proto = ndif_l4_offsets(frame, &desc.ntd_CsumStart, &desc.ntd_CsumOffset);
        if (proto != 0xFFFF)
        {
            desc.ntd_Flags |= NDTF_L4CSUM;
            if (proto == IP_PROTO_UDP)
                desc.ntd_Flags |= NDTF_L4_UDP;
        }
        /* not offloadable: per-netif GEN switches are only cleared for
         * TCP/UDP, so anything else was checksummed by lwIP already */
    }

    /* The driver owns the frame until nso_TxDone; lwIP may free its
     * reference right after we return. A clone is born with the one ref
     * that nso_TxDone's pbuf_free consumes. */
    if (!cloned)
        pbuf_ref(frame);

    PERF_T0(t_submit);
    LONG accepted = ndi->ndi_Ops->ndo_TxSubmit(ndi->ndi_Drv, &desc, 1);
    PERF_ADD(&ns_perf, NSP_TX_SUBMIT, t_submit);
    if (accepted != 1)
    {
        pbuf_free(frame);
        return ERR_MEM; /* ring full; TCP retries on timer */
    }

    PERF_ADD(&ns_perf, NSP_TX_LINKOUT, t_out);
    return ERR_OK;
}

/* netif->output — the TX hot path for every IP frame (TCP segments, UDP
 * datagrams and their fragments). On a header-cache hit the prebuilt L2
 * header is prepended and the frame goes straight to linkoutput,
 * bypassing etharp_output/ethernet_output. Misses and the periodic
 * revalidation take the slow path, which snoop-primes the cache in
 * ndif_linkoutput. MIB2 netif counters are maintained to match
 * ethernet_output. */
err_t ndif_ip4_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    struct NetdevIf *ndi = nif->state;
    ULONG dst = ip4_addr_get_u32(ipaddr);
    struct NdHhEntry *e = &ndi->ndi_Hh[dst & (NDIF_HH_ENTRIES - 1)];
    err_t err;

    if (e->nhh_DstIp == dst && e->nhh_Left != 0 &&
        pbuf_add_header(p, e->nhh_Len) == 0)
    {
        e->nhh_Left--;
        UBYTE *out = p->payload;
        for (ULONG i = 0; i < e->nhh_Len; i++)
            out[i] = e->nhh_Hdr[i];

        MIB2_STATS_NETIF_ADD(nif, ifoutoctets, p->tot_len);
        if (e->nhh_Hdr[0] & 1) /* group bit: broadcast/multicast dst MAC */
            MIB2_STATS_NETIF_INC(nif, ifoutnucastpkts);
        else
            MIB2_STATS_NETIF_INC(nif, ifoutucastpkts);

        err = ndif_linkoutput(nif, p);
    }
    else
    {
        /* slow path: ARP/ethernet build the header, linkoutput snoops it */
        ndi->ndi_HhPrimeDst = dst;
        err = etharp_output(nif, p, ipaddr);
        ndi->ndi_HhPrimeDst = 0;
    }

    return err;
}
