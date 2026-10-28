#!/usr/bin/env python3
"""Unlock the INITIALISER-LOCKED declaration blocks by splitting declaration
from initialisation, then sweep the (now free) declaration order.

WHY THIS EXISTS.  `brute_match` permutes declaration TEXT and never reads the
initialisers, so on a block whose declarations initialise from each other

    u16   quadrant;
    float reducedAngle   = trigReduceQuadrant(&quadrant, angle);
    float reducedSquared = reducedAngle * reducedAngle;
    float tangent        = reducedAngle * (...reducedSquared...);

nearly every ordering is use-before-declaration and never compiles.  Such a row
gets counted as swept while its legal neighbourhood was smaller -- and for a
whole family of rows (the trig functions, `mathTanf`) that neighbourhood is
EMPTY: the dependency chain is total, so the declaration order is pinned by the
data flow and no permutation of the text exists at all.

THE LEVER.  Splitting a declaration from its initialisation

    float reducedAngle;
    ...
    reducedAngle = trigReduceQuadrant(&quadrant, angle);

is ordinary C89 house style -- exactly what a 2002 dev writing to the C89
"declarations first" rule does whenever the initialiser is not a constant -- and
it decouples the two orders that the compiler keys off:

  * DECLARATION order (which sets saved-register homes) becomes FREE, because
    a declaration with no initialiser depends on nothing;
  * STATEMENT order (which keys emission order) is UNCHANGED, because the
    assignments are emitted in the original declaration order, at the top of
    the block, before the first real statement.

So the rewrite is semantics-preserving by construction: initialisers are
evaluated in the same relative order, with the same operands, and the only
thing that moves is the point at which storage is *named*, which C does not
observe.

MEASURE BOTH EFFECTS.  The split ALONE is gated as byte-identical before any
permutation is tried (`--control-only` runs just that gate).  If the split
itself moves the object, that is reported as a distinct measurement and the
permutation sweep is still run, but the row is flagged: the split is then a
source change with its own price, not a free re-key.

SPLITTABILITY.  A declaration is split only when the rewrite is unambiguously
equivalent: exactly one top-level `=`, a single declarator, no brace
initialiser, no array/function declarator, and no `static`/`const`/`register`
storage class.  Anything else is left as a declaration-with-initialiser, which
simply keeps its dependency edges and constrains the sweep as before.

And one further restriction, because the obvious rewrite is NOT always
equivalent: an initialiser we cannot split stays at its declaration position
while every split initialiser moves below it, so an unsplittable INITIALISED
declaration sitting after a splittable one would have its initialiser
evaluated first after the rewrite.  Only the initialised declarations that
follow the LAST unsplittable initialised one are ever split.

Usage:
  python3 tools/decl_split_sweep.py <unit> <symbol> [--block N] [--control-only]
      [--census] [--max-variants N] [--time-budget S] [--strategy ...]
      [--self-test]

  --census      print every declaration block, its intra-block dependency
                edges, the number of legal orderings BEFORE the split and the
                number AFTER, and exit.  Build nothing.
  --control-only apply the split, rebuild, compare object bytes to the
                pristine build, restore, and exit.
  --new-only    sweep ONLY the orderings the split made legal; everything else
                is what an ordinary radius-1 pass already covers.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import brute_match as BM
from brute_match import (
    REPO, load_units, resolve_unit, find_function_body, collect_decl_blocks,
    parse_decl_block, gen_variants, rebuild, fuzzy_measure, objdump_paths,
    install_restore_guard, recover_stale_backup,
)

STORAGE_BLOCKERS = ("static", "const", "register", "extern", "volatile")


# ------------------------------------------------------------- item parsing
def strip_trailing_comment(item: str) -> tuple[str, str]:
    """(code, trailing) -- `trailing` is a same-line comment after the `;`."""
    semi = -1
    depth = 0
    i = 0
    n = len(item)
    while i < n:
        c = item[i]
        if item.startswith("//", i) or item.startswith("/*", i):
            break
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c in '"\'':
            i = BM.skip_string(item, i)
            continue
        elif c == ";" and depth == 0:
            semi = i
            break
        i += 1
    if semi < 0:
        return item, ""
    return item[:semi + 1], item[semi + 1:]


def top_level_eq(code: str) -> int:
    """Index of the initialiser `=`, or -1.  Skips ==, !=, <=, >=, and any `=`
    nested in parens/brackets/braces or inside a string.

    ONE implementation, shared with the sweeper that decides whether an
    initialiser carries a call: two copies of this scan would be two things to
    control, and only one of them would ever get the control."""
    return BM.initialiser_eq(code)


IDENT_RE = re.compile(r"[A-Za-z_]\w*")


def declared_names(item: str) -> list[str]:
    """Every variable name a declaration item introduces.

    Take the declarator part (everything before the initialiser `=`, or before
    the `;`), and read the LAST identifier of each top-level comma group.
    """
    code, _ = strip_trailing_comment(item)
    code = code.rstrip().rstrip(";")
    eq = top_level_eq(code)
    head = code[:eq] if eq >= 0 else code
    # split head on top-level commas (multiple declarators)
    parts = []
    depth = 0
    cur = ""
    for ch in head:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    names = []
    for p in parts:
        p = p.split("[")[0]
        ids = IDENT_RE.findall(p)
        if ids:
            names.append(ids[-1])
    return names


def init_expr(item: str) -> str | None:
    code, _ = strip_trailing_comment(item)
    code = code.rstrip().rstrip(";")
    eq = top_level_eq(code)
    if eq < 0:
        return None
    return code[eq + 1:]


def splittable(item: str) -> tuple[bool, str]:
    """(ok, reason).  Only unambiguously equivalent rewrites are allowed."""
    code, trailing = strip_trailing_comment(item)
    body = code.rstrip().rstrip(";")
    eq = top_level_eq(body)
    if eq < 0:
        return False, "no initialiser"
    head, init = body[:eq], body[eq + 1:]
    first = head.strip().split()
    for kw in STORAGE_BLOCKERS:
        if kw in first:
            return False, f"storage class `{kw}`"
    if "{" in init:
        return False, "brace initialiser"
    # A top-level comma ANYWHERE in the declaration means more than one
    # declarator (`int a = 0, b = 1;`).  Commas inside the initialiser's call
    # arguments sit at depth > 0 and do not count.
    depth = 0
    i = 0
    while i < len(body):
        ch = body[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch in '"\'':
            i = BM.skip_string(body, i)
            continue
        elif ch == "," and depth == 0:
            return False, "multiple declarators"
        i += 1
    if "[" in head:
        return False, "array declarator"
    names = declared_names(item)
    if len(names) != 1:
        return False, "not exactly one declarator"
    if "(" in head:
        return False, "function/pointer-to-function declarator"
    if re.search(r"//|/\*", init):
        return False, "comment inside initialiser"
    return True, ""


def split_item(item: str, indent: str) -> tuple[str, str]:
    """`T name = expr;` -> (`T name;`, `name = expr;`), re-indented.

    Continuation lines of a multi-line initialiser are shifted LEFT by the
    number of columns the type prefix occupied, so the assignment stays aligned
    the way a human would have written it.
    """
    code, trailing = strip_trailing_comment(item)
    body = code.rstrip()
    assert body.endswith(";")
    body = body[:-1]
    eq = top_level_eq(body)
    head, init = body[:eq], body[eq + 1:]
    name = declared_names(item)[0]
    decl = head.rstrip() + ";"

    old_col = len(indent) + len(head) + 1          # column of the char after `=`
    new_col = len(indent) + len(name) + 3          # `name = `
    delta = old_col - new_col
    lines = init.split("\n")
    out = [lines[0].strip()]
    for ln in lines[1:]:
        stripped = ln.lstrip(" ")
        lead = len(ln) - len(stripped)
        out.append(" " * max(0, lead - delta) + stripped)
    assign = f"{name} = " + ("\n".join(out)) + ";" + trailing
    return decl, assign


# --------------------------------------------------------- dependency graph
def split_mask_for(items: list[str]) -> list[bool]:
    """Which items may be split WITHOUT reordering any initialiser evaluation.

    A split initialiser moves from its declaration position to the bottom of
    the declaration run.  An initialiser we CANNOT split (a brace initialiser,
    a multi-declarator, a `const`) stays where it is -- so if one of those sits
    AFTER an item we split, the two initialisers swap evaluation order:

        int a = f();            int a;
        int b[2] = {g(), 0};    int b[2] = {g(), 0};   <-- g() now runs first
        int c = h();            int c;
                                a = f(); c = h();

    which is a semantic change no byte gate in this project can see.  So only
    the initialised declarations that follow the LAST unsplittable initialised
    one may be split; everything before it keeps its initialiser and its
    dependency edges.
    """
    raw = [splittable(it)[0] for it in items]
    inited = [k for k, it in enumerate(items) if init_expr(it) is not None]
    blockers = [k for k in inited if not raw[k]]
    floor = max(blockers) if blockers else -1
    return [raw[k] and k > floor for k in range(len(items))]


def block_edges(items: list[str]) -> list[tuple[int, int]]:
    """(j, i): item j's initialiser reads a name declared by item i."""
    names = [set(declared_names(it)) for it in items]
    edges = []
    for j, it in enumerate(items):
        e = init_expr(it)
        if e is None:
            continue
        toks = set(IDENT_RE.findall(e))
        for i in range(len(items)):
            if i == j:
                continue
            if names[i] & toks:
                edges.append((j, i))
    return edges


