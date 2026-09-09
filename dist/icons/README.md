# dist/icons — Workbench icon assets

Source art and generator for the `.info` icons shipped with the tools. The
produced `.info` files are committed as **static binary assets** under `dist/`,
so building the stack needs no Python, pypng or icontool — the files here only
matter when *regenerating* them.

| Output (committed) | From | Kind |
|---|---|---|
| `dist/NetLogViewer.info` | `NetLogViewer.png` + `NetLogViewer.info.src` | tool (stack 16384, `DONOTWAIT`, `CX_POPKEY` / `CX_PRIORITY` / `CX_POPUP` tooltypes), ColorIcon + classic fallback |

`make_icons.py` drives the icontool fork, one invocation per icon — `--create`
synthesises the DiskObject, the imports supply the art, and the tooltype options
are applied in the order given. Descriptor keys are `TYPE`, `STACK`, `DEFAULTTOOL`,
`TOOLTYPES` (boolean) and a repeatable `TOOLTYPE = KEY=VALUE` for value tooltypes.

## Regenerating

Host-side only. Needs python3 with **pypng** (a venv is fine) and the
[icontool fork](https://github.com/rondoval/icontool) (branch `set-defaulttool`,
which adds `--create`, the ColorIcon writer, DefaultTool set/clear and repeatable
tooltype options):

```sh
python3 -m venv .venv && .venv/bin/pip install pypng
ICONTOOL=/path/to/icontool/icontool .venv/bin/python make_icons.py   # all icons
ICONTOOL=/path/to/icontool/icontool .venv/bin/python make_icons.py NetLogViewer
```

## Licensing

`NetLogViewer.png` is original art for this project (BSD-3-Clause, like the rest of
the repository). Nothing here is taken from AmigaOS or third-party icon sets.
