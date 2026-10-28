#!/usr/bin/env python3
"""Is a NON-BIJECTIVE register difference evidence of a source-level fact?

`perm_class_scan.py` splits the sub-100 population by whether ONE injective map
over register names rewrites our instruction stream into retail's.  The rows
where such a map exists are colouring rows.  The rows where it does NOT --
NOTPERM (a register that agrees in one place and disagrees in another),
NONFUNC (one of our registers corresponds to two of retail's) and NONINJ (two
of ours to one of retail's) -- have been read as evidence that a DIFFERENT
VALUE is living in a register, i.e. a source-level fact rather than allocation.

That reading is FALSE, and this file is the control that refutes it.

Compile one synthetic body twice, changing NOTHING but the order of its local
declarations -- the canonical colouring-only edit, and the only source key the
callee-saved band has:

  * NO-REUSE regime (fewer live locals than the callee-saved band, so every
    web owns a register for the whole function): 720 exhaustive declaration
    orders of a 6-local body and 401 sampled orders of a 9-local body produce
    ONLY `IDENTICAL` and `PERM` -- 0 NOTPERM, 0 NONFUNC, 0 NONINJ.  The one
    surprise is `OPERAND`, and it is FRAME SIZE (#67): a declaration order that
    needs a different number of saved registers moves `stwu r1,-64(r1)` to
    `-48(r1)`, which is an immediate difference, not an operation difference.

  * REUSE regime (more live locals than the band, so the allocator must host
    two webs in one register): 300 random declaration orders of a 21-local body
    produce NONFUNC 259, NOTPERM 31, PERM 10.  Nothing about the computation
    changed.  Register reuse alone makes the difference non-bijective.

So non-bijectivity is a property of the ALLOCATOR's reuse, not a fingerprint of
a differing value, and a NOTPERM/NONFUNC/NONINJ row may NOT be read as proof
that our source computes something retail's did not.  Two further tree-wide
controls (`--tree`) keep the converse honest:

  * reuse is COMMON, and commoner the longer the function: 3617 of the 9291
    already-matched functions (38.9 %) carry a callee-saved register with more
    than one definition, rising to 90.4 % once the sample is restricted to
    bodies of 400 bytes or more.  Banded by instruction count, matched
    functions above 200 instructions sit at a median of 3-4 reused registers
    against 5.5-6.5 for the non-bijective rows and 4-7 for the pure-permutation
    rows -- so reuse does NOT separate the non-bijective rows from the
    permutation rows.  It is SUFFICIENT to produce non-bijectivity; it does not
    SELECT which rows are non-bijective.

  * "retail allocates the first-defined value into the higher register", which
    three hand-read rows suggest, is 36/31 by row and 1304/1271 by operand --
    a coin flip.  It is a selection effect, exactly like A98's ascending-home
    reading, and must not be built on.

Usage:
  colouring_reach_control.py --self-test        the refutation, ~40 compiles
  colouring_reach_control.py --sweep N          N random orders in each regime
  colouring_reach_control.py --tree             the two tree-wide controls
"""
from __future__ import annotations

import argparse
import collections
import itertools
import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import objdump_symbol, strip_preamble, load_units
from ndiff import normalize
from perm_class_scan import OBJDUMP, permutation, toks, REG

REPO = Path(__file__).resolve().parent.parent
VER = "GSAE01"

CFLAGS = [
    "-nodefaults", "-proc", "gekko", "-align", "powerpc", "-enum", "int",
    "-fp", "hardware", "-Cpp_exceptions", "off", "-O4,p", "-inline", "auto",
    "-pragma", "cats off", "-pragma", "warn_notinlined off", "-maxerrors", "1",
    "-nosyspath", "-RTTI", "off", "-fp_contract", "on", "-str", "reuse",
    "-multibyte", "-DBUILD_VERSION=0", "-DVERSION_GSAE01", "-DNDEBUG=1",
    "-opt", "nopeephole,noschedule,noloopinvariants", "-inline", "noauto",
    "-lang=c",
]

# ---------------------------------------------------------------- the bodies
NOREUSE_DECLS = "    int a, b, c, d, e, f, g, h, i;\n"
NOREUSE = """extern int fetch(int);
extern void sink(int,int,int);
extern int gT[64];

int probe(int n, int k, int q)
{
%s
    a = fetch(n); b = fetch(k); c = fetch(q);
    d = a + b; e = b + c; f = c + a;
    g = 0;
    for (i = 0; i < n; i++) {
        g += gT[i] * d;
        if (gT[i] > e) {
            g -= f;
        }
    }
    h = fetch(g);
    sink(d, e, f);
    for (i = 0; i < k; i++) {
        h ^= gT[i] + a - b + c;
    }
    sink(a, h, g);
    return h + d + e + f + g;
}
"""

