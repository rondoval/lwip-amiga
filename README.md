# lwip-amiga

> **Releases:** this component is developed and built as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack). That repository
> publishes the downloadable `.lha` and bundled documentation; this one is source-only
> and versioned via git tags.

A modern TCP/IP stack for AmigaOS 3.2, built on
[lwIP](https://savannah.nongnu.org/projects/lwip/).

**Status: pre-alpha.** The zero-copy `netdev` datapath (through `genet.device`) is
validated on real PiStorm/CM4 hardware — DHCP binds and ICMP is answered. The
`bsdsocket.library` socket layer is implemented and builds clean (native + container)
but is **not yet HW-validated**: it stages to `install/Testing/`, not `LIBS:`, until it
passes on-target tests.

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
- **A TCP/IP core** — lwIP (git submodule, unmodified) plus an AmigaOS port layer,
  running in **core-locking direct-path** mode: application tasks execute stack code in
  their own context under a single core semaphore, with Exec signals as the blocking
  primitive. Built as the `netstack` static library.
- **`bsdsocket.library`** — the standard application socket API, compatible with the
  Roadshow-era contract documented in NDK 3.2 (`SANA+RoadshowTCP-IP/`), built directly
  on the lwIP raw API. Per-opener child library bases, an in-library stack task, and
  DHCP/DNS out of the box.

Scope is IPv4 + DHCP + DNS, with a fresh minimal lwIP config and DHCP by default.

Design record: [planning/concept.md](planning/concept.md) and
[planning/netdev-abi.md](planning/netdev-abi.md). Running development log:
[notes.md](notes.md).

## Layout

- `include/netdev.h` — the `netdev` driver ABI header. Installed and exported as the `Netdev` CMake package
  (`Netdev::netdev_headers`), which is how `genet.device` consumes it.
- `lwip/` — lwIP core, git submodule (BSD-3-Clause, pinned to `STABLE-2_2_1_RELEASE`,
  used unmodified).
- `port/amiga/` — AmigaOS port layer: `lwipopts.h` (the core-locking config),
  `netstack.c` (singleton, core lock, `EClock`→ms time, DMA-aware heap), `netdev_if.c`
  (lwIP netif ⇄ `netdev` glue: zero-copy TX scatter-gather + L4 checksum offsets, RX
  `pbuf_custom` recycle, link events).
- `src/bsdsocket/` — `bsdsocket.library` 4.0 (socket layer, LVO table, stack task).
- `src/socktest/` — `bsdsocket.library` end-to-end test (installs to `C/`).
- `sfd/`, `scripts/gen-vectors.py` — the NDK `bsdsocket` SFD and the generator that emits
  the full 139-slot LVO vector table from it.
- `planning/` — design documents.

## Features

**`netdev` ABI (v1)** — driver-owned RX buffers with a release hook + prefix-consume
batches, driver-provided DMA allocator for the TX pool, offset-based TX L4 checksum
(GENET TSB shape), dual RX checksum reporting (VALID / RAW), declarative RX filter,
STOP/DETACH quiesce protocol. genet's negotiated caps: interrupt coalescing, link
events, TX L4 checksum offload, RX checksum (RAW + VALID).

**`bsdsocket.library`** — 67 of 121 LVOs implemented, including:

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

TCP receive consumes driver RX pbufs with zero copies until the app buffer; TCP send
copies into the DMA-backed heap with `sndbuf` blocking. UDP/RAW get bounded datagram
queues. Blocking is done with Exec signals under the core lock, so there are no lost
wakeups; break signals (Ctrl-C by default) surface as `EINTR`.

See [notes.md](notes.md) for the full LVO scoreboard (what's stubbed and why).

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

## Test tools

- **`socktest`** (`C/`) — app-side exercise of the socket layer via the library's LVOs
  (own inline glue, no netinclude dependency): `inet_addr`/`gethostbyname` → TCP
  connect/send/recv, default an HTTP/1.0 GET. Copy `Testing/bsdsocket.library` to
  `LIBS:` first, then `socktest <host> 80` — this drives DHCP + DNS + TCP through the
  whole zero-copy `netdev` path with hardware checksums active.

## Known limitations

- **`bsdsocket.library` is not HW-validated yet** — staged in `install/Testing/`, not
  installed to `LIBS:`. The `netdev` genet datapath is validated on hardware.
- **Router-side first-packet drop (under investigation)** — the first inbound packet of
  a fresh through-NAT flow after a few seconds of TX-idle can be lost *upstream* of the
  NIC (MIB counters show it never reaches the MAC); local-LAN flows are unaffected.
  Suspected router hardware-NAT / flow-offload. Mitigated with a 500 ms TCP RTO in
  `lwipopts.h` (SYN retransmit beats apps' 1 s connect timeouts). See
  [notes.md](notes.md).
- **TCP loss handling** — lwIP emits SACKs but does not act on received ones, so ~2%
  loss can trigger RTO collapse. Fine for a clean wired LAN.
- Stubbed LVOs include `getaddrinfo`, `GetSocketEvents`, the Roadshow
  interface-config/route/monitor families, `mbuf_*`, `bpf_*`, and (intentionally) the
  private `ipf_*` filter. No reverse DNS yet (`gethostbyaddr` returns the dotted quad).

## License

`BSD-3-Clause` throughout — own code, the `include/` `netdev` ABI headers, and the
bundled `lwip/` submodule (© the lwIP developers, used unmodified) all match. Any
driver or stack, under any license, may implement the `netdev` ABI contract. See
[LICENSE.md](LICENSE.md), including a note on the `emu68-common` build dependency
used by the Amiga binaries.
