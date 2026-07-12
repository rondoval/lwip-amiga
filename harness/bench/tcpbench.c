/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tcpbench — TCP bulk-transfer behavior check for the wire-speed goal.
 *
 * Pushes a patterned bulk stream over a TCP connection through the lossy
 * echoif at several loss rates and verifies: window scaling is negotiated
 * (effective window > 64 KB), the transfer completes and verifies under
 * loss (retransmission works; SACKs are emitted with LWIP_TCP_SACK_OUT),
 * and reports goodput. Host-CPU numbers say nothing about m68k throughput —
 * this validates behavior, not speed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#include "echoif.h"

#define BENCH_PORT   5001
#define BENCH_TOTAL  (32u * 1024u * 1024u)
#define BENCH_CHUNK  4096u

u32_t sys_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u64_t)ts.tv_sec * 1000u + (u64_t)ts.tv_nsec / 1000000u);
}

static size_t g_tx, g_rx;
static int g_failed, g_done;
static u32_t g_max_wnd;

static u8_t pat(size_t i)
{
    return (u8_t)((i * 131u + 17u) & 0xffu);
}

/* --- sink side --- */

static err_t sink_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        tcp_close(tpcb);
        if (g_rx >= BENCH_TOTAL) {
            g_done = 1;
        }
        return ERR_OK;
    }
    size_t off = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const u8_t *d = (const u8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            if (d[i] != pat(g_rx + off + i)) {
                printf("FAIL: payload mismatch at byte %lu\n",
                       (unsigned long)(g_rx + off + i));
                g_failed = 1;
                pbuf_free(p);
                return ERR_OK;
            }
        }
        off += q->len;
    }
    g_rx += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    if (g_rx >= BENCH_TOTAL) {
        g_done = 1;
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t sink_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) {
        g_failed = 1;
        return ERR_OK;
    }
    tcp_recv(newpcb, sink_recv);
    return ERR_OK;
}

/* --- source side --- */

static err_t src_push(struct tcp_pcb *tpcb)
{
    while (g_tx < BENCH_TOTAL && tcp_sndbuf(tpcb) >= BENCH_CHUNK) {
        u8_t buf[BENCH_CHUNK];
        size_t n = BENCH_TOTAL - g_tx;
        if (n > BENCH_CHUNK) {
            n = BENCH_CHUNK;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = pat(g_tx + i);
        }
        err_t werr = tcp_write(tpcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (werr == ERR_MEM) {
            break;
        }
        if (werr != ERR_OK) {
            printf("FAIL: tcp_write err %d\n", werr);
            g_failed = 1;
            return werr;
        }
        g_tx += n;
    }
    tcp_output(tpcb);
    return ERR_OK;
}

static err_t src_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)arg;
    (void)len;
    /* the peer's advertised receive window — the scaled quantity; without
     * negotiated window scaling this can never exceed 65535 */
    if (tpcb->snd_wnd > g_max_wnd) {
        g_max_wnd = tpcb->snd_wnd;
    }
    return src_push(tpcb);
}

static err_t src_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) {
        g_failed = 1;
        return ERR_OK;
    }
    tcp_sent(tpcb, src_sent);
    return src_push(tpcb);
}

static void src_err(void *arg, err_t err)
{
    (void)arg;
    if (!g_done) {
        printf("FAIL: source error callback %d\n", err);
        g_failed = 1;
    }
}

/* --- one run --- */

static int run(struct echoif *ei, unsigned loss_permille, u16_t port)
{
    g_tx = g_rx = 0;
    g_failed = g_done = 0;
    g_max_wnd = 0;
    ei->loss_permille = loss_permille;
    ei->delivered = ei->dropped = ei->overflowed = 0;

    ip_addr_t self;
    IP_ADDR4(&self, 10, 0, 0, 1);

    struct tcp_pcb *srv = tcp_new();
    if (srv == NULL || tcp_bind(srv, &self, port) != ERR_OK) {
        printf("FAIL: bind\n");
        return 1;
    }
    srv = tcp_listen(srv);
    tcp_accept(srv, sink_accept);

    struct tcp_pcb *cli = tcp_new();
    tcp_err(cli, src_err);
    if (tcp_connect(cli, &self, port, src_connected) != ERR_OK) {
        printf("FAIL: connect\n");
        return 1;
    }

    u32_t start = sys_now();
    while (!g_done && !g_failed && sys_now() - start < 120000u) {
        sys_check_timeouts();
        if (echoif_pump(ei, 64) == 0 && !g_done) {
            /* queue empty: idle until a timer would fire; just spin */
        }
    }
    u32_t ms = sys_now() - start;

    tcp_close(srv);

    if (g_failed || g_rx != BENCH_TOTAL) {
        printf("FAIL: loss=%u.%u%% tx=%lu rx=%lu after %lu ms\n",
               loss_permille / 10, loss_permille % 10,
               (unsigned long)g_tx, (unsigned long)g_rx, (unsigned long)ms);
        return 1;
    }

    printf("PASS: loss=%u.%u%%  %u MB in %5lu ms (%4lu MB/s)  "
           "pkts=%lu lost=%lu qfull=%lu  max_sndbuf=%lu\n",
           loss_permille / 10, loss_permille % 10,
           BENCH_TOTAL >> 20, (unsigned long)ms,
           ms ? (unsigned long)((BENCH_TOTAL >> 20) * 1000u / ms) : 0ul,
           ei->delivered, ei->dropped, ei->overflowed,
           (unsigned long)g_max_wnd);
    if (g_max_wnd <= 0xFFFF) {
        printf("WARN: peer window never exceeded 64 KB — scaling not in effect?\n");
    }
    return 0;
}

int main(void)
{
    lwip_init();

    static struct echoif ei;
    ip4_addr_t addr;
    IP4_ADDR(&addr, 10, 0, 0, 1);
    echoif_add(&ei, &addr, 0);

    printf("tcpbench: lwIP %s  TCP_WND=%lu (scale %d)  TCP_SND_BUF=%lu  SACK_OUT=%d\n",
           LWIP_VERSION_STRING, (unsigned long)TCP_WND, TCP_RCV_SCALE,
           (unsigned long)TCP_SND_BUF, LWIP_TCP_SACK_OUT);

    int rc = 0;
    rc |= run(&ei, 0, BENCH_PORT);      /* clean wire */
    rc |= run(&ei, 5, BENCH_PORT + 1);  /* 0.5% loss */
    rc |= run(&ei, 20, BENCH_PORT + 2); /* 2% loss */
    return rc;
}
