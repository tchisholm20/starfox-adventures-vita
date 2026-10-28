#!/usr/bin/env python3
"""What, if anything, moves MWCC's VOLATILE (f0-f13) float register assignment?

A100 measured that a DECLARATION never touches the volatile half -- 8085
differing FPR operands across both reuse regimes, zero of them in f0-f13.  That
closes the declaration axis but leaves the question the frontier actually needs:
the residual of every volatile-only float row IS a volatile assignment
difference, so something must select it.  #82 says "the expression is the only
key"; that had never been run as an experiment.

This measures the volatile assignment directly, and the FIRST signature it used
was defective -- recorded here because the defect is the finding.  The obvious
float analogue of the integer lane's callee-saved BAND SIGNATURE is the sequence
of volatile FPRs in first-write order (`sig`).  It cannot fail: MWCC fills the
volatile half strictly top-down from the highest register a body needs, so that
sequence is `[n-1 ... 0]` for essentially every body and an axis measured with it
reads INERT whether or not it moved anything.

`vsig` is the signature that can fail: the map from the VALUE (a load's
displacement and base) to the volatile register it lands in.  Two spellings that
put the same value in a different register differ under `vsig` and are identical
under `sig`.

Every axis is additionally reported with a VACUITY flag -- how many of its
variants compile to a BYTE-IDENTICAL stream.  An axis whose variants are
byte-identical did not fail to move the assignment; it never presented the
allocator with a choice, and reading it as "inert" is the same error one level
down.  Neither signature is the score: a score can move for reasons that have
nothing to do with the assignment.

Axes swept, each against the natural spelling:
  ORDER   statement order of the loads
  ASSOC   association / operand order of the combining expression
  TEMP    number and reuse of named temporaries
  CSE     a repeated subexpression stated once vs twice
  CALL    a call inserted between the loads (forces volatiles to be reloaded)
  COUNT   number of simultaneously live loaded values

Every synthetic arm is PAIRED with a real-code arm on the tree's own
volatile-only rows -- a synthetic that is too uniform cannot fail.

Usage:
  python3 tools/a101_volatile_fpr_control.py --self-test
  python3 tools/a101_volatile_fpr_control.py --synthetic
  python3 tools/a101_volatile_fpr_control.py --real
"""
from __future__ import annotations

import argparse
import itertools
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from colouring_reach_control import build
import fp_perm_class_scan as N

FPR = N.FPR
VOL = frozenset(range(0, 14))

HEAD = """extern float gF[64];
extern float fsink(float);
struct S { float a, b, c, d, e, f; };

float probe(const struct S* p, int k)
{
    float a, b, c, d, e, f, t, u, v;
%s
}
"""


def sig(stream):
    """Volatile FPRs in FIRST-WRITE order: the SCHEDULE of the register file.

    WARNING -- this is NOT the assignment.  MWCC fills the volatile half
    strictly top-down from the highest register it needs, so this sequence is
    `[n-1, n-2, ... 0]` for essentially every body and is CONSTANT BY
    CONSTRUCTION.  An axis measured with it reads INERT whether or not it moved
    anything.  `vsig` is the signature that can actually fail."""
    out, seen = [], set()
    for line in stream:
        tk = N.toks(line)
        if tk is None:
            continue
        m = re.match(r"^f(\d{1,2})\b", tk[1])
        if not m:
            continue
        r = int(m.group(1))
        if r in VOL and r not in seen:
            seen.add(r)
            out.append(r)
    return tuple(out)


LOADOP = re.compile(r"^(lfs|lfd|psq_l)\s+f(\d{1,2}),(-?\d+)\((r\d{1,2})\)")


def vsig(stream):
    """VALUE -> volatile register: which memory slot lands in which register.

    This is the assignment `sig` cannot see.  Keyed on the load's displacement
    and base register, so two spellings that put the SAME value in a DIFFERENT
    register differ here even when the register-write sequence is identical."""
    out = {}
    for line in stream:
        m = LOADOP.match(line)
        if not m:
            continue
        reg = int(m.group(2))
        if reg not in VOL:
            continue
        key = (m.group(3), m.group(4))
        out.setdefault(key, reg)
    return tuple(sorted(out.items()))


def full_fsig(stream):
    """Every float register in first-write order (volatile AND saved)."""
    out, seen = [], set()
    for line in stream:
        tk = N.toks(line)
        if tk is None:
            continue
        m = re.match(r"^f(\d{1,2})\b", tk[1])
        if not m:
            continue
        r = int(m.group(1))
        if r not in seen:
            seen.add(r)
            out.append(r)
    return tuple(out)


LOADS = ["    a = p->a;", "    b = p->b;", "    c = p->c;",
         "    d = p->d;", "    e = p->e;", "    f = p->f;"]

COMBINE3 = """    t = a * gF[k] + b * gF[k+1] + c * gF[k+2];
    return t;"""


