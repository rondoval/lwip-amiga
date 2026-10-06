/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Backend-agnostic interface glue: the shared base every hardware-interface
 * struct embeds first. See netif_base.h for the ownership story and the
 * contracts; this file is the implementation, in the same order:
 *
 *   lifecycle          — reset the base, then stamp its identity,
 *   transmit capacity  — the whole-datagram admission accounting,
 *   VLAN hooks         — in-band 802.1Q, per frame,
 *   IGMP RX filter     — the refcounted joined-multicast MAC set.
 *
 * Locking is per group and noted with each: the lwIP hooks and the transmit
 * calls run with the core lock already held, netifbase_mcast_snapshot takes it
 * itself, and lifecycle runs at create time on the stack task.
 */

#include "netstack_sys.h"

#include <lwip/netif.h>

#include "netif_base.h"
#include "netstack.h"
#include "netstack_lwiphooks.h"

/* ----------------------------------------------------------- lifecycle --- */

/* Reset the base's own fields for a (re)create. The embedded lwIP netif is not
 * touched: netif_add, in the backend's create, owns that. */
void netifbase_init(struct NetIfBase *nib, UWORD kind)
{
    nib->nib_Kind = kind;
    nib->nib_HwMtu = 0;
    nib->nib_NumRead = 0;
    nib->nib_NumWrite = 0;
    nib->nib_VlanTci = -1; /* untagged by default; the opener overrides from prefs */
    nib->nib_Name[0] = '\0';
    nib->nib_Device[0] = '\0';
    nib->nib_Unit = 0;
    nib->nib_Dhcp = FALSE;
    nib->nib_Hostname[0] = '\0';
    nib->nib_McastCount = 0;
    nib->nib_McastOverflow = 0;
    nib->nib_RxFilterDirty = FALSE;
    nib->nib_TxFree = ~0UL; /* ungated until the backend calls netifbase_tx_setup */
    nib->nib_TxMaxFrames = 0;
}

/* Identity, from the control-port config the opener parsed: runs between the
 * backend's create() and netif_set_up, so the query LVOs never see a half
 * interface. The VLAN TCI arrives with it, before the first frame. */
void netifbase_stamp(struct NetIfBase *nib, const struct NetCtlIfConfig *nif,
                     const char *hostFallback)
{
    for (ULONG i = 0; i < NETCTL_IFNAME_MAX; i++)
        nib->nib_Name[i] = nif->nif_Name[i];
    for (ULONG i = 0; i < NETCTL_DEV_MAX; i++)
        nib->nib_Device[i] = nif->nif_Device[i];
    nib->nib_Unit = nif->nif_Unit;
    nib->nib_Dhcp = (nif->nif_Flags & NETCTL_IFF_DHCP) != 0;
    nib->nib_VlanTci = nif->nif_VlanTci;

    /* netif_set_hostname keeps the pointer, so the name needs storage that
     * lives with the interface — the per-add ID, or the global pref */
    const char *host = nif->nif_Id[0] != '\0' ? nif->nif_Id : hostFallback;
    ULONG h = 0;
    for (; h < NETCTL_ID_MAX - 1 && host[h] != '\0'; h++)
        nib->nib_Hostname[h] = host[h];
    nib->nib_Hostname[h] = '\0';
}

/* --------------------------------------------------- transmit capacity --- */
/* Whole-datagram admission; the contract is in netif_base.h. Everything here
 * runs under the core lock. Two groups: the backend reports what its transmit
 * path did (setup/freed, plus the inline taken), the socket layer asks whether
 * a datagram fits and arranges to be told when one does (admit/writable/
 * want_space/detached). */

void netifbase_tx_setup(struct NetIfBase *nib, ULONG capacity, ULONG mtu)
{
    nib->nib_TxFree = capacity;
    nib->nib_TxMaxFrames = netifbase_tx_frames(NIB_MAX_L3, mtu);
}

/* The one wake, shared by the two events that can let a blocked sender run
 * again: room crossing the threshold, and the interface going away. One-shot —
 * ns_TxWantSpace is cleared here, and the socket layer re-arms it (through
 * netifbase_tx_want_space) for any waiter that re-checks and still cannot go. */
static inline void netifbase_tx_wake(void)
{
    if (netstack.ns_TxWantSpace && netstack.ns_TxSpaceCb != NULL)
    {
        netstack.ns_TxWantSpace = FALSE;
        netstack.ns_TxSpaceCb();
    }
}

/* Edge-triggered on purpose: the wake fires only as the free count crosses
 * nib_TxMaxFrames, so a draining ring wakes the waiters once instead of once
 * per frame — and only then is a maximum-size datagram guaranteed to fit. */
void netifbase_tx_freed(struct NetIfBase *nib, ULONG n)
{
    ULONG was = nib->nib_TxFree;
    nib->nib_TxFree = was + n;
    if (was < nib->nib_TxMaxFrames && nib->nib_TxFree >= nib->nib_TxMaxFrames)
        netifbase_tx_wake();
}

/* No interface, or no destination to route: admit. There is nothing to account
 * against, and the send that follows fails on its own with the right errno (no
 * route, or a dead pcb) — refusing here would invent a different one. */
BOOL netifbase_tx_admit(const ip4_addr_t *dst, ULONG l3len)
{
    struct NetIfBase *nib = netstack.ns_ActiveIf;
    if (nib == NULL || dst == NULL)
        return TRUE;
    struct netif *nif = &nib->nib_Netif;
    /* diverted to the loopback path before any fragmenting (ip4.c) */
    if (ip4_addr_isloopback(dst) || ip4_addr_eq(dst, netif_ip4_addr(nif)))
        return TRUE;
    return netifbase_tx_frames(l3len, nif->mtu) <= nib->nib_TxFree;
}

