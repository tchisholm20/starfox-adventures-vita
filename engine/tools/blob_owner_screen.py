#!/usr/bin/env python3
"""For every unclaimed `auto_*` data blob, report which of OUR objects already
DEFINES each symbol in the range, so a splits.txt claim converts directly into
`matched_data`.

A blob range is only claimable into unit U when every symbol in it is defined by
U's object, the run is contiguous, and U's emitted section covers it.
"""
import json, os, re, subprocess, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OD = os.path.join(ROOT, "build/binutils/powerpc-eabi-objdump")


def symbols():
    out = {}
    txt = open(os.path.join(ROOT, "config/GSAE01/symbols.txt")).read()
    for m in re.finditer(r'^([A-Za-z_]\w*)\s*=\s*(\.\w+):0x([0-9A-Fa-f]+);(.*)$', txt, re.M):
        s = re.search(r'size:0x([0-9A-Fa-f]+)', m.group(4))
        out[m.group(1)] = (m.group(2), int(m.group(3), 16),
                           int(s.group(1), 16) if s else 4)
    return out


def main():
    sym = symbols()
    rep = json.load(open(os.path.join(ROOT, 'build/GSAE01/report.json')))

    ranges = []
    for u in rep['units']:
        if not u['metadata'].get('auto_generated'):
            continue
        s0 = u['sections'][0]
        if s0['name'] == '.sdata2':
            continue
        base = int(re.search(r'_([0-9A-Fa-f]{8})_', u['name']).group(1), 16)
        ranges.append((base, base + int(s0['size']), s0['name'], u['name']))
    ranges.sort()

    defined = {}
    for u in rep['units']:
        if u['metadata'].get('auto_generated'):
            continue
        rel = u['name'].split('/', 1)[1]
        o = os.path.join(ROOT, 'build/GSAE01/src', rel + '.o')
        if not os.path.exists(o):
            continue
        out = subprocess.run([OD, '-t', o], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r'^[0-9a-f]{8}\s+(\S+)\s+(\S+)\s+(\S+)\s+([0-9a-f]{8})\s+(\S+)$', line)
            if not m:
                continue
            sec, name = m.group(3), m.group(5)
            if sec.startswith('*'):
                continue
            defined.setdefault(name, []).append((rel, sec, int(m.group(4), 16)))

    for lo, hi, sec, name in ranges:
        print(f"=== {name}  {sec} 0x{lo:08X}-0x{hi:08X} ({hi-lo} B)")
        ss = sorted((a, n, sz) for n, (s, a, sz) in sym.items() if s == sec and lo <= a < hi)
        for a, n, sz in ss:
            d = defined.get(n, [])
            tag = ' '.join(f'{u}[{s}:{z}]' for u, s, z in d) if d else 'UNDEFINED-in-src'
            print(f"   0x{a:08X} +{sz:<5d} {n:<40s} {tag}")
        if not ss:
            print("   (no symbols)")
        print()


if __name__ == '__main__':
    main()
