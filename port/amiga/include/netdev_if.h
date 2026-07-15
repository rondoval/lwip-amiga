/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev_if — the lwIP netif over the netdev driver ABI.
 *
 * The caller (interface-config code, Phase 2) owns the exec side: it opens
 * the device, issues NETDEV_CMD_ATTACH with netdevif_stack_ops()/the glue
 * context, then hands the attach results to netdevif_create(). The glue
 * owns everything between lwIP and the driver's direct-call surface:
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

#include <netdev.h>

struct NdRxWrap;

struct NetdevIf
{
    struct netif ndi_Netif;
    APTR ndi_Drv;                       /* nda_DrvCtx */
    const struct NetDevDrvOps *ndi_Ops; /* nda_DrvOps */
    struct NetDevCaps ndi_Caps;
    LONG ndi_VlanTci;                   /* in-band 802.1Q: -1 = no VLAN, else
                                           (pcp<<13)|(vid&0xFFF); read by the
                                           lwIP VLAN hooks. Set before create. */

    struct NdRxWrap *ndi_FreeWraps;     /* under the core lock */
    APTR ndi_WrapStorage;
    ULONG ndi_WrapStorageSize;
    BOOL ndi_RxOffload;                 /* lwIP TCP/UDP checking disabled */
    ULONG ndi_RxNoWrap;                 /* backpressure: wrap pool empty */
    ULONG ndi_TxOversize;               /* dropped: segs > caps even coalesced */
    ULONG ndi_RxCsumBad;                /* RAW-fold verification failures */
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

/* TX-pool memory for the netstack heap (routes to ndo_DmaAlloc/Free). */
APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size);
void netdevif_dma_free(struct NetdevIf *ndi, APTR ptr, ULONG size);

#endif /* LWIPAMIGA_NETDEV_IF_H */
