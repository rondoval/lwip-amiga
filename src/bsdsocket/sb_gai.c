/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * getaddrinfo/freeaddrinfo/gai_strerror/getnameinfo — RFC 2553 subset for
 * IPv4 (netinclude/netdb.h ABI, register conventions from the NDK sfd).
 *
 * The whole result list lives in ONE AllocMem block with a hidden size
 * header before the first addrinfo, so freeaddrinfo needs the list head
 * only (standard usage). No CNAME chasing: AI_CANONNAME echoes the queried
 * name. getnameinfo does reverse DNS via sb_ptr_resolve (PTR query); when it
 * finds nothing the host falls back to numeric unless NI_NAMEREQD is set (then
 * EAI_NONAME). Service names come from the static services table.
 */

#include "sb_base.h"

#include <lwip/ip4_addr.h>

#include <debug.h>

/* hidden allocation header; 8 bytes keeps the addrinfos LONG-aligned */
struct SbGaiBlock
{
    ULONG size;
    ULONG pad;
};

static ULONG sb_gai_strlen(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* strict decimal port: returns -1 unless the whole string is 0..65535 */
static LONG sb_gai_parse_port(const char *s)
{
    if (s == NULL || *s == '\0')
        return -1;
    LONG v = 0;
    for (; *s != '\0'; s++)
    {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > 0xFFFF)
            return -1;
    }
    return v;
}

