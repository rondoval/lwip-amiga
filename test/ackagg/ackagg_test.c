/* SPDX-License-Identifier: BSD-3-Clause */
/* Deterministic regression tests for the fork's LWIP_TCP_ACK_AGGREGATES rule:
 * acknowledge at once when ONE input advances rcv_nxt by more than TCP_MSS.
 *
 * Oracle: every segment leaving netif->output while a crafted segment is being
 * input is captured and its wire header decoded. The receive callback is
 * tcp_helper's, which never calls tcp_recved(), so no window update can stand
 * in for the ACK under test — what is captured is tcp_input's own decision.
 *
 * Built twice (build.sh): ACKAGG=1 asserts the rule, ACKAGG=0 asserts stock
 * lwIP's segment-counting behaviour on the very same inputs, which is what
 * proves the scenarios can tell the two apart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

#include "tcp_helper.h"

/* ---- hooks required by the fuzz shim (reused check.h / arch/cc.h) ---- */

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

void tcp_timer_needed(void) {} /* NO_SYS without timers: tests drive TCP directly */

static int checks_run;
#define CHECK(x) do { \
    if (!(x)) { fprintf(stderr, "FAIL: %s at %s:%d\n", #x, __FILE__, __LINE__); exit(1); } \
    checks_run++; \
  } while (0)

/* ---- TX capture: decode every segment leaving netif->output ---- */

struct cap {
  u32_t ackno;
  u16_t paylen;
  u8_t flags;
};
static struct cap caps[16];
static int ncaps;

static err_t ackagg_netif_output(struct netif *netif, struct pbuf *p,
                                 const ip4_addr_t *ipaddr)
{
  u8_t hdr[64];
  u16_t want = (u16_t)(p->tot_len < sizeof(hdr) ? p->tot_len : sizeof(hdr));
  LWIP_UNUSED_ARG(netif);
  LWIP_UNUSED_ARG(ipaddr);

  pbuf_copy_partial(p, hdr, want, 0);
  {
    u16_t iphl = (u16_t)((hdr[0] & 0x0f) * 4);
    u16_t thl = (u16_t)((hdr[iphl + 12] >> 4) * 4);
    struct cap *c = &caps[ncaps < 16 ? ncaps : 15];
    c->ackno = ((u32_t)hdr[iphl + 8] << 24) | ((u32_t)hdr[iphl + 9] << 16) |
               ((u32_t)hdr[iphl + 10] << 8) | hdr[iphl + 11];
    c->flags = hdr[iphl + 13];
    c->paylen = (u16_t)(p->tot_len - iphl - thl);
    ncaps++;
  }
  return ERR_OK;
}

/* ---- scenario scaffolding: one ESTABLISHED pcb per scenario ---- */

#define MSS         TCP_MSS
#define GRO_FRAMES  44          /* the port's RXGRO_MAX_FRAMES */

static struct netif ackagg_netif;
static struct test_tcp_txcounters txcounters;
static char stream[GRO_FRAMES * MSS + 4 * MSS]; /* the peer's byte stream */
static u32_t irs;                                /* seqno of stream[0] */

static struct tcp_pcb *scenario_pcb(struct test_tcp_counters *counters, u32_t exlen)
{
  struct tcp_pcb *pcb;
  memset(counters, 0, sizeof(*counters));
  counters->expected_data = stream;
  counters->expected_data_len = exlen;
  pcb = test_tcp_new_counters_pcb(counters);
  CHECK(pcb != NULL);
  tcp_set_state(pcb, ESTABLISHED, &test_local_ip, &test_remote_ip,
                TEST_LOCAL_PORT, TEST_REMOTE_PORT);
  irs = pcb->rcv_nxt;
  ncaps = 0;
  return pcb;
}

/* Input `len` stream bytes starting at stream offset `off`; returns how many
   segments lwIP put on the wire while processing it. */
static int input(struct tcp_pcb *pcb, u32_t off, u32_t len)
{
  int before = ncaps;
  /* the helper's seqno offset is relative to the CURRENT rcv_nxt */
  struct pbuf *p = tcp_create_rx_segment_wnd(pcb, &stream[off], len,
                                             irs + off - pcb->rcv_nxt, 0,
                                             TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  test_tcp_input(p, &ackagg_netif);
  return ncaps - before;
}

/* The last captured segment is a pure ACK for stream offset `off`. */
static void check_pure_ack(struct tcp_pcb *pcb, u32_t off)
{
  const struct cap *c = &caps[ncaps - 1];
  CHECK(c->flags == TCP_ACK);
  CHECK(c->paylen == 0);
  CHECK(c->ackno == irs + off);
  CHECK(pcb->rcv_nxt == irs + off);
  CHECK(!(pcb->flags & (TF_ACK_DELAY | TF_ACK_NOW)));
}

/* ---- scenarios ---- */

/* The stock rule must survive untouched for ordinary segments: the first
   full-sized segment is delayed, the second one releases the ACK. */
static void test_single_segments_unchanged(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, 2 * MSS);

  CHECK(input(pcb, 0, MSS) == 0);
  CHECK(pcb->flags & TF_ACK_DELAY);
  CHECK(pcb->rcv_nxt == irs + MSS);

  CHECK(input(pcb, MSS, MSS) == 1);
  check_pure_ack(pcb, 2 * MSS);
  CHECK(counters.recved_bytes == 2 * MSS);

  tcp_remove_all();
}

/* One byte over TCP_MSS is already more than any peer can put in a segment. */
static void test_threshold(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, MSS + 1);

#if LWIP_TCP_ACK_AGGREGATES
  CHECK(input(pcb, 0, MSS + 1) == 1);
  check_pure_ack(pcb, MSS + 1);
#else
  CHECK(input(pcb, 0, MSS + 1) == 0);
  CHECK(pcb->flags & TF_ACK_DELAY);
  CHECK(pcb->rcv_nxt == irs + MSS + 1);
#endif

  tcp_remove_all();
}

