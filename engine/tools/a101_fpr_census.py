#!/usr/bin/env python3
"""Exact FPR census of the sub-100 population + the spill-exemption controls.

Three questions the float-aware re-partition needs answered separately:

  A. How many sub-100 rows have a DIFFERING FLOAT REGISTER OPERAND at all, how
     many bytes, and how many of those are FLOAT-ONLY (no differing GPR)?
  B. Of the float-only rows, how many are VOLATILE-ONLY (every differing float
     operand in f0-f13, so no declaration can reach them -- A100 measured 8085
     differing FPR operands with zero in f0-f13 for the SAVED half)?
  C. Does the callee-save-block exemption change the partition, and does
     A100's looser rule (exempt ANY `stfd/lfd/psq_st/psq_l fN,imm(r1)`
     regardless of the register band or whether the line even differs) hide
     real residual that the banded rule keeps?

Usage:  python3 tools/a101_fpr_census.py [--json FILE] [--self-test]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import objdump_symbol, strip_preamble
from ndiff import normalize
import perm_class_scan as OLD
import fp_perm_class_scan as NEW

REPO = Path(__file__).resolve().parent.parent
OBJDUMP = NEW.OBJDUMP
GPR, FPR = NEW.GPR, NEW.FPR
VOL_F = frozenset(range(0, 14))

# A100's rule, verbatim in shape: no band test, no identity test
LOOSE = re.compile(r"^(stfd|lfd|psq_st|psq_l)\s+f\d+,-?\d+\(r1\)")


def loose_perm(cur, tgt):
    """NEW.permutation with A100's looser exemption."""
    if len(cur) != len(tgt):
        return None, "LENDIFF"
    mapping = {"r": defaultdict(set), "f": defaultdict(set)}
    for a, b in zip(cur, tgt):
        if a == b or LOOSE.match(a) or LOOSE.match(b):
            continue
        ta, tb = NEW.toks(a), NEW.toks(b)
        if ta is None or tb is None:
            continue
        if ta[0] != tb[0]:
            return None, "MNEMONIC"
        if NEW.abstract(ta[1]) != NEW.abstract(tb[1]):
            return None, "OPERAND"
        for kind, rx in (("r", GPR), ("f", FPR)):
            xa, xb = rx.findall(ta[1]), rx.findall(tb[1])
            if len(xa) != len(xb):
                return None, "OPERAND"
            for x, y in zip(xa, xb):
                if x != y:
                    mapping[kind][int(x)].add(int(y))
    for k in ("r", "f"):
        if any(len(v) != 1 for v in mapping[k].values()):
            return None, "NONFUNC"
    m = {k: {a: next(iter(v)) for a, v in mapping[k].items()} for k in ("r", "f")}
    for k in ("r", "f"):
        if len(set(m[k].values())) != len(m[k]):
            return None, "NONINJ"

    def rep(ops):
        ops = GPR.sub(lambda mo: "r%d" % m["r"].get(int(mo.group(1)),
                                                    int(mo.group(1))), ops)
        return FPR.sub(lambda mo: "f%d" % m["f"].get(int(mo.group(1)),
                                                    int(mo.group(1))), ops)

    for a, b in zip(cur, tgt):
        if LOOSE.match(a) or LOOSE.match(b):
            continue
        ta, tb = NEW.toks(a), NEW.toks(b)
        if ta is None or tb is None:
            continue
        if ta[0] + " " + rep(ta[1]) != tb[0] + " " + tb[1]:
            return None, "NOTPERM"
    return m, "PERM"


def analyze(args):
    unit_name, obj_our, obj_tgt, syms = args
    out = []
    for sym, fz, size in syms:
        try:
            t = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_tgt, sym)), sym)
            c = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_our, sym)), sym)
        except Exception:
            continue
        if t == c:
            continue
        dg = df = 0
        fops = []          # (ours, theirs) differing float operand pairs
        aligned = len(c) == len(t)
        if aligned:
            for a, b in zip(c, t):
                if a == b:
                    continue
                ta, tb = NEW.toks(a), NEW.toks(b)
                if ta is None or tb is None:
                    continue
                for x, y in zip(GPR.findall(ta[1]), GPR.findall(tb[1])):
                    if x != y:
                        dg += 1
                for x, y in zip(FPR.findall(ta[1]), FPR.findall(tb[1])):
                    if x != y:
                        fops.append((int(x), int(y)))
                        df += 1
        row = dict(unit=unit_name, sym=sym, fuzzy=fz, size=size,
                   aligned=aligned, diff_gpr=dg, diff_fpr=df,
                   fops=fops,
                   fvol=sum(1 for x, y in fops
                            if x in VOL_F and y in VOL_F),
                   fsav=sum(1 for x, y in fops
                            if x not in VOL_F or y not in VOL_F),
                   new=NEW.permutation(c, t)[1],
                   loose=loose_perm(c, t)[1])
        out.append(row)
    return out


