/* SPDX-License-Identifier: BSD-3-Clause */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/etharp.h>
#include <lwip/inet_chksum.h>
#include <lwip/prot/ethernet.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/ip4.h>
#include <lwip/prot/tcp.h>
#include <lwip/prot/udp.h>
#include <lwip/snmp.h>
#include <netif/ethernet.h>

#include "netdev_if.h"
#include "netstack.h"
#include "netstack_lwiphooks.h"
#include "nsprof.h"

/* One RX buffer in flight to lwIP: a custom pbuf wrapping driver memory.
 * Freeing the pbuf recycles the driver buffer. Wrappers live in a free
 * list mutated only under the core lock. */
struct NdRxWrap
{
    struct pbuf_custom nrw_Pc; /* must stay first */
    APTR nrw_Cookie;
    struct NetdevIf *nrw_If;
    struct NdRxWrap *nrw_Next;
};

#define NDIF_MAX_STACK_SEGS 16
#define NDIF_MIN_WRAPS      64

/* Fairness yield stride: during an unflow-controlled RX flood (a UDP blast at
 * line rate) the unit task processes every frame under ns_Core while the app
 * task queues on the lock to drain its socket — starving it, so its recv queue
 * overflows and ~everything is dropped past the app. Every this-many frames,
 * IF a task is actually queued on ns_Core, hand the FIFO lock over so the app
 * drains (and frees pool buffers) before we fill its queue with drops. Gated on
 * ss_QueueCount, so the uncontended fast path never pays. Tunable: smaller =
 * fairer to the app but more lock handoffs (2 context switches each). */
#define NDIF_RX_YIELD_STRIDE 8u

static BOOL ndif_rx_csum_ok(const struct NetDevRxDesc *d, ULONG raw);

/* GRO-lite (defined after ndif_ip_offset, used by ndif_rx_input) */
static void ndif_gro_classify(const struct NetDevRxDesc *d, struct NdGroMeta *m);
static void ndif_gro_rx(struct NetdevIf *ndi, struct pbuf *p,
                        const struct NetDevRxDesc *d, const struct NdGroMeta *m);
static void ndif_gro_flush_all(struct NetdevIf *ndi);

/* ---------------------------------------------------------------- RX --- */

static void ndif_rx_pbuf_freed(struct pbuf *p)
{
    // KprintfH("[netdevif] %s: pbuf 0x%08lx\n", __func__, (ULONG)p);
    struct NdRxWrap *w = (struct NdRxWrap *)p;
    struct NetdevIf *ndi = w->nrw_If;

    ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, w->nrw_Cookie);

    w->nrw_Next = ndi->ndi_FreeWraps;
    ndi->ndi_FreeWraps = w;
}

