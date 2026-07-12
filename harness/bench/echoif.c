/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdlib.h>

#include "lwip/ip4.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "echoif.h"

static err_t echoif_output4(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    (void)ipaddr;
    struct echoif *ei = (struct echoif *)nif->state;

    if (ei->loss_permille != 0 &&
        (unsigned)rand_r(&ei->seed) % 1000u < ei->loss_permille) {
        ei->dropped++;
        return ERR_OK; /* the wire ate it */
    }

    unsigned next = (ei->head + 1) % ECHOIF_QUEUE_LEN;
    if (next == ei->tail) {
        ei->overflowed++; /* queue full: congestion drop */
        return ERR_OK;
    }

    /* Clone: the sender keeps its pbuf (TCP holds segments for retransmit) */
    struct pbuf *q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (q == NULL) {
        return ERR_MEM;
    }
    ei->queue[ei->head] = q;
    ei->head = next;
    return ERR_OK;
}

static err_t echoif_init_cb(struct netif *nif)
{
    nif->output = echoif_output4;
    nif->mtu = 1500;
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void echoif_add(struct echoif *ei, const ip4_addr_t *addr, unsigned loss_permille)
{
    ip4_addr_t mask, gw;

    ei->head = ei->tail = 0;
    ei->loss_permille = loss_permille;
    ei->seed = 0x4c774970u;
    ei->delivered = ei->dropped = ei->overflowed = 0;

    IP4_ADDR(&mask, 255, 255, 255, 0);
    ip4_addr_set_any(&gw);
    netif_add(&ei->nif, addr, &mask, &gw, ei, echoif_init_cb, ip4_input);
    netif_set_default(&ei->nif);
    netif_set_up(&ei->nif);
}

unsigned echoif_pump(struct echoif *ei, unsigned budget)
{
    unsigned n = 0;
    while (n < budget && ei->tail != ei->head) {
        struct pbuf *p = ei->queue[ei->tail];
        ei->tail = (ei->tail + 1) % ECHOIF_QUEUE_LEN;
        ei->delivered++;
        n++;
        if (ei->nif.input(p, &ei->nif) != ERR_OK) {
            pbuf_free(p);
        }
    }
    return n;
}
