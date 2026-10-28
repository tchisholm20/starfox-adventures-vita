#!/usr/bin/env python3
"""Detect the f64-HALF TRANSCRIPTION bug: an f32 literal holding the high word
of retail's 8-byte double.

This is a live CORRECTNESS defect, not a match defect. Retail's datum is an
8-byte f64; our source holds that double's HIGH WORD reinterpreted as an f32.
Both sides then emit `lfd` against a same-sized pool slot, so the instruction
encoding is identical and every existing gate stays green while the value the
game computes is wrong:

  * objdiff/fuzzy_match_percent normalises `@N` relocation TARGETS, so a wrong
    constant behind a right displacement reads as a perfect match.
  * The main.dol sha1 is blind while the unit is INCOMPLETE, because the link
    consumes the retail object. The defect appears the instant it is promoted.
  * pool_content_check only reads the DOL's own .sdata2, so a DLL's pool -- and
    every case found so far has been in a DLL -- is out of its reach.

The relation, exactly: our source literal L is retail's f64 with its low 32
bits discarded, so bits(L) == (retail_f64 >> 32).  MWCC then widens L back to a
double for the `lfd`, giving us (double)(float)L.  Recovering L from our own
emitted datum and testing it against retail's high word is therefore a closed
identity -- no tolerance on the exponent, no heuristic about "round-looking"
numbers.

  ours   0x400e880000000000 = 3.81640625        source wrote 3.8164062f
  retail 0x4074400000000000 = 324.0             high word 0x40744000 -> 3.8164062f

Three things this must get right, because each one alone manufactures a census
of phantom defects:

  * READ THE OPCODE AT THE INSTRUCTION WORD, not at the raw reloc offset (see
    f64_operands); half the IMM16 relocations are recorded at the immediate
    field, and reading four bytes from there straddles two instructions.
  * GATE ON `lfd`. Reading 8 bytes at every reloc target also swallows a 4-byte
    float plus whatever pool word happens to follow it, and for powers of two
    the high-word relation is degenerate (an f32 0.5 at 0x3f000000 "matches" a
    double 0.5), which alone produced 34 hits of which 0 were real.
  * OURS MUST BE A WIDENED f32 -- low 29 bits zero, not low 32. Widening moves
    23 mantissa bits into 52, leaving 29 trailing zeros, so three mantissa bits
    land in the low word: (double)(float)1.65 is 0x3ffa666660000000. Testing
    for a zero low WORD silently drops every case whose literal needs those
    bits, which is how the 1.65f/0.3f case hides.
  * SKIP complete=True UNITS. The sha1 proves those byte-identical, so a
    finding there is false by construction.

Ground truth is orig/GSAE01/sys/main.dol plus the retail objects; nothing here
consults report.json except for the `complete` flag.

usage: python3 tools/f64_half_check.py [unit-substring ...]
exit status: 1 if any finding, else 0.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_guard import require_nonempty
import sda_reloc_check as S
from sda_reloc_check import ROOT, LBL_RE, Obj, load_retail_addrs

LFD = {50, 51}  # primary opcode of lfd / lfdu -- the only 8-byte float loads
WIDENED_F32 = 0x1FFFFFFF  # (double)(float)x has its low 29 bits clear


def f64_operands(path, RA):
    """Multiset of the 8-byte data this object's .text loads with lfd."""
    o = Obj(path)
    if o.text_i is None:
        return None
    txt = o.text()
    out = {}
    for off, typ, nm, add, shndx, val, sz in o.text_relocs():
        # Reloc offsets are not uniform: R_PPC_EMB_SDA21 is recorded at the
        # instruction WORD, ADDR16_HA/LO at the 16-bit immediate FIELD two bytes
        # into it. Reading the opcode at the raw offset straddles two
        # instructions for the latter and yields nonsense, which silently
        # dropped 292 lfd sites -- every double reached through a lis/lfd pair
        # rather than through the SDA base.
        w = off & ~3
        if w + 4 > len(txt):
            continue
        if (struct.unpack(">I", txt[w:w + 4])[0] >> 26) not in LFD:
            continue
        a = RA.get(nm)
        if a is None:
            g = LBL_RE.match(nm)
            a = int(g.group(1), 16) if g else None
        b = S.dol_read(a + add, 8) if a is not None else None
        if b is None and shndx and shndx < 0xFF00:
            b = o.value_of(shndx, val, add, 8)
        if b is None or len(b) != 8:
            continue
        q = struct.unpack(">Q", b)[0]
        out[q] = out.get(q, 0) + 1
    return out


def main():
    pats = [a for a in sys.argv[1:] if not a.startswith("--")]
    RA = load_retail_addrs()
    rep = json.load(open(os.path.join(ROOT, "build/GSAE01/report.json")))
    complete = {u["name"]: u["metadata"].get("complete") for u in rep["units"]}
    units = [u for u in json.load(open(os.path.join(ROOT, "objdiff.json")))["units"]
             if "base_path" in u and "target_path" in u]
    require_nonempty(pats, [u["name"] for u in units])

    findings, scanned = [], 0
    for u in units:
        name = u["name"]
        if pats and not any(p in name for p in pats):
            continue
        if complete.get(name):  # sha1 proves these byte-identical
            continue
        po = os.path.join(ROOT, u["base_path"])
        pr = os.path.join(ROOT, u["target_path"])
        if not (os.path.exists(po) and os.path.exists(pr)):
            continue
        try:
            ours, retail = f64_operands(po, RA), f64_operands(pr, RA)
        except Exception:
            continue
        if not ours or not retail:
            continue
        scanned += 1
        for qo in ours:
            if qo & WIDENED_F32:
                continue  # not a widened f32: cannot be a transcribed half
            if qo in retail:
                continue  # retail loads the very same datum
            vo = struct.unpack(">d", struct.pack(">Q", qo))[0]
            for qt in retail:
                lit = struct.unpack(">f", struct.pack(">I", qt >> 32))[0]
                if lit == 0.0 or abs(vo - lit) > 1e-6 * abs(lit):
                    continue
                vt = struct.unpack(">d", struct.pack(">Q", qt))[0]
                findings.append((name, qo, vo, qt, vt))

    for name, qo, vo, qt, vt in findings:
        print("*** %s" % name)
        print("      ours   0x%016x = %-22r  (source literal %.9gf)" % (qo, vo, vo))
        print("      retail 0x%016x = %r" % (qt, vt))
    print("[incomplete units with f64 operands: %d] [findings: %d]" % (scanned, len(findings)))
    return 1 if findings else 0


sys.exit(main())
