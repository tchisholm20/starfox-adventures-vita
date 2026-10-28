# The dead-stripped-code census (measured 2026-08-03)

Code that our translation units emit and the retail link discarded is the one corner of the tree
no score gate can see. `objdiff` pairs our functions against retail functions **by name**, so a body
that is not in `main.dol` has no pair and is never scored; `matched_code` never counts it, the
forced link never notices it, and `obj_equal` compares our objects only with each other. That makes
it a place where fabricated code could sit forever. This file is the census, and
`tools/dead_strip_census.py` re-runs it.

## Population

Comparing `.text` sizes over the 1005 unit pairs (`build/GSAE01/obj` carve vs `build/GSAE01/src`):

| | count |
|---|---|
| unit pairs whose `.text` size differs | 71 |
| ...where OURS is larger | 56 (12 408 B) |
| ...where the CARVE is larger | 15 |
| units carrying whole functions only we have | **49** |
| such functions | **86 (12 348 B)** |

The 15 carve-larger rows are not this class: 2 are unsplit gap symbols (`render.o gap_03`,
`targimpl.o gap_03`) and the other 13 are 4–28 B body-size differences inside already-sub-100 units.

## The three mechanisms, which are not interchangeable

**The surplus is NOT one phenomenon, and the majority of it is not dead code at all.**

| class | mechanism | count | bytes |
|---|---|---|---|
| **A** | **INLINED-AND-STRIPPED** — live code calls it, MWCC inlined every call site and emitted the out-of-line copy anyway, mwld stripped the copy | **50** | **7 332** |
| **B** | **UNCALLED STATIC** — nothing calls it, not even transitively (`UNCALLED_STATIC_FN`) | 27 | 2 228 |
| **C** | **STRIPPED GLOBAL** — not static, and the link dropped it anyway | 9 | 2 788 |

Class C's test is `cls = "C" if not is_static` — **linkage only**. Do not read it as "nothing
references it": that was never measured, and three of the nine are demonstrably called in
**compiled** sources — `__OSBootDol` at `OSExec.c:349`, `__OSBootDolSimple` at `:333`,
`__OSSetExecParams` at `:80` and `:220`.

They sit in **three** units, not two. Six are in `OSExec.o` and two in `synth_seq_queue.o`, whose
carve `.text` is 0: the whole object never entered the link, which is what took their callers with
it. The ninth, `__OSFPRInit`, is in `OS.o` — a **live** unit, carve `.text` `0x95c` — and it has no
caller in anything the build compiles. Excising it alone reproduces `OS.o`'s carve byte for byte,
so its `0x128` is the whole of that unit's surplus.

`src/dolphin/os/__ppc_eabi_init.cpp` is **not** a witness for `__OSFPRInit` and was wrongly cited as
one: `configure.py` builds the sibling `.c`, that `.cpp` is one of the sources the build never
compiles (see `docs/orphan_sources.md` for the full adjudication), and its `__init_hardware`
carries three `bl`s where retail's carries two — `.init:0x80003354`
is `0x20` long and calls `__OSPSInit` and `__OSCacheInit` only, which our compiled
`Runtime.PPCEABI.H/__start.c` reproduces exactly. A file the build does not compile is not evidence
about the link. `tools/source_coverage_audit.py` holds the partition; `dead_strip_census.py`'s
self-test now carries both directions as controls.

Class A is 58% of the population and it is **the normal fate of a small static helper at `-O4`**.
`446.c`'s `lavaball1be_applyDebrisGravity` is the type specimen: it is called at line 167 of its own
file, our object carries its body at `.text+0`, and **our object contains no `bl` to it anywhere** —
every call was inlined. Retail's compiler did the same, which is exactly WHY the carve's `.text`
starts 0x2c later. Reading a `.text` surplus as evidence of invented code is therefore wrong for
most of this population, and a non-transitive call scan gets it wrong in the other direction too:
`curves.c`'s four helpers form a chain (`curveBuildArcSegments` → `curveSolveArcParam` →
`curveIntegrateSpeed` → `curveSpeedAt`) in which only the head is unreferenced, so the inner three
look "called" unless liveness is iterated to a fixpoint. Class B must be taken from
`banned_shapes_check.scan_uncalled_statics`, which does iterate.

