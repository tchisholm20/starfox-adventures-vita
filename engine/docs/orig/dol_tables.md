# `orig/GSAE01/sys/main.dol` runtime table notes

This pass focuses on retail EN `main.dol` tables that are directly useful for loader recovery and split scaffolding.

## Tool

- `python tools/orig/dol_tables.py`
  - Auto-recovers the runtime file-ID table from the retail EN DOL instead of trusting older XML constants.
  - Cross-checks each runtime alias against the retail EN FST to show which names are real disc basenames, which only match by case/path, and which are alias-only.
  - Dumps the live `.ctors` / `.dtors` entries and resolves them through `config/GSAE01/symbols.txt` when available.

## High-value findings

### 1. The EN retail DOL still contains the full runtime file-ID table

`dol_tables.py` recovers an 88-entry table at `0x802CBECC` covering file IDs `0x00` through `0x57`.

This matters because it gives loader-facing names directly from the retail binary:

- `0x25` `BLOCKS.bin`
- `0x26` `BLOCKS.tab`
- `0x42` `DLLS.bin`
- `0x43` `DLLS.tab`
- `0x44` `DLLSIMPO.bin`
- `0x4F` `TEXPRE.bin`
- `0x50` `TEXPRE.tab`
- `0x51` `PREANIM.bin`
- `0x52` `PREANIM.tab`
- `0x57` `ENVFXACT.bin`

This is a much better anchor for naming file-loader code than one-off DOL strings or old reference-project constants.

### 2. The duplicate file IDs cleanly expose the map-local file families

The table has 15 duplicate IDs that reuse earlier filename pointers.

The important ones are:

- `0x45` / `0x46`: `MODELS.tab` / `MODELS.bin`
- `0x47` / `0x48`: `BLOCKS.bin` / `BLOCKS.tab`
- `0x49` / `0x4A`: `ANIM.TAB` / `ANIM.BIN`
- `0x4B` / `0x4C`: `TEX1.bin` / `TEX1.tab`
- `0x4D` / `0x4E`: `TEX0.bin` / `TEX0.tab`
- `0x53` / `0x54`: `VOXMAP.tab` / `VOXMAP.bin`
- `0x55` / `0x56`: `ANIMCURV.bin` / `ANIMCURV.tab`

That is direct evidence that the runtime distinguishes:

- the early global/root-facing IDs
- a later set of map-local load IDs that intentionally reuse the same filenames

This should help when reconstructing file loader enums, switch tables, and map-directory code paths.

### 3. Several runtime aliases are not real retail basenames

The EN audit finds 17 file IDs whose names do not appear as retail EN FST basenames at all.

The most useful examples are:

- `AUDIO.tab`
- `AUDIO.bin`
- `AMBIENT.tab`
- `AMBIENT.bin`
- `MPEG.tab`
- `MPEG.bin`
- `MUSICACT.bin`
- `BLOCKS.bin`
- `BLOCKS.tab`
- `DLLS.bin`
- `DLLSIMPO.bin`

These are exactly the kinds of names that can mislead recovery work if we assume every runtime alias corresponds to a literal extracted file.

The safer read is:

- some aliases are old or internal loader names
- some point at renamed data families
- some may refer to resident/generated data rather than a retail basename

### 4. A few important runtime aliases only match by case or subpath

The DOL table also shows cases where the runtime alias is close to a retail filename, but not exact:

- `0x03` `SFX.bin` -> `audio/data/Sfx.bin`
- `0x07` `MUSIC.bin` -> `audio/data/Music.bin`
- `0x51` `PREANIM.bin` -> `PREANIM.BIN`
- `0x52` `PREANIM.tab` -> `PREANIM.TAB`

That is useful when tracing file IDs through the loader, because a missing exact basename match does not always mean the family is absent from disc.

### 5. The live constructor/destructor tables are tiny and already actionable

`dol_tables.py` also recovers the EN init tables:

- `.ctors[0]` -> `__init_cpp_exceptions` at `0x80286E78`
- `.ctors[1]` -> `fn_802952E8`
- `.dtors[0]` -> `__destroy_global_chain` at `0x802866D0`
- `.dtors[1]` -> `__fini_cpp_exceptions` at `0x80286E44`
- `.dtors[2]` -> `__destroy_global_chain` at `0x802866D0`

This is immediately useful for split work:

- the `.ctors` and `.dtors` ranges in EN are confirmed live, not guessed
- there is one still-unnamed constructor target at `0x802952E8`
- the destructor table intentionally references `__destroy_global_chain` twice

That gives a concrete next target for naming and for checking adjacent source-file boundaries around the remaining unknown constructor.

## Practical use

- Summary: `python tools/orig/dol_tables.py`
- CSV dump of runtime file IDs: `python tools/orig/dol_tables.py --format csv`
- Targeted lookup:
  - `python tools/orig/dol_tables.py --search BLOCKS DLLS PREANIM`
  - `python tools/orig/dol_tables.py --search __init_cpp __destroy_global_chain`
