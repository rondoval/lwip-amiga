/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lwIP configuration for the Linux host harness.
 *
 * NO_SYS single-threaded polling: the harness drives sys_check_timeouts() and
 * netif_poll_all() from its main loop. This exists to prove the core builds and
 * moves data; the Amiga port gets its own lwipopts.h with core locking enabled.
 */

#ifndef LWIPAMIGA_HARNESS_LWIPOPTS_H
#define LWIPAMIGA_HARNESS_LWIPOPTS_H

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0

#define LWIP_SOCKET             0
#define LWIP_NETCONN            0

#define LWIP_NETIF_LOOPBACK     1
#define LWIP_HAVE_LOOPIF        1

#define MEM_ALIGNMENT           8
#define MEM_SIZE                (256 * 1024)
#define MEMP_NUM_TCP_SEG        64
#define PBUF_POOL_SIZE          64

#define TCP_MSS                 1460
#define TCP_WND                 (4 * TCP_MSS)
#define TCP_SND_BUF             (16 * TCP_MSS)

#define LWIP_RAND()             ((u32_t)rand())

#endif /* LWIPAMIGA_HARNESS_LWIPOPTS_H */
