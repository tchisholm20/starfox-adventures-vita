#!/usr/bin/env python3
"""Find `extern` declarations a .c file no longer uses.

Distinct from tools/dead_decl_sweep.py: that one finds symbols nothing in the
TREE references. This finds declarations that this FILE does not reference --
the symbol may be alive and well elsewhere. A local `extern` for a symbol the
file never names is pure transcription noise: it emits no code, so removing it
is byte-neutral by construction.

"By construction" is not "in fact", though: the name may be reached through a
macro, in which case removal fails to compile. Every candidate is therefore
gated through tools/byteneutral.py, which treats a failed compile as FAIL and
never as equal.

    python3 tools/unused_externs.py src/main            # report
    python3 tools/unused_externs.py src/main --prune    # remove the safe ones
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

# a single-line, file-scope extern declaration of one named entity
EXTERN = re.compile(
    r"^extern\s+[A-Za-z_][\w \t\*&]*?([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*;\s*$")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def candidates(path: Path) -> list[tuple[int, str, str]]:
    text = path.read_text(errors="surrogateescape")
    stripped = COMMENT.sub(" ", text)
    out = []
    for i, line in enumerate(text.split("\n"), 1):
        m = EXTERN.match(line)
        if not m:
            continue
        name = m.group(1)
        # count uses outside this declaration line
        uses = len(re.findall(r"\b%s\b" % re.escape(name), stripped))
        if uses <= 1:
            out.append((i, name, line.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+", help="files or directories under src/")
    ap.add_argument("--prune", action="store_true")
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
    total = removed = 0
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        for f in files:
            rel = str(f.relative_to(REPO))
            cands = candidates(f)
            if not cands:
                continue
            total += len(cands)
            print(f"{rel}: {len(cands)}")
            for _ln, _name, decl in cands[:4]:
                print(f"    {decl[:76]}")
            if not args.prune or info.get(rel) is None:
                continue
            orig = f.read_text(errors="surrogateescape").split("\n")
            base = compile_md5(f.read_bytes(), rel, info, work)
            if base in ("FAIL", "NOCC", "NOBUILD"):
                print(f"    !! cannot baseline ({base})")
                continue

            def cut(lines: list[int]) -> str:
                o = list(orig)
                for ln in sorted(lines, reverse=True):
                    del o[ln - 1]
                return "\n".join(o)

            want = [c[0] for c in cands]
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
    print(f"-- {total} file-local unused externs"
          + (f"; removed {removed}" if args.prune else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
