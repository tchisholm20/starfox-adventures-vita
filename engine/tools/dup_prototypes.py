#!/usr/bin/env python3
"""Find local function prototypes that an already-included header re-declares.

A .c file that carries its own `void foo(int);` while a header it already
includes declares the same function is holding a fossil: the local copy is what
the decomp needed before the header existed. Deleting it emits nothing, so it is
byte-neutral -- but only when the header is ALREADY included. Adding an
`#include` is NOT byte-neutral (it can pull in declarations that change
codegen), so this tool never proposes one.

Matching is exact on the normalised signature: return type, then each parameter
TYPE with parameter names stripped. Signedness is part of the type and is
load-bearing (docs/STYLE.md), so `u8` vs `s8` counts as a mismatch and the local
prototype is kept and reported as a DISCREPANCY -- those are latent bugs worth a
human look, not deletions.

    python3 tools/dup_prototypes.py src/main            # report
    python3 tools/dup_prototypes.py src --discrepancies # only the mismatches
    python3 tools/dup_prototypes.py src/main --prune    # delete the safe ones

Every deletion is gated through tools/byteneutral.py.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from byteneutral import REPO, BuildInfo, compile_md5  # noqa: E402
from source_coverage_audit import live_files_under  # noqa: E402

INCLUDE_DIRS = [REPO / "include", REPO / "build" / "GSAE01" / "include"]
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# one-line file-scope prototype: not static, ends in ');'
PROTO = re.compile(r"^([A-Za-z_][\w \t\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*;\s*$")


def strip_comments(text: str) -> str:
    """Blank out comments WITHOUT changing line count.

    Replacing a block comment with a single space collapses the lines it spanned,
    which silently shifts every line number after it -- and a wrong deletion of a
    COMMENT line still passes an md5 gate, because comments do not reach codegen.
    Keep the newlines.
    """
    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    return COMMENT.sub(blank, text)


def norm_type(t: str) -> str:
    t = re.sub(r"\s+", " ", t.strip())
    # storage class is not part of the type
    t = re.sub(r"\bextern\s+", "", t)
    # `struct Foo*` and `Foo*` name the same type when Foo is a typedef'd struct
    t = re.sub(r"\b(struct|union|enum)\s+", "", t)
    t = re.sub(r"\s*\*\s*", "*", t)
    return t


def norm_params(params: str) -> str:
    params = params.strip()
    # C89: `f()` leaves the parameters UNSPECIFIED; `f(void)` says there are none.
    # Conflating them would let us delete a weaker declaration as if it matched.
    if params == "":
        return "<unspecified>"
    if params == "void":
        return "void"
    out = []
    depth = 0
    cur = ""
    for ch in params:
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        cur += ch
    out.append(cur)
    res = []
    for p in out:
        p = norm_type(p)
        # drop a trailing parameter NAME, keep the type (and any [] suffix)
        m = re.match(r"^(.*?[\s\*])([A-Za-z_]\w*)((\[[^\]]*\])*)$", p)
        if m and m.group(2) not in ("void", "char", "int", "short", "long",
                                    "unsigned", "signed", "float", "double"):
            p = norm_type(m.group(1)) + m.group(3)
        res.append(p)
    return ",".join(res)


def signature(ret: str, params: str) -> str:
    return f"{norm_type(ret)}({norm_params(params)})"


def find_protos(text: str) -> list[tuple[int, str, str, str]]:
    out = []
    for i, line in enumerate(strip_comments(text).split("\n"), 1):
        if line.startswith(("static", "typedef", "#")):
            continue
        m = PROTO.match(line)
        if not m:
            continue
        ret, name, params = m.group(1), m.group(2), m.group(3)
        if ret.strip() in ("return", "else", "case"):
            continue
        # `Type var AT_ADDRESS(addr);` is a VARIABLE declaration whose macro
        # call the PROTO regex reads as a function named AT_ADDRESS.
        if name == "AT_ADDRESS":
            continue
        out.append((i, name, signature(ret, params), line.strip()))
    return out


def included_headers(path: Path, seen: set[Path] | None = None) -> set[Path]:
    """Transitive set of project headers reachable from this file."""
    if seen is None:
        seen = set()
    try:
        text = strip_comments(path.read_text(errors="surrogateescape"))
    except OSError:
        return seen
    for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        for d in INCLUDE_DIRS:
            cand = (d / m.group(1))
            if cand.is_file() and cand not in seen:
                seen.add(cand)
                included_headers(cand, seen)
                break
    return seen


def header_decls(headers: set[Path]) -> dict[str, set[str]]:
    decls: dict[str, set[str]] = {}
    for h in headers:
        try:
            text = h.read_text(errors="surrogateescape")
        except OSError:
            continue
        for _ln, name, sig, _raw in find_protos(text):
            decls.setdefault(name, set()).add(sig)
    return decls


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--discrepancies", action="store_true",
                    help="only list name matches whose signature differs")
    args = ap.parse_args()

    files: list[Path] = []
    for r in args.roots:
        p = REPO / r
        if p.is_dir():
            files += [Path(x) for x in
                      live_files_under(str(p), exts=(".c", ".cp", ".cpp"))]
        else:
            files.append(p)

    info = BuildInfo()
    n_dup = n_mismatch = removed = 0
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        for f in files:
            rel = str(f.relative_to(REPO))
            text = f.read_text(errors="surrogateescape")
            protos = find_protos(text)
            if not protos:
                continue
            decls = header_decls(included_headers(f))
            dup, mism, benign = [], [], []
            for ln, name, sig, raw in protos:
                if name not in decls:
                    continue
                if sig in decls[name]:
                    dup.append((ln, name, sig, raw))
                elif sig.replace("(void)", "(<unspecified>)") in decls[name]:
                    # C89: a zero-arg `f(void)` prototype and an unspecified
                    # `f()` declaration are compatible -- no codegen difference,
                    # but NOT prunable (deleting the local copy would weaken the
                    # visible declaration), so it is neither a dup nor a bug.
                    benign.append((ln, name, sig, raw))
                else:
                    mism.append((ln, name, sig, raw))
            n_dup += len(dup)
            n_mismatch += len(mism)
            if args.discrepancies:
                for ln, name, sig, raw in mism:
                    print(f"{rel}:{ln}  {name}\n    local  {sig}\n    header {sorted(decls[name])}")
                for ln, name, sig, raw in benign:
                    print(f"{rel}:{ln}  {name}  [benign: (void) vs () only]")
                continue
            if dup:
                print(f"{rel}: {len(dup)} duplicate"
                      + (f", {len(mism)} signature mismatch" if mism else ""))
                for _ln, _n, _s, raw in dup[:3]:
                    print(f"    {raw[:74]}")
            if not args.prune or not dup or info.get(rel) is None:
                continue
            orig = text.split("\n")
            base = compile_md5(f.read_bytes(), rel, info, work)
            if base in ("FAIL", "NOCC", "NOBUILD"):
                print(f"    !! cannot baseline ({base})")
                continue

            def cut(lines: list[int]) -> str:
                o = list(orig)
                for x in sorted(lines, reverse=True):
                    del o[x - 1]
                return "\n".join(o)

            want = [d[0] for d in dup]
            keep = want if compile_md5(cut(want).encode(errors="surrogateescape"),
                                       rel, info, work) == base else []
            if not keep:
                for ln in want:
                    if compile_md5(cut([ln]).encode(errors="surrogateescape"),
                                   rel, info, work) == base:
                        keep.append(ln)
                while keep and compile_md5(cut(keep).encode(errors="surrogateescape"),
                                           rel, info, work) != base:
                    keep.pop()
            if keep:
                f.write_text(cut(keep), errors="surrogateescape")
                removed += len(keep)
            print(f"    -> removed {len(keep)}/{len(want)}")
    print(f"-- {n_dup} header-duplicated prototypes, {n_mismatch} signature mismatches"
          + (f"; removed {removed}" if args.prune else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
