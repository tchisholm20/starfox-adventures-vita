#!/usr/bin/env python3
"""Relocation-name oracle, in the two forms that are actually sound.

Usage:  reloc_extent_check.py {control|extent|bases}

WHAT THE RETAIL OBJECTS ACTUALLY ARE
    build/GSAE01/obj/**.o are `dtk dol split` carves.  Every relocation name in
    them is looked up in config/GSAE01/symbols.txt BY ADDRESS.  So "retail's
    object does not name X" is never evidence that the original source did not
    name X; it is evidence about the address the original compiler materialised
    and about how symbols.txt partitions that address range.

WHY THE NAME-MULTISET FORM IS UNSOUND
    Comparing per-unit relocation-name multisets (ours vs retail's) reports 584
    surplus references over 210 names on this tree, and every one of them is an
    artifact of one of five confounds:

      C1  the unit's content differs -- our TU carries code or data the retail
          link dropped (dolphin/os/OSExec.o: retail .text is 0 bytes, ours is
          0x1074; dolphin/os/OS.o: 0x95c vs 0xa84).  410 of the 584.
      C2  the candidate name is not in symbols.txt at all, so the carve is
          incapable of emitting it (every named .sdata2 float constant, every
          MWCC `name$NN` static local).  117 of the 584.
      C3  the unit is an opaque `.s` carve, or the carve emitted no relocations
          for it at all (TRK_MINNOW_DOLPHIN/__exception.o: retail carries ZERO
          relocations against our 49, at identical section size and fuzzy
          100.0).  49 of the 584.
      C4  anonymous pool symbols: MWCC's `@NNN` against the carve's `lbl_<addr>`
          / `jumptable_<addr>` for the same address.  26k references tree-wide.
      C5  internal linkage: MWCC references an object defined in the same
          translation unit through its SECTION symbol (`...bss.0`), so the name
          can never appear on our side while the carve always names it.  This is
          the whole non-const-lane MISSING residue -- __CARDBlock, gxData,
          WaitingQueue, dspStudio, vs, gs, dataSmpSDirs, __files, ...

    Plus addend folding: `gFoo+4` and `gFoo` are the same name, so a multiset
    comparison silently merges a neighbour reach into an in-object access.

THE TWO SOUND FORMS
    `extent` -- for every relocation our objects emit against a symbols.txt
    symbol with a declared size, check that the addend still lands inside that
    symbol.  When it does not, report which symbols.txt object the address
    really belongs to.  This is score-invisible (#70), so no other screen here
    sees it.  Complementary to displacement_oob_check.py, which screens the
    INSTRUCTION displacement; MWCC folds an array index into the displacement
    and a pointer-constant fold into the ADDEND, so both are needed.

    `bases` -- compare the ABSOLUTE ADDRESSES each side materialises: resolve
    every ADDR16_HA/LO and SDA21 relocation that names a real symbol, over units
    whose code/data section sizes agree, dropping the pool and everything C5 and
    C2 make unobservable.  Both sides name by address, so a difference here is a
    real difference in the address the compiler put in a register -- the one
    thing about the original source text that the carve does testify to.  Rows
    that agree on the address but differ on the base symbol are listed
    separately: those are #70, score-neutral, and prove nothing either way.

    A raw `(char*)&X + K` reaching past X is FAITHFUL when retail materialises X
    too; it means symbols.txt has over-split that run, not that the source named
    the wrong object.  That is how expgfx's `(char*)&gObjFxCrystalSparkleTbl +
    0xd0` and shader_dolphin's `sEnvMapBumpIndMtx + 0x30..0x50` were settled:
    they are the only two symbol families displacement_oob_check.py reports, and
    retail's carve materialises exactly those two bases at exactly those sites,
    so both are faithful and the over-split is in symbols.txt.
"""
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJDUMP = os.path.join(REPO, "build/binutils/powerpc-eabi-objdump")
OURS = os.path.join(REPO, "build/GSAE01/src")
RETAIL = os.path.join(REPO, "build/GSAE01/obj")
SYMBOLS = os.path.join(REPO, "config/GSAE01/symbols.txt")
SPLITS = os.path.join(REPO, "config/GSAE01/splits.txt")

