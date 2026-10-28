# The version-parity frontier — seven units, one function each

`complete_code_percent` for the non-EN versions is gated by exactly seven shared units, and in every
one of them a **single function** is unmatched. Closing all seven closes the whole gap:
`tools/version_progress.py GSAP01` plus the per-unit rows give **+3.883 pts**, and the measured gap is
3.883 (92.7016 -> 96.5895). The same seven units gate `GSAE01_rev1` and `GSAP01_rev1`.

Read this before spending builds on any of them: each row's refutation list is measured, not guessed.

| unit | pts | function | shape | PAL GPR band |
| --- | --- | --- | --- | --- |
| `dlls/engine/0/0.c` | +2.620 | `pauseMenuDraw` | 1 web on the wrong home | 6 |
| `dlls/objects/653_WCLevelCont` | +0.295 | `wclevelcont_update` | `f0`/`f1` scratch pair | 2 |
| `main/gameloop.c` | +0.248 | `askProgressiveScanMode` | band rotation | 6 (EN 4) |
| `dlls/engine/2/maketex.c` | +0.236 | `loadMemCardImages` | 2 instructions too long | 1 |
| `dlls/engine/53/53.c` | +0.229 | `SaveSelectScreen_render` | band rotation | 9 |
| `dlls/objects/589_BossDrakor` | +0.222 | `bossdrakor_update` | band rotation | 7 |
| `dlls/objects/611_GM_MazeWell` | +0.034 | `GM_MazeWell_update` | `r28`/`r29` exchange | 4 |

Two arithmetic checks worth keeping, because they prove the "one function" claim rather than assuming
it: engine/0 is `75304 - 70652 = 4652 = 1163 x 4`, and engine/53 is `6640 - 5616 = 1024 = 256 x 4`.

## What landed

`pauseMenuDraw` went **19 -> 4** positional diffs on all three lagging versions by deleting two
reconstructed block temps. The token prompt arm measured each line into
`{ s32 tokenLineHeight = tokenBottom - tokenTop; tokenTextY = tokenLineHeight + tokenTextY; }`
before a separate `tokenTextY += K;`. Two of the four advances in the same arm already used the direct
`tokenTextY += (tokenBottom - tokenTop) + K;` form, so all four now use it. The temps had been
introduced to force the `add rX,r0,rX` operand order; the direct form produces that order by itself.
Their real cost was a **live-range split**: with the temps, PAL put the case 1 accumulator in `r26`
and then moved it to `r28` at the first advance, where retail (and EN) keep it in one register for the
whole arm. EN/JP stay byte-identical, and the source is a consistent single idiom.

This is lever 16 of `source_shape_levers.md` (two reconstructed locals where retail had one) firing on
a band the width screen calls hopeless. **A wide flat band does not mean the function is capped — it
means the defect is upstream of allocation.**

## Refutations, per unit

### `wclevelcont_update` has NO FPR colouring graph — that row is not a colouring row at all

```sh
python3 tools/tricky_backend_trace.py --unit main/dlls/objects/653_WCLevelCont/WCLevelCont         --function wclevelcont_update --graph --register-class fpr
# ValueError: missing requested register class in the graph capture
```

MWCC never builds an FPR interference graph for this function. Its `f0`/`f1` pair is therefore assigned
by the code generator's scratch handling, not by graph colouring — so there is no graph to influence and
**no source lever can reach it**, which finally explains why all ~40 spellings, the twelve `-opt`
profiles, the whole non-`-opt` flag space, the declaration forms, the expression sweep and the helper
boundary were every one of them exactly inert at 4. It also matches `priced_classes.md` §31e: "a
declaration never touches `f0`-`f13`", measured over 8 085 differing FPR operands with 0 volatile.

Do not spend further probes on this row. The GPR side of the function already matches.

### `bossdrakor_update`: a real gradient, 150 -> 70, then a hard plateau

The alias merge plus a reordered declaration list takes PAL from **150 to 70** positional diffs — the
largest movement found anywhere on this frontier. The merge half is byte-identical in all five versions
and is landed. The reorder half is not: it needs `state` declared late, EN wants it early (EN 0 -> 94),
and the behaviour is **binary** rather than graded — every position from 2 to 14 gives exactly
(EN 94, PAL 70) and positions 0-1 give (EN 0, PAL 150), with nothing in between. A randomised multi-move
climb over all 15 declaration items, PAL-only, found 70 in three seconds and never beat it.

The trace says why the row is close: `obj` (deg 105), `drakorState` (102) and `state` (100) are nearly
tied, so their colouring order turns on a 3-5 point degree margin rather than the 13-point gap
`pauseMenuDraw` needs — which is what makes any movement possible at all.

And the residual 70 is **one exchange, not scattered noise**: the substitution census is
`r31->r29` x51, `r29->r31` x14, `r29->r30` x2, `r30->r31` x5, so 65 of the 70 are the single
`obj` <-> `state` swap. Ours gives `obj` 29 and `state` 31; retail gives `obj` 31. `obj` is coloured
first because it has the highest degree, so this is again a *pick* inside a large free set rather than a
constraint, and moving it two positions along needs two more nodes coloured before it.

### And `obj` is a PARAMETER, so its node index cannot be raised — BossDrakor is closed too

After the alias merge the degrees are `obj` **104** and `state` **102** — a two-point margin, the
tightest anywhere here. But the colouring order is `[164, 40, 39, 32]`: descending node index, so
`state` (node 40) colours before `obj` (node 32), and retail needs the reverse. Parameters get low node
indices and every local sits above them, so no declaration position can put `obj` first. That is exactly
why the position sweep is binary — 150 or 70, never anything between.

The obvious escape is to copy the parameter into a local whose position *is* controllable
(`GameObject* self = obj;` used throughout). Swept over all 16 positions: **PAL 150 -> 136 at positions
0-2 only** (EN 0 -> 65), and 150 everywhere else. Binary again, and never 0 — the copy coalesces with the
parameter and inherits its index constraint rather than getting a fresh one.

**So a parameter's effective node index is not source-controllable**, which closes this row: the single
`obj` <-> `state` exchange that is 65 of its 70 diffs requires an ordering the front end will not produce.

### Why no node-count lever exists in these functions — the contradiction, stated

Adding a graph node is the one mechanism that reorders colouring (Jack's `modelDoRenderInstrs`: 256 ->
257). Three measurements pin down what it takes and why it is unavailable here:

1. **A value defined and immediately consumed gets no web.** Confirmed three independent ways — a
   `fontId` temp (294 nodes before and after, only renumbered), GM_MazeWell's hoisted
   `isItemBeingUsed` result, and the thrice-used `0xc0 - shadeReduction` subexpression that folds away on
   EN but not PAL. All inert.
2. **A node therefore needs a value that genuinely SURVIVES something** — Jack's joint matrix had to
   survive the second argument's setup.
3. **But every surviving value in these functions is already a web**, and promoting one that is *not*
   already a web changes the stream. The cleanest test: `taskTextIds[gPauseMenuTokenIndex * 4]` is the
   first argument to `gameTextMeasureById(..., 0, 0, &a, &b, &c, &d)`, so it survives four address
   setups — a perfect candidate. Retail loads it **twice** (two `lhax`), so hoisting it into one local
   costs 4 instructions on *both* versions (EN and PAL both 10004).

So the requirement "adds a web" and the requirement "leaves the stream identical" are in direct conflict
in these bodies: anything that survives is already counted, and anything not counted does not survive.
That is the structural reason the frontier is closed, and it is the thing to re-test first if a future
change alters one of these functions' streams for an unrelated reason.

### The graph is FORCED by the code — so every one of these rows is a selection difference

This is the load-bearing result, and it is a theorem the trace then confirms. The interference graph is
*derived* from liveness, and liveness is derived from the instruction stream. All five blockers have
**byte-identical streams**, so our interference graph and retail's are the same graph. Retail's register
assignment is therefore *necessarily* a valid colouring of our graph, and no amount of live-range work
can be the fix.

Measured confirmation on `askProgressiveScanMode`, reading retail's colours off the asm
(`counter` 29, `sel` 28, `savedAlignment` 31, `i` 30, `j` 31, `showId` 30, `messageY` 27) and checking
every edge among those nodes in our own graph: **0 conflicts**. The edges are real
(`messageY`-`savedAlignment`-`sel`-`counter` form a clique, `j`-`showId` an edge) and retail's colours
respect all of them. Same story on `pauseMenuDraw`: colour 29 is free for the case-2 node.

