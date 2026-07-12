/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Socket objects, lwIP raw-API callbacks, readiness and blocking.
 *
 * Everything here runs under the netstack core lock: API entry points take
 * it, and every callback (driver RX, stack-task timers, DNS) already holds
 * it. That single fact makes the state machines plain sequential code.
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#include <debug.h>

#include "netstack.h"

LONG sb_map_err(signed char e)
{
    switch (e)
    {
    case ERR_OK:
        return 0;
    case ERR_MEM:
    case ERR_BUF:
        return SB_ENOBUFS;
    case ERR_TIMEOUT:
        return SB_ETIMEDOUT;
    case ERR_RTE:
        return SB_ENETUNREACH;
    case ERR_VAL:
    case ERR_ARG:
        return SB_EINVAL;
    case ERR_WOULDBLOCK:
        return SB_EWOULDBLOCK;
    case ERR_USE:
        return SB_EADDRINUSE;
    case ERR_ALREADY:
        return SB_EALREADY;
    case ERR_ISCONN:
        return SB_EISCONN;
    case ERR_CONN:
        return SB_ENOTCONN;
    case ERR_ABRT:
        return SB_ECONNABORTED;
    case ERR_RST:
        return SB_ECONNRESET;
    case ERR_CLSD:
        return SB_ESHUTDOWN;
    default:
        return SB_EINVAL;
    }
}

void sb_wake(struct SbSocket *s)
{
    struct SocketBase *b = s->owner;
    if (b != NULL && b->task != NULL)
        Signal(b->task, 1UL << b->sigBit);
}

/* Drop the core lock and sleep until this base's socket signal or a break
 * signal arrives. Returns 0 or SB_EINTR. State can only move while we are
 * outside the lock, so callers re-test their condition afterwards. */
LONG sb_wait(struct SocketBase *base)
{
    SetSignal(0UL, 1UL << base->sigBit);
    netstack_unlock();
    ULONG sigs = Wait((1UL << base->sigBit) | base->breakMask);
    netstack_lock();

    if (sigs & base->breakMask)
    {
        /* the break stays posted, per the WaitSelect contract; apply the
         * same rule to every blocking call */
        Signal(base->task, sigs & base->breakMask);
        return SB_EINTR;
    }
    return 0;
}

BOOL sb_sock_readable(const struct SbSocket *s)
{
    if (s->err != 0 || s->rxeof)
        return TRUE;
    if (s->type == SBT_TCP)
        return s->rxq != NULL || s->naccept != 0;
    return s->ndgrams != 0;
}

BOOL sb_sock_writable(struct SbSocket *s)
{
    if (s->err != 0)
        return TRUE;
    if (s->type == SBT_TCP)
    {
        if (s->connecting)
            return FALSE;
        if (!s->connected || s->pcb.tcp == NULL)
            return TRUE; /* write attempt fails immediately — that is "ready" */
        return tcp_sndbuf(s->pcb.tcp) > 0;
    }
    return TRUE;
}

/* ------------------------------------------------------- TCP callbacks --- */

static err_t sb_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct SbSocket *s = arg;
    (void)tpcb;
    (void)err;

    if (s == NULL)
    {
        if (p != NULL)
            pbuf_free(p);
        return ERR_OK;
    }

    if (p == NULL)
    {
        s->rxeof = TRUE;
    }
    else if (s->rxq == NULL)
    {
        s->rxq = p;
    }
    else
    {
        pbuf_cat(s->rxq, p);
    }

    sb_wake(s);
    return ERR_OK;
}

static err_t sb_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    struct SbSocket *s = arg;
    (void)tpcb;
    (void)len;
    if (s != NULL)
        sb_wake(s);
    return ERR_OK;
}

static void sb_tcp_err_cb(void *arg, err_t err)
{
    struct SbSocket *s = arg;
    if (s == NULL)
        return;

    /* the pcb is already gone */
    s->pcb.any = NULL;
    s->err = (s->connecting && err == ERR_RST) ? SB_ECONNREFUSED : sb_map_err(err);
    s->connecting = FALSE;
    s->connected = FALSE;
    sb_wake(s);
}

err_t sb_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    struct SbSocket *s = arg;
    (void)tpcb;

    if (s != NULL)
    {
        if (err == ERR_OK)
        {
            s->connecting = FALSE;
            s->connected = TRUE;
        }
        else
        {
            s->err = sb_map_err(err);
        }
        sb_wake(s);
    }
    return ERR_OK;
}

void sb_tcp_wire(struct SbSocket *s)
{
    tcp_arg(s->pcb.tcp, s);
    tcp_recv(s->pcb.tcp, sb_tcp_recv_cb);
    tcp_sent(s->pcb.tcp, sb_tcp_sent_cb);
    tcp_err(s->pcb.tcp, sb_tcp_err_cb);
}

static err_t sb_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    struct SbSocket *lst = arg;

    if (lst == NULL || err != ERR_OK || newpcb == NULL)
        return ERR_VAL;

    struct SbSocket *s = NULL;
    if (lst->naccept < SB_ACCEPT_QMAX)
        s = sb_sock_alloc(lst->owner, SBT_TCP);
    if (s == NULL)
    {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    s->pcb.tcp = newpcb;
    s->connected = TRUE;
    sb_tcp_wire(s);

    AddTailMinList(&lst->acceptq, &s->node);
    lst->naccept++;
    sb_wake(lst);
    return ERR_OK;
}

