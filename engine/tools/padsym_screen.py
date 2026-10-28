#!/usr/bin/env python3
"""Alignment-pad phantom-symbol screen.

A symbols.txt symbol that is zero-valued in the retail image, has NO reader in
any decoded access, and sits at an alignment position inside a claimed data
range is almost certainly mwld alignment padding rather than a real object.
dtk carves it as a real symbol in the retail .o; our object cannot reproduce
it, so objdiff scores the entire containing section 0.

Reports every such candidate, with the unit that claims its address range and
that section's current fuzzy score.
"""

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reader_census as rc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPLITS = os.path.join(ROOT, "config", "GSAE01", "splits.txt")
REPORT = os.path.join(ROOT, "build", "GSAE01", "report.json")
ELF = os.path.join(ROOT, "build", "GSAE01", "main.elf")
OBJDUMP = os.path.join(ROOT, "build", "binutils", "powerpc-eabi-objdump")

DATA_SECTIONS = (".sdata2", ".sdata", ".data", ".rodata", ".sbss", ".bss",
                 ".sbss2", "extab", "extabindex")


def load_image():
    """Return {section_name: (vma, bytes)} for every allocated ELF section."""
    hdr = subprocess.run([OBJDUMP, "-h", ELF], capture_output=True,
                         text=True).stdout
    lines = hdr.splitlines()
    out = {}
    for i, line in enumerate(lines):
        m = re.match(
            r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
            line)
        if not m:
            continue
        flags = lines[i + 1] if i + 1 < len(lines) else ""
        name = m.group(1)
        size, vma, off = int(m.group(2), 16), int(m.group(3), 16), int(m.group(5), 16)
        if "ALLOC" not in flags:
            continue
        if "LOAD" in flags:
            blob = open(ELF, "rb").read()[off:off + size]
        else:
            blob = b"\x00" * size
        out[name] = (vma, blob)
    return out


def parse_splits():
    """Return list of (unit, section, start, end)."""
    out = []
    unit = None
    for line in open(SPLITS, encoding="utf-8", errors="replace"):
        s = line.rstrip("\n")
        if not s.strip():
            continue
        if not s.startswith((" ", "\t")):
            if s.endswith(":"):
                unit = s[:-1]
            continue
        m = re.match(r"\s*(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)", s)
        if m and unit:
            out.append((unit, m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return out


def report_scores():
    """Return {(unit_basename, section): fuzzy}."""
    if not os.path.exists(REPORT):
        return {}
    r = json.load(open(REPORT))
    out = {}
    for u in r["units"]:
        for s in u.get("sections", []):
            out[(u["name"], s["name"])] = s.get("fuzzy_match_percent")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--section", default=None)
    ap.add_argument("--all", action="store_true",
                    help="list every zero-valued unread symbol, not just aligned ones")
    args = ap.parse_args()

    syms = rc.parse_symbols()
    sda, sda2 = rc.sda_bases()
    hits = rc.scan(sda, sda2)
    image = load_image()
    splits = parse_splits()
    scores = report_scores()

    # map addr -> owning claim
    def owner(addr, section):
        for unit, sec, a, b in splits:
            if sec == section and a <= addr < b:
                return unit
        return None

    def score_for(unit, section):
        if unit is None:
            return None
        base = unit
        if base.endswith(".c"):
            base = base[:-2]
        for key in ("main/" + base, base):
            if (key, section) in scores:
                return scores[(key, section)]
        return None

    def value_at(addr, size, section):
        for nm, (vma, blob) in image.items():
            if nm == section and vma <= addr and addr + size <= vma + len(blob):
                return blob[addr - vma:addr - vma + size]
        return None

    def claim(addr, section):
        for unit, sec, a, b in splits:
            if sec == section and a <= addr < b:
                return unit, a, b
        return None, None, None

    rows = []
    for i, s in enumerate(syms):
        if s["section"] not in DATA_SECTIONS:
            continue
        if args.section and s["section"] != args.section:
            continue
        size = s["size"]
        if size == 0:
            continue
        readers = set()
        for a in range(s["addr"], s["addr"] + size):
            if a in hits:
                readers |= hits[a]
        if readers:
            continue
        val = value_at(s["addr"], size, s["section"])
        if val is None or any(val):
            continue
        u, cs, ce = claim(s["addr"], s["section"])
        if u is None:
            pos = "UNCLAIMED"
        elif s["addr"] + size == ce:
            pos = "TAIL"
        elif s["addr"] == cs:
            pos = "HEAD"
        else:
            pos = "INTERIOR"
        sc = score_for(u, s["section"])
        if not args.all and pos != "TAIL":
            continue
        if not args.all and sc is not None and sc >= 100.0:
            continue
        rows.append((s, pos, u, sc))

    rows.sort(key=lambda r: (r[3] is None, r[3] if r[3] is not None else 999,
                             r[0]["addr"]))
    print(f"# {len(rows)} candidates")
    print(f"{'symbol':30s} {'sect':9s} {'addr':10s} {'sz':4s} {'pos':10s} "
          f"{'score':>8s}  owner")
    for s, pos, u, sc in rows:
        scs = "       -" if sc is None else f"{sc:8.3f}"
        print(f"{s['name']:30s} {s['section']:9s} 0x{s['addr']:08X} "
              f"{s['size']:4d} {pos:10s} {scs}  {u}")


if __name__ == "__main__":
    main()