static ULONG ndif_rx_input(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count)
{
    // KprintfH("[netdevif] %s: count %lu\n", __func__, count);
    struct NetdevIf *ndi = stackctx;
    ULONG consumed = 0;
    ULONG since_yield = 0;
    /* GRO rides on the RX csum offload: with lwIP's own TCP checksum check
     * active, a merged (rewritten) header would fail re-verification */
    BOOL gro = ndi->ndi_RxOffload;

    while (consumed < count)
    {
        const struct NetDevRxDesc *cd = &descs[consumed];
        ULONG chunk = count - consumed;
        if (chunk > NDIF_RX_CHUNK)
            chunk = NDIF_RX_CHUNK;

        /* Checksum verdicts + GRO classification BEFORE taking the core
         * lock — both read only frame bytes, and nso_RxInput runs on the
         * driver's unit task alone, so nothing here needs protection.
         * lwIP's TCP/UDP checking is off when the driver offloads RX csum;
         * frames without the VALID verdict get their RAW sum folded here. */
        UBYTE drop[NDIF_RX_CHUNK];
        PERF_T0(t_csum);
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];
            drop[i] = ndi->ndi_RxOffload &&
                      !(d->nrd_Flags & NDRF_CSUM_VALID) &&
                      (d->nrd_Flags & NDRF_CSUM_RAW) &&
                      !ndif_rx_csum_ok(d, d->nrd_CsumRaw);
            if (gro)
                ndif_gro_classify(d, &ndi->ndi_GroMeta[i]);
        }
        PERF_ADD(&ns_perf, NSP_RX_CSUM, t_csum);

        PERF_T0(t_lock);
        netstack_lock();
        PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_lock);
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];

            /* a bad frame is consumed and its buffer released immediately
             * (releases stay under the lock: it is the recycle ring's
             * single-producer guarantee). Its headers are untrustworthy,
             * so any held merge run is flushed — a mid-run drop is a
             * sequence discontinuity. */
            if (drop[i])
            {
                ndif_gro_flush_all(ndi);
                ndi->ndi_RxCsumBad++;
                ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, d->nrd_Cookie);
                consumed++;
                continue;
            }

            struct NdRxWrap *w = ndi->ndi_FreeWraps;
            if (w == NULL)
            {
                ndi->ndi_RxNoWrap++;
                ndif_gro_flush_all(ndi); /* held frames are consumed: deliver */
                netstack_unlock();
                return consumed; /* backpressure: driver recycles the tail */
            }
            ndi->ndi_FreeWraps = w->nrw_Next;

            w->nrw_Cookie = d->nrd_Cookie;
            w->nrw_Pc.custom_free_function = ndif_rx_pbuf_freed;

            struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, (u16_t)d->nrd_Len,
                                                 PBUF_REF, &w->nrw_Pc,
                                                 d->nrd_Data, (u16_t)d->nrd_Len);
            consumed++;

            /* non-TCP frames bypass the GRO dispatch entirely */
            if (gro && ndi->ndi_GroMeta[i].ngm_Class != NDIF_GRO_NO)
                ndif_gro_rx(ndi, p, d, &ndi->ndi_GroMeta[i]);
            else
            {
                PERF_T0(t_in);
                if (ndi->ndi_Netif.input(p, &ndi->ndi_Netif) != ERR_OK)
                    pbuf_free(p);
                PERF_ADD(&ns_perf, NSP_RX_INPUT, t_in);
            }

            /* Fairness yield (see NDIF_RX_YIELD_STRIDE): under contention, hand
             * the FIFO lock to a queued app task so it drains its socket queue
             * mid-batch instead of starving. Held merge runs are delivered
             * first — a context never outlives a lock hold. The loop head
             * re-reads ndi_FreeWraps after the relock, so wraps freed
             * meanwhile are seen. */
            if (++since_yield >= NDIF_RX_YIELD_STRIDE &&
                netstack.ns_Core.ss_QueueCount > 0)
            {
                since_yield = 0;
                ndif_gro_flush_all(ndi);
                netstack_unlock();
                PERF_T0(t_relock);
                netstack_lock();
                PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_relock);
            }
        }
        ndif_gro_flush_all(ndi);
        netstack_unlock();
    }

    return consumed;
}

/* ---------------------------------------------------------------- TX --- */

static void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count)
{
    // KprintfH("[netdevif] %s: count %lu\n", __func__, count);
    (void)stackctx;

    PERF_T0(t_done);
    netstack_lock();
    for (ULONG i = 0; i < count; i++)
        pbuf_free((struct pbuf *)cookies[i]);
    PERF_ADD(&ns_perf, NSP_TX_DONE, t_done);
    netstack_unlock();
}

/* 16-bit one's-complement sum of the IPv4 pseudo-header (src, dst, proto,
 * L4 length), folded. */
static UWORD ndif_pseudo_sum(const struct ip_hdr *ip, ULONG start)
{
    // KprintfH("[netdevif] %s: start 0x%08lx\n", __func__, start);
    ULONG sum = start;
    const UWORD *addr = (const UWORD *)&ip->src;
    for (int i = 0; i < 4; i++)
        sum += addr[i];
    sum += IPH_PROTO(ip);
    sum += (ULONG)lwip_ntohs(IPH_LEN(ip)) - (ULONG)IPH_HL(ip) * 4;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (UWORD)sum;
}

