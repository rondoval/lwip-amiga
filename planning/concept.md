# lwip-amiga — Concept

*Conceptual-phase document. Captures the problem, the decisions made, and the open
questions for the design phase. 2026-07-10. (Working title was "stack-ng"; the
project is now **lwip-amiga**.)*

## Problem statement

TCP/IP stacks available on AmigaOS are outdated, closed source, or both. The SANA-II
driver interface is copy-based and offload-blind: it forces bounce buffers, prevents
checksum offloading, and caps what genet.device (BCM GENET on Pi4/CM4 under
PiStorm/Emu68) can deliver.

lwip-amiga is three deliverables:

1. **A modern TCP/IP core** for AmigaOS 3.2.
2. **bsdsocket.library** — the application-facing contract, compatible with the API
   the NDK 3.2R4 documents (`SANA+RoadshowTCP-IP/`: `bsdsocket_lib.sfd`, `netinclude/`,
   `bsdsocket.doc`).
3. **A modern driver ABI** replacing SANA-II between the stack and NIC drivers,
   unlocking zero-copy DMA and hardware offloads. genet.device is the first driver.

## Decisions (locked in conceptual phase)

| Decision | Choice | Rationale |
|---|---|---|
| Stack core | **Port lwIP** | BSD-3, active upstream, single-CPU design fits Amiga; pbuf chains map to scatter-gather DMA; per-netif csum-offload switches exist; host-testable on Linux. Its offload ceiling (no TSO/GRO) matches what GENET hardware actually offers. |
| lwIP tracking | **Git submodule + port layer** | All our code in the port layer (`sys_arch`, `cc.h`, `lwipopts.h`, custom pbufs), which lwIP is designed to keep separate. Core patches, if ever needed, go through a `patches/` dir or upstream. |
| Runtime model | **Core locking, direct path** | `LWIP_TCPIP_CORE_LOCKING(_INPUT)`: app tasks run stack code in caller context under a core semaphore; driver task injects RX under the same lock; a small stack task handles timers only. No mailbox marshalling, minimal context switches — same philosophy as the Poseidon direct-call unification. |
| bsdsocket glue | **Built on lwIP raw/callback API** (not netconn/sockets) | lwIP's own socket layer blocks on private semaphores, incompatible with `WaitSelect()` + signal-mask semantics. On the raw API, Exec signals are the one true blocking primitive. Per-opener bases (per-base errno, fd table, SocketBaseTags) live entirely in our layer. |
| Legacy interop | **Clean break** (v1 = new stack + new ABI + genet only) | No SANA-II backend, no SANA-II personality requirement on drivers. The lwIP `netif` seam keeps a later SANA-II adapter netif possible as isolated, bounded work — nothing in the core may assume the new ABI. |
| Protocol scope v1 | **IPv4 + TCP + UDP + ICMP + DHCP client + DNS resolver** | Smallest thing that replaces Roadshow for daily use. Design stays IPv6-ready (lwIP config flag + future bsdsocket API extensions). |
| Perf bar for ABI v1 | **Push toward wire speed** | ABI designed for hundreds of Mbit from day one: batching, interrupt-coalescing hints, room for multiple rings and future offloads in the descriptor format. Retrofitting batching into a per-packet ABI is an ABI break — spend the complexity now. |
| App-compat definition of done | **AmiSSL + browsers/mail (IBrowse, NetSurf, YAM) + file transfer (FTP, wget, smbfs)** | Servers/daemons (`ObtainSocket`/`ReleaseSocket` handoff corner) deferred past v1. |
| Config UX | **Fresh, minimal** | One plain config file (ENVARC:) + a small set of new CLI tools. DHCP by default: a bare install just works. No Roadshow file-format compatibility. |
| Repo integration | **Submodule of emu68-driver-stack** | Atomic ABI churn with genet, in-tree use of emu68-common (dma_mem, EMU68_DEBUG_BACKEND). lwip-amiga stays its own repo (own history, public release); standalone builds target the Linux host harness only — all Amiga builds go through the superproject. The driver ABI header lives in lwip-amiga (the stack defines the contract); genet includes it via the submodule. Nesting: emu68-driver-stack → lwip-amiga → lwIP (docker build needs recursive submodule init). |
| RX buffer ownership | **Driver-generated** | The driver allocates/recycles its own RX buffers and hands filled ones up with a release hook. Geometry (GENET's 2 KB buffers, 64-byte status-block headroom, ring depth) never crosses the ABI, copy-break stays possible driver-side, and it is the lwIP `pbuf_custom` idiom. Starvation risk is symmetric with stack-pre-posting (socket queues hold buffers either way), so encapsulation decides. |
| Naming | **Repo: `lwip-amiga`; driver ABI: `netdev`** (`include/netdev.h`) | Repo named after the upstream being ported, like poseidon-backport. Product branding decided at release. Stack task name and CLI tool prefix still open. |
| License | **GPL-2.0-or-later own code; BSD-2-Clause for `include/` (netdev ABI headers); lwIP stays BSD-3** | SPDX header per file is authoritative (fleet convention). The ABI header is permissive so any driver or stack, under any license, may implement the contract — same spirit as SANA-II being an open spec. |
| Hosting | **Private GitHub (rondoval/lwip-amiga) until release** | Real submodule URLs in the superproject from day one; flip public at release like the Poseidon plan. |

## Architecture

```
 Applications (AmiSSL, IBrowse, YAM, wget, ...)
        │  bsdsocket.library API  (NDK 3.2R4 contract, version 4)
 ┌──────▼──────────────────────────────────────┐
 │ bsdsocket.library                           │  per-opener bases, errno, fd tables,
 │   socket layer on lwIP raw API              │  WaitSelect via Exec signals
 │ ┌───────────────────────────────────────┐   │
 │ │ lwIP core (submodule)                 │   │  caller-context under core lock;
 │ │   + Amiga port layer (ours)           │   │  stack task = timers only
 │ └───────────────────────────────────────┘   │
 │   netif: newabi-netif   (netif: sana2 later)│
 └──────┬──────────────────────────────────────┘
        │  new driver ABI (direct-call, context-based, batched, zero-copy)
 ┌──────▼──────────┐
 │ genet.device    │  first implementation; others later
 └─────────────────┘
```

### Execution model

Three customers of the core lock, zero copies until the socket boundary:

1. **App tasks** — socket calls execute stack code in caller context under the lock.
2. **Driver task** — RX injection via `netif->input` under the lock (Exec semaphores
   cannot be taken from interrupts, so injection is task-context by construction).
3. **Stack task** — lwIP timeouts only (retransmit, DHCP renew, DNS timeouts).

Socket events wake blocked tasks by `Signal()`; `WaitSelect()` waits on socket events
and the caller's signal mask with the same primitive.

### Driver ABI — design principles

The performance unlock. Founding ideas:

- **Split buffer ownership: driver-owned RX, stack-owned TX.** Only the driver knows
  what its DMA engine reaches (the emu68-common `dma_mem` predicate on PiStorm).
  RX: the driver allocates and recycles its own receive buffers — geometry, headroom
  (GENET's 64-byte status block), ring depth, copy-break policy all stay
  driver-internal — and hands filled buffers up as ABI-neutral descriptors
  (ptr, len, offload flags, release hook); the port layer wraps them as custom pbufs
  whose free recycles them to the driver. TX: the stack draws its transmit pool from
  a DMA allocator the driver provides at attach, and submits scatter-gather
  descriptor lists. No lwIP types in the ABI, no bounce buffers, no copies either way.
- **Batched by design.** TX: array of descriptors per submit, one doorbell per batch.
  RX: completion delivers a batch of filled buffers per lock acquisition; recycling
  is batched too. Note: pbuf free runs in app-task context under the core lock, so
  the driver's recycle path must be foreign-task-safe (lock-free SPSC ring).
- **Capability negotiation at attach.** Driver advertises: TX/RX csum offload
  (IPv4/TCP/UDP), TX buffer constraints (alignment — DMA reachability is implicit in
  the driver-provided allocator), ring counts, coalescing support. Stack enables
  what it uses; descriptor format leaves room for future offloads and multi-ring
  without an ABI break. Driver-owned RX means hardware headroom quirks never appear
  in the ABI at all.
- **Interrupt-coalescing hints.** Stack can set usecs/frames moderation (GENET DMA
  rings support timeout-based coalescing).
- **Context ABI, ROM-able.** Follows the emu68 driver conventions: no writable
  globals, every exported call carries an explicit context argument, lifecycle =
  `OpenDevice()` + direct-call attach handshake exchanging mutual context pointers
  and callback tables with versioning (the Poseidon `NSCMD_USB_ATTACH` pattern).
- **Also in scope:** link-state event callback, multicast filter add/remove,
  promiscuous mode, MAC get/set, 64-bit stats.

### bsdsocket.library — semantic corners to get right

- Per-opener library base: per-base `errno`/`h_errno` (and app-provided errno pointer
  via `SocketBaseTags`), per-base fd table.
- `WaitSelect()` with signal mask; non-blocking IO; async event notification tags.
- `ObtainSocket`/`ReleaseSocket` task handoff — deferred past v1 with the server use
  case, but the fd-table design must not preclude it.
- Library version ≥ 4 (apps check this).
- `usergroup.library` stub may be needed by some ports — investigate in design phase.

## Platform constraints (inputs to all of the above)

- PiStorm/Emu68 on Pi4/CM4: PCIe DMA reaches ONLY the RAM Emu68 itself provides
  (pri-40 expansion memory) — not Chip RAM, not motherboard or Zorro Fast RAM, not
  unaligned buffers. Reachability is the emu68-common `dma_mem` predicate and is
  strictly driver-side knowledge → the stack never AllocMem's packet memory itself;
  it always comes through driver-provided allocators.
- Exec semaphores are task-context only → no stack entry from interrupts, ever.
- 68k is big-endian = network byte order; no swapping on header field access.
- NDK 3.2 only; container build (amiga-build-container), poseidon-style `build.sh`;
  switchable debug backend convention.
- Emu68 RAM is fast (ARM-speed); the one unavoidable copy (socket boundary) is cheap
  when both sides are Emu68 expansion RAM — which apps get by default, it being the
  largest, fastest MEMF_FAST in the system.

## Risks / things to validate early

1. **lwIP TCP throughput ceiling.** Window scaling (`LWIP_WND_SCALE`), SACK-out, and
   buffer sizing must be validated against the wire-speed ambition early — host-side
   benchmark first, then an on-target spike before the ABI is frozen.
2. **lwIP DNS resolver adequacy.** It is minimal (UDP-only, small cache). Browsers
   want many concurrent queries; may need a beefier resolver in our layer.
3. **bsdsocket semantic corners** (errno tags, WaitSelect edge cases, inet_ntoa
   static-buffer semantics, netdb accessors) — drive with real apps (AmiSSL demos)
   early, not at the end.
4. **Coexistence story.** Only one `bsdsocket.library` can exist at runtime; install
   replaces Roadshow. Uninstall/switch-back must be clean.
5. ~~License of our code~~ — resolved: GPL-2.0-or-later + BSD-2 ABI headers (see
   decisions table).

## Open questions for the design phase

- Exact attach handshake + callback-table layout of the driver ABI (draft header).
- Custom pbuf lifecycle details: RX pool sizing vs socket-queue hold times,
  refcounting across driver/stack, recycle-ring design (foreign-task-safe),
  CachePreDMA/CachePostDMA semantics on Emu68.
- Multi-netif TX later: egress netif isn't always known at pbuf-alloc time — per-netif
  TX pools need a routing-aware alloc or a fallback copy (non-issue for v1's single NIC).
- lwipopts.h first cut: memory model (pools vs heap), TCP window/buffer sizes.
- DNS resolver: extend lwIP's vs own resolver over UDP sockets in our layer.
- Config file format + CLI tool set (names, minimal surface).
- Test strategy details: Linux host harness (core + socket-layer logic against tap
  netif), on-target amiga_devtest-style tests, throughput benchmark tool for Amiga.
- Naming: `bsdsocket.library` fixed, repo `lwip-amiga`, ABI `netdev` — still open:
  stack task name, CLI tool prefix, release branding.

## Phase plan (coarse)

- **Phase 0 — skeleton:** repo layout, lwIP submodule (pinned STABLE-2_2_1_RELEASE),
  lwip-amiga wired into emu68-driver-stack as a submodule (recursive init in the
  docker build); container build producing an empty-but-linking bsdsocket.library;
  Linux host harness building the same core standalone. *Status 2026-07-10: repo
  scaffolded, lwIP pinned, host harness green (256 KB loopback TCP echo verified);
  Amiga-side library skeleton + container wiring remain.*
- **Phase 1 — driver ABI design:** header draft + genet.device implementation behind
  a build flag; loopback + static-IP ping on target.
- **Phase 2 — socket layer:** bsdsocket.library on raw API; DHCP, DNS; wget/AmiFTP
  class apps working.
- **Phase 3 — the definition of done:** AmiSSL, browsers, mail; benchmarks vs
  Roadshow+SANA-II published.
