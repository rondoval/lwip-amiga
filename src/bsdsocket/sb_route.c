/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GetRouteInfo (LVO -438) / FreeRouteInfo (LVO -432): a copy of the routing
 * table in the 4.4BSD rt_msghdr wire format. lwIP keeps no route table beyond
 * netif + gateway, so the table is synthesized from the live netifs: a
 * loopback host route, the default netif's on-link network route and its
 * default-gateway route. The struct layouts are transcribed from the NDK's
 * <net/route.h> — NOT from the autodoc, whose rt_msghdr field listing has
 * rtm_pid/rtm_addrs swapped — and the _Static_asserts below pin the wire ABI.
 * Add/Delete/ChangeRouteTagList remain unimplemented and
 * SBTC_HAVE_ROUTING_API deliberately reports FALSE.
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>

#include <lwip/netif.h>

#include "netstack.h"

/* --- <net/route.h> wire format (verbatim field order) --------------------- */

struct sb_rt_metrics
{
    ULONG rmx_locks, rmx_mtu, rmx_hopcount, rmx_expire, rmx_recvpipe;
    ULONG rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent;
};

struct sb_rt_msghdr
{
    UWORD rtm_msglen; /* entry length incl. appended sockaddrs; 0 terminates */
    UBYTE rtm_version;
    UBYTE rtm_type;
    UWORD rtm_index; /* netif index */
    LONG rtm_flags;  /* RTF_* */
    LONG rtm_addrs;  /* RTA_* bitmask of the appended sockaddrs */
    LONG rtm_pid, rtm_seq, rtm_errno, rtm_use;
    ULONG rtm_inits;
    struct sb_rt_metrics rtm_rmx;
};

#define SB_RTM_VERSION 3
#define SB_RTM_GET 4

#define SB_RTF_UP 0x1
#define SB_RTF_GATEWAY 0x2
#define SB_RTF_HOST 0x4

/* callers parse the sockaddrs positionally, in RTAX slot order:
 * DST, GATEWAY, NETMASK */
#define SB_RTA_DST 0x1
#define SB_RTA_GATEWAY 0x2
#define SB_RTA_NETMASK 0x4

_Static_assert(sizeof(struct sb_rt_metrics) == 40, "rt_metrics wire ABI");
_Static_assert(sizeof(struct sb_rt_msghdr) == 74, "rt_msghdr wire ABI");
_Static_assert(__builtin_offsetof(struct sb_rt_msghdr, rtm_flags) == 6,
               "rt_msghdr wire ABI");
_Static_assert(__builtin_offsetof(struct sb_rt_msghdr, rtm_addrs) == 10,
               "rt_msghdr wire ABI");
_Static_assert(__builtin_offsetof(struct sb_rt_msghdr, rtm_rmx) == 34,
               "rt_msghdr wire ABI");

/* one synthesized route before encoding */
struct sb_route
{
    LONG flags; /* RTF_* */
    LONG addrs; /* RTA_* */
    UWORD index;
    ULONG dst, gw, mask;
};

static ULONG sb_rt_entry_len(LONG addrs)
{
    ULONG n = 0;
    for (LONG bit = SB_RTA_DST; bit <= SB_RTA_NETMASK; bit <<= 1)
        if (addrs & bit)
            n++;
    return sizeof(struct sb_rt_msghdr) + n * sizeof(struct sb_sockaddr_in);
}