**Beware the naive per-node test.** Asking "is retail's colour for node X free, given OUR colours for
everything else" reports FALSE for `messageY` (blocked by `sel`) and `showId` (blocked by `j`) and looks
like a live-range defect. It is not — those neighbours also move under retail's assignment. Always test
retail's assignment as a whole.

So the lever is the **colour-selection order** and nothing else. That order is set by the node count,
the degrees, and the simplification worklist — i.e. by how many webs the FRONT END created, which can
differ between two sources that emit identical code (a coalesced copy is a web the allocator counted and
the code never shows). That is exactly why Jack's `modelDoRenderInstrs` fix is described as the graph
growing 256 -> 257, and it is why liveness-shaped reasoning has been barren here.

The practical corollary, measured twice: **naming a value does not create a web.** Adding a `fontId`
temp left the graph at 294 nodes; hoisting GM_MazeWell's `isItemBeingUsed` result changed nothing. A
value that is defined and immediately consumed coalesces straight back. The joint-matrix case added a
node because its value had to survive the second argument's setup. So a node-count lever needs a value
that genuinely *survives* something, and finding one that also leaves the stream intact is the open
problem on this frontier.

### The allocator graph is directly observable — use `tools/tricky_backend_trace.py` FIRST

This is the tool for this whole class and it turns blind sweeping into a measurement. It intercepts a
private GC/1.3 process's dump hook, verifies the instrumented object is byte-identical to an ordinary
compile, and replays simplification and physical colouring:

```sh
python3 configure.py --version GSAP01          # it traces the CONFIGURED version
python3 tools/tricky_backend_trace.py --unit main/dlls/engine/0/0         --function pauseMenuDraw --graph --output build/pmd_trace
```

`snapshots[15]` carries `register_objects`, which maps **source variable names to graph node indices**,
plus `coloring_graph` with each node's `neighbors`; `snapshots[16]` carries the same graph after
colouring. Each node's `prefix` decodes as `[…, node_index, order, colour, flags, degree]`, and the
colour **is** the physical register number.

### `pauseMenuDraw`'s 4 diffs, diagnosed at the graph

| node | source local | degree | interference (>=r14) | our colour | retail wants |
| --- | --- | --- | --- | --- | --- |
| 36 | `tokenTextY` (case 1) | 33 | `{28, 31}` | 30 | 30 (already right) |
| 35 | `tokenTextY` (case 2) | 21 | `{28, 31}` | **26** | **29** |

Three facts fall straight out, and together they replace the guesswork above:

1. **Retail's assignment is legal in our graph.** Node 35 interferes with *nothing* in the saved band
   except colours 28 and 31 — not with `boxDrawParamA/B/C` (nodes 32/33/34), `player`, `statusTable`,
   `taskTextIds` or node 36. Colour 29 is free for it. So this is a colour *preference*, not a
   constraint, and no amount of live-range work is needed to "make room".
2. **The two nodes have IDENTICAL constraint sets** — same neighbours-in-band, same free set — so the
   colour is decided purely by their position in the colouring sequence. The tool prints that sequence:
   `[76, 53, 44, 39, 36, 293, 292, 291, …]`, node 36 fifth. Ours colours 36 first (takes 30) then 35
   (which wraps past the taken 31 to **26**). Retail colours **35 first** (takes 29) then 36 (30).
3. **So the target is exact: reverse the colouring order of nodes 35 and 36.** Simplification removed
   every node as low-degree ("0 high-degree removals"), so the stack order is the removal order and the
   lower-degree node is coloured last. Reversing it needs **deg(35) > deg(36)**, i.e. 21 must exceed 33.

That is a 13-interference swing, which is why every spelling-level perturbation is flat: none of them
changes a degree by anything like that. Splitting case 1's accumulator to lower node 36's degree was
tried at all three advance points and breaks EN (14/11/6 diffs) without reaching it.

**Renaming does not add a node — verified twice.** Jack's `modelDoRenderInstrs` fix worked because its
graph grew 256 -> 257. Re-tracing after adding a `fontId` temp to the PAL-only statement: still **294
nodes**, only the indices renumbered (`[77, 54, 45, 40, 36, …]`), node 35 still colour 26. Same for
hoisting GM_MazeWell's `isItemBeingUsed` result into a local: EN and PAL both unchanged. And a third,
strongest case: in `askProgressiveScanMode` the subexpression `0xc0 - shadeReduction` appears **three
times** in one call and CSE already computes it once, and on EN it folds away entirely because
`shadeReduction` is a `const int = 0` there — so hoisting it into a local looked like a
version-asymmetric, stream-neutral way to add a node on PAL alone. Swept over all ten declaration
positions: EN 0 and PAL 25 at every one. It coalesces too. A call result
that is immediately consumed coalesces straight back; the joint-matrix case added a node because its
value had to live across the argument setup. **So "hoist it into a local" only moves anything when the
value genuinely survives something.**

### `pauseMenuDraw` — the remaining 4 diffs

The whole 1163-instruction stream matches; one web is on the wrong home. PAL copies the three `int`
parameters to `r29`/`r27`/`r26` and `player` to `r28`; the `case 2:` `tokenTextY` lands on **paramC's**
register (`r26`) where retail reuses **paramA's** (`r29`). EN is a built-in control: it puts case 1 on
`player`'s register and case 2 on paramC's, and our source reproduces EN exactly.

Refuted (EN held at 0 diffs throughout unless noted):
- One function-scope `tokenTextY` shared by both arms — EN breaks at **every** one of the 20 positions
  (7 or 22 diffs). Retail genuinely has two separate block-scoped locals.
- Two distinctly-named siblings at switch scope, either order — flat at 4.
- Nested block with initializer, in either arm or both — flat at 4.
- `taskTextIds` moved to block scope in either/both arms, either declaration order — **4 -> 7/10**.
  The knob is live in the right region; it only pushes the wrong way.
- Reusing an existing function-scope local (`x`, `stringIndex`, `textY`, `textHeight`, `lineHeight`,
  `panelAlpha`) instead of a dedicated one — EN breaks identically within each group regardless of
  which name, i.e. it is the missing dedicated web that breaks EN, not the spelling.
- Typed recovery `statusTable->tokens[gPauseMenuTokenIndex].alt` for the raw
  `taskTextIds[gPauseMenuTokenIndex * 4]` — EN gains an instruction. `PauseMenuTokenEntry` really is
  8 bytes with an `alt` field, so the index arithmetic is right; the pointer local is load-bearing.
- Inlining the named constants `f32 zero = 0.0f` and `f32 backdropScale = 435.2f` — both EN-breaking,
  so both are load-bearing despite looking like reconstruction litter.
- PAL-only guard-group (`f32 lineSpan;`) position, all 18 destinations — flat at 4.
- Re-spelling the PAL-exclusive `lineSpan` block six ways — 4, or worse (5, 10).
- **Parameter reordering is not available here.** The stream already matches retail, so the three
  parameters' use sites are already correct; permuting them would change the stream, not just the
  homes.
- A 25-minute randomised **joint** search over the product of the declaration order (19 items, 1-3
  random moves per step, plateau walking) and the `lineSpan` block spelling, gating EN at 0 every
  step, logged nothing below 4. Single-knob flatness at width 6 is not an artifact of searching one
  knob at a time.
- **The PAL-only statement knob is live, and its reachable states were enumerated.** `case 4:` carries
  a PAL-exclusive `lineHeight = gGameTextFontMetrics[sLanguageNameTable[getCurLanguage()].fontId].lineHeight;`
  whose spelling EN never sees, so it steers PAL's band for free. Introducing a GPR temp there really
  does re-colour: a `fontId` temp gives **8**, a `language` temp **13**, reusing `textHeight` or `x`
  **8**, reusing `stringIndex` **23**, reusing `textY` **29**, a result-only temp **4**. Chaining 1, 2,
  3 or 4 temps all give **13** — it saturates immediately, exactly as the pressure-counter model says.
  Baseline 4 remains the minimum over every state reachable this way, and none of them is retail's
  colouring. Merging `lineSpan` into `timer` or `zero`, or declaring it unguarded, is inert at 4 —
  consistent with the GPR and FP counters being independent.

### `maketex` / `loadMemCardImages` — root-caused, 2 instructions

The unit has exactly one defect. Every other "differing function" in it is the 8-byte shift cascade
from this one being long: with it fixed the whole object lines up (`saveCardBuildComment` at 0xf58
onward shifts by exactly 8).

Retail keeps the name-table base in `r31` across the call and spends one instruction per use:

```
lis r3,HA(sMemoryCardFileNameString) ; addi r31,r3,LO ; bl saveCardBuildComment ; addi r3,r31,160
```

