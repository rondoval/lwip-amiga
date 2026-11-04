/* SPDX-License-Identifier: BSD-3-Clause */
/* See tcp_tw_reopen.h. */

#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ethernet.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "netif/ethernet.h"

#include "tcp_tw_reopen.h"

#ifndef TCP_TW_REOPEN_NEXT
#define TCP_TW_REOPEN_NEXT ethernet_input
#else
err_t TCP_TW_REOPEN_NEXT(struct pbuf *p, struct netif *inp); /* the host test */
#endif

__attribute__((cold, noinline)) static void tw_reopen_syn(const u8_t *iph, u16_t hlen, const struct netif *inp)
{
    const struct ip_hdr *ip = (const struct ip_hdr *)iph;
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)(iph + hlen);

    /* A fragment is not a whole SYN; a short total length is dropped by ip4_input(). */
    if ((IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0 ||
        lwip_ntohs(IPH_LEN(ip)) < (u16_t)(hlen + TCP_HLEN))
        return;

    const u16_t sport = lwip_ntohs(tcp->src);
    const u16_t dport = lwip_ntohs(tcp->dest);

    for (struct tcp_pcb *pcb = tcp_tw_pcbs; pcb != NULL; pcb = pcb->next)
    {
        if (pcb->remote_port != sport || pcb->local_port != dport ||
            !IP_IS_V4(&pcb->remote_ip) || !IP_IS_V4(&pcb->local_ip) ||
            !ip4_addr_eq(ip_2_ip4(&pcb->remote_ip), &ip->src) ||
            !ip4_addr_eq(ip_2_ip4(&pcb->local_ip), &ip->dest) ||
            (pcb->netif_idx != NETIF_NO_INDEX && pcb->netif_idx != netif_get_index(inp)))
            continue;

        /* Only above the previous incarnation: anything else may be an old
         * duplicate and keeps lwIP's TIME-WAIT behaviour. */
        if (TCP_SEQ_GT(lwip_ntohl(tcp->seqno), pcb->rcv_nxt))
        {
            /* As tcp_slowtmr() does on expiry. Safe here: nothing walks the
             * list while a frame enters the stack. */
            tcp_pcb_purge(pcb);
            TCP_RMV(&tcp_tw_pcbs, pcb);
            tcp_free(pcb);
        }
        return;
    }
}

err_t tcp_tw_reopen_input(struct pbuf *p, struct netif *inp)
{
    if (p->len >= SIZEOF_ETH_HDR + IP_HLEN + TCP_HLEN)
    {
        const u8_t *e = (const u8_t *)p->payload;
        const u8_t *ip = e + SIZEOF_ETH_HDR;
        if (e[12] == 0x08 && e[13] == 0x00 && ip[9] == IP_PROTO_TCP)
        {
            const u16_t hlen = (u16_t)((ip[0] & 0x0F) * 4);
            if ((ip[0] >> 4) == 4 && hlen >= IP_HLEN &&
                p->len >= (u16_t)(SIZEOF_ETH_HDR + hlen + TCP_HLEN) &&
                (ip[hlen + 13] & TCP_FLAGS) == TCP_SYN && tcp_tw_pcbs != NULL)
            {
                tw_reopen_syn(ip, hlen, inp);
            }
        }
    }
    return TCP_TW_REOPEN_NEXT(p, inp);
}