/* ------------------------------------------------- UDP / RAW callbacks --- */

static void sb_dgram_queue(struct SbSocket *s, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct SocketBase *root = s->rootBase;

    if (s->ndgrams >= SB_DGRAM_QMAX)
    {
        pbuf_free(p);
        return;
    }

    struct SbDgram *d = AllocPooled(root->sockPool, sizeof(struct SbDgram));
    if (d == NULL)
    {
        pbuf_free(p);
        return;
    }

    d->p = p;
    d->addr = ip4_addr_get_u32(ip_2_ip4(addr));
    d->port = port;
    AddTailMinList(&s->dgrams, &d->node);
    s->ndgrams++;
    sb_wake(s);
}

static void sb_udp_recv_cb(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                           const ip_addr_t *addr, u16_t port)
{
    struct SbSocket *s = arg;
    (void)upcb;

    if (s == NULL)
    {
        pbuf_free(p);
        return;
    }
    sb_dgram_queue(s, p, addr, port);
}

static u8_t sb_raw_recv_cb(void *arg, struct raw_pcb *rpcb, struct pbuf *p,
                           const ip_addr_t *addr)
{
    struct SbSocket *s = arg;
    (void)rpcb;

    if (s == NULL)
        return 0;

    /* raw sockets deliver the packet including the IP header (BSD) — lwIP
     * hands it to us that way already */
    sb_dgram_queue(s, p, addr, 0);
    return 1; /* consumed */
}

/* ---------------------------------------------------------- lifecycle --- */

struct SbSocket *sb_sock_alloc(struct SocketBase *base, SbSockType type)
{
    struct SocketBase *root = SB_ROOT(base);

    struct SbSocket *s = AllocPooled(root->sockPool, sizeof(struct SbSocket));
    if (s == NULL)
        return NULL;

    for (ULONG i = 0; i < sizeof(*s); i++)
        ((UBYTE *)s)[i] = 0;

    s->owner = base;
    s->rootBase = root;
    s->refs = 1;
    s->type = (UBYTE)type;
    _NewMinList(&s->dgrams);
    _NewMinList(&s->acceptq);
    return s;
}

/* Drop one fd reference; tear the socket down on the last: detach callbacks,
 * close/abort the pcb, flush queues. Core lock held. */
void sb_sock_free(struct SocketBase *base, struct SbSocket *s)
{
    struct SocketBase *root = s->rootBase;
    (void)base;

    if (--s->refs != 0)
        return;

    switch (s->type)
    {
    case SBT_TCP:
        if (s->pcb.tcp != NULL)
        {
            struct tcp_pcb *pcb = s->pcb.tcp;
            s->pcb.tcp = NULL;
            tcp_arg(pcb, NULL);
            if (s->listening)
            {
                tcp_accept(pcb, NULL);
                tcp_close(pcb);
            }
            else
            {
                tcp_recv(pcb, NULL);
                tcp_sent(pcb, NULL);
                tcp_err(pcb, NULL);
                if (tcp_close(pcb) != ERR_OK)
                    tcp_abort(pcb);
            }
        }
        break;
    case SBT_UDP:
        if (s->pcb.udp != NULL)
        {
            udp_remove(s->pcb.udp);
            s->pcb.udp = NULL;
        }
        break;
    case SBT_RAW:
        if (s->pcb.raw != NULL)
        {
            raw_remove(s->pcb.raw);
            s->pcb.raw = NULL;
        }
        break;
    }

    if (s->rxq != NULL)
    {
        pbuf_free(s->rxq);
        s->rxq = NULL;
    }

    struct MinNode *n;
    while ((n = RemHeadMinList(&s->dgrams)) != NULL)
    {
        struct SbDgram *d = (struct SbDgram *)n;
        pbuf_free(d->p);
        FreePooled(root->sockPool, d, sizeof(struct SbDgram));
    }
    s->ndgrams = 0;

    while ((n = RemHeadMinList(&s->acceptq)) != NULL)
    {
        struct SbSocket *q = (struct SbSocket *)n;
        sb_sock_free(root, q);
    }
    s->naccept = 0;

    FreePooled(root->sockPool, s, sizeof(struct SbSocket));
}

LONG sb_fd_alloc(struct SocketBase *base, struct SbSocket *s)
{
    for (LONG i = 0; i < SB_FD_COUNT; i++)
    {
        if (base->fd[i] == NULL)
        {
            base->fd[i] = s;
            return i;
        }
    }
    return -1;
}

struct SbSocket *sb_fd_get(struct SocketBase *base, LONG fd)
{
    if (fd < 0 || fd >= SB_FD_COUNT || base->fd == NULL)
        return NULL;
    return base->fd[fd];
}

void sb_listen_wire(struct SbSocket *s)
{
    tcp_arg(s->pcb.tcp, s);
    tcp_accept(s->pcb.tcp, sb_tcp_accept_cb);
}

void sb_udp_wire(struct SbSocket *s)
{
    udp_recv(s->pcb.udp, sb_udp_recv_cb, s);
}

void sb_raw_wire(struct SbSocket *s)
{
    raw_recv(s->pcb.raw, sb_raw_recv_cb, s);
}
