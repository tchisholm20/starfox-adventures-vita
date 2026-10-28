#!/usr/bin/env python3
"""Compare a unit's built CONSTANT-POOL CONTENT against the retail image.

The missing third leg beside order_check (function placement) and
section_size_check (section extents). Both of those, plus objdiff's fuzzy score,
are structurally blind to a pool whose SIZE is right and whose VALUES are wrong:

  * objdiff pairs .text functions by name and scores instruction content. An
    `lfs f1, -0x49f4(r2)` scores identical to the retail `lfs f1, -0x49e8(r2)`
    under mnemonic comparison of a matched function only if the displacement
    also matches -- but when the WRONG VALUE lands at the RIGHT SLOT the .text
    is byte-identical and the defect is entirely in .sdata2, which fuzzy never
    reads.
  * section_size_check compares sizes only. A same-size, wrong-content pool
    passes it.
  * main.dol's sha1 is blind while the unit is INCOMPLETE, because the link
    consumes the retail object. The defect appears the instant it is promoted.

A wrong VALUE is rarer than it looks, so the verdicts are graded. Only
VALUE-DIFF -- a word present in ours that retail does not hold anywhere in the
unit's pool -- can be a miscoded literal. PERM (same multiset, other order) and
PAD (differs only in zero/0x80000000 alignment words) are emission-order
artifacts; SUBSET means the unit is merely incomplete. cameramodeforcebehind,
long cited as a wrong-value case, is in fact a PERM.

Four things this must get right, because each one on its own manufactures a
census of phantom defects, and a phantom pool defect is the documented bait for
the banned pool-reconstruction hack.

  * A unit may claim `.sdata2` MORE THAN ONCE -- keeping only the last claim
    compares a whole pool against a fragment of itself.
  * A SHIFT (pool matching verbatim at another address, meaning the claim is
    misplaced) is only evidence when the untruncated pool is long enough to be
    unique: an 8-byte prefix recurs hundreds of times in a float pool.
  * The MULTISET must be built from our WHOLE pool. When ours is longer than
    the claim the positional walk is truncated to the claim, and reusing that
    truncated prefix for the multiset compares a fragment of ourselves against
    all of retail -- SUPERSET becomes unreachable and every under-claimed unit
    reads as a wrong-value BAD. objhits, track/intersect, engine/86,
    SB_Galleon and synth_seq_dispatch were all BAD for this reason alone.
  * Weak (COMDAT) words are not pool content. MSL's inlined sqrt/sqrtf carry
    `_half`/`_three` f64 local statics as weak symbols that the linker keeps
    ONE copy of image-wide, so they are in no unit's split range.

UNDERCLAIM is the graded verdict for a superset the retail object itself
explains: when retail's own code reaches BELOW its claimed start, the claim is
the TAIL of the real pool and the surplus is a split-boundary artifact. A
residual BAD is also annotated with any function we define that retail does
not, since an extra function mints its own constants.

Ground truth is orig/GSAE01/sys/main.dol. For each unit that claims a data
range in config/GSAE01/splits.txt, this reads the same-named section out of our
built object and compares it word-for-word against the retail bytes at the
claimed address.

_SDA2_BASE_ = 0x803E6500, _SDA_BASE_ = 0x803E31E0 (displacements are printed
relative to whichever base covers the address, to line up with the .text).

usage: python3 tools/pool_content_check.py [unit-substring ...]
       --sections .sdata2,.sdata   sections to compare (default .sdata2)
       --all-sections              compare every rodata/data-class section
       --quiet                     one line per failing unit
exit status 1 if any scanned unit has a content mismatch.
"""
import json
from collections import Counter
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_guard import require_nonempty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJDUMP = os.path.join(ROOT, 'build/binutils/powerpc-eabi-objdump')
DOL = os.path.join(ROOT, 'orig/GSAE01/sys/main.dol')
SPLITS = os.path.join(ROOT, 'config/GSAE01/splits.txt')

SDA2_BASE = 0x803E6500
SDA_BASE = 0x803E31E0