/* What select() promises: room for a datagram of ANY size, since the caller is
 * not telling us what it will send. Writable with no interface, as admit is. */
BOOL netifbase_tx_writable(void)
{
    struct NetIfBase *nib = netstack.ns_ActiveIf;
    return nib == NULL || nib->nib_TxFree >= nib->nib_TxMaxFrames;
}

/* Arm only while an interface exists: with none there will never be a
 * netifbase_tx_freed to clear the flag again. */
void netifbase_tx_want_space(void)
{
    if (netstack.ns_ActiveIf != NULL)
        netstack.ns_TxWantSpace = TRUE;
}

/* An interface went away, so the room its waiters are blocked on is never
 * coming. Wake them unconditionally — there is no capacity left to compare
 * against — and they re-check, are admitted (see above) and let the send report
 * the real error. */
void netifbase_tx_detached(void)
{
    netifbase_tx_wake();
}

/* ---------------------------------------------------------- VLAN hooks --- */
/* In-band 802.1Q (lwIP LWIP_HOOK_VLAN_SET/CHECK, wired in
 * netstack_lwiphooks.h). The per-interface TCI is nib_VlanTci (-1 =
 * untagged), set from the interface config before the netif comes up. The
 * hooks fire only from ethernet_output/ethernet_input — never for loopback —
 * so netif->state is always a NetIfBase here, whichever backend built it. */

s32_t netifbase_vlan_set(struct netif *nif, struct pbuf *p,
                         const struct eth_addr *src, const struct eth_addr *dst,
                         u16_t eth_type)
{
    (void)p;
    (void)src;
    (void)dst;
    (void)eth_type;
    return (s32_t)((struct NetIfBase *)nif->state)->nib_VlanTci; /* <0 = no tag */
}

int netifbase_vlan_check(struct netif *nif, struct eth_hdr *eth,
                         struct eth_vlan_hdr *vlan)
{
    (void)eth;
    LONG tci = ((struct NetIfBase *)nif->state)->nib_VlanTci;
    if (tci < 0)
        return 0; /* not on a VLAN: drop tagged frames */
    return VLAN_ID(vlan) == (UWORD)(tci & 0xFFF);
}

/* ------------------------------------------------------ IGMP RX filter --- */
/* lwIP calls igmp_mac_filter under the core lock on the first join / last
 * leave of every multicast group (the all-systems group included, added at
 * igmp_start). We keep the exact set of joined multicast MACs here and let
 * the stack task push it to the driver; see the nib_Mcast* comment in
 * netif_base.h. */

/* IPv4 multicast group -> Ethernet MAC: 01:00:5e | low 23 bits of the group. */
static void nib_mcast_mac(const ip4_addr_t *group, UBYTE mac[6])
{
    ULONG g = lwip_ntohl(ip4_addr_get_u32(group));
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5e;
    mac[3] = (UBYTE)((g >> 16) & 0x7f);
    mac[4] = (UBYTE)((g >> 8) & 0xff);
    mac[5] = (UBYTE)(g & 0xff);
}

static BOOL nib_mac_eq(const UBYTE *a, const UBYTE *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return FALSE;
    return TRUE;
}

err_t netifbase_igmp_mac_filter(struct netif *nif, const ip4_addr_t *group,
                                enum netif_mac_filter_action action)
{
    struct NetIfBase *nib = nif->state;
    UBYTE mac[6];
    nib_mcast_mac(group, mac);

    LONG idx = -1;
    for (UWORD i = 0; i < nib->nib_McastCount; i++)
    {
        if (nib_mac_eq(nib->nib_McastList[i], mac))
        {
            idx = (LONG)i;
            break;
        }
    }

    BOOL hadOverflow = nib->nib_McastOverflow > 0;
    BOOL listChanged = FALSE;

    if (action == NETIF_ADD_MAC_FILTER)
    {
        if (idx >= 0)
        {
            nib->nib_McastRefs[idx]++; /* another group aliases this MAC */
        }
        else if (nib->nib_McastCount < NIB_MCAST_MAX)
        {
            UWORD n = nib->nib_McastCount++;
            for (int i = 0; i < 6; i++)
                nib->nib_McastList[n][i] = mac[i];
            nib->nib_McastRefs[n] = 1;
            listChanged = TRUE;
        }
        else
        {
            nib->nib_McastOverflow++; /* no slot: the backend's fallback covers it */
        }
    }
    else /* NETIF_DEL_MAC_FILTER */
    {
        if (idx >= 0)
        {
            if (--nib->nib_McastRefs[idx] == 0)
            {
                UWORD last = --nib->nib_McastCount; /* swap-remove */
                for (int i = 0; i < 6; i++)
                    nib->nib_McastList[idx][i] = nib->nib_McastList[last][i];
                nib->nib_McastRefs[idx] = nib->nib_McastRefs[last];
                listChanged = TRUE;
            }
        }
        else if (nib->nib_McastOverflow > 0)
        {
            nib->nib_McastOverflow--;
        }
    }

    if (listChanged || (nib->nib_McastOverflow > 0) != hadOverflow)
        nib->nib_RxFilterDirty = TRUE;
    return ERR_OK;
}

UWORD netifbase_mcast_snapshot(struct NetIfBase *nib, UBYTE out[][6],
                               UWORD *overflow)
{
    netstack_lock();
    UWORD count = nib->nib_McastCount;
    for (UWORD i = 0; i < count; i++)
        for (int b = 0; b < 6; b++)
            out[i][b] = nib->nib_McastList[i][b];
    *overflow = nib->nib_McastOverflow;
    nib->nib_RxFilterDirty = FALSE;
    netstack_unlock();
    return count;
}
