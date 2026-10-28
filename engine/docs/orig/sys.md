# `orig/*/sys` notes

This pass focuses on retail disc metadata that was present in `orig/*/sys` but not yet captured locally in a reproducible tool.

## Tool

- `python tools/orig/sys_audit.py`
  - Parses `boot.bin`, `bi2.bin`, `apploader.img`, and `fst.bin`.
  - Verifies the extracted `orig/<region>/files/` tree against the retail FST.
  - Emits either a markdown summary, a full EN FST CSV, or direct file-ID lookups.

## High-value findings

### 1. The extracted `files/` trees are complete

For all three bundled regions:

- missing FST files: `0`
- extra extracted files: `0`
- missing FST dirs: `0`
- extra extracted dirs: `0`

That means the current `orig/<region>/files/` extraction is faithful to the retail FST. There is no hidden “leftover file on disc but not in the extracted tree” gap here.

### 2. Exact FST order differs across regions, but shared-path order is stable

The full file lists differ because PAL and JP do not have the same path set as EN. But once you ignore region-only paths, the common-path order is stable in every pair:

- `GSAE01` vs `GSAP01_rev1`: 3865 shared files, stable order
- `GSAE01` vs `GSAJ01`: 3410 shared files, stable order
- `GSAP01_rev1` vs `GSAJ01`: 3205 shared files, stable order

This is the useful part for recovery work. It means file IDs from the retail FST can be compared across regions as long as you translate by path first instead of assuming the full index space is identical.

### 3. The EN retail FST gives immediate file-ID anchors

Examples from `orig/GSAE01/sys/fst.bin`:

- `0x022D` `frontend.romlist.zlb`
- `0x0755` `GAMETEXT.bin`
- `0x0757` `globalma.bin`
- `0x0896` `MAPINFO.bin`
- `0x107A` `WARPTAB.bin`
- `0x0C24` `starfox.thp`

This is useful whenever code is passing around file IDs or indexing into loader tables and the decomp still lacks names.

### 4. `boot.bin` and `apploader.img` add a few retail facts worth preserving

For EN `GSAE01`:

- `boot.bin` game code is `GSAE`
- maker code is `01`
- disc number is `0`
- revision byte is `0x01`
- DOL offset is `0x0001E000`
- FST offset is `0x0035CA00`
- FST size is `0x1A716`
- `apploader.img` version string is `2002/04/10`

Important: keep treating the active repo target as EN `GSAE01`, per [AGENTS.md](/C:/Projects/SFA-Decomp/AGENTS.md). The tool reports the raw disc-header revision byte only; it does **not** try to relabel the project target from that byte.

## Practical use

- Summary: `python tools/orig/sys_audit.py`
- Full EN FST dump: `python tools/orig/sys_audit.py --format csv`
- Retail file-ID lookup:
  - `python tools/orig/sys_audit.py --path MAPINFO.bin WARPTAB.bin frontend.romlist.zlb`

The lookup mode is the important one during recovery. It turns retail filenames into FST indices and disc offsets without depending on old XML exports or reference-project assumptions.
