/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * rx_gro — GRO-lite: merge N consecutive in-order same-flow TCP data frames
 * from one RX batch into a single pbuf chain and feed lwIP once; coalesce
 * pure ACKs to the freshest per flow. Shared by both driver backends (the
 * netdev unit-task injection loop and the SANA-II pump).
 *
 * Contract: candidates are classified in the caller's pre-lock pass
 * (rxgro_classify reads frame bytes only); dispatch/flush run under the
 * netstack core lock. Merge contexts live only WITHIN one lock hold — the
 * caller MUST rxgro_flush_all() before every unlock (fairness yields
 * included), so nothing survives across holds, link changes or teardown.
 * The caller gates dispatch on lwIP's own TCP checksum check being off for
 * the netif: a rewritten merged header would fail re-verification.
 */

#ifndef LWIPAMIGA_RX_GRO_H
#define LWIPAMIGA_RX_GRO_H

#include <exec/types.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>

struct ip_hdr;  /* lwip/prot/ip4.h */
struct tcp_hdr; /* lwip/prot/tcp.h */

#define RXGRO_FLOWS      4u  /* direct-mapped merge contexts */
#define RXGRO_MAX_FRAMES 44u /* per merge — the u16 IPH_LEN ceiling:
                                1500 + 43*1460 = 64280 <= 65535. The caller's
                                RX batch is the other bound on a run; 64-frame
                                batches split into a 44 + a 20. */

/* Fairness yield stride: during an unflow-controlled RX flood (a UDP blast at
 * line rate) the RX task (netdev unit task / SANA-II pump) processes every
 * frame under ns_Core while the app task queues on the lock to drain its
 * socket — starving it, so its recv queue overflows and ~everything is
 * dropped past the app. Every this-many frames, IF a task is actually queued
 * on ns_Core, hand the FIFO lock over so the app drains (and frees pool
 * buffers) before we fill its queue with drops. Gated on ss_QueueCount, so
 * the uncontended fast path never pays. Tunable: smaller = fairer to the app
 * but more lock handoffs (2 context switches each). */
#define RXGRO_YIELD_STRIDE 8u

/* per-frame pre-lock classification verdict */
#define RXGRO_NO      0 /* not IPv4/TCP: deliver immediately */
#define RXGRO_NOMERGE 1 /* IPv4 TCP but unmergeable (SYN/FIN/RST, options,
                           padded, fragment): flush its flow first */
#define RXGRO_MERGE   2 /* in-order-candidate data segment */
#define RXGRO_ACK     3 /* payload-free pure ACK: coalesce to the freshest
                           per flow (a bulk sender only needs the newest
                           cumulative ackno + window) */

struct RxGroMeta
{
    ULONG rgm_SrcIp; /* flow key, raw network order */
    ULONG rgm_DstIp;
    ULONG rgm_Ports; /* src<<16 | dst, raw */
    union {
        ULONG rgm_Seq;   /* MERGE frame: data seqno, host order */
        ULONG rgm_AckNo; /* ACK frame:   ackno,      host order */
    };
    UWORD rgm_PayOff; /* frame offset of TCP payload (l2 + 20 + 20) */
    UWORD rgm_PayLen; /* TCP payload bytes (from IPH_LEN, pad excluded) */
    UBYTE rgm_Class;  /* RXGRO_* */
    UBYTE rgm_Flags;  /* raw TCP flag byte (PSH propagation) */
};

struct RxGroCtx
{
    struct pbuf *rgc_Head;   /* first frame, headers intact; NULL = idle */
    struct pbuf *rgc_Tail;   /* append point (manual linking; tot_len of
                                the chain is fixed up once at flush) */
    struct ip_hdr *rgc_Ip;   /* head's IP header (length/csum patch) */
    struct tcp_hdr *rgc_Tcp; /* head's TCP header (ackno/wnd/PSH patch) */
    ULONG rgc_SrcIp;         /* flow key, raw */
    ULONG rgc_DstIp;
    ULONG rgc_Ports;
    union {
        ULONG rgc_NextSeq; /* data run: host order, expected next seqno */
        ULONG rgc_AckNo;   /* ack hold: host order, held (freshest) ackno */
    };
    ULONG rgc_PayloadAdd; /* Σ payload bytes appended after the head */
    UWORD rgc_Frames;     /* frames absorbed, head included */
    UBYTE rgc_IsAck;      /* held run is a coalesced pure-ACK, not data
                             (valid only while rgc_Head != NULL) */
};

/* One engine instance, embedded in the backend struct. rg_Netif is the
 * delivery target (nif->input under the core lock). */
struct RxGro
{
    struct netif *rg_Netif;
    struct RxGroCtx rg_Flows[RXGRO_FLOWS];
};

void rxgro_init(struct RxGro *g, struct netif *nif);

/* Pre-lock classification of one RX frame; reads frame bytes only. */
void rxgro_classify(const UBYTE *frame, ULONG len, struct RxGroMeta *m);

/* Per-frame dispatch inside the locked RX loop. Only IPv4 TCP frames get
 * here (class NOMERGE, MERGE or ACK); the caller short-circuits class NO.
 * The frame's headers must still sit at p->payload (classification offsets
 * are frame-relative). */
void rxgro_rx(struct RxGro *g, struct pbuf *p, const struct RxGroMeta *m);

/* Deliver every held run. MANDATORY before any core-lock release. */
void rxgro_flush_all(struct RxGro *g);

/* Hand one frame (or merged chain) to lwIP; frees the pbuf on input error.
 * Under the core lock. Owns the NSP_RX_INPUT perf bracket. */
void rxgro_deliver(struct RxGro *g, struct pbuf *p);

/* The mid-batch fairness yield's cold path: flush every held run, then
 * unlock/relock the core (NSP_RX_LOCKWAIT bracket on the relock) so a
 * queued app task gets the FIFO lock. The caller owns the fast-path check
 * (stride reached AND ss_QueueCount > 0) — the per-frame path stays
 * call-free. Owning the flush here keeps the flush-before-unlock contract
 * unforgettable. */
void rxgro_yield(struct RxGro *g);

/* Route one classified frame under the core lock: the merge engine, or
 * straight to lwIP. Folds the class-NO short-circuit so RXGRO_NO stops
 * leaking into callers. @gro: the caller's netif-level gate (lwIP's TCP
 * check off — see the header contract above). */
static inline void rxgro_input(struct RxGro *g, struct pbuf *p,
                               const struct RxGroMeta *m, BOOL gro)
{
    if (gro && m->rgm_Class != RXGRO_NO)
        rxgro_rx(g, p, m);
    else
        rxgro_deliver(g, p);
}

#endif /* LWIPAMIGA_RX_GRO_H */
