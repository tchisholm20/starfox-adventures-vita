# Building

This tree contains **no game data**. The build needs your own disc image: some build checks read it, and
the finished `eboot.bin` loads the game's data from it at boot. Anything extracted from it at build time goes
into the build output folder only.

## Layout

```
engine/                 the game: the Star Fox Adventures decompilation (CC0) + the PC-port layer, with this
                        project's modifications (src/port/...). Built for ARM by the scripts in vita/host/engine.
vita/                   the PS Vita backend (MIT): src/ (GXM renderer, render thread, platform layer, eboot wrapper),
                        sce_sys/ (LiveArea assets), host/engine/ (the build scripts and the gates).
gxaot/release/c3/       two headers (c3_cuts.h / c3_cuts.inc) the backend includes.
third_party/vitagl/     how the LGPL libraries the eboot statically links are built (vitaGL + our patches,
                        vitaShaRK, math-neon, SceShaccCgExt). Corresponding-source note below.
```

## What you need

1. **VitaSDK** (https://vitasdk.org) installed at `/usr/local/vitasdk` inside an **MSYS2** shell (this is how the
   build was developed and tested: Windows 11, MSYS2, `arm-vita-eabi-gcc` 15.2).
   Python 3.8+ must be on `PATH`. A Linux/macOS bash with the same VitaSDK path should work but is untested.
2. **The libraries**, installed into the SDK. Run once:
   ```
   VITAGL_ROOT="$HOME/vitagl-src" bash third_party/vitagl/build-vitagl.sh     # VITAGL_ROOT = where it clones/builds (default: inside this tree)
   ```
   It clones SceShaccCgExt, vitaShaRK, math-neon and vitaGL (pinned commit) from GitHub, applies the three
   patches in that directory to vitaGL, and installs everything under `$VITASDK`. The `libshacccg.suprx`
   runtime module must be on the console (`ur0:/data/libshacccg.suprx`); see the vitaGL documentation.
3. **Your own disc image**: a 1:1 dump of *Star Fox Adventures*, USA, **Rev 1** (game id `GSAE01`, version 1),
   as a plain `.iso` (about 1.4 GB). Put it at `engine/orig/GSAE01/sfa_rev1.iso`, or point `STAIRFAX_ISO` at it.
   Nothing extracted from it is ever written into this tree; extracted data goes only into the build output.
4. About 3 GB of disk for the build output, and memory for 4 parallel compiles.

## Build

All of it from an **MSYS2 bash** (`MSYS2 MSYS` or `MINGW64` shell). From the directory that holds this file:

```bash
export STFX_SRC="$PWD/engine"                  # the engine tree (MSYS /d/... form is fine)
export OUT="$PWD/../stairfax-build"            # build output: objects, logs, game/eboot.bin   (pick anywhere)
export STAIRFAX_ISO="$PWD/engine/orig/GSAE01/sfa_rev1.iso"    # your disc (default is exactly this path)
export TMPDIR="D:/tmp"                         # a WINDOWS-form path (C:/... or D:/...) that exists. NOT /c/... or /d/...
unset MSYS_NO_PATHCONV                         # the Windows gcc takes /d/... only through MSYS path conversion
mkdir -p "$OUT" "$TMPDIR"

cd vita/host/engine
bash compile_all.sh     > "$OUT/compile_all.log" 2>&1     # 1. every game + port source -> $OUT/obj  (4 jobs, ~950 units)
python closure.py       > "$OUT/closure.log"     2>&1     # 2. grow the link set until it links; builds the Vita backend objects
bash build_game_vita.sh > "$OUT/build.log"       2>&1     # 3. link, GATES, package
tail -5 "$OUT/build.log"
```

Check that each log reached its last line: `compile_all.log` ends with `failed outside src/MSL_C: N`
(N is 1 or 2), `closure.log` ends with the `MIXED objects` report after `=== RESULT === link rc=0`, and
`build.log` ends with the `eboot_sha256=` line. **Do not trust a run whose log stops early**: a failed
`closure.py` leaves a stale link list and the build will then link old objects without complaint.

Result: `$OUT/game/eboot.bin` (a compressed FSELF), `$OUT/game/stairfax_game.elf`, `$OUT/game/build_id.txt`.
The build id line `stfx-<UTC timestamp>-<hash>-<random>` differs on every build by design; the objects and
the ELF's symbol table do not.

