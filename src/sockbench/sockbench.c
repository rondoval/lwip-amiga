/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sockbench — repeatable LAN TCP/UDP throughput benchmark over bsdsocket.library.
 *
 * Runs against scripts/tcp-bench-peer.py (start it on a PC; port 5001 discards
 * what we send, port 5002 blasts data at us — same ports for TCP and UDP):
 *
 *   sockbench rx|tx|txblock|udprx|udptx <host> [streams] [seconds] [sizeKB] [mbps]
 *
 * rx/tx are TCP; udprx/udptx are UDP. N nonblocking sockets driven from one
 * WaitSelect loop; per-stream and aggregate Mb/s from ReadEClock. Defaults:
 * 1 stream, 10 s; TCP 64 KB buffer, UDP one 1500-MTU frame per datagram.
 * For UDP the size arg sets the datagram size (fragmented above one frame).
 * udprx asks the peer for a PACED stream (default 100 Mb/s, iperf -b style):
 * the benchmark is whether this machine keeps up with X Mb/s, and received
 * against X is the loss. mbps 0 = unpaced line-rate flood. udptx is
 * paced by the stack (a datagram is accepted only when it can go out whole).
 *
 * txblock is a diagnostic mode, not a benchmark: one BLOCKING socket driven by
 * a tight send() loop with no WaitSelect, mirroring AmiSpeedTest's upload test.
 * It exists because the nonblocking modes cannot reach sb_tcp_send's blocking
 * path at all — they take the dontwait early-out on every would-block, so they
 * never loop over a multi-chunk write, never sleep in sb_send_block, and never
 * hit the mid-write lock break that re-enters lwIP with a half-built unsent
 * tail.
 *
 *   sockbench rr|rrsel <host> [respBytes] [seconds] [reqBytes]
 *
 * rr/rrsel measure LATENCY, which the streaming modes cannot see: a strict
 * request/response ping-pong on one blocking TCP_NODELAY socket against the
 * peer's port 5003 — send a small request, read the whole reply, repeat. That
 * is the shape of SMB2, NFS, HTTP keep-alive: throughput = reply size / time
 * per exchange, so a fixed per-exchange delay (interrupt coalescing, delayed
 * ACKs, wakeup latency) that a saturated stream amortises to nothing becomes
 * the whole result. respBytes takes a K/M suffix; 0 or omitted sweeps 1 byte
 * to 1 MB on one connection. Defaults: 5 s per point, 128-byte requests.
 *   rr     the ideal client: one send(), then recv() straight into the buffer.
 *   rrsel  libsmb2's call sequence: WaitSelect + send, WaitSelect, then the
 *          reply as separate 4 / 64 / 16-byte recv()s before the payload.
 *          rrsel minus rr prices that pattern on this stack.
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
#include <netinet/tcp.h> /* TCP_NODELAY */
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
#define BENCH_RR_PORT 5003     /* peer answers each request with N bytes (rr test) */
#define BENCH_MAX_STREAMS 8
#define BENCH_UDP_DGRAM 1472   /* default UDP payload: one 1500-MTU frame */
#define BENCH_UDP_RATE_MBPS 100 /* default udprx pace; 0 = unpaced flood */

#define BENCH_RR_HDR 8                   /* {u32 resp_len, u32 req_len}, network order */
#define BENCH_RR_REQ 128                 /* default request: an SMB2 READ request is ~120 bytes */
#define BENCH_RR_REQ_MAX 65536
#define BENCH_RR_MAX (8UL * 1024 * 1024) /* largest reply; the peer clamps to the same */
#define BENCH_RR_WARMUP 2                /* untimed exchanges per point: the peer's cwnd ramp is not the subject */
#define BENCH_RR_HIST_N 240              /* rr_bucket() range for a 32-bit tick count */

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

/* "65536", "64K" or "1M" -> bytes */
static BOOL bench_parse_size(const char *s, ULONG *out)
{
    char *end;
    ULONG v = strtoul(s, &end, 10);
    if (end == s)
        return FALSE;
    if (*end == 'K' || *end == 'k')
    {
        v <<= 10;
        end++;
    }
    else if (*end == 'M' || *end == 'm')
    {
        v <<= 20;
        end++;
    }
    *out = v;
    return (BOOL)(*end == '\0');
}

/* Latency histogram over EClock ticks: exact below 8, then 8 sub-buckets per
 * octave (12.5 % resolution) — fine enough to tell 150 us from 650 us, and
 * small enough to keep every sample of a run without allocating. Bucketing
 * ticks rather than microseconds keeps the 64-bit divide out of the loop. */