def variants():
    """(axis, name, body) triples, all semantically identical within an axis."""
    V = []

    # ORDER: every permutation of three independent loads
    for perm in itertools.permutations(range(3)):
        body = "\n".join(LOADS[i] for i in perm) + "\n" + COMBINE3
        V.append(("ORDER", "loads " + "".join("abc"[i] for i in perm), body))

    # ASSOC: operand order / association of the SAME sum of products
    base_loads = "\n".join(LOADS[:3]) + "\n"
    for name, expr in [
        ("a*w0+b*w1+c*w2", "a * gF[k] + b * gF[k+1] + c * gF[k+2]"),
        ("w0*a+w1*b+w2*c", "gF[k] * a + gF[k+1] * b + gF[k+2] * c"),
        ("(a*w0+b*w1)+c*w2", "(a * gF[k] + b * gF[k+1]) + c * gF[k+2]"),
        ("a*w0+(b*w1+c*w2)", "a * gF[k] + (b * gF[k+1] + c * gF[k+2])"),
        ("c*w2+b*w1+a*w0", "c * gF[k+2] + b * gF[k+1] + a * gF[k]"),
    ]:
        V.append(("ASSOC", name, base_loads + "    t = %s;\n    return t;" % expr))

    # TEMP: how many named temporaries carry the accumulation
    for name, tail in [
        ("one temp", "    t = a * gF[k];\n    t = t + b * gF[k+1];\n"
                     "    t = t + c * gF[k+2];\n    return t;"),
        ("two temps", "    t = a * gF[k];\n    u = b * gF[k+1];\n"
                      "    t = t + u;\n    u = c * gF[k+2];\n"
                      "    t = t + u;\n    return t;"),
        ("three temps", "    t = a * gF[k];\n    u = b * gF[k+1];\n"
                        "    v = c * gF[k+2];\n    return t + u + v;"),
        ("no temp", "    return a * gF[k] + b * gF[k+1] + c * gF[k+2];"),
    ]:
        V.append(("TEMP", name, base_loads + tail))

    # CSE: a repeated subexpression stated once vs twice
    for name, tail in [
        ("stated twice", "    t = a * b + c;\n    u = a * b - c;\n"
                         "    return t + u;"),
        ("stated once", "    v = a * b;\n    t = v + c;\n    u = v - c;\n"
                        "    return t + u;"),
    ]:
        V.append(("CSE", name, base_loads + tail))

    # CALL: a call between the loads forces the volatiles to be re-established
    for name, body in [
        ("no call", base_loads + COMBINE3),
        ("call after loads", base_loads + "    a = fsink(a);\n" + COMBINE3),
        ("call between loads",
         LOADS[0] + "\n" + LOADS[1] + "\n    b = fsink(b);\n" + LOADS[2]
         + "\n" + COMBINE3),
    ]:
        V.append(("CALL", name, body))

    # COUNT: how many loaded values are simultaneously live
    for n in (2, 3, 4, 5, 6):
        ld = "\n".join(LOADS[:n]) + "\n"
        expr = " + ".join("%s * gF[k+%d]" % ("abcdef"[i], i) for i in range(n))
        V.append(("COUNT", "%d live" % n, ld + "    t = %s;\n    return t;" % expr))
    return V


def synthetic(workdir):
    V = variants()
    print(f"# SYNTHETIC ARM: {len(V)} variants across "
          f"{len(set(a for a, _, _ in V))} axes\n")
    by_axis = {}
    for axis, name, body in V:
        st = build(HEAD % body, workdir, re.sub(r"\W", "_", axis + "_" + name))
        by_axis.setdefault(axis, []).append(
            (name, sig(st), full_fsig(st), vsig(st), tuple(st)))
    total_moved = total_vac = 0
    for axis, rows in by_axis.items():
        bs, bv, bstream = rows[0][1], rows[0][3], rows[0][4]
        moved = sum(1 for r in rows[1:] if r[3] != bv)
        vac = sum(1 for r in rows[1:] if r[4] == bstream)
        total_moved += moved
        total_vac += vac
        print(f"## {axis}: {moved} of {len(rows)-1} variants MOVE the VALUE->reg "
              f"assignment (vs `{rows[0][0]}`); {vac} are BYTE-IDENTICAL "
              f"(vacuous, nothing could have moved)")
        for name, s, fs, vs, stream in rows:
            if stream == bstream and name != rows[0][0]:
                tag = "VACUO"
            elif vs != bv:
                tag = "MOVED"
            elif name == rows[0][0]:
                tag = " base"
            else:
                tag = " held"
            print(f"   {tag}  {name:22s} regseq={str(list(s)):26s} "
                  f"val->reg={ {k[0]: v for k, v in vs} }")
        print()
    n = len(V) - len(by_axis)
    print(f"TOTAL: {total_moved} of {n} non-base variants move the VALUE->reg "
          f"assignment; {total_vac} of {n} are byte-identical to their base "
          f"(the axis could not have moved anything there)")
    return by_axis


