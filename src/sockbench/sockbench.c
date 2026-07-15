/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sockbench — repeatable LAN TCP throughput benchmark over bsdsocket.library.
 *
 * Runs against scripts/tcp-bench-peer.py (start it on a PC; port 5001 discards
 * what we send, port 5002 blasts data at us):
 *
 *   sockbench rx|tx <host> [streams] [seconds] [bufKB]
 *
 * N nonblocking sockets driven from one WaitSelect loop; per-stream and
 * aggregate Mb/s from ReadEClock. Defaults: 1 stream, 10 s, 64 KB buffer.
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

#include <debug.h>

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */
struct Device *TimerBase;

/* bsdsocket.library reports BSD errno numbering; this NDK's netinclude ships no
 * errno constants, and Roadshow's Errno() is independent of the C library's. */
#define SB_EWOULDBLOCK 35

#define BENCH_SINK_PORT 5001   /* peer discards what we send (tx test) */
#define BENCH_SOURCE_PORT 5002 /* peer blasts at us (rx test) */
#define BENCH_MAX_STREAMS 8

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

static int run_bench(int argc, char **argv)
{
    /* argv: [0]=sockbench [1]=rx|tx [2]=host [3]=streams [4]=secs [5]=bufKB */
    if (argc < 3 || (strcmp(argv[1], "rx") != 0 && strcmp(argv[1], "tx") != 0))
    {
        printf("usage: sockbench rx|tx <host> [streams<=%d] [seconds] [bufKB]\n",
               BENCH_MAX_STREAMS);
        return 5;
    }
    BOOL rx = (strcmp(argv[1], "rx") == 0);
    LONG streams = (argc > 3) ? atoi(argv[3]) : 1;
    LONG seconds = (argc > 4) ? atoi(argv[4]) : 10;
    LONG bufKB = (argc > 5) ? atoi(argv[5]) : 64;
    if (streams < 1 || streams > BENCH_MAX_STREAMS || seconds < 1 || bufKB < 1 || bufKB > 512)
    {
        printf("sockbench: bad parameters\n");
        return 5;
    }
    ULONG buflen = (ULONG)bufKB * 1024;

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
        printf("sockbench: no memory for %lu KB buffer\n", (unsigned long)bufKB);
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
        sock[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (sock[i] < 0 || connect(sock[i], (struct sockaddr *)&sin, sizeof(sin)) != 0)
        {
            printf("sockbench: stream %ld connect failed, errno %ld\n", (long)i, (long)Errno());
            goto bench_close;
        }
        LONG one = 1;
        IoctlSocket(sock[i], FIONBIO, &one);
    }
    if (rx)
    {
        /* start gun — the peer's source blasts only after this byte, so no
         * stream floods the RX pool while later handshakes are still in
         * flight (a dry pool eats SYN-ACKs; seen live as errno 53 with 4
         * streams). One byte always fits the fresh send buffer; content is
         * irrelevant, the peer just waits for it. */
        buf[0] = 'G';
        for (LONG i = 0; i < streams; i++)
            send(sock[i], buf, 1, 0);
    }
    printf("bench %s: %ld stream(s) to port %u, %ld s, %ld KB buffer\n",
           rx ? "rx" : "tx", (long)streams, port, (long)seconds, (long)bufKB);

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    unsigned long long start = bench_now();
    unsigned long long deadline = start + (unsigned long long)seconds * freq;
    unsigned long long now = start;
    LONG active = streams;

    while (active > 0 && (now = bench_now()) < deadline)
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
                if (r == 0 || Errno() != SB_EWOULDBLOCK)
                {
                    if (r != 0)
                        printf("sockbench: stream %ld errno %ld\n", (long)i, (long)Errno());
                    CloseSocket(sock[i]);
                    sock[i] = -1;
                    active--;
                }
                break;
            }
        }
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
        printf("(tx counts bytes accepted into send buffers; ~256 KB/stream tail margin)\n");
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

int main(int argc, char **argv)
{
    Kprintf("[sockbench] %s: argc=%ld\n", __func__, (LONG)argc);
    return run_bench(argc, argv);
}
