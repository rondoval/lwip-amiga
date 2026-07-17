/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Second API wave: netdb services + iterators, scatter-gather I/O
 * (sendmsg/recvmsg), fd duplication, the ObtainSocket/ReleaseSocket task
 * handoff, syslog, address classification, and DNS-server configuration.
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#include <debug.h>

#include "netstack.h"

/* ------------------------------------------------------------ Dup2Socket --- */

LONG bsd_Dup2Socket(LONG oldSock asm("d0"), LONG newSock asm("d1"),
                    struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: old=%ld new=%ld\n", __func__, oldSock, newSock);
    struct SbSocket *s = sb_fd_get(base, oldSock);
    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }

    netstack_lock();
    if (newSock == -1)
    {
        newSock = sb_fd_alloc(base, s);
        if (newSock < 0)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EMFILE);
            return -1;
        }
    }
    else
    {
        if (newSock < 0 || newSock >= (LONG)base->fdCount)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EBADF);
            return -1;
        }
        if (base->fd[newSock] == s)
        {
            netstack_unlock();
            return newSock;
        }
        if (base->fd[newSock] != NULL)
            sb_sock_free(base, base->fd[newSock]);
        base->fd[newSock] = s;
    }
    s->refs++;
    sb_owner_incref(s, base); /* same base, extra fd: bumps its existing slot */
    netstack_unlock();
    return newSock;
}

/* -------------------------------------------------------- sendmsg/recvmsg --- */

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

/* -------------------------------------------------------------- syslog --- */

/* Minimal C-style formatter: bebbo varargs promote every integer to 32 bits,
 * so %d/%u/%x and their l-variants all read one long word; %s reads a
 * pointer. Anything else is copied through. */
