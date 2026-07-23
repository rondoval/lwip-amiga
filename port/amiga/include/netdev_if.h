/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev_if — the lwIP netif over the netdev driver ABI.
 *
 * The caller (the library's stack task, src/bsdsocket/sb_stack.c) owns the
 * exec side: it opens the device, issues NETDEV_CMD_ATTACH with
 * netdevif_stack_ops()/the glue context, then hands the attach results to
 * netdevif_create(). The glue owns everything between lwIP and the
 * driver's direct-call surface:
 *
 *   TX: pbuf chain -> scatter-gather NetDevTxDesc, L4 checksum offsets +
 *       pseudo-header seed when the driver offloads, one extra pbuf_ref
 *       held until nso_TxDone frees it.
 *   RX: driver buffers wrapped as custom pbufs (zero copy); pbuf free
 *       recycles the buffer to the driver via ndo_RxRelease.
 *
 * Locking: the nso_* callbacks (driver task) take the netstack core lock
 * before entering lwIP; linkoutput is called by lwIP under that same lock.
 */

#ifndef LWIPAMIGA_NETDEV_IF_H
#define LWIPAMIGA_NETDEV_IF_H

#include <exec/types.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>

#include <devices/netdev.h>

struct NdRxWrap;
struct ip_hdr;  /* lwip/prot/ip4.h */
struct tcp_hdr; /* lwip/prot/tcp.h */

/* Upper bound on frames processed per netstack_lock() hold in ndif_rx_input.
 * Kept >= the driver's ND_RX_BATCH so a whole driver batch lands in ONE lock
 * hold (no mid-batch re-lock). A ceiling, not a target: the RX lock cadence
 * is swept driver-side via ND_RX_BATCH + budget (see bcmgenet.c). Also sizes
 * the per-chunk verdict arrays (drop[], ndi_GroMeta). */
#define NDIF_RX_CHUNK 64

/* L2 header cache: one prebuilt Ethernet (or Ethernet+802.1Q) header
 * per recently-used destination IP, so the TX hot path skips
 * etharp_output/ethernet_output entirely. Entries revalidate
 * through the slow path every NDIF_HH_REVALIDATE hits (bounded staleness
 * against ARP re-resolution or a DHCP netmask/gateway change) and are
 * flushed on link change. All access is under the core lock. */
#define NDIF_HH_ENTRIES    4u  /* direct-mapped by dst-IP low bits */
#define NDIF_HH_HDR_MAX    18u /* Ethernet 14 + one 802.1Q tag */

/* Exact multicast RX filter: how many distinct multicast MACs the stack
 * tracks and hands the driver in one NETDEV_CMD_SET_RXFILTER. Beyond this the
 * glue falls back to NDFF_ALLMULTI (see ndif_igmp_mac_filter). Generous vs
 * real group counts; the driver imposes its own (smaller) exact-slot bound and
 * falls back to all-multi independently if the list overruns it. */
#define NDIF_MCAST_MAX     32u

struct NdHhEntry
{
    ULONG nhh_DstIp;   /* network-order dst IP; 0 = empty */
    UWORD nhh_Len;     /* 14, or 18 when the frame carries a VLAN tag */
    UWORD nhh_Left;    /* fast hits left before a slow-path revalidation */
    UBYTE nhh_Hdr[NDIF_HH_HDR_MAX];
};

/* GRO-lite: merge N consecutive in-order same-flow TCP data frames from one
 * driver RX batch into a single pbuf chain and feed lwIP once. Candidates are
 * classified in the pre-lock pass; merge contexts live only WITHIN one core
 * lock hold (flushed before every unlock), so nothing survives across
 * holds, link changes or teardown. Gated on ndi_RxOffload: with lwIP's own
 * TCP checksum check active, a rewritten merged header would fail it. */
#define NDIF_GRO_FLOWS      4u  /* direct-mapped merge contexts */
#define NDIF_GRO_MAX_FRAMES 44u /* per merge — the u16 IPH_LEN ceiling:
                                   1500 + 43*1460 = 64280 <= 65535. The
                                   driver batch (ND_RX_BATCH) is the other
                                   bound on a run; 64-frame batches split
                                   into a 44 + a 20. */

/* per-frame pre-lock classification verdict */
#define NDIF_GRO_NO      0  /* not IPv4/TCP: deliver immediately */
#define NDIF_GRO_NOMERGE 1  /* IPv4 TCP but unmergeable (SYN/FIN/RST, options,
                               padded, fragment): flush its flow first */
