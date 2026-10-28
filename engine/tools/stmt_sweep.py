#!/usr/bin/env python3
"""Brute-force a function's register allocation by permuting the ORDER OF ITS
ASSIGNMENT STATEMENTS (first-definition order), rebuilding + measuring each
variant against true objdiff fuzzy%.

WHY THIS EXISTS -- the lever tools/brute_match.py cannot reach
-------------------------------------------------------------
Declaration order keys REGISTER ASSIGNMENT; the order of the assignment
statements keys EMISSION ORDER.  They are different axes and a function can be
inert on one and not the other, so a declaration sweep coming back flat says
nothing about how the same statements would score written in another sequence.
That second axis is what this tool permutes.

(An earlier rationale here claimed the saved-GPR band was filled in two phases,
with a "phase 1" ordered by reverse first-definition order for webs that never
reach a loop header.  That model has been REFUTED by measurement and must not
be reinstated: the saved band is contiguous and monotone downward, and the FPR
band has no separate law.  The tool's own axis stands on its own.)

WHAT IT PERMUTES
----------------
Maximal runs of ADJACENT simple assignment statements at one scope level, in
every scope of the function (body, loop bodies, if/else arms, case blocks).
A statement joins a run only when it is provably reorderable:

  * form is exactly `name = expr;` or `name <op>= expr;`
  * `name` is a local declared in this function (or a parameter)
  * `name` never has its address taken anywhere in the function
  * `expr` contains NO call, NO `++`/`--`, NO nested assignment, no top-level
    comma

Everything else -- a call, a store through a pointer (`p->f = x`, `a[i] = x`,
`*p = x`), a declaration, any control flow -- is a BARRIER and terminates the
run.  Because the only memory WRITES in a run are to non-address-taken locals,
memory reads inside the run commute freely, so the only orderings that must be
preserved are the RAW / WAR / WAW edges over those local names.  Every emitted
permutation is checked against that dependence relation, so a rewrite is always
semantics-preserving.

Runs are swept one at a time by coordinate ascent -- each run is permuted with
the others pinned at the best ordering found so far.

Usage:
  python3 tools/stmt_sweep.py <unit> <symbol> [-v GSAE01]
      [--list-runs] [--run N] [--max-variants N] [--time-budget SECONDS]
      [--strategy swaps|moves|all] [--min-items N] [--dry-run]

Notes:
  * SJIS-safe: the file is read/written as latin-1 (byte-transparent).
  * Each statement keeps its exact source text (multi-line expressions move as
    one unit).
  * Crash-safe: a `<file>.stmtbak` sidecar holds the pristine bytes for the
    whole run; a hard kill is repaired on the next invocation.
  * SHARED-TREE safe: the final restore only fires when the on-disk bytes are
    still the ones THIS process last wrote.  A concurrent peer edit inside the
    sweep window aborts the restore loudly and leaves the sidecar in place
    rather than silently reverting the peer's work.
"""
from __future__ import annotations

import argparse
import itertools
import random
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from function_objdump import (
    load_units,
    resolve_unit,
    objdump_symbol,
    strip_preamble,
)
from ndiff import normalize
from brute_match import (
    REPO,
    TYPE_TOKENS,
    fuzzy_measure,
    collect_decl_blocks,
    find_function_body,
    find_objdump,
    find_scope_blocks,
    match_score,
    objdump_norm,
    objdump_paths,
    rebuild,
    skip_string,
    skip_ws_comments,
)

IDENT = re.compile(r"(?<![\w.])[A-Za-z_]\w*")
CALL = re.compile(r"[A-Za-z_]\w*\s*\(")
KEYWORDS = {
    "return", "if", "for", "while", "do", "switch", "goto", "case", "default",
    "break", "continue", "else", "asm", "sizeof", "typedef",
}


