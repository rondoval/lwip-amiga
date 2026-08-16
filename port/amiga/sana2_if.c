/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SANA-II interface lifecycle: netif creation/teardown, the OpenDevice
 * buffer-management tag list and the driver-called copy callbacks. The
 * datapaths live in sana2_tx.c / sana2_pump.c.
 */

#include "netstack_sys.h"

#include <debug.h>
#include <memory.h>

#include <lwip/etharp.h>
#include <lwip/snmp.h>
#include <netif/ethernet.h>

#include "netstack.h"
#include "sana2_priv.h"

/* ------------------------------------------------------ copy callbacks --- */
/* Driver-called, possibly from interrupt context per the SANA-II spec: pure
 * copies only — no Exec calls (emu68-common's memcpy routes through
 * CopyMem, which is not interrupt-callable), no locks, no allocation.
 * Longword loop when both sides share alignment (the common case: pbuf
 * payloads are MEM_ALIGNMENT-aligned and driver staging buffers are fresh
 * allocations); byte loop otherwise. */
static void s2if_copy(UBYTE *dst, const UBYTE *src, ULONG n)
{
    if ((((ULONG)dst ^ (ULONG)src) & 3) == 0)
    {
        while (((ULONG)dst & 3) != 0 && n > 0)
        {
            *dst++ = *src++;
            n--;
        }
        ULONG *dl = (ULONG *)dst;
        const ULONG *sl = (const ULONG *)src;
        for (; n >= 16; n -= 16)
        {
            dl[0] = sl[0];
            dl[1] = sl[1];
            dl[2] = sl[2];
            dl[3] = sl[3];
            dl += 4;
            sl += 4;
        }
        for (; n >= 4; n -= 4)
            *dl++ = *sl++;
        dst = (UBYTE *)dl;
        src = (const UBYTE *)sl;
    }
    while (n > 0)
    {
        *dst++ = *src++;
        n--;
    }
}

/* RX: the driver hands us one received frame. `to` is the CMD_READ's
 * ios2_Data cookie verbatim — our S2RxReq. `len` may exceed the true frame
 * length by up to 3 bytes (Miami-workaround drivers round up to a longword
 * multiple); srx_Cap includes that slack, so the bound check only trips on
 * a genuinely oversized frame. */
BOOL s2if_copy_to_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"))
{
    struct S2RxReq *r = to;
    if (len > r->srx_Cap)
        return FALSE; /* refuse rather than overrun the pbuf */
    s2if_copy(r->srx_Dst, from, len);
    return TRUE;
}

/* TX: the driver pulls the frame body. `from` is the write's ios2_Data
 * cookie — the ref'd pbuf chain, walked from logical offset 14 (cooked mode:
 * the Ethernet header travels in ios2_DstAddr/ios2_PacketType instead).
 * A `len` rounded past the frame's real end is zero-padded, never failed. */
BOOL s2if_copy_from_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"))
{
    const struct pbuf *p = from;
    UBYTE *dst = to;
    ULONG off = SIZEOF_ETH_HDR;
    ULONG want = len;

    while (want > 0 && p != NULL)
    {
        if (off >= p->len)
        {
            off -= p->len;
            p = p->next;
            continue;
        }
        ULONG chunk = p->len - off;
        if (chunk > want)
            chunk = want;
        s2if_copy(dst, (const UBYTE *)p->payload + off, chunk);
        dst += chunk;
        want -= chunk;
        p = p->next;
        off = 0;
    }
    while (want > 0)
    {
        *dst++ = 0;
        want--;
    }
    return TRUE;
}

const struct TagItem *sana2if_buffer_tags(void)
{
    static const struct TagItem tags[] = {
        { S2_CopyToBuff, (ULONG)s2if_copy_to_buff },
        { S2_CopyFromBuff, (ULONG)s2if_copy_from_buff },
        { TAG_DONE, 0 },
    };
    return tags;
}

/* ----------------------------------------------------------- lifecycle --- */

static err_t s2if_netif_init(struct netif *nif)
{
    Kprintf("[sana2if] %s: nif 0x%08lx\n", __func__, (ULONG)nif);
    struct Sana2If *s2i = nif->state;

    nif->name[0] = 's';
    nif->name[1] = '2';
    nif->output = etharp_output; /* no L2 header cache: SANA-II pays a copy
                                    per frame anyway, the etharp path is not
                                    the bottleneck */
    nif->linkoutput = s2if_linkoutput;
    nif->mtu = s2i->s2i_Mtu;
    nif->hwaddr_len = ETH_HWADDR_LEN;
    for (int i = 0; i < ETH_HWADDR_LEN; i++)
        nif->hwaddr[i] = s2i->s2i_Mac[i];
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;
#if LWIP_IGMP
    /* joins land as S2_ADDMULTICASTADDRESS deltas (sb_sana_mcast_sync) */
    netif_set_igmp_mac_filter(nif, netifbase_igmp_mac_filter);
#endif

    /* SANA-II has no checksum offload of any kind: lwIP generates and
     * verifies everything in software — EXCEPT inbound TCP, which the pump
     * verifies pre-lock (s2if_rx_csum_ok) so the GRO-lite merge can rewrite
     * headers without failing lwIP's re-check. IP + UDP checking stays with
     * lwIP; fragmented TCP passes unverified past reassembly (the documented
     * netdev-parity gap). */
    NETIF_SET_CHECKSUM_CTRL(nif, (UWORD)(NETIF_CHECKSUM_ENABLE_ALL &
                                         ~NETIF_CHECKSUM_CHECK_TCP));

    MIB2_INIT_NETIF(nif, snmp_ifType_ethernet_csmacd, s2i->s2i_Bps);
    return ERR_OK;
}

