/* SPDX-License-Identifier: BSD-3-Clause */
/* Deterministic tests of the TIME-WAIT reopen filter (port/amiga/tcp_tw_reopen.c).
 *
 * Real frames enter through tcp_tw_reopen_input() and go on to ip_input(), so the
 * filter and tcp_input() both run. A helper brings a connection to TIME-WAIT with a server-side close, as
 * rxnetd does on a bad request. Each check is one assertion on the wire reply
 * captured at netif->output and on the TIME-WAIT list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "tcp_tw_reopen.h"

void fuzz_fail_hook(const char *expr, const char *file, int line)
{
  fprintf(stderr, "FAIL(helper): %s at %s:%d\n", expr, file, line);
  exit(1);
}
void fuzz_platform_assert(const char *msg, const char *file, int line)
{
  fprintf(stderr, "LWIP_ASSERT: %s at %s:%d\n", msg, file, line);
  exit(1);
}
void tcp_timer_needed(void) {}

/* TCP_TW_REOPEN_NEXT: no Ethernet layer in this test, drop the header. */
err_t tw_test_next(struct pbuf *p, struct netif *inp)
{
  pbuf_remove_header(p, 14);
  return ip_input(p, inp);
}

static int checks_run, failed, nohook;
#define CHECK(x) do { \
    checks_run++; \
    if (!(x)) { failed++; fprintf(stderr, "FAIL: %s at %s:%d\n", #x, __FILE__, __LINE__); \
      if (!nohook) exit(1); } \
  } while (0)

#define PEER 0xC0A8013CUL /* 192.168.1.60 */
#define ME   0xC0A8011BUL /* 192.168.1.27 */
#define SRV  7373

static struct netif nif;
static struct { u32_t seq, ack; u8_t flags; int n; } tx;
static struct tcp_pcb *accepted;
static int accepts;

static err_t out(struct netif *n, struct pbuf *p, const ip4_addr_t *dst)
{
  u8_t b[60];
  pbuf_copy_partial(p, b, sizeof(b), 0);
  const struct tcp_hdr *t = (const struct tcp_hdr *)(b + IP_HLEN);
  tx.seq = lwip_ntohl(t->seqno);
  tx.ack = lwip_ntohl(t->ackno);
  tx.flags = (u8_t)(TCPH_FLAGS(t));
  tx.n++;
  return ERR_OK;
}
static err_t nif_init(struct netif *n)
{
  n->output = out;
  n->mtu = 1500;
  n->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
  return ERR_OK;
}
static err_t on_recv(void *a, struct tcp_pcb *pcb, struct pbuf *p, err_t e)
{
  if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); }
  return ERR_OK;
}
static err_t on_accept(void *a, struct tcp_pcb *pcb, err_t e)
{
  accepted = pcb;
  accepts++;
  tcp_recv(pcb, on_recv);
  return ERR_OK;
}

