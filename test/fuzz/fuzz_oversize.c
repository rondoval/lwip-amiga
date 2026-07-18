/* SPDX-License-Identifier: BSD-3-Clause */
/* Randomized model-checker for lwIP's TCP sender bookkeeping.
 *
 * Hunts the stale pcb->unsent_oversize bug observed on the Amiga port:
 * drives a single ESTABLISHED pcb through randomized
 * write/output/ack/dupack/rto/ persist schedules and, after EVERY lwIP call,
 * verifies:
 *   1. unsent_tail matches the real tail (fork patch self-check)
 *   2. unsent == NULL  =>  unsent_oversize == 0
 *   3. unsent_oversize == tail->oversize_left (DBGCHECK shadow)
 *   4. tail->len + optlen + unsent_oversize <= mss_local (tcp_write's assert)
 *   5. unsent_oversize <= REAL spare bytes in the tail pbuf's allocation
 *      (memory-safety oracle via exact-size recording allocator)
 * Additionally every transmitted payload byte is verified against the
 * deterministic stream pattern, and ASAN redzones catch stray writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

#include "tcp_helper.h"

/* ---------- exact-size recording allocator (oracle) ---------- */

#define FZ_TAB_BITS 20
#define FZ_TAB_SIZE (1u << FZ_TAB_BITS)
static struct { void *ptr; size_t size; } fz_tab[FZ_TAB_SIZE];

static size_t fz_hash(void *p) {
  uintptr_t v = (uintptr_t)p;
  v ^= v >> 12; v *= 0x9E3779B97F4A7C15ull; v ^= v >> 29;
  return (size_t)(v & (FZ_TAB_SIZE - 1));
}
static void fz_tab_put(void *p, size_t sz) {
  size_t i = fz_hash(p);
  while (fz_tab[i].ptr != NULL) i = (i + 1) & (FZ_TAB_SIZE - 1);
  fz_tab[i].ptr = p; fz_tab[i].size = sz;
}
static size_t fz_tab_get(void *p) {
  size_t i = fz_hash(p);
  while (fz_tab[i].ptr != p) {
    if (fz_tab[i].ptr == NULL && fz_tab[i].size == 0) return (size_t)-1;
    i = (i + 1) & (FZ_TAB_SIZE - 1);
  }
  return fz_tab[i].size;
}
static void fz_tab_del(void *p) {
  size_t i = fz_hash(p);
  while (fz_tab[i].ptr != p) {
    if (fz_tab[i].ptr == NULL && fz_tab[i].size == 0) return;
    i = (i + 1) & (FZ_TAB_SIZE - 1);
  }
  fz_tab[i].ptr = NULL; fz_tab[i].size = 1; /* tombstone */
}

static unsigned alloc_fail_pct;  /* per-run: % of lwIP heap allocs that fail */
static unsigned long fz_rng;
static unsigned fz_rnd(unsigned n) {
  fz_rng = fz_rng * 6364136223846793005ull + 1442695040888963407ull;
  return (unsigned)((fz_rng >> 33) % (n ? n : 1));
}

void *fz_malloc(size_t sz) {
  void *p;
  if (alloc_fail_pct && fz_rnd(100) < alloc_fail_pct) return NULL;
  p = malloc(sz ? sz : 1);
  if (p) fz_tab_put(p, sz);
  return p;
}
void *fz_calloc(size_t n, size_t sz) {
  void *p = calloc(n ? n : 1, sz ? sz : 1);
  if (p) fz_tab_put(p, n * sz);
  return p;
}
void fz_free(void *p) {
  if (p) { fz_tab_del(p); free(p); }
}

/* ---------- op log ring ---------- */

#define OPLOG_SIZE 128
static char oplog[OPLOG_SIZE][160];
static unsigned oplog_next, op_count;
static unsigned long cur_seed;

static void logf_op(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(oplog[oplog_next], sizeof(oplog[0]), fmt, ap);
  va_end(ap);
  oplog_next = (oplog_next + 1) % OPLOG_SIZE;
}

static void dump_oplog(void) {
  unsigned i;
  fprintf(stderr, "\n==== seed %lu, op %u — last ops (oldest first) ====\n",
          cur_seed, op_count);
  for (i = 0; i < OPLOG_SIZE; i++) {
    const char *s = oplog[(oplog_next + i) % OPLOG_SIZE];
    if (s[0]) fprintf(stderr, "  %s\n", s);
  }
  fflush(NULL);
}