DEFAULT_SECTIONS = ('.sdata2',)
ALL_SECTIONS = ('.rodata', '.data', '.sdata', '.sdata2')


class Dol(object):
    def __init__(self, path):
        blob = open(path, 'rb').read()
        offs = struct.unpack('>18I', blob[0x00:0x48])
        addrs = struct.unpack('>18I', blob[0x48:0x90])
        sizes = struct.unpack('>18I', blob[0x90:0xD8])
        self.blob = blob
        self.spans = [(addrs[i], sizes[i], offs[i])
                      for i in range(18) if sizes[i]]

    def read(self, addr, length):
        """Bytes at virtual address, or None if not fully mapped."""
        for a, sz, off in self.spans:
            if a <= addr and addr + length <= a + sz:
                start = off + (addr - a)
                return self.blob[start:start + length]
        return None


def parse_splits(path):
    """unit source path (e.g. main/foo.c) -> {section: [(start, end), ...]}

    A unit may claim the SAME section more than once (MWCC emits one input
    section per alignment run, and the split mirrors that). Keeping only the
    last claim compares the unit's whole pool against a fragment of it, which
    manufactures a spurious mismatch for every such unit -- so collect them
    all, in file order, and treat them as one concatenated range.
    """
    units = {}
    cur = None
    for line in open(path):
        if not line.strip() or line.startswith('#'):
            continue
        if not line[0].isspace():
            name = line.strip()
            if name.endswith(':'):
                name = name[:-1]
            if name == 'Sections':
                cur = None
                continue
            cur = units.setdefault(name, {})
            continue
        if cur is None:
            continue
        m = re.match(r'\s*(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)',
                     line)
        if m:
            cur.setdefault(m.group(1), []).append(
                (int(m.group(2), 16), int(m.group(3), 16)))
    return units


def section_bytes(obj_path, section):
    """Raw contents of `section` in an object file, or None if absent."""
    r = subprocess.run([OBJDUMP, '-s', '-j', section, obj_path],
                       capture_output=True)
    if r.returncode != 0:
        return None
    data = bytearray()
    seen = False
    for line in r.stdout.decode('utf8', 'replace').splitlines():
        if line.startswith('Contents of section '):
            seen = line.split()[3].rstrip(':') == section
            continue
        if not seen:
            continue
        m = re.match(r'\s*([0-9a-f]+)\s((?:[0-9a-f]{2,8}\s){1,4})\s', line)
        if not m:
            continue
        data += bytes.fromhex(m.group(2).replace(' ', ''))
    return bytes(data) if seen or data else None


def has_relocs(obj_path, section):
    r = subprocess.run([OBJDUMP, '-r', obj_path], capture_output=True)
    if r.returncode != 0:
        return False
    active = False
    for line in r.stdout.decode('utf8', 'replace').splitlines():
        if line.startswith('RELOCATION RECORDS FOR ['):
            active = line.split('[')[1].split(']')[0] == section
            continue
        if active and re.match(r'^[0-9a-f]{8}\s', line):
            return True
    return False


def comdat_words(obj_path, section):
    """Words in `section` that belong to a WEAK (COMDAT) symbol.

    MSL's inlined sqrt/sqrtf carry their `_half`/`_three` f64 local statics as
    weak symbols. The linker keeps ONE copy of each for the whole image, so
    they are absent from every individual unit's split range -- counting them
    as pool content makes every unit that inlines a square root look like it
    emits surplus literals. synth_seq_dispatch's four "extra" words are exactly
    these two constants, twice.
    """
    r = subprocess.run([OBJDUMP, '-t', obj_path], capture_output=True)
    if r.returncode != 0:
        return Counter()
    data = section_bytes(obj_path, section) or b''
    out = Counter()
    for line in r.stdout.decode('utf8', 'replace').splitlines():
        m = re.match(r'^([0-9a-f]{8})\s(.{7})\s+O?\s*(\S+)\s+([0-9a-f]{8})\s',
                     line)
        if not m or m.group(3) != section or 'w' not in m.group(2):
            continue
        off, size = int(m.group(1), 16), int(m.group(4), 16)
        for i in range(off, min(off + size, len(data)) & ~3, 4):
            out[data[i:i + 4]] += 1
    return out


