# Release notes — lwip-amiga 1.1

Changes since v1.0.

---

## Breaking changes

These only matter if you're writing a driver or another stack against the `netdev`
interface — they have no effect on end users.

### The `netdev` header moved

It now lives at `include/devices/netdev.h`, alongside the other Amiga device headers,
instead of `include/netdev.h`.

### A few driver-only statistics moved out of the standard counters

A handful of low-level counters (RX buffer shortages, rejected sends, and similar) moved
out of the standard statistics block and into the new `GET_COUNTERS` command described
below, where each driver can report whatever counters fit its hardware. The standard
block stays small and the same for every driver. `netdev-stats` already uses the new
command, so nothing changes for anyone using the tools.

---

## New features

### Reachable by name: mDNS and DNS-SD

The stack answers for `<HOSTNAME>.local`, so any Mac, PC, or phone on the network can
reach the Amiga by name with no DNS server and no static address — `ping amiga.local`
just works. Services are advertised too (DNS-SD/Bonjour): list them in `netstack.prefs`
as `MDNS_SERVICE = _ftp._tcp 21`, or register them as they start with
`mdns ADD _ftp._tcp PORT 21`, which is usually what you want since Amiga daemons
launch after the stack. Set `MDNS = no` to turn it all off.

### More detailed driver statistics

`netdev-stats COUNTERS` now shows a much fuller picture of what the network hardware is
doing — for `genet.device`, that's every statistic the chip itself keeps, the same set
you'd see with `ethtool -S` on Linux, alongside the driver's own counters.

---

## Bug fixes / Improvements

### Fixed a duplicated drop count

A dropped-packet count reported to interface-monitoring tools (`IFQ_OutputDrops`) was
counting some drops twice. It now reports the correct number.

---

## Tools

`mdns` ships as a new tool: multicast DNS client and DNS-SD service control (see the
[README](README.md)). `netdev-stats` gains the `COUNTERS` view above; both tools take
ReadArgs-style command lines (`?` prints the template).

---

# Release notes — lwip-amiga 1.0

First public release. A fast, modern TCP/IP stack for classic AmigaOS 3.2, released
together with the first driver that supports it: `genet.device` 4.x, for the Raspberry
Pi 4/CM4's onboard Ethernet under PiStorm/Emu68.

**This is not a SANA-II stack** — lwip-amiga uses a new driver interface, not the one
Roadshow, AmiTCP, and every other Amiga network driver speaks. See the
[README](README.md) for what that means and who this release is for.

Ships `bsdsocket.library`, version 4.100 — the standard AmiTCP/Roadshow-compatible
socket API, installed to `LIBS:`.

---

## Highlights

### Performance

Measured with `sockbench` on a local wired gigabit network (release build, single stream,
against `genet.device` 4.0):

| Test | Speed | % of line rate |
|---|---|---|
| Download (TCP) | **942 Mb/s** | 94% |
| Upload (TCP) | **681 Mb/s** | 68% |
| Download (UDP) | **944 Mb/s** | 94% |
| Upload (UDP, 64 KB chunks) | **961 Mb/s** | 96% |

Over a real internet connection (measured with AmiSpeedTest), the stack reached
**847 Mb/s down / 69 Mb/s up** — this network's full ISP line rate in both directions, so
the internet link, not the Amiga, was the limit.

These gains come from a handful of changes under the hood:

- **A zero-copy driver interface.** The new `netdev` interface hands data directly
  between the driver and the stack instead of copying it back and forth, and offloads
  checksum calculation to the network hardware where it can.
- **Batching incoming data.** Packets that arrive together on the same connection get
  merged into a single delivery to the application, instead of being handled one at a
  time — roughly 25x fewer trips through the stack.
- **A faster memory pool for packets.** Packet buffers are handed out and returned from
  a small set of fixed-size pools instead of a general-purpose allocator, and each
  buffer is sized to line up cleanly with the hardware's cache and DMA requirements.
- **Cached outgoing headers.** The network header for each destination is worked out
  once and reused, instead of being recalculated for every outgoing packet.
- **A small fix to lwIP itself**, speeding up repeated sends on the same
  connection.

Every change above was measured and verified on real hardware before being kept.

### Test results

Run against bsdsocktest, a conformance suite for `bsdsocket.library`, on real Raspberry
Pi 4/PiStorm hardware:

**138 of 142 tests pass outright. 4 are skipped. None fail.**

Three of the 4 skips cover advanced or rarely-used features:

- Out-of-band TCP data (`MSG_OOB`) — 2 tests
- Asynchronous socket notification (`FIOASYNC`)

The fourth is more a buffer-sizing quirk than a gap: the test forces a non-blocking
`send()` to return `EWOULDBLOCK` by writing 1 MB without draining it, but lwip-amiga's
TCP send buffer is deliberately sized to exactly 1 MiB for throughput on fast links, so
the test's fixed 1 MB probe runs out just short of the wall it's trying to hit. The
backpressure path itself (`tcp_sndbuf()` accounting) is real and correct; this test just
wasn't sized to reach it.

(A fifth test, `ReleaseCopyOfSocket`, is implemented and counted above as passing — the
raw suite log can show it skipped on a second run of the suite without a reboot, a
test-harness quirk rather than a library limitation.)

None of these affect typical networking software.

### A new driver interface: `netdev`

lwip-amiga does not use SANA-II, the driver interface every other Amiga network stack
speaks. Instead it defines a new interface, `netdev` (`include/netdev.h`), built from
the ground up for zero-copy transfers and hardware offload — the driver owns and hands
off its own buffers instead of copying data back and forth, and negotiates capabilities
like checksum offload and interrupt coalescing at startup. It's an open, freely reusable
interface — any driver or stack can implement it. Today, `genet.device` 4.x is the only
driver that does.

### VLAN support

lwip-amiga can tag and filter a single VLAN (`VLAN = vid[,pcp]` in `netstack.prefs`),
with hardware checksum offload still active on tagged traffic.

### The TCP/IP core

Built on [lwIP](https://savannah.nongnu.org/projects/lwip/) 2.2.1, a mature,
widely-used open-source TCP/IP stack, with an AmigaOS-specific layer on top. IPv4, TCP,
UDP, ICMP, DHCP, and DNS are all supported.

### `bsdsocket.library`

The standard Amiga networking API programs already use — 76 of 121 entry points
implemented, covering everyday socket use (connect, send, receive, select, DNS lookups
both forward and reverse, `getaddrinfo`, and more). See the [README](README.md) for
what's not yet implemented and why.

### Tools

`netinfo` and `netdev-stats` ship with this release — see the [README](README.md) for
what each does.

---

## Known limitations

- Recovery from several dropped packets in a row is slower than it could be (falls back
  to a full timeout instead of a fast selective resend) — not an issue on a normal wired
  LAN.
- TCP upload runs at about 68% of line rate; the driver's packet-submission path is the
  current bottleneck, not the stack.
- A handful of advanced/legacy `bsdsocket.library` calls aren't implemented: Roadshow's
  interface-configuration/routing/monitoring calls, `mbuf_*`/`bpf_*`, and (by design) the
  private `ipf_*` packet filter.
