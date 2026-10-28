# Semantic frontier stocktake (C112, 2026-08-05, measured at ac839ff83a, re-gated and landed at 42bf6185fb)

Re-derivation of the semantic-recovery frontier from scratch with the corrected
instruments: population = `build.ninja` via `source_coverage_audit.live_sources()`
(1008 live files; `live_sources()` returns ABSOLUTE paths -- relativise before
membership tests, a repo-relative probe reads every file as DEAD); omitted-zero
rule; comment-stripped scans. Every population below states its filter.

## Verdict up front

At GLOBAL scope, machine-name recovery in GSAE01 game code is EXHAUSTED outside
the const lane's `lbl_<hex>` world. The live semantic frontier is (1) `lbl_<hex>`
data naming (const lane, 8354 rows), (2) cross-version `fn_<addr>` rows in the
NON-default symtabs (146 rows, needs the other DOLs), and (3) owner-hot pun and
cast families already priced by C101-104. Everything else measured here is either
zero, vendor-unnameable, or closed with the measurement recorded.

## (a) Machine-named FILE-SCOPE globals

* `lbl_<hex>`: 8354 in `config/GSAE01/symbols.txt`; 584 distinct referenced from
  src; 1717 occurrences across 111 files (junk_names kind=addr). CONST LANE'S --
  excluded by charter.
* Non-`lbl_` address-suffixed globals in GSAE01 symbols: **5 total** (filter:
  name ends `_80[0-9A-Fa-f]{5,}`, minus `@etb/@eti` extab and `jumptable_`).
  - `DSPInitCode_8032C520` -> **renamed to `DSPInitCode` this window.** Oracle:
    library-content rule -- MKDD and Strikers SDK sources both spell
    `static u8 DSPInitCode[128]` in OSAudioSystem.c. Two-sided rename (OS.c
    definition, OSAudioSystem.c 5 uses, all four version symtabs). Gated:
    unitfuzzy 100.0 on both units before and after, pairing_check 0 retail-only.
  - `SaveStart_803DEAD0`, `SaveEnd_803DEAD4`, `Prepared_803DEAD8`,
    `init_803DF080`: `.sdata2` float-pool entries, symtab-only (no src
    occurrence). `.sdata2` mint order is A-lane/owner-hot -- NOT ours; left.
* Unknown-vocabulary AT_ADDRESS globals (vendor SDK, all in `src/dolphin/`):
  `UNK_817FFFF8`, `UNK_817FFFFC` (OSReboot), `g_unk_800030E2` (OSExec),
  `__gUnknown800030E3`, `__gUnkThread1` (__os.h). References DIVERGE
  (pikmin invents `OS_UNK_CODE`/`OS_HOT_RESET_CODE`, prime `UNK_HOT_RESET2`,
  five others keep `UNK_*`) or share the identical placeholder -- no SDK name
  ever leaked. Another decomp's guess is not an oracle => **unnameable, DECLINED**.
