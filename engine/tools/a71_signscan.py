#!/usr/bin/env python3
"""Sweep every unit for a literal whose SIGN lives in the pool word on one side only.

A66 measured that retail's trig stores each Horner coefficient already negated
and adds it, so `A + -C` is the faithful spelling and `A - C` mints a word retail
never had.  If that habit is anywhere else in the tree it shows as an exact
mirror pair between the two pools: retail holds -v where we hold +v (or the
other way round) inside the SAME unit's data sections.  This reports every such
pair, plus the values each side holds alone, so a real sign row is separable
from an ordinary missing/extra constant.
"""
from __future__ import annotations

import json
import struct
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"
sys.path.insert(0, str(REPO / "tools"))
from score_delta_gate import elf_progbits  # noqa: E402
from function_objdump import load_units  # noqa: E402


def floats(path):
    """f32 and f64 values held in a unit's pool sections."""
    c = Counter()
    try:
        secs = elf_progbits(path)
    except Exception:
        return c
    for name, data in secs.items():
        if "sdata2" not in name and "rodata" not in name and not name.endswith(".sdata"):
            continue
        for off in range(0, len(data) - 3, 4):
            v, = struct.unpack_from(">f", data, off)
            if v == v and abs(v) != float("inf") and v != 0.0:
                c[("f32", round(v, 9))] += 1
        for off in range(0, len(data) - 7, 8):
            if off % 8:
                continue
            v, = struct.unpack_from(">d", data, off)
            if v == v and abs(v) != float("inf") and v != 0.0:
                c[("f64", round(v, 15))] += 1
    return c


def main():
    units_cfg = load_units(REPO / "build" / VER / "config.json")
    nsign = 0
    for cfg in units_cfg:
        tgt = REPO / cfg["object"]
        our = REPO / cfg["object"].replace("/obj/", "/src/", 1)
        if not tgt.is_file() or not our.is_file():
            continue
        ft, fo = floats(tgt), floats(our)
        if ft == fo:
            continue
        onlyT, onlyC = ft - fo, fo - ft
        pairs = []
        for (k, v), n in sorted(onlyT.items()):
            if (k, round(-v, 15 if k == "f64" else 9)) in onlyC:
                pairs.append((k, v))
        if not pairs:
            continue
        nsign += 1
        print(f"{cfg['name']}")
        for k, v in pairs:
            print(f"    SIGN-PAIR {k}  retail {v!r}   ours {-v!r}")
    print(f"\n# {nsign} units hold a value at OPPOSITE SIGN from retail's pool",
          file=sys.stderr)


if __name__ == "__main__":
    main()