def scan():
    res = []
    with ProcessPoolExecutor(max_workers=10) as ex:
        for r in ex.map(analyze, OLD.jobs()):
            res.extend(r)
    return res


def self_test():
    ok = True

    def chk(name, cond):
        nonlocal ok
        print(f"  {'PASS' if cond else 'FAIL'}  {name}")
        ok = ok and bool(cond)

    # the loose rule silently swallows a VOLATILE spill difference
    S = ["stfd f0,8(r1)", "fadds f1,f2,f3", "blr"]
    T = ["stfd f0,12(r1)", "fadds f1,f2,f3", "blr"]
    chk("banded rule KEEPS a volatile-spill displacement difference",
        NEW.permutation(S, T)[1] == "OPERAND")
    chk("A100's loose rule HIDES it (reports a clean PERM)",
        loose_perm(S, T)[1] == "PERM")
    U = ["stfd f31,8(r1)", "fadds f1,f2,f3", "blr"]
    V = ["stfd f31,12(r1)", "fadds f1,f2,f3", "blr"]
    chk("banded rule keeps a DIFFERING callee-save displacement too",
        NEW.permutation(U, V)[1] == "OPERAND")
    chk("loose rule hides that as well", loose_perm(U, V)[1] == "PERM")
    chk("volatile set is f0-f13", 13 in VOL_F and 14 not in VOL_F)
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        raise SystemExit(self_test())
    rows = scan()
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)

    fl = [r for r in rows if r["diff_fpr"] > 0]
    fonly = [r for r in fl if r["diff_gpr"] == 0]
    print(f"# {len(rows)} sub-100 rows; {sum(r['size'] for r in rows)} B\n")
    print(f"## A. rows with ANY differing float operand: {len(fl)} rows "
          f"{sum(r['size'] for r in fl)} B")
    print(f"##    of which FLOAT-ONLY (no differing GPR):  {len(fonly)} rows "
          f"{sum(r['size'] for r in fonly)} B\n")

    vol = [r for r in fonly if r["fsav"] == 0]
    sav = [r for r in fonly if r["fvol"] == 0]
    mix = [r for r in fonly if r["fvol"] and r["fsav"]]
    print(f"## B. of the {len(fonly)} float-only rows:")
    print(f"##    VOLATILE-only (all differing f in f0-f13): {len(vol):3d} rows "
          f"{sum(r['size'] for r in vol):6d} B")
    print(f"##    SAVED-touching only                      : {len(sav):3d} rows "
          f"{sum(r['size'] for r in sav):6d} B")
    print(f"##    MIXED                                    : {len(mix):3d} rows "
          f"{sum(r['size'] for r in mix):6d} B")
    tv = sum(r["fvol"] for r in fl)
    ts = sum(r["fsav"] for r in fl)
    print(f"##    differing float OPERANDS tree-wide: {tv+ts} "
          f"({tv} volatile-pair, {ts} saved-touching)\n")

    print("## the FLOAT-ONLY rows")
    for r in sorted(fonly, key=lambda x: -x["size"]):
        band = ("VOL" if r["fsav"] == 0 else "SAV" if r["fvol"] == 0 else "MIX")
        print(f"   {r['size']:6d} B {r['fuzzy']:8.3f} {r['new']:9s} {band}  "
              f"fdiff={r['diff_fpr']:3d}  {r['unit']}  {r['sym']}")
    print()

    print("## C. EXEMPTION CONTROL: banded+identical rule vs A100's loose rule")
    d = defaultdict(lambda: [0, 0])
    for r in rows:
        if r["new"] != r["loose"]:
            c = d[(r["new"], r["loose"])]
            c[0] += 1
            c[1] += r["size"]
    if not d:
        print("   no row classifies differently under the two rules")
    for (n, l), (c, b) in sorted(d.items(), key=lambda x: -x[1][1]):
        print(f"   banded={n:9s} loose={l:9s} {c:4d} rows {b:7d} B")
    nb = sum(1 for r in rows if r["new"] == "PERM")
    nl = sum(1 for r in rows if r["loose"] == "PERM")
    print(f"   PERM under banded rule {nb}, under loose rule {nl}")


if __name__ == "__main__":
    main()
