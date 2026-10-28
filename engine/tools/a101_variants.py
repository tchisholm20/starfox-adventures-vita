#!/usr/bin/env python3
"""Apply a named source variant to a unit, score it, restore, and report.

Every variant is a (anchor, replacement) pair asserted to occur EXACTLY once,
so a silently-missed paste can never be read as an inert result.  The file is
restored from an in-memory copy of the original bytes after every probe and the
restoration is verified byte-wise; a probe that leaves the tree dirty aborts the
run rather than contaminating the next measurement.
"""
from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from a101_probe import score

REPO = Path(__file__).resolve().parent.parent


def run(path, unit, sym, variants, verbose=True):
    p = REPO / path
    orig = p.read_bytes()
    base = score(unit, sym)
    if verbose:
        print(f"BASE {base:.3f}   {unit}  {sym}   ({len(variants)} variants)")
    out = []
    try:
        for name, anchor, repl in variants:
            s = orig.decode("utf-8")
            n = s.count(anchor)
            if n != 1:
                print(f"  SKIP  {name:44s} anchor count={n} (expected 1)")
                out.append((name, None))
                continue
            p.write_bytes(s.replace(anchor, repl).encode("utf-8"))
            v = score(unit, sym)
            p.write_bytes(orig)
            assert p.read_bytes() == orig
            if v is None:
                print(f"  BUILD-FAIL {name}")
                out.append((name, None))
                continue
            tag = ("HIT " if v < base - 1e-9 else
                   "same" if abs(v - base) < 1e-9 else "worse")
            if verbose:
                print(f"  {tag}  {name:44s} {v:8.3f}  ({v - base:+.3f})")
            out.append((name, v))
    finally:
        p.write_bytes(orig)
        assert p.read_bytes() == orig
        score(unit, sym)          # leave the object tree at the original
    return base, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spec", help="python file defining PATH/UNIT/SYM/VARIANTS")
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("vspec", a.spec)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    base, out = run(mod.PATH, mod.UNIT, mod.SYM, mod.VARIANTS)
    hits = [(n, v) for n, v in out if v is not None and v < base - 1e-9]
    print(f"\n{len(out)} probes, {sum(1 for _, v in out if v is None)} skipped/failed, "
          f"{len(hits)} hits")
    for n, v in sorted(hits, key=lambda x: x[1]):
        print(f"  HIT {n}  {base:.3f} -> {v:.3f}")


if __name__ == "__main__":
    main()
