/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Loopback TCP echo smoke test.
 *
 * Proves the lwIP core builds and moves data on the host: a raw-API echo server
 * and client on 127.0.0.1 push SMOKE_TOTAL patterned bytes through the loopback
 * netif, verifying every byte that comes back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#define SMOKE_PORT  7777
#define SMOKE_TOTAL (256u * 1024u)
#define SMOKE_CHUNK 512u

u32_t sys_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u64_t)ts.tv_sec * 1000u + (u64_t)ts.tv_nsec / 1000000u);
}

static size_t g_tx;     /* client bytes queued for send */
static size_t g_rx;     /* client bytes received and verified */
static int g_failed;
static int g_done;

static u8_t pat(size_t i)
{
    return (u8_t)((i * 31u + 7u) & 0xffu);
}

static void fail(const char *what, int err)
{
    printf("FAIL: %s (err %d)\n", what, err);
    g_failed = 1;
}

/* --- server: echo everything back --- */

static err_t srv_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        tcp_close(tpcb);
        return ERR_OK;
    }
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        err_t werr = tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
        if (werr != ERR_OK) {
            fail("server tcp_write", werr);
            break;
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    tcp_output(tpcb);
    return ERR_OK;
}

static err_t srv_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) {
        fail("accept", err);
        return ERR_OK;
    }
    tcp_recv(newpcb, srv_recv);
    return ERR_OK;
}

/* --- client: push pattern, verify echo --- */

static err_t cli_push(struct tcp_pcb *tpcb)
{
    while (g_tx < SMOKE_TOTAL && tcp_sndbuf(tpcb) >= SMOKE_CHUNK) {
        u8_t buf[SMOKE_CHUNK];
        size_t n = SMOKE_TOTAL - g_tx;
        if (n > SMOKE_CHUNK) {
            n = SMOKE_CHUNK;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = pat(g_tx + i);
        }
        err_t werr = tcp_write(tpcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (werr == ERR_MEM) {
            break; /* retry from cli_sent */
        }
        if (werr != ERR_OK) {
            fail("client tcp_write", werr);
            return werr;
        }
        g_tx += n;
    }
    tcp_output(tpcb);
    return ERR_OK;
}

static err_t cli_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)arg;
    (void)len;
    return cli_push(tpcb);
}

static err_t cli_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        if (!g_done) {
            fail("client connection closed early", err);
        }
        return ERR_OK;
    }
    size_t off = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const u8_t *d = (const u8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            if (d[i] != pat(g_rx + off + i)) {
                fail("payload mismatch", 0);
                pbuf_free(p);
                return ERR_OK;
            }
        }
        off += q->len;
    }
    g_rx += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    if (g_rx >= SMOKE_TOTAL) {
        g_done = 1;
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t cli_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) {
        fail("connect", err);
        return ERR_OK;
    }
    tcp_recv(tpcb, cli_recv);
    tcp_sent(tpcb, cli_sent);
    return cli_push(tpcb);
}

static void cli_err(void *arg, err_t err)
{
    (void)arg;
    if (!g_done) {
        fail("client error callback", err);
    }
}

int main(void)
{
    lwip_init();

    struct tcp_pcb *srv = tcp_new();
    if (srv == NULL || tcp_bind(srv, IP4_ADDR_ANY, SMOKE_PORT) != ERR_OK) {
        fail("server bind", 0);
        return 1;
    }
    srv = tcp_listen(srv);
    tcp_accept(srv, srv_accept);

    struct tcp_pcb *cli = tcp_new();
    ip_addr_t dst;
    IP_ADDR4(&dst, 127, 0, 0, 1);
    tcp_err(cli, cli_err);
    if (tcp_connect(cli, &dst, SMOKE_PORT, cli_connected) != ERR_OK) {
        fail("connect call", 0);
        return 1;
    }

    u32_t start = sys_now();
    while (!g_done && !g_failed && sys_now() - start < 10000u) {
        sys_check_timeouts();
        netif_poll_all();
    }

    if (g_done && !g_failed && g_rx == SMOKE_TOTAL) {
        printf("PASS: %u bytes echoed and verified over loopback TCP (%u ms, lwIP %s)\n",
               (unsigned)g_rx, (unsigned)(sys_now() - start), LWIP_VERSION_STRING);
        return 0;
    }
    printf("FAIL: tx=%u rx=%u failed=%d done=%d\n",
           (unsigned)g_tx, (unsigned)g_rx, g_failed, g_done);
    return 1;
}
