/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The data path: TCP send/recv, datagram send/recv, and their sendto/
 * recvfrom/sendmsg/recvmsg LVO fronts. Registers per the NDK sfd; every
 * path takes the core lock around lwIP work and blocks via sb_wait_to
 * (which drops the lock).
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/priv/tcp_priv.h> /* sb_tcp_unsent_guard peeks at pcb->unsent */

#include <debug.h>

#include "netstack.h"
#include "nsprof.h"

/* ----------------------------------------------------------------- send --- */

/* The block-or-bail tail shared by every "cannot write more right now" case
 * in sb_tcp_send: push queued data, then either bail out (nonblocking) or
 * sleep until progress/timeout. Returns TRUE to retry the send loop (lock
 * held); FALSE with *ret set to sb_tcp_send's final result (lock released). */
static BOOL sb_send_block(struct SocketBase *base, struct SbSocket *s,
                          LONG sent, BOOL dontwait, struct SbTimedWait *tw,
                          BOOL output, LONG *ret)
{
    if (output)
    {
        PERF_T0(t_out);
        tcp_output(s->pcb.tcp);
        PERF_ADD(&ns_perf, NSP_SEND_OUTPUT, t_out);
    }
    if (dontwait)
    {
        netstack_unlock();
        *ret = sent > 0 ? sent : sb_fail(base, SB_EWOULDBLOCK);
        return FALSE;
    }
    PERF_T0(t_sleep);
    LONG we = sb_wait_to(base, s->sndTimeoMs, tw);
    PERF_ADD(&ns_perf, NSP_SEND_SLEEP, t_sleep);
    if (we != 0)
    {
        netstack_unlock();
        *ret = sent > 0 ? sent : sb_fail(base, we);
        return FALSE;
    }
    return TRUE;
}

/* Guard against lwIP's tcp_write tail-reuse overflow. The fork caches the
 * last unsent segment (pcb->unsent_tail) and tracks its spare pbuf tail room
 * in pcb->unsent_oversize; when that bookkeeping goes stale, tcp_write's
 * phase-1 "oversize" path appends into tail room the segment does not
 * actually have — a heap overflow into the neighboring block. Zeroing the
 * field before every write makes tcp_write take the fresh-pbuf path
 * unconditionally; the only cost is small-write coalescing. The Kprintf
 * evidence-printers (DEBUG builds only) record the pcb state whenever the
 * invariant was genuinely violated, feeding the upstream unsent-tail fix
 * (docs/TODO.md).
 *
 * Returns TRUE when the tail segment is too long for tcp_write's mss_local
 * (its u16 space math would underflow): the caller must flush and wait for
 * the tail to drain, exactly like a full send buffer. Core lock held. */
