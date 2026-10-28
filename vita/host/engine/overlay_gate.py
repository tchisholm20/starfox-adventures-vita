#!/usr/bin/env python3
"""overlay_gate.py -- does any game TU lay a struct over a global that the PORT's object
is too small to hold?

Decompiled code reaches neighbouring globals through one base symbol by RETAIL layout:
    struct MldfTables* tbl = (struct MldfTables*)gResourceFileTable;   // a 0x160-byte array
    tbl->owners[slot] = mapId;                                          // +0x19738
MWCC laid those arrays out contiguously; GCC does not, so the access lands in unrelated
data (Vita run 9: mapLoadDataFile read a garbage adjacent mapId through struct MldfNames
laid over the 10-byte string "AUDIO.tab"). The string-pool form of the same class is
pool_gate.py.

Method, per game TU that the link uses:
  1. preprocess it with the build's own flags and find every `(T *) &?sym` cast (after
     macro expansion, so MLDF_*-style accessors and dead #if branches are seen correctly);
  2. sizeof(T) from a probe object compiled with the same flags (#include of the TU);
  3. the bytes the LINKED ELF actually has from sym's address to the end of the largest
     sized object containing it (so an alias into a bigger block gets the block);
  4. FLAG sizeof(T) > available.
What it cannot see (stated, not hidden): `(T*)sym` then `p[i]` past one element, raw
`sym + 0x...` arithmetic, and casts whose type only exists in function scope (listed as
UNRESOLVED, never passed).

    python overlay_gate.py ELF [--allow overlay_open.txt]   (allowed = printed OPEN, not passed silently)
Exit 0 = no flags, 1 = flags (listed), 2 = tool error.
"""
import bisect, collections, os, re, subprocess, sys, tempfile, concurrent.futures as cf

import stfx_paths as P_
R = P_.SRC
V = P_.VITA              # projects/stairfax-vita, from stfx_paths (STFX_VITA overrides)
OUT = P_.OUT
BIN = (os.environ.get('VITASDK_BIN') or 'C:/dev/msys64/usr/local/vitasdk/bin').replace('\\', '/').rstrip('/') + '/'   # VITASDK_BIN overrides
GCC, NM = BIN + 'arm-vita-eabi-gcc.exe', BIN + 'arm-vita-eabi-nm.exe'
FLAGS = ['-std=gnu11', '-O2', '-fno-strict-aliasing', '-fno-section-anchors', '-mcpu=cortex-a9',
         '-mfpu=neon', '-mfloat-abi=hard', '-DTARGET_PC=1', '-DSTAIRFAX_PORT=1', '-w',
         '-I' + R + '/src/port/include', '-I' + V + '/src', '-I' + OUT + '/gen',
         '-idirafter', R + '/include', '-idirafter', R]
CAST = re.compile(r'\(\s*((?:struct\s+|union\s+)?[A-Za-z_]\w*)\s*\*\s*\)\s*\(?\s*(&?)\s*([gs][A-Z]\w*)\b(?!\s*[\(\[.+-]|\s*->|\s*\)\s*\[)')
# not an overlay: sym(...) sym[..] sym.x sym->x; `(T*)(&sym)[i]` (casts the LOADED element);
# `(T*)(sym - k)` / `(sym + k)` (arithmetic on the base: a stated blind spot, see above)
# 2 = array (its name IS its storage), 1 = pointer; a compile failure (integer) reads as None.
ARR = '__builtin_types_compatible_p(__typeof__(%s), __typeof__(&(%s)[0])) ? 1 : 2'
SKIP_T = {'u8', 's8', 'char', 'void', 'u16', 's16', 'u32', 's32', 'int', 'f32', 'float',
          'unsigned', 'signed', 'short', 'long', 'double', 'f64', 'u64', 's64', 'uint8_t',
          'uint16_t', 'uint32_t', 'int8_t', 'int16_t', 'int32_t', 'BOOL', 'bool'}

def tu_list():
    """The game TUs in the link, with the source each was compiled from (wgpipe copy if any)."""
    srcs = [l.strip() for l in open(OUT + '/all_srcs.txt') if l.strip()]
    out = []
    for f in srcs:
        m = P_._win(f)                       # all_srcs.txt spells the tree /d/... (compile_all.sh); R is D:/...
        rel = m[len(R) + 1:] if m.lower().startswith(R.lower() + '/') else m   # the path under the engine tree
        if not rel.startswith(('src/main/', 'src/dlls/', 'src/track/')) or not rel.endswith('.c'):
            continue
        tid = re.sub(r'[/.]', '_', rel)
        rw = OUT + '/rw/' + tid + '.c'
        out.append((rel, os.path.join(R, rel), rw if os.path.exists(rw) else None, tid))
    return out

