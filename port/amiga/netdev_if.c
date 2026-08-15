/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev interface lifecycle: attach glue between the driver ABI and lwIP —
 * netif creation/teardown, the stack-ops callback table and link events.
 * The datapaths live in netdev_rx.c / netdev_tx.c; the backend-agnostic
 * parts (identity, VLAN hooks, the IGMP multicast set) in netif_base.c.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/snmp.h>
#include <netif/ethernet.h>

#include "netdev_priv.h"
#include "netstack.h"

#define NDIF_MIN_WRAPS 64

/* ---------------------------------------------------- L2 header cache --- */

void netdevif_hh_invalidate(struct NetdevIf *ndi)
{
    for (ULONG i = 0; i < NDIF_HH_ENTRIES; i++)
    {
        ndi->ndi_Hh[i].nhh_DstIp = 0;
        ndi->ndi_Hh[i].nhh_Left = 0;
    }
    ndi->ndi_HhPrimeDst = 0;
}

/* -------------------------------------------------------- link events --- */

static void ndif_link_change(APTR stackctx, const struct NetDevLinkState *state)
{
    Kprintf("[netdevif] %s: flags 0x%08lx\n", __func__, (ULONG)state->ndls_Flags);
    struct NetdevIf *ndi = stackctx;

    netstack_lock();
    /* a link transition may mean a new peer/port: drop the L2 header cache */
    netdevif_hh_invalidate(ndi);
    if (state->ndls_Flags & NDLF_UP)
        netif_set_link_up(&ndi->ndi_Base.nib_Netif);
    else
        netif_set_link_down(&ndi->ndi_Base.nib_Netif);
    netstack_unlock();
}

/* ------------------------------------------------------------ plumbing --- */

static const struct NetDevStackOps ndif_stack_ops = {
    ndif_rx_input,
    ndif_tx_done,
    ndif_link_change,
};

const struct NetDevStackOps *netdevif_stack_ops(void)
{
    KprintfT("[netdevif] %s\n", __func__);
    return &ndif_stack_ops;
}

/* Frames the stack can pin at once: one full receive window per expected
 * concurrent bulk stream plus in-flight slack.
 * = 4 x ceil(TCP_WND / TCP_MSS) + 64 = 784 with the current lwipopts. */
#define NDIF_RX_HOLD_STREAMS 4
#define NDIF_RX_HOLD_SLACK   64

UWORD netdevif_rx_hold_budget(void)
{
    ULONG wndFrames = (TCP_WND + TCP_MSS - 1) / TCP_MSS;
    return (UWORD)(NDIF_RX_HOLD_STREAMS * wndFrames + NDIF_RX_HOLD_SLACK);
}

/* Max frames per nso_RxInput the stack accepts = its per-chunk array capacity. */
UWORD netdevif_rx_batch(void)
{
    return NDIF_RX_CHUNK;
}

APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size, ULONG align)
{
    return ndi->ndi_Ops->ndo_DmaAlloc(ndi->ndi_Drv, size, align);
}

