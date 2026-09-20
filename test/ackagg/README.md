# LWIP_TCP_ACK_AGGREGATES regression tests

Deterministic host tests (gcc + ASAN/UBSAN) for the fork's byte-counting ACK
rule: an input that advances `rcv_nxt` by more than `TCP_MSS` is acknowledged at
once, from `tcp_input`. Reuses the fuzz harness shim (`../fuzz/shim`) and lwIP's
own unit-test helper (`tcp_helper.c`); nothing is vendored.

```
./build.sh && ./ackagg_test && ./ackagg_test_off
```

`build.sh` compiles the same scenarios twice: `ackagg_test` with the option on
(the port's setting) and `ackagg_test_off` with stock lwIP behaviour. The second
binary asserts what upstream does with the very same inputs, which is what
proves the scenarios can tell the two apart.

The oracle is the wire: every segment leaving `netif->output` while a crafted
segment is being input is captured and decoded. The helper's receive callback
never calls `tcp_recved()`, so no window update can stand in for the ACK under
test.

Covered:

- Ordinary segments keep the stock rule: the first full-sized segment is
  delayed, the second releases the ACK.
- The threshold: `TCP_MSS + 1` bytes is already more than any peer can put in
  one segment, so it is acknowledged; stock lwIP delays it.
- A full 44-frame GRO aggregate (what `rx_gro` hands `tcp_input` for a 64 KB
  reply) draws exactly one pure ACK for all of it; stock lwIP sets
  `TF_ACK_DELAY` and sends nothing.
- An aggregate on top of a pending delayed ACK: one ACK covers both and no
  delayed ACK is left for the timer.
- Back-to-back aggregates, as in a multi-flight reply: every flight is
  acknowledged; stock lwIP acknowledges every other one.
- A retransmission that fills a hole and releases queued ooseq data is
  acknowledged immediately (RFC 5681 4.2); stock lwIP delays it.
- An out-of-sequence aggregate is not an in-sequence advance: it is queued and
  draws the ordinary duplicate ACK only.

The other half of the ACK policy — the delayed-ACK bound (`NETSTACK_TICK_MS`,
`netstack_tick()` calling `tcp_fasttmr()`) — lives in the port and is validated
on the wire: a server-side capture must show no retransmission of a small reply
after an idle period.