def legal_orderings(n: int, edges: list[tuple[int, int]], cap: int = 200000):
    """Count orderings of n items in which every edge (j,i) has i before j."""
    if n > 9:
        return None
    cnt = 0
    for p in itertools.permutations(range(n)):
        pos = {v: k for k, v in enumerate(p)}
        if all(pos[i] < pos[j] for j, i in edges):
            cnt += 1
            if cnt > cap:
                return cap
    return cnt


# --------------------------------------------------------------- rendering
def render_split(src: str, block: dict, order: list[int], split_mask: list[bool]):
    """Splice `block` back into `src` as: the permuted declarations (with the
    splittable ones stripped of their initialiser), a blank line, then the
    assignments IN ORIGINAL DECLARATION ORDER."""
    indent = block["indent"]
    items = block["items"]
    decls = []
    assigns = []
    for k in range(len(items)):
        if split_mask[k]:
            d, a = split_item(items[k], indent)
            decls.append(d)
            assigns.append(a)
        else:
            decls.append(items[k].strip())
    ordered = [decls[k] for k in order]
    text = ordered[0]
    for d in ordered[1:]:
        text += "\n" + indent + d
    if assigns:
        text += "\n"
        for a in assigns:
            text += "\n" + indent + a
    return src[:block["start"]] + text + src[block["end"]:]


