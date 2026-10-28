#!/usr/bin/env python3
"""Screen for `.sdata2` pool-claim candidates and prove ownership of each run.

A unit whose splits.txt entry has no `.sdata2` range cannot link as itself: its
constant pool is carved into a neighbouring `auto_*_sdata2` unit, so the object
stays NonMatching even at fuzzy 100.  Claiming the range is only safe when the
run belongs to that unit alone (modellight, shader_dolphin and 279_AppleOnTree
each flipped this way), and only pays when the unit's pool already reproduces --
or can be made to reproduce -- retail's exact emission order.

Oracle: decode every r2-relative access in the RETAIL main.dol text, resolve it
to an absolute .sdata2 address and attribute it to the owning unit through the
splits.txt .text ranges.  For a unit with no .sdata2 claim, the addresses its own
retail code touches form its pool run; a run no OTHER unit references is
EXCLUSIVE and can be claimed, a run with foreign consumers is SHARED and cannot.

Subcommands
    runs [unit ...]     ownership verdict per unclaimed run (default: every unit)
    diff <unit> <lo> <hi>
                        word-by-word retail run against the pool we emit, so the
                        missing constants and the order defects are visible
    multiset <unit> <lo> <hi>
                        which constants retail has that we never mint (the ghost
                        groups to recover) and which we mint that it does not
"""
import collections
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOL = os.path.join(ROOT, 'orig/GSAE01/sys/main.dol')
SPLITS = os.path.join(ROOT, 'config/GSAE01/splits.txt')
REPORT = os.path.join(ROOT, 'build/GSAE01/report.json')
OBJDUMP = os.path.join(ROOT, 'build/binutils/powerpc-eabi-objdump')
OBJCOPY = os.path.join(ROOT, 'build/binutils/powerpc-eabi-objcopy')
SDA2_BASE = 0x803E6500
SDATA2_LO, SDATA2_HI = 0x803DE500, 0x803E8440

dol = open(DOL, 'rb').read()
sec_off = struct.unpack_from('>18I', dol, 0)
sec_adr = struct.unpack_from('>18I', dol, 0x48)
sec_siz = struct.unpack_from('>18I', dol, 0x90)


def dol_offset(addr):
    for i in range(18):
        if sec_siz[i] and sec_adr[i] <= addr < sec_adr[i] + sec_siz[i]:
            return sec_off[i] + (addr - sec_adr[i])
    raise SystemExit('address 0x%08X is not in the DOL' % addr)


def read_splits():
    units, cur = {}, None
    for line in open(SPLITS):
        if line.strip() and not line[0].isspace() and line.rstrip().endswith(':'):
            cur = line.strip()[:-1]
            units.setdefault(cur, collections.defaultdict(list))
            continue
        m = re.match(r'\s+(\.?\w+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)', line)
        if m and cur:
            units[cur][m.group(1)].append((int(m.group(2), 16), int(m.group(3), 16)))
    return units


