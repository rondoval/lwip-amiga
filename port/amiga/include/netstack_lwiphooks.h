/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * lwIP hook file (LWIP_HOOK_FILENAME) — in-band 802.1Q VLAN.
 *
 * lwIP #includes this from inside its own .c files (ethernet.c and others),
 * after the core types exist, so the prototypes use real lwIP types. The
 * hooks are implemented in netdev_if.c and read the per-interface VID from
 * netif->state (the struct NetdevIf). See lwipopts.h for the switches.
 */

#ifndef NETSTACK_LWIPHOOKS_H
#define NETSTACK_LWIPHOOKS_H

#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"

/* TX: return the 802.1Q TCI to tag this frame with, or < 0 for no tag. */
s32_t netdevif_vlan_set(struct netif *netif, struct pbuf *p,
                        const struct eth_addr *src, const struct eth_addr *dst,
                        u16_t eth_type);

/* RX (tagged frames only): return non-zero to accept, 0 to drop. */
int netdevif_vlan_check(struct netif *netif, struct eth_hdr *ethhdr,
                        struct eth_vlan_hdr *vlan);

#define LWIP_HOOK_VLAN_SET(netif, p, src, dst, eth_type) \
    netdevif_vlan_set((netif), (p), (src), (dst), (eth_type))
#define LWIP_HOOK_VLAN_CHECK(netif, eth_hdr, vlan_hdr) \
    netdevif_vlan_check((netif), (eth_hdr), (vlan_hdr))

#endif /* NETSTACK_LWIPHOOKS_H */