* `Debug_BBA_8032EFE0` exists ONLY in the never-compiled orphan
  `src/dolphin/os/__start.c` (VENDOR keep; the live startup TU is
  `src/Runtime.PPCEABI.H/__start.c`). A never-compiled file proves nothing; no
  GSAE01 symtab row exists (retail address sits inside `lbl_8032EDD0`,
  .data 0x280 -- const lane's blob). If that file ever goes live the SDK name is
  `Debug_BBA` (7-reference consensus). No action.
* Ghidra vocabulary kinds var/stack/param/func (junk_names): **0 files** -- the
  C79/80 closure holds.

## (b) Untyped casts

Filter: primitive deref-casts `*(int|u32|s32|u16|s16|u8|s8|f32|f64|float|short|char *)`
in live game code (vendor `src/dolphin`, `src/musyx`, Runtime, TRK excluded),
comments stripped.

* Total: **2639 occurrences in 242 files**.
* `*(T*)&expr` launder subclass: **849** -- the load-bearing pun family
  (`docs/load_bearing_puns.md`, scope = the function). Do not respell blind.
* In owner-hot files (player.c, 202/*, model.c, 597.c, object.c, engine/21,
  objprint.c, objanim, sharpclaw, shadow_dolphin, track_dolphin): **1049**.
* Raw-offset subclass `*(T*)(p+N)`: adjudicated by C101-104 -- FIXED offsets
  respell free, INDEXED never; scale audit 0/275 proved a scale error is never
  byte-identical. The prior bare `(int*)`/`(void*)` no-named-type remainder
  (~11+20 rows) stands adjudicated no-majority; today's broader instrument
  surfaced **no new free rows**. This class is a per-row byte-identity-probe
  lane, not a sweep lane.

## (c) `fn_<addr>` functions

* GSAE01: **EMPTY, re-verified twice** (symbols.txt 0 rows; src/include 0
  identifiers; junk_names kind=func 0 files).
* **PREMISE REFUTED: the census was version-scoped.** The non-default symtabs
  still carry fn_ rows: `GSAJ01` **18**, `GSAE01_rev1` **45**, `GSAP01_rev1` **83**,
  `GSAP01` **90** (236 total). Sampling shows the same functions at shifted addresses across
  versions (matching size runs 0x4,0x4,0xC,0xC,0x104...). Naming them needs
  cross-version content correlation against those versions' DOLs (not present in
  a standard worktree -- only `orig/GSAE01/sys/main.dol` is seeded). Size-run
  correlation alone is a guess; DECLINED this window. Ownership: free for a lane
  that stages the other DOLs.
* One prose comment in `src/dlls/objects/202/sharpclaw.c:179` still speaks
  `fn_8014*` vocabulary -- stale, but the file is owner-hot (202 lane); flagged,
  not fixed.

## (d) dup_prototypes -- the 5 rows, adjudicated

Ran at parent: `1 duplicate + 4 signature mismatches`. After adjudication:
**0 / 0**.

1. `snd_midictrl.c:30 u8 inpTranslateExCtrl(u8);` -- true fossil duplicate of
   `include/musyx/inp_ctrl.h:20` (header included at line 7). **Pruned** via the
   tool's own gate (compile_md5 byte-identical). Survivor of the afede8b95e
   138-row campaign, not a protected musyx row (unrelated to the CC96 pin).
2. + 3. `OSLink.c:4/5` -- **instrument false positives**: the PROTO regex read
   `OSModuleQueue __OSModuleInfoList AT_ADDRESS(OS_BASE_CACHED|0x30C8);` as a
   prototype NAMED `AT_ADDRESS` and diffed it against every other AT_ADDRESS
   variable in the headers. Fixed in `tools/dup_prototypes.py` (skip
   name==AT_ADDRESS).
4. + 5. `OSSync.c:6/7 void __OSSystemCallVector{Start,End}(void)` vs
   `__os.h extern void ...()` -- two stacked instrument gaps, then a benign row:
   (i) `norm_type` folded the storage class `extern` into the return type
   (fixed: strip it); (ii) a zero-arg `(void)` prototype vs an unspecified `()`
   declaration is C89-compatible with no codegen difference, but NOT prunable
   (deleting the local copy would weaken the visible declaration). The tool now
   classes this pair `[benign]`, outside both counts. References split on the
   spelling (MP4/melee/sunshine/AC `()`, TP `(void)`; the header `extern ...()`
   IS the attested SDK __os.h text) -- no oracle favours changing either side;
   source left untouched.

Historical prior confirmed: cross-TU signature conflict remains 0-for-free; the
only landed change in this class was the duplicate deletion (byte-identical) --
the mismatch rows were all noise or benign.

## (e) What the routed screens surface today

* `unused_externs src`: **0**.
* `dangling_extern_check`: **42**, mechanically fixable **0**. Composition:
  linker-defined LCF symbols (`_e_*`, `_f*`, `_heap_addr/_end`,
  `__copy_rom_section`, `__init_bss_section`, `_e_PPC_EMB_*`) referenced from
  __start/OS, plus 4 SDK callees on dead-stripped OSExec paths (`DVDResume`,
  `OSAllocFromArenaLo`, `OSGetSaveRegion`, `__OSShutdownDevices`). Structural
  steady state -- do not re-run as a work queue.
* `proto_screen`: 91 implicit-declaration call sites, 79 callees, 26 units;
  printed sites concentrate in vendor TRK (6) and musyx (5) plus DLL legacy
  sfx-API spellings -- the `(int)obj` sfx family is the C103-closed closure, and
  `Sfx_PlayFromObject`'s dual header signature is the priced C70/71 cross-TU
  conflict class. No free rows; any fix is per-site and must respect A81 (a new
  `#include` is free only if the header emits no object) with a byte-identity
  probe.
* `pairing_check`: 0 retail-only; 47 ours-only (benign statics); the 2 expected
  foreign blobs.
* `junk_names`: addr kind only (see (a)); all Ghidra kinds 0.

## Evidence standards (unchanged, restated for the next ten lanes)

RETAIL STRINGS in main.dol = the only Rare names; the evidence hierarchy is
STRINGS > struct close > enum > mask > callee sig (C22-42/A34-42 lane files
carry the case law). A reference decomp is an
oracle ONLY for a vendor library file's real content, and only where independent
references CONVERGE (DSPInitCode: yes; UNK_817FFFF8: no). ONE WRITER + ZERO
READERS = A GUESS; NO MAJORITY => DECLINE; renames are two-sided
(src + EVERY version symtab, one commit) and gated with pairing_check +
unitfuzzy on the full blast radius, never byteneutral.py alone.
