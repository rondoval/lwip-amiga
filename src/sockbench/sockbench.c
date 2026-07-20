/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sockbench — repeatable LAN TCP/UDP throughput benchmark over bsdsocket.library.
 *
 * Runs against scripts/tcp-bench-peer.py (start it on a PC; port 5001 discards
 * what we send, port 5002 blasts data at us — same ports for TCP and UDP):
 *
 *   sockbench rx|tx|txblock|udprx|udptx <host> [streams] [seconds] [sizeKB]
 *
 * rx/tx are TCP; udprx/udptx are UDP. N nonblocking sockets driven from one
 * WaitSelect loop; per-stream and aggregate Mb/s from ReadEClock. Defaults:
 * 1 stream, 10 s; TCP 64 KB buffer, UDP one 1500-MTU frame per datagram.
 * For UDP the size arg sets the datagram size (fragmented above one frame),
 * and the aggregate is best-effort — the peer's own count shows any loss.
 *
 * txblock is a diagnostic mode, not a benchmark: one BLOCKING socket driven by
 * a tight send() loop with no WaitSelect, mirroring AmiSpeedTest's upload test.
 * It exists because the nonblocking modes cannot reach sb_tcp_send's blocking
 * path at all — they take the dontwait early-out on every would-block, so they
 * never loop over a multi-chunk write, never sleep in sb_send_block, and never
 * hit the mid-write lock break that re-enters lwIP with a half-built unsent
 * tail.
 *
 * A developer tool, so it just uses the NDK bsdsocket headers (BSD sockets +
 * the library's inline glue) rather than open-coding LVO stubs.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/libraries.h>
#include <devices/timer.h>

#include <sys/socket.h>  /* AF_INET, SOCK_STREAM, fd_set, FD_* */
#include <sys/filio.h>   /* FIONBIO */
#include <netinet/in.h>  /* struct sockaddr_in, INADDR_NONE, htons */
#include <netdb.h>       /* struct hostent */

#include <proto/exec.h>
#include <proto/socket.h> /* bsdsocket.library: socket/connect/send/recv/... */
#define TIMER_BASE_NAME TimerBase
#include <proto/timer.h>

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */
struct Device *TimerBase;

/* bsdsocket.library reports BSD errno numbering; this NDK's netinclude ships no
 * errno constants, and Roadshow's Errno() is independent of the C library's. */
#define SB_EWOULDBLOCK 35

#define BENCH_SINK_PORT 5001   /* peer discards what we send (tx test) */
#define BENCH_SOURCE_PORT 5002 /* peer blasts at us (rx test) */
#define BENCH_MAX_STREAMS 8
#define BENCH_UDP_DGRAM 1472   /* default UDP payload: one 1500-MTU frame */

static ULONG bench_resolve(const char *host)
{
    ULONG addr = inet_addr((STRPTR)host);
    if (addr != INADDR_NONE)
        return addr;
    struct hostent *he = gethostbyname((STRPTR)host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
        return INADDR_NONE;
    return *(ULONG *)he->h_addr_list[0];
}

static unsigned long long bench_now(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
}

static void bench_report(const char *tag, unsigned long long bytes,
                         unsigned long long ticks, ULONG freq)
{
    /* decimal without printf %llu (libnix support is not a given) */
    char dec[22];
    char *p = dec + 21;
    unsigned long long v = bytes;
    *--p = '\0';
    do
    {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);

    /* Mb/s ×10 without float: bits * freq / ticks / 1e5 */
    unsigned long long mbit_x10 = 0;
    if (ticks != 0)
        mbit_x10 = bytes * 8ULL / 100000ULL * freq / ticks;
    printf("%s: %s bytes, %lu.%lu Mb/s\n", tag, p,
           (unsigned long)(mbit_x10 / 10), (unsigned long)(mbit_x10 % 10));
}

int main(int argc, char **argv)
{
    /* argv: [0]=sockbench [1]=rx|tx|udprx|udptx [2]=host [3]=streams [4]=secs [5]=sizeKB */
    const char *dir = (argc > 1) ? argv[1] : "";
    BOOL rx = FALSE, udp = FALSE, blocking = FALSE, dir_ok = TRUE;
    if (strcmp(dir, "rx") == 0)         rx = TRUE;
    else if (strcmp(dir, "tx") == 0)    rx = FALSE;
    else if (strcmp(dir, "txblock") == 0) { rx = FALSE; blocking = TRUE; }
    else if (strcmp(dir, "udprx") == 0) { rx = TRUE;  udp = TRUE; }
    else if (strcmp(dir, "udptx") == 0) { rx = FALSE; udp = TRUE; }
    else                                dir_ok = FALSE;

    if (!dir_ok || argc < 3)
    {
        printf("usage: sockbench rx|tx|txblock|udprx|udptx <host> [streams<=%d] [seconds] [sizeKB]\n"
               "  rx/tx = TCP; udprx/udptx = UDP. sizeKB is the TCP buffer or the UDP\n"
               "  datagram size (default: TCP 64 KB, UDP one 1500-MTU frame).\n"
               "  txblock = diagnostic: 1 blocking socket, tight send() loop, 32 KB\n"
               "  default — reproduces AmiSpeedTest's upload path.\n",
               BENCH_MAX_STREAMS);
        return 5;
    }
    LONG streams = (argc > 3) ? atoi(argv[3]) : 1;
    LONG seconds = (argc > 4) ? atoi(argv[4]) : 10;
    LONG sizeKB = (argc > 5) ? atoi(argv[5]) : 0; /* 0 = per-protocol default */
    if (streams < 1 || streams > BENCH_MAX_STREAMS || seconds < 1)
    {
        printf("sockbench: bad parameters\n");
        return 5;
    }
    /* A blocking send parks the whole task, so one task can only drive one
     * stream; multiplexing would need the WaitSelect loop we are avoiding. */
    if (blocking && streams != 1)
    {
        printf("sockbench: txblock drives a single stream\n");
        return 5;
    }
    /* bytes per send()/recv(): a TCP working buffer, or one UDP datagram */
    ULONG buflen;
    if (udp)
    {
        buflen = (sizeKB > 0) ? (ULONG)sizeKB * 1024 : BENCH_UDP_DGRAM;
        if (buflen > 65507)
            buflen = 65507; /* max UDP payload */
    }
    else
    {
        /* 32 KB to match AmiSpeedTest's g_appBufLen: big enough that a single
         * write splits into several 16 KB chunks inside sb_tcp_send, which is
         * what puts the lock break in the middle of the write. */
        LONG bufKB = (sizeKB > 0) ? sizeKB : (blocking ? 32 : 64);
        if (bufKB > 512)
        {
            printf("sockbench: bad parameters\n");
            return 5;
        }
        buflen = (ULONG)bufKB * 1024;
    }

    /* args are good — now bring the stack up (OpenLibrary starts DHCP) */
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        printf("sockbench: cannot open bsdsocket.library v4\n");
        return 20;
    }

    int rc = 10;
    LONG sock[BENCH_MAX_STREAMS];
    unsigned long long bytes[BENCH_MAX_STREAMS];
    UBYTE *buf = NULL;
    for (LONG i = 0; i < BENCH_MAX_STREAMS; i++)
    {
        sock[i] = -1;
        bytes[i] = 0;
    }

    /* timer.device just for ReadEClock */
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = (struct timerequest *)CreateIORequest(tp, sizeof(struct timerequest));
    if (tr == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &tr->tr_node, 0) != 0)
    {
        printf("sockbench: no timer.device\n");
        if (tr != NULL)
            DeleteIORequest(&tr->tr_node);
        if (tp != NULL)
            DeleteMsgPort(tp);
        CloseLibrary(SocketBase);
        return 10;
    }
    TimerBase = tr->tr_node.io_Device;

    ULONG addr = bench_resolve(argv[2]);
    if (addr == INADDR_NONE)
    {
        printf("sockbench: cannot resolve %s\n", argv[2]);
        goto bench_out;
    }

    buf = AllocVec(buflen, MEMF_PUBLIC);
    if (buf == NULL)
    {
        printf("sockbench: no memory for %lu byte buffer\n", (unsigned long)buflen);
        goto bench_out;
    }
    if (!rx)
        memset(buf, 'x', buflen);

    UWORD port = rx ? BENCH_SOURCE_PORT : BENCH_SINK_PORT;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = addr;

    for (LONG i = 0; i < streams; i++)
    {
        sock[i] = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
        /* connect() on a UDP socket just pins the peer (no handshake), so
         * send()/recv() work for both and recv() accepts only the peer. */
        if (sock[i] < 0 || connect(sock[i], (struct sockaddr *)&sin, sizeof(sin)) != 0)
        {
            printf("sockbench: stream %ld connect failed, errno %ld\n", (long)i, (long)Errno());
            goto bench_close;
        }
        if (!blocking)
        {
            LONG one = 1;
            IoctlSocket(sock[i], FIONBIO, &one);
        }
    }
    if (rx)
    {
        /* start gun — the peer's source blasts only after this, so no stream
         * floods the RX pool while later handshakes are still in flight (a dry
         * pool eats SYN-ACKs; seen live as errno 53 with 4 TCP streams). TCP
         * sends one 'G' byte; UDP sends "G<bytes>" so the peer also learns
         * where to send and how big to make each datagram. */
        LONG glen = 1;
        buf[0] = 'G';
        if (udp)
            glen = sprintf((char *)buf, "G%lu", (unsigned long)buflen);
        for (LONG i = 0; i < streams; i++)
            send(sock[i], buf, glen, 0);
    }
    printf("bench %s %s: %ld stream(s) to port %u, %ld s, %lu byte %s\n",
           udp ? "udp" : "tcp", blocking ? "txblock" : rx ? "rx" : "tx",
           (long)streams, port, (long)seconds,
           (unsigned long)buflen, udp ? "datagrams" : "buffer");

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    unsigned long long start = bench_now();
    unsigned long long deadline = start + (unsigned long long)seconds * freq;
    unsigned long long now = start;
    LONG active = streams;

    /* Blocking mode: no readiness loop at all. send() returns only once the
     * whole buffer is queued, so the task sits inside sb_tcp_send across
     * several chunks, sleeping in sb_send_block whenever the window closes.
     * That is precisely the path the nonblocking modes skip. */
    while (blocking && active > 0 && (now = bench_now()) < deadline)
    {
        LONG r = send(sock[0], buf, (LONG)buflen, 0);
        if (r > 0)
        {
            bytes[0] += (unsigned long long)r;
            continue;
        }
        /* Blocking send has no EWOULDBLOCK to forgive: r<=0 is the end. */
        if (r < 0)
            printf("sockbench: stream 0 errno %ld\n", (long)Errno());
        CloseSocket(sock[0]);
        sock[0] = -1;
        active--;
    }

    while (!blocking && active > 0 && (now = bench_now()) < deadline)
    {
        fd_set set;
        FD_ZERO(&set);
        LONG maxfd = -1;
        for (LONG i = 0; i < streams; i++)
        {
            if (sock[i] < 0)
                continue;
            FD_SET(sock[i], &set);
            if (sock[i] > maxfd)
                maxfd = sock[i];
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000; /* recheck the deadline even when idle */
        LONG n = WaitSelect(maxfd + 1, rx ? &set : NULL, rx ? NULL : &set, NULL, &tv, NULL);
        if (n < 0)
        {
            printf("sockbench: WaitSelect errno %ld\n", (long)Errno());
            break;
        }
        if (n == 0)
            continue;

        for (LONG i = 0; i < streams; i++)
        {
            if (sock[i] < 0 || !FD_ISSET(sock[i], &set))
                continue;

            /* drain/fill a few rounds per readiness, then yield to the others */
            for (LONG round = 0; round < 8; round++)
            {
                LONG r = rx ? recv(sock[i], buf, (LONG)buflen, 0)
                            : send(sock[i], buf, (LONG)buflen, 0);
                if (r > 0)
                {
                    bytes[i] += (unsigned long long)r;
                    if ((ULONG)r < buflen)
                        break;
                    continue;
                }
                /* r<=0: EWOULDBLOCK means "nothing more right now" (retry on
                 * the next readiness). A real error, or a TCP r==0 (EOF), ends
                 * the stream; a UDP r==0 is just an empty datagram, so keep it. */
                BOOL failed = (r < 0 && Errno() != SB_EWOULDBLOCK);
                if (failed || (r == 0 && !udp))
                {
                    if (failed)
                        printf("sockbench: stream %ld errno %ld\n", (long)i, (long)Errno());
                    CloseSocket(sock[i]);
                    sock[i] = -1;
                    active--;
                }
                break;
            }
        }
    }

    /* UDP has no close handshake: nudge the peer to stop and report its side.
     * Sent a few times per stream, since a lone datagram may be dropped. */
    if (udp)
    {
        buf[0] = 'S';
        for (LONG i = 0; i < streams; i++)
            if (sock[i] >= 0)
                for (LONG k = 0; k < 3; k++)
                    send(sock[i], buf, 1, 0);
    }

    unsigned long long ticks = now - start;
    unsigned long long total = 0;
    for (LONG i = 0; i < streams; i++)
    {
        char tag[16];
        sprintf(tag, "  stream %ld", (long)i);
        bench_report(tag, bytes[i], ticks, freq);
        total += bytes[i];
    }
    bench_report("total", total, ticks, freq);
    if (!rx)
        printf(udp ? "(udp tx counts datagrams the stack accepted; the peer's count shows loss)\n"
                   : "(tx counts bytes accepted into send buffers; ~256 KB/stream tail margin)\n");
    rc = 0;

bench_close:
    for (LONG i = 0; i < streams; i++)
        if (sock[i] >= 0)
            CloseSocket(sock[i]);

bench_out:
    if (buf != NULL)
        FreeVec(buf);
    CloseDevice(&tr->tr_node);
    DeleteIORequest(&tr->tr_node);
    DeleteMsgPort(tp);
    CloseLibrary(SocketBase);
    return rc;
}
