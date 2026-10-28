# `orig/GSAE01/files/mod*.{tab,zlb.bin}` notes

This pass focuses on the retail `BLOCKS` family that the DOL still names `BLOCKS.bin` / `BLOCKS.tab`, but which lands on the extracted `modXX.zlb.bin` / `modXX.tab` files in `orig/GSAE01/files/`.

## Tool

- `python tools/orig/blocks_catalog.py`
  - Audits every retail `mod*.tab` in `orig/GSAE01/files/`.
  - Confirms which same-name `mod*.zlb.bin` pairs resolve cleanly as direct inner-ZLB bundles.
  - Flags secondary tab-only families that have no same-name binary.
  - Ties directory-backed `mod*.tab` families back to map names via the EN retail DOL dir tables.

## High-value findings

### 1. Most retail `mod*.tab` families already expose real embedded file boundaries

`blocks_catalog.py` resolves `56` out of `64` `mod*.tab` files directly against a same-name `mod*.zlb.bin`.

For those resolved families:

- every unique table offset lands on a valid inner `ZLB` header
- each inner stream ends with only `0..31` bytes of padding before the next one
- the table is not opaque metadata anymore; it is already a reproducible split map for the concatenated block payloads

This is the main takeaway. The direct pairs are immediately useful for scaffolding file boundaries inside the retail `BLOCKS` binaries.

### 2. Flag byte `0x10` marks one canonical table entry per inner block stream

Across the direct families, the only observed flag bytes are:

- `0x00`
- `0x10`

More importantly:

- the total count of `0x10` entries exactly matches the total count of unique inner-ZLB offsets
- for every resolved family, each unique offset appears exactly once with `0x10`

That is strong evidence that `0x10` is the "primary" or canonical reference for one embedded block chunk, while the many `0x00` entries are aliasing or repeated placement references to the same chunk.

### 3. Root `mod*.zlb.bin` files are mirrors of the directory copies

The EN audit confirms:

- all `52` directory `mod*.zlb.bin` files have a byte-identical root duplicate

This is useful evidence when naming or splitting loader code:

- the disc root is not hiding a separate `BLOCKS` content family
- the runtime alias can be backed by root mirrors of map-owned binaries

### 4. Several maps carry extra `mod*.tab` files without a same-name binary

These are the direct unresolved/candidate leads:

- candidate same-dir bin:
  - `crfort/mod12.tab` -> `crfort/mod19.zlb.bin`
  - `dragrock/mod10.tab` -> `dragrock/mod4.zlb.bin`
  - `dragrock/mod29.tab` -> `dragrock/mod4.zlb.bin`
  - `dragrockbot/mod29.tab` -> `dragrockbot/mod11.zlb.bin`
  - `mmshrine/mod32.tab` -> `mmshrine/mod41.zlb.bin`
- still unresolved:
  - `shipbattle/mod30.tab`
  - `volcano/mod16.tab`
  - `worldmap/mod3.tab`

This is useful because it exposes maps where `BLOCKS` loading still has one more level of indirection than "same-name tab points at same-name binary".

### 5. A few tiny direct families are ideal block-loader testcases

Good low-complexity targets:

- `arwing/mod3.tab`: `58` table entries but only `1` inner ZLB stream
- `worldmap/mod46.tab`: `973` entries, still only `1` inner ZLB stream
- `linka/mod65.tab`: `1551` entries, only `3` inner streams
- `shipbattle/mod14.tab`: `288` entries, `7` inner streams
- `animtest/mod6.tab`: `144` entries, `22` inner streams

These are much cheaper places to validate a future `BLOCKS` extractor than starting from `warlock/mod16` or another large map family.

## Why this helps decomp

- It gives real file boundaries inside the retail `BLOCKS` data instead of treating `modXX.zlb.bin` as undifferentiated blobs.
- It gives a principled reason to split block families into embedded chunks before trying to name code or data ownership.
- It exposes the small set of map families whose `BLOCKS` indirection is still unresolved, which is where further loader work should focus.
- It gives immediate testcase candidates for future tools that want to extract or diff block chunks one-by-one.

## Usage

- Summary:
  - `python tools/orig/blocks_catalog.py`
- CSV:
  - `python tools/orig/blocks_catalog.py --format csv`
- Search by mod, dir, or map:
  - `python tools/orig/blocks_catalog.py --search mod:13`
  - `python tools/orig/blocks_catalog.py --search dir:swaphol`
  - `python tools/orig/blocks_catalog.py --search map:"ThornTail Hollow"`
