/* SPDX-License-Identifier: BSD-3-Clause */
/* Deterministic regression tests for the fork's LWIP_TCP_URG core.
 *
 * TX oracle: every transmitted segment is captured at netif->output and its
 * wire header decoded; the tests assert exactly when URG appears, that the
 * urgent pointer is recomputed per transmission (splits/retransmits), that
 * it clamps at 0xFFFF, and that a stale URG bit is actively cleared when a
 * partially-acked segment retransmits after the mark retired.
 *
 * RX oracle: crafted in-window segments (tcp_helper + a local URG poke that
 * re-checksums) must latch pcb->rcv_up/TF_URG_RCV at ARRIVAL with
 * BSD urgp semantics (mark = seqno + urgp - 1), obey latest-wins, latch
 * from ooseq arrivals, and stay correct when the first-edge trim rewrites
 * the header seqno of a partially-duplicate segment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/inet_chksum.h"
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
  u32_t seqno;
  u16_t urgp;
  u16_t paylen;
  u8_t flags;
};
static struct cap caps[64];
static int ncaps;

static err_t urg_netif_output(struct netif *netif, struct pbuf *p,
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
    struct cap *c = &caps[ncaps < 64 ? ncaps : 63];
    c->seqno = ((u32_t)hdr[iphl + 4] << 24) | ((u32_t)hdr[iphl + 5] << 16) |
               ((u32_t)hdr[iphl + 6] << 8) | hdr[iphl + 7];
    c->flags = hdr[iphl + 13];
    c->urgp = (u16_t)(((u16_t)hdr[iphl + 18] << 8) | hdr[iphl + 19]);
    c->paylen = (u16_t)(p->tot_len - iphl - thl);
    ncaps++;
  }
  return ERR_OK;
}

/* ---- RX crafting: set URG+urgp on a helper-built segment, re-checksum ---- */

static void urg_poke(struct pbuf *p, u16_t urgp)
{
  struct tcp_hdr *th;
  pbuf_remove_header(p, IP_HLEN);
  th = (struct tcp_hdr *)p->payload;
  TCPH_SET_FLAG(th, TCP_URG);
  th->urgp = lwip_htons(urgp);
  th->chksum = 0;
  th->chksum = ip_chksum_pseudo(p, IP_PROTO_TCP, p->tot_len,
                                &test_remote_ip, &test_local_ip);
  pbuf_add_header(p, IP_HLEN);
}

/* ---- scenario scaffolding: one ESTABLISHED pcb per scenario ---- */

static struct netif urg_netif;
static struct test_tcp_txcounters txcounters;

static struct tcp_pcb *scenario_pcb(struct test_tcp_counters *counters,
                                    char *expected, u32_t exlen)
{
  struct tcp_pcb *pcb;
  memset(counters, 0, sizeof(*counters));
  counters->expected_data = expected;
  counters->expected_data_len = exlen;
  pcb = test_tcp_new_counters_pcb(counters);
  CHECK(pcb != NULL);
  tcp_set_state(pcb, ESTABLISHED, &test_local_ip, &test_remote_ip,
                TEST_LOCAL_PORT, TEST_REMOTE_PORT);
  /* tcp_set_state bypasses the handshake: give the pcb a live send window
     and cwnd or tcp_output sends nothing */
  pcb->snd_wnd = 65535;
  pcb->snd_wnd_max = 65535;
  pcb->cwnd = 65535;
  tcp_nagle_disable(pcb); /* small back-to-back writes must hit the wire */
  ncaps = 0;
  return pcb;
}

static void scenario_end(void)
{
  tcp_remove_all();
}

/* ---- TX scenarios ---- */