def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace')

def extra(src, rw):
    return (['-iquote', os.path.dirname(src)] if rw else [])

def casts_of(rel, src, rw, tid):
    p = run([GCC] + FLAGS + extra(src, rw) + ['-E', '-P', rw or src])
    if p.returncode:
        return rel, None, 'preprocess failed'
    # (type, symbol, took-the-address). `(T*)ptrVar` casts a VALUE and is no overlay;
    # `(T*)&sym` and `(T*)array` reinterpret the symbol's own storage.
    found = {(t.strip(), s, bool(a)) for t, a, s in CAST.findall(p.stdout) if t.split()[-1] not in SKIP_T}
    return rel, sorted(found), None

def sizes_of(rel, src, rw, types):
    """Evaluate each probe expression in the TU's own scope. Returns {expr: value or None}."""
    res = {}
    def probe(ts):
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, 'p.c')
            with open(c, 'w') as f:
                f.write('#include "%s"\n' % (rw or src).replace('\\', '/'))
                for i, t in enumerate(ts):
                    f.write('char __ovl_probe_%d[%s];\n' % (i, t))
            o = os.path.join(d, 'p.o')
            if run([GCC] + FLAGS + extra(src, rw) + ['-fno-common', '-c', c, '-o', o]).returncode:
                return None
            got = {}
            for line in run([NM, '-S', o]).stdout.splitlines():
                m = re.match(r'[0-9a-f]+ ([0-9a-f]+) \w __ovl_probe_(\d+)$', line)
                if m: got[ts[int(m.group(2))]] = int(m.group(1), 16)
            return got
    g = probe(types)
    if g is None:                                    # one bad type spoils the batch: go one by one
        for t in types:
            one = probe([t]); res[t] = one.get(t) if one else None
    else:
        for t in types: res[t] = g.get(t)
    return res

def elf_avail(elf):
    """name -> list of bytes available from the symbol to the end of its largest container."""
    syms = []
    for line in run([NM, '-S', '--defined-only', elf]).stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'bBdDrRgGsS':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        elif len(p) == 3 and p[1] in 'bBdDrRgGsS':
            syms.append((int(p[0], 16), 0, p[2]))
    objs = sorted((a, a + s) for a, s, _ in syms if s)
    import bisect
    starts = [a for a, _ in objs]
    # prefix max of end over objects sorted by start: an object starting at or before `a` whose
    # end is past `a` contains it. (A 64-entry look-back missed the whole-.bss retail block,
    # whose start is ~1850 symbols before most of its aliases -- 11 false FLAGs on run 18.)
    pmax, m = [], 0
    for _, e in objs: m = max(m, e); pmax.append(m)
    avail = {}
    for a, s, n in syms:
        best = a + s
        i = bisect.bisect_right(starts, a)
        if i and pmax[i - 1] > a: best = max(best, pmax[i - 1])
        avail.setdefault(n, []).append(best - a)
    return avail

def elf_addrs(elf):
    """(name -> address, name -> own size) for GLOBAL data symbols with a unique name."""
    addr, size, seen = {}, {}, collections.Counter()
    for line in run([NM, '-S', '--defined-only', elf]).stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[-2] in 'BDRGS':
            seen[p[-1]] += 1; addr[p[-1]] = int(p[0], 16)
            size[p[-1]] = int(p[1], 16) if len(p) == 4 else 0
    for n, c in seen.items():
        if c > 1: addr.pop(n, None)
    return addr, size

