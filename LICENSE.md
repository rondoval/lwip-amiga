# License

This repository's own code (port layer, `bsdsocket.library`, tools, and the
`include/` netdev driver ABI headers) is licensed `BSD-3-Clause` — see
[LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt). This matches the
license of the bundled lwIP core, so the whole stack is permissively
licensed end to end; any driver or stack, under any license, may implement
the `netdev` ABI.

- **`lwip/` submodule**: `BSD-3-Clause`, © the lwIP developers (license text in
  the submodule; lwIP is used unmodified).

## Third-party build dependency: emu68-common

Amiga (m68k) binaries built from this repo (`netstack`, `bsdsocket.library`,
test tools) statically link
[`emu68-common`](https://github.com/rondoval/emu68-common), a separate
project dual-licensed `MPL-2.0 OR GPL-2.0+`. Its source is public at that
repository; recipients of built binaries may rely on either license option
for the emu68-common portions, per its own `LICENSE`.
