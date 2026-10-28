# Purge-campaign audit — what each purge commit stated, and what it cost

Every purge-shaped commit in the hack-purge campaign (2026-07-28 .. 2026-08-02, 42 commits)
rebuilt at BOTH of its own endpoints and diffed on every axis a match can be lost on, using
`tools/score_delta_gate.py`. This is the ledger those rebuilds produce. It supersedes any
per-commit self-report that disagrees with it.

## Method

For each commit, a full `ninja all_source` + `ninja build/GSAE01/report.json` at the commit AND
at its parent — 79 endpoints in all — then `score_delta_gate.py --reports` over each parent/child
pair. The gate's schema guard and differ-layer control run on every comparison and passed on
every one; its `--self-test` was 20/20 when the ledger was built and is 33/33 now. Endpoint
reports were produced by a content-synced incremental walker rather than 79 from-scratch
worktrees; the walker was validated by building the same sha both ways and asserting the two
`report.json` files are byte-identical.

Two independent cross-checks say the ledger is measuring the right thing:

- it reproduces the owner's own corrections to the byte and to the last digit —
  `f596800ffa` +4936 B / +0.007680 exactly cancels `d59088213a`, `8cdd8d2791` +16 B exactly
  cancels `620b69dc2d`, `6a3b8d684a` +64 B / +0.001320 exactly cancels `09bb75cc96`;
- it reproduces `4461d0aa45`'s three stated figures (`matched_code +0`, `matched_functions +0`,
  `matched_data -272`) exactly, while also showing the eight per-function rows those three
  counters cannot see.

## Ledger

`ddata` = summed per-unit `matched_data` delta. `REG` = per-function `fuzzy_match_percent`
regressions. `DEM` = units demoted (complete true -> false).

| Commit | ddata | dfuzzy | REG | DEM | Verdict |
|---|---:|---:|---:|---:|---|
| `4846fe928d` | +0 | +0.000000 | 0 | 0 | clean |
| `9d28838e66` | +0 | +0.000000 | 0 | 0 | clean |
| `68b6000836` | +0 | +0.000025 | 0 | 0 | clean |
| `5d467157cb` | **-144** | +0.000000 | 0 | 0 | **RED** |
| `f5fe00213f` | **-60** | +0.000000 | 0 | 0 | **RED** |
| `6983af1b5a` | +0 | +0.000110 | 0 | 0 | clean |
| `953103973c` | +0 | +0.000000 | 0 | 0 | clean on the score axes, **RED on the pool word-diff** [^pool] |
| `c5f7503917` | +0 | +0.000000 | 0 | 0 | clean |
| `8f575d63b5` | +0 | +0.000000 | 0 | 0 | clean |
| `fddaac2ca5` | +0 | +0.000000 | 0 | 0 | clean |
| `e3677e7823` | +0 | +0.000000 | 0 | 0 | clean |
| `0ef2b008a0` | +0 | +0.000000 | 0 | 0 | clean |
| `441a126ec0` | +0 | +0.000000 | 0 | 0 | clean |
| `e4415eaee2` | +0 | +0.000000 | 0 | 0 | clean |
| `f4ebff3ffa` | +0 | +0.000000 | 0 | 0 | clean |
| `6b3e5f48ef` | +0 | +0.000000 | 0 | 0 | clean |
| `676a92c068` | +0 | +0.000000 | 0 | 0 | clean |
| `88823247ad` | +0 | +0.000000 | 0 | 0 | clean |
| `ffe9ac9dee` | +0 | +0.000000 | 0 | 0 | clean |
| `72eec6655f` | **-1684** | +0.000000 | 0 | **4** | **RED** |
| `4be5cb0359` | +144 | **-0.000156** | 1 | 0 | **RED** |
| `4f2c298b73` | +0 | **-0.001810** | 1 | 0 | **RED** (self-declared) |
| `248cc29dc2` | +0 | **-0.000530** | 1 | 0 | **RED** |
| `bea1487c75` | +0 | +0.000020 | 0 | 0 | clean |
| `c2e9f24453` | +0 | **-0.007094** | 3 | 0 | **RED** |
| `646cd1e9b7` | +0 | **-0.001766** | 1 | 0 | **RED** |
| `2a5f5897d7` | +0 | +0.009160 | 0 | 0 | clean (retune) |
| `d59088213a` | **-4936** | **-0.007680** | 6 | 0 | **RED** |
| `f596800ffa` | +4936 | +0.007680 | 0 | 0 | clean (retune) |
| `620b69dc2d` | **-16** | +0.000000 | 0 | 0 | **RED** |
| `8cdd8d2791` | +16 | +0.000000 | 0 | 0 | clean (retune) |
| `5159cf60ca` | +0 | **-0.007840** | 3 | 0 | **RED** |
| `09bb75cc96` | **-64** | **-0.001320** | 3 | 0 | **RED** |
| `6a3b8d684a` | +64 | +0.001320 | 0 | 0 | clean (revert) |
| `ca7470b8b1` | +0 | +0.000450 | 0 | 0 | clean (retune) |
| `5f09825740` | +0 | +0.007840 | 0 | 0 | clean (retune) |
| `21b90aff9f` | **-992** | **-0.000660** | 3 | **13** | **RED** |
| `5b120c0545` | +0 | **-0.000450** | 2 | **2** | **RED** |
| `4461d0aa45` | **-272** | **-0.003674** | 8 | **1** | **RED** (self-declared) |
| `a76a6b85ac` | +0 | +0.000000 | 0 | 0 | clean |
| `0f24c49e82` | +0 | +0.000000 | 0 | 0 | clean |
| `08c31701b2` | +0 | +0.000000 | 0 | 0 | clean |

