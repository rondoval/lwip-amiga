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

#include "netstack.h"

static BOOL sb_if_is_loopback(const struct netif *nif)
{
    return nif->name[0] == 'l' && nif->name[1] == 'o';
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
    KprintfH("[bsdsocket] %s\n", __func__);
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
    KprintfH("[bsdsocket] %s: list=0x%08lx\n", __func__, (ULONG)list);
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
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    if (name == NULL || tags == NULL)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    struct SbNetConfig *cfg = &SB_ROOT(base)->netCfg;
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
            *(LONG *)d = netif_is_link_up(nif) ? SM_Up : SM_Down;
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
        default:
            break; /* unrecognized query tag: ignore */
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
    KprintfH("[bsdsocket] %s: interface configuration is not supported\n", __func__);
    sb_set_errno(base, SB_EINVAL);
    return -1;
}
