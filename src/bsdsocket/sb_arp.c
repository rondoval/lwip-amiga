/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ARP table ioctls (SIOCSARP/SIOCDARP/SIOCGARP/SIOCGARPT) — the backend of
 * the Arp command and of AmiTCP-era clients. Runs on the caller task like
 * every IoctlSocket request, one core-lock hold per request. The argument
 * structs mirror the NDK netinclude layouts (published contract in
 * include/net/if_arp_ioctl.h); the library core never includes those
 * headers, so the mirrors are asserted to the wire sizes below.
 *
 * Published/proxy ARP (ATF_PUBL) and trailers are not supported and never
 * will be — rejected with EINVAL.
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>
#include <memory.h>

#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>

#include "netdev_if.h"
#include "netstack.h"

/* --- netinclude net/if_arp.h mirrors ------------------------------------- */

struct sb_arp_sockaddr
{
    UBYTE sa_len;
    UBYTE sa_family;
    UBYTE sa_data[14];
};

struct sb_arpreq
{
    struct sb_arp_sockaddr arp_pa; /* protocol address (a sockaddr_in) */
    struct sb_arp_sockaddr arp_ha; /* hardware address (MAC in sa_data) */
    LONG arp_flags;                /* SB_ATF_* */
};

struct sb_arptabreq
{
    LONG atr_size;  /* in: capacity of atr_table; out: entries copied */
    LONG atr_inuse; /* out: entries present in the table */
    struct sb_arpreq *atr_table;
};

/* m68k only: host IntelliSense sees 64-bit LONG/pointers */
#ifndef __INTELLISENSE__
_Static_assert(sizeof(struct sb_arpreq) == 36, "arpreq wire ABI");
_Static_assert(sizeof(struct sb_arptabreq) == 12, "arptabreq wire ABI");
#endif

/* --- helpers (core lock where noted) ------------------------------------- */

/* Read the IPv4 address out of arp_pa; network order == native on 68k. */
static LONG sb_arp_pa_ip(const struct sb_arpreq *ar, ip4_addr_t *ip)
{
    const struct sb_sockaddr_in *sin = (const struct sb_sockaddr_in *)&ar->arp_pa;
    if (sin->sin_family != SB_AF_INET && sin->sin_family != 0)
        return -1;
    ip4_addr_set_u32(ip, sin->sin_addr);
    return 0;
}

static void sb_arp_fill(struct sb_arpreq *out, const ip4_addr_t *ip,
                        const struct eth_addr *eth, u8_t info)
{
    struct sb_sockaddr_in *sin = (struct sb_sockaddr_in *)&out->arp_pa;
    memset(out, 0, sizeof(*out));
    sin->sin_len = sizeof(*sin);
    sin->sin_family = SB_AF_INET;
    sin->sin_addr = ip4_addr_get_u32(ip);
    out->arp_ha.sa_len = sizeof(out->arp_ha);
    out->arp_ha.sa_family = 0; /* AF_UNSPEC, BSD convention */

    LONG flags = SB_ATF_INUSE;
    if (!(info & ETHARP_ENTRY_PENDING))
    {
        CopyMem((APTR)eth, out->arp_ha.sa_data, 6);
        flags |= SB_ATF_COM;
    }
    if (info & ETHARP_ENTRY_STATIC)
        flags |= SB_ATF_PERM;
    out->arp_flags = flags;
}

/* An IP->MAC binding changed under the L2 header cache: drop it. Core lock
 * held; v1 has a single hardware netif. */
static void sb_arp_hh_flush(void)
{
    struct NetdevIf *ndi = netstack.ns_ActiveNetdev;
    if (ndi != NULL)
        netdevif_hh_invalidate(ndi);
}

/* --- the four requests ---------------------------------------------------- */

LONG sb_arp_ioctl(struct SocketBase *base, ULONG req, APTR argp)
{
    KprintfT("[bsdsocket] %s: req 0x%lx\n", __func__, req);

    switch (req)
    {
    case SB_SIOCSARP:
    {
        struct sb_arpreq *ar = argp;
        if (ar->arp_flags & (SB_ATF_PUBL | SB_ATF_USETRAILERS))
            return sb_fail(base, SB_EINVAL); /* never supported */
        ip4_addr_t ip;
        if (sb_arp_pa_ip(ar, &ip) < 0)
            return sb_fail(base, SB_EAFNOSUPPORT);
        struct eth_addr eth;
        CopyMem(ar->arp_ha.sa_data, eth.addr, 6);

        netstack_lock();
        err_t err = etharp_add_entry(&ip, &eth, (ar->arp_flags & SB_ATF_PERM) != 0);
        if (err == ERR_OK)
            sb_arp_hh_flush();
        netstack_unlock();

        switch (err)
        {
        case ERR_OK:
            return 0;
        case ERR_MEM: /* table full */
            return sb_fail(base, SB_ENOBUFS);
        case ERR_RTE: /* no netif routes to that address */
            return sb_fail(base, SB_ENETUNREACH);
        case ERR_VAL: /* dynamic add over a permanent entry */
            return sb_fail(base, SB_EADDRINUSE);
        default: /* ERR_ARG: non-unicast address */
            return sb_fail(base, SB_EINVAL);
        }
    }

    case SB_SIOCDARP:
    {
        ip4_addr_t ip;
        if (sb_arp_pa_ip(argp, &ip) < 0)
            return sb_fail(base, SB_EAFNOSUPPORT);

        netstack_lock();
        err_t err = etharp_remove_entry(&ip);
        if (err == ERR_OK)
            sb_arp_hh_flush();
        netstack_unlock();

        return (err == ERR_OK) ? 0 : sb_fail(base, SB_ENXIO);
    }

    case SB_SIOCGARP:
    {
        struct sb_arpreq *ar = argp;
        ip4_addr_t ip;
        if (sb_arp_pa_ip(ar, &ip) < 0)
            return sb_fail(base, SB_EAFNOSUPPORT);

        /* plain index walk: etharp_find_addr() hides pending entries */
        netstack_lock();
        for (ULONG i = 0; i < ARP_TABLE_SIZE; i++)
        {
            ip4_addr_t *entry_ip;
            struct netif *nif;
            struct eth_addr *eth;
            u8_t info;
            u16_t ctime;
            if (!etharp_get_entry_info(i, &entry_ip, &nif, &eth, &info, &ctime))
                continue;
            if (!ip4_addr_eq(entry_ip, &ip))
                continue;
            sb_arp_fill(ar, entry_ip, eth, info);
            netstack_unlock();
            return 0;
        }
        netstack_unlock();
        return sb_fail(base, SB_ENXIO);
    }

    case SB_SIOCGARPT:
    {
        struct sb_arptabreq *atr = argp;
        struct sb_arpreq *tab = atr->atr_table;
        LONG cap = (tab != NULL && atr->atr_size > 0) ? atr->atr_size : 0;
        LONG copied = 0;
        LONG present = 0;

        netstack_lock();
        for (ULONG i = 0; i < ARP_TABLE_SIZE; i++)
        {
            ip4_addr_t *entry_ip;
            struct netif *nif;
            struct eth_addr *eth;
            u8_t info;
            u16_t ctime;
            if (!etharp_get_entry_info(i, &entry_ip, &nif, &eth, &info, &ctime))
                continue;
            present++;
            if (copied < cap)
                sb_arp_fill(&tab[copied++], entry_ip, eth, info);
        }
        netstack_unlock();

        atr->atr_size = copied;
        atr->atr_inuse = present;
        return 0;
    }

    default: /* unreachable via the sb_api.c dispatch */
        return sb_fail(base, SB_EINVAL);
    }
}
