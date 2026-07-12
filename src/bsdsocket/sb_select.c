/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WaitSelect — the autodoc contract, exactly:
 *  - break signal: return -1, errno EINTR, break re-posted, sets UNMODIFIED
 *  - user signal, nothing ready: return 0, *signals = received user bits
 *  - timeout elapsed: return 0
 *  - otherwise: count of ready descriptors, sets rewritten as ready subsets,
 *    *signals = received user bits (0 if none)
 *  - sets are only written back on returns >= 0
 * fd bits use the BSD layout: 32-bit words, bit (fd % 32) in word (fd / 32).
 */

#include "sb_base.h"

#include "netstack.h"

#define SB_FD_WORDS ((SB_FD_COUNT + 31) / 32)

static BOOL sb_fd_bit(const ULONG *set, LONG fd)
{
    return set != NULL && (set[fd >> 5] & (1UL << (fd & 31))) != 0;
}

static void sb_fd_setbit(ULONG *set, LONG fd)
{
    set[fd >> 5] |= 1UL << (fd & 31);
}

/* scan under the core lock; -1 = EBADF */
static LONG sb_select_scan(struct SocketBase *base, LONG nfds,
                           const ULONG *r_in, const ULONG *w_in, const ULONG *e_in,
                           ULONG *r_out, ULONG *w_out, ULONG *e_out)
{
    LONG hits = 0;
    (void)e_out; /* no exceptional conditions defined (no OOB) */

    for (LONG fd = 0; fd < nfds; fd++)
    {
        BOOL wr = sb_fd_bit(r_in, fd);
        BOOL ww = sb_fd_bit(w_in, fd);
        BOOL we = sb_fd_bit(e_in, fd);
        if (!wr && !ww && !we)
            continue;

        struct SbSocket *s = base->fd[fd];
        if (s == NULL)
            return -1;

        if (wr && sb_sock_readable(s))
        {
            sb_fd_setbit(r_out, fd);
            hits++;
        }
        if (ww && sb_sock_writable(s))
        {
            sb_fd_setbit(w_out, fd);
            hits++;
        }
        /* exceptions: none defined (no OOB support) */
    }
    return hits;
}

LONG bsd_WaitSelect(LONG nfds asm("d0"), APTR readfds asm("a0"), APTR writefds asm("a1"),
                    APTR exceptfds asm("a2"), APTR timeout asm("a3"), ULONG *signals asm("d1"),
                    struct SocketBase *base asm("a6"))
{
    const struct sb_timeval *tv = timeout;
    ULONG userMask = signals != NULL ? *signals : 0;

    if (nfds < 0)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    if (nfds > SB_FD_COUNT)
        nfds = SB_FD_COUNT; /* truncate, per the autodoc */
    if (tv != NULL && tv->tv_micro >= 1000000)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    /* local working copies; the caller's sets stay untouched until success */
    ULONG r_in[SB_FD_WORDS], w_in[SB_FD_WORDS], e_in[SB_FD_WORDS];
    ULONG r_out[SB_FD_WORDS], w_out[SB_FD_WORDS], e_out[SB_FD_WORDS];
    ULONG copyBytes = (ULONG)((nfds + 31) / 32) * sizeof(ULONG);

    for (ULONG i = 0; i < SB_FD_WORDS; i++)
    {
        r_in[i] = w_in[i] = e_in[i] = 0;
        r_out[i] = w_out[i] = e_out[i] = 0;
    }
    if (readfds != NULL)
        CopyMem(readfds, r_in, copyBytes);
    if (writefds != NULL)
        CopyMem(writefds, w_in, copyBytes);
    if (exceptfds != NULL)
        CopyMem(exceptfds, e_in, copyBytes);

    BOOL timerArmed = FALSE;
    BOOL timedOut = FALSE;
    ULONG gotUser = 0;
    LONG hits;

    netstack_lock();
    for (;;)
    {
        for (ULONG i = 0; i < SB_FD_WORDS; i++)
            r_out[i] = w_out[i] = e_out[i] = 0;

        hits = sb_select_scan(base, nfds,
                              readfds != NULL ? r_in : NULL,
                              writefds != NULL ? w_in : NULL,
                              exceptfds != NULL ? e_in : NULL,
                              r_out, w_out, e_out);
        if (hits != 0 || gotUser != 0 || timedOut)
            break;

        if (tv != NULL && tv->tv_secs == 0 && tv->tv_micro == 0)
            break; /* poll */

        if (tv != NULL && !timerArmed)
        {
            base->timerReq->tr_node.io_Command = TR_ADDREQUEST;
            base->timerReq->tr_time.tv_secs = tv->tv_secs;
            base->timerReq->tr_time.tv_micro = tv->tv_micro;
            SendIO(&base->timerReq->tr_node);
            timerArmed = TRUE;
        }

        ULONG timerBit = timerArmed ? (1UL << base->timerPort->mp_SigBit) : 0;
        ULONG waitMask = (1UL << base->sigBit) | base->breakMask | userMask | timerBit;

        SetSignal(0UL, 1UL << base->sigBit);
        netstack_unlock();
        ULONG sigs = Wait(waitMask);
        netstack_lock();

        if (sigs & base->breakMask)
        {
            netstack_unlock();
            if (timerArmed)
            {
                AbortIO(&base->timerReq->tr_node);
                WaitIO(&base->timerReq->tr_node);
            }
            /* the break stays posted; the sets stay unmodified */
            Signal(base->task, sigs & base->breakMask);
            sb_set_errno(base, SB_EINTR);
            return -1;
        }

        gotUser |= sigs & userMask;
        if (timerArmed && (sigs & timerBit))
        {
            WaitIO(&base->timerReq->tr_node);
            timerArmed = FALSE;
            timedOut = TRUE;
        }
    }
    netstack_unlock();

    if (timerArmed)
    {
        AbortIO(&base->timerReq->tr_node);
        WaitIO(&base->timerReq->tr_node);
    }

    if (hits < 0)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }

    if (readfds != NULL)
        CopyMem(r_out, readfds, copyBytes);
    if (writefds != NULL)
        CopyMem(w_out, writefds, copyBytes);
    if (exceptfds != NULL)
        CopyMem(e_out, exceptfds, copyBytes);
    if (signals != NULL)
        *signals = gotUser;

    return hits;
}
