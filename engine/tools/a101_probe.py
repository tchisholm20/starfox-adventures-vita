#!/usr/bin/env python3
"""Probe harness: apply a source variant, rebuild ONE object, score ONE function.

Scores the function directly from the two objects with the same normalisation
`ndiff`/`perm_class_scan` use, so a probe costs one compile and one objdump pair
instead of a whole-tree report.  The objdiff weights are applied exactly:
1.00 per differing mnemonic or length mismatch, 0.05 per differing register
operand, 0.01 per differing immediate/displacement, 0.00 for a relocation name
at an equal address -- the ordering of probes was cross-checked against the real
`report.json` figure (2.43 here reproduced 98.785 on a 200-instruction body to
the digit), so the cheap score is not trusted blind.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize
from direct_build import direct_build
import fp_perm_class_scan as N

REPO = Path(__file__).resolve().parent.parent
OBJDUMP = N.OBJDUMP
NUM = re.compile(r"-?\b\d+\b")


def unit_objs(unit):
    cfgs = {u["name"].replace("\\", "/"): u
            for u in load_units(REPO / "build/GSAE01/config.json")}
    c = cfgs[unit]
    return REPO / c["object"], REPO / c["object"].replace("/obj/", "/src/", 1)


def cost(cur, tgt):
    """objdiff-weighted distance between two normalised streams."""
    if len(cur) != len(tgt):
        return 1000.0 + abs(len(cur) - len(tgt))
    c = 0.0
    for a, b in zip(cur, tgt):
        if a == b:
            continue
        ta, tb = N.toks(a), N.toks(b)
        if ta is None or tb is None:
            continue                       # reloc name at an equal address
        if ta[0] != tb[0]:
            c += 1.0
            continue
        for rx in (N.GPR, N.FPR):
            xa, xb = rx.findall(ta[1]), rx.findall(tb[1])
            if len(xa) != len(xb):
                c += 1.0
                break
            c += 0.05 * sum(1 for x, y in zip(xa, xb) if x != y)
        na = NUM.findall(N.abstract(ta[1]))
        nb = NUM.findall(N.abstract(tb[1]))
        if len(na) == len(nb):
            c += 0.01 * sum(1 for x, y in zip(na, nb) if x != y)
        else:
            c += 1.0
    return c


def score(unit, sym, rebuild=True):
    ot, ou = unit_objs(unit)
    if rebuild and not direct_build(str(ou.relative_to(REPO))):
        return None
    t = normalize(strip_preamble(objdump_symbol(OBJDUMP, ot, sym)), sym)
    c = normalize(strip_preamble(objdump_symbol(OBJDUMP, ou, sym)), sym)
    return cost(c, t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit")
    ap.add_argument("sym")
    ap.add_argument("--no-rebuild", action="store_true")
    a = ap.parse_args()
    print(score(a.unit, a.sym, rebuild=not a.no_rebuild))


if __name__ == "__main__":
    main()
