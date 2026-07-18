/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * lwIP configuration for AmigaOS — the real port.
 *
 * Execution model: NO_SYS=1 with EXTERNAL serialization. lwIP's own
 * threading/mailbox machinery is unused; instead every entry into the core
 * (bsdsocket calls from app tasks, RX injection from driver tasks, the
 * timer tick from the stack task) runs under the netstack core semaphore
 * (see netstack.h). That is the LWIP_TCPIP_CORE_LOCKING idea implemented
 * with an Exec SignalSemaphore and without the tcpip_thread — blocking
 * socket semantics live in bsdsocket.library on Exec signals, not in lwIP.
 * No lwIP call ever happens in interrupt context, so SYS_LIGHTWEIGHT_PROT
 * is off and no sys_arch primitives are needed beyond sys_now().
 *
 * Memory: the lwIP heap (PBUF_RAM — i.e. every TX payload) is routed to
 * the netstack allocator, which draws from the attached netdev driver's
 * DMA allocator; the stack never AllocMem()s packet memory itself. The
 * memp pools are static arrays in the library's data space.
 */

#ifndef LWIPAMIGA_PORT_LWIPOPTS_H
#define LWIPAMIGA_PORT_LWIPOPTS_H

/* --- execution model --- */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TCPIP_CORE_LOCKING         0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
/* PROFILE_LEAN (EMU68_PROFILE_LEAN=lwip-amiga): the release-truth profiling
 * tier — DEBUG backend + nsprof stay, but the assert-class overhead that
 * inflates DEBUG profiles goes: LWIP_NOASSERT kills every LWIP_ASSERT, and
 * ASSERT_CORE_LOCKED stops costing a FindTask() per lwIP entry point.
 * Functional LWIP_ERROR guards are unaffected. */
#ifdef PROFILE_LEAN
#define LWIP_NOASSERT
#define LWIP_ASSERT_CORE_LOCKED()
#else
#define LWIP_ASSERT_CORE_LOCKED()       netstack_assert_locked()
#endif

/* --- protocols, v1 scope --- */
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1   /* SOCK_RAW (ping) via bsdsocket */
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_IGMP                       1   /* multicast (mDNS later) */
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_IPV6                       0   /* design stays v6-ready; off in v1 */

#define LWIP_NETIF_LOOPBACK             1   /* 127.0.0.1 for local apps */
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_HOSTNAME             1   /* netstack.prefs HOSTNAME; DHCP option 12 */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_SUPPORT_CUSTOM_PBUF        1   /* RX buffers are driver-owned */

/* --- in-band 802.1Q VLAN ---
 * Software VLAN: the tag rides inside the frame; the driver moves opaque
 * bytes (no NIC HW VLAN offload). The VID is per-interface (netstack.prefs
 * VLAN=), read from netif->state by the hooks in netstack_lwiphooks.h.
 * LWIP_VLAN_PCP is deliberately OFF (its per-PCB tci path is dead once the
 * SET hook is defined, and there is no LWIP_SOCKET API to set per-PCB tci).
 * PBUF_LINK_HLEN must be pinned to 18 here: opt.h derives it BEFORE any .c
 * sees LWIP_HOOK_FILENAME, so it would otherwise stay 14 and every tagged
 * TX frame would fail pbuf_add_header(SIZEOF_VLAN_HDR). */
#define ETHARP_SUPPORT_VLAN             1
#define PBUF_LINK_HLEN                  (18 + ETH_PAD_SIZE)
#define LWIP_HOOK_FILENAME              "netstack_lwiphooks.h"

/* --- memory --- */
#define MEM_ALIGNMENT                   4
#define MEM_CUSTOM_ALLOCATOR            1
#define MEM_CUSTOM_MALLOC               netstack_malloc
#define MEM_CUSTOM_CALLOC               netstack_calloc
#define MEM_CUSTOM_FREE                 netstack_free

#define MEMP_NUM_PBUF                   512  /* PBUF_REF/ROM headers (TX) */
#if defined(DEBUG) && defined(DEBUG_HIGH)
/* 2026-07-14 corruption hunt: tcp_seg/pcb pools are static arrays outside
 * the guarded netstack heap — let lwIP police them too. DEBUG_HIGH tier:
 * OVERFLOW_CHECK pads every memp element, changing pool layout (see the
 * heap-guard note in netstack.c). */
#define MEMP_OVERFLOW_CHECK             1
#define MEMP_SANITY_CHECK               1
/* Walk-and-compare check of the forked lwIP's cached pcb->unsent_tail —
 * O(n) per call, so DEBUG_HIGH only. */
#define TCP_UNSENT_TAIL_DBGCHECK        1
#endif
#define MEMP_NUM_TCP_PCB                256
#define MEMP_NUM_TCP_PCB_LISTEN         16
#define MEMP_NUM_UDP_PCB                32
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
#define PBUF_POOL_SIZE                  64   /* loopback traffic only */

