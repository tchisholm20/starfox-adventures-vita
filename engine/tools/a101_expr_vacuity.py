#!/usr/bin/env python3
"""How many of `expr_sweep`'s cleared rewrites present the allocator a CHOICE?

`expr_sweep --greedy` reported 381 semantically-cleared rewrites over 12
volatile-only float rows and 0 hits, and the class was read as closed.  A zero
is only evidence if the probes could have been non-zero.  On a FLOAT row the
tool's live transforms are `commute`, `relflip`, `ternflip` and `sign`;
`assoc` is INTEGER-ONLY by construction (`sem.ASSOCIATIVE_INT`) and opt-in, and
float re-association is not semantics-preserving, so the gate is right to
refuse it.  But a synthetic control measured that commuting a float product is
absorbed by the front end and compiles BYTE-IDENTICALLY, while it is exactly
re-association / term order that moves the value->register assignment.

If that held on real code, the sweep's zero would be a VACUITY result rather
than a closure.  MEASURED: it does NOT hold -- on `mtxRotateByVec3s` 41 of 41
cleared rewrites move the object and 0 are byte-identical.  The synthetic's
independent loads were canonicalised by the front end; real bodies are not.
`expr_sweep`'s zero on the float rows is genuine, and this tool is kept as the
instrument that would catch a future sweep whose zero is not.

This reuses `expr_sweep`'s OWN region finder, parser, variant generator and
semantic gate -- so the candidate set is exactly the one the sweep would have
compiled -- then compiles each cleared variant and compares the OBJECT BYTES,
not the score.  A score can hold while the bytes move; only byte-identity
proves nothing could have moved.

Usage:  python3 tools/a101_expr_vacuity.py <unit> <symbol> [--limit N]
        python3 tools/a101_expr_vacuity.py --rows FILE
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cexpr
import expr_sweep as ES
import semantic_equivalence as sem
from cexpr import Ambiguous, ParseError, parse_expression
from brute_match import find_function_body, recover_stale_backup
from function_objdump import load_units
from a101_probe import unit_objs, score
from direct_build import direct_build

REPO = Path(__file__).resolve().parent.parent


def digest(p):
    return hashlib.sha1(Path(p).read_bytes()).hexdigest()


def cleared_variants(src, unit_name, symbol):
    """Exactly `expr_sweep`'s gated candidate list for this function."""
    body = find_function_body(src, symbol)
    if not body:
        return None, "no function body"
    dtext = ES.decl_text_of(src, body, symbol)
    oracle = sem.TypeOracle(dtext)
    known_ids = frozenset(oracle.kind) | frozenset(
        re.findall(r"\b([a-z]\w*)\s*(?:=[^=]|\+\+|--|\[)", dtext)) | \
        cexpr.project_objects()
    ktypes = cexpr.project_typedefs()
    regions = ES.expression_regions(src, body[0], body[1])
    out, nfail = [], 0
    for reg in regions:
        try:
            root = parse_expression(src, known_ids, ktypes, reg.start, reg.end)
        except (Ambiguous, ParseError):
            nfail += 1
            continue
        for v in ES.variants(root, src, oracle, False, "none"):
            new_region = (src[reg.start:v.node.start] + v.new_sub +
                          src[v.node.end:reg.end])
            vd = sem.prove(src[reg.start:reg.end], new_region,
                           known_types=ktypes, decl_text=dtext,
                           known_ids=known_ids, allow_assoc=False, trials=64)
            if vd.ok:
                out.append((reg, v, new_region))
    return (out, nfail), None


def run(unit_name, symbol, limit=None):
    src_file = REPO / "src" / unit_name
    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")
    got, err = cleared_variants(src, unit_name, symbol)
    if err:
        print(f"  {unit_name} {symbol}: {err}")
        return None
    cands, nfail = got
    _, ou = unit_objs(unit_name)
    rel = str(ou.relative_to(REPO))
    direct_build(rel)
    base = digest(ou)
    print(f"# {unit_name}  {symbol}: {len(cands)} cleared rewrites, "
          f"{nfail} regions unparsed, base obj {base[:12]}")
    nv = nm = nb = 0
    rules = {}
    try:
        for reg, v, new_region in (cands[:limit] if limit else cands):
            cand = src[:reg.start] + new_region + src[reg.end:]
            src_file.write_bytes(cand.encode("latin-1"))
            if not direct_build(rel):
                nb += 1
                src_file.write_bytes(original)
                continue
            d = digest(ou)
            r = rules.setdefault(v.rule, [0, 0])
            if d == base:
                nv += 1
                r[0] += 1
            else:
                nm += 1
                r[1] += 1
                print(f"   MOVES OBJECT  [{v.rule}]  {v.label[:64]}")
            src_file.write_bytes(original)
    finally:
        src_file.write_bytes(original)
        assert src_file.read_bytes() == original
        direct_build(rel)
    print(f"   -> {nv} BYTE-IDENTICAL (vacuous), {nm} moved the object, "
          f"{nb} build-failed")
    for k, (a, b) in sorted(rules.items()):
        print(f"      {k:9s} vacuous {a:4d}  moved {b:4d}")
    return nv, nm, nb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit", nargs="?")
    ap.add_argument("sym", nargs="?")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--rows", help="file of `unit<TAB>symbol` lines")
    a = ap.parse_args()
    rows = []
    if a.rows:
        for line in Path(a.rows).read_text().split("\n"):
            if line.strip() and not line.startswith("#"):
                rows.append(tuple(line.split()[:2]))
    else:
        rows = [(a.unit, a.sym)]
    tv = tm = tb = 0
    for u, s in rows:
        r = run(u, s, a.limit)
        if r:
            tv += r[0]
            tm += r[1]
            tb += r[2]
    print(f"\nTOTAL {tv + tm + tb} rewrites compiled: {tv} VACUOUS "
          f"(byte-identical), {tm} moved the object, {tb} build-failed")


if __name__ == "__main__":
    main()
