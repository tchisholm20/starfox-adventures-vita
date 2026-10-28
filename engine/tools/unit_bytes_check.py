#!/usr/bin/env python3
"""Certify a change to a NonMatching unit, which both standing gates are blind to.

THE GAP. A unit marked NonMatching has its source object EXCLUDED from the link:
main.dol is built from the retail target object instead. So main.dol's sha1 --
the project's strongest gate -- cannot see any change to that unit at all, and
reports OK no matter what the edit did. objdiff is the other gate, and it
NORMALISES the constant-pool relocation: an `lfs f1, -0x49f4(r2)` against pool
word A and an `lfs f1, -0x49e8(r2)` against pool word B compare EQUAL once the
displacement is resolved through the symbol, so fuzzy_match_percent holds to the
digit while .sdata2 underneath has been rewritten. A lightmap.c probe once
reported `main.dol: OK` AND an identical report.json while it had in fact
mutated .sdata2. Both gates were false negatives on the same change.

Neither gate is wrong; they are measuring the linked image and the paired
function text. Nothing was measuring the object. For a NonMatching unit the only
certification is a direct byte comparison of the object itself, and that needs
four things, because each one hides a different class of change:

  .text md5        instruction bytes -- the thing objdiff scores, but unpaired,
                   so a function that changed NAME or was added/removed shows.
  .sdata2 bytes    the constant pool, verbatim and word-addressed. This is the
                   leg both standing gates lack. Printed as a word diff.
  section sizes    every section, including .bss/.sbss, which carry no bytes at
                   all and so are invisible to any content comparison.
  relocations      offset/type/RESOLVED TARGET per section. Catches a pool word
                   that kept its value but moved slot, and a call that changed
                   target without changing the instruction encoding.

Relocations are compared by resolved target, never by symbol NAME. A defined
symbol reduces to `section+value`, an undefined one to its name. This matters in
both directions and a name comparison gets both wrong: purging a pool-
reconstruction const renames `lbl_803DEBCC` to `@1709` at the same slot with the
same value, which a name diff calls a change when nothing moved; and retail's
split objects name every pool word `lbl_`/`@NNN` where we name them ourselves,
which would make every unit differ. Conversely a reference that silently moved
to a DIFFERENT word keeps neither name nor slot, and only the resolved form
shows it.

TWO MODES, for two different questions.

  --save / default  SELF vs SELF across an edit. `--save` snapshots the four
                    artifacts; the default rebuilds and diffs against that
                    snapshot. This is the per-change gate: run --save before
                    touching the file, run the default after, and a clean exit 0
                    is the certification main.dol and report.json could not give.

  --vs-retail       OURS vs RETAIL. Compares the built object against the retail
                    target object in build/GSAE01/obj/, which carries the exact
                    section content the split assigned to this unit. This needs
                    no baseline and is the audit instrument: it answers "does
                    this unit's pool agree with retail RIGHT NOW", for units the
                    link has never checked.

FALSE-POSITIVE CLASSES, all filtered by default (--no-filter to see them):

  complete=True units are certified by main.dol's sha1 already; a --vs-retail
  finding on one is false by construction and is dropped.

  Retail encodes some calls as an already-resolved direct branch carrying NO
  relocation where we emit a relocated `bl`. That is an ABSENT reloc, and a
  check that only inspects relocations which exist cannot see it -- so the
  per-section reloc COUNT is compared before the entries are.

  A section absent on ONE side only is reported as a size finding, never as a
  content finding, so an empty pool does not masquerade as a rewritten one.

  .comment and .note.split are toolchain provenance, never compared.

UNDER-CLAIM is a real verdict, not a defect. Our .sdata2 legitimately runs
LONGER than retail's when the split's claimed range is too short -- the words
past retail's end belong to the unit but were assigned to an auto_* blob. The
comparison is therefore run over the OVERLAP and the tail is reported
separately, so a short claim does not present as a rewritten pool.

usage:
  python3 tools/unit_bytes_check.py --save main/objhits.c     # before an edit
  python3 tools/unit_bytes_check.py main/objhits.c            # after; 1 if changed
  python3 tools/unit_bytes_check.py --vs-retail               # audit every unit
  python3 tools/unit_bytes_check.py --vs-retail track/        # audit a subtree
exit status 1 if any scanned unit differs.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build/GSAE01')
CONFIG = os.path.join(BUILD, 'config.json')
REPORT = os.path.join(BUILD, 'report.json')
SNAPDIR = os.path.join(BUILD, '.unitbytes')

# Toolchain provenance, not content. Never compared.
IGNORED_SECTIONS = ('.comment', '.note.split', '.shstrtab', '.strtab',
                    '.symtab', '')

SHT_NOBITS = 8
SHT_RELA = 4
SHT_SYMTAB = 2


# ---------------------------------------------------------------- ELF reading

class Elf(object):
    """Minimal ELF32-BE reader. Enough for sections, symbols and RELA."""

    def __init__(self, path):
        self.path = path
        blob = open(path, 'rb').read()
        self.blob = blob
        if blob[:4] != b'\x7fELF' or blob[4] != 1 or blob[5] != 2:
            raise ValueError('%s: not ELF32 big-endian' % path)
        shoff, = struct.unpack_from('>I', blob, 0x20)
        shent, shnum, shstr = struct.unpack_from('>HHH', blob, 0x2E)
        self.sections = []
        raw = []
        for i in range(shnum):
            o = shoff + i * shent
            nm, ty, fl, ad, off, sz, lk, inf = struct.unpack_from('>8I', blob, o)
            raw.append(dict(nameoff=nm, type=ty, offset=off, size=sz,
                            link=lk, info=inf, index=i))
        strbase = raw[shstr]['offset']
        for s in raw:
            s['name'] = self._cstr(strbase + s['nameoff'])
            self.sections.append(s)
        self._symcache = {}

    def _cstr(self, off):
        end = self.blob.index(b'\0', off)
        return self.blob[off:end].decode('utf-8', 'replace')

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def data(self, sec):
        """Raw bytes of a section. NOBITS sections hold no file bytes."""
        if sec is None or sec['type'] == SHT_NOBITS:
            return b''
        return self.blob[sec['offset']:sec['offset'] + sec['size']]

    def section_sizes(self):
        return {s['name']: s['size'] for s in self.sections
                if s['name'] not in IGNORED_SECTIONS
                and not s['name'].startswith('.rela')}

    def symbols(self, symtab_index):
        if symtab_index in self._symcache:
            return self._symcache[symtab_index]
        sec = self.sections[symtab_index]
        strbase = self.sections[sec['link']]['offset']
        out = []
        for o in range(sec['offset'], sec['offset'] + sec['size'], 16):
            nm, val, sz, info, other, shndx = struct.unpack_from(
                '>IIIBBH', self.blob, o)
            name = self._cstr(strbase + nm)
            if not name and shndx < len(self.sections):
                # A section symbol carries the section's name.
                name = self.sections[shndx]['name']
            out.append(dict(name=name, value=val, bind=info >> 4,
                            shndx=shndx))
        self._symcache[symtab_index] = out
        return out

    def relocs(self):
        """{target section: [(offset, type, resolved, addend, symname), ...]}

        `resolved` is the reloc's target reduced past its NAME: a defined symbol
        becomes `section+value`, an undefined one keeps its name because that is
        all the object knows about it. Comparing this instead of the name is what
        makes a pure rename read as identical and a genuine retarget read as a
        difference.
        """
        out = {}
        for s in self.sections:
            if s['type'] != SHT_RELA:
                continue
            target = self.sections[s['info']]['name']
            if target in IGNORED_SECTIONS:
                continue
            syms = self.symbols(s['link'])
            entries = []
            for o in range(s['offset'], s['offset'] + s['size'], 12):
                roff, info, addend = struct.unpack_from('>IIi', self.blob, o)
                symidx, rtype = info >> 8, info & 0xFF
                if symidx < len(syms):
                    sym = syms[symidx]
                    sname = sym['name']
                    if sym['shndx'] and sym['shndx'] < len(self.sections):
                        resolved = '%s+0x%x' % (
                            self.sections[sym['shndx']]['name'], sym['value'])
                    else:
                        resolved = 'U:' + sname
                else:
                    sname = resolved = '?%d' % symidx
                entries.append((roff, rtype, resolved, addend, sname))
            entries.sort()
            out[target] = entries
        return out


# ------------------------------------------------------------- unit registry

def load_units():
    """[{name, source, our_obj, retail_obj, complete, fuzzy}] for every unit."""
    cfg = json.load(open(CONFIG))
    rep = {}
    if os.path.exists(REPORT):
        for u in json.load(open(REPORT))['units']:
            md = u.get('metadata') or {}
            if md.get('source_path'):
                rep[md['source_path']] = (
                    bool(md.get('complete')),
                    u['measures'].get('fuzzy_match_percent'))
    units = []
    for u in cfg['units']:
        if u.get('autogenerated'):
            continue
        retail = os.path.join(ROOT, u['object'])
        ours = retail.replace('/GSAE01/obj/', '/GSAE01/src/')
        # config.json carries no source path; recover it from the object path.
        src = 'src/' + os.path.relpath(retail, os.path.join(BUILD, 'obj'))
        stem = os.path.splitext(src)[0]
        source = None
        for ext in ('.c', '.cpp', '.s'):
            if os.path.exists(os.path.join(ROOT, stem + ext)):
                source = stem + ext
                break
        complete, fuzzy = rep.get(source, (None, None))
        units.append(dict(name=u['name'], source=source, our_obj=ours,
                          retail_obj=retail, complete=complete, fuzzy=fuzzy))
    return units


def select(units, patterns):
    if not patterns:
        return units
    out = []
    for u in units:
        hay = '%s %s' % (u['name'], u['source'] or '')
        if any(p in hay for p in patterns):
            out.append(u)
    return out


# ------------------------------------------------------------------ snapshot

def snapshot(obj_path):
    """The four artifacts, as a plain JSON-able dict."""
    e = Elf(obj_path)
    text = e.data(e.section('.text'))
    sd2 = e.data(e.section('.sdata2'))
    relocs = {}
    names = {}
    for tgt, entries in e.relocs().items():
        relocs[tgt] = ['%08x %d %s %+d' % (o, t, r, a)
                       for o, t, r, a, _ in entries]
        names[tgt] = ['%08x %s' % (o, n) for o, _, _, _, n in entries]
    return dict(
        text_md5=hashlib.md5(text).hexdigest(),
        text_size=len(text),
        sdata2=sd2.hex(),
        sections=e.section_sizes(),
        relocs=relocs,
        reloc_names=names,
    )


def snap_path(unit):
    key = unit['name'].replace('/', '__')
    return os.path.join(SNAPDIR, key + '.json')


# ----------------------------------------------------------------- reporting

def word_diff(a_hex, b_hex, a_label, b_label, limit=12):
    """Word-by-word diff of two pools, over their common prefix length."""
    a = bytes.fromhex(a_hex)
    b = bytes.fromhex(b_hex)
    n = min(len(a), len(b))
    lines = []
    for i in range(0, n - n % 4, 4):
        wa = a[i:i + 4]
        wb = b[i:i + 4]
        if wa != wb:
            fa = struct.unpack('>f', wa)[0]
            fb = struct.unpack('>f', wb)[0]
            lines.append('      +0x%03x  %s %-12s != %s %-12s  (%s | %s)'
                         % (i, wa.hex(), '%g' % fa, wb.hex(), '%g' % fb,
                            a_label, b_label))
        if len(lines) >= limit:
            lines.append('      ... (truncated)')
            break
    return lines


def compare(cur, base, cur_label, base_label, strict_symbols=False,
            overlap_pool=False):
    """List of human-readable findings. Empty list means identical.

    A finding tagged NOTE is not a byte difference and does not fail the gate;
    everything else does.
    """
    out = []

    if cur['sections'] != base['sections']:
        keys = sorted(set(cur['sections']) | set(base['sections']))
        for k in keys:
            a = cur['sections'].get(k)
            b = base['sections'].get(k)
            if a != b:
                out.append('    SECTION-SIZE %-12s %s=%s %s=%s' % (
                    k,
                    cur_label, 'absent' if a is None else '0x%x' % a,
                    base_label, 'absent' if b is None else '0x%x' % b))

    if cur['text_md5'] != base['text_md5']:
        out.append('    TEXT-DIFF     md5 %s=%s %s=%s (size 0x%x vs 0x%x)' % (
            cur_label, cur['text_md5'][:12], base_label, base['text_md5'][:12],
            cur['text_size'], base['text_size']))

    a, b = cur['sdata2'], base['sdata2']
    if a != b:
        if overlap_pool and not b:
            # The split gave this unit no .sdata2 at all: its pool was assigned
            # to an auto_* blob. There is no retail claim to disagree with, so
            # this is the shape of the blind class, not a defect in itself.
            out.append('    NOTE POOL-UNCLAIMED  retail assigns this unit no '
                       '.sdata2 (pool lives in an auto_* blob); ours emits '
                       '0x%x bytes -- unverifiable from the split' % (len(a) // 2))
        elif overlap_pool and len(a) != len(b):
            n = min(len(a), len(b))
            n -= n % 8
            if a[:n] == b[:n]:
                longer = cur_label if len(a) > len(b) else base_label
                out.append('    POOL-TAIL     overlap 0x%x bytes identical; '
                           '%s carries 0x%x more' % (
                               n // 2, longer, abs(len(a) - len(b)) // 2))
            else:
                out.append('    POOL-DIFF     within the shared 0x%x bytes' % (n // 2))
                out.extend(word_diff(a, b, cur_label, base_label))
        else:
            out.append('    POOL-DIFF     %s=0x%x bytes %s=0x%x bytes' % (
                cur_label, len(a) // 2, base_label, len(b) // 2))
            out.extend(word_diff(a, b, cur_label, base_label))

    for tgt in sorted(set(cur['relocs']) | set(base['relocs'])):
        ca = cur['relocs'].get(tgt, [])
        cb = base['relocs'].get(tgt, [])
        if len(ca) != len(cb):
            # An absent relocation is a real difference and shows up here only.
            out.append('    RELOC-COUNT   %s %s=%d %s=%d' % (
                tgt, cur_label, len(ca), base_label, len(cb)))
        if ca != cb:
            shown = 0
            for x, y in zip(ca, cb):
                if x != y:
                    out.append('    RELOC-DIFF    %s  %s=%s  %s=%s' % (
                        tgt, cur_label, x, base_label, y))
                    shown += 1
                    if shown >= 6:
                        out.append('    ... (truncated)')
                        break
        if strict_symbols:
            na = cur.get('reloc_names', {}).get(tgt, [])
            nb = base.get('reloc_names', {}).get(tgt, [])
            if ca == cb and na != nb:
                n = sum(1 for x, y in zip(na, nb) if x != y)
                out.append('    NOTE RELOC-RENAME  %s  %d reloc(s) reach the '
                           'same slot under a different symbol name' % (tgt, n))
    return out


def is_failure(findings):
    """A NOTE is informational; anything else is a byte difference."""
    return any(not f.lstrip().startswith('NOTE') for f in findings)


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('patterns', nargs='*')
    ap.add_argument('--save', action='store_true',
                    help='snapshot the four artifacts as the baseline')
    ap.add_argument('--vs-retail', action='store_true',
                    help='compare against the retail target object')
    ap.add_argument('--strict-symbols', action='store_true',
                    help='also note reloc symbol RENAMES that reach the same '
                         'slot (informational; never fails the gate)')
    ap.add_argument('--no-filter', action='store_true',
                    help='do not drop findings on complete=True units')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('-h', '--help', action='store_true')
    args = ap.parse_args()
    if args.help:
        print(__doc__)
        return 0

    units = select(load_units(), args.patterns)
    if not units:
        print('no units matched %r' % (args.patterns,), file=sys.stderr)
        return 2

    if args.save:
        os.makedirs(SNAPDIR, exist_ok=True)
        n = 0
        for u in units:
            if not os.path.exists(u['our_obj']):
                continue
            json.dump(snapshot(u['our_obj']), open(snap_path(u), 'w'))
            n += 1
        print('snapshotted %d/%d unit(s) to %s'
              % (n, len(units), os.path.relpath(SNAPDIR, ROOT)))
        return 0

    scanned = differing = skipped = filtered = noted = 0
    for u in units:
        if not os.path.exists(u['our_obj']):
            skipped += 1
            continue
        cur = snapshot(u['our_obj'])

        if args.vs_retail:
            if not os.path.exists(u['retail_obj']):
                skipped += 1
                continue
            if u['complete'] and not args.no_filter:
                # main.dol's sha1 already certifies this unit.
                filtered += 1
                continue
            base = snapshot(u['retail_obj'])
            findings = compare(cur, base, 'ours', 'retail',
                               strict_symbols=args.strict_symbols,
                               overlap_pool=True)
        else:
            p = snap_path(u)
            if not os.path.exists(p):
                skipped += 1
                continue
            base = json.load(open(p))
            findings = compare(cur, base, 'now', 'base',
                               strict_symbols=args.strict_symbols)

        scanned += 1
        if findings:
            failed = is_failure(findings)
            if failed:
                differing += 1
            else:
                noted += 1
            print('%s  (complete=%s fuzzy=%s) %s' % (
                u['name'], u['complete'],
                '%.4f' % u['fuzzy'] if u['fuzzy'] is not None else '?',
                'MUTATED' if failed else 'note only'))
            if not args.quiet:
                for f in findings:
                    print(f)

    print('\n%d unit(s) scanned, %d differ, %d note-only, %d skipped '
          '(no object/baseline)%s'
          % (scanned, differing, noted, skipped,
             ', %d filtered (complete=True)' % filtered if filtered else ''))
    return 1 if differing else 0


if __name__ == '__main__':
    sys.exit(main())
