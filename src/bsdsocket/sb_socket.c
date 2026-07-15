/* SPDX-License-Identifier: BSD-3-Clause */
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
    KprintfH("[bsdsocket] %s: e=%ld\n", __func__, (LONG)e);
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

/* ------------------------------------------------------------- ownership --- */

struct SocketBase *sb_owner_first(struct SbSocket *s)
{
    for (ULONG i = 0; i < SB_SOCK_OWNERS; i++)
        if (s->owners[i].base != NULL)
            return s->owners[i].base;
    return NULL;
}

BOOL sb_owner_incref(struct SbSocket *s, struct SocketBase *b)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx b=0x%08lx\n", __func__, (ULONG)s, (ULONG)b);
    struct SbOwnerRef *slot = NULL;
    for (ULONG i = 0; i < SB_SOCK_OWNERS; i++)
    {
        if (s->owners[i].base == b)
        {
            s->owners[i].fdrefs++;
            return TRUE;
        }
        if (slot == NULL && s->owners[i].base == NULL)
            slot = &s->owners[i];
    }
    if (slot == NULL)
        return FALSE; /* SB_SOCK_OWNERS distinct owners already */
    slot->base = b;
    slot->fdrefs = 1;
    return TRUE;
}

void sb_owner_decref(struct SbSocket *s, struct SocketBase *b)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx b=0x%08lx\n", __func__, (ULONG)s, (ULONG)b);
    for (ULONG i = 0; i < SB_SOCK_OWNERS; i++)
    {
        if (s->owners[i].base == b)
        {
            if (--s->owners[i].fdrefs == 0)
                s->owners[i].base = NULL;
            return;
        }
    }
}

void sb_wake(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    /* Wake every owner: normally one, but a socket shared via
     * ReleaseCopyOfSocket has several and all of them must be woken (the
     * 2026 "last claimant only" gap). sigIoMask = AmiTCP-style async SIGIO
     * (SetSocketSignals / SBTC_SIGIOMASK): apps park in Wait() on it instead
     * of WaitSelect — every readiness change must deliver it or they hang
     * (AExplorer post-accept, 2026-07-14). Spurious delivery is fine, SIGIO
     * consumers re-poll. sigEventMask delivered likewise until a real
     * GetSocketEvents queue exists. */
    for (ULONG i = 0; i < SB_SOCK_OWNERS; i++)
    {
        struct SocketBase *b = s->owners[i].base;
        if (b != NULL && b->task != NULL)
            Signal(b->task, (1UL << b->sigBit) | b->sigIoMask | b->sigEventMask);
    }
}

/* Record FD_* events for GetSocketEvents and signal the owner's event
 * mask (SBTC_SIGEVENTMASK). Core lock held — every caller is an lwIP
 * callback or an API path under the lock. Only subscribed bits accumulate,
 * so unsubscribed sockets cost one masked AND. */
void sb_event(struct SbSocket *s, ULONG ev)
{
    ev &= s->eventMask;
    if (ev == 0)
        return;
    s->eventsPending |= ev;
    for (ULONG i = 0; i < SB_SOCK_OWNERS; i++)
    {
        struct SocketBase *b = s->owners[i].base;
        if (b != NULL && b->task != NULL && b->sigEventMask != 0)
            Signal(b->task, b->sigEventMask);
    }
}

/* Drop the core lock and sleep until this base's socket signal or a break
 * signal arrives. Returns 0 or SB_EINTR. State can only move while we are
 * outside the lock, so callers re-test their condition afterwards. */
LONG sb_wait(struct SocketBase *base)
{
    KprintfH("[bsdsocket] %s: base=0x%08lx\n", __func__, (ULONG)base);
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

/* sb_wait with a per-call deadline (SO_RCVTIMEO/SO_SNDTIMEO). The first
 * blocking iteration latches the deadline in @tw; each call arms the
 * opener's timer for the remainder and fully reclaims it (message AND port
 * signal — a stale signal would derail the next timed wait) before
 * returning. The deadline check is the authority, not the timer request:
 * a wake racing the expiry returns 0 and the next iteration reports
 * SB_EWOULDBLOCK. */
LONG sb_wait_to(struct SocketBase *base, ULONG ms, struct SbTimedWait *tw)
{
    KprintfH("[bsdsocket] %s: base=0x%08lx ms=%lu\n", __func__, (ULONG)base, ms);
    if (ms == 0 || base->timerReq == NULL)
        return sb_wait(base);

    ULONG now = netstack_now_ms();
    if (!tw->set)
    {
        tw->deadlineMs = now + ms;
        tw->set = TRUE;
    }
    if ((LONG)(tw->deadlineMs - now) <= 0)
        return SB_EWOULDBLOCK;
    ULONG left = tw->deadlineMs - now;

    struct timerequest *tr = base->timerReq;
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = left / 1000;
    tr->tr_time.tv_micro = (left % 1000) * 1000;
    SendIO(&tr->tr_node);

    ULONG timerBit = 1UL << base->timerPort->mp_SigBit;
    SetSignal(0UL, 1UL << base->sigBit);
    netstack_unlock();
    ULONG sigs = Wait((1UL << base->sigBit) | base->breakMask | timerBit);

    if (CheckIO(&tr->tr_node) == NULL)
        AbortIO(&tr->tr_node);
    WaitIO(&tr->tr_node);
    SetSignal(0UL, timerBit);
    netstack_lock();

    if (sigs & base->breakMask)
    {
        Signal(base->task, sigs & base->breakMask);
        return SB_EINTR;
    }
    return 0;
}

BOOL sb_sock_readable(const struct SbSocket *s)
{
    // KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    if (s->err != 0 || s->rxeof)
        return TRUE;
    if (s->type == SBT_TCP)
        return s->rxq != NULL || s->naccept != 0;
    return s->ndgrams != 0;
}

BOOL sb_sock_writable(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
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
    KprintfH("[bsdsocket] %s: s=0x%08lx p=0x%08lx err=%ld\n", __func__, (ULONG)arg, (ULONG)p, (LONG)err);
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
    else
    {
        /* tail-linked, own byte count: pbuf_cat would walk the whole chain
         * per segment AND its u16 tot_len bookkeeping overflows at 64 KB */
        if (s->rxq == NULL)
            s->rxq = p;
        else
            s->rxqTail->next = p;
        struct pbuf *t = p;
        while (t->next != NULL)
            t = t->next;
        s->rxqTail = t;
        s->rxBytes += p->tot_len;
    }

    sb_event(s, p == NULL ? SB_FD_CLOSE : SB_FD_READ);
    sb_wake(s);
    return ERR_OK;
}

static err_t sb_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx len=%lu\n", __func__, (ULONG)arg, (ULONG)len);
    struct SbSocket *s = arg;
    (void)tpcb;
    (void)len;
    if (s != NULL)
    {
        sb_event(s, SB_FD_WRITE);
        sb_wake(s);
    }
    return ERR_OK;
}

