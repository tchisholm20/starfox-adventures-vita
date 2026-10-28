# Retail Source Corridors

This pass fills a specific gap in the `orig/` workflow.

`source_recovery.py` and `source_skeleton.py` can tell us that a retail source name survives in EN and where its current xrefs land. What they do not answer is whether that current EN span is actually a plausible whole-file boundary, or whether it is only a tiny seed inside a much larger file, or a too-wide span that likely crosses into neighboring files.

## Tool

- `python tools/orig/source_corridors.py`
  - Starts from the same retail EN source-tag crosswalk used by `source_recovery.py`.
  - Compares each EN xref-backed span against the matching `sfadebug` split size when an exact debug source path exists.
  - Uses `notes/srcfiles.txt` only as approximate source-order context so adjacent retail-backed anchors can expose likely in-between files.
  - Shows the current EN functions sitting in those corridors so the next split pass can open the right local address window immediately.

## High-Value Findings

### 1. Current EN xref spans are not reliable file boundaries by themselves

The strongest current examples:

- `objanim.c`
  - EN seed span: `0x8002EC4C-0x80030780`
  - exact debug split size: `0x3A8`
  - current EN seed is `7.44x` wider than the debug split
  - practical read: the current retail-backed span almost certainly crosses into neighboring files, especially `objhits.c`

- `curves.c`
  - EN seed span: `0x800E556C-0x800E56A4`
  - exact debug split size: `0x634C`
  - current EN seed only covers about `1%` of the debug-side file size
  - practical read: the retail warning is only a tiny anchor inside a much larger file

- `camcontrol.c`
  - EN seed span: `0x80102D3C-0x80103130`
  - exact debug split size: `0x10E0`
  - current EN seed only covers about `23%` of the debug-side file size

- `laser.c`
  - EN seed span: `0x802096AC-0x802096D8`
  - exact debug split size: `0x934`
  - current EN seed is only a tiny constructor-style foothold

The flip side also matters:

- `SHthorntail.c`
  - EN seed span is within `0x20` bytes of the debug split size
  - practical read: this is one of the cleanest current file-boundary seeds

- `DIMBoss.c`
  - EN seed span is still slightly small, but close enough to be a credible first-pass source window

### 2. Short source-order corridors expose immediate missing files

The most useful short corridors right now:

- `objanim.c -> objHitReact.c`
  - only one intervening debug-side source: `objhits.c`
  - current EN gap: `0x4D1C`
  - practical read: this is a strong local split-planning target, and explains why the current `objanim.c` seed is too wide

- `expgfx.c -> curves.c`
  - intervening sources: `modgfx.c`, `modelfx.c`, `dim_partfx.c`, `df_partfx.c`, `objfsa.c`
  - current EN gap: `0x45604`
  - practical read: the `expgfx` seed is useful, but the actual recovery window clearly spans a larger render/effects neighborhood

- `curves.c -> camcontrol.c`
  - intervening sources: `gameplay.c`, `pickup.c`, `modanimeflash1.c`, `modcloudrunner2.c`
  - current EN gap: `0x1D698`
  - practical read: the corridor is small enough to use directly when planning a first source skeleton in this region

### 3. Some retail-backed anchors still have no usable debug-side order

Current example:

- `textblock.c`
  - clean EN seed at `0x80209624-0x802096AC`
  - no exact debug split and no `srcfiles.txt` placement
  - practical read: keep it as a strong retail-backed micro-window, but do not infer broader source order from debug-side inventory yet

## Caveat

`notes/srcfiles.txt` is approximate source-order evidence based on string appearance, not source-truth. It is most useful for short local corridors. It should not override exact retail EN evidence or exact debug split-path matches.

## Practical Use

- Summary:
  - `python tools/orig/source_corridors.py`
- One anchor in detail:
  - `python tools/orig/source_corridors.py --search objanim`
  - `python tools/orig/source_corridors.py --search curves camcontrol`
  - `python tools/orig/source_corridors.py --search textblock laser`
- Spreadsheet-friendly dump:
  - `python tools/orig/source_corridors.py --format csv`

## How It Fits

- Use `source_skeleton.py` to find retail-backed EN islands.
- Use `source_corridors.py` when you need to judge whether that island is too small, too wide, or sitting next to one or two likely missing files.
- Use `source_object_packets.py` when the same source name also needs DLL/object/class evidence.

The main value here is split planning discipline. It is a guardrail against treating every retail-backed xref envelope as a whole file.
