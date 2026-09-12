/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * rx_gro — the GRO-lite engine (see rx_gro.h for the contract). Moved
 * verbatim from the netdev RX path when the SANA-II pump adopted it.
 * All frame pointers derive from p->payload (for netdev RX pbufs that
 * equals the driver descriptor's nrd_Data).
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/inet_chksum.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/tcp.h>

#include "inet_frame.h"
#include "netstack.h"
#include "nsprof.h"
#include "rx_gro.h"

void rxgro_init(struct RxGro *g, struct netif *nif)
{
    g->rg_Netif = nif;
    for (ULONG i = 0; i < RXGRO_FLOWS; i++)
        g->rg_Flows[i].rgc_Head = NULL; /* contexts idle outside lock holds */
}

void rxgro_deliver(struct RxGro *g, struct pbuf *p)
{
    PERF_T0(t_in);
    struct netif *nif = g->rg_Netif;
    if (nif->input(p, nif) != ERR_OK)
        pbuf_free(p);
    PERF_ADD(&ns_perf, NSP_RX_INPUT, t_in);
}

/* wrap-safe: TRUE iff a is strictly newer than b in TCP sequence space
 * (mirrors lwIP's TCP_SEQ_GT without pulling in <lwip/priv/tcp_priv.h>). */
static inline BOOL rxgro_seq_gt(u32_t a, u32_t b)
{
    return ((u32_t)(b - a) & 0x80000000u) != 0;
}

void rxgro_classify(const UBYTE *frame, ULONG len, struct RxGroMeta *m)
{
    ULONG l3 = inetfrm_ip_offset(frame, len);
    if (l3 == 0)
    {
        m->rgm_Class = RXGRO_NO;
        return;
    }

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    if (IPH_PROTO(ip) != IP_PROTO_TCP)
    {
        m->rgm_Class = RXGRO_NO;
        return;
    }

    /* IPv4 TCP: record the flow key whatever the verdict — an unmergeable
     * segment (FIN, options, ...) must still flush its flow's held run */
    ULONG ihl = (ULONG)IPH_HL(ip) * 4;
    const struct tcp_hdr *th = (const struct tcp_hdr *)(frame + l3 + ihl);
    m->rgm_SrcIp = ip4_addr_get_u32(&ip->src);
    m->rgm_DstIp = ip4_addr_get_u32(&ip->dest);
    m->rgm_Ports = ((ULONG)th->src << 16) | th->dest;
    m->rgm_Class = RXGRO_NOMERGE;

    ULONG iplen = lwip_ntohs(IPH_LEN(ip));
    UWORD flags = TCPH_FLAGS(th);

    /* Mergeable-header shape shared by data segments and pure ACKs: no IP
     * options, not a fragment, no TCP options, flags ⊆ {ACK,PSH} with ACK. */
    if (ihl != IP_HLEN ||
        (IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0 ||
        TCPH_HDRLEN_BYTES(th) != TCP_HLEN ||
        (flags & ~(ULONG)(TCP_ACK | TCP_PSH)) != 0 || (flags & TCP_ACK) == 0)
        return; /* stays NOMERGE */

    if (iplen <= IP_HLEN + TCP_HLEN)
    {
        /* No TCP payload. A pure ACK (flags exactly ACK) coalesces to the
         * freshest per flow; a zero-payload PSH stays per-frame. The frame is
         * delivered untouched (lwIP trims the Ethernet pad to IPH_LEN), so the
         * pad-free check the data path needs does not apply here. */
        if (iplen == IP_HLEN + TCP_HLEN && flags == TCP_ACK)
        {
            m->rgm_AckNo = lwip_ntohl(th->ackno);
            m->rgm_Class = RXGRO_ACK;
        }
        return;
    }

    /* Data segment: require a pad-free frame — padded runts would splice pad
     * bytes into the reassembled stream. */
    if (iplen != len - l3)
        return;

    m->rgm_Seq = lwip_ntohl(th->seqno);
    m->rgm_PayOff = (UWORD)(l3 + IP_HLEN + TCP_HLEN);
    m->rgm_PayLen = (UWORD)(iplen - IP_HLEN - TCP_HLEN);
    m->rgm_Flags = (UBYTE)flags;
    m->rgm_Class = RXGRO_MERGE;
}

/* Deliver a held run: restore the pbuf-chain tot_len invariant (deferred
 * during manual linking), patch the head's IP length + checksum, and feed
 * lwIP once. A 1-frame run is delivered untouched. Under the core lock. */
static void rxgro_flush(struct RxGro *g, struct RxGroCtx *c)
{
    struct pbuf *head = c->rgc_Head;
    if (head == NULL)
        return;
    c->rgc_Head = NULL;

    if (c->rgc_Frames > 1)
    {
        ULONG remaining = (ULONG)head->len + c->rgc_PayloadAdd;
        for (struct pbuf *q = head; q != NULL; q = q->next)
        {
            q->tot_len = (u16_t)remaining;
            remaining -= q->len;
        }

        /* IP total length + RFC 1624 incremental header-checksum fixup
         * (CHECK_IP is always on for the netif). ackno/wnd/PSH were
         * already patched as each frame merged; seqno stays the head's. */
        u16_t old_len = IPH_LEN(c->rgc_Ip); /* raw big-endian, as is _chksum */
        u16_t new_len = lwip_htons((u16_t)(lwip_ntohs(old_len) +
                                           c->rgc_PayloadAdd));
        ULONG sum = (ULONG)(u16_t)~IPH_CHKSUM(c->rgc_Ip) +
                    (ULONG)(u16_t)~old_len + (ULONG)new_len;
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        IPH_LEN_SET(c->rgc_Ip, new_len);
#ifdef TRACE
        /* cross-check the incremental fixup against a full recompute */
        IPH_CHKSUM_SET(c->rgc_Ip, 0);
        u16_t full = inet_chksum(c->rgc_Ip, IP_HLEN);
        if (full != (u16_t)~sum)
            Kprintf("[rxgro] csum fixup mismatch: inc 0x%04lx full 0x%04lx\n",
                    (ULONG)(u16_t)~sum, (ULONG)full);
#endif
        IPH_CHKSUM_SET(c->rgc_Ip, (u16_t)~sum);
    }

    rxgro_deliver(g, head);
}

void rxgro_flush_all(struct RxGro *g)
{
    for (ULONG i = 0; i < RXGRO_FLOWS; i++)
        rxgro_flush(g, &g->rg_Flows[i]);
}

void rxgro_yield(struct RxGro *g)
{
    rxgro_flush_all(g); /* a context never outlives a lock hold */
    netstack_unlock();
    PERF_T0(t_relock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_relock);
}

void rxgro_rx(struct RxGro *g, struct pbuf *p, const struct RxGroMeta *m)
{
    struct RxGroCtx *c =
        &g->rg_Flows[(m->rgm_Ports ^ m->rgm_SrcIp) & (RXGRO_FLOWS - 1)];
    BOOL sameflow = c->rgc_Head != NULL &&
                    c->rgc_SrcIp == m->rgm_SrcIp &&
                    c->rgc_DstIp == m->rgm_DstIp &&
                    c->rgc_Ports == m->rgm_Ports;

    if (m->rgm_Class == RXGRO_NOMERGE)
    {
        /* a FIN/RST/option-bearing segment must not overtake held data */
        if (sameflow)
            rxgro_flush(g, c);
        rxgro_deliver(g, p);
        return;
    }

    if (m->rgm_Class == RXGRO_ACK)
    {
        /* Coalesce pure ACKs: hold only the freshest (strictly-advancing
         * ackno) per flow, delivering one per flush instead of one per ACK.
         * Non-advancing (duplicate/reordered) ACKs are delivered individually,
         * so lwIP still sees the dup-ACK run fast retransmit needs. */
        if (sameflow && c->rgc_IsAck && rxgro_seq_gt(m->rgm_AckNo, c->rgc_AckNo))
        {
            PERF_T0(t_gro);
            pbuf_free(c->rgc_Head); /* recycle the superseded ACK's buffer */
            c->rgc_Head = c->rgc_Tail = p;
            c->rgc_AckNo = m->rgm_AckNo;
            PERF_ADD(&ns_perf, NSP_RX_GRO, t_gro);
            return;
        }

        /* different flow in the slot, a data run held, or a non-advancing
         * ackno: deliver whatever was held, then hold this ACK */
        if (c->rgc_Head != NULL)
            rxgro_flush(g, c);
        c->rgc_Head = c->rgc_Tail = p;
        c->rgc_SrcIp = m->rgm_SrcIp;
        c->rgc_DstIp = m->rgm_DstIp;
        c->rgc_Ports = m->rgm_Ports;
        c->rgc_AckNo = m->rgm_AckNo;
        c->rgc_Frames = 1;
        c->rgc_IsAck = TRUE;
        return;
    }

    if (sameflow && !c->rgc_IsAck && m->rgm_Seq == c->rgc_NextSeq &&
        c->rgc_Frames < RXGRO_MAX_FRAMES)
    {
        /* absorb: strip headers, link via the tail pointer (tot_len of the
         * chain is restored at flush), take the freshest cumulative ackno
         * and window, OR the PSH hint. Capture the TCP header BEFORE
         * pbuf_remove_header moves p->payload past it. */
        PERF_T0(t_gro);
        const struct tcp_hdr *th =
            (const struct tcp_hdr *)((const UBYTE *)p->payload +
                                     m->rgm_PayOff - TCP_HLEN);
        pbuf_remove_header(p, m->rgm_PayOff);
        c->rgc_Tail->next = p;
        c->rgc_Tail = p;
        c->rgc_NextSeq += m->rgm_PayLen;
        c->rgc_PayloadAdd += m->rgm_PayLen;
        c->rgc_Frames++;

        c->rgc_Tcp->ackno = th->ackno;
        c->rgc_Tcp->wnd = th->wnd;
        if (m->rgm_Flags & TCP_PSH)
            TCPH_SET_FLAG(c->rgc_Tcp, TCP_PSH);
        PERF_ADD(&ns_perf, NSP_RX_GRO, t_gro);
        return;
    }

    /* other flow in the slot, sequence discontinuity, or run full */
    if (c->rgc_Head != NULL)
        rxgro_flush(g, c);

    c->rgc_Head = p;
    c->rgc_Tail = p;
    c->rgc_Ip = (struct ip_hdr *)((UBYTE *)p->payload +
                                  m->rgm_PayOff - TCP_HLEN - IP_HLEN);
    c->rgc_Tcp = (struct tcp_hdr *)((UBYTE *)p->payload +
                                    m->rgm_PayOff - TCP_HLEN);
    c->rgc_SrcIp = m->rgm_SrcIp;
    c->rgc_DstIp = m->rgm_DstIp;
    c->rgc_Ports = m->rgm_Ports;
    c->rgc_NextSeq = m->rgm_Seq + m->rgm_PayLen;
    c->rgc_PayloadAdd = 0;
    c->rgc_Frames = 1;
    c->rgc_IsAck = FALSE;
}
