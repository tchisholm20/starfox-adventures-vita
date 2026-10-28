#!/usr/bin/env python3
"""Find the forward-substitution shape A76 measured on engine/5 renderSunAndMoon.

The law
-------
MWCC GC/2.0 forward-substitutes a local that is DEFINED ONCE and READ ONCE into
its single use, and it will move that computation PAST INTERVENING CALLS when
every operand of the defining expression is register-resident (another local, or
a literal).  It cannot do so when an operand is a memory reference a call might
clobber -- which is why the same source read 99.476 while `0.55f` was hidden
behind an undefined `extern const f32`, and 96.828 once the extern was deleted
and the literal became visible.

One substitution produces three residuals that look unrelated:
  * the defining arithmetic instruction appears at the STORE site, not where the
    source writes it (a displaced fmuls/fnmsubs/fadds/...);
  * the callee-saved FPR the source would give the temp goes to the OTHER local,
    because the temp no longer spans the calls;
  * and the pool ROTATES, because literal words mint at emission order, so a
    sunk expression mints its constants late.

The cure is plain C: reuse ONE local for both roles, deleting the temp the
substitution needs.  That is what `riseScale` did -- +2.012 fuzzy on the
function and .sdata2 95.45 -> 100.0.

What this scans
---------------
Source side, over every function objdiff scores below 100:

  candidate = a function-scope scalar local with EXACTLY ONE plain `X = expr;`
              assignment and EXACTLY ONE read, at least one call between them,
              whose defining expression is arithmetic over literals and other
              locals only (no global/field/array operand that would block the
              substitution).

A candidate is CURABLE when the defining expression names another local that is
itself dead from that point on: the two can be folded into one variable.

Usage
  python3 tools/fwdsub_scan.py [--report R.json] [--min-size N] [--all]
"""

import argparse
import json
import os
import re
import sys

C_KEYWORDS = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "return",
    "break", "continue", "goto", "sizeof", "typedef", "struct", "union", "enum",
    "static", "const", "volatile", "register", "extern", "unsigned", "signed",
    "void", "int", "char", "short", "long", "float", "double",
}

SCALAR_TYPES = {
    "f32", "f64", "float", "double", "s32", "u32", "int", "s16", "u16", "s8",
    "u8", "long", "unsigned", "short",
}

DECL_RE = re.compile(
    r"^\s*(?:const\s+)?(?:unsigned\s+|signed\s+)?"
    r"(f32|f64|float|double|s32|u32|int|s16|u16|s8|u8|long|short)\s+"
    r"([A-Za-z_][A-Za-z_0-9]*(?:\s*,\s*[A-Za-z_][A-Za-z_0-9]*)*)\s*;\s*$")

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
# a numeric literal, INCLUDING its f/u/l suffix -- otherwise `0.55f` donates an
# identifier `f` and every float expression scans as having a global operand.
NUMLIT_RE = re.compile(
    r"(?<![A-Za-z_0-9.])(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][-+]?\d+)?)"
    r"[fFuUlL]*")
