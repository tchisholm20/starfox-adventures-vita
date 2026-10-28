# The allocation model — saved-register band assignment

What decides which value lives in `r31`, `r30`, … `f31`, `f30`, …, how much of it is
predictable, and where prediction provably stops.

**Status: closed at a named boundary.** The within-tier laws are confirmed on held-out real
code and are usable. Cross-tier order above band width 4 is **not** predictable from
post-allocation machine code — that is a measured result, not an untried axis, and it replaces
the older "wide bands are unsteerable chaos" doctrine with a mechanism.

---

## 1. The confirmed model

The band is populated by four distinct classes. The class of a value, not its position in the
source, is the primitive.

| class | definition | within-tier key | held-out accuracy |
|---|---|---|---|
| **R** return copy | copied from `r3`–`r10` **after** the first `bl` | first-definition order | **62–75%** at widths 5–10 (67.1% pooled) |
| **L** load | materialized by a load, computation or constant | declaration order | not object-side scorable (see below) |
| **P** param copy | copied from `r3`–`r10` **before** the first `bl` | ABI source register | **50–68%** at widths 5–10 (73.7% pooled) |
| **sqrtf** | produced by an inlined helper returning via a `volatile` stack slot | assignment order | rare: **0.2%** of functions |

At width 2 the within-tier keys are near-exact (R 100%, P 91.8%). **They do not decay with
width** — this is the single most useful result in the file, because it means the wide-band
regime is not orderless, it is orderless only *across* tiers.

**State the direction explicitly.** Reading the band **bottom-up** (lowest home first), both
copy tiers run *ascending* in their key. Read top-down (`r31` first) they run *descending*.
An earlier scoring got 9.8% and 11.0% purely from reading the enumeration the other way, and
61.6% / 77.8% once corrected. The law is worthless without its direction attached.

**A copy is not always `mr`.** MWCC also emits `addi rX,rY,0` as a register move. Classifying
that as a load inverts the partition and manufactures results; it was caught by hand-checking
three functions against disassembly, on the third.

### Membership: most of what looks like a band member is not one

A value is a **member** only if it is *held across a call*. Measured over the retail image
(28,066 candidate registers):

- **13.4% are phantoms** — `li` constants, `(int)&global` addresses and cheap param
  computations that are **rematerialized after calls, never held**. They are not band members
  at all. An extractor that counts them scores a value that does not exist.
- **9.8% are recycled** — the register holds two values with disjoint live ranges. **45.8% of
  those are cross-class** (a load early, a return copy late), which under first-write
  attribution reads as "load above return" and invents inversions.

Nearly **a quarter** of naive band members are one of these. Both must be excluded, and pair
comparisons are meaningful only between values that are **simultaneously live** — order is
undefined for values that never coexist.

### The rematerialization-cost axis

The "load class" is not one population. Pairwise (load, param), chance 50%, over matched code:

| load kind | n | above the param |
|---|---|---|
| memory load (`lwz/lbz/lfs/…`) | 3694 | **80.7%** |
| constant (`li/lis`) | 471 | 58.8% |
| computed (`add/or/slwi/…`) | 835 | 56.6% |
| arithmetic `addi/addis`, **no relocation** | 311 | 50.8% — chance |
| **address materialization**, `addi/addis` **carrying a relocation** | 48 | **33.3% — inverted** |

Under pressure the allocator protects values that are *expensive to free*: a param copy must be
spilled, a load can be rematerialized, so loads demote first — and the cheaper a value is to
rematerialize, the more readily it demotes. The gradient is that cost ordering.