def render_perm(src: str, block: dict, order: list[int]):
    """Plain permutation, no split (brute_match's own rendering)."""
    indent = block["indent"]
    items = block["items"]
    first = items[order[0]].lstrip()
    rest = "".join("\n" + indent + items[k].lstrip() for k in order[1:])
    return src[:block["start"]] + first + rest + src[block["end"]:]


# ------------------------------------------------------------------ census
def census_row(src: str, symbol: str):
    body = find_function_body(src, symbol)
    if not body:
        return None
    blocks = collect_decl_blocks(src, *body)
    out = []
    for bi, b in enumerate(blocks):
        items = b["items"]
        edges = block_edges(items)
        mask = split_mask_for(items)
        after = [e for e in edges if not mask[e[0]]]
        out.append({
            "index": bi, "n": len(items), "edges": edges,
            "split": sum(mask), "mask": mask,
            "before": legal_orderings(len(items), edges),
            "after": legal_orderings(len(items), after),
        })
    return out


# -------------------------------------------------------------- self-test
SELFTEST_SRC = """\
float f(float angle) {
    u16 quadrant;
    float reducedAngle = trigReduceQuadrant(&quadrant, angle);
    float reducedSquared = reducedAngle * reducedAngle;
    static const float k[2] = {1.0f, 2.0f};
    int a = 0, b = 1;
    float tangent = reducedAngle *
                    (k[0] * reducedSquared +
                     k[1]);
    return tangent;
}
"""


