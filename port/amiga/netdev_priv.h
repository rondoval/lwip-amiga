/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Internals shared by the netdev_*.c compilation units (RX, TX, lifecycle).
 * Not part of the port's public surface — port/amiga/include is on the
 * netstack target's PUBLIC include path, this file deliberately is not.
 */

#ifndef LWIPAMIGA_NETDEV_PRIV_H
#define LWIPAMIGA_NETDEV_PRIV_H

#include "inet_frame.h"
#include "netdev_if.h"

/* One RX buffer in flight to lwIP: a custom pbuf wrapping driver memory.
 * Freeing the pbuf recycles the driver buffer. Wrappers live in a free
 * list mutated only under the core lock. */
struct NdRxWrap
{
    struct pbuf_custom nrw_Pc; /* must stay first */
    APTR nrw_Cookie;
    struct NetdevIf *nrw_If;
    struct NdRxWrap *nrw_Next;
#ifdef DEBUG
    ULONG nrw_Live; /* double-free tripwire: 1 while lent to lwIP */
#endif
};

/* netdev_rx.c — the NetDevStackOps RX entry */
ULONG ndif_rx_input(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count);

/* netdev_tx.c — the NetDevStackOps TX-reclaim entry and the netif outputs */
void ndif_tx_done(APTR stackctx, APTR const *cookies, ULONG count);
err_t ndif_linkoutput(struct netif *nif, struct pbuf *p);
err_t ndif_ip4_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr);

#endif /* LWIPAMIGA_NETDEV_PRIV_H */