We rematerialize the address for the **first** use only (`lis; addi; addi r3,r3,160`), then use
`addi r3,r31,K` for the other five. A synthetic probe reproduces this exactly and isolates the
trigger: **a call between the constant-address assignment and its first use.** On EN the `sprintf`
calls in the SJIS arm use `names` before the DVD chain and anchor it; PAL's arm is a bare
`saveCardBuildComment();`, so nothing does.

- Spelling sweeps (`&names[K]`, a temp local, the global written out, `names` declared last, the
  one-element-array idiom `char* names[1]` that the surrounding code uses for scalars) — all still
  rematerialize. Writing the global out breaks **EN** by the same +2, which confirms the anchor
  reading.
- Twelve `-opt` profiles: only `nopropagation` (and `-opt off/level<=1`, `-O0/-O1`) suppress it, and
  every one of those wrecks EN (unit 3216+). The configured `-opt nopeephole,noschedule` is optimal
  for EN.
- All 13 GC compiler versions (1.0 through 2.7) behave identically — not a toolchain difference.
- **Assigning `names` after the call gets 247/247 with 3 diffs**, purely ordering: the `bl` then
  lands *before* the materialization where retail has it after. A redundant second assignment before
  the call is dead-code-eliminated and gives the same 3.

So the residual is one MWCC rematerialization heuristic, and the missing piece is a PAL-only use of
`names` before the call that costs no instruction. Nothing in retail's stream is such a use.

### `wclevelcont_update` — a genuine either/or between code and data

Four diffs, all in the PAL-exclusive message-timer clamp: retail loads the field into `f1` and the
zero into `f0` and stores `f0`; we swap the pair. The first compare in the same block already agrees
with retail, so only the second site inverts.

- Writing the clamp's zero as the plain literal `0.0f` makes the **code** byte-match
  (report `matched_code 8556/8556`) — but MWCC pools a second zero, `.sdata2` grows 0x38 -> 0x3c, and
  the section fails all-or-nothing (`matched_data 504/560`).
- Keeping `gWcLevelContZero` keeps the data exactly right: 0x38 bytes, and its **13 references match
  retail's 13 references to `lbl_803E85D8` one-for-one** — that atom is the unit's own `.sdata2`
  offset 0 (`splits.txt`: `.sdata2 start:0x803E85D8 end:0x803E8610`), so the symbol is the same object
  under a different name.
- Replacing the const with literals *everywhere* keeps `.sdata2` correct (MWCC merges the literals
  into one atom) but breaks 19 functions: the `base + (224.0f + px + gWcLevelContZero[0])` additions
  fold `+ 0.0f` away and lose an `fadds`. So the named atom is genuine and required.
- Declaration forms (`static`, `[1]`, scalar), eight no-op expression wrappers, a subtraction temp,
  reversed compare operands (MWCC canonicalizes — `zero > field` also gives 4), and twelve `-opt`
  profiles: all 4.
- **The code-for-data trade is worthless, verified end to end.** Despite the metric being named
  `complete_code_percent`, taking the literal (code 8556/8556, data 504/560) through a full
  build → report → `--write-matching` → report cycle leaves PAL at exactly 92.7016 and does **not** add
  the unit to `matching_units.txt`. `tools/version_progress.py:report_unit_is_exact` requires
  `matched_code == total_code` **and** `matched_data == total_data`, with both `fuzzy_match_percent` and
  `matched_data_percent` at 100. So a unit must be whole; there is no partial credit to harvest anywhere
  on this frontier.

### The three rotations, and `GM_MazeWell`

`bossdrakor_update`'s 150 diffs are **one** exchange, not 150 defects: retail is
(`r31`=`obj` param, `r30`=first `obj->extra`, `r29`=second), ours is rotated by one. Applying lever
14's diagnostic (list every definition of each saved register in the target) shows the definition
counts match under the rotation — 1/2/6 against our 2/6/1 — so there is **no** missing local here;
it is a true rotation at band width 7. The named-param-copy lever moves EN by 65 diffs when the copy
is declared first and is inert when declared last. Coalescing-copy edits in the PAL-only
`curveStep`/`advanceStep` block are flat or size-breaking.

### The colouring order rule, read off the trace — and a correction to "gameloop is decl-inert"

Colouring pops the simplification stack, so nodes are coloured in roughly **descending degree**, and
**ties are broken by node index — which runs REVERSE to declaration position** (in
`askProgressiveScanMode` the declarations `showId, counter, sel, textId, i, j, box, savedAlignment` get
nodes 41, 40, 39, 38, 37, 36, 35, 34, so the last-declared local has the *lowest* index and is coloured
first among equals).

That makes a whole class of rows predictable instead of blind. In gameloop, `savedAlignment`, `sel` and
`counter` **all have degree 35** — a three-way tie, and ours colours them in ascending index
(34, 39, 40) taking 28, 29, 30. Retail's assignment is `sel` 28, `counter` 29, `savedAlignment` 31, so
retail needs index order `sel < counter < savedAlignment`, i.e. **`sel` declared last, `counter` just
before it, `savedAlignment` early**.

The prediction holds: moving `savedAlignment` to the front alone takes PAL **25 -> 16**, and orders built
to satisfy the full index requirement reach **14**, as does an independent randomised climb. **So this row
is NOT declaration-order inert, and the earlier verdict here was wrong** — it came from a *single-move*
sweep, which cannot express "move three locals to satisfy a joint index ordering". Count what a sweep
can express before believing its zero.

It still does not close: 14 is the floor over the orders tried, the residual is in `i`, `j`, `showId` and
the second `messageY` web whose colours are not decided by that tie, and every order satisfying the
requirement breaks EN (which wants `savedAlignment` last). A per-version declaration order is available
in principle — this list already differs per version, since PAL carries `messageY`/`shadeReduction` that
EN lacks — but it is only worth spending if a PAL order reaching 0 is found first.

`askProgressiveScanMode`'s definition counts also say it is **not** a relabeling: the per-register definition counts are
`[2,2,3,4,4,5]` in retail against `[1,2,2,4,5,6]` in ours, same total (20 definition points, same
stream), different grouping. Retail coalesces the 600-frame counter and `messageY` into one register;
we split them across two. That is a coalescing decision, so the lever is the local set, not the order
— and the PAL-only region is the only place we may change it. An 8-item declaration single-move sweep
found nothing below 25; the PAL-only `messageY`/`shadeReduction` declaration order and branch
assignment order give 31/35/37, so the knob is live and only pushes the wrong way so far. This is the
most promising of the six remaining walls, because the tell is structural rather than an allocator
residue.

`SaveSelectScreen_render` is band width **9** — two loop triples each rotated. In its first loop the
only named local among the three contested values is the PAL-exclusive `bulletY`; the other two are
strength-reduced walking pointers over `taskTexts` (stride 4) and `gSaveSelectInfoTextIds` (stride 1),
and the second loop's three values are *all* compiler temps with no named local behind them. Sweeping
`bulletY` through every function-scope position and every position inside the `OPEN_FILE` case block
where it is actually used (a more plausible scope) is flat at 20, or 23 at the last two case positions.
Dinosaur Planet's `dll_63_draw` — the ancestor — declares `y` before `i` around the same
`y = start; for (i...) { ... y += step; }` shape, which is what we already have.

`GM_MazeWell_update`'s `r28`/`r29` exchange is between `itemIndex` and a **strength-reduced walking
pointer that has no named local behind it**, so no ordering knob reaches it: the block-scoped
`found`/`itemIndex` swap and all 56 EN-safe single moves of the 8 function-scope declarations are flat
at 14.

## The v1.1 frontier is a DIFFERENT set — check it separately

`tools/version_progress.py GSAE01_rev1` does not list the same units as `GSAP01`. EN v1.1's gap is
**+4.393** over 6 real units, and two of them are not on PAL's list at all:

| unit | pts | blocker on EN v1.1 |
| --- | --- | --- |
| `dlls/engine/0/0.c` | +2.620 | `pauseMenuDraw`, same 4 diffs as PAL |
| `main/gametext.c` | +0.791 | `gameTextBuildSystemFontAtlas` — **v1.1 only**, complete on PAL and PAL v1.1 |
| `dlls/objects/653_WCLevelCont` | +0.295 | `wclevelcont_update`, same 4 diffs |
| `dlls/engine/2/maketex.c` | +0.236 | `loadMemCardImages`, plus `saveCardBuildComment` SIZE 108/109 on v1.1 only |
| `dlls/engine/53/53.c` | +0.229 | `SaveSelectScreen_render`, same |
| `dlls/objects/589_BossDrakor` | +0.222 | `bossdrakor_update`, same rotation |

