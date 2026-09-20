/* SPDX-License-Identifier: BSD-3-Clause */
/* Host test config for the LWIP_TCP_ACK_AGGREGATES regression tests: the
 * TCP-relevant parts of lwip-amiga/port/amiga/include/lwipopts.h, as in
 * test/urg. build.sh compiles it twice — ACKAGG=1 (the port's setting) and
 * ACKAGG=0 (stock lwIP) — so the tests pin down both behaviours and prove
 * they can tell them apart. */
#ifndef ACKAGG_LWIPOPTS_H
#define ACKAGG_LWIPOPTS_H

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

/* --- memory: plain libc malloc --- */
#define MEM_LIBC_MALLOC                 1
#define MEM_STATS                       0
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

/* --- the feature under test --- */
#ifndef ACKAGG
#define ACKAGG                          1
#endif
#define LWIP_TCP_ACK_AGGREGATES         ACKAGG

#define LWIP_DEBUG                      1
#define TCP_UNSENT_TAIL_DBGCHECK        1

/* one 44-frame aggregate is ~43 pool pbufs; leave room for ooseq scenarios */
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
