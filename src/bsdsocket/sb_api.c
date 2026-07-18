/* SPDX-License-Identifier: BSD-3-Clause */
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
#include <lwip/priv/tcp_priv.h> /* TCPGUARD in sb_tcp_send peeks at pcb->unsent */

#include <debug.h>

#include "netstack.h"
#include "nsprof.h"

/* -------------------------------------------------------------- helpers --- */

static LONG sb_fail(struct SocketBase *base, LONG code)
{
    KprintfH("[bsdsocket] %s: code %ld\n", __func__, code);
    sb_set_errno(base, code);
    return -1;
}

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

static void sb_addr_out(APTR name, LONG *namelen, ULONG addr, UWORD port)
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

/* ----------------------------------------------------------- send/sendto --- */

static LONG sb_tcp_send(struct SocketBase *base, struct SbSocket *s,
                        const UBYTE *buf, LONG len, LONG flags)
{
    KprintfH("[bsdsocket] %s: len %ld flags 0x%lx\n", __func__, len, (ULONG)flags);
    LONG sent = 0;
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    struct SbTimedWait tw = { 0, FALSE };

    /* no TCP urgent data: lwIP never sets the URG bit, so accepting the flag
     * would silently send the byte inline — refuse instead (callers probe) */
    if (flags & SB_MSG_OOB)
        return sb_fail(base, SB_EOPNOTSUPP);

    PERF_T0(t_lock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_SEND_LOCKWAIT, t_lock);
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
        if (s->connecting)
        {
            /* Hold sends until established. lwIP would happily queue data
             * in SYN_SENT, but the SYN-ACK RESETS snd_wnd_max to the raw
             * handshake window (tcp_in.c) — anything already queued at
             * full MSS then trips tcp_write's "mss_local is too small"
             * assert (observed 2026-07-14: amispeedtest upload vs a
             * small-window SYN-ACK; the assert-continue underflow wedged
             * the stack). Matches sb_sock_writable, which reports
             * not-writable while connecting. */
            if (dontwait)
            {
                netstack_unlock();
                return sb_fail(base, SB_EWOULDBLOCK);
            }
            LONG we = sb_wait_to(base, s->sndTimeoMs, &tw);
            if (we != 0)
            {
                netstack_unlock();
                return sb_fail(base, we);
            }
            continue;
        }

        /* TCPGUARD — guard + diagnostic for lwIP's tcp_write assert
         * "mss_local is too small" (2026-07-14 upload wedge, root cause
         * still open): tcp_write computes mss_local = min(mss,
         * snd_wnd_max/2) and, if the last unsent segment is longer, its
         * u16 space math underflows and shreds the queues. Statically
         * that state looks unreachable, yet it happened mid-flow on a
         * 26 MB upload — so detect it here, print the pcb state we could
         * never observe, and wait it out like a full send buffer (the
         * tail drains via tcp_output/ACKs). Remove once understood. */
        struct tcp_seg *lu = s->pcb.tcp->unsent;
        if (lu == NULL)
        {
            if (s->pcb.tcp->unsent_oversize != 0)
            {
                /* Unambiguous staleness: no unsent segment, yet the pcb
                 * claims tail room — tcp_write would halt on its
                 * "pcb->unsent is NULL" assert. Print and clamp. */
                Kprintf("[bsdsocket] TCPGUARD-OVZ0 s=0x%08lx: ovz=%lu state=%ld pcbfl=0x%lx qlen=%lu\n",
                        (ULONG)s, (ULONG)s->pcb.tcp->unsent_oversize, (LONG)s->pcb.tcp->state,
                        (ULONG)s->pcb.tcp->flags, (ULONG)s->pcb.tcp->snd_queuelen);
                s->pcb.tcp->unsent_oversize = 0;
            }
        }
        else
        {
            /* cached tail from the forked lwIP (pcb->unsent_tail) */
            lu = s->pcb.tcp->unsent_tail;
            u16_t ml = LWIP_MIN(s->pcb.tcp->mss, TCPWND_MIN16(s->pcb.tcp->snd_wnd_max / 2));
            if (ml == 0)
                ml = s->pcb.tcp->mss;
            u32_t luneed = (u32_t)lu->len + LWIP_TCP_OPT_LENGTH(lu->flags);
#if TCP_OVERSIZE_DBGCHECK
            /* lwIP's own desync detector (tcp_out.c "unsent_oversize
             * mismatch"), rendered non-halting: a pcb-vs-segment mismatch
             * is exactly the one-sided bookkeeping bug we're hunting.
             * The shadow must then be zeroed in lockstep with the clamp
             * below, or that assert halts on the very next write. */
            if (s->pcb.tcp->unsent_oversize != lu->oversize_left)
                Kprintf("[bsdsocket] TCPGUARD-DESYNC s=0x%08lx: ovz=%lu luovz=%lu len=%lu segfl=0x%lx "
                        "state=%ld qlen=%lu plen=%lu ptot=%lu pref=%lu\n",
                        (ULONG)s, (ULONG)s->pcb.tcp->unsent_oversize, (ULONG)lu->oversize_left,
                        (ULONG)lu->len, (ULONG)lu->flags, (LONG)s->pcb.tcp->state,
                        (ULONG)s->pcb.tcp->snd_queuelen, (ULONG)(lu->p != NULL ? lu->p->len : 0),
                        (ULONG)(lu->p != NULL ? lu->p->tot_len : 0), (ULONG)(lu->p != NULL ? lu->p->ref : 0));
            lu->oversize_left = 0;
#endif
            if (s->pcb.tcp->unsent_oversize != 0)
            {
                /* INTERIM: disable tcp_write's phase-1 tail reuse. The
                 * 2026-07-14 22:25 run proved unsent_oversize goes
                 * stale-high ("inconsistent oversize vs. space" halted the
                 * task) — phase 1 trusts it and appends into the last
                 * segment's pbuf tail, so a stale value writes PAST the
                 * real allocation into the neighboring heap block: that
                 * overflow is the corruption behind every wild write and
                 * crash this week. Staleness means the field describes the
                 * WRONG segment, so even values that pass lwIP's assert
                 * can overflow — hence clamp ALWAYS (data goes into fresh
                 * pbufs; only small-write coalescing is lost), and print
                 * the evidence whenever the invariant was truly violated
                 * so the bookkeeping bug can still be root-caused. */
                if (luneed > ml || s->pcb.tcp->unsent_oversize > ml - luneed)
                Kprintf("[bsdsocket] TCPGUARD-OVZ s=0x%08lx: ovz=%lu space=%lu ml=%lu mss=%lu swm=%lu swnd=%lu "
                        "len=%lu segfl=0x%lx state=%ld pcbfl=0x%lx qlen=%lu luovz=%lu plen=%lu ptot=%lu pref=%lu\n",
                        (ULONG)s, (ULONG)s->pcb.tcp->unsent_oversize, (ULONG)(ml - luneed), (ULONG)ml,
                        (ULONG)s->pcb.tcp->mss, (ULONG)s->pcb.tcp->snd_wnd_max, (ULONG)s->pcb.tcp->snd_wnd,
                        (ULONG)lu->len, (ULONG)lu->flags, (LONG)s->pcb.tcp->state, (ULONG)s->pcb.tcp->flags,
                        (ULONG)s->pcb.tcp->snd_queuelen, (ULONG)lu->oversize_left,
                        (ULONG)(lu->p != NULL ? lu->p->len : 0), (ULONG)(lu->p != NULL ? lu->p->tot_len : 0),
                        (ULONG)(lu->p != NULL ? lu->p->ref : 0));
                s->pcb.tcp->unsent_oversize = 0;
            }
            (void)luneed;
            if (luneed > ml)
            {
                Kprintf("[bsdsocket] TCPGUARD s=0x%08lx: ml=%lu mss=%lu swm=%lu swnd=%lu len=%lu segfl=0x%lx "
                        "state=%ld pcbfl=0x%lx qlen=%lu sndbuf=%lu ovz=%lu luovz=%lu ptot=%lu\n",
                        (ULONG)s, (ULONG)ml, (ULONG)s->pcb.tcp->mss, (ULONG)s->pcb.tcp->snd_wnd_max,
                        (ULONG)s->pcb.tcp->snd_wnd, (ULONG)lu->len, (ULONG)lu->flags,
                        (LONG)s->pcb.tcp->state, (ULONG)s->pcb.tcp->flags, (ULONG)s->pcb.tcp->snd_queuelen,
                        (ULONG)s->pcb.tcp->snd_buf, (ULONG)s->pcb.tcp->unsent_oversize,
                        (ULONG)lu->oversize_left, (ULONG)(lu->p != NULL ? lu->p->tot_len : 0));
                tcp_output(s->pcb.tcp);
                if (dontwait)
                {
                    netstack_unlock();
                    if (sent > 0)
                        return sent;
                    return sb_fail(base, SB_EWOULDBLOCK);
                }
                LONG gwe = sb_wait_to(base, s->sndTimeoMs, &tw);
                if (gwe != 0)
                {
                    netstack_unlock();
                    if (sent > 0)
                        return sent;
                    return sb_fail(base, gwe);
                }
                continue;
            }
        }

        ULONG avail = tcp_sndbuf(s->pcb.tcp);
        if (avail == 0)
        {
            PERF_T0(t_out);
            tcp_output(s->pcb.tcp);
            PERF_ADD(&ns_perf, NSP_SEND_OUTPUT, t_out);
            if (dontwait)
            {
                netstack_unlock();
                if (sent > 0)
                    return sent;
                return sb_fail(base, SB_EWOULDBLOCK);
            }
            PERF_T0(t_sleep);
            LONG we = sb_wait_to(base, s->sndTimeoMs, &tw);
            PERF_ADD(&ns_perf, NSP_SEND_SLEEP, t_sleep);
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
        /* Cap the per-hold work: tcp_write memcpys the chunk into pbufs
         * and tcp_output pushes it to the driver, all under ns_Core — a
         * 64 KB chunk held the lock 15-18 ms,
         * starving RX injection into ring overruns. 16 KB + the lock
         * break below bounds the hold. */
        if (chunk > 16384)
            chunk = 16384;
        /* Keep split chunks whole-MSS: the TCPGUARD-OVZ clamp disables
         * tcp_write's tail top-up, so a split remainder would ship as a
         * runt segment on the wire, every chunk.
         * The app buffer's own tail stays exact. */
        if ((LONG)chunk < len - sent)
        {
            ULONG mss = s->pcb.tcp->mss;
            if (chunk > mss)
                chunk -= chunk % mss;
        }

        u8_t wf = TCP_WRITE_FLAG_COPY;
        if ((LONG)(sent + (LONG)chunk) < len)
            wf |= TCP_WRITE_FLAG_MORE;

        PERF_T0(t_write);
        err_t r = tcp_write(s->pcb.tcp, buf + sent, (u16_t)chunk, wf);
        PERF_ADD(&ns_perf, NSP_SEND_WRITE, t_write);
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
            LONG we = sb_wait_to(base, s->sndTimeoMs, &tw);
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

        if (sent < len && netstack.ns_Core.ss_QueueCount > 0)
        {
            /* Lock break between chunks — but only when someone is
             * actually queued on ns_Core (the pri-10 unit task blocked
             * on RX injection): push what's queued and hand the FIFO
             * semaphore over. An unconditional break here cost a
             * contended handoff (two context switches). The racy
             * ss_QueueCount read is safe: a waiter arriving right after
             * the check just waits out one more chunk. The loop head
             * re-validates the socket state after the break. */
            PERF_T0(t_out);
            tcp_output(s->pcb.tcp);
            PERF_ADD(&ns_perf, NSP_SEND_OUTPUT, t_out);
            netstack_unlock();
            PERF_T0(t_relock);
            netstack_lock();
            PERF_ADD(&ns_perf, NSP_SEND_LOCKWAIT, t_relock);
        }
    }
    if (s->pcb.tcp != NULL)
    {
        PERF_T0(t_out);
        tcp_output(s->pcb.tcp);
        PERF_ADD(&ns_perf, NSP_SEND_OUTPUT, t_out);
    }
    netstack_unlock();
    return sent;
}

