"""Drive the interning-law adjudicator over a tu_profile_census run.

For every CONFLICT unit the census reports, partition its functions into
  group0 = the functions only a NON-configured profile reproduces
  group1 = the functions that profile would BREAK (they need the configured one)
and ask the retail DOL whether those two groups can possibly be two objects.

Verdict per unit:
  ONE_TU        the groups share an interned atom, or their atoms interleave
  SPLITTABLE    the groups' atoms form two contiguous runs and a duplicate atom
                sits between them (positive evidence for two objects)
  SILENT        one group mints nothing; the law cannot speak
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def main():
    census = Path(sys.argv[1])
    for line in census.read_text().splitlines():
        d = json.loads(line)
        if d.get("status") != "CONFLICT":
            continue
        # the profile that buys the most, weighted by what it costs
        cands = [(v["n"], p) for p, v in d["per_profile"].items()
                 if p != "as-configured" and v["gain"]]
        cands.sort(reverse=True)
        g0, g1 = set(), set()
        for _, p in cands:
            g0 |= set(d["per_profile"][p]["gain"])
            g1 |= set(d["per_profile"][p]["loss"])
        g1 -= g0
        if not g0 or not g1:
            print(f"### {d['unit']}: degenerate groups g0={len(g0)} g1={len(g1)}")
            continue
        args = [sys.executable, str(REPO / "tools/interning_adjudicate.py"), d["unit"],
                "--groups", ",".join(sorted(g0)), ",".join(sorted(g1))]
        out = subprocess.run(args, capture_output=True, text=True, cwd=REPO).stdout
        keep = [l for l in out.splitlines()
                if ("=>" in l or "DUPLICATE" in l or l.startswith("===")
                    or "no duplicate" in l or "no shared" in l)]
        print(f"### {d['unit']}  g0={len(g0)} g1={len(g1)}  profiles={[p for _, p in cands[:3]]}")
        for l in keep:
            print("   " + l.strip())


if __name__ == "__main__":
    main()