PAL v1.0 has `main/gameloop.c` and `611_GM_MazeWell` instead, and its `gametext` is complete. So the
two lagging families need partly different work, and a fix verified only on PAL can leave v1.1 short.

**One of these was a real mis-guard, now fixed.** `bossdrakor_update`'s `" DRAKOR SPEED %f "` trace was
guarded to the European builds, but EN v1.1 emits it too: its retail `.data` carries the 18-byte string
right after the jump table, and the call sequence is
`lfs f1,0(r30); fmr f30,f1; lis/addi r3,<string>; crset 4*cr1+eq; bl logPrintf`, then the unscaled
`advanceStep` goes straight to `Obj_UpdateRomCurveFollowVelocityIndexed` — v1.1 has **no** 50Hz
rescaling, so it needs its own arm, not the PAL one. That took the unit's `.text` size from 0x1914 to
retail's 0x192c, its **data from 144/496 to 496/496**, and made `bossdrakor_init` match. The lesson is
general: **a size or data mismatch is a mis-guarded version feature and is findable; a same-length
register permutation is not.** Screen for the former before spending probes on the latter.

**Infrastructure note.** While chasing that, two of rev1's BossDrakor functions
(`bossdrakor_spawnAttackObjects`, `bossdrakor_handleActionEvent`) showed diffs that are **not** source
defects. In `build/GSAE01_rev1/obj/.../BossDrakor.o` six `bl`s are left as raw displacements with no
relocation. The branch at object offset 0x828 resolves to `0x8020A800 + 0x828 - 0x18AC24 = 0x80080404`,
which `config/GSAE01_rev1/symbols.txt` names `s16toFloat` — and the equivalent site in **every other
version's** carve does carry `R_PPC_REL24 s16toFloat`. So that object is mis-carved, not mis-written.

Scope it by **content, not timestamps**: counting raw `4b`-prefixed branches across the frontier units
for all four versions finds this in **GSAE01_rev1's BossDrakor only** (`gametext` has 3 and `gameloop`
1 on every version, which is normal). It is not a systemic understatement. Nothing in the ninja graph
produces these objects — they are inputs to `main.elf` — so the build cannot notice.

**Do not use mtime to detect this.** Every retail object of every version, *including GSAE01 where all
of these units match perfectly*, is older than its own `symbols.txt`; that file is touched far more
often than the carve needs to re-run. The only sound test is the relocation content itself.

**`gameTextBuildSystemFontAtlas` (v1.1, +0.791)** is a textbook scratch-band signature: 306/306
instructions, everything identical through instruction 236, then a clean **rotation of `r3..r11` by
one** (`r3`→`r4`, … `r10`→`r11`, `r11`→`r3`) across the inlined `gameTextCopySystemFontTile` body.
CLAUDE.md calls that a per-TU flag signature, but no profile reaches it: eight `-opt` profiles and
seven `-inline` settings all leave EN at 0 and v1.1 at 36, except those that break EN too. Reshaping
the caller's tile-copy block is a live knob that moves **EN** (hoisting the texture base gives EN 29
diffs at 275/275 instructions), which only confirms the mechanism — the block is already right for the
four versions that match. Coalescing aliases in v1.1's own `switch (OSGetFontEncode())` arms are
completely inert, because MWCC folds them away without allocating anything.

**Measurement hazard found the hard way:** `gameTextSetLanguage` reads as 17 diffs on PAL from a
stale object and as `MATCH` once rebuilt, and the same unit is reported complete by `report.json`.
Always rebuild immediately before extracting, and prefer the ninja-built object for any unit listed in
that version's `matching_units.txt`.

## The bands are IDENTICAL, which is the strongest evidence these are not source defects

For every remaining blocker, retail's saved-register band and ours are the *same set*, not merely the
same width:

| function | band | width |
| --- | --- | --- |
| `pauseMenuDraw` (PAL) | `r26..r31` | 6 |
| `askProgressiveScanMode` (PAL) | `r26..r31` | 6 |
| `bossdrakor_update` (PAL) | `r25..r31` | 7 |
| `SaveSelectScreen_render` (PAL) | `r23..r31` | 9 |
| `gameTextBuildSystemFontAtlas` (v1.1) | `r20..r31` | 12 |

Identical bands mean **identical register pressure**: there is no extra value in our source to remove
and none missing to add. Combined with identical instruction streams, that leaves only which colour the
allocator hands each web — and every one of these widths is past the cliff where the band model has any
predictive power (97.7% at width 2, 0.1% at width 6+). This is why the ordering sweeps are flat, and it
is a positive result rather than an absence of one: it rules out the upstream-of-allocation defect that
lever 16 fixes, which is the only documented escape at these widths.

The corollary for `SaveSelectScreen_render` specifically: retail assigns the slot loop's four values to
*consecutive* `r23..r26` while we scatter them across `r23, r25, r26, r28`, with `slotIndex` landing on
`r23` in both. Only `slotIndex` is a named local; the other three are strength-reduced walkers, and one
of them indexes a base that is re-loaded from a global pointer every iteration
(`lwz r5,0(0); lbzx r5,r5,r0`), which an explicit pointer local cannot reproduce without hoisting that
load. So the source shape is already right.

## The gap set is PROVEN closed — there is no alternative route

Worth establishing rather than assuming, because the natural hope is that some *other* unit could close
the gap instead of the hard ones:

- **No unit is complete on a lagging version but incomplete on EN.** That reverse set is **empty** for
  GSAP01, so there is no banked gap closure anywhere and none to find.
- **Every unit incomplete on EN is incomplete by the identical amount on all five versions.** `objhits`
  (8392/8480), `sal_volume` (592/612), `trigf` (56/60), `objprint_dolphin` (18104/25004) report the same
  numbers everywhere. Closing any of them raises EN's ceiling by the same points, so the *delta* does not
  move. Only `model` differs at all (EN 23600/25288, PAL 23276/25308) and it is far from complete on both.

So the parity gap is exactly the seven units in the table above, and the five colouring ties are the
whole of it. Any future idea has to attack one of those functions.

### `objhits` looks like the biggest prize anywhere (+0.905 on every version) and is rule-bound shut

Its code is **already 100%** (25988/25988) on all five versions and only its `.sdata2` fails, so it
reads as an 88-byte data fix. It is not:

- All **134** float-pool loads resolve to the **correct value** on both sides — the code is genuinely
  right, and a wrong-constant bug hiding behind objdiff's `@N`-vs-`lbl_` leniency was the obvious
  suspect. Ruled out by resolving every referenced atom's bytes.
- Retail's pool holds **18** atoms to our 17, including one at 0x14 that **no instruction references** —
  a dead literal in retail's source — and its order differs (`0.0, 2.0, 1.0, 0.1, 1e-6, 0.0(dead), …`
  against our `0.0, 1.0, 1e-6, …, 0.1, 2.0`).

That is a genuine pool-**order** difference, which CLAUDE.md addresses directly: *"If the pool ORDER
genuinely differs … that is a TU-boundary artifact — leave the unit NonMatching, do not reconstruct the
pool."* Reconstructing it would also mean planting a dead literal to reproduce an unreferenced atom.
Leave it. `sal_volume` (20 B) and `trigf` (4 B) are the same shape at smaller scale.

## `tools/expr_sweep.py` is the right tool for the two scratch-band rows — and it comes back empty

Two of the blockers put their residual entirely in volatile registers, which declaration order provably
cannot reach: `wclevelcont_update`'s `f0`/`f1` pair and `gameTextBuildSystemFontAtlas`'s `r3..r11`
rotation. That is exactly the class `tools/expr_sweep.py` exists for — it enumerates commute / relflip /
ternflip / assoc rewrites on a real parse tree and refuses any variant `semantic_equivalence.prove()`
has not certified. Run on both:

| function | version | base fuzzy | variants cleared | best |
| --- | --- | --- | --- | --- |
| `wclevelcont_update` | GSAP01 | 99.77273 | 3 | 99.77273 |
| `gameTextBuildSystemFontAtlas` | GSAE01_rev1 | 98.85621 | 13 | 98.85621 |

No rewrite moves either. So the scratch rows are not expression-shape rows.

**Two gotchas that cost time, both reusable.** `expr_sweep` needs the target version **configured**
(`configure.py --version <V>`) or `fuzzy_measure` returns `-1.0` and every candidate ties at the base.
And it parses real C, so it dies on `#if` inside a function body — which is every interesting
version-divergent function here. The workaround is to resolve the guards for the one version first, then
**verify the transformation by rebuilding**: the diff count must be unchanged (4 for `wclevelcont_update`
on PAL, 36 for the atlas on v1.1) before any sweep result can be trusted.
`scratchpad/prepfn.py` in this session's notes does the guard resolution with a proper nested
`#if/#elif/#else/#endif` stack.

