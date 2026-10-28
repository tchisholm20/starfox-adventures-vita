#!/usr/bin/env python3
"""Cluster xdiff JSON regions per function / per TU, classify whole-function
insertions, and emit a per-TU table.
Usage: summarize.py REGIONS.json [--config GSAE01] [--side A]"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdiff import parse_dol, load_splits, load_symbols, lookup, ISOS

def word_at(secs, addr):
    for key, (a, size, data) in secs.items():
        if a <= addr < a + size:
            return struct.unpack(">I", data[addr-a:addr-a+4])[0]
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json_file")
    ap.add_argument("--config", default="GSAE01")
    ap.add_argument("--side", default="A", choices=["A", "B"])
    args = ap.parse_args()

    d = json.load(open(args.json_file))
    va, vb = d["va"], d["vb"]
    secs_b = parse_dol(os.path.join(ISOS, vb, "sys", "main.dol"))
    splits = load_splits(args.config).get(d["section"], [])
    symbols = load_symbols(args.config).get(d["section"], [])

    # per-function clustering keyed on containing symbol of region start (side)
    funcs = {}   # (unit, symname) -> dict
    for r in d["regions"]:
        s = r["a_start"] if args.side == "A" else r["b_start"]
        e = max(s, (r["a_end"] if args.side == "A" else r["b_end"]) - 4)
        u = lookup(splits, s)
        unit = u[2] if u and s < u[1] else "?(past last unit)"
        sym = lookup(symbols, s)
        symname = sym[2] if sym else "?"
        f = funcs.setdefault((unit, symname), dict(n=0, delta=0, a_lo=None, a_hi=None, kinds=set(), newfn=[]))
        f["n"] += 1
        f["delta"] += r["b_len"] - r["a_len"]
        f["kinds"].add(r["kind"])
        f["a_lo"] = min(f["a_lo"] or s, s)
        f["a_hi"] = max(f["a_hi"] or e, e)
        # whole-function-insert heuristic: pure insert whose B range contains
        # both a stwu r1,-X(r1)/mflr prologue and a blr
        if r["kind"] == "INSERT" and r["b_len"] >= 0x20:
            words = [word_at(secs_b, a) for a in range(r["b_start"], r["b_end"], 4)]
            has_pro = any(w is not None and ((w >> 16) & 0xFFFF) == 0x9421 for w in words)
            has_blr = any(w == 0x4E800020 for w in words)
            if has_pro and has_blr:
                f["newfn"].append((r["b_start"], r["b_len"]))

    # per-TU rollup
    tus = {}
    for (unit, symname), f in sorted(funcs.items(), key=lambda kv: kv[1]["a_lo"]):
        t = tus.setdefault(unit, dict(funcs=[], delta=0, n=0, newfn=[]))
        t["funcs"].append((symname, f))
        t["delta"] += f["delta"]
        t["n"] += f["n"]
        t["newfn"].extend(f["newfn"])

    print(f"## {va} -> {vb} ({d['section']}), mapped on side {args.side} via {args.config}")
    print(f"{'TU':52s} {'sites':>5s} {'delta':>8s}  functions touched")
    for unit, t in sorted(tus.items(), key=lambda kv: min(f[1]['a_lo'] for f in kv[1]['funcs'])):
        fnames = ", ".join(sym + ("[NEWFN]" if f["newfn"] else "") for sym, f in t["funcs"])
        print(f"{unit:52s} {t['n']:5d} {t['delta']:+8x}  {fnames}")
    allnew = [nf for t in tus.values() for nf in t["newfn"]]
    if allnew:
        print("\nwhole-function insertions (B addr, size):")
        for a, l in allnew:
            print(f"  {a:08X} +{l:#x}")

if __name__ == "__main__":
    main()