def claim_start_hint(retail_obj, claims):
    """Lowest .sdata2 address the RETAIL object references below its claim.

    A retail object whose own code reaches BELOW the range splits.txt hands it
    proves the claim is a tail FRAGMENT of the real pool -- the words ahead of
    `start` were attributed to a neighbour or to an auto_ blob. Our object then
    holds pool words the claim cannot contain, and that surplus is a split
    boundary artifact, not a surplus literal. Only `lbl_ADDR` targets are read
    here: their address is in the name, so the hint needs no symbol map and
    cannot mis-resolve.
    """
    if not retail_obj or not os.path.exists(retail_obj):
        return None
    r = subprocess.run([OBJDUMP, '-r', retail_obj], capture_output=True)
    if r.returncode != 0:
        return None
    lo = min(b for b, _ in claims)
    win = sda_window(lo)
    if win is None:
        return None
    # An SDA21 reloc is r2- OR r13-relative, so the same relocation type also
    # names .sdata words. Only an address in the SAME small-data window as the
    # claim is evidence about THIS section's boundary.
    below = [int(a, 16) for a in
             re.findall(r'R_PPC_(?:EMB_)?SDA21\s+lbl_([0-9A-Fa-f]{8})',
                        r.stdout.decode('utf8', 'replace'))]
    below = [a for a in below if win[0] <= a < lo]
    return (min(below), len(below)) if below else None


def surplus_fns(ours_obj, retail_obj):
    """Functions our object defines that the retail object does not.

    A pool word retail has nowhere is only a MISCODED literal if the two
    objects hold the same code. An extra function mints its own constants, so
    it accounts for surplus pool words without any literal being wrong.
    """
    def fns(p):
        r = subprocess.run([OBJDUMP, '-t', p], capture_output=True)
        if r.returncode != 0:
            return set()
        return {m.group(1) for m in
                (re.match(r'^[0-9a-f]{8}\s.*\sF\s+\.text\s+[0-9a-f]{8}\s+(\S+)',
                          ln) for ln in
                 r.stdout.decode('utf8', 'replace').splitlines()) if m}
    if not (ours_obj and retail_obj and os.path.exists(retail_obj)):
        return set()
    return fns(ours_obj) - fns(retail_obj)


def sda_window(addr):
    """The +/-32K small-data window (r2 or r13) that covers `addr`."""
    for base in (SDA2_BASE, SDA_BASE):
        if base - 0x8000 <= addr < base + 0x8000:
            return (base - 0x8000, base + 0x8000)
    return None


def disp(addr):
    if SDA2_BASE - 0x8000 <= addr < SDA2_BASE + 0x8000:
        return 'r2%+d (%04x)' % (addr - SDA2_BASE,
                                 (addr - SDA2_BASE) & 0xFFFF)
    if SDA_BASE - 0x8000 <= addr < SDA_BASE + 0x8000:
        return 'r13%+d (%04x)' % (addr - SDA_BASE,
                                  (addr - SDA_BASE) & 0xFFFF)
    return '-'


def fmt_vals(counter):
    parts = []
    for w, c in sorted(counter.items()):
        v = '%.9g' % f32(w) if len(w) == 4 else w.hex()
        parts.append(v + ('' if c == 1 else ' x%d' % c))
    return ', '.join(parts)


def f32(word):
    return struct.unpack('>f', word)[0]


def shift_hint(ours, dol, base, span):
    """If our whole pool appears verbatim elsewhere nearby, say where.

    A pool that matches at a DIFFERENT address is a CLAIM/ORDER defect (the
    split hands the unit the wrong slice), not a wrong literal in the source.
    """
    if len(ours) < 8:
        return None
    for delta in range(-0x200, 0x201, 4):
        if delta == 0:
            continue
        blob = dol.read(base + delta, len(ours))
        if blob is not None and blob == ours:
            return delta
    return None


