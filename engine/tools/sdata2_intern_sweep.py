#!/usr/bin/env python3
"""Tree-wide .sdata2 interning sweep.

MWCC interns a float/double constant once per translation unit.  The linker
concatenates each TU's .sdata2 block in link order, so a block belongs to
exactly one TU.  If a .sdata2 word carved to unit A is referenced from unit B's
.text, then A and B did not each emit their own copy -- they are one TU.

Reads the carved retail objects (build/GSAE01/obj) for their relocations and
config/GSAE01/splits.txt for the per-unit .sdata2 carve.
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OBJDUMP = ROOT / "build/binutils/powerpc-eabi-objdump"
OBJ = ROOT / "build/GSAE01/obj"
SPLITS = ROOT / "config/GSAE01/splits.txt"
SYMBOLS = ROOT / "config/GSAE01/symbols.txt"


def parse_splits():
    """unit -> {section: (start, end)}, preserving file order."""
    units = {}
    order = []
    cur = None
    for line in SPLITS.read_text().splitlines():
        if not line.strip():
            continue
        if not line.startswith((" ", "\t")):
            if line.rstrip().endswith(":"):
                cur = line.rstrip()[:-1]
                units[cur] = {}
                order.append(cur)
            else:
                cur = None
            continue
        if cur is None:
            continue
        m = re.match(r"\s+(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)", line)
        if m:
            units[cur][m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return units, order


def parse_symbols():
    """symbol name -> address, for GLOBAL .sdata2 objects only.

    `@N` compiler-minted names are section-local and renumbered per TU, so the
    same name means a different object in every unit -- they can never identify
    a cross-TU reference and are dropped.  Any other name that is not unique
    tree-wide is dropped for the same reason.
    """
    out = {}
    dropped = set()
    for line in SYMBOLS.read_text().splitlines():
        m = re.match(r"\s*(\S+)\s*=\s*\.?\w*:?0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        name, addr = m.group(1), int(m.group(2), 16)
        if name.startswith("@") or "scope:local" in line:
            dropped.add(name)
            continue
        if name in out and out[name] != addr:
            dropped.add(name)
            continue
        out[name] = addr
    for n in dropped:
        out.pop(n, None)
    return out


def main():
    units, order = parse_splits()
    syms = parse_symbols()

    # Every .sdata2 range in the carve.
    ranges = []
    for u, secs in units.items():
        if ".sdata2" in secs:
            ranges.append((secs[".sdata2"][0], secs[".sdata2"][1], u))
    ranges.sort()
    lo = min(r[0] for r in ranges)
    hi = max(r[1] for r in ranges)

    def owner(addr):
        for s, e, u in ranges:
            if s <= addr < e:
                return u
        return None

    # Map a carved object file back to its unit name.
    obj_to_unit = {}
    for u in units:
        stem = Path(u).stem
        obj_to_unit.setdefault(stem, []).append(u)

    objs = sorted(OBJ.rglob("*.o"))
    print(f"scanning {len(objs)} carved objects; "
          f"{len(ranges)} .sdata2 blocks in [0x{lo:08X},0x{hi:08X})",
          file=sys.stderr)

    # addr -> set of referencing object stems
    refs = defaultdict(set)
    for o in objs:
        try:
            out = subprocess.run([str(OBJDUMP), "-r", str(o)],
                                 capture_output=True, text=True, timeout=120).stdout
        except Exception:
            continue
        # only relocations that live in .text
        section = None
        for line in out.splitlines():
            m = re.match(r"RELOCATION RECORDS FOR \[([^\]]+)\]", line)
            if m:
                section = m.group(1)
                continue
            if section != ".text":
                continue
            m = re.match(r"([0-9a-f]{8})\s+(\S+)\s+(\S+?)(?:[-+]0x[0-9a-f]+)?$", line.strip())
            if not m:
                continue
            name = m.group(3)
            addr = None
            lm = re.fullmatch(r"lbl_([0-9A-Fa-f]{8})", name)
            if lm:
                addr = int(lm.group(1), 16)
            elif name in syms:
                addr = syms[name]
            if addr is None or not (lo <= addr < hi):
                continue
            refs[addr].add(o.stem)

    # Which units does each stem correspond to?  Report conflicts.
    shared = {a: s for a, s in refs.items() if len(s) > 1}
    print(f"\n{len(refs)} distinct .sdata2 addresses referenced from .text; "
          f"{len(shared)} referenced by more than one object\n")

    pairs = defaultdict(set)
    for a, stems in sorted(shared.items()):
        own = owner(a)
        print(f"0x{a:08X}  carved to {own!s:45s} read from: {sorted(stems)}")
        key = tuple(sorted(stems))
        pairs[key].add(a)

    print("\n=== candidate ONE-TU groups (by shared interned constants) ===")
    for key, addrs in sorted(pairs.items(), key=lambda kv: -len(kv[1])):
        print(f"{len(addrs):3d} shared word(s): {list(key)}")

    # Also: a unit whose .text references .sdata2 but which owns no .sdata2
    # block at all -- it must be sharing someone's TU pool.
    print("\n=== objects that read .sdata2 but own no .sdata2 carve ===")
    own_stems = {Path(u).stem for u, s in units.items() if ".sdata2" in s}
    reader_stems = defaultdict(set)
    for a, stems in refs.items():
        for s in stems:
            reader_stems[s].add(owner(a))
    for s in sorted(reader_stems):
        if s not in own_stems:
            tgt = sorted(x for x in reader_stems[s] if x)
            print(f"{s:35s} reads blocks of: {tgt}")


if __name__ == "__main__":
    main()
