/* SPDX-License-Identifier: BSD-3-Clause */
/* Host fuzzing config mirroring the TCP-relevant parts of
 * lwip-amiga/port/amiga/include/lwipopts.h (NO_SYS, core TCP sizing),
 * with the fuzz oracle allocator wired in via MEM_LIBC_MALLOC. */
#ifndef FUZZ_LWIPOPTS_H
#define FUZZ_LWIPOPTS_H

#include <stddef.h>

/* --- execution model --- */
#define NO_SYS                          1
#define LWIP_TIMERS                     0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define SYS_LIGHTWEIGHT_PROT            0

/* --- protocols: IPv4 + TCP only --- */
#define LWIP_ARP                        0
#define LWIP_ETHERNET                   0
#define LWIP_ICMP                       0
#define LWIP_RAW                        0
#define LWIP_UDP                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_IPV6                       0
#define IP_FRAG                         0
#define IP_REASSEMBLY                   0

/* --- memory: exact-size recording allocator (fuzz oracle) --- */
#define MEM_LIBC_MALLOC                 1
#define MEM_STATS                       0
void *fz_malloc(size_t sz);
void *fz_calloc(size_t n, size_t sz);
void fz_free(void *p);
#define mem_clib_malloc fz_malloc
#define mem_clib_calloc fz_calloc
#define mem_clib_free   fz_free
#define MEM_ALIGNMENT                   4

/* --- TCP sizing: identical to the Amiga port --- */
#define TCP_MSS                         1460
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   5
#define TCP_WND                         (1024 * 1024)
#define TCP_SND_BUF                     (1024 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_SNDLOWAT                    (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define LWIP_TCP_SACK_OUT               1
#define TCP_LISTEN_BACKLOG              1

/* TCP_OVERSIZE defaults to TCP_MSS (as on the Amiga build; not overridden
 * there either). LWIP_DEBUG turns on TCP_OVERSIZE_DBGCHECK (per-seg shadow),
 * mirroring the Amiga DEBUG builds. Building with -DFUZZ_RELEASE_CFG drops
 * LWIP_DEBUG to mirror the RELEASE tier instead: no shadow bookkeeping and
 * no DBGCHECK-only code paths, but LWIP_ASSERT stays active - exactly like
 * a debug-tier build of the Amiga stack (asserts on). */
#ifndef FUZZ_RELEASE_CFG
#define LWIP_DEBUG                      1
#endif
#define TCP_UNSENT_TAIL_DBGCHECK        1

#define PBUF_POOL_SIZE                  1024

/* --- stats: tcp_helper.c requires TCP+MEMP stats --- */
#define LWIP_STATS                      1
#define TCP_STATS                       1
#define MEMP_STATS                      1
#define LWIP_STATS_DISPLAY              0

#define LWIP_NETIF_LOOPBACK             0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_CHECKSUM_CTRL_PER_NETIF    1

#endif
