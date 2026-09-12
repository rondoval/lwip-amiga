/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netif_base — state and behavior shared by every hardware-interface glue,
 * whatever driver ABI sits behind it (netdev today, SANA-II as the second
 * backend). Each backend embeds struct NetIfBase FIRST in its own interface
 * struct, so netif->state points at the backend struct, the base, and the
 * lwIP netif all at once; nib_Kind tags which backend owns the rest.
 *
 * What lives here is exactly what backend-agnostic code touches:
 *   - the lwIP netif and the interface kind,
 *   - the identity block the query LVOs and the control port read,
 *   - the in-band 802.1Q TCI (the lwIP VLAN hooks fire for every ethernet
 *     netif and must not care which backend built it),
 *   - the refcounted joined-multicast MAC set maintained by the lwIP
 *     igmp_mac_filter hook; how the set reaches the driver is per-backend
 *     (netdev: declarative RX-filter command; SANA-II: add/del deltas).
 */

#ifndef LWIPAMIGA_NETIF_BASE_H
#define LWIPAMIGA_NETIF_BASE_H

#include <exec/types.h>

#include <lwip/netif.h>

#include <netstack_ctl.h> /* NETCTL_* identity field sizes */

/* nib_Kind */
#define NIF_KIND_NETDEV 0
#define NIF_KIND_SANA2  1

/* Exact multicast RX filter: how many distinct multicast MACs the stack
 * tracks. Beyond this nib_McastOverflow counts the excess and the backend
 * falls back to whatever "accept more" mode it has (netdev: ALLMULTI; a
 * SANA-II driver manages its own table and has no such command). Generous
 * vs real group counts. */
#define NIB_MCAST_MAX 32u

struct NetCtlIfConfig;

struct NetIfBase
{
    struct netif nib_Netif; /* must stay first: netif* == base* == iface* */
    UWORD nib_Kind;         /* NIF_KIND_* */

    /* ifquery scalars, stamped by the backend at create time from whatever
     * it negotiated: the driver's own MTU (before any config clamp) and the
     * request/ring depths behind IFQ_NumRead/WriteRequests. */
    UWORD nib_HwMtu;
    UWORD nib_NumRead;
    UWORD nib_NumWrite;

    LONG nib_VlanTci; /* in-band 802.1Q: -1 = no VLAN, else (pcp<<13)|(vid&0xFFF);
                         read per-frame by the lwIP VLAN hooks. init() defaults
                         it; the opener overrides from prefs before the
                         interface comes up. */

    /* Identity, stamped by the opener (netifbase_stamp) right after the
     * backend's create(): the Roadshow-style interface name (from the
     * AddNetInterface config file), the OpenDevice pair it came from, and
     * the address mode. Read under the core lock by the query LVOs
     * (sb_ifquery.c) and the control port. lwIP's own short name stays with
     * the backend ("nd<n>" / "s2<n>"). */
    char nib_Name[NETCTL_IFNAME_MAX]; /* "" until stamped */
    char nib_Device[NETCTL_DEV_MAX];
    LONG nib_Unit;
    BOOL nib_Dhcp;
    char nib_Hostname[NETCTL_ID_MAX]; /* stable storage: netif_set_hostname
                                         keeps the pointer */

    /* IGMP -> driver RX filter. netifbase_igmp_mac_filter (lwIP hook, under
     * the core lock) keeps the set of joined multicast MACs — 01:00:5e + the
     * group's low 23 bits, refcounted so the several IPv4 groups that can
     * alias one MAC share a slot — and raises nib_RxFilterDirty on any
     * change (a 0<->nonzero overflow transition included). The stack task
     * pushes the set OFF the core lock, backend-specifically. */
    UBYTE nib_McastList[NIB_MCAST_MAX][6]; /* distinct joined multicast MACs */
    UWORD nib_McastRefs[NIB_MCAST_MAX];    /* per-MAC join refcount */
    UWORD nib_McastCount;                  /* distinct MACs in the list */
    UWORD nib_McastOverflow;               /* joins that didn't fit */
    BOOL nib_RxFilterDirty;                /* set changed; stack task must push */
};

/* Reset the base for a (re)create: kind stamped, multicast set empty, VLAN
 * untagged, identity cleared. The ifquery scalars stay 0 until the backend
 * fills them from its negotiated capabilities. */
void netifbase_init(struct NetIfBase *nib, UWORD kind);

/* Stamp the interface identity from the control-port config; @hostFallback
 * (the global HOSTNAME pref) is used when the config carries no ID. Runs on
 * the stack task between the backend's create() and netif_set_up. */
void netifbase_stamp(struct NetIfBase *nib, const struct NetCtlIfConfig *nif,
                     const char *hostFallback);

/* The lwIP igmp_mac_filter hook every backend registers in its netif init. */
err_t netifbase_igmp_mac_filter(struct netif *nif, const ip4_addr_t *group,
                                enum netif_mac_filter_action action);

/* Snapshot the joined-MAC set for an off-lock push to the driver: copies the
 * list into @out (NIB_MCAST_MAX entries), stores the overflow count and
 * clears nib_RxFilterDirty, all under one core-lock hold. Returns the entry
 * count. The caller gates on nib_RxFilterDirty (an unlocked read of the
 * flag is fine: a set left dirty is retried next tick). */
UWORD netifbase_mcast_snapshot(struct NetIfBase *nib, UBYTE out[][6],
                               UWORD *overflow);

#endif /* LWIPAMIGA_NETIF_BASE_H */