static ULONG sb_vformat(char *dst, ULONG max, const char *fmt, const ULONG *args)
{
    KprintfH("[bsdsocket] %s: fmt=%s\n", __func__, (ULONG)fmt);
    ULONG o = 0;
    ULONG ai = 0;

    while (*fmt != '\0' && o < max - 1)
    {
        if (*fmt != '%')
        {
            dst[o++] = *fmt++;
            continue;
        }
        fmt++;

        /* %[-][0][width][.prec] — flags, then the length modifiers we accept
         * but do not act on (all varargs cells are promoted to 32-bit). */
        BOOL leftJust = FALSE;
        BOOL zeroPad = FALSE;
        while (*fmt == '-' || *fmt == '0')
        {
            if (*fmt == '-')
                leftJust = TRUE;
            else
                zeroPad = TRUE;
            fmt++;
        }
        LONG width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        LONG prec = -1;
        if (*fmt == '.')
        {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h')
            fmt++;

        /* Format the converted body into tmp, then emit it padded to width.
         * pad = '0' only for right-justified numeric conversions. */
        char tmp[24];
        const char *body = tmp;
        LONG blen = 0;
        char pad = (zeroPad && !leftJust) ? '0' : ' ';

        char conv = *fmt != '\0' ? *fmt++ : '\0';
        switch (conv)
        {
        case 's':
        {
            const char *sp = (const char *)args[ai++];
            if (sp == NULL)
                sp = "(null)";
            body = sp;
            while (sp[blen] != '\0' && (prec < 0 || blen < prec))
                blen++;
            pad = ' '; /* strings never zero-pad */
            break;
        }
        case 'c':
            tmp[blen++] = (char)args[ai++];
            break;
        case 'd':
        case 'i':
        {
            LONG v = (LONG)args[ai++];
            BOOL neg = v < 0;
            ULONG uv = neg ? (ULONG)(-v) : (ULONG)v;
            char digs[12];
            int t = 0;
            do
            {
                digs[t++] = (char)('0' + (uv % 10));
                uv /= 10;
            } while (uv != 0);
            if (neg)
                tmp[blen++] = '-';
            while (t > 0)
                tmp[blen++] = digs[--t];
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p':
        {
            ULONG v = args[ai++];
            ULONG radix = (conv == 'u') ? 10 : 16;
            char digs[12];
            int t = 0;
            do
            {
                ULONG digit = v % radix;
                digs[t++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
                v /= radix;
            } while (v != 0);
            while (t > 0)
                tmp[blen++] = digs[--t];
            break;
        }
        case '%':
            tmp[blen++] = '%';
            break;
        default:
            tmp[blen++] = '%';
            if (conv != '\0')
                tmp[blen++] = conv;
            break;
        }

        LONG padlen = width - blen;
        if (!leftJust)
        {
            /* zero-fill goes between the sign and the digits: "%05d" of -12
             * is "-0012", never "00-12" (the sign still counts toward width) */
            if (pad == '0' && blen > 0 && body[0] == '-' && o < max - 1)
            {
                dst[o++] = '-';
                body++;
                blen--;
            }
            while (padlen-- > 0 && o < max - 1)
                dst[o++] = pad;
        }
        for (LONG i = 0; i < blen && o < max - 1; i++)
            dst[o++] = body[i];
        if (leftJust)
            while (padlen-- > 0 && o < max - 1)
                dst[o++] = ' ';
    }
    dst[o] = '\0';
    return o;
}

VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"),
                 struct SocketBase *base asm("a6"))
{
    char buf[256];

    if (msg == NULL)
        return;
    /* drop priorities the opener masked off via SBTC_LOGMASK (setlogmask) */
    if (base != NULL && !(base->logMask & SB_LOG_MASK(pri)))
        return;
    sb_vformat(buf, sizeof(buf), (const char *)msg, args);
    if (base != NULL && base->logTagPtr != NULL)
        Kprintf("[syslog:%ld] %s: %s\n", pri, (ULONG)base->logTagPtr, (ULONG)buf);
    else
        Kprintf("[syslog:%ld] %s\n", pri, (ULONG)buf);
}

/* --------------------------------------- ObtainSocket / ReleaseSocket --- */

static struct SbReleased *sb_released_find(struct SocketBase *root, LONG id)
{
    KprintfH("[bsdsocket] %s: id=%ld\n", __func__, id);
    for (struct MinNode *n = root->releasedSockets.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
    {
        struct SbReleased *r = (struct SbReleased *)n;
        if (r->id == id)
            return r;
    }
    return NULL;
}

static LONG sb_release_common(struct SocketBase *base, LONG sock, LONG id, BOOL copy)
{
    KprintfH("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    struct SocketBase *root = SB_ROOT(base);
    struct SbSocket *s = sb_fd_get(base, sock);

    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }

    netstack_lock();
    if (id == -1 /* UNIQUE_ID */)
    {
        do
        {
            id = root->nextSockId++;
            if (root->nextSockId < 0)
                root->nextSockId = 1;
        } while (sb_released_find(root, id) != NULL);
    }
    else if (sb_released_find(root, id) != NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EADDRINUSE);
        return -1;
    }

    struct SbReleased *r = AllocPooled(root->sockPool, sizeof(struct SbReleased));
    if (r == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_ENOBUFS);
        return -1;
    }

    r->id = id;
    r->s = s;
    if (copy)
    {
        s->refs++; /* the releaser keeps its fd (and its owner slot) */
    }
    else
    {
        base->fd[sock] = NULL;
        /* the releaser hands its fd to the parking lot; drop its ownership but
         * keep refs (the park holds that reference). All owners gone ==
         * parked: nobody to wake until obtained. */
        sb_owner_decref(s, base);
    }
    AddTailMinList(&root->releasedSockets, &r->node);
    netstack_unlock();
    return id;
}

LONG bsd_ReleaseSocket(LONG sock asm("d0"), LONG id asm("d1"),
                       struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    return sb_release_common(base, sock, id, FALSE);
}

LONG bsd_ReleaseCopyOfSocket(LONG sock asm("d0"), LONG id asm("d1"),
                             struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    return sb_release_common(base, sock, id, TRUE);
}

LONG bsd_ObtainSocket(LONG id asm("d0"), LONG domain asm("d1"), LONG type asm("d2"),
                      LONG protocol asm("d3"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: id=%ld type=%ld\n", __func__, id, type);
    struct SocketBase *root = SB_ROOT(base);
    (void)domain;
    (void)protocol;

    netstack_lock();
    struct SbReleased *r = sb_released_find(root, id);
    if (r == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    struct SbSocket *s = r->s;
    if (type != 0)
    {
        LONG have = s->type == SBT_TCP ? SB_SOCK_STREAM
                    : s->type == SBT_UDP ? SB_SOCK_DGRAM
                                         : SB_SOCK_RAW;
        if (have != type)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EINVAL);
            return -1;
        }
    }

    LONG fd = sb_fd_alloc(base, s);
    if (fd < 0)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EMFILE);
        return -1;
    }
    /* wakeups now also target this opener; a copy keeps the releaser's slot,
     * so both are woken. The parked refcount transferred into this fd already,
     * so refs is not bumped here. */
    if (!sb_owner_incref(s, base))
    {
        base->fd[fd] = NULL;
        netstack_unlock();
        sb_set_errno(base, SB_ENOBUFS);
        return -1;
    }

    RemoveMinNode(&r->node);
    FreePooled(root->sockPool, r, sizeof(struct SbReleased));
    netstack_unlock();
    return fd;
}