[^pool]: The verdict column records the four score axes, which is all the gate could read when
the ledger was built. `953103973c` is the one row where the later pool word-diff disagrees with
them; the counts below are the score-axis counts and are unchanged by it. See "the blind spot the
score axes cannot see" for the measurement.

**27 clean, 15 RED.** Gross loss over the campaign: **-8168 B** `matched_data` and
**-0.032980** tree fuzzy. The owner's own retunes and reverts gave back +5016 B and +0.026450,
which is most of it — the campaign was self-correcting wherever anybody looked. The residue is
what nobody looked at.

## The unrecorded residue, and where it still stands

Three commits carried a loss still standing at the time of the audit. One of them no longer does.

| Commit | Loss standing at audit | Standing now | Where |
|---|---:|---:|---|
| `72eec6655f` | 1288 B, 2 demotions | **0** | player, tricky, 237 all recovered — see below (modellight -88 recovered earlier, `558c86a421`) |
| `21b90aff9f` | 920 B, 13 demotions, 3 fns | 920 B | `priced_classes` 6 — banked |
| `4461d0aa45` | 272 B, 1 demotion, 8 fns | 272 B | `priced_classes` 6b — banked |

The columns close against the gross both before and after. At audit time: 5016 B given back by the
owner's retunes and reverts, 2480 B still standing, 672 B absorbed by later pool work —
5016 + 2480 + 672 = 8168. The 672 decomposed into nine recovered rows: SB_Galleon 80, engine/86 40,
DFropenode 24, objprint 40, 285 20, KT_Rex 164, DIMSnowHorn 144, modellight 88, 203 72. With
`e500fdb6cb` and `997e72e3e1` the recovered column grows by the whole 1288 to **1960** and the
standing column falls to **1192**, which is exactly the two banked rows: 5016 + 1192 + 1960 = 8168.
**Everything the campaign lost that is recoverable inside policy has now been recovered**; what
remains is priced. (tricky's shortfall at tip was 408, of which 404 is `72eec6655f`'s; the other
4 B arrived with a later change in the unit's `total_data` and was not attributed. Restoring the
bodies closed all 408.)

