#!/usr/bin/env python3
"""Batch-run brute_match --apply-best across all sub-100 -O4,p game functions.
Cheap (asm-free, self-measuring) sweep over decl-order register re-homing.
Applies any variant that strictly beats baseline;
logs applied wins for review + path-scoped commit. Skips hot/owned files.

Usage: python3 tools/batch_brute.py [min_fuzzy] [--exclude f1,f2] [--budget N]
       python3 tools/batch_brute.py --list        # population only, no writes
Then review /tmp/batch_brute.log; git diff shows applied wins to verify+commit.

IMPORTING THIS MODULE MUST NOT SWEEP.  The whole body used to run at import
time, so `import batch_brute` -- to read its selector, to reuse `select()`, to
check its population -- launched a tree-wide `--apply-best` run that edits the
working tree of whatever repo it was imported from.  Everything now lives under
`main()`, behind a `__main__` guard, and `select()` is a pure function.
"""
import json, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
DEFAULT_EXCLUDE = {"player", "dll_0000_gameui", "objseq", "objhits", "gameloop",
                   "track_dolphin", "newshadows", "shader", "bossdrakor"}


def select(min_fuzzy=90.0, exclude=None):
    """The sweep's population: (fuzzy, unit-basename, symbol), worst first.

    Pure -- reads report.json and returns rows.  A missing fuzzy_match_percent
    is 0.0 (the report omits every zero), so the worst rows are IN, not
    silently perfect.
    """
    exclude = DEFAULT_EXCLUDE if exclude is None else exclude
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    # skip noopt/audio/dolphin: brute_match's decl re-home only bites in -O4,p;
    # and skip units where we can't cheaply tell config. Use basename heuristic.
    targets = []
    for u in report["units"]:
        un = u.get("name", "")
        if not un.startswith("main/"):
            continue
        if any(x in un for x in ("dolphin/", "MSL", "audio", "/play", "dll_0000_game")):
            continue
        base = Path(un[len("main/"):]).stem
        if base in exclude:
            continue
        for f in u.get("functions", []):
            fz = f.get("fuzzy_match_percent", 0.0)
            if min_fuzzy <= fz < 100.0:
                targets.append((fz, base, f.get("name", "")))
    targets.sort()  # lowest fuzzy first = most headroom
    return targets


def main(argv):
    min_fuzzy = float(argv[1]) if len(argv) > 1 and not argv[1].startswith("-") else 90.0
    budget = 100.0
    exclude = set(DEFAULT_EXCLUDE)
    list_only = "--list" in argv
    for i, a in enumerate(argv):
        if a == "--budget" and i + 1 < len(argv):
            budget = float(argv[i + 1])
        if a == "--exclude" and i + 1 < len(argv):
            exclude |= set(argv[i + 1].split(","))

    targets = select(min_fuzzy, exclude)
    if list_only:
        print(f"# batch_brute population: {len(targets)} sub-100 candidates "
              f">= {min_fuzzy}% (no build, no writes)")
        for fz, base, sym in targets:
            print(f"  {fz:8.3f}  {base:28s} {sym}")
        return 0

    log = open("/tmp/batch_brute.log", "w")

    def out(s):
        print(s)
        log.write(s + "\n")
        log.flush()

    out(f"# batch_brute: {len(targets)} sub-100 candidates >= {min_fuzzy}%, "
        f"budget {budget}s each")
    wins = []
    for fz, base, sym in targets:
        try:
            r = subprocess.run(
                ["python3", "tools/brute_match.py", base + ".c", sym,
                 "--strategy", "all", "--time-budget", str(budget), "--apply-best"],
                cwd=REPO, capture_output=True, text=True, timeout=budget + 240)
        except subprocess.TimeoutExpired:
            out(f"  TIMEOUT {base} {sym}")
            continue
        tail = r.stdout.strip().splitlines()[-4:] if r.stdout else []
        applied = any("APPLIED best variant" in ln
                      for ln in (r.stdout or "").splitlines())
        if applied:
            newfz = ""
            for ln in tail:
                if "APPLIED" in ln:
                    newfz = ln
            out(f"  *** WIN {base} {sym} ({fz:.3f}) :: {newfz}")
            wins.append((base, sym, fz))
        elif r.returncode != 0 or "report generate failed" in (r.stderr or "") + (r.stdout or ""):
            out(f"  skip {base} {sym} (measure-fail/parse: {(r.stderr or '')[:60].strip()})")
        else:
            out(f"  --   {base} {sym} ({fz:.3f}) welded")

    out(f"\n# DONE. {len(wins)} wins applied (in working tree, verify+commit):")
    for base, sym, fz in wins:
        out(f"  {base}.c {sym} (was {fz:.3f})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
