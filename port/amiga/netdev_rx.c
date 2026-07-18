/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev RX path: driver batches injected into lwIP as zero-copy custom
 * pbufs, with pre-lock checksum verification (RAW-offload fold) and the
 * GRO-lite merge of in-order TCP runs. Everything here runs on the driver's
 * unit task; lwIP entry is bracketed by the netstack core lock.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/inet_chksum.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/tcp.h>
#include <lwip/prot/udp.h>

#include "netdev_priv.h"
#include "netstack.h"
#include "nsprof.h"

/* Fairness yield stride: during an unflow-controlled RX flood (a UDP blast at
 * line rate) the unit task processes every frame under ns_Core while the app
 * task queues on the lock to drain its socket — starving it, so its recv queue
 * overflows and ~everything is dropped past the app. Every this-many frames,
 * IF a task is actually queued on ns_Core, hand the FIFO lock over so the app
 * drains (and frees pool buffers) before we fill its queue with drops. Gated on
 * ss_QueueCount, so the uncontended fast path never pays. Tunable: smaller =
 * fairer to the app but more lock handoffs (2 context switches each). */
#define NDIF_RX_YIELD_STRIDE 8u

static void ndif_rx_pbuf_freed(struct pbuf *p)
{
    struct NdRxWrap *w = (struct NdRxWrap *)p;
    struct NetdevIf *ndi = w->nrw_If;

    ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, w->nrw_Cookie);

    w->nrw_Next = ndi->ndi_FreeWraps;
    ndi->ndi_FreeWraps = w;
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

/* Hand one frame (or merged chain) to lwIP. Under the core lock. */
static void ndif_deliver(struct NetdevIf *ndi, struct pbuf *p)
{
    PERF_T0(t_in);
    if (ndi->ndi_Netif.input(p, &ndi->ndi_Netif) != ERR_OK)
        pbuf_free(p);
    PERF_ADD(&ns_perf, NSP_RX_INPUT, t_in);
}

/* ------------------------------------------------------------ GRO-lite --- */
/* See netdev_if.h for the scheme. The candidate rule is deliberately narrow
 * (clean-LAN bulk shape): IPv4 without options or fragmentation, TCP without
 * options, flags ⊆ {ACK,PSH} with ACK, payload present, and a pad-free frame
 * (nrd_Len − l2 == IPH_LEN — Ethernet-padded runts would splice pad bytes
 * into the stream). Everything else takes the per-frame path. */

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

    ndif_deliver(ndi, head);
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
        ndif_deliver(ndi, p);
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

/* ----------------------------------------------------------- injection --- */

ULONG ndif_rx_input(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count)
{
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
                ndif_deliver(ndi, p);

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
