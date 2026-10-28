#!/usr/bin/env python3
"""Float-aware re-partition of the sub-100 population.

`perm_class_scan.py` abstracts only GPR names (`\\br\\d{1,2}\\b`).  Two
consequences, both measured here:

  1. Any row whose only difference is WHICH FLOAT REGISTER a value was coloured
     with fails the `REG.sub("r#", ...)` operand-text equality and is filed
     under OPERAND -- the bucket that was read as frame size / displacement
     (#67).  Float colouring rows are invisible to the GPR-only classifier.

  2. MWCC saves callee-saved FPRs one instruction per register
     (`stfd f31,-8(r1)`, `stfd f30,-16(r1)`, ...) rather than through a
     `_savefpr`-style helper.  When the SET of saved registers is the same the
     save block is byte-identical on both sides -- but a whole-stream
     permutation check still applies sigma to those lines and finds
     `stfd sigma(f31),...` != `stfd f31,...`.  So EVERY float colouring
     difference reports NOTPERM.  The save block declares the SET; it is not a
     use of the value, so sigma is not required to explain it.

This scanner abstracts `r` AND `f` names, derives the two maps separately
(the namespaces are disjoint), and exempts an IDENTICAL callee-saved FPR
save/restore line against r1 from sigma rewriting.  It reports the OLD bucket
alongside the NEW one so the re-partition can be read as a cross-tab.

Usage:
  python3 tools/fp_perm_class_scan.py                  re-partition + cross-tab
  python3 tools/fp_perm_class_scan.py --json FILE      dump the rows
  python3 tools/fp_perm_class_scan.py --self-test      controls (must all pass)
  python3 tools/fp_perm_class_scan.py --spill-control  exemption on/off delta
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
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize
import perm_class_scan as OLD

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"

GPR = re.compile(r"\br(\d{1,2})\b")
FPR = re.compile(r"\bf(\d{1,2})\b")
MR = re.compile(r"^mr\s+r(\d+),r(\d+)$")
SAVED_G = frozenset(range(14, 32))
SAVED_F = frozenset(range(14, 32))

# an FPR callee-save / restore against the stack pointer
FPSPILL = re.compile(r"^(stfd|lfd|psq_st|psq_l|stfs|lfs)\s+f(\d{1,2}),"
                     r"(-?(?:0x)?[0-9a-fA-F]+)\(r1\)")
# a GPR callee-save / restore against the stack pointer (individual, not stmw)
GPSPILL = re.compile(r"^(stw|lwz)\s+r(\d{1,2}),(-?(?:0x)?[0-9a-fA-F]+)\(r1\)")


def toks(line):
    if line.startswith("RELOC"):
        return None
    p = line.split(None, 1)
    return (p[0], p[1] if len(p) > 1 else "")


def is_savedreg_spill(line):
    """(kind, reg) if `line` is a callee-saved register's r1 save/restore."""
    m = FPSPILL.match(line)
    if m and int(m.group(2)) in SAVED_F:
        return ("f", int(m.group(2)))
    m = GPSPILL.match(line)
    if m and int(m.group(2)) in SAVED_G:
        return ("r", int(m.group(2)))
    return None


def abstract(ops):
    return FPR.sub("f#", GPR.sub("r#", ops))