static LONG sb_dgram_send(struct SocketBase *base, struct SbSocket *s,
                          const UBYTE *buf, LONG len,
                          BOOL have_dst, ip_addr_t *dst, u16_t port)
{
    KprintfH("[bsdsocket] %s: len %ld have_dst %ld port %lu\n", __func__, len, (LONG)have_dst, (ULONG)port);
    if (len < 0 || len > 0xFFFF)
        return sb_fail(base, SB_EMSGSIZE);

    PERF_T0(t_lock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_SEND_LOCKWAIT, t_lock);
    PERF_T0(t_send);
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
    PERF_ADD(&ns_perf, NSP_UDP_SEND, t_send);
    netstack_unlock();

    if (r != ERR_OK)
        return sb_fail(base, sb_map_err(r));
    return len;
}

LONG bsd_sendto(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
                LONG flags asm("d2"), APTR to asm("a1"), LONG tolen asm("d3"),
                struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld len %ld flags 0x%lx\n", __func__, sock, len, (ULONG)flags);
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
    KprintfH("[bsdsocket] %s: fd %ld len %ld flags 0x%lx\n", __func__, sock, len, (ULONG)flags);
    return bsd_sendto(sock, buf, len, flags, NULL, 0, base);
}

/* --------------------------------------------------------- recv/recvfrom --- */

