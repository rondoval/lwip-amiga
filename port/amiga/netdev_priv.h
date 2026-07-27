/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Internals shared by the netdev_*.c compilation units (RX, TX, lifecycle).
 * Not part of the port's public surface — port/amiga/include is on the
 * netstack target's PUBLIC include path, this file deliberately is not.
 */

#ifndef LWIPAMIGA_NETDEV_PRIV_H
#define LWIPAMIGA_NETDEV_PRIV_H

#include <lwip/prot/ethernet.h>
#include <lwip/prot/ip4.h>

#include "netdev_if.h"

/* One RX buffer in flight to lwIP: a custom pbuf wrapping driver memory.
 * Freeing the pbuf recycles the driver buffer. Wrappers live in a free
 * list mutated only under the core lock. */
struct NdRxWrap
{
    struct pbuf_custom nrw_Pc; /* must stay first */
    APTR nrw_Cookie;
    struct NetdevIf *nrw_If;
    struct NdRxWrap *nrw_Next;
#ifdef DEBUG
    ULONG nrw_Live; /* double-free tripwire: 1 while lent to lwIP */
#endif
};

/* Byte offset of the IPv4 header inside a (possibly in-band 802.1Q-tagged)
 * Ethernet frame, or 0 if the frame is not IPv4 or its headers are not
 * contiguous in `len`. Recognises a single VLAN tag: IP sits at +14 untagged,
 * +18 tagged. The offset-based csum offloads work on tagged frames precisely
 * because everything downstream keys off this shifted L3 position. */
static inline ULONG ndif_ip_offset(const UBYTE *frame, ULONG len)
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

/* 16-bit one's-complement sum of the IPv4 pseudo-header (src, dst, proto,
 * L4 length), folded. */
static inline UWORD ndif_pseudo_sum(const struct ip_hdr *ip, ULONG start)
{
    ULONG sum = start;
    /* ip_hdr is a lwIP PACK_STRUCT; src/dst always land on an even offset
     * here (14- or 18-byte Ethernet header + 12-byte fixed offset into the
     * IP header, both even), which m68k reads natively -- the packed-member
     * warning doesn't reflect a real alignment risk on this target. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
    const UWORD *addr = (const UWORD *)&ip->src;
#pragma GCC diagnostic pop
    for (int i = 0; i < 4; i++)
        sum += addr[i];
    sum += IPH_PROTO(ip);
    sum += (ULONG)lwip_ntohs(IPH_LEN(ip)) - (ULONG)IPH_HL(ip) * 4;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (UWORD)sum;
}

/* netdev_rx.c — the NetDevStackOps RX entry */
ULONG ndif_rx_input(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count);

/* netdev_tx.c — the NetDevStackOps TX-reclaim entry and the netif outputs */
void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count);
err_t ndif_linkoutput(struct netif *nif, struct pbuf *p);
err_t ndif_ip4_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr);

#endif /* LWIPAMIGA_NETDEV_PRIV_H */