/* Byte offset of the IPv4 header inside a (possibly in-band 802.1Q-tagged)
 * Ethernet frame, or 0 if the frame is not IPv4 or its headers are not
 * contiguous in `len`. Recognises a single VLAN tag: IP sits at +14 untagged,
 * +18 tagged. The offset-based csum offloads work on tagged frames precisely
 * because everything downstream keys off this shifted L3 position. */
static ULONG ndif_ip_offset(const UBYTE *frame, ULONG len)
{
    if (len < SIZEOF_ETH_HDR + sizeof(struct ip_hdr))
        return 0;
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    if (eth->type == PP_HTONS(ETHTYPE_IP))
        return SIZEOF_ETH_HDR;
#if ETHARP_SUPPORT_VLAN
    if (eth->type == PP_HTONS(ETHTYPE_VLAN))
    {
        if (len < SIZEOF_ETH_HDR + SIZEOF_VLAN_HDR + sizeof(struct ip_hdr))
            return 0;
        const struct eth_vlan_hdr *vlan =
            (const struct eth_vlan_hdr *)(frame + SIZEOF_ETH_HDR);
        if (vlan->tpid == PP_HTONS(ETHTYPE_IP))
            return SIZEOF_ETH_HDR + SIZEOF_VLAN_HDR;
    }
#endif
    return 0;
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

/* ------------------------------------------------------------ GRO-lite --- */
/* See netdev_if.h for the scheme. The candidate rule is deliberately narrow
 * (clean-LAN bulk shape): IPv4 without options or fragmentation, TCP without
 * options, flags ⊆ {ACK,PSH} with ACK, payload present, and a pad-free frame
 * (nrd_Len − l2 == IPH_LEN — Ethernet-padded runts would splice pad bytes
 * into the stream). Everything else takes today's per-frame path. */

/* Pre-lock classification of one RX frame; reads frame bytes only. */
static void ndif_gro_classify(const struct NetDevRxDesc *d, struct NdGroMeta *m)
{
    const UBYTE *frame = d->nrd_Data;
    ULONG l3 = ndif_ip_offset(frame, d->nrd_Len);
    if (l3 == 0)
    {
        m->ngm_Class = NDIF_GRO_NO;
        return;
    }

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    if (IPH_PROTO(ip) != IP_PROTO_TCP)
    {
        m->ngm_Class = NDIF_GRO_NO;
        return;
    }

    /* IPv4 TCP: record the flow key whatever the verdict — an unmergeable
     * segment (FIN, options, ...) must still flush its flow's held run */
    ULONG ihl = (ULONG)IPH_HL(ip) * 4;
    const struct tcp_hdr *th = (const struct tcp_hdr *)(frame + l3 + ihl);
    m->ngm_SrcIp = ip4_addr_get_u32(&ip->src);
    m->ngm_DstIp = ip4_addr_get_u32(&ip->dest);
    m->ngm_Ports = ((ULONG)th->src << 16) | th->dest;
    m->ngm_Class = NDIF_GRO_NOMERGE;

    ULONG iplen = lwip_ntohs(IPH_LEN(ip));
    UWORD flags = TCPH_FLAGS(th);
    if (ihl != IP_HLEN ||
        (IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0 ||
        TCPH_HDRLEN_BYTES(th) != TCP_HLEN ||
        (flags & ~(ULONG)(TCP_ACK | TCP_PSH)) != 0 || (flags & TCP_ACK) == 0 ||
        iplen <= IP_HLEN + TCP_HLEN || iplen != d->nrd_Len - l3)
        return;

    m->ngm_Seq = lwip_ntohl(th->seqno);
    m->ngm_PayOff = (UWORD)(l3 + IP_HLEN + TCP_HLEN);
    m->ngm_PayLen = (UWORD)(iplen - IP_HLEN - TCP_HLEN);
    m->ngm_Flags = (UBYTE)flags;
    m->ngm_Class = NDIF_GRO_MERGE;
}

/* Hand one frame (or chain) to lwIP; shared by flush and the direct path. */
static void ndif_gro_deliver(struct NetdevIf *ndi, struct pbuf *p)
{
    PERF_T0(t_in);
    if (ndi->ndi_Netif.input(p, &ndi->ndi_Netif) != ERR_OK)
        pbuf_free(p);
    PERF_ADD(&ns_perf, NSP_RX_INPUT, t_in);
}

/* Deliver a held run: restore the pbuf-chain tot_len invariant (deferred
 * during manual linking), patch the head's IP length + checksum, and feed
 * lwIP once. A 1-frame run is delivered untouched. Under the core lock. */
static void ndif_gro_flush(struct NetdevIf *ndi, struct NdGroCtx *c)
{
    struct pbuf *head = c->ngc_Head;
    if (head == NULL)
        return;
    c->ngc_Head = NULL;

    if (c->ngc_Frames > 1)
    {
        ULONG remaining = (ULONG)head->len + c->ngc_PayloadAdd;
        for (struct pbuf *q = head; q != NULL; q = q->next)
        {
            q->tot_len = (u16_t)remaining;
            remaining -= q->len;
        }

        /* IP total length + RFC 1624 incremental header-checksum fixup
         * (CHECK_IP is always on for this netif). ackno/wnd/PSH were
         * already patched as each frame merged; seqno stays the head's. */
        u16_t old_len = IPH_LEN(c->ngc_Ip); /* raw big-endian, as is _chksum */
        u16_t new_len = lwip_htons((u16_t)(lwip_ntohs(old_len) +
                                           c->ngc_PayloadAdd));
        ULONG sum = (ULONG)(u16_t)~IPH_CHKSUM(c->ngc_Ip) +
                    (ULONG)(u16_t)~old_len + (ULONG)new_len;
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        IPH_LEN_SET(c->ngc_Ip, new_len);
#if defined(DEBUG) && defined(DEBUG_HIGH)
        /* cross-check the incremental fixup against a full recompute */
        IPH_CHKSUM_SET(c->ngc_Ip, 0);
        u16_t full = inet_chksum(c->ngc_Ip, IP_HLEN);
        if (full != (u16_t)~sum)
            Kprintf("[netdevif] GRO csum fixup mismatch: inc 0x%04lx full 0x%04lx\n",
                    (ULONG)(u16_t)~sum, (ULONG)full);
#endif
        IPH_CHKSUM_SET(c->ngc_Ip, (u16_t)~sum);
    }

    ndif_gro_deliver(ndi, head);
}

static void ndif_gro_flush_all(struct NetdevIf *ndi)
{
    for (ULONG i = 0; i < NDIF_GRO_FLOWS; i++)
        ndif_gro_flush(ndi, &ndi->ndi_Gro[i]);
}

/* Per-frame dispatch inside the locked RX loop. Only IPv4 TCP frames get
 * here (class NOMERGE or MERGE); the caller short-circuits class NO. */
static void ndif_gro_rx(struct NetdevIf *ndi, struct pbuf *p,
                        const struct NetDevRxDesc *d, const struct NdGroMeta *m)
{
    struct NdGroCtx *c =
        &ndi->ndi_Gro[(m->ngm_Ports ^ m->ngm_SrcIp) & (NDIF_GRO_FLOWS - 1)];
    BOOL sameflow = c->ngc_Head != NULL &&
                    c->ngc_SrcIp == m->ngm_SrcIp &&
                    c->ngc_DstIp == m->ngm_DstIp &&
                    c->ngc_Ports == m->ngm_Ports;

    if (m->ngm_Class == NDIF_GRO_NOMERGE)
    {
        /* a FIN/RST/option-bearing segment must not overtake held data */
        if (sameflow)
            ndif_gro_flush(ndi, c);
        ndif_gro_deliver(ndi, p);
        return;
    }

    if (sameflow && m->ngm_Seq == c->ngc_NextSeq &&
        c->ngc_Frames < NDIF_GRO_MAX_FRAMES)
    {
        /* absorb: strip headers, link via the tail pointer (tot_len of the
         * chain is restored at flush), take the freshest cumulative ackno
         * and window, OR the PSH hint */
        PERF_T0(t_gro);
        pbuf_remove_header(p, m->ngm_PayOff);
        c->ngc_Tail->next = p;
        c->ngc_Tail = p;
        c->ngc_NextSeq += m->ngm_PayLen;
        c->ngc_PayloadAdd += m->ngm_PayLen;
        c->ngc_Frames++;

        const struct tcp_hdr *th =
            (const struct tcp_hdr *)(d->nrd_Data + m->ngm_PayOff - TCP_HLEN);
        c->ngc_Tcp->ackno = th->ackno;
        c->ngc_Tcp->wnd = th->wnd;
        if (m->ngm_Flags & TCP_PSH)
            TCPH_SET_FLAG(c->ngc_Tcp, TCP_PSH);
        PERF_ADD(&ns_perf, NSP_RX_GRO, t_gro);
        return;
    }

    /* other flow in the slot, sequence discontinuity, or run full */
    if (c->ngc_Head != NULL)
        ndif_gro_flush(ndi, c);

    c->ngc_Head = p;
    c->ngc_Tail = p;
    c->ngc_Ip = (struct ip_hdr *)(d->nrd_Data + m->ngm_PayOff - TCP_HLEN - IP_HLEN);
    c->ngc_Tcp = (struct tcp_hdr *)(d->nrd_Data + m->ngm_PayOff - TCP_HLEN);
    c->ngc_SrcIp = m->ngm_SrcIp;
    c->ngc_DstIp = m->ngm_DstIp;
    c->ngc_Ports = m->ngm_Ports;
    c->ngc_NextSeq = m->ngm_Seq + m->ngm_PayLen;
    c->ngc_PayloadAdd = 0;
    c->ngc_Frames = 1;
}

/* TX offload preparation: seed the L4 checksum field with the pseudo-header
 * sum (CHECKSUM_PARTIAL style) and compute the offload offsets. Returns the
 * L4 protocol (IP_PROTO_TCP/UDP), or 0xFFFF when the frame is not
 * offloadable (non-IP, fragments, other protocols). */
static ULONG ndif_l4_offsets(struct pbuf *p, UWORD *csum_start, UWORD *csum_offset)
{
    // KprintfH("[netdevif] %s: len %lu\n", __func__, (ULONG)p->len);
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

/* RX verification for frames the driver reported only a RAW checksum for.
 * `raw` is the 1's-complement sum over the frame past the Ethernet
 * header (IP header + payload). A valid IP header folds to -0, so for a
 * valid TCP/UDP checksum fold(raw + pseudo-header) == 0xFFFF. Non-IP,
 * non-TCP/UDP and checksum-less UDP pass through; fragments pass and are
 * validated only by the reassembled IP checksum (documented gap — the
 * Ethernet FCS already covered the wire). */
static BOOL ndif_rx_csum_ok(const struct NetDevRxDesc *d, ULONG raw)
{
    // KprintfH("[netdevif] %s: len %lu\n", __func__, (ULONG)d->nrd_Len);
    const UBYTE *frame = d->nrd_Data;
    ULONG l3 = ndif_ip_offset(frame, d->nrd_Len);
    if (l3 == 0)
        return TRUE; /* non-IPv4: nothing to fold, accept */

    /* `raw` is the 1's-complement sum over [frame + SIZEOF_ETH_HDR .. end].
     * On a tagged frame that region opens with the two 16-bit tag words (the
     * TCI and the inner ethertype) that sit before the IP header, so remove
     * them here — ndif_pseudo_sum expects a sum over [IP .. end]. */
    if (l3 != SIZEOF_ETH_HDR)
    {
        const UWORD *tag = (const UWORD *)(frame + SIZEOF_ETH_HDR);
        raw += (~(ULONG)tag[0]) & 0xFFFF;   /* subtract the 802.1Q TCI word */
        raw += (~(ULONG)tag[1]) & 0xFFFF;   /* subtract the inner ethertype word */
    }

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    if ((IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0)
        return TRUE;

    ULONG proto = IPH_PROTO(ip);
    if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP)
        return TRUE;

    if (proto == IP_PROTO_UDP)
    {
        const struct udp_hdr *uh =
            (const struct udp_hdr *)(frame + l3 + (ULONG)IPH_HL(ip) * 4);
        if (uh->chksum == 0)
            return TRUE; /* UDP without checksum is legal on IPv4 */
    }

    return ndif_pseudo_sum(ip, raw) == 0xFFFF;
}

static err_t ndif_linkoutput(struct netif *nif, struct pbuf *p)
{
    // KprintfH("[netdevif] %s: pbuf 0x%08lx tot_len %lu\n", __func__, (ULONG)p, (ULONG)p->tot_len);
    struct NetdevIf *ndi = nif->state;
    PERF_T0(t_out);

    /* a slow-path ndif_ip4_output send is in flight: capture its header */
    if (ndi->ndi_HhPrimeDst != 0)
        ndif_hh_prime(ndi, p);

#ifdef DEBUG
    /* 2026-07-14 corruption hunt: the observed wild 2-byte writes match
     * ndif_l4_offsets' checksum seed going through a trashed pbuf payload.
     * Refuse implausible chains here — a dropped frame beats a wild write —
     * and log the moment the stack hands us garbage. */
    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        if ((ULONG)q->payload < 0x1000 || (ULONG)q->payload >= 0xF0000000UL ||
            q->len > 2048)
        {
            Kprintf("[netdevif] BAD TX PBUF: p=0x%08lx q=0x%08lx payload=0x%08lx len=%lu ref=%lu — frame dropped\n",
                    (ULONG)p, (ULONG)q, (ULONG)q->payload, (ULONG)q->len, (ULONG)q->ref);
            ndi->ndi_TxOversize++;
            return ERR_IF;
        }
    }
#endif

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

/* -------------------------------------------------------- link events --- */

static void ndif_link_change(APTR stackctx, const struct NetDevLinkState *state)
{
    Kprintf("[netdevif] %s: flags 0x%08lx\n", __func__, (ULONG)state->ndls_Flags);
    struct NetdevIf *ndi = stackctx;

    netstack_lock();
    /* a link transition may mean a new peer/port: drop the L2 header cache */
    for (ULONG i = 0; i < NDIF_HH_ENTRIES; i++)
    {
        ndi->ndi_Hh[i].nhh_DstIp = 0;
        ndi->ndi_Hh[i].nhh_Left = 0;
    }
    if (state->ndls_Flags & NDLF_UP)
        netif_set_link_up(&ndi->ndi_Netif);
    else
        netif_set_link_down(&ndi->ndi_Netif);
    netstack_unlock();
}

/* ---------------------------------------------------------- VLAN hooks --- */
/* In-band 802.1Q (lwIP LWIP_HOOK_VLAN_SET/CHECK, wired in netstack_lwiphooks.h).
 * The per-interface TCI is ndi_VlanTci (-1 = untagged), set from
 * netstack.prefs before the netif comes up. GENET has no hardware VLAN
 * offload, so the tag rides inside the frame both ways; the checksum
 * offloads stay on because ndif_l4_offsets/ndif_rx_csum_ok are tag-aware. */

s32_t netdevif_vlan_set(struct netif *nif, struct pbuf *p,
                        const struct eth_addr *src, const struct eth_addr *dst,
                        u16_t eth_type)
{
    (void)p;
    (void)src;
    (void)dst;
    (void)eth_type;
    return (s32_t)((struct NetdevIf *)nif->state)->ndi_VlanTci; /* <0 = no tag */
}

int netdevif_vlan_check(struct netif *nif, struct eth_hdr *eth,
                        struct eth_vlan_hdr *vlan)
{
    (void)eth;
    LONG tci = ((struct NetdevIf *)nif->state)->ndi_VlanTci;
    if (tci < 0)
        return 0; /* not on a VLAN: drop tagged frames */
    return VLAN_ID(vlan) == (UWORD)(tci & 0xFFF);
}

/* ------------------------------------------------------------ plumbing --- */

static const struct NetDevStackOps ndif_stack_ops = {
    ndif_rx_input,
    ndif_tx_done,
    ndif_link_change,
};

const struct NetDevStackOps *netdevif_stack_ops(void)
{
    KprintfH("[netdevif] %s\n", __func__);
    return &ndif_stack_ops;
}

/* Frames the stack can pin at once: one full receive window per expected
 * concurrent bulk stream plus in-flight slack. Four streams also sized the
 * old fixed genet pool. = 4 x ceil(TCP_WND / TCP_MSS) + 64 = 784 with the current
 * lwipopts. */
#define NDIF_RX_HOLD_STREAMS 4
#define NDIF_RX_HOLD_SLACK   64

UWORD netdevif_rx_hold_budget(void)
{
    ULONG wndFrames = (TCP_WND + TCP_MSS - 1) / TCP_MSS;
    return (UWORD)(NDIF_RX_HOLD_STREAMS * wndFrames + NDIF_RX_HOLD_SLACK);
}

APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size, ULONG align)
{
    // KprintfH("[netdevif] %s: size %lu align %lu\n", __func__, size, align);
    return ndi->ndi_Ops->ndo_DmaAlloc(ndi->ndi_Drv, size, align);
}

void netdevif_dma_free(struct NetdevIf *ndi, APTR ptr, ULONG size)
{
    // KprintfH("[netdevif] %s: ptr 0x%08lx size %lu\n", __func__, (ULONG)ptr, size);
    if (ndi == NULL)
    {
        Kprintf("[netdevif] DMA free after detach — leaked %lu bytes\n", size);
        return;
    }
    ndi->ndi_Ops->ndo_DmaFree(ndi->ndi_Drv, ptr, size);
}

/* netif->output — the TX hot path for every IP frame (TCP segments, UDP
 * datagrams and their fragments). On a header-cache hit the prebuilt L2
 * header is prepended and the frame goes straight to linkoutput,
 * bypassing etharp_output/ethernet_output. Misses and the periodic
 * revalidation take the slow path, which snoop-primes the cache in
 * ndif_linkoutput. MIB2 netif counters are maintained to match
 * ethernet_output. */
static err_t ndif_ip4_output(struct netif *nif, struct pbuf *p,
                             const ip4_addr_t *ipaddr)
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

static err_t ndif_netif_init(struct netif *nif)
{
    Kprintf("[netdevif] %s: nif 0x%08lx\n", __func__, (ULONG)nif);
    struct NetdevIf *ndi = nif->state;

    nif->name[0] = 'n';
    nif->name[1] = 'd';
    nif->output = ndif_ip4_output;
    nif->linkoutput = ndif_linkoutput;
    nif->mtu = ndi->ndi_Caps.ndc_Mtu;
    nif->hwaddr_len = ETH_HWADDR_LEN;
    for (int i = 0; i < ETH_HWADDR_LEN; i++)
        nif->hwaddr[i] = ndi->ndi_Caps.ndc_Mac[i];
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;

    /* Checksum policy from the capabilities:
     *  - TX L4 offload -> stop lwIP generating TCP/UDP checksums (the glue
     *    seeds the pseudo-header instead); IP header stays software.
     *  - RX: with VALID and/or RAW offload, lwIP's TCP/UDP checking is off;
     *    per-frame VALID verdicts pass directly, RAW-only frames are folded
     *    and verified in ndif_rx_input. */
    ULONG csum = NETIF_CHECKSUM_ENABLE_ALL;
    if (ndi->ndi_Caps.ndc_Features & NDCF_TX_L4CSUM)
        csum &= ~(ULONG)(NETIF_CHECKSUM_GEN_TCP | NETIF_CHECKSUM_GEN_UDP);
    ndi->ndi_RxOffload =
        (ndi->ndi_Caps.ndc_Features & (NDCF_RX_CSUM_VALID | NDCF_RX_CSUM_RAW)) != 0;
    if (ndi->ndi_RxOffload)
        csum &= ~(ULONG)(NETIF_CHECKSUM_CHECK_TCP | NETIF_CHECKSUM_CHECK_UDP);
    NETIF_SET_CHECKSUM_CTRL(nif, (UWORD)csum);

    MIB2_INIT_NETIF(nif, snmp_ifType_ethernet_csmacd, 1000000000);
    return ERR_OK;
}

LONG netdevif_create(struct NetdevIf *ndi, APTR drvCtx,
                     const struct NetDevDrvOps *drvOps,
                     const struct NetDevCaps *caps)
{
    ndi->ndi_Drv = drvCtx;
    ndi->ndi_Ops = drvOps;
    ndi->ndi_Caps = *caps;
    ndi->ndi_RxNoWrap = 0;
    ndi->ndi_TxOversize = 0;
    ndi->ndi_VlanTci = -1; /* untagged by default; the opener overrides from prefs */
    for (ULONG i = 0; i < NDIF_HH_ENTRIES; i++)
    {
        ndi->ndi_Hh[i].nhh_DstIp = 0;
        ndi->ndi_Hh[i].nhh_Left = 0;
    }
    ndi->ndi_HhPrimeDst = 0;
    for (ULONG i = 0; i < NDIF_GRO_FLOWS; i++)
        ndi->ndi_Gro[i].ngc_Head = NULL; /* contexts idle outside lock holds */

    /* RX wrappers: one per buffer the stack can possibly hold. The driver
     * advertises its pool size; a wrap count below it silently re-imposes
     * the old limit as ndi_RxNoWrap backpressure. Ring*2 is the fallback
     * for drivers that predate ndc_RxPoolBufs (field reads 0). */
    ULONG count = caps->ndc_RxPoolBufs;
    if (count < (ULONG)caps->ndc_RxRingSlots * 2)
        count = (ULONG)caps->ndc_RxRingSlots * 2;
    if (count < NDIF_MIN_WRAPS)
        count = NDIF_MIN_WRAPS;
    ndi->ndi_WrapStorageSize = count * sizeof(struct NdRxWrap);
    ndi->ndi_WrapStorage = AllocMem(ndi->ndi_WrapStorageSize, MEMF_PUBLIC | MEMF_CLEAR);
    if (ndi->ndi_WrapStorage == NULL)
        return -1;

    struct NdRxWrap *w = ndi->ndi_WrapStorage;
    ndi->ndi_FreeWraps = NULL;
    for (ULONG i = 0; i < count; i++, w++)
    {
        w->nrw_If = ndi;
        w->nrw_Next = ndi->ndi_FreeWraps;
        ndi->ndi_FreeWraps = w;
    }

    netstack_lock();
    struct netif *added = netif_add_noaddr(&ndi->ndi_Netif, ndi,
                                           ndif_netif_init, ethernet_input);
    if (added != NULL)
        netstack.ns_ActiveNetdev = ndi;
    netstack_unlock();

    if (added == NULL)
    {
        FreeMem(ndi->ndi_WrapStorage, ndi->ndi_WrapStorageSize);
        return -1;
    }

    Kprintf("[netdevif] netif nd up: mtu %lu, features 0x%08lx\n",
            (ULONG)ndi->ndi_Caps.ndc_Mtu, ndi->ndi_Caps.ndc_Features);
    return 0;
}

void netdevif_destroy(struct NetdevIf *ndi)
{
    Kprintf("[netdevif] %s: ndi 0x%08lx\n", __func__, (ULONG)ndi);
    netstack_lock();
    netif_remove(&ndi->ndi_Netif);
    if (netstack.ns_ActiveNetdev == ndi)
    {
        /* return the slab arenas while the ABI pointer is still valid */
        netstack_slab_detach(ndi);
        netstack.ns_ActiveNetdev = NULL;
    }
    netstack_unlock();

    FreeMem(ndi->ndi_WrapStorage, ndi->ndi_WrapStorageSize);
    ndi->ndi_WrapStorage = NULL;
    ndi->ndi_FreeWraps = NULL;
}
