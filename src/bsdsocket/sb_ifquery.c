/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Read-only interface status: the Roadshow SBTC_HAVE_INTERFACE_API query
 * subset (ObtainInterfaceList / ReleaseInterfaceList / QueryInterfaceTagList).
 * The stack self-configures from ENVARC:netstack.prefs, so the *config*
 * half of this API (AddInterfaceTagList, ConfigureInterfaceTagList, ...) is
 * declined — those LVOs share bsd_InterfaceConfigUnsupported below and fail
 * cleanly with EINVAL.
 *
 * Every field is read from the live lwIP netif plus the startup config
 * (SB_ROOT(base)->netCfg); netif access is bracketed by the core lock, as
 * in bsd_In_LocalAddr.
 */

#include "sb_base.h"

#include <exec/lists.h>
#include <utility/tagitem.h>

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>
#include <minlist.h>

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/stats.h>

#include "netstack.h"

static BOOL sb_if_is_loopback(const struct netif *nif)
{
    return nif->name[0] == 'l' && nif->name[1] == 'o';
}

/* Tags whose answers come from the NIC-stats cache / driver caps — data that
 * describes only the active netdev's interface, never e.g. loopback. */
static BOOL sb_ifq_is_nic_tag(ULONG tag)
{
    switch (tag)
    {
    case IFQ_PacketsReceived:
    case IFQ_PacketsSent:
    case IFQ_BadData:
    case IFQ_Overruns:
    case IFQ_InputDrops:
    case IFQ_OutputDrops:
    case IFQ_InputErrors:
    case IFQ_OutputErrors:
    case IFQ_GetBytesIn:
    case IFQ_GetBytesOut:
    case IFQ_LastStart:
    case IFQ_BPS:
    case IFQ_NumReadRequests:
    case IFQ_NumWriteRequests:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Fill a caller-provided sockaddr as an AF_INET sockaddr_in. addr is in
 * network byte order (== host order on 68k). */
static void sb_if_set_sockaddr(APTR dst, ULONG addr)
{
    struct sb_sockaddr_in *sa = dst;
    if (sa == NULL)
        return;
    sa->sin_len = sizeof(*sa);
    sa->sin_family = SB_AF_INET;
    sa->sin_port = 0;
    sa->sin_addr = addr;
    for (int i = 0; i < 8; i++)
        sa->sin_zero[i] = 0;
}

APTR bsd_ObtainInterfaceList(struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s\n", __func__);
    struct List *list = AllocVec(sizeof(struct List), MEMF_PUBLIC | MEMF_CLEAR);
    if (list == NULL)
    {
        sb_set_errno(base, SB_ENOBUFS);
        return NULL;
    }
    /* emu68-common's KS-safe NewList; struct List overlays struct MinList on
     * the head/tail/tailpred links (lh_Type/l_pad zeroed by MEMF_CLEAR) */
    _NewMinList((struct MinList *)list);

    netstack_lock();
    struct netif *nif;
    NETIF_FOREACH(nif)
    {
        if (sb_if_is_loopback(nif))
            continue;

        /* one node per interface; the name string trails the node body,
         * pointed to by ln_Name (the documented ObtainInterfaceList shape) */
        struct Node *n = AllocVec(sizeof(struct Node) + NETIF_NAMESIZE,
                                  MEMF_PUBLIC | MEMF_CLEAR);
        if (n == NULL)
            break;
        n->ln_Name = (char *)(n + 1);
        netif_index_to_name(netif_get_index(nif), n->ln_Name);
        AddTail(list, n);
    }
    netstack_unlock();
    return list;
}

VOID bsd_ReleaseInterfaceList(APTR list asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: list=0x%08lx\n", __func__, (ULONG)list);
    (void)base;
    struct List *l = list;
    if (l == NULL)
        return;

    struct Node *n;
    while ((n = RemHead(l)) != NULL)
        FreeVec(n);
    FreeVec(l);
}

LONG bsd_QueryInterfaceTagList(STRPTR name asm("a0"), struct TagItem *tags asm("a1"),
                               struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    if (name == NULL || tags == NULL)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    struct SocketBase *root = SB_ROOT(base);
    struct SbNetConfig *cfg = &root->netCfg;
    LONG result = 0;

    netstack_lock();
    struct netif *nif = netif_find((const char *)name);
    if (nif == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    ULONG addr = ip4_addr_get_u32(netif_ip4_addr(nif));
    ULONG mask = ip4_addr_get_u32(netif_ip4_netmask(nif));
    /* The NIC-stats cache and driver caps describe exactly one netif — the
     * active netdev's. Counter/link tags are answered only for that interface
     * (and skipped for e.g. loopback), so a query never gets another NIC's
     * numbers. */
    BOOL isNic = netstack.ns_ActiveNetdev != NULL &&
                 nif == &netstack.ns_ActiveNetdev->ndi_Netif;

    for (struct TagItem *t = tags; t->ti_Tag != TAG_END;)
    {
        if (t->ti_Tag == TAG_MORE)
        {
            t = (struct TagItem *)t->ti_Data;
            continue;
        }
        if (t->ti_Tag == TAG_SKIP)
        {
            t += 1 + t->ti_Data;
            continue;
        }
        if (t->ti_Tag == TAG_IGNORE || !(t->ti_Tag & TAG_USER))
        {
            t++;
            continue;
        }
        if (!isNic && sb_ifq_is_nic_tag(t->ti_Tag))
        {
            t++; /* counters describe the NIC only; leave storage untouched */
            continue;
        }

        APTR d = (APTR)t->ti_Data;
        switch (t->ti_Tag)
        {
        case IFQ_DeviceName:
            *(STRPTR *)d = (STRPTR)cfg->cfg_Device;
            break;
        case IFQ_DeviceUnit:
            *(LONG *)d = cfg->cfg_Unit;
            break;
        case IFQ_HardwareAddressSize:
            *(LONG *)d = (LONG)nif->hwaddr_len * 8; /* in bits */
            break;
        case IFQ_HardwareAddress:
            for (int i = 0; i < nif->hwaddr_len; i++)
                ((UBYTE *)d)[i] = nif->hwaddr[i];
            break;
        case IFQ_MTU:
            *(LONG *)d = nif->mtu;
            break;
        case IFQ_HardwareType:
            *(LONG *)d = SB_S2WIRETYPE_ETHERNET;
            break;
        case IFQ_Address:
            sb_if_set_sockaddr(d, addr);
            break;
        case IFQ_DestinationAddress:
            sb_if_set_sockaddr(d, addr); /* not point-to-point: same as address */
            break;
        case IFQ_BroadcastAddress:
            sb_if_set_sockaddr(d, addr | ~mask);
            break;
        case IFQ_NetMask:
            sb_if_set_sockaddr(d, mask);
            break;
        case IFQ_State:
            /* operational readiness: admin-up AND link-up (the autodoc's
             * "ready to receive and transmit" axis, not admin state alone) */
            *(LONG *)d = (netif_is_up(nif) && netif_is_link_up(nif)) ? SM_Up : SM_Down;
            break;
        case IFQ_AddressBindType:
            *(LONG *)d = cfg->cfg_Dhcp ? IFABT_Dynamic : IFABT_Static;
            break;
        case IFQ_PrimaryDNSAddress:
            sb_if_set_sockaddr(d, ip4_addr_get_u32(ip_2_ip4(dns_getserver(0))));
            break;
        case IFQ_SecondaryDNSAddress:
            sb_if_set_sockaddr(d, ip4_addr_get_u32(ip_2_ip4(dns_getserver(1))));
            break;

        /* --- counters: from the once-a-second NIC-stats cache (zero until
         * the first poll); answered only for the active NIC's netif — the
         * isNic gate above skips them for any other interface --- */
        case IFQ_PacketsReceived:
            *(ULONG *)d = root->netStats.nds_RxPackets.ndu_Lo;
            break;
        case IFQ_PacketsSent:
            *(ULONG *)d = root->netStats.nds_TxPackets.ndu_Lo;
            break;
        case IFQ_BadData:
            *(ULONG *)d = root->netStats.nds_RxErrors.ndu_Lo;
            break;
        case IFQ_Overruns:
            *(ULONG *)d = root->netStats.nds_RxOverruns + root->netStats.nds_RxFifoOvfl;
            break;
        case IFQ_UnknownTypes:
            *(ULONG *)d = nif->mib2_counters.ifinunknownprotos;
            break;
        case IFQ_InputDrops:
            *(LONG *)d = (LONG)root->netStats.nds_RxDropped.ndu_Lo;
            break;
        case IFQ_OutputDrops:
            /* nds_TxDropped already totals every accepted-but-unsent frame */
            *(LONG *)d = (LONG)root->netStats.nds_TxDropped.ndu_Lo;
            break;
        case IFQ_InputErrors:
            *(LONG *)d = (LONG)root->netStats.nds_RxErrors.ndu_Lo;
            break;
        case IFQ_OutputErrors:
            *(LONG *)d = (LONG)root->netStats.nds_TxErrors.ndu_Lo;
            break;
        case IFQ_IPDrops:
            *(LONG *)d = (LONG)lwip_stats.ip.drop;
            break;
        case IFQ_ARPDrops:
            *(LONG *)d = (LONG)lwip_stats.etharp.drop;
            break;
        case IFQ_GetBytesIn:
            sb_squad_from_u64(d, root->netStats.nds_RxBytes);
            break;
        case IFQ_GetBytesOut:
            sb_squad_from_u64(d, root->netStats.nds_TxBytes);
            break;
        case IFQ_LastStart:
        {
            struct sb_timeval *tv = d;
            tv->tv_secs = root->netLastStart.tv_secs;
            tv->tv_micro = root->netLastStart.tv_micro;
            break;
        }

        /* --- link/driver-derived scalars --- */
        case IFQ_BPS:
            *(LONG *)d = (LONG)root->netLink.ndls_SpeedMbps * 1000000;
            break;
        case IFQ_HardwareMTU:
            *(LONG *)d = isNic ? netstack.ns_ActiveNetdev->ndi_Caps.ndc_Mtu
                               : nif->mtu;
            break;
        /* isNic (checked above) guarantees ns_ActiveNetdev != NULL here */
        case IFQ_NumReadRequests:
            *(LONG *)d = netstack.ns_ActiveNetdev->ndi_Caps.ndc_RxRingSlots;
            break;
        case IFQ_NumWriteRequests:
            *(LONG *)d = netstack.ns_ActiveNetdev->ndi_Caps.ndc_TxRingSlots;
            break;
        case IFQ_Metric:
            *(LONG *)d = 0; /* single-homed host, no routing metric */
            break;
        case IFQ_GetDebugMode:
            *(LONG *)d = FALSE;
            break;

        default:
            /* Unanswerable here (documented): the Max/Pending request tags,
             * Input/OutputMulticasts (MIB-2 lumps broadcast+multicast, so a
             * value would be wrong), AddressLeaseExpires, GetSANA2CopyStats.
             * Caller storage is left untouched. */
            break;
        }
        t++;
    }
    netstack_unlock();
    return result;
}

/* The interface-config LVOs (AddInterfaceTagList, ConfigureInterfaceTagList,
 * RemoveInterface): configuration is prefs-file-only, so refuse cleanly with
 * EINVAL rather than the bare -1 a generic stub would return. One function
 * serves them all — it reads only a6, ignoring whatever args the caller
 * passed in the other registers. */
LONG bsd_InterfaceConfigUnsupported(struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: interface configuration is not supported\n", __func__);
    sb_set_errno(base, SB_EINVAL);
    return -1;
}
