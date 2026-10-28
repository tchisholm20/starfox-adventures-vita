# `orig/GSAE01` DLL catalog notes

This pass turns the object `dll_id` field into something directly actionable: a reproducible map from `OBJECTS.bin` DLL IDs to the live EN retail DLL tables in `main.dol`.

## Tool

- `python tools/orig/dll_catalog.py`
  - Recovers the active EN DLL pointer table directly from `config/GSAE01/symbols.txt`.
  - Parses descriptor-like DLL records from `orig/GSAE01/sys/main.dol`.
  - Cross-links those DLL IDs back to `OBJECTS.bin` and root romlist placement counts.
  - Flags object DLL IDs whose table targets are not real descriptor tables.

## High-value findings

### 1. The active EN DLL table is at `0x802C6A80`, not the older `0x802C6300` anchor from legacy notes

In this repo's current EN symbols, `0x802C6A80` is the only `.data` object with size `0xB08`, which is exactly `0x2C2 * 4`.

That parses cleanly as the full DLL pointer table for IDs `0x0000` through `0x02C1`.

This matters because it gives a current, repo-local anchor for DLL recovery instead of depending on older external tooling assumptions.

### 2. The retail EN DOL already contains hundreds of real DLL descriptors

`dll_catalog.py` recovers:

- `706` total DLL slots
- `669` descriptor-like entries
- `2` null slots
- `35` non-descriptor or otherwise invalid in-DOL targets

That is enough structure to start naming DLL families and function slots from retail data alone.

### 3. Most object DLL IDs already resolve cleanly to real descriptor tables

From `OBJECTS.bin`, the EN retail data uses `448` non-`0xFFFF` DLL IDs across object defs.

Of those:

- `427` resolve cleanly to descriptor tables in `main.dol`
- `21` do not

The unresolved object DLL IDs are:

- `0x0146` `CloudShipCo`
- `0x014D` `LaserBeam`
- `0x0151` `CFScalesGal`
- `0x0152` `CF_ObjCreat`
- `0x015C` `CFForceFiel`
- `0x0161` `CFTreasRobo`
- `0x0165` `CFRemovalSh`
- `0x0168` `HoloPoint`
- `0x0208` `WM_Wallpowe`
- `0x0214` `WM_TransTop`
- `0x023D` `DBPointMum`
- `0x0240` `GCRobotBlas`
- `0x026A` `DR_Geezer`
- `0x026D` `DR_Vines`
- `0x0270` `DR_Rock`
- `0x0274` `DR_pulley`
- `0x0275` `DR_cradle`
- `0x0277` `CFWindLiftL`
- `0x0278` `DRCollapseP` / `DRPlatformC`
- `0x027A` `DR_Collapse`
- `0x027F` `DR_LightHal` / `DR_LightPol` / `DR_LightLam`

Several of these point at tiny `.sdata` blobs rather than function tables, which makes them good candidates for follow-up around `DLLS.bin` / `DLLSIMPO.bin` handling or special-case object families.

### 4. The common object-DLL callback layout is visible directly in retail data

The most common descriptor shape is:

- `10` slots with mask `1101111111` across `294` DLLs

Other common shapes are:

- `4` slots with mask `1101`
- `6` slots with mask `110111`
- `7` slots with mask `1101111`
- `12` slots with mask `110111111111`

This is strong evidence for a shared callback interface, which is useful when carving out real DLL vtable-like structs and scaffolding source files.

### 5. High-placement object families already tie cleanly to concrete DOL function tables

Examples:

- `0x0125` `curve`: `5480` placements, `12` slots, mask `000100111111`
- `0x0100` `TrickyWarp`: `2139` placements, `10` slots, mask `0001100101`
- `0x0126` trigger family (`TrigPnt`, `TrigCyl`, `TrigPln`, `TrigArea`, ...): `1483` placements, `10` slots, mask `1101111111`
- `0x02AD` foliage family: `994` placements, `10` slots, mask `1101111111`
- `0x02B1` `CmbSrc*`: `922` placements, `10` slots, mask `1101111111`

This is exactly the kind of clustering that can drive first-pass DLL naming and file grouping before deeper matching work starts.

## Practical use

- Summary:
  - `python tools/orig/dll_catalog.py`
- CSV:
  - `python tools/orig/dll_catalog.py --format csv`
- Search by DLL ID, object name, symbol, or slot address:
  - `python tools/orig/dll_catalog.py --search dll:0x0126`
  - `python tools/orig/dll_catalog.py --search curve TrickyWarp`
  - `python tools/orig/dll_catalog.py --search fn_8019AFF4 0x80102D3C`

The intended workflow is simple:

1. Find an object family in `OBJECTS.bin`.
2. Resolve its DLL with `dll_catalog.py`.
3. Open the recovered slot functions in the EN DOL.
4. Use `dol_xrefs.py` or surrounding strings to push from DLL-family recovery into source-file naming and split work.
