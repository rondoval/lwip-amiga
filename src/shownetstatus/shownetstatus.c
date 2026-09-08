/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ShowNetStatus — display information about the status of the network
 * configuration, Roadshow-style. Template:
 *   INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S,DNS=DOMAINNAMESERVERS/S,
 *   ICMP/S,IGMP/S,IP/S,MB=MEMORY/S,MR=MULTICASTROUTING/S,RT=ROUTING/S,TCP/S,
 *   UDP/S,TCPSOCKETS/S,UDPSOCKETS/S,NAMES/S,ALL/S,REPEAT/S,QUIET/S
 *
 * Works like a combination of "route", "ifconfig" and "netstat": with no
 * options a general summary; otherwise the selected sections. Fresh
 * implementation of the Roadshow command of the same name (behavior and
 * output layout per the NDK's ShowNetStatus.doc); English-only.
 *
 * The protocol statistics come from GetNetworkStatistics(); the stat struct
 * layouts below are transcribed from the bsdsocket.doc autodoc (the same
 * transcription the library serves) so the offsets match the wire format.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include <dos/dos.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/route.h>
#include <net/if_arp.h>
#include <net/if_arp_ioctl.h>
/* GetNetworkStatistics result structs, as in the genuine tool; icmp_var.h
 * brings ip_icmp.h (ICMP_*), route.h above brings struct rtstat */
#include <netinet/icmp_var.h>
#include <netinet/igmp_var.h>
#include <netinet/ip_mroute.h>
#include <netinet/ip_var.h>
#include <netinet/tcp_var.h>
#include <netinet/udp_var.h>
#include <netinet/tcp_fsm.h> /* TCP_NSTATES */
#include <sys/mbuf.h>
#include <libraries/bsdsocket.h>

#include <proto/socket.h> /* bsdsocket.library inline glue */

struct Library *SocketBase;

/* $VER: cookie appended to the ReadArgs template (the leading NUL terminates
 * the template string). TOOL_VERSION/TOOL_DATE come from the build. */
#define VERSTAG "\0$VER: ShowNetStatus " TOOL_VERSION " " TOOL_DATE

#define ARG_TEMPLATE                                                        \
    "INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S,"                     \
    "DNS=DOMAINNAMESERVERS/S,ICMP/S,IGMP/S,IP/S,MB=MEMORY/S,"               \
    "MR=MULTICASTROUTING/S,RT=ROUTING/S,TCP/S,UDP/S,TCPSOCKETS/S,"          \
    "UDPSOCKETS/S,NAMES/S,ALL/S,REPEAT/S,QUIET/S" VERSTAG
enum
{
    ARG_INTERFACE,
    ARG_INTERFACES,
    ARG_ARP,
    ARG_ROUTES,
    ARG_DNS,
    ARG_ICMP,
    ARG_IGMP,
    ARG_IP,
    ARG_MB,
    ARG_MR,
    ARG_RT,
    ARG_TCP,
    ARG_UDP,
    ARG_TCPSOCKETS,
    ARG_UDPSOCKETS,
    ARG_NAMES,
    ARG_ALL,
    ARG_REPEAT,
    ARG_QUIET,
    ARG_COUNT
};

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif
#define LINE_LEN (MAXHOSTNAMELEN + 32) /* room for a ":port" suffix */

static BOOL opt_names, opt_all, opt_quiet;
static LONG arp_socket = -1; /* scratch UDP socket for SIOCGARPT */

/* console emphasis, enabled only when Output() is interactive */
static const char *ul_on = "", *ul_off = "";

/* one blank line between sections; reset for every REPEAT screen */
static BOOL something;
static void section_gap(void)
{
    if (something)
        printf("\n");
    else
        something = TRUE;
}

/* --- statistics struct layouts: the NDK's netinet <proto>_var.h headers,
 * net/route.h and sys/mbuf.h, as in the genuine tool. The library (sb_netstat.c) cannot
 * include those headers and mirrors them instead; both sides pin the same
 * wire sizes, so drift on either side breaks a build. (m68k only: host
 * IntelliSense sees a 64-bit __LONG.) ------------------------------------- */

#ifndef __INTELLISENSE__
_Static_assert(sizeof(struct icmpstat) == 184, "icmpstat wire ABI");
_Static_assert(sizeof(struct igmpstat) == 36, "igmpstat wire ABI");
_Static_assert(sizeof(struct ipstat) == 96, "ipstat wire ABI");
_Static_assert(sizeof(struct mbstat) == 28, "mbstat wire ABI");
_Static_assert(sizeof(struct mrtstat) == 32, "mrtstat wire ABI");
_Static_assert(sizeof(struct rtstat) == 10, "rtstat wire ABI");
_Static_assert(sizeof(struct tcpstat) == 208, "tcpstat wire ABI");
_Static_assert(sizeof(struct udpstat) == 36, "udpstat wire ABI");
_Static_assert(sizeof(struct protocol_connection_data) == 24,
               "protocol_connection_data wire ABI");
#endif

/* --- data-driven stat printing (labels/layout per the genuine tool) ------- */

struct stat_field
{
    const char *fmt; /* full line with one %ld */
    UWORD off;
    UBYTE half; /* TRUE: 16-bit WORD field (struct rtstat) */
};

#define FLD(str, fmt, field) \
    {fmt, (UWORD)offsetof(struct str, field), FALSE}
#define FLDH(str, fmt, field) \
    {fmt, (UWORD)offsetof(struct str, field), TRUE}

#define ICPS(fmt, field) FLD(icmpstat, fmt, field)
static const struct stat_field icmp_fields[] = {
    ICPS("Errors generated                           = %ld\n", icps_error),
    ICPS("IP packets too short                       = %ld\n", icps_oldshort),
    ICPS("Packets not responded to                   = %ld\n", icps_oldicmp),
    ICPS("'Echo reply' packets sent                  = %ld\n", icps_outhist[ICMP_ECHOREPLY]),
    ICPS("'Destination unreachable' packets sent     = %ld\n", icps_outhist[ICMP_UNREACH]),
    ICPS("'Source quench' packets sent               = %ld\n", icps_outhist[ICMP_SOURCEQUENCH]),
    ICPS("'Redirect' packets sent                    = %ld\n", icps_outhist[ICMP_REDIRECT]),
    ICPS("'Echo' packets sent                        = %ld\n", icps_outhist[ICMP_ECHO]),
    ICPS("'Router advertizement' packets sent        = %ld\n", icps_outhist[ICMP_ROUTERADVERT]),
    ICPS("'Router solicitation' packets sent         = %ld\n", icps_outhist[ICMP_ROUTERSOLICIT]),
    ICPS("'Time exceeded' packets sent               = %ld\n", icps_outhist[ICMP_TIMXCEED]),
    ICPS("'Bad IP header' packets sent               = %ld\n", icps_outhist[ICMP_PARAMPROB]),
    ICPS("'Timestamp request' packets sent           = %ld\n", icps_outhist[ICMP_TSTAMP]),
    ICPS("'Timestamp reply' packets sent             = %ld\n", icps_outhist[ICMP_TSTAMPREPLY]),
    ICPS("'Information request' packets sent         = %ld\n", icps_outhist[ICMP_IREQ]),
    ICPS("'Information reply' packets sent           = %ld\n", icps_outhist[ICMP_IREQREPLY]),
    ICPS("'Address mask request' packets sent        = %ld\n", icps_outhist[ICMP_MASKREQ]),
    ICPS("'Address mask reply' packets sent          = %ld\n", icps_outhist[ICMP_MASKREPLY]),
    ICPS("Received codes out of range                = %ld\n", icps_badcode),
    ICPS("Received packets too short                 = %ld\n", icps_tooshort),
    ICPS("Received packet checksum errors            = %ld\n", icps_checksum),
    ICPS("Received codes bound mismatch              = %ld\n", icps_badlen),
    ICPS("Responses sent                             = %ld\n", icps_reflect),
    ICPS("'Echo reply' packets received              = %ld\n", icps_inhist[ICMP_ECHOREPLY]),
    ICPS("'Destination unreachable' packets received = %ld\n", icps_inhist[ICMP_UNREACH]),
    ICPS("'Source quench' packets received           = %ld\n", icps_inhist[ICMP_SOURCEQUENCH]),
    ICPS("'Redirect' packets received                = %ld\n", icps_inhist[ICMP_REDIRECT]),
    ICPS("'Echo' packets received                    = %ld\n", icps_inhist[ICMP_ECHO]),
    ICPS("'Router advertizement' packets received    = %ld\n", icps_inhist[ICMP_ROUTERADVERT]),
    ICPS("'Router solicitation' packets received     = %ld\n", icps_inhist[ICMP_ROUTERSOLICIT]),
    ICPS("'Time exceeded' packets received           = %ld\n", icps_inhist[ICMP_TIMXCEED]),
    ICPS("'Bad IP header' packets received           = %ld\n", icps_inhist[ICMP_PARAMPROB]),
    ICPS("'Timestamp request' packets received       = %ld\n", icps_inhist[ICMP_TSTAMP]),
    ICPS("'Timestamp reply' packets received         = %ld\n", icps_inhist[ICMP_TSTAMPREPLY]),
    ICPS("'Information request' packets received     = %ld\n", icps_inhist[ICMP_IREQ]),
    ICPS("'Information reply' packets received       = %ld\n", icps_inhist[ICMP_IREQREPLY]),
    ICPS("'Address mask request' packets received    = %ld\n", icps_inhist[ICMP_MASKREQ]),
    ICPS("'Address mask reply' packets received      = %ld\n", icps_inhist[ICMP_MASKREPLY]),
};

#define IGPS(fmt, field) FLD(igmpstat, fmt, field)
static const struct stat_field igmp_fields[] = {
    IGPS("Total messages received                   = %ld\n", igps_rcv_total),
    IGPS("Messages received with too few bytes      = %ld\n", igps_rcv_tooshort),
    IGPS("Messages received with bad checksums      = %ld\n", igps_rcv_badsum),
    IGPS("Membership queries received               = %ld\n", igps_rcv_queries),
    IGPS("Messages received with invalid queries    = %ld\n", igps_rcv_badqueries),
    IGPS("Membership reports received               = %ld\n", igps_rcv_reports),
    IGPS("Messages received with invalid reports    = %ld\n", igps_rcv_badreports),
    IGPS("Membership reports received for our group = %ld\n", igps_rcv_ourreports),
    IGPS("Membership reports sent                   = %ld\n", igps_snd_reports),
};

#define IPS(fmt, field) FLD(ipstat, fmt, field)
static const struct stat_field ip_fields[] = {
    IPS("Total packets received                       = %ld\n", ips_total),
    IPS("Packets with checksum errors                 = %ld\n", ips_badsum),
    IPS("Packets shorter than expected                = %ld\n", ips_tooshort),
    IPS("Packets with not enough data                 = %ld\n", ips_toosmall),
    IPS("Packets with bad data size                   = %ld\n", ips_badhlen),
    IPS("Packets with bad header size                 = %ld\n", ips_badlen),
    IPS("Fragments received                           = %ld\n", ips_fragments),
    IPS("Fragments dropped                            = %ld\n", ips_fragdropped),
    IPS("Fragments timed out                          = %ld\n", ips_fragtimeout),
    IPS("Packets forwarded                            = %ld\n", ips_forward),
    IPS("Packets received for unreachable destination = %ld\n", ips_cantforward),
    IPS("Packets forwarded on same network            = %ld\n", ips_redirectsent),
    IPS("Packets with unknown protocols               = %ld\n", ips_noproto),
    IPS("Datagrams delivered                          = %ld\n", ips_delivered),
    IPS("Packets generated                            = %ld\n", ips_localout),
    IPS("Packets lost                                 = %ld\n", ips_odropped),
    IPS("Packets reassembled                          = %ld\n", ips_reassembled),
    IPS("Packets fragmented                           = %ld\n", ips_fragmented),
    IPS("Output fragments created                     = %ld\n", ips_ofragments),
    IPS("Packets that could not be fragmented         = %ld\n", ips_cantfrag),
    IPS("Errors in option processing                  = %ld\n", ips_badoptions),
    IPS("Packets discarded due to no route            = %ld\n", ips_noroute),
    IPS("Packets with IP version other than 4         = %ld\n", ips_badvers),
    IPS("Raw IP packets sent                          = %ld\n", ips_rawout),
};

#define MBS(fmt, field) FLD(mbstat, fmt, field)
static const struct stat_field mb_fields[] = {
    MBS("Buffers obtained from page pool                       = %ld\n", m_mbufs),
    MBS("Clusters obtained from page pool                      = %ld\n", m_clusters),
    MBS("Free clusters                                         = %ld\n", m_clfree),
    MBS("Number of times no free space was available           = %ld\n", m_drops),
    MBS("Number of times had to wait for free space            = %ld\n", m_wait),
    MBS("Number of times had to drain protocols for free space = %ld\n", m_drain),
};

#define MRTS(fmt, field) FLD(mrtstat, fmt, field)
static const struct stat_field mrt_fields[] = {
    MRTS("Multicast route lookups                 = %ld\n", mrts_mrt_lookups),
    MRTS("Multicast route cache misses            = %ld\n", mrts_mrt_misses),
    MRTS("Group address lookups                   = %ld\n", mrts_grp_lookups),
    MRTS("Group address cache misses              = %ld\n", mrts_grp_misses),
    MRTS("Packets with no routes to their origins = %ld\n", mrts_no_route),
    MRTS("Packets with malformed tunnel options   = %ld\n", mrts_bad_tunnel),
    MRTS("Packets with no room for tunnel options = %ld\n", mrts_cant_tunnel),
    MRTS("Packets arrived on the wrong interface  = %ld\n", mrts_wrong_if),
};

#define RTS(fmt, field) FLDH(rtstat, fmt, field)
static const struct stat_field rt_fields[] = {
    RTS("Bogus redirect calls              = %ld\n", rts_badredirect),
    RTS("Routes created by redirect calls  = %ld\n", rts_dynamic),
    RTS("Routes modified by redirect calls = %ld\n", rts_newgateway),
    RTS("Lookups which failed              = %ld\n", rts_unreach),
    RTS("Lookups satisfied by a wildcard   = %ld\n", rts_wildcard),
};

/* tcps_rexmttimeo and tcps_pcbcachemiss are not displayed, as in the genuine
 * tool */
#define TCPS(fmt, field) FLD(tcpstat, fmt, field)
static const struct stat_field tcp_fields[] = {
    TCPS("Connections initiated                           = %ld\n", tcps_connattempt),
    TCPS("Connections accepted                            = %ld\n", tcps_accepts),
    TCPS("Connections established                         = %ld\n", tcps_connects),
    TCPS("Connections dropped                             = %ld\n", tcps_drops),
    TCPS("Embryonic connections dropped                   = %ld\n", tcps_conndrops),
    TCPS("Connections closed                              = %ld\n", tcps_closed),
    TCPS("Segments timed                                  = %ld\n", tcps_segstimed),
    TCPS("Round trip times updated                        = %ld\n", tcps_rttupdated),
    TCPS("Delayed acknowledgments sent                    = %ld\n", tcps_delack),
    TCPS("Connections dropped in timeout state            = %ld\n", tcps_timeoutdrop),
    TCPS("Persist timeouts                                = %ld\n", tcps_persisttimeo),
    TCPS("Keep-alive timeouts                             = %ld\n", tcps_keeptimeo),
    TCPS("Keep-alive probes sent                          = %ld\n", tcps_keepprobe),
    TCPS("Connections dropped due to keep-alive timeouts  = %ld\n", tcps_keepdrops),
    TCPS("Total packets sent                              = %ld\n", tcps_sndtotal),
    TCPS("Data packets sent                               = %ld\n", tcps_sndpack),
    TCPS("Data bytes sent                                 = %ld\n", tcps_sndbyte),
    TCPS("Data packets retransmitted                      = %ld\n", tcps_sndrexmitpack),
    TCPS("Data bytes retransmitted                        = %ld\n", tcps_sndrexmitbyte),
    TCPS("Acknowledge-only packets sent                   = %ld\n", tcps_sndacks),
    TCPS("Window probes sent                              = %ld\n", tcps_sndprobe),
    TCPS("Packets sent with 'urgent' attribute            = %ld\n", tcps_sndurg),
    TCPS("Window-update-only packets sent                 = %ld\n", tcps_sndwinup),
    TCPS("Packets sent with control attributes            = %ld\n", tcps_sndctrl),
    TCPS("Total packets received                          = %ld\n", tcps_rcvtotal),
    TCPS("Packets received in sequence                    = %ld\n", tcps_rcvpack),
    TCPS("Bytes received in sequence                      = %ld\n", tcps_rcvbyte),
    TCPS("Packets received with checksum errors           = %ld\n", tcps_rcvbadsum),
    TCPS("Packets received with bad offset                = %ld\n", tcps_rcvbadoff),
    TCPS("Packets received that were too short            = %ld\n", tcps_rcvshort),
    TCPS("Duplicate packets received                      = %ld\n", tcps_rcvduppack),
    TCPS("Duplicate bytes received                        = %ld\n", tcps_rcvdupbyte),
    TCPS("Out-of-order packets received                   = %ld\n", tcps_rcvoopack),
    TCPS("Out-of-order bytes received                     = %ld\n", tcps_rcvoobyte),
    TCPS("Packets received with data after window         = %ld\n", tcps_rcvpackafterwin),
    TCPS("Bytes received after window                     = %ld\n", tcps_rcvbyteafterwin),
    TCPS("Packets received after 'close'                  = %ld\n", tcps_rcvafterclose),
    TCPS("Window probe packets received                   = %ld\n", tcps_rcvwinprobe),
    TCPS("Duplicate acknowledgements received             = %ld\n", tcps_rcvdupack),
    TCPS("Acknowledgements received for unsent data       = %ld\n", tcps_rcvacktoomuch),
    TCPS("Acknowledgement packets received                = %ld\n", tcps_rcvackpack),
    TCPS("Bytes acknowledged                              = %ld\n", tcps_rcvackbyte),
    TCPS("Window update packets received                  = %ld\n", tcps_rcvwinupd),
    TCPS("Segments dropped due to PAWS                    = %ld\n", tcps_pawsdrop),
    TCPS("Correct header predictions for acknowledgements = %ld\n", tcps_predack),
    TCPS("Correct header predictions for data packets     = %ld\n", tcps_preddat),
    TCPS("Timeouts in persist state                       = %ld\n", tcps_persistdrop),
    TCPS("Packets with bogus 'SYN' attribute received     = %ld\n", tcps_badsyn),
};

#define UDPS(fmt, field) FLD(udpstat, fmt, field)
static const struct stat_field udp_fields[] = {
    UDPS("Total packets received                       = %ld\n", udps_ipackets),
    UDPS("Packets received shorter than header         = %ld\n", udps_hdrops),
    UDPS("Packets received with checksum errors        = %ld\n", udps_badsum),
    UDPS("Packets received larger than expected        = %ld\n", udps_badlen),
    UDPS("Packets received for unbound ports           = %ld\n", udps_noport),
    UDPS("Broadcast packets received for unbound ports = %ld\n", udps_noportbcast),
    UDPS("Packets dropped                              = %ld\n", udps_fullsock),
    UDPS("Total packets sent                           = %ld\n", udps_opackets),
};

#define N_FIELDS(t) (sizeof(t) / sizeof((t)[0]))

/* --- address rendering ----------------------------------------------------- */

static char *ip4_quad(ULONG a, char *buf)
{
    sprintf(buf, "%lu.%lu.%lu.%lu",
            (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

/* 64-bit decimal for the SBQUAD_T byte counters */
static char *u64_str(ULONG hi, ULONG lo, char *buf, size_t len)
{
    unsigned long long v = ((unsigned long long)hi << 32) | lo;
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);
    size_t i = 0;
    while (n > 0 && i + 1 < len)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

/* host rendering: dotted quad, or the resolved name with the local domain
 * stripped when NAMES is in effect */
static char *routename(ULONG addr, char *buf, size_t len)
{
    if (opt_names)
    {
        static BOOL have_domain;
        static char domain[MAXHOSTNAMELEN];
        if (!have_domain)
        {
            have_domain = TRUE;
            char *dot;
            if (gethostname((STRPTR)domain, sizeof(domain)) == 0 &&
                (dot = strchr(domain, '.')) != NULL)
                memmove(domain, dot + 1, strlen(dot + 1) + 1);
            else
                domain[0] = '\0';
        }

        struct hostent *hp =
            gethostbyaddr((STRPTR)&addr, sizeof(addr), AF_INET);
        if (hp != NULL && hp->h_name != NULL)
        {
            strncpy(buf, (const char *)hp->h_name, len - 1);
            buf[len - 1] = '\0';
            char *dot = strchr(buf, '.');
            if (dot != NULL && domain[0] != '\0' && strcmp(dot + 1, domain) == 0)
                *dot = '\0';
            return buf;
        }
    }
    return ip4_quad(addr, buf);
}

/* network rendering: trailing zero octets suppressed, netstat-style */
static char *netname(ULONG a, char *buf)
{
    if (a & 0xff)
        sprintf(buf, "%lu.%lu.%lu.%lu",
                (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    else if (a & 0xff00)
        sprintf(buf, "%lu.%lu.%lu", (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff);
    else if (a & 0xff0000)
        sprintf(buf, "%lu.%lu", (a >> 24) & 0xff, (a >> 16) & 0xff);
    else
        sprintf(buf, "%lu", (a >> 24) & 0xff);
    return buf;
}

/* append ":service-or-port" for the socket tables */
static void append_port(char *buf, size_t len, UWORD port, const char *proto)
{
    struct servent *s =
        opt_names ? getservbyport((LONG)port, (STRPTR)proto) : NULL;
    size_t used = strlen(buf);
    if (s != NULL && s->s_name != NULL)
        snprintf(buf + used, len - used, ":%s", (const char *)s->s_name);
    else
        snprintf(buf + used, len - used, ":%u", (unsigned)port);
}

/* --- GetNetworkStatistics two-pass fetch ----------------------------------- */

static const char *const status_names[] = {
    "Internet Control Message Protocol",  /* NETSTATUS_icmp 0 */
    "Internet Group Management Protocol", /* NETSTATUS_igmp 1 */
    "Internet Protocol",                  /* NETSTATUS_ip 2 */
    "memory buffer",                      /* NETSTATUS_mb 3 */
    "multicast routing",                  /* NETSTATUS_mrt 4 */
    "routing",                            /* NETSTATUS_rt 5 */
    "Transmission Control Protocol",      /* NETSTATUS_tcp 6 */
    "User Datagram Protocol",             /* NETSTATUS_udp 7 */
    "?",                                  /* 8 unused */
    "Transmission Control Protocol sockets", /* NETSTATUS_tcp_sockets 9 */
    "User Datagram Protocol sockets",     /* NETSTATUS_udp_sockets 10 */
};

static APTR query_stats(LONG type, LONG *size_out)
{
    const char *what = (type >= 0 && type <= 10) ? status_names[type] : "?";
    LONG size = GetNetworkStatistics(type, NETWORKSTATUS_VERSION, NULL, 0);
    if (size < 0)
    {
        if (!opt_quiet)
            fprintf(stderr, "ShowNetStatus: Status information for \"%s\" "
                            "could not be obtained.\n", what);
        return NULL;
    }
    /* small slack: socket-list entries may appear between the two calls */
    size += 4 * (LONG)sizeof(struct protocol_connection_data);
    APTR buf = AllocVec((ULONG)size, MEMF_ANY | MEMF_CLEAR);
    if (buf == NULL)
    {
        if (!opt_quiet)
            fprintf(stderr, "ShowNetStatus: Not enough memory to obtain "
                            "status information for \"%s\".\n", what);
        return NULL;
    }
    LONG got = GetNetworkStatistics(type, NETWORKSTATUS_VERSION, buf, size);
    if (got < 0)
    {
        if (!opt_quiet)
            fprintf(stderr, "ShowNetStatus: Status information for \"%s\" "
                            "could not be obtained.\n", what);
        FreeVec(buf);
        return NULL;
    }
    *size_out = got;
    return buf;
}

/* fetch a stat struct and print it as a titled table; FALSE on failure */
static BOOL show_stats(LONG type, const char *title,
                       const struct stat_field *fields, size_t n)
{
    LONG size = 0;
    UBYTE *buf = query_stats(type, &size);
    if (buf == NULL)
        return FALSE;

    section_gap();
    printf("%s%s%s\n", ul_on, title, ul_off);
    for (size_t i = 0; i < n; i++)
    {
        long v = 0;
        if (fields[i].half)
        {
            if ((LONG)(fields[i].off + sizeof(WORD)) <= size)
                v = *(const WORD *)(buf + fields[i].off);
        }
        else
        {
            if ((LONG)(fields[i].off + sizeof(ULONG)) <= size)
                v = (long)*(const ULONG *)(buf + fields[i].off);
        }
        printf(fields[i].fmt, v);
    }
    FreeVec(buf);
    return TRUE;
}

/* --- interfaces ------------------------------------------------------------ */

/* SANA-II wire type reported by IFQ_HardwareType (<devices/sana2.h> is not
 * part of the netinclude set) */
#ifndef S2WireType_Ethernet
#define S2WireType_Ethernet 1
#endif

static const char *hardware_type_name(LONG type, char *buf)
{
    if (type == S2WireType_Ethernet)
        return "Ethernet";
    sprintf(buf, "Unknown (%ld)", type);
    return buf;
}

/* detailed per-interface block (INTERFACE/M). Lines the library cannot
 * answer (lease expiry, request high-water marks, SANA-II copy stats, peer
 * address) are omitted. */
static BOOL show_interface_info(STRPTR name)
{
    STRPTR device_name = NULL;
    LONG device_unit = 0, hw_bits = 0, mtu = 0, bps = 0, hw_type = 0;
    LONG state = SM_Down, bind_type = IFABT_Unknown;
    ULONG received = 0, sent = 0, bad = 0, overrun = 0, unknown = 0;
    LONG in_dropped = 0, out_dropped = 0;
    UBYTE hw_addr[16];
    struct sockaddr_in sin, sin_mask;
    SBQUAD_T bytes_in, bytes_out;

    memset(hw_addr, 0, sizeof(hw_addr));
    memset(&sin, 0, sizeof(sin));
    memset(&sin_mask, 0, sizeof(sin_mask));
    memset(&bytes_in, 0, sizeof(bytes_in));
    memset(&bytes_out, 0, sizeof(bytes_out));

    if (QueryInterfaceTags(name,
                           IFQ_DeviceName, (Tag)&device_name,
                           IFQ_DeviceUnit, (Tag)&device_unit,
                           IFQ_HardwareAddressSize, (Tag)&hw_bits,
                           IFQ_HardwareAddress, (Tag)hw_addr,
                           IFQ_MTU, (Tag)&mtu,
                           IFQ_BPS, (Tag)&bps,
                           IFQ_HardwareType, (Tag)&hw_type,
                           IFQ_PacketsReceived, (Tag)&received,
                           IFQ_PacketsSent, (Tag)&sent,
                           IFQ_BadData, (Tag)&bad,
                           IFQ_Overruns, (Tag)&overrun,
                           IFQ_UnknownTypes, (Tag)&unknown,
                           IFQ_InputDrops, (Tag)&in_dropped,
                           IFQ_OutputDrops, (Tag)&out_dropped,
                           IFQ_Address, (Tag)&sin,
                           IFQ_NetMask, (Tag)&sin_mask,
                           IFQ_State, (Tag)&state,
                           IFQ_AddressBindType, (Tag)&bind_type,
                           IFQ_GetBytesIn, (Tag)&bytes_in,
                           IFQ_GetBytesOut, (Tag)&bytes_out,
                           TAG_END) != 0)
        return FALSE;

    section_gap();

    char buf[LINE_LEN];
    printf("%sInterface \"%s\"%s\n", ul_on, (const char *)name, ul_off);
    printf("Device name                  = %s\n",
           device_name != NULL ? (const char *)device_name : "-");
    printf("Device unit number           = %ld\n", (long)device_unit);
    printf("Hardware address             = ");
    if (hw_bits > (LONG)sizeof(hw_addr) * 8)
        hw_bits = sizeof(hw_addr) * 8;
    for (LONG i = 0; i < hw_bits / 8; i++)
        printf(i != 0 ? ":%02lx" : "%02lx", (ULONG)hw_addr[i]);
    printf("\n");
    printf("Maximum transmission unit    = %ld Bytes\n", (long)mtu);
    printf("Transmission speed           = %ld Bits/Second\n", (long)bps);
    printf("Hardware type                = %s\n",
           hardware_type_name(hw_type, buf));
    printf("Packets sent                 = %ld\n", (long)sent);
    printf("Packets received             = %ld\n", (long)received);
    printf("Packets dropped              = %ld (in = %ld, out = %ld)\n",
           (long)bad, (long)in_dropped, (long)out_dropped);
    printf("Buffer overruns              = %ld\n", (long)overrun);
    printf("Unknown packets              = %ld\n", (long)unknown);
    printf("Address                      = %s\n",
           sin.sin_addr.s_addr != INADDR_ANY
               ? routename(sin.sin_addr.s_addr, buf, sizeof(buf))
               : "(Not configured)");
    printf("Network mask                 = %s\n",
           sin_mask.sin_addr.s_addr != INADDR_ANY
               ? ip4_quad(sin_mask.sin_addr.s_addr, buf)
               : "(Not configured)");
    printf("Number of bytes received     = %s\n",
           u64_str(bytes_in.sbq_High, bytes_in.sbq_Low, buf, sizeof(buf)));
    printf("Number of bytes sent         = %s\n",
           u64_str(bytes_out.sbq_High, bytes_out.sbq_Low, buf, sizeof(buf)));
    if (bind_type == IFABT_Static || bind_type == IFABT_Dynamic)
        printf("Address binding              = %s\n",
               bind_type == IFABT_Static ? "Static" : "Dynamic");
    printf("Link status                  = %s\n", state == SM_Up ? "Up" : "Down");
    return TRUE;
}

static void show_interface_list(struct List *interface_list)
{
    section_gap();

    LONG n = 0;
    for (struct Node *node = interface_list->lh_Head; node->ln_Succ != NULL;
         node = node->ln_Succ)
    {
        struct sockaddr_in sin;
        LONG mtu = 0, hw_type = 0, state = SM_Down;
        ULONG received = 0, sent = 0, bad = 0, overrun = 0, unknown = 0;
        memset(&sin, 0, sizeof(sin));

        if (QueryInterfaceTags((STRPTR)node->ln_Name,
                               IFQ_MTU, (Tag)&mtu,
                               IFQ_HardwareType, (Tag)&hw_type,
                               IFQ_PacketsReceived, (Tag)&received,
                               IFQ_PacketsSent, (Tag)&sent,
                               IFQ_BadData, (Tag)&bad,
                               IFQ_Overruns, (Tag)&overrun,
                               IFQ_UnknownTypes, (Tag)&unknown,
                               IFQ_Address, (Tag)&sin,
                               IFQ_State, (Tag)&state,
                               TAG_END) != 0)
            continue;

        if (n++ == 0)
            printf("%s%-20s %4s %-20s %-20s %8s %8s %8s %8s %8s %s%s\n",
                   ul_on, "Name", "MTU", "Type", "Address", "Received",
                   "Sent", "Dropped", "Overruns", "Unknown", "Status", ul_off);

        char type_buf[32], addr_buf[LINE_LEN];
        const char *addr = sin.sin_addr.s_addr != INADDR_ANY
                               ? routename(sin.sin_addr.s_addr, addr_buf,
                                           sizeof(addr_buf))
                               : "-";
        printf("%-20s %4ld %-20s %-20s %8ld %8ld %8ld %8ld %8ld %s\n",
               node->ln_Name, (long)mtu, hardware_type_name(hw_type, type_buf),
               addr, (long)received, (long)sent, (long)bad, (long)overrun,
               (long)unknown, state == SM_Up ? "Up" : "Down");
    }

    if (n == 0)
        printf("No interfaces are currently available.\n");
}

/* --- DNS ------------------------------------------------------------------- */

static void show_dns_list(struct List *dns_list)
{
    section_gap();

    LONG n = 0;
    for (struct DomainNameServerNode *dnsn =
             (struct DomainNameServerNode *)dns_list->lh_Head;
         dnsn->dnsn_MinNode.mln_Succ != NULL;
         dnsn = (struct DomainNameServerNode *)dnsn->dnsn_MinNode.mln_Succ)
    {
        if (n++ == 0)
            printf("%s%-20s %-10s%s\n", ul_on, "Address", "Type", ul_off);

        char buf[LINE_LEN];
        ULONG addr = inet_addr(dnsn->dnsn_Address);
        printf("%-20s %s\n",
               addr != INADDR_NONE ? routename(addr, buf, sizeof(buf))
                                   : (const char *)dnsn->dnsn_Address,
               dnsn->dnsn_UseCount < 0 ? "Static" : "Dynamic");
    }

    if (n == 0)
        printf("No domain name servers are configured.\n");

    char domain[MAXHOSTNAMELEN];
    domain[0] = '\0';
    if (GetDefaultDomainName((STRPTR)domain, sizeof(domain)) && domain[0] != '\0')
        printf("\nThe default domain name is set to \"%s\".\n", domain);
    else
        printf("\nThe default domain name is not set.\n");
}

/* --- routes ---------------------------------------------------------------- */

/* walk one rt_msghdr entry's sockaddr list; NULL when absent. The walk
 * follows the documented format: sockaddrs trail the header in RTAX order,
 * each advancing by sa_len (sizeof(long) when sa_len is 0). */
static struct sockaddr_in *rtm_addr(struct rt_msghdr *rtm, LONG which)
{
    if ((rtm->rtm_addrs & which) == 0)
        return NULL;
    UBYTE *p = (UBYTE *)(rtm + 1);
    for (LONG bit = 1; bit < which; bit <<= 1)
    {
        if ((rtm->rtm_addrs & bit) == 0)
            continue;
        struct sockaddr *sa = (struct sockaddr *)p;
        p += sa->sa_len != 0 ? sa->sa_len : sizeof(long);
    }
    return (struct sockaddr_in *)p;
}

static const char *route_dest_name(struct sockaddr_in *sin, LONG flags,
                                   char *buf, size_t len)
{
    if (sin->sin_addr.s_addr == INADDR_ANY)
        return "(Default)";
    if (flags & RTF_HOST)
        return routename(sin->sin_addr.s_addr, buf, len);
    return netname(sin->sin_addr.s_addr, buf);
}

static void show_route_table(struct rt_msghdr *route_table)
{
    section_gap();

    LONG n = 0;
    for (struct rt_msghdr *rtm = route_table; rtm->rtm_msglen > 0;
         rtm = (struct rt_msghdr *)((ULONG)rtm + rtm->rtm_msglen))
    {
        if (rtm->rtm_version != RTM_VERSION || (rtm->rtm_addrs & RTA_DST) == 0)
            continue;

        struct sockaddr_in *dst = rtm_addr(rtm, RTA_DST);
        if (dst == NULL || dst->sin_family != AF_INET)
            continue;

        char dst_buf[LINE_LEN], gw_buf[LINE_LEN];
        const char *gateway;
        struct sockaddr_in *gw = rtm_addr(rtm, RTA_GATEWAY);
        if (gw != NULL)
        {
            if (gw->sin_family != AF_INET)
                continue; /* as in the genuine parser */
            /* "the default route is indicated by having a gateway address of
             * 'default'" (ShowNetStatus.doc). Non-zero gateways keep
             * routename() rather than the genuine tool's netname(): without a
             * netent database netname() would only cost us the reverse-DNS
             * name under NAMES. */
            gateway = gw->sin_addr.s_addr == INADDR_ANY
                          ? "(Default)"
                          : routename(gw->sin_addr.s_addr, gw_buf, sizeof(gw_buf));
        }
        else
        {
            gateway = "-";
        }

        if (n++ == 0)
            printf("%s%-16s %-16s %s%s\n", ul_on, "Destination", "Gateway",
                   "Attributes", ul_off);

        printf("%-16s %-16s",
               route_dest_name(dst, rtm->rtm_flags, dst_buf, sizeof(dst_buf)),
               gateway);
        printf(" %s", (rtm->rtm_flags & RTF_UP) ? "Up" : "Down");
        if (rtm->rtm_flags & RTF_GATEWAY)
            printf(" Gateway");
        if (rtm->rtm_flags & RTF_HOST)
            printf(" Host");
        printf("\n");
    }

    if (n == 0)
        printf("No routes are currently configured.\n");
}

/* --- ARP cache (via SIOCGARPT, like the Arp command) ----------------------- */

static BOOL show_arp_cache(void)
{
    struct arptabreq atr = {0, 0, NULL};
    if (IoctlSocket(arp_socket, SIOCGARPT, &atr) != 0)
        return FALSE;

    section_gap();
    if (atr.atr_inuse == 0)
    {
        printf("The address resolution protocol cache is currently empty.\n");
        return TRUE;
    }

    /* small slack: entries may appear between the size query and the dump */
    LONG cap = atr.atr_inuse + 4;
    struct arpreq *tab = AllocVec((ULONG)cap * sizeof(*tab), MEMF_ANY | MEMF_CLEAR);
    if (tab == NULL)
        return FALSE;

    atr.atr_size = cap;
    atr.atr_table = tab;
    if (IoctlSocket(arp_socket, SIOCGARPT, &atr) != 0)
    {
        FreeVec(tab);
        return FALSE;
    }

    for (LONG i = 0; i < atr.atr_size; i++)
    {
        const struct arpreq *ar = &tab[i];
        const struct sockaddr_in *sin =
            (const struct sockaddr_in *)&ar->arp_pa;

        if (i == 0)
            printf("%s%-16s %-16s %-20s %s%s\n\n", ul_on, "Host", "IP address",
                   "Hardware address", "Attributes", ul_off);

        char host_buf[LINE_LEN], quad_buf[24], hw_buf[24];
        const char *host = "(Unknown)";
        if (opt_names)
        {
            struct hostent *hp = gethostbyaddr((STRPTR)&sin->sin_addr,
                                               sizeof(sin->sin_addr), AF_INET);
            if (hp != NULL && hp->h_name != NULL)
            {
                strncpy(host_buf, (const char *)hp->h_name, sizeof(host_buf) - 1);
                host_buf[sizeof(host_buf) - 1] = '\0';
                host = host_buf;
            }
        }
        else
        {
            host = ip4_quad(sin->sin_addr.s_addr, host_buf);
        }

        if (ar->arp_flags & ATF_COM)
        {
            const UBYTE *e = (const UBYTE *)ar->arp_ha.sa_data;
            sprintf(hw_buf, "%02lx:%02lx:%02lx:%02lx:%02lx:%02lx",
                    (ULONG)e[0], (ULONG)e[1], (ULONG)e[2], (ULONG)e[3],
                    (ULONG)e[4], (ULONG)e[5]);
        }
        else
        {
            strcpy(hw_buf, "(Incomplete)");
        }

        printf("%-16s %-16s %-20s %s\n", host,
               ip4_quad(sin->sin_addr.s_addr, quad_buf), hw_buf,
               (ar->arp_flags & ATF_PERM) ? "permanent" : "");
    }

    FreeVec(tab);
    return TRUE;
}

/* --- socket tables ---------------------------------------------------------- */

static const char *const tcp_state_names[] = {
    "Closed",                                        /* TCPS_CLOSED */
    "Listening for connection",                      /* TCPS_LISTEN */
    "Active; 'SYN' sent",                            /* TCPS_SYN_SENT */
    "Sent and received 'SYN'",                       /* TCPS_SYN_RECEIVED */
    "Established",                                   /* TCPS_ESTABLISHED */
    "Received 'FIN', waiting for close",             /* TCPS_CLOSE_WAIT */
    "Closed; 'FIN' sent",                            /* TCPS_FIN_WAIT_1 */
    "Closed, 'FIN' exchanged, waiting for 'FIN ACK'", /* TCPS_CLOSING */
    "Closed; waiting for last 'FIN ACK'",            /* TCPS_LAST_ACK */
    "Closed, 'FIN' acknowledged",                    /* TCPS_FIN_WAIT_2 */
    "Closed; waiting...",                            /* TCPS_TIME_WAIT */
};
_Static_assert(sizeof(tcp_state_names) / sizeof(tcp_state_names[0]) == TCP_NSTATES,
               "one name per <netinet/tcp_fsm.h> state");

static BOOL show_sockets(BOOL tcp)
{
    LONG size = 0;
    struct protocol_connection_data *pcd =
        query_stats(tcp ? NETSTATUS_tcp_sockets : NETSTATUS_udp_sockets, &size);
    if (pcd == NULL)
        return FALSE; /* error already reported */

    section_gap();

    LONG entries = size / (LONG)sizeof(*pcd);
    LONG shown = 0, hidden = 0;
    const char *proto = tcp ? "tcp" : "udp";

    for (LONG i = 0; i < entries; i++)
    {
        const struct protocol_connection_data *p = &pcd[i];

        /* without ALL, sockets bound to local addresses stay hidden */
        if (p->pcd_local_address.s_addr == INADDR_ANY && !opt_all)
        {
            hidden++;
            continue;
        }

        if (shown++ == 0)
        {
            if (tcp)
                printf("%s%-30s %-30s %10s %10s %-30s%s\n", ul_on, "Local",
                       "Remote", "Rcv.len", "Snd.len", "Status", ul_off);
            else
                printf("%s%-30s %-30s %10s %10s%s\n", ul_on, "Local", "Remote",
                       "Rcv.len", "Snd.len", ul_off);
        }

        char local[LINE_LEN], remote[LINE_LEN];
        if (p->pcd_local_address.s_addr != INADDR_ANY || !opt_names)
            routename(p->pcd_local_address.s_addr, local, sizeof(local));
        else
            strcpy(local, "(Any)");
        append_port(local, sizeof(local), p->pcd_local_port, proto);

        if (p->pcd_foreign_address.s_addr != INADDR_ANY)
        {
            routename(p->pcd_foreign_address.s_addr, remote, sizeof(remote));
            append_port(remote, sizeof(remote), p->pcd_foreign_port, proto);
        }
        else
        {
            strcpy(remote, "-");
        }

        if (tcp)
        {
            const char *state = "-";
            if (p->pcd_tcp_state >= 0 &&
                p->pcd_tcp_state < (LONG)(sizeof(tcp_state_names) /
                                          sizeof(tcp_state_names[0])))
                state = tcp_state_names[p->pcd_tcp_state];
            printf("%-30s %-30s %10ld %10ld %s\n", local, remote,
                   (long)p->pcd_receive_queue_size,
                   (long)p->pcd_send_queue_size, state);
        }
        else
        {
            printf("%-30s %-30s %10ld %10ld\n", local, remote,
                   (long)p->pcd_receive_queue_size,
                   (long)p->pcd_send_queue_size);
        }
    }

    if (shown == 0)
    {
        if (hidden == 0)
            printf("No %s sockets are currently in use.\n", tcp ? "TCP" : "UDP");
        else
            printf("Currently, %ld local %s sockets are in use, but not "
                   "displayed (use the \"ALL\" option to show them).\n",
                   (long)hidden, tcp ? "TCP" : "UDP");
    }

    FreeVec(pcd);
    return TRUE;
}

/* --- summary (the no-arguments mode) --------------------------------------- */

static void show_summary(void)
{
    section_gap();
    printf("%sNetwork status summary%s\n", ul_on, ul_off);

    /* the local host address is the interface address matching gethostid() */
    char name_buf[LINE_LEN];
    char if_name[LINE_LEN];
    ULONG host_addr = 0;
    if_name[0] = '\0';

    struct List *interface_list = ObtainInterfaceList();
    if (interface_list != NULL)
    {
        ULONG host_id = gethostid();
        for (struct Node *node = interface_list->lh_Head;
             node->ln_Succ != NULL; node = node->ln_Succ)
        {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            if (QueryInterfaceTags((STRPTR)node->ln_Name,
                                   IFQ_Address, (Tag)&sin,
                                   TAG_END) != 0)
                continue;
            if (sin.sin_addr.s_addr != INADDR_ANY &&
                sin.sin_addr.s_addr == host_id)
            {
                host_addr = sin.sin_addr.s_addr;
                strncpy(if_name, node->ln_Name, sizeof(if_name) - 1);
                if_name[sizeof(if_name) - 1] = '\0';
                break;
            }
        }
    }

    printf("Local host address         = ");
    if (if_name[0] != '\0')
        printf("%s (on interface '%s')", ip4_quad(host_addr, name_buf), if_name);
    else
        printf("(Not configured)");
    printf("\n");

    /* the default route, if the route table holds one */
    ULONG gateway = 0;
    BOOL have_gateway = FALSE;
    struct rt_msghdr *route_table = GetRouteInfo(AF_UNSPEC, 0);
    if (route_table != NULL)
    {
        for (struct rt_msghdr *rtm = route_table; rtm->rtm_msglen > 0;
             rtm = (struct rt_msghdr *)((ULONG)rtm + rtm->rtm_msglen))
        {
            if (rtm->rtm_version != RTM_VERSION ||
                (rtm->rtm_addrs & RTA_DST) == 0 ||
                (rtm->rtm_addrs & RTA_GATEWAY) == 0)
                continue;
            struct sockaddr_in *dst = rtm_addr(rtm, RTA_DST);
            struct sockaddr_in *gw = rtm_addr(rtm, RTA_GATEWAY);
            if (dst == NULL || gw == NULL || dst->sin_family != AF_INET ||
                gw->sin_family != AF_INET)
                continue;
            if (dst->sin_addr.s_addr != INADDR_ANY)
                continue;
            gateway = gw->sin_addr.s_addr;
            have_gateway = TRUE;
            break;
        }
    }

    printf("Default gateway address    = ");
    if (have_gateway)
        printf("%s", ip4_quad(gateway, name_buf));
    else
        printf("(Not configured)");
    printf("\n");

    printf("Domain name system servers = ");
    struct List *dns_list = ObtainDomainNameServerList();
    if (dns_list != NULL && dns_list->lh_Head->ln_Succ != NULL)
    {
        BOOL first = TRUE;
        for (struct DomainNameServerNode *dnsn =
                 (struct DomainNameServerNode *)dns_list->lh_Head;
             dnsn->dnsn_MinNode.mln_Succ != NULL;
             dnsn = (struct DomainNameServerNode *)dnsn->dnsn_MinNode.mln_Succ)
        {
            printf(first ? "%s" : ", %s", (const char *)dnsn->dnsn_Address);
            first = FALSE;
        }
    }
    else
    {
        printf("(Not configured)");
    }
    printf("\n");

    if (interface_list != NULL)
        ReleaseInterfaceList(interface_list);
    if (dns_list != NULL)
        ReleaseDomainNameServerList(dns_list);
    if (route_table != NULL)
        FreeRouteInfo(route_table);
}

/* --------------------------------------------------------------------------- */

int main(void)
{
    LONG args[ARG_COUNT];
    memset(args, 0, sizeof(args));
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "ShowNetStatus");
        return RETURN_ERROR;
    }
    STRPTR *interfaces = (STRPTR *)args[ARG_INTERFACE];
    opt_names = args[ARG_NAMES] != 0;
    opt_all = args[ARG_ALL] != 0;
    opt_quiet = args[ARG_QUIET] != 0;
    BOOL repeat = args[ARG_REPEAT] != 0;

    BOOL summary = interfaces == NULL;
    for (LONG i = ARG_INTERFACES; i <= ARG_UDPSOCKETS; i++)
        if (args[i] != 0)
            summary = FALSE;

    int rc = RETURN_FAIL;
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        if (!opt_quiet)
            fprintf(stderr,
                    "ShowNetStatus: Failed to open \"bsdsocket.library\" V4.\n");
        goto out_args;
    }

    /* API capability gates, as in the genuine tool — minus the routing gate:
     * this stack keeps SBTC_HAVE_ROUTING_API FALSE while GetRouteInfo works,
     * so GetRouteInfo() == NULL is handled per use instead */
    {
        LONG have_status = FALSE, have_interface = FALSE, have_dns = FALSE;
        struct TagItem tags[4];
        tags[0].ti_Tag = SBTM_GETREF(SBTC_HAVE_STATUS_API);
        tags[0].ti_Data = (ULONG)&have_status;
        tags[1].ti_Tag = SBTM_GETREF(SBTC_HAVE_INTERFACE_API);
        tags[1].ti_Data = (ULONG)&have_interface;
        tags[2].ti_Tag = SBTM_GETREF(SBTC_HAVE_DNS_API);
        tags[2].ti_Data = (ULONG)&have_dns;
        tags[3].ti_Tag = TAG_END;
        if (SocketBaseTagList(tags) != 0 ||
            !have_status || !have_interface || !have_dns)
        {
            if (!opt_quiet)
                fprintf(stderr, "ShowNetStatus: \"%s\" V%ld.%ld does not "
                                "support the query methods used by this "
                                "program.\n",
                        SocketBase->lib_Node.ln_Name,
                        (long)SocketBase->lib_Version,
                        (long)SocketBase->lib_Revision);
            goto out_lib;
        }
    }

    arp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (arp_socket < 0)
    {
        if (!opt_quiet)
            fprintf(stderr, "ShowNetStatus: No free socket available.\n");
        goto out_lib;
    }

    if (IsInteractive(Output()))
    {
        ul_on = "\33[4m";
        ul_off = "\33[0m";
    }

    for (;;)
    {
        something = FALSE;

        if (summary)
            show_summary();

        if (interfaces != NULL)
        {
            for (STRPTR *name = interfaces; *name != NULL; name++)
            {
                if (!show_interface_info(*name))
                {
                    if (!opt_quiet)
                        fprintf(stderr, "ShowNetStatus: Unable to obtain "
                                        "information on interface \"%s\".\n",
                                (const char *)*name);
                    goto out_socket;
                }
            }
        }

        if (args[ARG_INTERFACES])
        {
            struct List *list = ObtainInterfaceList();
            if (list == NULL)
            {
                if (!opt_quiet)
                    fprintf(stderr, "ShowNetStatus: Unable to obtain "
                                    "interface information.\n");
                goto out_socket;
            }
            show_interface_list(list);
            ReleaseInterfaceList(list);
        }

        if (args[ARG_DNS])
        {
            struct List *list = ObtainDomainNameServerList();
            if (list == NULL)
            {
                if (!opt_quiet)
                    fprintf(stderr, "ShowNetStatus: Unable to obtain domain "
                                    "name server information.\n");
                goto out_socket;
            }
            show_dns_list(list);
            ReleaseDomainNameServerList(list);
        }

        if (args[ARG_ROUTES])
        {
            struct rt_msghdr *table = GetRouteInfo(AF_UNSPEC, 0);
            if (table == NULL)
            {
                if (!opt_quiet)
                    fprintf(stderr, "ShowNetStatus: Unable to obtain routing "
                                    "information.\n");
                goto out_socket;
            }
            show_route_table(table);
            FreeRouteInfo(table);
        }

        if (args[ARG_ARP])
        {
            if (!show_arp_cache())
            {
                if (!opt_quiet)
                    fprintf(stderr, "ShowNetStatus: Unable to obtain address "
                                    "resolution protocol information.\n");
                goto out_socket;
            }
        }

        if (args[ARG_ICMP] &&
            !show_stats(NETSTATUS_icmp,
                        "Internet Control Message Protocol statistics",
                        icmp_fields, N_FIELDS(icmp_fields)))
            goto out_socket;
        if (args[ARG_IGMP] &&
            !show_stats(NETSTATUS_igmp,
                        "Internet Group Management Protocol statistics",
                        igmp_fields, N_FIELDS(igmp_fields)))
            goto out_socket;
        if (args[ARG_IP] &&
            !show_stats(NETSTATUS_ip, "Internet Protocol statistics",
                        ip_fields, N_FIELDS(ip_fields)))
            goto out_socket;
        if (args[ARG_MB] &&
            !show_stats(NETSTATUS_mb, "Memory buffer statistics",
                        mb_fields, N_FIELDS(mb_fields)))
            goto out_socket;
        if (args[ARG_MR] &&
            !show_stats(NETSTATUS_mrt, "Multicast routing statistics",
                        mrt_fields, N_FIELDS(mrt_fields)))
            goto out_socket;
        if (args[ARG_RT] &&
            !show_stats(NETSTATUS_rt, "Routing statistics",
                        rt_fields, N_FIELDS(rt_fields)))
            goto out_socket;
        if (args[ARG_TCP] &&
            !show_stats(NETSTATUS_tcp,
                        "Transmission Control Protocol statistics",
                        tcp_fields, N_FIELDS(tcp_fields)))
            goto out_socket;
        if (args[ARG_UDP] &&
            !show_stats(NETSTATUS_udp, "User Datagram Protocol statistics",
                        udp_fields, N_FIELDS(udp_fields)))
            goto out_socket;

        if (args[ARG_TCPSOCKETS] && !show_sockets(TRUE))
            goto out_socket;
        if (args[ARG_UDPSOCKETS] && !show_sockets(FALSE))
            goto out_socket;

        if (!repeat)
            break;

        /* one-second cadence, responsive to Ctrl-C */
        BOOL stopped = FALSE;
        for (int i = 0; i < 10; i++)
        {
            Delay(TICKS_PER_SECOND / 10);
            if (CheckSignal(SIGBREAKF_CTRL_C))
            {
                stopped = TRUE;
                break;
            }
        }
        if (stopped)
        {
            if (!opt_quiet) /* QUIET: "do not show any error messages" */
                PrintFault(ERROR_BREAK, (CONST_STRPTR) "ShowNetStatus");
            rc = RETURN_WARN;
            goto out_socket;
        }
        printf("\f"); /* clear the console for the next round */
    }

    rc = RETURN_OK;

out_socket:
    if (arp_socket >= 0)
        CloseSocket(arp_socket);
out_lib:
    CloseLibrary(SocketBase);
out_args:
    FreeArgs(rda);
    if (opt_quiet && rc > RETURN_WARN)
        rc = RETURN_WARN;
    return rc;
}
