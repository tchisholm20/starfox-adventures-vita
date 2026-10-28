# `orig/*/files/modules` notes

`orig/GSAE01/files/modules/` contains two tiny leftovers that are easy to ignore:

- `testmod.rel`
- `dino.str`

They are worth keeping around because they give one compact, retail-authored example of the game's REL/module path without needing to guess from the main DOL.

## Tool

- `python tools/orig/module_audit.py`
  - Audits `orig/*/files/modules/testmod.rel` and `dino.str`
  - Confirms whether the module leftovers are region-stable
  - Recovers the import table, relocation streams, and section-like payload layout
  - Extracts the preserved external path from `dino.str`

## High-value findings

### 1. The module leftovers are byte-identical across EN, PAL, and JP

Both files match exactly across `GSAE01`, `GSAP01_rev1`, and `GSAJ01`.

That means any loader or format work done from `testmod.rel` only needs to be solved once.

### 2. `dino.str` preserves an original content path

The only string in `dino.str` is:

- `baddies/testmod.plf`

That is useful because it ties the retail module sample back to a plausible original asset/pipeline name instead of just an extracted disc path.

### 3. `testmod.rel` is a minimal REL testcase, not random junk

The local parser recovers a compact layout:

- one executable payload at file `0xB0`, size `0x9C`
- one data/string payload at file `0x150`, size `0x49`
- one tiny BSS-like section of size `0x1`
- one import table with exactly two streams

This is ideal for loader work because it strips the problem down to the smallest believable retail example.

### 4. The self-relocation stream gives a clean local-address testcase

The module's self-import stream uses `ADDR16_HA` / `ADDR16_LO` relocations against local section data, including the string offsets:

- `+0x04` -> `_prolog() called.  Zero=%d  One=%d`
- `+0x28` -> `epilogue called`
- `+0x3C` -> `Main called`

This is a much cleaner relocation testcase than trying to start from a large unknown module format.

### 5. The main-module import stream is tiny and strongly suggests a debug-print harness

The `module 0` stream consists of:

- one `R_DOLPHIN_SECTION`
- three `R_PPC_REL24`
- one `R_DOLPHIN_END`

Combined with the local strings, that looks like three calls into one external main-module function, which is exactly the shape you'd expect from a tiny test module that just reports when `_prolog`, `epilogue`, and `Main` run.

## Why this helps decomp

- It gives a known-good retail REL sample for recovering the game's module loader path.
- It gives a minimal relocation corpus for validating `R_PPC_REL24`, `ADDR16_HA`, `ADDR16_LO`, `R_DOLPHIN_SECTION`, and `R_DOLPHIN_END` handling.
- It gives one preserved original pipeline path, `baddies/testmod.plf`, that may help if any PLF/module handling turns up in code or assets later.
- It gives a concrete target for naming any retail REL loader support in `main.dol` without having to start from a large content module.

## Usage

- `python tools/orig/module_audit.py`

