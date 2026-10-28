# `orig/GSAE01/sys/main.dol` string-xref notes

This pass focuses on one question the earlier `orig/` inventory did not answer well: which retail EN DOL strings still point back at concrete code clusters that can help recover source-file boundaries, loader code, or subsystem names.

## Tool

- `python tools/orig/dol_xrefs.py`
  - Extracts source-tagged and file-path-like strings directly from retail EN `main.dol`.
  - Scans text for direct `lis` + `addi` / `ori` string loads and resolves each xref through `config/GSAE01/symbols.txt` when possible.
  - Supports focused substring search and CSV export so the results are easy to reuse while naming functions or proposing first split boundaries.

## High-value findings

### 1. Several source-tagged retail strings land in single unknown game functions

These are strong naming anchors because they are direct text xrefs, not just ambient strings left in rodata:

- `objHitReact.c: sphere overflow! %d`
  - xref: `0x800355DC`
  - current symbol: `fn_8003549C+0x140`
- `curves.c: MAX_ROMCURVES exceeded!!`
  - xref: `0x800E5584`
  - current symbol: `fn_800E556C+0x18`
- `<camcontrol.c>  failed to load triggered camaction actionno %d`
  - xref: `0x80102FC4`
  - current symbol: `fn_80102D3C+0x288`
- `SHthorntail.c`
  - xref: `0x801D5AC8`
  - current symbol: `fn_801D5764+0x364`

If someone wants a first-pass naming pass that stays grounded in retail evidence, those four are clean targets.

### 2. `expgfx.c` is not one isolated string; it marks a small function cluster

The EN retail DOL has four `expgfx.c` diagnostics with direct xrefs:

- `expgfx.c: mismatch in add/remove in exptab`
  - xrefs: `fn_8009B36C+0xE4`, `fn_8009B4E0+0xE0`
- `expgfx.c: addToTable usage overflow`
  - xref: `fn_8009E078+0x74`
- `expgfx.c: exptab is FULL`
  - xref: `fn_8009E078+0xFC`
- `expgfx.c: invalid tabindex`
  - xref: `fn_8009F558+0x374`
- `expgfx.c: scale overflow`
  - xref: `fn_8009F558+0x434`

That is enough to treat `0x8009B36C` through `0x8009F558` as one promising recovery island instead of unrelated anonymous functions.

### 3. Loader-facing filename strings point at concrete file-handling functions

The same pass gives useful EN anchors for file-loader recovery:

- `%s.romlist.zlb`
  - xrefs: `fn_800484A4+0x40`, `fn_800484A4+0xDC`
- `gametext/%s/%s.bin`
  - xrefs: `fn_80019C5C+0x144`, `fn_8001A6A4+0x220`
- `LACTIONS.bin`
  - xrefs: `fn_8001A950+0x14`, `fn_8001A950+0x1C`
- `AUDIO.tab`
  - xref: `fn_80044548+0x1C`
- `/savegame/save%d.bin`
  - xref: `fn_8011B0FC+0x190`

This is especially useful for split scaffolding because it ties retail filenames and format strings to actual EN code addresses instead of leaving them as free-floating DOL strings.

### 4. A few source tags still matter even without direct text xrefs

The tool also surfaces source-tagged strings that remain useful as context, even though they are not loaded by a direct `lis`/`addi` pair:

- `n_attractmode.c`
  - sits beside title-movie-related strings, including `starfox.thp` and `malloc for movie failed`
- `dvdfs.c`
  - still present in retail rodata and consistent with the already-known DVD SDK block
- `H<textblock.c Init>No Longer supported`
  - likely a duplicate or garbled variant of the nearby `textblock.c` warning string

The `n_attractmode.c` case is the main one worth using: it gives a retail source tag for title-screen movie code even when the exact xref pattern is indirect.

### 5. The retail DOL preserves a few object-init warnings that line up with Rena notes

Two examples:

- `<textblock.c Init>No Longer supported`
  - xrefs: `fn_80209624+0xC`, `fn_80209650+0xC`, `fn_80209680+0xC`
- `<laser.c Init>No Longer supported`
  - xref: `fn_802096AC+0xC`

That lines up with `reference_projects/rena-tools/StarFoxAdventures/notes/objlist.md`, where `KP_textbloc` is noted as spamming the same `textblock.c` warning. The local tool makes that evidence reproducible from this repo alone.

## Practical use

- Summary: `python tools/orig/dol_xrefs.py`
- Focused search:
  - `python tools/orig/dol_xrefs.py --search camcontrol curves SHthorntail`
  - `python tools/orig/dol_xrefs.py --search romlist gametext LACTIONS savegame`
  - `python tools/orig/dol_xrefs.py --search attractmode dvdfs`
- CSV dump: `python tools/orig/dol_xrefs.py --format csv`

The most productive use is to search one source tag or loader token, then open the matching current `fn_...` range and recover the adjacent dependency cluster around it.
