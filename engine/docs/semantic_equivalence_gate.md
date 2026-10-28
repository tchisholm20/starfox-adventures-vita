# The semantic-equivalence gate

**Every brute-force lane that rewrites an expression must route the rewrite through
`tools/semantic_equivalence.py` before compiling it.**

## The hole it closes

This project's gates all compare our output against retail's *bytes*:

| gate | compares |
| --- | --- |
| objdiff `fuzzy_match_percent` | our instructions vs retail's |
| `tools/obj_equal.py --tree` | our object bytes vs retail's |
| `tools/score_delta_gate.py` | the above, per function and per section |
| the forced link | the resulting `main.dol` vs retail's hash |

None of them compares our output against retail's *meaning*. A rewrite that changes
the arithmetic and happens to land bytes closer to retail therefore scores as a win
and lands, and no instrument downstream can object.

That is not hypothetical. A regex operand-swapper rewrote

```c
verts + j * 12          /* a base plus a scaled index */
```
as
```c
(j + verts) * 12        /* a different computation */
```

because a regular expression cannot see precedence: it took `verts` and `j` as the
two operands of the `+` without noticing that `j` was already bound to the `*`. The
score went up, the tool's own guard passed it, and it was applied.

## What the gate does

`prove(before, after)` re-parses **both** spellings from text with the real C
expression parser in `tools/cexpr.py`, so a splicing bug in whatever generated the
rewrite cannot hide from it. Two independent instruments must both agree:

1. **A structural proof.** The two syntax trees must be relatable by a closed list of
   identities that hold for every input:
   - `COMMUTE` — commutativity of `+ * & | ^ == !=`, both operands provably pure;
   - `RELFLIP` — `a < b` == `b > a` (and `<=`/`>=`);
   - `TERNFLIP` — `c ? a : b` == `!c ? b : a`;
   - `SIGN` — `-(x * y)` == `(-x) * y` == `x * (-y)`, descending through `*` only;
   - `ASSOC-INT` — associativity of `+ * & | ^`, **only** with a positive
     integer-type proof from the oracle, and off unless explicitly enabled.

   `&&` and `||` are never reordered: swapping them changes which side is evaluated.
   Any rule that moves one operand across another additionally requires both to be
   pure — no call, assignment, `++`/`--`, or comma.

2. **A differential evaluation.** Both spellings are evaluated under randomized
   inputs in an integer model (32-bit wrapping) and a floating-point model (compared
   **bitwise**). Unknown sub-terms are modelled as deterministic *functions* of their
   inputs, not as opaque constants, so `a[i * j]` and `a[j * i]` agree while
   `a[i] + b` and `a[j] + b` do not.

The two catch different mistakes. The structural proof rejects the `verts + j * 12`
mutation at the root — an addition cannot equal a multiplication. The differential
evaluation is what stops an identity being applied to a type it is not true for:
**floating-point addition and multiplication are not associative**, and it is the
float model that says so.

## Controls

`python3 tools/semantic_equivalence.py --self-test` runs the suite. It carries:

- the **positive control**: the exact `verts + j * 12` → `(j + verts) * 12` mutation,
  which the gate must reject — reported separately so no future edit can silence it;
- a **negative control on the type oracle**: the oracle is deliberately broken so it
  swears three floats are integers, the structural proof duly clears the regrouping,
  and the differential evaluation must still refuse it. Without this the float model
  could rot into a no-op unnoticed, because every other rejection in the suite is
  delivered by the structural proof;
- adversarial probes for the shapes a buggy *generator* would emit — a parenthesis
  moved one operator over, a parenthesis dropped, a minus or a division regrouped, a
  call moved across an operand.

## Callers

- `tools/expr_sweep.py` — the expression sweep. Gated by construction: a variant that
  is not cleared is never compiled.
- `tools/operand_sweep.py` — the regex predecessor, retrofitted. Its `sites()` guards
  are heuristics; the gate in its apply path is the actual guarantee.
- `tools/stmt_sweep.py` — **not** routed through the gate, and does not need to be: it
  moves whole statements verbatim and proves its own reorderings against the
  RAW/WAR/WAW dependence relation over non-address-taken locals. It never splices
  inside an expression, which is the failure this gate exists to prevent.
- Declaration sweeps (`tools/brute_match.py`, `tools/decl_split_sweep.py`,
  `tools/perm_solve.py`, `tools/slot_oracle.py`) reorder declarations only — which
  **can** change meaning, and this line used to say it could not. A declaration
  whose INITIALISER calls something is a declaration by every test the parser
  applies, and its evaluation moves with it:

  ```c
  s32 rnd1 = randomGetRange(0, 0x1e) * 2;
  s32 rnd2 = randomGetRange(0, 0x1e) * 2;
  ```

  is two draws from a PRNG that the call site consumes in *different* argument
  positions, so swapping the two declarations swaps which draw lands where. The
  sweeps do not route through this gate; they carry `brute_match.side_effect_reorders`,
  which FLAGS any permutation that changes the relative order of a pair where at
  least one initialiser calls something, and refuses to auto-apply it without
  `--allow-side-effect-reorder`. Flagging rather than refusing keeps every prior
  "N orderings, 0 hits" count comparable.

  `initialiser_calls` counts an INDIRECT call — `p->fn(x)`, `s.fn(x)`,
  `tbl[i].fn(x)` — as a call. Only the `(` distinguishes a member call from a
  member read, and this tree dispatches its DLL interfaces
  (`(*gCameraInterface)->getCamera()`) exactly that way; those are the calls whose
  purity cannot be established from the source at all, so they are the last ones a
  conservative detector may miss.

  The per-block adjudication of every call-carrying block in the sub-100
  population is in `docs/declaration_order_side_effects.md`.

## Limits, stated honestly

- The type oracle is deliberately one-sided. It reads plain scalar declarations of
  locals and parameters and nothing else; everything it cannot establish is UNKNOWN,
  and the only rule that consults it refuses on UNKNOWN. It will not prove an
  associativity that is legal but expressed through a struct field.
- A rewrite the parser cannot read is **refused**, and refusals are reported by cause.
  A region a tool cannot parse reads exactly like a region it examined and cleared, so
  the sweep prints its parse failures rather than dropping them.
- The differential evaluation is a necessary condition, not a sufficient one. The
  structural proof is what authorises; the evaluation is the cross-check that keeps it
  honest.