static LONG sb_tcp_recv(struct SocketBase *base, struct SbSocket *s,
                        UBYTE *buf, LONG len, LONG flags)
{
    KprintfH("[bsdsocket] %s: len %ld flags 0x%lx\n", __func__, len, (ULONG)flags);
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    BOOL peek = (flags & SB_MSG_PEEK) != 0;
    BOOL waitall = (flags & SB_MSG_WAITALL) != 0;
    LONG copied = 0;
    struct SbTimedWait tw = { 0, FALSE };

    /* no urgent data support (sends refuse MSG_OOB), so there is never OOB
     * to read — EINVAL, per BSD for "no out-of-band data pending" */
    if (flags & SB_MSG_OOB)
        return sb_fail(base, SB_EINVAL);

    PERF_T0(t_lock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_RECV_LOCKWAIT, t_lock);
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
            PERF_T0(t_sleep);
            LONG we = sb_wait_to(base, s->rcvTimeoMs, &tw);
            PERF_ADD(&ns_perf, NSP_RECV_SLEEP, t_sleep);
            if (we != 0)
            {
                netstack_unlock();
                if (copied > 0)
                    return copied;
                return sb_fail(base, we);
            }
        }

        if (peek)
        {
            /* non-destructive; pbuf_copy_partial walks per-pbuf len fields
             * but takes a u16 count — cap the peek */
            ULONG want = (ULONG)(len - copied);
            if (want > s->rxBytes)
                want = s->rxBytes;
            if (want > 0xFFFF)
                want = 0xFFFF;
            pbuf_copy_partial(s->rxq, buf + copied, (u16_t)want, 0);
            copied += (LONG)want;
            break;
        }

        /* Consume head pbufs. The memcpy dominates (up to 64 KB per call)
         * and under ns_Core it stalls RX injection (2026-07-15 lock
         * profile: app copy-outs ≈ 25% of wall time, matching unit-task
         * lock waits) — so detach a run of whole pbufs under the lock,
         * copy with the lock RELEASED, then free and acknowledge the
         * window in one short re-acquired section (ndo_RxRelease and
         * tcp_recved must run under the lock). Only per-pbuf len fields
         * are trusted — the chain-wide tot_len is u16 and the backlog
         * under a 256 KB window overflows it (the old single-counter
         * consume spun forever under the core lock exactly there). */
        ULONG acked = 0;
        while (copied < len && s->rxq != NULL)
        {
            struct pbuf *run = s->rxq;
            struct pbuf *last = NULL;
            ULONG take = 0;
            for (struct pbuf *p = run;
                 p != NULL && take + p->len <= (ULONG)(len - copied) && take + p->len <= 0xFFFF;
                 p = p->next)
            {
                take += p->len;
                last = p;
            }

            if (last == NULL)
            {
                /* the head pbuf alone exceeds the remaining request:
                 * partial consume in place, at most one buffer's worth */
                struct pbuf *h = run;
                ULONG n = (ULONG)(len - copied);
                pbuf_copy_partial(h, buf + copied, (u16_t)n, 0);
                copied += (LONG)n;
                s->rxBytes -= n;
                pbuf_remove_header(h, n);
                acked += n;
                continue; /* copied == len now; the loop exits */
            }

            /* unlink the run — exclusively ours once detached */
            s->rxq = last->next;
            if (s->rxq == NULL)
                s->rxqTail = NULL;
            last->next = NULL;
            s->rxBytes -= take;

            netstack_unlock();
            PERF_T0(t_copy);
            ULONG off = 0;
            for (struct pbuf *p = run; p != NULL; p = p->next)
            {
                pbuf_copy_partial(p, buf + copied + off, p->len, 0);
                off += p->len;
            }
            PERF_ADD(&ns_perf, NSP_RECV_COPY, t_copy);
            PERF_T0(t_relock);
            netstack_lock();
            PERF_ADD(&ns_perf, NSP_RECV_LOCKWAIT, t_relock);
            copied += (LONG)take;

            struct pbuf *p = run;
            while (p != NULL)
            {
                struct pbuf *nx = p->next;
                p->next = NULL;
                pbuf_free(p);
                p = nx;
            }
            acked += take;
        }

        /* One window update for the whole drain pass instead of one per run:
         * shortens the app-task lock hold (which starves RX injection) and
         * coalesces the ACK/tcp_output burst. Flushed here — before the loop
         * either blocks in sb_wait_to or breaks to return — so the receive
         * window never stays closed across a wait. tcp_recved takes a u16, so
         * a >64 KB drain is emitted in <=0xFFFF chunks. */
        if (acked > 0)
        {
            PERF_T0(t_ack);
            while (acked > 0 && s->pcb.tcp != NULL)
            {
                u16_t n = (acked > 0xFFFF) ? 0xFFFF : (u16_t)acked;
                tcp_recved(s->pcb.tcp, n);
                acked -= n;
            }
            PERF_ADD(&ns_perf, NSP_RECV_ACKFLUSH, t_ack);
        }

        if (copied >= len || !waitall)
            break;
    }
    netstack_unlock();
    return copied;
}

