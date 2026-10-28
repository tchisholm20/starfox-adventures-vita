# Retail Source Skeleton

This pass adds one missing view over the bundled `orig/` evidence: not just "which retail source names survive", but "which current EN text islands those names already carve out".

## Tool

- `python tools/orig/source_skeleton.py`
  - Starts from the same retail EN `main.dol` source-tag xrefs used by `source_recovery.py`.
  - Resolves them through current `config/GSAE01/symbols.txt`.
  - Clusters nearby xref-backed spans into address-ordered EN islands.
  - Shows the uncovered current EN functions sitting between retail-backed spans inside the same island.
  - Keeps debug/reference path hints attached so each island already points at the most plausible source-file skeleton.

The main use is split planning. Instead of treating each retail string as an isolated clue, this report turns them into recoverable EN windows.

## High-value findings

### 1. The current retail-backed source map collapses into eight EN islands

The new report finds:

- `9` xref-backed retail source groups
- `8` EN text islands
- `1` multi-source island
- all `8` islands still sit outside current `splits.txt`

That is a much better starting shape for skeleton recovery than a flat list of source names.

### 2. `textblock.c` and `laser.c` are one real shared micro-island

The strongest new structural result is the merged island:

- `0x80209624-0x802096D8`
- `5` current EN functions total
- `4` retail-backed functions
- sources: `textblock.c` and `laser.c`
- one uncovered function in the middle: `fn_8020967C`

That is exactly the kind of window you want for first-pass source skeleton work: tiny, bounded, and already carrying two retail file names plus one obvious unnamed straggler.

### 3. `expgfx.c` is the largest DLL-style skeleton seed

The `expgfx.c` island is currently the biggest retail-backed unsplit DLL-style target:

- span: `0x8009B36C-0x8009FF68`
- size: `0x4BFC`
- `19` current EN functions in the window
- `4` retail-backed functions with `6` xrefs total

This matters because the retail warnings already prove the subsystem name, while the new island view shows how much adjacent anonymous code needs to move with it.

### 4. `objanim.c` is still the cleanest early `main/` skeleton seed

The `objanim.c` island stays one of the best early main-text recovery targets:

- span: `0x8002EC4C-0x80030780`
- size: `0x1B34`
- `13` current EN functions in the window
- retail label: `setBlendMove`

The island view helps because it shows the real cost of the split immediately: three retail-backed functions plus ten adjacent anonymous helpers that almost certainly belong in the same source neighborhood.

### 5. Several singleton islands are already clean enough for direct source windows

These remain strong one-file seeds:

- `camcontrol.c` at `0x80102D3C-0x80103130`
- `curves.c` at `0x800E556C-0x800E56A4`
- `DIMBoss.c` at `0x801BD0E8-0x801BD7F4`
- `SHthorntail.c` at `0x801D5764-0x801D5AFC`
- `objHitReact.c` at `0x8003549C-0x80035728`

The new value is that each one now comes with its immediate neighboring functions, so the next recovery pass can open the right local address window instead of hunting for context by hand.

## Practical use

- Summary:
  - `python tools/orig/source_skeleton.py`
- Inspect one island:
  - `python tools/orig/source_skeleton.py --search textblock laser`
  - `python tools/orig/source_skeleton.py --search objanim`
  - `python tools/orig/source_skeleton.py --search 0x8009B36C`
- Spreadsheet-friendly dump:
  - `python tools/orig/source_skeleton.py --format csv`

## How it fits with the existing orig tools

- Use `source_skeleton.py` first when you want the next retail-backed source window to recover.
- Use `source_boundaries.py` when you want one-source split status and path hints in more detail.
- Use `source_object_packets.py` when the same source window also needs object/class/DLL packet context.

The main improvement here is not new evidence. It is better shape: retail leftovers are now grouped into concrete EN source islands that are much closer to a usable source skeleton.