`21b90aff9f` and `4461d0aa45` were already priced. **`72eec6655f` was not, anywhere.** It is the
largest single unrecorded loss in the campaign — larger than `21b90aff9f` — and it shipped with
an **empty commit body**: no measurement was stated, so no measurement was wrong, so nothing
downstream had anything to check. It deleted seven uncalled static helpers across six units and
took `matched_data` 1191719 -> 1190035 with four demotions.

Rows recovered by others since, and not to be re-litigated: `5d467157cb` (SB_Galleon, engine/86),
`f5fe00213f` (objprint), `72eec6655f` (KT_Rex, DIMSnowHorn), `4be5cb0359` (sky2_run, now
99.65298), `4f2c298b73` (renderSunAndMoon, back to 99.476), `c2e9f24453`/`5159cf60ca`/`646cd1e9b7`
(newshadows + textrender), `d59088213a`, `620b69dc2d`, `09bb75cc96`.

One row is partially open and is NOT a pool row: `248cc29dc2` (remove global-register reservation
hack) cost `modelRenderInterpolateRootTransform` 96.465 -> 95.774 undeclared; it stands at 96.365,
still 0.100 below its pre-commit score. The removal was policy-correct — the shape was a
`register ... asm("rN")` allocator hack — so this is a priced row, not a revert candidate.

## Three sensor blind spots, all confirmed by measurement

1. **Demotion.** Demoting a unit removes it from the DOL link, so the forced-link gate cannot
   see the loss. `21b90aff9f` (13 demotions), `5b120c0545` (2), `72eec6655f` (4), `4461d0aa45` (1).

2. **Threshold counters.** `matched_code` and `matched_functions` move only when a function
   crosses 100.0. `4461d0aa45` moved neither and still knocked eight `trig` functions off
   99.97-99.98, worst 99.981 -> 94.212.

3. **Pool rotation in an already-NonMatching unit** — not covered by either of the above, and the
   one this audit adds. `5d467157cb`, `f5fe00213f` and `620b69dc2d` lost 144, 60 and 16 bytes with
   `dfuzzy +0.000000`, **zero** per-function regressions and **zero** demotions: the units were
   already NonMatching, so nothing was demoted, and literalising a foreign-TU pool read mints a
   fresh `@N` anon that rotates the unit's own `.sdata2` without touching a single instruction.
   Only per-unit `matched_data` sees this class. `5d467157cb`'s own commit body identifies the
   mechanism — "literalising there mints a NEW anonymous @N entry ... grows the section past its
   retail claim" — but applies the guard only to MatchingFor/Matching units. The same minting
   costs `matched_data` in a NonMatching unit; it just costs it silently.

   Even `matched_data` only half sees it, because `matched_data` is all-or-nothing per section: a
   section already short of 100 can be rotated arbitrarily further from its carve for free.
   `tools/score_delta_gate.py` therefore now carries a **pool word-diff** that is not derived from
   any score — it reads our built object and the retail carve straight out of the ELF and compares
   their `.sdata2` / `.sdata` / `.rodata` / `.data` sections word for word, positionally and as a
   multiset. A rotation preserves the multiset exactly, so it is the positional-hit count that
   collapses; an outright word loss moves the multiset shortfall instead. Re-running the three
   commits above through it, all three come back RED on the word-diff alone: `5d467157cb`
   **8 rows** where `matched_data` moves in 3 units, `f5fe00213f` **5 rows** where it moves in 2,
   `620b69dc2d` **1 row**. The rows in excess of the `matched_data` rows are units whose pool grew
   past its retail claim inside a section that was already scoring zero — precisely the
   free-of-charge drift this blind spot is about, and invisible on every other axis. Each of the
   three also drops positional hits (`SB_Galleon` 20 -> 0, `285` 5 -> 0, `object` 4 -> 2), so the
   verdict does not rest on the growth rows alone.

   Two commits the table above calls clean were re-run as negative controls. `08c31701b2` stays
   clean on the word-diff too. **`953103973c` does not**: it turns RED on one row,
   `engine/68 .sdata2 extra 8 -> 28`, at `ddata +0`, `dfuzzy +0.000000`, zero regressions and zero
   demotions — the exact shape of this blind spot. Its own commit body explains it without
   realising: it literalised "20 of the 23 pool-const externs", and each of those was a read of
   *another* TU's pool, so each literal minted a word in `engine/68`'s own `.sdata2` — twenty of
   them, which is the delta to the word. The commit's three stated figures are all true and all
   blind. This is **not** a standing loss: a later re-split carved those words into the unit's own
   claim, and at tip `engine/68 .sdata2` scores 96.875 with a single word missing (the `120.0f` at
   `0x44` that commit deliberately held back). The table's verdict for `953103973c` should be read
   as clean-on-the-score-axes, RED on the word-diff, and the row now says so.

   Re-verified independently by building only `engine/68` at both endpoints and counting the
   words: at `6e8a7e1af8` our `.sdata2` is 8 words against a carve of 0, at `953103973c` it is
   28 against 0 — surplus 8 -> 28, the twenty literals to the word. The carve claimed no
   `.sdata2` for the unit at that time, which is why every minted word was surplus and why the
   later re-split is what absorbed them.