static ULONG rr_bucket(ULONG ticks)
{
    if (ticks < 8)
        return ticks;
    ULONG top = 31;
    while ((ticks >> top) == 0)
        top--;
    return 8 + (top - 3) * 8 + ((ticks >> (top - 3)) & 7);
}

static ULONG rr_bucket_floor(ULONG idx)
{
    if (idx < 8)
        return idx;
    ULONG top = (idx - 8) / 8 + 3;
    return (8 + (idx - 8) % 8) << (top - 3);
}

static ULONG rr_percentile(const ULONG *hist, ULONG count, ULONG pct)
{
    ULONG want = (ULONG)(((unsigned long long)count * pct + 99) / 100);
    ULONG seen = 0;
    for (ULONG i = 0; i < BENCH_RR_HIST_N; i++)
    {
        seen += hist[i];
        if (seen >= want)
            return rr_bucket_floor(i);
    }
    return 0;
}

static ULONG rr_us(unsigned long long ticks, ULONG freq)
{
    return (ULONG)(ticks * 1000000ULL / freq);
}

/* rrsel: one readiness wait shaped like libsmb2's poll() shim — the fd in the
 * read OR the write set, always in the except set, 1 s timeout, retried on
 * expiry. Blocking here arms and cancels a timer request inside the stack,
 * which is part of what rrsel is pricing. */
static BOOL rr_wait(LONG s, BOOL forwrite)
{
    for (;;)
    {
        fd_set set, eset;
        FD_ZERO(&set);
        FD_ZERO(&eset);
        FD_SET(s, &set);
        FD_SET(s, &eset);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        LONG n = WaitSelect(s + 1, forwrite ? NULL : &set, forwrite ? &set : NULL, &eset, &tv, NULL);
        if (n != 0)
            return (BOOL)(n > 0);
    }
}

/* Receive phases, 0 = the rest of the reply. rr reads like an ideal client;
 * rrsel replays libsmb2's receive state machine — NetBIOS length, SMB2 header,
 * fixed part, then the payload — each phase its own recv(). */
static const ULONG rr_phases_plain[] = {0};
static const ULONG rr_phases_smb2[] = {4, 64, 16, 0};

/* One exchange. Request and reply share buf: the header is rewritten every
 * time because the previous reply overwrote it; the filler is don't-care. */
static BOOL rr_exchange(LONG s, BOOL sel, UBYTE *buf, ULONG req, ULONG resp)
{
    ULONG *hdr = (ULONG *)(void *)buf;
    hdr[0] = htonl(resp);
    hdr[1] = htonl(req);

    if (sel && !rr_wait(s, TRUE))
        return FALSE;
    for (ULONG done = 0; done < req;)
    {
        LONG r = send(s, buf + done, (LONG)(req - done), 0);
        if (r <= 0)
            return FALSE;
        done += (ULONG)r;
    }

    if (sel && !rr_wait(s, FALSE))
        return FALSE;
    const ULONG *phase = sel ? rr_phases_smb2 : rr_phases_plain;
    for (ULONG got = 0; got < resp;)
    {
        ULONG want = resp - got;
        if (*phase != 0)
        {
            if (*phase < want)
                want = *phase;
            phase++;
        }
        /* a blocking recv() returns what is queued, not what was asked for */
        for (ULONG end = got + want; got < end;)
        {
            LONG r = recv(s, buf + got, (LONG)(end - got), 0);
            if (r <= 0)
                return FALSE;
            got += (ULONG)r;
        }
    }
    return TRUE;
}