/* Basic arm + emit, no URG on a following plain write, retirement on ACK. */
static void test_tx_basic(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, NULL, 0);
  u32_t base = pcb->snd_lbb;
  struct pbuf *p;

  CHECK(tcp_write(pcb, "AB", 2, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_URG) == ERR_OK);
  CHECK(pcb->urgflags & TF_URG_SND);
  CHECK(pcb->snd_up == base + 2);
  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 1);
  CHECK(caps[0].seqno == base);
  CHECK(caps[0].flags & TCP_URG);
  CHECK(caps[0].urgp == 2);
  CHECK(caps[0].paylen == 2);

  CHECK(tcp_write(pcb, "CD", 2, TCP_WRITE_FLAG_COPY) == ERR_OK);
  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 2);
  CHECK(caps[1].seqno == base + 2);
  CHECK(!(caps[1].flags & TCP_URG));
  CHECK(caps[1].urgp == 0);

  /* ACK everything: mark retires */
  p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, 4, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  test_tcp_input(p, &urg_netif);
  CHECK(!(pcb->urgflags & TF_URG_SND));

  scenario_end();
}

/* An RTO retransmit of the marked segment re-emits URG with the same urgp. */
static void test_tx_rexmit_keeps_urg(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, NULL, 0);
  u32_t base = pcb->snd_lbb;

  CHECK(tcp_write(pcb, "AB", 2, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_URG) == ERR_OK);
  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 1 && (caps[0].flags & TCP_URG) && caps[0].urgp == 2);

  tcp_rexmit_rto(pcb);
  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 2);
  CHECK(caps[1].seqno == base);
  CHECK(caps[1].flags & TCP_URG);
  CHECK(caps[1].urgp == 2);

  scenario_end();
}

/* The stale-URG hazard: a plain write coalesces into the marked segment's
 * oversize room, the mark retires via a partial ACK, and the segment then
 * retransmits — its header still carries URG from the first transmission
 * and tcp_output_segment must actively clear it. */
static void test_tx_stale_clear_after_retire(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, NULL, 0);
  u32_t base = pcb->snd_lbb;
  struct pbuf *p;

  CHECK(tcp_write(pcb, "AB", 2, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_URG) == ERR_OK);
  CHECK(tcp_write(pcb, "CD", 2, TCP_WRITE_FLAG_COPY) == ERR_OK);
  /* both writes must share one segment, putting the mark mid-segment */
  CHECK(pcb->unsent != NULL && pcb->unsent->next == NULL);
  CHECK(pcb->snd_up == base + 2);

  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 1);
  CHECK((caps[0].flags & TCP_URG) && caps[0].urgp == 2 && caps[0].paylen == 4);

  /* partial ACK exactly at the mark: retires TF_URG_SND, segment stays */
  p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, 2, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  test_tcp_input(p, &urg_netif);
  CHECK(!(pcb->urgflags & TF_URG_SND));

  tcp_rexmit_rto(pcb);
  CHECK(tcp_output(pcb) == ERR_OK);
  CHECK(ncaps == 2);
  CHECK(!(caps[1].flags & TCP_URG));
  CHECK(caps[1].urgp == 0);

  scenario_end();
}

/* A mark more than 64 KiB past a segment's seqno clamps urgp at 0xFFFF. */
static void test_tx_clamp(void)
{
  struct test_tcp_counters counters;
  struct tcp_pcb *pcb = scenario_pcb(&counters, NULL, 0);
  u32_t base = pcb->snd_lbb;
  char *buf = malloc(35000);

  CHECK(buf != NULL);
  memset(buf, 'x', 35000);
  CHECK(tcp_write(pcb, buf, 35000, TCP_WRITE_FLAG_COPY) == ERR_OK);
  CHECK(tcp_write(pcb, buf, 35000, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_URG) == ERR_OK);
  CHECK(pcb->snd_up == base + 70000);

  CHECK(tcp_output(pcb) == ERR_OK); /* cwnd limits how much; seg 1 is enough */
  CHECK(ncaps >= 1);
  CHECK(caps[0].seqno == base);
  CHECK(caps[0].flags & TCP_URG);
  CHECK(caps[0].urgp == 0xFFFF);

  free(buf);
  scenario_end();
}

/* ---- RX scenarios ---- */

/* In-sequence URG latches mark = seqno + urgp - 1; latest mark wins while
 * unconsumed; an old duplicate's mark never regresses rcv_up. */
