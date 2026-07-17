/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * errno plumbing, SocketBaseTagList, the inet_* utilities, gethostbyname
 * (lwIP DNS, blocking on the opener's signal) and the tiny netdb subset.
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

/* --------------------------------------------------------------- errno --- */

void sb_set_errno(struct SocketBase *base, LONG code)
{
    KprintfH("[bsdsocket] %s: code=%ld\n", __func__, code);
    base->internalErrno = code;
    if (base->errnoPtr != NULL && base->errnoPtr != &base->internalErrno)
    {
        switch (base->errnoSize)
        {
        case 1:
            *(BYTE *)base->errnoPtr = (BYTE)code;
            break;
        case 2:
            *(WORD *)base->errnoPtr = (WORD)code;
            break;
        default:
            *(LONG *)base->errnoPtr = code;
            break;
        }
    }
}

void sb_set_herrno(struct SocketBase *base, LONG code)
{
    KprintfH("[bsdsocket] %s: code=%ld\n", __func__, code);
    base->hErrno = code;
    if (base->hErrnoPtr != NULL)
        *base->hErrnoPtr = code;
}

LONG bsd_Errno(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: errno=%ld\n", __func__, base->internalErrno);
    return base->internalErrno;
}

VOID bsd_SetErrnoPtr(APTR errnoPtr asm("a0"), LONG size asm("d0"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: ptr=0x%08lx size=%ld\n", __func__, (ULONG)errnoPtr, size);
    if (errnoPtr != NULL && (size == 1 || size == 2 || size == 4))
    {
        base->errnoPtr = errnoPtr;
        base->errnoSize = (ULONG)size;
    }
    else
    {
        base->errnoPtr = &base->internalErrno;
        base->errnoSize = sizeof(LONG);
    }
}

/* ---------------------------------------------------- SocketBaseTagList --- */

LONG bsd_SocketBaseTagList(struct TagItem *tags asm("a0"),
                           struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: tags=0x%08lx\n", __func__, (ULONG)tags);
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
        /* Not (yet) implemented groups -> FALSE; the packet-filter probe
         * reports zero channels. Answering keeps the taglist succeeding
         * rather than erroring on an unrecognised tag. */
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

/* ----------------------------------------------------------- inet utils --- */

STRPTR bsd_Inet_NtoA(ULONG ip asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: ip=0x%08lx\n", __func__, ip);
    ip4_addr_t a;
    ip4_addr_set_u32(&a, ip);
    ip4addr_ntoa_r(&a, base->ntoaBuf, sizeof(base->ntoaBuf));
    return (STRPTR)base->ntoaBuf;
}

ULONG bsd_inet_addr(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    ip4_addr_t a;
    if (cp == NULL || ip4addr_aton((const char *)cp, &a) == 0)
        return 0xFFFFFFFF; /* INADDR_NONE */
    return ip4_addr_get_u32(&a);
}

LONG bsd_inet_aton(STRPTR cp asm("a0"), APTR addr asm("a1"),
                   struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    ip4_addr_t a;
    if (cp == NULL || addr == NULL || ip4addr_aton((const char *)cp, &a) == 0)
        return 0;
    *(ULONG *)addr = ip4_addr_get_u32(&a);
    return 1;
}

STRPTR bsd_inet_ntop(LONG af asm("d0"), APTR src asm("a0"), STRPTR dst asm("a1"),
                     LONG size asm("d1"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: af=%ld size=%ld\n", __func__, af, size);
    if (af != SB_AF_INET || src == NULL || dst == NULL)
    {
        sb_set_errno(base, SB_EAFNOSUPPORT);
        return NULL;
    }
    ip4_addr_t a;
    ip4_addr_set_u32(&a, *(ULONG *)src);
    if (ip4addr_ntoa_r(&a, (char *)dst, (int)size) == NULL)
    {
        sb_set_errno(base, SB_EMSGSIZE);
        return NULL;
    }
    return dst;
}

LONG bsd_inet_pton(LONG af asm("d0"), STRPTR src asm("a0"), APTR dst asm("a1"),
                   struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: af=%ld src=%s\n", __func__, af, src != NULL ? (ULONG)src : (ULONG)"(null)");
    if (af != SB_AF_INET)
    {
        sb_set_errno(base, SB_EAFNOSUPPORT);
        return -1;
    }
    ip4_addr_t a;
    if (src == NULL || dst == NULL || ip4addr_aton((const char *)src, &a) == 0)
        return 0;
    *(ULONG *)dst = ip4_addr_get_u32(&a);
    return 1;
}

/* classful helpers — legacy API surface, exact 4.3BSD semantics */
ULONG bsd_Inet_LnaOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: in=0x%08lx\n", __func__, in);
    (void)base;
    if ((in & 0x80000000UL) == 0)
        return in & 0x00FFFFFF;
    if ((in & 0xC0000000UL) == 0x80000000UL)
        return in & 0x0000FFFF;
    return in & 0x000000FF;
}

ULONG bsd_Inet_NetOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: in=0x%08lx\n", __func__, in);
    (void)base;
    if ((in & 0x80000000UL) == 0)
        return (in >> 24) & 0xFF;
    if ((in & 0xC0000000UL) == 0x80000000UL)
        return (in >> 16) & 0xFFFF;
    return (in >> 8) & 0xFFFFFF;
}

ULONG bsd_Inet_MakeAddr(ULONG net asm("d0"), ULONG host asm("d1"),
                        struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: net=0x%08lx host=0x%08lx\n", __func__, net, host);
    (void)base;
    if (net < 128)
        return (net << 24) | (host & 0x00FFFFFF);
    if (net < 65536)
        return (net << 16) | (host & 0x0000FFFF);
    return (net << 8) | (host & 0x000000FF);
}

ULONG bsd_inet_network(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    if (cp == NULL)
        return 0xFFFFFFFF; /* INADDR_NONE */

    /* Classic 4.4BSD inet_network(): up to four dot-separated parts, each in
     * C notation (leading 0x hex, leading 0 octal, else decimal). Each part
     * contributes one byte, packed right-aligned into the low bytes in host
     * order — "192.168" -> 0xC0A8, "10.0.0.0" -> 0x0A000000, "127" -> 0x7F.
     * Parts are masked to a byte (BSD behaviour), not rejected when > 255. */
    const char *s = (const char *)cp;
    ULONG parts[4];
    ULONG nparts = 0;

    for (;;)
    {
        ULONG radix = 10;
        if (*s == '0')
        {
            radix = 8;
            s++;
            if (*s == 'x' || *s == 'X')
            {
                radix = 16;
                s++;
            }
        }

        ULONG val = 0;
        BOOL any = (radix != 10); /* a consumed leading '0' is itself digit 0 */
        for (;;)
        {
            char c = *s;
            ULONG digit;
            if (c >= '0' && c <= '9')
                digit = (ULONG)(c - '0');
            else if (radix == 16 && c >= 'a' && c <= 'f')
                digit = (ULONG)(c - 'a' + 10);
            else if (radix == 16 && c >= 'A' && c <= 'F')
                digit = (ULONG)(c - 'A' + 10);
            else
                break;
            if (digit >= radix)
                break;
            val = val * radix + digit;
            any = TRUE;
            s++;
        }

        if (!any || nparts >= 4)
            return 0xFFFFFFFF;
        parts[nparts++] = val;

        if (*s != '.')
            break;
        s++;
    }

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s != '\0')
        return 0xFFFFFFFF;

    ULONG result = 0;
    for (ULONG i = 0; i < nparts; i++)
        result = (result << 8) | (parts[i] & 0xFF);
    return result;
}

/* ---------------------------------------------------------- DNS / netdb --- */

static void sb_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    KprintfH("[bsdsocket] %s: name=%s found=%ld\n", __func__, (ULONG)name, (LONG)(ipaddr != NULL));
    struct SocketBase *base = arg;
    (void)name;

    if (ipaddr != NULL)
    {
        base->dnsAddr = *ipaddr;
        base->dnsErr = 0;
    }
    else
    {
        base->dnsErr = SB_HOST_NOT_FOUND;
    }
    base->dnsDone = TRUE;
    Signal(base->task, 1UL << base->sigBit);
}

