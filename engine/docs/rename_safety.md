# Renaming a function safely

A pure rename can silently destroy a unit's score with a completely green build.
Five game-text renames once produced `locked_ninja.sh` EXIT=0, 0 FAILED, and:

```
textrender_run.c     99.40815 -> 96.46698
textrender_drawbox.c 99.95742 -> 93.76387
gametext_tail.c      99.06977 -> 55.46511
```

## The mechanism

1. **`config/GSAE01/symbols.txt` is build input.** `configure.py` lists it in
   `config.split_deps`, and the split step is
   `build/tools/dtk dol split config/GSAE01/config.yml build/GSAE01`.
   Editing it re-runs the splitter, which **re-carves the whole retail tree
   under `build/GSAE01/obj/`** with the new names. (CLAUDE.md's "retail objs are
   READ-ONLY" is about *manual* interference — the splitter owns them.)
   Note the carve is a *side effect*: ninja's only declared output for that rule
   is `config.json`, so the `.o` files are untracked and there is no dependency
   edge that would catch an inconsistency.

2. **objdiff pairs functions by symbol name.** If our object and the retail
   carve disagree, the pair is lost. A **retail-only** symbol scores **zero**;
   an **ours-only** symbol is simply *not scored* (harmless -- that is what a
   `static` helper retail inlined looks like, and 65 units are in that state
   today, all benign).

3. **objdiff also compares relocation target names.** So a name mismatch is not
   confined to the defining unit: it shows as a diff at **every call site in
   every calling unit**. That is what turns a one-function slip into a
   three-unit collapse.

So the failure is a *partial* rename -- `symbols.txt` and the C source
disagreeing, or agreeing in the defining unit while some other unit still
carries the old name. The build stays green because nothing about *linking* is
wrong.

## Why two functions were safe and five were not

Blast radius, and it is measurable:

```
$ python3 tools/pairing_check.py --refs gameTextLoadCompleteCallback
'gameTextLoadCompleteCallback' is referenced by 1 unit(s):
      6 reloc(s)  main/textrender_run.c

$ python3 tools/pairing_check.py --refs gameTextRun
'gameTextRun' is referenced by 10 unit(s):
     12 reloc(s)  main/pi_dolphin.c
      3 reloc(s)  main/gameloop_main.c
      ... 8 more
```

The safe commit renamed two leaf callbacks with a **one-unit** radius. The
failing one included a widely-declared API function with a **ten-unit** radius,
and only the edited unit was checked.


## The stale-object race — the step everyone misses

`dtk dol split` re-carves the retail tree, but **the compile of your edited
source can land on the wrong side of that inside a single `locked_ninja.sh`
invocation**. The checks below then read a stale `.o` and report a fake
regression on a file that is already correct.

This is the same hazard as above wearing a different hat: ninja's only declared
output for the split rule is `config.json`, so the regenerated `.o` files have
**no dependency edge**. The build graph therefore cannot order the re-carve and
the recompile against each other — which means the `touch` below is not a
workaround, it is the required discipline.

`touch config/GSAE01/symbols.txt` plus a full rebuild is **not** sufficient: it
re-runs the split over ~1054 objects and still leaves our object behind. Only
touching the *source* and rebuilding that object **by name** refreshes it.

### A retail-only symbol has two causes, and they need opposite fixes

`pairing_check.py` now labels which one it is, so nobody "repairs" a correct file:

- **`[STALE OBJECT]`** — the name *is* present in `src/` or `include/`, so the
  source is already right and only our object is behind.
  **Fix: touch the source, rebuild that object by name. Do not edit the source.**
- **`[partial rename]`** — the name is nowhere in `src/` or `include/`, so
  `symbols.txt` and the source genuinely disagree. **Fix: rename in the C source.**

The manual cross-check is the complement: grep `src`/`include` for the **old**
name — zero hits means the source is fully renamed, so it is staleness.

### Measured, live

`main/textrender_drawbox.c` was found in exactly this state while writing this
section: `subtitleInit` retail-only and scoring **0.000** on 192 B, the unit at
**93.76387**, and zero references to the old `gameTextInitFn_8001bd14` anywhere
in `src`/`include`. No source was wrong. `touch src/main/textrender_drawbox.c`
plus a rebuild of that one object restored it to **99.95742**.

## The gate

Renames are **not** byte-neutral and `tools/byteneutral.py` cannot gate them
(the md5 must change). Before committing any rename:

1. `python3 tools/pairing_check.py --refs <oldname>` — get the blast radius.
2. Rename in **both** `config/GSAE01/symbols.txt` and every C occurrence — **and check
   `config/GSAE01/config.yml`**, which is a third home for a symbol name. Its `FORCEACTIVE` list
   keeps the link from dead-stripping symbols nothing references (unreferenced `.sbss` words,
   uncalled functions retained by the retail link). A name left behind there does **not** fail the
   build: `mwld` prints `FORCEACTIVE symbol 'X' is either not a global symbol or doesn't exist.
   Ignored.` and carries on. Measured live renaming `lbl_803DCFFC` → `sIntersectUnused0`
   (`90cabeaf76`): the tree's baseline two FORCEACTIVE warnings became three, and renaming the
   `config.yml` entry in the same commit returned it to two. Count the warnings before and after —
   a new one is a partial rename that the gate will not otherwise catch.
   ⚠️ **A missing warning does not mean FORCEACTIVE works.** `mwld` resolves FORCEACTIVE only
   through the external symbol table, so an entry naming a `static` is **silently inert** — no
   warning, and no effect either. `90cabeaf76` shipped in exactly that state: the entry was renamed
   to `sIntersectUnused0` while the source still said `static u32 sIntersectUnused0;`, the warning
   count returned to two, and the symbol was in fact unreachable until `1de25bbba7` removed the
   `static`. If a FORCEACTIVE entry is meant to keep an atom alive, check the object's binding
   (`objdump -t`, `g` not `l`) — the warning count alone cannot tell you.
3. `tools/locked_ninja.sh` as its **own step**; read EXIT before continuing.
   ⚠️ Then rebuild **every touched object by name** —
   `tools/locked_ninja.sh build/GSAE01/src/<...>.o`. Because the re-carve is an
   undeclared side effect (see above), **ninja has no dependency edge telling it
   our objects are stale relative to the retail side**, so it will happily leave
   them alone: the splitter re-carves retail with the new name while your own
   object still holds the old one. That is a *complete, correct* rename
   measuring as a total collapse.

   This is NOT only a DLL problem — DLL objects are additionally missing from
   the default target graph, but the staleness bites anywhere. Observed live on
   `main/shader.c` after someone else's rename: `pairing_check.py` reported four
   retail-only symbols and the unit read **90.49737**; a single
   `locked_ninja.sh build/GSAE01/src/main/shader.o` restored it to **99.43728**
   and the tree-wide count returned to the expected 2. Nothing was ever wrong
   with that rename.

   Measured four ways on `Link_refreshOverlappingItemTimers` in `dlls/engine/60`:

   | state | unitfuzzy |
   |---|---|
   | clean baseline | 99.88345 |
   | C source only, `.o` rebuilt by name | 91.72495 |
   | `symbols.txt` only | 91.72495 |
   | **both renamed, bare `locked_ninja.sh`** | **91.72495** ← stale `.o`, artifact |
   | both renamed, every `.o` rebuilt by name | 99.88345 |

   Only the last row is the truth. Do not revert on the fourth row.
4. `python3 tools/pairing_check.py` — must report **0 retail-only** symbols.
   (Two are expected and permanent: `gap_03_80006C6C_text` in `main/render.c`
   and `gap_03_8028C964_text` in `targimpl.c` — foreign-toolchain blobs.)
5. `python3 tools/unitfuzzy.py <unit>` on **every unit from step 1**, not just
   the one you edited.
6. `git update-index --refresh` **first**, then **commit the ENTIRE dirty tree,
   not just your files** — and name in the message which batches you carried
   that were not yours. The refresh matters: without it, stat-dirty entries
   (first row of the table below) look exactly like foreign work waiting to be
   carried, and you will write a commit message describing batches that do not
   exist.

### Why step 6 commits foreign work on purpose

Three lanes share one `config/GSAE01/symbols.txt`. "Commit only your own files"
deadlocks: your rename needs its `symbols.txt` line, that line sits beside
someone else's, and committing source without the line leaves the retail carve
holding a name your source no longer defines — the exact retail-only failure
this document exists to prevent. Waiting does not help either: a foreign batch
can land *during* your ~2-minute gate, which happened three times before the
rule changed.

**The gate is the safety check.** A half-applied foreign batch either fails the
build or shows up as a retail-only symbol, so a green gate means every dirty
batch in the tree is internally consistent and safe to snapshot together. Do not
reason about whether a peer's batch looks finished — the gate already answered.

Still forbidden: reverting or stashing a peer's work, and committing on a red
gate.

## Three ways this tree lies to you about state

All three look like damage and none of them is. Check in this order — they are
ordered by how cheap they are to rule out.

| symptom | real cause | fix |
|---|---|---|
| `git status` says `M`, but `git diff` on the file is **empty** | **stat-dirty index** — our own step-3 `touch` commands updated mtimes without changing content. Can also make `git merge` refuse with "local changes would be overwritten". | `git update-index --refresh`. Nothing to investigate. |
| `pairing_check` reports a retail-only symbol **and the name IS in `src/`/`include/`** | **stale object** — the source is already correct, only our `.o` is behind. Labelled `[STALE OBJECT]` by the tool. | `touch` the source, rebuild that object **by name**. Do **not** edit the source. |
| `pairing_check` reports a retail-only symbol **and the name is nowhere in `src/`/`include/`** | **genuine partial rename** — `symbols.txt` and the source really disagree. Labelled `[partial rename]`. | Rename in the C source to match. |
| `pairing_check` reports N retail-only, then 0 on a re-run, with nothing changed | **torn read** — the scan sampled the object tree while a peer lane was rebuilding into it. **No cheap tell; a single reading is not evidence.** | Fixed at source: the tool now takes the same directory mutex as `locked_ninja.sh` and re-reads to confirm any non-zero verdict, printing `TORN READ` (exit 2) if two locked scans disagree. If you see that, something outside `locked_ninja.sh` is writing `build/GSAE01/`. |

A corollary of the middle row: a unit can carry an **understated** score for a
long time without any symptom at all. `main/objprint_dolphin.c` read 99.78250
for an unknown period and jumped to **99.88340** the moment an unrelated rename
forced a by-name rebuild — the rename could not have changed codegen. So a
by-name rebuild sweep is worth doing as post-merge hygiene, not only after a
rename.

## Measuring a true baseline

A green full `locked_ninja.sh` does **not** mean your objects are current — see
the stale-object race. So a baseline measured right after one can be fiction.
**Rebuild the units you are about to measure by name, then measure.** Two
independent observations of the same corollary: `main/objprint_dolphin.c` read
99.78250 for an unknown period and jumped to 99.88340 on the first by-name
rebuild, and an `engine/2` baseline taken straight after a green build showed a
phantom +0.06 that was actually a peer's already-committed fix (95.737 →
99.211, matching their commit message).

**Verify the rebuild list actually ran, and count it.** `xargs` over a units
list dies on entries with no ninja target (e.g. `dolphin/os/__start.c`) and
**silently truncates the rest** — which reproduces exactly the staleness step 4
exists to prevent. Non-empty output is not sufficient: check the rebuilt count
matches the number of objects you expected.

## Two side effects of the whole-tree carry rule

- **An empty commit is normal.** If the owning lane commits a batch inside your
  gate window, your carry commit can come out with nothing in it. Harmless.
- **Never mutate a tracked file for a probe.** Anything dirty when a peer gates
  gets swept into *their* commit. A temporary rename probe of mine
  (`objFuzzSetupGxState` → `…ZZ`, reverted seconds later) was carried into
  `88b2013d53`, landing a genuine partial rename in HEAD: source said `…ZZ`
  while `symbols.txt` said the real name. Repaired in `805619805e`. Probe on
  copies under the scratchpad, or in a file no build consumes.

## Safe subset

Lowest risk to highest:

- **`static` function with no `symbols.txt` entry** — our object only; the
  retail carve never named it. Renaming cannot unpair anything.
- **Leaf function with a 1-unit blast radius** — check that one unit.
- **Anything referenced by >1 unit** — check all of them, or don't rename it.
