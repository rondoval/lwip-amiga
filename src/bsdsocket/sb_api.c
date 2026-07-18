/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Socket lifecycle and control: socket/bind/listen/accept/connect,
 * shutdown/CloseSocket, name queries and IoctlSocket. Register annotations
 * follow the NDK sfd exactly; the base arrives in a6 and is always a
 * per-opener child base.
 *
 * Every function: set errno on failure and return -1 (or NULL), take the
 * core lock around lwIP work, block with sb_wait() (which drops the lock).
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#include <debug.h>

#include "netstack.h"

/* -------------------------------------------------------------- helpers --- */

/* parse an app sockaddr (4.4BSD, sin_len first) */
LONG sb_addr_in(const struct sb_sockaddr_in *sa, LONG salen, ip_addr_t *ip, u16_t *port)
{
    KprintfH("[bsdsocket] %s: sa 0x%08lx salen %ld\n", __func__, (ULONG)sa, salen);
    if (sa == NULL || salen < 8)
        return SB_EINVAL;
    if (sa->sin_family != SB_AF_INET && sa->sin_family != 0)
        return SB_EAFNOSUPPORT;

    ip_addr_set_ip4_u32(ip, sa->sin_addr);
    *port = sa->sin_port;
    return 0;
}

/* write an ip/port back into an app sockaddr, value-result */
void sb_addr_out(APTR name, LONG *namelen, ULONG addr, UWORD port)
{
    KprintfH("[bsdsocket] %s: addr 0x%08lx port %lu\n", __func__, addr, (ULONG)port);
    struct sb_sockaddr_in out;

    if (name == NULL || namelen == NULL || *namelen <= 0)
        return;

    for (ULONG i = 0; i < sizeof(out); i++)
        ((UBYTE *)&out)[i] = 0;
    out.sin_len = sizeof(out);
    out.sin_family = SB_AF_INET;
    out.sin_port = port;
    out.sin_addr = addr;

    LONG n = *namelen < (LONG)sizeof(out) ? *namelen : (LONG)sizeof(out);
    CopyMem(&out, name, (ULONG)n);
    *namelen = n;
}

/* --------------------------------------------------------------- socket --- */

LONG bsd_socket(LONG domain asm("d0"), LONG type asm("d1"), LONG protocol asm("d2"),
                struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: domain %ld type %ld protocol %ld\n", __func__, domain, type, protocol);
    if (domain != SB_AF_INET)
        return sb_fail(base, SB_EAFNOSUPPORT);

    SbSockType st;
    switch (type)
    {
    case SB_SOCK_STREAM:
        if (protocol != 0 && protocol != SB_IPPROTO_TCP)
            return sb_fail(base, SB_EPROTONOSUPPORT);
        st = SBT_TCP;
        break;
    case SB_SOCK_DGRAM:
        if (protocol != 0 && protocol != SB_IPPROTO_UDP)
            return sb_fail(base, SB_EPROTONOSUPPORT);
        st = SBT_UDP;
        break;
    case SB_SOCK_RAW:
        st = SBT_RAW;
        break;
    default:
        return sb_fail(base, SB_ESOCKTNOSUPPORT);
    }

    netstack_lock();
    struct SbSocket *s = sb_sock_alloc(base, st);
    if (s != NULL)
    {
        switch (st)
        {
        case SBT_TCP:
            s->pcb.tcp = tcp_new();
            if (s->pcb.tcp != NULL)
                sb_tcp_wire(s);
            break;
        case SBT_UDP:
            s->pcb.udp = udp_new();
            if (s->pcb.udp != NULL)
                sb_udp_wire(s);
            break;
        case SBT_RAW:
            s->pcb.raw = raw_new((u8_t)protocol);
            if (s->pcb.raw != NULL)
                sb_raw_wire(s);
            break;
        }
    }

    if (s == NULL || s->pcb.any == NULL)
    {
        if (s != NULL)
            sb_sock_free(base, s);
        netstack_unlock();
        return sb_fail(base, SB_ENOBUFS);
    }

    LONG fd = sb_fd_alloc(base, s);
    if (fd < 0)
    {
        sb_sock_free(base, s);
        netstack_unlock();
        return sb_fail(base, SB_EMFILE);
    }

    netstack_unlock();
    return fd;
}

/* ----------------------------------------------------------------- bind --- */

LONG bsd_bind(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"),
              struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld name 0x%08lx namelen %ld\n", __func__, sock, (ULONG)name, namelen);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    ip_addr_t ip;
    u16_t port;
    LONG e = sb_addr_in(name, namelen, &ip, &port);
    if (e != 0)
        return sb_fail(base, e);

    netstack_lock();
    err_t r = ERR_VAL;
    switch (s->type)
    {
    case SBT_TCP:
        if (s->pcb.tcp != NULL)
            r = tcp_bind(s->pcb.tcp, &ip, port);
        break;
    case SBT_UDP:
        if (s->pcb.udp != NULL)
            r = udp_bind(s->pcb.udp, &ip, port);
        break;
    case SBT_RAW:
        if (s->pcb.raw != NULL)
            r = raw_bind(s->pcb.raw, &ip);
        break;
    }
    netstack_unlock();

    if (r != ERR_OK)
        return sb_fail(base, sb_map_err(r));
    return 0;
}

