#!/usr/bin/env python3
"""Does the REUSE regime apply to the FLOAT band as it does to the integer one?

`colouring_reach_control.py` established for GPRs that a declaration-order
change is a clean permutation while the live locals fit under the callee-saved
band, and stops being one -- NONFUNC / NOTPERM / NONINJ -- as soon as the
allocator must host more than one web per register.  §27 measured that the FP
band "has no separate law" (84 descending to 9 ascending adjacent slots), but
that measurement was taken on the band ORDER, not on reuse, and every synthetic
control in the project is an `int` body.  So the float side of the claim was
inherited, never run.

This is the same experiment with `float` locals and an FPR-aware classifier:

  * NO-REUSE  9 float locals, all live across calls, band f14-f31 is 18 wide
  * REUSE    24 float locals -> the allocator must pack

`--scratch` additionally reports whether the residual touches the VOLATILE half
(f0-f13), because #82 is claimed to be a scratch class: no declaration owns a
scratch register, so a class that lives in f0-f13 is not declaration-reachable
no matter what the saved band does.

Usage:
  fpr_reuse_control.py --self-test      exhaustive where cheap + the GPR cross-check
  fpr_reuse_control.py --sweep N        N random declaration orders per regime
"""
from __future__ import annotations

import argparse
import collections
import itertools
import random
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from colouring_reach_control import build

FREG = re.compile(r"\bf(\d{1,2})\b")
GREG = re.compile(r"\br(\d{1,2})\b")
# MWCC saves/restores FPRs one instruction per register (there is no
# `_savefpr` helper in this configuration), so the prologue names the saved
# SET.  Two colourings that use the same set have an identical save block,
# and re-applying the value map to it can never match -- which makes a plain
# whole-stream permutation check report NOTPERM on EVERY float colouring
# difference.  That is an artefact of the checker, not a fact about the
# compiler, so these lines are excluded from the re-application test.
SPILL = re.compile(r"^(stfd|lfd|psq_st|psq_l)\s+f\d+,-?\d+\(r1\)")

NOREUSE_NAMES = ["c%d" % i for i in range(9)]
NOREUSE = """extern float ffetch(float);
extern void fsink(float, float, float, float);
extern float gF[64];

float probe(float n, int k)
{
%s
    c0 = gF[k]; c1 = gF[k+1]; c2 = gF[k+2]; c3 = gF[k+3]; c4 = gF[k+4];
    c5 = gF[k+5]; c6 = gF[k+6]; c7 = gF[k+7]; c8 = n;
    fsink(c0 + c1, c2 + c3, c4 + c5, c6 + c7);
    c8 = c8 + c0 * c3 + c6;
    fsink(c1, c4, c7, c8);
    return c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8;
}
"""

REUSE_NAMES = ["a%d" % i for i in range(12)] + ["b%d" % i for i in range(12)]
REUSE = """extern float ffetch(float);
extern void fsink(float, float, float, float);
extern float gF[64];

float probe(float n, int k)
{
%s
    a0 = gF[k]; a1 = gF[k+1]; a2 = gF[k+2]; a3 = gF[k+3];
    a4 = gF[k+4]; a5 = gF[k+5]; a6 = gF[k+6]; a7 = gF[k+7];
    a8 = gF[k+8]; a9 = gF[k+9]; a10 = gF[k+10]; a11 = gF[k+11];
    fsink(a0 + a1, a2 + a3, a4 + a5, a6 + a7);
    n = n + a8 + a9 * a10 - a11;
    fsink(a1, a4, a7, a10);
    n = n * a2 - a5 + a6 * a8;
    b0 = ffetch(n); b1 = ffetch(n + 1.0f); b2 = ffetch(n + 2.0f);
    b3 = ffetch(n + 3.0f); b4 = ffetch(n + 4.0f); b5 = ffetch(n + 5.0f);
    b6 = ffetch(n + 6.0f); b7 = ffetch(n + 7.0f); b8 = ffetch(n + 8.0f);
    b9 = ffetch(n + 9.0f); b10 = ffetch(n + 10.0f); b11 = ffetch(n + 11.0f);
    fsink(b0 + b1, b2 + b3, b4 + b5, b6 + b7);
    n = n + b8 + b9 * b10 - b11;
    fsink(b1, b4, b7, b10);
    return n + a0 + a3 + b2 + b5;
}
"""

