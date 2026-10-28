#!/usr/bin/env python3
"""Solve a colouring row's declaration order from the diff instead of searching it.

The callee-saved band is CONTIGUOUS -- [r(32-k)..r31] / [f(32-k)..f31] -- and the
k saved webs map onto it MONOTONICALLY in source DECLARATION order (measured:
tools lab, 24/24 definition-order permutations leave the assignment
byte-identical, while every declaration permutation moves it).  So the set of
register assignments reachable from declaration order is the full symmetric
group on the band, and the ordering that reaches retail's assignment is not
something to search -- it is the permutation the diff already states.

Given the aligned target/current instruction streams, every same-mnemonic
instruction pair yields register correspondences ours->retail.  Collect them
into a permutation pi, then reorder the declaration block by pi (or pi^-1,
under an ascending or a descending band, and per register class), which is at
most a handful of candidates instead of n!.

Usage: perm_solve.py <unit> <symbol> [-v GSAE01] [--class f|r|both] [--apply]
"""
from __future__ import annotations
import argparse, difflib, re, sys
from collections import Counter, defaultdict
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, resolve_unit
from brute_match import (REPO, collect_decl_blocks, find_function_body, find_objdump,
                         fuzzy_measure, objdump_norm, objdump_paths, rebuild,
                         recover_stale_backup)

TOK = re.compile(r"\b([rf])(\d+)\b")


def corr(t: list[str], c: list[str], cls: str):
    """ours-register -> Counter(retail-register), from aligned same-mnemonic pairs."""
    sm = difflib.SequenceMatcher(None, t, c, autojunk=False)
    m = defaultdict(Counter)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if i2 - i1 != j2 - j1:
            continue
        for a, b in zip(t[i1:i2], c[j1:j2]):
            if a.startswith("RELOC") or b.startswith("RELOC"):
                continue
            if a.split(None, 1)[0] != b.split(None, 1)[0]:
                continue
            ta = TOK.findall(a); tb = TOK.findall(b)
            if len(ta) != len(tb):
                continue
            # the non-register skeleton must agree, else the pair is not aligned
            if TOK.sub("X", a) != TOK.sub("X", b):
                continue
            for (ka, va), (kb, vb) in zip(ta, tb):
                if ka != kb or ka != cls:
                    continue
                m[int(vb)][int(va)] += 1     # ours(b) -> retail(a)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit"); ap.add_argument("symbol")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--cls", choices=["f", "r", "both"], default="both")
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    unit = resolve_unit(load_units(REPO / "build" / a.version / "config.json"), a.unit)
    src_file = REPO / "src" / unit["name"].replace("\\", "/")
    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")
    body = find_function_body(src, a.symbol)
    if not body:
        raise SystemExit(f"PARSE-ERROR could not locate {a.symbol}")
    blocks = collect_decl_blocks(src, body[0], body[1])
    if not blocks:
        raise SystemExit("PARSE-ERROR no declaration block")

    def render(orders):
        out = src
        for bi in sorted(orders, reverse=True):
            b = blocks[bi]; it = b["items"]; o = orders[bi]
            first = it[o[0]].lstrip()
            rest = "".join("\n" + b["indent"] + it[k].lstrip() for k in o[1:])
            out = out[:b["start"]] + first + rest + out[b["end"]:]
        return out

    objdump = find_objdump()
    tgt_o, cur_o = objdump_paths(unit, a.version)
    rebuild(unit["object"], a.version)
    t = objdump_norm(objdump, tgt_o, a.symbol)
    c = objdump_norm(objdump, cur_o, a.symbol)
    base = fuzzy_measure(unit, a.symbol, a.version)
    print(f"# {a.symbol} baseline fuzzy={base:.4f}")

    classes = ["f", "r"] if a.cls == "both" else [a.cls]
    perms = {}
    for cl in classes:
        m = corr(t, c, cl)
        pi = {}
        for ours, cnt in sorted(m.items()):
            tgt, n = cnt.most_common(1)[0]
            tot = sum(cnt.values())
            pi[ours] = (tgt, n, tot)
        band = {k: v for k, v in pi.items() if k >= 14 and v[0] >= 14 and v[0] != k}
        print(f"# class {cl}: saved-band moves {len(band)}: "
              + ", ".join(f"{cl}{k}->{cl}{v[0]}({v[1]}/{v[2]})" for k, v in sorted(band.items())))
        perms[cl] = {k: v[0] for k, v in pi.items() if k >= 14 and v[0] >= 14}

    # candidate declaration orderings induced by the permutation
    cands = {}
    for bi, b in enumerate(blocks):
        n = len(b["items"])
        if n < 2:
            continue
        for cl in classes:
            p = perms[cl]
            if not p or all(k == v for k, v in p.items()):
                continue
            lo, hi = min(p), max(p)
            k = hi - lo + 1
            if k > n:
                continue
            for asc in (True, False):
                for inv in (False, True):
                    # declaration slot d (0..n-1) currently owns register:
                    #   asc : lo + d       desc: hi - d
                    # after the move it must own pi(that) -- so the item at slot d
                    # goes to the slot that owns pi(reg).
                    order = list(range(n))
                    newslot = {}
                    ok = True
                    for d in range(n):
                        reg = (lo + d) if asc else (hi - d)
                        if reg not in p:
                            newslot[d] = d; continue
                        tgt = p[reg] if not inv else \
                              next((kk for kk, vv in p.items() if vv == reg), reg)
                        s = (tgt - lo) if asc else (hi - tgt)
                        if not (0 <= s < n):
                            ok = False; break
                        newslot[d] = s
                    if not ok or len(set(newslot.values())) != n:
                        continue
                    out = [None] * n
                    for d, s in newslot.items():
                        out[s] = d
                    if any(x is None for x in out) or tuple(out) == tuple(range(n)):
                        continue
                    cands[(bi, cl, asc, inv)] = tuple(out)

    print(f"# {len(cands)} induced candidate ordering(s)")
    best = (base, None)
    try:
        for key, o in cands.items():
            src_file.write_bytes(render({key[0]: o}).encode("latin-1"))
            if not rebuild(unit["object"], a.version):
                continue
            fz = fuzzy_measure(unit, a.symbol, a.version)
            flag = "  <== BETTER" if fz > base + 1e-4 else ""
            print(f"  {key} {list(o)} fuzzy={fz:.4f}{flag}")
            if fz > best[0] + 1e-4:
                best = (fz, (key[0], o))
    finally:
        src_file.write_bytes(original)
    if a.apply and best[1]:
        src_file.write_bytes(render({best[1][0]: best[1][1]}).encode("latin-1"))
        rebuild(unit["object"], a.version)
        print(f"# APPLIED: {base:.4f} -> {fuzzy_measure(unit, a.symbol, a.version):.4f}")
    else:
        rebuild(unit["object"], a.version)
    print(f"#BASE={base:.4f} BEST={best[0]:.4f}")


if __name__ == "__main__":
    main()