A fourth is not a sensor problem: `09bb75cc96` stated "Zero cost, and no residue" and quoted
per-unit `matched_code` figures, but actually moved `matched_code` **-2584** and
`matched_functions` **-3** along with -64 B and three regressions. Even the counter it did gate
on had moved. That is the dead-sensor failure `169d26bd42` retracts, reaching one commit further
back than that retraction covers.

## The recovery law, and what it costs to state it

`558c86a421` recovered `modellight` in full, and the mechanism generalises. Restoring
`72eec6655f`'s deleted helper was **not** sufficient on its own: the TU also carried
`const f32 gModelLightMaxProjectionFarZ[1] = {500.0f}`, a one-element pool anchor, and MWCC does
not intern a file-scope const against a pool literal, so the two spellings together emitted
500.0f twice and shifted every later word by four. Retail's pool has it once.

> **When restoring a deleted literal-minter, look for the one-element anchor in the same TU that
> was minting the same value. The helper and the anchor are two reconstructions of one retail
> object; keeping both is worse than keeping neither. Delete the anchor.**

That direction matters: it removes a banned `SINGLE_ELEM_CONST_ARRAY` and the restored body is a
`UNCALLED_STATIC_FN`, which section 7 already establishes is not automatically a hack. The
adjudication is stronger here than section 7 requires — the restored bodies take `.sdata2` from
87.209 to a byte-exact 100.0 against retail, which is direct evidence that retail's TU minted
those literals at exactly those points.

**These rows are now CLOSED. The `72eec6655f` residue is zero.**

| Unit | Was | Deleted statics | Closed by |
|---|---:|---|---|
| `src/dlls/objects/237/237.c` | -100 B, demoted | `collectible_getRotX`, `collectible_updateSeqEffects` | `e500fdb6cb` |
| `src/dlls/objects/195_Player/player.c` | -784 B | `playerIsInDeepWater` (10.0f), `playerIsAtFullSpeed` (1.0f) | `997e72e3e1` |
| `src/dlls/objects/196_Tricky/tricky.c` | -404 B | `trickyEventTimeExpired`, `trickyApproachSpeedStep`, `trickyRouteTurnRate`, `trickyRouteStep`, `trickyBinAngleToRadians` | `997e72e3e1` |

`src/dlls/objects/202/202.c` was also cut by `72eec6655f` but measures no data delta; its two
deletions were `static inline`, which mint nothing.