LONG sana2if_create(struct Sana2If *s2i, struct Device *dev, struct Unit *unit,
                    APTR bufMgmt, const UBYTE mac[6], UWORD mtu, UWORD hwMtu,
                    ULONG bps, LONG vlanTci)
{
    memset(s2i, 0, sizeof(*s2i));
    netifbase_init(&s2i->s2i_Base, NIF_KIND_SANA2);
    s2i->s2i_Base.nib_HwMtu = hwMtu;
    ULONG nIp4, nArp, nVlan;
    s2if_rx_classes(vlanTci, &nIp4, &nArp, &nVlan);
    s2i->s2i_Base.nib_NumRead = (UWORD)(nIp4 + nArp + nVlan);
    s2i->s2i_Base.nib_NumWrite = S2IF_TX_REQS;
    /* the pump sizes RX pbufs and decides on the 0x8100 read class from
     * this before the identity stamp re-sets it */
    s2i->s2i_Base.nib_VlanTci = vlanTci;

    s2i->s2i_Device = dev;
    s2i->s2i_Unit = unit;
    s2i->s2i_BufMgmt = bufMgmt;
    s2i->s2i_Bps = bps;
    s2i->s2i_Mtu = mtu;
    for (int i = 0; i < 6; i++)
        s2i->s2i_Mac[i] = mac[i];
    rxgro_init(&s2i->s2i_Gro, &s2i->s2i_Base.nib_Netif);

    /* Write-request pool. Cloned identity per the sanctioned duplication
     * (io_Device/io_Unit/ios2_BufferManagement from the opened request);
     * reply ports are stamped by the pump, which owns them. */
    s2i->s2i_TxStorageSize = S2IF_TX_REQS * sizeof(struct S2TxReq);
    s2i->s2i_TxStorage = AllocMem(s2i->s2i_TxStorageSize, MEMF_PUBLIC | MEMF_CLEAR);
    if (s2i->s2i_TxStorage == NULL)
        return -1;
    struct S2TxReq *t = s2i->s2i_TxStorage;
    for (ULONG i = 0; i < S2IF_TX_REQS; i++, t++)
    {
        t->stx_Io.ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
        t->stx_Io.ios2_Req.io_Device = dev;
        t->stx_Io.ios2_Req.io_Unit = unit;
        t->stx_Io.ios2_BufferManagement = bufMgmt;
        t->stx_Next = s2i->s2i_TxFree;
        s2i->s2i_TxFree = t;
    }
    s2i->s2i_TxStagedTail = &s2i->s2i_TxStagedHead;

    netstack_lock();
    struct netif *added = netif_add_noaddr(&s2i->s2i_Base.nib_Netif, s2i,
                                           s2if_netif_init, ethernet_input);
    if (added != NULL)
    {
        netstack.ns_ActiveSana2 = s2i;
        netstack.ns_ActiveIf = &s2i->s2i_Base;
    }
    netstack_unlock();

    if (added == NULL)
    {
        FreeMem(s2i->s2i_TxStorage, s2i->s2i_TxStorageSize);
        s2i->s2i_TxStorage = NULL;
        return -1;
    }

    Kprintf("[sana2if] netif s2 up: mtu %lu (hw %lu), %lu bps\n",
            (ULONG)mtu, (ULONG)hwMtu, bps);
    return 0;
}

void sana2if_destroy(struct Sana2If *s2i)
{
    Kprintf("[sana2if] %s: s2i 0x%08lx\n", __func__, (ULONG)s2i);
    netstack_lock();
    /* pump stop drained everything; a violation here means a write request
     * (and its pbuf ref) leaks with the pool below */
    if (s2i->s2i_TxInFlight != 0 || s2i->s2i_TxStagedHead != NULL)
        Kprintf("[sana2if] destroy with TX outstanding (%lu in flight)!\n",
                s2i->s2i_TxInFlight);
    netif_remove(&s2i->s2i_Base.nib_Netif);
    if (netstack.ns_ActiveSana2 == s2i)
    {
        netstack.ns_ActiveSana2 = NULL;
        netstack.ns_ActiveIf = NULL;
    }
    netstack_unlock();

    /* Delivered RX pbufs a socket still holds are plain heap pbufs — the
     * heap outlives the interface, so unlike netdev's wrap pool there is
     * nothing to leak or mark dead here. */
    if (s2i->s2i_TxStorage != NULL)
    {
        FreeMem(s2i->s2i_TxStorage, s2i->s2i_TxStorageSize);
        s2i->s2i_TxStorage = NULL;
    }
    s2i->s2i_TxFree = NULL;
}
