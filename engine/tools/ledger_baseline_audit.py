#!/usr/bin/env python3
"""Audit `docs/priced_classes.md` for prices taken against a baseline that moved.

A76's finding, generalised: **a price is only valid against the baseline it was
measured at.**  A69 priced `engine/5`'s pool crutch as "+176 data for
renderSunAndMoon 99.476 -> 98.214, a net tree loss" -- but 99.476 was a score an
undefined `extern` was buying, and once that extern was deleted the honest
baseline was 96.828, against which the same +176 costs nothing and *gains* two
points.  The ruling inverted without a single new idea, purely because the
number it was measured against had moved.

So: every figure the ledger quotes for a named function is a claim about that
function's score at some past sha.  This tool reads them all back out and
compares them to the CURRENT score.  A row whose quoted figures no longer appear
anywhere in that function's history-of-record is a row whose price must be
re-taken before it can be trusted.

Scoring figures are matched to the nearest function or unit name mentioned in
the same paragraph, so a stray tree-level percentage is not attributed to a row.

  python3 tools/ledger_baseline_audit.py [--doc docs/priced_classes.md]
                                         [--report build/GSAE01/report.json]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

NUM_RE = re.compile(r"(?<![\w.])(\d{1,3}\.\d{3,6})(?![\w.])")
TICK_RE = re.compile(r"`([A-Za-z_][A-Za-z_0-9]*)`")


def load_scores(report_path):
    doc = json.load(open(report_path))
    fn, unit = {}, {}
    for u in doc["units"]:
        m = u.get("measures") or {}
        unit[u["name"]] = m.get("fuzzy_match_percent")
        for f in u.get("functions") or []:
            fn.setdefault(f["name"], []).append(
                (u["name"], f.get("fuzzy_match_percent", 0.0)))
    return fn, unit


def paragraphs(text):
    out, buf, start = [], [], 0
    pos = 0
    for line in text.split("\n"):
        if not line.strip():
            if buf:
                out.append((start, "\n".join(buf)))
                buf = []
        else:
            if not buf:
                start = pos
            buf.append(line)
        pos += len(line) + 1
    if buf:
        out.append((start, "\n".join(buf)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--doc", default="docs/priced_classes.md")
    ap.add_argument("--report", default="build/GSAE01/report.json")
    ap.add_argument("--tol", type=float, default=0.0005)
    args = ap.parse_args()

    fnscore, _unit = load_scores(REPO / args.report)
    text = (REPO / args.doc).read_text(encoding="utf-8", errors="replace")

    # section headings, so each finding can be reported against its ledger row
    heads = [(m.start(), m.group(0).strip())
             for m in re.finditer(r"^#{2,3} .*$", text, re.M)]

    def section_of(off):
        cur = "(preamble)"
        for o, h in heads:
            if o <= off:
                cur = h
            else:
                break
        return cur

    # Attribute a figure to the NEAREST PRECEDING known name inside the same
    # sentence.  A paragraph-wide join credits every stray tree percentage to
    # every name it mentions and buries the real rows in false alarms.
    attributed = {}          # (section, name) -> set of figures
    for off, para in paragraphs(text):
        sec = section_of(off)
        names = [(m.start(), m.group(1)) for m in TICK_RE.finditer(para)
                 if m.group(1) in fnscore]
        if not names:
            continue
        for m in NUM_RE.finditer(para):
            p = m.start()
            # do not cross a sentence boundary looking backwards
            stop = max(para.rfind(". ", 0, p), para.rfind("\n\n", 0, p),
                       para.rfind("| ", 0, p))
            prev = [(q, n) for q, n in names if stop < q < p]
            if not prev:
                prev = [(q, n) for q, n in names if q < p]
                if not prev:
                    continue
            attributed.setdefault((sec, prev[-1][1]), set()).add(
                float(m.group(1)))

    stale, ok, unresolved = [], [], []
    for (sec, name), nums in sorted(attributed.items()):
        cands = fnscore[name]
        if len(cands) != 1:
            unresolved.append((name, sec))
            continue
        unit, cur = cands[0]
        near = [v for v in nums if abs(v - cur) <= args.tol]
        row = (sec, name, unit, cur, sorted(nums)[:6])
        (ok if near else stale).append(row)

    seen = set()
    print("=" * 100)
    print("ROWS WHOSE QUOTED FIGURES NO LONGER INCLUDE THE FUNCTION'S CURRENT "
          "SCORE  (baseline moved -> re-price)")
    print("=" * 100)
    n = 0
    for sec, name, unit, cur, nums in stale:
        key = (sec, name)
        if key in seen:
            continue
        seen.add(key)
        if any(k == (s2, name) for (s2, n2, u2, c2, q2) in ok
               for k in [(s2, n2)]):
            continue
        n += 1
        print("%-52s %-34s now %8.4f   ledger quotes %s"
              % (sec[:52], name[:34], cur, nums))
    print("\n%d stale rows" % n)

    seen2 = set()
    print("\n" + "=" * 100)
    print("ROWS STILL STANDING AT THE FIGURE THEY WERE PRICED AT")
    print("=" * 100)
    m = 0
    for sec, name, unit, cur, nums in ok:
        key = (sec, name)
        if key in seen2:
            continue
        seen2.add(key)
        m += 1
        print("%-52s %-34s %8.4f" % (sec[:52], name[:34], cur))
    print("\n%d confirmed rows" % m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
