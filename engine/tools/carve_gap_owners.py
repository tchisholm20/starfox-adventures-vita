#!/usr/bin/env python3
"""Name the owner of every unattributed carve gap.

dtk turns each stretch of an allocated section that splits.txt leaves unclaimed
into a synthetic `auto_<n>_<ADDR>_<section>` unit. Those synthetic units are the
real carve-artifact population: each one is a run of bytes that some translation
unit genuinely emitted, and that our splits.txt failed to give back to it.

Two sweeps are structurally blind to them. The `.text` contiguity lens never
sees a gap because a gap has no code, and the report's own per-unit scores never
flag one because a synthetic unit is compared against itself and always reads
100.0. What does see them is the reader census below.

An anonymous `.sdata2` literal is referenced only by the code of the TU that
emitted it, so for a `.sdata2` gap the set of retail objects whose `.text`
relocations land inside the gap names its owner outright. A single reader is a
verdict; several readers means the gap holds a named cross-TU object and the
reader set decides nothing. `.data`/`.bss`/`.sdata`/`.sbss` gaps are reported
with the same census, but there the single-reader inference is only a nomination
-- a named global can legitimately live in a TU that never reads it.

Reads the retail objects under build/GSAE01/obj, so it needs a configured tree.

    python3 tools/carve_gap_owners.py [--section .sdata2] [--single-only]
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = "build/GSAE01/report.json"
SYMBOLS = "config/GSAE01/symbols.txt"
OBJROOT = "build/GSAE01/obj"
OBJDUMP = "build/binutils/powerpc-eabi-objdump"

RE_SYM = re.compile(r"^(\S+) = \.(\S+?):0x([0-9A-Fa-f]+);")
RE_RELOC = re.compile(r"R_PPC\S+\s+(\S+?)(?:[+-]0x[0-9a-f]+)?$")


def load_symbol_addresses(base):
    addrs = collections.defaultdict(list)
    with open(os.path.join(base, SYMBOLS)) as fh:
        for line in fh:
            m = RE_SYM.match(line)
            if m:
                addrs[m.group(1)].append((m.group(2), int(m.group(3), 16)))
    return addrs


def load_gaps(base):
    with open(os.path.join(base, REPORT)) as fh:
        report = json.load(fh)
    gaps = []
    for unit in report["units"]:
        if not unit["metadata"].get("auto_generated"):
            continue
        size = int(unit["measures"].get("total_data", 0) or 0)
        for sec in unit.get("sections", []):
            va = int(sec["metadata"]["virtual_address"])
            gaps.append((unit["name"], sec["name"], va, va + size))
    return sorted(gaps, key=lambda g: (g[1], g[2]))


def load_splits(base):
    rows = []
    cur = None
    with open(os.path.join(base, "config/GSAE01/splits.txt")) as fh:
        for line in fh:
            if not line.startswith("\t"):
                m = re.match(r"^(\S+):\s*$", line)
                if m:
                    cur = m.group(1)
                continue
            m = re.match(r"^\t(\.\S+)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
            if m and cur:
                rows.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), cur))
    return rows


def neighbours(splits, section, lo, hi):
    same = sorted([r for r in splits if r[0] == section], key=lambda r: r[1])
    before = [r for r in same if r[2] <= lo]
    after = [r for r in same if r[1] >= hi]
    return (before[-1][3] if before else None), (after[0][3] if after else None)


def reader_census(base, gaps, addrs):
    objs = []
    for root, _dirs, files in os.walk(os.path.join(base, OBJROOT)):
        for f in files:
            if f.endswith(".o"):
                objs.append(os.path.join(root, f))
    objdump = os.path.join(base, OBJDUMP)
    readers = collections.defaultdict(set)
    for obj in sorted(objs):
        out = subprocess.run([objdump, "-r", obj], capture_output=True, text=True).stdout
        seen = set()
        for line in out.split("\n"):
            m = RE_RELOC.search(line.strip())
            if m:
                seen.add(m.group(1))
        rel = os.path.relpath(obj, os.path.join(base, OBJROOT))
        for name in seen:
            for sec, addr in addrs.get(name, []):
                for key, gsec, lo, hi in gaps:
                    if gsec.lstrip(".") == sec and lo <= addr < hi:
                        readers[key].add(rel)
    return readers


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", default=REPO)
    ap.add_argument("--section", help="restrict to one section, e.g. .sdata2")
    ap.add_argument("--single-only", action="store_true",
                    help="print only gaps with exactly one reader")
    args = ap.parse_args()

    base = args.base
    if not os.path.exists(os.path.join(base, REPORT)):
        sys.exit("no %s -- run `ninja build/GSAE01/report.json` first" % REPORT)

    gaps = load_gaps(base)
    if args.section:
        gaps = [g for g in gaps if g[1] == args.section]
    addrs = load_symbol_addresses(base)
    splits = load_splits(base)
    readers = reader_census(base, gaps, addrs)

    counts = collections.Counter()
    for key, sec, lo, hi in gaps:
        rd = sorted(readers.get(key, []))
        counts[("single" if len(rd) == 1 else "shared" if rd else "unread")] += 1
        if args.single_only and len(rd) != 1:
            continue
        prev, nxt = neighbours(splits, sec, lo, hi)
        print("%-30s %-8s %#010x..%#010x %5d B" % (key.split("/")[-1], sec, lo, hi, hi - lo))
        print("    between %s and %s" % (prev or "-", nxt or "-"))
        print("    readers: %s" % (", ".join(rd) if rd else "NONE"))
    print("\n%d gaps: %d single-reader, %d shared, %d unread"
          % (len(gaps), counts["single"], counts["shared"], counts["unread"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