# ----------------------------------------------------------- source scanning
def strip_comments(s: str) -> str:
    """Blank out comments and string bodies, preserving length."""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        if s.startswith("//", i):
            while i < n and s[i] != "\n":
                out[i] = " "
                i += 1
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if s[k] != "\n":
                    out[k] = " "
            i = j
        elif s[i] in "\"'":
            j = skip_string(s, i)
            for k in range(i + 1, min(j, n) - 1):
                out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def match_brace(src: str, i: int, limit: int) -> int:
    """Index just past the `}` matching the `{` at i."""
    depth = 0
    n = min(len(src), limit)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            i = skip_string(src, i)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def scan_statement(src: str, start: int, limit: int):
    """End index of the top-level statement beginning at `start`.

    Ends at the first `;` at paren-depth 0, or just past the `}` matching a
    `{` seen at paren-depth 0 (a compound statement).  A `{` that follows `=`
    or `,` is a brace INITIALISER: it is skipped whole and scanning continues
    to the terminating `;`.  `else` / `while(c);` tails are simply returned as
    their own statements -- both classify as barriers, so run detection is
    unaffected and the scanner stays simple.
    """
    depth = 0
    last = ""
    i = start
    n = min(len(src), limit)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            i = skip_string(src, i)
            last = c
            continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "{":
            j = match_brace(src, i, limit)
            if last in "=,":          # brace initialiser -- keep going
                i = j
                continue
            return j                   # compound statement
        elif c == "}":
            return i                   # ran off the end of the scope
        elif c == ";" and depth == 0:
            end = i + 1
            j = end
            while j < n and src[j] in " \t":
                j += 1
            if src.startswith("//", j):
                while j < n and src[j] != "\n":
                    j += 1
                end = j
            return end
        if not c.isspace():
            last = c
        i += 1
    return None


def iter_statements(src: str, scope_open: int, scope_close: int):
    """(start, end) for each top-level statement inside a `{...}` scope."""
    i = scope_open + 1
    while True:
        i = skip_ws_comments(src, i)
        if i >= scope_close or src[i] == "}":
            return
        end = scan_statement(src, i, scope_close)
        if end is None or end <= i:
            return
        yield i, end
        i = end


# -------------------------------------------------------- statement analysis
def top_level_assign(core: str):
    """(lhs_end, compound) for the single top-level assignment op, else None.

    `lhs_end` excludes the whole operator, so `x += 1` yields lhs `x`, not
    `x +`.  Comparisons (`==`, `!=`, `<=`, `>=`) are not assignments.
    """
    depth = 0
    found = None
    for i, c in enumerate(core):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "=" and depth == 0:
            if core[i + 1:i + 2] == "=":
                continue
            prev = core[i - 1] if i else ""
            if prev == "=" or prev == "!":
                continue
            if prev in "<>":
                # `<=`/`>=` compare vs `<<=`/`>>=` shift-assign
                if core[i - 2:i - 1] != prev:
                    continue
                op = 2
            elif prev in "+-*/%&|^":
                op = 1
            else:
                op = 0
            if found is not None:
                return None            # chained assignment -- refuse
            found = (i - op, i + 1, op > 0)
    return found


def classify(core_raw: str, locals_: set, addr_taken: set):
    """(lhs, uses, compound) when `core_raw` is a reorderable assignment."""
    core = strip_comments(core_raw)
    semi = core.rfind(";")
    if semi < 0:
        return None
    core = core[:semi]
    head = core.lstrip()
    tok = IDENT.match(head)
    if not tok or tok.group(0) in KEYWORDS:
        return None
    hit = top_level_assign(core)
    if hit is None:
        return None
    lhs_end, rhs_start, compound = hit
    lhs = core[:lhs_end].strip()
    if not re.fullmatch(r"[A-Za-z_]\w*", lhs):
        return None                    # store through a pointer/index/member
    if lhs not in locals_ or lhs in addr_taken:
        return None
    rhs = core[rhs_start:]
    if CALL.search(rhs) or "++" in rhs or "--" in rhs or "=" in rhs:
        return None
    depth = 0
    for c in rhs:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            return None
    uses = {m.group(0) for m in IDENT.finditer(rhs)}
    if compound:
        uses.add(lhs)
    return lhs, uses, compound