/* Sends one packet: extra lets a test damage the header. */
enum { EX_NONE, EX_MF, EX_OFFSET, EX_SHORT_P, EX_IPOPT, EX_UDP, EX_VLAN, EX_IPV6 };
static void rx_ex(u16_t sport, u32_t seq, u32_t ack, u8_t flags, int ex)
{
  const u16_t hl = ex == EX_IPOPT ? 24 : IP_HLEN;
  const u16_t len = (u16_t)(hl + TCP_HLEN);
  struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(14 + (ex == EX_SHORT_P ? hl + 10 : len)), PBUF_RAM);
  u8_t b[80];
  memset(b, 0, sizeof(b));
  b[12] = ex == EX_VLAN ? 0x81 : 0x08;
  struct ip_hdr *ip = (struct ip_hdr *)(b + 14);
  struct tcp_hdr *t = (struct tcp_hdr *)(b + 14 + hl);
  IPH_VHL_SET(ip, ex == EX_IPV6 ? 6 : 4, hl / 4);
  IPH_LEN_SET(ip, lwip_htons(len));
  IPH_TTL_SET(ip, 64);
  IPH_PROTO_SET(ip, ex == EX_UDP ? IP_PROTO_UDP : IP_PROTO_TCP);
  if (ex == EX_MF) IPH_OFFSET_SET(ip, PP_HTONS(IP_MF));
  if (ex == EX_OFFSET) IPH_OFFSET_SET(ip, PP_HTONS(100));
  ip->src.addr = lwip_htonl(PEER);
  ip->dest.addr = lwip_htonl(ME);
  IPH_CHKSUM_SET(ip, inet_chksum(ip, hl));
  t->src = lwip_htons(sport);
  t->dest = lwip_htons(SRV);
  t->seqno = lwip_htonl(seq);
  t->ackno = lwip_htonl(ack);
  TCPH_HDRLEN_FLAGS_SET(t, 5, flags);
  t->wnd = lwip_htons(8192);
  ip_addr_t s, d;
  ip_addr_set_ip4_u32(&s, lwip_htonl(PEER));
  ip_addr_set_ip4_u32(&d, lwip_htonl(ME));
  struct pbuf *c = pbuf_alloc(PBUF_RAW, TCP_HLEN, PBUF_RAM);
  pbuf_take(c, t, TCP_HLEN);
  t->chksum = ip_chksum_pseudo(c, IP_PROTO_TCP, TCP_HLEN, &s, &d);
  pbuf_free(c);
  pbuf_take(p, b, p->tot_len);
  tx.n = 0;
#ifdef TW_NO_FILTER
  tw_test_next(p, &nif);
#else
  tcp_tw_reopen_input(p, &nif);
#endif
}
static void rx(u16_t sport, u32_t seq, u32_t ack, u8_t flags)
{
  rx_ex(sport, seq, ack, flags, EX_NONE);
}
static struct tcp_pcb *tw_find(u16_t sport)
{
  for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next)
    if (p->remote_port == sport) return p;
  return NULL;
}

/* SYN, SYN+ACK, ACK, server close, peer FIN+ACK: leaves sport in TIME-WAIT. */
static void to_time_wait(u16_t sport, u32_t isn)
{
  rx(sport, isn, 0, TCP_SYN);
  const u32_t iss = tx.seq;
  rx(sport, isn + 1, iss + 1, TCP_ACK);
  tcp_close(accepted);
  const u32_t fin = tx.seq;
  rx(sport, isn + 1, fin + 1, TCP_ACK);
  rx(sport, isn + 1, fin + 1, TCP_FIN | TCP_ACK);
  if (!tw_find(sport)) { fprintf(stderr, "setup: %u not in TIME-WAIT\n", sport); exit(1); }
}
#define OLD 1000U
#define END (OLD + 2) /* rcv_nxt after isn=OLD: SYN and FIN consumed */

