#!/usr/bin/env python3
"""Compare every built object's initialised-data layout against the retail carve.

MWCC GC/2.0 emits `.data`/`.rodata`/`.sdata` in STRICT SOURCE ORDER: a file-scope
initialised array lands at its DECLARATION, and a jumptable or string literal lands at
the FUNCTION that owns it. Nothing is grouped or sorted, so the carve's layout reads
back the original declaration order directly -- the exact parallel of the `.bss`
first-use law measured in section 11 of docs/priced_classes.md.

`bss_order_scan.py` only compares NAMED symbols and skips anonymous ones. That hides the
whole signal here, because a jumptable/string-pool blob is anonymous (`@123`,
`@stringBase0`) and MWCC RENUMBERS those per TU, so they cannot be paired by name. This
tool pairs them POSITIONALLY instead: the sequence of (kind, size) slots, with named
symbols keeping their names.

A row is a claim that our TU declares those objects in the wrong order. Only declaration
moves fix it -- never code motion.

Nothing scores a `.data` permutation directly: objdiff pairs data symbols by name, and a
NonMatching unit links from the carve so its layout never reaches the DOL. The oracle for
those is to flip the unit to Object(Matching, ...), force the link and read the sha1.

  python3 tools/data_order_scan.py [--sections .data,.rodata] [--verbose]
"""

import os, subprocess, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OD = ROOT + '/build/binutils/powerpc-eabi-objdump'
SRC = ROOT + '/build/GSAE01/src'
OBJ = ROOT + '/build/GSAE01/obj'

DEFAULT_SECTIONS = ('.data', '.rodata', '.sdata', '.sdata2', '.data2')


def layout(path, wanted):
    """objdump -t -> {section: [(offset, size, name, anonymous)]} sorted by offset."""
    out = subprocess.run([OD, '-t', path], capture_output=True, text=True).stdout
    d = {}
    for ln in out.splitlines():
        m = re.match(r'^([0-9a-f]{8}) (.{7}) +(\S+)\t([0-9a-f]{8}) (?:\.hidden )?(.+)$', ln)
        if not m:
            continue
        off, flags, sec, size, name = m.groups()
        if 'O' not in flags or sec not in wanted:
            continue
        anon = name.startswith('@') or name.startswith('.')
        d.setdefault(sec, []).append((int(off, 16), int(size, 16), name, anon))
    return {k: sorted(v) for k, v in d.items()}


def slot(item):
    """Comparison key. Anonymous names are section-local and renumbered per TU, so an
    anonymous slot is identified only by its SIZE; a named slot by its name."""
    off, size, name, anon = item
    return ('@', size) if anon else ('=', name)


def main():
    wanted = set(DEFAULT_SECTIONS)
    verbose = '--verbose' in sys.argv
    for i, a in enumerate(sys.argv):
        if a == '--sections':
            wanted = set(sys.argv[i + 1].split(','))

    units = 0
    populated = 0
    rows = []
    size_rows = []
    for dp, _, fs in os.walk(SRC):
        for f in sorted(fs):
            if not f.endswith('.o'):
                continue
            ours = os.path.join(dp, f)
            rel = os.path.relpath(ours, SRC)
            th = os.path.join(OBJ, rel)
            if not os.path.isfile(th):
                continue
            units += 1
            a = layout(ours, wanted)
            b = layout(th, wanted)
            for sec in sorted(set(a) | set(b)):
                av, bv = a.get(sec, []), b.get(sec, [])
                if not av and not bv:
                    continue
                populated += 1
                ak = [slot(x) for x in av]
                bk = [slot(x) for x in bv]
                if ak == bk:
                    continue
                # A pure count/size mismatch is a CONTENT question, not an order one.
                if sorted(ak) == sorted(bk):
                    rows.append((rel, sec, av, bv))
                else:
                    size_rows.append((rel, sec, av, bv))

    def show(tag, rs):
        print('%d %s' % (len(rs), tag))
        for rel, sec, av, bv in rs:
            print('\n%s  %s' % (rel, sec))
            print('  ours  :', ' '.join('%s/%d' % ('@' if x[3] else x[2], x[1]) for x in av))
            print('  retail:', ' '.join('%s/%d' % ('@' if x[3] else x[2], x[1]) for x in bv))
        print()

    print('%d units compared, %d populated sections in %s\n' % (units, populated, sorted(wanted)))
    show('sections that are a pure PERMUTATION of the carve (fixable by declaration moves)', rows)
    if verbose:
        show('sections whose slot MULTISET differs from the carve (content, not order)', size_rows)
    else:
        print('%d sections whose slot MULTISET differs from the carve (--verbose to list)'
              % len(size_rows))


if __name__ == "__main__":
    main()
