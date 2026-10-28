#!/usr/bin/env python3
"""Partition the PURE REGISTER-PERMUTATION rows out of the sub-100 population.

A row is a PURE PERMUTATION when target and current disassembly have the same
length, align 1:1, every differing pair carries the SAME mnemonic and the same
operand text once register names are abstracted, and one injective map sigma
over register names rewrites our whole stream into retail's.  Such a row asks
the code generator for exactly retail's operations in exactly retail's order and
differs only in which register each value was coloured with -- so no operation,
ordering or addressing lever can reach it, and the only question left is the
allocator's colouring.

The rows are then split by WHAT sigma moves, because the three parts have
different (and differently reachable) keys:

  PARAM-HOME    sigma moves at least one register that the entry block homes an
                incoming argument into (`mr rC,rA`, rA in r3..r10).  Declaration
                order does not reach these: measured inert over declaration
                permutations, A91 split forms, parameter-list permutations,
                parameter retypes and 16 flag combinations.
  LOCALS-ONLY   sigma stays inside the callee-saved band but touches no home --
                the band-membership axis `slot_oracle`/`brute_match` work.
  SCRATCH       sigma moves a volatile register (r0-r13).  No declaration owns a
                scratch register; the key there is the EXPRESSION (A86).

`--canon` prints the tree-wide control this partition needs to be read against:
the share of functions whose parameter homes ascend with the argument register
number, measured on BOTH sides.  It is ~83 % on each -- "retail always allocates
parameter homes in argument order" is FALSE, and the elevated non-ascending rate
inside the mismatching rows is a selection effect, not a compiler difference.

Usage:
  python3 tools/perm_class_scan.py               partition + per-row table
  python3 tools/perm_class_scan.py --json FILE   also dump the rows
  python3 tools/perm_class_scan.py --canon       tree-wide ascending-home control
  python3 tools/perm_class_scan.py --self-test   controls (must all pass)
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"

REG = re.compile(r"\br(\d{1,2})\b")
MR = re.compile(r"^mr\s+r(\d+),r(\d+)$")
SAVED = frozenset(range(14, 32))


def toks(line):
    """(mnemonic, operands) for an instruction, None for a RELOC pseudo-line."""
    if line.startswith("RELOC"):
        return None
    p = line.split(None, 1)
    return (p[0], p[1] if len(p) > 1 else "")


def param_homes(stream):
    """Entry-block `mr rC,rA` homes, rA an incoming argument register.

    Stops at the first instruction that is neither a home nor part of the
    prologue, and at a repeat of an argument register (a later `mr` into the
    same argument register is a reload, not a home)."""
    out = []
    seen = set()
    for line in stream:
        tk = toks(line)
        if tk is None:
            continue
        m = MR.match(tk[0] + " " + tk[1])
        if m:
            dst, src = int(m.group(1)), int(m.group(2))
            if 3 <= src <= 10 and dst in SAVED:
                if src in seen:
                    break
                seen.add(src)
                out.append((src, dst))
                continue
        if out:
            break
    return out


def permutation(cur, tgt):
    """The register map rewriting `cur` into `tgt`, or (None, reason)."""
    if len(cur) != len(tgt):
        return None, "LENDIFF"
    mapping = {}
    for a, b in zip(cur, tgt):
        if a == b:
            continue
        ta, tb = toks(a), toks(b)
        if ta is None or tb is None:
            continue                      # reloc NAME at an equal address (#70)
        if ta[0] != tb[0]:
            return None, "MNEMONIC"
        ra, rb = REG.findall(ta[1]), REG.findall(tb[1])
        if len(ra) != len(rb) or REG.sub("r#", ta[1]) != REG.sub("r#", tb[1]):
            return None, "OPERAND"
        for x, y in zip(ra, rb):
            if x != y:
                mapping.setdefault(int(x), set()).add(int(y))
    if any(len(v) != 1 for v in mapping.values()):
        return None, "NONFUNC"
    m = {k: next(iter(v)) for k, v in mapping.items()}
    if len(set(m.values())) != len(m):
        return None, "NONINJ"

    def rep(mo):
        return "r%d" % m.get(int(mo.group(1)), int(mo.group(1)))

    for a, b in zip(cur, tgt):
        ta, tb = toks(a), toks(b)
        if ta is None or tb is None:
            continue
        if ta[0] + " " + REG.sub(rep, ta[1]) != tb[0] + " " + tb[1]:
            return None, "NOTPERM"
    return m, "PERM"


def classify(m, homes_cur):
    if not all(r in SAVED for r in set(m) | set(m.values())):
        return "SCRATCH"
    if set(m) & {d for _, d in homes_cur}:
        return "PARAM-HOME"
    return "LOCALS-ONLY"


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
        m, why = permutation(c, t)
        if m is None:
            out.append(dict(unit=unit_name, sym=sym, fuzzy=fz, size=size,
                            cls=why, map=None, T=None, C=None))
            continue
        hc, ht = param_homes(c), param_homes(t)
        out.append(dict(unit=unit_name, sym=sym, fuzzy=fz, size=size,
                        cls=classify(m, hc), map=m, T=ht, C=hc))
    return out


def jobs():
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    units_cfg = {u["name"].replace("\\", "/"): u
                 for u in load_units(REPO / "build" / VER / "config.json")}
    js = []
    for u in report["units"]:
        sp = (u.get("metadata") or {}).get("source_path")
        if not sp:
            continue
        cfg = units_cfg.get(sp[4:] if sp.startswith("src/") else sp)
        if cfg is None:
            continue
        syms = [(f["name"], f.get("fuzzy_match_percent", -1.0), int(f.get("size", 0)))
                for f in u.get("functions", [])
                if f.get("fuzzy_match_percent", -1.0) < 100.0]
        if not syms:
            continue
        obj_tgt = REPO / cfg["object"]
        obj_our = REPO / cfg["object"].replace("/obj/", "/src/", 1)
        if obj_our.is_file() and obj_tgt.is_file():
            js.append((cfg["name"], obj_our, obj_tgt, syms))
    return js


def scan():
    res = []
    with ProcessPoolExecutor(max_workers=10) as ex:
        for r in ex.map(analyze, jobs()):
            res.extend(r)
    return res


# ------------------------------------------------------------------ canon
def _canon_job(a):
    obj, syms = a
    out = []
    for s in syms:
        try:
            h = param_homes(normalize(strip_preamble(objdump_symbol(OBJDUMP, obj, s)), s))
        except Exception:
            continue
        if len(h) >= 2:
            hs = sorted(h)
            out.append(all(hs[i][1] < hs[i + 1][1] for i in range(len(hs) - 1)))
    return out


def canon():
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    units_cfg = {u["name"].replace("\\", "/"): u
                 for u in load_units(REPO / "build" / VER / "config.json")}
    jt, jc = [], []
    for u in report["units"]:
        sp = (u.get("metadata") or {}).get("source_path")
        if not sp:
            continue
        cfg = units_cfg.get(sp[4:] if sp.startswith("src/") else sp)
        if cfg is None:
            continue
        syms = [f["name"] for f in u.get("functions", [])]
        if not syms:
            continue
        ot = REPO / cfg["object"]
        ou = REPO / cfg["object"].replace("/obj/", "/src/", 1)
        if ot.is_file():
            jt.append((ot, syms))
        if ou.is_file():
            jc.append((ou, syms))
    for name, js in (("RETAIL", jt), ("OURS", jc)):
        res = []
        with ProcessPoolExecutor(max_workers=10) as ex:
            for r in ex.map(_canon_job, js):
                res.extend(r)
        n = len(res)
        a = sum(1 for x in res if x)
        print(f"{name}: {n} fns with >=2 parameter homes; ascending in argument "
              f"order {a} ({100.0 * a / max(1, n):.2f}%)")


# -------------------------------------------------------------- self-test
def self_test():
    ok = True

    def chk(name, cond):
        nonlocal ok
        print(f"  {'PASS' if cond else 'FAIL'}  {name}")
        ok = ok and bool(cond)

    S = ["mr r26,r3", "add r27,r26,r4", "blr"]
    T = ["mr r28,r3", "add r27,r28,r4", "blr"]
    m, why = permutation(S, T)
    chk("a real permutation is recognised", m == {26: 28} and why == "PERM")
    chk("its class is PARAM-HOME when the moved reg is a home",
        classify(m, param_homes(S)) == "PARAM-HOME")
    chk("a differing MNEMONIC is rejected",
        permutation(["add r3,r3,r4"], ["sub r3,r3,r4"])[1] == "MNEMONIC")
    chk("a differing DISPLACEMENT is rejected",
        permutation(["lwz r3,8(r4)"], ["lwz r3,12(r4)"])[1] == "OPERAND")
    chk("a length difference is rejected",
        permutation(["blr"], ["nop", "blr"])[1] == "LENDIFF")
    chk("a NON-INJECTIVE rename is rejected",
        permutation(["mr r26,r3", "mr r27,r4"],
                    ["mr r28,r3", "mr r28,r4"])[1] == "NONINJ")
    chk("a register renamed INCONSISTENTLY is rejected",
        permutation(["mr r26,r3", "mr r4,r26"],
                    ["mr r28,r3", "mr r4,r29"])[1] == "NONFUNC")
    chk("a reloc NAME at an equal address is weighted 0 (#70)",
        permutation(["lfs f0,0(0)", "RELOC lbl_1"],
                    ["lfs f0,0(0)", "RELOC @7"]) == ({}, "PERM"))
    chk("a scratch-register move classifies as SCRATCH",
        classify({4: 5, 5: 4}, []) == "SCRATCH")
    chk("a saved move that misses every home is LOCALS-ONLY",
        classify({28: 29, 29: 28}, [(3, 30)]) == "LOCALS-ONLY")
    chk("param_homes stops at the first non-home instruction",
        param_homes(["mr r26,r3", "mr r27,r4", "cmplwi r26,0", "mr r28,r5"])
        == [(3, 26), (4, 27)])
    chk("param_homes ignores a RELOAD of an argument register",
        param_homes(["mr r26,r3", "mr r28,r3"]) == [(3, 26)])
    chk("param_homes ignores a saved-to-saved copy",
        param_homes(["mr r26,r27"]) == [])
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--canon", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--all-buckets", action="store_true",
                    help="also print the rejected buckets and their bytes")
    a = ap.parse_args()
    if a.self_test:
        raise SystemExit(self_test())
    if a.canon:
        canon()
        return
    rows = scan()
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
    n = Counter(r["cls"] for r in rows)
    b = Counter()
    for r in rows:
        b[r["cls"]] += r["size"]
    print(f"# {len(rows)} sub-100 rows compared")
    if a.all_buckets:
        for k, v in n.most_common():
            print(f"  {k:12s} {v:4d} rows  {b[k]:7d} B")
        print()
    keep = ("PARAM-HOME", "LOCALS-ONLY", "SCRATCH")
    print("# PURE REGISTER-PERMUTATION CLASS")
    for k in keep:
        print(f"  {k:12s} {n[k]:3d} rows  {b[k]:6d} B")
    print(f"  {'TOTAL':12s} {sum(n[k] for k in keep):3d} rows  "
          f"{sum(b[k] for k in keep):6d} B")
    print()
    for k in keep:
        for r in sorted((x for x in rows if x["cls"] == k), key=lambda x: -x["size"]):
            print(f"  {k:11s} {r['size']:6d} B {r['fuzzy']:8.3f}  "
                  f"{r['unit']}  {r['sym']}")
            if k == "PARAM-HOME":
                print(f"                homes T={r['T']}\n"
                      f"                homes C={r['C']}   sigma={r['map']}")


if __name__ == "__main__":
    main()