/* --------------------------------------------------------------- netdb --- */

/* Built-in /etc/services equivalent (no file on this platform). Each row may
 * carry up to two aliases, matched by getservbyname and the getaddrinfo
 * service lookup. Ordered by port for readability. */
static const struct
{
    const char *name;
    UWORD port;
    const char *proto;
    const char *alias[2];
} sb_services[] = {
    {"echo", 7, "tcp", {0}},       {"echo", 7, "udp", {0}},
    {"discard", 9, "tcp", {"sink", "null"}},
    {"discard", 9, "udp", {"sink", "null"}},
    {"daytime", 13, "tcp", {0}},   {"daytime", 13, "udp", {0}},
    {"chargen", 19, "tcp", {"ttytst", "source"}},
    {"chargen", 19, "udp", {"ttytst", "source"}},
    {"ftp-data", 20, "tcp", {0}},  {"ftp", 21, "tcp", {0}},
    {"ssh", 22, "tcp", {0}},       {"telnet", 23, "tcp", {0}},
    {"smtp", 25, "tcp", {"mail"}},
    {"time", 37, "tcp", {"timserver"}},
    {"time", 37, "udp", {"timserver"}},
    {"whois", 43, "tcp", {"nicname"}},
    {"domain", 53, "udp", {"nameserver"}},
    {"domain", 53, "tcp", {"nameserver"}},
    {"tftp", 69, "udp", {0}},      {"gopher", 70, "tcp", {0}},
    {"finger", 79, "tcp", {0}},
    {"http", 80, "tcp", {"www", "www-http"}},
    {"pop3", 110, "tcp", {"postoffice"}},
    {"auth", 113, "tcp", {"ident", "tap"}},
    {"nntp", 119, "tcp", {"usenet"}},
    {"ntp", 123, "udp", {0}},
    {"imap", 143, "tcp", {"imap2"}},
    {"snmp", 161, "udp", {0}},     {"snmptrap", 162, "udp", {0}},
    {"irc", 194, "tcp", {0}},      {"ldap", 389, "tcp", {0}},
    {"https", 443, "tcp", {0}},
    {"smtps", 465, "tcp", {"urd", "submissions"}},
    {"shell", 514, "tcp", {"cmd"}},
    {"syslog", 514, "udp", {0}},
    {"printer", 515, "tcp", {"spooler"}},
    {"router", 520, "udp", {"route"}},
    {"submission", 587, "tcp", {0}}, {"ipp", 631, "tcp", {0}},
    {"rsync", 873, "tcp", {0}},    {"ftps", 990, "tcp", {0}},
    {"telnets", 992, "tcp", {0}},  {"imaps", 993, "tcp", {0}},
    {"pop3s", 995, "tcp", {0}},    {"socks", 1080, "tcp", {0}},
    {NULL, 0, NULL, {0}}};

