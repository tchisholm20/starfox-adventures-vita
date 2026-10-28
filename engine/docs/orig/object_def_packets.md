# Retail Object-Def Recovery Packets

This pass pushes the existing object packet work one level further: instead of only ranking DLL families or class packets, it can materialize a non-built exploratory source file for each retail object def.

## Tool

- `python tools/orig/object_def_packets.py`
  - Cross-links every retail object def with root romlist widths, class packets, DLL families, EN descriptor slots, and reference-only object/DLL hints.
  - Can materialize non-built per-object packet stubs under `src/main/unknown/objects/`.
  - Keeps the output retail-backed: placement counts, widths, remap aliases, inline object-def substructures, and sibling defs all come from `orig/GSAE01`.

## Why This Helps

The earlier packet tools were good for choosing one promising family or class, but they still left a gap when the goal was:

- generate a broad sweep of exploratory split files quickly
- keep one concrete object def per file so a side-agent can chip away without arguing about final boundaries first
- preserve the outer class/DLL context so those exploratory files can later be merged into a real boundary instead of becoming dead-end junk

That is what `object_def_packets.py` is for.

## Practical Filters

- all placed retail defs:
  - `python tools/orig/object_def_packets.py`
- only tighter class/DLL packets:
  - `python tools/orig/object_def_packets.py --max-class-defs 8 --max-dll-defs 8`
- one object in detail:
  - `python tools/orig/object_def_packets.py --search def:0x051C`
  - `python tools/orig/object_def_packets.py --search curve`
- one family or class slice:
  - `python tools/orig/object_def_packets.py --search dll:0x0126 --materialize-all`
  - `python tools/orig/object_def_packets.py --search class:0x0015 --materialize-all`

## Current High-Value Packets

The current top packets are:

- `0x0491` `curve`
  - singleton class and singleton DLL boundary
  - variable retail widths: `13w`, `14w`, `15w`, `17w`
  - best current target for principled variable-length placement recovery
- `0x051C` `TrigPln`
  - single-def class inside the shared trigger DLL
  - strong leaf object target with the full trigger family still visible around it
- `0x04BC` `HitAnimator`
  - cheap singleton packet with a compact EN slot map
- `0x0493` `setuppoint`
  - another singleton packet that is easy to split and inspect
- `0x059C` `CmbSrc`
  - useful because the object stub keeps the wider `CmbSrc` class and DLL family attached instead of pretending it is already solved

## Materialized Stub Batch

The current broad exploratory batch was materialized with:

- `python tools/orig/object_def_packets.py --min-placements 20 --materialize-all`

That wrote `118` non-built object stubs under `src/main/unknown/objects/`.

These files are intentionally exploratory. They do not claim to be final original filenames or final source boundaries. Their job is to give object recovery work a fast, retail-backed foothold.

## Suggested Use

1. Materialize a batch with a placement threshold that keeps the set manageable.
2. Open one `src/main/unknown/objects/obj_*.c` packet.
3. Use the linked class packet and family packet paths to decide whether the object is really a singleton, a class packet, or part of a wider DLL source unit.
4. Use the listed EN descriptor slots and retail placement widths as the first recovery anchors.
5. Promote or merge the exploratory stub only after the surrounding boundary survives EN code inspection.
