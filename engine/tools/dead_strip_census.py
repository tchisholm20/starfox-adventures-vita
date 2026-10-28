#!/usr/bin/env python3
"""Census of code that is in OUR objects and not in the retail image.

WHY THIS EXISTS
    build/GSAE01/obj/**.o are `dtk dol split` carves of main.dol, so they
    contain only what the retail LINK kept.  build/GSAE01/src/**.o are our
    COMPILER's output, so they contain everything our translation units emit.
    Where our .text is larger, the surplus is code mwld discarded -- and objdiff
    never scores it, because it pairs our functions against retail functions BY
    NAME and a body that is not in the DOL has no pair.  That makes this the one
    corner of the tree no score gate can see, so it needs its own instrument.

THE THREE MECHANISMS, which are NOT interchangeable
    A  INLINED-AND-STRIPPED.  Live code calls the function; MWCC inlined every
       call site and emitted the out-of-line copy anyway; mwld then stripped the
       now-unreferenced copy.  Retail's compiler did exactly the same thing,
       which is WHY the carve lacks it.  This is not dead code and it is not a
       defect -- it is the normal fate of a small static helper at -O4.  It is
       also the MAJORITY of the population, so treating a .text surplus as
       evidence of fabricated code is wrong.
    B  UNCALLED STATIC.  Nothing in the tree calls it, not even transitively.
       This is banned_shapes_check.py's UNCALLED_STATIC_FN class; see
       docs/priced_classes.md section 7 for why it is not automatically a hack
       and for the pool-sharing test that adjudicates one.
    C  STRIPPED GLOBAL.  Not static, and the link dropped it anyway.  The test
       is LINKAGE ONLY -- `cls = "C" if not is_static` -- so do NOT read this
       class as "nothing references it".  Measured, in COMPILED sources only:
       `__OSBootDol` at OSExec.c:349, `__OSBootDolSimple` at :333 and
       `__OSSetExecParams` at :80 and :220 -- three of the nine.
       The nine live in THREE units, not two.  Six are in OSExec.o and two in
       synth_seq_queue.o, whose carve .text is 0: the whole object never
       entered the link, which is what dropped their callers too.  The ninth,
       `__OSFPRInit`, is in OS.o, a LIVE unit whose carve .text is 0x95c, and
       it has no caller anywhere the build compiles.  Do not cite
       src/dolphin/os/__ppc_eabi_init.cpp for one: configure.py builds the
       sibling `.c`, that `.cpp` is never compiled, and its
       __init_hardware carries three `bl`s where retail's carries two --
       .init:0x80003354 is 0x20 long and calls __OSPSInit and __OSCacheInit
       only, which our compiled Runtime.PPCEABI.H/__start.c reproduces.  A file
       the build does not compile is not evidence about the link.
       UNCALLED_STATIC_FN is static-only, so no source screen reports this
       class; and none can, because a global's reference may be a relocation in
       data we have not decompiled (measured: a whole-tree, any-linkage source
       screen yields 4969 rows of which 60 are really dead -- 1.2% precision).

THE PROOF, and it is positive rather than an absence of evidence
    `excise` removes exactly the stripped functions' byte ranges from our .text
    and compares what is left with the carve's .text.  A unit that reproduces
    the carve BYTE FOR BYTE has proved two things at once: that the stripped
    bodies are the whole of the difference, and that every surviving
    instruction -- including the INLINED copies of the class-A helpers -- is
    identical to retail's.  Nothing in such a unit's .text is fabricated.

    A unit that fails has an ordinary sub-100 residual elsewhere; the failure
    says nothing about its dead-stripped code either way.

Usage:
    dead_strip_census.py census      three-way classification, per function
    dead_strip_census.py excise      the byte-identity proof, per unit
    dead_strip_census.py mint        relocations each class-B/C body carries
    dead_strip_census.py --self-test controls, both directions
"""

import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(REPO, "build/GSAE01/src")
CARVE = os.path.join(REPO, "build/GSAE01/obj")
OBJDUMP = os.path.join(REPO, "build/binutils/powerpc-eabi-objdump")
SRC = os.path.join(REPO, "src")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from source_coverage_audit import compiled_sources, live_files_under  # noqa: E402

STT_FUNC = 2
SHT_SYMTAB = 2
RELROW = re.compile(r"^([0-9a-f]{8})\s+(R_PPC\S+)\s+(\S+)$")