def permutation(cur, tgt, exempt_spills=True):
    """Register map(s) rewriting `cur` into `tgt`, or (None, reason).

    Returns ({'r': map, 'f': map}, "PERM") on success.  With `exempt_spills`,
    an IDENTICAL callee-saved-register save/restore against r1 is not required
    to satisfy sigma (the block names the SET, not the assignment)."""
    if len(cur) != len(tgt):
        return None, "LENDIFF"
    mapping = {"r": defaultdict(set), "f": defaultdict(set)}
    for a, b in zip(cur, tgt):
        if a == b:
            continue
        ta, tb = toks(a), toks(b)
        if ta is None or tb is None:
            continue                      # reloc NAME at an equal address (#70)
        if ta[0] != tb[0]:
            return None, "MNEMONIC"
        if abstract(ta[1]) != abstract(tb[1]):
            return None, "OPERAND"
        for kind, rx in (("r", GPR), ("f", FPR)):
            xa, xb = rx.findall(ta[1]), rx.findall(tb[1])
            if len(xa) != len(xb):
                return None, "OPERAND"
            for x, y in zip(xa, xb):
                if x != y:
                    mapping[kind][int(x)].add(int(y))
    for kind in ("r", "f"):
        if any(len(v) != 1 for v in mapping[kind].values()):
            return None, "NONFUNC"
    m = {k: {a: next(iter(v)) for a, v in mapping[k].items()} for k in ("r", "f")}
    for kind in ("r", "f"):
        if len(set(m[kind].values())) != len(m[kind]):
            return None, "NONINJ"

    def rep(ops):
        ops = GPR.sub(lambda mo: "r%d" % m["r"].get(int(mo.group(1)),
                                                    int(mo.group(1))), ops)
        return FPR.sub(lambda mo: "f%d" % m["f"].get(int(mo.group(1)),
                                                    int(mo.group(1))), ops)

    exempted = 0
    for a, b in zip(cur, tgt):
        ta, tb = toks(a), toks(b)
        if ta is None or tb is None:
            continue
        if ta[0] + " " + rep(ta[1]) == tb[0] + " " + tb[1]:
            continue
        if exempt_spills and a == b and is_savedreg_spill(a):
            exempted += 1
            continue
        return None, "NOTPERM"
    m["_exempted"] = exempted
    return m, "PERM"


def kinds_moved(m):
    g, f = bool(m["r"]), bool(m["f"])
    return "GPR+FPR" if g and f else ("FPR" if f else ("GPR" if g else "NONE"))


