#!/usr/bin/env python3
"""sizedelta.py -- per-function emitted-size delta census (our .o vs retail .o).

Ranks every .text symbol whose size in our object differs from the retail
target object.  The delta in bytes ("wB") is the surplus/deficit of emitted
instructions and names an instruction-count divergence *before* any diff read.

Usage:
  python3 tools/sizedelta.py                 rank all units
  python3 tools/sizedelta.py --unit <name>   one unit
  python3 tools/sizedelta.py --json          machine readable
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def nm_path() -> Path:
    for cand in ("powerpc-eabi-nm.exe", "powerpc-eabi-nm"):
        p = REPO / "build" / "binutils" / cand
        if p.is_file():
            return p
    raise SystemExit("missing powerpc-eabi-nm")


def text_syms(nm: Path, obj: Path) -> dict[str, int]:
    """symbol -> size, for symbols in a .text section."""
    out: dict[str, int] = {}
    try:
        res = subprocess.run(
            [str(nm), "-S", "--size-sort", str(obj)],
            check=True, capture_output=True, text=True,
        )
    except subprocess.CalledProcessError:
        return out
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        _addr, size, typ, name = parts
        if typ.lower() != "t":
            continue
        out[name] = int(size, 16)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="GSAE01")
    ap.add_argument("--unit")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min", type=int, default=1)
    args = ap.parse_args()

    sys.path.insert(0, str(REPO / "tools"))
    import unitfuzzy

    nm = nm_path()
    cfg = json.load(open(REPO / "build" / args.version / "config.json"))
    rows = []
    for unit in cfg["units"]:
        name = unit["name"]
        if args.unit and args.unit not in name:
            continue
        tgt = REPO / unit["object"]
        cur = REPO / unit["object"].replace(
            f"build/{args.version}/obj/", f"build/{args.version}/src/"
        )
        if not tgt.is_file() or not cur.is_file():
            continue
        t = text_syms(nm, tgt)
        c = text_syms(nm, cur)
        for sym, tsize in t.items():
            csize = c.get(sym)
            if csize is None or csize == tsize:
                continue
            d = csize - tsize
            if abs(d) < args.min:
                continue
            rows.append(
                {"unit": name, "sym": sym, "target": tsize, "cur": csize,
                 "delta": d, "wB": abs(d)}
            )

    # attach weighted unmatched bytes (wB) = size * (1 - fuzzy)
    by_unit: dict[str, list] = {}
    for r in rows:
        by_unit.setdefault(r["unit"], []).append(r)
    for uname, urows in by_unit.items():
        try:
            u = unitfuzzy.measure(
                unitfuzzy.find_unit(args.version, uname), args.version)
        except Exception:
            continue
        fz = {}
        for f in (u.get("functions") or []):
            if "fuzzy_match_percent" not in f or "size" not in f:
                continue
            fz[f["name"]] = (float(f["fuzzy_match_percent"]), int(f["size"]))
        for r in urows:
            if r["sym"] in fz:
                pct, size = fz[r["sym"]]
                r["wB"] = round(size * (100.0 - pct) / 100.0, 1)
                r["fuzzy"] = pct

    rows.sort(key=lambda r: -r["wB"])
    if args.json:
        print(json.dumps(rows, indent=1))
        return
    total = sum(r["wB"] for r in rows)
    print(f"{len(rows)} functions with size delta, {total:.0f} wB total\n")
    print(f"{'wB':>7} {'sizeD':>6} {'tgt':>6} {'cur':>6} {'fuzzy':>8}  "
          f"{'symbol':<44} unit")
    for r in rows:
        print(f"{r['wB']:7.1f} {r['delta']:+6d} {r['target']:6d} {r['cur']:6d} "
              f"{r.get('fuzzy', 0):8.3f}  {r['sym']:<44} {r['unit']}")


if __name__ == "__main__":
    main()
