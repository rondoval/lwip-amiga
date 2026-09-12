/* SPDX-License-Identifier: BSD-4-Clause-UC */
/*
 * Copyright (c) 1990, 1993
 * The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Van Jacobson.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 * This product includes software developed by the University of
 * California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * traceroute — print the route packets take to a network host.
 *
 * Van Jacobson's traceroute (LBL, December 1988) as shipped with
 * 4.4BSD-Lite2, via Olaf Barthel's Roadshow port from the AmigaOS 3.2 NDK
 * (Amiga-side changes placed in the public domain). The ReadArgs template
 * and output are Roadshow-compatible. Probes are UDP datagrams with a
 * caller-built IP header sent over a raw IP_HDRINCL socket; replies are the
 * ICMP TIME_EXCEEDED / PORT_UNREACHABLE errors read from a raw ICMP socket.
 *
 * lwip-amiga notes:
 *  - bsdsocket.library completes IP_HDRINCL headers kernel-style (checksum
 *    and length always; source and id when left zero), so the probe header
 *    is built exactly as the BSD original built it.
 *  - DEBUG and DONTROUTE are accepted but the stack ignores the socket
 *    options, as the original ignored their setsockopt() results.
 *  - Round-trip times are integer microseconds end to end; no floating
 *    point, so the default shell stack suffices.
 *  - Break handling is explicit (EINTR from WaitSelect); libnix's stdio
 *    break check is disabled so output never stops mid-line.
 */

#include <sys/errno.h>
#include <sys/socket.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>

#include <arpa/inet.h>

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libraries/bsdsocket.h>

#include <exec/execbase.h>

#include <dos/dosextens.h>
#include <dos/rdargs.h>

#include <utility/tagitem.h>

#define __USE_OLD_TIMEVAL__
#include <devices/timer.h>

#include <proto/socket.h> /* bsdsocket.library inline glue */
#include <proto/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* $VER: cookie appended to the ReadArgs template (the leading NUL terminates
 * the template string). TOOL_VERSION/TOOL_DATE come from the build. */
#define VERSTAG "\0$VER: traceroute " TOOL_VERSION " " TOOL_DATE

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

typedef LONG *NUMBER;
typedef LONG SWITCH;
typedef STRPTR KEY;

struct RDArgs *rda;

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */
struct Device *TimerBase;
struct MsgPort *TimePort;
struct timerequest *TimeRequest;

/* Break handling is explicit (EINTR from WaitSelect): keep libnix's stdio
 * Ctrl-C check from exiting mid-line behind our back. */
void __chkabort(void) {}

static int open_bsdsocket(void)
{
    /* Opening bsdsocket.library brings the stack up, so this runs only after
     * the arguments have been validated. */
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        fprintf(stderr, "traceroute: Error opening \"bsdsocket.library\" V4.\n");
        return -1;
    }

    /* timer.device is opened for ReadEClock() only; no requests are sent */
    TimePort = CreateMsgPort();
    if (TimePort == NULL)
    {
        fprintf(stderr, "traceroute: Could not create timer message port.\n");
        return -1;
    }

    TimeRequest = (struct timerequest *)CreateIORequest(TimePort, sizeof(*TimeRequest));
    if (TimeRequest == NULL)
    {
        fprintf(stderr, "traceroute: Could not create timer I/O request.\n");
        return -1;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)TimeRequest, 0) != 0)
    {
        fprintf(stderr, "traceroute: Could not open '%s' unit %ld.\n", TIMERNAME, (long)UNIT_VBLANK);
        return -1;
    }

    TimerBase = TimeRequest->tr_node.io_Device;

    /* Let the library write errno directly so perror() just works */
    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno)));
    tags[0].ti_Data = (ULONG)&errno;
    tags[1].ti_Tag = TAG_END;

    SocketBaseTagList(tags);

    return 0;
}

