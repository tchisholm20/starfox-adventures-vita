# `orig/GSAE01/files/*.romlist.zlb` parameter notes

This pass turns retail root romlists into a reusable parameter-width catalog for object placement recovery.

## Tool

- `python tools/orig/romlist_params.py`
  - Resolves placement-space object IDs through `OBJINDEX.bin`.
  - Cross-links each canonical placed object back to its `OBJECTS.bin` def, DLL ID, class ID, and fixed-map affinity.
  - Reports stable retail record sizes, effective parameter byte lengths, and sample param blobs.
  - Supports direct lookup by object name, def ID, DLL ID, class ID, map, or size.

## High-value findings

### 1. Retail romlists already define stable per-object parameter widths

The EN retail root romlists contain placements for `814` canonical object defs:

- `813` are fixed-size across all observed placements
- `1` is variable-size: `0x0491` `curve`

For fixed-size defs, the placement payload width is fully determined by retail data:

- total record size = `size_words * 4`
- object-specific tail size = `(size_words * 4) - 0x18`

That is enough to scaffold many placement structs without first decoding loader code.

### 2. The trigger family is already a coherent `0x38`-byte param cluster

Searching `dll:0x0126` shows every observed trigger-family placement uses the same retail width:

- `TrigPnt`
- `TrigCyl`
- `TrigPln`
- `TrigArea`
- `TrigBits`
- `TriggSetp`
- `TrigButt`

All of them are fixed `20w` records, which means a `0x38`-byte object-specific tail after the common `0x18`-byte header.

This is a strong recovery hint: the trigger DLL family already behaves like one shared placement struct family in retail data.

### 3. The smallest placed objects are real header-only records

`size:6w` shows several placed objects with zero trailing param bytes:

- `ARWLevelCon`
- `SH_LevelCon`
- `KT_RexLevel`
- `WCLevelCont`
- `WM_LevelCon`

Most of these are class `0x0039` "level controller"-style objects spread across different maps and DLLs.

This matters because it proves the retail loader accepts pure `0x18`-byte headers with no extra object tail at all.

### 4. `7w` objects expose a useful "single extra word" tier

The next tier up is `7w`, meaning one `u32`-sized parameter after the common header.

Examples:

- `sideload`
- `siderepel`
- `siderepelWi`
- several `InfoText`-style objects under DLL `0x0121`

That makes `6w`, `7w`, and `8w` good first targets when validating placement struct layouts in source.

### 5. `curve` is the only variable-length retail placement family

`curve` appears with four observed widths:

- `13w`
- `14w`
- `15w`
- `17w`

Sample blobs come from concrete retail romlists:

- `13w`: `arwingcity.romlist.zlb`
- `14w`: `clouddungeon.romlist.zlb`
- `15w`: `clouddungeon.romlist.zlb`
- `17w`: `capeclaw.romlist.zlb`

That makes `curve` the clearest single target for recovering a principled variable-length romlist decoder.

## Practical use

- Summary:
  - `python tools/orig/romlist_params.py`
- CSV:
  - `python tools/orig/romlist_params.py --format csv`
- Search examples:
  - `python tools/orig/romlist_params.py --search curve`
  - `python tools/orig/romlist_params.py --search dll:0x0126`
  - `python tools/orig/romlist_params.py --search size:6w`
  - `python tools/orig/romlist_params.py --search fixed map:dimpushblock`