def declared_names(src: str, body_open: int, body_end: int, sym: str) -> set:
    """Locals declared anywhere in the function, plus its parameters."""
    names = set()
    for b in collect_decl_blocks(src, body_open, body_end, min_items=1):
        for item in b["items"]:
            txt = strip_comments(item)
            toks = IDENT.findall(txt)
            for t in toks[1:]:
                if t not in TYPE_TOKENS:
                    names.add(t)
    # parameter list
    head = src.rfind(sym, 0, body_open)
    if head >= 0:
        params = src[head + len(sym):body_open]
        for t in IDENT.findall(strip_comments(params)):
            if t not in TYPE_TOKENS:
                names.add(t)
    return names


def address_taken(src: str, body_open: int, body_end: int) -> set:
    body = strip_comments(src[body_open:body_end])
    return {m.group(1) for m in re.finditer(r"&\s*([A-Za-z_]\w*)", body)}


# ------------------------------------------------------------ run detection
def collect_runs(src: str, body_open: int, body_end: int, sym: str,
                 min_items: int = 2):
    """Maximal runs of adjacent reorderable assignments, in source order.

    -> [{'start','end','indent','items','deps','depth'}]
    `deps` is the set of ordered index pairs (i, j), i<j, that MUST keep their
    relative order (RAW / WAR / WAW on a local).
    """
    locals_ = declared_names(src, body_open, body_end, sym)
    addr = address_taken(src, body_open, body_end)
    scopes = find_scope_blocks(src, body_open, body_end)
    depth_of = {o: sum(1 for o2, c2 in scopes if o2 < o and c2 > o)
                for o, c in scopes}
    runs = []
    for o, c in scopes:
        cur = []
        for s, e in iter_statements(src, o, c):
            info = classify(src[s:e], locals_, addr)
            if info is None:
                if len(cur) >= min_items:
                    runs.append((o, cur))
                cur = []
                continue
            cur.append((s, e, info))
        if len(cur) >= min_items:
            runs.append((o, cur))

    out = []
    for o, items in runs:
        deps = set()
        for i in range(len(items)):
            for j in range(i + 1, len(items)):
                di, ui, _ = items[i][2]
                dj, uj, _ = items[j][2]
                if di in uj or dj in ui or di == dj:
                    deps.add((i, j))
        if len(deps) == len(items) * (len(items) - 1) // 2:
            continue                   # fully ordered -- nothing to permute
        start = items[0][0]
        end = items[-1][1]
        nl = src.rfind("\n", 0, start)
        indent = src[nl + 1:start] if nl >= 0 else "    "
        if indent.strip():
            indent = " " * 4
        out.append({
            "start": start, "end": end, "indent": indent,
            "items": [src[s:e] for s, e, _ in items],
            "names": [inf[0] for _, _, inf in items],
            "deps": deps, "depth": depth_of.get(o, 0),
        })
    out.sort(key=lambda r: r["start"])
    return out


def valid(order, deps) -> bool:
    pos = {v: k for k, v in enumerate(order)}
    return all(pos[i] < pos[j] for i, j in deps)


def gen_variants(n: int, deps, strategy: str, cap: int, seed: int = 12345):
    """Dependence-legal orderings, identity first."""
    base = tuple(range(n))
    seen = {base}
    out = []

    def add(order):
        t = tuple(order)
        if t not in seen and valid(t, deps):
            seen.add(t)
            out.append(t)

    if n <= 7:                          # exhaustive when it is cheap
        for p in itertools.permutations(range(n)):
            add(p)
    else:
        if strategy in ("swaps", "all"):
            for a, b in itertools.combinations(range(n), 2):
                o = list(base)
                o[a], o[b] = o[b], o[a]
                add(o)
        if strategy in ("moves", "all"):
            for s_i in range(n):
                for d in range(n):
                    if s_i != d:
                        o = list(base)
                        o.insert(d, o.pop(s_i))
                        add(o)
        if strategy == "all":
            add(list(reversed(base)))
            rng = random.Random(seed)
            for _ in range(200):
                o = list(base)
                rng.shuffle(o)
                add(o)
    return [base] + out[: max(0, cap - 1)]


