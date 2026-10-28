#!/usr/bin/env python3
"""Iterated-ascent declaration-order sweep (radius >= 2).

brute_match.py sweeps ONE radius-1 neighbourhood per block (every single swap,
every single move) and stops.  A colouring row whose target ordering is two
moves away therefore reads exactly like a row that has no reachable ordering at
all.  This tool restarts the radius-1 neighbourhood from the winner and repeats
until a round adds nothing, so k rounds reach radius k.

It also reports, per block, whether the fuzzy score VARIES across the
neighbourhood at all: a block whose every ordering scores the baseline is inert
on this key and deeper rounds cannot help it, while a block that varies but
never rises is exactly the population deepening is for.

Usage: deep_decl.py <unit> <symbol> [-v GSAE01] [--rounds N] [--time-budget S]
                    [--apply]
"""
from __future__ import annotations
import argparse, itertools, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import load_units, resolve_unit
from brute_match import (REPO, collect_decl_blocks, find_function_body, find_objdump,
                         fuzzy_measure, gen_variants, match_score, objdump_norm,
                         objdump_paths, rebuild, recover_stale_backup)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit"); ap.add_argument("symbol")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--time-budget", type=float, default=1800.0)
    ap.add_argument("--max-variants", type=int, default=4000)
    ap.add_argument("--beam", type=int, default=3)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    unit = resolve_unit(load_units(REPO / "build" / a.version / "config.json"), a.unit)
    src_file = REPO / "src" / unit["name"].replace("\\", "/")
    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")
    body = find_function_body(src, a.symbol)
    if not body:
        raise SystemExit(f"PARSE-ERROR could not locate definition of {a.symbol}")
    blocks = collect_decl_blocks(src, body[0], body[1])
    if not blocks:
        raise SystemExit("PARSE-ERROR no declaration block with >=2 items")

    def render(orders):
        out = src
        for bi in sorted(orders, reverse=True):
            b = blocks[bi]; it = b["items"]; order = orders[bi]
            first = it[order[0]].lstrip()
            rest = "".join("\n" + b["indent"] + it[k].lstrip() for k in order[1:])
            out = out[:b["start"]] + first + rest + out[b["end"]:]
        return out

    objdump = find_objdump()
    tgt_o, cur_o = objdump_paths(unit, a.version)
    rebuild(unit["object"], a.version)
    t_norm = objdump_norm(objdump, tgt_o, a.symbol)
    base_fz = fuzzy_measure(unit, a.symbol, a.version)
    print(f"# {a.symbol}  blocks={len(blocks)}  baseline fuzzy={base_fz:.4f}")
    for bi, b in enumerate(blocks):
        print(f"#  block {bi}: depth={b['depth']} {len(b['items'])} items")

    held = {bi: tuple(range(len(blocks[bi]["items"]))) for bi in range(len(blocks))}
    cur = base_fz
    t0 = time.time(); n = 0; varied = 0

    def score(orders):
        nonlocal n
        src_file.write_bytes(render(orders).encode("latin-1"))
        if not rebuild(unit["object"], a.version):
            return None
        n += 1
        return fuzzy_measure(unit, a.symbol, a.version)

    # BEAM SEARCH over declaration orderings.  Round 0 is the radius-1
    # neighbourhood of the original; each later round expands the radius-1
    # neighbourhood of the BEST-SCORING orderings found so far, whether or not
    # they beat the baseline.  Depth k therefore reaches radius k along the k
    # most promising directions instead of stopping at radius 1.
    try:
        for bi in range(len(blocks)):
            m = len(blocks[bi]["items"])
            if m < 2:
                continue
            beam = [(base_fz, tuple(range(m)))]
            seen = {beam[0][1]}
            best = (base_fz, beam[0][1])
            for rnd in range(a.rounds):
                pool = []
                for _, base_order in beam:
                    for o in gen_variants(m, "all", a.max_variants)[1:]:
                        ro = tuple(base_order[i] for i in o)
                        if ro in seen:
                            continue
                        seen.add(ro)
                        if time.time() - t0 > a.time_budget:
                            print(f"# TIME BUDGET (block {bi} round {rnd})")
                            raise TimeoutError
                        trial = dict(held); trial[bi] = ro
                        fz = score(trial)
                        if fz is None:
                            continue
                        if abs(fz - base_fz) > 1e-6:
                            varied += 1
                        pool.append((fz, ro))
                        if fz > best[0] + 1e-4:
                            best = (fz, ro)
                            print(f"[b{bi} r{rnd}] fuzzy={fz:.4f} {list(ro)}  <== best")
                if not pool:
                    break
                pool.sort(key=lambda t: -t[0])
                beam = pool[:a.beam]
                print(f"# block {bi} round {rnd}: probes={n} best={best[0]:.4f} "
                      f"beam_top={[round(f,4) for f, _ in beam]}")
            if best[0] > cur + 1e-4:
                held[bi] = best[1]; cur = best[0]
    except TimeoutError:
        pass
    finally:
        src_file.write_bytes(original)

    print(f"#PROBES={n} BASE={base_fz:.4f} BEST={cur:.4f} "
          f"VARIED={varied}")
    if a.apply and cur > base_fz + 1e-4:
        src_file.write_bytes(render(held).encode("latin-1"))
        rebuild(unit["object"], a.version)
        conf = fuzzy_measure(unit, a.symbol, a.version)
        if conf > base_fz + 1e-4:
            print(f"# APPLIED: fuzzy {base_fz:.4f} -> {conf:.4f}")
            for bi, o in held.items():
                if tuple(o) != tuple(range(len(o))):
                    print(f"#   block {bi} order = {list(o)}")
        else:
            src_file.write_bytes(original); rebuild(unit["object"], a.version)
            print("# NOT CONFIRMED; restored")
    else:
        rebuild(unit["object"], a.version)


if __name__ == "__main__":
    main()
