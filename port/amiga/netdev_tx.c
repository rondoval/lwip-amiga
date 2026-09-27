/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev TX path: pbuf chains submitted to the driver as scatter-gather
 * descriptors with L4-checksum offload preparation, the L2 header cache
 * that lets the IP output hot path bypass etharp/ethernet_output, and the
 * TX-done reclaim.
 *
 * In the order a frame travels, which is also the order of this file:
 *
 *   L2 header cache    — the prebuilt header the fast path prepends,
 *   checksum offload   — pseudo-header seed and the offload offsets,
 *   submit             — descriptor build, one handoff, the pending queue,
 *   output             — the two netif entry points lwIP calls,
 *   doorbell           — publish a staged burst, at the outermost unlock,
 *   completion         — the driver's TX-done, reclaimed under the next hold.
 *
 * Everything down to the doorbell runs in whatever task called into lwIP,
 * always under the netstack core lock. The completion callback is the
 * exception: it runs on the driver's unit task and stays lock-free.
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
    ULONG l3 = inetfrm_ip_offset(frame, p->len);
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
    ULONG l3 = inetfrm_ip_offset(p->payload, p->len);
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
    *csum_field = inetfrm_pseudo_sum(ip, 0);

    *csum_start = (UWORD)l4_start;
    *csum_offset = (UWORD)(l4_start + field);
    return proto;
}

/* ------------------------------------------------------------- submit --- */
/* One frame's journey into the driver: build its descriptor, hand it over, and
 * — when the ring will not take it — park it on the pending queue that
 * netdevif_tx_reclaim drains as room appears. */

/* Build the driver's descriptor for @frame: one scatter entry per pbuf, L4
 * checksum offload where the frame qualifies. @segs is the caller's storage. */
static void ndif_tx_desc(struct NetdevIf *ndi, struct pbuf *frame,
                         struct NetDevSg *segs, struct NetDevTxDesc *desc)
{
    UWORD nsegs = 0;
    for (struct pbuf *q = frame; q != NULL; q = q->next)
    {
        segs[nsegs].nsg_Data = q->payload;
        segs[nsegs].nsg_Len = q->len;
        nsegs++;
    }
    desc->ntd_Segs = segs;
    desc->ntd_NumSegs = nsegs;
    desc->ntd_Flags = 0;
    desc->ntd_CsumStart = 0;
    desc->ntd_CsumOffset = 0;
    desc->ntd_Cookie = frame;

    if (ndi->ndi_Caps.ndc_Features & NDCF_TX_L4CSUM)
    {
        ULONG proto = ndif_l4_offsets(frame, &desc->ntd_CsumStart, &desc->ntd_CsumOffset);
        if (proto != 0xFFFF)
        {
            desc->ntd_Flags |= NDTF_L4CSUM;
            if (proto == IP_PROTO_UDP)
                desc->ntd_Flags |= NDTF_L4_UDP;
        }
        /* not offloadable: per-netif GEN switches are only cleared for
         * TCP/UDP, so anything else was checksummed by lwIP already */
    }
}

/* Hand one frame (already ref'd for the driver) to the ring. TRUE: staged,
 * doorbell deferred to netdevif_tx_kick so a whole burst rings once. FALSE:
 * the ring had no room and the frame is untouched. */
static BOOL ndif_tx_submit(struct NetdevIf *ndi, struct pbuf *frame)
{
    struct NetDevSg segs[NDIF_MAX_STACK_SEGS];
    struct NetDevTxDesc desc;
    ndif_tx_desc(ndi, frame, segs, &desc);

    PERF_T0(t_submit);
    LONG accepted = ndi->ndi_Ops->ndo_TxSubmit(ndi->ndi_Drv, &desc, 1);
    PERF_ADD(&ns_perf, NSP_TX_SUBMIT, t_submit);
    if (accepted != 1)
        return FALSE;
    ndi->ndi_TxKickPending = TRUE;
    return TRUE;
}

/* Resubmit pending frames in order while the ring takes them; each one that
 * leaves the queue frees a slot for admission. */
static void ndif_tx_pend_drain(struct NetdevIf *ndi)
{
    while (ndi->ndi_TxPendHead != ndi->ndi_TxPendTail)
    {
        struct pbuf *frame = ndi->ndi_TxPend[ndi->ndi_TxPendHead & ndi->ndi_TxPendMask];
        if (!ndif_tx_submit(ndi, frame))
            break;
        ndi->ndi_TxPendHead++;
        netifbase_tx_freed(&ndi->ndi_Base, 1);
    }
}

