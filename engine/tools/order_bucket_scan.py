#!/usr/bin/env python3
"""Re-derive A72's ORDER bucket at the current tip, and name the slid instruction.

A71/§14 partitions the sub-100 code frontier three ways from the two instruction
streams alone:

    opcode SEQUENCE identical, registers differ  -> COLOURING (#108/#82)
    opcode MULTISET identical, sequence differs  -> ORDER
    multiset differs                             -> OPERATION (source text)

A72/§15 opened all ten order rows and closed the bucket with a test: *an order
row is worth opening only when the SLID INSTRUCTION is one the source text
names.*  A76's renderSunAndMoon is the counter-example that motivates rerunning
it -- a forward-substituted temp slides an `fnmsubs` to the store site, which is
exactly an arithmetic instruction the source names.

So this prints the bucket AND classifies each slide by whether the slid opcode
is arithmetic (source-nameable: f*/add/sub/mul/rlwinm on a value) or plumbing
(mr/li/lis/lfd-hoist/parameter home), which is what A72 found in nine of ten.

  python3 tools/order_bucket_scan.py
"""
from __future__ import annotations

import difflib
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
if not OBJDUMP.is_file():
    OBJDUMP = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"

# Opcodes the SOURCE TEXT can place: they compute a value the C names or orders.
ARITH = {
    "fmuls", "fmul", "fadds", "fadd", "fsubs", "fsub", "fdivs", "fdiv",
    "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fmadd", "fmsub", "fnmadd",
    "fnmsub", "fneg", "fabs", "frsp", "fctiwz", "fres", "frsqrte",
    "add", "addi", "addis", "subf", "subfic", "neg", "mullw", "mulli",
    "divw", "divwu", "slwi", "srwi", "srawi", "rlwinm", "and", "or", "xor",
    "andi.", "ori", "xori", "extsb", "extsh", "clrlwi",
}


def mn(s):
    return s.split(None, 1)[0]


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
        mt = [mn(x) for x in t if not x.startswith("RELOC")]
        mc = [mn(x) for x in c if not x.startswith("RELOC")]
        if Counter(mt) != Counter(mc):
            continue                      # OPERATION row
        if mt == mc:
            continue                      # COLOURING row
        # ORDER row: name the slid opcodes and count how many slots moved.
        sm = difflib.SequenceMatcher(a=mt, b=mc, autojunk=False)
        slid, nslid = [], 0
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            nslid += max(i2 - i1, j2 - j1)
            slid += mt[i1:i2] + mc[j1:j2]
        out.append((unit_name, sym, round(fz, 3), len(mt), nslid,
                    sorted(set(slid))))
    return out


def main():
    report = json.load(open(REPO / f"build/{VER}/report.json"))
    units_cfg = load_units(REPO / "build" / VER / "config.json")
    by_src = {u["name"].replace("\\", "/"): u for u in units_cfg}
    jobs = []
    for u in report["units"]:
        sp = (u.get("metadata") or {}).get("source_path")
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
    print("%-9s %-38s %-32s %-6s %-5s %s" %
          ("SCORE", "UNIT", "FUNCTION", "INSTRS", "SLID", "SLID OPCODES"))
    nameable = 0
    for un, sym, fz, n, nslid, slid in sorted(res, key=lambda r: -r[2]):
        hit = [s for s in slid if s in ARITH]
        if hit:
            nameable += 1
        print("%-9.3f %-38s %-32s %-6d %-5d %s%s" %
              (fz, un[:38], sym[:32], n, nslid, ",".join(slid),
               "   <== SOURCE-NAMEABLE" if hit else ""))
    print("\n%d order rows; %d slide an opcode the source text names" %
          (len(res), nameable))


if __name__ == "__main__":
    main()
