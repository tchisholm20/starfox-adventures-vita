#!/usr/bin/env python3
"""Mutation controls for this project's checkers: prove each one CAN fail.

Why this exists
---------------
Every screen in tools/ earns its keep by reporting a ZERO.  A zero is only
evidence if the instrument was capable of reporting something else, and that is
not something a passing run can tell you.  The campaign has now produced two
demonstrations that it is a real hazard, not a hypothetical:

  * a parse-soundness check imported the parser's own precedence table, so on
    the one question it existed to ask it agreed by construction.  Its
    re-association mutation came back 57258/57258 CLEAN; with a locally written
    table the same run found 2144 mis-parses.

  * `tools/semantic_equivalence.py` decided operand purity with
    `cexpr.is_pure`, and `tools/expr_sweep.py` calls the SAME predicate to
    decide whether to generate the rewrite in the first place.  Generator and
    prover therefore agreed by construction, and the differential evaluator
    could not back them up because it evaluates each spelling with its own
    fresh Evaluator, so a discarded `i++` reads identically on both sides.
    Forcing `is_pure` to True blessed `f(x) + y -> y + f(x)`.

So: deliberately corrupt the SUBJECT (or the model), and require the instrument
to scream.  Six families; `scope`, `object`, `delegate` and `report` run by
default, `build` and `oracle` are opt-in because they write into the tree.

  SCOPE     run the screen with a unit filter that matches nothing.  A screen
            that answers `scanned=0 ... =0` and exits 0 will answer the same
            way for a typo'd unit name, and the operator reads it as clean.
            The instrument must REFUSE an empty scope, not report it clean.
            Each row first PROVES the argument is a filter at all -- the
            filtered and unfiltered runs must differ -- or the probe would be
            as vacuous as the thing it audits.

  OBJECT    hand the screen a deliberately damaged copy of a real object --
            a section resized, its alignment changed, a byte flipped in .text,
            a symbol renamed, a symbol's size changed, two symbols' addresses
            swapped -- through a shadow repo root, and require a finding.  This
            is the only control that proves the two-sided comparators are wired
            to their subject at all.  Every row also runs against an UNDAMAGED
            shadow and the outputs must differ, so a finding that was already
            there cannot pass for a detection.  The ELF patcher here is written
            from the spec with `struct` and shares no code with any instrument
            it tests.

  DELEGATE  the instrument owns its own mutation suite; run it.

  BUILD     what every declaration sweep silently rests on and nothing had ever
            asserted: `direct_build` must reproduce ninja's object BYTES, and
            `unitfuzzy` must agree with report.json.  (opt-in: writes objects)

  REPORT    vacuities in the score itself, reported rather than fixed.

  ORACLE    a POSITIVE control for tools/slot_oracle.py, whose census answers
            "pi not realisable within this block" almost every time -- exactly
            the shape a broken instrument produces.  (opt-in: writes source)

Usage:
  python3 tools/vacuity_audit.py                # scope+object+delegate+report
  python3 tools/vacuity_audit.py --family scope
  python3 tools/vacuity_audit.py --family build --family oracle
  python3 tools/vacuity_audit.py --list
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable

# The measure objdiff omits when it is 0.0.  Held in a name so the ablation
# that demonstrates the pre-fix unguarded read is not itself flagged by the
# scan that looks for one.
ZERO_KEY = "fuzzy_match_percent"


# ------------------------------------------------------------------ ELF32 BE
#
# Written from the ELF spec so the mutation shares nothing with the readers it
# is used to test.  Only the fields the mutations need.

class Elf:
    def __init__(self, data: bytes):
        self.b = bytearray(data)
        if self.b[:4] != b"\x7fELF" or self.b[5] != 2:
            raise ValueError("not a big-endian ELF")
        self.shoff = struct.unpack_from(">I", self.b, 0x20)[0]
        self.shentsize = struct.unpack_from(">H", self.b, 0x2E)[0]
        self.shnum = struct.unpack_from(">H", self.b, 0x30)[0]
        self.shstrndx = struct.unpack_from(">H", self.b, 0x32)[0]

    def sh(self, i):
        o = self.shoff + i * self.shentsize
        (name, typ, flags, addr, off, size, link, info,
         align, entsize) = struct.unpack_from(">10I", self.b, o)
        return dict(i=i, o=o, name=name, type=typ, flags=flags, addr=addr,
                    off=off, size=size, link=link, info=info, align=align,
                    entsize=entsize)

    def _str(self, strtab_off, idx):
        e = self.b.index(b"\0", strtab_off + idx)
        return self.b[strtab_off + idx:e].decode("latin-1")

    def secname(self, s):
        return self._str(self.sh(self.shstrndx)["off"], s["name"])

    def find(self, want):
        for i in range(self.shnum):
            s = self.sh(i)
            if self.secname(s) == want:
                return s
        return None

    def set_field(self, s, field, value):
        idx = ["name", "type", "flags", "addr", "off", "size", "link",
               "info", "align", "entsize"].index(field)
        struct.pack_into(">I", self.b, s["o"] + idx * 4, value)

    def symtab(self):
        s = self.find(".symtab")
        if s is None:
            return None, None
        return s, self.sh(s["link"])


def mutate_object(path: str, kind: str) -> str:
    """Damage a copy of `path` in place.  Returns a human description."""
    e = Elf(open(path, "rb").read())
    if kind == "size":
        t = e.find(".text") or e.find(".data")
        e.set_field(t, "size", max(0, t["size"] - 4))
        what = "%s size %d -> %d" % (e.secname(t), t["size"], t["size"] - 4)
    elif kind == "align":
        t = e.find(".text") or e.find(".data")
        e.set_field(t, "align", 64 if t["align"] != 64 else 128)
        what = "%s alignment -> 64" % e.secname(t)
    elif kind == "content":
        t = e.find(".text")
        if t is None or t["size"] < 8:
            raise ValueError("no .text to corrupt")
        e.b[t["off"] + 4] ^= 0x01
        what = ".text byte at +4 flipped"
    elif kind == "symsize":
        st, _strt = e.symtab()
        if st is None:
            raise ValueError("no .symtab")
        for k in range(st["size"] // 16):
            o = st["off"] + k * 16
            _n, _v, sz, info, _o2, _sh = struct.unpack_from(">IIIBBH", e.b, o)
            if (info & 0xF) == 2 and sz > 8:                # STT_FUNC
                struct.pack_into(">I", e.b, o + 8, sz - 4)
                what = "first function's st_size %d -> %d" % (sz, sz - 4)
                break
        else:
            raise ValueError("no sized function")
    elif kind in ("symname", "symorder"):
        st, strt = e.symtab()
        if st is None:
            raise ValueError("no .symtab")
        n = st["size"] // 16
        funcs = []
        for k in range(n):
            o = st["off"] + k * 16
            nameoff, value, sz, info, other, shndx = struct.unpack_from(
                ">IIIBBH", e.b, o)
            if (info & 0xF) == 2 and sz:                    # STT_FUNC
                funcs.append((o, nameoff, value, sz))
        if len(funcs) < 2:
            raise ValueError("fewer than two functions")
        if kind == "symname":
            o, nameoff, _v, _s = funcs[0]
            base = strt["off"] + nameoff
            end = e.b.index(b"\0", base)
            e.b[end - 1] = ord("Z") if e.b[end - 1] != ord("Z") else ord("Y")
            what = "renamed the first function's last character"
        else:
            (o1, _n1, v1, _s1), (o2, _n2, v2, _s2) = funcs[0], funcs[1]
            struct.pack_into(">I", e.b, o1 + 4, v2)
            struct.pack_into(">I", e.b, o2 + 4, v1)
            what = "swapped two function addresses"
    else:
        raise ValueError("unknown mutation " + kind)
    open(path, "wb").write(bytes(e.b))
    return what


# --------------------------------------------------------------- shadow root

def shadow_root(unit_name: str, mutation: str, tmp: str, tag: str = None):
    """A complete miniature repo root holding ONE unit.

    Screens find their root two different ways -- some from
    `os.path.abspath(__file__)`, some from `Path(__file__).resolve()` (which
    follows a symlink straight back to the real tree), and some by directory
    convention under `build/<ver>/{src,obj}` rather than through a manifest.
    So the shadow COPIES tools/ and lays out a real `build/GSAE01/{src,obj}`
    pair plus both manifests: whichever route a screen takes, it lands here.

    `mutation` of None builds the shadow undamaged -- that is the control run
    the mutated run must differ from, without which "it reported a finding"
    proves nothing about whether it saw the mutation.
    """
    units = json.load(open(os.path.join(REPO, "objdiff.json")))["units"]
    u = next((x for x in units if x["name"] == unit_name), None)
    if u is None or not u.get("base_path") or not u.get("target_path"):
        return None, "unit %s has no two-sided objects" % unit_name
    ours = os.path.join(REPO, u["base_path"])
    retail = os.path.join(REPO, u["target_path"])
    if not (os.path.exists(ours) and os.path.exists(retail)):
        return None, "objects for %s are not built" % unit_name

    root = os.path.join(tmp, tag or (mutation or "clean"))
    os.makedirs(root, exist_ok=True)
    shutil.copytree(os.path.join(REPO, "tools"), os.path.join(root, "tools"),
                    ignore=shutil.ignore_patterns("__pycache__", "refcorpus",
                                                  "xref", "dolphin_mcp",
                                                  "ghidra_scripts"))
    rel_src = u["base_path"]
    rel_obj = u["target_path"]
    for rel, src in ((rel_src, ours), (rel_obj, retail)):
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    os.symlink(os.path.join(REPO, "build", "binutils"),
               os.path.join(root, "build", "binutils"))
    os.symlink(os.path.join(REPO, "build", "tools"),
               os.path.join(root, "build", "tools"))
    for extra in ("config", "src", "include"):
        p = os.path.join(REPO, extra)
        if os.path.exists(p):
            os.symlink(p, os.path.join(root, extra))

    what = "undamaged control"
    if mutation:
        what = mutate_object(os.path.join(root, rel_src), mutation)

    v = dict(u)
    v["base_path"] = rel_src
    v["target_path"] = rel_obj
    json.dump({"units": [v]}, open(os.path.join(root, "objdiff.json"), "w"))
    cfg_p = os.path.join(REPO, "build", "GSAE01", "config.json")
    if os.path.exists(cfg_p):
        cfg = json.load(open(cfg_p))
        cfg["units"] = [x for x in cfg["units"]
                        if x.get("object") == rel_obj]
        json.dump(cfg, open(os.path.join(root, "build", "GSAE01",
                                         "config.json"), "w"))
    return root, what


# ------------------------------------------------------------------ registry
#
# `screams` is a predicate over (returncode, text).  The DEFAULT is "the tool
# did not answer a bare zero"; a tool that must gate additionally has to exit
# non-zero.

def _refuses_empty(rc, t):
    """An empty scope must be an ERROR, not a clean zero."""
    if rc != 0:
        return True
    return bool(re.search(r"no units? match|nothing to (?:scan|check)|"
                          r"EMPTY SCOPE|refus", t, re.I))


def _has_finding(rc, t):
    return rc != 0 or bool(re.search(
        r"MISMATCH|DIFFER|ONLY-OURS|RETAIL-ONLY|misordered=[1-9]|"
        r"real-mismatch=[1-9]|content-mismatch=[1-9]|first divergence|"
        r"CONTENT|RELOC|ours (?:SHORT|LARGER)", t))


# name -> (argv-from-root, object mutations it must detect)
OBJECT_SCREENS = {
    "section_size_check": ("size",),
    "section_align_check": ("align",),
    "order_check": ("symorder",),
    "pairing_check": ("symname",),
    "obj_equal": ("content", "size"),
    # a SECTION-size change is invisible to size_parity_check by design -- it
    # reads per-function symbol sizes -- so its control has to mutate the axis
    # it actually looks at, or the row would pass for the wrong reason.
    "size_parity_check": ("symsize",),
}

# Screens that accept a unit filter.  An empty scope must be refused.
SCOPE_SCREENS = [
    "section_size_check", "order_check", "pairing_check", "pool_content_check",
    "reloc_width_check", "skew_value_check", "f64_half_check",
    "data_reloc_check", "size_parity_check", "section_align_check",
    "data_sym_size_check", "sda_base_register_check", "unit_bytes_check",
]

# Instruments that own their mutation suite.
DELEGATED = [
    ("semantic_equivalence", ["--mutate"]),
    ("cexpr_roundtrip", ["--self-test"]),
    ("cexpr", ["--self-test"]),
    ("banned_shapes_check", ["--self-test"]),
    ("obj_equal", ["--self-test"]),
    ("stmt_sweep", ["--self-test"]),
    ("stmt_sweep", ["--self-test", "--mutate"]),
    ("score_delta_gate", ["--self-test"]),
    ("missing_param_check", ["--selftest"]),
    ("decl_split_sweep", ["--self-test"]),
]


def run(argv, cwd=None, timeout=900):
    try:
        r = subprocess.run(argv, cwd=cwd or REPO, capture_output=True,
                           text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except Exception as exc:                                  # pragma: no cover
        return 125, str(exc)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def family_scope(verbose):
    print("=== MUTATION: EMPTY SCOPE "
          "(a filter that matches nothing must be refused, not reported clean) ===")
    print("    each row first PROVES the argument is a filter at all, by "
          "requiring the\n    filtered and unfiltered runs to differ -- "
          "otherwise the probe itself is vacuous.")
    bad = 0
    for tool in SCOPE_SCREENS:
        p = os.path.join(REPO, "tools", tool + ".py")
        if not os.path.exists(p):
            continue
        rc0, full = run([PY, p])
        token = "__vacuity_no_such_unit__"
        rc, out = run([PY, p, token])
        # Some screens spell their positional as an OUTPUT PATH, not a filter.
        spilled = os.path.join(REPO, token)
        if os.path.exists(spilled):
            os.unlink(spilled)
            print("  [n/a ] %-24s the positional argument is an output path"
                  % tool)
            continue
        if out == full:
            print("  [n/a ] %-24s the positional argument is not a unit filter"
                  % tool)
            continue
        ok = _refuses_empty(rc, out)
        bad += not ok
        print("  [%s] %-24s rc=%-3d %s"
              % ("held" if ok else "VACUOUS", tool, rc,
                 " ".join(out.split())[:74]))
    return bad


def family_object(unit, verbose):
    print("\n=== MUTATION: DAMAGED OBJECT "
          "(a real object, deliberately broken, must produce a finding) ===")
    print("    every row is run TWICE -- once against an UNDAMAGED shadow of the "
          "same unit --\n    and the two outputs must DIFFER, or the finding was "
          "not caused by the mutation.")
    bad = 0
    tmp = tempfile.mkdtemp(prefix="vacuity_")
    try:
        base_root, _ = shadow_root(unit, None, tmp, tag="control")
        if base_root is None:
            print("  [skip] no two-sided objects for %s" % unit)
            return 0
        for tool, kinds in sorted(OBJECT_SCREENS.items()):
            for kind in kinds:
                root, what = shadow_root(unit, kind, tmp,
                                         tag="%s_%s" % (tool, kind))
                if root is None:
                    print("  [skip] %-24s %s" % (tool, what))
                    continue

                def go(r):
                    if tool == "obj_equal":
                        # ours-vs-ours: the damaged copy against the UNDAMAGED
                        # shadow of the same object, so the ONLY difference is
                        # the mutation.  Against retail it would report the
                        # pre-existing `.comment` delta and look like a pass
                        # for entirely the wrong reason.
                        return run([PY, os.path.join(r, "tools",
                                                     "obj_equal.py"),
                                    os.path.join(base_root, "build", "GSAE01",
                                                 "src", *unit.split("/")[1:])
                                    + ".o",
                                    os.path.join(r, "build", "GSAE01", "src",
                                                 *unit.split("/")[1:]) + ".o"],
                                   cwd=r)
                    return run([PY, os.path.join(r, "tools", tool + ".py")],
                               cwd=r)

                rc, out = go(root)
                rc0, ctl = go(base_root)
                saw = out != ctl
                ok = _has_finding(rc, out) and saw
                bad += not ok
                note = "" if saw else "  <-- IDENTICAL TO THE CONTROL"
                print("  [%s] %-22s %-9s %s -> %s%s"
                      % ("held" if ok else "BLIND", tool, kind, what,
                         " ".join(out.split())[:44], note))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return bad


def family_delegate(verbose):
    print("\n=== DELEGATED: instruments that own a mutation suite ===")
    bad = 0
    for tool, args in DELEGATED:
        p = os.path.join(REPO, "tools", tool + ".py")
        if not os.path.exists(p):
            continue
        rc, out = run([PY, p] + args)
        ok = rc == 0
        bad += not ok
        tail = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
        print("  [%s] %-24s %-12s %s"
              % ("held" if ok else "FAILED", tool, " ".join(args),
                 tail[0].strip()[:64]))
    return bad


def family_build(unit_object, verbose):
    """The two instruments every declaration sweep silently depends on.

    A sweep ranks orderings on `unitfuzzy.measure` and compiles them with
    `direct_build`, and NEITHER is what the standing gate uses.  If unitfuzzy's
    number diverged from report.json, or if direct_build's object were not
    byte-for-byte ninja's, every sweep in this campaign would have optimised
    something the gate cannot see -- and both would still have looked perfectly
    healthy from inside.

    NOTE: this family WRITES into build/<ver>/src.  Do not run it against a
    worktree that has a sweep in flight; point --unit-object at a unit no
    sweep touches.
    """
    print("\n=== BUILD/MEASURE CHAIN "
          "(what the sweeps actually rank and compile on) ===")
    bad = 0
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import unitfuzzy                                          # noqa: E402
    from direct_build import direct_build                     # noqa: E402
    import hashlib

    def md5(p):
        return hashlib.md5(open(p, "rb").read()).hexdigest()

    src_o = os.path.join(REPO, unit_object.replace("/obj/", "/src/"))
    if not os.path.exists(src_o):
        print("  [skip] %s is not built" % src_o)
        return 0
    before = md5(src_o)
    os.unlink(src_o)
    ok_build = direct_build(unit_object, "GSAE01")
    after = md5(src_o) if os.path.exists(src_o) else None
    ok = bool(ok_build) and after == before
    bad += not ok
    print("  [%s] direct_build reproduces ninja's object bytes  %s"
          % ("held" if ok else "DIVERGES", before[:12]))

    # THE CLI IS A SEPARATE SURFACE.  direct_build was import-only, with no
    # `__main__` at all: a shell caller got a silent no-op and exit 0, then
    # measured whatever object the PREVIOUS probe had left behind.  So the CLI
    # must (a) actually rebuild a deleted object, and (b) FAIL, not succeed,
    # when it cannot -- the second half is the control, because a no-op passes
    # (a) trivially once the object is already there.
    dbp = os.path.join(REPO, "tools", "direct_build.py")
    os.unlink(src_o)
    rc_ok, _ = run([PY, dbp, unit_object])
    built = os.path.exists(src_o) and md5(src_o) == before
    ok = rc_ok == 0 and built
    bad += not ok
    print("  [%s] ...and its CLI rebuilds a DELETED object (rc=%s, rebuilt=%s)"
          % ("held" if ok else "SILENT NO-OP", rc_ok, built))
    rc_bad, _ = run([PY, dbp, "build/GSAE01/obj/main/__no_such_unit__.o"])
    rc_none, _ = run([PY, dbp])
    ok = rc_bad != 0 and rc_none != 0
    bad += not ok
    print("  [%s] ...and it reports FAILURE for a target it cannot build "
          "(rc=%s) and for no argument at all (rc=%s)"
          % ("held" if ok else "EXITS 0 ANYWAY", rc_bad, rc_none))
    # non-vacuity: the comparison must be able to see a difference at all
    with open(src_o, "r+b") as fh:
        fh.seek(0x30)
        b = fh.read(1)
        fh.seek(0x30)
        fh.write(bytes([b[0] ^ 0xFF]))
    saw = md5(src_o) != before
    bad += not saw
    print("  [%s] ...and the md5 comparison can see a one-byte change"
          % ("held" if saw else "BLIND"))
    direct_build(unit_object, "GSAE01")

    # THE PATH THE SWEEPERS ACTUALLY TAKE.  Neither surface above is it: every
    # sweeper calls `brute_match.rebuild`, which does the obj/->src/ rewrite and
    # an env override of its own before it ever reaches direct_build.  The BUILD
    # family passed for months while the CLI was a no-op precisely because it
    # controlled a surface no caller used, so this half mutates the SOURCE and
    # requires the object to move -- the only question that distinguishes a
    # build tool from a program that does nothing.
    import brute_match as _bm                                 # noqa: E402
    _cfg = json.load(open(os.path.join(REPO, "build", "GSAE01", "config.json")))
    _name = next((u["name"] for u in _cfg["units"]
                  if u["object"].replace("\\", "/") == unit_object), None)
    src_c = os.path.join(REPO, "src", _name.replace("\\", "/")) if _name else None
    if src_c and os.path.exists(src_c):
        with open(src_c, "rb") as fh:
            pristine = fh.read()
        try:
            with open(src_c, "wb") as fh:
                fh.write(pristine + b"\nint _vacuityProbeFn(int x) { return x * 37 + 11; }\n")
            moved = _bm.rebuild(unit_object, "GSAE01") and md5(src_o) != before
            with open(src_c, "wb") as fh:
                fh.write(pristine + b"\nthis is not C;\n")
            refused = _bm.rebuild(unit_object, "GSAE01") is False
        finally:
            with open(src_c, "wb") as fh:
                fh.write(pristine)
            _bm.rebuild(unit_object, "GSAE01")
        restored = os.path.exists(src_o) and md5(src_o) == before
        for ok_, msg in ((moved, "brute_match.rebuild (the CALLER path) sees a source change"),
                         (refused, "...and returns False when the source cannot compile"),
                         (restored, "...and the restored source rebuilds the original bytes")):
            bad += not ok_
            print("  [%s] %s" % ("held" if ok_ else "VACUOUS", msg))

    # objdiff OMITS fuzzy_match_percent at 0.0.  `unitfuzzy`'s CLI reads it with
    # a default; `fuzzy_measure`, which is the API every sweep ranks on, did not
    # -- so a variant that zeroed the function raised KeyError and killed the
    # sweep, and an aborted sweep reads exactly like an exhausted one.  Same
    # shape as the direct_build defect with the surfaces swapped.
    _real_measure = unitfuzzy.measure
    unitfuzzy.measure = lambda unit, version: {"functions": [{"name": "probeSym"}]}
    try:
        try:
            got = _bm.fuzzy_measure({"object": unit_object, "name": "x"},
                                    "probeSym", "GSAE01", retries=1)
            handled = isinstance(got, float)
        except Exception:
            handled = False
        # ABLATION: the pre-fix unguarded read must still blow up, or the
        # tolerance above is somebody else's doing.  The key goes through a
        # name, not a literal, so this demonstration of the defect is not
        # itself an instance of it for the scan further down.
        try:
            u_ = unitfuzzy.measure(None, None)
            float(u_["functions"][0][ZERO_KEY])
            ablated_raises = False
        except KeyError:
            ablated_raises = True
    finally:
        unitfuzzy.measure = _real_measure
    for ok_, msg in ((handled, "fuzzy_measure survives an OMITTED fuzzy key (a 0.0 row) "
                               "instead of killing the sweep"),
                     (ablated_raises, "...and ablating the guard raises KeyError again")):
        bad += not ok_
        print("  [%s] %s" % ("held" if ok_ else "VACUOUS", msg))

    rep = json.load(open(os.path.join(REPO, "build", "GSAE01",
                                      "report.json")))
    cfg = json.load(open(os.path.join(REPO, "build", "GSAE01",
                                      "config.json")))
    rmap = {}
    for u in rep["units"]:
        for f in u.get("functions") or []:
            rmap[(u["name"], f["name"])] = f.get("fuzzy_match_percent")
    checked = agreed = discriminating = 0
    seen = set()
    for (un, fn), v in rmap.items():
        if v is None or v >= 100.0 or un in seen:
            continue
        seen.add(un)
        base = "/".join(un.split("/")[1:]) + ".c"
        u = next((x for x in cfg["units"] if x["name"] == base), None)
        if u is None:
            continue
        try:
            m = unitfuzzy.measure(u, "GSAE01")
        except Exception:
            continue
        got = next((f.get("fuzzy_match_percent")
                    for f in (m.get("functions") or []) if f["name"] == fn),
                   None)
        checked += 1
        agreed += got is not None and abs(got - v) < 1e-9
        discriminating += 1
        if checked >= 6:
            break
    ok = checked and agreed == checked and discriminating
    bad += not ok
    print("  [%s] unitfuzzy agrees with report.json on %d/%d sub-100 "
          "functions" % ("held" if ok else "DIVERGES", agreed, checked))

    # THE SAME TRAP, EVERYWHERE ELSE IT WAS PASTED -- AND THE OTHER POLARITY.
    #
    # `fuzzy_measure` was fixed where the defect was noticed; `stmt_sweep`,
    # `permsweep` and `probe_spelling` each carried their own copy of the
    # identical unguarded read and none was touched.  Enumerating fourteen
    # ranking tools by name fixed that round and reproduced the shape one level
    # up: the SCOPE was a hand-written list, so a reader outside it is invisible
    # exactly the way a paste site outside the fix was.  The scope is now
    # DERIVED -- every tool that reads one of these keys is in it.
    #
    # And the rule is bigger than the key it was found on.  `report.json`
    # SERIALISES NO ZERO, at any level: `fuzzy_match_percent`,
    # `complete_units`, `matched_code`, `matched_data`, `matched_functions`,
    # `total_code` and `total_data` are all simply absent when they are 0, and
    # NONE of them is ever present-as-zero.  So the key set is derived from the
    # live report too, and both failure modes are checked:
    #
    #   * a bare subscript  -> KeyError, kills a sweep mid-run, and an aborted
    #     sweep reads exactly like an exhausted one;
    #   * `.get(key, 100.0)` -> reports the WORST rows as perfect and a
    #     `< 100.0` filter then drops them in silence.  This one is worse: a
    #     crash is loud, a screen that finds nothing looks like a clean tree.
    import ast

    # ...and the KEY SET has to be derived too, for the same reason the reader
    # scope did.  This block used to enumerate seven key names by hand while
    # its own comment explained that a hand-written scope hides whatever sits
    # outside it.  `matched_data_percent` sat outside it, so the
    # `.get(key, 100.0)` half below -- the half the comment calls "worse" --
    # could not see the one live instance of that exact shape in the tree
    # (`promotion_candidates.is_report_exact`).  The vocabulary is the
    # tree-level `measures` dict; a key is omitted-at-zero if some instance of
    # a scope kind that uses it does not carry it.  Each kind is asked
    # separately, because a function scope has no `matched_data` to omit.
    rep_doc = rep
    measure_vocab = set(rep_doc["measures"])

    def _omitted(scopes):
        used = set()
        for s in scopes:
            used |= measure_vocab & set(s)
        return {k for k in used if any(k not in s for s in scopes)}

    omit_keys = _omitted([u.get("measures") or {} for u in rep_doc["units"]])
    omit_keys |= _omitted([f for u in rep_doc["units"]
                           for f in (u.get("functions") or [])])
    omit_keys |= _omitted([s for u in rep_doc["units"]
                           for s in (u.get("sections") or [])])
    present_zero = [(u["name"], k) for u in rep_doc["units"]
                    for k, v in (u.get("measures") or {}).items() if v == 0]
    ok = bool(omit_keys) and not present_zero
    bad += not ok
    print("  [%s] report.json serialises no zero: %d measure keys go ABSENT, "
          "0 are present-as-zero (%d present-as-zero found)"
          % ("held" if ok else "PREMISE BROKEN", len(omit_keys),
             len(present_zero)))

    def _guarded_keys(tree):
        """Keys the file tests for with `in` / `not in` -- AST, so the guard is
        found whichever quote style wrote it."""
        out = set()
        for n in ast.walk(tree):
            if isinstance(n, ast.Compare) and isinstance(n.left, ast.Constant) \
               and isinstance(n.left.value, str) \
               and any(isinstance(o, (ast.In, ast.NotIn)) for o in n.ops):
                out.add(n.left.value)
        return out

    def offences(text, keys):
        """(bare-subscript keys, [(key, default)] positive defaults)."""
        try:
            tree = ast.parse(text)
        except SyntaxError:
            return set(), []
        guarded = _guarded_keys(tree)
        bare, defaults = set(), []
        for n in ast.walk(tree):
            if isinstance(n, ast.Subscript) \
               and isinstance(getattr(n, "slice", None), ast.Constant) \
               and n.slice.value in keys and n.slice.value not in guarded:
                base = n.value
                chain = isinstance(base, ast.Subscript) \
                    and isinstance(getattr(base, "slice", None), ast.Constant) \
                    and base.slice.value == "measures"
                if not chain:
                    bare.add(n.slice.value)
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
               and n.func.attr == "get" and len(n.args) > 1 \
               and isinstance(n.args[0], ast.Constant) \
               and n.args[0].value in keys \
               and isinstance(n.args[1], ast.Constant) \
               and isinstance(n.args[1].value, (int, float)) \
               and not isinstance(n.args[1].value, bool) \
               and n.args[1].value > 0:
                defaults.append((n.args[0].value, n.args[1].value))
        return bare, defaults

    tools_dir = os.path.join(REPO, "tools")
    readers, bare_off, dflt_off = [], [], []
    for fn_ in sorted(os.listdir(tools_dir)):
        if not fn_.endswith(".py"):
            continue
        text = open(os.path.join(tools_dir, fn_)).read()
        if not any(k in text for k in omit_keys):
            continue
        readers.append(fn_)
        b, d = offences(text, omit_keys)
        if b:
            bare_off.append((fn_, sorted(b)))
        if d:
            dflt_off.append((fn_, d))
    ok = not bare_off and len(readers) >= 30
    bad += not ok
    print("  [%s] no tool subscripts an omitted-at-zero measure unguarded "
          "(%d readers discovered)%s"
          % ("held" if ok else "UNGUARDED", len(readers),
             "" if not bare_off else "  *** " + ", ".join(
                 "%s%s" % (f, k) for f, k in bare_off)))
    ok = not dflt_off
    bad += not ok
    print("  [%s] ...and none DEFAULTS one to a positive value, which would "
          "report a 0.0 row as perfect%s"
          % ("held" if ok else "WRONG DEFAULT",
             "" if not dflt_off else "  *** " + ", ".join(
                 "%s%s" % (f, d) for f, d in dflt_off)))

    # ...and the one live instance, checked by BEHAVIOUR and not by grep, so a
    # future respelling of the same mistake is still caught.  A unit with no
    # `matched_data_percent` matched no data; `promotion_candidates` must
    # reject it, and must still accept the same unit once the key is present at
    # 100.0 -- otherwise a stricter default would simply have turned the tool
    # off.
    # The predicate is lifted out by AST and executed on its own -- importing
    # the module would be the very side effect the arm below checks for.
    _pc_src = open(os.path.join(REPO, "tools", "promotion_candidates.py")).read()
    _pc_fn = next(n for n in ast.parse(_pc_src).body
                  if isinstance(n, ast.FunctionDef)
                  and n.name == "is_report_exact")
    _ns = {}
    exec(compile(ast.Module(body=[_pc_fn], type_ignores=[]),
                 "promotion_candidates.py", "exec"), _ns)
    is_report_exact = _ns["is_report_exact"]
    _base = {"measures": {"matched_code_percent": 100.0},
             "metadata": {"complete": False, "auto_generated": False}}
    _absent = is_report_exact(_base)
    _present = dict(_base, measures=dict(_base["measures"],
                                         matched_data_percent=100.0))
    _zero = dict(_base, measures=dict(_base["measures"],
                                      matched_data_percent=0.0))
    ok = (not _absent) and is_report_exact(_present) \
        and not is_report_exact(_zero)
    bad += not ok
    print("  [%s] promotion_candidates rejects a unit whose "
          "matched_data_percent is ABSENT, and still accepts one at 100.0"
          % ("held" if ok else "WRONG DEFAULT"))

    # ABLATION.  A checker that finds nothing because it is looking in the
    # wrong place is indistinguishable from a clean tree, so every arm gets an
    # injected subject -- including the two spellings that must NOT fire and
    # the single-quoted guard the old textual test could not see.
    K = "fuzzy_match_percent"
    probes = [
        ("bare subscript caught", "def f(r):\n    return float(r['%s'])\n" % K,
         True, False),
        (".get with no default passes", "def f(r):\n    return r.get('%s')\n" % K,
         False, False),
        (".get(key, 0.0) passes", "def f(r):\n    return r.get('%s', 0.0)\n" % K,
         False, False),
        (".get(key, -1.0) passes (the MISSING sentinel)",
         "def f(r):\n    return r.get('%s', -1.0)\n" % K, False, False),
        (".get(key, 100.0) caught",
         "def f(r):\n    return r.get('%s', 100.0)\n" % K, False, True),
        ("single-quoted `in` guard passes",
         "def f(r):\n    if '%s' in r:\n        return r['%s']\n" % (K, K),
         False, False),
        ("double-quoted `in` guard passes",
         'def f(r):\n    if "%s" in r:\n        return r["%s"]\n' % (K, K),
         False, False),
        ("a ['measures'] chain passes",
         "def f(u):\n    return u['measures']['%s']\n" % K, False, False),
    ]
    misread = []
    for label, text, want_bare, want_dflt in probes:
        b, d = offences(text, {K})
        if bool(b) != want_bare or bool(d) != want_dflt:
            misread.append(label)
    ok = not misread
    bad += not ok
    print("  [%s] the scan reads all %d injected spellings correctly%s"
          % ("held" if ok else "BLIND", len(probes),
             "" if not misread else "  *** " + "; ".join(misread)))

    # A SWEEP THAT DIES ON ONE ROW READS EXACTLY LIKE A SWEEP THAT FINISHED.
    #
    # Restoring the omitted-at-zero rows put a `.s` unit -- `.init`, no split
    # object -- into `pool_value_sequence`'s population, and its per-row helper
    # answered a missing object with `raise SystemExit`.  That killed the
    # `--all` loop at row 18 of 21, so the three rows behind it were never
    # scanned and the run still printed a full-looking table.  A row a tool
    # cannot open must be REPORTED as unscanned, never skipped by ending the
    # loop; so the sweep is required to state its own population and to account
    # for every row in it.
    import importlib.util as _ilu

    def _load(name):
        spec = _ilu.spec_from_file_location(
            "va_" + name, os.path.join(REPO, "tools", name + ".py"))
        mod = _ilu.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    pvs = _load("pool_value_sequence")
    population = len(pvs.sub100_sections())
    rc, out = run([sys.executable,
                   os.path.join(REPO, "tools", "pool_value_sequence.py"),
                   "--all"], cwd=REPO)
    tail = [l for l in out.splitlines() if l.startswith("population ")]
    accounted = None
    if tail:
        f = tail[-1].split()
        accounted = int(f[1]), int(f[3]) + int(f[7])
    ok = (accounted is not None and accounted[0] == population
          and accounted[1] == population and population > 0)
    bad += not ok
    print("  [%s] pool_value_sequence --all accounts for every row it selected"
          " (population %d, %s)"
          % ("held" if ok else "LOOP TRUNCATED", population,
             "scanned+unscanned %d" % accounted[1] if accounted
             else "NO POPULATION LINE"))

    # ABLATION: make the first row unopenable and prove the loop still reaches
    # the last one.  Without it, "no differing rows" would be unfalsifiable.
    saved = pvs.compare
    seen = []

    def _explode(src_rel, section, quiet=False):
        seen.append(src_rel)
        if len(seen) == 1:
            raise pvs.MissingObject("injected")
        return 0, 0

    pvs.compare = _explode
    try:
        pvs.main_argv = None
        rows = [r for r in pvs.sub100_sections()]
        reached = 0
        for src, sec, _s, _z in rows:
            try:
                pvs.compare(src, sec, quiet=True)
            except pvs.MissingObject:
                pass
            reached += 1
    finally:
        pvs.compare = saved
    ok = reached == len(rows) and len(seen) == len(rows)
    bad += not ok
    print("  [%s] ...and an unopenable row does not end the loop "
          "(%d of %d rows still reached after an injected failure)"
          % ("held" if ok else "ABORTS", reached, len(rows)))

    # IMPORTING A SWEEPER MUST NOT SWEEP.
    #
    # `batch_brute` kept its whole body at module level with no `__main__`
    # guard, so `import batch_brute` -- to read its selector, to reuse its
    # population -- launched a tree-wide `brute_match --apply-best` run that
    # EDITS THE WORKING TREE of whatever repo it was imported from.  That is
    # the mirror of A91's `direct_build` defect (no `__main__`, so the CLI
    # built nothing): the same missing line, opposite blast radius, and no
    # gate in this project can see a sweep that ran when nobody asked.
    #
    # Static, so it costs nothing and cannot itself run anything: a tool that
    # calls out or opens a file for writing at MODULE level, and has no
    # `__main__` guard, is an import-time side effect.
    #
    # The first version of this check only looked for the effect SYNTACTICALLY
    # inside the module-level statement, so a module-level loop that calls a
    # helper defined in the same file -- `for o in objs: rows = secs(o)`, with
    # `subprocess.run` one frame down inside `secs` -- read as clean.  Nine
    # tools had exactly that shape and the check reported zero.  The call graph
    # is followed now, and the write vocabulary covers the pathlib/shutil verbs
    # that never go through `open`.  Names that collide with str/list methods
    # (`replace`, `remove`, `rename`, `copy`, `move`) are deliberately NOT in
    # it: they cost four false offenders and caught nothing.
    WRITE_CALLS = {"run", "check_call", "check_output", "call", "Popen", "system"}
    WRITE_METHODS = {"write_text", "write_bytes", "makedirs", "mkdir",
                     "rmtree", "unlink"}

    def import_side_effects(text):
        try:
            tree = ast.parse(text)
        except SyntaxError:
            return None
        guarded = any(
            isinstance(n, ast.If) and isinstance(n.test, ast.Compare)
            and isinstance(n.test.left, ast.Name) and n.test.left.id == "__name__"
            for n in tree.body)
        if guarded:
            return []
        funcs = {n.name: n for n in ast.walk(tree)
                 if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}

        def effects(node, seen):
            hits = []
            for n in ast.walk(node):
                if not isinstance(n, ast.Call):
                    continue
                f = n.func
                name = f.attr if isinstance(f, ast.Attribute) else \
                    (f.id if isinstance(f, ast.Name) else "")
                if name in WRITE_CALLS or name in WRITE_METHODS:
                    hits.append(name)
                elif name == "open" and any(
                        isinstance(a, ast.Constant) and isinstance(a.value, str)
                        and ("w" in a.value or "a" in a.value) for a in n.args[1:]):
                    hits.append("open-for-write")
                elif isinstance(f, ast.Name) and f.id in funcs and f.id not in seen:
                    hits += effects(funcs[f.id], seen | {f.id})
            return hits

        hits = []
        for node in tree.body:
            if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef,
                                 ast.AsyncFunctionDef, ast.ClassDef)):
                continue
            hits += effects(node, frozenset())
        return hits

    offenders = []
    for fn_ in sorted(os.listdir(tools_dir)):
        if not fn_.endswith(".py") or fn_ == "__init__.py":
            continue
        h = import_side_effects(open(os.path.join(tools_dir, fn_)).read())
        if h:
            offenders.append((fn_, sorted(set(h))))
    ok = not offenders
    bad += not ok
    print("  [%s] no tool runs a subprocess or opens a file for writing at "
          "IMPORT time%s"
          % ("held" if ok else "IMPORT SWEEPS",
             "" if not offenders else "  *** " + ", ".join(
                 "%s%s" % (f, h) for f, h in offenders)))

    # ABLATION -- the check must catch the exact shape that bit us, and must
    # not fire on the guarded form or on a call inside a function.
    shapes = [
        ("unguarded module-level subprocess.run caught",
         "import subprocess\nsubprocess.run(['x'])\n", True),
        ("unguarded module-level open(w) caught",
         "log = open('/tmp/x', 'w')\n", True),
        ("the same body behind a __main__ guard passes",
         "import subprocess\nif __name__ == '__main__':\n    subprocess.run(['x'])\n",
         False),
        ("a call inside a function passes",
         "import subprocess\ndef f():\n    subprocess.run(['x'])\n", False),
        ("a module-level read-only open passes",
         "d = open('/tmp/x')\n", False),
        ("the INDIRECT shape -- a module-level loop calling a local helper "
         "that shells out -- is caught",
         "import subprocess\ndef secs(p):\n    return subprocess.run([p])\n"
         "for p in ['a']:\n    secs(p)\n", True),
        ("...and the same body behind a __main__ guard passes",
         "import subprocess\ndef secs(p):\n    return subprocess.run([p])\n"
         "if __name__ == '__main__':\n    for p in ['a']:\n        secs(p)\n",
         False),
        ("a module-level Path.write_text is caught without any open()",
         "from pathlib import Path\nPath('/tmp/x').write_text('y')\n", True),
        ("...and two mutually recursive helpers do not hang the walk",
         "def a():\n    b()\ndef b():\n    a()\na()\n", False),
        ("a module-level str.replace is NOT a filesystem write",
         "s = 'a'.replace('a', 'b')\n", False),
    ]
    misread = [lbl for lbl, text, want in shapes
               if bool(import_side_effects(text)) != want]
    ok = not misread
    bad += not ok
    print("  [%s] ...and the import-side-effect scan reads all %d injected "
          "shapes correctly%s"
          % ("held" if ok else "BLIND", len(shapes),
             "" if not misread else "  *** " + "; ".join(misread)))
    return bad


def family_report(verbose):
    """Vacuities in the SCORE ITSELF -- reported, because they cannot be fixed
    from here, and a number nobody can fix is exactly the one people forget is
    not evidence."""
    print("\n=== THE REPORT AS AN INSTRUMENT ===")
    rep = json.load(open(os.path.join(REPO, "build", "GSAE01",
                                      "report.json")))
    cfg = json.load(open(os.path.join(REPO, "objdiff.json")))
    have = {u["name"]: bool(u.get("base_path")) for u in cfg["units"]}
    blind = [u for u in rep["units"] if not have.get(u["name"], True)]
    at100 = [u for u in blind
             if u["measures"].get("fuzzy_match_percent") == 100.0]
    print("  units in the report: %d;  with NO ours-side object: %d, of which "
          "%d read 100.0" % (len(rep["units"]), len(blind), len(at100)))
    autocomp = sum(1 for u in blind if u["measures"].get("complete_units"))
    print("     -> those %d are compared against THEMSELVES.  Their 100.0 is a "
          "tautology,\n        and they are inside total_units (%d of them are "
          "inside complete_units)." % (len(at100), autocomp))
    zero = sum(1 for u in rep["units"]
               for f in (u.get("functions") or [])
               if f.get("fuzzy_match_percent") is None)
    print("  functions carrying NO fuzzy_match_percent at all: %d "
          "(objdiff omits the field at 0.0, so a MISSING score must be read "
          "as -1, never as absent)" % zero)

    # THE COMPLETION FIGURE'S OWN DENOMINATOR AND NUMERATOR, both of which
    # contain rows that are not statements about correctness.
    #
    #   * AUTO-GENERATED units carry `total_data` and no `total_code` at all --
    #     they are dtk's data-only carve residue, never ours to match, and they
    #     are never complete.  They belong in neither half.
    #   * VACUOUS units carry NO total_code, NO total_data and NO total_functions
    #     -- their carve object is empty, so objdiff compared nothing and
    #     returned 100.0/complete.  Two of them (OSExec, synth_seq_queue) have a
    #     real translation unit whose whole .text the retail link never pulled
    #     in; three are stub .c files that emit nothing.  Either way the carve
    #     can never gain content, so such a row can neither be earned nor lost:
    #     it is excluded from BOTH halves, not just the numerator.
    #
    # This is stated here rather than left as folklore because the raw
    # `complete_units/total_units` pair reads as an achievable score and is not.
    us = rep["units"]
    auto = [u for u in us if u.get("metadata", {}).get("auto_generated")]
    vac = [u for u in us
           if not u["measures"].get("total_code")
           and not u["measures"].get("total_data")
           and not u["measures"].get("total_functions")]
    comp = [u for u in us if u["measures"].get("complete_units")]
    vacnames = {u["name"] for u in vac}
    inform_num = len([u for u in comp if u["name"] not in vacnames])
    inform_den = len(us) - len(auto) - len(vac)
    print("  complete_units as reported: %d of %d" % (len(comp), len(us)))
    print("     -- %d auto-generated data-only units (never ours to match)"
          % len(auto))
    print("     -- %d VACUOUS units: no code, no data, no functions, so their "
          "100.0/complete\n        compared nothing: %s"
          % (len(vac), ", ".join(sorted(u["name"] for u in vac))))
    print("     => INFORMATIVE completion: %d of %d" % (inform_num, inform_den))
    # ...and the controls, because an exclusion nobody can see fail is folklore
    # with a line number.  The predicate must FIRE, must not swallow the tree,
    # and must not swallow the auto-generated units -- those carry total_data,
    # which is exactly what tells the two populations apart.
    ok = (0 < len(vac) < len(us)
          and not (vacnames & {u["name"] for u in auto})
          and all(u["measures"].get("complete_units") for u in vac))
    print("  vacuity predicate: %s  (fires on %d, silent on %d, disjoint from "
          "auto-generated, every row complete)"
          % ("held" if ok else "DIVERGES", len(vac), len(us) - len(vac)))
    return 0 if ok else 1


_PERTURB = """
import sys;sys.path.insert(0,'tools')
from pathlib import Path
import brute_match as B
from function_objdump import load_units, resolve_unit
u=resolve_unit(load_units(Path('build/GSAE01/config.json')), %r)
f=Path('src')/u['name']
src=f.read_bytes().decode('latin-1')
o,e=B.find_function_body(src, %r)
bl=B.collect_decl_blocks(src,o,e)[0]
order=list(range(len(bl['items']))); order[%d],order[%d]=order[%d],order[%d]
it=bl['items']
first=it[order[0]].lstrip()
rest=''.join('\\n'+bl['indent']+it[k].lstrip() for k in order[1:])
f.write_bytes((src[:bl['start']]+first+rest+src[bl['end']:]).encode('latin-1'))
"""


def family_oracle(verbose):
    """POSITIVE CONTROL for tools/slot_oracle.py.

    Swept over the sub-100 rows the oracle answers "pi not realisable within
    this block" essentially every time.  A uniform verdict is exactly the shape
    a broken instrument produces, so the census means nothing until the oracle
    has been seen saying something else AND being right.

    Perturb a function that is already at 100.0 by swapping two of its
    declarations: our object now differs from retail by precisely the register
    transposition that swap caused, which is a pi that IS realisable and whose
    solution is the swap back.  (An unperturbed 100.0 function is useless --
    pi is empty and the oracle short-circuits before it probes anything, which
    is why the perturbation has to come first.)

    NOTE: writes into src/ and rebuilds.  Restores from HEAD after every probe.
    Do not run against a worktree with a sweep in flight.
    """
    print("\n=== POSITIVE CONTROL: tools/slot_oracle.py ===")
    rep = json.load(open(os.path.join(REPO, "build/GSAE01/report.json")))
    cfg = json.load(open(os.path.join(REPO, "build/GSAE01/config.json")))
    names = {u["name"] for u in cfg["units"]}
    cands = []
    for u in rep["units"]:
        for f in u.get("functions") or []:
            if f.get("fuzzy_match_percent") != 100.0:
                continue
            if not (1200 <= int(f.get("size", 0)) <= 6000):
                continue
            base = "/".join(u["name"].split("/")[1:]) + ".c"
            if base in names and "dolphin" not in base and "musyx" not in base:
                cands.append((base, f["name"], int(f["size"])))
    cands.sort(key=lambda r: -r[2])

    def restore(rel):
        g = subprocess.run(["git", "show", "HEAD:src/" + rel], cwd=REPO,
                           capture_output=True)
        if g.returncode == 0:
            open(os.path.join(REPO, "src", rel), "wb").write(g.stdout)
        b = os.path.join(REPO, "src", rel) + ".brutebak"
        if os.path.exists(b):
            os.unlink(b)

    tried = 0
    for cfgname, fn, size in cands:
        r = run([PY, os.path.join(REPO, "tools", "brute_match.py"),
                 cfgname, fn, "--list-blocks"])
        m = re.search(r"block 0: depth=\d+ line \d+-\d+\s+(\d+) items", r[1])
        if not m or int(m.group(1)) < 6:
            continue
        n = int(m.group(1))
        for (i, j) in ((0, 1), (0, 2), (1, 2), (2, 3)):
            tried += 1
            if tried > 24:
                print("  *** no subject produced a positive control in 24 "
                      "perturbations ***")
                return 1
            w = run([PY, "-c", _PERTURB % (cfgname, fn, i, j, j, i)])
            if w[0]:
                restore(cfgname)
                continue
            rc, txt = run([PY, os.path.join(REPO, "tools", "slot_oracle.py"),
                           cfgname, fn])
            restore(cfgname)
            b = re.search(r"baseline fuzzy=([\d.]+)", txt)
            if not b or float(b.group(1)) >= 100.0:
                continue                    # the swap was inert; try another
            pi = bool(re.search(r"# pi \(saved band\): \{.+\}", txt))
            unreal = "not realisable" in txt
            sol = re.search(r"SOLVED order \[[^\]]*\] -> fuzzy=([\d.]+)", txt)
            back = bool(sol) and abs(float(sol.group(1)) - 100.0) < 1e-9
            print("  %s %s: perturbed decl %d<->%d, fuzzy 100.0 -> %s;  "
                  "pi non-empty=%s  unrealisable=%s  solved-back-to-100=%s"
                  % (cfgname, fn, i, j, b.group(1), pi, unreal, back))
            if pi and not unreal and back:
                print("  [held] the oracle CAN report a realisable pi, and the "
                      "ordering it computes is the right one")
                return 0
    print("  *** NO SUBJECT PRODUCED A POSITIVE CONTROL ***")
    return 1


def family_parser(verbose):
    """The DECLARATION PARSER every ordering sweep permutes through.

    `brute_match.looks_like_decl` decides what a "declaration block" contains,
    and `brute_match`, `slot_oracle`, `stmt_sweep`, `perm_solve`, `deep_decl`,
    `expr_sweep` and `decl_split_sweep` all permute exactly what it hands them.
    If it accepts a STATEMENT, the sweepers reorder side-effecting stores
    against each other -- and NO gate in this project can see that, because
    objdiff fuzzy, obj_equal, score_delta_gate and the forced link all compare
    retail's BYTES and never its MEANING.  A reordered pair of stores that
    happened to score better would have landed as "a declaration ordering".

    So the parser needs a control of its own: subjects that MUST be refused,
    and an ablation that removes the guard and shows the refusal was the guard
    doing work rather than something else declining them anyway.
    """
    print("\n=== PARSER (what the ordering sweeps are allowed to permute) ===")
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import brute_match as B                                   # noqa: E402
    bad = 0

    must_refuse = [
        "tri->edgeOutBits = 0;",
        "cfg.startPosX += spawnParams->posX;",
        "slot->scaleTarget = slot->scaleCurrent;",
        "obj->anim.localPosX = knockbackDistance * -obj->anim.velocityX;",
        "state->dirX /= length;",
    ]
    must_accept = [
        "int x;",
        "GameObject* obj = p;",
        "f32 *a, *b;",
        "u8 buf[4];",
        "ShaderRomListSlot* romListSlot = (ShaderRomListSlot*)(base + 4) + i;",
    ]
    ref = [s for s in must_refuse if B.looks_like_decl(s)]
    acc = [s for s in must_accept if not B.looks_like_decl(s)]
    bad += bool(ref) + bool(acc)
    print("  [%s] a member-access STORE is not a declaration  (%d/%d refused)"
          % ("held" if not ref else "ACCEPTED", len(must_refuse) - len(ref),
             len(must_refuse)))
    for s in ref:
        print("        *** accepted: %s" % s)
    print("  [%s] a real declaration still parses  (%d/%d accepted)"
          % ("held" if not acc else "REFUSED", len(must_accept) - len(acc),
             len(must_accept)))
    for s in acc:
        print("        *** refused: %s" % s)

    # ABLATION: remove the member-access guard and require the subjects to be
    # accepted again.  If they are still refused, something else is declining
    # them and the guard is decorative.
    src = open(os.path.join(REPO, "tools", "brute_match.py")).read()
    guard = 'if "->" in head or "." in head:\n        return False'
    if guard not in src:
        print("  [MISSING] the member-access guard is not in brute_match.py")
        return bad + 1
    ns = {"__file__": os.path.join(REPO, "tools", "brute_match.py"),
          "__name__": "brute_match_ablated"}
    exec(compile(src.replace(guard, "pass"), "<ablated>", "exec"), ns)
    ablated = ns["looks_like_decl"]
    revived = [s for s in must_refuse if ablated(s)]
    ok = len(revived) == len(must_refuse)
    bad += not ok
    print("  [%s] ablating the guard makes the parser accept all %d stores "
          "again (%d)" % ("held" if ok else "VACUOUS", len(must_refuse),
                          len(revived)))

    # THE SAME HAZARD, ONE STEP PAST THE PARSER.  Everything above asks "is
    # this a declaration".  What the sweeps actually depend on is "is this
    # permutation semantics-preserving", and for a declaration whose
    # INITIALISER calls something the two come apart: it is a declaration by
    # every test the parser applies, and swapping two of them changes the
    # computation exactly the way a swapped pair of stores would.  So the
    # detector that guards the apply gate gets its own subjects.
    must_flag = [
        "s32 rnd1 = randomGetRange(0, 0x1e) * 2;",
        "s32 rnd2 = randomGetRange(0, 0x1e) * 2;",
    ]
    must_not_flag = [
        "int i;",
        "u64 x = (u32)(p);",
        "int n = a->count;",
        "f32 v = base[i] * 0.5f;",
    ]
    got = [B.side_effect_reorders(must_flag, [1, 0]),
           B.side_effect_reorders(must_flag, [0, 1])]
    ok = got[0] == [(0, 1)] and got[1] == []
    bad += not ok
    print("  [%s] swapping two PRNG-draw initialisers is flagged, and the "
          "identity is not  (%s)" % ("held" if ok else "BLIND", got))
    quiet = [t for t in must_not_flag if B.initialiser_calls(t)]
    ok = not quiet
    bad += not ok
    print("  [%s] a plain declaration, a cast and a field read carry no call "
          "(%d/%d quiet)" % ("held" if not quiet else "FALSE POSITIVE",
                             len(must_not_flag) - len(quiet), len(must_not_flag)))
    for t in quiet:
        print("        *** flagged: %s" % t)

    # THE CALLS THE DETECTOR COULD NOT SEE.  Only the `(` tells a member CALL
    # apart from a member READ, so a lookbehind that skipped an identifier
    # preceded by `>` or `.` went blind to every indirect dispatch -- and this
    # tree dispatches its DLL interfaces exactly that way.  Those are the calls
    # whose purity is LEAST establishable from source, so they are the last
    # ones a conservative detector may miss.
    indirect = [
        "int* cam = (int*)(*gCameraInterface)->getCamera();",
        "PauseMenuCharacterState* s = mapEvents->getCurCharacterState();",
        "int pt = (int)(*gRomCurveInterface)->getById(found);",
        "f32 v = iface.getValue(obj);",
        "int r = handlers[i].run(a);",
    ]
    unseen = [t for t in indirect if not B.initialiser_calls(t)]
    ok = not unseen
    bad += not ok
    print("  [%s] an INDIRECT call through a struct member is a call "
          "(%d/%d seen)" % ("held" if ok else "BLIND",
                            len(indirect) - len(unseen), len(indirect)))
    for t in unseen:
        print("        *** missed: %s" % t)

    # ABLATION: restore the lookbehind that excluded `>` and `.` and require
    # every one of those subjects to go dark again, so the detection above is
    # the fix doing work and not the regex being permissive for another reason.
    saved_re = B.CALL_TOKEN_RE
    try:
        B.CALL_TOKEN_RE = re.compile(r"(?<![\w>.])([A-Za-z_]\w*)\s*\(")
        went_dark = [t for t in indirect if not B.initialiser_calls(t)]
        still_seen = B.initialiser_calls("int w = plainCall(1);")
    finally:
        B.CALL_TOKEN_RE = saved_re
    ok = len(went_dark) == len(indirect) and still_seen == ["plainCall"]
    bad += not ok
    print("  [%s] the pre-fix lookbehind misses all %d of them while still "
          "seeing a direct call (%d dark)"
          % ("held" if ok else "VACUOUS", len(indirect), len(went_dark)))

    # ABLATION: a detector that never finds a call reports nothing at all and
    # looks exactly like a clean tree, so blind the call scanner and require
    # the flag to disappear.
    saved_re = B.CALL_TOKEN_RE
    try:
        B.CALL_TOKEN_RE = re.compile(r"(?!x)x")
        blinded = B.side_effect_reorders(must_flag, [1, 0])
    finally:
        B.CALL_TOKEN_RE = saved_re
    ok = blinded == []
    bad += not ok
    print("  [%s] blinding the call scanner makes the flag vanish, so the "
          "flag comes from the CALL (%s)"
          % ("held" if ok else "VACUOUS", blinded))

    # THE OTHER THING A SWEEP CAN DESTROY: A PEER'S EDIT.
    #
    # Every sweeper writes variants into a real source file and restores at
    # exit.  `stmt_sweep` compares the on-disk bytes against the bytes IT last
    # wrote before restoring, so a peer lane that edited the same file inside
    # the sweep window is not reverted; the copy in `brute_match` -- the tool
    # the fleet actually runs, and the one `decl_split_sweep` imports -- did
    # not, and silently overwrote the peer with its own pristine copy.  No
    # score gate can see a lost edit either, so the guard gets subjects.
    import tempfile as _tf
    from pathlib import Path as _Path
    for mod_name, mod in (("brute_match", B),
                          ("stmt_sweep", __import__("stmt_sweep"))):
        d = _tf.mkdtemp(prefix="vac_guard_")
        f = os.path.join(d, "probe.c")
        orig = b"int a;\n"
        open(f, "wb").write(orig)
        got = mod.install_restore_guard(_Path(f), orig)
        w, _k = got if isinstance(got, tuple) else (None, got)
        arity_ok = w is not None
        peer_refused = own_ok = False
        if arity_ok:
            own_ok = w(b"int b;\n") and open(f, "rb").read() == b"int b;\n"
            open(f, "wb").write(b"/* peer lane */\nint a;\n")   # peer edit
            peer_refused = (w(b"int c;\n") is False
                            and open(f, "rb").read() == b"/* peer lane */\nint a;\n")
        for ok_, msg in (
                (arity_ok, "%s's restore guard hands back a tracked writer"
                           % mod_name),
                (own_ok, "  ...which writes while the file still holds our bytes"),
                (peer_refused, "  ...and REFUSES once a peer has edited it")):
            bad += not ok_
            print("  [%s] %s" % ("held" if ok_ else "CLOBBERS", msg))
        shutil.rmtree(d, ignore_errors=True)

    # And the gate must be wired into both sweepers, not merely defined.
    wired = 0
    for tool in ("brute_match.py", "decl_split_sweep.py"):
        txt = open(os.path.join(REPO, "tools", tool)).read()
        if "report_side_effect_reorders" in txt and \
           "allow_side_effect_reorder" in txt:
            wired += 1
    ok = wired == 2
    bad += not ok
    print("  [%s] the apply gate is wired into both sweepers (%d/2)"
          % ("held" if ok else "DEFINED BUT UNUSED", wired))

    # And the ablation must be VISIBLE where it matters: a block whose stores
    # are order-dependent.  `slot->scaleCurrent = v; slot->scaleTarget =
    # slot->scaleCurrent;` reads a value the previous store wrote, so swapping
    # the two changes the computation -- which is the whole point.
    dependent = ("    slot->scaleCurrent = scaleVal;\n"
                 "    slot->scaleTarget = slot->scaleCurrent;\n")
    body = "void f(void) {\n" + dependent + "    g();\n}\n"
    o, e = B.find_function_body(body, "f")
    now = B.collect_decl_blocks(body, o, e)
    then = ns["collect_decl_blocks"](body, o, e)
    ok = (not now) and len(then) == 1 and len(then[0]["items"]) == 2
    bad += not ok
    print("  [%s] an ORDER-DEPENDENT pair of stores is no longer a permutable "
          "block (was %d item(s), now %d)"
          % ("held" if ok else "STILL PERMUTABLE",
             len(then[0]["items"]) if then else 0,
             len(now[0]["items"]) if now else 0))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--family",
                    choices=("scope", "object", "delegate", "build",
                             "report", "oracle", "parser"),
                    action="append")
    ap.add_argument("--unit-object",
                    default="build/GSAE01/obj/main/camera.o",
                    help="unit the BUILD family rebuilds "
                         "(must not be under a live sweep)")
    ap.add_argument("--unit", default="main/main/render",
                    help="unit whose objects the OBJECT family damages")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.list:
        print("scope:    " + " ".join(SCOPE_SCREENS))
        print("object:   " + " ".join(sorted(OBJECT_SCREENS)))
        print("delegate: " + " ".join(t for t, _ in DELEGATED))
        print("build:    direct_build unitfuzzy   (opt-in: writes objects)")
        print("report:   build/GSAE01/report.json")
        print("oracle:   slot_oracle              (opt-in: writes source)")
        print("parser:   brute_match.looks_like_decl -- what every ordering "
              "sweep is allowed to permute")
        return 0
    fams = a.family or ["scope", "object", "delegate", "report",
                        "parser"]
    bad = 0
    if "scope" in fams:
        bad += family_scope(a.verbose)
    if "object" in fams:
        bad += family_object(a.unit, a.verbose)
    if "delegate" in fams:
        bad += family_delegate(a.verbose)
    if "build" in fams:
        bad += family_build(a.unit_object, a.verbose)
    if "report" in fams:
        bad += family_report(a.verbose)
    if "oracle" in fams:
        bad += family_oracle(a.verbose)
    if "parser" in fams:
        bad += family_parser(a.verbose)
    print("\nvacuity audit: %s" % ("ALL INSTRUMENTS SCREAMED" if not bad
                                   else "%d INSTRUMENT(S) STAYED SILENT" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