#define NDIF_GRO_MERGE   2  /* in-order-candidate data segment */
#define NDIF_GRO_ACK     3  /* payload-free pure ACK: coalesce to the freshest
                               per flow (a bulk sender only needs the newest
                               cumulative ackno + window) */

struct NdGroMeta
{
    ULONG ngm_SrcIp;   /* flow key, raw network order */
    ULONG ngm_DstIp;
    ULONG ngm_Ports;   /* src<<16 | dst, raw */
    union {
        ULONG ngm_Seq;   /* MERGE frame: data seqno, host order */
        ULONG ngm_AckNo; /* ACK frame:   ackno,      host order */
    };
    UWORD ngm_PayOff;  /* frame offset of TCP payload (l2 + 20 + 20) */
    UWORD ngm_PayLen;  /* TCP payload bytes (from IPH_LEN, pad excluded) */
    UBYTE ngm_Class;   /* NDIF_GRO_* */
    UBYTE ngm_Flags;   /* raw TCP flag byte (PSH propagation) */
};

struct NdGroCtx
{
    struct pbuf *ngc_Head;    /* first frame, headers intact; NULL = idle */
    struct pbuf *ngc_Tail;    /* append point (manual linking; tot_len of
                                 the chain is fixed up once at flush) */
    struct ip_hdr *ngc_Ip;    /* head's IP header (length/csum patch) */
    struct tcp_hdr *ngc_Tcp;  /* head's TCP header (ackno/wnd/PSH patch) */
    ULONG ngc_SrcIp;          /* flow key, raw */
    ULONG ngc_DstIp;
    ULONG ngc_Ports;
    union {
        ULONG ngc_NextSeq;    /* data run: host order, expected next seqno */
        ULONG ngc_AckNo;      /* ack hold: host order, held (freshest) ackno */
    };
    ULONG ngc_PayloadAdd;     /* Σ payload bytes appended after the head */
    UWORD ngc_Frames;         /* frames absorbed, head included */
    UBYTE ngc_IsAck;          /* held run is a coalesced pure-ACK, not data
                                 (valid only while ngc_Head != NULL) */
};

struct NetdevIf
{
    struct netif ndi_Netif;
    APTR ndi_Drv;                       /* nda_DrvCtx */
    const struct NetDevDrvOps *ndi_Ops; /* nda_DrvOps */
    struct NetDevCaps ndi_Caps;
    LONG ndi_VlanTci;                   /* in-band 802.1Q: -1 = no VLAN, else
                                           (pcp<<13)|(vid&0xFFF); read by the
                                           lwIP VLAN hooks. create() defaults it;
                                           the opener overrides from prefs before
                                           the interface is brought up. */

    struct NdRxWrap *ndi_FreeWraps;     /* under the core lock */
    APTR ndi_WrapStorage;
    ULONG ndi_WrapStorageSize;
    BOOL ndi_RxOffload;                 /* lwIP TCP/UDP checking disabled */
    ULONG ndi_RxNoWrap;                 /* backpressure: wrap pool empty */
    ULONG ndi_TxOversize;               /* dropped: segs > caps even coalesced */
    ULONG ndi_RxCsumBad;                /* RAW-fold verification failures */
    BOOL ndi_TxKickPending;             /* a TX batch is staged awaiting ndo_TxKick;
                                           set on submit, flushed at outermost unlock */