/* match @name against a service row's canonical name or either alias */
static BOOL sb_serv_name_matches(ULONG idx, const char *name)
{
    if (sb_strieq(name, sb_services[idx].name))
        return TRUE;
    for (ULONG a = 0; a < 2 && sb_services[idx].alias[a] != NULL; a++)
        if (sb_strieq(name, sb_services[idx].alias[a]))
            return TRUE;
    return FALSE;
}

/* getaddrinfo/getnameinfo service lookups (sb_gai.c) — same table, no
 * per-base servent involved */
LONG sb_serv_port_by_name(const char *name, const char *proto)
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, (ULONG)name);
    for (ULONG i = 0; sb_services[i].name != NULL; i++)
    {
        if (!sb_serv_name_matches(i, name))
            continue;
        if (proto != NULL && !sb_strieq(proto, sb_services[i].proto))
            continue;
        return sb_services[i].port;
    }
    return -1;
}

const char *sb_serv_name_by_port(UWORD port, const char *proto)
{
    KprintfH("[bsdsocket] %s: port=%lu\n", __func__, (ULONG)port);
    for (ULONG i = 0; sb_services[i].name != NULL; i++)
    {
        if (sb_services[i].port != port)
            continue;
        if (proto != NULL && !sb_strieq(proto, sb_services[i].proto))
            continue;
        return sb_services[i].name;
    }
    return NULL;
}

static struct sb_servent *sb_serv_fill(struct SocketBase *base, ULONG idx)
{
    KprintfH("[bsdsocket] %s: name=%s port=%lu\n", __func__, (ULONG)sb_services[idx].name, (ULONG)sb_services[idx].port);
    base->serv.s_name = (char *)sb_services[idx].name;
    ULONG n = 0;
    for (ULONG a = 0; a < 2 && sb_services[idx].alias[a] != NULL; a++)
        base->servAliases[n++] = (char *)sb_services[idx].alias[a];
    base->servAliases[n] = NULL;
    base->serv.s_aliases = base->servAliases;
    base->serv.s_port = sb_services[idx].port;
    base->serv.s_proto = (char *)sb_services[idx].proto;
    return &base->serv;
}

APTR bsd_getservbyname(STRPTR name asm("a0"), STRPTR proto asm("a1"),
                       struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    if (name == NULL)
        return NULL;
    for (ULONG i = 0; sb_services[i].name != NULL; i++)
    {
        if (!sb_serv_name_matches(i, (const char *)name))
            continue;
        if (proto != NULL && !sb_strieq((const char *)proto, sb_services[i].proto))
            continue;
        return sb_serv_fill(base, i);
    }
    return NULL;
}

APTR bsd_getservbyport(LONG port asm("d0"), STRPTR proto asm("a0"),
                       struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: port=%lu\n", __func__, (ULONG)port);
    for (ULONG i = 0; sb_services[i].name != NULL; i++)
    {
        if (sb_services[i].port != (UWORD)port)
            continue;
        if (proto != NULL && !sb_strieq((const char *)proto, sb_services[i].proto))
            continue;
        return sb_serv_fill(base, i);
    }
    return NULL;
}

/* networks database: built-in /etc/networks equivalent (no file on this
 * platform) plus the user's NETWORK= entries from netstack.prefs (read-only
 * after stack startup, so no locking). Config entries are searched first —
 * they shadow the built-ins. n_net is the classful network number in host
 * order, per BSD. */
static const struct
{
    const char *name;
    ULONG net;
} sb_networks[] = {
    {"default", 0},
    {"loopback", 127},
    {NULL, 0}};

#define SB_NETWORKS_BUILTIN (sizeof(sb_networks) / sizeof(sb_networks[0]) - 1)

