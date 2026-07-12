/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lwIP configuration for the TCP behavior benchmark (tcpbench).
 *
 * Differs from the smoke-test opts: loopback is OFF so self-addressed
 * traffic travels through the lossy echoif instead of short-circuiting,
 * and the TCP features under validation for the wire-speed goal are ON:
 * window scaling (RFC 7323) and SACK emission (RFC 2018).
 */

#ifndef LWIPAMIGA_BENCH_LWIPOPTS_H
#define LWIPAMIGA_BENCH_LWIPOPTS_H

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0

#define LWIP_SOCKET             0
#define LWIP_NETCONN            0

#define LWIP_NETIF_LOOPBACK     0
#define LWIP_HAVE_LOOPIF        0

#define MEM_ALIGNMENT           8
#define MEM_SIZE                (8 * 1024 * 1024)
#define MEMP_NUM_PBUF           1024
#define PBUF_POOL_SIZE          1024

#define TCP_MSS                 1460
#define LWIP_WND_SCALE          1
#define TCP_RCV_SCALE           4                   /* advertise up to 1 MB */
#define TCP_WND                 (256 * 1024)
#define TCP_SND_BUF             (256 * 1024)
#define TCP_SND_QUEUELEN        ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_SNDLOWAT            (8 * TCP_MSS)       /* default SND_BUF/2 overflows u16 math */
#define MEMP_NUM_TCP_SEG        TCP_SND_QUEUELEN
#define LWIP_TCP_SACK_OUT       1

#define LWIP_RAND()             ((u32_t)rand())

#endif /* LWIPAMIGA_BENCH_LWIPOPTS_H */
