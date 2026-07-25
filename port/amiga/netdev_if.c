/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev interface lifecycle: attach glue between the driver ABI and lwIP —
 * netif creation/teardown, the stack-ops callback table, link events and
 * the in-band VLAN hooks. The datapaths live in netdev_rx.c / netdev_tx.c.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/snmp.h>
#include <netif/ethernet.h>

#include "netdev_priv.h"
#include "netstack.h"
#include "netstack_lwiphooks.h"

#define NDIF_MIN_WRAPS 64

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

/* ------------------------------------------------------- IGMP RX filter --- */
/* lwIP calls igmp_mac_filter under the core lock on the first join / last
 * leave of every multicast group (the all-systems group included, added at
 * igmp_start). We keep the exact set of joined multicast MACs here and let the
 * stack task push it to the driver; see the ndi_Mcast* comment in netdev_if.h. */

/* IPv4 multicast group -> Ethernet MAC: 01:00:5e | low 23 bits of the group. */
static void ndif_mcast_mac(const ip4_addr_t *group, UBYTE mac[6])
{
    ULONG g = lwip_ntohl(ip4_addr_get_u32(group));
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5e;
    mac[3] = (UBYTE)((g >> 16) & 0x7f);
    mac[4] = (UBYTE)((g >> 8) & 0xff);
    mac[5] = (UBYTE)(g & 0xff);
}

static BOOL ndif_mac_eq(const UBYTE *a, const UBYTE *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return FALSE;
    return TRUE;
}

static err_t ndif_igmp_mac_filter(struct netif *nif, const ip4_addr_t *group,
                                  enum netif_mac_filter_action action)
{
    struct NetdevIf *ndi = nif->state;
    UBYTE mac[6];
    ndif_mcast_mac(group, mac);

    LONG idx = -1;
    for (UWORD i = 0; i < ndi->ndi_McastCount; i++)
    {
        if (ndif_mac_eq(ndi->ndi_McastList[i], mac))
        {
            idx = (LONG)i;
            break;
        }
    }

    UWORD oldWant = ndi->ndi_RxFilterWant;
    BOOL listChanged = FALSE;

    if (action == NETIF_ADD_MAC_FILTER)
    {
        if (idx >= 0)
        {
            ndi->ndi_McastRefs[idx]++; /* another group aliases this MAC */
        }
        else if (ndi->ndi_McastCount < NDIF_MCAST_MAX)
        {
            UWORD n = ndi->ndi_McastCount++;
            for (int i = 0; i < 6; i++)
                ndi->ndi_McastList[n][i] = mac[i];
            ndi->ndi_McastRefs[n] = 1;
            listChanged = TRUE;
        }
        else
        {
            ndi->ndi_McastOverflow++; /* no slot: covered by the all-multi fallback */
        }
    }
    else /* NETIF_DEL_MAC_FILTER */
    {
        if (idx >= 0)
        {
            if (--ndi->ndi_McastRefs[idx] == 0)
            {
                UWORD last = --ndi->ndi_McastCount; /* swap-remove */
                for (int i = 0; i < 6; i++)
                    ndi->ndi_McastList[idx][i] = ndi->ndi_McastList[last][i];
                ndi->ndi_McastRefs[idx] = ndi->ndi_McastRefs[last];
                listChanged = TRUE;
            }
        }
        else if (ndi->ndi_McastOverflow > 0)
        {
            ndi->ndi_McastOverflow--;
        }
    }

    UWORD want = ndi->ndi_McastOverflow > 0 ? NDFF_ALLMULTI : 0;
    ndi->ndi_RxFilterWant = want;
    if (listChanged || want != oldWant)
        ndi->ndi_RxFilterDirty = TRUE;
    return ERR_OK;
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
    netif_set_igmp_mac_filter(nif, ndif_igmp_mac_filter);
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
    ndi->ndi_Drv = drvCtx;
    ndi->ndi_Ops = drvOps;
    ndi->ndi_Caps = *caps;
    ndi->ndi_RxNoWrap = 0;
    ndi->ndi_TxOversize = 0;
    ndi->ndi_RxCsumBad = 0;
    ndi->ndi_TxKickPending = FALSE;
    ndi->ndi_McastCount = 0;
    ndi->ndi_McastOverflow = 0;
    ndi->ndi_RxFilterWant = 0;
    ndi->ndi_RxFilterDirty = FALSE;
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
    struct netif *added = netif_add_noaddr(&ndi->ndi_Netif, ndi,
                                           ndif_netif_init, ethernet_input);
    if (added != NULL)
        netstack.ns_ActiveNetdev = ndi;
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
    FreeMem(ndi->ndi_TxFree, (ndi->ndi_TxFreeMask + 1) * sizeof(APTR));
    ndi->ndi_TxFree = NULL;
}
