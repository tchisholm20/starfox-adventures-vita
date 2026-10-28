# `orig/GSAE01/sys/main.dol` string notes

This pass turns the EN retail `main.dol` into a reusable source-name and file-alias lookup instead of leaving those clues trapped in one-off notes.

## Tool

- `python tools/orig/dol_strings.py`
  - Extracts printable ASCII strings directly from the retail EN DOL.
  - Records both RAM address and DOL file offset for each string.
  - Summarizes source-like names, internal file-family aliases, and warning/error strings.
  - Supports substring search for quick lookup while naming code.

## High-value findings

### 1. The DOL still leaks a small but useful set of source-file names

The current EN retail DOL exposes these source-like names:

- `expgfx.c` at `0x803107B0`
- `objanim.c` at `0x802CB928`
- `objHitReact.c` at `0x802CB9D4`
- `curves.c` at `0x8031230C`
- `camcontrol.c` at `0x8031A764`
- `n_attractmode.c` at `0x8031AFDC`
- `SHthorntail.c` at `0x803280C8`
- `DIMBoss.c` at `0x80326784`
- `textblock.c` at `0x8032A838`
- `laser.c` at `0x8032A860`
- `dvdfs.c` at `0x803DD1C8`

That is not a full source map, but it is enough to seed naming around nearby functions, especially when a function cluster already has matching strings.

### 2. The DOL confirms several runtime file-family aliases that do not appear on disc verbatim

Useful internal aliases with addresses:

- `BLOCKS.bin` at `0x802CBCE4`
- `BLOCKS.tab` at `0x802CBCF0`
- `CACHEFON.bin` at `0x802CBBEC`
- `DLLSIMPO.bin` at `0x802CBE7C`
- `PREANIM.bin` at `0x802CBEA4`
- `PREANIM.tab` at `0x802CBEB0`
- `AUDIO.bin` at `0x802CBB2C`
- `AMBIENT.bin` at `0x802CBB44`
- `MUSIC.bin` at `0x802CBB5C`
- `MPEG.bin` at `0x802CBB74`
- `SFX.bin` at `0x803DC108`

This is direct evidence that some runtime loader names are aliases rather than one-to-one mirrors of the extracted disc filenames. The clearest case is `BLOCKS.*`, which lines up with the `modXX.zlb.bin` and `modXX.tab` family discussed in older SFA notes.

### 3. A few tagged warning strings give immediate subsystem anchors

Examples:

- `<camcontrol.c>  failed to load triggered camaction actionno %d`
- `<DIMBoss.c> freeing assets for DIMBoss`
- `<DIMBoss.c> loading assets for DIMTop`

These are good anchors when searching for logging callsites or when deciding where a recovered function cluster should live.

### 4. `dvdfs.c` is a direct loader clue near the late-DOL file I/O strings

`dvdfs.c` shows up at RAM `0x803DD1C8`. That is a useful naming anchor when working around the retail DVD/file-loading code, especially alongside nearby strings like `SFX.bin` / `SFX.tab`.

## Practical use

- Summary: `python tools/orig/dol_strings.py`
- Search specific anchors:
  - `python tools/orig/dol_strings.py --search camcontrol BLOCKS.bin dvdfs.c`
  - `python tools/orig/dol_strings.py --search PREANIM.bin DLLSIMPO.bin`

The search mode is the part that matters during decomp work. It gives an exact RAM address and DOL offset for each string hit, which makes it easy to pivot into Ghidra, Dolphin symbol work, or nearby recovered functions.