/* Block until a datagram is queued on @s. On success returns the head SbDgram
 * with the core lock HELD — the caller consumes it and calls netstack_unlock().
 * On failure returns NULL with the lock released and *err set. */
static struct SbDgram *sb_dgram_wait(struct SocketBase *base, struct SbSocket *s,
                                     LONG flags, LONG *err)
{
    KprintfH("[bsdsocket] %s: flags 0x%lx\n", __func__, (ULONG)flags);
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    struct SbTimedWait tw = { 0, FALSE };

    netstack_lock();
    while (s->ndgrams == 0)
    {
        if (s->err != 0)
        {
            *err = s->err;
            s->err = 0;
            netstack_unlock();
            return NULL;
        }
        if (dontwait)
        {
            netstack_unlock();
            *err = SB_EWOULDBLOCK;
            return NULL;
        }
        LONG we = sb_wait_to(base, s->rcvTimeoMs, &tw);
        if (we != 0)
        {
            netstack_unlock();
            *err = we;
            return NULL;
        }
    }
    return (struct SbDgram *)s->dgrams.mlh_Head;
}

static LONG sb_dgram_recv(struct SocketBase *base, struct SbSocket *s,
                          UBYTE *buf, LONG len, LONG flags,
                          APTR from, LONG *fromlen)
{
    KprintfH("[bsdsocket] %s: len %ld flags 0x%lx\n", __func__, len, (ULONG)flags);
    BOOL peek = (flags & SB_MSG_PEEK) != 0;
    struct SocketBase *root = SB_ROOT(base);
    LONG err = 0;

    struct SbDgram *d = sb_dgram_wait(base, s, flags, &err);
    if (d == NULL)
        return sb_fail(base, err);

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

/* recvmsg for datagram sockets: scatter the head datagram straight from its
 * pbuf chain across the caller's iovs — no bounce buffer, no size cap. Excess
 * beyond the total iov space is dropped and flagged with MSG_TRUNC. */
LONG sb_dgram_recvmsg(struct SocketBase *base, struct SbSocket *s,
                      struct sb_msghdr *mh, LONG flags)
{
    KprintfH("[bsdsocket] %s: flags 0x%lx\n", __func__, (ULONG)flags);
    BOOL peek = (flags & SB_MSG_PEEK) != 0;
    struct SocketBase *root = SB_ROOT(base);
    LONG err = 0;

    struct SbDgram *d = sb_dgram_wait(base, s, flags, &err);
    if (d == NULL)
        return sb_fail(base, err);

    ULONG total = d->p->tot_len; /* datagram length is u16-bounded */
    ULONG addr = d->addr;
    UWORD port = d->port;

    ULONG off = 0;
    for (ULONG i = 0; i < mh->msg_iovlen && off < total; i++)
    {
        struct sb_iovec *iv = &mh->msg_iov[i];
        if (iv->iov_len == 0)
            continue;
        ULONG want = total - off;
        ULONG chunk = iv->iov_len < want ? iv->iov_len : want;
        pbuf_copy_partial(d->p, iv->iov_base, (u16_t)chunk, (u16_t)off);
        off += chunk;
    }
    if (off < total)
        mh->msg_flags |= SB_MSG_TRUNC;

    if (!peek)
    {
        RemoveMinNode(&d->node);
        s->ndgrams--;
        pbuf_free(d->p);
        FreePooled(root->sockPool, d, sizeof(struct SbDgram));
    }
    netstack_unlock();

    /* sender address, value-result: honour the caller's namelen */
    if (mh->msg_name != NULL)
    {
        LONG namelen = (LONG)mh->msg_namelen;
        sb_addr_out(mh->msg_name, &namelen, addr, port);
        mh->msg_namelen = (ULONG)namelen;
    }
    else
    {
        mh->msg_namelen = 0;
    }
    return (LONG)off;
}

LONG bsd_recvfrom(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"),
                  LONG flags asm("d2"), APTR addr asm("a1"), APTR addrlen asm("a2"),
                  struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld len %ld flags 0x%lx\n", __func__, sock, len, (ULONG)flags);
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
    KprintfH("[bsdsocket] %s: fd %ld len %ld flags 0x%lx\n", __func__, sock, len, (ULONG)flags);
    return bsd_recvfrom(sock, buf, len, flags, NULL, NULL, base);
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

/* --------------------------------------------------- names and options --- */

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

LONG bsd_setsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"),
                    APTR optval asm("a0"), LONG optlen asm("d3"),
                    struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: fd %ld level %ld optname %ld\n", __func__, sock, level, optname);
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
            break; /* accepted, fixed internally */
        case SB_SO_LINGER:
        {
            const struct sb_linger *lg = optval;
            if (lg == NULL || optlen < (LONG)sizeof(*lg))
            {
                e = SB_EINVAL;
                break;
            }
            s->lingerOn = lg->l_onoff != 0;
            s->lingerTime = (UWORD)lg->l_linger;
            break;
        }
        case SB_SO_EVENTMASK:
        {
            /* subscription may arrive AFTER the state it cares about
             * (AExplorer arms right after accept, request already queued):
             * synthesize events from the current state so none are lost */
            s->eventMask = (ULONG)val;
            ULONG ev = 0;
            if (s->type == SBT_TCP ? (s->rxq != NULL) : (s->ndgrams != 0))
                ev |= SB_FD_READ;
            if (s->naccept != 0)
                ev |= SB_FD_ACCEPT;
            if (s->rxeof)
                ev |= SB_FD_CLOSE;
            if (s->err != 0)
                ev |= SB_FD_ERROR;
            if (s->type == SBT_TCP && s->connected && s->pcb.tcp != NULL &&
                tcp_sndbuf(s->pcb.tcp) > 0)
                ev |= SB_FD_WRITE;
            if (ev != 0)
                sb_event(s, ev);
            break;
        }
        case SB_SO_SNDTIMEO:
        case SB_SO_RCVTIMEO:
        {
            const struct sb_timeval *tv = optval;
            if (tv == NULL || optlen < (LONG)sizeof(*tv))
            {
                e = SB_EINVAL;
                break;
            }
            ULONG ms = tv->tv_secs * 1000 + tv->tv_micro / 1000;
            if (optname == SB_SO_SNDTIMEO)
                s->sndTimeoMs = ms;
            else
                s->rcvTimeoMs = ms;
            break;
        }
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
    KprintfH("[bsdsocket] %s: fd %ld level %ld optname %ld\n", __func__, sock, level, optname);
    struct SbSocket *s = sb_fd_get(base, sock);
    if (s == NULL)
        return sb_fail(base, SB_EBADF);
    if (optval == NULL || optlen == NULL || *(LONG *)optlen < (LONG)sizeof(LONG))
        return sb_fail(base, SB_EINVAL);

    if (level == SB_IPPROTO_TCP)
    {
        if (optname != SB_TCP_NODELAY || s->type != SBT_TCP)
            return sb_fail(base, SB_ENOPROTOOPT);
        netstack_lock();
        LONG nd = (s->pcb.tcp != NULL && tcp_nagle_disabled(s->pcb.tcp)) ? 1 : 0;
        netstack_unlock();
        *(LONG *)optval = nd;
        *(LONG *)optlen = sizeof(LONG);
        return 0;
    }
    if (level != SB_SOL_SOCKET)
        return sb_fail(base, SB_ENOPROTOOPT);

    LONG val;
    switch (optname)
    {
    case SB_SO_EVENTMASK:
        val = (LONG)s->eventMask;
        break;
    case SB_SO_SNDTIMEO:
    case SB_SO_RCVTIMEO:
    {
        if (*(LONG *)optlen < (LONG)sizeof(struct sb_timeval))
            return sb_fail(base, SB_EINVAL);
        ULONG ms = optname == SB_SO_SNDTIMEO ? s->sndTimeoMs : s->rcvTimeoMs;
        struct sb_timeval *tv = optval;
        tv->tv_secs = ms / 1000;
        tv->tv_micro = (ms % 1000) * 1000;
        *(LONG *)optlen = sizeof(*tv);
        return 0;
    }
    case SB_SO_LINGER:
    {
        if (*(LONG *)optlen < (LONG)sizeof(struct sb_linger))
            return sb_fail(base, SB_EINVAL);
        struct sb_linger *lg = optval;
        lg->l_onoff = s->lingerOn;
        lg->l_linger = s->lingerTime;
        *(LONG *)optlen = sizeof(*lg);
        return 0;
    }
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
    case SB_SO_REUSEADDR:
    case SB_SO_KEEPALIVE:
    case SB_SO_BROADCAST:
    {
        u8_t optbit = optname == SB_SO_REUSEADDR  ? SOF_REUSEADDR
                      : optname == SB_SO_KEEPALIVE ? SOF_KEEPALIVE
                                                   : SOF_BROADCAST;
        netstack_lock();
        val = (s->pcb.any != NULL &&
               ip_get_option((struct ip_pcb *)s->pcb.any, optbit)) ? 1 : 0;
        netstack_unlock();
        break;
    }
    case SB_SO_SNDBUF:
        /* fixed internally; report the real capacity (setsockopt is a no-op) */
        val = s->type == SBT_TCP ? TCP_SND_BUF : 0xFFFF;
        break;
    case SB_SO_RCVBUF:
        val = s->type == SBT_TCP ? TCP_WND : 0xFFFF;
        break;
    default:
        return sb_fail(base, SB_ENOPROTOOPT);
    }

    *(LONG *)optval = val;
    *(LONG *)optlen = sizeof(LONG);
    return 0;
}

/* GetSocketEvents [AmiTCP V4]: next socket with pending FD_* events, or
 * -1. Scan order is fd order (the autodoc promises no ordering); a
 * listener with more connections still queued keeps FD_ACCEPT pending so
 * every pending connection produces an event, per the autodoc. */
LONG bsd_GetSocketEvents(ULONG *eventsp asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    if (eventsp == NULL)
        return -1;

    netstack_lock();
    for (ULONG fd = 0; fd < base->fdCount; fd++)
    {
        struct SbSocket *s = base->fd[fd];
        if (s == NULL || s->eventsPending == 0)
            continue;
        *eventsp = s->eventsPending;
        s->eventsPending = 0;
        if (s->naccept > 1 && (s->eventMask & SB_FD_ACCEPT))
            s->eventsPending = SB_FD_ACCEPT;
        netstack_unlock();
        return (LONG)fd;
    }
    netstack_unlock();
    return -1;
}

LONG bsd_getdtablesize(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    return (LONG)base->fdCount;
}

VOID bsd_SetSocketSignals(ULONG intMask asm("d0"), ULONG ioMask asm("d1"),
                          ULONG urgMask asm("d2"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: intMask 0x%08lx ioMask 0x%08lx urgMask 0x%08lx\n", __func__, intMask, ioMask, urgMask);
    base->breakMask = intMask != 0 ? intMask : SIGBREAKF_CTRL_C;
    base->sigIoMask = ioMask;
    base->sigUrgMask = urgMask;
    /* see SBTC_SIGIOMASK: readiness may predate the mask */
    if (ioMask != 0 && base->task != NULL)
        Signal(base->task, ioMask);
}
