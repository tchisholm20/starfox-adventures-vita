# Retail Object-Family Recovery Packets

This pass fills a gap between the existing `orig/` audits and actual recovery work: it turns retail object/DLL evidence into concrete family packets that can be opened like makeshift source files.

## Tool

- `python tools/orig/object_family_packets.py`
  - Combines retail `OBJECTS.bin`, `OBJECTS.tab`, `OBJINDEX.bin`, root `*.romlist.zlb`, and the active EN DLL descriptor table in `main.dol`.
  - Optionally overlays reference-only slot names, class names, and object-param field hints from `reference_projects/rena-tools/SFA-Browser/data/U0/*.xml`.
  - Ranks descriptor-backed object families by recovery leverage.
  - Can materialize non-built packet stubs under `src/main/unknown/` so object/DLL families have a real file boundary before final names or directories are proven.

## Why This Helps

The existing tools already answered:

- which retail object defs exist
- which DLL each object def points at
- how wide the root romlist placements are
- which DLL slot addresses exist in the current EN build

What was still missing was the synthesis step: one place that says "this DLL family is a plausible file boundary, these are the object defs it owns, these are the EN slot functions to open, and these are the reference-only field names worth checking next."

That is what `object_family_packets.py` provides.

## First Packet Set

The current materialized packet stubs are:

- [dll_0125_curve.c](/C:/Projects/SFA-Decomp/src/main/unknown/dll_0125_curve.c)
- [dll_0126_trigger.c](/C:/Projects/SFA-Decomp/src/main/unknown/dll_0126_trigger.c)
- [dll_0139_hitanimator.c](/C:/Projects/SFA-Decomp/src/main/unknown/dll_0139_hitanimator.c)
- [dll_00E9_setuppoint.c](/C:/Projects/SFA-Decomp/src/main/unknown/dll_00E9_setuppoint.c)

These are intentionally non-built. They are packet files, not claims that the original game used those exact filenames.

## High-Value Families

### 1. `curve` is still the cleanest single-def boundary

- DLL `0x0125`
- one retail def: `0x0491 curve`
- `5480` root placements
- one EN descriptor with `12` slots
- one family, but four observed root placement widths: `13w`, `14w`, `15w`, `17w`

That combination makes `curve` the best current target for recovering a real variable-length placement decoder and then pushing into its DLL interface.

### 2. The trigger family already behaves like one shared file boundary

- DLL `0x0126`
- nine retail defs: `TrigPnt`, `TrigCyl`, `TrigPln`, `TrigArea`, `TrigTime`, `TrigButt`, `TriggSetp`, `TrigBits`, `TrigCrve`
- `1483` root placements
- one EN descriptor with `10` slots
- every placed trigger variant uses the same root record width: `20w`

That is strong evidence for a shared family implementation rather than nine unrelated files.

### 3. `HitAnimator` and `setuppoint` are cheap, high-signal singletons

- `HitAnimator`:
  - DLL `0x0139`
  - one retail def
  - `304` placements
  - clean `init` / `update` / `getExtraSize` slot mapping by index
- `setuppoint`:
  - DLL `0x00E9`
  - one retail def
  - `278` placements
  - mostly stubbed DLL interface, but good retail/root pressure and a compact param layout

These are good starter targets because they already have small, coherent family boundaries.

## Practical Use

- Summary:
  - `python tools/orig/object_family_packets.py`
- Search one family:
  - `python tools/orig/object_family_packets.py --search dll:0x0126 trigger`
  - `python tools/orig/object_family_packets.py --search dll:0x0125`
- Materialize packet stubs:
  - `python tools/orig/object_family_packets.py --search dll:0x0125 --materialize-top 1`
  - `python tools/orig/object_family_packets.py --search dll:0x0126 --materialize-top 1`
  - `python tools/orig/object_family_packets.py --search dll:0x0139 --materialize-top 1`
  - `python tools/orig/object_family_packets.py --search dll:0x00E9 --materialize-top 1`

The intended workflow is:

1. Pick a family packet.
2. Open the EN slot functions listed in that packet.
3. Cross-check the retail placement widths and object-def cluster.
4. Use the reference-only slot names or field names as hypotheses, not source truth.
