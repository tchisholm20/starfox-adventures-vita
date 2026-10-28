# Post-merge hygiene

Four checks to run after every daily `git merge --no-ff origin/staging` into `main`. They take a few
minutes, they are all read-only until you ask them to act, and each one catches a
class of problem that a green `locked_ninja.sh` does not.

```bash
python3 tools/pairing_check.py                 # 1. FIRST -- measurement integrity
python3 tools/unused_locals.py                 # 2. dead locals
python3 tools/dup_prototypes.py src            # 3. header-duplicated prototypes
python3 tools/unused_externs.py src            # 4. file-local dead externs
```

## 1. `pairing_check.py` — run this before you measure anything

Expect **exactly 2** retail-only symbols, permanently: `gap_03_80006C6C_text`
(`main/render.c`) and `gap_03_8028C964_text` (`targimpl.c`) — foreign-toolchain
blobs. Anything else means our objects and the retail carve disagree on a name,
and every affected function scores **zero** while the build stays green.

⚠️ **A hit is usually a stale object, not a broken rename.** `config/GSAE01/symbols.txt`
is build input: editing it re-runs the splitter, which re-carves the retail tree
under `build/GSAE01/obj/`. That re-carve is an *undeclared* side effect — ninja's
only declared output for the rule is `config.json` — so ninja has no edge telling
it your `.o` is now stale relative to retail. Rebuild the named object explicitly
before concluding anything:

```bash
tools/locked_ninja.sh build/GSAE01/src/<path>.o
```

Measured twice, by two people independently, on `main/shader.c`: four retail-only
symbols and **90.497**; source, headers and `symbols.txt` all already agreed; one
by-name rebuild restored **99.437** and the count returned to 2. Nothing was ever
wrong with that rename. See `docs/rename_safety.md`.

*A complete, correct rename measures as a total collapse until you force the rebuild.*

## 2–4. The recovery sweeps

These delete only what is provably byte-neutral: each candidate is compiled
before and after and kept only if the object is bit-identical
(`tools/byteneutral.py`, which encodes four traps in its docstring — read it
before trusting any md5 result).

Merges reintroduce this steadily. In a single session, after sweeping the tree
clean, incoming merges brought back **95 header-duplicated prototypes** and
**15 unreferenced locals**. It is not a one-off cleanup; it is maintenance.

## What a rejection means

A candidate the gate refuses is **evidence, not a failure**. If removing an
unreferenced local is *not* byte-neutral, the local is occupying a stack slot in
retail too — it is faithful recovery and must be kept. Every `src/dolphin/`
candidate rejected this way sits in a unit at 100.000, and musyx's
`unused`/`unused2`/`unusedA`/`unusedTime` are deliberate slot reservations.

## Two traps that made tools lie

Both produced silent false PASSes and are worth knowing before you write another
sweep:

- **A stale object survives a failed compile.** Reading its md5 reports the last
  *good* build, so every broken edit "passes". Unlink the object first and treat
  a missing object as FAIL, never as equal.
- **Blanking a block comment to a single space collapses line numbering.** Every
  line number after it shifts, so a line-number-driven tool deletes the wrong
  lines — and a wrongly deleted *comment* still passes an md5 gate, because
  comments do not reach codegen. Preserve newlines when stripping comments.
  (This one shipped: `dup_prototypes.py` deleted comment lines in 8 files before
  it was caught and repaired.)

## Scope note

`tools/byteneutral.py` reads cflags, `mw_version` and the object path from
`build.ninja`, so it covers whatever the build covers — currently **988**
sources. It once resolved only 732 because build statements that wrap
immediately after the `:` were skipped; if a file reports `NOBUILD`, suspect the
parser before suspecting the file.