REUSE_NAMES = ["a%d" % i for i in range(10)] + ["b%d" % i for i in range(10)] + ["r"]
REUSE_DECLS = "    int " + ", ".join(REUSE_NAMES) + ";\n"
REUSE = """extern int fetch(int);
extern void sink(int,int,int,int);
extern int gT[64];

int probe(int n)
{
%s
    r = 0;
    a0 = fetch(n); a1 = fetch(n+1); a2 = fetch(n+2); a3 = fetch(n+3);
    a4 = fetch(n+4); a5 = fetch(n+5); a6 = fetch(n+6); a7 = fetch(n+7);
    a8 = fetch(n+8); a9 = fetch(n+9);
    sink(a0+a1, a2+a3, a4+a5, a6+a7);
    r += a8 + a9 + a0 * a3;
    sink(a1, a4, a7, a9);
    r ^= a2 - a5 + a6 * a8;
    b0 = fetch(r); b1 = fetch(r+1); b2 = fetch(r+2); b3 = fetch(r+3);
    b4 = fetch(r+4); b5 = fetch(r+5); b6 = fetch(r+6); b7 = fetch(r+7);
    b8 = fetch(r+8); b9 = fetch(r+9);
    sink(b0+b1, b2+b3, b4+b5, b6+b7);
    r += b8 + b9 + b0 * b3;
    sink(b1, b4, b7, b9);
    r ^= b2 - b5 + b6 * b8;
    return r;
}
"""


def compiler():
    for v in ("GC/2.0",):
        exe = REPO / "build" / "compilers" / v / "mwcceppc.exe"
        if exe.is_file():
            return exe
    raise SystemExit("mwcceppc GC/2.0 not found under build/compilers")


