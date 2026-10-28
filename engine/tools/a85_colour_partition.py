#!/usr/bin/env python3
"""Full three-way partition of the sub-100 code frontier, printing EVERY row.

a71_mnhist_scan only prints the OPERATION rows (multiset differs).  The
colouring mass -- the rows this lane has to attack -- is exactly what that
scan drops on the floor, so it was never enumerated.  This prints all three
kinds with the source path, so a decl-order sweep can be driven off it.
"""
from __future__ import annotations

import json
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


def hist(stream):
    c = Counter()
    for s in stream:
        if s.startswith("RELOC"):
            continue
        c[s.split(None, 1)[0]] += 1
    return c


def analyze(args):
    unit_name, src, obj_our, obj_tgt, syms = args
    out = []
    for sym, fz in syms:
        try:
            t = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_tgt, sym)), sym)
            c = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_our, sym)), sym)
        except Exception:
            out.append((unit_name, src, sym, fz, "ERROR", 0, 0))
            continue
        if t == c:
            kind = "IDENTICAL"
        else:
            ht, hc = hist(t), hist(c)
            if ht != hc:
                kind = "operation"
            elif [x.split(None, 1)[0] for x in t] == [x.split(None, 1)[0] for x in c]:
                kind = "colouring"
            else:
                kind = "order"
        out.append((unit_name, src, sym, fz, kind, len(t), len(c)))
    return out


def main():
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    units_cfg = load_units(REPO / "build" / VER / "config.json")
    by_src = {u["name"].replace("\\", "/"): u for u in units_cfg}
    jobs = []
    for u in report["units"]:
        meta = u.get("metadata") or {}
        sp = meta.get("source_path")
        if not sp:
            continue
        cfg = by_src.get(sp[4:] if sp.startswith("src/") else sp)
        if cfg is None:
            continue
        syms = [(f["name"], f.get("fuzzy_match_percent", -1.0))
                for f in u.get("functions", [])
                if f.get("fuzzy_match_percent", -1.0) < 100.0]
        if not syms:
            continue
        obj_tgt = REPO / cfg["object"]
        obj_our = REPO / cfg["object"].replace("/obj/", "/src/", 1)
        if not obj_our.is_file() or not obj_tgt.is_file():
            continue
        jobs.append((cfg["name"], sp, obj_our, obj_tgt, syms))

    res = []
    with ProcessPoolExecutor(max_workers=10) as ex:
        for r in ex.map(analyze, jobs):
            res.extend(r)
    cls = Counter()
    for un, src, sym, fz, kind, lt, lc in sorted(res, key=lambda r: -r[3]):
        cls[kind] += 1
        lenmark = "LEN=" if lt == lc else f"LEN{lc-lt:+d}"
        print(f"{fz:9.5f} {kind:10s} {lenmark:7s} {src}  {sym}")
    print(f"\n# {len(res)} sub-100 rows", file=sys.stderr)
    for k, n in cls.most_common():
        print(f"#  {k:12s} {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
