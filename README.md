# lwip-amiga

A modern TCP/IP stack for AmigaOS 3.2, built on [lwIP](https://savannah.nongnu.org/projects/lwip/).

**Status: pre-alpha, Phase 0 (skeleton).** Nothing here is usable yet.

## What this is

The TCP/IP stacks available on AmigaOS are outdated, closed source, or both, and the
SANA-II driver interface is copy-based and offload-blind. This project delivers:

- **A TCP/IP core** — lwIP (git submodule) with an AmigaOS port layer, running in
  core-locking direct-path mode (application tasks execute stack code in their own
  context under a core semaphore).
- **bsdsocket.library** — the standard application API, compatible with the
  Roadshow-era contract documented in NDK 3.2 (`SANA+RoadshowTCP-IP/`), built
  directly on the lwIP raw API with Exec signals as the blocking primitive.
- **The `netdev` driver ABI** — a SANA-II replacement for NIC drivers: direct-call,
  context-based, batched, zero-copy, with capability negotiation (checksum offload,
  interrupt coalescing). First implementation: `genet.device` (BCM GENET on
  Pi4/CM4 under PiStorm/Emu68, in
  [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack)).

Design record: [planning/concept.md](planning/concept.md).

## Layout

- `planning/` — design documents
- `include/` — the `netdev` driver ABI headers (BSD-2-Clause; the public contract)
- `lwip/` — lwIP core, git submodule (BSD-3-Clause, unmodified)
- `harness/` — Linux host harness: builds the lwIP core natively and runs smoke tests
- `port/` — AmigaOS port layer (Phase 1+)
- `src/` — bsdsocket.library (Phase 2+)

## Building

Amiga binaries are built through the emu68-driver-stack superproject (this repo is a
submodule there). The standalone build targets only the Linux host harness:

```sh
git submodule update --init
cmake -B build && cmake --build build
./build/harness/smoke
```

## License

- Own code: GPL-2.0-or-later (SPDX headers per file are authoritative)
- `include/` (netdev ABI headers): BSD-2-Clause, so any driver or stack may
  implement the contract
- `lwip/` submodule: BSD-3-Clause (upstream lwIP license)

See [LICENSE.md](LICENSE.md).
