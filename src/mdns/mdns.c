/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mdns — resolve and watch .local names, and manage what this machine
 * advertises.
 *
 * Two halves. On the wire it is an ordinary multicast DNS client: it joins
 * 224.0.0.251, asks for <name>.local and prints the answers, or just listens
 * and shows what the LAN announces. Off the wire it talks to the stack's mDNS
 * control port (mdns_ctl.h) to list, add and withdraw the DNS-SD services
 * bsdsocket.library advertises for this Amiga.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/memory.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dos/dos.h> /* struct DateStamp */
#include <dos/rdargs.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h> /* bsdsocket.library inline glue */

#include <mdns_ctl.h>

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */

#define ARG_TEMPLATE "NAME,LISTEN/S,SECS/K/N,STATUS/S,ADD/K,PORT/K/N,INSTANCE/K,DEL/K/N"
enum
{
    ARG_NAME,
    ARG_LISTEN,
    ARG_SECS,
    ARG_STATUS,
    ARG_ADD,
    ARG_PORT,
    ARG_INSTANCE,
    ARG_DEL,
    ARG_COUNT
};

#define LISTEN_SECS 10 /* default for LISTEN with no SECS */

#define MDNS_GROUP    "224.0.0.251"
#define MDNS_PORT     5353
#define MDNS_QUERY_MS 2000 /* answers arrive in well under a second */

/* DNS record types we bother to name */
#define DNS_TYPE_A    1
#define DNS_TYPE_PTR  12
#define DNS_TYPE_TXT  16
#define DNS_TYPE_SRV  33
#define DNS_TYPE_AAAA 28

/* ------------------------------------------------------------- wire ------ */

/* Every read below is bounds-checked: this parses whatever the LAN sends. */

/* Decode a domain name at `off` into dst, following 0xC0 compression
 * pointers. Returns the offset just past the name as encoded AT `off`
 * (i.e. past the pointer, not past its target), or -1 on a malformed name. */
static long dns_name(const unsigned char *pkt, long len, long off, char *dst, long dstMax)
{
    long out = 0;
    long ret = -1;
    int hops = 0;

    while (off >= 0 && off < len)
    {
        unsigned char n = pkt[off];

        if ((n & 0xC0) == 0xC0)
        {
            if (off + 1 >= len || ++hops > 16) /* a pointer loop is hostile */
                return -1;
            if (ret < 0)
                ret = off + 2;
            off = ((long)(n & 0x3F) << 8) | pkt[off + 1];
            continue;
        }
        if (n == 0)
        {
            if (ret < 0)
                ret = off + 1;
            break;
        }
        if (off + 1 + n > len)
            return -1;
        if (out != 0 && out < dstMax - 1)
            dst[out++] = '.';
        for (unsigned char i = 0; i < n; i++)
        {
            if (out < dstMax - 1)
                dst[out++] = (char)pkt[off + 1 + i];
        }
        off += 1 + n;
    }

    dst[out] = '\0';
    return ret;
}

/* Encode "name.local" as QNAME labels into buf. Returns bytes written. */
static long dns_encode(unsigned char *buf, long max, const char *name)
{
    long out = 0;
    const char *p = name;

    while (*p != '\0')
    {
        const char *dot = p;
        while (*dot != '\0' && *dot != '.')
            dot++;
        long n = dot - p;
        if (n == 0 || n > 63 || out + n + 1 >= max)
            return -1;
        buf[out++] = (unsigned char)n;
        for (long i = 0; i < n; i++)
            buf[out++] = (unsigned char)p[i];
        p = (*dot == '.') ? dot + 1 : dot;
    }
    if (out + 1 >= max)
        return -1;
    buf[out++] = 0;
    return out;
}