# ------------------------------------------------------------ crash safety
def install_restore_guard(src_file: Path, original: bytes):
    """Restore-on-exit that never clobbers a concurrent peer edit.

    `restore()` compares the on-disk bytes against the bytes THIS process last
    wrote.  If they differ, another lane touched the file inside our sweep
    window: we refuse to write, keep the `.stmtbak` sidecar (which holds our
    pristine copy) and say so loudly, instead of silently reverting the peer.
    """
    import atexit
    import os
    import signal

    bak = src_file.with_suffix(src_file.suffix + ".stmtbak")
    bak.write_bytes(original)
    state = {"done": False, "last": original}

    def restore():
        if state["done"]:
            return
        state["done"] = True
        try:
            on_disk = src_file.read_bytes()
        except OSError:
            return
        if on_disk == original:
            return
        if on_disk != state["last"]:
            print(f"\n!! {src_file.name} was modified by another process "
                  f"during this sweep -- NOT restoring.\n"
                  f"!! pristine bytes are preserved in {bak.name}; "
                  f"reconcile by hand.", file=sys.stderr)
            state["keepbak"] = True
            return
        src_file.write_bytes(original)

    def finish():
        restore()
        if not state.get("keepbak"):
            try:
                bak.unlink()
            except OSError:
                pass

    def on_signal(_s, _f):
        finish()
        os._exit(130)

    atexit.register(finish)
    sigs = [signal.SIGINT, signal.SIGTERM]
    if hasattr(signal, "SIGHUP"):
        sigs.append(signal.SIGHUP)
    for s in sigs:
        try:
            signal.signal(s, on_signal)
        except (ValueError, OSError):
            pass

    def write(data: bytes) -> bool:
        """Write `data`, but only while the file still holds OUR last bytes."""
        try:
            on_disk = src_file.read_bytes()
        except OSError:
            on_disk = state["last"]
        if on_disk != state["last"]:
            print(f"\n!! {src_file.name} changed underneath this sweep "
                  f"(peer edit) -- refusing to write.\n"
                  f"!! pristine bytes preserved in {bak.name}.",
                  file=sys.stderr)
            state["done"] = True
            state["keepbak"] = True
            return False
        state["last"] = data
        src_file.write_bytes(data)
        return True

    def keep(data: bytes):
        state["done"] = True
        src_file.write_bytes(data)
        try:
            bak.unlink()
        except OSError:
            pass

    return write, keep


def recover_stale_backup(src_file: Path):
    bak = src_file.with_suffix(src_file.suffix + ".stmtbak")
    if bak.is_file():
        good = bak.read_bytes()
        if src_file.read_bytes() != good:
            print(f"# recovering {src_file.name} from a killed run's backup")
            src_file.write_bytes(good)
        bak.unlink()


def dirty_in_git(src_file: Path) -> bool:
    r = subprocess.run(["git", "status", "--porcelain", "--", str(src_file)],
                       cwd=REPO, capture_output=True, text=True)
    return bool(r.stdout.strip())


# --------------------------------------------------------------------- main
# ---------------------------------------------------------- control suite
#
# `collect_runs` decides which statements may join a run AND derives the
# dependence relation that keeps their reorderings legal -- one function, one
# model, checking itself.  If `classify` under-reports a statement's uses, the
# missing RAW edge is missing from `deps` too, `valid()` agrees, and an illegal
# ordering is emitted with nothing to catch it: no build gate in this project
# compares retail's MEANING, so it would land as a win.
#
# So the control below re-derives the relation a SECOND time, from the
# statement TEXT, with no use of `classify`, `top_level_assign` or `IDENT`'s
# call sites -- and every ordering the tool would emit must be legal under
# BOTH.  `--self-test --mutate` corrupts `classify` on purpose and requires the
# independent relation to catch the illegal ordering that results; a control
# that has never been shown to fail is not a control.

_ASSIGN_OPS = ("<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
               "=")


