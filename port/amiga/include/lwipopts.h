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
#define LWIP_ASSERT_CORE_LOCKED()       netstack_assert_locked()

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
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_SUPPORT_CUSTOM_PBUF        1   /* RX buffers are driver-owned */

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
#endif
#define MEMP_NUM_TCP_PCB                64
#define MEMP_NUM_TCP_PCB_LISTEN         16
#define MEMP_NUM_UDP_PCB                32
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
#define PBUF_POOL_SIZE                  64   /* loopback traffic only */

/* --- TCP, sized for the wire-speed goal (validated by harness/tcpbench:
 * scaling negotiates; recovery is fast-retransmit only — lwIP emits SACKs
 * but does not use received ones, so lossy paths degrade to RTO) --- */
#define TCP_MSS                         1460
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   4
#define TCP_WND                         (256 * 1024)
#define TCP_SND_BUF                     (256 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_SNDLOWAT                    (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define LWIP_TCP_SACK_OUT               1
/* init.c's sanity check requires PBUF_POOL to cover TCP_WND, assuming RX
 * allocates from it. Our RX buffers are driver-owned custom pbufs — the
 * pool only serves loopback — so that premise doesn't hold here. The other
 * TCP sizing relations were validated with checks enabled (harness/bench). */
#define LWIP_DISABLE_TCP_SANITY_CHECKS  1
#define TCP_LISTEN_BACKLOG              1
#define LWIP_TCP_KEEPALIVE              1
#define SO_REUSE                        1
/* Initial RTO 500 ms (lwIP default 3000). Mitigation for a router-side
 * quirk: the first inbound packet of a fresh NAT flow after idle gets
 * dropped upstream (MIB-verified 2026-07-13 — frame never reaches the
 * MAC), so the first SYN retransmit must beat the 1 s connect timeout
 * common in apps. Harmless on the LAN (RTT ~15 ms); reconsider if the
 * router's flow-offload behavior ever gets fixed. */
#define LWIP_TCP_RTO_TIME               500

/* --- checksums: everything on at compile time, disabled per netif when
 * the driver's capabilities cover it (netdev_if.c) --- */
#define LWIP_CHECKSUM_CTRL_PER_NETIF    1

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