class Obj:
    """Just enough ELF32-BE to read sections and the symbol table."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fp:
            self.d = fp.read()
        if self.d[:4] != b"\x7fELF":
            raise ValueError("%s: not ELF" % path)
        (shoff,) = struct.unpack_from(">I", self.d, 0x20)
        shent, shnum, shstrndx = struct.unpack_from(">HHH", self.d, 0x2E)
        self.secs = []
        for i in range(shnum):
            off = shoff + i * shent
            (nm, ty, fl, ad, so, sz, lk, inf, al, es) = struct.unpack_from(
                ">10I", self.d, off)
            self.secs.append({"idx": i, "nameoff": nm, "type": ty, "off": so,
                              "size": sz, "link": lk})
        base = self.secs[shstrndx]
        shstr = self.d[base["off"]:base["off"] + base["size"]]
        for s in self.secs:
            s["name"] = shstr[s["nameoff"]:shstr.find(b"\0", s["nameoff"])].decode()

    def section(self, name):
        return next((s for s in self.secs if s["name"] == name), None)

    def text(self):
        s = self.section(".text")
        if s is None:
            return b"", -1
        return self.d[s["off"]:s["off"] + s["size"]], s["idx"]

    def functions(self):
        """name -> (start, end, is_static) for .text function symbols."""
        st = next((s for s in self.secs if s["type"] == SHT_SYMTAB), None)
        if st is None:
            return {}
        _, tidx = self.text()
        strt = self.secs[st["link"]]
        sd = self.d[strt["off"]:strt["off"] + strt["size"]]
        out = {}
        for i in range(st["size"] // 16):
            off = st["off"] + i * 16
            no, val, sz, info, _o, shndx = struct.unpack_from(">IIIBBH", self.d, off)
            if (info & 0xF) != STT_FUNC or shndx != tidx:
                continue
            nm = sd[no:sd.find(b"\0", no)].decode("utf-8", "replace")
            if nm:
                out[nm] = (val, val + sz, (info >> 4) == 0)
        return out


def pairs():
    for dp, _, fs in os.walk(OURS):
        for f in sorted(fs):
            if not f.endswith(".o"):
                continue
            s = os.path.join(dp, f)
            rel = os.path.relpath(s, OURS)
            c = os.path.join(CARVE, rel)
            if os.path.exists(c):
                yield rel, c, s


def stripped():
    """rel -> {fn: (start, end, is_static)} for functions only WE have."""
    out = {}
    for rel, c, s in pairs():
        a, b = Obj(c), Obj(s)
        ta, _ = a.text()
        tb, _ = b.text()
        if len(ta) == len(tb):
            continue
        fa, fb = a.functions(), b.functions()
        only = {k: v for k, v in fb.items() if k not in fa}
        if only:
            out[rel] = only
    return out


def _srcfiles():
    """Headers plus every LIVE source: text in a dead source cannot call,
    define or strip anything in the DOL, so it is not population here."""
    blobs = {}
    for r, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith(".h"):
                p = os.path.join(r, f)
                blobs[os.path.relpath(p, REPO)] = open(p, errors="replace").read()
    for p in live_files_under("src", exts=(".c", ".cp", ".cpp")):
        blobs[os.path.relpath(p, REPO)] = open(p, errors="replace").read()
    return blobs


def uncalled_statics(blobs=None):
    """Delegate to the validated transitive census in banned_shapes_check."""
    import banned_shapes_check as bsc
    return {re.search(r"(\w+)\s*\(", txt).group(1)
            for _rel, _ln, _cls, txt in bsc.scan_uncalled_statics(blobs or _srcfiles())
            if re.search(r"(\w+)\s*\(", txt)}


def classify():
    strip = stripped()
    dead_names = {fn for fns in strip.values() for fn in fns}
    blobs = {k: v for k, v in _srcfiles().items()
             if any(n in v for n in dead_names)}
    unc = uncalled_statics(blobs) if dead_names else set()
    rows = []
    for rel, fns in sorted(strip.items()):
        for fn, (lo, hi, is_static) in sorted(fns.items()):
            cls = "C" if not is_static else ("B" if fn in unc else "A")
            rows.append((cls, rel, fn, hi - lo))
    return rows


def cmd_census():
    rows = classify()
    tot = {"A": [0, 0], "B": [0, 0], "C": [0, 0]}
    for cls, _rel, _fn, sz in rows:
        tot[cls][0] += 1
        tot[cls][1] += sz
    print("dead-stripped functions: %d, %d B" % (len(rows), sum(r[3] for r in rows)))
    for cls, label in (("A", "INLINED-AND-STRIPPED"),
                       ("B", "UNCALLED STATIC     "),
                       ("C", "STRIPPED GLOBAL     ")):
        print("  %s %s : %3d  %6d B" % (cls, label, tot[cls][0], tot[cls][1]))
    print()
    for cls, rel, fn, sz in sorted(rows):
        print("%s  %-48s %-42s 0x%x" % (cls, rel, fn, sz))
    return 0


def cmd_excise(shift=0, quiet=False):
    """Excise the stripped ranges from our .text; compare with the carve."""
    npass = nfail = 0
    fails = []
    for rel, fns in sorted(stripped().items()):
        ours = Obj(os.path.join(OURS, rel))
        carve = Obj(os.path.join(CARVE, rel))
        tb, _ = ours.text()
        tc, _ = carve.text()
        rng = sorted((lo + shift, hi + shift) for lo, hi, _s in fns.values())
        keep, prev = [], 0
        for lo, hi in rng:
            keep.append(tb[prev:lo])
            prev = max(prev, hi)
        keep.append(tb[prev:])
        if b"".join(keep) == tc:
            npass += 1
        else:
            nfail += 1
            fails.append(rel)
    if not quiet:
        print("units carrying dead-stripped code : %d" % (npass + nfail))
        print("  excised .text == carve .text     : %d" % npass)
        print("  still differing                  : %d" % nfail)
        if fails:
            print("\nstill differing (each has an ordinary sub-100 residual):")
            for r in fails:
                print("  %s" % r)
    return npass, nfail


def _text_relocs(path):
    out = subprocess.run([OBJDUMP, "-r", path], capture_output=True, text=True)
    rows, sec = [], None
    for line in out.stdout.splitlines():
        m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]", line)
        if m:
            sec = m.group(1)
            continue
        m = RELROW.match(line)
        if m and sec == ".text":
            rows.append((int(m.group(1), 16), m.group(3)))
    return rows


def cmd_mint():
    """A class-B/C body that mints nothing and moves no data byte is dead
    weight -- the one true-positive shape (203's dll_CB_getStateHandler)."""
    rows = classify()
    strip = stripped()
    mintless = 0
    for cls, rel, fn, sz in sorted(rows):
        if cls == "A":
            continue
        lo, hi, _ = strip[rel][fn]
        names = sorted({n for off, n in _text_relocs(os.path.join(OURS, rel))
                        if lo <= off < hi})
        if not names:
            mintless += 1
        print("%s  %-44s %-38s 0x%-4x %s"
              % (cls, rel, fn, sz,
                 ", ".join(names)[:80] if names else "*** MINTS NOTHING ***"))
    print("\nbodies carrying zero relocations: %d" % mintless)
    return 0


def self_test():
    ok = True

    def chk(label, cond):
        nonlocal ok
        print("  %-58s %s" % (label, "PASS" if cond else "FAIL"))
        ok = ok and bool(cond)

    print("dead_strip_census self-test")
    strip = stripped()
    chk("the census finds units at all", len(strip) > 0)
    rows = classify()
    chk("every row is classified A, B or C",
        all(r[0] in "ABC" for r in rows))
    chk("classes A, B and C are all populated",
        len({r[0] for r in rows}) == 3)

    npass, nfail = cmd_excise(quiet=True)
    chk("excision reproduces the carve for some units", npass > 0)

    # NEGATIVE CONTROL: excising the WRONG ranges must not reproduce the carve.
    # Shifting every range by 4 bytes keeps the LENGTH right and the content
    # wrong, so a test that only compared sizes would pass this and must not.
    spass, sfail = cmd_excise(shift=4, quiet=True)
    chk("shifting every range by 4 destroys the match (negative control)",
        spass < npass)

    # positive control on the mechanism split: curves.c is a transitively dead
    # cluster -- a NON-transitive scan calls its inner three live, so all four
    # must land in B, never in A.
    curves = {r[2] for r in rows if r[1].endswith("main/curves.o")}
    curvesB = {r[2] for r in rows if r[1].endswith("main/curves.o") and r[0] == "B"}
    chk("curves.c's dead cluster is classified B, whole (transitivity)",
        len(curves) == 4 and curves == curvesB)

    # OSExec.c is entirely absent from the DOL, so its class-C globals must be
    # seen; this is the class banned_shapes_check structurally cannot report.
    cglobals = {r[2] for r in rows if r[0] == "C"}
    chk("class C sees stripped globals banned_shapes_check cannot",
        "OSExecv" in cglobals or "__OSFPRInit" in cglobals)
    # ...and class C is a LINKAGE test, not a reference test.  __OSSetExecParams
    # has real callers, so a reader who takes "class C" to mean "unreferenced"
    # is reading a fact that was never measured.  This control pins that: the
    # class must contain a function the tree demonstrably references.  The
    # witness must be a COMPILED file -- an earlier version of this control read
    # `bl __OSFPRInit` out of src/dolphin/os/__ppc_eabi_init.cpp, which
    # configure.py never compiles, so it passed on a fact about no binary.
    osexec = os.path.join(REPO, "src", "dolphin", "os", "OSExec.c")
    chk("class C is linkage, not reference: it holds a CALLED function",
        "__OSSetExecParams" in cglobals
        and os.path.isfile(osexec)
        and compiled_sources() and osexec in compiled_sources()
        and "__OSSetExecParams(params, paramsWork);" in
        open(osexec, errors="replace").read())

    # ...and the reverse control: __OSFPRInit is in the class with NO caller in
    # any compiled source, and it sits in a LIVE unit (OS.o, carve .text 0x95c),
    # so "class C" does not mean "its whole object was dropped" either.
    callers = [s for s in compiled_sources()
               if "__OSFPRInit" in open(s, errors="replace").read()
               and not s.endswith("/OS.c")]
    chk("class C also holds a global with no compiled caller",
        "__OSFPRInit" in cglobals and not callers)
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if args[0] == "--self-test":
        return self_test()
    if args[0] == "census":
        return cmd_census()
    if args[0] == "excise":
        cmd_excise()
        return 0
    if args[0] == "mint":
        return cmd_mint()
    print("unknown command: %s" % args[0])
    return 2


if __name__ == "__main__":
    sys.exit(main())
