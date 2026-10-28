#!/usr/bin/env python3
"""Brute-force a function's register allocation by permuting its local
declaration ORDER (decl order sets saved-register homes, per CLAUDE.md) and
rebuilding + measuring each variant.

It sweeps EVERY declaration block in the function: the leading block of the
function body AND the leading block of every nested scope (loop bodies,
if/else blocks, case blocks, bare blocks).  Blocks are swept one at a time by
coordinate ascent -- each block is permuted with the others pinned at the best
ordering found so far -- with an optional --cross pass over the small blocks.

For each candidate ordering it:
  1. rewrites one declaration block of the target function,
  2. rebuilds ONLY that unit's src .o (locked_ninja),
  3. measures the function's instruction-match against the retail target .o
     (objdump both, normalize branch labels, SequenceMatcher ratio),
and tracks the best. At the end it applies the best variant if it beats the
baseline, otherwise restores the original file byte-for-byte.

The inner metric is a proxy (exact normalized-instruction match ratio, which is
sensitive to reg-perm diffs); confirm the true objdiff fuzzy% with a report.json
regen afterwards.

Usage:
  python3 tools/brute_match.py <unit> <symbol> [-v GSAE01]
      [--max-variants N] [--time-budget SECONDS]
      [--strategy swaps|moves|all|radius2|radius2only]
      [--dry-run] [--apply-best]

  --strategy radius2      radius 1 (every swap and every move) followed by
                          every composition of TWO INDEPENDENT edits -- pairs
                          with disjoint supports, smallest total disturbance
                          first.  n<=5 blocks get the full symmetric group.
  --strategy radius2only  the same two-edit compositions WITHOUT the radius-1
                          prefix.  On a big block that prefix alone exceeds any
                          sane cap (81 items = 3240 swaps before a single
                          move), so a capped `radius2` run never reaches
                          radius 2; use this once radius 1 is known swept.

  --dry-run       parse + print the decl block and the variants it WOULD try,
                  build nothing.
  --apply-best    write the best-scoring variant even if the tool is re-run
                  (default: applies best iff strictly better than baseline).

  --list-blocks   print every permutable declaration block and exit.
  --block N       sweep only block N (repeatable).
  --cross         after the per-block pass, try the cross-product of the
                  small (<=4 item) blocks.
  --top-level-only  restore the historical behaviour (body's leading block).

Notes:
  * SJIS-safe: the file is read/written as latin-1 (byte-transparent).
  * Each item keeps its exact source text (multi-line union/struct decls are
    moved as one unit).
  * Crash-safe: a `<file>.brutebak` sidecar holds the pristine bytes for the
    whole run and a hard kill is repaired on the next invocation, so a killed
    sweep can never leave the target file mid-permutation.

WHY NESTED SCOPES MATTER: declaration order is the key to register assignment,
and a local declared inside a loop body is a declaration like any other -- it
just is not in the block this tool used to look at.  Any historical
"declaration order is inert" verdict for a function with inner-scope
declarations was measured against the top-level block only and is void.

(A "two-phase saved band" was once asserted here as the mechanism.  It has been
REFUTED by measurement and must not be reinstated: the saved band is contiguous
and monotone downward, and the FPR band has no separate law.)

WHY A CAP IS NOT A ZERO: an 81-item block is 3240 swaps before a single move,
so any run under a cap -- or under one --strategy -- leaves most of the block
unvisited.  A row swept that way is UNSWEPT, not inert.
"""
from __future__ import annotations

import argparse
import itertools
import random
import re
import subprocess
import sys
import tempfile
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
import difflib
import os
import shutil


def shell_bash() -> str:
    """Path to a POSIX bash that can run the repo's shell tools.

    On Windows a bare "bash" resolves to WSL's System32\\bash.exe, which cannot
    see the repo's Windows paths: locked_ninja then fails *after* the caller has
    already deleted the .o, so every sweep reports a spurious build failure.
    """
    if os.name != "nt":
        return "bash"
    for env in ("GIT_BASH", "BASH"):
        p = os.environ.get(env)
        if p and Path(p).is_file():
            return p
    for p in (r"C:\Program Files\Git\bin\bash.exe",
              r"C:\Program Files (x86)\Git\bin\bash.exe"):
        if Path(p).is_file():
            return p
    found = shutil.which("bash")
    if found and "system32" not in found.lower():
        return found
    return "bash"

REPO = Path(__file__).resolve().parent.parent

TYPE_TOKENS = {
    "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
    "int", "uint", "char", "short", "long", "float", "double",
    "void", "f32", "f64", "f80", "bool", "BOOL", "u128",
    "unsigned", "signed", "const", "static", "volatile", "register",
    "struct", "union", "enum", "undefined4", "undefined2", "undefined1",
    "undefined", "size_t", "vec3", "Vec", "Vec3f", "Mtx", "GXColor",
}


# ------------------------------------------------------------------ scoring
def objdump_paths(unit: dict, version: str):
    tgt = REPO / unit["object"]
    cur = REPO / unit["object"].replace(f"build/{version}/obj/",
                                        f"build/{version}/src/")
    return tgt, cur


def objdump_norm(objdump: Path, obj: Path, symbol: str):
    """Normalized instruction list, or [] when the symbol cannot be read.

    A unit that is not part of the build (no src .o) used to surface as a bare
    CalledProcessError traceback out of a batch run, which reads exactly like
    "swept and inert". match_score() maps [] to a sentinel -1.0 instead.
    """
    try:
        return normalize(strip_preamble(objdump_symbol(objdump, obj, symbol)))
    except Exception:
        return []