## Axes that are exhausted across all seven, not just one

These were each run against every unit where they could apply, always with EN gated at 0 diffs. None
produced a match anywhere, so a new idea for this frontier should not start here.

- **The complete flag space on engine/0, the +2.620 unit.** 33 trials: `-O0` through `-O4,s`, eight
  `-inline` settings, `-fp_contract`, `-use_lmw_stmw`, three `-str` modes, `-sdata`/`-sdata2` at 0/4/8/64,
  and `-enum int|min`. Every setting that keeps EN byte-identical leaves PAL at exactly **4**; every
  other setting breaks EN by 900-1100 diff words. `-O4,p` is the configured level and is correct.
- **Symbol boundaries verified, so the compared windows are right.** `pauseMenuDraw` is declared
  `size:0x11D4` on EN (= 1141 x 4) and `size:0x122C` on PAL, EN v1.1 and PAL v1.1 (= 1163 x 4);
  `wclevelcont_update` is `size:0x1B8` (= 110 x 4) on both lagging configs. The diffs are real, not
  windowing artifacts.
- **The "make PAL match and version-guard it" strategy is ruled out.** Every sweep above gated on EN at
  0 and only then measured PAL, which leaves open the idea that some shape makes *PAL* match while
  breaking EN — that shape would be PAL's source, and the divergence would simply go behind a guard.
  Measuring PAL for the EN-breaking variants too (typed `statusTable->tokens[i].alt` recovery in either
  or both arms, dropping the pointer local, inlining `backdropScale`, block-scoped `taskTextIds`, and a
  function-scope shared `tokenTextY` at all 20 positions) finds **no PAL match**, and more tellingly EN
  and PAL move *together*: each variant produces the identical diff count on both. The 4-diff gap is
  invariant to all of them, so there is no per-version shape to split.
- **The inlined-callee surface does not exist for engine/0.** If a helper were inlined into
  `pauseMenuDraw`, its locals would join that function's IR and its shape would be a live knob. The unit
  is built `-inline noauto,deferred`, so only explicitly `inline` functions are candidates, and of the
  14 `static inline` helpers in the file `pauseMenuDraw` calls **none**. Nothing is inlined into it, so
  there is no callee local set to reshape.
- **Redundant occurrences in SHARED code are inert too.** Duplicating `tokenTextY`'s initializer (x2 and
  x3) or the `taskTextIds` assignment, in either token case, leaves EN at 0 and PAL at 4: MWCC removes
  the redundant store outright, so the IR is unchanged. This was the last mechanism that could have
  produced a stream-invisible difference in code EN also compiles.
- **TU `-opt` profiles.** Twelve profiles per unit. Every unit's configured profile is already the one
  that keeps EN byte-identical, and every alternative wrecks EN by thousands of diff words. Probed on
  engine/0 (`pauseMenuDraw` 4 under the configured profile, 4 or 200+ under all others), maketex,
  WCLevelCont. BossDrakor already carries `nocse,nopropagation`.
- **Compiler version.** All 13 GC builds (1.0, 1.1, 1.1p1, 1.2.5, 1.2.5n, 1.3, 1.3.2, 1.3.2r, 2.0,
  2.0p1, 2.5, 2.6, 2.7) produce byte-identical results for maketex. Not a toolchain axis.
- **Local type.** `int` / `u32` / `s32` / `long` / `short` / `u16` / `s16` / `u8` on the PAL-exclusive
  locals: `bulletY` in engine/53 is flat at 20 for all eight; `messageY` in gameloop is 25 for the
  32-bit types and 26 for the narrow ones. Allocator-visible in principle, inert here.
- **Coalescing copies as a pressure knob.** `b = a;` before the use emits no instruction (verified:
  the stream stays 242 instructions) and is therefore the one documented way to change the count of
  values the allocator sees without touching the stream. Added in gameloop's loop 1, loop 2, and both,
  at two declaration positions: **all flat at 25**. Consistent with the counter saturating, which also
  means the knob only exists in the decreasing direction, and nothing can be removed from these
  regions without changing the stream.
- **Inlined helper boundaries (lever 6).** A `static inline` helper that MWCC fully inlines keeps the
  instruction count exact and can move homes wholesale — extracting engine/0's `case 2:` body into one
  held `pauseMenuDraw` at 1141/1141 instructions while moving **127** registers. But the movement comes
  from the helper's *own locals* (its four measurement temps), not from the boundary: WCLevelCont's
  PAL-only block extracted into a helper with no new locals, in both a `state` and an `f32*` form, is
  **exactly inert at 4**. So the boundary is only a knob when it also changes the local set, and in all
  seven units EN matches without the helper, which means retail has no helper there to restore.
- **Splitting a web into two source locals.** gameloop's `messageY` has two disjoint webs (loop 1 in
  `r27`, matching; loop 2 in `r29` retail / `r28` ours), which looks exactly like two variables. Giving
  loop 2 its own function-scope local costs **one instruction** at every declaration position, so
  retail's is one variable whose web the allocator splits, and the split is not a source fossil.

## Stream-neutral perturbations — the right idea, and it is also exhausted

Identical stream + identical band + a deterministic compiler means the colouring should be identical
too, so a source difference must exist that is **invisible in the final stream**. That is the correct
deduction and it is worth keeping, because it says the defect is real rather than unreachable. Every
mechanism that produces such a difference has now been tried, all gated on EN at 0 diffs:

- **Coalescing copies** (`b = a;` then use `b`): verified emit-nothing, and inert wherever placed —
  gameloop's two loops in every combination, WCLevelCont, and aliases in v1.1's atlas arms, which MWCC
  folds away without allocating anything.
- **Redundant CSE-folded occurrences** (lever 9): duplicating pauseMenuDraw's PAL-only division once or
  twice is inert at 4; duplicating the PAL-only `lineHeight` statement costs an instruction.
- **Perturbing version-exclusive code through an existing local** rather than a new one: routing v1.1's
  atlas `glyphCount` stores through the function's own `glyphCount`, in either arm or both, is inert
  at 36.
- **Block-scoping the macro-aliased measurement locals on PAL** (the hypothesis that retail declared 12
  and MWCC overlapped them into the 4 slots PAL actually uses): MWCC does **not** overlap them, giving
  151 diffs. The `#define tokenLeft boundsLeft` aliasing really is the faithful representation of PAL's
  4-slot group, ugly as it looks.

So the invisible difference exists but is not in any construct reachable from these five functions'
sources. That is where this frontier stands.

## `docs/priced_classes.md` already priced this class — read it FIRST next time

This whole frontier sits inside classes the campaign measured long before this session, and the ledger's
index states them in one line each. Consulting it first would have saved most of the work above:

- **The colouring frontier is MEASURED EXHAUSTED on both order axes.** §26/§27 built and scored
  **34 100 orderings** over all 128 attackable colouring rows (25 335 declaration + 8 236 statement + 529
  by hand) for **8 hits**; §31 then added **7 000** more orderings and rewrites over the 67-row /
  113 512 B non-bijective population — 5 717 statement, 503 split, 381 operand, 399 declaration — for
  **0 hits, 0 bytes**. Roughly 41 000 gated builds, and the five rows here are in exactly that
  population.
- **§31e covers the float half directly:** a declaration never touches `f0`-`f13`, measured over
  **8 085 differing FPR operands, 0 volatile**. `wclevelcont_update`'s `f0`/`f1` pair is that row, and
  `expr_sweep` (above) closes the expression axis on it too.
- **§29 already proved the parameter-home point** this session re-derived: "the six-permutation proof
  that a parameter home is not reachable from the declaration list", 8 rows worked off the byte ranking,
  33 spellings, yield 0.
- **The pool-order rows are priced, not open.** The ledger's "mover" row says only one construct puts a
  pool word ahead of its first live loader — a `static const` aggregate of at most 8 bytes — and that is
  the banned `SINGLE_ELEM_CONST_ARRAY` shape, with the patch deliberately parked and never landed. That
  is exactly `objhits`/`sal_volume`/`trigf`, and §12b showed the ban is on the shape rather than the
  bracket, so there is no legal spelling.

This session's independent sweeps agree with all four rows, which is reassuring but was not new
information. **The lesson: price a row against `docs/priced_classes.md` before sweeping it.**

## Reference mining — what the projects do and do not give