/* One sweep point: exchanges back to back until the deadline, one report line. */
static BOOL bench_rr_point(LONG s, BOOL sel, UBYTE *buf, ULONG req, ULONG resp,
                           LONG seconds, ULONG freq)
{
    static ULONG hist[BENCH_RR_HIST_N];
    memset(hist, 0, sizeof(hist));

    for (LONG i = 0; i < BENCH_RR_WARMUP; i++)
        if (!rr_exchange(s, sel, buf, req, resp))
            return FALSE;

    ULONG count = 0, dt_min = ~0UL, dt_max = 0;
    unsigned long long sum = 0;
    unsigned long long start = bench_now();
    unsigned long long deadline = start + (unsigned long long)seconds * freq;
    unsigned long long t1;
    do
    {
        /* a fresh t0 per exchange keeps the bookkeeping below out of the sample */
        unsigned long long t0 = bench_now();
        if (!rr_exchange(s, sel, buf, req, resp))
            return FALSE;
        t1 = bench_now();

        ULONG dt = (ULONG)(t1 - t0);
        hist[rr_bucket(dt)]++;
        sum += dt;
        if (dt < dt_min)
            dt_min = dt;
        if (dt > dt_max)
            dt_max = dt;
        count++;
    } while (t1 < deadline);

    unsigned long long ticks = t1 - start;
    ULONG tps = (ULONG)((unsigned long long)count * freq / ticks);
    /* MB/s x10 of reply payload; bytes*10*freq stays far below 2^63 */
    unsigned long long mb_x10 = (unsigned long long)count * resp * 10ULL * freq / ticks / 1048576ULL;
    printf("%s resp=%lu req=%lu: %lu trans/s  lat us avg/min/p50/p90/p99/max "
           "%lu/%lu/%lu/%lu/%lu/%lu  %lu.%lu MB/s\n",
           sel ? "rrsel" : "rr", (unsigned long)resp, (unsigned long)req, (unsigned long)tps,
           (unsigned long)rr_us(sum / count, freq), (unsigned long)rr_us(dt_min, freq),
           (unsigned long)rr_us(rr_percentile(hist, count, 50), freq),
           (unsigned long)rr_us(rr_percentile(hist, count, 90), freq),
           (unsigned long)rr_us(rr_percentile(hist, count, 99), freq),
           (unsigned long)rr_us(dt_max, freq),
           (unsigned long)(mb_x10 / 10), (unsigned long)(mb_x10 % 10));
    return TRUE;
}

static int bench_rr(ULONG addr, BOOL sel, ULONG resp, ULONG req, LONG seconds)
{
    static const ULONG sweep[] = {1, 1460, 4096, 16384, 32768, 65536, 262144, 1048576};
    const ULONG nsweep = sizeof(sweep) / sizeof(sweep[0]);

    ULONG biggest = (resp != 0) ? resp : sweep[nsweep - 1];
    if (req > biggest)
        biggest = req;
    UBYTE *buf = AllocVec(biggest, MEMF_PUBLIC);
    if (buf == NULL)
    {
        printf("sockbench: no memory for %lu byte buffer\n", (unsigned long)biggest);
        return 10;
    }
    memset(buf, 'x', req);

    int rc = 10;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(BENCH_RR_PORT);
    sin.sin_addr.s_addr = addr;

    LONG s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
    {
        printf("sockbench: connect failed, errno %ld\n", (long)Errno());
        goto rr_out;
    }
    /* Without it the request itself would wait on Nagle; say so rather than
     * publish numbers that measure the peer's delayed-ACK timer. */
    LONG one = 1;
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
        printf("sockbench: WARNING TCP_NODELAY refused, errno %ld\n", (long)Errno());

    printf("bench tcp %s: port %u, %ld s per point, %lu byte requests\n",
           sel ? "rrsel" : "rr", BENCH_RR_PORT, (long)seconds, (unsigned long)req);

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    BOOL ok = TRUE;
    if (resp != 0)
        ok = bench_rr_point(s, sel, buf, req, resp, seconds, freq);
    for (ULONG i = 0; resp == 0 && ok && i < nsweep; i++)
        ok = bench_rr_point(s, sel, buf, req, sweep[i], seconds, freq);
    if (ok)
        rc = 0;
    else
        printf("sockbench: exchange failed, errno %ld\n", (long)Errno());

rr_out:
    if (s >= 0)
        CloseSocket(s);
    FreeVec(buf);
    return rc;
}