def _independent_deps(items):
    """RAW/WAR/WAW pairs derived from the raw statement text.

    Deliberately naive and deliberately SEPARATE: split at the first assignment
    operator that is not a comparison, take the whole left side as the written
    name and every identifier on the right as read.  A compound operator also
    reads its target.  Over-approximating is fine -- this relation only has to
    be a SUPERSET of the truth to catch a missing edge.
    """
    import re as _re
    ident = _re.compile(r"[A-Za-z_]\w*")
    parsed = []
    for text in items:
        core = text.split(";")[0]
        cut = None
        for k, ch in enumerate(core):
            if ch != "=":
                continue
            if core[k + 1:k + 2] == "=" or core[k - 1:k] in ("=", "!", "<",
                                                             ">"):
                continue
            cut = k
            break
        if cut is None:
            parsed.append((None, set()))
            continue
        lhs, rhs = core[:cut], core[cut + 1:]
        compound = lhs.rstrip()[-1:] in "+-*/%&|^<>"
        names = ident.findall(lhs)
        w = names[0] if names else None
        r = set(ident.findall(rhs)) | set(names[1:])
        if compound and w:
            r.add(w)
        parsed.append((w, r))
    deps = set()
    for i in range(len(parsed)):
        for j in range(i + 1, len(parsed)):
            wi, ri = parsed[i]
            wj, rj = parsed[j]
            if wi is None or wj is None:
                deps.add((i, j))
                continue
            if wi in rj or wj in ri or wi == wj:
                deps.add((i, j))
    return deps


_SELFTEST_SRC = """
void probe(void) {
    int a;
    int b;
    int c;
    int d;
    int e;
    a = 1;
    b = 2;
    c = 3;
    d = a + b;
    e = d * c;
}
"""