static BOOL sb_tcp_unsent_guard(struct SbSocket *s)
{
    struct tcp_pcb *pcb = s->pcb.tcp;

    if (pcb->unsent == NULL)
    {
        if (pcb->unsent_oversize != 0)
        {
            /* no unsent segment, yet the pcb claims tail room — tcp_write
             * would halt on its "pcb->unsent is NULL" assert */
            Kprintf("[bsdsocket] TCPGUARD-OVZ0 s=0x%08lx: ovz=%lu state=%ld pcbfl=0x%lx qlen=%lu\n",
                    (ULONG)s, (ULONG)pcb->unsent_oversize, (LONG)pcb->state,
                    (ULONG)pcb->flags, (ULONG)pcb->snd_queuelen);
            pcb->unsent_oversize = 0;
        }
        return FALSE;
    }

    struct tcp_seg *lu = pcb->unsent_tail; /* the fork's cached tail */
    if (lu == NULL)
    {
        /* a head exists but the tail cache is gone — the very desync being
         * guarded against; clamp and write fresh pbufs (no tail to reuse) */
        Kprintf("[bsdsocket] TCPGUARD-NULLTAIL s=0x%08lx: ovz=%lu state=%ld qlen=%lu\n",
                (ULONG)s, (ULONG)pcb->unsent_oversize, (LONG)pcb->state,
                (ULONG)pcb->snd_queuelen);
        pcb->unsent_oversize = 0;
        return FALSE;
    }

    u16_t ml = LWIP_MIN(pcb->mss, TCPWND_MIN16(pcb->snd_wnd_max / 2));
    if (ml == 0)
        ml = pcb->mss;
    u32_t luneed = (u32_t)lu->len + LWIP_TCP_OPT_LENGTH(lu->flags);
#if TCP_OVERSIZE_DBGCHECK
    /* lwIP's own desync detector (tcp_out.c "unsent_oversize mismatch"),
     * rendered non-halting. The shadow must be zeroed in lockstep with the
     * clamp below, or that assert halts on the very next write. */
    if (pcb->unsent_oversize != lu->oversize_left)
        Kprintf("[bsdsocket] TCPGUARD-DESYNC s=0x%08lx: ovz=%lu luovz=%lu len=%lu segfl=0x%lx "
                "state=%ld qlen=%lu plen=%lu ptot=%lu pref=%lu\n",
                (ULONG)s, (ULONG)pcb->unsent_oversize, (ULONG)lu->oversize_left,
                (ULONG)lu->len, (ULONG)lu->flags, (LONG)pcb->state,
                (ULONG)pcb->snd_queuelen, (ULONG)(lu->p != NULL ? lu->p->len : 0),
                (ULONG)(lu->p != NULL ? lu->p->tot_len : 0), (ULONG)(lu->p != NULL ? lu->p->ref : 0));
    lu->oversize_left = 0;
#endif
    if (pcb->unsent_oversize != 0)
    {
        if (luneed > ml || pcb->unsent_oversize > ml - luneed)
            Kprintf("[bsdsocket] TCPGUARD-OVZ s=0x%08lx: ovz=%lu space=%lu ml=%lu mss=%lu swm=%lu swnd=%lu "
                    "len=%lu segfl=0x%lx state=%ld pcbfl=0x%lx qlen=%lu luovz=%lu plen=%lu ptot=%lu pref=%lu\n",
                    (ULONG)s, (ULONG)pcb->unsent_oversize, (ULONG)(ml - luneed), (ULONG)ml,
                    (ULONG)pcb->mss, (ULONG)pcb->snd_wnd_max, (ULONG)pcb->snd_wnd,
                    (ULONG)lu->len, (ULONG)lu->flags, (LONG)pcb->state, (ULONG)pcb->flags,
                    (ULONG)pcb->snd_queuelen, (ULONG)lu->oversize_left,
                    (ULONG)(lu->p != NULL ? lu->p->len : 0), (ULONG)(lu->p != NULL ? lu->p->tot_len : 0),
                    (ULONG)(lu->p != NULL ? lu->p->ref : 0));
        pcb->unsent_oversize = 0;
    }
    if (luneed > ml)
    {
        Kprintf("[bsdsocket] TCPGUARD s=0x%08lx: ml=%lu mss=%lu swm=%lu swnd=%lu len=%lu segfl=0x%lx "
                "state=%ld pcbfl=0x%lx qlen=%lu sndbuf=%lu ovz=%lu luovz=%lu ptot=%lu\n",
                (ULONG)s, (ULONG)ml, (ULONG)pcb->mss, (ULONG)pcb->snd_wnd_max,
                (ULONG)pcb->snd_wnd, (ULONG)lu->len, (ULONG)lu->flags,
                (LONG)pcb->state, (ULONG)pcb->flags, (ULONG)pcb->snd_queuelen,
                (ULONG)pcb->snd_buf, (ULONG)pcb->unsent_oversize,
                (ULONG)lu->oversize_left, (ULONG)(lu->p != NULL ? lu->p->tot_len : 0));
        return TRUE;
    }
    return FALSE;
}