static ULONG sb_strlcpy(char *dst, const char *src, ULONG max)
{
    KprintfH("[bsdsocket] %s\n", __func__);
    ULONG n = 0;
    while (src[n] != '\0' && n < max - 1)
    {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}

static struct sb_hostent *sb_host_fill(struct SocketBase *base, const char *name, ULONG addr)
{
    KprintfH("[bsdsocket] %s: name=%s addr=0x%08lx\n", __func__, (ULONG)name, addr);
    sb_strlcpy(base->hostName, name, sizeof(base->hostName));
    base->hostAddr = addr;
    base->hostAddrList[0] = (char *)&base->hostAddr;
    base->hostAddrList[1] = NULL;
    base->hostAliases[0] = NULL;
    base->host.h_name = base->hostName;
    base->host.h_aliases = base->hostAliases;
    base->host.h_addrtype = SB_AF_INET;
    base->host.h_length = 4;
    base->host.h_addr_list = base->hostAddrList;
    return &base->host;
}

/* One blocking forward-DNS lookup of @name. Returns 0 with *addr filled on
 * success; SB_TRY_AGAIN if interrupted (errno already set); or
 * SB_HOST_NOT_FOUND on a negative or failed answer. */
static LONG sb_dns_query(struct SocketBase *base, const char *name, ULONG *addr)
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, (ULONG)name);
    netstack_lock();
    base->dnsDone = FALSE;
    base->dnsErr = 0;

    err_t r = dns_gethostbyname(name, &base->dnsAddr, sb_dns_cb, base);
    if (r == ERR_INPROGRESS)
    {
        while (!base->dnsDone)
        {
            LONG we = sb_wait(base);
            if (we != 0)
            {
                netstack_unlock();
                sb_set_errno(base, we);
                return SB_TRY_AGAIN;
            }
        }
        r = base->dnsErr == 0 ? ERR_OK : ERR_VAL;
    }
    netstack_unlock();

    if (r != ERR_OK)
        return SB_HOST_NOT_FOUND;

    *addr = ip4_addr_get_u32(ip_2_ip4(&base->dnsAddr));
    return 0;
}

