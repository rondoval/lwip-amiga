/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The local netdb databases — protocols, services and networks — with their
 * get*by* lookups and set/get/end*ent iterators. All three are fixed tables
 * (no /etc files on this platform); the networks database additionally
 * serves the user's NETWORK= entries from netstack.prefs.
 */

#include "sb_base.h"

#include <debug.h>

/* ------------------------------------------------------------ protocols --- */

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
static struct sb_protoent *sb_proto_at(struct SocketBase *base, ULONG idx)
{
    KprintfH("[bsdsocket] %s: idx=%lu\n", __func__, idx);
    ULONG count = sizeof(sb_protos) / sizeof(sb_protos[0]) - 1; /* minus sentinel */
    if (idx >= count)
        return NULL;
    return sb_proto_fill(base, idx);
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

/* ------------------------------------------------------------- services --- */

/* Built-in /etc/services equivalent. Each row may carry up to two aliases,
 * matched by getservbyname and the getaddrinfo service lookup. Ordered by
 * port for readability. */
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

/* The one table search: row index of the first service matching @name
 * (canonical or alias) or — with @name NULL — @port. A non-NULL @proto
 * ("tcp"/"udp") filters. -1 when absent. */
static LONG sb_serv_find(const char *name, LONG port, const char *proto)
{
    for (ULONG i = 0; sb_services[i].name != NULL; i++)
    {
        if (name != NULL ? !sb_serv_name_matches(i, name)
                         : sb_services[i].port != (UWORD)port)
            continue;
        if (proto != NULL && !sb_strieq(proto, sb_services[i].proto))
            continue;
        return (LONG)i;
    }
    return -1;
}

/* getaddrinfo/getnameinfo service lookups (sb_gai.c) — same table, no
 * per-base servent involved */
LONG sb_serv_port_by_name(const char *name, const char *proto)
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, (ULONG)name);
    LONG idx = sb_serv_find(name, 0, proto);
    return idx < 0 ? -1 : sb_services[idx].port;
}

const char *sb_serv_name_by_port(UWORD port, const char *proto)
{
    KprintfH("[bsdsocket] %s: port=%lu\n", __func__, (ULONG)port);
    LONG idx = sb_serv_find(NULL, port, proto);
    return idx < 0 ? NULL : sb_services[idx].name;
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
    LONG idx = sb_serv_find((const char *)name, 0, (const char *)proto);
    return idx < 0 ? NULL : (APTR)sb_serv_fill(base, (ULONG)idx);
}

APTR bsd_getservbyport(LONG port asm("d0"), STRPTR proto asm("a0"),
                       struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: port=%lu\n", __func__, (ULONG)port);
    LONG idx = sb_serv_find(NULL, port, (const char *)proto);
    return idx < 0 ? NULL : (APTR)sb_serv_fill(base, (ULONG)idx);
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

/* ------------------------------------------------------------- networks --- */

/* Built-in /etc/networks equivalent plus the user's NETWORK= entries from
 * netstack.prefs (read-only after stack startup, so no locking). Config
 * entries are searched first — they shadow the built-ins. n_net is the
 * classful network number in host order, per BSD. */
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