/* A full GRO aggregate — what rx_gro hands tcp_input for a 64 KB reply. Stock
   lwIP sees "one segment" and delays the ACK for 44 segments of data. */
static void test_full_aggregate(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, GRO_FRAMES * MSS);

#if LWIP_TCP_ACK_AGGREGATES
  CHECK(input(pcb, 0, GRO_FRAMES * MSS) == 1);
  check_pure_ack(pcb, GRO_FRAMES * MSS);
#else
  CHECK(input(pcb, 0, GRO_FRAMES * MSS) == 0);
  CHECK(pcb->flags & TF_ACK_DELAY);
#endif
  CHECK(pcb->rcv_nxt == irs + GRO_FRAMES * MSS);
  CHECK(counters.recved_bytes == GRO_FRAMES * MSS);

  tcp_remove_all();
}

/* An aggregate arriving on top of a pending delayed ACK: one ACK covers both,
   and no delayed ACK is left behind for the timer. */
static void test_aggregate_after_delayed(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, 4 * MSS);

  CHECK(input(pcb, 0, MSS) == 0);
  CHECK(pcb->flags & TF_ACK_DELAY);

  /* stock lwIP acknowledges here too, but only as "the second segment" */
  CHECK(input(pcb, MSS, 3 * MSS) == 1);
  check_pure_ack(pcb, 4 * MSS);

  tcp_remove_all();
}

/* Back-to-back aggregates, as in a multi-flight reply: every flight is
   acknowledged. Stock lwIP acknowledges every other one. */
static void test_consecutive_aggregates(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, 3 * 8 * MSS);

#if LWIP_TCP_ACK_AGGREGATES
  for (u32_t i = 0; i < 3; i++) {
    CHECK(input(pcb, i * 8 * MSS, 8 * MSS) == 1);
    check_pure_ack(pcb, (i + 1) * 8 * MSS);
  }
#else
  CHECK(input(pcb, 0, 8 * MSS) == 0);
  CHECK(input(pcb, 8 * MSS, 8 * MSS) == 1);
  CHECK(input(pcb, 16 * MSS, 8 * MSS) == 0);
  CHECK(pcb->rcv_nxt == irs + 3 * 8 * MSS);
#endif

  tcp_remove_all();
}

/* A retransmission that fills a hole releases the queued ooseq segment: two
   segments' worth becomes deliverable from one ordinary-sized input. RFC 5681
   4.2 wants that acknowledged immediately; stock lwIP delays it. */
static void test_gap_fill(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, 2 * MSS);

  /* the second segment first: queued on ooseq, answered with a duplicate ACK */
  CHECK(input(pcb, MSS, MSS) == 1);
  CHECK(caps[ncaps - 1].ackno == irs);
  CHECK(pcb->ooseq != NULL);
  CHECK(counters.recved_bytes == 0);

#if LWIP_TCP_ACK_AGGREGATES
  CHECK(input(pcb, 0, MSS) == 1);
  check_pure_ack(pcb, 2 * MSS);
#else
  CHECK(input(pcb, 0, MSS) == 0);
  CHECK(pcb->flags & TF_ACK_DELAY);
  CHECK(pcb->rcv_nxt == irs + 2 * MSS);
#endif
  CHECK(pcb->ooseq == NULL);
  CHECK(counters.recved_bytes == 2 * MSS);

  tcp_remove_all();
}

/* An out-of-sequence aggregate is not an in-sequence advance: it is queued and
   draws the ordinary duplicate ACK, nothing more. */
static void test_ooseq_aggregate(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, 0);

  CHECK(input(pcb, MSS, 3 * MSS) == 1);
  CHECK(caps[ncaps - 1].ackno == irs);
  CHECK(pcb->rcv_nxt == irs);
  CHECK(counters.recved_bytes == 0);

  tcp_remove_all();
}

int main(void)
{
  for (size_t i = 0; i < sizeof(stream); i++) {
    stream[i] = (char)(i * 31 + (i >> 8));
  }

  lwip_init();
  test_tcp_init_netif(&ackagg_netif, &txcounters, &test_local_ip, &test_netmask);
  ackagg_netif.output = ackagg_netif_output;

  test_single_segments_unchanged();
  test_threshold();
  test_full_aggregate();
  test_aggregate_after_delayed();
  test_consecutive_aggregates();
  test_gap_fill();
  test_ooseq_aggregate();

  printf("ackagg_test (LWIP_TCP_ACK_AGGREGATES=%d): all scenarios passed (%d checks)\n",
         LWIP_TCP_ACK_AGGREGATES, checks_run);
  return 0;
}
