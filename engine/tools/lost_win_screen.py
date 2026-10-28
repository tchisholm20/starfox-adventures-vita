#!/usr/bin/env python3
"""Find functions that score BELOW a fuzzy_match_percent this repo already recorded.

WHY THIS EXISTS
Every other screen in tools/ compares the tree against RETAIL.  None of them
compares the tree against ITSELF-IN-THE-PAST, so a win that was landed,
measured and written into a commit message can be silently undone later -- by a
TU merge, a rehome, a bulk retype, a purge, or a plain accidental revert -- and
nothing in the toolchain notices.  The per-unit and tree fuzzy barely move, the
DOL stays green, and the function simply drops off the radar at a lower number
than it once held.

Measured instance (wave 146): ObjSeq_onMapSetup was taken 80.58 -> 82.51 by
170e3173db (constant-trip remainder loop, kills MWCC's x2 auto-unroll).  The
maketex+objseq+objseqinit merge later re-instated the runtime-trip spelling and
the function sat at 80.57895 for eight days, still the single largest
addressable entry on tools/sizedelta.py.  Re-applying the recorded spelling put
it straight back to 82.505264 with zero regressions tree-wide.

HOW TO READ THE OUTPUT
A row is a LEAD, not a defect.  Three populations are mixed together:
  * genuine lost wins -- re-apply the recorded spelling (the case above);
  * accepted structural costs -- a peak that was only reachable under a TU
    boundary, cflag profile or split this project has since corrected on DOL
    evidence.  Those are correct and must stay lost;
  * purged hacks -- a peak reached with a #pragma, a goto, a match-volatile or
    a pool pun, all banned by CLAUDE.md.  Also correct, also stays lost.
Read the peak commit before acting: `git show <commit>`.  The three are easy to
tell apart from its message.

Peaks are mined from commit messages, so they are only as good as the wording
that was committed; a peak with no matching current symbol is dropped, and the
regex can still pair a symbol with a nearby unrelated number.  Verify the peak
commit actually claims that number FOR that symbol before spending a build on
it.

Usage:
    python3 tools/lost_win_screen.py [--min-bytes N] [--since DATE] [--top N]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REPORT = REPO / "build/GSAE01/report.json"

# "<symbol> ... <a>-><b>" with at most a short run of filler between them, so a
# subject that mentions two unrelated functions does not cross-assign numbers.
PAT = re.compile(
    r"([A-Za-z_][A-Za-z0-9_]{3,})[^\n]{0,44}?"
    r"\b(\d{1,3}(?:\.\d+)?)\s*->\s*(\d{1,3}(?:\.\d+)?)\b"
)


def mine_peaks(since: str | None) -> dict[str, tuple[float, str, str, str]]:
    cmd = ["git", "log", "--format=%h\x01%ci\x01%s%n%b\x02"]
    if since:
        cmd.append(f"--since={since}")
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO).stdout
    peaks: dict[str, tuple[float, str, str, str]] = {}
    for entry in out.split("\x02"):
        if not entry.strip():
            continue
        parts = entry.split("\x01")
        if len(parts) < 3:
            continue
        sha, date, body = parts[0].strip(), parts[1][:10], parts[2]
        subject = body.split("\n", 1)[0]
        for m in PAT.finditer(body):
            sym, before, after = m.group(1), float(m.group(2)), float(m.group(3))
            if before > 100 or after > 100 or after <= before:
                continue
            cur = peaks.get(sym)
            if cur is None or after > cur[0]:
                peaks[sym] = (after, sha, date, subject)
    return peaks


def load_current() -> dict[str, tuple[float, str, int]]:
    if not REPORT.is_file():
        sys.exit("build/GSAE01/report.json is absent -- "
                 "rm it and `ninja build/GSAE01/report.json` first")
    data = json.loads(REPORT.read_text(encoding="utf-8"))
    now: dict[str, tuple[float, str, int]] = {}
    for unit in data["units"]:
        for fn in (unit.get("functions") or []):
            if "fuzzy_match_percent" in fn:
                now[fn["name"]] = (fn["fuzzy_match_percent"], unit["name"],
                                   int(fn.get("size") or 0))
    return now


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", default=None, help="only mine commits after DATE")
    ap.add_argument("--min-bytes", type=float, default=0.0,
                    help="drop rows worth fewer than N wasted bytes")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    peaks, now = mine_peaks(args.since), load_current()
    rows = []
    for sym, (peak, sha, date, subject) in peaks.items():
        if sym not in now:
            continue
        cur, unit, size = now[sym]
        if cur >= peak - 0.01:
            continue
        lost = size * (peak - cur) / 100.0
        if lost < args.min_bytes:
            continue
        rows.append((lost, sym, unit, cur, peak, sha, date, subject))
    rows.sort(reverse=True)

    print(f"{len(rows)} function(s) below a fuzzy this repo has already recorded\n")
    print(f"{'lostB':>7} {'now':>8} {'peak':>8}  symbol")
    print(f"{'':>7} {'':>8} {'':>8}  unit | peak commit")
    for lost, sym, unit, cur, peak, sha, date, subject in rows[: args.top]:
        print(f"{lost:7.1f} {cur:8.3f} {peak:8.3f}  {sym}")
        print(f"{'':>7} {'':>8} {'':>8}  {unit} | {sha} {date} {subject[:96]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
