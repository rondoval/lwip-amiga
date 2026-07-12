/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The core socket API. Register annotations follow the NDK sfd exactly;
 * the base arrives in a6 and is always a per-opener child base.
 *
 * Every function: set errno on failure and return -1 (or NULL), take the
 * core lock around lwIP work, block with sb_wait() (which drops the lock).
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#include "netstack.h"

/* -------------------------------------------------------------- helpers --- */

static LONG sb_fail(struct SocketBase *base, LONG code)
{
    sb_set_errno(base, code);
    return -1;
}

/* parse an app sockaddr (4.4BSD, sin_len first) */
static LONG sb_addr_in(const struct sb_sockaddr_in *sa, LONG salen, ip_addr_t *ip, u16_t *port)
{
    if (sa == NULL || salen < 8)
        return SB_EINVAL;
    if (sa->sin_family != SB_AF_INET && sa->sin_family != 0)
        return SB_EAFNOSUPPORT;

    ip_addr_set_ip4_u32(ip, sa->sin_addr);
    *port = sa->sin_port;
    return 0;
}

static void sb_addr_out(APTR name, LONG *namelen, ULONG addr, UWORD port)
{
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

    ULONG raddr = 0;
    UWORD rport = 0;
    if (ns->pcb.tcp != NULL)
    {
        raddr = ip4_addr_get_u32(ip_2_ip4(&ns->pcb.tcp->remote_ip));
        rport = ns->pcb.tcp->remote_port;
    }
    netstack_unlock();

    sb_addr_out(addr, addrlen, raddr, rport);
    return fd;
}

/* -------------------------------------------------------------- connect --- */

LONG bsd_connect(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"),
                 struct SocketBase *base asm("a6"))
{
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

/* ----------------------------------------------------------- send/sendto --- */

static LONG sb_tcp_send(struct SocketBase *base, struct SbSocket *s,
                        const UBYTE *buf, LONG len, LONG flags)
{
    LONG sent = 0;
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);

    netstack_lock();
    while (sent < len)
    {
        if (s->pcb.tcp == NULL || s->err != 0)
        {
            LONG e = s->err != 0 ? s->err : SB_EPIPE;
            netstack_unlock();
            if (sent > 0)
                return sent;
            return sb_fail(base, e);
        }
        if (s->shut_wr)
        {
            netstack_unlock();
            return sb_fail(base, SB_ESHUTDOWN);
        }

        ULONG avail = tcp_sndbuf(s->pcb.tcp);
        if (avail == 0)
        {
            tcp_output(s->pcb.tcp);
            if (dontwait)
            {
                netstack_unlock();
                if (sent > 0)
                    return sent;
                return sb_fail(base, SB_EWOULDBLOCK);
            }
            LONG we = sb_wait(base);
            if (we != 0)
            {
                netstack_unlock();
                if (sent > 0)
                    return sent;
                return sb_fail(base, we);
            }
            continue;
        }

        ULONG chunk = (ULONG)(len - sent);
        if (chunk > avail)
            chunk = avail;
        if (chunk > 0xFFFF)
            chunk = 0xFFFF;

        u8_t wf = TCP_WRITE_FLAG_COPY;
        if ((LONG)(sent + (LONG)chunk) < len)
            wf |= TCP_WRITE_FLAG_MORE;

        err_t r = tcp_write(s->pcb.tcp, buf + sent, (u16_t)chunk, wf);
        if (r == ERR_MEM)
        {
            tcp_output(s->pcb.tcp);
            if (dontwait)
            {
                netstack_unlock();
                if (sent > 0)
                    return sent;
                return sb_fail(base, SB_EWOULDBLOCK);
            }
            LONG we = sb_wait(base);
            if (we != 0)
            {
                netstack_unlock();
                if (sent > 0)
                    return sent;
                return sb_fail(base, we);
            }
            continue;
        }
        if (r != ERR_OK)
        {
            netstack_unlock();
            if (sent > 0)
                return sent;
            return sb_fail(base, sb_map_err(r));
        }
        sent += (LONG)chunk;
    }
    if (s->pcb.tcp != NULL)
        tcp_output(s->pcb.tcp);
    netstack_unlock();
    return sent;
}

