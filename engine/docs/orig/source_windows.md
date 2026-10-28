# Retail source window candidates

This pass closes the next practical gap after `source_corridors.py`.

`source_corridors.py` can tell us that a retail-backed EN span is too small or too wide compared with the matching debug-side split. What it still leaves open is the actual split-planning question: which current EN function window is the best fit for that file size right now?

## Tool

- `python tools/orig/source_windows.py`
  - Starts from the same retail EN source anchors used by `source_corridors.py`.
  - Keeps only anchors that also have an exact debug-side split size.
  - Searches nearby current EN functions for ranked windows whose total size best matches that debug split.
  - Carries corridor context forward so each candidate still shows the likely neighboring filenames.
  - Reports xref coverage, which matters when one retail source tag currently fans out across too many EN functions to fit one file cleanly.

## High-value findings

### 1. `SHthorntail.c` is already a strong one-function source window

Current best-fit window:

- `0x801D5764-0x801D5AFC`
- size `0x398`
- debug target `0x3B8`
- delta `-0x20`
- xref coverage `1/1`

Practical read:

- the current seed is already close enough to the debug split to use directly as a first-pass file window
- this is one of the cleanest current retail-backed DLL skeletons

### 2. `DIMBoss.c` expands cleanly to an 8-function EN window

Current best-fit window:

- `0x801BD0E8-0x801BDA04`
- size `0x91C`
- debug target `0x900`
- delta `+0x1C`
- xref coverage `1/1`

Practical read:

- the current one-function seed is slightly too small
- adding the immediately adjacent EN functions produces a near-perfect match without needing to guess across a broad neighborhood

### 3. `camcontrol.c` now has an exact-size EN window candidate

Current best-fit window:

- `0x80102CFC-0x80103DDC`
- size `0x10E0`
- debug target `0x10E0`
- delta `0x0`
- xref coverage `1/1`

Practical read:

- this is the first report in the repo that turns the retail `camcontrol.c` seed into a concrete whole-file-sized EN window
- the surrounding corridor still matters, but the size target is no longer abstract

### 4. `curves.c` has a plausible 68-function DLL window instead of a 1-function seed

Current best-fit window:

- `0x800E260C-0x800E8954`
- size `0x6348`
- debug target `0x634C`
- delta `-0x4`
- xref coverage `1/1`

Practical read:

- the old retail warning only gave a tiny foothold
- the new report turns it into a bounded EN block between the `expgfx.c` and `camcontrol.c` corridors

### 5. `objanim.c` is a real warning sign, not a whole-file seed

Current best-fit shrink candidate:

- `0x800303FC-0x80030780`
- size `0x384`
- debug target `0x3A8`
- delta `-0x24`
- xref coverage only `1/3`

Practical read:

- the current grouped `objanim.c` xrefs do not fit one debug-sized file window cleanly
- the `objanim.c -> objHitReact.c` corridor still resolves to one missing `objhits.c`
- this is exactly the kind of case where xref coverage matters: the retail source tag is useful, but the grouped EN span should not be treated as one file

## Practical use

- Summary:
  - `python tools/orig/source_windows.py`
- Inspect one source in detail:
  - `python tools/orig/source_windows.py --search SHthorntail`
  - `python tools/orig/source_windows.py --search camcontrol`
  - `python tools/orig/source_windows.py --search objanim`
- Spreadsheet-friendly dump:
  - `python tools/orig/source_windows.py --format csv`

## How it fits

- Use `source_skeleton.py` to find the retail-backed EN island.
- Use `source_corridors.py` to decide whether the current retail-backed span is too small or too wide.
- Use `source_windows.py` when that source also has an exact debug-side split size and you want a ranked current EN window instead of only a fit verdict.
- Use `source_gap_packets.py` when the next step is recovering the files between two anchors rather than tightening one anchor itself.