- `reference_projects/rena-tools/sfadebug` is a decomp of the SFA **PAL debug build** (from
  *Interactive Multi-Game Demo Disc - July 2002*, minimally optimized, so its stack slot order would
  reveal original local sets and declaration order directly — the one input this whole frontier is
  missing). **Its code bytes are not obtainable from this tree.** `orig/GSAP01-DEBUG/` holds only
  `.gitkeep`, and `default.dol.gzf` is a 43 MB Ghidra packed database, not a DOL: a Java-serialized
  header, then one zip entry `FOLDER_ITEM` at offset 52 which raw-inflates (`zlib`, `wbits=-15`) to
  171 MB of database buffers. Those buffers carry the symbol and source-file analysis — searching them
  finds `WClevcontrol`, `maketex.c`, `n_pausemenu` — but **no program bytes**: `blr` (`4e800020`),
  `mflr r0`, `mtlr r0` and `stwu r1,-x(r1)` each occur exactly **once** in 171 MB, where real GC code
  would have thousands. Do not spend time on this container again; getting the debug build's code
  needs the disc. `src/main/` has 11 hand-decompiled files and covers none of the frontier units.
- `sfadebug/notes/srcfiles.csv` is the useful artifact: 11,797 rows of
  `start,end,function,source-file` recovered from the debug build's `__FILE__` strings. It gives the
  **original filenames** — `WClevcontrol.c`, `main/maketex.c`, `n_pausemenu.c`, `frontend_control.c`,
  `savegame.c`, `picmenu.c` — and shows the original object-DLL callback naming: within
  `WClevcontrol.c` the functions are plain `initialise`, `release`, `init`, `update`, `hitDetect`,
  `render`, `free`, `getExtraSize`, `setScale`, not our `wclevelcont_*` prefixes. Useful for naming and
  TU-boundary work; it carries no local-variable information.
- `reference_projects/dinosaur-planet` carries the **real original DLL names** (`66_pausemenu`,
  `63_gameselect`, `18_objfsa`, ...) and per-DLL `syms.txt` with real global names, recovered from the
  prototype's symbol table. Local variable names there are the decomp authors' reconstructions, not
  DWARF, so it gives structure and declaration *order*, never a guaranteed local set.
  `66_pausemenu` is a far earlier ancestor of our pause menu (no tokens, no task text).
  `63_gameselect`'s `dll_63_draw` is the ancestor of `SaveSelectScreen_render` and declares `y` before
  `i` around the same `y = start; for (i...) { ... y += step; }` shape.

## CLOSED: pauseMenuDraw, and the colour-selection law that closed it

`pauseMenuDraw` matched on all five versions, which **met the parity goal**: EN v1.0 96.589485,
JP 96.589775, PAL v1.0 97.852310, EN v1.1 97.331440, PAL v1.1 97.852554 (each lagging version
+5.15, from 92.70/92.18). The unit is 2.622% of all code — two thirds of PAL's entire 3.888 gap —
because `complete_code` is all-or-nothing per function and this one function is 4652 bytes.

The four residual diffs were one web: case 2's token-prompt accumulator, retail `r29`, ours `r26`.
`tools/tricky_backend_trace.py --graph` gives the allocator's own decision, and
`replay_coloring()` in `tools/tricky_backend_graph.py` is the exact rule:

- **A colour is `min(enabled - blockers)`.** `blockers` are the colours of *already coloured*
  neighbours, so a neighbour coloured later does not constrain a node at all.
- **`enabled` starts with the scratch band only.** Saved registers are added one at a time by
  *bank expansion*, in the fixed reserve order `r31, r30, r29, r28, r27, r26, r25 ...`, and only
  when a node has no free colour left. This is the concrete mechanism behind the "rotation"
  description in CLAUDE.md: what varies between two builds is *how many* registers have been
  expanded by the time a given node is coloured.
- **The colouring order is: nodes of degree >= 32 first, then every remaining node in DESCENDING
  node index.** Node index runs **reverse to declaration order** (first-declared = highest index),
  and a **block-scoped local is numbered below every function-scope local**, i.e. it is coloured
  *last*, after every function-scope local has already forced its expansions.

That last point is the whole defect. Both `tokenTextY`s were block-scoped in their `case` blocks
(nodes 35 and 36), so case 2's was coloured at position 250 of 254 — after `textY` expanded `r26`
at position 246 — leaving `min(free) = 26`. On EN the same node is coloured with `enabled` only
`{r27..r31}` (EN never expands `r26`) and blockers `{27,30}`, giving `min = 28`, which is what EN
retail has. The two versions want *different registers from the same source*, and both are simply
`min(free)`; nothing needed to be steered per version.

**The fix:** hoist case 2's accumulator out of the `case` block into the function's declaration
list, as its own local (`s32 taskTextY;`, beside its sibling `taskTextIds`), leaving case 1's
`tokenTextY` block-scoped. That raises its node index above `stringIndex`/`textY`, so it is coloured
while `enabled` is `{r28..r31}`: blockers `{28,31}` -> `r29` on PAL, blockers `{27,30}` -> `r28` on
EN. Byte-identical on EN and JP, exact on PAL/rev1/PAL_rev1. Positions 0..7 of the declaration list
all work; position 8 (beside `textY`) does not, because the new node must outrank `stringIndex`.

Corrections this forces to earlier entries in this document:
- The frontier was **not** allocator-gated. "Needs a 13-interference swing in node degree" was
  measured on the pre-fix graph and was the wrong lever: degree only decides the >= 32 prefix, and
  the actual knob was **which population the local belongs to** (block-scoped = coloured last).
- **Merging** the two accumulators into one shared function-scope variable breaks EN (22 diffs).
  Splitting the declaration while keeping two webs is what works. This is `source_shape_levers.md`
  §16 ("change the SET of source locals") with the direction that matters: *scope*, not order.
- CLAUDE.md's note that hoisting a block-scoped local "reaches orderings no permutation of the
  top-level list can express -- worth trying at width <=4" understates it. Here the band is width 6
  and the hoist was the *only* move that works, because block scope is not a position in the
  ordering, it is a separate lower-numbered population.

## engine/2 maketex: an explicit cast blocks constant-address rematerialization

`loadMemCardImages` was +2 instructions on PAL/rev1/PAL_rev1 at one site: retail reaches the first
filename as `addi r3,r31,160`, we emitted `lis`/`addi`/`addi`. Derived from the compiler on a minimal
body: **MWCC rematerializes a link-time-constant address when the def and its first use are in the
SAME basic block with a call between them.** A branch between def and use (EN's `if (sjis)`, which is
why EN never showed this) or a def below the call both suppress it; a plain `{}` block, `if (0)`,
`while (0)`, `p = p;`, the index form `&p[k]`, and every `-opt` token do not (`nopropagation` is a
demolition, +26 instructions).

**What does suppress it is an explicit cast on the initializer:**

    char* names = (char*)sMemoryCardFileNameString;   /* held in r31 across the call */
    char* names = sMemoryCardFileNameString;          /* rematerialized after the call */

Measured over eight declaration forms: `(char*)tab`, `(char*)` of a `[][12]` array and `(char*)` of a
struct array all produce retail's stream; plain `tab`, `&tab[0]`, `tab[0]` and `&tab[0][0]` all
rematerialize, and the object's declared size and storage class are inert. The cast closed
`loadMemCardImages` on **all five versions**. Worth trying wherever a `char*`/`u8*` local aliases a
global array and the diff is a stray `lis`/`addi` pair after a call.

`saveCardBuildComment` (rev1 only) is a second instance with a different residue: names-before-call
routes through `r0` and copies (`addi r0,r3,0 ; mr r31,r0`, +1), names-after-call has the right length
but materializes below the `bl`. Best found, and applied, is `int language = getCurLanguage();` before
the cast declaration: exact length (108/108) with **3** ordering diffs, down from a +1 size mismatch.
This is the only thing still holding maketex on rev1; PAL and PAL v1.1 are complete.

## main/model: a missing version-guarded cache invalidate

`ObjModel_LoadModelData` was 5 instructions short on PAL/rev1/PAL_rev1 and the size screen found it
immediately: retail keeps the allocation size in a saved register and calls
`DCInvalidateRange(model, totalSize)` after `roundUpTo16`, which EN v1.0 and JP do not. Three
instructions for the call plus the `r29` save/restore pair the extra live value forces = 5. Naming the
size (`totalSize`) and guarding the call with `#if !defined(VERSION_GSAE01) && !defined(VERSION_GSAJ01)`
matched the function on all five versions.

