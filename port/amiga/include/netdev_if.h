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
#include "netif_base.h"
#include "rx_gro.h"

struct NdRxWrap;

/* Upper bound on frames processed per netstack_lock() hold in ndif_rx_input,
 * and the size of the per-chunk verdict arrays (drop[], ndi_GroMeta). This is
 * the stack's capacity; it is PROPAGATED to the driver at ATTACH via
 * nda_RxBatch (netdevif_rx_batch), which caps the driver's flush granularity so
 * a whole driver batch lands in ONE lock hold. ndif_rx_input still clamps each
 * pass to this and re-locks, so a driver that ignores it only pays extra holds,
 * never an overflow. */
#define NDIF_RX_CHUNK 64

/* L2 header cache: one prebuilt Ethernet (or Ethernet+802.1Q) header
 * per recently-used destination IP, so the TX hot path skips
 * etharp_output/ethernet_output entirely. Entries revalidate
 * through the slow path every NDIF_HH_REVALIDATE hits (bounded staleness
 * against ARP re-resolution or a DHCP netmask/gateway change) and are
 * flushed on link change. All access is under the core lock. */
#define NDIF_HH_ENTRIES    4u  /* direct-mapped by dst-IP low bits */
#define NDIF_HH_HDR_MAX    18u /* Ethernet 14 + one 802.1Q tag */

struct NdHhEntry
{
    ULONG nhh_DstIp;   /* network-order dst IP; 0 = empty */
    UWORD nhh_Len;     /* 14, or 18 when the frame carries a VLAN tag */
    UWORD nhh_Left;    /* fast hits left before a slow-path revalidation */
    UBYTE nhh_Hdr[NDIF_HH_HDR_MAX];
};

/* Deferred TX reclaim: nso_TxDone (unit task) enqueues completed pbuf cookies
 * onto ndi_TxFree instead of freeing them under a dedicated lock hold; the
 * frees run in bulk at the next outermost netstack_lock, folded into a hold
 * some task already owns. Single-producer (unit task) / single-consumer (the
 * core-lock holder, serialized by the lock). The ring is sized at create() to
 * cover the driver's advertised in-flight ceiling (ndc_TxInFlightMax) so, like
 * the driver's own recycle ring, it can never fill; the inline-free backstop in
 * ndif_tx_done is the guarantee if a driver ever under-reports. This floor is
 * the fallback when the driver advertises 0 (pre-field). */
#define NDIF_TX_FREE_MIN 256u

struct NetdevIf
{
    struct NetIfBase ndi_Base;          /* must stay first: netif->state points
                                           at this struct, the base, and the
                                           netif all at once (netif_base.h) */
    APTR ndi_Drv;                       /* nda_DrvCtx */
    const struct NetDevDrvOps *ndi_Ops; /* nda_DrvOps */
    struct NetDevCaps ndi_Caps;

    struct NdRxWrap *ndi_FreeWraps;     /* under the core lock */
    ULONG ndi_WrapsOut;                 /* wraps lent to lwIP (under the core
                                           lock). Nonzero at destroy = sockets
                                           still hold RX pbufs past a forced
                                           remove -> the pool is marked dead
                                           and leaked (see netdevif_destroy) */
    APTR ndi_WrapStorage;
    ULONG ndi_WrapStorageSize;
    BOOL ndi_RxOffload;                 /* lwIP TCP/UDP checking disabled */
    ULONG ndi_RxNoWrap;                 /* backpressure: wrap pool empty */
    ULONG ndi_TxOversize;               /* dropped: segs > caps even coalesced */
    ULONG ndi_RxCsumBad;                /* RAW-fold verification failures */
    BOOL ndi_TxKickPending;             /* a TX batch is staged awaiting ndo_TxKick;
                                           set on submit, flushed at outermost unlock */

    /* Deferred TX-reclaim SPSC ring (see NDIF_TX_FREE_RING_N above). */
    APTR *ndi_TxFree;                   /* ring of completed TX pbuf cookies */
    ULONG ndi_TxFreeMask;               /* NDIF_TX_FREE_RING_N - 1 */
    volatile ULONG ndi_TxFreeProd;      /* unit task (producer) */
    volatile ULONG ndi_TxFreeCons;      /* core-lock holder (consumer) */
    ULONG ndi_TxFreeOverflow;           /* backstop: inline frees on a full ring
                                           (unreachable at correct sizing) */

    struct NdHhEntry ndi_Hh[NDIF_HH_ENTRIES];
    ULONG ndi_HhPrimeDst;               /* dst IP whose header linkoutput should
                                           snoop from the next slow-path frame;
                                           0 = none */

    /* GRO-lite state (rx_gro.h; gated on ndi_RxOffload — with lwIP's own TCP
     * checksum check active, a rewritten merged header would fail it). Both
     * are unit-task exclusive during nso_RxInput. */
    struct RxGroMeta ndi_GroMeta[NDIF_RX_CHUNK];
    struct RxGro ndi_Gro;
};

/* The stack-side callback table to pass in NetDevAttach.nda_StackOps; use
 * the struct NetdevIf pointer as nda_StackCtx. */
const struct NetDevStackOps *netdevif_stack_ops(void);

/* The RX-hold budget to declare in NetDevAttach.nda_RxHoldReq: how many
 * driver buffers the stack may pin at once, derived from the lwIP receive
 * window (port-layer knowledge the exec-side opener doesn't have). */
UWORD netdevif_rx_hold_budget(void);

/* The RX-input batch to declare in NetDevAttach.nda_RxBatch: the max frames the
 * stack accepts per nso_RxInput call (its per-hold verdict/GRO array capacity).
 * The driver caps its flush granularity to this. */
UWORD netdevif_rx_batch(void);

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

/* Drain the deferred TX-reclaim ring, freeing every completed pbuf cookie
 * nso_TxDone (ndif_tx_done) staged since the last drain. Called under the core
 * lock at every outermost netstack_lock entry (and once from netdevif_destroy).
 * Cheap no-op when the ring is empty. */
void netdevif_tx_reclaim(struct NetdevIf *ndi);

/* TX-pool memory for the netstack heap (routes to ndo_DmaAlloc/Free).
 * @align: 1:1 with the ABI's ndo_DmaAlloc alignment (the heap passes
 * MEM_ALIGNMENT for one-off blocks, 64 for cache-line-tiled slab arenas). */
APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size, ULONG align);
void netdevif_dma_free(struct NetdevIf *ndi, APTR ptr, ULONG size);

/* Drop the whole L2 header cache (and the pending snoop prime). Needed
 * whenever an IP->MAC binding changes underneath it: link transitions and
 * manual ARP table mutations (SIOCSARP/SIOCDARP). Caller holds the core
 * lock; the cache refills lazily through the slow path. */
void netdevif_hh_invalidate(struct NetdevIf *ndi);

#endif /* LWIPAMIGA_NETDEV_IF_H */