def compare(name, section, ours, claims, dol, quiet, ours_obj=None,
            retail_obj=None):
    """Return list of report lines (empty == clean)."""
    base = claims[0][0]
    span = sum(e - b for b, e in claims)
    lines = []
    n = len(ours)
    ours_full = ours
    if n > span:
        lines.append('  [size] %s ours=0x%x claim=0x%x -- comparing the '
                     'claimed 0x%x only (section_size_check owns the rest)'
                     % (section, n, span, span))
        ours = ours[:span]
    retail = b''
    for b, e in claims:
        chunk = dol.read(b, e - b)
        if chunk is None:
            return ['  [skip] %s not mapped in the DOL at 0x%08X'
                    % (section, b)]
        retail += chunk
    retail = retail[:len(ours)]
    bad = []
    for off in range(0, len(ours) & ~3, 4):
        a, b = ours[off:off + 4], retail[off:off + 4]
        if a != b:
            bad.append((off, a, b))
    tail = len(ours) & 3
    if tail and ours[-tail:] != retail[-tail:]:
        bad.append((len(ours) - tail, ours[-tail:], retail[-tail:]))
    if not bad:
        return lines
    words = max(1, len(ours) // 4)
    kind = 'BAD'
    note = ''
    full = b''.join(dol.read(b, e - b) or b'' for b, e in claims) or retail
    # The multiset must see our WHOLE pool. Building it from the prefix that was
    # truncated to the claim compares a fragment of ourselves against all of
    # retail, which makes SUPERSET unreachable and reports every under-claimed
    # unit as a wrong-value [BAD] -- the precise bait for the banned
    # pool-reconstruction hack. objhits, track/intersect, engine/86, SB_Galleon
    # and synth_seq_dispatch all read [BAD] under the truncated multiset and are
    # SUPERSET under this one.
    ow = Counter(ours_full[i:i + 4] for i in range(0, len(ours_full) & ~3, 4))
    ow -= comdat_words(ours_obj, section) if ours_obj else Counter()
    rw = Counter(full[i:i + 4] for i in range(0, len(full) & ~3, 4))
    pad = b'\x00\x00\x00\x00'
    only_ours, only_retail = ow - rw, rw - ow
    if not only_ours and not only_retail:
        kind = 'PERM'
        note = '  -- same VALUES in a different ORDER: an emission-order ' \
               '(first-use) artifact, NOT a wrong literal'
    elif not only_ours and set(only_retail) == {pad}:
        kind = 'PERM'
        note = '  -- same VALUES in a different ORDER, plus retail zero ' \
               'word(s): the alignment PAD the reordering pushes ahead of ' \
               'the 8-aligned int-to-float magic pair. Still an emission-' \
               'order artifact, NOT a wrong literal'
    elif not only_ours:
        kind = 'SUBSET'
        note = '  -- every word we emit also occurs in retail; retail has ' \
               '%d MORE (%s). The unit is INCOMPLETE, not miscoded' \
               % (sum(only_retail.values()), fmt_vals(only_retail))
    elif not only_retail:
        hint = claim_start_hint(retail_obj, claims)
        if hint:
            kind = 'UNDERCLAIM'
            note = '  -- every retail word occurs in ours and we emit %d ' \
                   'MORE, but the retail object itself reaches down to ' \
                   '0x%08X (%d refs below the claimed start): the claim is ' \
                   'the TAIL of the real pool, so the surplus is a SPLIT ' \
                   'BOUNDARY artifact, not a wrong literal' \
                   % (sum(only_ours.values()), hint[0], hint[1])
        else:
            kind = 'SUPERSET'
            note = '  -- every retail word occurs in ours; we emit %d EXTRA ' \
                   '(%s). Over-claim or surplus literals, not a wrong value' \
                   % (sum(only_ours.values()), fmt_vals(only_ours))
    else:
        delta = (shift_hint(ours, dol, base, span)
                 if n <= span and len(ours) >= 0x10 else None)
        if delta is not None:
            kind = 'SHIFT'
            note = '  -- our pool matches retail VERBATIM at %+d (0x%08X): ' \
                   'the CLAIM is misplaced, not the values' \
                   % (delta, base + delta)
        else:
            note = '  -- ours-only %s | retail-only %s' \
                   % (fmt_vals(only_ours), fmt_vals(only_retail))
            extra = surplus_fns(ours_obj, retail_obj)
            if extra:
                note += '. NOTE: we define %d function(s) retail does not ' \
                        '(%s) -- surplus words are theirs before any literal ' \
                        'is suspect' % (len(extra), ', '.join(sorted(extra)))
    lines.append('  [%s] %-8s %d/%d words differ  (0x%08X..0x%08X)%s'
                 % (kind, section, len(bad), words, base, base + len(ours),
                    note))
    if quiet or kind in ('SHIFT', 'SUBSET', 'SUPERSET', 'UNDERCLAIM'):
        return lines
    for off, a, b in bad:
        addr = base + off
        av = '%.9g' % f32(a) if len(a) == 4 else ''
        bv = '%.9g' % f32(b) if len(b) == 4 else ''
        lines.append('    +0x%03x  0x%08X  %-14s ours=%s (%s)  retail=%s (%s)'
                     % (off, addr, disp(addr), a.hex(), av, b.hex(), bv))
    return lines


def main():
    argv = sys.argv[1:]
    quiet = '--quiet' in argv
    sections = list(DEFAULT_SECTIONS)
    if '--all-sections' in argv:
        sections = list(ALL_SECTIONS)
    filters = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--quiet', '--all-sections'):
            pass
        elif a == '--sections':
            i += 1
            sections = [s if s.startswith('.') else '.' + s
                        for s in argv[i].split(',')]
        elif a.startswith('--sections='):
            sections = [s if s.startswith('.') else '.' + s
                        for s in a.split('=', 1)[1].split(',')]
        else:
            filters.append(a)
        i += 1

    dol = Dol(DOL)
    splits = parse_splits(SPLITS)
    units = json.load(open(os.path.join(ROOT, 'objdiff.json')))['units']
    require_nonempty(filters,
                     [u.get('name', '') + ' '
                      + ((u.get('metadata') or {}).get('source_path') or '')
                       for u in units])

    scanned = bad_units = 0
    bad_names = []
    for u in units:
        name = u.get('name', '')
        src = (u.get('metadata') or {}).get('source_path', '')
        if filters and not any(f in name or f in src for f in filters):
            continue
        ours_p = u.get('base_path')
        if not ours_p or not src:
            continue
        ours_p = os.path.join(ROOT, ours_p)
        if not os.path.exists(ours_p):
            continue
        retail_p = u.get('target_path')
        retail_p = os.path.join(ROOT, retail_p) if retail_p else None
        key = src[4:] if src.startswith('src/') else src
        claims = splits.get(key)
        if not claims:
            continue
        scanned += 1
        report = []
        for section in sections:
            if section not in claims:
                continue
            data = section_bytes(ours_p, section)
            if not data:
                continue
            if has_relocs(ours_p, section):
                report.append('  [skip] %s carries relocations' % section)
                continue
            report += compare(name, section, data, claims[section], dol, quiet,
                              ours_obj=ours_p, retail_obj=retail_p)
        if any(k in ln for ln in report for k in
                   ('[BAD]', '[SHIFT]', '[PERM]', '[SUBSET]', '[SUPERSET]',
                    '[UNDERCLAIM]')):
            bad_units += 1
            bad_names.append(name)
        if report:
            print('\n=== %s' % name)
            print('\n'.join(report))

    print('\nscanned=%d content-mismatch=%d  sections=%s'
          % (scanned, bad_units, ','.join(sections)))
    for n in bad_names:
        print('  MISMATCH %s' % n)
    return 1 if bad_units else 0


if __name__ == '__main__':
    sys.exit(main())