CALL_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(")

# operand kinds that BLOCK the substitution: a memory reference the call may
# clobber.  `a.b`, `a->b`, `a[i]`, `*p`, and a bare global identifier.
MEMREF_RE = re.compile(r"(->|\.\s*[A-Za-z_]|\[)")


def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            if j < 0:
                break
            out.append(' ' * (j - i))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:min(j + 1, n)])
            i = min(j + 1, n)
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def find_functions(text):
    """Yield (name, body_start, body_end) for top-level `... name(...) {`."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text[i] != '{':
            i += 1
            continue
        # Both brace styles: the body-opening `{` may sit at column 0 or at the
        # end of the signature line (K&R).  What identifies it is that the
        # previous non-blank character is the `)` of the parameter list -- an
        # aggregate initialiser is preceded by `=`, a struct/enum by a name.
        line_start = i
        # Walk back from the `{` to the `)` that closes the parameter list, then
        # to the `(` that opens it; the identifier before that `(` is the name.
        # (A `\n\n` heuristic misses every function whose predecessor is only one
        # blank line away -- that was 64 of 210 sub-100 bodies.)
        k = line_start - 1
        while k >= 0 and text[k] in ' \t\r\n':
            k -= 1
        # K&R-1978 parameter declaration list between the `)` and the body
        # (`int f(a, b) int a; int b; { ... }`) -- track_dolphin still has one.
        guard = 0
        while k >= 0 and text[k] == ';' and guard < 40:
            ls = text.rfind('\n', 0, k) + 1
            if not re.match(r"^\s*(?:const\s+|unsigned\s+|signed\s+|struct\s+)*"
                            r"[A-Za-z_][A-Za-z_0-9]*\s*\**\s*"
                            r"[A-Za-z_][A-Za-z_0-9]*(\s*\[[^\]]*\])?\s*;\s*$",
                            text[ls:k + 1]):
                break
            k = ls - 1
            while k >= 0 and text[k] in ' \t\r\n':
                k -= 1
            guard += 1
        m = None
        if k >= 0 and text[k] == ')':
            pd, p = 0, k
            while p >= 0:
                if text[p] == ')':
                    pd += 1
                elif text[p] == '(':
                    pd -= 1
                    if pd == 0:
                        break
                p -= 1
            if p >= 0:
                q = p - 1
                while q >= 0 and text[q] in ' \t\r\n':
                    q -= 1
                nm = re.search(r"([A-Za-z_][A-Za-z_0-9]*)$", text[:q + 1])
                if nm:
                    m = nm
        depth, j = 0, i
        while j < n:
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if m and m.group(1) not in C_KEYWORDS:
            out.append((m.group(1), i, j))
        i = j + 1
    return out


def local_decls(body):
    decls = {}
    for line in body.split('\n'):
        m = DECL_RE.match(line)
        if not m:
            continue
        for name in m.group(2).split(','):
            name = name.strip()
            if name:
                decls[name] = m.group(1)
    return decls


ANYDECL_RE = re.compile(
    r"^\s*(?:const\s+|volatile\s+|static\s+|unsigned\s+|signed\s+|struct\s+)*"
    r"([A-Za-z_][A-Za-z_0-9]*)\s*\**\s*"
    r"([A-Za-z_][A-Za-z_0-9]*(?:\s*\[[^\]]*\])?"
    r"(?:\s*,\s*\**\s*[A-Za-z_][A-Za-z_0-9]*(?:\s*\[[^\]]*\])?)*)\s*;\s*$")


def all_locals(body):
    """Every name declared in the body, whatever its type -- so an operand that
    is a local aggregate is not mistaken for a global."""
    names = set()
    for line in body.split('\n'):
        m = ANYDECL_RE.match(line)
        if not m or m.group(1) in ("return", "goto", "break", "continue"):
            continue
        for name in m.group(2).split(','):
            name = re.sub(r"\[[^\]]*\]", "", name).strip().lstrip('*').strip()
            if name:
                names.add(name)
    return names


def statements(body):
    """Split a body into (offset, text) statement-ish chunks on ; and braces."""
    out = []
    start = 0
    for i, ch in enumerate(body):
        if ch in ';{}':
            chunk = body[start:i]
            if chunk.strip():
                out.append((start, chunk))
            start = i + 1
    if body[start:].strip():
        out.append((start, body[start:]))
    return out


ASSIGN_RE_CACHE = {}


def assign_re(name):
    r = ASSIGN_RE_CACHE.get(name)
    if r is None:
        r = re.compile(r"(?<![A-Za-z_0-9])" + re.escape(name) +
                       r"\s*=(?![=])")
        ASSIGN_RE_CACHE[name] = r
    return r


def uses_of(body, name):
    return [m.start() for m in
            re.finditer(r"(?<![A-Za-z_0-9.>])" + re.escape(name) +
                        r"(?![A-Za-z_0-9])", body)]


def decl_lines(body):
    """Offsets covered by a declaration line, so a decl is not read as a use."""
    spans = []
    pos = 0
    for line in body.split('\n'):
        if DECL_RE.match(line):
            spans.append((pos, pos + len(line)))
        pos += len(line) + 1
    return spans


def in_spans(o, spans):
    return any(a <= o <= b for a, b in spans)


def analyse_function(name, body, decls, locals_all):
    """Candidates are DEF-USE REGIONS, not whole-variable counts.

    A variable clamped in a loop or written once per branch has many defs; what
    matters is a single def whose value reaches exactly ONE read before the next
    def of the same variable.  renderSunAndMoon's `scale` had two such regions.
    """
    cands = []
    # A call only INTERVENES when its whole argument list closes before the use.
    # `sqrtf(dxsq + dzsq)` lexically opens before `dxsq`, but nothing is
    # clobbered between the multiply and the add -- there is no call to sink
    # past.  Record each call's closing paren and test against that.
    calls = []
    for m in CALL_RE.finditer(body):
        if m.group(1) in C_KEYWORDS or m.group(1) in locals_all:
            continue
        depth, j = 0, m.end() - 1
        while j < len(body):
            if body[j] == '(':
                depth += 1
            elif body[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        calls.append((m.start(), m.group(1), j))
    # brace depth at every offset, so a def and its use can be required to sit
    # at the same nesting level (a loop-invariant hoist is not this lever).
    depths, dep = [], 0
    for ch in body:
        if ch == '{':
            dep += 1
        depths.append(dep)
        if ch == '}':
            dep -= 1
    dspans = decl_lines(body)
    for var, ty in decls.items():
        if re.search(r"&\s*" + re.escape(var) + r"(?![A-Za-z_0-9])", body):
            continue
        defs = [m.start() for m in assign_re(var).finditer(body)]
        if not defs:
            continue
        # compound assignments and ++/-- are defs that also read: treat as
        # region boundaries so they never look like a clean single-def temp.
        bounds = sorted(set(defs) | {
            m.start() for m in re.finditer(
                r"(?<![A-Za-z_0-9])" + re.escape(var) +
                r"\s*(\+\+|--|[-+*/&|^%]=|<<=|>>=)", body)} | {
            m.start() for m in re.finditer(
                r"(\+\+|--)\s*" + re.escape(var) + r"(?![A-Za-z_0-9])", body)})
        occ = [o for o in uses_of(body, var) if not in_spans(o, dspans)]
        for d in defs:
            semi = body.find(';', d)
            if semi < 0:
                continue
            eq = body.index('=', d)
            expr = body[eq + 1:semi].strip()
            nxt = min([b for b in bounds if b > d], default=len(body))
            reads = [o for o in occ if semi < o < nxt]
            if len(reads) != 1:
                continue
            u = reads[0]
            between = [c for c in calls if semi < c[0] and c[2] < u]
            if not between:
                continue
            if depths[d] != depths[u]:
                continue          # loop-invariant hoist, not a sinkable temp
            if MEMREF_RE.search(expr) or '(' in expr and re.search(
                    r"[A-Za-z_][A-Za-z_0-9]*\s*\(", expr):
                continue
            bare = NUMLIT_RE.sub(" ", expr)
            idents = [i for i in IDENT_RE.findall(bare) if i not in C_KEYWORDS]
            if any(i not in locals_all for i in idents):
                continue
            if not re.search(r"[-+*/]", expr):
                continue
            has_literal = bool(re.search(r"(?<![A-Za-z_0-9.])\d", expr))
            # curable: an operand local whose next occurrence after this def is
            # itself a def (dead through the interval), or which is never used
            # again at all.  Folding the two into one variable deletes the temp.
            curable = []
            for other in set(idents):
                if other == var:
                    continue
                later = [o for o in uses_of(body, other)
                         if o > semi and not in_spans(o, dspans)]
                if not later:
                    curable.append(other)
                    continue
                odefs = [m.start() for m in assign_re(other).finditer(body)]
                if any(abs(od - later[0]) <= 1 for od in odefs):
                    curable.append(other)
            cands.append({
                "var": var, "type": ty, "expr": expr.replace('\n', ' ')[:90],
                "calls_between": sorted(set(c[1] for c in between))[:4],
                "ncalls": len(between),
                "curable_with": curable,
                "has_literal": has_literal,
                "def_line": body[:d].count('\n'),
                "use_line": body[:u].count('\n'),
            })
    return cands


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", default="/private/tmp/A77_full.json")
    ap.add_argument("--min-size", type=int, default=0)
    ap.add_argument("--all", action="store_true",
                    help="include functions already at 100")
    ap.add_argument("--max-score", type=float, default=100.0)
    ap.add_argument("--curable-only", action="store_true")
    args = ap.parse_args()

    rep = json.load(open(args.report))
    scores = {}
    for u in rep["units"]:
        src = (u.get("metadata") or {}).get("source_path")
        if not src:
            continue
        for f in u.get("functions") or []:
            s = f.get("fuzzy_match_percent", 0.0)
            if not args.all and s >= 100.0:
                continue
            if s > args.max_score:
                continue
            if int(f.get("size", "0")) < args.min_size:
                continue
            scores.setdefault(src, {})[f["name"]] = (s, int(f.get("size", "0")),
                                                     u["name"])

    rows = []
    for src, fns in sorted(scores.items()):
        if not os.path.isfile(src):
            continue
        text = strip_comments(open(src, encoding="utf-8", errors="replace").read())
        for fname, fstart, fend in find_functions(text):
            if fname not in fns:
                continue
            score, size, unit = fns[fname]
            body = text[fstart:fend]
            decls = local_decls(body)
            if not decls:
                continue
            lall = all_locals(body) | set(decls)
            for c in analyse_function(fname, body, decls, lall):
                if args.curable_only and not c["curable_with"]:
                    continue
                c.update(src=src, fn=fname, score=score, size=size, unit=unit)
                rows.append(c)

    rows.sort(key=lambda r: (-len(r["curable_with"]), r["score"], -r["size"]))
    print("%-42s %-34s %-7s %-6s %-5s %s" %
          ("UNIT", "FUNCTION", "SCORE", "SIZE", "VAR", "DEF EXPR"))
    for r in rows:
        print("%-42s %-34s %-7.3f %-6d %-5s %s" %
              (r["unit"][:42], r["fn"][:34], r["score"], r["size"],
               r["var"][:5], r["expr"][:46]))
        print("      %s  type=%s  calls=%d %s  curable_with=%s" %
              (" " * 0, r["type"], r["ncalls"], ",".join(r["calls_between"]),
               ",".join(r["curable_with"]) or "-"))
    print("\n%d candidate sites" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
