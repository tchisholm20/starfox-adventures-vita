"""Interning-law adjudicator for a suspected TU boundary.

MWCC interns every FP literal and each int->float bias exactly ONCE per
translation unit.  Two consequences, both usable as DOL evidence:

  SHARED     two functions that both reference the SAME .sdata2 word must have
             been compiled into the same object, because a second object would
             have had to mint its own copy.  This half holds.
  DUPLICATE  two .sdata2 words with the same value inside one address span were
             expected to prove the span was produced by at least two objects.
             MEASURED: FALSE.  Of the 635 units whose .sdata2 our single-.c
             compile already reproduces byte-for-byte, 15 have a retail pool
             carrying a value with two or more DIRECTLY REFERENCED copies
             (195_Player mints 60.0f and 35.0f twice each, 209_TumbleWeedB three
             values).  One TU can therefore mint the same atom more than once --
             an inline expansion mints at its expansion site -- so a duplicate is
             reported below as an observation, never as proof of a split.

The block-separability test is the useful companion: two objects produce two
CONTIGUOUS pool runs, so interleaved group atoms also prove one TU.

The oracle is the retail DOL itself: decode every r2-relative (SDA2) access in
the retail .text, attribute it to the retail function that contains it via
config/GSAE01/symbols.txt, and read the referenced word out of the DOL.

Usage:
  python3 tools/interning_adjudicate.py <unit> [<unit> ...]
  python3 tools/interning_adjudicate.py <unit> --groups fnA,fnB  fnC,fnD
        adjudicate a proposed split between two function groups
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOL = REPO / "orig/GSAE01/sys/main.dol"
SPLITS = REPO / "config/GSAE01/splits.txt"
SYMBOLS = REPO / "config/GSAE01/symbols.txt"
SDA2_BASE = 0x803E6500
DFORM = set(list(range(32, 56)) + [14])


def load_dol():
    d = DOL.read_bytes()
    off = struct.unpack_from(">18I", d, 0)
    adr = struct.unpack_from(">18I", d, 0x48)
    siz = struct.unpack_from(">18I", d, 0x90)
    return d, off, adr, siz


def read_word(d, off, adr, siz, addr):
    for i in range(18):
        if siz[i] and adr[i] <= addr < adr[i] + siz[i]:
            return struct.unpack_from(">I", d, off[i] + addr - adr[i])[0]
    return None


def load_splits():
    units: dict[str, dict[str, list]] = {}
    cur = None
    for line in SPLITS.read_text(errors="replace").splitlines():
        if line.strip() and not line[0].isspace() and line.rstrip().endswith(":"):
            cur = line.strip()[:-1]
            units.setdefault(cur, collections.defaultdict(list))
            continue
        m = re.match(r"\s+(\.?\w+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)", line)
        if m and cur:
            units[cur][m.group(1)].append((int(m.group(2), 16), int(m.group(3), 16)))
    return units


def load_functions():
    fns = []
    rx = re.compile(r"^(\S+)\s*=\s*\.text:0x([0-9A-Fa-f]+);.*?size:0x([0-9A-Fa-f]+)")
    for line in SYMBOLS.read_text(errors="replace").splitlines():
        m = rx.match(line)
        if m and "type:function" in line:
            a = int(m.group(2), 16)
            fns.append((a, a + int(m.group(3), 16), m.group(1)))
    fns.sort()
    return fns


def fn_at(fns, a):
    lo, hi = 0, len(fns)
    while lo < hi:
        m = (lo + hi) // 2
        if fns[m][0] <= a:
            lo = m + 1
        else:
            hi = m
    return fns[lo - 1][2] if lo and fns[lo - 1][1] > a else None


def build_refs():
    d, off, adr, siz = load_dol()
    fns = load_functions()
    fn2addr = collections.defaultdict(set)
    addr2fn = collections.defaultdict(set)
    n = 0
    for i in (0, 1):
        if not siz[i]:
            continue
        for k in range(0, siz[i], 4):
            w = struct.unpack_from(">I", d, off[i] + k)[0]
            if (w >> 26) not in DFORM or ((w >> 16) & 31) != 2:
                continue
            disp = w & 0xFFFF
            disp -= 0x10000 if disp & 0x8000 else 0
            target = SDA2_BASE + disp
            f = fn_at(fns, adr[i] + k)
            if f is None:
                continue
            fn2addr[f].add(target)
            addr2fn[target].add(f)
            n += 1
    assert n > 15000, f"SDA21 sweep found only {n} refs"
    return (d, off, adr, siz), fns, fn2addr, addr2fn


BIAS = {0x43300000: "BIAS_hi", 0x80000000: "BIAS_S_lo", 0x00000000: "BIAS_U_lo/zero"}


def fmt_word(w):
    f = struct.unpack(">f", struct.pack(">I", w))[0]
    tag = BIAS.get(w, "")
    return f"0x{w:08X} {f:>16.8g} {tag}"


def report(unit_key, splits, dolinfo, fns, fn2addr, addr2fn, groups=None):
    d, off, adr, siz = dolinfo
    info = splits.get(unit_key)
    if info is None:
        print(f"!! no splits entry for {unit_key}")
        return
    text = info.get(".text", [])
    pool = info.get(".sdata2", [])
    print(f"=== {unit_key}")
    print(f"  .text   {' '.join(f'0x{a:08X}..0x{b:08X}' for a, b in text)}")
    print(f"  .sdata2 {' '.join(f'0x{a:08X}..0x{b:08X}' for a, b in pool) or '(unclaimed)'}")

    own = [f for a, b, f in fns if any(s <= a < e for s, e in text)]
    print(f"  functions: {len(own)}")

    # every .sdata2 address this unit's retail code touches
    touched = sorted({x for f in own for x in fn2addr.get(f, ())})
    if not touched:
        print("  no SDA2 references")
        return
    lo, hi = min(touched), max(touched) + 4
    print(f"  touched span 0x{lo:08X}..0x{hi:08X} ({len(touched)} distinct words)")

    # dump the whole physical run (claim if any, else touched span) word by word,
    # promoting a 0x43300000-headed 8-byte pair to a single bias atom
    dlo = min([lo] + [a for a, _ in pool])
    dhi = max([hi] + [b for _, b in pool])
    atoms: dict[tuple, list] = collections.defaultdict(list)
    a = dlo
    while a < dhi:
        w = read_word(d, off, adr, siz, a)
        if w is None:
            a += 4
            continue
        w2 = read_word(d, off, adr, siz, a + 4)
        if w == 0x43300000 and a % 8 == 0 and w2 is not None:
            key = ("f64", w, w2)
            label = f"0x{w:08X}{w2:08X}  {'BIAS_S' if w2 == 0x80000000 else 'BIAS_U' if w2 == 0 else 'f64'}"
            step = 8
        else:
            key = ("f32", w)
            label = fmt_word(w)
            step = 4
        users = sorted(addr2fn.get(a, ()))
        # an unreferenced zero word is an alignment pad, not an interned atom
        if not (w == 0 and step == 4 and not users):
            atoms[key].append(a)
        mine = [u for u in users if u in own]
        foreign = [u for u in users if u not in own]
        mark = " " if (users or a in touched) else "."
        print(f"   {mark}0x{a:08X}  {label:52s} used-by={','.join(mine)}"
              + (f"   FOREIGN={','.join(foreign)}" if foreign else ""))
        a += step

    inclaim = (lambda x: any(s <= x < e for s, e in pool)) if pool else (lambda x: True)
    dups = {k: v for k, v in atoms.items()
            if len([x for x in v if inclaim(x)]) > 1 and any(x in touched for x in v)}
    if dups:
        print("  duplicate atoms in this span (observation only, NOT proof of two objects):")
        for k, v in dups.items():
            print(f"    {k} at " + ", ".join(f"0x{x:08X}" for x in v))
    else:
        print("  no duplicate atoms in the span")

    if groups:
        print("  group adjudication:")
        sets = []
        for g in groups:
            s = set()
            for f in g:
                s |= fn2addr.get(f, set())
            sets.append(s)
            print(f"    [{','.join(g)}] -> " + (", ".join(f"0x{x:08X}" for x in sorted(s)) or "(none)"))
        for i in range(len(sets)):
            for j in range(i + 1, len(sets)):
                sh = sets[i] & sets[j]
                if sh:
                    print(f"    SHARED between group{i} and group{j}: "
                          + ", ".join(f"0x{x:08X}" for x in sorted(sh))
                          + "  => ONE TU (interning law)")
                else:
                    print(f"    no shared atom between group{i} and group{j}")
                # block-separability: two objects produce two CONTIGUOUS pool
                # runs, so interleaved group atoms also prove one TU
                seq = [(x, k) for k, s in ((i, sets[i]), (j, sets[j])) for x in s]
                seq.sort()
                runs = [k for _, k in seq]
                nblocks = 1 + sum(1 for a, b in zip(runs, runs[1:]) if a != b)
                if nblocks > 2:
                    print(f"    atoms INTERLEAVE ({nblocks} alternating blocks) "
                          "=> ONE TU (two objects would give two contiguous runs)")
                elif nblocks == 2:
                    print("    atoms are block-separable => a split is geometrically possible")
                else:
                    print("    one group has no atoms => interning law is silent")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("units", nargs="+")
    ap.add_argument("--groups", nargs="*", default=None)
    args = ap.parse_args()
    splits = load_splits()
    dolinfo, fns, fn2addr, addr2fn = build_refs()
    groups = [g.split(",") for g in args.groups] if args.groups else None
    for u in args.units:
        key = u
        if key not in splits:
            cands = [k for k in splits if k.endswith(u) or Path(k).stem == Path(u).stem]
            if len(cands) == 1:
                key = cands[0]
        report(key, splits, dolinfo, fns, fn2addr, addr2fn, groups)
        print()


if __name__ == "__main__":
    main()