def match_score(t: list[str], c: list[str]):
    """(pct, regions): exact-normalized-instruction match ratio and #regions.

    This is only a fast pre-filter proxy. The DECIDING metric is the true
    objdiff fuzzy% (fuzzy_measure below); a sibling proved region-count can be
    ANTI-correlated with fuzzy for unroll/schedule-sensitive functions, so never
    rank or commit on this proxy alone.
    """
    if not t or not c:
        return -1.0, 999
    sm = difflib.SequenceMatcher(None, t, c, autojunk=False)
    matched = sum(b.size for b in sm.get_matching_blocks())
    pct = 200.0 * matched / (len(t) + len(c))
    regions = sum(1 for op in sm.get_opcodes() if op[0] != "equal")
    return pct, regions


# --------------------------------------------------- ground-truth fuzzy%
def _rv(b, i):
    r = 0; s = 0
    while True:
        x = b[i]; i += 1; r |= (x & 0x7f) << s
        if not x & 0x80:
            break
        s += 7
    return r, i


def _fields(b):
    i = 0; o = []
    while i < len(b):
        t, i = _rv(b, i); f = t >> 3; w = t & 7
        if w == 0:
            v, i = _rv(b, i); o.append((f, 0, v))
        elif w == 2:
            l, i = _rv(b, i); o.append((f, 2, b[i:i + l])); i += l
        elif w == 5:
            o.append((f, 5, b[i:i + 4])); i += 4
        elif w == 1:
            o.append((f, 1, b[i:i + 8])); i += 8
    return o


def report_unit_name(unit: dict) -> str:
    """config unit name (e.g. 'main/render.c') -> report/proto name
    ('main/main/render')."""
    name = unit["name"].replace("\\", "/").rsplit(".", 1)[0]
    return "main/" + name


def decode_fuzzy(binpb: bytes, report_unit: str, symbol: str):
    import struct as _st
    for f, w, v in _fields(binpb):
        if f != 2 or w != 2:
            continue
        u = _fields(v); un = None
        for a, b2, c in u:
            if a == 1 and b2 == 2:
                un = c.decode(errors="replace")
        if un != report_unit:
            continue
        for a, b2, c in u:
            if a == 4 and b2 == 2:
                fn = _fields(c); nm = None; fz = None
                for d, e, g in fn:
                    if d == 1 and e == 2:
                        nm = g.decode(errors="replace")
                    if d == 3 and e == 5:
                        fz = _st.unpack("<f", g)[0]
                if nm == symbol:
                    return fz
    return None


def fuzzy_measure(unit: dict, symbol: str, version: str,
                  retries: int = 3) -> float:
    """True objdiff fuzzy_match_percent for one function.

    Uses a throwaway ONE-UNIT objdiff project (tools/unitfuzzy.measure) rather
    than regenerating the shared build/<ver>/report.json: ~0.3s instead of
    ~2.6s, and immune to peer agents deleting that shared file mid-sweep (which
    previously showed up as silent holes in a sweep). Returns -1.0 on failure.
    """
    import unitfuzzy
    for attempt in range(retries):
        try:
            u = unitfuzzy.measure(unit, version)
        except Exception:
            time.sleep(0.3 * (attempt + 1))
            continue
        for f in (u.get("functions") or []):
            if f["name"] == symbol:
                # objdiff OMITS fuzzy_match_percent when it is 0.0, so a
                # variant that drops the function to zero has no key at all --
                # three rows in the tree are already in that state.  Reading it
                # unguarded raised KeyError out of the measure call and killed
                # the sweep mid-run, and a sweep that dies part-way reads
                # exactly like a sweep that finished and found nothing.  The
                # absent key means 0.0, which is also the right ranking.
                return float(f.get("fuzzy_match_percent") or 0.0)
        return -1.0
    return -1.0


def _fuzzy_measure_via_shared_report(report_unit: str, symbol: str, version: str,
                                     retries: int = 8) -> float:
    """True objdiff fuzzy_match_percent for one function. Regenerates the whole
    report (fast: objects are prebuilt) and decodes the proto. `report generate`
    is all-or-nothing, so a concurrent agent mid-rebuild can make it fail -- we
    retry with backoff. Returns -1.0 if it never succeeds."""
    out = Path(tempfile.gettempdir()) / f"bm_fuzzy_{version}.binpb"
    cli = REPO / "build" / "tools" / "objdiff-cli.exe"
    if not cli.is_file():
        cli = REPO / "build" / "tools" / "objdiff-cli"
    for attempt in range(retries):
        r = subprocess.run(
            [str(cli), "report", "generate",
             "-o", str(out), "-f", "proto"],
            cwd=REPO, capture_output=True, text=True)
        if r.returncode == 0 and out.is_file():
            fz = decode_fuzzy(out.read_bytes(), report_unit, symbol)
            if fz is not None:
                return fz
        time.sleep(0.4 * (attempt + 1))
    return -1.0


# ------------------------------------------------------------- source parse
def find_objdump() -> Path:
    p = REPO / "build" / "binutils" / "powerpc-eabi-objdump.exe"
    if not p.is_file():
        p = REPO / "build" / "binutils" / "powerpc-eabi-objdump"
    return p


