#!/usr/bin/env python3
"""Find extern declarations that nothing in the tree references.

An `extern T foo;` that no translation unit reads or writes emits no
relocation, so it is invisible to the compiler, to the linker and to the DOL
gate. It is pure noise that survives forever unless something screens for it.

Screen: for every identifier declared `extern` in src/ or include/, count its
whole-word occurrences over all of src/ and include/. When the count equals
the number of declaration sites, no use exists anywhere and every declaration
of it is dead.

Occurrences are counted over the raw text including comments, so the screen
errs towards keeping a declaration alive -- a name mentioned only in a comment
still suppresses the report.

Usage:
    python3 tools/dead_decl_sweep.py               # report
    python3 tools/dead_decl_sweep.py --known-only  # only symbols.txt symbols
    python3 tools/dead_decl_sweep.py --apply       # delete the dead lines
"""

import argparse
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(ROOT, "config", "GSAE01", "symbols.txt")
ROOTS = ("src", "include")
EXTS = (".c", ".h", ".cpp", ".hpp")

IDENT = re.compile(r"[A-Za-z_]\w*")
TRAIL = re.compile(r"(?:\s*\[[^\[\]]*\])+$")


def declared_name(line):
    """Name declared by a single-line `extern ...;`, or None."""
    s = line.strip()
    if not s.startswith("extern ") or not s.endswith(";") or s.startswith('extern "C"'):
        return None
    s = s[len("extern "):-1].strip()
    s = TRAIL.sub("", s).strip()          # trailing array bounds
    if s.endswith(")"):                   # function declarator
        depth = 0
        for i in range(len(s) - 1, -1, -1):
            if s[i] == ")":
                depth += 1
            elif s[i] == "(":
                depth -= 1
                if depth == 0:
                    s = s[:i].strip()
                    break
        else:
            return None
        if s.endswith(")"):               # pointer-to-function declarator
            return None
    m = re.search(r"([A-Za-z_]\w*)$", s)
    return m.group(1) if m else None


def symbol_names():
    names = set()
    for line in open(SYMBOLS, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*([A-Za-z_\$][\w\$\.@]*)\s*=", line)
        if m:
            names.add(m.group(1))
    return names


def walk():
    for r in ROOTS:
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, r)):
            for f in files:
                if f.endswith(EXTS):
                    yield os.path.join(dirpath, f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--known-only", action="store_true",
                    help="restrict to identifiers config/GSAE01/symbols.txt defines")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    known = symbol_names()
    counts = defaultdict(int)
    decls = defaultdict(list)  # name -> [(path, lineno, text)]

    files = sorted(walk())
    texts = {}
    for path in files:
        text = open(path, "rb").read().decode("utf-8", errors="replace")
        texts[path] = text
        for tok in IDENT.findall(text):
            counts[tok] += 1
        for i, line in enumerate(text.split("\n"), 1):
            name = declared_name(line)
            if name:
                decls[name].append((path, i, line))

    dead = []
    for name, sites in sorted(decls.items()):
        if args.known_only and name not in known:
            continue
        if counts[name] == len(sites):
            dead.append((name, sites))

    total = 0
    per_file = defaultdict(list)
    for name, sites in dead:
        for path, lineno, line in sites:
            total += 1
            per_file[path].append((lineno, name, line))
    for path in sorted(per_file):
        rel = os.path.relpath(path, ROOT)
        for lineno, name, line in sorted(per_file[path]):
            mark = "" if name in known else "  [not in symbols.txt]"
            print(f"{rel}:{lineno}: {name}{mark}")
    print(f"\n{len(dead)} dead identifiers over {total} declaration lines "
          f"in {len(per_file)} files")

    if args.apply:
        for path, items in per_file.items():
            lines = texts[path].split("\n")
            drop = {ln - 1 for ln, _n, _l in items}
            out = [l for i, l in enumerate(lines) if i not in drop]
            open(path, "wb").write("\n".join(out).encode("utf-8"))
        print("applied")


if __name__ == "__main__":
    sys.exit(main())