/* --- IP reassembly: each RX frame is one driver-owned custom pbuf, so one
 * IP fragment costs one pbuf. lwIP's default cap of 10 held pbufs limits a
 * reassembled datagram to ~10 x 1480 = 14.8 KB, silently dropping anything
 * larger. Size for a full 64 KB datagram: ceil(65535/1480) = 45 fragments.
 * RX-wrap headroom is ~784 (netdev_if.c), so this is comfortably covered. */
#define IP_REASS_MAX_PBUFS              48

/* --- IP fragmentation (TX): symmetric to reassembly. ip4_frag emits one
 * PBUF_REF per fragment (MEMP_FRAG_PBUF pool), and the driver holds each
 * frame's pbuf ref until async TX-done — so every fragment of a datagram is
 * outstanding at once. lwIP's default pool of 15 caps a fragmented send at
 * 15 x 1480 = ~21.7 KB, returning ERR_MEM (-> ENOBUFS) above it. Size for a
 * full 64 KB datagram: ceil(65515/1480) = 45 fragments. The genet TX ring
 * (TX_DESCS = 256) covers the descriptor side. */
#define MEMP_NUM_FRAG_PBUF              48

/* --- TCP, sized for the wire-speed goal: window scaling negotiates, and
 * recovery is fast-retransmit only — lwIP emits SACKs but does not use
 * received ones, so lossy paths degrade to RTO --- */
#define TCP_MSS                         1460
#define LWIP_WND_SCALE                  1
/* Window sized for high-BDP internet paths, not just LAN: throughput caps at
 * TCP_WND/RTT, so 256 KB ceilinged a clean 30 ms path at ~70 Mbit. 1 MB covers
 * ~1 Gbit up to ~8 ms RTT / ~270 Mbit at 30 ms. RCV_SCALE=5 (2 MB advertisable)
 * clears the 65535<<4 = 1048560 ceiling so a full 1 MB window is advertised.
 * Cost: the RX hold budget (4*ceil(TCP_WND/TCP_MSS)+64 wraps, netdev_if.c) and
 * the send-side seg pool scale with this — ~6 MB of genet RX buffers now.
 * Bigger is wasted until received-SACK recovery and the ns_Core lock ceiling
 * land (docs/TODO.md). */
#define TCP_RCV_SCALE                   5
#define TCP_WND                         (1024 * 1024)
#define TCP_SND_BUF                     (1024 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_SNDLOWAT                    (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define LWIP_TCP_SACK_OUT               1
/* init.c's sanity check requires PBUF_POOL to cover TCP_WND, assuming RX
 * allocates from it. Our RX buffers are driver-owned custom pbufs — the
 * pool only serves loopback — so that premise doesn't hold here. */
#define LWIP_DISABLE_TCP_SANITY_CHECKS  1
#define TCP_LISTEN_BACKLOG              1
#define LWIP_TCP_KEEPALIVE              1
#define SO_REUSE                        1

/* --- checksums: everything on at compile time, disabled per netif when
 * the driver's capabilities cover it (netdev_if.c) --- */
#define LWIP_CHECKSUM_CTRL_PER_NETIF    1

/* --- statistics: back GetNetworkStatistics and the IFQ / SBTC byte counters.
 * LWIP_STATS_LARGE makes the protocol counters 32-bit (u16 would wrap under
 * load); MIB2_STATS adds the SNMP MIB-2 counters (per-protocol + per-netif,
 * e.g. ifinunknownprotos) that the BSD stat structs are mapped from. */
#define LWIP_STATS_LARGE                1
#define MIB2_STATS                      1

/* --- platform glue --- */
unsigned int netstack_lwip_rand(void);
void *netstack_malloc(unsigned int size);
void *netstack_calloc(unsigned int count, unsigned int size);
void netstack_free(void *ptr);
void netstack_assert_locked(void);
void netstack_platform_diag(const char *msg);

#define LWIP_RAND()                     ((u32_t)netstack_lwip_rand())
#define LWIP_PLATFORM_ASSERT(x)         netstack_platform_diag(x)

/* --- lwIP debug output ---
 * lwIP diag format strings use 32-bit %d/%u, which RawDoFmt-based output
 * would misread as 16-bit (fleet gotcha) — so LWIP_PLATFORM_DIAG goes
 * through netstack_diag_printf, which formats with C argument promotion
 * and hands the backend a finished string. Follows the stack-wide DEBUG;
 * toggle modules below as the hunt moves. */
#ifdef DEBUG
#define LWIP_DEBUG
void netstack_diag_printf(const char *fmt, ...);
#define LWIP_PLATFORM_DIAG(x)           netstack_diag_printf x
#define DHCP_DEBUG                      LWIP_DBG_ON
/* UDP/IP debug print PER PACKET — each Kprintf char is a trap, so under
 * load the logging throttles the very traffic being debugged. Flip on only
 * for connectivity hunts, never for throughput work. */
#define UDP_DEBUG                       LWIP_DBG_OFF
#define IP_DEBUG                        LWIP_DBG_OFF
#endif

#endif /* LWIPAMIGA_PORT_LWIPOPTS_H */