def skip_ws_comments(s: str, i: int) -> int:
    n = len(s)
    while i < n:
        ch = s[i]
        if ch in " \t\r\n":
            i += 1
        elif s.startswith("//", i):
            while i < n and s[i] != "\n":
                i += 1
        elif s.startswith("/*", i):
            i += 2
            while i < n and not s.startswith("*/", i):
                i += 1
            i += 2
        else:
            break
    return i


def find_function_body(src: str, name: str):
    """Return (body_open_idx, body_close_idx) for the DEFINITION of `name`."""
    n = len(src)
    start = 0
    while True:
        idx = src.find(name, start)
        if idx < 0:
            return None
        start = idx + len(name)
        # boundary check
        if idx > 0 and (src[idx - 1].isalnum() or src[idx - 1] == "_"):
            continue
        j = skip_ws_comments(src, idx + len(name))
        if j >= n or src[j] != "(":
            continue
        # skip to matching close paren
        depth = 0
        k = j
        while k < n:
            c = src[k]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        k += 1
        k2 = skip_ws_comments(src, k)
        # K&R-style definition: the parameter declarations sit between the close
        # paren and the body.  Without this a K&R row is reported as "could not
        # locate", which reads exactly like a row that was swept and found
        # inert, so it silently leaves the population.
        while k2 < n and src[k2] not in "{;":
            semi = src.find(";", k2)
            if semi < 0:
                break
            brace = src.find("{", k2)
            if 0 <= brace < semi:
                break
            k2 = skip_ws_comments(src, semi + 1)
        # allow attribute / trailing tokens up to a '{' or ';'
        if k2 < n and src[k2] == "{":
            # find matching close brace
            depth = 0
            b = k2
            while b < n:
                c = src[b]
                if src.startswith("//", b):
                    while b < n and src[b] != "\n":
                        b += 1
                    continue
                elif src.startswith("/*", b):
                    b += 2
                    while b < n and not src.startswith("*/", b):
                        b += 1
                    b += 2
                    continue
                elif c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        return k2, b
                elif c == '"' or c == "'":
                    b = skip_string(src, b)
                    continue
                b += 1
            return None
        # else: it was a declaration/prototype, keep searching


def skip_string(s: str, i: int) -> int:
    q = s[i]
    i += 1
    n = len(s)
    while i < n:
        if s[i] == "\\":
            i += 2
            continue
        if s[i] == q:
            return i + 1
        i += 1
    return i


def next_statement(src: str, i: int, body_end: int):
    """From i (inside body), return (core_start, sep_text, end_after_semi) of the
    next statement, or None. sep_text = ws/comments preceding core."""
    core_start = skip_ws_comments(src, i)
    if core_start >= body_end:
        return None
    sep = src[i:core_start]
    depth = 0
    k = core_start
    while k < body_end:
        c = src[k]
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif c in '"\'':
            k = skip_string(src, k)
            continue
        elif src.startswith("//", k):
            while k < body_end and src[k] != "\n":
                k += 1
            continue
        elif src.startswith("/*", k):
            k += 2
            while k < body_end and not src.startswith("*/", k):
                k += 1
            k += 1
        elif c == ";" and depth == 0:
            end = k + 1
            # absorb a trailing same-line comment
            j = end
            while j < body_end and src[j] in " \t":
                j += 1
            if src.startswith("//", j):
                while j < body_end and src[j] != "\n":
                    j += 1
                end = j
            elif src.startswith("/*", j):
                j += 2
                while j < body_end and not src.startswith("*/", j):
                    j += 1
                end = j + 2
            return core_start, sep, end
        k += 1
    return None


def looks_like_decl(core: str) -> bool:
    s = core.lstrip()
    # first token
    tok = ""
    for ch in s:
        if ch.isalnum() or ch == "_":
            tok += ch
        else:
            break
    if not tok:
        return False
    if tok in TYPE_TOKENS:
        return True
    if tok in {"return", "if", "for", "while", "do", "switch", "goto",
               "case", "default", "break", "continue", "else", "asm"}:
        return False
    # typedef'd type heuristic: `Ident (*|space)+ ident ...` with no '(' before
    # first '=' / ';' / '[' (i.e. not a call/assignment)
    rest = s[len(tok):]
    # position of first terminator among = ; [
    stop = len(rest)
    for term in ("=", ";", "["):
        p = rest.find(term)
        if p != -1:
            stop = min(stop, p)
    head = rest[:stop]
    if "(" in head:
        return False
    # A DECLARATION INTRODUCES A NAME.  `tri->edgeOutBits = 0;` and
    # `cfg.startPosX += p->x;` match the `Ident ... ident` shape above but
    # declare nothing -- they are STATEMENTS, and absorbing them into a
    # declaration block means the sweepers permute side-effecting stores
    # against each other.  No gate in this project can see that: objdiff,
    # obj_equal and the forced link all compare retail's BYTES, never its
    # MEANING, so a reordered pair of stores that happens to score better
    # would have landed as a "declaration ordering".  Member access in the
    # declarator position is the tell, and no C declaration has one.
    if "->" in head or "." in head:
        return False
    # head must contain another identifier (the variable name), possibly via '*'
    import re as _re
    if _re.search(r"[A-Za-z_]\w*", head) or "*" in head:
        # also require the first token be a plausible type (Capitalized or has *)
        if tok[0].isupper() or "*" in head or tok.islower():
            return True
    return False