static const char *dns_type_name(unsigned short type)
{
    switch (type)
    {
    case DNS_TYPE_A:
        return "A";
    case DNS_TYPE_PTR:
        return "PTR";
    case DNS_TYPE_TXT:
        return "TXT";
    case DNS_TYPE_SRV:
        return "SRV";
    case DNS_TYPE_AAAA:
        return "AAAA";
    default:
        return "?";
    }
}

static void print_ip(unsigned long addr)
{
    printf("%lu.%lu.%lu.%lu", (addr >> 24) & 0xff, (addr >> 16) & 0xff, (addr >> 8) & 0xff,
           addr & 0xff);
}

/* Print one packet's answer section. `want` non-NULL prints only records whose
 * name matches it (query mode); NULL prints everything (monitor mode). */
static void print_answers(const unsigned char *pkt, long len, struct sockaddr_in *from,
                          const char *want)
{
    if (len < 12)
        return;

    unsigned short qd = (unsigned short)((pkt[4] << 8) | pkt[5]);
    unsigned short an = (unsigned short)((pkt[6] << 8) | pkt[7]);
    an = (unsigned short)(an + ((pkt[8] << 8) | pkt[9]));   /* authority */
    an = (unsigned short)(an + ((pkt[10] << 8) | pkt[11])); /* additional */
    if (an == 0)
        return;

    char name[256];
    long off = 12;

    for (unsigned short i = 0; i < qd; i++)
    {
        off = dns_name(pkt, len, off, name, sizeof(name));
        if (off < 0 || off + 4 > len)
            return;
        off += 4;
    }

    for (unsigned short i = 0; i < an; i++)
    {
        off = dns_name(pkt, len, off, name, sizeof(name));
        if (off < 0 || off + 10 > len)
            return;

        unsigned short type = (unsigned short)((pkt[off] << 8) | pkt[off + 1]);
        unsigned long ttl = ((unsigned long)pkt[off + 4] << 24) |
                            ((unsigned long)pkt[off + 5] << 16) |
                            ((unsigned long)pkt[off + 6] << 8) | pkt[off + 7];
        unsigned short rdlen = (unsigned short)((pkt[off + 8] << 8) | pkt[off + 9]);
        long rd = off + 10;
        if (rd + rdlen > len)
            return;
        off = rd + rdlen;

        if (want != NULL && stricmp(name, want) != 0)
            continue;

        printf("%-40s %-5s ", name, dns_type_name(type));
        if (type == DNS_TYPE_A && rdlen == 4)
        {
            print_ip(((unsigned long)pkt[rd] << 24) | ((unsigned long)pkt[rd + 1] << 16) |
                     ((unsigned long)pkt[rd + 2] << 8) | pkt[rd + 3]);
        }
        else if (type == DNS_TYPE_PTR)
        {
            char target[256];
            if (dns_name(pkt, len, rd, target, sizeof(target)) > 0)
                printf("%s", target);
        }
        else if (type == DNS_TYPE_SRV && rdlen >= 7)
        {
            char target[256];
            unsigned short port = (unsigned short)((pkt[rd + 4] << 8) | pkt[rd + 5]);
            if (dns_name(pkt, len, rd + 6, target, sizeof(target)) > 0)
                printf("%s port %lu", target, (unsigned long)port);
        }
        else if (type == DNS_TYPE_TXT)
        {
            long p = rd;
            while (p < rd + rdlen)
            {
                unsigned char n = pkt[p];
                if (p + 1 + n > rd + rdlen)
                    break;
                printf("%.*s ", (int)n, (const char *)&pkt[p + 1]);
                p += 1 + n;
            }
        }
        else
        {
            printf("(%lu bytes)", (unsigned long)rdlen);
        }

        printf("  ttl %lu  from ", ttl);
        print_ip(ntohl(from->sin_addr.s_addr));
        printf("\n");
    }
}

#define TICKS_PER_SEC 50 /* DateStamp ds_Tick resolution */
#define TICKS_PER_DAY (24UL * 60UL * 60UL * TICKS_PER_SEC)

