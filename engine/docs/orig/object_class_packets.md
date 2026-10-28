# Retail Object-Class Recovery Packets

This pass extends the existing `orig/` object tooling with a class-level view. The DLL-family packets are still the best lens when one DLL cleanly owns a subsystem, but many recovery targets are easier to reason about as "all objects in this retail class with the same placement width and a very small DLL set."

## Tool

- `python tools/orig/object_class_packets.py`
  - Groups retail object defs by `class_id` from `OBJECTS.bin`.
  - Cross-links each class with root romlist record widths, fixed-map hints, and EN DLL descriptors.
  - Overlays reference-only class names from `reference_projects/rena-tools/StarFoxAdventures/data/U0/objcats.xml`.
  - Overlays per-object param hints from `reference_projects/rena-tools/StarFoxAdventures/data/U0/objects.xml`.
  - Can materialize non-built class packet stubs under `src/main/unknown/classes/`.

## Why This Helps

The existing tooling already answered:

- which object defs exist
- which DLL each object def points at
- which root record widths each object uses
- which DLL slot functions exist in EN

What was still missing was a class-level boundary view that stays useful even when one subsystem spans two DLLs or when the DLL family is too broad. This tool fills that gap.

## Practical Filters

The raw class list includes broad umbrella categories like `various0030`, so the useful workflow is to filter for coherent classes first:

- tight classes with small DLL fanout:
  - `python tools/orig/object_class_packets.py --max-dlls 2 --max-defs 12 --min-placements 200`
- slightly wider but still plausible boundary candidates:
  - `python tools/orig/object_class_packets.py --max-dlls 3 --max-defs 32 --min-placements 100`
- one class in detail:
  - `python tools/orig/object_class_packets.py --search class:0x0015`
  - `python tools/orig/object_class_packets.py --search setuppoint`

These filters keep the output focused on packet classes that are actually plausible makeshift source files.

## Current High-Value Class Packets

Using `--max-dlls 2 --max-defs 12 --min-placements 200`, the best current packets are:

- `0x002C` `curve`
  - 1 def, 1 DLL, 5480 placements
  - still the best variable-length placement target
- `0x0015` `TrigPln`
  - 1 def, 1 DLL, 1225 placements
  - stable `20w` packet and a live EN descriptor map
- `0x007E` `CmbSrc`
  - 7 defs across only 2 DLLs, 1089 placements
  - uniform `12w` packet width across the whole class
- `0x004B` `HitAnimator`
  - 1 def, 1 DLL, 304 placements
  - cheap singleton boundary with a compact slot map
- `0x0004` `setuppoint`
  - 1 def, 1 DLL, 278 placements
  - broad map coverage and a very small parameter footprint
- `0x0065` `MagicPlant`
  - 1 def, 1 DLL, 217 placements
  - another cheap singleton packet
- `0x0021` `texscroll`
  - 2 defs across 2 DLLs, 226 placements
  - useful if the texscroll family needs to be recovered as a shared boundary

## Materialized Class Stubs

The current class packet materialization command was:

- `python tools/orig/object_class_packets.py --max-dlls 2 --max-defs 12 --min-placements 200 --materialize-top 7`

That wrote:

- [class_0004_setuppoint.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_0004_setuppoint.c)
- [class_0015_trigpln.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_0015_trigpln.c)
- [class_0021_texscroll.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_0021_texscroll.c)
- [class_002C_curve.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_002C_curve.c)
- [class_004B_hitanimator.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_004B_hitanimator.c)
- [class_0065_magicplant.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_0065_magicplant.c)
- [class_007E_cmbsrc.c](/C:/Projects/SFA-Decomp/src/main/unknown/classes/class_007E_cmbsrc.c)

These are intentionally non-built. They are packet files for exploratory split planning, not claims about final original filenames.

## Suggested Use

1. Run the filtered summary command first.
2. Open one packet stub under `src/main/unknown/classes/`.
3. Use the listed EN DLL slots as the first code entry points.
4. Use the retail placement widths to recover the class parameter struct.
5. Promote a packet into a real source split only after the boundary survives EN code inspection.