static void sb_tcp_err_cb(void *arg, err_t err)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx err=%ld\n", __func__, (ULONG)arg, (LONG)err);
    struct SbSocket *s = arg;
    if (s == NULL)
        return;

    /* the pcb is already gone */
    s->pcb.any = NULL;
    s->err = (s->connecting && err == ERR_RST) ? SB_ECONNREFUSED : sb_map_err(err);
    s->connecting = FALSE;
    s->connected = FALSE;
    sb_event(s, SB_FD_ERROR | SB_FD_CLOSE);
    sb_wake(s);
}

err_t sb_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx err=%ld\n", __func__, (ULONG)arg, (LONG)err);
    struct SbSocket *s = arg;
    (void)tpcb;

    if (s != NULL)
    {
        if (err == ERR_OK)
        {
            s->connecting = FALSE;
            s->connected = TRUE;
            sb_event(s, SB_FD_CONNECT);
        }
        else
        {
            s->err = sb_map_err(err);
            sb_event(s, SB_FD_ERROR);
        }
        sb_wake(s);
    }
    return ERR_OK;
}

void sb_tcp_wire(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    tcp_arg(s->pcb.tcp, s);
    tcp_recv(s->pcb.tcp, sb_tcp_recv_cb);
    tcp_sent(s->pcb.tcp, sb_tcp_sent_cb);
    tcp_err(s->pcb.tcp, sb_tcp_err_cb);
}

static err_t sb_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    KprintfH("[bsdsocket] %s: lst=0x%08lx newpcb=0x%08lx err=%ld\n", __func__, (ULONG)arg, (ULONG)newpcb, (LONG)err);
    struct SbSocket *lst = arg;

    if (lst == NULL || err != ERR_OK || newpcb == NULL)
        return ERR_VAL;

    struct SbSocket *s = NULL;
    if (lst->naccept < SB_ACCEPT_QMAX)
        s = sb_sock_alloc(sb_owner_first(lst), SBT_TCP);
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
    sb_event(lst, SB_FD_ACCEPT);
    sb_wake(lst);
    return ERR_OK;
}

/* ------------------------------------------------- UDP / RAW callbacks --- */

static void sb_dgram_queue(struct SbSocket *s, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx len=%lu port=%lu\n", __func__, (ULONG)s, (ULONG)p->tot_len, (ULONG)port);
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
    sb_event(s, SB_FD_READ);
    sb_wake(s);
}

static void sb_udp_recv_cb(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                           const ip_addr_t *addr, u16_t port)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx p=0x%08lx port=%lu\n", __func__, (ULONG)arg, (ULONG)p, (ULONG)port);
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
    KprintfH("[bsdsocket] %s: s=0x%08lx p=0x%08lx\n", __func__, (ULONG)arg, (ULONG)p);
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
    KprintfH("[bsdsocket] %s: type=%ld\n", __func__, (LONG)type);
    struct SocketBase *root = SB_ROOT(base);

    struct SbSocket *s = AllocPooled(root->sockPool, sizeof(struct SbSocket));
    if (s == NULL)
        return NULL;

    for (ULONG i = 0; i < sizeof(*s); i++)
        ((UBYTE *)s)[i] = 0;

    s->owners[0].base = base;
    s->owners[0].fdrefs = 1;
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
    KprintfH("[bsdsocket] %s: s=0x%08lx refs=%lu\n", __func__, (ULONG)s, (ULONG)s->refs);
    struct SocketBase *root = s->rootBase;

    sb_owner_decref(s, base); /* this base gives up one fd on the socket */
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
        pbuf_free(s->rxq); /* frees the whole tail-linked chain */
        s->rxq = NULL;
    }
    s->rxqTail = NULL;
    s->rxBytes = 0;

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
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
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
    // KprintfH("[bsdsocket] %s: fd=%ld\n", __func__, fd);
    if (fd < 0 || fd >= SB_FD_COUNT || base->fd == NULL)
        return NULL;
    return base->fd[fd];
}

void sb_listen_wire(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    tcp_arg(s->pcb.tcp, s);
    tcp_accept(s->pcb.tcp, sb_tcp_accept_cb);
}

void sb_udp_wire(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    udp_recv(s->pcb.udp, sb_udp_recv_cb, s);
}

void sb_raw_wire(struct SbSocket *s)
{
    KprintfH("[bsdsocket] %s: s=0x%08lx\n", __func__, (ULONG)s);
    raw_recv(s->pcb.raw, sb_raw_recv_cb, s);
}