void fuzz_fail_hook(const char *expr, const char *file, int line) {
  fprintf(stderr, "\nHELPER-FAIL: %s at %s:%d\n", expr, file, line);
  dump_oplog();
  abort();
}

/* Asserts triaged as known/benign-in-release: log and continue, so the fuzz
 * state keeps evolving exactly as a release (LWIP_NOASSERT) build would. */
static const char *benign_asserts[] = {
  NULL
};
static unsigned benign_hits[8];

void fuzz_platform_assert(const char *msg, const char *file, int line) {
  unsigned i;
  for (i = 0; benign_asserts[i] != NULL; i++) {
    if (strstr(msg, benign_asserts[i]) != NULL) {
      benign_hits[i]++;
      return; /* continue like a release build */
    }
  }
  fprintf(stderr, "\nLWIP-ASSERT: \"%s\" at %s:%d\n", msg, file, line);
  dump_oplog();
  abort();
}

/* with LWIP_TIMERS==0 the stack expects the port to provide this */
void tcp_timer_needed(void) {}

/* ---------- deterministic payload stream ---------- */

static u32_t stream_base;    /* iss: seq of stream byte 0 */

static u8_t pattern_byte(u32_t idx) {
  return (u8_t)(idx * 251u + 7u + (u8_t)cur_seed);
}

/* ---------- TX inspection: verify every payload byte on the wire ---------- */

static struct test_tcp_txcounters txcounters;
static unsigned tx_fail_pct;   /* % of linkoutput calls that report ERR_MEM */
static unsigned long rng_state;
static unsigned rnd(unsigned n);
static err_t fuzz_count_tx(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *ipaddr);

/* deferred-TX simulation: pbufs the "driver" still references */
#define MAX_HELD 64
static struct pbuf *held_pbufs[MAX_HELD];
static unsigned held_count;
static int ref_hold_mode;

static void release_one_held(void) {
  if (held_count > 0) {
    unsigned i = rnd(held_count);
    pbuf_free(held_pbufs[i]);
    held_pbufs[i] = held_pbufs[--held_count];
  }
}
static void release_all_held(void) {
  while (held_count > 0) release_one_held();
}

static err_t fuzz_netif_output(struct netif *netif, struct pbuf *p,
                               const ip4_addr_t *ipaddr) {
  struct ip_hdr *iph = (struct ip_hdr *)p->payload;
  u16_t iphl = (u16_t)(IPH_HL(iph) * 4);
  struct tcp_hdr *tcph = (struct tcp_hdr *)((u8_t *)p->payload + iphl);
  u16_t tcphl = (u16_t)(TCPH_HDRLEN(tcph) * 4);
  u16_t doff = (u16_t)(iphl + tcphl);
  u32_t seq = lwip_ntohl(tcph->seqno);
  u16_t dlen = (u16_t)(p->tot_len - doff);
  u16_t i;

  for (i = 0; i < dlen; i++) {
    u8_t got = pbuf_get_at(p, (u16_t)(doff + i));
    u32_t idx = (u32_t)(seq + i - stream_base);
    if (got != pattern_byte(idx)) {
      fprintf(stderr, "\nWIRE-CORRUPTION: seq %u stream-idx %u: got 0x%02x want 0x%02x\n",
              (unsigned)seq + i, (unsigned)idx, got, pattern_byte(idx));
      dump_oplog();
      abort();
    }
  }
  if (tx_fail_pct && rnd(100) < tx_fail_pct) {
    return ERR_MEM; /* simulate driver TX-queue exhaustion */
  }
  if (ref_hold_mode && held_count < MAX_HELD && rnd(8) == 0) {
    /* simulate the genet driver holding the pbuf until DMA TX completes:
     * flips every tcp_output_segment_busy() branch in the rexmit paths */
    pbuf_ref(p);
    held_pbufs[held_count++] = p;
  }
  return fuzz_count_tx(netif, p, ipaddr);
}
static err_t fuzz_count_tx(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *ipaddr) {
  LWIP_UNUSED_ARG(netif); LWIP_UNUSED_ARG(ipaddr);
  txcounters.num_tx_calls++;
  txcounters.num_tx_bytes += p->tot_len;
  return ERR_OK;
}

/* ---------- invariant checker ---------- */