static LONG sb_dgram_send(struct SocketBase *base, struct SbSocket *s,
                          const UBYTE *buf, LONG len,
                          BOOL have_dst, ip_addr_t *dst, u16_t port)
{
    if (len < 0 || len > 0xFFFF)
        return sb_fail(base, SB_EMSGSIZE);

    netstack_lock();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (p == NULL)
    {
        netstack_unlock();
        return sb_fail(base, SB_ENOBUFS);
    }
    pbuf_take(p, buf, (u16_t)len);

    err_t r = ERR_VAL;
    if (s->type == SBT_UDP && s->pcb.udp != NULL)
        r = have_dst ? udp_sendto(s->pcb.udp, p, dst, port)
                     : udp_send(s->pcb.udp, p);
    else if (s->type == SBT_RAW && s->pcb.raw != NULL)
        r = have_dst ? raw_sendto(s->pcb.raw, p, dst)
                     : raw_send(s->pcb.raw, p);

    pbuf_free(p);
    netstack_unlock();

    if (r != ERR_OK)
        return sb_fail(base, sb_map_err(r));
    return len;
}

LONG bsd_sendto(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
                LONG flags asm("d2"), APTR to asm("a1"), LONG tolen asm("d3"),
                struct SocketBase *base asm("a6"))
{
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (buf == NULL || len < 0)
        return sb_fail(base, SB_EINVAL);

    if (s->type == SBT_TCP)
        return sb_tcp_send(base, s, buf, len, flags);

    if (to != NULL)
    {
        ip_addr_t ip;
        u16_t port;
        LONG e = sb_addr_in(to, tolen, &ip, &port);
        if (e != 0)
            return sb_fail(base, e);
        return sb_dgram_send(base, s, buf, len, TRUE, &ip, port);
    }
    return sb_dgram_send(base, s, buf, len, FALSE, NULL, 0);
}

LONG bsd_send(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
              LONG flags asm("d2"), struct SocketBase *base asm("a6"))
{
    return bsd_sendto(sock, buf, len, flags, NULL, 0, base);
}

/* --------------------------------------------------------- recv/recvfrom --- */

static LONG sb_tcp_recv(struct SocketBase *base, struct SbSocket *s,
                        UBYTE *buf, LONG len, LONG flags)
{
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    BOOL peek = (flags & SB_MSG_PEEK) != 0;
    BOOL waitall = (flags & SB_MSG_WAITALL) != 0;
    LONG copied = 0;

    netstack_lock();
    for (;;)
    {
        while (s->rxq == NULL)
        {
            if (s->rxeof || s->shut_rd)
            {
                netstack_unlock();
                return copied;
            }
            if (s->err != 0)
            {
                LONG e = s->err;
                netstack_unlock();
                if (copied > 0)
                    return copied;
                s->err = 0;
                return sb_fail(base, e);
            }
            if (s->pcb.tcp == NULL)
            {
                netstack_unlock();
                return copied; /* orderly gone */
            }
            if (!s->connected && !s->connecting)
            {
                netstack_unlock();
                if (copied > 0)
                    return copied;
                return sb_fail(base, SB_ENOTCONN);
            }
            if (copied > 0 && !waitall)
            {
                netstack_unlock();
                return copied;
            }
            if (dontwait)
            {
                netstack_unlock();
                if (copied > 0)
                    return copied;
                return sb_fail(base, SB_EWOULDBLOCK);
            }
            LONG we = sb_wait(base);
            if (we != 0)
            {
                netstack_unlock();
                if (copied > 0)
                    return copied;
                return sb_fail(base, we);
            }
        }

        ULONG want = (ULONG)(len - copied);
        ULONG have = s->rxq->tot_len;
        ULONG n = want < have ? want : have;

        pbuf_copy_partial(s->rxq, buf + copied, (u16_t)n, 0);
        copied += (LONG)n;

        if (!peek)
        {
            s->rxq = pbuf_free_header(s->rxq, (u16_t)n);
            if (s->pcb.tcp != NULL)
                tcp_recved(s->pcb.tcp, (u16_t)n);
        }

        if (peek || copied >= len || (!waitall && copied > 0))
            break;
    }
    netstack_unlock();
    return copied;
}