static void test_rx_latch_and_latest_wins(void)
{
  struct test_tcp_counters counters;
  static char expected[] = "WXYZABCD";
  struct tcp_pcb *pcb = scenario_pcb(&counters, expected, 8);
  u32_t rn0 = pcb->rcv_nxt;
  u32_t rn1;
  struct pbuf *p;

  p = tcp_create_rx_segment_wnd(pcb, &expected[0], 4, 0, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  urg_poke(p, 2);
  test_tcp_input(p, &urg_netif);
  CHECK(pcb->urgflags & TF_URG_RCV);
  CHECK(pcb->rcv_up == rn0 + 1);
  CHECK(counters.recv_calls == 1 && counters.recved_bytes == 4);

  /* later mark overwrites the still-unconsumed one */
  rn1 = pcb->rcv_nxt;
  CHECK(rn1 == rn0 + 4);
  p = tcp_create_rx_segment_wnd(pcb, &expected[4], 4, 0, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  urg_poke(p, 4);
  test_tcp_input(p, &urg_netif);
  CHECK(pcb->rcv_up == rn1 + 3);
  CHECK(counters.recved_bytes == 8);

  /* full duplicate with an old mark: rcv_up must not regress */
  p = tcp_create_rx_segment_wnd(pcb, &expected[4], 4, (u32_t)-4, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  urg_poke(p, 1);
  test_tcp_input(p, &urg_netif);
  CHECK(pcb->rcv_up == rn1 + 3);
  CHECK(counters.recved_bytes == 8);

  scenario_end();
}

/* An out-of-order URG segment latches at arrival, before delivery. */
static void test_rx_ooseq_latch(void)
{
  struct test_tcp_counters counters;
  static char expected[] = "EFGHIJKL";
  struct tcp_pcb *pcb = scenario_pcb(&counters, expected, 8);
  u32_t rn0 = pcb->rcv_nxt;
  struct pbuf *p;

  p = tcp_create_rx_segment_wnd(pcb, &expected[4], 4, 4, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  urg_poke(p, 1);
  test_tcp_input(p, &urg_netif);
  CHECK(pcb->urgflags & TF_URG_RCV);
  CHECK(pcb->rcv_up == rn0 + 4);
  CHECK(counters.recv_calls == 0); /* still queued on ooseq */

  p = tcp_create_rx_segment_wnd(pcb, &expected[0], 4, 0, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  test_tcp_input(p, &urg_netif);
  CHECK(counters.recved_bytes == 8); /* gap filled, coalesced delivery */
  CHECK(pcb->rcv_up == rn0 + 4);

  scenario_end();
}

/* A partially-duplicate segment gets its header seqno rewritten by the
 * first-edge trim; the mark must be computed from the seqno as sent. */
static void test_rx_trim_keeps_mark(void)
{
  struct test_tcp_counters counters;
  static char expected[] = "MNOPQRST";
  struct tcp_pcb *pcb = scenario_pcb(&counters, expected, 8);
  u32_t rn1;
  struct pbuf *p;

  p = tcp_create_rx_segment_wnd(pcb, &expected[0], 4, 0, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  test_tcp_input(p, &urg_netif);
  CHECK(counters.recved_bytes == 4);
  rn1 = pcb->rcv_nxt;

  /* [rn1-4, rn1+4) with urgp 8: mark = (rn1-4) + 8 - 1 = rn1 + 3.
   * A post-trim latch would misread it as rn1 + 7. */
  p = tcp_create_rx_segment_wnd(pcb, &expected[0], 8, (u32_t)-4, 0, TCP_ACK, 0xFFFF);
  CHECK(p != NULL);
  urg_poke(p, 8);
  test_tcp_input(p, &urg_netif);
  CHECK(pcb->urgflags & TF_URG_RCV);
  CHECK(pcb->rcv_up == rn1 + 3);
  CHECK(counters.recved_bytes == 8);

  scenario_end();
}

int main(void)
{
  lwip_init();
  test_tcp_init_netif(&urg_netif, &txcounters, &test_local_ip, &test_netmask);
  urg_netif.output = urg_netif_output;

  test_tx_basic();
  test_tx_rexmit_keeps_urg();
  test_tx_stale_clear_after_retire();
  test_tx_clamp();
  test_rx_latch_and_latest_wins();
  test_rx_ooseq_latch();
  test_rx_trim_keeps_mark();

  printf("urg_test: all scenarios passed (%d checks)\n", checks_run);
  return 0;
}