PAL then still failed on data (`.sbss` 77.78%) for a naming reason, not a content one: the two
source-declared globals `lbl_803DCB58`/`lbl_803DCB5C` are named after EN addresses, and **every other
version's `symbols.txt` keeps those EN-derived names mapped to its own address** (JP `0x803DCC78`,
rev1 `0x803DD7D8`, PAL v1.1 `0x803DE510`) — only `config/GSAP01/symbols.txt` deviated, naming them
`lbl_803DE350`/`lbl_803DE354` after PAL addresses, so objdiff could not pair them. Renaming those two
entries to the shared convention (gated with `tools/pairing_check.py`, 0 retail-only symbols) took the
unit to 604/604 data. When a mapped unit's code is perfect and only its data is short, check the
target's symbol NAMES against the other versions before looking for a content defect.

## engine/53: a version-guarded local that retail never had

`SaveSelectScreen_render` had a 20-diff 3-cycle among the task-text loop's three walkers (strides 42,
1 and 4). Retail colours the bullet-Y walker FIRST, giving it the highest of the three registers — but
a named local is numbered below every compiler temp and is therefore coloured LAST, so retail's bullet
Y cannot be a local at all. Writing it as the strength-reduced expression it must be:

    gameTextShowStr(sSaveSelectTaskBullet, 0x93, 0x41, 52 + taskTextIndex * 42);

and deleting the guarded `int bulletY;` plus its `= 52` / `+= 42` statements matched the function on
all five versions. `(taskTextIndex + 1) * 42 + 10` is NOT equivalent to the allocator (52 diffs), so
the walker's initial value and stride both have to come out right.

## gametext: an exhaustive permutation, because single moves are a plateau

`gameTextBuildSystemFontAtlas` sat at 36 diffs on EN v1.1 — a permutation of the SCRATCH band in the
tile-copy block. Every scratch colour is enabled from the start, so `min(enabled - blockers)` makes the
assignment a pure function of the colouring order, i.e. of that block's five declarations. Of the 120
permutations exactly **four** keep EN byte-identical, and exactly **one** of those is also rev1-exact:

    int tileColumn; int tileRow; int firstTileColumn; int firstTileRow; u32* glyphPixels;

Every single move from the old order scored 36, so a single-move sweep could not have found this: the
exchange is a 3-cycle. When five or fewer locals are in play, run the full permutation set.

## Still open, with what each one now needs

EN v1.0 and JP are at 100.000000. PAL v1.0 99.199320, PAL v1.1 99.199420, EN v1.1 99.239685.

| unit | share | versions | residue |
|---|---|---|---|
| `653_WCLevelCont` | 0.298 | all three | 4 diffs, one f0/f1 exchange at the message-timer clamp |
| `main/gameloop` | 0.247 | PAL, PAL v1.1 | 25 diffs, band rotated by one |
| `dlls/engine/2/maketex` | 0.238 | EN v1.1 | `saveCardBuildComment`, 3 ordering diffs |
| `589_BossDrakor` | 0.226 | all three | 150/154 diffs, one copy-vs-load exchange |
| `611_GM_MazeWell` | 0.030 | PAL, PAL v1.1 | 14 diffs, `i` vs `questBitPtr` |

**WCLevelCont** needs a single-`.sdata2`-atom shape whose clamp constant is coloured before the field
reload. Retail's unit has exactly one `0.0f` atom (offset 0) serving both the clamp and the four
`x + 0.0f` adds in `traceMoveA`/`traceMoveB` — verified from the relocations — so the clamp cannot use a
literal: a literal is always a second atom (measured: an extra `00000000` at 0x34, which fails all 56
bytes and costs the unit its registration), and dropping the const array folds the four adds away.
Measured flat at 4: block-local and function-scope locals for the constant, for the field, and for both
with declaration and assignment order split all four ways; `*gWcLevelContZero`; reversed compare; `<=`;
`!(>=)`; ternary; else-form; and six upstream reorderings of the guard and subtraction. Naming does not
move it, which is CLAUDE.md's coalescing rule, and the flag space for this unit was measured inert
earlier. The FPR band is far weaker than GPR for the band model, so this is the expected shape of a
hard FP row.

**gameloop**, **GM_MazeWell** and **BossDrakor** are measured plateaus on ordering: 30 EN-safe
declaration orderings x 2 guard positions and 16 PAL-only block permutations for `askProgressiveScanMode`
(all 25), 38 EN+rev1-safe orderings plus 18 hoist positions for `GM_MazeWell_update` (all 14). Their
exchanges are rotations, which per CLAUDE.md ordering cannot reach at this band width, so the lever is
the SET of locals — also probed: merging `i`/`j` in `askProgressiveScanMode` takes PAL 25 -> 23 but costs
EN 6 diffs, and dropping the `showId` copy makes PAL a size mismatch. `bossdrakor_update` wants the
parameter copy at the TOP of the band (`obj` = r31, `state` = r30) where ours puts the copy at the
bottom; the named param-copy lever (`GameObject* self = obj;`) moves PAL 150 -> 136 but costs EN 65, in
both declaration positions tried. `saveCardBuildComment` is flat at 3 across seven
declaration/assignment splits.

## Why the last two rows are closed on structure, from the allocator's own graph

**`askProgressiveScanMode` (PAL, PAL v1.1).** The trace shows four nodes tied at degree 35 --
`box`'s temp (node 53), `counter` (40), `sel` (39) and `savedAlignment` (34) -- and they take
r31/r30/r29/r28 in that order because the colouring order is descending degree with ties broken by
descending NODE INDEX. Retail wants `savedAlignment` = r31, `box` = r30, `counter` = r29,
`sel` = r28, i.e. `savedAlignment` coloured FIRST. Node index runs reverse to declaration order, so
`savedAlignment`, declared last of the four, has the lowest index and can never win an index
tie-break; and it cannot be given a higher index than node 53 because compiler temps are numbered
above every named local (`box` is coalesced into such a temp, being a call return). The only route is
degree: retail's graph must have `savedAlignment` one degree above the other three, where ours has all
four equal. Every web inside the do-while interferes with all four alike, because
`savedAlignment`'s range spans the loop exactly as `counter`'s and `box`'s do, so no web can be added
or removed that separates them. Measured flat: 30 EN-safe single moves x 2 guard positions, 16
permutations of the two PAL-only blocks, and the set-of-locals probes (merging `i`/`j` takes PAL
25 -> 23 but costs EN 6 diffs; dropping the `showId` copy makes PAL a size mismatch).

EN is the control that makes this a real obstruction rather than an unfound spelling: EN colours
`savedAlignment` LAST too and EN is byte-exact, so the same source must produce
`savedAlignment`-last on EN and `savedAlignment`-first on PAL. The difference has to come from PAL's
two extra locals changing degrees, not from anything orderable.

**`wclevelcont_update` (all three).** The f0/f1 pair is pinned to the VALUES, not to source order:
with the clamp written either way round (`field < const` or `const > field`) the field takes f0 and
the constant f1, because the constant's web outlives the compare -- the clamp's store reuses it --
while retail gives the constant f0. Forcing two separate loads needs a second `.sdata2` atom and
retail's unit has exactly one `0.0f` atom serving both the clamp and the four `x + 0.0f` adds
(verified from the relocations), so the literal route costs the unit its registration. Naming either
value does not move it, which is CLAUDE.md's coalescing rule.

## The TU cflag axis, closed on all five rows (and the one thing it proved)

`-opt`/`-inline` profiles are the sanctioned alternative to the banned per-function pragmas, so all
five remaining functions were swept across twelve profiles on all five versions
(as-configured, noprop, nocse, nocse+noprop, nolifetimes, nostrength, noloopinv, -inline noauto,
-inline off, noprop+noauto, peephole, schedule).

`askProgressiveScanMode`, `GM_MazeWell_update`, `saveCardBuildComment` and `bossdrakor_update` are
flag-INERT: no profile beats the configured one (25, 14, 3 and 150 respectively), and every profile
that changes anything makes it worse or changes the length.

**`wclevelcont_update` is the exception, and it identifies the mechanism.** Under
`-opt ...,nocse` the function matches on ALL FIVE versions. So the defect is that MWCC value-numbers
the guard's load of the zero atom and the clamp's load of the same atom into ONE web: that web is
created at the guard, which gives it the lowest node index, which means it is coloured LAST and takes
the higher free register (f1) — while retail has the clamp's constant in f0, i.e. coloured first, i.e.
a separately created web. `nocse` splits them and the assignment becomes retail's.