/* If @name is unqualified (contains no dot) and @domain is set, compose
 * "name.domain" into @out and return TRUE; otherwise FALSE. A dotted name is
 * already qualified (or a literal) and is left alone. */
static BOOL sb_qualify(const char *name, const char *domain, char *out, ULONG outmax)
{
    if (name == NULL || name[0] == '\0' || domain == NULL || domain[0] == '\0')
        return FALSE;

    ULONG nlen = 0;
    while (name[nlen] != '\0')
    {
        if (name[nlen] == '.')
            return FALSE; /* already qualified */
        nlen++;
    }
    ULONG dlen = 0;
    while (domain[dlen] != '\0')
        dlen++;
    if (nlen + 1 + dlen + 1 > outmax)
        return FALSE; /* would not fit */

    ULONG o = 0;
    for (ULONG i = 0; i < nlen; i++)
        out[o++] = name[i];
    out[o++] = '.';
    for (ULONG i = 0; i < dlen; i++)
        out[o++] = domain[i];
    out[o] = '\0';
    return TRUE;
}

/* Blocking resolver core shared with gethostbyname_r: resolves @name, fills
 * the per-base hostent, and reports the address/h_errno through the out
 * parameters. An unqualified name that misses is retried once as
 * "name.<search-domain>" (Roadshow order: bare first, qualified fallback).
 * Returns the per-base hostent or NULL. */
struct sb_hostent *sb_host_resolve(struct SocketBase *base, const char *name, ULONG *addrOut, LONG *herrOut)
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");

    if (name == NULL)
    {
        if (herrOut != NULL)
            *herrOut = SB_HOST_NOT_FOUND;
        sb_set_herrno(base, SB_HOST_NOT_FOUND);
        return NULL;
    }

    ULONG addr = 0;
    LONG rc = sb_dns_query(base, name, &addr);

    const char *canon = name;
    char qbuf[256]; /* DNS names are <= 253 chars */
    if (rc == SB_HOST_NOT_FOUND &&
        sb_qualify(name, SB_ROOT(base)->defaultDomain, qbuf, sizeof(qbuf)))
    {
        /* take the retry's verdict as-is: an interrupted retry must surface
         * as TRY_AGAIN, not as the bare lookup's definitive NOT_FOUND */
        rc = sb_dns_query(base, qbuf, &addr);
        if (rc == 0)
            canon = qbuf; /* report the name that actually resolved */
    }

    if (rc != 0)
    {
        sb_set_herrno(base, rc); /* SB_TRY_AGAIN or SB_HOST_NOT_FOUND */
        if (herrOut != NULL)
            *herrOut = rc;
        return NULL;
    }

    if (addrOut != NULL)
        *addrOut = addr;
    if (herrOut != NULL)
        *herrOut = 0;
    return sb_host_fill(base, canon, addr);
}