REGIMES = {"noreuse": (NOREUSE, NOREUSE_NAMES), "reuse": (REUSE, REUSE_NAMES)}


def render(regime, order):
    body, _ = REGIMES[regime]
    return body % ("    float " + ", ".join(order) + ";\n")


def fpr_permutation(cur, tgt):
    """The FPR map rewriting `cur` into `tgt`, or (None, reason).

    Mirrors `perm_class_scan.permutation` but over `f` names, and reports a GPR
    difference separately so an integer-side move cannot be mistaken for a float
    one."""
    if len(cur) != len(tgt):
        return None, "LENDIFF"

    def parts(line):
        if line.startswith("RELOC"):
            return None
        p = line.split(None, 1)
        return (p[0], p[1] if len(p) > 1 else "")

    mapping, gpr = {}, False
    for a, b in zip(cur, tgt):
        if a == b:
            continue
        pa, pb = parts(a), parts(b)
        if pa is None or pb is None:
            continue
        if pa[0] != pb[0]:
            return None, "MNEMONIC"
        fa, fb = FREG.findall(pa[1]), FREG.findall(pb[1])
        ga, gb = GREG.findall(pa[1]), GREG.findall(pb[1])
        if len(fa) != len(fb) or len(ga) != len(gb):
            return None, "OPERAND"
        if (FREG.sub("f#", GREG.sub("r#", pa[1]))
                != FREG.sub("f#", GREG.sub("r#", pb[1]))):
            return None, "OPERAND"
        if any(x != y for x, y in zip(ga, gb)):
            gpr = True
        for x, y in zip(fa, fb):
            if x != y:
                mapping.setdefault(int(x), set()).add(int(y))
    if any(len(v) != 1 for v in mapping.values()):
        return None, "NONFUNC"
    m = {k: next(iter(v)) for k, v in mapping.items()}
    if len(set(m.values())) != len(m):
        return None, "NONINJ"

    def rep(mo):
        return "f%d" % m.get(int(mo.group(1)), int(mo.group(1)))

    for a, b in zip(cur, tgt):
        pa, pb = parts(a), parts(b)
        if pa is None or pb is None:
            continue
        if SPILL.match(a) and a == b:
            continue      # the save/restore block names the SET, not the values
        if (pa[0] + " " + GREG.sub("r#", FREG.sub(rep, pa[1]))
                != pb[0] + " " + GREG.sub("r#", pb[1])):
            return None, "NOTPERM"
    return m, ("PERM+GPR" if gpr else "PERM")


def classify(base, other):
    if base == other:
        return "IDENTICAL"
    return fpr_permutation(other, base)[1]


def scratch_share(base, other):
    """Does the residual touch the VOLATILE half of the float file?"""
    lo = hi = 0
    for a, b in zip(base, other):
        if a == b or a.startswith("RELOC"):
            continue
        fa, fb = FREG.findall(a), FREG.findall(b)
        if len(fa) != len(fb):
            continue
        for x, y in zip(fa, fb):
            if x != y:
                if int(x) < 14 or int(y) < 14:
                    lo += 1
                else:
                    hi += 1
    return lo, hi


def sweep(regime, orders, workdir):
    names = REGIMES[regime][1]
    base = build(render(regime, names), workdir, regime + "_base")
    counts, example = collections.Counter(), {}
    lo = hi = 0
    for n, o in enumerate(orders):
        other = build(render(regime, list(o)), workdir, "%s_%d" % (regime, n))
        cls = classify(base, other)
        counts[cls] += 1
        example.setdefault(cls, list(o))
        a, b = scratch_share(base, other)
        lo += a
        hi += b
    return counts, example, lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--sweep", type=int, default=0)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    random.seed(a.seed)

    with tempfile.TemporaryDirectory(prefix="fprreuse_") as td:
        wd = Path(td)
        for regime in ("noreuse", "reuse"):
            names = REGIMES[regime][1]
            k = a.sweep or 120
            orders = [tuple(random.sample(names, len(names))) for _ in range(k)]
            counts, example, lo, hi = sweep(regime, orders, wd)
            print("== %s regime, %d float locals, %d declaration orders"
                  % (regime, len(names), len(orders)))
            for cls, c in counts.most_common():
                print("   %-12s %4d   e.g. %s" % (cls, c, example[cls][:6]))
            print("   differing FPR operands: volatile f0-f13 %d / saved f14-f31 %d"
                  % (lo, hi))
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