static LONG sb_dgram_recv(struct SocketBase *base, struct SbSocket *s,
                          UBYTE *buf, LONG len, LONG flags,
                          APTR from, LONG *fromlen)
{
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    BOOL peek = (flags & SB_MSG_PEEK) != 0;
    struct SocketBase *root = SB_ROOT(base);

    netstack_lock();
    while (s->ndgrams == 0)
    {
        if (s->err != 0)
        {
            LONG e = s->err;
            s->err = 0;
            netstack_unlock();
            return sb_fail(base, e);
        }
        if (dontwait)
        {
            netstack_unlock();
            return sb_fail(base, SB_EWOULDBLOCK);
        }
        LONG we = sb_wait(base);
        if (we != 0)
        {
            netstack_unlock();
            return sb_fail(base, we);
        }
    }

    struct SbDgram *d = (struct SbDgram *)s->dgrams.mlh_Head;
    ULONG n = d->p->tot_len;
    if (n > (ULONG)len)
        n = (ULONG)len; /* excess is discarded, BSD-style */
    pbuf_copy_partial(d->p, buf, (u16_t)n, 0);

    ULONG addr = d->addr;
    UWORD port = d->port;

    if (!peek)
    {
        RemoveMinNode(&d->node);
        s->ndgrams--;
        pbuf_free(d->p);
        FreePooled(root->sockPool, d, sizeof(struct SbDgram));
    }
    netstack_unlock();

    sb_addr_out(from, fromlen, addr, port);
    return (LONG)n;
}

LONG bsd_recvfrom(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
                  LONG flags asm("d2"), APTR addr asm("a1"), APTR addrlen asm("a2"),
                  struct SocketBase *base asm("a6"))
{
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (buf == NULL || len < 0)
        return sb_fail(base, SB_EINVAL);

    if (s->type == SBT_TCP)
    {
        LONG r = sb_tcp_recv(base, s, buf, len, flags);
        if (r >= 0 && addr != NULL && addrlen != NULL)
        {
            netstack_lock();
            ULONG a = 0;
            UWORD p = 0;
            if (s->pcb.tcp != NULL)
            {
                a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.tcp->remote_ip));
                p = s->pcb.tcp->remote_port;
            }
            netstack_unlock();
            sb_addr_out(addr, addrlen, a, p);
        }
        return r;
    }
    return sb_dgram_recv(base, s, buf, len, flags, addr, addrlen);
}

LONG bsd_recv(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
              LONG flags asm("d2"), struct SocketBase *base asm("a6"))
{
    return bsd_recvfrom(sock, buf, len, flags, NULL, NULL, base);
}

/* ------------------------------------------------------------- shutdown --- */

LONG bsd_shutdown(LONG sock asm("d0"), LONG how asm("d1"),
                  struct SocketBase *base asm("a6"))
{
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
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    netstack_lock();
    sb_sock_free(base, s);
    netstack_unlock();
    base->fd[sock] = NULL;
    return 0;
}

/* --------------------------------------------------- names and options --- */

LONG bsd_getsockname(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"),
                     struct SocketBase *base asm("a6"))
{
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
            a = ip4_addr_get_u32(ip_2_ip4(&s->pcb.tcp->remote_ip));
            p = s->pcb.tcp->remote_port;
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

LONG bsd_IoctlSocket(LONG sock asm("d0"), ULONG req asm("d1"), APTR argp asm("a0"),
                     struct SocketBase *base asm("a6"))
{
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
            n = s->rxq != NULL ? (LONG)s->rxq->tot_len : 0;
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

LONG bsd_setsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"),
                    APTR optval asm("a0"), LONG optlen asm("d3"),
                    struct SocketBase *base asm("a6"))
{
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);

