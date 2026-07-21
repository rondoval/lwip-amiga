/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SocketBaseTagList — the per-opener configuration dispatcher: signal
 * masks, errno/h_errno redirection, syslog configuration, fd-table growth,
 * the release string, Roadshow capability probes and the status/byte
 * counters. Returns 0 on success or the 1-based index of the failing tag.
 */

#include "sb_base.h"

#include <exec/memory.h>
#include <utility/tagitem.h>

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <debug.h>

#include "netstack.h"

/* SBTC_RELEASESTRPTR (RELEASE_STRING from the build): "lwip-amiga x.y" */
static const char releaseString[] = RELEASE_STRING;

LONG bsd_SocketBaseTagList(struct TagItem *tags asm("a0"),
                           struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: tags=0x%08lx\n", __func__, (ULONG)tags);
    if (tags == NULL)
        return 0;

    LONG index = 0;
    for (struct TagItem *t = tags; t->ti_Tag != TAG_END;)
    {
        if (t->ti_Tag == TAG_MORE)
        {
            t = (struct TagItem *)t->ti_Data;
            continue;
        }
        if (t->ti_Tag == TAG_SKIP)
        {
            t += 1 + t->ti_Data;
            continue;
        }
        index++;
        if (t->ti_Tag == TAG_IGNORE || !(t->ti_Tag & TAG_USER))
        {
            t++;
            continue;
        }

        ULONG td = t->ti_Tag;
        BOOL isSet = (td & SBTF_SET) != 0;
        BOOL isRef = (td & SBTF_REF) != 0;
        ULONG code = SBTM_CODE(td);
        ULONG *valp = isRef ? (ULONG *)t->ti_Data : &t->ti_Data;

        if (isRef && valp == NULL)
            return index;

        switch (code)
        {
        case SBTC_BREAKMASK:
            if (isSet)
                base->breakMask = *valp;
            else
                *valp = base->breakMask;
            break;
        case SBTC_SIGIOMASK:
            if (isSet)
            {
                base->sigIoMask = *valp;
                /* readiness may predate the mask (data queued before the
                 * app armed SIGIO — AExplorer does exactly this after
                 * accept): one spurious delivery makes it re-poll */
                if (*valp != 0 && base->task != NULL)
                    Signal(base->task, *valp);
            }
            else
                *valp = base->sigIoMask;
            break;
        case SBTC_SIGURGMASK:
            if (isSet)
                base->sigUrgMask = *valp;
            else
                *valp = base->sigUrgMask;
            break;
        case SBTC_SIGEVENTMASK:
            if (isSet)
            {
                base->sigEventMask = *valp;
                /* events latched before the mask was registered (SO_EVENTMASK
                 * armed first) would otherwise never signal — but a bare
                 * registration must stay silent: an unconditional Signal here
                 * reads as a spurious event to apps that trust the mask */
                if (*valp != 0 && base->task != NULL && base->fd != NULL)
                {
                    netstack_lock();
                    for (ULONG i = 0; i < base->fdCount; i++)
                    {
                        struct SbSocket *sk = base->fd[i];
                        if (sk != NULL && sk->eventsPending != 0)
                        {
                            Signal(base->task, *valp);
                            break;
                        }
                    }
                    netstack_unlock();
                }
            }
            else
                *valp = base->sigEventMask;
            break;
        case SBTC_ERRNO:
            if (isSet)
                sb_set_errno(base, (LONG)*valp);
            else
                *valp = (ULONG)base->internalErrno;
            break;
        case SBTC_HERRNO:
            if (isSet)
                sb_set_herrno(base, (LONG)*valp);
            else
                *valp = (ULONG)base->hErrno;
            break;
        /* syslog config (openlog/setlogmask). Stored per-opener and honoured by
         * bsd_vsyslog; the C runtimes set LOGTAGPTR at socket-init time. */
        case SBTC_LOGSTAT:
            if (isSet)
                base->logStat = *valp;
            else
                *valp = base->logStat;
            break;
        case SBTC_LOGTAGPTR:
            if (isSet)
                base->logTagPtr = (STRPTR)*valp;
            else
                *valp = (ULONG)base->logTagPtr;
            break;
        case SBTC_LOGFACILITY:
            if (isSet)
                base->logFacility = *valp;
            else
                *valp = base->logFacility;
            break;
        case SBTC_LOGMASK:
            if (isSet)
                base->logMask = *valp;
            else
                *valp = base->logMask;
            break;
        case SBTC_DTABLESIZE:
            if (isSet)
            {
                /* grow-only: shrinking under open sockets is unanswerable
                 * (AmiTCP keeps the bigger table too); a shrink request
                 * succeeds but keeps the current size */
                ULONG want = *valp;
                if (want > SB_FD_MAX)
                    want = SB_FD_MAX;
                if (want > base->fdCount)
                {
                    struct SbSocket **nfd =
                        AllocMem(want * sizeof(struct SbSocket *), MEMF_PUBLIC | MEMF_CLEAR);
                    if (nfd == NULL)
                        return index;
                    netstack_lock();
                    CopyMem(base->fd, nfd, base->fdCount * sizeof(struct SbSocket *));
                    struct SbSocket **ofd = base->fd;
                    ULONG ocount = base->fdCount;
                    base->fd = nfd;
                    base->fdCount = want;
                    netstack_unlock();
                    FreeMem(ofd, ocount * sizeof(struct SbSocket *));
                }
            }
            else
                *valp = base->fdCount;
            break;
        case SBTC_ERRNOBYTEPTR:
            if (!isSet)
                return index;
            base->errnoPtr = (APTR)*valp;
            base->errnoSize = 1;
            break;
        case SBTC_ERRNOWORDPTR:
            if (!isSet)
                return index;
            base->errnoPtr = (APTR)*valp;
            base->errnoSize = 2;
            break;
        case SBTC_ERRNOLONGPTR:
            if (!isSet)
            {
                /* readback: the registered pointer, but only while it is
                 * actually long-sized (SetErrnoPtr can have narrowed it) */
                *valp = base->errnoSize == 4 ? (ULONG)base->errnoPtr : 0;
                break;
            }
            if ((APTR)*valp != NULL)
            {
                base->errnoPtr = (APTR)*valp;
                base->errnoSize = 4;
            }
            else
            {
                base->errnoPtr = &base->internalErrno;
                base->errnoSize = 4;
            }
            break;
        case SBTC_HERRNOLONGPTR:
            if (!isSet)
            {
                *valp = (ULONG)base->hErrnoPtr;
                break;
            }
            base->hErrnoPtr = (APTR)*valp != NULL ? (LONG *)*valp : &base->hErrno;
            break;
        case SBTC_RELEASESTRPTR:
            if (isSet)
                return index;
            *valp = (ULONG)releaseString;
            break;

        /* Roadshow feature-capability probes (read-only): report which
         * extension LVO groups this library implements so apps can discover
         * them instead of guessing. Implemented groups -> TRUE. */
        case SBTC_HAVE_DNS_API:
        case SBTC_HAVE_LOCAL_DATABASE_API:
        case SBTC_HAVE_ADDRESS_CONVERSION_API:
        case SBTC_HAVE_GETHOSTADDR_R_API:
        /* status API: GetNetworkStatistics is implemented (sb_netstat.c). */
        case SBTC_HAVE_STATUS_API:
        /* interface API: the read-only query subset (ObtainInterfaceList /
         * QueryInterfaceTagList) is implemented; the config LVOs refuse
         * gracefully with EINVAL (sb_ifquery.c). */
        case SBTC_HAVE_INTERFACE_API:
            if (isSet)
                return index;
            *valp = TRUE;
            break;
        /* Unimplemented groups -> FALSE; the packet-filter probe reports
         * zero channels. Answering keeps the taglist succeeding rather
         * than erroring on an unrecognised tag. */
        case SBTC_HAVE_ROUTING_API:
        case SBTC_HAVE_MONITORING_API:
        case SBTC_HAVE_SERVER_API:
        case SBTC_HAVE_ROADSHOWDATA_API:
        case SBTC_HAVE_KERNEL_MEMORY_API:
            if (isSet)
                return index;
            *valp = FALSE;
            break;
        case SBTC_NUM_PACKET_FILTER_CHANNELS:
            if (isSet)
                return index;
            *valp = 0;
            break;
        case SBTC_SYSTEM_STATUS:
        {
            if (isSet)
                return index;
            ULONG st = 0;
            netstack_lock();
            struct netif *nif;
            NETIF_FOREACH(nif)
            {
                if (nif->name[0] == 'l' && nif->name[1] == 'o')
                    continue; /* loopback is not an "interface" here */
                if (netif_is_up(nif) && ip4_addr_get_u32(netif_ip4_addr(nif)) != 0)
                    st |= SBSYSSTAT_Interfaces | SBSYSSTAT_BCast_Interfaces;
            }
            if (ip4_addr_get_u32(ip_2_ip4(dns_getserver(0))) != 0)
                st |= SBSYSSTAT_Resolver;
            if (netif_default != NULL &&
                ip4_addr_get_u32(netif_ip4_gw(netif_default)) != 0)
                st |= SBSYSSTAT_Routes | SBSYSSTAT_DefaultRoute;
            netstack_unlock();
            *valp = st;
            break;
        }
        case SBTC_GET_BYTES_RECEIVED:
        case SBTC_GET_BYTES_SENT:
        {
            /* 64-bit counters: GET by reference only (an SBQUAD_T can't fit a
             * VAL slot). Sourced from the once-a-second NIC-stats cache. */
            if (isSet || !isRef)
                return index;
            struct SocketBase *root = SB_ROOT(base);
            netstack_lock();
            struct NetDevU64 v = (code == SBTC_GET_BYTES_RECEIVED)
                                     ? root->netStats.nds_RxBytes
                                     : root->netStats.nds_TxBytes;
            netstack_unlock();
            sb_squad_from_u64((struct sb_squad *)valp, v);
            break;
        }
        default:
            return index; /* unknown tag: report its 1-based position */
        }
        t++;
    }
    return 0;
}