/* ------------------------------------------------------------- output --- */
/* The two entry points lwIP calls: netif->linkoutput for a frame that already
 * carries its L2 header, netif->output for an IP packet that still needs one.
 * linkoutput is defined first because the fast path in ndif_ip4_output calls
 * it directly. */

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
            return ERR_IF; /* shape error, not capacity: nothing to retry */
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

    /* The driver owns the frame until nso_TxDone; lwIP may free its
     * reference right after we return. A clone is born with the one ref
     * that nso_TxDone's pbuf_free consumes. */
    if (!cloned)
        pbuf_ref(frame);

    /* Behind frames already waiting, or refused by a full ring, the frame
     * waits its turn instead of being dropped: lwIP would not notice a drop
     * (ip4_frag ignores a failed fragment) and a datagram would go out short.
     * Admission (netifbase_tx_admit) keeps the queue from filling for any
     * datagram the socket layer let through; lwIP's own senders (ARP, DHCP)
     * can still meet a full one. */
    if (ndi->ndi_TxPendHead != ndi->ndi_TxPendTail || !ndif_tx_submit(ndi, frame))
    {
        if (ndi->ndi_TxPendTail - ndi->ndi_TxPendHead > ndi->ndi_TxPendMask)
        {
            pbuf_free(frame);
            return ERR_MEM;
        }
        ndi->ndi_TxPend[ndi->ndi_TxPendTail++ & ndi->ndi_TxPendMask] = frame;
        netifbase_tx_taken(&ndi->ndi_Base, 1);
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

/* ----------------------------------------------------------- doorbell --- */

/* Publish a staged TX burst: called at every outermost netstack_unlock (and as
 * the STOP backstop). A no-op unless ndif_linkoutput staged frames since the
 * last kick — the common case for the RX/tick/tx-done unlock callers. */
void netdevif_tx_kick(struct NetdevIf *ndi)
{
    if (ndi != NULL && ndi->ndi_TxKickPending)
    {
        ndi->ndi_Ops->ndo_TxKick(ndi->ndi_Drv);
        ndi->ndi_TxKickPending = FALSE;
    }
}

/* --------------------------------------------------------- completion --- */
/* The far end of a submit, and the only group here that does NOT run under the
 * core lock: nso_TxDone is the driver's unit task. It stages, the next
 * lock holder frees (netdevif_tx_reclaim, from the outermost netstack_lock). */

/* nso_TxDone: the driver's unit task hands up completed TX cookies (the pbufs
 * transmitted, one per ndo_TxSubmit). Rather than take the core lock here and
 * ping-pong it with the app sender, stage the cookies on the lock-free reclaim
 * ring; netdevif_tx_reclaim frees them under the next hold that already owns the
 * lock. stackctx is the struct NetdevIf * (nda_StackCtx). Single producer (this
 * unit task, the only nso_* caller), so the enqueue needs no lock and never
 * blocks the unit task on the app's hold. */
void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count)
{
    struct NetdevIf *ndi = stackctx;
    ULONG prod = ndi->ndi_TxFreeProd;
    ULONG cons = ndi->ndi_TxFreeCons; /* snapshot; a stale (older) value only
                                         makes the fullness test conservative */

    if ((ULONG)(prod - cons) + count > ndi->ndi_TxFreeMask + 1)
    {
        /* Backstop: the ring is sized above the driver's in-flight ceiling, so
         * this cannot trip at current constants — free inline so a cookie is
         * never leaked should that sizing ever be invalidated. */
        netstack_lock();
        for (ULONG i = 0; i < count; i++)
            pbuf_free((struct pbuf *)cookies[i]);
        netstack_unlock();
        return;
    }

    for (ULONG i = 0; i < count; i++)
        ndi->ndi_TxFree[prod++ & ndi->ndi_TxFreeMask] = cookies[i];
    asm volatile("" ::: "memory"); /* publish the cookies before the index */
    ndi->ndi_TxFreeProd = prod;

    /* A sender blocked on transmit room (netifbase_tx_admit) with frames in
     * the pending queue has nobody else to take the lock for it: do it here,
     * once per completion batch and only in that state (both reads are
     * unlocked snapshots; a stale one costs one more batch). The obtain runs
     * netdevif_tx_reclaim - resubmit and the space wake - and the release
     * rings the doorbell. */
    if (netstack.ns_TxWantSpace && ndi->ndi_TxPendHead != ndi->ndi_TxPendTail)
    {
        netstack_lock();
        netstack_unlock();
    }
}

/* Free the completed TX cookies ndif_tx_done staged since the last drain. Runs
 * under the core lock (the outermost netstack_lock, and netdevif_destroy), so
 * the pbuf_free work folds into a hold that already exists instead of the unit
 * task taking a contended lock per completion batch. Snapshot-drain-commit,
 * mirroring the driver's netdev_drain_recycle. */
void netdevif_tx_reclaim(struct NetdevIf *ndi)
{
    if (ndi == NULL)
        return;

    ULONG cons = ndi->ndi_TxFreeCons;
    ULONG prod = ndi->ndi_TxFreeProd; /* snapshot bounds this pass */
    if (cons != prod)
    {
        PERF_T0(t_done);
        while (cons != prod)
        {
            pbuf_free((struct pbuf *)ndi->ndi_TxFree[cons & ndi->ndi_TxFreeMask]);
            cons++;
        }
        asm volatile("" ::: "memory"); /* commit the frees before releasing slots */
        ndi->ndi_TxFreeCons = cons;
        PERF_ADD(&ns_perf, NSP_TX_DONE, t_done);
    }
    /* Ring room comes back ahead of the cookies (the driver's consumer index
     * moves before its completion FIFO does), so the queue is tried whenever
     * it holds anything, not only after a completion. */
    if (ndi->ndi_TxPendHead != ndi->ndi_TxPendTail)
        ndif_tx_pend_drain(ndi);
}

void netdevif_tx_pend_free(struct NetdevIf *ndi)
{
    while (ndi->ndi_TxPendHead != ndi->ndi_TxPendTail)
        pbuf_free((struct pbuf *)ndi->ndi_TxPend[ndi->ndi_TxPendHead++ & ndi->ndi_TxPendMask]);
}