### The gates (all must pass; none may be skipped)

`build_game_vita.sh` refuses to package unless every one of these passes (the exit code says which):

| Gate | What it enforces |
|---|---|
| `gen_gx_rec.py prelink` / `postlink` | the GX boundary table, its generated wrappers and the `--wrap` set agree; every wrapper really links (13) |
| `gxt_lint.py` | the render thread shares no mutable state with the game thread (15) |
| `retail_gate.py` | **the ELF contains no retail bytes**: every table entry of 8+ bytes is searched for in the ELF in both byte orders (16) |
| symbol checks | the hooks that bind weakly across objects are really defined (a stale object would silently run a legacy path) (15) |
| `named_diff_gate.py` | only when a baseline exists (`$OUT/rt_integ/base_obj.sha`); absent in a fresh build, so skipped |
| `wgpipe_gate.py` | no code builds the GameCube write-gather-pipe address (data abort on the Vita) (7) |
| `pool_gate.py` | the string-pool offsets land on the same strings as the retail disc (8) |
| `overlay_gate.py` | no struct is laid over a global the port holds smaller (9) |
| `mldf_rules_check.py` | every byte-order rule in `mldf_endian.c` beats its other width on the real disc (10) |
| NEON check | no aligned NEON access in game objects (11) |

`retail_gate.py`, `pool_gate.py`, `overlay_gate.py` and `mldf_rules_check.py` read your disc image; that is
why a build needs it.

## Paths and environment

| Variable | Meaning | Default |
|---|---|---|
| `STFX_SRC` | engine tree | none: required |
| `OUT` | build output directory | none: required |
| `STAIRFAX_ISO` (or `STFX_ISO`) | your Rev 1 disc image | `$STFX_SRC/orig/GSAE01/sfa_rev1.iso` |
| `TMPDIR` | scratch dir, **Windows form** | none: required (a `/c/...` form fails ~140 units at the assembler) |
| `STFX_VITA` | the `vita/` directory | derived from the scripts' own location |
| `STFX_C3` | directory with `c3_cuts.h` | `vita/../gxaot/release/c3` |
| `MSYS_BASH`, `VITASDK_BIN` | MSYS2 bash / VitaSDK `bin` for the Python tools | `C:/dev/msys64/usr/bin/bash.exe`, `C:/dev/msys64/usr/local/vitasdk/bin` |
| `STFX_VERSION` | disc revision the source is compiled as | `GSAE01_rev1` |

## Running it

From the top of this tree, pack it into an installable VPK and install that with VitaShell:

```bash
bash vita/host/engine/package_vpk.sh "$OUT/game/eboot.bin" StarFoxAdventures.vpk
```

An optional third argument is a folder holding `pack.bin` and `gxm/*.gxp`, pre-compiled shaders that are
packed into the vpk as `shaders/` (the release vpk's `shaders/` folder is one). Without it the game compiles
every shader on the console the first time it needs it, in the background.

Put your disc image on the
console at `ux0:data/stairfax/sfa_rev1.iso`. The app shows an on-screen message if the image is missing, is a
different game, the wrong revision, or unreadable. Runtime options are marker files in `ux0:data/stairfax/`.

## Licensing and corresponding source

The eboot statically links vitaGL, vitaShaRK, math-neon and SceShaccCgExt (LGPL). Everything needed to rebuild
the eboot from source with a modified copy of those libraries is here: the application source (this tree), the
exact recipe and patches that built the libraries (`third_party/vitagl/`), and the link line (`build_game_vita.sh`,
`closure.py`). Relink with your own build of the libraries by re-running step 2 above and the build.

## Known traps

* `TMPDIR` in `/c/...` form: ~140 units fail at the assembler. Use `C:/...` or `D:/...`.
* `MSYS_NO_PATHCONV=1` in the environment: every compile misses its input (`compile_all.sh` then exits 12).
* More than 4 parallel compiles can exhaust memory; `compile_all.sh` fixes it at 4.
* `compile_all.sh` allows at most 5 failing units outside `src/MSL_C`; the expected count is 1 (`hal/pad_shim.c`,
  superseded on purpose).