    LONG val = 0;
    if (optval != NULL && optlen >= (LONG)sizeof(LONG))
        val = *(LONG *)optval;

    netstack_lock();
    LONG e = 0;
    if (level == SB_SOL_SOCKET)
    {
        switch (optname)
        {
        case SB_SO_REUSEADDR:
            if (s->pcb.any != NULL)
            {
                if (val)
                    ip_set_option((struct ip_pcb *)s->pcb.any, SOF_REUSEADDR);
                else
                    ip_reset_option((struct ip_pcb *)s->pcb.any, SOF_REUSEADDR);
            }
            break;
        case SB_SO_KEEPALIVE:
            if (s->pcb.any != NULL)
            {
                if (val)
                    ip_set_option((struct ip_pcb *)s->pcb.any, SOF_KEEPALIVE);
                else
                    ip_reset_option((struct ip_pcb *)s->pcb.any, SOF_KEEPALIVE);
            }
            break;
        case SB_SO_BROADCAST:
            if (s->pcb.any != NULL)
            {
                if (val)
                    ip_set_option((struct ip_pcb *)s->pcb.any, SOF_BROADCAST);
                else
                    ip_reset_option((struct ip_pcb *)s->pcb.any, SOF_BROADCAST);
            }
            break;
        case SB_SO_SNDBUF:
        case SB_SO_RCVBUF:
        case SB_SO_LINGER:
            break; /* accepted, fixed internally */
        default:
            e = SB_ENOPROTOOPT;
            break;
        }
    }
    else if (level == SB_IPPROTO_TCP && s->type == SBT_TCP && s->pcb.tcp != NULL)
    {
        if (optname == SB_TCP_NODELAY)
        {
            if (val)
                tcp_nagle_disable(s->pcb.tcp);
            else
                tcp_nagle_enable(s->pcb.tcp);
        }
        else
        {
            e = SB_ENOPROTOOPT;
        }
    }
    else
    {
        e = SB_ENOPROTOOPT;
    }
    netstack_unlock();

    if (e != 0)
        return sb_fail(base, e);
    return 0;
}

LONG bsd_getsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"),
                    APTR optval asm("a0"), APTR optlen asm("a1"),
                    struct SocketBase *base asm("a6"))
{
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (optval == NULL || optlen == NULL || *(LONG *)optlen < (LONG)sizeof(LONG))
        return sb_fail(base, SB_EINVAL);

    LONG val;
    switch (optname)
    {
    case SB_SO_ERROR:
        netstack_lock();
        val = s->err;
        s->err = 0;
        netstack_unlock();
        break;
    case SB_SO_TYPE:
        val = s->type == SBT_TCP ? SB_SOCK_STREAM
              : s->type == SBT_UDP ? SB_SOCK_DGRAM
                                   : SB_SOCK_RAW;
        break;
    default:
        if (level != SB_SOL_SOCKET)
            return sb_fail(base, SB_ENOPROTOOPT);
        return sb_fail(base, SB_ENOPROTOOPT);
    }

    *(LONG *)optval = val;
    *(LONG *)optlen = sizeof(LONG);
    return 0;
}

LONG bsd_getdtablesize(struct SocketBase *base asm("a6"))
{
    (void)base;
    return SB_FD_COUNT;
}

VOID bsd_SetSocketSignals(ULONG intMask asm("d0"), ULONG ioMask asm("d1"),
                          ULONG urgMask asm("d2"), struct SocketBase *base asm("a6"))
{
    base->breakMask = intMask != 0 ? intMask : SIGBREAKF_CTRL_C;
    base->sigIoMask = ioMask;
    base->sigUrgMask = urgMask;
}
