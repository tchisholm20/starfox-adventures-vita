#!/usr/bin/env python3
"""mldf_rules_check.py -- test mldf_endian.c's per-file byte-order rules against the REAL disc.

A wrong element width does not crash: a u16 table swapped as u32 swaps every PAIR, and the game
reads plausible, wrong values. So each ARRAY rule is judged against its alternative width on the
file's actual bytes (GSAE01 Rev 1 ISO):
  - index tabs (offsets into a partner .bin): at the right width the entries, masked of flag
    bits, are ~monotonic and bounded by the partner's size; at the wrong width they are not;
  - value tables (remaps, id lists): reported as a range, judged by the reader.
Exit 1 if any index-tab rule is beaten by the other width. Host only, no lane.

    python mldf_rules_check.py
"""
import re, struct, sys, os

import stfx_paths as P_
ISO = P_.ISO
SRC = P_.SRC + '/src/port/src/hal/mldf_endian.c'
ENUM = P_.SRC + '/include/main/mldf_fileid.h'

def fst(f):
    f.seek(0x424); off, size = struct.unpack('>II', f.read(8))
    f.seek(off); raw = f.read(size)
    n = struct.unpack('>I', raw[8:12])[0]
    names = raw[n * 12:]
    files, stack = {}, [('', n)]
    i = 1
    def name(k): return names[k:names.index(b'\0', k)].decode('ascii', 'replace')
    dirs = [(0, n, '')]
    while i < n:
        w0, a, b = struct.unpack('>III', raw[i * 12:i * 12 + 12])
        while dirs and i >= dirs[-1][1]: dirs.pop()
        nm = name(w0 & 0xffffff); path = (dirs[-1][2] + '/' if dirs and dirs[-1][2] else '') + nm
        if w0 >> 24: dirs.append((i, b, path))
        else: files[path.lower()] = (a, b)
        i += 1
    return files

def read(f, files, name):
    e = files.get(name.lower())
    if not e: return None
    f.seek(e[0]); return f.read(e[1])

def mono(vals):
    if len(vals) < 3: return 1.0
    return sum(1 for a, b in zip(vals, vals[1:]) if b >= a) / (len(vals) - 1)

def _be(data, fmt, off, n):
    return struct.unpack_from('>%d%s' % (n, fmt), data, off)

# Added 2026-10-12 (objdef audit). Known readings from Krystal (ObjDef 3), read big-endian from the real bytes.
VALUE_CHECKS = {
    0x16: lambda d: (lambda v: (v[0] == 4210 and v[17] == -1 and 0 < v[1] < 0x10000, 'TABLES.bin words[0..1]=%d,%d words[17]=%d (texture ids, -1 terminated)' % (v[0], v[1], v[17])))(_be(d, 'i', 0, 20)),
    0x41: lambda d: (lambda v: (v[0] == 0x800, 'OBJHITS.bin u32 @13200 = %#x (Krystal move 0 first hit mask, expect 0x800)' % v[0]))(_be(d, 'I', 13200, 1)),
    0x34: lambda d: (lambda v: (v == (1626, 3438, -7085, -398, 3203, -2466), 'WEAPONDA.bin Krystal move-0 first sample @0x6828 = %s' % (v,)))(_be(d, 'h', 0x6828, 6)),
    0x40: lambda d: (lambda v: (v == 0x0ad2, 'OBJEVENT.bin Krystal move-0 event @0x1658 = %#x (expect 0xad2)' % v))(_be(d, 'H', 0x1658, 1)[0]),
}

def main():
    names = {}
    for l in open(ENUM):
        m = re.search(r'(0x[0-9a-f]+), /\* (\S+)', l)
        if m: names[int(m.group(1), 16)] = m.group(2).rstrip(',')
    rules = [(int(i, 16), w) for i, w in re.findall(r'\[(0x[0-9a-f]+)\] = A(16|32)\(', open(SRC).read())]
    flip = {int(x, 16) for x in sys.argv[sys.argv.index('--flip') + 1].split(',')} if '--flip' in sys.argv else set()
    rules = [(i, ('16' if w == '32' else '32') if i in flip else w) for i, w in rules]   # watch it fail
    if len(rules) < 20: sys.exit('refusing: parsed only %d rules from %s' % (len(rules), SRC))
    f = open(ISO, 'rb'); files = fst(f)
    if len(files) < 4000: sys.exit('refusing: FST has %d files' % len(files))
    bad = 0
    # map-scoped tabs live per map directory; judge them in one representative map
    for i, w in sorted(rules):
        nm = names.get(i, '?')
        path = nm if '/' in nm else nm
        data = read(f, files, nm)
        where = nm
        if data is None:                                  # map-owned: take the first map that has it
            cands = sorted(p for p in files if p.endswith('/' + nm.lower()))
            if cands: where = cands[0]; data = read(f, files, where)
        if data is None:
            print('0x%02x %-14s NOT ON DISC' % (i, nm)); continue
        partner = None
        if nm.lower().endswith('.tab'):
            pb = where[:-4] + ('.bin' if where.endswith('.tab') else '.BIN')
            for c in (pb, pb.lower(), where[:-4] + '.zlb.bin'):
                if c.lower() in files: partner = files[c.lower()][1]; break
        v16 = list(struct.unpack('>%dH' % (len(data) // 2), data[:len(data) // 2 * 2]))
        v32 = list(struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4]))
        t32 = [x & 0x00ffffff for x in v32 if x != 0xffffffff]
        t16 = [x for x in v16 if x != 0xffff]
        m32, m16 = mono(t32), mono(t16)
        line = '0x%02x %-14s A%s  %7d B  mono32 %.3f  mono16 %.3f' % (i, nm, w, len(data), m32, m16)
        tw = t32 if w == '32' else t16
        if partner is not None:
            inb = sum(1 for x in tw if x <= partner) / max(1, len(tw))
            line += '  <=partner(%d) %.3f' % (partner, inb)
        else:
            line += '  range16 %d..%d' % (min(t16 or [0]), max(t16 or [0]))
        verdict = ''
        if partner is not None and len(tw) < 16:
            verdict = '  (INCONCLUSIVE: %d entries; the rule rests on the consumer type)' % len(tw)
        elif partner is not None:          # an index tab: the chosen width must win on monotonicity
            ok = (m32 >= m16) if w == '32' else (m16 >= m32)
            if not ok: verdict = '  <-- OTHER WIDTH FITS BETTER'; bad += 1
        print(line + verdict + ('   [%s]' % where if where != nm else ''))
        chk = VALUE_CHECKS.get(i)           # value tables with no partner .bin: judge the swapped values against a known reading
        if chk is not None:
            ok, why = chk(data)
            print('      value check 0x%02x: %s%s' % (i, why, '' if ok else '   <-- FAILS')); bad += 0 if ok else 1
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