LONG bsd_getaddrinfo(STRPTR hostname asm("a0"), STRPTR servname asm("a1"),
                     struct sb_addrinfo *hints asm("a2"), struct sb_addrinfo **res asm("a3"),
                     struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: host=%s serv=%s\n", __func__,
             hostname != NULL ? (ULONG)hostname : (ULONG) "(null)",
             servname != NULL ? (ULONG)servname : (ULONG) "(null)");
    if (res == NULL)
        return SB_EAI_FAIL;
    *res = NULL;
    if (hostname == NULL && servname == NULL)
        return SB_EAI_NONAME;

    LONG flags = 0, family = SB_PF_UNSPEC, socktype = 0, protocol = 0;
    if (hints != NULL)
    {
        flags = hints->ai_flags;
        family = hints->ai_family;
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
        if (hints->ai_addrlen != 0 || hints->ai_addr != NULL ||
            hints->ai_canonname != NULL || hints->ai_next != NULL)
            return SB_EAI_BADHINTS;
    }
    if ((flags & ~(LONG)(SB_AI_MASK | SB_AI_EXT)) != 0)
        return SB_EAI_BADFLAGS;
    if (family != SB_PF_UNSPEC && family != SB_AF_INET)
        return SB_EAI_FAMILY;
    if (socktype != 0 && socktype != SB_SOCK_STREAM && socktype != SB_SOCK_DGRAM)
        return SB_EAI_SOCKTYPE;
    if (protocol == SB_IPPROTO_TCP)
    {
        if (socktype == SB_SOCK_DGRAM)
            return SB_EAI_BADHINTS;
        socktype = SB_SOCK_STREAM;
    }
    else if (protocol == SB_IPPROTO_UDP)
    {
        if (socktype == SB_SOCK_STREAM)
            return SB_EAI_BADHINTS;
        socktype = SB_SOCK_DGRAM;
    }
    else if (protocol != 0)
        return SB_EAI_PROTOCOL;

    /* candidate socket types, most-specific first */
    LONG types[2];
    ULONG ntypes = 0;
    if (socktype == 0 || socktype == SB_SOCK_STREAM)
        types[ntypes++] = SB_SOCK_STREAM;
    if (socktype == 0 || socktype == SB_SOCK_DGRAM)
        types[ntypes++] = SB_SOCK_DGRAM;

    /* service -> per-type port; a named service may exist for one proto
     * only, which then drops the other candidate type */
    LONG port[2] = { 0, 0 };
    if (servname != NULL)
    {
        LONG numeric = sb_gai_parse_port((const char *)servname);
        ULONG kept = 0;
        for (ULONG i = 0; i < ntypes; i++)
        {
            LONG p = numeric;
            if (p < 0)
            {
                if (flags & SB_AI_NUMERICSERV)
                    return SB_EAI_NONAME;
                p = sb_serv_port_by_name((const char *)servname,
                                         types[i] == SB_SOCK_STREAM ? "tcp" : "udp");
            }
            if (p < 0)
                continue;
            types[kept] = types[i];
            port[kept] = p;
            kept++;
        }
        if (kept == 0)
            return SB_EAI_SERVICE;
        ntypes = kept;
    }

    /* node -> one IPv4 address (network order) */
    ULONG addr;
    if (hostname == NULL)
    {
        addr = (flags & SB_AI_PASSIVE) ? 0UL /* INADDR_ANY */
                                       : lwip_htonl(0x7F000001UL);
    }
    else
    {
        ip4_addr_t ip;
        if (ip4addr_aton((const char *)hostname, &ip))
        {
            addr = ip4_addr_get_u32(&ip);
        }
        else
        {
            if (flags & SB_AI_NUMERICHOST)
                return SB_EAI_NONAME;
            LONG herr = 0;
            if (sb_host_resolve(base, (const char *)hostname, &addr, &herr) == NULL)
                return herr == SB_TRY_AGAIN ? SB_EAI_AGAIN : SB_EAI_NONAME;
        }
    }

    /* one block: header + addrinfos + sockaddrs + optional canonname */
    ULONG canonLen = 0;
    if ((flags & SB_AI_CANONNAME) && hostname != NULL)
        canonLen = sb_gai_strlen((const char *)hostname) + 1;
    ULONG total = sizeof(struct SbGaiBlock) +
                  ntypes * (sizeof(struct sb_addrinfo) + sizeof(struct sb_sockaddr_in)) +
                  canonLen;

    struct SbGaiBlock *blk = AllocMem(total, MEMF_PUBLIC | MEMF_CLEAR);
    if (blk == NULL)
        return SB_EAI_MEMORY;
    blk->size = total;

    struct sb_addrinfo *ai = (struct sb_addrinfo *)(blk + 1);
    struct sb_sockaddr_in *sa = (struct sb_sockaddr_in *)(ai + ntypes);
    char *canon = NULL;
    if (canonLen != 0)
    {
        canon = (char *)(sa + ntypes);
        const char *src = (const char *)hostname;
        for (ULONG i = 0; i < canonLen; i++)
            canon[i] = src[i];
    }

    for (ULONG i = 0; i < ntypes; i++)
    {
        sa[i].sin_len = sizeof(struct sb_sockaddr_in);
        sa[i].sin_family = SB_AF_INET;
        sa[i].sin_port = (UWORD)port[i]; /* network order == native */
        sa[i].sin_addr = addr;

        ai[i].ai_flags = flags;
        ai[i].ai_family = SB_AF_INET;
        ai[i].ai_socktype = types[i];
        ai[i].ai_protocol = types[i] == SB_SOCK_STREAM ? SB_IPPROTO_TCP : SB_IPPROTO_UDP;
        ai[i].ai_addrlen = sizeof(struct sb_sockaddr_in);
        ai[i].ai_addr = &sa[i];
        ai[i].ai_canonname = i == 0 ? canon : NULL;
        ai[i].ai_next = i + 1 < ntypes ? &ai[i + 1] : NULL;
    }

    *res = ai;
    return 0;
}

VOID bsd_freeaddrinfo(struct sb_addrinfo *ai asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: ai=0x%08lx\n", __func__, (ULONG)ai);
    (void)base;
    if (ai == NULL)
        return;
    /* must be the list head handed out by getaddrinfo */
    struct SbGaiBlock *blk = (struct SbGaiBlock *)ai - 1;
    FreeMem(blk, blk->size);
}