def classify(m, homes_cur):
    """Sub-class of a PERM row, reported per register kind."""
    out = []
    if m["r"]:
        touched = set(m["r"]) | set(m["r"].values())
        if not all(r in SAVED_G for r in touched):
            out.append("r:SCRATCH")
        elif set(m["r"]) & {d for _, d in homes_cur}:
            out.append("r:PARAM-HOME")
        else:
            out.append("r:LOCALS-ONLY")
    if m["f"]:
        touched = set(m["f"]) | set(m["f"].values())
        if not all(r in SAVED_F for r in touched):
            out.append("f:VOLATILE" if not (touched & SAVED_F) else "f:MIXED")
        else:
            out.append("f:SAVED")
    return "+".join(out) or "NONE"


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
        old_m, old_why = OLD.permutation(c, t)
        if old_m is not None:
            old_why = OLD.classify(old_m, OLD.param_homes(c))
        m, why = permutation(c, t)
        m_nx, why_nx = permutation(c, t, exempt_spills=False)
        row = dict(unit=unit_name, sym=sym, fuzzy=fz, size=size,
                   old=old_why, new=why, new_nospill=why_nx)
        if m is not None:
            hc = OLD.param_homes(c)
            row.update(kinds=kinds_moved(m), cls=classify(m, hc),
                       rmap=m["r"], fmap=m["f"], exempted=m["_exempted"])
        else:
            # for a non-PERM row still record which kinds have differing operands
            dg = df = 0
            for a, b in zip(c, t):
                if a == b:
                    continue
                ta, tb = toks(a), toks(b)
                if ta is None or tb is None:
                    continue
                dg += sum(1 for x, y in zip(GPR.findall(ta[1]),
                                            GPR.findall(tb[1])) if x != y)
                df += sum(1 for x, y in zip(FPR.findall(ta[1]),
                                            FPR.findall(tb[1])) if x != y)
            row.update(kinds=None, cls=None, diff_gpr=dg, diff_fpr=df)
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

    # the exact A100 trap: identical save block, permuted float uses
    S = ["stfd f31,-8(r1)", "stfd f30,-16(r1)", "fmuls f31,f1,f2",
         "fadds f30,f31,f3", "lfd f30,-16(r1)", "lfd f31,-8(r1)", "blr"]
    T = ["stfd f31,-8(r1)", "stfd f30,-16(r1)", "fmuls f30,f1,f2",
         "fadds f31,f30,f3", "lfd f30,-16(r1)", "lfd f31,-8(r1)", "blr"]
    chk("the GPR-only classifier files this float perm under OPERAND",
        OLD.permutation(S, T)[1] == "OPERAND")
    m, why = permutation(S, T)
    chk("float-aware + spill exemption recognises it as PERM",
        why == "PERM" and m["f"] == {31: 30, 30: 31} and m["_exempted"] == 4)
    chk("without the exemption the SAME row reports NOTPERM",
        permutation(S, T, exempt_spills=False)[1] == "NOTPERM")
    chk("its class is f:SAVED", classify(m, []) == "f:SAVED")
    chk("kinds_moved says FPR", kinds_moved(m) == "FPR")

    # a volatile-only float permutation needs no exemption at all
    V = ["fmuls f0,f1,f2", "fadds f3,f0,f4", "blr"]
    W = ["fmuls f5,f1,f2", "fadds f3,f5,f4", "blr"]
    m2, why2 = permutation(V, W)
    chk("a volatile-only float perm is PERM with 0 exemptions",
        why2 == "PERM" and m2["_exempted"] == 0 and m2["f"] == {0: 5})
    chk("its class is f:VOLATILE", classify(m2, []) == "f:VOLATILE")
    chk("the GPR-only classifier files IT under OPERAND too",
        OLD.permutation(V, W)[1] == "OPERAND")

    # the exemption must not hide a REAL difference
    chk("a DIFFERING save line is never exempted",
        permutation(["stfd f31,-8(r1)", "blr"],
                    ["stfd f31,-16(r1)", "blr"])[1] == "OPERAND")
    chk("a VOLATILE fpr spill against r1 is not a callee-save",
        is_savedreg_spill("stfd f2,8(r1)") is None)
    chk("a callee-saved fpr spill against r1 is",
        is_savedreg_spill("stfd f31,-8(r1)") == ("f", 31))
    chk("a spill against a base OTHER than r1 is not",
        is_savedreg_spill("stfd f31,-8(r30)") is None)

    # mixed GPR+FPR permutation
    X = ["lfs f31,0(r26)", "fmuls f0,f31,f31", "blr"]
    Y = ["lfs f30,0(r27)", "fmuls f0,f30,f30", "blr"]
    m3, why3 = permutation(X, Y)
    chk("a mixed GPR+FPR permutation is recognised in both namespaces",
        why3 == "PERM" and m3["r"] == {26: 27} and m3["f"] == {31: 30})
    chk("kinds_moved says GPR+FPR", kinds_moved(m3) == "GPR+FPR")
    chk("the two namespaces do not alias (f26 and r26 are distinct)",
        permutation(["fadds f26,f1,f2", "add r3,r26,r4"],
                    ["fadds f27,f1,f2", "add r3,r26,r4"])[0]["f"] == {26: 27})
    chk("a non-injective FLOAT rename is rejected",
        permutation(["fmr f30,f1", "fmr f31,f2"],
                    ["fmr f29,f1", "fmr f29,f2"])[1] == "NONINJ")
    chk("an inconsistent FLOAT rename is rejected",
        permutation(["fmr f30,f1", "fmr f2,f30"],
                    ["fmr f29,f1", "fmr f2,f28"])[1] == "NONFUNC")
    chk("a pure GPR permutation still classifies as before",
        permutation(["mr r26,r3", "add r27,r26,r4", "blr"],
                    ["mr r28,r3", "add r27,r28,r4", "blr"])[0]["r"] == {26: 28})
    chk("a differing MNEMONIC is still rejected",
        permutation(["fadds f1,f2,f3"], ["fsubs f1,f2,f3"])[1] == "MNEMONIC")
    chk("a differing DISPLACEMENT is still rejected",
        permutation(["lfs f1,8(r4)"], ["lfs f1,12(r4)"])[1] == "OPERAND")
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def report(rows, spill_control=False):
    n_old, b_old = Counter(), Counter()
    n_new, b_new = Counter(), Counter()
    for r in rows:
        n_old[r["old"]] += 1
        b_old[r["old"]] += r["size"]
        n_new[r["new"]] += 1
        b_new[r["new"]] += r["size"]
    print(f"# {len(rows)} sub-100 rows compared\n")
    print("## OLD (GPR-only) buckets            ## NEW (float-aware) buckets")
    keys = sorted(set(n_old) | set(n_new), key=lambda k: -b_old[k] - b_new[k])
    for k in keys:
        print(f"  {k:14s} {n_old[k]:4d} rows {b_old[k]:7d} B "
              f"  |  {k:14s} {n_new[k]:4d} rows {b_new[k]:7d} B")
    print()
    if spill_control:
        moved = [r for r in rows
                 if r["new"] == "PERM" and r["new_nospill"] != "PERM"]
        print(f"## SPILL-EXEMPTION CONTROL: {len(moved)} rows are PERM only "
              f"because the identical callee-save block is exempt")
        for r in sorted(moved, key=lambda x: -x["size"]):
            print(f"   {r['new_nospill']:9s} -> PERM  {r['size']:6d} B  "
                  f"{r['kinds']:8s} {r['cls']:22s} exempt={r['exempted']:2d}  "
                  f"{r['unit']}  {r['sym']}")
        print()
        return

    print("## CROSS-TAB  old bucket -> new bucket (rows / bytes)")
    ct = defaultdict(lambda: [0, 0])
    for r in rows:
        c = ct[(r["old"], r["new"])]
        c[0] += 1
        c[1] += r["size"]
    for (o, nw), (c, bts) in sorted(ct.items(), key=lambda x: -x[1][1]):
        tag = "  <== RECLASSIFIED" if o != nw else ""
        print(f"  {o:14s} -> {nw:14s} {c:4d} rows {bts:7d} B{tag}")
    print()

    perm = [r for r in rows if r["new"] == "PERM"]
    kn, kb = Counter(), Counter()
    for r in perm:
        kn[r["kinds"]] += 1
        kb[r["kinds"]] += r["size"]
    print(f"## PURE PERMUTATION CLASS, float-aware: {len(perm)} rows "
          f"{sum(r['size'] for r in perm)} B")
    for k, v in kn.most_common():
        print(f"   {k:10s} {v:4d} rows {kb[k]:7d} B")
    print()
    cn, cb = Counter(), Counter()
    for r in perm:
        cn[r["cls"]] += 1
        cb[r["cls"]] += r["size"]
    print("## by sub-class")
    for k, v in cn.most_common():
        print(f"   {k:26s} {v:4d} rows {cb[k]:7d} B")
    print()
    print("## the float-carrying PERM rows")
    for r in sorted((x for x in perm if "f" in (x["kinds"] or "").lower()),
                    key=lambda x: -x["size"]):
        print(f"   {r['size']:6d} B {r['fuzzy']:8.3f} {r['cls']:22s} "
              f"was={r['old']:9s} fmap={r['fmap']}  {r['unit']}  {r['sym']}")
    print()
    print("## residual non-PERM rows, by which register kind actually differs")
    resid = [r for r in rows if r["new"] != "PERM"]
    rn, rb = Counter(), Counter()
    for r in resid:
        g, f = r.get("diff_gpr", 0), r.get("diff_fpr", 0)
        k = ("GPR+FPR" if g and f else "FPR-only" if f else
             "GPR-only" if g else "no-reg-diff")
        rn[(r["new"], k)] += 1
        rb[(r["new"], k)] += r["size"]
    for k, v in sorted(rn.items(), key=lambda x: -rb[x[0]]):
        print(f"   {k[0]:12s} {k[1]:12s} {v:4d} rows {rb[k]:7d} B")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--spill-control", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        raise SystemExit(self_test())
    rows = scan()
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
    report(rows, spill_control=a.spill_control)


if __name__ == "__main__":
    main()