static struct sb_netent *sb_net_fill(struct SocketBase *base, const char *name, ULONG net)
{
    KprintfH("[bsdsocket] %s: name=%s net=%lu\n", __func__, (ULONG)name, net);
    base->net.n_name = (char *)name;
    base->netAliases[0] = NULL;
    base->net.n_aliases = base->netAliases;
    base->net.n_addrtype = SB_AF_INET;
    base->net.n_net = net;
    return &base->net;
}

APTR bsd_getnetbyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)");
    if (name == NULL)
        return NULL;
    const struct SbNetConfig *cfg = &SB_ROOT(base)->netCfg;
    for (ULONG i = 0; i < cfg->cfg_NumNetworks; i++)
    {
        if (sb_strieq((const char *)name, cfg->cfg_Networks[i].name))
            return sb_net_fill(base, cfg->cfg_Networks[i].name, cfg->cfg_Networks[i].net);
    }
    for (ULONG i = 0; sb_networks[i].name != NULL; i++)
    {
        if (sb_strieq((const char *)name, sb_networks[i].name))
            return sb_net_fill(base, sb_networks[i].name, sb_networks[i].net);
    }
    return NULL;
}

APTR bsd_getnetbyaddr(ULONG net asm("d0"), LONG type asm("d1"),
                      struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: net=%lu type=%ld\n", __func__, net, type);
    if (type != SB_AF_INET)
        return NULL;
    const struct SbNetConfig *cfg = &SB_ROOT(base)->netCfg;
    for (ULONG i = 0; i < cfg->cfg_NumNetworks; i++)
    {
        if (cfg->cfg_Networks[i].net == net)
            return sb_net_fill(base, cfg->cfg_Networks[i].name, cfg->cfg_Networks[i].net);
    }
    for (ULONG i = 0; sb_networks[i].name != NULL; i++)
    {
        if (sb_networks[i].net == net)
            return sb_net_fill(base, sb_networks[i].name, sb_networks[i].net);
    }
    return NULL;
}

VOID bsd_setnetent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    (void)stayOpen;
    base->netIdx = 0;
}

VOID bsd_endnetent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    base->netIdx = 0;
}

APTR bsd_getnetent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: idx=%lu\n", __func__, (ULONG)base->netIdx);
    /* config entries first, then the built-ins — same order lookups use */
    const struct SbNetConfig *cfg = &SB_ROOT(base)->netCfg;
    ULONG idx = base->netIdx;
    if (idx < cfg->cfg_NumNetworks)
    {
        base->netIdx++;
        return sb_net_fill(base, cfg->cfg_Networks[idx].name, cfg->cfg_Networks[idx].net);
    }
    idx -= cfg->cfg_NumNetworks;
    if (idx >= SB_NETWORKS_BUILTIN)
        return NULL;
    base->netIdx++;
    return sb_net_fill(base, sb_networks[idx].name, sb_networks[idx].net);
}

VOID bsd_setprotoent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    (void)stayOpen;
    base->protoIdx = 0;
}

VOID bsd_endprotoent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    base->protoIdx = 0;
}

APTR bsd_getprotoent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: idx=%lu\n", __func__, (ULONG)base->protoIdx);
    /* walk the same sb_protos table getprotobyname/-number use — no parallel
     * list to drift out of sync */
    struct sb_protoent *pe = sb_proto_at(base, base->protoIdx);
    if (pe != NULL)
        base->protoIdx++;
    return pe;
}

VOID bsd_setservent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    (void)stayOpen;
    base->servIdx = 0;
}

VOID bsd_endservent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    base->servIdx = 0;
}

APTR bsd_getservent(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: idx=%lu\n", __func__, (ULONG)base->servIdx);
    if (sb_services[base->servIdx].name == NULL)
        return NULL;
    return sb_serv_fill(base, base->servIdx++);
}

/* ------------------------------------------------- reentrant resolvers --- */

