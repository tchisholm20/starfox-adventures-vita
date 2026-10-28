#!/usr/bin/env python3
"""WIDTH-AWARE reloc-target value check: catch a wrong constant that a 4-byte
comparison cannot see.

Sibling of reloc_target_audit.py, which deliberately resolves every relocation
target to a FIXED 4 bytes. That choice buys immunity to the two objects
disagreeing about a datum's declared st_size, and it pays for it with two blind
spots that are exactly where a value-level defect hides:

  * AN 8-BYTE DATUM IS TRUNCATED. `lfd` loads a double; comparing its leading 4
    bytes passes any pair whose HIGH words agree. (double)(float)0.3 and the
    true 0.3 differ only from bit 32 down -- 0x3fd3333340000000 against
    0x3fd3333333333333 -- so a float literal transcribed into a double context,
    or a double rounded through a float, reads as a perfect match. This is the
    near neighbour of the f64-half bug (f64_half_check.py) and needs the low
    word to be seen.
  * A 1- OR 2-BYTE DATUM IS OVER-READ. A `lbz`/`lhz` off a pool symbol compares
    the datum plus whatever follows it, so a difference in the NEIGHBOUR is
    reported as a difference in the datum.

So the width comes from the INSTRUCTION, which always knows it: the primary
opcode of the relocated load/store gives 1, 2, 4 or 8. Address-forming
relocations (addi/addis/ori building a pointer) are excluded -- their target is
an array or a string, not a scalar, and reloc_target_audit already covers them.

Comparison is by MULTISET of (width, value-bytes) per function, order-insensitive
for the reason reloc_target_audit documents: register allocation and scheduling
permute reloc order without changing WHICH values a function touches, so a
positional pairing manufactures findings out of pure drift.

  MISSING  retail loads a value we never load
  EXTRA    we load a value retail never loads
A finding is normally a MISSING/EXTRA pair; the classifier names the relation
when it recognises one (SIGN, F64-LOW, F32-IN-F64, INT-AS-FLOAT).

Note on reloc offsets, which are NOT uniform: R_PPC_EMB_SDA21 (109) is recorded
at the INSTRUCTION word, while ADDR16_HA/LO are recorded at the 16-bit immediate
FIELD, i.e. instruction+2. Reading the opcode at the raw offset therefore
straddles two instructions half the time and yields nonsense; every opcode read
here is aligned down to the word first.

Four filters. Each census below is measured on the tree this was written
against, and the matching --no-* flag re-derives it:

  * ASYMMETRIC DECLINE, the one that matters. When the two sides decline to
    resolve a DIFFERENT NUMBER of relocations, one side's unresolved datum has
    silently left its multiset and its partner surfaces alone as a MISSING or an
    EXTRA. Skip the function outright; an equal decline count cannot produce that
    pairing. 3 phantoms, all the same shape: model.c's vertex transforms report
    an EXTRA f64 0x4330000080000000, which is not a constant of the program at
    all but the int-to-float conversion magic, resolved by a different route on
    each side.
  * SKIP complete=True UNITS -- the DOL sha1 proves them byte-identical, so any
    finding there is false by construction. 0 phantoms here, which is luck and
    not a reason to drop it: the mechanism that produces them is a call retail
    encodes as an already-resolved direct branch carrying NO relocation, and a
    guard that counts relocations which exist and fail cannot see one that is
    absent.
  * DATA THAT CARRIES ITS OWN RELOCATIONS (jump tables, pointer arrays) reads as
    zeros in our unlinked object and as resolved addresses in retail's carved
    copy. 0 phantoms at these widths; kept because the failure it prevents is
    silent and total.
  * COUNT-ONLY CHANGES are reported only under --counts: CSE folding two loads
    of 0.5 into one changes a count without changing any value.

Ground truth is orig/GSAE01/sys/main.dol plus the retail objects. report.json is
consulted only for the `complete` flag and for printing a unit's score.

usage: python3 tools/reloc_width_check.py [unit-substring ...] [--counts]
                 [--all-widths] [--no-complete-filter] [--no-asym-guard]
                 [--no-relocdata-guard]
       --all-widths  also compare 4-byte loads (reloc_target_audit's domain);
                     by default only widths 1, 2 and 8 are reported, which is
                     the coverage that tool does not have.
exit status: 1 if any finding, else 0.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_guard import require_nonempty
import sda_reloc_check as S
from sda_reloc_check import ROOT, LBL_RE, IMM16, Obj, load_retail_addrs

R_ADDR32 = 1
VALUE_RELOCS = IMM16 | {R_ADDR32}

# primary opcode -> bytes touched by the memory access
WIDTH = {}
for _op in (34, 35, 38, 39):                      # lbz lbzu stb stbu
    WIDTH[_op] = 1
for _op in (40, 41, 42, 43, 44, 45):              # lhz lhzu lha lhau sth sthu
    WIDTH[_op] = 2
for _op in (32, 33, 36, 37, 48, 49, 52, 53):      # lwz lwzu stw stwu lfs lfsu stfs stfsu
    WIDTH[_op] = 4
for _op in (50, 51, 54, 55):                      # lfd lfdu stfd stfdu
    WIDTH[_op] = 8


def access_width(text, off):
    """Bytes the relocated instruction reads, or None if not a scalar access.

    The instruction is at the word containing `off`: SDA21 records the word,
    ADDR16_HA/LO record the immediate field two bytes into it.
    """
    w = off & ~3
    if w + 4 > len(text):
        return None
    return WIDTH.get(struct.unpack(">I", text[w:w + 4])[0] >> 26)


GUARDS = {"relocdata": True}


def resolve(o, rel, RA, is_retail, text):
    """(width, value-bytes) for one relocated scalar access.

    Returns None when the reloc is not a scalar access at all (no finding, no
    decline) and False when it is one whose value cannot be read (a decline,
    which the caller counts for the asymmetry guard).
    """
    off, typ, nm, add, shndx, val, sz = rel
    if typ not in VALUE_RELOCS or nm.startswith("jumptable_"):
        return None
    n = 4 if typ == R_ADDR32 else access_width(text, off)
    if n is None:
        return None
    local = shndx and shndx < 0xFF00
    # Pointer tables read as zeros in our unlinked object, as addresses in
    # retail's carved copy: never value-comparable.
    if local and GUARDS["relocdata"] and o.has_reloc(shndx, val + add, n):
        return False
    v = None
    if local and not is_retail:
        v = o.value_of(shndx, val, add, n)
    else:
        a = RA.get(nm)
        if a is None:
            g = LBL_RE.match(nm)
            a = int(g.group(1), 16) if g else None
        if a is not None:
            v = S.dol_read(a + add, n)
        if v is None and local:
            v = o.value_of(shndx, val, add, n)
    if v is None or len(v) != n:
        return False
    return (n, v)


def show(n, v):
    if n == 8:
        return "f64 %r (0x%016x)" % (struct.unpack(">d", v)[0], struct.unpack(">Q", v)[0])
    if n == 4:
        u = struct.unpack(">I", v)[0]
        return "f32 %.9g / int %d (0x%08x)" % (struct.unpack(">f", v)[0], u, u)
    if n == 2:
        return "u16 %d (0x%04x)" % (struct.unpack(">H", v)[0], struct.unpack(">H", v)[0])
    return "u8 %d (0x%02x)" % (v[0], v[0])


def classify(extra, missing):
    """Name the relation between an EXTRA and a MISSING of the same width."""
    out = []
    for no, vo in extra:
        for nt, vt in missing:
            if no != nt:
                continue
            io = int.from_bytes(vo, "big")
            it = int.from_bytes(vt, "big")
            sign = 1 << (no * 8 - 1)
            if io ^ it == sign:
                out.append("SIGN: ours %s vs retail %s" % (show(no, vo), show(nt, vt)))
            elif no == 8 and (io >> 32) == (it >> 32):
                tag = "F32-IN-F64" if (io & 0x1FFFFFFF) == 0 or (it & 0x1FFFFFFF) == 0 \
                      else "F64-LOW"
                out.append("%s: ours %s vs retail %s" % (tag, show(no, vo), show(nt, vt)))
            elif no == 4:
                fo = struct.unpack(">f", vo)[0]
                ft = struct.unpack(">f", vt)[0]
                if it and abs(fo - it) < 1e-9 * max(1.0, abs(it)):
                    out.append("INT-AS-FLOAT: ours f32 %g vs retail int %d" % (fo, it))
                elif io and abs(ft - io) < 1e-9 * max(1.0, abs(io)):
                    out.append("FLOAT-AS-INT: ours int %d vs retail f32 %g" % (io, ft))
    return out


def main():
    counts = "--counts" in sys.argv
    all_widths = "--all-widths" in sys.argv
    no_complete = "--no-complete-filter" in sys.argv
    no_asym = "--no-asym-guard" in sys.argv
    GUARDS["relocdata"] = "--no-relocdata-guard" not in sys.argv
    pats = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep = {1, 2, 4, 8} if all_widths else {1, 2, 8}

    RA = load_retail_addrs()
    rep = json.load(open(os.path.join(ROOT, "build/GSAE01/report.json")))
    complete = {u["name"]: u["metadata"].get("complete") for u in rep["units"]}
    pct = {u["name"]: u["measures"].get("fuzzy_match_percent", 0) for u in rep["units"]}
    units = [u for u in json.load(open(os.path.join(ROOT, "objdiff.json")))["units"]
             if "base_path" in u and "target_path" in u]
    require_nonempty(pats, [u["name"] for u in units])

    findings = []
    n_units = n_fns = n_skip_asym = n_skip_complete = 0
    for u in units:
        name = u["name"]
        if pats and not any(p in name for p in pats):
            continue
        if complete.get(name) and not no_complete:
            n_skip_complete += 1
            continue
        po = os.path.join(ROOT, u["base_path"])
        pr = os.path.join(ROOT, u["target_path"])
        if not (os.path.exists(po) and os.path.exists(pr)):
            continue
        try:
            O, T = Obj(po), Obj(pr)
        except Exception as ex:
            sys.stderr.write("[skip %s: %s]\n" % (name, ex))
            continue
        if O.text_i is None or T.text_i is None:
            continue
        n_units += 1
        to, tt = O.text(), T.text()
        ro, rt = O.text_relocs(), T.text_relocs()
        fo, ft = O.fns(), T.fns()
        for fn in sorted(set(fo) & set(ft)):
            (so, szo), (sr, szr) = fo[fn], ft[fn]
            mo, mt, do, dt = {}, {}, 0, 0
            for r in ro:
                if not (so <= r[0] < so + szo):
                    continue
                k = resolve(O, r, RA, False, to)
                if k is False:
                    do += 1
                elif k:
                    mo[k] = mo.get(k, 0) + 1
            for r in rt:
                if not (sr <= r[0] < sr + szr):
                    continue
                k = resolve(T, r, RA, True, tt)
                if k is False:
                    dt += 1
                elif k:
                    mt[k] = mt.get(k, 0) + 1
            if do != dt and not no_asym:
                n_skip_asym += 1
                continue
            n_fns += 1
            extra = [k for k in mo if k[0] in keep and (mo[k] > mt.get(k, 0))]
            missing = [k for k in mt if k[0] in keep and (mt[k] > mo.get(k, 0))]
            hard_e = [k for k in extra if k not in mt]
            hard_m = [k for k in missing if k not in mo]
            if not (hard_e or hard_m) and not (counts and (extra or missing)):
                continue
            findings.append(dict(unit=name, pct=pct.get(name, 0), fn=fn,
                                 extra=[(k, mo[k], mt.get(k, 0)) for k in extra],
                                 missing=[(k, mo.get(k, 0), mt[k]) for k in missing],
                                 rel=classify(hard_e, hard_m)))

    for f in findings:
        print("*** %s  %s  (%.2f%%)" % (f["unit"], f["fn"], f["pct"]))
        for (n, v), a, b in f["extra"]:
            print("      EXTRA   %-42s ours x%d retail x%d" % (show(n, v), a, b))
        for (n, v), a, b in f["missing"]:
            print("      MISSING %-42s ours x%d retail x%d" % (show(n, v), a, b))
        for r in f["rel"]:
            print("      -> %s" % r)
    print("[incomplete units: %d] [fns compared: %d] "
          "[skipped: %d complete units, %d asymmetric-decline fns] [findings: %d]"
          % (n_units, n_fns, n_skip_complete, n_skip_asym, len(findings)))
    return 1 if findings else 0


sys.exit(main())
