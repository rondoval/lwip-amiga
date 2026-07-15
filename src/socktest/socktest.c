/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * socktest — exercise bsdsocket.library end to end from the app side:
 * OpenLibrary starts the stack (DHCP), gethostbyname resolves over lwIP DNS,
 * then a TCP connect/send/recv round trip.
 *
 *   socktest <host> <port> [request text]
 *   socktest example.com 80              (sends a HTTP/1.0 GET)
 *
 * Exercises: per-opener base, errno, DNS blocking, TCP connect/send/recv
 * blocking paths, orderly close.
 *
 * Bench mode — repeatable LAN throughput against scripts/tcp-bench-peer.py
 * (run it on a PC; port 5001 discards, port 5002 blasts):
 *
 *   socktest bench rx|tx <host> [streams] [seconds] [bufKB]
 *
 * N nonblocking sockets driven from one WaitSelect loop; per-stream and
 * aggregate Mb/s from ReadEClock. Defaults: 1 stream, 10 s, 64 KB buffer.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <devices/timer.h>
#include <exec/types.h>
#include <exec/libraries.h>

#include <proto/exec.h>
#define TIMER_BASE_NAME TimerBase
#include <proto/timer.h>

#include <debug.h>

struct Device *TimerBase;

/* ---- minimal inline glue (NDK inline-header idiom, LVOs from the sfd) ---- */

static struct Library *SocketBase;

#define SB_JSR(offs) "jsr a6@(-" #offs ":W)"

static inline LONG sb_socket(LONG dom, LONG ty, LONG pr)
{
    KprintfH("[socktest] %s: dom=%ld ty=%ld pr=%ld\n", __func__, dom, ty, pr);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = dom;
    register LONG d1 __asm("d1") = ty;
    register LONG d2 __asm("d2") = pr;
    __asm volatile(SB_JSR(30) : "=r"(res) : "r"(a6), "0"(d0), "r"(d1), "r"(d2) : "a0", "a1", "cc", "memory");
    return res;
}