> ### ⚠️ Correction — the original `addr` row was a mislabelled blend
> This table was first published as a four-way split with a single **address** row at
> **52.6% — chance**, and that number was read as "address materializations are unmodeled",
> convergent with CLAUDE.md's *"no named local behind it … array base … unreachable from
> source"*. **That reading was wrong.** The classifier treated **every** `addi`/`addis` as an
> address materialization. Measured against the actual relocations, **87.4% of that bucket
> (847 of 969 members) is plain arithmetic carrying no relocation** — only 12.6% are true
> address materializations.
>
> Split correctly, the bucket separates into two different things: non-reloc arithmetic sits at
> **50.8%, genuinely at chance**, while true reloc-bearing addresses sit at **33.3% — below
> chance, i.e. systematically toward the BOTTOM of the band, under the params.** An inverted
> signal is not an absent one.
>
> Consequences, stated plainly: the claimed two-independent-derivations convergence with
> CLAUDE.md's array-base note **does not hold** — that note may still be true, but this
> measurement is not evidence for it. And any census of "unmodeled, `L_addr`-dominated"
> functions built on the old classifier is counting mostly arithmetic: the true address
> population is **122 members image-wide**, with only 48 co-live address/param pairs.
>
> **If you are writing a value classifier, do not copy the naive form.** `addi`/`addis` is an
> address materialization **only when the instruction carries a relocation**; otherwise it is
> arithmetic. The check is two lines against `objdump -drz` output, and skipping it inflates the
> class ~8x and silently averages two populations that behave differently. Every tool in this
> session's record already carries the fix; the hazard is new tooling reproducing the old shape.
>
> **Follow-up: the "fourth tier at the bottom" lead is REFUTED — do not reopen it.** The
> corrected numbers suggested the stack might extend to `R > L_mem > {L_const,L_comp} > P >
> L_ADDR`. Grown on TRAIN with the reloc-aware classifier it does not survive, and the
> positional test contradicts it outright (95% Wilson intervals, chance = 50%):
>
> | test | k/n | point | 95% CI | verdict |
> |---|---|---|---|---|
> | ADDR **above** param | 8/16 | 50.0% | [28, 72] | no effect |
> | ADDR above return copy | 39/87 | 44.8% | [35, 55] | no effect |
> | ADDR above memory load | 4/9 | 44.4% | [19, 73] | no effect |
> | ADDR above **constant** | 14/19 | 73.7% | [51, 88] | **above** |
> | ADDR above **computed** | 11/13 | 84.6% | [58, 96] | **above** |
> | **ADDR is the band's LOWEST member** | 6/38 | **15.8%** | **[7, 30]** | **against a bottom tier** |
>
> The core claim (addresses below params) sits at exactly chance with an interval spanning it.
> The two comparisons that *do* reach significance point the **opposite** way — addresses sit
> **above** the cheap-to-rematerialize classes — and addresses are the band's lowest member only
> 15.8% of the time, significantly *below* what a bottom tier requires. The retail-image 33.3%
> (16/48, CI [22, 47]) that produced the lead does not replicate on TRAIN and came from a
> different population (it included unmatched functions).
>
> **The true-ADDR population is simply too small to carry a tier claim: 38 members in TRAIN,
> 122 image-wide.** The model declines to place this class, and that is now a measured
> refusal rather than an untested gap.

### The arrival principle

A parameter reloaded after a call **leaves the param tier** — class follows how the value
*arrives* in the register, not what it originally was.

---

## 2. The boundary, stated as law

> **Cross-tier band order above width 4 is not predictable from post-allocation machine code.**

Scored on the retail image (an independent corpus, disjoint from the derivation set), pairwise
on co-live modelled pairs:

| w | fns | pairwise | per-function | old recorded rule |
|---|---|---|---|---|
| 2 | 2001 | 83.9% | 83.9% | 97.7 |
| 3 | 1665 | 73.3% | 62.8% | 99.3 |
| 4 | 822 | 55.1% | 31.1% | 98.8 |
| 5 | 510 | **42.0%** | 13.4% | 1.4 |
| 6 | 309 | **35.7%** | 12.0% | 0.1 |
| 7 | 141 | 34.0% | 2.3% | — |

Above width 4 it is not weak, it is **anti-predictive** — below the 50% chance line. The model
beats the old rule 10–120× at widths 5–6 and is substantially worse at 2–4; it replaces nothing.

**What the falsification survived**, in order: an independent corpus; three attribution
corrections (phantoms, recycling, cross-class recycling); restriction to co-live pairs; a
five-way population split (game 38.5%, audio 49.2%, SDK 49.2%, matched 36.4%, unmatched 47.8% —
all at or below chance, and *matched functions are no better than unmatched*, ruling out
tuned-on-the-outcome bias); an extractor bug fix that moved no digit; and a pressure-conditional
model tested on two independently-constructed proxies, stratified by width against the size
confound. The reversal is not in the compiler — the rig refuted that four ways.

### The pressure mechanism — rig-confirmed, object-side unrecoverable

Holding the band fixed at width 6 and varying only simultaneously-live scratch temporaries,
Lane B's probes show a sharp threshold: **K≤6 gives `R>L>P`; K=7 transitions; K≥8 gives
`R>P>L`** — params promoted above loads. Confirmed against an independent pressure source and
an indexed variant; a back-edge/loop explanation came back null.