/* Ticks since `start`, where both come from the time-of-day part of DateStamp
 * — so one midnight rollover is folded back. Counting WaitSelect timeouts
 * instead would never expire on a busy group, where every wait returns early
 * with a packet. */
static ULONG now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * (60UL * TICKS_PER_SEC) + (ULONG)ds.ds_Tick;
}

static ULONG elapsed_ticks(ULONG start)
{
    ULONG now = now_ticks();
    return now >= start ? now - start : now + TICKS_PER_DAY - start;
}

/* Socket bound to 5353 and joined to the mDNS group. SO_REUSEADDR is what
 * lets this coexist with the stack's own responder on the same port. */
static LONG mdns_socket(void)
{
    LONG s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
    {
        fprintf(stderr, "mdns: socket failed (errno %ld)\n", (long)Errno());
        return -1;
    }

    LONG on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "mdns: cannot bind port %d (errno %ld)\n", MDNS_PORT,
                (long)Errno());
        CloseSocket(s);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr((STRPTR)MDNS_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        fprintf(stderr, "mdns: cannot join %s (errno %ld)\n", MDNS_GROUP, (long)Errno());
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* Receive and print until the deadline passes or Ctrl-C arrives. */
static void mdns_listen(LONG s, ULONG secs, const char *want)
{
    unsigned char pkt[1500];
    ULONG start = now_ticks();

    while (elapsed_ticks(start) < secs * TICKS_PER_SEC)
    {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_micro = 0;

        LONG n = WaitSelect(s + 1, &rd, NULL, NULL, &tv, NULL);
        if (n < 0)
        {
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
                break;
            fprintf(stderr, "mdns: WaitSelect errno %ld\n", (long)Errno());
            break;
        }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            break;
        if (n == 0)
            continue;

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        LONG got = recvfrom(s, (UBYTE *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from,
                            &fromLen);
        if (got > 0)
            print_answers(pkt, got, &from, want);
    }
}

static int mdns_query(const char *host)
{
    char qname[256];
    if (strchr(host, '.') == NULL)
        snprintf(qname, sizeof(qname), "%s.local", host);
    else
        strncpy(qname, host, sizeof(qname) - 1), qname[sizeof(qname) - 1] = '\0';

    LONG s = mdns_socket();
    if (s < 0)
        return 20;

    unsigned char pkt[512];
    memset(pkt, 0, 12); /* id 0, no flags: a standard multicast query */
    pkt[5] = 1;         /* qdcount = 1 */
    long len = dns_encode(&pkt[12], (long)sizeof(pkt) - 12, qname);
    if (len < 0)
    {
        fprintf(stderr, "mdns: name too long\n");
        CloseSocket(s);
        return 20;
    }
    len += 12;
    pkt[len++] = 0;
    pkt[len++] = DNS_TYPE_A;
    pkt[len++] = 0;
    pkt[len++] = 1; /* class IN, multicast response */

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    to.sin_addr.s_addr = inet_addr((STRPTR)MDNS_GROUP);
    if (sendto(s, (UBYTE *)pkt, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
    {
        fprintf(stderr, "mdns: sendto failed (errno %ld)\n", (long)Errno());
        CloseSocket(s);
        return 20;
    }

    printf("querying %s\n", qname);
    mdns_listen(s, MDNS_QUERY_MS / 1000, qname);
    CloseSocket(s);
    return 0;
}

/* ---------------------------------------------------------- control ------ */

static const char *ctl_error(LONG result)
{
    switch (result)
    {
    case MDNSCTL_ERR_VERSION:
        return "control protocol mismatch — tool and library differ";
    case MDNSCTL_ERR_OP:
        return "unsupported operation";
    case MDNSCTL_ERR_PARAM:
        return "bad service type, port or slot";
    case MDNSCTL_ERR_FULL:
        return "no free service slot";
    case MDNSCTL_ERR_INACTIVE:
        return "responder not running";
    default:
        return "unknown error";
    }
}

/* One request/reply round trip with the stack task. */
static LONG ctl_call(struct MdnsCtlMsg *msg, UWORD op)
{
    struct MsgPort *reply = CreateMsgPort();
    if (reply == NULL)
    {
        fprintf(stderr, "mdns: out of memory\n");
        return MDNSCTL_ERR_OP;
    }

    msg->mcm_Msg.mn_Node.ln_Type = NT_MESSAGE;
    msg->mcm_Msg.mn_Length = sizeof(*msg);
    msg->mcm_Msg.mn_ReplyPort = reply;
    msg->mcm_Version = MDNSCTL_VERSION;
    msg->mcm_Op = op;
    msg->mcm_Result = MDNSCTL_ERR_INACTIVE;

    /* Forbid() spans find+send so the port cannot be withdrawn between them. */
    Forbid();
    struct MsgPort *port = FindPort((CONST_STRPTR)MDNSCTL_PORT_NAME);
    if (port != NULL)
        PutMsg(port, &msg->mcm_Msg);
    Permit();

    if (port == NULL)
    {
        fprintf(stderr, "mdns: %s is not published — is the stack up with MDNS=yes?\n",
                MDNSCTL_PORT_NAME);
        DeleteMsgPort(reply);
        return MDNSCTL_ERR_INACTIVE;
    }

    WaitPort(reply);
    GetMsg(reply);
    DeleteMsgPort(reply);
    return msg->mcm_Result;
}

static int ctl_status(void)
{
    struct MdnsCtlMsg msg;
    memset(&msg, 0, sizeof(msg));

    LONG res = ctl_call(&msg, MDNSCTL_OP_LIST);
    if (res != MDNSCTL_OK)
    {
        if (res != MDNSCTL_ERR_INACTIVE)
            fprintf(stderr, "mdns: %s\n", ctl_error(res));
        return 10;
    }

    printf("advertising %s.local\n", msg.mcm_Host);
    if (msg.mcm_Count == 0)
    {
        printf("no services\n");
        return 0;
    }
    printf("slot  port   service                          name\n");
    for (UWORD i = 0; i < msg.mcm_Count; i++)
    {
        const struct MdnsCtlService *s = &msg.mcm_List[i];
        printf("%4ld  %5lu  %-32s %s\n", (long)s->mcs_Slot, (unsigned long)s->mcs_Port,
               s->mcs_Type, s->mcs_Name);
    }
    return 0;
}

static int ctl_add(const char *type, LONG port, const char *name)
{
    struct MdnsCtlMsg msg;
    memset(&msg, 0, sizeof(msg));

    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "mdns: bad port %ld\n", (long)port);
        return 20;
    }
    strncpy(msg.mcm_Service.mcs_Type, type, MDNSCTL_TYPE_MAX - 1);
    if (name != NULL)
        strncpy(msg.mcm_Service.mcs_Name, name, MDNSCTL_NAME_MAX - 1);
    msg.mcm_Service.mcs_Port = (UWORD)port;

    LONG res = ctl_call(&msg, MDNSCTL_OP_ADD);
    if (res != MDNSCTL_OK)
    {
        if (res != MDNSCTL_ERR_INACTIVE)
            fprintf(stderr, "mdns: %s\n", ctl_error(res));
        return 10;
    }
    printf("advertising %s on port %ld (slot %ld)\n", type, (long)port,
           (long)msg.mcm_Service.mcs_Slot);
    return 0;
}

static int ctl_del(LONG slot)
{
    struct MdnsCtlMsg msg;
    memset(&msg, 0, sizeof(msg));

    if (slot < 0 || slot >= MDNSCTL_MAX_SERVICES)
    {
        fprintf(stderr, "mdns: bad slot %ld\n", (long)slot);
        return 20;
    }
    msg.mcm_Service.mcs_Slot = (BYTE)slot;

    LONG res = ctl_call(&msg, MDNSCTL_OP_DEL);
    if (res != MDNSCTL_OK)
    {
        if (res != MDNSCTL_ERR_INACTIVE)
            fprintf(stderr, "mdns: %s\n", ctl_error(res));
        return 10;
    }
    printf("withdrew slot %ld\n", (long)slot);
    return 0;
}

/* ------------------------------------------------------------- main ------ */

static int mdns_watch(ULONG secs)
{
    LONG s = mdns_socket();
    if (s < 0)
        return 20;

    printf("listening %lu s (Ctrl-C to stop)\n", (unsigned long)secs);
    mdns_listen(s, secs, NULL);
    CloseSocket(s);
    return 0;
}

static void usage(void)
{
    printf("mdns — multicast DNS for the Amiga\n"
           "\n"
           "  mdns <name>                    resolve <name>.local on the LAN\n"
           "  mdns LISTEN [SECS <n>]         watch mDNS announcements (default %d s)\n"
           "  mdns STATUS                    what this machine advertises\n"
           "  mdns ADD <_type._proto> PORT <n> [INSTANCE <name>]\n"
           "                                 advertise a service until the stack stops\n"
           "  mdns DEL <slot>                withdraw an advertised service\n"
           "\n"
           "Template: " ARG_TEMPLATE "\n"
           "Boot-time services come from ENVARC:netstack.prefs (MDNS_SERVICE).\n",
           LISTEN_SECS);
}

int main(void)
{
    LONG argvals[ARG_COUNT] = {0};
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, argvals, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "mdns");
        return 10;
    }

    /* the strings point into rda's buffers: FreeArgs only at exit */
    const char *name = (const char *)argvals[ARG_NAME];
    const char *type = (const char *)argvals[ARG_ADD];
    const char *instance = (const char *)argvals[ARG_INSTANCE];

    int modes = (name != NULL) + (argvals[ARG_LISTEN] != 0) + (argvals[ARG_STATUS] != 0) +
                (type != NULL) + (argvals[ARG_DEL] != 0);
    if (modes == 0)
    {
        usage();
        FreeArgs(rda);
        return 5;
    }

    int rc = 10;
    if (modes > 1)
    {
        fprintf(stderr, "mdns: pick one of <name>, LISTEN, STATUS, ADD or DEL\n");
        goto out_args;
    }
    if (argvals[ARG_SECS] != 0 &&
        (argvals[ARG_LISTEN] == 0 || *(LONG *)argvals[ARG_SECS] < 1))
    {
        fprintf(stderr, "mdns: SECS applies to LISTEN only, and must be >= 1\n");
        goto out_args;
    }
    if ((argvals[ARG_PORT] != 0 || instance != NULL) && type == NULL)
    {
        fprintf(stderr, "mdns: PORT and INSTANCE only apply to ADD\n");
        goto out_args;
    }
    if (type != NULL && argvals[ARG_PORT] == 0)
    {
        fprintf(stderr, "mdns: ADD needs PORT <n>\n");
        goto out_args;
    }

    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        fprintf(stderr, "mdns: cannot open bsdsocket.library\n");
        rc = 20;
        goto out_args;
    }

    if (name != NULL)
        rc = mdns_query(name);
    else if (argvals[ARG_LISTEN] != 0)
        rc = mdns_watch(argvals[ARG_SECS] ? (ULONG)*(LONG *)argvals[ARG_SECS] : LISTEN_SECS);
    else if (argvals[ARG_STATUS] != 0)
        rc = ctl_status();
    else if (type != NULL)
        rc = ctl_add(type, *(LONG *)argvals[ARG_PORT], instance);
    else
        rc = ctl_del(*(LONG *)argvals[ARG_DEL]);

    CloseLibrary(SocketBase);
out_args:
    FreeArgs(rda);
    return rc;
}