def parse_decl_block(src: str, body_open: int, body_end: int):
    """Return (block_start, block_end, indent, items[list[str core]]).

    Reads the run of declarations at the TOP of the block that opens at
    `body_open` (C89 style).  `body_end` is the index of that block's closing
    brace.  Works for the function body and for any nested scope alike.
    """
    i = body_open + 1
    items = []
    block_start = None
    block_end = None
    indent = "    "
    while True:
        st = next_statement(src, i, body_end)
        if not st:
            break
        core_start, sep, end = st
        core = src[core_start:end]
        if not looks_like_decl(core):
            break
        if block_start is None:
            block_start = core_start
            # infer indent from sep tail
            nl = sep.rfind("\n")
            if nl != -1:
                indent = sep[nl + 1:]
        items.append(core)
        block_end = end
        i = end
    return block_start, block_end, indent, items


# ------------------------------------------------- nested-scope enumeration
SCOPE_PREV_CHARS = set(")};:{")
SCOPE_PREV_WORDS = {"else", "do", "try"}


def find_scope_blocks(src: str, body_open: int, body_end: int):
    """Every SCOPE `{...}` in the function, outermost first, in source order.

    Returns [(open_idx, close_idx), ...] including the function body itself.
    Brace-initialisers (`= { ... }`, `, { ... }`) are classified as NON-scope
    and are skipped whole, so `static const Foo t[] = {{..},{..}};` inside a
    body never produces phantom blocks.

    This is the fix for the long-standing false-negative: the sweeper used to
    look only at the function body's leading declaration run, so a local
    declared inside a loop body / if-block / case block was NEVER permuted, and
    every "declaration order is inert" verdict for such a function was vacuous.
    """
    blocks = []
    stack = []            # list of (open_idx, is_scope)
    init_depth = 0        # >0 while inside a brace initialiser
    last_code = ""        # last non-ws/comment char seen
    last_word = ""        # last identifier token seen
    i = body_open
    n = body_end + 1
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            i += 2
            while i < n and not src.startswith("*/", i):
                i += 1
            i += 2
            continue
        if c in '"\'':
            i = skip_string(src, i)
            last_code = c
            last_word = ""
            continue
        if c == "{":
            if init_depth:
                init_depth += 1
                is_scope = False
            else:
                is_scope = (last_code in SCOPE_PREV_CHARS
                            or last_word in SCOPE_PREV_WORDS
                            or last_code == "")
                if not is_scope:
                    init_depth = 1
            stack.append((i, is_scope))
        elif c == "}":
            if stack:
                o, is_scope = stack.pop()
                if is_scope:
                    blocks.append((o, i))
                elif init_depth:
                    init_depth -= 1
        if not c.isspace():
            last_code = c
            if c.isalnum() or c == "_":
                last_word += c
            else:
                last_word = ""
        i += 1
    blocks.sort()
    return blocks


def collect_decl_blocks(src: str, body_open: int, body_end: int, min_items: int = 2):
    """All permutable declaration blocks in a function: top-level AND nested.

    -> [{'start','end','indent','items','scope','depth'}] in source order.
    """
    scopes = find_scope_blocks(src, body_open, body_end)
    depth_of = {}
    for o, c in scopes:
        depth_of[o] = sum(1 for o2, c2 in scopes if o2 < o and c2 > c)
    out = []
    for o, c in scopes:
        bs, be, indent, items = parse_decl_block(src, o, c)
        if not items or len(items) < min_items:
            continue
        out.append({
            "start": bs, "end": be, "indent": indent, "items": items,
            "scope": (o, c), "depth": depth_of[o],
        })
    out.sort(key=lambda b: b["start"])
    return out


# ------------------------------------------------------------ variant gen
def _elementary_ops(n: int):
    """Every radius-1 declaration edit, with the index range it disturbs.

    A swap of positions a,b moves exactly those two declarations and leaves
    everything between them where it was, so its support is {a, b}.  A move of
    i to j shifts every declaration between i and j by one, so its support is
    the whole closed interval.  Two ops with DISJOINT supports commute, which
    is what makes "apply both to the base" well defined.
    """
    ops = []
    for a, b in itertools.combinations(range(n), 2):
        ops.append(("swap", (a, b), frozenset((a, b))))
    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            ops.append(("move", (i, j), frozenset(range(min(i, j),
                                                       max(i, j) + 1))))
    return ops


def _apply_op(order: list, op) -> list:
    kind, arg, _ = op
    o = list(order)
    if kind == "swap":
        a, b = arg
        o[a], o[b] = o[b], o[a]
    else:
        i, j = arg
        x = o.pop(i)
        o.insert(j, x)
    return o


# ------------------------------------------- side effects in an initialiser
CALL_TOKEN_RE = re.compile(r"(?<!\w)([A-Za-z_]\w*)\s*\(")
NON_CALL_KEYWORDS = {"sizeof", "if", "while", "for", "switch", "return",
                     "defined", "asm", "offsetof", "__alignof__"}