def interval_owner(intervals, addr):
    lo, hi = 0, len(intervals)
    while lo < hi:
        mid = (lo + hi) // 2
        if intervals[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    return intervals[lo - 1][2] if lo and intervals[lo - 1][1] > addr else None


def sda2_references(text_intervals):
    dform = set(list(range(32, 56)) + [14])
    refs, seen = collections.defaultdict(set), 0
    for i in (0, 1):
        if not sec_siz[i]:
            continue
        for k in range(0, sec_siz[i], 4):
            word = struct.unpack_from('>I', dol, sec_off[i] + k)[0]
            if (word >> 26) not in dform or ((word >> 16) & 31) != 2:
                continue
            disp = word & 0xFFFF
            disp -= 0x10000 if disp & 0x8000 else 0
            addr = SDA2_BASE + disp
            if SDATA2_LO <= addr < SDATA2_HI:
                refs[addr].add(interval_owner(text_intervals, sec_adr[i] + k))
                seen += 1
    assert seen > 15000, 'SDA21 sweep found only %d references' % seen
    return refs


def our_object(unit):
    return os.path.join(ROOT, 'build/GSAE01/src', re.sub(r'\.c(pp)?$', '.o', unit))


def our_pool(unit):
    obj = our_object(unit)
    if not os.path.exists(obj):
        return None
    raw = os.path.join(ROOT, 'build/poolclaim.bin')
    subprocess.run([OBJCOPY, '-O', 'binary', '--only-section=.sdata2', obj, raw], check=True)
    return open(raw, 'rb').read()


def fmt_word(w):
    return '%08X(%g)' % (w, struct.unpack('>f', struct.pack('>I', w))[0])


def cmd_runs(targets):
    units = read_splits()
    text_iv = sorted((s, e, u) for u, x in units.items() for s, e in x.get('.text', []))
    claims = sorted((s, e, u) for u, x in units.items() for s, e in x.get('.sdata2', []))
    refs = sda2_references(text_iv)
    owned = {a: {x for x in s if x} for a, s in refs.items()
             if interval_owner(claims, a) is None}

    fuzzy = {}
    if os.path.exists(REPORT):
        import json
        for u in json.load(open(REPORT))['units']:
            src = (u['metadata'].get('source_path') or '')[4:]
            if src:
                fuzzy[src] = (u['measures'].get('fuzzy_match_percent', 0),
                              bool(u['metadata'].get('complete')))

    if not targets:
        targets = [u for u in units if units[u].get('.text') and not units[u].get('.sdata2')]
    print('%-8s %-46s %-23s %5s %5s %s' % ('fuzzy', 'unit', 'run', 'retl', 'ours', 'verdict'))
    for unit in sorted(targets):
        mine = sorted(a for a in owned if unit in owned[a])
        if not mine:
            continue
        runs, start, prev = [], mine[0], mine[0]
        for addr in mine[1:]:
            if addr - prev > 16:
                runs.append((start, prev))
                start = addr
            prev = addr
        runs.append((start, prev))
        pool = our_pool(unit)
        fz, complete = fuzzy.get(unit, (0, False))
        for lo, hi in runs:
            end = hi + 4
            while end < SDATA2_HI and interval_owner(claims, end) is None and end not in owned:
                end += 4
            foreign = collections.Counter()
            for addr in range(lo, end, 4):
                for other in owned.get(addr, ()):
                    if other != unit:
                        foreign[other] += 1
            verdict = 'EXCLUSIVE' if not foreign else 'SHARED %s' % dict(foreign)
            print('%8.3f %-46s 0x%08X..0x%08X %5d %5s %s%s' %
                  (fz, unit, lo, end, end - lo,
                   'n/a' if pool is None else len(pool), verdict,
                   ' [complete]' if complete else ''))


def cmd_diff(unit, lo, hi):
    ours = our_pool(unit) or b''
    obj = our_object(unit)
    syms = collections.defaultdict(list)
    for line in subprocess.run([OBJDUMP, '-t', obj], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r'([0-9a-f]{8})\s+\S+\s+\S*\s*\.sdata2\s+[0-9a-f]{8}\s+(\S+)', line)
        if m:
            syms[int(m.group(1), 16)].append(m.group(2))
    base, n = dol_offset(lo), hi - lo
    print('%-42s retail 0x%08X..0x%08X (%d bytes)   ours %d bytes' % (unit, lo, hi, n, len(ours)))
    for i in range(0, max(n, len(ours)), 4):
        r = fmt_word(struct.unpack_from('>I', dol, base + i)[0]) if i < n else ''
        o = fmt_word(struct.unpack_from('>I', ours, i)[0]) if i + 4 <= len(ours) else ''
        print(' +%03X %s %-26s | %-26s %s' % (i, ' ' if r == o else '*', r, o, ','.join(syms.get(i, []))))


def cmd_multiset(unit, lo, hi):
    ours = our_pool(unit) or b''
    base = dol_offset(lo)
    retail = collections.Counter(struct.unpack_from('>I', dol, base + i)[0] for i in range(0, hi - lo, 4))
    mine = collections.Counter(struct.unpack_from('>I', ours, i)[0] for i in range(0, len(ours), 4))
    print('%-42s retail %3d words  ours %3d words  common %3d' %
          (unit, sum(retail.values()), sum(mine.values()), sum((retail & mine).values())))
    for label, delta in (('retail has, we lack ', retail - mine), ('we have, retail lacks', mine - retail)):
        if delta:
            print('   %s: %s' % (label, ' '.join(fmt_word(w) * c for w, c in sorted(delta.items()))))


def main():
    argv = sys.argv[1:]
    cmd = argv[0] if argv and argv[0] in ('runs', 'diff', 'multiset') else 'runs'
    rest = argv[1:] if argv and argv[0] == cmd else argv
    if cmd == 'runs':
        cmd_runs(rest)
    else:
        if len(rest) != 3:
            raise SystemExit(__doc__)
        (cmd_diff if cmd == 'diff' else cmd_multiset)(rest[0], int(rest[1], 16), int(rest[2], 16))


if __name__ == "__main__":
    main()
