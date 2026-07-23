# lwip-amiga

> **Releases:** this component is developed and built as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack). That repository
> publishes the downloadable `.lha` and bundled documentation; this one is source-only
> and versioned via git tags.

A fast, modern TCP/IP stack for classic AmigaOS 3.2.

> **This is not a SANA-II stack.** Almost every Amiga network stack and driver —
> Roadshow, AmiTCP, Miami, and virtually every network card driver ever written for
> AmigaOS — speaks SANA-II. lwip-amiga does not. It's built on a new, purpose-built
> driver interface called `netdev`, designed for speed rather than backward
> compatibility. Today the only driver that supports it is
> [`genet.device`](https://github.com/rondoval/emu68-driver-stack), **version 4.x or
> later** — the onboard Ethernet driver for a Raspberry Pi 4 or CM4 running under
> PiStorm/Emu68. If your network card only has a SANA-II driver, lwip-amiga will not
> work with it.

> **Who this is for.** lwip-amiga is built for classic Amigas with an accelerator,
> plenty of RAM, and a fast network connection — machines that can actually put a
> gigabit link to use. It's not aimed at a stock, unaccelerated Amiga on an old network
> card; for that, Roadshow or AmiTCP remain the right choice.

## Why use it

- **Fast.** On a local gigabit network, lwip-amiga has measured up to 942 Mb/s
  downloading and 681 Mb/s uploading over TCP, and up to 961 Mb/s over UDP — several
  times what older Amiga TCP/IP stacks manage on the same hardware. See
  [Performance](#performance) below for the full numbers and what they mean.
- **Modern protocol support.** IPv4, TCP, UDP, ICMP, DHCP, DNS (forward and reverse),
  and 802.1Q VLAN tagging, all handled by [lwIP](https://savannah.nongnu.org/projects/lwip/),
  a mature, actively-maintained open-source TCP/IP stack used across embedded devices
  worldwide.
- **A standard socket API.** Programs talk to lwip-amiga through `bsdsocket.library`,
  the same API used by Roadshow and AmiTCP. Most existing networking software should
  just work, unless it depends on one of the handful of calls not yet implemented (see
  [Known limitations](#known-limitations)).
- **Well tested.** Validated against the bsdsocktest conformance suite: 138 of 142 tests
  pass, and the rest are skipped for advanced features that ordinary software never
  touches. See [Test results](#test-results) below.

## Requirements

- AmigaOS 3.2.
- [`genet.device`](https://github.com/rondoval/emu68-driver-stack) version 4.x or
  later — currently the only supported network driver, for the onboard Ethernet on a
  Raspberry Pi 4/CM4 running PiStorm or Emu68.
- An accelerated CPU and plenty of RAM are strongly recommended to make full use of the
  available network speed.

## Installing / configuring

lwip-amiga reads its settings from **`ENV:netstack.prefs`** once, the first time a
program opens `bsdsocket.library`. Keep the master copy in `ENVARC:`, alongside a
commented example, `ENVARC:netstack.prefs.default`. Every setting is optional — with no
file at all, lwip-amiga runs DHCP on `networks/genet.device` unit 0.

| Key | Default | Meaning |
|---|---|---|
| `DEVICE` | `networks/genet.device` | which network driver to open (path form loads from `DEVS:`) |
| `UNIT` | `0` | which unit/port on that driver |
| `MODE` | `DHCP` | `DHCP` (automatic) or `STATIC` (fixed address) |
| `ADDRESS`, `NETMASK` | — | your fixed IP address and subnet mask (`STATIC`; else the stack falls back to DHCP) |
| `GATEWAY` | — | your router's address (`STATIC`, optional) |
| `DNS1`, `DNS2` | — | DNS servers to use (`STATIC`; DHCP supplies its own automatically) |
| `HOSTNAME` | `amiga` | the name your Amiga reports to the network |
| `MDNS` | `yes` | answer for `HOSTNAME.local` on the local network (Bonjour/Avahi), so other machines can reach the Amiga by name with no DNS server |
| `MDNS_SERVICE` | — | advertise a service over DNS-SD: `_type._proto port [instance name]` (e.g. `_ftp._tcp 21`); repeatable up to 4 times, and services can also be registered while running with the `mdns` command |
| `NETWORK` | — | adds an entry to the networks database (`getnetbyname`/`getnetbyaddr`); repeatable up to 8 times, `/etc/networks` notation — `name classful-network` (e.g. `homelan 192.168.0`) |

## Test results

lwip-amiga has been run against bsdsocktest, a conformance test suite for
`bsdsocket.library` implementations, on real Raspberry Pi 4/PiStorm hardware.

**138 of 142 tests pass. 4 are skipped, and none fail.**

Three of the 4 skips cover advanced features that ordinary networking software (web
browsers, FTP/mail clients, terminal programs, file transfer tools) doesn't use:

- Sending "out-of-band" urgent TCP data (`MSG_OOB`) — 2 tests
- Asynchronous socket notifications (`FIOASYNC`)

The fourth is more a compliment than a gap: the test tries to force a non-blocking
`send()` to return `EWOULDBLOCK` by writing 1 MB without ever reading it back, but
lwip-amiga's TCP send buffer is deliberately sized to exactly 1 MiB (tuned for
throughput on fast links), so the test's fixed 1 MB probe runs out just short of the
wall it's trying to hit. The buffer-full/`EWOULDBLOCK` code path is real and
byte-accurate — this test just wasn't big enough to reach it.

(A fifth raw skip, `ReleaseCopyOfSocket`, is implemented and counted as passing above —
the raw suite log can show it as skipped if the suite is re-run a second time without
rebooting, a quirk in the test harness's socket-sharing state rather than a gap in the
library.)

## Performance

Measured with `sockbench` on the release build, over a local wired gigabit network — not
the internet, so your real-world speed also depends on your Amiga, your network card, and
what's on the other end of the connection.

| Test | Speed | % of line rate |
|---|---|---|
| Download (TCP) | 942 Mb/s | 94% |
| Upload (TCP) | 681 Mb/s | 68% |
| Download (UDP) | 944 Mb/s | 94% |
| Upload (UDP, 64 KB chunks) | 961 Mb/s | 96% |

Over a real internet connection (measured with AmiSpeedTest), lwip-amiga reached
847 Mb/s down and 69 Mb/s up — this network's full ISP line rate in both directions. On
that link the stack saturated the connection; the ISP, not the Amiga, set the ceiling.

That comes from a few optimizations under the hood:

- **A zero-copy driver interface.** The new `netdev` interface hands data directly
  between the driver and the stack instead of copying it back and forth, and offloads
  checksum calculation to the network hardware where it can.
- **Batched incoming packets.** Packets that arrive together on the same connection get
  merged into a single delivery to the stack, instead of being handled one at a time.
- **A reused memory pool.** Packet buffers come from a small set of fixed-size pools
  instead of being constantly allocated and freed.
- **Cached outgoing headers.** The network header for each destination is worked out
  once and reused, instead of being rebuilt for every outgoing packet.

See [RELEASE-NOTES.md](RELEASE-NOTES.md) for more on what's behind these numbers.

## Tools

- **`netinfo`** — shows your current network status at a glance: address, netmask,
  broadcast, MTU, MAC address, link state, DHCP/static, and DNS servers.
- **`netdev-stats`** — shows live driver statistics (packet/error counters, link state)
  and lets you tune interrupt coalescing, without restarting the stack. `netdev-stats
  COUNTERS` switches to the driver's own counter list — for `genet.device` that is the
  complete UniMAC hardware MIB, the same set Linux exposes through `ethtool -S`.

- **`mdns`** — multicast DNS. `mdns pi.local` resolves a name on the local network with
  no DNS server involved, `mdns LISTEN` watches what the network announces, and `mdns
  STATUS` shows what this Amiga advertises. Services can be advertised as they start —
  `mdns ADD _ftp._tcp PORT 21` — and withdrawn again with `mdns DEL <slot>`; anything
  listed under `MDNS_SERVICE` in `netstack.prefs` is advertised from boot.

`netinfo` and `netdev-stats` are read-only status tools; apart from `mdns`'s service
list, the stack is configured entirely through `netstack.prefs`, above.

## Known limitations

- **Lossy connections recover slowly.** If a connection drops several packets in a row
  (for example, over a flaky link or a long-distance internet path), lwip-amiga's TCP
  falls back to a slow, full timeout before resending, rather than a fast selective
  resend. This isn't an issue on a clean connection, such as a normal wired LAN.
- **Upload has more headroom than download.** TCP upload currently runs at about 68% of
  line rate; the bottleneck is the driver's packet-submission path rather than the stack
  itself. This will be optimized in future releases.
- **A handful of advanced or legacy `bsdsocket.library` calls aren't implemented**:
  Roadshow's interface-configuration, routing, and monitoring calls (the read-only
  interface *query* calls used by `netinfo` above do work), the low-level
  `mbuf_*`/`bpf_*` families, and (by design) the private `ipf_*` packet filter.

## For developers

Everything below is for people building lwip-amiga from source, contributing to it, or
writing a driver against its `netdev` ABI. If you just want to use it, you can stop
here.

### What this is

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
- **A TCP/IP core** — lwIP (git submodule) plus an AmigaOS port layer, running in
  **core-locking direct-path** mode: application tasks execute stack code in their own
  context under a single core semaphore, with Exec signals as the blocking primitive.
  Built as the `netstack` static library.
- **`bsdsocket.library`** — the standard application socket API, compatible with the
  Roadshow-era contract documented in NDK 3.2 (`SANA+RoadshowTCP-IP/`), built directly
  on the lwIP raw API. Per-opener child library bases, an in-library stack task, and
  DHCP/DNS out of the box.

Scope is IPv4 + DHCP + DNS, with a fresh minimal lwIP config and DHCP by default.

See [docs/architecture.md](docs/architecture.md) for how the stack works.

### Source layout

- `include/netdev.h` — the `netdev` driver ABI header. Installed and exported as the
  `Netdev` CMake package (`Netdev::netdev_headers`), which is how `genet.device`
  consumes it.
- `lwip/` — lwIP core, git submodule (BSD-3-Clause, forked from `STABLE-2_2_1_RELEASE`).
- `port/amiga/` — AmigaOS port layer: `lwipopts.h` (the core-locking config),
  `netstack.c` (singleton, core lock, `EClock`→ms time), `netstack_mem.c` (DMA-aware
  heap + slab), `netdev_rx.c`/`netdev_tx.c` (lwIP netif ⇄ `netdev` datapaths: RX
  `pbuf_custom` recycle + GRO-lite, zero-copy TX scatter-gather + L4 checksum
  offsets), `netdev_if.c` (netif lifecycle, link events).
- `src/bsdsocket/` — `bsdsocket.library` (socket layer, LVO table, stack task).
- `src/sockbench/` — LAN TCP/UDP throughput benchmark over `bsdsocket.library`
  (developer tool; built but not shipped).
- `sfd/`, `scripts/gen-vectors.py` — the NDK `bsdsocket` SFD and the generator that
  emits the full 139-slot LVO vector table from it.
- `docs/` — architecture and TODO documents.

### `netdev` ABI details

**`netdev` ABI (v1)** — driver-owned RX buffers with a release hook + prefix-consume
batches, driver-provided DMA allocator for the TX pool, offset-based TX L4 checksum
(GENET TSB shape), dual RX checksum reporting (VALID / RAW), declarative RX filter,
STOP/DETACH quiesce protocol. genet's negotiated caps: interrupt coalescing, link
events, TX L4 checksum offload, RX checksum (RAW).

**`bsdsocket.library`** — 76 of 121 LVOs implemented, including:

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
- `getaddrinfo`/`freeaddrinfo`/`gai_strerror`/`getnameinfo` (RFC 2553 IPv4 subset) and
  the full AmiTCP V4 async event API (`GetSocketEvents`, `SO_EVENTMASK`).

TCP receive consumes driver RX pbufs with zero copies until the app buffer; TCP send
copies into the DMA-backed heap with `sndbuf` blocking. UDP/RAW get bounded datagram
queues. Blocking is done with Exec signals under the core lock, so there are no lost
wakeups; break signals (Ctrl-C by default) surface as `EINTR`.

### Building

#### Amiga (m68k) binaries

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
exported `Netdev` package) and shares the stack-wide debug backend / `EMU68_TIER`
options — `lwip-amiga` is a valid component name for `EMU68_PROFILE`,
`EMU68_DEBUG` and `EMU68_TRACE`.

#### `sockbench` (developer tool, built but not shipped)

LAN TCP/UDP throughput benchmark over the NDK BSD socket API (`bsdsocket.library`):
`sockbench rx|tx|udprx|udptx <host> [streams] [seconds] [sizeKB]` runs N nonblocking
sockets from one `WaitSelect` loop against `scripts/tcp-bench-peer.py` and reports
per-stream + aggregate Mb/s (`rx`/`tx` are TCP, `udprx`/`udptx` are UDP; `sizeKB` is the
TCP buffer or the UDP datagram size). Exercises DHCP + DNS + TCP/UDP through the whole
zero-copy `netdev` path with hardware checksums active.

## License

`BSD-3-Clause` throughout — own code, the `include/` `netdev` ABI headers, and the
bundled `lwip/` submodule (© the lwIP developers) all match. Any driver or stack, under
any license, may implement the `netdev` ABI contract. See [LICENSE](LICENSE), including
a note on the `emu68-common` build dependency used by the Amiga binaries.