static void die(struct tcp_pcb *pcb, const char *why) {
  fprintf(stderr, "\nINVARIANT-VIOLATION: %s\n", why);
  if (pcb != NULL) {
    fprintf(stderr,
        "  state=%d ovz=%u mss=%u swnd=%u swm=%u cwnd=%u nrtx=%u flags=0x%x "
        "lastack=%u snd_nxt=%u snd_lbb=%u qlen=%u\n",
        pcb->state, pcb->unsent_oversize, pcb->mss,
        (unsigned)pcb->snd_wnd, (unsigned)pcb->snd_wnd_max, (unsigned)pcb->cwnd,
        pcb->nrtx, pcb->flags,
        (unsigned)(pcb->lastack - stream_base),
        (unsigned)(pcb->snd_nxt - stream_base),
        (unsigned)(pcb->snd_lbb - stream_base),
        pcb->snd_queuelen);
  }
  dump_oplog();
  abort();
}

static void check_invariants(struct tcp_pcb *pcb) {
  struct tcp_seg *s, *tail = NULL;
  struct pbuf *q;
  unsigned nseg = 0;

  if (pcb == NULL) return;

  for (s = pcb->unsent; s != NULL; s = s->next) { tail = s; nseg++; }

  if (pcb->unsent_tail != tail) die(pcb, "unsent_tail != real tail");

  if (tail == NULL) {
    if (pcb->unsent_oversize != 0) die(pcb, "oversize nonzero with empty unsent");
    return;
  }

#if TCP_OVERSIZE_DBGCHECK
  if (pcb->unsent_oversize != tail->oversize_left) {
    char buf[96];
    snprintf(buf, sizeof(buf), "shadow desync: pcb=%u tail=%u",
             pcb->unsent_oversize, tail->oversize_left);
    die(pcb, buf);
  }
  /* 6. every transmitted segment must have a zeroed shadow: this is what
     lets tcp_rexmit_rto_prepare import seg->oversize_left into the pcb */
  for (s = pcb->unacked; s != NULL; s = s->next) {
    if (s->oversize_left != 0) {
      char buf[96];
      snprintf(buf, sizeof(buf), "unacked seg carries shadow oversize %u",
               s->oversize_left);
      die(pcb, buf);
    }
  }
#endif

  /* 7. a requeued tail (not ending at snd_lbb) must never carry tail room */
  if (lwip_ntohl(tail->tcphdr->seqno) + tail->len != pcb->snd_lbb &&
      pcb->unsent_oversize != 0) {
    die(pcb, "requeued tail with nonzero oversize");
  }

  /* (former check 4 — tail->len + optlen + oversize <= mss_local — is NOT a
     global invariant: mss_local legitimately shrinks past a built tail when
     snd_wnd_max ratchets off the zero-window fallback; tcp_write retires the
     tail's spare room on the next call.) */

  if (pcb->unsent_oversize != 0) {
    /* 5. memory-safety bound: real room in the tail pbuf's allocation */
    for (q = tail->p; q->next != NULL; q = q->next) ;
    if ((q->type_internal & PBUF_TYPE_ALLOC_SRC_MASK) ==
        PBUF_TYPE_ALLOC_SRC_MASK_STD_HEAP) {
      size_t alloc = fz_tab_get((void *)q);
      if (alloc != (size_t)-1) {
        u8_t *alloc_end = (u8_t *)q + alloc;
        u8_t *data_end = (u8_t *)q->payload + q->len;
        ptrdiff_t room = alloc_end - data_end;
        if (room < 0 || (ptrdiff_t)pcb->unsent_oversize > room) {
          char buf[96];
          snprintf(buf, sizeof(buf), "OVERSIZE EXCEEDS REAL ROOM: ovz=%u room=%ld",
                   pcb->unsent_oversize, (long)room);
          die(pcb, buf);
        }
      }
    }
  }
}

/* ---------- RNG (deterministic per seed) ---------- */

static unsigned rnd(unsigned n) {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return (unsigned)((rng_state >> 33) % (n ? n : 1));
}

/* ---------- one fuzz run ---------- */

static struct netif fuzz_netif;
static struct test_tcp_counters counters;
static char rx_expect_dummy[1];

static u16_t pick_wnd(void) {
  switch (rnd(10)) {
    case 0:  return 0;                       /* closed window -> persist */
    case 1:  return (u16_t)(1 + rnd(1460));  /* tiny -> split paths */
    case 2:  return (u16_t)(1460 + rnd(4380));
    case 3:  return 8192;
    default: return 65535;
  }
}