/* --------------------------------------------------------------- listen --- */

LONG bsd_listen(LONG sock asm("d0"), LONG backlog asm("d1"),
                struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld backlog %ld\n", __func__, sock, backlog);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (s->type != SBT_TCP)
        return sb_fail(base, SB_EOPNOTSUPP);

    if (backlog < 1)
        backlog = 1;
    if (backlog > SB_ACCEPT_QMAX)
        backlog = SB_ACCEPT_QMAX;

    netstack_lock();
    if (s->pcb.tcp == NULL)
    {
        netstack_unlock();
        return sb_fail(base, s->err != 0 ? s->err : SB_EINVAL);
    }

    if (!s->listening)
    {
        struct tcp_pcb *lpcb = tcp_listen_with_backlog(s->pcb.tcp, (u8_t)backlog);
        if (lpcb == NULL)
        {
            netstack_unlock();
            return sb_fail(base, SB_ENOBUFS);
        }
        s->pcb.tcp = lpcb;
        s->listening = TRUE;
        sb_listen_wire(s);
    }
    netstack_unlock();
    return 0;
}

/* --------------------------------------------------------------- accept --- */

LONG bsd_accept(LONG sock asm("d0"), APTR addr asm("a0"), APTR addrlen asm("a1"),
                struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld\n", __func__, sock);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (s->type != SBT_TCP || !s->listening)
        return sb_fail(base, SB_EINVAL);

    netstack_lock();
    while (s->naccept == 0)
    {
        if (s->err != 0)
        {
            LONG e = s->err;
            netstack_unlock();
            return sb_fail(base, e);
        }
        if (s->nonblock)
        {
            netstack_unlock();
            return sb_fail(base, SB_EWOULDBLOCK);
        }
        LONG e = sb_wait(base);
        if (e != 0)
        {
            netstack_unlock();
            return sb_fail(base, e);
        }
    }

    struct SbSocket *ns = (struct SbSocket *)RemHeadMinList(&s->acceptq);
    s->naccept--;

    LONG fd = sb_fd_alloc(base, ns);
    if (fd < 0)
    {
        sb_sock_free(base, ns);
        netstack_unlock();
        return sb_fail(base, SB_EMFILE);
    }

    ULONG raddr;
    UWORD rport;
    sb_peer_ip(ns, &raddr, &rport);
    netstack_unlock();

    sb_addr_out(addr, addrlen, raddr, rport);
    return fd;
}

/* -------------------------------------------------------------- connect --- */

LONG bsd_connect(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"),
                 struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld name 0x%08lx namelen %ld\n", __func__, sock, (ULONG)name, namelen);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    ip_addr_t ip;
    u16_t port;
    LONG e = sb_addr_in(name, namelen, &ip, &port);
    if (e != 0)
        return sb_fail(base, e);

    netstack_lock();
    err_t r = ERR_VAL;
    switch (s->type)
    {
    case SBT_TCP:
        if (s->connected)
        {
            netstack_unlock();
            return sb_fail(base, SB_EISCONN);
        }
        if (s->connecting)
        {
            netstack_unlock();
            return sb_fail(base, SB_EALREADY);
        }
        if (s->pcb.tcp == NULL)
        {
            LONG pe = s->err != 0 ? s->err : SB_EINVAL;
            netstack_unlock();
            return sb_fail(base, pe);
        }
        s->err = 0;
        s->connecting = TRUE;
        r = tcp_connect(s->pcb.tcp, &ip, port, sb_tcp_connected_cb);
        if (r != ERR_OK)
        {
            s->connecting = FALSE;
            netstack_unlock();
            return sb_fail(base, sb_map_err(r));
        }
        if (s->nonblock)
        {
            netstack_unlock();
            return sb_fail(base, SB_EINPROGRESS);
        }
        while (s->connecting && s->err == 0)
        {
            LONG we = sb_wait(base);
            if (we != 0)
            {
                netstack_unlock();
                return sb_fail(base, we);
            }
        }
        if (!s->connected)
        {
            LONG pe = s->err != 0 ? s->err : SB_ECONNREFUSED;
            s->err = 0;
            netstack_unlock();
            return sb_fail(base, pe);
        }
        netstack_unlock();
        return 0;

    case SBT_UDP:
        if (s->pcb.udp != NULL)
            r = udp_connect(s->pcb.udp, &ip, port);
        break;
    case SBT_RAW:
        if (s->pcb.raw != NULL)
            r = raw_connect(s->pcb.raw, &ip);
        break;
    }
    netstack_unlock();

    if (r != ERR_OK)
        return sb_fail(base, sb_map_err(r));
    return 0;
}

/* ------------------------------------------------------------- shutdown --- */