**This is true of the compiler and unusable for prediction.** Pressure is a property of the
*pre-allocation* IR. Post-allocation code shows the allocator's **solution**, not its input —
and high pressure is discharged *precisely by moving values into the saved band*, the structure
we are trying to predict. Two proxies confirm it is unrecoverable: scratch-liveness at
definition points is degenerate (92% of wide-band functions at K∈{0,1}, **none at K≥8**, so the
rig's threshold has no population), and spill traffic — the correct residue-based proxy —
produces **no partition** (`R>L>P` vs `R>P>L` within 2 points in every width stratum, nothing
near the 60% recovery line).

### The orderliness effect — the model's one open question, fenced on four sides

Spilling functions score ~9 points higher than zero-spill functions under **both** stacking
orders (pooled 34.1% vs 43.1%). Pressure organises the band without selecting between orders.
**Four hypotheses have been tested and all four are dead** — this is a named mystery, not an
unexamined gap. Do not re-propose any of these:

| # | hypothesis | how it died |
|---|---|---|
| H1 | recycling suppresses ordering | refuted in the synthetic rig |
| H2 | pressure forces an order | refuted in the synthetic rig |
| P10 | **recycling density** is the collapsing variable | gap persists at fixed density (mean 7.0 vs pooled 8.7) |
| P11 | **kind-mix population shift** (H3's actual claim) | gap persists at fixed kind-mix, signed +3.0/+8.0/+6.0/+16.1 |
| H5 | **constant register shift** displaces correct assignments | **refuted a priori**: the metric scores `(a if ra<rb else b)==max(a,b)`, a comparison between register *numbers*. A constant shift preserves every pairwise comparison, so shifted functions score **perfect**, not zero. No re-binning can rescue it. |
| P12 | **occupant contamination** (H5's testable steelman): one unmodeled member carries a rank and generates wrong pairs | headline 50.3% single-occupant-explained looked CONFIRMED — **killed by its own control**: the rate is confounded with pair count (92.0% at 0–4 co-live pairs, 51.3% at 5–9, **2.7% at 15–19**). Removing one member from a 3-pair function trivially clears it. In the pair-rich regime the rate is below the registered 15% DEAD line. |

**Two structural facts any future attack must explain.**

*It is not single-signed.* Stratified by recycling density the gap **reverses**: +7.7 and +8.3 in
the low strata, **−4.9** at [0.15, 0.30) where zero-spill functions jump to 47.8%. An effect that
flips sign across strata is not a main effect, and marginal stratification — which is all P10 and
P11 were — cannot resolve an interaction. That reversal does **not** trace to kind-mix (the cell's
mean memfrac is 0.366 against 0.414 elsewhere).

*Kind-mix amplifies rather than absorbs it.* The gap is **largest (+16.1)** in the pure-memory-load
stratum, where zero-spill orderliness is the corpus **worst (16.6%)**. That is the opposite
direction from the kind gradient, which ranks memory loads the *most* orderly kind (74.9%
pairwise). The gradient is a per-**pair** property and this is a per-**function** one; the tension
between the two levels is unexplained and is the sharpest lead on record.

### P13 — the interaction design is INFEASIBLE at this corpus size (the terminal)

Every prior test was *marginal*, and marginal designs cannot resolve an interaction. P13 ran the
two-way designs — kind-mix × spill within recycling bands, and recycling × spill within kind bands
— with cell inclusion registered at n ≥ 20 (both arms) and testability at ≥ 4 qualifying cells in
one holding band.

**Neither design reaches 4 qualifying cells in any band** (best: 3). The corpus of 994 width-≥5
functions cannot support the design. That is the mystery's true data boundary, and it is where the
investigation stops — not for want of ideas, but for want of n.

**The one observation the qualifying cells carry** (reported, *not* a finding — the design that
would confirm it is the infeasible one): both designs converge on the same single cell.
**`mf=0 × rd=0` — no memory loads, no recycling — is the only negative gap in the entire table
(−6.6)**, surrounded by +9.3, +12.4, +13.2, +15.5. The P10 sign reversal localizes there.

**What a future attempt needs:** the binding constraint is zero-spill counts in the memory-heavy
strata (n = 4, 14, 17 against the required 20). Roughly 3–5× the current width-≥5 population, or a
lower band-width threshold to admit more functions. State the n before designing the test.

**A third methodological trap, from P12:** any "does removing one X fix it" statistic is
confounded with how many things there were to fix. Control against the denominator before reading
such a rate — the headline was 50.3% and the pair-rich truth was 2.7%.

**Methodological note carried with it:** report **signed** gaps per cell, never a
mean-of-absolutes. P10's summary statistic was generous precisely because a sign flip was present;
P11's criteria were registered on signed gaps for that reason.

---

## 3. What this means for matching work

- **The parameter-order lever is usable now.** The P tier is keyed on ABI source register, and a
  callee's parameter list determines which ABI register each parameter arrives in — and
  CLAUDE.md already records that reordering a parameter list is ABI-neutral. That makes
  parameter order a genuine steering knob for saved homes in param-heavy functions. It is the
  campaign's one actionable product; A/B it, never assume it.
- **Within-tier reasoning only.** Any tool or argument that needs *cross-tier* order at width ≥5
  is unsupported. Scope verdicts to within-tier, and emit `unmodeled` — not `diagnostic` — for
  the rest. At width ≥5, **20–42% of pairs involve an unmodeled `addr` member** and are
  excluded by construction, so even a repaired cross-tier law would leave a large residue.
- **A flat declaration sweep is a membership measurement, not a dead end.** Pure load bands are
  perfectly declaration-keyed and fully injective at widths 5–8 in the rig (120 permutations →
  120 distinct outcomes). If a real sweep is flat, the band is *not* pure-load: the values you
  permuted are phantoms, recycled, or in another tier. The cliff is a **population** property —
  wide real-code bands are dominated by non-load members — not a compiler property.
- **"Wide-band, closed" dispositions stand, and their reasons upgrade.** They were closed for
  "band too wide to model"; they are now closed for a stated mechanism, which means a future
  attack knows exactly what it must beat.

---

## 4. Method appendix

This campaign is the project's worked example for an allocator-shaped question. The discipline
is the transferable part.

- **Pre-register with falsification conditions before each run.** Every claim (P1–P8) was written
  down with what would refute it *before* the measurement. P2 refuted its author's own unifier;
  P6, P7 and P8 all returned nulls that stuck because their thresholds were fixed in advance.
- **Reserve a holdout, declare the split before a predictor exists, score once.** Deterministic
  hash split, derivation on TRAIN only, one scoring pass on TEST. When a "free" holdout was
  proposed from newly-matched functions it was **zero rows**, and would have been invalid anyway
  — selected on the dependent variable, since source tuned until bytes match measures lane
  convergence, not law generality. The **retail objects** are the right holdout: large, disjoint,
  and immune to that bias.
- **A taxonomy is an instrument too.** Three defects were caught in *value classification*, each
  of which silently changed a headline: `addi rX,rY,0` misread as a load (inverts the
  copy/load partition), substring register counting, and — found only when a probe target's
  disassembly was read directly — **every `addi`/`addis` classified as an address
  materialization**, which turned out to be 87.4% arithmetic and produced a published
  "unmodeled at chance" result that was really two classes averaged together. Bucket names are
  claims; check them against something external (here, the relocations) before trusting a row.
- **Validate the instrument against ground truth before any number counts.** Retail-side
  extraction was required to reproduce source-side extraction function-for-function: **9276/9276,
  100.00%**. Three separate defects were caught by hand-checking a handful of cases —
  `addi rX,rY,0` misclassification, substring register counting (`ops.count("r3")` matching
  `r30`), and a whole-repo scan where only `src/` was intended. Each looked correct until checked.
- **The degeneracy trap — a variable definitionally consumed by the thing being measured.**
  Two instances: *live-across-call* (558/561 — true of saved-band members by definition) and
  *scratch-liveness at definition points* (92% in {0,1}, because pressure is discharged into the
  band). Generalized: **a pressure proxy must observe the regime's residue (spills), not the
  allocator's answer to it.** Check that a candidate variable *can vary* before spending a run.
- **Amend a proxy blind, and escalate rather than decide.** When the registered proxy proved
  degenerate, the swap was made after seeing **only its distribution**, with zero accuracy
  computed against any candidate, and was approved before use. Swapping after seeing outcome data
  is fitting; swapping after seeing distribution is protocol repair.
- **Two designs, no peeking.** Observational corpus mining and synthetic probes ran without
  seeing each other; agreement crowned a law, disagreement localized a missing variable. The
  kind gradient and the rig's swap turned out to be one mechanism from two directions.
- **Rate is not count.** The composition dominating a miss census may do so by being the largest
  population, not the worst rate. Conflating them targets the wrong fix.

---

## Attribution

Observational corpus mining, held-out scoring, membership/liveness machinery, kind gradient,
degeneracy findings, retail confirmation and the P8 null: this lane. Synthetic probe families,
the phantom/recycling/cross-class mechanisms, the arrival principle, the K-threshold pressure
inversion and the pure-load injectivity result: Lane B. Convergence protocol and adjudication:
coordinator.

## See also

- `CLAUDE.md` "A few MWCC facts" — the saved-band summary this file supersedes for widths ≥5.
- `docs/band_width_worklist.md` — where a structural fix can stick; its width screen is the
  entry point to this model.
- `docs/source_shape_levers.md` — the source-shape axis, including `fn_flag_probe` as screen
  step 1.
- `docs/data_axis.md` — the data axis, closed; same documentation shape.
