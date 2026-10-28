#!/usr/bin/env python3
"""MEASURE the declaration-slot -> saved-register map, then SOLVE the ordering.

Declaration order is the only source key for the callee-saved band (measured:
every one of 24 definition-order permutations leaves the assignment
byte-identical, for GPRs and FPRs alike, while declaration permutations move
it).  But WHICH slot owns WHICH register is not guessable -- most declarations
never get a saved home at all.  It is, however, directly measurable: swapping
two adjacent declarations transposes exactly the two registers they own, and
that transposition is visible as a register substitution between our own
baseline object and the probe's.

  n-1 adjacent-swap probes  ->  the complete slot -> register map
  the target diff           ->  the permutation pi (ours -> retail)
  one more probe            ->  the ordering that realises pi

which is O(n) probes instead of the O(n^2) radius-1 neighbourhood, and it
computes the answer rather than searching for it.

Usage: slot_oracle.py <unit> <symbol> [-v GSAE01] [--block N] [--apply]
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


def subst(a_stream, b_stream):
    """register substitutions a->b over aligned identical-skeleton pairs."""
    sm = difflib.SequenceMatcher(None, a_stream, b_stream, autojunk=False)
    m = defaultdict(Counter)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if i2 - i1 != j2 - j1:
            continue
        for a, b in zip(a_stream[i1:i2], b_stream[j1:j2]):
            if a.startswith("RELOC") or b.startswith("RELOC"):
                continue
            if a.split(None, 1)[0] != b.split(None, 1)[0]:
                continue
            ta, tb = TOK.findall(a), TOK.findall(b)
            if len(ta) != len(tb) or TOK.sub("X", a) != TOK.sub("X", b):
                continue
            for (ka, va), (kb, vb) in zip(ta, tb):
                if ka == kb:
                    m[ka + va][ka + vb] += 1
    return m


def transposition(m):
    """If the substitution is exactly one register pair swapped, return it."""
    moved = {k: v.most_common(1)[0][0] for k, v in m.items()
             if v.most_common(1)[0][0] != k}
    if len(moved) == 2:
        (a, x), (b, y) = sorted(moved.items())
        if x == b and y == a:
            return (a, b)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit"); ap.add_argument("symbol")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--block", type=int, default=None)
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

    def render(bi, order):
        b = blocks[bi]; it = b["items"]
        first = it[order[0]].lstrip()
        rest = "".join("\n" + b["indent"] + it[k].lstrip() for k in order[1:])
        return src[:b["start"]] + first + rest + src[b["end"]:]

    objdump = find_objdump()
    tgt_o, cur_o = objdump_paths(unit, a.version)
    rebuild(unit["object"], a.version)
    T = objdump_norm(objdump, tgt_o, a.symbol)
    C0 = objdump_norm(objdump, cur_o, a.symbol)
    base = fuzzy_measure(unit, a.symbol, a.version)
    print(f"# {a.symbol} baseline fuzzy={base:.4f}  blocks={len(blocks)}")

    # target permutation pi: ours -> retail, restricted to the saved band
    pm = subst(C0, T)
    pi = {}
    for k, cnt in pm.items():
        v, n = cnt.most_common(1)[0]
        if v != k and int(k[1:]) >= 14 and int(v[1:]) >= 14 and k[0] == v[0]:
            pi[k] = v
    print(f"# pi (saved band): { {k: v for k, v in sorted(pi.items())} }")
    if not pi:
        print("#BASE=%.4f BEST=%.4f  NO-SAVED-BAND-PERMUTATION" % (base, base))
        rebuild(unit["object"], a.version)
        return

    sel = range(len(blocks)) if a.block is None else [a.block]
    best = (base, None)
    try:
        for bi in sel:
            n = len(blocks[bi]["items"])
            if n < 2:
                continue
            ident = list(range(n))
            pairs = {}
            nonperm = []
            for k in range(n - 1):
                o = list(ident); o[k], o[k + 1] = o[k + 1], o[k]
                src_file.write_bytes(render(bi, o).encode("latin-1"))
                if not rebuild(unit["object"], a.version):
                    continue
                Ck = objdump_norm(objdump, cur_o, a.symbol)
                sb = subst(C0, Ck)
                tp = transposition(sb)
                if tp:
                    pairs[k] = tp
                elif Ck != C0:
                    moved = {x: v.most_common(1)[0][0] for x, v in sb.items()
                             if v.most_common(1)[0][0] != x}
                    nonperm.append((k, len(Ck) - len(C0), moved))
            if nonperm:
                print(f"# block {bi}: {len(nonperm)} non-transposition swaps, e.g. "
                      + "; ".join(f"swap{k}(dlen={dl}){m}" for k, dl, m in nonperm[:3]))
            if not pairs:
                print(f"# block {bi}: no slot owns a register "
                      f"({len(nonperm)} swaps changed the object non-transpositionally)")
                continue
            # chain consecutive pairs to pin each slot's register
            slot = {}
            for k in sorted(pairs):
                if k + 1 in pairs:
                    common = set(pairs[k]) & set(pairs[k + 1])
                    if len(common) == 1:
                        r = common.pop()
                        slot[k + 1] = r
                        other = [x for x in pairs[k] if x != r]
                        if other:
                            slot[k] = other[0]
                        other2 = [x for x in pairs[k + 1] if x != r]
                        if other2:
                            slot[k + 2] = other2[0]
            for k, tp in sorted(pairs.items()):
                if k not in slot and k + 1 in slot and slot[k + 1] in tp:
                    slot[k] = [x for x in tp if x != slot[k + 1]][0]
                if k + 1 not in slot and k in slot and slot[k] in tp:
                    slot[k + 1] = [x for x in tp if x != slot[k]][0]
            print(f"# block {bi}: {len(pairs)} responsive swaps, "
                  f"slot->reg { {k: v for k, v in sorted(slot.items())} }")
            reg2slot = {v: k for k, v in slot.items()}
            if len(reg2slot) != len(slot):
                print(f"# block {bi}: slot->reg not injective; skipping solve")
                continue
            # variable at slot t holds slot[t]; retail wants pi(slot[t]);
            # so it must move to reg2slot[pi(slot[t])]
            dest = {}
            ok = True
            for t, r in slot.items():
                want = pi.get(r, r)
                if want not in reg2slot:
                    ok = False; break
                dest[t] = reg2slot[want]
            if not ok or len(set(dest.values())) != len(dest):
                print(f"# block {bi}: pi not realisable within this block")
                continue
            order = [None] * n
            for t, d in dest.items():
                order[d] = t
            free = [i for i in range(n) if i not in dest]
            holes = [i for i in range(n) if order[i] is None]
            for h, f in zip(holes, free):
                order[h] = f
            if tuple(order) == tuple(ident):
                print(f"# block {bi}: solved ordering is the identity")
                continue
            src_file.write_bytes(render(bi, order).encode("latin-1"))
            rebuild(unit["object"], a.version)
            fz = fuzzy_measure(unit, a.symbol, a.version)
            print(f"# block {bi}: SOLVED order {order} -> fuzzy={fz:.4f} "
                  f"{'<== BETTER' if fz > base + 1e-4 else ''}")
            if fz > best[0] + 1e-4:
                best = (fz, (bi, order))
    finally:
        src_file.write_bytes(original)
    if a.apply and best[1]:
        src_file.write_bytes(render(*best[1]).encode("latin-1"))
        rebuild(unit["object"], a.version)
        print(f"# APPLIED: {base:.4f} -> {fuzzy_measure(unit, a.symbol, a.version):.4f}")
    else:
        rebuild(unit["object"], a.version)
    print(f"#BASE={base:.4f} BEST={best[0]:.4f}")


if __name__ == "__main__":
    main()
