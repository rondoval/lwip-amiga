# LWIP_TCP_URG regression tests

Deterministic host tests (gcc + ASAN/UBSAN) for the fork's TCP urgent-data
core — the `LWIP_TCP_URG` patch carried on top of `STABLE-2_2_1_RELEASE`.
Reuses the fuzz harness shim (`../fuzz/shim`) and lwIP's own unit-test
helper (`tcp_helper.c`); nothing is vendored.

```
./build.sh && ./urg_test
```

Covered, all against the wire image captured at `netif->output`:

- TX: `TCP_WRITE_FLAG_URG` arms `snd_up` one past the write; the segment
  carries URG with BSD-convention `urgp`; a following plain write does not.
- TX: an RTO retransmit re-emits URG with the same `urgp` (output-time
  recomputation from `snd_up`, not enqueue-time state).
- TX: after the mark retires (ACK at/past `snd_up`), a retransmit of a
  partially-acked segment whose header still carries URG from its first
  transmission is actively cleaned (the stale-URG hazard).
- TX: a mark > 64 KiB ahead of a segment's seqno clamps `urgp` at 0xFFFF.
- RX: an in-window URG segment latches `rcv_up = seqno + urgp - 1` with
  `TF_URG_RCV`; latest mark wins while unconsumed; a duplicate's old mark
  never regresses `rcv_up`.
- RX: an out-of-order URG segment latches at arrival (before its data can
  be delivered).
- RX: a partially-duplicate segment whose header seqno is rewritten by the
  first-edge trim still yields the mark computed from the seqno as sent.

The mark's consumer side (extraction, excision, `SO_OOBINLINE`,
`SIOCATMARK`) lives in the bsdsocket layer and is HW-validated via
bsdsocktest; this harness pins down the protocol core only.