int main(int argc, char **argv)
{
    /* argv: [0]=sockbench [1]=rx|tx|udprx|udptx [2]=host [3]=streams [4]=secs [5]=sizeKB [6]=mbps (udprx)
     *       rr|rrsel reuse the slots as     [2]=host [3]=respBytes [4]=secs [5]=reqBytes */
    const char *dir = (argc > 1) ? argv[1] : "";
    BOOL rx = FALSE, udp = FALSE, blocking = FALSE, rr = FALSE, rrsel = FALSE, dir_ok = TRUE;
    if (strcmp(dir, "rx") == 0)         rx = TRUE;
    else if (strcmp(dir, "tx") == 0)    rx = FALSE;
    else if (strcmp(dir, "txblock") == 0) { rx = FALSE; blocking = TRUE; }
    else if (strcmp(dir, "udprx") == 0) { rx = TRUE;  udp = TRUE; }
    else if (strcmp(dir, "udptx") == 0) { rx = FALSE; udp = TRUE; }
    else if (strcmp(dir, "rr") == 0)    rr = TRUE;
    else if (strcmp(dir, "rrsel") == 0) { rr = TRUE;  rrsel = TRUE; }
    else                                dir_ok = FALSE;

    if (!dir_ok || argc < 3)
    {
        printf("usage: sockbench rx|tx|txblock|udprx|udptx <host> [streams<=%d] [seconds] [sizeKB] [mbps]\n"
               "  rx/tx = TCP; udprx/udptx = UDP. sizeKB is the TCP buffer or the UDP\n"
               "  datagram size (default: TCP 64 KB, UDP one 1500-MTU frame).\n"
               "  udprx: the peer paces its stream to mbps (default %d); 0 = unpaced\n"
               "  line-rate flood, an overload test rather than a benchmark.\n"
               "  txblock = diagnostic: 1 blocking socket, tight send() loop, 32 KB\n"
               "  default — reproduces AmiSpeedTest's upload path.\n"
               "       sockbench rr|rrsel <host> [respBytes] [seconds] [reqBytes]\n"
               "  request/response latency: small request out, respBytes back, repeat.\n"
               "  respBytes takes K/M; 0 or omitted sweeps 1 byte..1M. Default 5 s per\n"
               "  point, 128 byte requests. rrsel = libsmb2's WaitSelect + split-recv\n"
               "  call pattern; rr = one send, one recv loop.\n",
               BENCH_MAX_STREAMS, BENCH_UDP_RATE_MBPS);
        return 5;
    }
    LONG streams = (!rr && argc > 3) ? atoi(argv[3]) : 1;
    LONG seconds = (argc > 4) ? atoi(argv[4]) : (rr ? 5 : 10);
    LONG sizeKB = (!rr && argc > 5) ? atoi(argv[5]) : 0; /* 0 = per-protocol default */
    LONG mbps = (udp && rx) ? (argc > 6 ? atoi(argv[6]) : BENCH_UDP_RATE_MBPS) : 0;
    ULONG rr_resp = 0, rr_req = BENCH_RR_REQ;            /* resp 0 = sweep */
    if (rr && ((argc > 3 && !bench_parse_size(argv[3], &rr_resp)) ||
               (argc > 5 && !bench_parse_size(argv[5], &rr_req)) ||
               rr_resp > BENCH_RR_MAX || rr_req < BENCH_RR_HDR || rr_req > BENCH_RR_REQ_MAX))
    {
        printf("sockbench: bad parameters (respBytes <= 8M, reqBytes 8..64K)\n");
        return 5;
    }
    if (streams < 1 || streams > BENCH_MAX_STREAMS || seconds < 1 || mbps < 0)
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

    /* rr/rrsel share only the setup above: one socket, its own buffer and loop */
    if (rr)
    {
        rc = bench_rr(addr, rrsel, rr_resp, rr_req, seconds);
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
        if (udp && mbps > 0)
            glen = sprintf((char *)buf, "G%lu/%ld", (unsigned long)buflen, (long)mbps);
        else if (udp)
            glen = sprintf((char *)buf, "G%lu", (unsigned long)buflen); /* the old gun: unpaced */
        for (LONG i = 0; i < streams; i++)
            send(sock[i], buf, glen, 0);
    }
    printf("bench %s %s: %ld stream(s) to port %u, %ld s, %lu byte %s\n",
           udp ? "udp" : "tcp", blocking ? "txblock" : rx ? "rx" : "tx",
           (long)streams, port, (long)seconds,
           (unsigned long)buflen, udp ? "datagrams" : "buffer");
    if (udp && rx)
        printf(mbps > 0 ? "peer paced at %ld Mb/s per stream\n"
                        : "peer UNPACED: line-rate flood, an overload test, not a benchmark\n",
               (long)mbps);

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
    else if (udp && mbps > 0 && ticks != 0)
    {
        /* the same Mb/s x10 arithmetic as bench_report, against the pace */
        unsigned long long got_x10 = total * 8ULL / 100000ULL * freq / ticks;
        unsigned long long target_x10 = (unsigned long long)mbps * 10ULL * (unsigned long long)streams;
        printf("(udp rx: %lu.%lu of %lu Mb/s target = %lu %%; the peer's sent count is the truth)\n",
               (unsigned long)(got_x10 / 10), (unsigned long)(got_x10 % 10),
               (unsigned long)(target_x10 / 10), (unsigned long)(got_x10 * 100ULL / target_x10));
    }
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
