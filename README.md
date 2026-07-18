# lwip-amiga

> **Releases:** this component is developed and built as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack). That repository
> publishes the downloadable `.lha` and bundled documentation; this one is source-only
> and versioned via git tags.

A modern TCP/IP stack for AmigaOS 3.2, built on
[lwIP](https://savannah.nongnu.org/projects/lwip/).

**Status: early release.** The zero-copy `netdev` datapath (through `genet.device`) is
validated on real PiStorm/CM4 hardware — DHCP binds, ICMP is answered, and TCP flows with
hardware checksum offload. The `bsdsocket.library` socket layer is implemented and
exercised on hardware; it installs to `LIBS:`. Throughput is still being optimized and a
few areas carry known limitations (see below and [docs/TODO.md](docs/TODO.md)).

## What this is

The TCP/IP stacks available on AmigaOS are outdated, closed source, or both, and the
SANA-II driver interface is copy-based and offload-blind. This project delivers three
layers, built bottom-up:

- **`netdev` — a new NIC driver ABI** (`include/netdev.h`, BSD-3-Clause). A clean-break
  SANA-II replacement: direct-call, context-based (the `xhci.device` context-ABI idiom),
  batched and zero-copy in both directions, with capability negotiation (checksum
  offload, interrupt coalescing, link events). The ABI header is the open public
  contract — any driver or stack may implement it. First implementation:
  `genet.device` (BCM GENET on Pi4/CM4 under PiStorm/Emu68, in
  [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack)).
- **A TCP/IP core** — lwIP (git submodule) plus an AmigaOS port layer,
  running in **core-locking direct-path** mode: application tasks execute stack code in
  their own context under a single core semaphore, with Exec signals as the blocking
  primitive. Built as the `netstack` static library.
- **`bsdsocket.library`** — the standard application socket API, compatible with the
  Roadshow-era contract documented in NDK 3.2 (`SANA+RoadshowTCP-IP/`), built directly
  on the lwIP raw API. Per-opener child library bases, an in-library stack task, and
  DHCP/DNS out of the box.

Scope is IPv4 + DHCP + DNS, with a fresh minimal lwIP config and DHCP by default.

See [docs/architecture.md](docs/architecture.md) for how the stack works, and
[docs/TODO.md](docs/TODO.md) for outstanding work and open investigations.

## Layout

- `include/netdev.h` — the `netdev` driver ABI header. Installed and exported as the `Netdev` CMake package
  (`Netdev::netdev_headers`), which is how `genet.device` consumes it.
- `lwip/` — lwIP core, git submodule (BSD-3-Clause, forked from `STABLE-2_2_1_RELEASE`).
- `port/amiga/` — AmigaOS port layer: `lwipopts.h` (the core-locking config),
  `netstack.c` (singleton, core lock, `EClock`→ms time, DMA-aware heap), `netdev_if.c`
  (lwIP netif ⇄ `netdev` glue: zero-copy TX scatter-gather + L4 checksum offsets, RX
  `pbuf_custom` recycle, link events).
- `src/bsdsocket/` — `bsdsocket.library` (socket layer, LVO table, stack task).
- `src/sockbench/` — LAN TCP/UDP throughput benchmark over `bsdsocket.library` (developer
  tool; built but not shipped).
- `sfd/`, `scripts/gen-vectors.py` — the NDK `bsdsocket` SFD and the generator that emits
  the full 139-slot LVO vector table from it.
- `docs/` — architecture and TODO documents.

## Features

**`netdev` ABI (v1)** — driver-owned RX buffers with a release hook + prefix-consume
batches, driver-provided DMA allocator for the TX pool, offset-based TX L4 checksum
(GENET TSB shape), dual RX checksum reporting (VALID / RAW), declarative RX filter,
STOP/DETACH quiesce protocol. genet's negotiated caps: interrupt coalescing, link
events, TX L4 checksum offload, RX checksum (RAW + VALID).

**`bsdsocket.library`** — 72 of 121 LVOs implemented, including:

- Socket core: `socket` `bind` `listen` `accept` `connect` `send`/`sendto`
  `recv`/`recvfrom` `shutdown` `CloseSocket` `setsockopt`/`getsockopt`
  `getsockname`/`getpeername` `IoctlSocket` (`FIONBIO`/`FIONREAD`).
- `WaitSelect` (full autodoc semantics: user signal mask, break repost,
  sets-unmodified-on-error), `SetSocketSignals`, `getdtablesize`.
- `errno` family + `SocketBaseTagList`; the inet utilities
  (`inet_addr`/`aton`/`ntop`/`pton`, `Inet_NtoA`, `LnaOf`/`NetOf`/`MakeAddr`/`network`).
- Resolver: `gethostbyname`/`_r` (lwIP DNS, blocking), `gethostbyaddr`/`_r`,
  `gethostname`, `gethostid`; netdb iterators (`getproto*`, `getserv*`, `getnet*`).
- `Dup2Socket`, `sendmsg`/`recvmsg` (iovec scatter-gather),
  `ObtainSocket`/`ReleaseSocket`/`ReleaseCopyOfSocket`, `vsyslog`/`syslog`,
  DNS-server config + default-domain LVOs.
- `getaddrinfo`/`freeaddrinfo`/`gai_strerror`/`getnameinfo` (RFC 2553 IPv4 subset) and the
  full AmiTCP V4 async event API (`GetSocketEvents`, `SO_EVENTMASK`).

TCP receive consumes driver RX pbufs with zero copies until the app buffer; TCP send
copies into the DMA-backed heap with `sndbuf` blocking. UDP/RAW get bounded datagram
queues. Blocking is done with Exec signals under the core lock, so there are no lost
wakeups; break signals (Ctrl-C by default) surface as `EINTR`.

See [docs/TODO.md](docs/TODO.md) for the full LVO scoreboard (what's stubbed and why).

## Configuration

The stack reads **`ENV:netstack.prefs`** once at startup (the first `OpenLibrary` of
`bsdsocket.library`); keep the master copy in `ENVARC:`. Flat `KEY = VALUE` lines,
case-insensitive keys, `#`/`;` comments, unknown keys ignored. Every key is optional —
with no file at all the stack runs DHCP on `networks/genet.device` unit 0. A commented
template ships as `ENVARC:netstack.prefs.default`.

| Key | Default | Meaning |
|---|---|---|
| `DEVICE` | `networks/genet.device` | netdev driver to open (path form loads from `DEVS:`) |
| `UNIT` | `0` | device unit |
| `MODE` | `DHCP` | `DHCP` or `STATIC` |
| `ADDRESS`, `NETMASK` | — | required for `STATIC` (else the stack falls back to DHCP) |
| `GATEWAY` | — | optional default gateway (`STATIC`) |
| `DNS1`, `DNS2` | — | optional DNS servers (`STATIC`; DHCP supplies its own) |
| `HOSTNAME` | `amiga` | DHCP option 12 and `gethostname()` |

Additional interfaces are a reserved extension (`IF1_`-prefixed keys); see
[docs/architecture.md](docs/architecture.md).

## Building

### Amiga (m68k) binaries

The `netstack` library, `bsdsocket.library`, and the test tools are built through the
**emu68-driver-stack** superproject, which supplies the Bebbo cross-toolchain and
`emu68-common`. From a superproject checkout:

```sh
cmake -S . -B build         # or: ./scripts/docker-build.sh   (no local toolchain)
cmake --build build
```

To work on this component against a superproject build, point its source override at
your checkout:

```sh
cmake -S . -B build -D LWIP_AMIGA_SOURCE_DIR=/home/user/lwip-amiga
cmake --build build
```

The superproject orders `lwip-amiga` before `genet.device` (which depends on the
exported `Netdev` package) and shares the stack-wide debug backend / `EMU68_DEBUG_HIGH`
options — `lwip-amiga` is a valid `EMU68_DEBUG_HIGH` component name.

## Tools

- **`netinfo`** (`C/`, shipped) — read-only ifconfig-like interface status: address,
  netmask, broadcast, MTU, MAC, link state, DHCP/static and DNS. A `bsdsocket.library`
  client using the Roadshow interface-query LVOs (`ObtainInterfaceList` /
  `QueryInterfaceTagList`). Display only — the stack is configured via `netstack.prefs`.
- **`netdev-stats`** (`C/`, shipped) — a second `netdev` opener beside the running stack
  that reads live loss-point counters (`GET_STATS`/`GET_LINK`) and drives interrupt
  coalescing (`SET_COALESCE`) — no ATTACH needed. The release-build replacement for the
  debug driver's serial counters.
- **`sockbench`** (developer tool, built but not shipped) — LAN TCP/UDP throughput benchmark
  over the NDK BSD socket API (`bsdsocket.library`): `sockbench rx|tx|udprx|udptx <host>
  [streams] [seconds] [sizeKB]` runs N nonblocking sockets from one `WaitSelect` loop against
  `scripts/tcp-bench-peer.py` and reports per-stream + aggregate Mb/s (`rx`/`tx` are TCP,
  `udprx`/`udptx` are UDP; `sizeKB` is the TCP buffer or the UDP datagram size). Exercises
  DHCP + DNS + TCP/UDP through the whole zero-copy `netdev` path with hardware checksums active.

## Known limitations

- **Throughput is still being optimized** — download runs below line rate; the ceiling is
  serialized stack-side work under the core lock. See [docs/TODO.md](docs/TODO.md).
- **TCP loss handling** — lwIP emits SACKs but does not act on received ones, so ~2%
  loss can trigger RTO collapse. Fine for a clean wired LAN.
- Declined/stubbed LVOs include the Roadshow interface-*config* family (the interface
  *query* subset is implemented — see `netinfo` above), the route/monitor families,
  `mbuf_*`, `bpf_*`, and (intentionally) the private `ipf_*` filter.

## License

`BSD-3-Clause` throughout — own code, the `include/` `netdev` ABI headers, and the
bundled `lwip/` submodule (© the lwIP developers) all match. Any
driver or stack, under any license, may implement the `netdev` ABI contract. See
[LICENSE](LICENSE), including a note on the `emu68-common` build dependency
used by the Amiga binaries.
