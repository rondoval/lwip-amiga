# lwip-amiga notes

Conceptual phase complete 2026-07-10 — decisions and open questions captured in
[planning/concept.md](planning/concept.md). (Working title "stack-ng" → renamed
**lwip-amiga**.)

TL;DR: lwIP core (submodule + Amiga port layer), core-locking direct path,
bsdsocket.library built on the raw API with Exec signals as the blocking primitive,
clean break from SANA-II (adapter netif possible later), new batched zero-copy
`netdev` driver ABI designed for wire speed, genet.device first. IPv4+DHCP+DNS scope,
fresh minimal config, DHCP by default. GPL-2.0-or-later own code, BSD-2 ABI headers.

Refinements 2026-07-10 (round 2): lwip-amiga is a submodule of emu68-driver-stack
(standalone build = Linux host harness only; ABI header lives here, genet includes it);
RX buffers are driver-generated with a release hook (geometry/headroom/copy-break stay
driver-internal), TX pool drawn from a driver-provided DMA allocator — PCIe DMA reaches
only Emu68 expansion RAM (not Chip, not Zorro/motherboard Fast), so reachability is
strictly driver-side knowledge.

## Phase 0 status (2026-07-10)

- [x] Repo scaffolded (README, LICENSE.md + LICENSES/, .gitignore, include/netdev.h placeholder)
- [x] lwIP submodule pinned to STABLE-2_2_1_RELEASE
- [x] Host harness green: `./build/harness/smoke` — 256 KB loopback TCP echo, verified byte-exact
- [ ] Wire components/lwip-amiga into emu68-driver-stack (submodule + cmake)
- [ ] Empty-but-linking bsdsocket.library from the container build