int main(int argc, char **argv)
{
  nohook = argc > 1 && !strcmp(argv[1], "nohook");
  lwip_init();
  ip4_addr_t a, m, g;
  ip4_addr_set_u32(&a, lwip_htonl(ME));
  ip4_addr_set_u32(&m, lwip_htonl(0xFFFFFF00UL));
  ip4_addr_set_u32(&g, 0);
  netif_add(&nif, &a, &m, &g, NULL, nif_init, ip_input);
  netif_set_default(&nif);
  netif_set_up(&nif);
  netif_set_link_up(&nif);
  struct tcp_pcb *l = tcp_new();
  tcp_bind(l, IP_ANY_TYPE, SRV);
  l = tcp_listen(l);
  tcp_accept(l, on_accept);
  int n;

  /* --- reopen: a SYN above the old connection connects at once --- */
  to_time_wait(40000, OLD);
  CHECK(tw_find(40000)->rcv_nxt == END);
  n = accepts;
  rx(40000, END + 5000, 0, TCP_SYN);
  CHECK(tw_find(40000) == NULL);
  CHECK(tx.n == 1);
  CHECK(tx.flags == (TCP_SYN | TCP_ACK));
  CHECK(tx.ack == END + 5000 + 1);
  rx(40000, END + 5001, tx.seq + 1, TCP_ACK);
  CHECK(accepts == n + 1);
  CHECK(accepted->state == ESTABLISHED);
  CHECK(accepted->remote_port == 40000);
  tcp_abort(accepted);

  /* in-window SYN (lwIP alone would answer RST) */
  to_time_wait(40002, OLD);
  rx(40002, END + 1, 0, TCP_SYN);
  CHECK(tw_find(40002) == NULL);
  CHECK(tx.flags == (TCP_SYN | TCP_ACK));
  rx(40002, END + 2, tx.seq + 1, TCP_ACK);
  tcp_abort(accepted);

  /* across the sequence wrap */
  to_time_wait(40004, 0xFFFFFFF0U);
  rx(40004, 0x10, 0, TCP_SYN);
  CHECK(tw_find(40004) == NULL);
  CHECK(tx.flags == (TCP_SYN | TCP_ACK));
  rx(40004, 0x11, tx.seq + 1, TCP_ACK);
  tcp_abort(accepted);

  /* IP options: the TCP header is found behind them */
  to_time_wait(40006, OLD);
  rx_ex(40006, END + 5000, 0, TCP_SYN, EX_IPOPT);
  CHECK(tw_find(40006) == NULL);

  /* the other TIME-WAIT entries stay, whatever their list position */
  to_time_wait(40010, OLD);
  to_time_wait(40012, OLD);
  to_time_wait(40014, OLD);
  rx(40012, END + 5000, 0, TCP_SYN);
  CHECK(tw_find(40012) == NULL);
  CHECK(tw_find(40010) != NULL);
  CHECK(tw_find(40014) != NULL);

  if (nohook) {
    printf("without the filter: %d of %d checks failed (expected: not 0)\n", failed, checks_run);
    return failed ? 0 : 1;
  }

  /* --- not above: lwIP's TIME-WAIT behaviour is kept --- */
  to_time_wait(40020, OLD);
  rx(40020, END, 0, TCP_SYN); /* == rcv_nxt: lwIP's in-window RST */
  CHECK(tw_find(40020) != NULL);
  CHECK(tx.flags & TCP_RST);
  rx(40020, END - 1, 0, TCP_SYN); /* old duplicate: ACK */
  CHECK(tw_find(40020) != NULL);
  CHECK(tx.n == 1);
  CHECK(tx.flags == TCP_ACK);
  CHECK(tx.ack == END);

  /* --- not a bare SYN --- */
  rx(40020, END + 5000, 1, TCP_SYN | TCP_ACK);
  CHECK(tw_find(40020) != NULL);
  rx(40020, END + 5000, 0, TCP_SYN | TCP_FIN);
  CHECK(tw_find(40020) != NULL);
  rx(40020, END + 5000, 0, TCP_SYN | TCP_RST);
  CHECK(tw_find(40020) != NULL);
  rx(40020, END + 5000, 1, TCP_ACK);
  CHECK(tw_find(40020) != NULL);

  /* --- not a whole, valid TCP packet --- */
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_MF);
  CHECK(tw_find(40020) != NULL);
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_OFFSET);
  CHECK(tw_find(40020) != NULL);
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_SHORT_P);
  CHECK(tw_find(40020) != NULL);
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_UDP);
  CHECK(tw_find(40020) != NULL);
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_IPV6);
  CHECK(tw_find(40020) != NULL);
  rx_ex(40020, END + 5000, 0, TCP_SYN, EX_VLAN); /* documented limit: not filtered */
  CHECK(tw_find(40020) != NULL);

  /* --- another tuple: an ordinary new connection, entry untouched --- */
  rx(40030, 7, 0, TCP_SYN);
  CHECK(tw_find(40020) != NULL);
  CHECK(tx.flags == (TCP_SYN | TCP_ACK));
  rx(40030, 8, tx.seq + 1, TCP_ACK);
  tcp_abort(accepted);

  /* --- no listener left: the client is refused at once, it does not stall --- */
  tcp_close(l);
  rx(40020, END + 5000, 0, TCP_SYN);
  CHECK(tw_find(40020) == NULL);
  CHECK(tx.n == 1);
  CHECK(tx.flags & TCP_RST);

  printf("twreopen: %d checks passed\n", checks_run);
  return 0;
}
