#!/usr/bin/env python3
"""rev_map.py -- translate GSAE01 Rev 0 symbol addresses to Rev 1.

The decomp's primary map is Rev 0 (config/GSAE01/symbols.txt); our disc is Rev 1.
A Rev 0 symbol is placed in Rev 1 using the nearest symbols IN THE SAME SECTION
that carry the same name in both maps: address + (anchor_rev1 - anchor_rev0).
It is CONFIDENT only when the preceding and following anchors agree on that shift;
disagreement means bytes were inserted/removed between them, and the symbol is
reported AMBIGUOUS rather than guessed.

    python rev_map.py --validate          # predict symbols already named in BOTH maps
    python rev_map.py SYM [SYM ...]       # map symbols, print rev1 address + confidence
"""
import re, sys, bisect

import stfx_paths as P_
CFG = P_.SRC + '/config/'
LINE = re.compile(r'^(\S+)\s*=\s*(\.[\w.]+):0x([0-9A-Fa-f]+);\s*//(.*)$')

def load(path):
    out = {}
    for l in open(path, encoding='utf-8', errors='replace'):
        m = LINE.match(l.strip())
        if m:
            meta = dict(kv.split(':', 1) for kv in m.group(4).split() if ':' in kv)
            out[m.group(1)] = (m.group(2), int(m.group(3), 16), meta)
    return out

r0 = load(CFG + 'GSAE01/symbols.txt')
r1 = load(CFG + 'GSAE01_rev1/symbols.txt')

# anchors: same name, same section in both maps; skip auto names (lbl_/fn_/...) whose
# NAME encodes a Rev 0 address -- they are not the same object by construction.
auto = re.compile(r'^(lbl|fn|func|sub|gap|jumptable|jtbl|str|@)_?[0-9A-Fa-f]{6,}')
anchors = {}
for n, (sec, a0, _) in r0.items():
    if n in r1 and r1[n][0] == sec and not auto.match(n):
        anchors.setdefault(sec, []).append((a0, r1[n][1], n))
for sec in anchors: anchors[sec].sort()

def predict(name, exclude_self=True):
    if name not in r0: return None, 'not in rev0 map', None
    sec, a0, meta = r0[name]
    lst = anchors.get(sec, [])
    keys = [x[0] for x in lst]
    i = bisect.bisect_right(keys, a0)
    before = [x for x in lst[:i] if not (exclude_self and x[2] == name)]
    after = [x for x in lst[i:] if not (exclude_self and x[2] == name)]
    b = before[-1] if before else None
    a = after[0] if after else None
    if not b and not a: return None, 'no anchor in section', meta
    db = (b[1] - b[0]) if b else None
    da = (a[1] - a[0]) if a else None
    if db is not None and da is not None and db != da:
        return None, f'AMBIGUOUS shift before {db:+#x} ({b[2]}) vs after {da:+#x} ({a[2]})', meta
    d = db if db is not None else da
    return a0 + d, ('CONFIDENT' if (db is not None and da is not None) else 'one-sided'), meta

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--validate':
        names = sys.argv[2:] or None
        # validate on data objects present under the same name in both maps
        cand = [n for n in (names or r0) if n in r1 and r0[n][0] == r1[n][0]
                and r0[n][2].get('type') == 'object']
        ok = bad = amb = 0; badl = []
        for n in cand:
            p, conf, _ = predict(n, exclude_self=True)
            if p is None: amb += 1; continue
            if p == r1[n][1]: ok += 1
            else: bad += 1; badl.append((n, conf, hex(p), hex(r1[n][1])))
        print(f'validation over {len(cand)} data objects named in both maps: '
              f'{ok} exact, {bad} WRONG, {amb} not predicted (ambiguous/no anchor)')
        for x in badl[:15]: print('  WRONG', *x)
        sys.exit(1 if bad else 0)
    for n in sys.argv[1:]:
        p, conf, meta = predict(n, exclude_self=False)
        print(n, hex(p) if p else '-', conf, meta and meta.get('size'), meta and meta.get('data'))
