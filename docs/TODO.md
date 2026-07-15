# lwip-amiga — outstanding work & investigations

Forward-looking tracker: what works, what is left to build, and the open investigations.
For how the stack is put together, see [architecture.md](architecture.md).

## Status

- **netdev ABI + genet datapath** — validated on real PiStorm/CM4 hardware: DHCP binds,
  ICMP is answered, TCP flows, and the TSB/RSB checksum offloads are active. The zero-copy
  vertical slice works end to end.
- **`bsdsocket.library`** — exercised on hardware (speed tests, Amiga Explorer, browser
  traffic) and functional; 72 of 121 LVOs implemented. Throughput is being actively
  optimized (see below) and a few of the later LVO waves still want a HW retest.
- **ABI freeze** — the `netdev` ABI is still a pre-freeze internal prototype (v1); several
  questions below should be settled before it is frozen.

## `bsdsocket.library` — LVO gaps

72 implemented, 49 stubbed. The stubs, grouped:

- **Roadshow interface-config family (10)** — `AddInterfaceTagList`,
  `ConfigureInterfaceTagList`, `Obtain/ReleaseInterfaceList`, `QueryInterfaceTagList`,
  `Create/DeleteAddrAllocMessage`, `Begin/AbortInterfaceConfig`, `RemoveInterface`. The
  stack self-configures via DHCP; a `Query` subset would still serve status tools. Ties
  into the config-UX decision below.
- **Route API (5)** — `Add/Delete/ChangeRouteTagList`, `Free/GetRouteInfo`. lwIP has no
  route table beyond netif + gateway.
- **Monitor / stats (3)** — `Add/RemoveNetMonitorHook`, `GetNetworkStatistics`. The last is
  worth faking from lwIP's own stats eventually.
- **Server support (2)** — `ProcessIsServer`, `ObtainServerSocket` (inetd model). The
  `ObtainSocket`/`ReleaseSocket` family already covers the handoff apps actually need.
- **Roadshow data (3)** — `Obtain/Release/ChangeRoadshowData`.
- **`mbuf_*` (11)** — AmiTCP mbuf compatibility over pbufs; near-zero modern-app value,
  implement only if a real app demands it.
- **`bpf_*` (8)** — packet capture (tcpdump-class); would map to a promiscuous RAW netif
  tap. Future.
- **`ipf_*` (7)** — Roadshow's private IP filter; intentionally never implemented.

Deferred smalls: reverse DNS (`gethostbyaddr`/`getnameinfo` return the dotted quad),
static-IP env config for the stack task, and shared-socket wakeup fan-out (a copy shared
via `ReleaseCopyOfSocket` currently wakes only the last claimant).

## `netdev` ABI — questions to settle before freeze

- **RX pool sizing guidance** — the stack can hold up to `TCP_WND` per socket in receive
  queues; the driver's RX pool must cover ring + expected holding. Driver-internal, but
  worth a sizing note in the header before freeze. (Now runtime-configurable in genet via
  `RX_POOL_BUFS`; see throughput work below.)
- **RAW-mode RX checksum folding** — currently done in software in the glue for
  `CSUM_RAW`-only frames; a glue-local optimization, no ABI change.
- **VLAN** — unsupported in v1. Would be a TX flag + tag field and an RX flag + stripped-tag
  field in the reserved descriptor space.
- **Jumbo frames** — `ndc_Mtu` carries the capability; GENET can do 9k but v1 pins 1500.
  Enabling later is a driver matter plus an MTU-negotiation op.
- **Multi-queue** — descriptor and caps layouts leave room (reserved fields); revisit only
  when profiling shows the single unit task saturated.
- **`SET_MAC` while STARTed** — currently applied immediately; revisit if a driver needs a
  quiesce for it.
- **Header portability** — `netdev.h` uses NDK types (`ULONG`/`APTR`); a mock netdev driver
  on a host harness would need a small typedef shim.
- **Command numbering** — `0x4900` assumes no collision in the NSCMD space used on this
  platform (xhci holds `0x4800..0x481f`). Confirm before freeze.

## Configuration UX & driver selection

- **Driver selection.** The stack task currently hardcodes `OpenDevice("genet.device")`
  (`src/bsdsocket/sb_stack.c`). Needed: let the user configure *which* network driver to
  load, and load it from **`DEVS:Networks/`** — the standard AmigaOS location for network
  device drivers — rather than assuming a single built-in name. Only one driver exists
  today, but the mechanism must not preclude others.
- **Config file & tools.** A fresh, minimal config (one file in `ENVARC:`, DHCP by default)
  and a small set of CLI tools; interface-config LVOs vs pure DHCP self-config; tool
  naming; release branding. No Roadshow file-format compatibility.

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
  starvation caused by long TX holds. (HW retest pending.)
- **RX pool sizing** — the driver's RX pool is runtime-configurable (`RX_POOL_BUFS`), sized
  to cover multiple full receive windows so parallel streams don't dry the pool and starve
  the ring.
- **Coalescing tuning** — interrupt moderation (usecs/frames) via `SET_COALESCE`.
- **Next candidates** — batching `tcp_recved`, folding the RX checksum outside the lock,
  larger `ND_RX_BATCH`, and a larger `TCP_WND` coupled with a larger pool.

### TCP loss handling

lwIP emits SACKs but does not act on *received* ones, so a window with multiple losses
recovers by RTO rather than selective retransmission. This is acceptable for the clean
wired LAN this stack targets (loss ≈ 0), and pre-SACK Amiga stacks share the ceiling, but
bulk transfers over lossy/congested paths will underperform. Fixing it is upstream lwIP
work, not ABI work — revisit only if real-world use hits it.

### Router-side first-packet drop (re-examine)

The original symptom was the first inbound packet of a fresh through-NAT flow after some
TX-idle being lost *upstream* of the NIC (it never reaches the MAC), suspected to be router
hardware-NAT / flow-offload. Part of the early evidence turned out to rest on a probe host
that is unreachable for every device on the LAN, so before pursuing the router theory the
idle-then-connect case should be re-measured against hosts that verifiably answer. A 500 ms
initial TCP RTO in `lwipopts.h` mitigates it for now (the first SYN retransmit beats apps'
1 s connect timeouts). Note this can look like the (now-fixed) core-lock freeze at the app
level — distinguish via the MIB counters (a real router drop shows the frame never reaching
the MAC).

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
