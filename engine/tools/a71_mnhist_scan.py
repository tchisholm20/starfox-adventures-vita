#!/usr/bin/env python3
"""Per-function MNEMONIC-HISTOGRAM delta over every sub-100 row.

A register permutation, a spill rotation and a frame/displacement diff all leave
the multiset of opcodes alone.  So a function whose opcode histogram differs from
retail's is asking the code generator for a different *operation* somewhere, and
that is decided by the source text, not by the allocator.  This separates the
source-text rows from the #108/#82 colouring mass without needing the operands to
line up.
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize, strip_preamble as _sp  # noqa: F401

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"


def hist(stream):
    c = Counter()
    for s in stream:
        if s.startswith("RELOC"):
            continue
        c[s.split(None, 1)[0]] += 1
    return c


def analyze(args):
    unit_name, obj_our, obj_tgt, syms = args
    out = []
    for sym, fz in syms:
        try:
            t = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_tgt, sym)), sym)
            c = normalize(strip_preamble(objdump_symbol(OBJDUMP, obj_our, sym)), sym)
        except Exception:
            continue
        if t == c:
            continue
        ht, hc = hist(t), hist(c)
        if ht == hc:
            continue
        only_t = ht - hc
        only_c = hc - ht
        out.append((unit_name, sym, round(fz, 3), len(t), len(c),
                    dict(only_t), dict(only_c)))
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
        jobs.append((cfg["name"], obj_our, obj_tgt, syms))

    res = []
    with ProcessPoolExecutor(max_workers=10) as ex:
        for r in ex.map(analyze, jobs):
            res.extend(r)
    cls = Counter()
    for un, sym, fz, lt, lc, ot, oc in sorted(res, key=lambda r: -r[2]):
        same_len = "LEN=" if lt == lc else f"LEN{lc-lt:+d}"
        print(f"{fz:8.3f} {same_len:7s} {un}  {sym}")
        print(f"          T-only {ot}")
        print(f"          C-only {oc}")
        for k in ot:
            cls[("T", k)] += 1
        for k in oc:
            cls[("C", k)] += 1
    print(f"\n# {len(res)} of the sub-100 rows have a DIFFERENT OPCODE MULTISET", file=sys.stderr)
    for (side, k), n in cls.most_common(50):
        print(f"#  {side}-only {k:14s} {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
