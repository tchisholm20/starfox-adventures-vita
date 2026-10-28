# Retail function-label recovery

## Summary

- Retail-labeled function candidates: `3`
- Candidates with direct EN xrefs: `3`
- Candidates with debug-side name bridges: `2`

## Strongest function-name bridges

- `laser.c` retail label `Init`
  EN xref: `fn_802096AC+0xC`
  debug path: `dll/CF/laser.c`
  debug name bridge: `laser_init`
- `objanim.c` retail label `setBlendMove`
  EN xrefs: `fn_8002EC4C+0xAC`, `fn_8002F334+0x174`, `fn_8003042C+0x1DC`
  debug path: `main/objanim.c`
  debug name bridge: `Object_ObjAnimSetMove`

## Retail labels without a stronger bridge yet

- `textblock.c` retail label `Init`
  EN xrefs: `fn_80209624+0xC`, `fn_80209650+0xC`, `fn_80209680+0xC`
  No trustworthy debug-side bridge is bundled here yet, but the retail label is still enough to name the recovery target and its entrypoint cluster.

## Tool

- `python tools/orig/source_functions.py`
  - Starts from EN retail `main.dol` source-tagged strings that carry function labels.
  - Keeps the retail file name, retail label, retail message, and current EN xrefs together.
  - Cross-references debug-side paths and function names only as side evidence, not source truth.

## Practical use

- Summary:
  - `python tools/orig/source_functions.py`
- Focus one label or file:
  - `python tools/orig/source_functions.py --search objanim setBlendMove Init`
- Dump spreadsheet-friendly rows:
  - `python tools/orig/source_functions.py --format csv`

This is the fastest way to answer "does `orig/` already tell us what this function was called?" before inventing another placeholder name.