void netdevif_dma_free(struct NetdevIf *ndi, APTR ptr, ULONG size)
{
    if (ndi == NULL)
    {
        Kprintf("[netdevif] DMA free after detach — leaked %lu bytes\n", size);
        return;
    }
    ndi->ndi_Ops->ndo_DmaFree(ndi->ndi_Drv, ptr, size);
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
#if LWIP_IGMP
    /* exact multicast filtering: joins land as driver RX-filter updates */
    netif_set_igmp_mac_filter(nif, netifbase_igmp_mac_filter);
#endif

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
    netifbase_init(&ndi->ndi_Base, NIF_KIND_NETDEV);
    ndi->ndi_Base.nib_HwMtu = caps->ndc_Mtu;
    ndi->ndi_Base.nib_NumRead = caps->ndc_RxRingSlots;
    ndi->ndi_Base.nib_NumWrite = caps->ndc_TxRingSlots;
    ndi->ndi_Drv = drvCtx;
    ndi->ndi_Ops = drvOps;
    ndi->ndi_Caps = *caps;
    ndi->ndi_RxNoWrap = 0;
    ndi->ndi_TxOversize = 0;
    ndi->ndi_RxCsumBad = 0;
    ndi->ndi_TxKickPending = FALSE;
    netdevif_hh_invalidate(ndi);
    for (ULONG i = 0; i < NDIF_GRO_FLOWS; i++)
        ndi->ndi_Gro[i].ngc_Head = NULL; /* contexts idle outside lock holds */

    /* RX wrappers: one per buffer the stack can possibly hold. The driver
     * advertises its pool size; a wrap count below it silently re-imposes
     * the old limit as ndi_RxNoWrap backpressure. Ring*2 is the fallback
     * for drivers that leave ndc_RxPoolBufs 0. */
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
    ndi->ndi_WrapsOut = 0;
    for (ULONG i = 0; i < count; i++, w++)
    {
        w->nrw_If = ndi;
        w->nrw_Next = ndi->ndi_FreeWraps;
        ndi->ndi_FreeWraps = w;
    }

    /* Deferred TX-reclaim ring (see netdevif_tx_reclaim), sized to cover the
     * driver's advertised in-flight TX ceiling. next-pow2(want + 1) so an SPSC
     * ring holding `want` cookies has its reserved slot; same idiom as the
     * driver's recycle ring. Falls back to 2x the BD ring, then a floor, when
     * the driver leaves ndc_TxInFlightMax 0. */
    ULONG want = ndi->ndi_Caps.ndc_TxInFlightMax;
    if (want == 0)
        want = (ULONG)ndi->ndi_Caps.ndc_TxRingSlots * 2;
    if (want < NDIF_TX_FREE_MIN)
        want = NDIF_TX_FREE_MIN;
    ULONG ring_n = 1;
    while (ring_n < want + 1)
        ring_n <<= 1;
    ndi->ndi_TxFree = AllocMem(ring_n * sizeof(APTR), MEMF_PUBLIC | MEMF_CLEAR);
    if (ndi->ndi_TxFree == NULL)
    {
        FreeMem(ndi->ndi_WrapStorage, ndi->ndi_WrapStorageSize);
        return -1;
    }
    ndi->ndi_TxFreeMask = ring_n - 1;
    ndi->ndi_TxFreeProd = 0;
    ndi->ndi_TxFreeCons = 0;
    ndi->ndi_TxFreeOverflow = 0;

    netstack_lock();
    struct netif *added = netif_add_noaddr(&ndi->ndi_Base.nib_Netif, ndi,
                                           ndif_netif_init, ethernet_input);
    if (added != NULL)
    {
        netstack.ns_ActiveNetdev = ndi;
        netstack.ns_ActiveIf = &ndi->ndi_Base;
    }
    netstack_unlock();

    if (added == NULL)
    {
        FreeMem(ndi->ndi_TxFree, (ndi->ndi_TxFreeMask + 1) * sizeof(APTR));
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
    /* Free the cookies STOP completed into the reclaim ring before the slab
     * arenas go back to the driver. (The netstack_lock entry-drain above already
     * did this while ns_ActiveNetdev == ndi; the explicit call keeps the ordering
     * vs. netstack_slab_detach self-evident and is a no-op if already drained.) */
    netdevif_tx_reclaim(ndi);
    netif_remove(&ndi->ndi_Base.nib_Netif);
    if (netstack.ns_ActiveNetdev == ndi)
    {
        /* return the slab arenas while the ABI pointer is still valid */
        netstack_slab_detach(ndi);
        netstack.ns_ActiveNetdev = NULL;
        netstack.ns_ActiveIf = NULL;
    }

    /* Wrap-pool disposition, decided under the lock (wrap frees run under it
     * too, so ndi_WrapsOut is exact). Sockets may still hold RX wraps — their
     * pbufs sit in receive queues across a forced RemoveNetInterface and are
     * freed only when the app drains or closes, possibly after a successor
     * interface reuses this NetdevIf. The pool must then outlive this
     * interface: mark every wrap dead (nrw_If = NULL turns its free into a
     * no-op — the driver reclaims the buffers itself at forced detach) and
     * leak the storage. */
    BOOL leakWraps = ndi->ndi_WrapsOut != 0;
    if (leakWraps)
    {
        Kprintf("[netdevif] %lu RX wraps still held by sockets — wrap pool leaked\n",
                ndi->ndi_WrapsOut);
        struct NdRxWrap *w = ndi->ndi_WrapStorage;
        for (ULONG i = 0; i < ndi->ndi_WrapStorageSize / sizeof(struct NdRxWrap); i++)
            w[i].nrw_If = NULL;
    }
    netstack_unlock();

    if (!leakWraps)
        FreeMem(ndi->ndi_WrapStorage, ndi->ndi_WrapStorageSize);
    ndi->ndi_WrapStorage = NULL;
    ndi->ndi_WrapStorageSize = 0;
    ndi->ndi_FreeWraps = NULL;
    ndi->ndi_WrapsOut = 0;
    FreeMem(ndi->ndi_TxFree, (ndi->ndi_TxFreeMask + 1) * sizeof(APTR));
    ndi->ndi_TxFree = NULL;
}