static struct sb_hostent *sb_host_to_caller(struct sb_hostent *src, struct sb_hostent *hp,
                                            char *buf, ULONG buflen)
{
    KprintfH("[bsdsocket] %s: buflen=%lu\n", __func__, buflen);
    /* layout in the caller's buffer: addr (4) + addr_list (8) + aliases (4)
     * + name string */
    ULONG need = 4 + 2 * sizeof(char *) + sizeof(char *) + 1;
    const char *name = src->h_name;
    ULONG namelen = 0;
    while (name[namelen] != '\0')
        namelen++;
    if (hp == NULL || buf == NULL || buflen < need + namelen)
        return NULL;

    char *addr = buf;
    char **addrList = (char **)(buf + 4);
    char **aliases = (char **)(buf + 4 + 2 * sizeof(char *));
    char *nameDst = buf + 4 + 3 * sizeof(char *);

    *(ULONG *)addr = *(ULONG *)src->h_addr_list[0];
    addrList[0] = addr;
    addrList[1] = NULL;
    aliases[0] = NULL;
    for (ULONG i = 0; i <= namelen; i++)
        nameDst[i] = name[i];

    hp->h_name = nameDst;
    hp->h_aliases = aliases;
    hp->h_addrtype = SB_AF_INET;
    hp->h_length = 4;
    hp->h_addr_list = addrList;
    return hp;
}

APTR bsd_gethostbyname_r(STRPTR name asm("a0"), APTR hp asm("a1"), APTR buf asm("a2"),
                         ULONG buflen asm("d0"), LONG *he asm("a3"),
                         struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s buflen=%lu\n", __func__, name != NULL ? (ULONG)name : (ULONG)"(null)", buflen);
    LONG herr = 0;
    struct sb_hostent *res = sb_host_resolve(base, (const char *)name, NULL, &herr);

    if (he != NULL)
        *he = herr;
    if (res == NULL)
        return NULL;
    return sb_host_to_caller(res, hp, buf, buflen);
}

APTR bsd_gethostbyaddr_r(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"),
                         APTR hp asm("a1"), APTR buf asm("a2"), ULONG buflen asm("d2"),
                         LONG *he asm("a3"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: len=%ld type=%ld\n", __func__, len, type);
    struct sb_hostent *res = (struct sb_hostent *)bsd_gethostbyaddr(addr, len, type, base);

    if (he != NULL)
        *he = res != NULL ? 0 : SB_HOST_NOT_FOUND;
    if (res == NULL)
        return NULL;
    return sb_host_to_caller(res, hp, buf, buflen);
}

/* --------------------------------------------- address classification --- */

LONG bsd_In_LocalAddr(ULONG address asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: addr=0x%08lx\n", __func__, address);
    (void)base;
    LONG local = 0;

    netstack_lock();
    struct netif *nif;
    NETIF_FOREACH(nif)
    {
        ULONG a = ip4_addr_get_u32(netif_ip4_addr(nif));
        ULONG m = ip4_addr_get_u32(netif_ip4_netmask(nif));
        if (a != 0 && (address & m) == (a & m))
        {
            local = 1;
            break;
        }
    }
    netstack_unlock();
    return local;
}

LONG bsd_In_CanForward(ULONG address asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: addr=0x%08lx\n", __func__, address);
    (void)base;
    /* 4.3BSD in_canforward(): a datagram to this address may be forwarded
     * unless it is class D (multicast) or class E (experimental) space, or a
     * class A datagram for net 0 or net 127 (loopback). The address arrives
     * as a network-order u32; on big-endian 68k that equals the host-order
     * integer used by the classful masks, so no byte swap is needed. */
    if ((address & 0xF0000000UL) == 0xE0000000UL || /* class D multicast */
        (address & 0xF0000000UL) == 0xF0000000UL)   /* class E / experimental */
        return 0;
    if ((address & 0x80000000UL) == 0) /* class A */
    {
        ULONG net = address & 0xFF000000UL;
        if (net == 0 || net == (127UL << 24))
            return 0;
    }
    return 1;
}

/* --------------------------------------------------- DNS configuration --- */

LONG bsd_AddDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: address=%s\n", __func__, address != NULL ? (ULONG)address : (ULONG)"(null)");
    ip_addr_t ip;

    if (address == NULL || ip4addr_aton((const char *)address, ip_2_ip4(&ip)) == 0)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    IP_SET_TYPE(&ip, IPADDR_TYPE_V4);

    LONG result = -1;
    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        const ip_addr_t *cur = dns_getserver(i);
        if (ip_addr_cmp(cur, &ip))
        {
            result = 0; /* already present */
            break;
        }
        if (ip_addr_isany(cur))
        {
            dns_setserver(i, &ip);
            result = 0;
            break;
        }
    }
    netstack_unlock();

    if (result != 0)
        sb_set_errno(base, SB_ENOBUFS);
    return result;
}