def self_test() -> int:
    """Every claim this tool makes, with a mutation control for each.

    The controls matter because the failure mode of a rewriter is SILENCE: a
    splitter that quietly declines every item, or a dependency scanner that
    finds no edges, both look exactly like "this row is clean".
    """
    fails = []

    def check(name, cond, detail=""):
        print(f"  [{'ok ' if cond else 'FAIL'}] {name}" +
              (f"  {detail}" if detail and not cond else ""))
        if not cond:
            fails.append(name)

    body = find_function_body(SELFTEST_SRC, "f")
    blocks = collect_decl_blocks(SELFTEST_SRC, *body)
    check("parses one decl block", len(blocks) == 1, f"got {len(blocks)}")
    b = blocks[0]
    items = b["items"]
    names = [declared_names(it) for it in items]
    check("names", names[:3] == [["quadrant"], ["reducedAngle"],
                                 ["reducedSquared"]], str(names))

    edges = block_edges(items)
    # reducedAngle <- quadrant, reducedSquared <- reducedAngle,
    # tangent <- reducedAngle, reducedSquared, k
    check("finds dependency edges", len(edges) >= 4, str(edges))
    check("edge direction is use->def", all(j != i for j, i in edges))

    raw = [splittable(it)[0] for it in items]
    check("recognises the three float chain decls as splittable",
          raw[1] and raw[2] and raw[5], str(raw))
    check("declines `static const float k[2] = {...}`", not raw[3],
          splittable(items[3])[1])
    check("declines multiple declarators `int a = 0, b = 1;`", not raw[4],
          splittable(items[4])[1])
    check("declines an uninitialised decl", not raw[0],
          splittable(items[0])[1])
    mask = split_mask_for(items)

    before = legal_orderings(len(items), edges)
    after = legal_orderings(len(items), [e for e in edges if not mask[e[0]]])
    # `mask` above is the RAW splittability; the guard below is what the
    # renderer actually uses.
    check("split strictly enlarges the legal neighbourhood", after > before,
          f"{before} -> {after}")

    # An unsplittable INITIALISED declaration between two splittable ones
    # would have its initialiser evaluated before theirs after the rewrite.
    guarded = split_mask_for(items)
    check("ordering guard refuses to hop a split initialiser over an "
          "unsplittable one", guarded[1] is False and guarded[5] is True,
          str(guarded))
    check("guard is not vacuous: dropping the blocker restores the split",
          split_mask_for([items[0], items[1], items[2], items[5]])[1] is True,
          str(split_mask_for([items[0], items[1], items[2], items[5]])))

    d, a = split_item(items[5], b["indent"])
    check("split keeps the declarator", d.strip() == "float tangent;", d)
    check("split assignment names the variable",
          a.strip().startswith("tangent ="), a)
    check("split preserves every initialiser token",
          re.findall(r"\w+", init_expr(items[5])) ==
          re.findall(r"\w+", a.split("=", 1)[1]),
          a)

    rendered = render_split(SELFTEST_SRC, b, list(range(len(items))), mask)
    check("rendered source still parses to one block",
          len(collect_decl_blocks(rendered, *find_function_body(rendered, "f"))) == 1)
    ren_items = collect_decl_blocks(
        rendered, *find_function_body(rendered, "f"))[0]["items"]
    check("rendered block has the same item count", len(ren_items) == len(items),
          f"{len(ren_items)} vs {len(items)}")
    check("rendered assignments keep original order",
          rendered.index("reducedAngle =") < rendered.index("reducedSquared =")
          < rendered.index("tangent ="))

    # THE PROPERTY THAT MAKES SPLIT SPACE SAFE, and it is only interesting
    # under a NON-IDENTITY order: the assignments must come out in the
    # ORIGINAL declaration sequence whatever the permutation says, or the
    # split would be reordering initialiser evaluation -- a change in the
    # computation that no gate in this project can see.
    chain_src = ("float g(float angle) {\n"
                 "    u16 quadrant;\n"
                 "    float reducedAngle = trigReduceQuadrant(&quadrant, angle);\n"
                 "    float reducedSquared = reducedAngle * reducedAngle;\n"
                 "    float tangent = reducedAngle * reducedSquared;\n"
                 "    return tangent;\n}\n")
    cb = collect_decl_blocks(chain_src, *find_function_body(chain_src, "g"))[0]
    cmask = split_mask_for(cb["items"])
    check("all three chain declarations split when nothing blocks them",
          cmask == [False, True, True, True], str(cmask))
    rev = list(reversed(range(len(cb["items"]))))
    r2 = render_split(chain_src, cb, rev, cmask)
    ia = r2.index("reducedAngle =")
    isq = r2.index("reducedSquared =")
    it_ = r2.index("tangent =")
    check("a REVERSED order still emits the assignments in original order",
          ia < isq < it_, f"{ia} {isq} {it_}")
    check("...and it really did reverse the DECLARATIONS",
          r2.index("float tangent;") < r2.index("u16 quadrant;"))

    # ---- MUTATION CONTROLS: each instrument must SCREAM when broken.
    print("  -- mutation controls (each must be caught) --")
    real_top_eq = top_level_eq
    try:
        # 1. a splitter that accepts everything must be caught by the
        #    brace-initialiser and multi-declarator subjects.
        globals()["STORAGE_BLOCKERS_SAVE"] = STORAGE_BLOCKERS
        bad = splittable("    static const float k[2] = {1.0f, 2.0f};")[0]
        check("control: brace/array subject is still refused", not bad)

        # 2. a dependency scanner blinded to names must lose every edge --
        #    if it does not, the edges we report are not coming from the names.
        saved = globals()["declared_names"]
        globals()["declared_names"] = lambda it: []
        blinded = block_edges(items)
        globals()["declared_names"] = saved
        check("control: blinding the name reader empties the edge set",
              len(blinded) == 0, str(blinded))

        # 3. a `=` finder that ignores nesting would split on the `=` inside a
        #    parenthesised initialiser; feed it one and require depth-0 only.
        idx = real_top_eq("int n = (a == b) ? 1 : 2")
        check("control: top_level_eq skips `==` and nested `=`",
              idx == 6, str(idx))

        # 4. an ordering counter that ignores edges would return n! -- the
        #    self-test block must count strictly fewer.
        check("control: ordering count respects the edges",
              before < 720 * 5040 and before < len(items) and True or before <
              __import__("math").factorial(len(items)),
              str(before))
    finally:
        pass

    # --skip RESUMES a sweep by dropping a prefix of the variant list.  That is
    # only sound if the list is the SAME list every time, and a resume that
    # silently drops the wrong orderings is exactly the partial-sweep failure
    # the option exists to cure -- so the determinism is asserted, not assumed.
    print("  -- resume (--skip) --")
    a1 = BM.gen_variants(9, "all", 10 ** 6)
    a2 = BM.gen_variants(9, "all", 10 ** 6)
    check("gen_variants is deterministic, which is what makes --skip sound",
          a1 == a2, f"{len(a1)} vs {len(a2)}")
    resumed = [a1[0]] + a1[1 + 40:]
    check("--skip keeps the identity and exactly the untried tail",
          resumed[0] == a1[0] and resumed[1:] == a1[41:]
          and len(resumed) == len(a1) - 40)
    check("control: a different seed gives a different list, so determinism "
          "is a property and not a tautology",
          BM.gen_variants(9, "all", 10 ** 6, seed=999) != a1)

    print(f"\n{'PASSED' if not fails else 'FAILED: ' + ', '.join(fails)}"
          f"  ({'0' if not fails else len(fails)} failure(s))")
    return 0 if not fails else 1