static inline LONG sb_connect(LONG s, void *name, LONG len)
{
    KprintfH("[socktest] %s: s=%ld len=%ld\n", __func__, s, len);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = name;
    register LONG d1 __asm("d1") = len;
    __asm volatile(SB_JSR(54) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_send(LONG s, void *buf, LONG len, LONG flags)
{
    KprintfH("[socktest] %s: s=%ld len=%ld flags=%ld\n", __func__, s, len, flags);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = buf;
    register LONG d1 __asm("d1") = len;
    register LONG d2 __asm("d2") = flags;
    __asm volatile(SB_JSR(66) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1), "r"(d2) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_recv(LONG s, void *buf, LONG len, LONG flags)
{
    KprintfH("[socktest] %s: s=%ld len=%ld flags=%ld\n", __func__, s, len, flags);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = buf;
    register LONG d1 __asm("d1") = len;
    register LONG d2 __asm("d2") = flags;
    __asm volatile(SB_JSR(78) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1), "r"(d2) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_CloseSocket(LONG s)
{
    KprintfH("[socktest] %s: s=%ld\n", __func__, s);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    __asm volatile(SB_JSR(120) : "=r"(res) : "r"(a6), "0"(d0) : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static inline LONG sb_Errno(void)
{
    KprintfH("[socktest] %s\n", __func__);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    __asm volatile(SB_JSR(162) : "=r"(res) : "r"(a6) : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static inline ULONG sb_inet_addr(const char *cp)
{
    KprintfH("[socktest] %s: cp=0x%08lx\n", __func__, (ULONG)cp);
    register struct Library *a6 __asm("a6") = SocketBase;
    register ULONG res __asm("d0");
    register const char *a0 __asm("a0") = cp;
    __asm volatile(SB_JSR(180) : "=r"(res) : "r"(a6), "r"(a0) : "d1", "a1", "cc", "memory");
    return res;
}

struct sb_hostent
{
    char *h_name;
    char **h_aliases;
    LONG h_addrtype;
    LONG h_length;
    char **h_addr_list;
};

static inline struct sb_hostent *sb_gethostbyname(const char *name)
{
    KprintfH("[socktest] %s: name=0x%08lx\n", __func__, (ULONG)name);
    register struct Library *a6 __asm("a6") = SocketBase;
    register struct sb_hostent *res __asm("d0");
    register const char *a0 __asm("a0") = name;
    __asm volatile(SB_JSR(210) : "=r"(res) : "r"(a6), "r"(a0) : "d1", "a1", "cc", "memory");
    return res;
}

static inline LONG sb_IoctlSocket(LONG s, ULONG req, void *argp)
{
    KprintfH("[socktest] %s: s=%ld req=0x%lx\n", __func__, s, req);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register ULONG d1 __asm("d1") = req;
    register void *a0 __asm("a0") = argp;
    __asm volatile(SB_JSR(114) : "=r"(res) : "r"(a6), "0"(d0), "r"(d1), "r"(a0) : "a1", "cc", "memory");
    return res;
}

struct sb_timeval
{
    ULONG tv_sec;
    ULONG tv_usec;
};

static inline LONG sb_WaitSelect(LONG nfds, ULONG *rd, ULONG *wr, ULONG *ex,
                                 struct sb_timeval *to, ULONG *sigs)
{
    KprintfH("[socktest] %s: nfds=%ld\n", __func__, nfds);
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = nfds;
    register ULONG *a0 __asm("a0") = rd;
    register ULONG *a1 __asm("a1") = wr;
    register ULONG *a2 __asm("a2") = ex;
    register struct sb_timeval *a3 __asm("a3") = to;
    register ULONG *d1 __asm("d1") = sigs;
    __asm volatile(SB_JSR(126)
                   : "=r"(res)
                   : "r"(a6), "0"(d0), "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(d1)
                   : "cc", "memory");
    return res;
}

/* ---- the test ---- */

struct sb_sockaddr_in
{
    UBYTE sin_len;
    UBYTE sin_family;
    UWORD sin_port;
    ULONG sin_addr;
    UBYTE sin_zero[8];
};

/* ---- bench mode ---- */

#define BENCH_SINK_PORT 5001   /* peer discards what we send (tx test) */
#define BENCH_SOURCE_PORT 5002 /* peer blasts at us (rx test) */
#define BENCH_MAX_STREAMS 8
#define SB_FIONBIO 0x8004667EUL
#define SB_ERR_EWOULDBLOCK 35

static ULONG bench_resolve(const char *host)
{
    ULONG addr = sb_inet_addr(host);
    if (addr != 0xFFFFFFFF)
        return addr;
    struct sb_hostent *he = sb_gethostbyname(host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
        return 0xFFFFFFFF;
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

static int bench_main(int argc, char **argv)
{
    /* argv: [0]="bench" [1]=rx|tx [2]=host [3]=streams [4]=secs [5]=bufKB */
    if (argc < 3 || (strcmp(argv[1], "rx") != 0 && strcmp(argv[1], "tx") != 0))
    {
        printf("usage: socktest bench rx|tx <host> [streams<=%d] [seconds] [bufKB]\n",
               BENCH_MAX_STREAMS);
        return 5;
    }
    BOOL rx = (strcmp(argv[1], "rx") == 0);
    LONG streams = (argc > 3) ? atoi(argv[3]) : 1;
    LONG seconds = (argc > 4) ? atoi(argv[4]) : 10;
    LONG bufKB = (argc > 5) ? atoi(argv[5]) : 64;
    if (streams < 1 || streams > BENCH_MAX_STREAMS || seconds < 1 || bufKB < 1 || bufKB > 512)
    {
        printf("socktest: bad bench parameters\n");
        return 5;
    }
    ULONG buflen = (ULONG)bufKB * 1024;

    int rc = 10;
    LONG fds[BENCH_MAX_STREAMS];
    unsigned long long bytes[BENCH_MAX_STREAMS];
    UBYTE *buf = NULL;
    for (LONG i = 0; i < BENCH_MAX_STREAMS; i++)
    {
        fds[i] = -1;
        bytes[i] = 0;
    }

    /* timer.device just for ReadEClock */
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = (struct timerequest *)CreateIORequest(tp, sizeof(struct timerequest));
    if (tr == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &tr->tr_node, 0) != 0)
    {
        printf("socktest: no timer.device\n");
        if (tr != NULL)
            DeleteIORequest(&tr->tr_node);
        if (tp != NULL)
            DeleteMsgPort(tp);
        return 10;
    }
    TimerBase = tr->tr_node.io_Device;

    ULONG addr = bench_resolve(argv[2]);
    if (addr == 0xFFFFFFFF)
    {
        printf("socktest: cannot resolve %s\n", argv[2]);
        goto bench_out;
    }

    buf = AllocVec(buflen, MEMF_PUBLIC);
    if (buf == NULL)
    {
        printf("socktest: no memory for %lu KB buffer\n", (unsigned long)bufKB);
        goto bench_out;
    }
    if (!rx)
        memset(buf, 'x', buflen);

    struct sb_sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = 2;
    sin.sin_port = rx ? BENCH_SOURCE_PORT : BENCH_SINK_PORT;
    sin.sin_addr = addr;

    for (LONG i = 0; i < streams; i++)
    {
        fds[i] = sb_socket(2, 1, 0);
        if (fds[i] < 0 || sb_connect(fds[i], &sin, sizeof(sin)) != 0)
        {
            printf("socktest: stream %ld connect failed, errno %ld\n", (long)i, (long)sb_Errno());
            goto bench_close;
        }
        LONG one = 1;
        sb_IoctlSocket(fds[i], SB_FIONBIO, &one);
    }
    if (rx)
    {
        /* start gun — the peer's source blasts only after this byte, so no
         * stream floods the RX pool while later handshakes are still in
         * flight (a dry pool eats SYN-ACKs; seen live as errno 53 with 4
         * streams). One byte always fits the fresh send buffer. */
        for (LONG i = 0; i < streams; i++)
            sb_send(fds[i], "G", 1, 0);
    }
    printf("bench %s: %ld stream(s) to port %u, %ld s, %ld KB buffer\n",
           rx ? "rx" : "tx", (long)streams, sin.sin_port, (long)seconds, (long)bufKB);

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    unsigned long long start = bench_now();
    unsigned long long deadline = start + (unsigned long long)seconds * freq;
    unsigned long long now = start;
    LONG active = streams;

    while (active > 0 && (now = bench_now()) < deadline)
    {
        ULONG mask = 0;
        LONG maxfd = -1;
        for (LONG i = 0; i < streams; i++)
        {
            if (fds[i] < 0)
                continue;
            mask |= 1UL << fds[i];
            if (fds[i] > maxfd)
                maxfd = fds[i];
        }

        struct sb_timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000; /* recheck the deadline even when idle */
        LONG n = sb_WaitSelect(maxfd + 1, rx ? &mask : NULL, rx ? NULL : &mask, NULL, &tv, NULL);
        if (n < 0)
        {
            printf("socktest: WaitSelect errno %ld\n", (long)sb_Errno());
            break;
        }
        if (n == 0)
            continue;

        for (LONG i = 0; i < streams; i++)
        {
            if (fds[i] < 0 || (mask & (1UL << fds[i])) == 0)
                continue;

            /* drain/fill a few rounds per readiness, then yield to the others */
            for (LONG round = 0; round < 8; round++)
            {
                LONG r = rx ? sb_recv(fds[i], buf, (LONG)buflen, 0)
                            : sb_send(fds[i], buf, (LONG)buflen, 0);
                if (r > 0)
                {
                    bytes[i] += (unsigned long long)r;
                    if ((ULONG)r < buflen)
                        break;
                    continue;
                }
                if (r == 0 || sb_Errno() != SB_ERR_EWOULDBLOCK)
                {
                    if (r != 0)
                        printf("socktest: stream %ld errno %ld\n", (long)i, (long)sb_Errno());
                    sb_CloseSocket(fds[i]);
                    fds[i] = -1;
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
        if (fds[i] >= 0)
            sb_CloseSocket(fds[i]);

bench_out:
    if (buf != NULL)
        FreeVec(buf);
    CloseDevice(&tr->tr_node);
    DeleteIORequest(&tr->tr_node);
    DeleteMsgPort(tp);
    return rc;
}

int main(int argc, char **argv)
{
    Kprintf("[socktest] %s: argc=%ld\n", __func__, (LONG)argc);

    if (argc >= 2 && strcmp(argv[1], "bench") == 0)
    {
        SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
        if (SocketBase == NULL)
        {
            printf("socktest: cannot open bsdsocket.library v4\n");
            return 20;
        }
        int rc = bench_main(argc - 1, argv + 1);
        CloseLibrary(SocketBase);
        return rc;
    }

    if (argc < 3)
    {
        printf("usage: socktest <host> <port> [text]\n"
               "       socktest bench rx|tx <host> [streams] [seconds] [bufKB]\n");
        return 5;
    }

    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        printf("socktest: cannot open bsdsocket.library v4\n");
        return 20;
    }
    printf("bsdsocket.library %u.%u open (stack starting brings DHCP up)\n",
           SocketBase->lib_Version, SocketBase->lib_Revision);

    int rc = 10;
    ULONG addr = sb_inet_addr(argv[1]);
    if (addr == 0xFFFFFFFF)
    {
        printf("resolving %s...\n", argv[1]);
        struct sb_hostent *he = sb_gethostbyname(argv[1]);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
        {
            printf("socktest: cannot resolve %s\n", argv[1]);
            goto out;
        }
        addr = *(ULONG *)he->h_addr_list[0];
    }
    printf("address %lu.%lu.%lu.%lu\n",
           (unsigned long)(addr >> 24) & 0xFF, (unsigned long)(addr >> 16) & 0xFF,
           (unsigned long)(addr >> 8) & 0xFF, (unsigned long)addr & 0xFF);

    LONG s = sb_socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
    if (s < 0)
    {
        printf("socktest: socket() errno %ld\n", (long)sb_Errno());
        goto out;
    }

    struct sb_sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = 2;
    sin.sin_port = (UWORD)atoi(argv[2]);
    sin.sin_addr = addr;

    printf("connecting...\n");
    if (sb_connect(s, &sin, sizeof(sin)) != 0)
    {
        printf("socktest: connect() errno %ld\n", (long)sb_Errno());
        sb_CloseSocket(s);
        goto out;
    }

    char req[512];
    if (argc > 3)
    {
        strncpy(req, argv[3], sizeof(req) - 3);
        req[sizeof(req) - 3] = '\0';
        strcat(req, "\r\n");
    }
    else
    {
        strcpy(req, "GET / HTTP/1.0\r\n\r\n");
    }

    LONG n = sb_send(s, req, (LONG)strlen(req), 0);
    printf("sent %ld bytes\n", (long)n);

    static char buf[2048];
    LONG total = 0;
    for (;;)
    {
        n = sb_recv(s, buf, sizeof(buf) - 1, 0);
        if (n < 0)
        {
            printf("socktest: recv() errno %ld\n", (long)sb_Errno());
            break;
        }
        if (n == 0)
            break; /* orderly EOF */
        buf[n] = '\0';
        if (total < 4096)
            printf("%s", buf);
        total += n;
    }
    printf("\n--- received %ld bytes total ---\n", (long)total);
    sb_CloseSocket(s);
    rc = 0;

out:
    CloseLibrary(SocketBase);
    return rc;
}