APTR bsd_gethostbyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    return sb_host_resolve(base, (const char *)name, NULL, NULL);
}

APTR bsd_gethostbyaddr(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"),
                       struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: addr=0x%08lx len=%ld\n", __func__, (ULONG)addr, len);
    if (addr == NULL || len != 4 || type != SB_AF_INET)
    {
        sb_set_herrno(base, SB_HOST_NOT_FOUND);
        return NULL;
    }
    ULONG a = *(ULONG *)addr;
    char nameBuf[SB_HOSTNAME_MAX];
    if (sb_ptr_resolve(base, a, nameBuf, sizeof(nameBuf)) != 0)
    {
        /* no PTR record (or no resolver / timeout): BSD reports not-found */
        sb_set_herrno(base, SB_HOST_NOT_FOUND);
        return NULL;
    }
    return sb_host_fill(base, nameBuf, a);
}

LONG bsd_gethostname(STRPTR name asm("a0"), LONG namelen asm("d0"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: namelen=%ld\n", __func__, namelen);
    const char *hostname = SB_ROOT(base)->netCfg.cfg_Hostname;
    if (name == NULL || namelen <= 0)
        return -1;
    ULONG i;
    for (i = 0; hostname[i] != '\0' && (LONG)i < namelen - 1; i++)
        name[i] = hostname[i];
    name[i] = '\0';
    return 0;
}

ULONG bsd_gethostid(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    (void)base;
    /* BSD convention: the host identifier is the primary IP address;
     * unconfigured hosts still answer non-zero with the loopback address */
    ULONG id = 0;
    netstack_lock();
    if (netif_default != NULL)
        id = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    netstack_unlock();
    if (id == 0)
        id = 0x7F000001UL;
    return id;
}

/* getprotobyname/-number over a fixed table */
static const struct
{
    const char *name;
    LONG proto;
} sb_protos[] = {
    {"ip", 0}, {"icmp", 1}, {"igmp", 2}, {"tcp", 6}, {"udp", 17}, {"raw", 255}, {NULL, 0}};

static struct sb_protoent *sb_proto_fill(struct SocketBase *base, ULONG idx)
{
    KprintfH("[bsdsocket] %s: proto=%ld\n", __func__, sb_protos[idx].proto);
    base->proto.p_name = (char *)sb_protos[idx].name;
    base->protoAliases[0] = NULL;
    base->proto.p_aliases = base->protoAliases;
    base->proto.p_proto = sb_protos[idx].proto;
    return &base->proto;
}

APTR bsd_getprotobyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    if (name != NULL)
    {
        for (ULONG i = 0; sb_protos[i].name != NULL; i++)
        {
            if (sb_strieq((const char *)name, sb_protos[i].name))
                return sb_proto_fill(base, i);
        }
    }
    return NULL;
}

APTR bsd_getprotobynumber(LONG proto asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: proto=%ld\n", __func__, proto);
    for (ULONG i = 0; sb_protos[i].name != NULL; i++)
    {
        if (sb_protos[i].proto == proto)
            return sb_proto_fill(base, i);
    }
    return NULL;
}

/* getprotoent iterator: the idx-th sb_protos entry, or NULL past the end.
 * Keeps get*ent walking the exact same table as getprotobyname/-number. */
struct sb_protoent *sb_proto_at(struct SocketBase *base, ULONG idx)
{
    KprintfH("[bsdsocket] %s: idx=%lu\n", __func__, idx);
    ULONG count = sizeof(sb_protos) / sizeof(sb_protos[0]) - 1; /* minus sentinel */
    if (idx >= count)
        return NULL;
    return sb_proto_fill(base, idx);
}
