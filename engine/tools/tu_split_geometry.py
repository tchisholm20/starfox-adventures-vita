"""Geometric feasibility test for a proposed per-profile TU split.

A real TU boundary is an ADDRESS: every function below it was compiled with one
profile, every function above it with the other.  So for a unit whose functions
disagree on profile, sort the functions by retail address and ask whether any
single cut point separates "only profile P reproduces this" from "only the
configured profile reproduces this".

Reads a tu_profile_census*.jsonl record and config/GSAE01/symbols.txt.
A unit is only a boundary candidate when some cut point misplaces ZERO
functions; anything else is an interleaved contradiction.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SYMBOLS = REPO / "config/GSAE01/symbols.txt"


def addrs():
    out = {}
    rx = re.compile(r"^(\S+)\s*=\s*\.text:0x([0-9A-Fa-f]+);")
    for line in SYMBOLS.read_text(errors="replace").splitlines():
        m = rx.match(line)
        if m and "type:function" in line:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def main():
    A = addrs()
    for line in Path(sys.argv[1]).read_text().splitlines():
        d = json.loads(line)
        if d.get("status") != "CONFLICT":
            continue
        print(f"### {d['unit']}   configured={d.get('configured','?')}")
        for p, v in sorted(d["per_profile"].items(), key=lambda kv: -kv[1]["n"]):
            if p == "as-configured" or not v["gain"]:
                continue
            g = [(A.get(f, -1), f, "P") for f in v["gain"]]
            l = [(A.get(f, -1), f, "C") for f in v["loss"]]
            seq = sorted(g + l)
            if any(a < 0 for a, _, _ in seq):
                print(f"   {p}: missing addresses, skipped")
                continue
            tags = [t for _, _, t in seq]
            # best cut: minimise functions on the wrong side
            best = min(
                min(sum(1 for t in tags[:k] if t != "P") + sum(1 for t in tags[k:] if t != "C"),
                    sum(1 for t in tags[:k] if t != "C") + sum(1 for t in tags[k:] if t != "P"))
                for k in range(len(tags) + 1))
            verdict = "SPLIT-FEASIBLE" if best == 0 else f"INTERLEAVED (min {best} misplaced)"
            print(f"   {p:16s} gain={len(v['gain'])} loss={len(v['loss'])}  "
                  f"order={''.join(tags)}  {verdict}")
            if best == 0:
                cut = [(seq[k - 1][0], seq[k][0]) for k in range(1, len(seq))
                       if seq[k - 1][2] != seq[k][2]]
                print(f"      cut between " + ", ".join(f"0x{a:08X}/0x{b:08X}" for a, b in cut))
        print()


if __name__ == "__main__":
    main()
