# TCP-sender fuzz harness

Host-side randomized model checker for lwIP's TCP send-path bookkeeping — the
tool that root-caused the 2026-07 "TX corruption under sustained heavy upload"
class: `tcp_write`/`tcp_send_fin` extending an unsent-queue tail that a
retransmission had requeued (its end no longer at `snd_lbb`), assigning new
stream bytes already-used sequence numbers. Fixed in this repo's lwIP fork.

It drives a single ESTABLISHED pcb through randomized schedules of
`tcp_write` / `tcp_output` / crafted ACKs / dupacks (fast rexmit) /
`tcp_slowtmr` (RTO, persist, zero-window probes), with per-run variations:
window scaling (peer scale 8), Nagle on/off, heap-allocation failure
injection, driver TX errors, and genet-style deferred-TX pbuf refs.

After **every** lwIP call it verifies:

1. `pcb->unsent_tail` matches the walked tail (fork patch self-check),
2. `unsent == NULL` ⇒ `unsent_oversize == 0`,
3. `unsent_oversize` equals the `TCP_OVERSIZE_DBGCHECK` per-seg shadow,
4. `tail->len + optlen + unsent_oversize <= mss_local` (tcp_write's assert),
5. `unsent_oversize` ≤ the tail pbuf's **real** spare allocation bytes
   (exact-size recording allocator; ASAN redzones back it up),

and every transmitted payload byte is checked against the deterministic
app-stream pattern — this wire check is what caught the root cause
(`tcp_write` extending a segment requeued by `tcp_rexmit`, assigning new
data already-used sequence numbers).

The TCP configuration mirrors `port/amiga/include/lwipopts.h` (MSS 1460,
1 MB windows, wnd-scale, SACK out). lwIP asserts stay fatal.

## Usage

```sh
./build.sh                          # host gcc, ASAN+UBSAN
./fuzz_oversize [seed] [num_seeds] [ops_per_seed]
./fuzz_oversize 1 20000 4000        # the standard sweep (~2 min)
```

On a violation it prints the failed invariant, the pcb state, and a ring
buffer of the last 128 operations, then aborts; runs are deterministic per
seed. Vanilla 2.2.1 reproduces the corruption in seconds (e.g. seed 68);
with the fork's `tcp_out.c`/`tcp_in.c` fixes the standard sweep passes.

## Licensing / provenance

All files here are original to this repository, BSD-3-Clause like the rest
of the component. The build additionally compiles lwIP's own unit-test
helper (`lwip/test/unit/tcp/tcp_helper.c`, lwIP BSD-3-Clause) straight from
the submodule — nothing is vendored. `shim/check.h` is a from-scratch stub
of the libcheck API used by that helper; no libcheck (LGPL) code is copied
or linked, and the harness is a host-only development tool that is never
part of release artifacts.