It is still a cap, for a measured reason: with `nocse` the unit as a whole REGRESSES on every version,
because `wclevelcont_traceMoveA` and `wclevelcont_traceMoveB` each grow 6 instructions (289 -> 295) —
retail plainly compiled this TU with CSE ON. A DOL-confirmed TU takes one profile, and blocking the
CSE from source would need a `volatile` or a pun, which CLAUDE.md bans by name. Retail's `.sdata2`
offset 0x1c is alignment padding for the 8-byte int->float magic at 0x20, not a second zero atom, so
there is no second atom to read either: the unit really does have exactly one `0.0f` serving the guard,
the clamp and the four `x + 0.0f` adds.

What would close it is the real source difference that makes retail's two loads distinct values while
CSE is on. Nothing in the spellings tried reaches it.

## askProgressiveScanMode: what is established, and what is NOT

The PAL band is our band rotated by one: ours has (box, counter, sel, savedAlignment) =
(r31, r30, r29, r28), retail has (r30, r29, r28, r31), i.e. retail colours `savedAlignment` FIRST.

**Established from the traced graph.**
1. `box`'s value is an unnamed temp, not the named local. Its register_object node has degree **0** in
   every spelling tried (plain call, `(GameTextBox*)` cast, `&gameTextGetBox(0)[0]`,
   `&(*gameTextGetBox(0))`): the return arrives in r3, a precoloured register, so the copy coalesces and
   the temp is the representative. `savedAlignment` is a plain load into its own home and keeps its
   named node (degree 35).
2. `deg(savedAlignment) <= deg(box)` identically, and here they are equal at 35. `savedAlignment` is
   defined immediately after `box` and both die at the same `stb`, so its live range is a strict subset
   and every web overlapping it also overlaps box. Inserting a web between the two definitions raises
   box's degree only, which is the wrong direction.
3. In the observed worklist the temps are coloured before every named local, so `savedAlignment` is
   coloured after box's temp and `min(enabled - blockers)` then hands it the lowest free register.

**NOT established.** `coloring_order()` recovers the compiler's actual linked worklist
(`prefix[0]` chains the nodes), not a priority derivable from degree and index. An earlier version of
this section claimed a proof of impossibility from a DFS over all 10! orders of the named locals; that
DFS was run against a STALE node-to-variable mapping and its conclusion should not be relied on. What
the ordering evidence really shows is empirical: 30 EN-safe single moves x 2 guard positions, all 28
pairwise swaps, 6 relative orders of the three band participants x 9 guard positions, and two ungated
searches (greedy and randomised multi-start) all plateau, at 25 gated and 10 ungated -- never 0.

Because the worklist is compiler-internal, the lever that can move it is the one that changes which
values exist, not their order. That is exactly what worked: `savedAlignment = (u8)box->alignH;` takes
PAL from 25 to 12, the same class as the `(char*)` cast that closed `loadMemCardImages`. It costs EN 24
diffs because the site is shared code, and it does not reach 0, so it is not applied.

Pressure-counter probes, all inert at 25: casts on `messageY` at either use or both, a cast inside the
`shadeReduction` expression, `int` -> `s32` on either guarded local, and hoisting `dvdCheckError()` into
a PAL-only local in either or both guarded blocks, declared above or below. A value defined and
immediately consumed gets no web, which is why the last one changes nothing.

## WCLevelCont: the nocse escape hatch is closed too

`-opt nocse` matches `wclevelcont_update` on all five versions but costs `traceMoveA`/`traceMoveB` six
instructions each. Those six are exactly one recomputed `gWcTileGrid[i][b]`: a `lis`/`addi` for the
global, `slwi`/`add` for the index and two `extsh` for the s16 conversion, read once for the `!= 0` test
and again for the `<= 4` test. Retail CSE'd it.

Hoisting it into a local (`u8 cell = gWcTileGridA[i][b];`) is the obvious way to let the TU take
`nocse` without paying for the recompute. Measured: it takes the unit from 96.349 to 97.561 under
`nocse` and lets `update` match on PAL -- but `traceMoveA` still fails, and under the CURRENT flags the
hoist BREAKS `traceMoveA`, which was matching. That is the decisive evidence: retail's source reads the
grid twice and relies on CSE, so the hoist is not retail's source and `nocse` cannot be adopted for this
TU without writing code retail did not have.

The sibling level-control DLLs in `reference_projects/dinosaur-planet` (`607_WL_LevelControl`,
`638_DFPlevcontrol`, and eight more `*levcontrol` objects) confirm the clamp idiom
`if (timer > 0) { ...; timer -= delta; if (timer < 0) timer = 0; }` with LITERAL zeros, but their timers
are integers, so they say nothing about the float pool atom. Retail's own relocations confirm our four
`x + gWcLevelContZero[0]` addends are real: the zero atom at `.sdata2` offset 0 is read twice in
`traceMoveA` and twice in `traceMoveB`.

So the unit needs the clamp's constant web created late while CSE stays on, and the only instruments
that do that are a per-function pragma (banned by name) or a `volatile`/pun (banned by name).

## The per-unit compiler-version axis, closed on all five rows

`mw_version` is a per-Object setting in `configure.py` and is already used in the tree for `mtx.c`,
`vec.c`, `__mem.c` and `__start.c`, so a per-unit compiler is a sanctioned knob rather than a hack. All
ten available GC compilers were swept on each remaining row, on all five versions:

| row | result |
|---|---|
| `askProgressiveScanMode` | 1.3 .. 2.7 all identical (EN 0 / PAL 25); 1.2.5 and 1.2.5n change length |
| `wclevelcont_update` | 1.3 .. 2.7 all identical (EN 0 / PAL 4); 1.2.5/1.2.5n change length |
| `GM_MazeWell_update` | 1.3 .. 2.7 all identical (EN 0 / PAL 14); 1.2.5/1.2.5n change length |
| `saveCardBuildComment` | 1.3 .. 2.7 all identical (PAL 0 / rev1 3); 1.2.5/1.2.5n worse |
| `bossdrakor_update` | only 1.3 is viable at all; 1.3.2 and later are +187 instructions |

So for these units GC/1.3 through 2.7 are output-identical and the axis carries no information. Together
with declaration order, the local set, expression spellings, the twelve `-opt`/`-inline` profiles and the
value-structure casts, every knob this project has is now measured on all five rows.

The one instrument that is NOT exhausted is understanding: `coloring_order()` shows the colouring follows
a compiler-internal linked worklist, and nothing here derives the rule that builds it. Until that rule is
known, moving these five is guesswork over value structure -- which is how `loadMemCardImages`,
`pauseMenuDraw`, `engine/53` and `gametext` were actually closed, and it did produce PAL 25 -> 12 on
gameloop before stalling.

## The per-function pragma axis: one win, four refusals

With per-function pragmas authorised, eighteen MWCC optimization pragmas were swept on each remaining
row across all five versions (`opt_common_subs`, `opt_propagation`, `opt_lifetimes`,
`opt_strength_reduction`, `opt_dead_assignments`, `opt_dead_code`, `opt_unroll_loops` off and on,
`peephole on`, `scheduling on`, `optimization_level 0..4`, `dont_inline on`, `opt_vectorize_loops off`,
`no_register_coloring on`).

**`wclevelcont_update` closes.** `#pragma opt_common_subs off` around that one function takes the unit
to 100% on all five versions, because `traceMoveA`/`traceMoveB` keep CSE and still match. That is now
applied; see the commit for why the TU-wide flag and the source alternatives do not work.

**The other four refuse every pragma.** `askProgressiveScanMode` stays at 25, `GM_MazeWell_update` at
14, `saveCardBuildComment` at 3 and `bossdrakor_update` at 150 under every pragma that does not change
the instruction count. The product space was also swept for gameloop: the ten viable pragmas x the
`(u8)` cast are all identical at EN 24 / PAL 12, so the pragma axis and the value-structure axis do not
interact there.

State after this: EN v1.0 and JP at 100, PAL v1.0 99.497430, PAL v1.1 99.497490, EN v1.1 99.537820, and
all five DOLs byte-identical. Four functions remain, worth 0.5026 on PAL v1.0, 0.5025 on PAL v1.1 and
0.4622 on EN v1.1.

Knobs now measured and closed on those four: declaration order (single moves, all pairwise swaps,
exhaustive permutations where feasible, scope hoists, ungated greedy and randomised search), the local
set, expression spellings, twelve TU `-opt`/`-inline` profiles, ten per-unit compiler versions, eighteen
per-function pragmas, and the pragma x value-structure product. What is left is the rule that builds the
compiler's colouring worklist.

## See also

- `docs/source_shape_levers.md` — levers 9, 14 and 16 are the ones this frontier keeps invoking.
- `docs/band_width_worklist.md` — the width screen that correctly called six of these seven flat.