def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    elf = a[0]
    allow = set()
    if '--allow' in a:
        allow = {l.split('#')[0].strip() for l in open(a[a.index('--allow') + 1]) if l.split('#')[0].strip()}
    tus = tu_list()
    if len(tus) < 500:
        print('refusing: only %d game TUs in %s/all_srcs.txt' % (len(tus), OUT)); sys.exit(2)
    avail = elf_avail(elf)
    if len(avail) < 1000:
        print('refusing: only %d data symbols in %s' % (len(avail), elf)); sys.exit(2)
    flags, unresolved, nosym, checked, errs, values = [], [], 0, 0, [], 0
    layout, reached, opened = [], 0, []
    retail = {}
    for l in open(P_.MAP, encoding='utf-8', errors='replace'):
        m = re.match(r'^(\S+) = \.[\w.]+:0x([0-9A-Fa-f]+);', l.strip())
        if m: retail[m.group(1)] = int(m.group(2), 16)
    raddrs = sorted((ad, nm) for nm, ad in retail.items())
    rkeys = [ad for ad, _ in raddrs]
    addr_of, own = elf_addrs(elf)
    contig = []

    def contiguous(s, n):
        """The object is too small, but are the bytes after it the RETAIL neighbours? True only
        if every retail symbol in [s, s+n) is in the ELF at exactly its retail offset from s and
        the last one's own size reaches s+n. (Since 2026-09-27 game TUs compile with
        -fno-toplevel-reorder and upstream's per-TU data layout is retail's, so a TU's own
        neighbours now land this way; each such cast is printed as CONTIGUOUS, not passed silently.)"""
        if s not in retail or s not in addr_of:
            return False
        ra, last = retail[s], (retail[s], own.get(s, 0))
        for rb, nb in raddrs[bisect.bisect_right(rkeys, ra):bisect.bisect_left(rkeys, ra + n)]:
            if nb not in addr_of or addr_of[nb] - addr_of[s] != rb - ra:
                return False
            last = (rb, own.get(nb, 0))
        return (last[0] - ra) + last[1] >= n

    with cf.ThreadPoolExecutor(4) as ex:
        scans = list(ex.map(lambda t: casts_of(*t), tus))
        work = [(t, c) for t, (_, c, e) in zip(tus, scans) if c]
        errs = [(r, e) for r, c, e in scans if e]
        def judge(tc):
            (rel, src, rw, tid), cs = tc
            probes = {'sizeof(%s)' % t for t, _, _ in cs}
            probes |= {ARR % (x, x) for _, x, amp in cs if not amp}
            sz = sizes_of(rel, src, rw, sorted(probes))
            return rel, cs, sz
        for rel, cs, sz in ex.map(judge, work):
            for t, s, amp in cs:
                if not amp and sz.get(ARR % (s, s)) != 2:
                    values += 1; continue            # a pointer/integer VALUE: not an overlay
                n = sz.get('sizeof(%s)' % t)
                if n is None: unresolved.append((rel, t, s)); continue
                if s not in avail: nosym += 1; continue
                checked += 1
                have = min(avail[s])                 # same-named statics: the tightest one
                if n > have and contiguous(s, n):
                    contig.append((rel, t, s, n, have)); continue
                if n > have:
                    (opened if '%s %s %s' % (rel, t, s) in allow else flags).append((rel, t, s, n, have))
                    continue
                # Enough bytes is not enough: a cast reaching past its own symbol expects the
                # RETAIL neighbours at their retail offsets. Check every one in range.
                if n > own.get(s, n) and s in retail and s in addr_of:
                    reached += 1; ra = retail[s]
                    for rb, nb in raddrs[bisect.bisect_right(rkeys, ra):bisect.bisect_left(rkeys, ra + n)]:
                        if nb in addr_of and addr_of[nb] - addr_of[s] != rb - ra:
                            layout.append((rel, t, s, nb, rb - ra, addr_of[nb] - addr_of[s]))
    print('overlay_gate: %d TUs, %d overlay casts checked, %d FLAGGED, %d value casts skipped, %d unresolved types, %d symbols not in ELF, %d preprocess errors'
          % (len(tus), checked, len(flags), values, len(unresolved), nosym, len(errs)))
    print('  layout: %d casts reach past their own symbol; %d retail neighbours MISPLACED' % (reached, len(layout)))
    for rel, t, s, nb, want_off, got in sorted(set(layout))[:60]:
        print('  MISPLACED  %-36s (%s*)%s: %s at %+#x, retail +%#x' % (rel, t, s, nb, got, want_off))
    for rel, t, s, n, have in sorted(contig):
        print('  CONTIGUOUS  %-34s (%s*)%s  sizeof=0x%x  object 0x%x; every retail neighbour at its retail offset'
              % (rel, t, s, n, have))
    for rel, t, s, n, have in sorted(opened):
        print('  OPEN  %-40s (%s*)%s  sizeof=0x%x  port object has 0x%x' % (rel, t, s, n, have))
    stale = allow - {'%s %s %s' % o[:3] for o in opened}
    for x in sorted(stale): print('  STALE OPEN ENTRY (no longer flagged -- remove it): ' + x)
    for rel, t, s, n, have in sorted(flags):
        print('  FLAG  %-40s (%s*)%s  sizeof=0x%x  port object has 0x%x' % (rel, t, s, n, have))
    for rel, t, s in sorted(unresolved)[:40]:
        print('  UNRESOLVED  %-40s (%s*)%s' % (rel, t, s))
    if len(unresolved) > 40: print('  ... %d more unresolved' % (len(unresolved) - 40))
    for r, e in errs[:10]: print('  ERROR  %s: %s' % (r, e))
    sys.exit(1 if flags or layout else 0)

if __name__ == '__main__':
    main()
