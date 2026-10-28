#!/usr/bin/env python3
"""Sweep the SHAPE OF AN EXPRESSION, under a semantic-equivalence gate.

Declaration order is the only source key for the callee-saved registers, so it
cannot reach a row whose residual sits entirely in r0-r13/f0-f13 -- and 58 of
the 142 colouring rows are exactly that.  Volatile registers hold unnamed
expression temporaries, so the only source text that decides them is the shape
of the expression itself:

    a * b + c * d   ->  fmuls f0,f3,f4 ; fmadds f1,f1,f2,f0
    b * a + c * d   ->  fmuls f0,f3,f4 ; fmadds f1,f2,f1,f0
    c * d + a * b   ->  fmuls f0,f1,f2 ; fmadds f1,f3,f4,f0

This tool enumerates those rewrites on a real parse tree rather than with a
regex, and -- the part that matters -- refuses to compile any variant that
semantic_equivalence.prove() has not certified as computing exactly what the
original computed.  A regex predecessor turned `verts + j * 12` into
`(j + verts) * 12`, scored it as an improvement and applied it; the gate is
what makes that impossible rather than merely unlikely.

Transforms offered:
    commute   a OP b -> b OP a           for + * & | ^ == !=
    relflip   a < b  -> b > a            (and <= >=)
    ternflip  c ? a : b -> !c ? b : a
    assoc     (a+b)+c -> a+(b+c) and operand permutations, INTEGER chains only
              and only with a positive type proof (opt in with --assoc)

Usage:
    expr_sweep.py <unit> <symbol> [--list] [--apply] [--greedy]
    expr_sweep.py --batch rows.txt
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cexpr
from cexpr import (Ambiguous, Node, ParseError, is_pure, parse_expression,
                   strip_parens, tokenize)
import semantic_equivalence as sem
from brute_match import (REPO, TYPE_TOKENS, find_function_body, fuzzy_measure,
                         rebuild, recover_stale_backup)
from function_objdump import load_units, resolve_unit

DECL_LEAD = {"static", "const", "volatile", "register", "extern",
             "struct", "union", "enum", "unsigned", "signed", "long", "short"}
KEYWORDS = {"if", "while", "switch", "for", "do", "else", "return", "case",
            "break", "continue", "goto", "default", "sizeof"}


# ------------------------------------------------------- region extraction


def _skip_ws(s: str, i: int, hi: int) -> int:
    while i < hi:
        if s[i] in " \t\r\n":
            i += 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = hi if j < 0 else j
        elif s.startswith("/*", i):
            j = s.find("*/", i)
            i = hi if j < 0 else j + 2
        else:
            return i
    return hi


def _scan_to(s: str, i: int, hi: int, stops: str) -> int:
    """First character from `stops` at nesting depth 0."""
    depth = 0
    while i < hi:
        c = s[i]
        if s.startswith("//", i):
            j = s.find("\n", i)
            i = hi if j < 0 else j
            continue
        if s.startswith("/*", i):
            j = s.find("*/", i)
            i = hi if j < 0 else j + 2
            continue
        if c in "\"'":
            i = _skip_lit(s, i, hi)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0 and c in stops:
                return i
            depth -= 1
        elif depth == 0 and c in stops:
            return i
        i += 1
    return hi


def _skip_lit(s: str, i: int, hi: int) -> int:
    q = s[i]
    i += 1
    while i < hi:
        if s[i] == "\\":
            i += 2
            continue
        if s[i] == q:
            return i + 1
        i += 1
    return hi


def _looks_like_decl(s: str, i: int, hi: int) -> bool:
    toks = []
    try:
        toks = tokenize(s, i, min(hi, i + 200))
    except ParseError:
        return False
    k = 0
    while k < len(toks) and toks[k].kind == "id" and toks[k].text in DECL_LEAD:
        k += 1
    if k >= len(toks) or toks[k].kind != "id":
        return False
    if toks[k].text in KEYWORDS:
        return False
    typed = (toks[k].text in TYPE_TOKENS or k > 0 or
             re.match(r"^(?:[usf]\d+|.*_t|[A-Z]\w*)$", toks[k].text))
    if not typed:
        return False
    k += 1
    while k < len(toks) and toks[k].kind == "punct" and toks[k].text == "*":
        k += 1
    if k >= len(toks) or toks[k].kind != "id":
        return False
    k += 1
    if k >= len(toks):
        return False
    # the region handed in stops BEFORE the `;`, so end-of-input is itself a
    # valid declarator terminator -- without this, `f32* dir;` reads as a
    # multiplication and gets offered as a swappable site.
    return toks[k].kind == "eof" or (toks[k].kind == "punct"
                                     and toks[k].text in ("=", ";", ",", "["))


@dataclass
class Region:
    start: int
    end: int
    what: str


def expression_regions(s: str, lo: int, hi: int) -> List[Region]:
    """Every full expression inside a function body, as source spans."""
    out: List[Region] = []
    i = lo + 1 if s[lo] == "{" else lo
    while i < hi:
        i = _skip_ws(s, i, hi)
        if i >= hi:
            break
        c = s[i]
        if c in "{};":
            i += 1
            continue
        m = re.match(r"[A-Za-z_]\w*", s[i:hi])
        kw = m.group(0) if m else ""
        if kw in ("if", "while", "switch"):
            p = s.find("(", i)
            q = _matching(s, p, hi)
            if q < 0:
                break
            out.append(Region(p + 1, q, kw + "-cond"))
            i = q + 1
            continue
        if kw == "for":
            p = s.find("(", i)
            q = _matching(s, p, hi)
            if q < 0:
                break
            a = p + 1
            for part in range(3):
                b = _scan_to(s, a, q, ";") if part < 2 else q
                if s[a:b].strip():
                    if _looks_like_decl(s, a, b):
                        out.extend(_decl_inits(s, a, b))
                    else:
                        out.append(Region(a, b, f"for{part}"))
                a = b + 1
            i = q + 1
            continue
        if kw in ("do", "else"):
            i += len(kw)
            continue
        if kw in ("case", "default"):
            i = _scan_to(s, i, hi, ":") + 1
            continue
        if kw in ("break", "continue", "goto"):
            i = _scan_to(s, i, hi, ";") + 1
            continue
        j = _scan_to(s, i, hi, ";")
        if kw == "return":
            a = _skip_ws(s, i + 6, j)
            if s[a:j].strip():
                out.append(Region(a, j, "return"))
        elif _looks_like_decl(s, i, j):
            out.extend(_decl_inits(s, i, j))
        elif s[i:j].strip():
            out.append(Region(i, j, "stmt"))
        i = j + 1
    return out


def _matching(s: str, i: int, hi: int) -> int:
    if i < 0 or i >= hi:
        return -1
    depth = 0
    while i < hi:
        if s[i] in "\"'":
            i = _skip_lit(s, i, hi)
            continue
        if s[i] in "([{":
            depth += 1
        elif s[i] in ")]}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _decl_inits(s: str, lo: int, hi: int) -> List[Region]:
    """The initialiser expressions of a declaration statement."""
    out = []
    i = lo
    while i < hi:
        e = _scan_to(s, i, hi, "=")
        if e >= hi:
            break
        if s[e + 1:e + 2] == "=" or s[e - 1] in "!<>=+-*/%&|^":
            i = e + 2
            continue
        a = _skip_ws(s, e + 1, hi)
        b = _scan_to(s, a, hi, ",")
        if s[a:b].strip() and not s[a:b].lstrip().startswith("{"):
            out.append(Region(a, b, "init"))
        i = b + 1
    return out


# ----------------------------------------------------------- the transforms


def _needs_paren_for_not(n: Node) -> bool:
    n2 = n
    return not (n2.kind in ("id", "num", "str", "chr", "paren", "call",
                            "index", "member", "postincr", "cast", "unary",
                            "sizeof"))


def _negate(n: Node, s: str) -> str:
    n = n if n.kind != "paren" else n
    inner = strip_parens(n)
    if inner.kind == "unary" and inner.op == "!":
        return inner.kids[0].src(s).strip()
    t = n.src(s).strip()
    return ("!(" + t + ")") if _needs_paren_for_not(strip_parens(n)) else "!" + t


def _render(n: Node, s: str, need_prec: int) -> str:
    """Source text for `n`, parenthesised only where the surrounding operator
    would otherwise re-associate it.

    Without this, swapping the two sides of the outer `+` in
    `nz*dz + nx*dx + ny*dy` emits `ny*dy + nz*dz + nx*dx`, which re-parses as a
    DIFFERENT grouping -- and float addition is not associative, so that is a
    different computation.  The gate catches it either way; this is what turns
    the catch back into a usable rewrite.
    """
    t = n.src(s).strip()
    if n.kind == "paren":
        return t
    inner = strip_parens(n)
    if inner.kind == "bin":
        if cexpr.BIN_PREC[inner.op] < need_prec:
            return "(" + t + ")"
        return t
    if inner.kind in ("cond", "assign", "comma"):
        return "(" + t + ")"
    return t


@dataclass
class Variant:
    node: Node
    rule: str
    new_sub: str
    label: str


def variants(root: Node, s: str, oracle: sem.TypeOracle,
             allow_assoc: bool, literals: str) -> List[Variant]:
    out: List[Variant] = []
    for n in root.walk():
        if n.kind == "bin":
            L, R = n.kids
            p = cexpr.BIN_PREC[n.op]
            lt, rt = L.src(s).strip(), R.src(s).strip()
            if n.op in sem.COMMUTATIVE and is_pure(L) and is_pure(R):
                if _literal_ok(lt, rt, n.op, literals):
                    out.append(Variant(
                        n, "commute",
                        f"{_render(R, s, p)} {n.op} {_render(L, s, p + 1)}",
                        f"{lt} {n.op} {rt}"))
            flip = sem.RELATIONAL_FLIP.get(n.op)
            if flip and is_pure(L) and is_pure(R):
                # `13 > i` is a Yoda condition: legal, and not what a 2002 Rare
                # dev wrote, so it only appears under --literals all.
                if _literal_ok(lt, rt, "cmp", literals):
                    out.append(Variant(
                        n, "relflip",
                        f"{_render(R, s, p)} {flip} {_render(L, s, p + 1)}",
                        f"{lt} {n.op} {rt}"))
            if allow_assoc and n.op in sem.ASSOCIATIVE_INT:
                out.extend(_assoc_variants(n, s, oracle))
            if n.op == "*" and is_pure(n):
                # the mirror move: pull a factor's sign out onto the product
                for idx, side in enumerate(n.kids):
                    inner = strip_parens(side)
                    if inner.kind == "unary" and inner.op == "-":
                        keep = n.kids[1 - idx]
                        bare = inner.kids[0]
                        a_, b_ = ((bare, keep) if idx == 0 else (keep, bare))
                        out.append(Variant(
                            n, "sign",
                            f"-({_render(a_, s, p)} * {_render(b_, s, p + 1)})",
                            "x * -y"))
        elif n.kind == "unary" and n.op == "-":
            k = strip_parens(n.kids[0])
            if k.kind == "bin" and k.op == "*" and is_pure(k):
                x, y = k.kids
                p = cexpr.BIN_PREC["*"]
                out.append(Variant(n, "sign",
                                   f"-{_render(x, s, 14)} * {_render(y, s, p + 1)}",
                                   "-(x * y)"))
                out.append(Variant(n, "sign",
                                   f"{_render(x, s, p)} * -{_render(y, s, 14)}",
                                   "-(x * y)"))
        elif n.kind == "cond":
            c, a, b = n.kids
            out.append(Variant(
                n, "ternflip",
                f"{_negate(c, s)} ? {_render(b, s, 3)} : {_render(a, s, 3)}",
                f"{c.src(s).strip()} ? ... : ..."))
    return out


def _literal_ok(lt: str, rt: str, op: str, mode: str) -> bool:
    if mode == "all":
        return True
    lit = re.compile(r"^[-+]?(?:0[xX][0-9a-fA-F]+|\d[\w.]*)$")
    if not (lit.match(rt) or rt in ("NULL", "TRUE", "FALSE")):
        return True
    # putting a literal FIRST is idiomatic in a product (`2 * n`, `0.5f * dt`)
    # and unidiomatic in a sum, so a sum is only offered under --literals all.
    return op == "*" if mode == "mul" else False


def _assoc_variants(n: Node, s: str, oracle: sem.TypeOracle) -> List[Variant]:
    import itertools
    ops = sem._flatten(n, n.op)
    if not (3 <= len(ops) <= 4):
        return []
    if not all(oracle.is_int_expr(o) and is_pure(o) for o in ops):
        return []
    texts = [o.src(s).strip() for o in ops]
    seen = {(" " + n.op + " ").join(texts)}
    out = []
    for perm in itertools.permutations(range(len(ops))):
        t = [texts[k] for k in perm]
        for shape in ("left", "right"):
            if shape == "left":
                txt = (" " + n.op + " ").join(t)
            else:
                txt = t[0]
                tail = t[1:]
                inner = (" " + n.op + " ").join(tail)
                txt = f"{txt} {n.op} ({inner})"
            if txt in seen:
                continue
            seen.add(txt)
            out.append(Variant(n, "assoc", txt, "chain"))
    return out


# ------------------------------------------------------------------ driver


def decl_text_of(src: str, body: Tuple[int, int], sym: str) -> str:
    """Parameter list + body, which is what the type oracle reads."""
    head = src.rfind("\n", 0, src.rfind(sym, 0, body[0]))
    return src[max(0, head):body[1]]


def sweep(unit, symbol, version, args):
    src_file = REPO / "src" / unit["name"].replace("\\", "/")
    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")
    body = find_function_body(src, symbol)
    if not body:
        return {"symbol": symbol, "error": "PARSE-ERROR: no function body"}

    dtext = decl_text_of(src, body, symbol)
    oracle = sem.TypeOracle(dtext)
    known_ids = frozenset(oracle.kind) | frozenset(
        re.findall(r"\b([a-z]\w*)\s*(?:=[^=]|\+\+|--|\[)", dtext)) | \
        cexpr.project_objects()

    ktypes = cexpr.project_typedefs()
    regions = expression_regions(src, body[0], body[1])
    parse_fail: List[Tuple[str, str]] = []
    cands: List[Tuple[Region, Node, Variant]] = []
    for reg in regions:
        text = src[reg.start:reg.end]
        try:
            root = parse_expression(src, known_ids, ktypes,
                                    reg.start, reg.end)
        except Ambiguous as e:
            parse_fail.append((reg.what, f"AMBIGUOUS {e}: {text.strip()[:70]}"))
            continue
        except ParseError as e:
            parse_fail.append((reg.what, f"{e}: {text.strip()[:70]}"))
            continue
        for v in variants(root, src, oracle, args.assoc, args.literals):
            cands.append((reg, root, v))

    # ---- the gate: every variant is proved before it is ever compiled
    cleared, refused = [], []
    for reg, root, v in cands:
        new_region = (src[reg.start:v.node.start] + v.new_sub +
                      src[v.node.end:reg.end])
        verdict = sem.prove(src[reg.start:reg.end], new_region,
                            known_types=ktypes, decl_text=dtext,
                            known_ids=known_ids,
                            allow_assoc=args.assoc, trials=args.trials)
        if verdict.ok:
            cleared.append((reg, v, new_region, verdict))
        else:
            refused.append((reg, v, new_region, verdict))

    # A tree that does not describe its own source is reported on its OWN
    # line, never folded into the refusal count.  A refusal means the rewrite
    # was judged and rejected; a mis-parse means nothing here was judged at
    # all, and the two must never be summed into one reassuring number.
    misparsed = [(v.rule, vd.misparse, new[:80])
                 for _, v, new, vd in refused if vd.misparse]

    info = {"symbol": symbol, "unit": unit["name"], "regions": len(regions),
            "parse_fail": parse_fail, "cleared": len(cleared),
            "misparsed": misparsed,
            "refused": [(v.rule, str(vd)[:110], new[:80])
                        for _, v, new, vd in refused]}

    if args.list:
        for reg, v, new, vd in cleared:
            print(f"  [{v.rule:8s}] {' '.join(src[reg.start:reg.end].split())[:60]}"
                  f"\n             -> {' '.join(new.split())[:60]}   {vd}")
        for _, v, new, vd in refused:
            print(f"  [REFUSED {v.rule}] {' '.join(new.split())[:60]}\n"
                  f"             {vd}")
        for what, why in parse_fail:
            print(f"  [PARSE-FAIL {what}] {why}")
        for rule, why, new in misparsed:
            print(f"  ** MIS-PARSE {rule}: {why}\n             {new}")
        return info

    if not cleared:
        info["base"] = info["best"] = None
        return info

    def apply_variant(text, reg, v, new_region):
        return text[:reg.start] + new_region + text[reg.end:]

    rebuild(unit["object"], version)
    base = fuzzy_measure(unit, symbol, version)
    info["base"] = base
    cur_src = src
    cur = base
    hits = []
    t0 = time.time()
    rounds = args.greedy_rounds if args.greedy else 1
    for rnd in range(rounds):
        best = (cur, None)
        for reg, v, new_region, vd in cleared:
            if time.time() - t0 > args.time_budget:
                info["timeout"] = True
                break
            probe = apply_variant(cur_src, reg, v, new_region)
            src_file.write_bytes(probe.encode("latin-1"))
            if not rebuild(unit["object"], version):
                continue
            fz = fuzzy_measure(unit, symbol, version)
            info.setdefault("builds", 0)
            info["builds"] += 1
            if fz > best[0] + 1e-4:
                best = (fz, (reg, v, new_region, vd))
        if best[1] is None:
            break
        reg, v, new_region, vd = best[1]
        hits.append({"rule": v.rule, "from": cur, "to": best[0],
                     "before": " ".join(cur_src[reg.start:reg.end].split()),
                     "after": " ".join(new_region.split()),
                     "verdict": str(vd)})
        cur_src = apply_variant(cur_src, reg, v, new_region)
        cur = best[0]
        # spans move once the file changes; recompute from the new text
        body2 = find_function_body(cur_src, symbol)
        if not body2:
            break
        cleared = _recompute(cur_src, body2, symbol, oracle, known_ids, args, ktypes)

    info["best"] = cur
    info["hits"] = hits
    if args.apply and cur > base + 1e-4:
        src_file.write_bytes(cur_src.encode("latin-1"))
        rebuild(unit["object"], version)
        info["applied"] = True
    else:
        src_file.write_bytes(original)
        rebuild(unit["object"], version)
    return info


def _recompute(src, body, symbol, oracle, known_ids, args, ktypes=frozenset()):
    out = []
    for reg in expression_regions(src, body[0], body[1]):
        try:
            root = parse_expression(src, known_ids, ktypes,
                                    reg.start, reg.end)
        except ParseError:
            continue
        for v in variants(root, src, oracle, args.assoc, args.literals):
            new_region = (src[reg.start:v.node.start] + v.new_sub +
                          src[v.node.end:reg.end])
            vd = sem.prove(src[reg.start:reg.end], new_region,
                           known_types=ktypes,
                           decl_text=src[body[0]:body[1]], known_ids=known_ids,
                           allow_assoc=args.assoc, trials=args.trials)
            if vd.ok:
                out.append((reg, v, new_region, vd))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit", nargs="?")
    ap.add_argument("symbol", nargs="?")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--assoc", action="store_true")
    ap.add_argument("--greedy", action="store_true")
    ap.add_argument("--greedy-rounds", type=int, default=3)
    ap.add_argument("--literals", choices=("none", "mul", "all"), default="mul")
    ap.add_argument("--trials", type=int, default=64)
    ap.add_argument("--time-budget", type=float, default=1800.0)
    ap.add_argument("--batch", help="file of `unit<TAB>symbol` rows")
    a = ap.parse_args()

    units = load_units(REPO / "build" / a.version / "config.json")
    rows = []
    if a.batch:
        for line in Path(a.batch).read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                u, s = line.split()[:2]
                rows.append((u, s))
    else:
        rows = [(a.unit, a.symbol)]

    for u, s in rows:
        unit = resolve_unit(units, u)
        info = sweep(unit, s, a.version, a)
        if a.list:
            continue
        print(f"@@ {u} {s} base={info.get('base')} best={info.get('best')} "
              f"cleared={info.get('cleared')} refused={len(info.get('refused', []))} "
              f"parsefail={len(info.get('parse_fail', []))} "
              f"MISPARSE={len(info.get('misparsed', []))}", flush=True)
        for rule, why, new in info.get("misparsed", []):
            print(f"   ** MIS-PARSE[{rule}] {why}", flush=True)
        for h in info.get("hits", []):
            print(f"   HIT {h['rule']} {h['from']:.4f}->{h['to']:.4f}\n"
                  f"       - {h['before'][:110]}\n"
                  f"       + {h['after'][:110]}\n"
                  f"       {h['verdict']}", flush=True)
        for what, why in info.get("parse_fail", []):
            print(f"   PARSE-FAIL[{what}] {why}", flush=True)


if __name__ == "__main__":
    main()
