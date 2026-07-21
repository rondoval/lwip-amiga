/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Socket options (setsockopt/getsockopt), the GetSocketEvents /
 * SO_EVENTMASK event surface, and the per-opener signal configuration.
 */

#include "sb_base.h"

#include <dos/dos.h> /* SIGBREAKF_CTRL_C */

#include <lwip/tcp.h>
#include <lwip/igmp.h>
#include <lwip/ip_addr.h>

#include <debug.h>

#include "netstack.h"

/* Multicast-membership registry. lwIP tracks group membership per netif
 * (refcounted), not per socket, so we remember which (interface, group) pairs
 * each socket joined via IP_ADD_MEMBERSHIP and leave them when the socket is
 * torn down — mirroring lwIP's own sockets.c. All access is under the core
 * lock; a fixed table suits the handful of groups real apps join. */
#define SB_MCAST_MEMBERSHIPS 32
static struct SbMcastMember
{
    struct SbSocket *s; /* owning socket; NULL = free slot */
    ULONG group;        /* group address, network order */
    ULONG ifAddr;       /* local interface address, network order (0 = any) */
} sb_mcast[SB_MCAST_MEMBERSHIPS];

/* core lock held. Returns 0 or an SB_* errno. Idempotent per (s, group, if). */
static LONG sb_mcast_join(struct SbSocket *s, ULONG group, ULONG ifAddr)
{
    LONG slot = -1;
    for (LONG i = 0; i < SB_MCAST_MEMBERSHIPS; i++)
    {
        if (sb_mcast[i].s == NULL)
        {
            if (slot < 0)
                slot = i;
        }
        else if (sb_mcast[i].s == s && sb_mcast[i].group == group &&
                 sb_mcast[i].ifAddr == ifAddr)
        {
            return 0; /* this socket already joined: no double-count */
        }
    }
    if (slot < 0)
        return SB_ENOBUFS;

    ip4_addr_t g, ifa;
    ip4_addr_set_u32(&g, group);
    ip4_addr_set_u32(&ifa, ifAddr);
    if (igmp_joingroup(&ifa, &g) != ERR_OK)
        return SB_EADDRNOTAVAIL;

    sb_mcast[slot].s = s;
    sb_mcast[slot].group = group;
    sb_mcast[slot].ifAddr = ifAddr;
    return 0;
}

static LONG sb_mcast_leave(struct SbSocket *s, ULONG group, ULONG ifAddr)
{
    for (LONG i = 0; i < SB_MCAST_MEMBERSHIPS; i++)
    {
        if (sb_mcast[i].s == s && sb_mcast[i].group == group &&
            sb_mcast[i].ifAddr == ifAddr)
        {
            ip4_addr_t g, ifa;
            ip4_addr_set_u32(&g, group);
            ip4_addr_set_u32(&ifa, ifAddr);
            igmp_leavegroup(&ifa, &g);
            sb_mcast[i].s = NULL;
            return 0;
        }
    }
    return SB_EADDRNOTAVAIL; /* not a member on this socket */
}

void sb_mcast_drop_all(struct SbSocket *s)
{
    for (LONG i = 0; i < SB_MCAST_MEMBERSHIPS; i++)
    {
        if (sb_mcast[i].s != s)
            continue;
        ip4_addr_t g, ifa;
        ip4_addr_set_u32(&g, sb_mcast[i].group);
        ip4_addr_set_u32(&ifa, sb_mcast[i].ifAddr);
        igmp_leavegroup(&ifa, &g);
        sb_mcast[i].s = NULL;
    }
}

LONG bsd_setsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"),
                    APTR optval asm("a0"), LONG optlen asm("d3"),
                    struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: fd %ld level %ld optname %ld\n", __func__, sock, level, optname);
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
        case SB_SO_KEEPALIVE:
        case SB_SO_BROADCAST:
        {
            u8_t optbit = optname == SB_SO_REUSEADDR  ? SOF_REUSEADDR
                          : optname == SB_SO_KEEPALIVE ? SOF_KEEPALIVE
                                                       : SOF_BROADCAST;
            if (s->pcb.any != NULL)
            {
                if (val)
                    ip_set_option((struct ip_pcb *)s->pcb.any, optbit);
                else
                    ip_reset_option((struct ip_pcb *)s->pcb.any, optbit);
            }
            break;
        }
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
            /* a subscription may arrive AFTER the state it cares about
             * already exists (armed right after accept, request already
             * queued): synthesize events from the current state so none
             * are lost */
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
    else if (level == SB_IPPROTO_IP)
    {
        switch (optname)
        {
        case SB_IP_ADD_MEMBERSHIP:
        case SB_IP_DROP_MEMBERSHIP:
        {
            const struct sb_ip_mreq *mreq = optval;
            if (mreq == NULL || optlen < (LONG)sizeof(*mreq))
            {
                e = SB_EINVAL;
                break;
            }
            if (optname == SB_IP_ADD_MEMBERSHIP)
                e = sb_mcast_join(s, mreq->imr_multiaddr, mreq->imr_interface);
            else
                e = sb_mcast_leave(s, mreq->imr_multiaddr, mreq->imr_interface);
            break;
        }
        case SB_IP_MULTICAST_TTL:
        case SB_IP_MULTICAST_LOOP:
        case SB_IP_MULTICAST_IF:
            /* TX-side multicast options: accepted so multicast senders don't
             * fail on them. lwIP's LWIP_MULTICAST_TX_OPTIONS is off and there
             * is a single netif, so the defaults already match — nothing to
             * program. */
            break;
        default:
            e = SB_ENOPROTOOPT;
            break;
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
    KprintfT("[bsdsocket] %s: fd %ld level %ld optname %ld\n", __func__, sock, level, optname);
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
    KprintfT("[bsdsocket] %s\n", __func__);
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
    KprintfT("[bsdsocket] %s\n", __func__);
    return (LONG)base->fdCount;
}

VOID bsd_SetSocketSignals(ULONG intMask asm("d0"), ULONG ioMask asm("d1"),
                          ULONG urgMask asm("d2"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: intMask 0x%08lx ioMask 0x%08lx urgMask 0x%08lx\n", __func__, intMask, ioMask, urgMask);
    base->breakMask = intMask != 0 ? intMask : SIGBREAKF_CTRL_C;
    base->sigIoMask = ioMask;
    base->sigUrgMask = urgMask;
    /* see SBTC_SIGIOMASK: readiness may predate the mask */
    if (ioMask != 0 && base->task != NULL)
        Signal(base->task, ioMask);
}