    /* IGMP -> exact driver RX filter. ndif_igmp_mac_filter (lwIP hook, under
     * the core lock) keeps the set of joined multicast MACs — 01:00:5e + the
     * group's low 23 bits, refcounted so the several IPv4 groups that can alias
     * one MAC share a slot — and raises ndi_RxFilterDirty on any change. The
     * stack task (sb_rxfilter_sync) snapshots the list and issues
     * NETDEV_CMD_SET_RXFILTER OFF the lock — that command runs on the driver
     * unit task, which takes the core lock in its RX path, so issuing it under
     * the lock would deadlock. Joins past NDIF_MCAST_MAX bump ndi_McastOverflow,
     * falling back to NDFF_ALLMULTI until they drain. */
    UBYTE ndi_McastList[NDIF_MCAST_MAX][6]; /* distinct joined multicast MACs */
    UWORD ndi_McastRefs[NDIF_MCAST_MAX];    /* per-MAC join refcount */
    UWORD ndi_McastCount;                   /* distinct MACs in the list */
    UWORD ndi_McastOverflow;                /* joins that didn't fit -> allmulti */
    UWORD ndi_RxFilterWant;                 /* desired NDFF_* (0 or NDFF_ALLMULTI) */
    BOOL ndi_RxFilterDirty;                 /* set changed; stack task must push */

    struct NdHhEntry ndi_Hh[NDIF_HH_ENTRIES];
    ULONG ndi_HhPrimeDst;               /* dst IP whose header linkoutput should
                                           snoop from the next slow-path frame;
                                           0 = none */

    /* GRO-lite state: meta lives here; both are unit-task exclusive during nso_RxInput */
    struct NdGroMeta ndi_GroMeta[NDIF_RX_CHUNK];
    struct NdGroCtx ndi_Gro[NDIF_GRO_FLOWS];
};

/* The stack-side callback table to pass in NetDevAttach.nda_StackOps; use
 * the struct NetdevIf pointer as nda_StackCtx. */
const struct NetDevStackOps *netdevif_stack_ops(void);

/* The RX-hold budget to declare in NetDevAttach.nda_RxHoldReq: how many
 * driver buffers the stack may pin at once, derived from the lwIP receive
 * window (port-layer knowledge the exec-side opener doesn't have). */
UWORD netdevif_rx_hold_budget(void);

/* Wire the attach results into lwIP: allocates the RX wrapper pool, adds
 * the netif (down, unconfigured), programs per-netif checksum switches
 * from the capabilities. Returns 0 on success. */
LONG netdevif_create(struct NetdevIf *ndi, APTR drvCtx,
                     const struct NetDevDrvOps *drvOps,
                     const struct NetDevCaps *caps);

/* Tear down the lwIP side (netif removed, wrapper pool freed). The caller
 * must have quiesced (NETDEV_CMD_STOP) and drained all held RX buffers
 * before this + NETDEV_CMD_DETACH. */
void netdevif_destroy(struct NetdevIf *ndi);

/* Ring the driver's deferred TX doorbell if a batch was staged since the last
 * kick (ndi_TxKickPending). Called at every outermost netstack_unlock so a
 * locked burst's frames go out with a single doorbell. Cheap no-op when idle. */
void netdevif_tx_kick(struct NetdevIf *ndi);

/* TX-pool memory for the netstack heap (routes to ndo_DmaAlloc/Free).
 * @align: 1:1 with the ABI's ndo_DmaAlloc alignment (the heap passes
 * MEM_ALIGNMENT for one-off blocks, 64 for cache-line-tiled slab arenas). */
APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size, ULONG align);
void netdevif_dma_free(struct NetdevIf *ndi, APTR ptr, ULONG size);

#endif /* LWIPAMIGA_NETDEV_IF_H */
