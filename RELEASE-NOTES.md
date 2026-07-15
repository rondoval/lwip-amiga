# Release notes — lwip-amiga 0.1

First release. A modern TCP/IP stack for AmigaOS 3.2, built on lwIP 2.2.1, delivering three
layers on top of a fresh network driver ABI. Validated on real PiStorm/CM4 hardware.

Ships `bsdsocket.library` (runtime version 4.1 — ABI major 4, the AmiTCP/Roadshow contract;
the revision tracks this release). The first driver for the new ABI, `genet.device`, is in
the emu68-driver-stack.

---

## Highlights

### `netdev` — a new NIC driver ABI (`include/netdev.h`)

A clean-break SANA-II replacement: direct-call, context-based, batched, and zero-copy in
both directions, with capability negotiation (checksum offload, interrupt coalescing, link
events). RX buffers are driver-owned and returned via a release hook; TX memory is drawn
from a driver-provided DMA allocator, so nothing is copied and every buffer is
DMA-reachable. The header is the open, permissively-licensed public contract.

### TCP/IP core — lwIP + AmigaOS port layer

lwIP 2.2.1 as an unmodified git submodule plus a hand-written Amiga port layer, running in
core-locking direct-path mode: application tasks execute stack code in their own context
under a single core semaphore, with Exec signals as the blocking primitive. Built as the
`netstack` static library. IPv4 + TCP + UDP + ICMP + DHCP + DNS.

### `bsdsocket.library`

The standard application socket API, built directly on the lwIP raw API — per-opener child
bases, an in-library stack task, DHCP/DNS out of the box. 72 of 121 LVOs implemented: the
socket core, `WaitSelect`, the `errno`/inet/resolver families, `getaddrinfo`,
`sendmsg`/`recvmsg`, `ObtainSocket`/`ReleaseSocket`, `SO_SNDTIMEO`/`SO_RCVTIMEO`, and the
full AmiTCP V4 async event API. Installs to `LIBS:`.

### Tools

`netdev-stats` (shipped) reads live loss-point counters and drives interrupt coalescing on
release builds.

---

## Known limitations

- Throughput is still being optimized (download below line rate).
- lwIP acts on emitted but not received SACKs, so multi-loss windows recover by RTO — fine
  for a clean wired LAN.
- Stubbed LVOs: the Roadshow interface-config/route/monitor families, `mbuf_*`, `bpf_*`,
  and (intentionally) the private `ipf_*` filter. No reverse DNS yet.

See [docs/TODO.md](docs/TODO.md) for the full picture.
