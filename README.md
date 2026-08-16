# lwip-amiga

> **Releases:** this component is developed and built as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack). That repository
> publishes the downloadable `.lha` and bundled documentation; this one is source-only
> and versioned via git tags.

A fast, modern TCP/IP stack for classic AmigaOS 3.2.

> **Two driver interfaces: `netdev` for speed, SANA-II for everything else.**
> lwip-amiga is built on a new, purpose-built driver interface called `netdev` —
> zero-copy, batched, checksum-offloading — and that is where the headline numbers
> come from. Today the only netdev driver is
> [`genet.device`](https://github.com/rondoval/emu68-driver-stack), **version 4.x or
> later** — the onboard Ethernet driver for a Raspberry Pi 4 or CM4 running under
> PiStorm/Emu68. Everything else — Poseidon USB Ethernet adapters, network cards, and
> other Ethernet drivers written for AmigaOS — speaks classic SANA-II, and those work
> too: the stack detects the driver type when an interface is added and drives SANA-II
> hardware through a compatibility backend (Ethernet-type SANA-II only — no Token Ring,
> ArcNet, or serial-line drivers). SANA-II is copy-based and offload-blind by design, so
> expect a fraction of netdev throughput.

> **Who this is for.** lwip-amiga is built for classic Amigas with an accelerator,
> plenty of RAM, and a fast network connection — machines that can actually put a
> gigabit link to use. It's not aimed at a stock, unaccelerated Amiga on an old network
> card; for that, Roadshow or AmiTCP remain the right choice.

## Why use it

- **Fast.** On a local gigabit network, lwip-amiga has measured up to 945 Mb/s
  downloading and 906 Mb/s uploading over TCP, and up to 957 Mb/s over UDP — several
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
- **Well tested.** Validated against the bsdsocktest conformance suite: all 142 tests
  pass — including the TCP out-of-band data and asynchronous-notification corners. See [Test results](#test-results) below.

## Requirements

- AmigaOS 3.2.
- [`genet.device`](https://github.com/rondoval/emu68-driver-stack) version 4.x or
  later — currently the only supported network driver, for the onboard Ethernet on a
  Raspberry Pi 4/CM4 running PiStorm or Emu68.
- An accelerated CPU and plenty of RAM are strongly recommended to make full use of the
  available network speed.

## Installing / configuring

**Per-interface files in `DEVS:NetInterfaces/`** — one file per network interface; the
*file name is the interface name*. Opening `bsdsocket.library` only starts the stack
with the loopback interface; real interfaces are added by the **`AddNetInterface`**
command, normally from `S:Network-Startup` at boot:

    AddNetInterface DEVS:NetInterfaces/~(#?.info) QUIET

The installer sets this up with a DHCP interface file named `genet` (a commented sample
also ships in `SYS:Storage/NetInterfaces/`). One option per line; `#`/`;` start
comments; an unknown option is an error:

| Option | Default | Meaning |
|---|---|---|
| `DEVICE` | *(required)* | which network driver to open (path form loads from `DEVS:`) |
| `UNIT` | `0` | which unit/port on that driver |
| `TYPE` | `AUTO` | driver interface: `AUTO` (probe the device), `NETDEV` or `SANA2` |
| `ADDRESS` | `DHCP` | `DHCP`, or a fixed dotted-quad address |
| `NETMASK` | — | subnet mask (required with a fixed `ADDRESS`) |
| `GATEWAY` | — | your router's address (fixed address only, optional) |
| `MTU` | driver's | lower the packet size limit (may only shrink it) |
| `VLAN` | — | in-band 802.1Q tag: `vid[,pcp]` (vid 1..4094, pcp 0..7) |
| `ID` | `HOSTNAME` | DHCP client hostname for this interface |

**Stack-wide settings in `ENV:netstack.prefs`**, read once when the stack starts. Keep
the master copy in `ENVARC:`, alongside a commented example,
`ENVARC:netstack.prefs.default`. Every setting is optional:

| Key | Default | Meaning |
|---|---|---|
| `HOSTNAME` | `amiga` | the name your Amiga reports to the network |
| `DOMAIN` | — | resolver search domain: dot-less names are retried as `name.DOMAIN` |
| `DNS1`, `DNS2` | — | explicit DNS servers; they override whatever a DHCP lease supplies |
| `MDNS` | `yes` | answer for `HOSTNAME.local` on the local network (Bonjour/Avahi), so other machines can reach the Amiga by name with no DNS server |
| `MDNS_SERVICE` | — | advertise a service over DNS-SD: `_type._proto port [instance name]` (e.g. `_ftp._tcp 21`); repeatable up to 4 times, and services can also be registered while running with the `mdns` command |
| `NETWORK` | — | adds an entry to the networks database (`getnetbyname`/`getnetbyaddr`); repeatable up to 8 times, `/etc/networks` notation — `name classful-network` (e.g. `homelan 192.168.0`) |

## Test results

lwip-amiga has been run against bsdsocktest, a conformance test suite for
`bsdsocket.library` implementations, on real Raspberry Pi 4/PiStorm hardware.

**All 142 tests pass. Nothing is skipped, and none fail.**


## Performance

Measured with `sockbench` on the release build, over a local wired gigabit network — not
the internet, so your real-world speed also depends on your Amiga, your network card, and
what's on the other end of the connection.

| Test | Speed |
|---|---|
| Download (TCP) | 945 Mb/s |
| Upload (TCP) | 906 Mb/s |
| Download (UDP, 64KB chunks) | 957 Mb/s |
| Upload (UDP, 64 KB chunks) | 957 Mb/s |

Over a real internet connection (measured with AmiSpeedTest), lwip-amiga reached
854 Mb/s down and 84 Mb/s up — matching 841/82 Mb/s from a PC on the same line via
speedtest.net.

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

- **`AddNetInterface`** — adds network interfaces from `DEVS:NetInterfaces/` files
  (name, path, or wildcard; Roadshow-compatible template `INTERFACE/M,QUIET/S,
  TIMEOUT/K/N`). The add blocks until the interface is operational — link up for a
  static config, DHCP lease bound for a dynamic one (default timeout 30 s); on
  timeout the interface stays up and keeps trying in the background (exit code 5).
  Also works from Workbench as the Default Tool of an interface file (`QUIET`/`TIMEOUT`/
  `PRI` icon tooltypes).
- **`RemoveNetInterface`** — takes an interface down again (`INTERFACE/A,FORCE/S,
  QUIET/S`). Refuses while sockets are still bound to the interface's address unless
  `FORCE` is given.
- **`NetShutdown`** — stops the whole stack (`TIMEOUT/N,QUIET/S`, default 5 s): asks
  every network program to let go, waits for the last one, then removes
  `bsdsocket.library` from memory. While programs hold out, the shutdown waits; on
  timeout or Ctrl-C it is recalled and the network keeps running. Opening
  `bsdsocket.library` afterwards starts a fresh stack. Note that `LibOpen` returns
  failure while a shutdown is pending, so programs cannot sneak in mid-teardown.
- **`arp`** — displays, sets and deletes ARP table entries, ported from 4.3BSD arp(8)
  (template `-a=ALL/S,-d=DELETE/S,-s=SET/S,HOSTNAME,ADDRESS,TEMP/S,-f=FILE/K,
  -n=NONAMES/S=NUMBERS/S`). `Arp ALL` lists the table (`NONAMES` skips the reverse-DNS
  lookups, useful without a reachable resolver), `Arp SET <host> <mac>` pins an entry
  (permanent unless `TEMP`), `Arp DELETE <host>` removes one, `FILE` loads a batch in
  the Roadshow/BSD `hostname ether_addr [temp]` format. Entries live in the running
  stack and are dropped with their interface. Roadshow's `PUBLISH`/`PROXY` (answering
  ARP for other hosts) is not supported by this stack: the switches are omitted from
  the template and a `pub` token in a batch file is rejected. Third-party software can
  drive the same machinery through the classic `SIOCSARP`/`SIOCGARP`/`SIOCDARP` (plus
  whole-table `SIOCGARPT`) `IoctlSocket()` requests — see `include/net/if_arp_ioctl.h`.
- **`ping`** — the classic 4.4BSD ping with the Roadshow template (`-c=COUNT/K/N,
  -d=DEBUG/S,-i=INTERVAL/K/N,-l=LOAD/K/N,-n=NUMERICONLY/S=NUMERIC/S,-o=ONEREPLY/S,
  -q=QUIET/S,-R=RECORDROUTE/S,DONTROUTE/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,-v=VERBOSE/S,
  BELL/S,HOST/A`): ICMP echo with per-reply round-trip times and a
  min/avg/max/loss summary on Ctrl-C or `COUNT` completion. `RECORDROUTE` is
  refused (lwIP cannot send IP options); `DEBUG` and `DONTROUTE` are accepted but
  inert.
- **`traceroute`** — Van Jacobson's traceroute with the Roadshow template
  (`-d=DEBUG/S,-m=MAXTTL/K/N,-n=NUMERIC/S,-p=PORT/K/N,-q=QUERIES/K/N,-r=DONTROUTE/S,
  -s=SOURCE/K,-t=TOS/K/N,-v=VERBOSE/S,-w=WAIT/K/N,HOST/A,PACKETSIZE/N`): maps the
  gateways toward a host with TTL-stepped UDP probes over the raw-socket
  `IP_HDRINCL` path, `*` for hops that stay quiet and `!H`/`!N`/`!P` annotations
  for unreachables.
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

`netinfo` and `netdev-stats` are read-only status tools; the stack is configured
through the interface files and `netstack.prefs` above, plus the
`AddNetInterface`/`RemoveNetInterface`/`NetShutdown`/`Arp` commands at runtime.

Scripts can test the outcome Roadshow-style: with `QUIET`, the commands demote every
failure to exit code 5 (`IF WARN` in a script), and `AddNetInterface` returns 5 when
the interface is up but the DHCP lease has not arrived yet.

## Known limitations

- **Lossy connections recover slowly.** If a connection drops several packets in a row
  (for example, over a flaky link or a long-distance internet path), lwip-amiga's TCP
  falls back to a slow, full timeout before resending, rather than a fast selective
  resend. This isn't an issue on a clean connection, such as a normal wired LAN.
- **Both directions run near line rate.** TCP upload and download are both close to the
  wire on gigabit.
- **A handful of advanced or legacy `bsdsocket.library` calls aren't implemented**:
  Roadshow's interface-configuration, routing, and monitoring calls (the read-only
  interface *query* calls used by `netinfo` above do work — interface add/remove is
  done with the bundled `AddNetInterface`/`RemoveNetInterface` commands instead, so
  genuine Roadshow configuration binaries won't), the low-level `mbuf_*`/`bpf_*`
  families, and (by design) the private `ipf_*` packet filter.

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
- **A SANA-II compatibility backend** (`port/amiga/sana2_*.c`) drives classic drivers
  through the same lwIP glue — cooked-mode translation and a client-side RX pump task —
  with the driver type resolved per interface (`TYPE=AUTO|NETDEV|SANA2`); netdev remains
  the performance path.
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