LONG bsd_RemoveDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: address=%s\n", __func__, address != NULL ? (ULONG)address : (ULONG)"(null)");
    ip_addr_t ip;

    if (address == NULL || ip4addr_aton((const char *)address, ip_2_ip4(&ip)) == 0)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    IP_SET_TYPE(&ip, IPADDR_TYPE_V4);

    LONG result = -1;
    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        if (ip_addr_cmp(dns_getserver(i), &ip))
        {
            dns_setserver(i, NULL);
            result = 0;
            break;
        }
    }
    netstack_unlock();

    if (result != 0)
        sb_set_errno(base, SB_EINVAL);
    return result;
}

struct sb_dns_list
{
    struct MinList list; /* of sb_DomainNameServerNode */
};

APTR bsd_ObtainDomainNameServerList(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    struct sb_dns_list *l = AllocVec(sizeof(struct sb_dns_list), MEMF_PUBLIC);
    if (l == NULL)
    {
        sb_set_errno(base, SB_ENOBUFS);
        return NULL;
    }
    _NewMinList(&l->list);

    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        const ip_addr_t *cur = dns_getserver(i);
        if (ip_addr_isany(cur))
            continue;

        struct sb_DomainNameServerNode *n =
            AllocVec(sizeof(*n) + 20, MEMF_PUBLIC | MEMF_CLEAR);
        if (n == NULL)
            break;
        n->dnsn_Size = sizeof(*n);
        n->dnsn_Address = (STRPTR)(n + 1);
        n->dnsn_UseCount = 1;
        ip4addr_ntoa_r(ip_2_ip4(cur), (char *)n->dnsn_Address, 20);
        AddTailMinList(&l->list, &n->dnsn_MinNode);
    }
    netstack_unlock();
    return l;
}

VOID bsd_ReleaseDomainNameServerList(APTR list asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: list=0x%08lx\n", __func__, (ULONG)list);
    struct sb_dns_list *l = list;
    (void)base;

    if (l == NULL)
        return;

    struct MinNode *n;
    while ((n = RemHeadMinList(&l->list)) != NULL)
        FreeVec(n);
    FreeVec(l);
}

/* ------------------------------------------------------ default domain --- */

LONG bsd_GetDefaultDomainName(STRPTR buffer asm("a0"), LONG bufferSize asm("d0"),
                              struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: bufferSize=%ld\n", __func__, bufferSize);
    struct SocketBase *root = SB_ROOT(base);

    if (buffer == NULL || bufferSize <= 0 || root->defaultDomain[0] == '\0')
        return 0; /* FALSE */

    LONG i = 0;
    for (; i < bufferSize - 1 && root->defaultDomain[i] != '\0'; i++)
        buffer[i] = root->defaultDomain[i];
    buffer[i] = '\0';
    return 1; /* TRUE */
}

VOID bsd_SetDefaultDomainName(STRPTR buffer asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, buffer != NULL ? (ULONG)buffer : (ULONG)"(null)");
    struct SocketBase *root = SB_ROOT(base);

    if (buffer == NULL)
    {
        root->defaultDomain[0] = '\0';
        return;
    }
    ULONG i = 0;
    for (; i < sizeof(root->defaultDomain) - 1 && buffer[i] != '\0'; i++)
        root->defaultDomain[i] = (char)buffer[i];
    root->defaultDomain[i] = '\0';
}
