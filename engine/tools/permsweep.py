"""Permute a block of local declarations and rank the orderings by report.json.

WHAT CHANGED AND WHY IT MATTERS
This tool used to rank candidates by POSITIONAL INSTRUCTION-DIFF COUNT and
call the lowest one "best".  That verdict is unsound: byte-diff and
fuzzy_match_percent can move in OPPOSITE directions, and a lane trusting the
old ranking would have landed a regression.  Measured counterexample
(shader.c mapLoadUnloadObjects, wave 146): the byte-diff sweep's "best"
plateau scores 197 diffs against a baseline of 219 and reaches exact
instruction parity -- the function drops off tools/sizedelta.py entirely --
while the unit REGRESSES 99.33254 -> 99.316414 and sdiff REGBLIND gets worse
(4 -> 5).  Per CLAUDE.md: "Match % truth = report.json fuzzy_match_percent.
Diff tools locate divergence; they don't certify it."

So byte-diff is now only a cheap PRE-FILTER used to shortlist candidates.
The VERDICT is report.json fuzzy_match_percent, measured by rebuilding the
unit object and regenerating the report for every shortlisted ordering.

Every candidate is printed with THREE numbers:
  * the object sha1 -- an sha identical to the baseline's means the
    permutation was INERT (MWCC folded it straight back), which is the
    difference between "this lever does not apply here" and "this lever
    lost".  An inert candidate needs no report regeneration: an identical
    object scores identically by construction.
  * the byte-diff (fnbytes.compare), for locating divergence only.
  * the unit and per-function fuzzy_match_percent, which decides.
When the byte-diff ranking and the fuzzy ranking disagree, the tool says so
LOUDLY at the end -- that disagreement is itself a result worth recording.

Declaration order permutes saved-register assignment among locals with
disjoint live ranges.  This sweeps orderings of a contiguous decl block.

The --lines range is caller-supplied, so this tool has no opinion about WHICH
block you point it at -- and a function's declarations do NOT all live at the
top of the body.  Locals declared inside a loop body / if-block / case block
are a separate, independently order-sensitive block (the saved-GPR band is
filled in two phases; webs that never reach a loop header are allocated first,
in reverse first-definition order, and only the rest follow declaration order).
Enumerate every block first with
    python3 tools/brute_match.py <unit> <symbol> --list-blocks
and sweep each one; a sweep of the top-level block alone establishes nothing
about the nested ones.

A build failure or an absent symbol is recorded as ERROR and never as a
match -- several tools in this repo print nothing both when a function matches
and when the build failed, which has produced spurious "match" reports before.

Usage:
  python3 tools/permsweep.py <file.c> <unit> <symbol> --lines A:B [--max N]
      A:B is the 1-based inclusive line range of the decl block to permute.
      Lines must be independent declarations (no initializers depending on
      each other).  ARRAY declarations set the stack frame layout -- permuting
      them moves stack offsets, so keep them out of the range unless that is
      what you intend to test.

  --mode perm|moves   all n! orderings, or the O(n^2) single-decl relocations
  --top-k N           cap how many distinct-object candidates reach the
                      report.json phase, ranked by byte-diff.  DEFAULT 0 =
                      no cap: a warm `ninja report.json` costs ~0.1 s, so the
                      pre-filter normally only removes duplicate objects, not
                      candidates.  Capping re-admits byte-diff to the verdict,
                      so only do it when a regen is genuinely expensive.
  --keep-best   leave the best-scoring ordering in the file, and only when it
                strictly beats the baseline's report.json fuzzy (default:
                restore).
  --dry-run     print the permutations without building

Always restores the original file on exit unless --keep-best improved things.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fnbytes import REPO, disassemble, find_unit, load_units, objdump_path

REPORT = "build/GSAE01/report.json"


def compare(unit_q: str, symbol: str, version: str = "GSAE01"):
    """-> (ndiff, ntgt, ncur). ndiff==0 and sizes equal means byte-identical.

    Raises LookupError on any condition that means "no result".
    """
    units = load_units(REPO / "build" / version / "config.json")
    unit = find_unit(units, unit_q)
    od = objdump_path()
    tgt = disassemble(od, REPO / Path(unit["object"]), symbol)
    cur = disassemble(
        od,
        REPO / Path(unit["object"].replace(f"build/{version}/obj/", f"build/{version}/src/")),
        symbol,
    )
    if not tgt:
        raise LookupError(f"no instructions for {symbol} in target")
    if not cur:
        raise LookupError(f"no instructions for {symbol} in current (build failed / inlined)")
    n = min(len(tgt), len(cur))
    ndiff = sum(1 for i in range(n) if tgt[i][0] != cur[i][0]) + abs(len(tgt) - len(cur))
    return ndiff, len(tgt), len(cur)


def locked_ninja(*targets: str) -> bool:
    # Go through the build mutex: a bare `ninja` here races parallel matching
    # agents and corrupts .ninja_log / loses .d writes, which shows up as
    # spurious BUILD-FAIL entries mid-sweep.
    if os.name == "nt":
        lockdir = Path("/tmp/sfa_ninja.lock")
        lockdir.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(600):
            try:
                lockdir.mkdir()
            except FileExistsError:
                time.sleep(0.5)
                continue
            try:
                proc = subprocess.run(
                    ["ninja", *targets],
                    cwd=REPO, capture_output=True, text=True
                )
                return proc.returncode == 0
            finally:
                try:
                    lockdir.rmdir()
                except OSError:
                    pass
        return False

    proc = subprocess.run(
        ["bash", "--noprofile", "--norc", "tools/locked_ninja.sh", *targets],
        cwd=REPO, capture_output=True, text=True
    )
    return proc.returncode == 0


def rebuild(obj_rel: str) -> str | None:
    """Rebuild one object. Returns its sha1 prefix, or None on failure."""
    obj = REPO / obj_rel
    obj.unlink(missing_ok=True)
    if not locked_ninja(obj_rel) or not obj.is_file():
        return None
    return hashlib.sha1(obj.read_bytes()).hexdigest()[:10]


def regen_report() -> bool:
    """report.json does NOT reliably regenerate off a .o rebuild alone; rm it."""
    (REPO / REPORT).unlink(missing_ok=True)
    return locked_ninja(REPORT) and (REPO / REPORT).is_file()


def report_unit_name(config_unit_name: str) -> str:
    """config.json calls a unit 'main/shader.c'; report.json 'main/main/shader'."""
    stem = config_unit_name.replace("\\", "/").removesuffix(".c")
    with (REPO / REPORT).open(encoding="utf-8") as handle:
        data = json.load(handle)
    names = [u["name"] for u in data["units"]]
    hits = [n for n in names if n == stem or n.endswith("/" + stem)]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise LookupError(f"ambiguous report unit for '{config_unit_name}': {hits}")
    raise LookupError(f"no report.json unit for '{config_unit_name}'")


def read_report(report_name: str, symbol: str):
    """-> (unit fuzzy_match_percent, function fuzzy_match_percent or None).

    functions[] lives at UNIT level in report.json, NOT inside sections[];
    reading it the documented way yields a silent zero.
    """
    with (REPO / REPORT).open(encoding="utf-8") as handle:
        data = json.load(handle)
    for unit in data["units"]:
        if unit["name"] != report_name:
            continue
        fn = None
        for f in (unit.get("functions") or []):
            if f["name"] == symbol:
                # objdiff omits the key at 0.0; absent means zero, not absent
                fn = round(float(f.get("fuzzy_match_percent") or 0.0), 6)
        return round(float(unit["measures"].get("fuzzy_match_percent") or 0.0), 6), fn
    raise LookupError(f"unit '{report_name}' absent from report.json")


def fmt(value) -> str:
    return "   n/a   " if value is None else f"{value:9.5f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("unit")
    ap.add_argument("symbol")
    ap.add_argument("--lines", required=True, help="1-based inclusive A:B")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--max", type=int, default=0, help="cap permutations")
    ap.add_argument("--top-k", type=int, default=0,
                    help="cap distinct-object candidates promoted to report.json "
                         "scoring (0 = no cap, the default)")
    ap.add_argument("--keep-best", action="store_true")
    ap.add_argument("--mode", choices=("perm", "moves"), default="perm",
                    help="perm: all n! orderings. moves: O(n^2) single-decl relocations")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    src = REPO / args.source
    original = src.read_bytes()
    text = original.decode("utf-8", errors="surrogateescape").split("\n")

    a, b = (int(x) for x in args.lines.split(":"))
    block = text[a - 1 : b]
    if not all(ln.strip().endswith(";") for ln in block):
        print("ERROR: every line in the range must be a simple declaration", file=sys.stderr)
        return 2
    print(f"permuting {len(block)} declarations:")
    for ln in block:
        print("   ", ln.strip())

    units = load_units(REPO / "build" / args.version / "config.json")
    unit_cfg = find_unit(units, args.unit)
    obj_rel = unit_cfg["object"].replace(
        f"build/{args.version}/obj/", f"build/{args.version}/src/"
    )

    n = len(block)
    if args.mode == "moves":
        # Every "lift one declaration, reinsert it at position j" ordering.
        # O(n^2) instead of n!, and it is enough to retarget a single local's
        # saved-register home -- which is what a 2-register swap needs.
        seen, perms = set(), []
        for i in range(n):
            for j in range(n):
                rest = [k for k in range(n) if k != i]
                cand = tuple(rest[:j] + [i] + rest[j:])
                if cand not in seen:
                    seen.add(cand)
                    perms.append(cand)
    else:
        perms = list(itertools.permutations(range(n)))
    identity = tuple(range(n))
    perms = [p for p in perms if p != identity]
    if args.max:
        perms = perms[: args.max]
    print(f"{len(perms)} permutations (baseline measured separately), object {obj_rel}\n")
    if args.dry_run:
        return 0

    base_sha = base_ndiff = base_unit = base_fn = None
    rows: list[dict] = []
    scored: list[dict] = []
    keep_perm = None
    try:
        # ---------------------------------------------------------- baseline
        print("=== baseline ===", flush=True)
        base_sha = rebuild(obj_rel)
        if base_sha is None:
            print("ERROR: baseline build failed", file=sys.stderr)
            return 2
        base_ndiff = compare(args.unit, args.symbol, args.version)[0]
        if not regen_report():
            print("ERROR: baseline report.json regeneration failed", file=sys.stderr)
            return 2
        rname = report_unit_name(unit_cfg["name"])
        base_unit, base_fn = read_report(rname, args.symbol)
        print(f"  sha={base_sha}  bytediff {base_ndiff}  "
              f"unit {fmt(base_unit)}  fn {fmt(base_fn)}  ({rname})\n", flush=True)

        # --------------------------------------- phase 1: byte pre-filter
        print(f"=== phase 1: byte pre-filter over {len(perms)} orderings ===", flush=True)
        for idx, perm in enumerate(perms):
            text[a - 1 : b] = [block[i] for i in perm]
            src.write_bytes("\n".join(text).encode("utf-8", errors="surrogateescape"))

            sha = rebuild(obj_rel)
            if sha is None:
                print(f"[{idx:4d}] {perm}  BUILD-FAIL")
                continue
            try:
                ndiff, _, _ = compare(args.unit, args.symbol, args.version)
            except LookupError as exc:
                print(f"[{idx:4d}] {perm}  ERROR {exc}")
                continue
            inert = sha == base_sha
            print(f"[{idx:4d}] {perm}  sha={sha}  bytediff {ndiff}"
                  + ("  INERT (object identical to baseline)" if inert else ""), flush=True)
            rows.append({"perm": perm, "sha": sha, "ndiff": ndiff, "inert": inert,
                         "unit": base_unit if inert else None,
                         "fn": base_fn if inert else None})

        # ---------------------------- phase 2: report.json is the verdict
        by_sha: dict[str, dict] = {}
        for row in rows:
            if not row["inert"] and row["sha"] not in by_sha:
                by_sha[row["sha"]] = row
        distinct = sorted(by_sha.values(), key=lambda r: r["ndiff"])
        shortlist = distinct if args.top_k <= 0 else distinct[: args.top_k]
        # An exact byte match must never be dropped by the pre-filter.
        for row in distinct:
            if row["ndiff"] == 0 and row not in shortlist:
                shortlist.append(row)

        print(f"\n=== phase 2: report.json fuzzy for {len(shortlist)} of "
              f"{len(distinct)} distinct objects "
              f"({sum(1 for r in rows if r['inert'])} inert, reusing baseline) ===",
              flush=True)
        for row in shortlist:
            text[a - 1 : b] = [block[i] for i in row["perm"]]
            src.write_bytes("\n".join(text).encode("utf-8", errors="surrogateescape"))
            sha = rebuild(obj_rel)
            if sha is None or sha != row["sha"]:
                print(f"  {row['perm']}  UNSTABLE BUILD (sha {sha} != {row['sha']}) -- skipped")
                continue
            if not regen_report():
                print(f"  {row['perm']}  REPORT-FAIL -- skipped")
                continue
            unit_pct, fn_pct = read_report(rname, args.symbol)
            delta = unit_pct - base_unit
            mark = "WIN " if delta > 0 else ("LOSS" if delta < 0 else "same")
            print(f"  {row['perm']}  sha={sha}  bytediff {row['ndiff']:4d}  "
                  f"unit {fmt(unit_pct)}  fn {fmt(fn_pct)}  {mark} {delta:+.5f}", flush=True)
            # propagate to every ordering that produced this same object
            for other in rows:
                if other["sha"] == sha:
                    other["unit"], other["fn"] = unit_pct, fn_pct

        scored = [r for r in rows if r["unit"] is not None]
        if scored:
            winner = max(scored, key=lambda r: (r["unit"],
                                                r["fn"] if r["fn"] is not None else -1.0))
            if (args.keep_best
                    and (winner["unit"], winner["fn"] if winner["fn"] is not None else -1.0)
                    > (base_unit, base_fn if base_fn is not None else -1.0)):
                keep_perm = winner["perm"]
    finally:
        if keep_perm is not None:
            text[a - 1 : b] = [block[i] for i in keep_perm]
            src.write_bytes("\n".join(text).encode("utf-8", errors="surrogateescape"))
            rebuild(obj_rel)
            regen_report()
            print(f"\nkept ordering {keep_perm}")
        else:
            src.write_bytes(original)
            rebuild(obj_rel)
            regen_report()
            print("\nrestored original source")

    # ---------------------------------------------------------------- verdict
    print(f"\n{len(rows)}/{len(perms)} permutations built; "
          f"{len(scored)} carry a report.json score.")
    print(f"BASELINE  bytediff {base_ndiff}  unit {fmt(base_unit)}  fn {fmt(base_fn)}")
    if not scored:
        print("No candidate produced a report.json score -- no verdict.")
        return 0

    print("\nranked by report.json fuzzy_match_percent (the truth metric):")
    ranking = sorted(scored, key=lambda r: (-r["unit"],
                                            -(r["fn"] if r["fn"] is not None else -1.0),
                                            r["ndiff"]))
    for row in ranking[:10]:
        print(f"  unit {fmt(row['unit'])}  fn {fmt(row['fn'])}  bytediff {row['ndiff']:4d}  "
              f"sha={row['sha']}  {row['perm']}"
              + ("  INERT" if row["inert"] else ""))

    best_fuzzy = ranking[0]
    best_bytes = min(rows, key=lambda r: r["ndiff"])
    print(f"\nbest by FUZZY   : unit {fmt(best_fuzzy['unit'])}  bytediff "
          f"{best_fuzzy['ndiff']}  {best_fuzzy['perm']}")
    print(f"best by BYTEDIFF: unit {fmt(best_bytes['unit'])}  bytediff "
          f"{best_bytes['ndiff']}  {best_bytes['perm']}")

    if best_fuzzy["unit"] <= base_unit and (
            best_fuzzy["fn"] is None or base_fn is None or best_fuzzy["fn"] <= base_fn):
        print("\nVERDICT: NO WIN. The baseline ordering is at least as good as every "
              "candidate measured. Do not land any of these.")
    else:
        print(f"\nVERDICT: WIN unit {base_unit:.5f} -> {best_fuzzy['unit']:.5f} at "
              f"{best_fuzzy['perm']}")

    if best_bytes["unit"] is None:
        print("\nNOTE: the lowest-bytediff ordering was outside the report.json shortlist "
              "-- re-run with --top-k 0 to score it.")
    elif best_bytes["sha"] != best_fuzzy["sha"]:
        print("\n*** BYTEDIFF AND FUZZY DISAGREE ***")
        print(f"    lowest bytediff {best_bytes['ndiff']} scores {best_bytes['unit']:.5f}")
        print(f"    highest fuzzy {best_fuzzy['unit']:.5f} has bytediff {best_fuzzy['ndiff']}")
        print("    Byte-diff is a LOCATOR, not a verdict. Only the fuzzy column decides.")

    if (best_bytes["unit"] is not None and best_bytes["ndiff"] < base_ndiff
            and best_bytes["unit"] < base_unit):
        print("\n*** THE BYTE-DIFF TRAP FIRED ***")
        print(f"    an ordering with FEWER byte diffs ({best_bytes['ndiff']} < {base_ndiff}) "
              f"scores WORSE ({best_bytes['unit']:.5f} < {base_unit:.5f}).")
        print("    A byte-diff-gated sweep would have reported this as the win.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