SYMLINE = re.compile(r"^(\S+)\s*=\s*(\S+):0x([0-9A-Fa-f]{8});(.*)$")
RELROW = re.compile(r"^([0-9a-f]{8})\s+(R_PPC\S+)\s+(\S+)$")
SECROW = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})")
ANON = re.compile(r"^(lbl|jumptable|func|fn)_[0-9A-Fa-f]{6,}")

BASE_TYPES = ("R_PPC_ADDR16_HA", "R_PPC_ADDR16_LO", "R_PPC_EMB_SDA21")
CODE_SECTIONS = (".text", ".data", ".rodata", ".bss", ".sdata", ".sdata2",
                 ".sbss", ".sbss2", ".ctors", ".dtors", ".init")


def named(sym):
    return not sym.startswith("@") and not ANON.match(sym)


def load_symbols():
    ents = []
    for line in open(SYMBOLS):
        m = SYMLINE.match(line)
        if not m:
            continue
        size = re.search(r"size:0x([0-9A-Fa-f]+)", m.group(4))
        ents.append((int(m.group(3), 16), m.group(1), m.group(2),
                     int(size.group(1), 16) if size else 0))
    ents.sort()
    return ents, {e[1]: e for e in ents}


def container(ents, addr):
    lo, hi, best = 0, len(ents) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if ents[mid][0] <= addr:
            best = ents[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def split_target(text):
    if "+0x" in text:
        sym, add = text.split("+0x")
        return sym, int(add, 16)
    if "-0x" in text:
        sym, add = text.split("-0x")
        return sym, -int(add, 16)
    return text, 0


def relocations(path):
    out = subprocess.run([OBJDUMP, "-r", path], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    rows = []
    for line in out.stdout.splitlines():
        m = RELROW.match(line)
        if m:
            sym, add = split_target(m.group(3))
            rows.append((int(m.group(1), 16), m.group(2), sym, add))
    return rows


def sections(path):
    out = subprocess.run([OBJDUMP, "-h", path], capture_output=True, text=True)
    d = {}
    for line in out.stdout.splitlines():
        m = SECROW.match(line)
        if m and m.group(1) in CODE_SECTIONS:
            d[m.group(1)] = int(m.group(2), 16)
    return d


def source_objects():
    for dp, _, fn in os.walk(OURS):
        for f in sorted(fn):
            if f.endswith(".o"):
                p = os.path.join(dp, f)
                yield os.path.relpath(p, OURS), p


def asm_units():
    out = set()
    for line in open(SPLITS):
        m = re.match(r"^(\S+)\.s:", line)
        if m:
            out.add(m.group(1) + ".o")
    return out


def run_extent(sizes=None, quiet=False):
    ents, by = load_symbols()
    if len(ents) < 1000:
        sys.exit("VACUITY: symbols.txt parsed to %d entries" % len(ents))
    hits, scanned = [], 0
    for unit, path in source_objects():
        rows = relocations(path)
        if rows is None:
            sys.exit("VACUITY: objdump failed on %s" % unit)
        for off, typ, sym, add in rows:
            scanned += 1
            e = by.get(sym)
            if e is None:
                continue
            size = e[3] if sizes is None else sizes
            if size == 0 or 0 <= add < size:
                continue
            addr = e[0] + add
            c = container(ents, addr)
            hits.append((unit, off, typ, sym, add, size, addr, c))
    if scanned == 0:
        sys.exit("VACUITY: no relocation records read at all")
    if not quiet:
        print("relocation records scanned: %d" % scanned)
        print("out-of-extent addend sites: %d" % len(hits))
        for unit, off, typ, sym, add, size, addr, c in hits:
            where = "unmapped"
            if c is not None:
                where = "%s+0x%x (%s, size 0x%x)" % (c[1], addr - c[0], c[2], c[3])
            print("  %-46s @0x%04x %-18s %s%+#x size 0x%-5x -> 0x%08X %s"
                  % (unit, off, typ, sym, add, size, addr, where))
    return scanned, hits


def defined_symbols(path):
    out = subprocess.run([OBJDUMP, "-t", path], capture_output=True, text=True)
    names = set()
    for line in out.stdout.splitlines():
        if not re.match(r"^[0-9a-f]{8} ", line) or "*UND*" in line:
            continue
        parts = line.split()
        if parts:
            names.add(parts[-1])
    return names


def run_bases(quiet=False, bias=0):
    _, by = load_symbols()
    skip = asm_units()
    hits, spelling, compared = [], [], 0
    for unit, path in source_objects():
        retail = os.path.join(RETAIL, unit)
        if not os.path.exists(retail) or unit in skip:
            continue
        if sections(retail) != sections(path):
            continue
        a, b = relocations(retail), relocations(path)
        if a is None or b is None:
            sys.exit("VACUITY: objdump failed on %s" % unit)
        if not a:
            continue
        # C5: an object defined in this TU is referenced through its SECTION
        # symbol by MWCC and by name by the carve, so it can never agree.
        # C2/C4: .sdata2 and .sbss2 are the anonymous constant pool, where our
        # side is always `@NNN`.
        local = defined_symbols(path) | defined_symbols(retail)

        def bases(rows, shift):
            addrs, spelt = {}, {}
            for _, typ, sym, add in rows:
                if typ not in BASE_TYPES or not named(sym) or sym not in by:
                    continue
                if sym in local or by[sym][2] in (".sdata2", ".sbss2"):
                    continue
                add += shift
                a_ = by[sym][0] + add
                addrs[a_] = addrs.get(a_, 0) + 1
                spelt[(sym, add)] = spelt.get((sym, add), 0) + 1
            return addrs, spelt

        (aa, sa), (ab, sb) = bases(a, 0), bases(b, bias)
        compared += 1
        for k in set(aa) | set(ab):
            if aa.get(k, 0) != ab.get(k, 0):
                hits.append((unit, k, aa.get(k, 0), ab.get(k, 0)))
        for k in set(sa) | set(sb):
            if sa.get(k, 0) != sb.get(k, 0):
                spelling.append((unit, k[0], k[1], sa.get(k, 0), sb.get(k, 0)))
    if compared == 0:
        sys.exit("VACUITY: no content-clean unit pairs were compared")
    if not quiet:
        print("content-clean unit pairs compared: %d" % compared)
        print("BASE-ADDRESS disagreements (a real difference in what the "
              "compiler materialised): %d" % len(hits))
        for unit, addr, nr, no in hits:
            print("  %-46s 0x%08X  retail %d  ours %d" % (unit, addr, nr, no))
        print("spelling-only rows (same address, different base symbol -- "
              "#70, score-neutral): %d" % len(spelling))
        for unit, sym, add, nr, no in spelling:
            print("  %-46s %s%+#x  retail %d  ours %d" % (unit, sym, add, nr, no))
    return compared, hits


def run_control():
    scanned, real = run_extent(quiet=True)
    if scanned < 10000:
        sys.exit("control FAIL: only %d relocation records scanned" % scanned)
    # Positive: shrink every extent to one byte -- almost every non-zero addend
    # must now be reported, so the extent test is demonstrably live.
    _, tight = run_extent(sizes=1, quiet=True)
    # Negative: widen every extent past the whole address space -- nothing can
    # be out of extent, so any surviving hit would be a parsing artifact.
    _, loose = run_extent(sizes=0x10000000, quiet=True)
    print("control: %d records; real extents -> %d hits, extent=1 -> %d, "
          "extent=0x10000000 -> %d" % (scanned, len(real), len(tight), len(loose)))
    if not real:
        sys.exit("control FAIL: the screen reports nothing on the real tree")
    if len(tight) <= len(real):
        sys.exit("control FAIL: shrinking every extent did not raise the count")
    # Only a NEGATIVE addend may survive an unbounded extent; a surviving
    # non-negative one would be a parsing artifact rather than a real reach.
    if any(h[4] >= 0 for h in loose):
        sys.exit("control FAIL: a non-negative addend survives an unbounded extent")
    compared, bhits = run_bases(quiet=True)
    if compared < 100:
        sys.exit("control FAIL: only %d unit pairs compared by `bases`" % compared)
    # Positive: bias every base we materialise by four bytes.  If the address
    # comparison is live, essentially every base must now disagree.
    _, biased = run_bases(quiet=True, bias=4)
    print("control: `bases` compared %d unit pairs -> %d address disagreements, "
          "%d under a +4 bias" % (compared, len(bhits), len(biased)))
    if len(biased) <= len(bhits) + 100:
        sys.exit("control FAIL: a +4 address bias produced only %d rows" % len(biased))
    print("control: PASS")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("control", "extent", "bases"):
        sys.exit(__doc__)
    {"control": run_control, "extent": run_extent, "bases": run_bases}[sys.argv[1]]()


main()
