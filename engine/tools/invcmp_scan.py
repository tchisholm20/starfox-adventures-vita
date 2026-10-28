#!/usr/bin/env python3
"""Find retail compares whose LEFT operand is DEFINED LATER than the right one.

MWCC evaluates a comparison strictly left-to-right and lists the first-evaluated
operand first, so the natural emission has the left operand defined FIRST.  A
compare where retail defines the LEFT operand LAST is the "inverted" shape that
only a call (or a `static inline` expansion that kept the call's evaluation
order) on the right-hand side produces.  This scans every sub-100 function's
target stream for that shape and reports whether our stream reproduces it.
"""
from __future__ import annotations

import json
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, objdump_symbol, strip_preamble
from ndiff import normalize, regions

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"

CMP_RE = re.compile(r"^(cmpw|cmplw|fcmpo|fcmpu)\s+(?:cr\d,\s*)?([rf]\d+),\s*([rf]\d+)$")
DEF_RE = re.compile(r"^([a-z][a-z0-9._+-]*)\s+([rf]\d+)\s*,")
NO_DEF = {"stw", "sth", "stb", "stfs", "stfd", "stwu", "stwx", "sthx", "stbx",
          "stfsx", "stfdx", "cmpw", "cmplw", "cmpwi", "cmplwi", "fcmpo", "fcmpu",
          "mtctr", "mtlr", "mtspr", "stmw"}


def defs_of(stream, i, reg):
    """index of the last instruction before i that writes reg, else -1."""
    for j in range(i - 1, -1, -1):
        s = stream[j]
        if s.startswith("RELOC"):
            continue
        if s.startswith("bl ") or s == "blr":
            return j if reg in ("r3", "f1") else -2
        m = DEF_RE.match(s)
        if m and m.group(2) == reg and m.group(1) not in NO_DEF:
            return j
    return -1


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
        # region coverage on the target side
        diffspan = [(i1, i2) for tag, i1, i2, j1, j2 in regions(t, c)]

        def inv_list(stream):
            res = []
            for i, s in enumerate(stream):
                m = CMP_RE.match(s)
                if not m:
                    continue
                da = defs_of(stream, i, m.group(2))
                db = defs_of(stream, i, m.group(3))
                if da < 0 or db < 0:
                    res.append((i, s, None, None, None))
                else:
                    res.append((i, s, da > db, stream[da], stream[db]))
            return res

        ti, ci = inv_list(t), inv_list(c)
        paired = len(ti) == len(ci)
        hits = []
        for k, (i, s, invT, dA, dB) in enumerate(ti):
            if not invT:
                continue
            if dA is None:
                continue
            near = any(i1 - 4 <= i <= i2 + 4 for i1, i2 in diffspan)
            if not near:
                continue
            if paired:
                invC = ci[k][2]
                if invC:
                    continue  # we already reproduce the inverted order
                oursd = f"ours={ci[k][1]} L={ci[k][3]} R={ci[k][4]}"
            else:
                oursd = "ours=<unpaired>"
            hits.append((i, s, dA, dB, oursd))
        if hits:
            out.append((unit_name, sym, round(fz, 3), hits))
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
        syms = [(f["name"], f.get("fuzzy_match_percent", 0.0))
                for f in u.get("functions", [])
                if f.get("fuzzy_match_percent", 0.0) < 100.0]
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
    nnear = 0
    for un, sym, fz, hits in sorted(res):
        nnear += 1
        print(f"{fz:8.3f}  {un}  {sym}")
        for i, s, dA, dB, oursd in hits:
            print(f"          @{i:4d} {s:26s} LEFTdef={dA:24s} RIGHTdef={dB:24s} {oursd}")
    print(f"\n# {nnear} functions with an UNREPRODUCED inverted compare in a diff region", file=sys.stderr)


if __name__ == "__main__":
    main()