## The proof, which is positive rather than an absence of evidence

`dead_strip_census.py excise` removes exactly the stripped functions' byte ranges from our `.text`
and compares what is left with the carve's `.text`.

**34 of the 49 units reproduce the carve BYTE FOR BYTE.** That proves two things at once: the
stripped bodies are the whole of the difference, and every surviving instruction — including the
**inlined copies of the class-A helpers** — is identical to retail's. A fabricated helper would have
to inline into exactly retail's instruction stream. Nothing in those 34 units' `.text` is invented.

All 34 are at `fuzzy 100.0` with `complete: true`; all 15 that fail are below 100.0 and fail on an
ordinary residual elsewhere in the unit, which says nothing about their dead-stripped code either
way. The negative control matters: shifting every excision range by 4 bytes keeps the resulting
length right and the content wrong, and drops the pass count — so the test is not merely comparing
sizes. It is in the self-test.

## Fabrications found: none

The one true-positive shape is **a body that mints nothing and moves no data byte** — `203`'s
`dll_CB_getStateHandler`, deleted in `a21f332847` (see `priced_classes.md` §7). Disassembling every
class-B and class-C body over its own byte range:

**0 of the 36 carry zero relocations.** Every one mints pool literals (`@NNN`) and/or names real
data or callees. None is dead weight. **Nothing was deleted, and nothing should be.**

## What the evidence is, per class

- **A (50 fns)** — the excision byte-match, above. Positive and complete for the 100.0 units.
- **B (27 fns)** — 26 are already adjudicated in `tools/banned_shapes_baseline.txt` under the
  pool-sharing test of `priced_classes.md` §7. The 27th, `synth_jobs.c streamGainFromVolume`, has
  never been adjudicated **because `src/musyx` is an `EXEMPT_ROOT`** — the checker cannot see it.
  Its unit is 100.0/complete and passes excision, and it mints `@2` and `@4`, so it meets the same
  standard the other 26 met.
- **C (9 fns)** — `src/dolphin/os/OSExec.c` is a **line-for-line correspondent of an independent
  GameCube decompilation**: `reference_projects/super_mario_strikers/src/Dolphin/os/OSExec.c` has
  the same 18 functions in the same order, offset by one line. `__OSFPRInit` appears in that same
  project's `OS.c`, as `asm` on both sides. Genuine Nintendo SDK, faithful, keep.

## Genuinely undecidable: 1 unit

`musyx/runtime/synth_seq_queue.c` (0x194 B, `synthInitChannelEventQueues` and
`synthRefreshChannelEventQueue`). Its carve `.text` is **0** — the whole unit is absent from the
DOL — so the excision test compares 0 bytes with 0 bytes and has **no power at all**. Its callees
`GenerateNextTrackEvent` and `InsertGlobalEvent` are real MusyX names, corroborated in the symbol
tables of metroid_prime, marioparty4, super_mario_strikers and super_smash_brothers_melee; but the
two function names themselves appear in **no** independent decomp, and the only reference project
that carries them is an older copy of **this project**, which is circular. Undecidable, and it
should be left alone rather than guessed at in either direction.

`dolphin/os/OSExec.c` is the same shape — carve `.text` 0, no instrument power — but it is decided
anyway, by the reference decomp above. That is the difference between "no gate sees it" and
"nothing can be known about it".

## Two instrument defects this census exposed — and what closing them cost