def real():
    """Real-code arm: is the volatile signature of our build != retail's on the
    volatile-only rows, and is it the SAME LENGTH (a relabelling) or not?"""
    from function_objdump import objdump_symbol, strip_preamble
    from ndiff import normalize
    from a101_probe import unit_objs
    ROWS = [
        ("main/vecmath.c", "mtxRotateByVec3s"),
        ("main/lightmap.c", "updateEnvironment"),
        ("dlls/engine/78/78.c", "CameraModeWorldMap_update"),
        ("main/lightmap_draw.c", "lightmapQueueShadowRow"),
        ("main/tex_dolphin.c", "renderGlows"),
        ("main/objhits.c", "ObjHits_DetectObjectPair"),
        ("main/objhits.c", "ObjHits_CollectSkeletonHitsXZ"),
        ("main/objhits.c", "ObjHits_CollectSkeletonHits3D"),
        ("dlls/engine/1_camcontrol/camcontrol.c", "camcontrol_applyState"),
        ("dlls/engine/69/69.c", "CameraModeTalk_update"),
        ("dlls/engine/19/19.c", "waterfx_drawSplashBurst"),
        ("dlls/objects/625/625.c", "drakorhoverpad_updateMain"),
        ("main/newshadows.c", "createNewShadowDistortionTexture"),
    ]
    print("# REAL-CODE ARM: volatile signature, ours vs retail\n")
    same_set = diff_set = 0
    for unit, sym in ROWS:
        try:
            ot, ou = unit_objs(unit)
            t = normalize(strip_preamble(objdump_symbol(N.OBJDUMP, ot, sym)), sym)
            c = normalize(strip_preamble(objdump_symbol(N.OBJDUMP, ou, sym)), sym)
        except Exception as e:
            print(f"  ERROR {unit} {sym}: {e}")
            continue
        st, sc = sig(t), sig(c)
        setsame = set(st) == set(sc)
        same_set += setsame
        diff_set += not setsame
        print(f"  {unit:42s} {sym}")
        print(f"     retail vol order {list(st)}")
        print(f"     ours   vol order {list(sc)}")
        print(f"     same SET={setsame}  same ORDER={st == sc}  "
              f"|retail|={len(st)} |ours|={len(sc)}")
    print(f"\n{same_set} rows use the SAME volatile SET as retail, "
          f"{diff_set} use a different set")


def self_test(workdir):
    ok = True

    def chk(n, c):
        nonlocal ok
        print(f"  {'PASS' if c else 'FAIL'}  {n}")
        ok = ok and bool(c)

    chk("sig picks the DESTINATION register only",
        sig(["fmuls f5,f1,f2", "fadds f3,f5,f4"]) == (5, 3))
    chk("sig ignores saved registers",
        sig(["fmuls f31,f1,f2", "fadds f3,f31,f4"]) == (3,))
    chk("full_fsig keeps them", full_fsig(["fmuls f31,f1,f2"]) == (31,))
    chk("sig reports FIRST write only",
        sig(["fmr f4,f1", "fmr f4,f2", "fmr f5,f3"]) == (4, 5))
    chk("sig SEES a genuine assignment change",
        sig(["fmuls f4,f1,f2"]) != sig(["fmuls f6,f1,f2"]))
    chk("RELOC lines are skipped", sig(["RELOC foo", "fmuls f4,f1,f2"]) == (4,))
    chk("vsig distinguishes the SAME value in a DIFFERENT register",
        vsig(["lfs     f5,0(r3)", "lfs     f4,8(r3)"])
        != vsig(["lfs     f5,8(r3)", "lfs     f4,0(r3)"]))
    chk("sig is BLIND to that same swap (the defect, kept as a control)",
        sig(["lfs     f5,0(r3)", "lfs     f4,8(r3)"])
        == sig(["lfs     f5,8(r3)", "lfs     f4,0(r3)"]))
    # positive control: the compiler must actually be reachable
    st = build(HEAD % ("\n".join(LOADS[:3]) + "\n" + COMBINE3), workdir, "st_pc")
    chk("the synthetic compiles and yields volatile writes", len(sig(st)) > 0)
    chk("a DIFFERENT body yields a different stream",
        build(HEAD % ("\n".join(LOADS[:5]) + "\n    t = a+b+c+d+e;\n    return t;"),
              workdir, "st_pc2") != st)
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--real", action="store_true")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="a101vf_") as td:
        wd = Path(td)
        if a.self_test:
            raise SystemExit(self_test(wd))
        if a.synthetic:
            synthetic(wd)
        if a.real:
            real()


if __name__ == "__main__":
    main()
