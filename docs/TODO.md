# lwip-amiga — outstanding work & investigations

Forward-looking tracker: what works, what is left to build, and the open investigations.
For how the stack is put together, see [architecture.md](architecture.md).

## Status

- **netdev ABI + genet datapath** — validated on real PiStorm/CM4 hardware: DHCP binds,
  ICMP is answered, TCP flows, and the TSB/RSB checksum offloads are active. The zero-copy
  vertical slice works end to end.
- **`bsdsocket.library`** — exercised on hardware (speed tests, Amiga Explorer, browser
  traffic) and functional; 76 of 121 LVOs implemented. The full per-LVO map — what is
  done and the decision on each stub — is [bsdsocket-lvo-coverage.md](bsdsocket-lvo-coverage.md).
  Throughput is being actively optimized (see below) and a few of the later LVO waves
  still want a HW retest.


## Configuration UX & tools

Still open:

- **Release branding.** No Roadshow file-format compatibility.

## Ongoing investigations

### Throughput optimization

Download is meaningfully below line rate while upload saturates the uplink. Profiling
shows the ceiling is **serialized stack-side work under the core lock** (`ns_Core` is held
for the large majority of wall-clock time during a receive; the RX cost is dominated by
lwIP's input path plus lock waits, while the driver's ring mechanics are a few microseconds
per frame). The work is measurement-first, using the `netdev-stats` loss-point counters and
`sockbench` against a LAN peer.

Levers in flight / planned:

- **Lock-scope reduction** — copy received data out with the lock released; chunk `tcp_write`
  and break the lock between chunks *only when another task is waiting* (whole-MSS chunks,
  to avoid runt segments). These reduce the unit task's lock-wait and the RX ring
  starvation caused by long TX holds. (HW retest pending.) The RAW RX checksum fold now
  also runs before the lock (HW retest pending).
- **RX pool sizing** — autonegotiated at ATTACH: the stack declares its hold budget
  (`nda_RxHoldReq`, windows × streams + slack) and the driver sizes ring + budget;
  `RX_POOL_BUFS` in `genet.prefs` remains as an absolute operator override.
- **Coalescing tuning** — interrupt moderation (usecs/frames) via `SET_COALESCE`.
- **Window sizing (done)** — `TCP_WND`/`TCP_SND_BUF` raised 256 KB → 1 MB with
  `TCP_RCV_SCALE = 5` (advertises the full 1 MB past the 65535<<4 = 1048560 ceiling).
  The hold budget (`4*ceil(TCP_WND/TCP_MSS)+64` wraps) auto-scaled with it, so the
  genet RX pool grew ~1.6 MB → ~5.9 MB. This lifts only the *clean-path* window
  ceiling (throughput = `TCP_WND/RTT`): ~70 → ~270 Mbit at 30 ms RTT. It does
  nothing for the LAN download ceiling (still `ns_Core`-bound) or for lossy paths
  (still RTO recovery, see below). Going past 1 MB is wasted until those two land.
- **Cheap RX cluster (HW-measured 2026-07-16: TCP a few % up)** — levers landed:
  - *`tcp_recved` batched across the whole `recv()` drain pass* (one window update per
    pass instead of one per detached run, u16-chunked, flushed before any wait/return) —
    shortens the app-task lock hold that starves RX injection.
  - *RX lock-cadence sweep knobs made explicit.* `NDIF_RX_CHUNK` raised to 64 as the
    per-hold ceiling so a whole driver batch always lands in one lock hold; the sweep is
    now driver-only via `ND_RX_BATCH` + `budget` (both documented as the knobs). Values
    left at 32/64 — the 32→48→64 sweep is a HW step (watch rxprof `us/fr` vs `maxflush`).
  - *Recycle `CachePreDMA` elision — attempted, REVERTED, do not retry.* Eliding the
    pre-arm clean+invalidate collapsed `sockbench udprx` to ~1% while netdev-stats showed
    line rate arriving with zero driver drops. RX pbufs are **not** read-only: lwIP's
    `ip4_reass` writes its helper struct over the IP header of every queued fragment,
    leaving dirty lines; a dirty line at DMA time corrupts the frame regardless of the
    post-DMA invalidate (eviction writes it back over the payload, and Cortex-A cores
    execute `dc ivac` on a dirty line as clean+invalidate). The pre-arm
    clean+invalidate is the platform-wide device-writes-RAM contract (same as nvme's
    `nvme_cache_flush(to_device=FALSE)` and xhci's IN-transfer flush + whole-line gate).
- **Socket-layer RX visibility + fairness (implemented with the above)** —
  the root base's `dgramRxDrops` counts UDP/RAW datagrams dropped at the socket queue
  (`SB_DGRAM_QMAX`) — a loss point netdev-stats cannot see — surfaced as `udps_fullsock`
  in `GetNetworkStatistics(NETSTATUS_udp)`; and `ndif_rx_input` yields `ns_Core` every `NDIF_RX_YIELD_STRIDE` frames
  when a task is queued on the lock, so an unflow-controlled RX blast cannot starve the
  draining app.
- **Remaining big-ticket lever (chosen, not yet taken)** — the dominant RX cost is
  per-segment lwIP protocol processing under `ns_Core` (~58 % of wall, ~23 µs/segment);
  none of the cheap cluster touches it. Software RX coalescing (GRO-lite: merge in-order
  same-flow segments within an RX batch, feed lwIP once) is the structural way to cut how
  often the protocol stack is traversed. Pre-lock csum offload removes GRO's hardest part.

### TCP loss handling

lwIP emits SACKs but does not act on *received* ones, so a window with multiple losses
recovers by RTO rather than selective retransmission. This is acceptable for the clean
wired LAN this stack targets (loss ≈ 0), and pre-SACK Amiga stacks share the ceiling, but
bulk transfers over lossy/congested paths will underperform. Fixing it is upstream lwIP
work, not ABI work — revisit only if real-world use hits it.

## Resolved, with caveats

- **TX corruption under sustained heavy upload** — traced to lwIP's `pcb->unsent_oversize`
  going stale under heavy loss/retransmit pressure with `TCP_OVERSIZE = TCP_MSS`, causing a
  write past a pbuf's real allocation and a cascade of downstream corruption. Contained by
  an **interim clamp** (`TCPGUARD-OVZ`) that zeroes `unsent_oversize` before every
  `tcp_write` (losing only small-write tail coalescing). The clamp is HW-validated; the
  bookkeeping desync's root cause inside lwIP is a candidate for an upstream report and a
  proper fix.
- **Whole-stack ~1 s freeze at the core-lock layer** — root-caused to Executive's dynamic
  scheduler starving the (formerly pri-5) driver and stack tasks behind a CPU-burning app
  task. Fixed by moving both to **priority 10** (out of the managed band). Residual hazard:
  `ns_Core` is held at the caller's priority during every LVO, so a managed-band app
  holding it can still be starved (classic inversion, no priority inheritance in exec);
  hold times are microseconds with debug off, and a `SetTaskPri` ceiling in
  `netstack_lock`/`unlock` is the known workaround if it ever bites.

## Upstream lwIP report candidates

- Reproducible stale `unsent_oversize` with `TCP_OVERSIZE = TCP_MSS` under heavy RTO
  (the TX-corruption class above).
- Write-in-`SYN_SENT` hazards: `pcb->mss` shrinks to the peer's MSS option and `snd_wnd_max`
  resets at SYN-ACK, either of which can trip a `tcp_write` assert on pre-queued full-MSS
  data.