APTR bsd_GetRouteInfo(LONG af asm("d0"), LONG flags asm("d1"),
                      struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: af=%ld flags=0x%08lx\n", __func__, af,
             (ULONG)flags);
    if (af != SB_AF_UNSPEC && af != SB_AF_INET)
    {
        sb_set_errno(base, SB_EAFNOSUPPORT);
        return NULL;
    }

    /* snapshot the scalars every reportable route derives from */
    ULONG loAddr = 0, ifAddr = 0, ifMask = 0, ifGw = 0;
    UWORD loIndex = 0, ifIndex = 0;
    netstack_lock();
    struct netif *nif;
    NETIF_FOREACH(nif)
    {
        if (sb_if_is_loopback(nif))
        {
            loAddr = ip4_addr_get_u32(netif_ip4_addr(nif));
            loIndex = netif_get_index(nif);
        }
    }
    if (netif_default != NULL && !sb_if_is_loopback(netif_default))
    {
        ifAddr = ip4_addr_get_u32(netif_ip4_addr(netif_default));
        ifMask = ip4_addr_get_u32(netif_ip4_netmask(netif_default));
        ifGw = ip4_addr_get_u32(netif_ip4_gw(netif_default));
        ifIndex = netif_get_index(netif_default);
    }
    netstack_unlock();

    /* candidates; an entry is returned iff it carries every requested flag,
     * so e.g. GetRouteInfo(AF_INET, RTF_LLINFO) yields an empty table */
    struct sb_route routes[3];
    ULONG count = 0;
    if (loAddr != 0)
        routes[count++] = (struct sb_route){
            .flags = SB_RTF_UP | SB_RTF_HOST,
            .addrs = SB_RTA_DST,
            .index = loIndex,
            .dst = loAddr};
    if (ifAddr != 0)
        routes[count++] = (struct sb_route){
            .flags = SB_RTF_UP,
            .addrs = SB_RTA_DST | SB_RTA_NETMASK,
            .index = ifIndex,
            .dst = ifAddr & ifMask,
            .mask = ifMask};
    if (ifGw != 0)
        routes[count++] = (struct sb_route){
            .flags = SB_RTF_UP | SB_RTF_GATEWAY,
            .addrs = SB_RTA_DST | SB_RTA_GATEWAY | SB_RTA_NETMASK,
            .index = ifIndex,
            .gw = ifGw};

    ULONG total = sizeof(struct sb_rt_msghdr); /* zero-length terminator */
    for (ULONG i = 0; i < count; i++)
        if ((routes[i].flags & flags) == flags)
            total += sb_rt_entry_len(routes[i].addrs);

    UBYTE *table = AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    if (table == NULL)
    {
        sb_set_errno(base, SB_ENOBUFS);
        return NULL;
    }

    UBYTE *p = table;
    for (ULONG i = 0; i < count; i++)
    {
        if ((routes[i].flags & flags) != flags)
            continue;
        struct sb_rt_msghdr *rtm = (struct sb_rt_msghdr *)p;
        rtm->rtm_msglen = (UWORD)sb_rt_entry_len(routes[i].addrs);
        rtm->rtm_version = SB_RTM_VERSION;
        rtm->rtm_type = SB_RTM_GET;
        rtm->rtm_index = routes[i].index;
        rtm->rtm_flags = routes[i].flags;
        rtm->rtm_addrs = routes[i].addrs;
        /* pid/seq/errno/use/inits/rmx stay zero via MEMF_CLEAR */
        p += sizeof(*rtm);
        sb_if_set_sockaddr(p, routes[i].dst);
        p += sizeof(struct sb_sockaddr_in);
        if (routes[i].addrs & SB_RTA_GATEWAY)
        {
            sb_if_set_sockaddr(p, routes[i].gw);
            p += sizeof(struct sb_sockaddr_in);
        }
        if (routes[i].addrs & SB_RTA_NETMASK)
        {
            sb_if_set_sockaddr(p, routes[i].mask);
            p += sizeof(struct sb_sockaddr_in);
        }
    }
    /* terminator rtm_msglen == 0 already in place via MEMF_CLEAR */
    return table;
}

VOID bsd_FreeRouteInfo(APTR table asm("a0"), struct SocketBase *base asm("a6"))
{
    (void)base;
    KprintfT("[bsdsocket] %s: table=0x%08lx\n", __func__, (ULONG)table);
    FreeVec(table); /* FreeVec(NULL) is a no-op */
}
