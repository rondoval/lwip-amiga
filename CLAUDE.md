# lwip-amiga

AmigaOS TCP/IP stack: `bsdsocket.library` on the lwIP raw API (NO_SYS=1, Exec-semaphore
core locking), plus the netstack port layer (`port/amiga/`), the `netdev` driver ABI
(`include/netdev.h`), prefs, and CLI tools. See `docs/architecture.md` for the execution
model, locking, memory, and RX/TX paths.

## Building

Built as a component of the **emu68-driver-stack** superproject, which supplies the Bebbo
m68k cross-toolchain and `emu68-common`. From a superproject checkout, build through its
container wrapper — **never host `cmake`** (build trees configure at `/work` inside the
toolchain container, so host-side `cmake --build build` fails on a CMakeCache path
mismatch):

```sh
./scripts/docker-build.sh --target lwip-amiga
```

The library lands in `install/LIBS/bsdsocket.library`; tools in `install/C/`. Select the
debug backend with `EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=serial"` (default
`pistorm` | `serial` | `off`).

After editing a C file, build to confirm zero errors and zero warnings before reporting
the task done.

## Diagnostics: which printer to use

**Default to `Kprintf` / `KprintfT`** (emu68-common `<debug.h>`, tier-gated by
`EMU68_TIER`). They go through `RawDoFmt`, so follow the fleet conventions: `0x%08lx` for
pointers (there is **no `%p`**), and `%ld`/`%lu` with explicit `(LONG)`/`(ULONG)` casts —
`RawDoFmt` just casts everything to 32-bit, so bare `%d`/`%u`/`%x` produce malformed
output.

**Reach for `netstack_diag_printf`** (`port/amiga/include/netstack_diag.h`) *only* when the
format genuinely needs C semantics — `%p`, `%02x`, or 32-bit `%u`/`%d`/`%x`. It promotes
arguments correctly into a 256-byte buffer and hands the finished string to `Kprintf`; it
backs `LWIP_PLATFORM_DIAG` because lwIP's own format strings live in the submodule and
cannot be rewritten. `bsd_vsyslog` shares that formatter via `netstack_vformat_args`.
There is exactly one formatter in this component — do not add a second.

Below the debug tier `lwipopts.h` sets `LWIP_NOASSERT`, compiling out lwIP's ~490
`LWIP_ASSERT` sites (~19 KB off the library); `LWIP_ERROR` is unaffected and keeps
recovering gracefully in every build.

## Key documents

- `docs/architecture.md` — execution model, locking, memory, RX/TX paths
- `docs/bsdsocket-lvo-coverage.md` — per-LVO implementation status map
- `docs/TODO.md` — throughput plan and open work
- Conformance: the bsdsocktest suite (142 tests; run on real hardware, logs land in the
  stack root as `bsdsocktest.log` + serial capture)