def initialiser_eq(item: str) -> int:
    """Index of an item's initialiser `=`, or -1.  Skips ==/!=/<=/>=/+= and
    any `=` nested in parens, brackets, braces or a string literal."""
    depth = 0
    i = 0
    n = len(item)
    while i < n:
        c = item[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c in '"\'':
            i = skip_string(item, i)
            continue
        elif c == "=" and depth == 0:
            if i + 1 < n and item[i + 1] == "=":
                i += 2
                continue
            if i > 0 and item[i - 1] in "!<>=+-*/%&|^":
                i += 1
                continue
            return i
        i += 1
    return -1


def initialiser_calls(item: str) -> list[str]:
    """Call tokens in an item's INITIALISER (empty if it has none).

    A cast does not match: in `(u32)(p)` the identifier is inside the parens
    and is followed by `)`, not `(`.  A macro that expands to a cast DOES
    match, which is the conservative direction -- the reader can clear it.

    An INDIRECT call through a struct member matches too -- `p->fn(x)`,
    `s.fn(x)`, `tbl[i].fn(x)`.  It is only the `(` that distinguishes a member
    CALL from a member READ, so a lookbehind that excluded `>` and `.` went
    blind to every interface dispatch in the tree, which is exactly the class
    whose purity cannot be established from the source at all:
    `(*gCameraInterface)->getCamera()` resolves to a DLL vtable slot.  Being
    blind where the answer is least knowable is the wrong direction to fail in.
    """
    eq = initialiser_eq(item)
    if eq < 0:
        return []
    return [m.group(1) for m in CALL_TOKEN_RE.finditer(item[eq + 1:])
            if m.group(1) not in NON_CALL_KEYWORDS]


def side_effect_reorders(items, order):
    """Item pairs whose RELATIVE ORDER this permutation changes and where at
    least one of the two carries a call in its initialiser.

    WHY THIS EXISTS.  `looks_like_decl` refuses a member-access STORE because
    permuting stores changes the computation and no gate in this project can
    see that.  But a DECLARATION whose initialiser calls something is a
    declaration by every test the parser applies, and its side effect is just
    as invisible:

        s32 rnd1 = randomGetRange(0, 0x1e) * 2;
        s32 rnd2 = randomGetRange(0, 0x1e) * 2;

    is two draws from a PRNG, and swapping the two declarations swaps which
    draw lands in which variable.  objdiff fuzzy, obj_equal, score_delta_gate
    and the forced link all compare retail's BYTES, never its MEANING, so an
    ordering that scored better would have landed as "a declaration ordering".

    The rule is deliberately CONSERVATIVE rather than clever: a pure helper
    (`__fabsf(x)`) is flagged too.  Flagging is not refusing -- the sweep still
    measures the ordering; it only loses the right to APPLY it unread.
    """
    calls = [bool(initialiser_calls(it)) for it in items]
    pos = {v: k for k, v in enumerate(order)}
    out = []
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            if not (calls[i] or calls[j]):
                continue
            if pos[i] > pos[j]:
                out.append((i, j))
    return out


def report_side_effect_reorders(items, order, label=""):
    """Print the flagged pairs; return True if there are any."""
    pairs = side_effect_reorders(items, order)
    if not pairs:
        return False
    print(f"# SIDE-EFFECT REORDER{(' ' + label) if label else ''}: this "
          f"ordering changes the relative order of {len(pairs)} pair(s) in "
          f"which at least one initialiser CALLS something.")
    for i, j in pairs:
        print(f"#   [{i}] {' '.join(items[i].split())[:72]}")
        print(f"#   [{j}] {' '.join(items[j].split())[:72]}")
    print("# No gate in this project compares MEANING, so this is not applied "
          "automatically; read the pair and re-run with "
          "--allow-side-effect-reorder if the calls are pure.")
    return True


def gen_variants(n: int, strategy: str, cap: int, seed: int = 12345):
    """Yield orderings (as tuples of indices) excluding the identity first."""
    base = tuple(range(n))
    seen = {base}
    out = []

    def add(order):
        t = tuple(order)
        if t not in seen:
            seen.add(t)
            out.append(t)

    if strategy in ("radius2", "radius2only") and n <= 5:
        # Small enough to be exhaustive: radius 2 is a subset of the full
        # symmetric group, and at n<=5 the whole group is 119 orderings, so
        # sweep it and be done rather than reason about a radius at all.
        for p in itertools.permutations(range(n)):
            add(list(p))
        return [base] + out[: max(0, cap - 1)]

    if strategy in ("radius2", "radius2only"):
        # Radius 1 first, so a radius-2 run is a strict superset of the
        # sweeps that came before it and its own zero is comparable to
        # theirs.  Then every composition of TWO INDEPENDENT elementary
        # edits: disjoint supports, so the pair commutes and is genuinely
        # two edits rather than one edit spelled twice.  Anything a single
        # edit already reaches is deduped away by `add`.
        ops = _elementary_ops(n)
        for op in ops:
            add(_apply_op(list(base), op))
        if strategy == "radius2only":
            # On a big block the radius-1 prefix alone exceeds any sane cap
            # (an 81-item block has 3240 swaps before a single move), so a
            # capped `radius2` run never reaches radius 2 at all.  This mode
            # drops the prefix and spends the whole budget on the two-edit
            # compositions, for rows whose radius 1 has already been swept.
            seen |= {tuple(o) for o in out}
            out = []
        # Pair only ops with a SMALL support.  A pair of long-range moves
        # disturbs most of the list and is a different animal from "the dev
        # declared these two the other way round"; it also makes the pair
        # enumeration O(n^4) on the big blocks for no plausibility.  Swaps
        # always qualify (support 2); moves qualify out to distance 3.
        small = [o for o in ops if len(o[2]) <= 4]
        pairs = []
        for x in range(len(small)):
            for y in range(x + 1, len(small)):
                if small[x][2] & small[y][2]:
                    continue
                pairs.append((len(small[x][2]) + len(small[y][2]), x, y))
        # smallest total disturbance first: a two-swap edit is far likelier
        # to be the thing a 2002 dev's declaration list actually differs by
        # than a pair of long-range moves.
        pairs.sort()
        for _w, x, y in pairs:
            if len(out) >= cap - 1:
                break
            add(_apply_op(_apply_op(list(base), small[x]), small[y]))
        return [base] + out[: max(0, cap - 1)]

    if strategy in ("swaps", "all"):
        for a, b in itertools.combinations(range(n), 2):
            o = list(base)
            o[a], o[b] = o[b], o[a]
            add(o)
    if strategy in ("moves", "all"):
        for src_i in range(n):
            for dst in range(n):
                if src_i == dst:
                    continue
                o = list(base)
                x = o.pop(src_i)
                o.insert(dst, x)
                add(o)
    if strategy == "all":
        add(list(reversed(base)))
        rng = random.Random(seed)
        for _ in range(min(40, cap)):
            o = list(base)
            rng.shuffle(o)
            add(o)
    return [base] + out[: max(0, cap - 1)]


# ------------------------------------------------------------------- build
def rebuild(unit_object: str, version: str) -> bool:
    """Recompile one unit's src .o.

    By default this goes through tools/direct_build.py, which recovers the
    unit's compile command from ninja ONCE and then runs it directly: ninja's
    global build-dir mutex otherwise serialises every probe of every concurrent
    sweep onto a single invocation, which pins a ten-worker fleet on a ten-core
    box at a load average of 3.9.  The object produced is byte-identical (the
    command is ninja's own), and direct_build falls back to locked_ninja when
    the command cannot be recovered.  Set SFA_BRUTE_LOCKED_NINJA=1 to force the
    old path.
    """
    rel = unit_object.replace(f"build/{version}/obj/", f"build/{version}/src/")
    src_o = REPO / rel
    if not os.environ.get("SFA_BRUTE_LOCKED_NINJA"):
        try:
            from direct_build import direct_build
            return direct_build(unit_object, version)
        except Exception:
            pass
    try:
        src_o.unlink()
    except FileNotFoundError:
        pass
    # ninja resolves targets relative to the build root -- pass the repo-relative
    # path, never an absolute path (ninja won't recognise the latter as a target).
    r = subprocess.run([shell_bash(), "--noprofile", "--norc", "tools/locked_ninja.sh", rel],
                       cwd=REPO, capture_output=True, text=True)
    return r.returncode == 0 and src_o.is_file()


# ------------------------------------------------------------ crash safety
def install_restore_guard(src_file: Path, original: bytes):
    """Guarantee the target file is never left mid-permutation.

    A sidecar `<file>.brutebak` holds the pristine bytes for the whole run: if
    the process is hard-killed (SIGTERM/SIGKILL, a wrapper timeout, a closed
    terminal) the next invocation finds the sidecar and restores from it before
    doing anything else.  atexit + SIGINT/SIGTERM handle the recoverable cases.

    Restoring is CONDITIONAL on the file still holding the bytes this process
    last wrote.  Without that test the exit path reverts the file to `original`
    whatever is on disk, so a peer lane that edited the same file inside our
    sweep window loses its work silently -- and this is the most-run tool in
    the tree.  `stmt_sweep` grew the check when it was written; the copy here
    did not, which is the same fix-at-one-paste-site shape the zero-key read
    had.  Both now refuse and say so, keeping the sidecar for reconciliation.
    """
    import atexit
    import os
    import signal

    bak = src_file.with_suffix(src_file.suffix + ".brutebak")
    bak.write_bytes(original)

    state = {"done": False, "last": original, "keepbak": False}

    def restore(*_a):
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
        if not state["keepbak"]:
            try:
                bak.unlink()
            except OSError:
                pass

    def on_signal(signum, _frame):
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

    def keep(newbytes: bytes):
        """Adopt `newbytes` as the value to leave on disk (a confirmed win)."""
        state["done"] = True
        src_file.write_bytes(newbytes)
        try:
            bak.unlink()
        except OSError:
            pass

    return write, keep


def recover_stale_backup(src_file: Path):
    bak = src_file.with_suffix(src_file.suffix + ".brutebak")
    if bak.is_file():
        good = bak.read_bytes()
        if src_file.read_bytes() != good:
            print(f"# recovering {src_file.name} from a killed run's backup")
            src_file.write_bytes(good)
        bak.unlink()


# --------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("unit")
    ap.add_argument("symbol")
    ap.add_argument("-v", "--version", default="GSAE01")
    ap.add_argument("--max-variants", type=int, default=60)
    ap.add_argument("--time-budget", type=float, default=900.0)
    ap.add_argument("--strategy",
                    choices=["swaps", "moves", "all", "radius2", "radius2only"],
                    default="all")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--apply-best", action="store_true")
    ap.add_argument("--list-blocks", action="store_true",
                    help="print every permutable decl block (top-level AND "
                         "nested scopes) and exit")
    ap.add_argument("--block", type=int, action="append", default=None,
                    help="sweep only these block indices (repeatable); "
                         "default sweeps every block")
    ap.add_argument("--top-level-only", action="store_true",
                    help="old (buggy) behaviour: leading block of the body only")
    ap.add_argument("--allow-side-effect-reorder", action="store_true",
                    help="apply a winner even when it reorders two "
                         "declarations whose initialisers call something")
    ap.add_argument("--cross", action="store_true",
                    help="after the per-block pass, also try the cross-product "
                         "of the winning orderings of small blocks")
    args = ap.parse_args()

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

    if args.top_level_only:
        bstart, bend, indent, items = parse_decl_block(src, body_open, body_end)
        blocks = ([{"start": bstart, "end": bend, "indent": indent,
                    "items": items, "scope": (body_open, body_end), "depth": 0}]
                  if items and len(items) >= 2 else [])
    else:
        blocks = collect_decl_blocks(src, body_open, body_end)

    def lineno(idx):
        return src.count("\n", 0, idx) + 1

    print(f"# {args.symbol} in {src_file.relative_to(REPO)}")
    if not blocks:
        raise SystemExit("no declaration block with >=2 items; nothing to permute")
    print(f"# {len(blocks)} permutable declaration block(s) "
          f"(top-level + nested scopes):")
    for bi, b in enumerate(blocks):
        print(f"  block {bi}: depth={b['depth']} line {lineno(b['start'])}"
              f"-{lineno(b['end'])}  {len(b['items'])} items  "
              f"indent={b['indent']!r}")
        for k, it in enumerate(b["items"]):
            print(f"      [{k}] {' '.join(it.split())[:88]}")

    if args.list_blocks:
        return

    sel = list(range(len(blocks))) if args.block is None else sorted(set(args.block))
    for bi in sel:
        if bi < 0 or bi >= len(blocks):
            raise SystemExit(f"--block {bi} out of range (0..{len(blocks)-1})")

    def render(orders):
        """orders: {block_index: tuple-of-item-indices} -> full source text.

        Blocks are non-overlapping and spliced back-to-front so earlier offsets
        stay valid.  src[:start] already ends with the newline+indent preceding
        the first declaration, so item 0 carries no separator.
        """
        out = src
        for bi in sorted(orders, reverse=True):
            b = blocks[bi]
            order = orders[bi]
            it = b["items"]
            first = it[order[0]].lstrip()
            rest = "".join("\n" + b["indent"] + it[k].lstrip() for k in order[1:])
            out = out[:b["start"]] + first + rest + out[b["end"]:]
        return out

    plan = {bi: gen_variants(len(blocks[bi]["items"]), args.strategy,
                             args.max_variants) for bi in sel}
    total = sum(len(v) - 1 for v in plan.values())
    print(f"# sweeping blocks {sel}: {total} non-identity variants "
          f"(strategy={args.strategy}, cap={args.max_variants}/block, "
          f"budget={args.time_budget}s)")

    if args.dry_run:
        for bi in sel:
            vs = plan[bi]
            print(f"# block {bi}: {len(vs)} orderings, e.g. "
                  f"{list(vs[1] if len(vs) > 1 else vs[0])}")
            newsrc = render({bi: vs[1] if len(vs) > 1 else vs[0]})
            print(newsrc[blocks[bi]["start"]:blocks[bi]["start"] + 300])
        return

    write, keep = install_restore_guard(src_file, original)

    # baseline measure. ALWAYS rebuild cur_o from the on-disk (original) source
    # first: in a multi-agent tree the .o can be stale (a peer rebuilt it, or a
    # prior run left a variant applied), and measuring a stale baseline lets a
    # variant that is actually a REGRESSION beat the (wrongly-low) baseline and
    # get written to disk. Rebuilding here pins base_fz to the true current
    # source. (Cheap: one locked build.)
    rebuild(unit["object"], args.version)
    t_norm = objdump_norm(objdump, tgt_o, args.symbol)
    report_unit = report_unit_name(unit)

    def proxy():
        c = objdump_norm(objdump, cur_o, args.symbol)
        return match_score(t_norm, c)

    def fuzzy():
        return fuzzy_measure(unit, args.symbol, args.version)

    base_proxy, base_reg = proxy()
    if base_proxy < 0:
        raise SystemExit(
            f"cannot disassemble {args.symbol} from {tgt_o.name}/{cur_o.name} "
            "-- is this unit part of the build?")
    base_fz = fuzzy()
    if base_fz < 0:
        raise SystemExit(
            "could not read baseline fuzzy (report generate failed -- a "
            "concurrent build may be in flight; retry).")
    print(f"baseline: fuzzy={base_fz:.4f}%  proxy={base_proxy:.3f}%  "
          f"regions={base_reg}  (report_unit={report_unit})")

    # DECIDING metric is fuzzy; proxy/regions are informational only. A sibling
    # proved region-count can be anti-correlated with fuzzy, so we never rank on
    # it.  Coordinate ascent over the blocks: each block is swept with the other
    # blocks pinned at the best ordering found so far, so gains from separate
    # scopes accumulate.
    held: dict[int, tuple] = {}
    cur_fz, cur_px = base_fz, base_proxy
    results = []          # (fuzzy, proxy, reg, block_index, order)
    # PLANNED IS NOT SWEPT.  The variant plan is syntactic: it permutes the
    # declaration TEXT and does not look at initialisers, so on a block whose
    # declarations initialise from each other (`float y = f(&bits); float y2 =
    # y * y;`) most orderings are use-before-declaration and do not compile.
    # Those are skipped, correctly -- but a summary that reports the PLANNED
    # count makes such a row read as "N orderings, inert" when its legal
    # neighbourhood was smaller, and sometimes empty.  Measured over the 201
    # sub-100 rows: 68 of them carry at least one initialiser that reads
    # another declaration in the same block, 213 declarations in all, and nine
    # rows (the trig family, mathTanf) have NO legal reordering whatsoever.
    # So both numbers are reported, always.
    n_built = n_failed = 0
    t0 = time.time()
    stop = False

    class PeerEdit(Exception):
        pass

    def trial(orders):
        # A refused write means a peer edited the file. Abort loudly: an
        # aborted sweep that returns quietly reads exactly like an exhausted
        # one, and every "N orderings, 0 hits" count downstream would be wrong.
        if not write(render(orders).encode("latin-1")):
            raise PeerEdit()
        if not rebuild(unit["object"], args.version):
            return None
        px, reg = proxy()
        return fuzzy(), px, reg

    try:
        for bi in sel:
            if stop:
                break
            variants = plan[bi]
            n_items = len(blocks[bi]["items"])
            print(f"\n# --- block {bi} (depth {blocks[bi]['depth']}, "
                  f"{n_items} items, {len(variants) - 1} orderings) ---")
            block_best = None
            for vi, order in enumerate(variants):
                if vi == 0:
                    continue
                if time.time() - t0 > args.time_budget:
                    print(f"# time budget hit (block {bi}, variant {vi})")
                    stop = True
                    break
                res = trial({**held, bi: order})
                if res is None:
                    n_failed += 1
                    print(f"[b{bi} {vi:3d}] BUILD FAIL {list(order)}")
                    continue
                n_built += 1
                fz, px, reg = res
                results.append((fz, px, reg, bi, order))
                flag = ""
                if (fz, px) > (cur_fz, cur_px) and (
                        block_best is None or (fz, px) > block_best[:2]):
                    block_best = (fz, px, order)
                    flag = "  <== best"
                note = " (fuzzy read FAILED)" if fz < 0 else ""
                print(f"[b{bi} {vi:3d}] fuzzy={fz:8.4f}% proxy={px:7.3f}% "
                      f"reg={reg:2d} {list(order)}{flag}{note}")
            if block_best and block_best[0] > cur_fz + 1e-4:
                held[bi] = block_best[2]
                print(f"# block {bi}: HOLD {list(block_best[2])}  "
                      f"fuzzy {cur_fz:.4f}% -> {block_best[0]:.4f}%")
                cur_fz, cur_px = block_best[0], block_best[1]
            else:
                print(f"# block {bi}: inert (best {cur_fz:.4f}%)")

        # optional cross-product refinement over the small blocks
        if args.cross and not stop and len(sel) > 1:
            small = [bi for bi in sel if len(blocks[bi]["items"]) <= 4]
            combos = 1
            for bi in small:
                combos *= len(plan[bi])
            if 1 < combos <= args.max_variants:
                print(f"\n# --- cross-product over blocks {small} "
                      f"({combos} combos) ---")
                for combo in itertools.product(*(plan[bi] for bi in small)):
                    if time.time() - t0 > args.time_budget:
                        print("# time budget hit during cross-product")
                        break
                    orders = {**held}
                    orders.update(dict(zip(small, combo)))
                    res = trial(orders)
                    if res is None:
                        continue
                    fz, px, reg = res
                    if (fz, px) > (cur_fz, cur_px):
                        cur_fz, cur_px = fz, px
                        held = orders
                        print(f"[cross] fuzzy={fz:8.4f}% proxy={px:7.3f}%  "
                              f"{ {b: list(o) for b, o in orders.items()} }"
                              "  <== best")
            else:
                print(f"\n# cross-product skipped ({combos} combos > cap)")
    except PeerEdit:
        raise SystemExit("aborted: the source file was edited by another "
                         "process mid-sweep")
    finally:
        # never leave a permutation on disk while deciding
        write(original)

    print(f"\n# orderings BUILT {n_built}, rejected by the compiler "
          f"{n_failed} (planned {total}) -- a row whose declarations "
          f"initialise from each other has a smaller legal neighbourhood than "
          f"the plan, and 'planned' is never the swept count")

    print("\n# ranked by FUZZY (top 12):")
    for fz, px, reg, bi, order in sorted(results, key=lambda r: (-r[0], -r[1]))[:12]:
        print(f"  fuzzy={fz:8.4f}% proxy={px:7.3f}% reg={reg:2d} "
              f"block {bi} {list(order)}")

    # MEANING GATE, ahead of the score gate.  Every instrument in this project
    # compares retail's bytes; none of them can see that two calls swapped.
    flagged = False
    for bi, order in sorted(held.items()):
        if report_side_effect_reorders(blocks[bi]["items"], order,
                                       f"(block {bi})"):
            flagged = True
    if flagged and not args.allow_side_effect_reorder:
        write(original)
        rebuild(unit["object"], args.version)
        print("\n# NOT APPLIED: the winning ordering reorders side effects.")
        return

    # commit gate: true fuzzy must strictly rise
    if held and cur_fz > base_fz + 1e-4:
        newbytes = render(held).encode("latin-1")
        if not write(newbytes):
            return
        rebuild(unit["object"], args.version)
        confirm = fuzzy()
        print(f"\n# APPLIED: fuzzy {base_fz:.4f}% -> {confirm:.4f}%")
        for bi, order in sorted(held.items()):
            print(f"#   block {bi} order = {list(order)}")
        if confirm <= base_fz + 1e-4:
            print("# WARNING: re-measured fuzzy did NOT confirm the gain -- "
                  "restoring original.")
            write(original)
            rebuild(unit["object"], args.version)
        else:
            keep(newbytes)
    else:
        rebuild(unit["object"], args.version)
        print(f"\n# no fuzzy improvement (best {cur_fz:.4f}% vs "
              f"base {base_fz:.4f}%); restored original.")


if __name__ == "__main__":
    main()