`997e72e3e1` took `matched_data` +1192 (784 + 408) with both `.sdata2` sections **byte-identical
to the carve**, `.text` untouched in both, tree fuzzy and `complete_units` unchanged, 0 regressed
/ 0 lost / 0 demoted, `main.dol` md5 still retail's.

**The order law it turns on.** Neither pool was missing a single word: `tricky` differed from the
carve in **zero** words and 101 of 102 *positions*, `player` in zero words and 8 of 196 positions.
When the multiset already matches, nothing needs declaring — the only thing wrong is the **mint
point**, and the mint point is a function's position in text order. That is why the section-8 rule
(MWCC does not intern a file-scope const against a pool literal) does not bite here: restoring a
body mints nothing new, because MWCC *does* intern plain literals across a TU. **Restore is not
declare.** Check the multiset before assuming a loss; if it is flat, you are looking at a deleted
function, not a missing constant.

Reading the minters off the carve is then mechanical. `player`'s `0x6c..0x78` run is
`2.0, 10.0, 300.0, 1.0`; live code owns the `2.0` (`playerIsInWater`) and the `300.0`
(`playerFindNearestFirefly`) and they sit three slots apart, so exactly two minters lived between
them, in that order — which is the order the purge deleted them in. `tricky`'s `0x50..0x98` is
five consecutive groups, `(-100000, 8.0) (-0.15, 0.05) (0.02, 600.0, 0.005) (-2.0) (pi, 32768)`,
one per deleted body, in file order. A contiguous constant group with no live loader is a deleted
function's signature, and the group's order says where the body goes. Restoring all five in
`tricky` also moved the section's double-alignment pad from mid-section to the end, taking the
size 404 -> 408: an off-by-one-word size can be a pad in the wrong place, not a missing constant.

Neither TU carried a competing one-element anchor, so this is the **plain-restore half** of
`558c86a421`'s law; the anchor-delete half applies only where the TU has both spellings. The seven
bodies are baselined as `UNCALLED_STATIC_FN` under section 7 with the byte-exact pools as the
evidence — the strongest form available, since a byte-exact pool proves retail's TU minted at
exactly those points. `banned_shapes_check` regrowth was unchanged at 14. Where a helper has a
real inline call site, prefer `e500fdb6cb`'s idiom instead and wire the caller: it costs no
baseline entry at all.

## Standing recommendation

The purge lane's gate should be `tools/score_delta_gate.py`, not a bare rebuild: per-function
fuzzy AND per-unit `matched_data` AND the complete flags, over full rebuilds at both endpoints.
Two of the three blind spots above return an honest-looking `+0.000000` from a correctly written
harness that simply is not looking at `matched_data`, and the third returns it from a harness
that is not looking at per-function fuzzy. A purge whose price is real still has to be measured
and banked — the campaign shows the retunes land promptly whenever the price is visible at all.

**And run md5 of every `.o` beside it.** The gate's four layers are all derived from a comparison
against *retail*; md5-of-every-`.o` is a comparison against **the previous build of ourselves**, so
it is the only check that answers "did this window change anything it did not claim to change?".
It strictly dominates the score axes: any regression that moves a byte moves an md5, and it also
catches the class no score can see — a rewrite that is content-identical and score-flat but
renumbers the local pool symbols (`@103 -> @105` at the same address, section and size; class #70).
`8722b792b1` measured exactly that: a candidate include would have rewritten **33 DLL objects** at
zero score delta, and only the md5 count made the cost visible in time to reject it. The two
2026-08-03 watch windows show both readings — `f63cb3dc08..cb88387945` came back gate-CLEAN *and*
1013 of 1013 objects md5-identical, which is a far stronger statement than the scores alone, while
`590dce7361..f63cb3dc08` came back gate-RED on one row (`setGQR6` 70.000 -> 50.000, banked in
`docs/priced_classes.md` §5) against a net `matched_data` +52 and three pool word-diff
improvements. Neither verdict is reachable from the top-level `measures` block, which moved by
`-6e-05` across the second window and not at all across the first.
