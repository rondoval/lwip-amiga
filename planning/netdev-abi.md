# netdev ABI — design rationale (draft 1)

*Companion to [include/netdev.h](../include/netdev.h), which is the normative
contract. This records why the contract looks the way it does. 2026-07-10.*

## Shape

Fleet idiom, deliberately — the xHCI context ABI (`usbhcd_context.h`) proved the
pattern on this platform: an NSCMD command block with IOStdReq framing for
control/lifecycle ops (serialized by the driver's unit task), plus a one-time
ATTACH that exchanges explicit contexts and direct-call tables for the data path.
ROM-able drivers carry no writable data, so the context argument is their only
anchor; the stack-side context makes the stack re-entrant per netif for free.

Ops live in **tables** (not individual attach fields like xhci's three entries)
because the surface is wider and will grow — new entries append, gated by the
negotiated `nda_AbiVersion`.

## Mapping to GENET hardware (the first driver)

| ABI element | GENET reality |
|---|---|
| `NDTF_L4CSUM` + `ntd_CsumStart/Offset` | TSB (transmit status block) offset-based checksum engine, `DMA_TX_DO_CSUM` — present in `bcmgenet-regs.h`, unused by the SANA-II driver today |
| `NDRF_CSUM_RAW` + `nrd_CsumRaw` | RXCHK raw checksum in the 64-byte RSB (`RBUF_64B_EN` — the current driver has the enable commented out: *"we don't use Receive Status Block"*) |
| RX headroom for the RSB | driver-internal, never crosses the ABI (driver-owned RX buffers) |
| `ndo_TxSubmit` SG segments | one GENET descriptor per segment; `ndc_TxMaxSegs` bounds a packet's slots |
| `ndo_DmaAlloc` | emu68-common `dma_mem` pool (Emu68 expansion RAM only) |
| `NETDEV_CMD_SET_COALESCE` | DMA ring timeout + MBUF_DONE thresholds |
| `NDCF_MCAST_FILTER` | MDF exact-match slots; overflow → all-multi fallback (per the ABI's rule) |

The offset-based TX checksum (start/insert offsets, not protocol enums) is chosen
because it is what the hardware actually implements, it is protocol-agnostic
(TCP/UDP over v4/v6 alike), and the stack-side netif glue knows both offsets at
zero cost when it builds the frame. IPv4 header checksum stays in software —
20 bytes, computed inline by lwIP, not worth an ABI feature (same call Linux made
for this hardware).

Both RX checksum modes exist because hardware differs: `CSUM_VALID` for engines
that verify, `CSUM_RAW` for engines that just sum (GENET). The port-layer glue
folds raw sums against the pseudo-header and feeds lwIP a per-netif
"already checked" setting either way.

## Semantics choices

- **Prefix-consume on both batched paths** (`ndo_TxSubmit` returns accepted
  count, `nso_RxInput` returns consumed count): one integer encodes partial
  success without per-descriptor status; the tail is unambiguous. TX retry is
  event-driven — `nso_TxDone` is the "ring has space" signal, no polling.
- **RX backpressure is explicit and cheap**: refused descriptors never leave the
  driver, recycle immediately, and are counted (`nds_RxDropped`). The stack
  refuses only when truly saturated (pbuf pool empty).
- **Set-state RX filter, not add/del deltas**: idempotent, no state drift between
  stack and driver, trivially replayed after a driver reset, and it makes the
  all-multi overflow fallback a driver-internal detail.
- **STOP completes every in-flight TX cookie before replying** so the stack's
  memory accounting is exact at quiesce; DETACH then requires all RX cookies
  released and all `ndo_DmaAlloc` memory freed — after DETACH, no pointer of
  either side survives in the other.
- **Cache maintenance is driver-side only.** The driver knows DMA timing and
  the platform contract (68040 line ownership — `dma_mem`'s whole-cache-line
  allocation rule). The stack treating packet memory as plain memory keeps the
  port layer platform-agnostic.
- **`NETDEV_CMD_GET_LINK` duplicates `nso_LinkChange`** deliberately: events for
  the stack's route/interface state, polling for tools (netstat-style) that
  attach nothing.

## Findings from the host TCP spike (harness/tcpbench, 2026-07-10)

- Window scaling negotiates and carries a 256 KB window (RFC 7323 works).
- Sparse loss (0.5%) is absorbed by fast retransmit with negligible cost.
- **2% loss collapses throughput** (32 MB took 69 s): lwIP emits SACKs
  (`LWIP_TCP_SACK_OUT`) but does **not use received SACKs for
  retransmission**, so multi-loss windows recover by RTO. Acceptable for the
  clean wired LAN this stack targets (loss ≈ 0, and pre-SACK Amiga stacks
  share the ceiling), but it is a documented limit — bulk transfers over
  lossy/congested paths will underperform. Revisit only if real-world use
  hits it (upstream lwIP work, not ABI work).

## Open questions (to resolve before ABI freeze)

- **RX pool sizing guidance**: the stack may hold up to TCP_WND (256 KB ≈ 128
  buffers) per socket in receive queues; the driver's RX pool must be sized
  ring + expected holding (genet: ring 256 → pool 512+ buffers ≈ 1 MB of
  Emu68 RAM is cheap). Driver-internal per the ABI, but worth a sizing note
  in the header before freeze.
- **RAW-mode RX csum folding** is deferred in the glue v1 (frames with only
  `NDRF_CSUM_RAW` get software-checked; only `NDCF_RX_CSUM_VALID` disables
  lwIP's check). Glue-local optimization, no ABI change — do it when genet
  lands and can be measured.

- **VLAN**: no support in draft 1. If wanted later: a TX flag + tag field in
  the reserved descriptor space, an RX flag + stripped-tag field.
- **Jumbo frames**: `ndc_Mtu` carries the capability; GENET can do 9k but the
  RX buffer geometry is driver-internal, so enabling jumbo later is a driver
  matter plus an MTU-negotiation op (draft 1 pins 1500).
- **Multi-queue**: descriptor and caps layouts leave room (reserved fields);
  a `queue` UWORD in the TX descriptor + per-queue coalesce would be an
  appending change. Not before profiling shows the single unit task saturated.
- **SET_MAC while STARTed**: currently allowed (driver applies immediately);
  revisit if a driver needs quiesce for it.
- **Header portability**: `netdev.h` uses NDK types (`ULONG`/`APTR`), fine for
  both Amiga sides; the Linux host harness will need a small typedef shim when
  a mock netdev driver appears there. Decide when the harness grows one.
- **Command numbering**: `0x4900` assumes no collision in the NSCMD space used
  on this platform (xhci holds `0x4800..0x481f`). Confirm before freeze.

## What genet.device needs to grow (Phase 1 implementation list)

1. `NETDEV_CMD_*` dispatch in `device_beginio.c` (NSD query listing both
   personalities; ATTACH exclusivity vs SANA-II openers).
2. TSB path: `TBUF_64B_EN`, per-packet TSB build from `ntd_Csum*`, +64 bytes
   staging-free descriptor chain from SG segments (no more `CopyFromBuff`).
3. RSB path: `RBUF_64B_EN`, parse RSB for length/csum, hand frames up with
   `NDRF_CSUM_RAW` and data pointer past the 64-byte block.
4. RX buffer pool with cookies + SPSC recycle ring fed by `ndo_RxRelease`
   (foreign-task-safe), refill batched in the unit task.
5. `nso_TxDone` batching in `bcmgenet_tx_reclaim`.
6. Coalesce plumbing (ring timeouts) behind `NDCF_COALESCE`.
7. All behind a build flag until the stack can drive it.