static LONG sb_tcp_send(struct SocketBase *base, struct SbSocket *s,
                        const UBYTE *buf, LONG len, LONG flags)
{
    KprintfH("[bsdsocket] %s: len %ld flags 0x%lx\n", __func__, len, (ULONG)flags);
    LONG sent = 0;
    BOOL dontwait = s->nonblock || (flags & SB_MSG_DONTWAIT);
    struct SbTimedWait tw = { 0, FALSE };
    LONG ret;

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
            /* Hold sends until the connection is established. lwIP would
             * happily queue data in SYN_SENT, but the SYN-ACK RESETS
             * snd_wnd_max to the raw handshake window (tcp_in.c), and
             * anything already queued at full MSS then trips tcp_write's
             * "mss_local is too small" underflow. Mirrors sb_sock_writable,
             * which reports not-writable while connecting. */
            if (!sb_send_block(base, s, sent, dontwait, &tw, FALSE, &ret))
                return ret;
            continue;
        }

        /* oversized unsent tail: wait it out like a full send buffer (the
         * tail drains via tcp_output/ACKs) */
        if (sb_tcp_unsent_guard(s))
        {
            if (!sb_send_block(base, s, sent, dontwait, &tw, TRUE, &ret))
                return ret;
            continue;
        }

        ULONG avail = tcp_sndbuf(s->pcb.tcp);
        if (avail == 0)
        {
            if (!sb_send_block(base, s, sent, dontwait, &tw, TRUE, &ret))
                return ret;
            continue;
        }

        ULONG chunk = (ULONG)(len - sent);
        if (chunk > avail)
            chunk = avail;
        /* Cap the per-hold work: tcp_write memcpys the chunk into pbufs
         * and tcp_output pushes it to the driver, all under ns_Core — an
         * uncapped 64 KB chunk holds the lock long enough to starve RX
         * injection into ring overruns. 16 KB + the lock break below
         * bounds the hold. */
        if (chunk > 16384)
            chunk = 16384;
        /* Keep split chunks whole-MSS: sb_tcp_unsent_guard disables
         * tcp_write's tail top-up, so a split remainder would ship as a
         * runt segment on the wire, every chunk. The app buffer's own
         * tail stays exact. */
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
            if (!sb_send_block(base, s, sent, dontwait, &tw, TRUE, &ret))
                return ret;
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
             * semaphore over. The racy ss_QueueCount read is safe: a
             * waiter arriving right after the check just waits out one
             * more chunk. The loop head re-validates the socket state
             * after the break. */
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

LONG bsd_sendmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"),
                 struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: sock=%ld flags=%ld\n", __func__, sock, flags);
    const struct sb_msghdr *mh = msg;
    struct SbSocket *s = sb_fd_get(base, sock);

    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }
    if (mh == NULL || (mh->msg_iovlen != 0 && mh->msg_iov == NULL))
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    /* ancillary data (SCM_RIGHTS etc.) is not supported; like 4.4BSD's
     * datagram output paths we free/ignore it rather than fail the send */
    if (mh->msg_control != NULL && mh->msg_controllen != 0)
        KprintfH("[bsdsocket] %s: msg_control ignored (%lu bytes)\n", __func__,
                 mh->msg_controllen);

    if (s->type == SBT_TCP)
    {
        /* stream: iovs are just consecutive sends */
        LONG total = 0;
        for (ULONG i = 0; i < mh->msg_iovlen; i++)
        {
            const struct sb_iovec *iv = &mh->msg_iov[i];
            if (iv->iov_len == 0)
                continue;
            LONG n = bsd_send(sock, iv->iov_base, (LONG)iv->iov_len, flags, base);
            if (n < 0)
                return total > 0 ? total : -1;
            total += n;
            if ((ULONG)n < iv->iov_len)
                break;
        }
        return total;
    }

    /* datagram: one message from all iovs */
    ULONG total = 0;
    for (ULONG i = 0; i < mh->msg_iovlen; i++)
        total += mh->msg_iov[i].iov_len;
    if (total > 0xFFFF)
    {
        sb_set_errno(base, SB_EMSGSIZE);
        return -1;
    }

    netstack_lock();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)total, PBUF_RAM);
    if (p == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_ENOBUFS);
        return -1;
    }
    ULONG off = 0;
    for (ULONG i = 0; i < mh->msg_iovlen; i++)
    {
        const struct sb_iovec *iv = &mh->msg_iov[i];
        if (iv->iov_len != 0)
        {
            pbuf_take_at(p, iv->iov_base, (u16_t)iv->iov_len, (u16_t)off);
            off += iv->iov_len;
        }
    }

    err_t r = ERR_VAL;
    if (mh->msg_name != NULL)
    {
        ip_addr_t ip;
        u16_t port;
        LONG e = sb_addr_in(mh->msg_name, (LONG)mh->msg_namelen, &ip, &port);
        if (e != 0)
        {
            pbuf_free(p);
            netstack_unlock();
            sb_set_errno(base, e);
            return -1;
        }
        if (s->type == SBT_UDP && s->pcb.udp != NULL)
            r = udp_sendto(s->pcb.udp, p, &ip, port);
        else if (s->type == SBT_RAW && s->pcb.raw != NULL)
            r = raw_sendto(s->pcb.raw, p, &ip);
    }
    else
    {
        /* no destination: valid only on a connected datagram socket */
        if (s->type == SBT_UDP && s->pcb.udp != NULL)
        {
            if (!(s->pcb.udp->flags & UDP_FLAGS_CONNECTED))
            {
                pbuf_free(p);
                netstack_unlock();
                sb_set_errno(base, SB_EDESTADDRREQ);
                return -1;
            }
            r = udp_send(s->pcb.udp, p);
        }
        else if (s->type == SBT_RAW && s->pcb.raw != NULL)
            r = raw_send(s->pcb.raw, p);
    }
    pbuf_free(p);
    netstack_unlock();

    if (r != ERR_OK)
    {
        sb_set_errno(base, sb_map_err(r));
        return -1;
    }
    return (LONG)total;
}