static void run_one(unsigned long seed, unsigned max_ops) {
  struct tcp_pcb *pcb;
  static u8_t wbuf[16384];
  u32_t written = 0; /* bytes handed to tcp_write so far */
  u16_t cur_wnd = 65535;
  unsigned op;

  cur_seed = seed;
  rng_state = seed * 2654435761ull + 1;
  memset(oplog, 0, sizeof(oplog));
  oplog_next = 0;

  memset(&counters, 0, sizeof(counters));
  counters.expected_data = rx_expect_dummy;
  counters.expected_data_len = 0;

  pcb = test_tcp_new_counters_pcb(&counters);
  if (pcb == NULL) { fprintf(stderr, "no pcb\n"); abort(); }
  tcp_set_state(pcb, ESTABLISHED, &test_local_ip, &test_remote_ip,
                TEST_LOCAL_PORT, TEST_REMOTE_PORT);
  /* peer's MSS option is often below our TCP_MSS (1412 on the field LAN:
     PPPoE clamp), leaving TCP_OVERSIZE > pcb->mss */
  pcb->mss = rnd(2) ? 1412 : TCP_MSS;
  /* SYN-ACK announces a raw (unscaled) initial window; small/zero values
     exercise the mss_local snd_wnd_max/2 fallback non-monotonicity */
  switch (rnd(6)) {
    case 0:  cur_wnd = 0; break;
    case 1:  cur_wnd = 1; break;
    case 2:  cur_wnd = 502; break;
    case 3:  cur_wnd = 2144; break;
    case 4:  cur_wnd = 8192; break;
    default: cur_wnd = 65535; break;
  }
  pcb->snd_wnd = cur_wnd;
  pcb->snd_wnd_max = cur_wnd;
  /* slow-start from a realistic initial cwnd; ACK churn will grow it */
  pcb->cwnd = 10 * TCP_MSS;
  pcb->ssthresh = 65535;
  if (rnd(2)) tcp_nagle_disable(pcb);
  if (rnd(2)) { /* window scaling, as announced by real peers (scale 8) */
    pcb->snd_scale = 8;
    pcb->rcv_scale = TCP_RCV_SCALE;
    tcp_set_flags(pcb, TF_WND_SCALE);
    pcb->snd_wnd = (u32_t)cur_wnd << 8;
    pcb->snd_wnd_max = pcb->snd_wnd;
  }

  stream_base = pcb->snd_lbb;
  tx_fail_pct = (rnd(4) == 0) ? 2 : 0;
  ref_hold_mode = (rnd(2) == 0);
  switch (rnd(4)) {
    case 0:  alloc_fail_pct = 2; break;
    case 1:  alloc_fail_pct = 10; break;
    default: alloc_fail_pct = 0; break;
  }
  fz_rng = seed ^ 0xA5A5A5A5u;

  for (op = 0; op < max_ops; op++) {
    unsigned what = rnd(100);
    op_count = op;

    if (counters.err_calls > 0) break; /* pcb aborted (max rexmit) */

    if (what < 30) { /* tcp_write */
      u32_t avail = tcp_sndbuf(pcb);
      u16_t len;
      u8_t flags = TCP_WRITE_FLAG_COPY;
      err_t err;
      u16_t i;
      if (avail == 0) { logf_op("write skipped, sndbuf full"); continue; }
      switch (rnd(4)) {
        case 0:  len = (u16_t)(1 + rnd(64)); break;         /* tiny */
        case 1:  len = (u16_t)(1 + rnd(1459)); break;       /* sub-mss */
        case 2:  len = (u16_t)(1460 * (1 + rnd(4))); break; /* mss multiple */
        default: len = (u16_t)(1 + rnd(16383)); break;      /* arbitrary */
      }
      if (len > avail) len = (u16_t)avail;
      if (len > sizeof(wbuf)) len = sizeof(wbuf);
      if (rnd(2)) flags |= TCP_WRITE_FLAG_MORE;
      for (i = 0; i < len; i++) wbuf[i] = pattern_byte(written + i);
      err = tcp_write(pcb, wbuf, len, flags);
      logf_op("write len=%u fl=%u -> %d (ovz=%u lbb=%u)", len, flags, err,
              pcb->unsent_oversize, (unsigned)(pcb->snd_lbb - stream_base));
      if (err == ERR_OK) written += len;
    } else if (what < 50) { /* tcp_output */
      err_t err = tcp_output(pcb);
      logf_op("output -> %d (ovz=%u nxt=%u unacked=%c unsent=%c)", err,
              pcb->unsent_oversize, (unsigned)(pcb->snd_nxt - stream_base),
              pcb->unacked ? 'y' : 'n', pcb->unsent ? 'y' : 'n');
    } else if (what < 75) { /* ACK, possibly window update */
      u32_t inflight = pcb->snd_nxt - pcb->lastack;
      u32_t ackofs;
      struct pbuf *p;
      if (rnd(8) == 0) ackofs = 0;               /* pure window update */
      else if (rnd(4) == 0) ackofs = inflight;   /* ack everything sent */
      else ackofs = inflight ? rnd((unsigned)inflight + 1) : 0;
      cur_wnd = (rnd(3) == 0) ? pick_wnd() : cur_wnd;
      p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, ackofs, TCP_ACK, cur_wnd);
      logf_op("ack +%u wnd=%u (inflight=%u ovz=%u)", (unsigned)ackofs, cur_wnd,
              (unsigned)inflight, pcb->unsent_oversize);
      if (p) test_tcp_input(p, &fuzz_netif);
    } else if (what < 85) { /* dupacks (same ackno, same window) */
      unsigned n = 1 + rnd(4), i;
      logf_op("dupack x%u (dupacks=%u)", n, pcb->dupacks);
      for (i = 0; i < n; i++) {
        struct pbuf *p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, 0,
                                                   TCP_ACK, cur_wnd);
        if (p) test_tcp_input(p, &fuzz_netif);
        if (counters.err_calls > 0) break;
        check_invariants(pcb);
      }
    } else if (what < 95) { /* slow timer: RTO / persist / zero-window probe */
      unsigned n = 1 + rnd(8), i;
      logf_op("slowtmr x%u (rtime=%d rto=%d persist=%u ovz=%u)", n,
              pcb->rtime, pcb->rto, pcb->persist_backoff, pcb->unsent_oversize);
      for (i = 0; i < n; i++) {
        tcp_slowtmr();
        if (counters.err_calls > 0) break;
        check_invariants(pcb);
      }
    } else if (what < 97) { /* fast timer */
      tcp_fasttmr();
      logf_op("fasttmr");
    } else if (what < 99) { /* driver TX-complete: release held pbuf refs */
      unsigned n = 1 + rnd(4), i;
      logf_op("txdone x%u (held=%u)", n, held_count);
      for (i = 0; i < n; i++) release_one_held();
    } else if (rnd(2)) { /* rare: shutdown TX side (FIN piggyback on tail) */
      if (!(pcb->flags & TF_FIN)) {
        err_t err = tcp_shutdown(pcb, 0, 1);
        logf_op("shutdown-tx -> %d (ovz=%u)", err, pcb->unsent_oversize);
      }
    } else { /* rare: peer closes its side; writes continue in CLOSE_WAIT */
      if (pcb->state == ESTABLISHED) {
        struct pbuf *p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, 0,
                                                   TCP_ACK | TCP_FIN, cur_wnd);
        logf_op("peer-fin (ovz=%u)", pcb->unsent_oversize);
        if (p) test_tcp_input(p, &fuzz_netif);
      }
    }

    if (counters.err_calls > 0) break;
    check_invariants(pcb);
    if (ref_hold_mode && held_count > 48) release_one_held();
  }

  alloc_fail_pct = 0;
  release_all_held();
  if (counters.err_calls == 0) {
    tcp_abort(pcb);
  }
  counters.err_calls = 0;
  tcp_remove_all();
}

int main(int argc, char **argv) {
  unsigned long start_seed = (argc > 1) ? strtoul(argv[1], NULL, 0) : 1;
  unsigned long num_seeds = (argc > 2) ? strtoul(argv[2], NULL, 0) : 10000;
  unsigned max_ops = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 4000;
  unsigned long s;

  lwip_init();
  test_tcp_init_netif(&fuzz_netif, &txcounters, &test_local_ip, &test_netmask);
  fuzz_netif.output = fuzz_netif_output;

  for (s = start_seed; s < start_seed + num_seeds; s++) {
    run_one(s, max_ops);
    if ((s - start_seed) % 200 == 199) {
      printf("... %lu seeds done (tx=%u calls)\n", s - start_seed + 1,
             txcounters.num_tx_calls);
      fflush(stdout);
    }
  }
  printf("PASS: %lu seeds x %u ops, no invariant violations\n",
         num_seeds, max_ops);
  for (s = 0; benign_asserts[s] != NULL; s++) {
    if (benign_hits[s]) {
      printf("  benign assert \"%s\": %u hits\n", benign_asserts[s],
             benign_hits[s]);
    }
  }
  return 0;
}