LONG bsd_shutdown(LONG sock asm("d0"), LONG how asm("d1"),
                  struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld how %ld\n", __func__, sock, how);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (s->type != SBT_TCP)
        return sb_fail(base, SB_EOPNOTSUPP);
    if (how < 0 || how > 2)
        return sb_fail(base, SB_EINVAL);

    BOOL rd = (how == 0 || how == 2);
    BOOL wr = (how == 1 || how == 2);

    netstack_lock();
    if (s->pcb.tcp == NULL)
    {
        netstack_unlock();
        return sb_fail(base, SB_ENOTCONN);
    }
    struct tcp_pcb *pcb = s->pcb.tcp;
    if (rd && wr)
    {
        /* both directions == close semantics: detach before lwIP owns it */
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        s->pcb.tcp = NULL;
    }
    err_t r = tcp_shutdown(pcb, rd, wr);
    if (r == ERR_OK)
    {
        if (rd)
            s->shut_rd = TRUE;
        if (wr)
            s->shut_wr = TRUE;
    }
    netstack_unlock();

    if (r != ERR_OK)
        return sb_fail(base, sb_map_err(r));
    return 0;
}

/* ---------------------------------------------------------- CloseSocket --- */

LONG bsd_CloseSocket(LONG sock asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld\n", __func__, sock);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    netstack_lock();
    /* SO_LINGER with a non-zero timeout, on the last reference of a connected,
     * blocking TCP socket: wait (bounded by lingerTime seconds) for queued
     * data to drain and be acked before teardown. Each ACK wakes the opener
     * via sb_tcp_sent_cb; on timeout forceRst makes sb_sock_free abort (RST)
     * instead of closing gracefully. A non-blocking socket never blocks here —
     * lwIP still drains gracefully in the background. */
    if (s->type == SBT_TCP && s->refs == 1 && s->lingerOn && s->lingerTime > 0 &&
        !s->nonblock && s->connected && base->timerReq != NULL && s->pcb.tcp != NULL)
    {
        struct SbTimedWait tw = { 0, FALSE };
        while (s->pcb.tcp != NULL &&
               (s->pcb.tcp->unsent != NULL || s->pcb.tcp->unacked != NULL))
        {
            LONG we = sb_wait_to(base, (ULONG)s->lingerTime * 1000, &tw);
            if (we == SB_EWOULDBLOCK)
            {
                s->forceRst = TRUE; /* deadline expired: unsent data dies */
                break;
            }
            if (we == SB_EINTR)
                break; /* interrupted: proceed with a graceful close */
        }
    }
    sb_sock_free(base, s);
    netstack_unlock();
    base->fd[sock] = NULL;
    return 0;
}

/* ---------------------------------------------------------------- names --- */

LONG bsd_getsockname(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld\n", __func__, sock);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    ULONG a = 0;
    UWORD p = 0;
    netstack_lock();
    switch (s->type)
    {
    case SBT_TCP:
        if (s->pcb.tcp != NULL)
        {
            a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.tcp->local_ip));
            p = s->pcb.tcp->local_port;
        }
        break;
    case SBT_UDP:
        if (s->pcb.udp != NULL)
        {
            a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.udp->local_ip));
            p = s->pcb.udp->local_port;
        }
        break;
    case SBT_RAW:
        if (s->pcb.raw != NULL)
            a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.raw->local_ip));
        break;
    }
    netstack_unlock();

    sb_addr_out(name, namelen, a, p);
    return 0;
}

LONG bsd_getpeername(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld\n", __func__, sock);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    ULONG a = 0;
    UWORD p = 0;
    BOOL conn = FALSE;
    netstack_lock();
    switch (s->type)
    {
    case SBT_TCP:
        if (s->pcb.tcp != NULL && s->connected)
        {
            sb_peer_ip(s, &a, &p);
            conn = TRUE;
        }
        break;
    case SBT_UDP:
        if (s->pcb.udp != NULL && (s->pcb.udp->flags & UDP_FLAGS_CONNECTED))
        {
            a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.udp->remote_ip));
            p = s->pcb.udp->remote_port;
            conn = TRUE;
        }
        break;
    case SBT_RAW:
        break;
    }
    netstack_unlock();

    if (!conn)
        return sb_fail(base, SB_ENOTCONN);
    sb_addr_out(name, namelen, a, p);
    return 0;
}

/* ---------------------------------------------------------------- ioctl --- */

LONG bsd_IoctlSocket(LONG sock asm("d0"), ULONG req asm("d1"), APTR argp asm("a0"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld req 0x%lx\n", __func__, sock, req);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (argp == NULL)
        return sb_fail(base, SB_EFAULT);

    switch (req)
    {
    case SB_FIONBIO:
        s->nonblock = (*(LONG *)argp != 0);
        return 0;
    case SB_FIONREAD:
    {
        LONG n = 0;
        netstack_lock();
        if (s->type == SBT_TCP)
            n = (LONG)s->rxBytes;
        else if (s->ndgrams != 0)
            n = (LONG)((struct SbDgram *)s->dgrams.mlh_Head)->p->tot_len;
        netstack_unlock();
        *(LONG *)argp = n;
        return 0;
    }
    default:
        return sb_fail(base, SB_EINVAL);
    }
}
