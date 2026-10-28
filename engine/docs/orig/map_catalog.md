# `orig/GSAE01` map catalog notes

This is a focused follow-up to [audit.md](/C:/Projects/SFA-Decomp/docs/orig/audit.md). The goal is narrower: recover the retail EN map catalog directly from `orig/GSAE01` so map IDs, romlists, dir aliases, and root-only object maps stop being guesswork.

## Tool

- `python tools/orig/map_catalog.py`
  - Parses `MAPINFO.bin`, `MAPS.bin`, `MAPS.tab`, `globalma.bin`, `TRKBLK.tab`, and the relevant pointer/int tables from `orig/GSAE01/sys/main.dol`.
  - Auto-discovers the DOL tables from the retail binary instead of trusting older reference addresses.
  - Emits either a markdown summary or a CSV catalog for all 117 EN map IDs.

## High-value findings

### 1. The EN retail DOL still contains the full map-ID catalog

`map_catalog.py` recovers three contiguous EN tables directly from `orig/GSAE01/sys/main.dol`:

- map ID to romlist name: 117 entries at `0x802CC518`
- asset-dir name table: 73 entries at `0x802CC784`
- map ID to dir ID: 75 entries at `0x802CC8A8`

That matters because the bundled reference `U0` address XML points at different locations. For this repo, the safe path is to mine the retail DOL itself instead of assuming those older constants still line up.

### 2. Only map IDs `0x00` through `0x4A` are dir-backed

The recovered `map ID -> dir ID` table stops at 75 entries.

- `0x00` through `0x4A` have a dir mapping
- `0x4B` through `0x74` do not

That split lines up exactly with the type histogram:

- most early maps are full dir-backed map families
- the 42 later IDs are root-only type-1 object maps and helper maps

This is useful for loader recovery because it cleanly separates:

- "load a real map dir plus root romlist"
- "load a root romlist-only object map"

### 3. The DOL proves that romlist names and dir names are not the same namespace

Several important EN aliases fall straight out of the recovered tables:

- `0x00` `Ship Battle`: romlist `frontend`, dir `shipbattle`
- `0x07` `ThornTail Hollow`: romlist `hollow`, dir `swaphol`
- `0x0A` `SnowHorn Wastes`: romlist `wastes`, dir `nwastes`
- `0x0C` `CloudRunner Fortress`: romlist `fortress`, dir `crfort`
- `0x12` `Moon Mountain Pass`: romlist `moonpass`, dir `mmpass`
- `0x13` `DarkIce Mines - Top`: romlist `snowmines`, dir `darkicemines`
- `0x15` `Ocean Force Point - Bottom`: romlist `kraztest`, dir `desert`

This is directly useful when naming loader code and when deciding whether a split should follow:

- the map display name from `MAPINFO.bin`
- the runtime romlist name from the DOL
- the asset directory name from the DOL dir table

### 4. `MAPINFO.bin` and `MAPS.bin` now have firmer field meaning

The new tool confirms:

- `MAPINFO.bin` records are `>28s B B H`
- the last field is a real `playerObj` / spawn object ID, not a generic unknown short
- `MAPS.bin` info records are `0x20` bytes:
  - `sizeX`, `sizeZ`, `originX`, `originZ`
  - `unk08`
  - four `unk0C` words
  - `nBlocks`
  - `unk1E`

That meaning is especially visible in the root-only object maps, where many type-1 maps carry a nonzero `playerObj` that matches the one-off object-family behavior.

### 5. Two EN map IDs are explicit `FACEFEED` sentinels

`MAPS.bin` info records for these map IDs start with `0xFACEFEED` instead of real geometry:

- `0x68` `wmcolrise`
- `0x72` `KamColumn`

These are good edge cases for loader recovery because they show that "map has an ID and romlist" does not always imply "map has a normal block grid."

### 6. Seven root romlists are outside the 117-ID map table

These exist in `orig/GSAE01/files/` but are not referenced by the recovered map-ID romlist table:

- `cfledge`
- `dimcannon`
- `dimcannonbase`
- `drhighplat`
- `goldplains`
- `greatfoxworld`
- `shlily`

That makes them good candidates for special-case loaders, helper objects, or other code paths outside the main map-ID catalog.

## Practical use

- Human summary: `python tools/orig/map_catalog.py`
- Machine-readable catalog: `python tools/orig/map_catalog.py --format csv`

The CSV output is the more useful form for recovery work. It gives one row per map ID with:

- map name and type
- `playerObj`
- romlist name
- dir ID and dir name when present
- map geometry/origin
- non-empty block counts and module IDs
- global map placement data when present
