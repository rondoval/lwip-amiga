/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Internals shared by the sana2_*.c compilation units (lifecycle, TX, the
 * RX pump). Not part of the port's public surface.
 */

#ifndef LWIPAMIGA_SANA2_PRIV_H
#define LWIPAMIGA_SANA2_PRIV_H

#include <devices/sana2.h>

#include <lwip/pbuf.h>

#include "sana2_if.h"

/* Outstanding-request depths — internal constants, no config knobs.
 * Reads are per packet type (SANA-II demuxes CMD_READ on ios2_PacketType).
 * The DATA class must cover the WHOLE announced TCP window: a SANA-II
 * driver DROPS any frame that finds no pending read of its type
 * (genet-sana2 counts them as rx_orphan), we announce TCP_WND, and the
 * peer is entitled to burst all of it — anything less loses burst tails
 * and TCP limps between loss recoveries. window/MSS + slack is the
 * same sizing rule as netdevif_rx_hold_budget; ~780 reads * ~1.5 KB
 * ≈ 1.2 MB of heap.
 * Under in-band VLAN every data frame arrives as the 0x8100 class, so the
 * data depth follows the tag (s2if_rx_classes). */
#define S2IF_RX_READS_DATA       ((TCP_WND / TCP_MSS) + 64)
#define S2IF_RX_READS_IP4_TAGGED 32 /* untagged stragglers on a VLAN iface */
#define S2IF_RX_READS_ARP        8
#define S2IF_TX_REQS             32

/* Per-class read counts for an interface: the data depth goes to the class
 * that actually carries the TCP/UDP stream. Shared by sana2if_create
 * (nib_NumRead) and the pump's request-init loop. */
static inline void s2if_rx_classes(LONG vlanTci, ULONG *ip4, ULONG *arp,
                                   ULONG *vlan)
{
    *arp = S2IF_RX_READS_ARP;
    if (vlanTci >= 0)
    {
        *ip4 = S2IF_RX_READS_IP4_TAGGED;
        *vlan = S2IF_RX_READS_DATA;
    }
    else
    {
        *ip4 = S2IF_RX_READS_DATA;
        *vlan = 0;
    }
}

/* RX buffer slack past the driver's MTU: Miami-workaround drivers round the
 * CopyToBuff length up to a longword multiple (+3 worst case), and padded
 * runts may carry a few extra bytes. */
#define S2IF_RX_SLACK 8

/* The pump runs the whole lwIP RX path (ethernet_input through TCP input
 * and the ACKs it emits) on this stack — same depth as the stack task. */
#define S2IF_PUMP_STACK 32768

/* One write request: ios2_Data carries the ref'd pbuf chain (the
 * CopyFromBuff cookie — the driver copies from it, the frame is never
 * edited); the pbuf is freed at completion (s2if_tx_complete). */
struct S2TxReq
{
    struct IOSana2Req stx_Io;
    struct S2TxReq *stx_Next; /* free list / staged FIFO / harvest batch */
};

/* One read request: ios2_Data points at this struct (the CopyToBuff
 * cookie); srx_Dst/srx_Cap describe the payload area of srx_Pbuf past the
 * 14 bytes of headroom the synthesized Ethernet header lands in. */
struct S2RxReq
{
    struct IOSana2Req srx_Io;
    UBYTE *srx_Dst;
    ULONG srx_Cap;
    struct pbuf *srx_Pbuf;
    struct S2RxReq *srx_Next; /* harvest batch / parked list */
    UBYTE srx_State;          /* S2RX_*, pump-private */
};

#define S2RX_FLIGHT 0 /* SendIO'd, awaiting the driver */
#define S2RX_HOME   1 /* replied, not reposted (teardown drain) */
#define S2RX_PARKED 2 /* S2ERR_OUTOFSERVICE: reposted on the ONLINE event */

/* 64-bit add for the glue byte counters (hi/lo pair, under the core lock) */
static inline void s2if_u64_add(ULONG *hi, ULONG *lo, ULONG add)
{
    ULONG prev = *lo;
    *lo = prev + add;
    if (*lo < prev)
        (*hi)++;
}

/* sana2_tx.c */
err_t s2if_linkoutput(struct netif *nif, struct pbuf *p);
void s2if_tx_complete(struct Sana2If *s2i, struct S2TxReq *req); /* core lock */

/* sana2_if.c — the driver-called buffer callbacks (register convention,
 * interrupt-callable: pure copies, no Exec calls, no locks). Prototypes see
 * complete types only (the gcc16 regargs gate). */
BOOL s2if_copy_to_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"));
BOOL s2if_copy_from_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"));

#endif /* LWIPAMIGA_SANA2_PRIV_H */
