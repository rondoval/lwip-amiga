/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * errno plumbing, SocketBaseTagList, the inet_* utilities, gethostbyname
 * (lwIP DNS, blocking on the opener's signal) and the tiny netdb subset.
 */

#include "sb_base.h"

#include <utility/tagitem.h>

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>

#include <debug.h>

#include "netstack.h"

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
                if (*valp != 0 && base->task != NULL)
                    Signal(base->task, *valp);
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
        case SBTC_DTABLESIZE:
            if (isSet)
                return index; /* fixed in this implementation */
            *valp = SB_FD_COUNT;
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
                return index;
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
                return index;
            base->hErrnoPtr = (APTR)*valp != NULL ? (LONG *)*valp : &base->hErrno;
            break;
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
    ULONG a = bsd_inet_addr(cp, base);
    if (a == 0xFFFFFFFF)
        return 0xFFFFFFFF;
    return bsd_Inet_NetOf(a, base);
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

/* Blocking resolver core shared with gethostbyname_r: resolves @name, fills
 * the per-base hostent, and reports the address/h_errno through the out
 * parameters. Returns the per-base hostent or NULL. */
struct sb_hostent *sb_host_resolve(struct SocketBase *base, const char *name, ULONG *addrOut, LONG *herrOut)
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    LONG herr = 0;

    if (name == NULL)
    {
        if (herrOut != NULL)
            *herrOut = SB_HOST_NOT_FOUND;
        sb_set_herrno(base, SB_HOST_NOT_FOUND);
        return NULL;
    }

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
                sb_set_herrno(base, SB_TRY_AGAIN);
                if (herrOut != NULL)
                    *herrOut = SB_TRY_AGAIN;
                return NULL;
            }
        }
        r = base->dnsErr == 0 ? ERR_OK : ERR_VAL;
    }
    netstack_unlock();

    if (r != ERR_OK)
    {
        herr = base->dnsErr != 0 ? base->dnsErr : SB_HOST_NOT_FOUND;
        sb_set_herrno(base, herr);
        if (herrOut != NULL)
            *herrOut = herr;
        return NULL;
    }

    ULONG addr = ip4_addr_get_u32(ip_2_ip4(&base->dnsAddr));
    if (addrOut != NULL)
        *addrOut = addr;
    if (herrOut != NULL)
        *herrOut = 0;
    return sb_host_fill(base, name, addr);
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
    /* no reverse DNS yet: hand back the dotted quad as the name */
    if (addr == NULL || len != 4 || type != SB_AF_INET)
    {
        sb_set_herrno(base, SB_HOST_NOT_FOUND);
        return NULL;
    }
    ULONG a = *(ULONG *)addr;
    ip4_addr_t ip;
    ip4_addr_set_u32(&ip, a);
    ip4addr_ntoa_r(&ip, base->ntoaBuf, sizeof(base->ntoaBuf));
    return sb_host_fill(base, base->ntoaBuf, a);
}

LONG bsd_gethostname(STRPTR name asm("a0"), LONG namelen asm("d0"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: namelen=%ld\n", __func__, namelen);
    (void)base;
    static const char hostname[] = "amiga";
    if (name == NULL || namelen <= 0)
        return -1;
    ULONG i;
    for (i = 0; i < sizeof(hostname) && (LONG)i < namelen - 1; i++)
        name[i] = hostname[i];
    name[i] = '\0';
    return 0;
}

ULONG bsd_gethostid(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    (void)base;
    return 0;
}

/* getprotobyname/-number over a fixed table */
static const struct
{
    const char *name;
    LONG proto;
} sb_protos[] = {
    {"ip", 0}, {"icmp", 1}, {"tcp", 6}, {"udp", 17}, {"raw", 255}, {NULL, 0}};

static BOOL sb_strieq(const char *a, const char *b)
{
    // KprintfH("[bsdsocket] %s\n", __func__);
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return FALSE;
        a++;
        b++;
    }
    return *a == *b;
}

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
