/* SPDX-License-Identifier: BSD-3-Clause */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/etharp.h>
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
/* Upper bound on frames processed per netstack_lock() hold. Kept >= the
 * driver's ND_RX_BATCH so a whole driver batch lands in ONE lock hold (no
 * mid-batch re-lock). This is a ceiling, not a target: the RX lock cadence is
 * swept driver-side via ND_RX_BATCH + budget (see bcmgenet.c). 64 leaves head-
 * room for that sweep without ever splitting a batch. Sizes the drop[] VLA. */
#define NDIF_RX_CHUNK       64

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
#ifdef DEBUG
static UWORD ndif_sum_range(const UBYTE *data, ULONG len);
#endif

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

    while (consumed < count)
    {
        const struct NetDevRxDesc *cd = &descs[consumed];
        ULONG chunk = count - consumed;
        if (chunk > NDIF_RX_CHUNK)
            chunk = NDIF_RX_CHUNK;

        /* Checksum verdicts BEFORE taking the core lock — the fold reads
         * only frame bytes plus the IP header, and nso_RxInput runs on the
         * driver's unit task alone, so nothing here needs protection.
         * lwIP's TCP/UDP checking is off when the driver offloads RX csum;
         * frames without the VALID verdict get their RAW sum folded here. */
        UBYTE drop[NDIF_RX_CHUNK];
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];
            drop[i] = ndi->ndi_RxOffload &&
                      !(d->nrd_Flags & NDRF_CSUM_VALID) &&
                      (d->nrd_Flags & NDRF_CSUM_RAW) &&
                      !ndif_rx_csum_ok(d, d->nrd_CsumRaw);
#ifdef DEBUG
            if (drop[i])
            {
                /* debug cross-check of the hardware raw sum (the RXCHK
                 * decode was mapped live before it was trusted) */
                UWORD sw = ndif_sum_range(d->nrd_Data + SIZEOF_ETH_HDR,
                                          d->nrd_Len - SIZEOF_ETH_HDR);
                BOOL ok = ndif_rx_csum_ok(d, sw);
                Kprintf("[netdevif] RX csum: hw 0x%04lx sw 0x%04lx len %lu %s\n",
                        (ULONG)d->nrd_CsumRaw, (ULONG)sw, (ULONG)d->nrd_Len,
                        (ULONG)(ok ? "pass(sw)" : "DROP"));
                if (ok)
                    drop[i] = 0;
            }
#endif
        }

        netstack_lock();
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];

            /* a bad frame is consumed and its buffer released immediately
             * (releases stay under the lock: it is the recycle ring's
             * single-producer guarantee) */
            if (drop[i])
            {
                ndi->ndi_RxCsumBad++;
                ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, d->nrd_Cookie);
                consumed++;
                continue;
            }

            struct NdRxWrap *w = ndi->ndi_FreeWraps;
            if (w == NULL)
            {
                ndi->ndi_RxNoWrap++;
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

            if (ndi->ndi_Netif.input(p, &ndi->ndi_Netif) != ERR_OK)
                pbuf_free(p);

            /* Fairness yield (see NDIF_RX_YIELD_STRIDE): under contention, hand
             * the FIFO lock to a queued app task so it drains its socket queue
             * mid-batch instead of starving. The loop head re-reads
             * ndi_FreeWraps after the relock, so wraps freed meanwhile are seen. */
            if (++since_yield >= NDIF_RX_YIELD_STRIDE &&
                netstack.ns_Core.ss_QueueCount > 0)
            {
                since_yield = 0;
                netstack_unlock();
                netstack_lock();
            }
        }
        netstack_unlock();
    }

    return consumed;
}

/* ---------------------------------------------------------------- TX --- */

static void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count)
{
    // KprintfH("[netdevif] %s: count %lu\n", __func__, count);
    (void)stackctx;

    netstack_lock();
    for (ULONG i = 0; i < count; i++)
        pbuf_free((struct pbuf *)cookies[i]);
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

#ifdef DEBUG
/* 1's-complement sum over a byte range, network order, odd tail
 * zero-padded, folded to 16 bits. */
static UWORD ndif_sum_range(const UBYTE *data, ULONG len)
{
    ULONG sum = 0;
    while (len > 1)
    {
        sum += ((ULONG)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len != 0)
        sum += (ULONG)data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (UWORD)sum;
}
#endif /* DEBUG (ndif_sum_range) */

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

    struct NetDevSg segs[NDIF_MAX_STACK_SEGS];
    UWORD nsegs = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next)
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
    desc.ntd_Cookie = p;

    if (ndi->ndi_Caps.ndc_Features & NDCF_TX_L4CSUM)
    {
        ULONG proto = ndif_l4_offsets(p, &desc.ntd_CsumStart, &desc.ntd_CsumOffset);
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
     * reference right after we return. */
    pbuf_ref(p);

    LONG accepted = ndi->ndi_Ops->ndo_TxSubmit(ndi->ndi_Drv, &desc, 1);
    if (accepted != 1)
    {
        pbuf_free(p);
        return ERR_MEM; /* ring full; TCP retries on timer */
    }

    return ERR_OK;
}

/* -------------------------------------------------------- link events --- */

static void ndif_link_change(APTR stackctx, const struct NetDevLinkState *state)
{
    Kprintf("[netdevif] %s: flags 0x%08lx\n", __func__, (ULONG)state->ndls_Flags);
    struct NetdevIf *ndi = stackctx;

    netstack_lock();
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

APTR netdevif_dma_alloc(struct NetdevIf *ndi, ULONG size)
{
    // KprintfH("[netdevif] %s: size %lu\n", __func__, size);
    return ndi->ndi_Ops->ndo_DmaAlloc(ndi->ndi_Drv, size, MEM_ALIGNMENT);
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

static err_t ndif_netif_init(struct netif *nif)
{
    Kprintf("[netdevif] %s: nif 0x%08lx\n", __func__, (ULONG)nif);
    struct NetdevIf *ndi = nif->state;

    nif->name[0] = 'n';
    nif->name[1] = 'd';
    nif->output = etharp_output;
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
        netstack.ns_ActiveNetdev = NULL;
    netstack_unlock();

    FreeMem(ndi->ndi_WrapStorage, ndi->ndi_WrapStorageSize);
    ndi->ndi_WrapStorage = NULL;
    ndi->ndi_FreeWraps = NULL;
}
