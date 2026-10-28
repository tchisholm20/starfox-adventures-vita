#!/usr/bin/env python3
"""Partition every sub-100 row by WHICH REGISTER CLASS and WHICH BAND its residual permutes.

a71_mnhist_scan.py separates the rows whose opcode MULTISET differs (operation)
from the rest (colouring).  This splits the colouring mass further, on the two
axes that decide whether a source-level ordering can reach a row at all:

  * class  -- does the residual permute GPRs, FPRs, or neither?  #82 is the FPR
              half of the colouring bucket and is much smaller than it looks.
  * band   -- is a permuted register CALLEE-SAVED (>= r14/f14) or volatile?
              Declaration order is the only source key for the saved band
              (docs/priced_classes.md Sec 27), so a row whose whole residual sits
              in the volatile registers holds compiler temporaries and no
              declaration ordering can reach it.

  python3 tools/a86_reg_class_partition.py            # summary + full table
  python3 tools/a86_reg_class_partition.py --saved    # only the reachable rows
"""
from __future__ import annotations
import argparse, difflib, json, re, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"
TOK = re.compile(r"\b([rf])(\d+)\b")
SAVED = 14


def analyse(obj_our, obj_tgt, sym):
    t = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_tgt, sym)), sym)
    c = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_our, sym)), sym)
    if t == c:
        return None
    ht = Counter(s.split(None, 1)[0] for s in t if not s.startswith("RELOC"))
    hc = Counter(s.split(None, 1)[0] for s in c if not s.startswith("RELOC"))
    colouring = ht == hc
    st = {"savedF": 0, "volF": 0, "savedG": 0, "volG": 0, "other": 0}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(
            None, t, c, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        if i2 - i1 != j2 - j1:
            st["other"] += 1
            continue
        for a, b in zip(t[i1:i2], c[j1:j2]):
            if a == b or a.startswith("RELOC") or b.startswith("RELOC"):
                continue
            if a.split(None, 1)[0] != b.split(None, 1)[0]:
                st["other"] += 1
                continue
            ta, tb = TOK.findall(a), TOK.findall(b)
            if len(ta) != len(tb) or TOK.sub("X", a) != TOK.sub("X", b):
                st["other"] += 1
                continue
            for (ka, va), (kb, vb) in zip(ta, tb):
                if ka != kb or va == vb:
                    continue
                iv, ib = int(va), int(vb)
                sv = iv >= SAVED and ib >= SAVED
                st[("saved" if sv else "vol") + ("F" if ka == "f" else "G")] += 1
    return colouring, len(t), len(c), st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--saved", action="store_true",
                    help="print only rows with a saved-band register move")
    a = ap.parse_args()
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    by_src = {u["name"].replace("\\", "/"): u
              for u in load_units(REPO / "build" / VER / "config.json")}
    rows = []
    for u in report["units"]:
        sp = (u.get("metadata") or {}).get("source_path")
        if not sp:
            continue
        cfg = by_src.get(sp[4:] if sp.startswith("src/") else sp)
        if cfg is None:
            continue
        tgt = REPO / cfg["object"]
        our = REPO / cfg["object"].replace("/obj/", "/src/", 1)
        if not tgt.is_file() or not our.is_file():
            continue
        for f in u.get("functions", []):
            fz = f.get("fuzzy_match_percent", -1.0)
            if fz >= 100.0:
                continue
            try:
                r = analyse(our, tgt, f["name"])
            except Exception:
                continue
            if r:
                rows.append((cfg["name"], f["name"], fz) + r)
    rows.sort(key=lambda r: -r[2])
    col = [r for r in rows if r[3]]
    sav = [r for r in col if r[6]["savedF"] or r[6]["savedG"]]
    fpr = [r for r in col if r[6]["savedF"] or r[6]["volF"]]
    print(f"# {len(rows)} sub-100 rows: {len(col)} COLOURING / {len(rows)-len(col)} OPERATION")
    print(f"# colouring rows with any FPR difference (the #82 population): {len(fpr)}")
    print(f"# colouring rows with a SAVED-band move (declaration-order reachable): {len(sav)}")
    print(f"# colouring rows that are VOLATILE-ONLY (temporaries; unreachable): {len(col)-len(sav)}")
    print(f"# colouring rows with a saved-FPR move: "
          f"{sum(1 for r in col if r[6]['savedF'])}")
    for un, sym, fz, colr, lt, lc, st in (sav if a.saved else rows):
        print(f"{fz:9.4f} {'COLOUR' if colr else 'OPER  '} LEN{lc-lt:+d} "
              f"savedF={st['savedF']:4d} volF={st['volF']:4d} "
              f"savedG={st['savedG']:4d} volG={st['volG']:4d} other={st['other']:4d}"
              f"  {un}  {sym}")


if __name__ == "__main__":
    main()