1. **`banned_shapes_check`'s `UNCALLED_STATIC_FN` is static-only and skips `src/dolphin` and
   `src/musyx`.** Both restrictions are correct for what that check is for, but the consequence is
   that **10 dead functions totalling 2 818 B (the 9 class-C globals plus `streamGainFromVolume`)
   are screened by nothing in the tree.** `dead_strip_census.py` is what covers them.

   **Closed, in one direction only (measured 2026-08-03).** The census now runs over the whole
   tree, so `src/musyx`'s `streamGainFromVolume` is reported — under its own class,
   `UNCALLED_STATIC_FN_SDK`, which never gates, because an exempt root is exempt. The scope
   change takes the census from 26 to **30 rows, of which 27 are functions the link really
   dropped**; the three that are not are SDK `asm` exception vectors (`__OSDBIntegrator`,
   `__OSDBJump`, `OSExceptionVector`), referenced by address at install time. `lost-old=0`, and
   the **gating** hit set is byte-identical before and after at **103**.

   The other direction is **refused, measured**: extending the census to non-static linkage
   yields **4969 rows of which 60 are really dead — 1.2% precision.** A source-text screen has no
   power over a global, because the reference may be a relocation in data we have not
   decompiled, or a DLL export table. For a global the instrument is the *linker*, i.e. this
   file. Do not retry the widening.

   A third, quieter hole closed with it: `is_c_source` accepted only `.c`/`.h`, so
   `src/Runtime.PPCEABI.H/__init_cpp_exceptions.cpp` — the one compiled `.cpp` — was invisible to
   **every** scanner in the tree. It lives in an exempt root, so the game-code gate count did not
   move. **Corrected:** that hole was reported as *two* compiled C++ units, and
   `src/dolphin/os/__ppc_eabi_init.cpp` is not one — the build never compiles it. The measured
   partition at the pristine tree is 1005 compiled sources (1002 `.c`, 1 `.cpp`, 2 `.s`) against
   1070 on disk, leaving 65 the build never names — of which three (the `snd3d` trio) are still
   LIVE, `#include`d by compiled `snd3dgroup.c`, and 62 are dead (the earlier 66-of-1071 count was
   taken in a worktree carrying one stray probe `.c`). Scanners now select their sources from
   `live_sources()` rather than by extension, so the dead files are outside every screen's
   population; the two assembled `.s` units stay outside C-shape screens by design. All the dead
   files are outside `SCAN_ROOTS`, so the gating population of the whole file-type gap is **0**.
   See `tools/source_coverage_audit.py` and `docs/orphan_sources.md`.

2. **Five units report `fuzzy 100.0` and `complete: true` with no code, no data and no functions**
   — `dolphin/ax/AX`, `dolphin/TRK_MINNOW_DOLPHIN/MWCriticalSection_gc`, `dolphin/os/OSExec`,
   `musyx/runtime/synth_sequence`, `musyx/runtime/synth_seq_queue`. Their `total_code`,
   `total_data` and `total_functions` are all absent from `report.json`. **So 5 of the 915
   `complete_units` are vacuous**, and a 100.0 on those five rows is not a statement about
   correctness. Two of them (`OSExec`, `synth_seq_queue`) are exactly the units whose whole `.text`
   was dead-stripped, which is why the vacuity and the census meet here.

   **Decided, and made controlled (2026-08-03).** All five carry a **zero-length `.text` range**
   in `config/GSAE01/splits.txt` (`start:0xNNN end:0xNNN`) and all five carve objects really are
   `.text` size 0 — they are TU boundary markers. Three (`AX.c`, `MWCriticalSection_gc.c`,
   `synth_sequence.c`) are stub sources that emit nothing at all; two (`OSExec.c` 0x1074,
   `synth_seq_queue.c` 0x194) emit real code that never entered the link. Either way the carve
   can never gain content, so such a row can neither be earned nor lost: it is excluded from
   **both** halves of the completion figure, not just the numerator. Separately, the 38
   `main/auto_*` units are selected by `total_data` present / `matched_data` absent / no
   `total_code` (2 342 B) — **never** by a missing `complete_units` key — and **0 of them are
   inside `complete_units`**. The exclusion is not folklore: `vacuity_audit.py --family report`
   prints both figures and carries the controls (the predicate must fire, must stay silent on the
   other 1038 units, must be disjoint from the auto-generated ones, and every row it names must
   really be complete). **Informative completion: 910 of 1000.**

## What this closes, and what it does not

It closes the question the surplus `.text` raised: **no fabricated function is hiding there.**
It does not make the class self-policing — a future lane can still add an uncalled static, and
`banned_shapes_check` reports one anywhere in the tree (gating inside `SCAN_ROOTS`, informational
in the exempt roots) while this tool reports it for any linkage. Run
`dead_strip_census.py census` when a unit's `.text` grows without its score moving.

**And do not delete what it finds.** The standing registry entry is `docs/priced_classes.md`
§7b: a class-A body has live call sites and deleting it changes what those sites inline; the
deletion is invisible to every score gate in the tree.