STRPTR bsd_gai_strerror(LONG errnum asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: errnum=%ld\n", __func__, errnum);
    (void)base;
    static const char *const msgs[] = {
        "no error",                                    /* 0 */
        "invalid value for ai_flags",                  /* EAI_BADFLAGS */
        "name or service is not known",                /* EAI_NONAME */
        "temporary failure in name resolution",        /* EAI_AGAIN */
        "non-recoverable failure in name resolution",  /* EAI_FAIL */
        "no address associated with name",             /* EAI_NODATA */
        "ai_family not supported",                     /* EAI_FAMILY */
        "ai_socktype not supported",                   /* EAI_SOCKTYPE */
        "service not supported for ai_socktype",       /* EAI_SERVICE */
        "address family for name not supported",       /* EAI_ADDRFAMILY */
        "memory allocation failure",                   /* EAI_MEMORY */
        "system error",                                /* EAI_SYSTEM */
        "invalid value for hints",                     /* EAI_BADHINTS */
        "resolved protocol is unknown",                /* EAI_PROTOCOL */
    };
    LONG idx = -errnum;
    if (idx < 0 || idx > 13)
        return (STRPTR) "unknown resolver error";
    return (STRPTR)msgs[idx];
}

LONG bsd_getnameinfo(APTR sa asm("a0"), ULONG salen asm("d0"),
                     STRPTR host asm("a1"), ULONG hostlen asm("d1"),
                     STRPTR serv asm("a2"), ULONG servlen asm("d2"),
                     ULONG flags asm("d3"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: sa=0x%08lx flags=0x%lx\n", __func__, (ULONG)sa, flags);
    const struct sb_sockaddr_in *sin = sa;

    if ((flags & ~(ULONG)(SB_NI_NUMERICHOST | SB_NI_NUMERICSERV | SB_NI_NOFQDN |
                          SB_NI_NAMEREQD | SB_NI_DGRAM | SB_NI_WITHSCOPEID)) != 0)
        return SB_EAI_BADFLAGS;
    if (sin == NULL || salen < 8 ||
        (sin->sin_family != SB_AF_INET && sin->sin_family != 0))
        return SB_EAI_FAMILY;

    BOOL wantHost = host != NULL && hostlen != 0;
    BOOL wantServ = serv != NULL && servlen != 0;
    if (!wantHost && !wantServ)
        return SB_EAI_NONAME;

    if (wantHost)
    {
        char nameBuf[SB_HOSTNAME_MAX];
        const char *name = NULL;
        if (!(flags & SB_NI_NUMERICHOST) &&
            sb_ptr_resolve(base, sin->sin_addr, nameBuf, sizeof(nameBuf)) == 0)
            name = nameBuf;

        char numBuf[IP4ADDR_STRLEN_MAX];
        if (name == NULL)
        {
            if (flags & SB_NI_NAMEREQD)
                return SB_EAI_NONAME;
            ip4_addr_t ip;
            ip4_addr_set_u32(&ip, sin->sin_addr);
            ip4addr_ntoa_r(&ip, numBuf, sizeof(numBuf));
            name = numBuf;
        }
        ULONG n = sb_gai_strlen(name);
        if (n + 1 > hostlen)
            return SB_EAI_MEMORY;
        CopyMem((APTR)name, host, n + 1);
    }

    if (wantServ)
    {
        UWORD portNum = sin->sin_port; /* network order == native */
        const char *name = NULL;
        if (!(flags & SB_NI_NUMERICSERV))
            name = sb_serv_name_by_port(portNum,
                                        (flags & SB_NI_DGRAM) ? "udp" : "tcp");
        char buf[8];
        if (name == NULL)
        {
            /* u16 decimal */
            char tmp[8];
            ULONG n = 0, v = portNum;
            do
            {
                tmp[n++] = (char)('0' + v % 10);
                v /= 10;
            } while (v != 0);
            ULONG i = 0;
            while (n > 0)
                buf[i++] = tmp[--n];
            buf[i] = '\0';
            name = buf;
        }
        ULONG n = sb_gai_strlen(name);
        if (n + 1 > servlen)
            return SB_EAI_MEMORY;
        CopyMem((APTR)name, serv, n + 1);
    }

    return 0;
}