/* ----------------------------------------------------------------- recv --- */

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
         * and under ns_Core it stalls RX injection — so detach a run of
         * whole pbufs under the lock, copy with the lock RELEASED, then
         * free and acknowledge the window in one short re-acquired section
         * (ndo_RxRelease and tcp_recved must run under the lock). Only
         * per-pbuf len fields are trusted for accounting: the chain-wide
         * tot_len is u16, which the backlog under a 256 KB+ window
         * overflows. */
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

/* pop a consumed datagram off @s's queue and free it. Core lock held. */
static void sb_dgram_drop(struct SocketBase *root, struct SbSocket *s,
                          struct SbDgram *d)
{
    RemoveMinNode(&d->node);
    s->ndgrams--;
    pbuf_free(d->p);
    FreePooled(root->sockPool, d, sizeof(struct SbDgram));
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
        sb_dgram_drop(root, s, d);
    netstack_unlock();

    sb_addr_out(from, fromlen, addr, port);
    return (LONG)n;
}

/* recvmsg for datagram sockets: scatter the head datagram straight from its
 * pbuf chain across the caller's iovs — no bounce buffer, no size cap. Excess
 * beyond the total iov space is dropped and flagged with MSG_TRUNC. */
static LONG sb_dgram_recvmsg(struct SocketBase *base, struct SbSocket *s,
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
        sb_dgram_drop(root, s, d);
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
            ULONG a;
            UWORD p;
            sb_peer_ip(s, &a, &p);
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

LONG bsd_recvmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"),
                 struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: sock=%ld flags=%ld\n", __func__, sock, flags);
    struct sb_msghdr *mh = msg;
    struct SbSocket *s = sb_fd_get(base, sock);

    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }
    if (mh == NULL || (mh->msg_iovlen != 0 && mh->msg_iov == NULL))
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    /* no ancillary data is ever produced */
    mh->msg_flags = 0;
    mh->msg_controllen = 0;

    if (s->type == SBT_TCP)
    {
        LONG total = 0;
        for (ULONG i = 0; i < mh->msg_iovlen; i++)
        {
            struct sb_iovec *iv = &mh->msg_iov[i];
            if (iv->iov_len == 0)
                continue;
            LONG n = bsd_recv(sock, iv->iov_base, (LONG)iv->iov_len,
                              flags | (total > 0 ? SB_MSG_DONTWAIT : 0), base);
            if (n < 0)
                return total > 0 ? total : -1;
            total += n;
            if (n == 0 || (ULONG)n < iv->iov_len)
                break;
        }
        mh->msg_namelen = 0; /* stream sockets report no sender address */
        return total;
    }

    /* datagram: scatter the whole datagram across the iovs straight from its
     * pbuf chain, flagging (MSG_TRUNC) and dropping any excess */
    return sb_dgram_recvmsg(base, s, mh, flags);
}
