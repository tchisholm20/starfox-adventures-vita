#!/usr/bin/env python3
"""Localize a data/BSS section-size divergence to the SYMBOL responsible.

section_size_check reports that a unit's `.bss` is 0x20 bytes short. It cannot
say which variable is wrong. This walks the symbols of exactly those sections
whose size diverges and names the ones whose own size disagrees with retail --
the wrong array dimension behind the section total. For `.bss`/`.sbss` that is
the only handle there is: they are NOBITS, so every content-comparing
instrument (objdiff's data diff, pool_content_check, data_reloc_check) is
structurally silent about them and their SIZE is their entire content.

  WHY IT REFUSES TO REPORT A SIZE DIFFERENCE ON ITS OWN

Because a bare st_size difference is NOT evidence, and treating it as evidence
manufactures a confident, wrong bug report. Two independent reasons:

  * The retail sizes come from config/GSAE01/symbols.txt, and for data they are
    largely GAP-DERIVED -- the distance to the next named symbol -- not measured.
  * MWCC aligns a const object to 4, so a 10-byte array and a 12-byte array
    produce byte-identical images: the same data followed by the same zero
    padding, with the next symbol at the same offset either way.

Worked refutation, kept here so the lead is not re-derived: DR_EarthWar's
`gDREarthWarriorLookInitData1` is `const EWPathRange {s16 v[5]}` = 0xa in our
object against symbols.txt's 0xc, and the retail bytes 000a 000a 0000 0000 0000
0000 read equally well as five shorts plus padding or as six shorts. Widening it
to `s16 v[6]` -- the "fix" the raw size difference argues for -- REGRESSED
DR_EarthWarrior_init from 100.00 to 98.81, because the local it is copied into
then needs a longer copy. `s16 v[5]` was right and the 0xc was the gap. The same
shape holds for 332's `gBabyCloudRunnerHomeMoveState` (a `u8` padded to 4) and
for gametext's `gTextBoxes`, where our single 0x1280 array spans retail's
0xC48 `gTextBoxes` plus the 0x638 `lbl_802C8048` that follows it -- a naming and
structure question, byte-identical either way.

So a size difference is reported ONLY when the enclosing section ALSO diverges
in size, which is the case where something really did move. That corroboration
is what makes the finding sound; without it the raw census is 5 findings and all
5 are of the kinds above.

Filters, each suppressing a phantom census rather than a real one:

  * SECTIONS WHOSE SIZE AGREES. The corroboration requirement above. 5 findings.
  * ANONYMOUS `@NNN` POOL SYMBOLS. The compiler mints those per OBJECT, so the
    name is a counter and not an identity: our `@112` and the `@112` in
    symbols.txt are unrelated data that collided on a number. 92 findings, every
    one of them nothing -- long runs of `ours 0x4 retail 0x8` that are one
    object's float against another's double.
  * SIZE 0 ON EITHER SIDE. An extern, a section label or an assembler-emitted
    symbol carries no size; that is missing information, not a defect.
  * FUNCTIONS -- size_parity_check owns those -- and UNDEF/COMMON/ABS symbols.

usage: python3 tools/data_sym_size_check.py [unit-substring ...]
                 [--all] [--no-section-guard]
       --all                include complete=True units
       --no-section-guard   drop the corroboration requirement (re-derives the
                            5-finding phantom census; every one is a false lead)
exit status: 1 if any finding, else 0.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_guard import require_nonempty
import sda_reloc_check as S
from sda_reloc_check import ROOT, Obj

ANON_RE = re.compile(r"^@\d+$")
ALLOC_SECS = (".data", ".rodata", ".sdata", ".sdata2", ".bss", ".sbss", ".sbss2",
              ".data1", ".rodata1")


def map_sizes():
    """symbol -> declared size from symbols.txt; ambiguous names dropped."""
    m, dup = {}, set()
    for line in open(os.path.join(ROOT, "config/GSAE01/symbols.txt")):
        g = re.match(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]{8});(.*)$", line)
        if not g or "type:function" in g.group(4):
            continue
        h = re.search(r"size:0x([0-9A-Fa-f]+)", g.group(4))
        if not h:
            continue
        nm, sz = g.group(1), int(h.group(1), 16)
        if nm in m and m[nm] != sz:
            dup.add(nm)
        m[nm] = sz
    for nm in dup:
        m.pop(nm, None)
    sys.stderr.write("[symbols.txt object sizes: %d unique, %d ambiguous dropped]\n"
                     % (len(m), len(dup)))
    return m


def sec_sizes(o):
    """section name -> total size, summed over repeated same-named sections."""
    out = {}
    for s in o.e.sh:
        if s["name"] in ALLOC_SECS:
            out[s["name"]] = out.get(s["name"], 0) + s["size"]
    return out


def obj_sizes(o):
    """name -> (section, size) for data/bss symbols DEFINED in this object."""
    out = {}
    for nm, shndx, val, sz, bind in o.syms:
        if not nm or not shndx or shndx >= 0xFF00 or not sz or ANON_RE.match(nm):
            continue
        sec = o.e.sh[shndx]["name"]
        if sec in ALLOC_SECS:
            out.setdefault(nm, (sec, sz))
    return out


def main():
    do_all = "--all" in sys.argv
    no_guard = "--no-section-guard" in sys.argv
    pats = [a for a in sys.argv[1:] if not a.startswith("-")]

    MS = map_sizes()
    rep = json.load(open(os.path.join(ROOT, "build/GSAE01/report.json")))
    complete = {u["name"]: u["metadata"].get("complete") for u in rep["units"]}
    units = [u for u in json.load(open(os.path.join(ROOT, "objdiff.json")))["units"]
             if "base_path" in u and "target_path" in u]
    require_nonempty(pats, [u["name"] for u in units])

    findings = []
    n_units = n_syms = n_nomap = n_secguard = 0
    for u in units:
        name = u["name"]
        if pats and not any(p in name for p in pats):
            continue
        if complete.get(name) and not do_all:
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
        n_units += 1
        so, st = sec_sizes(O), sec_sizes(T)
        bad_secs = {k for k in so if so.get(k) != st.get(k, 0)}
        ours, theirs = obj_sizes(O), obj_sizes(T)
        for sym, (sec, sz) in sorted(ours.items()):
            ref = theirs[sym][1] if sym in theirs else MS.get(sym)
            if ref is None:
                n_nomap += 1
                continue
            n_syms += 1
            if ref == sz:
                continue
            if sec not in bad_secs and not no_guard:
                n_secguard += 1
                continue
            findings.append((name, sym, sec, sz, ref,
                             so.get(sec, 0), st.get(sec, 0)))

    for unit, sym, sec, sz, ref, ss, sr in findings:
        print("*** %-44s %-32s %-8s ours 0x%-6x retail 0x%-6x "
              "(section ours 0x%x retail 0x%x)" % (unit, sym, sec, sz, ref, ss, sr))
    print("[incomplete units: %d] [data/bss symbols compared: %d] "
          "[no size on the retail side: %d] "
          "[suppressed by the section-size corroboration: %d] [findings: %d]"
          % (n_units, n_syms, n_nomap, n_secguard, len(findings)))
    return 1 if findings else 0


sys.exit(main())
