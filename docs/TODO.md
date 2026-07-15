# lwip-amiga — outstanding work & investigations

Forward-looking tracker: what works, what is left to build, and the open investigations.
For how the stack is put together, see [architecture.md](architecture.md).

## Status

- **netdev ABI + genet datapath** — validated on real PiStorm/CM4 hardware: DHCP binds,
  ICMP is answered, TCP flows, and the TSB/RSB checksum offloads are active. The zero-copy
  vertical slice works end to end.
- **`bsdsocket.library`** — exercised on hardware (speed tests, Amiga Explorer, browser
  traffic) and functional; 72 of 121 LVOs implemented. The full per-LVO map — what is
  done and the decision on each stub — is [bsdsocket-lvo-coverage.md](bsdsocket-lvo-coverage.md).
  Throughput is being actively optimized (see below) and a few of the later LVO waves
  still want a HW retest.
- **ABI freeze** — the two open pre-freeze questions (VLAN, jumbo) are now settled; the v1
  layout carries the reserved surface both need, so it can be frozen (see below).

## `netdev` ABI — pre-freeze questions, resolved

- **VLAN** — implemented in-band via lwIP's software VLAN (single VID, `VLAN = vid[,pcp]`
  prefs key). GENET has no hardware tag insert/strip, so the 802.1Q tag rides inside the frame;
  the L4 checksum offloads stay on because the TX/RX offset helpers are tag-aware
  (`ndif_l4_offsets`/`ndif_rx_csum_ok`), matching how mainline Linux drives GENET. The frozen
  ABI also *reserves* hardware-offload surface for a future NIC: `ntd_VlanTci`/`nrd_VlanTci`,
  `NDTF_VLAN_INSERT`/`NDRF_VLAN_STRIPPED`, and `NDCF_VLAN_TX_INSERT`/`NDCF_VLAN_RX_STRIP`.
- **Jumbo frames** — ABI surface finalized: `nda_MtuReq` (IN) / `ndc_Mtu` (OUT) negotiate the
  MTU at ATTACH, and `NDCF_RX_SCATTER` + `NDRF_SOP`/`NDRF_EOP` reserve multi-buffer RX for
  frames past a single descriptor. GENET still pins `ndc_Mtu` at 1500 (no datapath work this
  release); a future jumbo-capable build raises it with no further ABI break.

## Configuration UX & tools

Still open:

- **`GetNetworkStatistics` / `SBTC_HAVE_STATUS_API`** — protocol-level counters
  (ipstat/tcpstat/…), fakeable from lwIP's own stats; separate from the interface query.
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
- **Next candidates** — batching `tcp_recved`, larger `ND_RX_BATCH`.

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
