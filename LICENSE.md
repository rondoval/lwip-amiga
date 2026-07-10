# License

This repository is mixed-license. The file-level SPDX header is the authoritative
license for each source file.

- **Own code** (port layer, bsdsocket.library, harness, tools):
  `GPL-2.0-or-later` — see [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt)
- **`include/` — the netdev driver ABI headers**: `BSD-2-Clause` — see
  [LICENSES/BSD-2-Clause.txt](LICENSES/BSD-2-Clause.txt). The ABI is an open
  contract: any driver or stack, under any license, may implement it.
- **`lwip/` submodule**: BSD-3-Clause, © the lwIP developers (license text in the
  submodule; lwIP is used unmodified).