static void close_bsdsocket(void)
{
    if (rda != NULL)
    {
        FreeArgs(rda);
        rda = NULL;
    }

    if (TimeRequest != NULL)
    {
        if (TimeRequest->tr_node.io_Device != NULL)
            CloseDevice((struct IORequest *)TimeRequest);

        DeleteIORequest((struct IORequest *)TimeRequest);
        TimeRequest = NULL;
    }

    if (TimePort != NULL)
    {
        DeleteMsgPort(TimePort);
        TimePort = NULL;
    }

    if (SocketBase != NULL)
    {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

/* Current system time from the monotonic EClock, in GetSysTime() format. */
static void get_eclock_time(struct timeval *eclock_time_now)
{
    struct EClockVal now;
    ULONG eclock_frequency = ReadEClock(&now);
    unsigned long long eclock_now = (((unsigned long long)now.ev_hi) << 32) | now.ev_lo;

    eclock_time_now->tv_secs = eclock_now / eclock_frequency;
    ULONG microseconds_remainder = eclock_now % eclock_frequency;
    eclock_time_now->tv_micro = (ULONG)(((unsigned long long)microseconds_remainder * 1000000) / eclock_frequency);
}

void finish(void)
{
    fflush(stdout);
    fflush(stderr);

    PrintFault(ERROR_BREAK, NULL);

    exit(RETURN_WARN);
}

#define getpid() ((unsigned long)FindTask(NULL))
#define gettimeofday(timeval, timezone) get_eclock_time(timeval)
#define select(nfds, readfds, writefds, exceptfds, timeval) WaitSelect(nfds, readfds, writefds, exceptfds, timeval, NULL)
#define inet_ntoa(in) Inet_NtoA((in).s_addr)

int wait_for_reply(int sock, struct sockaddr_in *from);
void send_probe(int seq, int ttl);
ULONG deltaT(struct timeval *t1p, struct timeval *t2p);
char *pr_type(UBYTE t);
int packet_ok(UBYTE *buf, int cc, struct sockaddr_in *from, int seq);
void print(UBYTE *buf, int cc, struct sockaddr_in *from);
char *inetname(struct in_addr in);

#define MAXPACKET 65535 /* max ip packet size */

/*
 * format of a (udp) probe packet.
 */
struct opacket
{
    struct ip ip;
    struct udphdr udp;
    UBYTE seq;         /* sequence number of this packet */
    UBYTE ttl;         /* ttl packet left with */
    struct timeval tv; /* time packet left */
};

UBYTE packet[512];         /* last inbound (icmp) packet */
struct opacket *outpacket; /* last output (udp) packet */

int s;       /* receive (icmp) socket file descriptor */
int sndsock; /* send (udp) socket file descriptor */

struct sockaddr whereto; /* Who to try to reach */
int datalen;             /* How much data */

char *source = 0;
char *hostname;

int nprobes = 3;
int max_ttl = 30;
UWORD ident;
UWORD port = 32768 + 666; /* start udp dest port # for probe packets */
int options;              /* socket options */
int verbose;
int waittime = 5;         /* time to wait for response (in seconds) */
int nflag;                /* print addresses numerically */

/****** ROADSHOW/TRACEROUTE **************************************************
*
*   NAME
*	TRACEROUTE - print the route packets take to network host
*
*   FORMAT
*	TRACEROUTE [-m|MAXTTL <ttl>] [-n|NUMERIC] [-p|PORT <number>]
*	           [-q|QUERIES <number>] [-r|DONTROUTE] [-s|SOURCE <address>]
*	           [-t|TOS <type>] [-w|WAIT <time>] [-v|VERBOSE] [HOST <name>]
*	           [PACKETSIZE <size>]
*
*   TEMPLATE
*	-d=DEBUG/S,-m=MAXTTL/K/N,-n=NUMERIC/S,-p=PORT/K/N,-q=QUERIES/K/N,
*	-r=DONTROUTE/S,-s=SOURCE/K,-t=TOS/K/N,-v=VERBOSE/S,-w=WAIT/K/N,
*	HOST/A,PACKETSIZE/N
*
*   FUNCTION
*	The Internet is a large and complex aggregation of network hardware,
*	connected together by gateways. Tracking the route one's packets follow
*	(or finding the miscreant gateway that's discarding your packets) can be
*	difficult. Traceroute utilizes the IP protocol `time to live' field and
*	attempts to elicit an ICMP TIME_EXCEEDED response from each gateway along
*	the path to some host.
*
*	The only mandatory parameter is the destination host name or IP number.
*	The default probe datagram length is 38 bytes, but this may be increased
*	by specifying a packet size (in bytes) after the destination host name.
*
*   OPTIONS
*	-m, MAXTTL <ttl>
*	    Set the max time-to-live (max number of hops) used in outgoing
*	    probe packets. The default is 30 hops (the same default used for
*	    TCP connections).
*
*	-n, NUMERIC
*	    Print hop addresses numerically rather than symbolically and
*	    numerically (saves a nameserver address-to-name lookup for each
*	    gateway found on the path).
*
*	-p, PORT <number>
*	    Set the base UDP port number used in probes (default is 33434).
*	    Traceroute hopes that nothing is listening on UDP ports base to
*	    base+nhops-1 at the destination host (so an ICMP PORT_UNREACHABLE
*	    message will be returned to terminate the route tracing). If
*	    something is listening on a port in the default range, this
*	    option can be used to pick an unused port range.
*
*	-q, QUERIES <number>
*	    Set the number of probes per ``ttl'' (default is three probes).
*
*	-r, DONTROUTE
*	    Bypass the normal routing tables and send directly to a host on
*	    an attached network. If the host is not on a directly-attached
*	    network, an error is returned. This option can be used to ping a
*	    local host through an interface that has no route through it.
*
*	-s, SOURCE <address>
*	    Use the following IP address (which must be given as an IP
*	    number, not a hostname) as the source address in outgoing probe
*	    packets. On hosts with more than one IP address, this option can
*	    be used to force the source address to be something other than
*	    the IP address of the interface the probe packet is sent on. If
*	    the IP address is not one of this machine's interface addresses,
*	    an error is returned and nothing is sent.
*
*	-t, TOS <type>
*	    Set the type-of-service in probe packets to the following value
*	    (default zero). The value must be a decimal integer in the range
*	    0 to 255. This option can be used to see if different
*	    types-of-service result in different paths. Not all values of TOS
*	    are legal or meaningful - see the IP spec for definitions. Useful
*	    values are probably `-t 16' (low delay) and `-t 8' (high
*	    throughput).
*
*	-v, VERBOSE
*	    Verbose output. Received ICMP packets other than TIME_EXCEEDED
*	    and UNREACHABLEs are listed.
*
*	-w, WAIT <time>
*	    Set the time (in seconds) to wait for a response to a probe
*	    (default 3 sec.).
*
*   DESCRIPTION
*	This program attempts to trace the route an IP packet would follow to
*	some internet host by launching UDP probe packets with a small ttl
*	(time to live) then listening for an ICMP "time exceeded" reply from a
*	gateway. We start our probes with a ttl of one and increase by one
*	until we get an ICMP "port unreachable" (which means we got to "host")
*	or hit a max (which defaults to 30 hops & can be changed with the -m
*	flag). Three probes (changed with -q flag) are sent at each ttl
*	setting and a line is printed showing the ttl, address of the gateway
*	and round trip time of each probe. If the probe answers come from
*	different gateways, the address of each responding system will be
*	printed. If there is no response within the timeout interval (changed
*	with the -w flag), a "*" is printed for that probe.
*
*	We don't want the destination host to process the UDP probe packets so
*	the destination port is set to an unlikely value (if some clod on the
*	destination is using that value, it can be changed with the -p flag).
*
*	Traceroute prints a "!" after the time if the reply's ttl is <= 1 (a
*	clue that the target uses the arriving datagram's ttl in its reply, so
*	it is closer than it appears). Other possible annotations after the
*	time are !H, !N, !P (got a host, network or protocol unreachable,
*	respectively), !S or !F (source route failed or fragmentation needed -
*	neither of these should ever occur and the associated gateway is
*	busted if you see one). If almost all the probes result in some kind
*	of unreachable, traceroute will give up and exit.
*
*	This program is intended for use in network testing, measurement and
*	management. It should be used primarily for manual fault isolation.
*	Because of the load it could impose on the network, it is unwise to
*	use traceroute during normal operations or from automated scripts.
*
*   AUTHOR
*	Implemented by Van Jacobson from a suggestion by Steve Deering.
*	Debugged by a cast of thousands with particularly cogent suggestions
*	or fixes from C. Philip Wood, Tim Seaver and Ken Adelman.
******************************************************************************
*/

int main(int argc, char **argv)
{
    struct
    {
        SWITCH debug;
        NUMBER max_ttl;
        SWITCH numeric;
        NUMBER port;
        NUMBER queries;
        SWITCH dont_route;
        KEY source;
        NUMBER tos;
        SWITCH verbose;
        NUMBER wait;
        KEY host;
        NUMBER packet_size;
    } args;

    STRPTR args_template = (STRPTR)
        "-d=DEBUG/S,"
        "-m=MAXTTL/K/N,"
        "-n=NUMERIC/S,"
        "-p=PORT/K/N,"
        "-q=QUERIES/K/N,"
        "-r=DONTROUTE/S,"
        "-s=SOURCE/K,"
        "-t=TOS/K/N,"
        "-v=VERBOSE/S,"
        "-w=WAIT/K/N,"
        "HOST/A,"
        "PACKETSIZE/N"
        VERSTAG;

    (void)argc;

    memset(&args, 0, sizeof(args));

    rda = ReadArgs(args_template, (LONG *)&args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (STRPTR)argv[0]);
        exit(RETURN_FAIL);
    }

    atexit(close_bsdsocket); /* also frees rda */

    int on = 1;
    int seq = 0;
    int tos = 0;
    struct sockaddr_in *to = (struct sockaddr_in *)&whereto;

    if (args.debug)
        options |= SO_DEBUG;

    if (args.max_ttl != NULL)
    {
        max_ttl = (*args.max_ttl);
        if (max_ttl <= 1)
        {
            fprintf(stderr, "traceroute: max ttl must be >1.\n");
            exit(RETURN_ERROR);
        }
    }

    if (args.numeric)
        nflag++;

    if (args.port != NULL)
    {
        port = (*args.port);
        if ((*args.port) < 1)
        {
            fprintf(stderr, "traceroute: port must be >0.\n");
            exit(RETURN_ERROR);
        }
    }

    if (args.queries != NULL)
    {
        nprobes = (*args.queries);
        if (nprobes < 1)
        {
            fprintf(stderr, "traceroute: nprobes must be >0.\n");
            exit(RETURN_ERROR);
        }
    }

    if (args.dont_route)
        options |= SO_DONTROUTE;

    if (args.source != NULL)
    {
        /*
         * set the ip source address of the outbound
         * probe (e.g., on a multi-homed host).
         */
        source = (char *)args.source;
    }

    if (args.tos != NULL)
    {
        tos = (*args.tos);
        if (tos < 0 || tos > 255)
        {
            fprintf(stderr, "traceroute: tos must be 0 to 255.\n");
            exit(RETURN_ERROR);
        }
    }

    if (args.verbose)
        verbose++;

    if (args.wait != NULL)
    {
        waittime = (*args.wait);
        if (waittime <= 1)
        {
            fprintf(stderr, "traceroute: wait must be >1 sec.\n");
            exit(RETURN_ERROR);
        }
    }

    if (args.packet_size != NULL)
    {
        datalen = (*args.packet_size);
        if (datalen < 0 || datalen >= MAXPACKET - (int)sizeof(struct opacket))
        {
            fprintf(stderr, "traceroute: packet size must be 0 <= s < %ld.\n", (long)(MAXPACKET - sizeof(struct opacket)));
            exit(RETURN_ERROR);
        }
    }

    if (open_bsdsocket() != 0)
        exit(RETURN_FAIL);

    memset((char *)&whereto, 0, sizeof(struct sockaddr));
    to->sin_family = AF_INET;
    to->sin_addr.s_addr = inet_addr(args.host);

    if (to->sin_addr.s_addr != (~0UL))
    {
        hostname = (char *)args.host;
    }
    else
    {
        struct hostent *hp = gethostbyname(args.host);
        if (hp)
        {
            to->sin_family = hp->h_addrtype;
            memcpy(&to->sin_addr, hp->h_addr, hp->h_length);
            hostname = (char *)hp->h_name;
        }
        else
        {
            fprintf(stderr, "traceroute: unknown host %s\n", (char *)args.host);
            exit(RETURN_ERROR);
        }
    }

    datalen += sizeof(struct opacket);

    outpacket = (struct opacket *)malloc((unsigned)datalen);
    if (!outpacket)
    {
        perror("traceroute: malloc");
        exit(RETURN_FAIL);
    }

    memset((char *)outpacket, 0, datalen);
    outpacket->ip.ip_dst = to->sin_addr;
    outpacket->ip.ip_tos = tos;
    outpacket->ip.ip_v = IPVERSION;
    outpacket->ip.ip_id = 0;

    ident = (getpid() & 0xffff) | 0x8000;

    struct protoent *pe = getprotobyname((STRPTR)"icmp");
    if (pe == NULL)
    {
        fprintf(stderr, "icmp: unknown protocol\n");
        exit(RETURN_FAIL);
    }

    if ((s = socket(AF_INET, SOCK_RAW, pe->p_proto)) < 0)
    {
        perror("traceroute: icmp socket");
        exit(RETURN_FAIL);
    }

    /* This stack ignores SO_DEBUG and SO_DONTROUTE; the results are ignored
     * here exactly as the original did. */
    if (options & SO_DEBUG)
        setsockopt(s, SOL_SOCKET, SO_DEBUG, (char *)&on, sizeof(on));

    if (options & SO_DONTROUTE)
        setsockopt(s, SOL_SOCKET, SO_DONTROUTE, (char *)&on, sizeof(on));

    if ((sndsock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
    {
        perror("traceroute: raw socket");
        exit(RETURN_FAIL);
    }

    if (setsockopt(sndsock, SOL_SOCKET, SO_SNDBUF, (char *)&datalen, sizeof(datalen)) < 0)
    {
        perror("traceroute: SO_SNDBUF");
        exit(RETURN_FAIL);
    }

    if (setsockopt(sndsock, IPPROTO_IP, IP_HDRINCL, (char *)&on, sizeof(on)) < 0)
    {
        perror("traceroute: IP_HDRINCL");
        exit(RETURN_FAIL);
    }

    if (options & SO_DEBUG)
        setsockopt(sndsock, SOL_SOCKET, SO_DEBUG, (char *)&on, sizeof(on));

    if (options & SO_DONTROUTE)
        setsockopt(sndsock, SOL_SOCKET, SO_DONTROUTE, (char *)&on, sizeof(on));

    if (source)
    {
        struct sockaddr_in from;

        memset((char *)&from, 0, sizeof(struct sockaddr));

        from.sin_family = AF_INET;
        from.sin_addr.s_addr = inet_addr((STRPTR)source);

        if (from.sin_addr.s_addr == (~0UL))
        {
            printf("traceroute: unknown host %s\n", source);
            exit(RETURN_ERROR);
        }

        outpacket->ip.ip_src = from.sin_addr;
    }

    fprintf(stderr, "traceroute to %s (%s)", hostname, inet_ntoa(to->sin_addr));

    if (source)
        fprintf(stderr, " from %s", source);

    fprintf(stderr, ", %d hops max, %d byte packets\n", max_ttl, datalen);

    fflush(stderr);

    for (int ttl = 1; ttl <= max_ttl; ++ttl)
    {
        ULONG lastaddr = 0;
        int got_there = 0;
        int unreachable = 0;

        printf("%2d ", ttl);

        for (int probe = 0; probe < nprobes; ++probe)
        {
            int cc;
            struct timeval t1, t2;
            struct sockaddr_in from;

            gettimeofday(&t1, &tz);
            send_probe(++seq, ttl);

            while ((cc = wait_for_reply(s, &from)) != 0)
            {
                gettimeofday(&t2, &tz);

                int i = packet_ok(packet, cc, &from, seq);
                if (i != 0)
                {
                    if (from.sin_addr.s_addr != lastaddr)
                    {
                        print(packet, cc, &from);
                        lastaddr = from.sin_addr.s_addr;
                    }

                    ULONG dt = deltaT(&t1, &t2);
                    printf("  %lu.%03lu ms", dt / 1000, dt % 1000);

                    switch (i - 1)
                    {
                    case ICMP_UNREACH_PORT:
                    {
                        struct ip *ip = (struct ip *)packet;
                        if (ip->ip_ttl <= 1)
                            printf(" !");

                        ++got_there;
                        break;
                    }

                    case ICMP_UNREACH_NET:
                        ++unreachable;
                        printf(" !N");
                        break;

                    case ICMP_UNREACH_HOST:
                        ++unreachable;
                        printf(" !H");
                        break;

                    case ICMP_UNREACH_PROTOCOL:
                        ++got_there;
                        printf(" !P");
                        break;

                    case ICMP_UNREACH_NEEDFRAG:
                        ++unreachable;
                        printf(" !F");
                        break;

                    case ICMP_UNREACH_SRCFAIL:
                        ++unreachable;
                        printf(" !S");
                        break;
                    }

                    break;
                }
            }

            if (cc == 0)
                printf(" *");

            fflush(stdout);
        }

        putchar('\n');

        if (got_there || unreachable >= nprobes - 1)
            exit(RETURN_OK);
    }

    return RETURN_OK;
}

int wait_for_reply(int sock, struct sockaddr_in *from)
{
    fd_set fds;
    struct timeval wait;
    int cc = 0;
    int fromlen = sizeof(*from);

    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    wait.tv_secs = waittime;
    wait.tv_micro = 0;

    int n = select(sock + 1, &fds, (fd_set *)0, (fd_set *)0, &wait);
    if (n > 0)
        cc = recvfrom(s, (char *)packet, sizeof(packet), 0, (struct sockaddr *)from, (void *)&fromlen);
    else if (n < 0 && errno == EINTR)
        finish(); /* Ctrl-C */

    return (cc);
}

void send_probe(int seq, int ttl)
{
    struct opacket *op = outpacket;
    struct ip *ip = &op->ip;
    struct udphdr *up = &op->udp;

    ip->ip_off = 0;
    ip->ip_hl = sizeof(*ip) >> 2;
    ip->ip_p = IPPROTO_UDP;
    ip->ip_len = datalen;
    ip->ip_ttl = ttl;
    ip->ip_v = IPVERSION;
    ip->ip_id = htons(ident + seq);

    up->uh_sport = htons(ident);
    up->uh_dport = htons(port + seq);
    up->uh_ulen = htons((UWORD)(datalen - sizeof(struct ip)));
    up->uh_sum = 0;

    op->seq = seq;
    op->ttl = ttl;
    gettimeofday(&op->tv, &tz);

    int i = sendto(sndsock, (char *)outpacket, datalen, 0, &whereto, sizeof(struct sockaddr));
    if (i < 0 || i != datalen)
    {
        if (i < 0)
            perror("sendto");

        printf("traceroute: wrote %s %d chars, ret=%d\n", hostname, datalen, i);

        fflush(stdout);
    }
}

/* elapsed time between two EClock samples, in microseconds */
ULONG deltaT(struct timeval *t1p, struct timeval *t2p)
{
    struct timeval delta;

    delta.tv_secs = t2p->tv_secs - t1p->tv_secs;

    if (t2p->tv_micro < t1p->tv_micro)
    {
        /* Borrow one second. */
        delta.tv_secs--;
        delta.tv_micro = 1000000 - t1p->tv_micro + t2p->tv_micro;
    }
    else
    {
        delta.tv_micro = t2p->tv_micro - t1p->tv_micro;
    }

    return delta.tv_secs * 1000000 + delta.tv_micro;
}

/*
 * Convert an ICMP "type" field to a printable string.
 */
char *pr_type(UBYTE t)
{
    static char *ttab[] =
    {
        "Echo Reply", "ICMP 1", "ICMP 2", "Dest Unreachable",
        "Source Quench", "Redirect", "ICMP 6", "ICMP 7",
        "Echo", "ICMP 9", "ICMP 10", "Time Exceeded",
        "Param Problem", "Timestamp", "Timestamp Reply", "Info Request",
        "Info Reply"
    };

    if (t > 16)
        return ("OUT-OF-RANGE");

    return (ttab[t]);
}

int packet_ok(UBYTE *buf, int cc, struct sockaddr_in *from, int seq)
{
    struct ip *ip = (struct ip *)buf;
    int hlen = ip->ip_hl << 2;

    if (cc < hlen + ICMP_MINLEN)
    {
        if (verbose)
            printf("packet too short (%d bytes) from %s\n", cc, inet_ntoa(from->sin_addr));

        return (0);
    }

    cc -= hlen;

    struct icmp *icp = (struct icmp *)(buf + hlen);
    UBYTE type = icp->icmp_type;
    UBYTE code = icp->icmp_code;

    if ((type == ICMP_TIMXCEED && code == ICMP_TIMXCEED_INTRANS) || type == ICMP_UNREACH)
    {
        struct ip *hip = &icp->icmp_ip;
        hlen = hip->ip_hl << 2;
        struct udphdr *up = (struct udphdr *)((UBYTE *)hip + hlen);

        if (hlen + 12 <= cc && hip->ip_p == IPPROTO_UDP && up->uh_sport == htons(ident) && up->uh_dport == htons(port + seq))
            return (type == ICMP_TIMXCEED ? -1 : code + 1);
    }

    if (verbose)
    {
        ULONG *lp = (ULONG *)&icp->icmp_ip;

        printf("\n%d bytes from %s to %s", cc, inet_ntoa(from->sin_addr), inet_ntoa(ip->ip_dst));
        printf(": icmp type %d (%s) code %d\n", type, pr_type(type), icp->icmp_code);
        for (int i = 4; i < cc; i += sizeof(long))
            printf("%2d: x%8.8lx\n", i, *lp++);
    }

    return (0);
}

void print(UBYTE *buf, int cc, struct sockaddr_in *from)
{
    struct ip *ip = (struct ip *)buf;
    int hlen = ip->ip_hl << 2;

    cc -= hlen;

    if (nflag)
        printf(" %s", inet_ntoa(from->sin_addr));
    else
        printf(" %s (%s)", inetname(from->sin_addr), inet_ntoa(from->sin_addr));

    if (verbose)
        printf(" %d bytes to %s", cc, inet_ntoa(ip->ip_dst));
}

static int _stricmp(const char *s1, const char *s2)
{
    while (tolower(*s1) == tolower(*s2))
    {
        if ((*s1) == '\0')
            break;

        s1++;
        s2++;
    }

    /* The comparison must be performed as if the
       characters were unsigned characters. */
    int c1 = tolower(*(unsigned char *)s1);
    int c2 = tolower(*(unsigned char *)s2);

    return (c1 - c2);
}

#define strcmp(a, b) _stricmp(a, b)

#define C(x) ((x) & 0xff)

/*
 * Construct an Internet address representation.
 * If the nflag has been supplied, give
 * numeric value, otherwise try for symbolic name.
 */
char *inetname(struct in_addr in)
{
    static char line[MAXHOSTNAMELEN + 1];
    static char domain[MAXHOSTNAMELEN + 1];
    static int first = 1;
    char *cp;

    if (first && !nflag)
    {
        first = 0;

        if (gethostname((STRPTR)domain, sizeof(domain) - 1) == 0 && (cp = strchr(domain, '.')) != NULL)
            memmove(domain, cp + 1, strlen(cp + 1) + 1);
        else
            domain[0] = '\0';
    }

    cp = NULL;

    if (!nflag && in.s_addr != INADDR_ANY)
    {
        struct hostent *hp = gethostbyaddr((STRPTR)&in, sizeof(in), AF_INET);
        if (hp != NULL)
        {
            cp = strchr((char *)hp->h_name, '.');
            if (cp != NULL)
            {
                if (strcmp(cp + 1, domain) == 0)
                    (*cp) = '\0';
            }

            cp = (char *)hp->h_name;
        }
    }

    if (cp != NULL)
    {
        int len = strlen(cp);
        if (len > (int)sizeof(line) - 1)
            len = sizeof(line) - 1;

        memcpy(line, cp, len);
        line[len] = '\0';
    }
    else
    {
        in.s_addr = ntohl(in.s_addr);

        sprintf(line, "%lu.%lu.%lu.%lu",
                C(in.s_addr >> 24),
                C(in.s_addr >> 16),
                C(in.s_addr >> 8),
                C(in.s_addr));
    }

    return (line);
}
