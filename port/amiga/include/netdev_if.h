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

    struct NdHhEntry ndi_Hh[NDIF_HH_ENTRIES];
    ULONG ndi_HhPrimeDst;               /* dst IP whose header linkoutput should
                                           snoop from the next slow-path frame;
                                           0 = none */
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