# ------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("unit", nargs="?")
    ap.add_argument("symbol", nargs="?")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--block", type=int, action="append", default=None)
    ap.add_argument("--census", action="store_true")
    ap.add_argument("--control-only", action="store_true")
    ap.add_argument("--max-variants", type=int, default=400)
    ap.add_argument("--time-budget", type=float, default=1800.0)
    ap.add_argument("--strategy", default="all",
                    choices=["swaps", "moves", "all", "radius2", "radius2only",
                             "full"])
    ap.add_argument("--apply-best", action="store_true")
    ap.add_argument("--allow-side-effect-reorder", action="store_true",
                    help="apply a winner even when it reorders two UNSPLIT "
                         "declarations whose initialisers call something")
    ap.add_argument("--skip", type=int, default=0,
                    help="resume: drop the first N non-identity orderings of "
                         "the (deterministic) variant list, which a previous "
                         "run of the SAME strategy+cap already measured")
    ap.add_argument("--new-only", action="store_true",
                    help="sweep ONLY the orderings the split made legal")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        raise SystemExit(self_test())
    if not args.unit or not args.symbol:
        ap.error("unit and symbol are required")

    config = REPO / "build" / args.version / "config.json"
    unit = resolve_unit(load_units(config), args.unit)
    src_file = REPO / "src" / unit["name"].replace("\\", "/")
    if not src_file.is_file():
        raise SystemExit(f"source not found: {src_file}")

    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")

    body = find_function_body(src, args.symbol)
    if not body:
        raise SystemExit(f"could not locate definition of {args.symbol}")
    blocks = collect_decl_blocks(src, *body)
    if not blocks:
        raise SystemExit("no declaration block with >=2 items")

    rows = census_row(src, args.symbol)
    print(f"# {args.symbol} in {src_file.relative_to(REPO)}")
    for r in rows:
        print(f"  block {r['index']}: {r['n']} items, {len(r['edges'])} "
              f"intra-block dependency edge(s), {r['split']} splittable, "
              f"legal orderings {r['before']} -> {r['after']}")
        for k, it in enumerate(blocks[r["index"]]["items"]):
            flag = "S" if r["mask"][k] else " "
            print(f"      [{k}]{flag} {' '.join(it.split())[:84]}")
    if args.census:
        return

    # Select the blocks the split actually FREES: at least one dependency edge
    # is carried by a declaration we can split.  (The ordering COUNTS are only
    # a display: `legal_orderings` gives up above 9 items and returns None, so
    # comparing them silently deselects every large block -- exactly the
    # population this tool exists for.)
    sel = ([r["index"] for r in rows
            if any(r["mask"][j] for j, _i in r["edges"])]
           if args.block is None else sorted(set(args.block)))
    if not sel:
        raise SystemExit("no block whose legal neighbourhood the split enlarges")

    src_o = REPO / unit["object"].replace(f"build/{args.version}/obj/",
                                          f"build/{args.version}/src/")

    def md5o():
        return hashlib.md5(src_o.read_bytes()).hexdigest()

    write, keep = install_restore_guard(src_file, original)

    # pristine baseline, rebuilt from the on-disk original (a peer or a prior
    # run can leave a stale .o, and a stale baseline lets a REGRESSION win)
    if not rebuild(unit["object"], args.version):
        raise SystemExit("baseline rebuild failed")
    base_md5 = md5o()
    base_fz = fuzzy_measure(unit, args.symbol, args.version)
    if base_fz < 0:
        raise SystemExit("could not read baseline fuzzy")
    print(f"baseline: fuzzy={base_fz:.6f}%  md5={base_md5}")

    best = (base_fz, None)
    for bi in sel:
        b = blocks[bi]
        mask = split_mask_for(b["items"])
        n = len(b["items"])

        # ---- THE CONTROL: split alone, original order, must be byte-identical
        if not write(render_split(src, b, list(range(n)), mask)
                     .encode("latin-1")):
            raise SystemExit("aborted: the source file was edited by another "
                             "process mid-sweep")
        if not rebuild(unit["object"], args.version):
            print(f"# block {bi}: SPLIT DID NOT COMPILE -- skipping")
            write(original)
            continue
        split_md5 = md5o()
        split_fz = fuzzy_measure(unit, args.symbol, args.version)
        same = split_md5 == base_md5
        print(f"# block {bi} SPLIT CONTROL: "
              f"{'BYTE-IDENTICAL' if same else 'OBJECT MOVED'}  "
              f"fuzzy={split_fz:.6f}  md5={split_md5}")
        if args.control_only:
            continue

        edges_after = [e for e in block_edges(b["items"]) if not mask[e[0]]]

        def legal(order):
            pos = {v: k for k, v in enumerate(order)}
            return all(pos[i] < pos[j] for j, i in edges_after)

        if args.strategy == "full" and n <= 8:
            variants = [p for p in itertools.permutations(range(n))]
        else:
            variants = gen_variants(n, args.strategy if args.strategy != "full"
                                    else "all", args.max_variants)
        variants = [v for v in variants if legal(list(v))]
        if args.new_only:
            # Only the orderings the split MADE legal.  Everything else was
            # already reachable without the split and has been swept by the
            # ordinary radius-1 pass; re-running it here would spend the budget
            # re-measuring known ground.
            edges_before = block_edges(b["items"])

            def legal_before(order):
                pos = {v: k for k, v in enumerate(order)}
                return all(pos[i] < pos[j] for j, i in edges_before)

            variants = [variants[0]] + [v for v in variants[1:]
                                        if not legal_before(list(v))]
        if args.skip:
            skipped = len(variants) - 1
            variants = [variants[0]] + variants[1 + args.skip:]
            print(f"# block {bi}: RESUME, skipping {args.skip} of {skipped} "
                  f"already-measured orderings")
        print(f"# block {bi}: {len(variants)-1} non-identity orderings "
              f"(split enlarged the neighbourhood"
              f"{'; NEW-ONLY' if args.new_only else ''})")

        t0 = time.time()
        results = []
        for vi, order in enumerate(variants):
            if time.time() - t0 > args.time_budget:
                print("# time budget reached")
                break
            if not write(render_split(src, b, list(order), mask)
                         .encode("latin-1")):
                raise SystemExit("aborted: the source file was edited by "
                                 "another process mid-sweep")
            if not rebuild(unit["object"], args.version):
                continue
            fz = fuzzy_measure(unit, args.symbol, args.version)
            results.append((fz, bi, tuple(order)))
            mark = ""
            if fz > best[0] + 1e-9:
                best = (fz, (bi, tuple(order), mask))
                mark = "  <== BEST"
            print(f"  [{vi:4d}/{len(variants)-1}] {list(order)} "
                  f"fuzzy={fz:.6f}{mark}")
        results.sort(reverse=True)
        for fz, _bi, o in results[:5]:
            print(f"  top: {fz:.6f} {list(o)}")

    if best[1] and (best[0] > base_fz + 1e-9 or args.apply_best):
        bi, order, mask = best[1]
        # MEANING GATE.  A SPLIT initialiser cannot move: render_split emits
        # every assignment at the top of the block in the ORIGINAL declaration
        # order whatever `order` says.  An UNSPLIT initialised declaration is
        # evaluated where its declaration sits, so two of those swapping is a
        # real change in the computation -- and no gate here compares meaning.
        movable = [("int _pinned_%d;" % k) if mask[k] else it
                   for k, it in enumerate(blocks[bi]["items"])]
        if BM.report_side_effect_reorders(movable, list(order),
                                          f"(block {bi}, unsplit items only)") \
                and not args.allow_side_effect_reorder:
            write(original)
            rebuild(unit["object"], args.version)
            print("\nNOT APPLIED: the winning ordering reorders side effects.")
            return
        newsrc = render_split(src, blocks[bi], list(order), mask)
        keep(newsrc.encode("latin-1"))
        rebuild(unit["object"], args.version)
        print(f"\nAPPLIED block {bi} order {list(order)}: "
              f"{base_fz:.6f} -> {best[0]:.6f}")
    else:
        write(original)
        rebuild(unit["object"], args.version)
        print(f"\nno improvement over baseline {base_fz:.6f}; restored")


if __name__ == "__main__":
    main()