def self_test(mutate: bool = False, verbose: bool = True) -> int:
    """Every ordering this tool would emit must be legal under an
    INDEPENDENTLY derived dependence relation."""
    bad = 0

    def chk(label, cond, detail=""):
        nonlocal bad
        if not cond:
            bad += 1
        print("  %-58s %s %s" % (label, "PASS" if cond else "*** FAIL ***",
                                 detail))

    src = _SELFTEST_SRC
    o = src.index("{", src.index("probe"))
    c = match_brace(src, o, len(src))
    undo = None
    if mutate:
        g = globals()
        real = g["classify"]

        def blind(core_raw, locals_, addr_taken):
            r = real(core_raw, locals_, addr_taken)
            return None if r is None else (r[0], set(), r[2])
        g["classify"] = blind
        undo = lambda: g.__setitem__("classify", real)      # noqa: E731
    try:
        runs = collect_runs(src, o, c, "probe", min_items=2)
        chk("the probe body yields one permutable run", len(runs) == 1,
            "(%d)" % len(runs))
        if not runs:
            return 1
        r = runs[0]
        chk("the run holds all five assignments", len(r["items"]) == 5,
            "(%d)" % len(r["items"]))
        indep = _independent_deps(r["items"])
        chk("the tool's relation contains the independent one",
            indep <= r["deps"] or mutate,
            "missing %s" % sorted(indep - r["deps"]))
        orders = gen_variants(len(r["items"]), r["deps"], "all", 100000)
        illegal = [tuple(x) for x in orders if not valid(x, indep)]
        if mutate:
            chk("MUTATION CONTROL: a blinded `classify` produces orderings "
                "the independent relation rejects", bool(illegal),
                "(%d of %d)" % (len(illegal), len(orders)))
        else:
            chk("every emitted ordering is legal under BOTH relations",
                not illegal, "(%d of %d illegal)" % (len(illegal),
                                                     len(orders)))
            chk("there is something to permute at all", len(orders) > 1,
                "(%d orderings)" % len(orders))
            # d = a + b reads a and b; e = d * c reads d and c.
            names = r["names"]
            need = {(names.index("a"), names.index("d")),
                    (names.index("b"), names.index("d")),
                    (names.index("c"), names.index("e")),
                    (names.index("d"), names.index("e"))}
            chk("the known RAW chain is present", need <= r["deps"],
                "missing %s" % sorted(need - r["deps"]))
    finally:
        if undo:
            undo()
    print("\nstmt_sweep self-test: %s" % ("PASSED" if not bad
                                           else "%d FAILURE(S)" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("unit", nargs="?")
    ap.add_argument("symbol", nargs="?")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--mutate", action="store_true",
                    help="with --self-test: blind `classify` and "
                         "require the independent relation to catch it")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--max-variants", type=int, default=60)
    ap.add_argument("--time-budget", type=float, default=1200.0)
    ap.add_argument("--strategy", choices=["swaps", "moves", "all"],
                    default="all")
    ap.add_argument("--min-items", type=int, default=2)
    ap.add_argument("--list-runs", action="store_true")
    ap.add_argument("--run", type=int, action="append", default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="sweep even if the file already has uncommitted "
                         "changes (default: refuse -- a peer lane may own it)")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args.mutate)
    if not (args.unit and args.symbol):
        ap.error("give a unit and a symbol, or --self-test")

    config = REPO / "build" / args.version / "config.json"
    unit = resolve_unit(load_units(config), args.unit)
    src_file = REPO / "src" / unit["name"].replace("\\", "/")
    if not src_file.is_file():
        raise SystemExit(f"source not found: {src_file}")
    objdump = find_objdump()
    tgt_o, cur_o = objdump_paths(unit, args.version)

    recover_stale_backup(src_file)
    original = src_file.read_bytes()
    src = original.decode("latin-1")

    body = find_function_body(src, args.symbol)
    if not body:
        raise SystemExit(f"could not locate definition of {args.symbol}")
    body_open, body_end = body

    runs = collect_runs(src, body_open, body_end, args.symbol, args.min_items)

    def lineno(idx):
        return src.count("\n", 0, idx) + 1

    print(f"# {args.symbol} in {src_file.relative_to(REPO)}")
    if not runs:
        raise SystemExit("no reorderable assignment run; nothing to permute")
    print(f"# {len(runs)} reorderable assignment run(s):")
    for ri, r in enumerate(runs):
        legal = len(gen_variants(len(r["items"]), r["deps"], args.strategy,
                                 10 ** 9)) - 1
        print(f"  run {ri}: depth={r['depth']} line {lineno(r['start'])}"
              f"-{lineno(r['end'])}  {len(r['items'])} stmts, "
              f"{legal} legal reorderings")
        for k, it in enumerate(r["items"]):
            print(f"      [{k}] {' '.join(it.split())[:88]}")
    if args.list_runs:
        return

    if not args.allow_dirty and dirty_in_git(src_file):
        raise SystemExit(f"{src_file.relative_to(REPO)} has uncommitted "
                         "changes -- another lane may own it (--allow-dirty "
                         "to override)")

    sel = list(range(len(runs))) if args.run is None else sorted(set(args.run))
    for ri in sel:
        if not 0 <= ri < len(runs):
            raise SystemExit(f"--run {ri} out of range (0..{len(runs)-1})")

    def render(orders):
        out = src
        for ri in sorted(orders, reverse=True):
            r = runs[ri]
            it = r["items"]
            order = orders[ri]
            first = it[order[0]].lstrip()
            rest = "".join("\n" + r["indent"] + it[k].lstrip()
                           for k in order[1:])
            out = out[:r["start"]] + first + rest + out[r["end"]:]
        return out

    plan = {ri: gen_variants(len(runs[ri]["items"]), runs[ri]["deps"],
                             args.strategy, args.max_variants) for ri in sel}
    total = sum(len(v) - 1 for v in plan.values())
    print(f"# sweeping runs {sel}: {total} legal non-identity orderings "
          f"(cap={args.max_variants}/run, budget={args.time_budget}s)")

    if args.dry_run:
        for ri in sel:
            vs = plan[ri]
            if len(vs) < 2:
                continue
            print(f"# run {ri} example {list(vs[1])}:")
            new = render({ri: vs[1]})
            print(new[runs[ri]["start"]:runs[ri]["end"] + 40])
        return

    write, keep = install_restore_guard(src_file, original)

    # ONE measurement, shared with the declaration sweeper.  This was a
    # verbatim copy of `brute_match.fuzzy_measure`, INCLUDING the unguarded
    # `f["fuzzy_match_percent"]` -- objdiff omits that key at 0.0, so a probe
    # that zeroed the function raised KeyError straight out of the sweep loop
    # and killed the run, and a sweep that dies part-way reads exactly like a
    # sweep that finished and found nothing.  The copy in brute_match was
    # fixed; this one was not, because the fix was made where the defect was
    # noticed rather than everywhere the code had been pasted.

    def fuzzy():
        return fuzzy_measure(unit, args.symbol, args.version)

    t_norm = objdump_norm(objdump, tgt_o, args.symbol)

    def proxy():
        return match_score(t_norm, objdump_norm(objdump, cur_o, args.symbol))

    # pin the baseline to the ON-DISK source: in a shared tree the .o can be
    # stale, and a stale (too low) baseline lets a REGRESSION look like a win.
    rebuild(unit["object"], args.version)
    base_px, base_reg = proxy()
    if base_px < 0:
        raise SystemExit(f"cannot disassemble {args.symbol} -- is this unit "
                         "part of the build?")
    base_fz = fuzzy()
    if base_fz < 0:
        raise SystemExit("could not read baseline fuzzy; retry.")
    print(f"baseline: fuzzy={base_fz:.4f}%  proxy={base_px:.3f}%  "
          f"regions={base_reg}")

    held: dict[int, tuple] = {}
    cur_fz, cur_px = base_fz, base_px
    results = []
    t0 = time.time()
    stop = False

    class PeerEdit(Exception):
        pass

    def trial(orders):
        if not write(render(orders).encode("latin-1")):
            raise PeerEdit()
        if not rebuild(unit["object"], args.version):
            return None
        px, reg = proxy()
        return fuzzy(), px, reg

    try:
        for ri in sel:
            if stop:
                break
            variants = plan[ri]
            print(f"\n# --- run {ri} (depth {runs[ri]['depth']}, "
                  f"{len(runs[ri]['items'])} stmts, {len(variants) - 1} "
                  f"orderings) ---")
            best = None
            for vi, order in enumerate(variants):
                if vi == 0:
                    continue
                if time.time() - t0 > args.time_budget:
                    print(f"# time budget hit (run {ri}, variant {vi})")
                    stop = True
                    break
                res = trial({**held, ri: order})
                if res is None:
                    print(f"[r{ri} {vi:3d}] BUILD FAIL {list(order)}")
                    continue
                fz, px, reg = res
                results.append((fz, px, reg, ri, order))
                flag = ""
                if (fz, px) > (cur_fz, cur_px) and (best is None
                                                    or (fz, px) > best[:2]):
                    best = (fz, px, order)
                    flag = "  <== best"
                note = " (fuzzy read FAILED)" if fz < 0 else ""
                print(f"[r{ri} {vi:3d}] fuzzy={fz:8.4f}% proxy={px:7.3f}% "
                      f"reg={reg:2d} {list(order)}{flag}{note}")
            if best and best[0] > cur_fz + 1e-4:
                held[ri] = best[2]
                print(f"# run {ri}: HOLD {list(best[2])}  "
                      f"fuzzy {cur_fz:.4f}% -> {best[0]:.4f}%")
                cur_fz, cur_px = best[0], best[1]
            else:
                print(f"# run {ri}: inert (best {cur_fz:.4f}%)")
    except PeerEdit:
        raise SystemExit("aborted: the source file was edited by another "
                         "process mid-sweep")
    finally:
        write(original)

    print("\n# ranked by FUZZY (top 12):")
    for fz, px, reg, ri, order in sorted(results,
                                         key=lambda r: (-r[0], -r[1]))[:12]:
        print(f"  fuzzy={fz:8.4f}% proxy={px:7.3f}% reg={reg:2d} "
              f"run {ri} {list(order)}")

    if held and cur_fz > base_fz + 1e-4:
        newbytes = render(held).encode("latin-1")
        write(newbytes)
        rebuild(unit["object"], args.version)
        confirm = fuzzy()
        print(f"\n# APPLIED: fuzzy {base_fz:.4f}% -> {confirm:.4f}%")
        for ri, order in sorted(held.items()):
            print(f"#   run {ri} order = {list(order)}")
        if confirm <= base_fz + 1e-4:
            print("# WARNING: re-measured fuzzy did NOT confirm -- restoring.")
            write(original)
            rebuild(unit["object"], args.version)
        else:
            keep(newbytes)
    else:
        rebuild(unit["object"], args.version)
        print(f"\n# no fuzzy improvement (best {cur_fz:.4f}% vs base "
              f"{base_fz:.4f}%); restored original.")


if __name__ == "__main__":
    main()
