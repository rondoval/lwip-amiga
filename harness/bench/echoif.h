/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * echoif — an in-process lwIP netif whose output re-enters its own input
 * queue, with configurable random packet loss. A lossy loopback wire: TCP
 * connections from the netif's address to itself flow through it packet by
 * packet, so retransmission, SACK and window behavior are exercised for
 * real. IP-level (no ethernet framing).
 */

#ifndef LWIPAMIGA_BENCH_ECHOIF_H
#define LWIPAMIGA_BENCH_ECHOIF_H

#include "lwip/netif.h"

#define ECHOIF_QUEUE_LEN 1024

struct echoif
{
    struct netif nif;
    struct pbuf *queue[ECHOIF_QUEUE_LEN];
    unsigned head, tail;          /* single-threaded ring */
    unsigned loss_permille;       /* drop rate: 0..1000 */
    unsigned seed;                /* deterministic rand_r state */
    unsigned long delivered, dropped, overflowed;
};

/* Add the netif with @addr/24, up and default-routed. */
void echoif_add(struct echoif *ei, const ip4_addr_t *addr, unsigned loss_permille);

/* Deliver up to @budget queued packets into the stack. Returns delivered. */
unsigned echoif_pump(struct echoif *ei, unsigned budget);

#endif /* LWIPAMIGA_BENCH_ECHOIF_H */