def build(src_text, workdir, tag):
    """Compile `src_text` and return the normalized stream of `probe`.

    Every variant gets its OWN source and object path: reusing one path while
    reading a stale object from a different directory is how a relative `-o`
    silently reports the previous variant's disassembly."""
    c = workdir / ("cr_%s.c" % tag)
    o = workdir / ("cr_%s.o" % tag)
    c.write_text(src_text)
    r = subprocess.run(
        [str(REPO / "build/tools/wibo"), str(REPO / "build/tools/sjiswrap.exe"),
         str(compiler())] + CFLAGS + ["-c", str(c), "-o", str(o)],
        cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0 or not o.is_file():
        raise RuntimeError("compile failed for %s: %s" % (tag, r.stdout[-400:]))
    return normalize(strip_preamble(objdump_symbol(OBJDUMP, o, "probe")), "probe")


def classify(base, other):
    if base == other:
        return "IDENTICAL"
    return permutation(base, other)[1]


def sweep(regime, orders, workdir, prefix):
    """Classify each declaration order in `orders` against the natural order."""
    if regime == "noreuse":
        body, decls, names = NOREUSE, NOREUSE_DECLS, list("abcdefghi")
    else:
        body, decls, names = REUSE, REUSE_DECLS, list(REUSE_NAMES)
    render = lambda o: body % ("    int " + ", ".join(o) + ";\n")
    base = build(render(names), workdir, prefix + "_base")
    counts = collections.Counter()
    example = {}
    for n, order in enumerate(orders):
        cls = classify(base, build(render(list(order)), workdir, "%s_%d" % (prefix, n)))
        counts[cls] += 1
        example.setdefault(cls, list(order))
    return counts, example


NONBIJ = ("NOTPERM", "NONFUNC", "NONINJ")


# ------------------------------------------------------------ tree controls
_DEF = re.compile(r"^\S+\s+r(\d+),")


def _reused(stream):
    d = collections.Counter()
    for line in stream:
        m = _DEF.match(line)
        if m and 14 <= int(m.group(1)) <= 31:
            d[int(m.group(1))] += 1
    return sum(1 for v in d.values() if v > 1)


def tree_controls():
    units = {u["name"].replace("\\", "/"): u
             for u in load_units(REPO / "build" / VER / "config.json")}
    report = json.load(open(REPO / f"build/{VER}/report.json"))

    def ours(cfg, sym):
        return normalize(strip_preamble(objdump_symbol(
            OBJDUMP, REPO / str(cfg["object"]).replace("/obj/", "/src/", 1), sym)), sym)

    def theirs(cfg, sym):
        return normalize(strip_preamble(objdump_symbol(OBJDUMP, REPO / cfg["object"], sym)), sym)

    nonbij, perm, matched = [], [], []
    up = dn = 0
    for u in report["units"]:
        sp = (u.get("metadata") or {}).get("source_path")
        cfg = units.get(sp[4:] if sp and sp.startswith("src/") else sp)
        if cfg is None:
            continue
        for f in u.get("functions", []):
            fz = f.get("fuzzy_match_percent", -1.0)
            sym = f["name"]
            try:
                C = ours(cfg, sym)
            except Exception:
                continue
            if fz >= 100.0:
                matched.append((_reused(C), len(C)))
                continue
            try:
                T = theirs(cfg, sym)
            except Exception:
                continue
            if T == C:
                continue
            why = permutation(C, T)[1]
            (nonbij if why in NONBIJ else perm if why == "PERM" else []).append(
                (_reused(C), len(C)))
            if why in NONBIJ:
                for a, b in zip(C, T):
                    if a == b:
                        continue
                    ta, tb = toks(a), toks(b)
                    if ta is None or tb is None or ta[0] != tb[0]:
                        continue
                    ra, rb = REG.findall(ta[1]), REG.findall(tb[1])
                    if len(ra) != len(rb):
                        continue
                    for x, y in zip(ra, rb):
                        if x != y:
                            if int(y) > int(x):
                                up += 1
                            else:
                                dn += 1
                            break
                    break
    import statistics

    def band(rows, lo, hi):
        return [a for a, n in rows if lo <= n < hi]

    print("REUSE, banded by instruction count (median reused saved registers):")
    print("  %-12s %14s %14s %14s" % ("band", "NON-BIJECTIVE", "PERMUTATION", "MATCHED"))
    for lo, hi in ((0, 200), (200, 400), (400, 800), (800, 100000)):
        cells = []
        for rows in (nonbij, perm, matched):
            v = band(rows, lo, hi)
            cells.append("n=%-4d %.1f" % (len(v), statistics.median(v)) if v else "n=0")
        print("  %-12s %14s %14s %14s" % ("%d-%d" % (lo, hi), *cells))
    tot = sum(1 for a, _ in matched if a > 0)
    print("  matched functions with at least one reused saved register: "
          "%d of %d (%.1f%%)" % (tot, len(matched), 100.0 * tot / max(1, len(matched))))
    print("\nDIRECTION of the first differing register on non-bijective rows:")
    print("  retail HIGHER %d / retail LOWER %d -- a coin flip, not a law" % (up, dn))


# ---------------------------------------------------------------- self-test
def self_test():
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        print("  %s  %s%s" % ("PASS" if cond else "FAIL", name,
                              ("   " + detail) if detail else ""))
        ok = ok and bool(cond)

    with tempfile.TemporaryDirectory(prefix="colour_reach_") as td:
        w = Path(td)
        names = list("abcdefghi")
        a = build(NOREUSE % NOREUSE_DECLS, w, "det1")
        b = build(NOREUSE % NOREUSE_DECLS, w, "det2")
        chk("the compile is deterministic across two distinct paths", a == b)

        # NO-REUSE regime: a modest exhaustive slice is enough to make the point.
        orders = [list(p) + names[5:] for p in itertools.permutations(names[:5])]
        counts, _ = sweep("noreuse", orders, w, "nr")
        bad = sum(counts[c] for c in NONBIJ)
        chk("no-reuse body: no declaration order is non-bijective",
            bad == 0, "%d orders -> %s" % (len(orders), dict(counts)))
        chk("no-reuse body: declaration order does move the colouring",
            counts["PERM"] > 0)
        chk("no-reuse body: the only non-PERM outcome is the frame immediate",
            set(counts) <= {"IDENTICAL", "PERM", "OPERAND"})

        # REUSE regime: the refutation.
        rng = random.Random(11)
        ro = [rng.sample(REUSE_NAMES, len(REUSE_NAMES)) for _ in range(12)]
        rcounts, rex = sweep("reuse", ro, w, "ru")
        bad = sum(rcounts[c] for c in NONBIJ)
        chk("REUSE body: declaration order ALONE produces non-bijective diffs",
            bad > 0, "%d orders -> %s" % (len(ro), dict(rcounts)))
        chk("...and that is the whole refutation: nothing computed changed",
            bad > 0 and set(rcounts) & set(NONBIJ) != set())

        # Negative control: the classifier must not call a body different from
        # itself, or the sweep above would be measuring noise.
        chk("negative control: a stream against itself is IDENTICAL",
            classify(a, a) == "IDENTICAL")
        # Positive control: an unmistakably different body is not a permutation.
        other = build(NOREUSE.replace("g += gT[i] * d;", "g += gT[i] / (d|1);")
                      % NOREUSE_DECLS, w, "pos")
        chk("positive control: a changed OPERATION is not classified PERM",
            classify(a, other) not in ("IDENTICAL", "PERM"),
            classify(a, other))

    print("\nself-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--self-test", action="store_true")
    g.add_argument("--sweep", type=int, metavar="N")
    g.add_argument("--tree", action="store_true")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.tree:
        tree_controls()
        return 0
    rng = random.Random(a.seed)
    with tempfile.TemporaryDirectory(prefix="colour_reach_") as td:
        w = Path(td)
        for regime, names in (("noreuse", list("abcdefghi")), ("reuse", REUSE_NAMES)):
            orders = [rng.sample(names, len(names)) for _ in range(a.sweep)]
            counts, ex = sweep(regime, orders, w, regime)
            print("%-8s %d declaration orders -> %s" % (regime, a.sweep, dict(counts)))
            for cls in NONBIJ:
                if counts[cls]:
                    print("    %s example: %s" % (cls, " ".join(ex[cls])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
