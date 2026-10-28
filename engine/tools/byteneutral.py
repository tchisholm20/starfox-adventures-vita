#!/usr/bin/env python3
"""Prove a source edit is byte-neutral: same source, same object, bit for bit.

Semantic recovery (deleting dead declarations, renaming, retyping) must not move
a single byte of a matched function. This compiles two versions of one source
file and compares the resulting objects, so "the code did not change" is a
measurement rather than a hope.

    python3 tools/byteneutral.py src/main/object.c            # HEAD vs worktree
    python3 tools/byteneutral.py --all                        # every modified .c
    python3 tools/byteneutral.py --md5 src/main/object.c      # just the md5

Exit status is 0 only when every file checked is byte-identical.

THREE TRAPS THIS TOOL EXISTS TO AVOID (each one silently produced a false PASS):

1. STALE OBJECT.  A failed compile leaves the previous object on disk.  Reading
   its md5 reports the last good build and every broken edit "passes".  We unlink
   the object before each compile and treat a missing object as FAIL, never as
   equal.

2. WRONG COMPILER.  The tree is not one toolchain: game code is MWCC GC/2.0 and
   audio/MSL is GC/1.2.5n.  Hard-coding GC/2.0 silently mis-compiles MusyX, so a
   real regression there reads as "identical".  We take mw_version and cflags
   from build.ninja, per object.

3. PATH-DEPENDENT OUTPUT.  MWCC embeds the source path in the object, so
   compiling old.c and new.c always differs even when the code is identical.
   Both versions are compiled to the SAME scratch path, one after the other.

4. FLAKE.  A gate that is sometimes wrong is worse than no gate, so a CHANGED
   verdict is only reported after recompiling the baseline and confirming the
   compiler was deterministic; otherwise you get a loud NONDET.

⚠️ THIS TOOL CANNOT GATE A FUNCTION RENAME, BY CONSTRUCTION.
A non-static function's name lives in the object's symbol table, so renaming it
MUST change the md5.  `CHANGED ... not byte-neutral` (exit 1) is the EXPECTED
result of a *correct* rename, not a failure -- do not "fix" it by reverting, and
do not keep adjusting until the md5 settles, because the only renames that
satisfy md5 are statics.  Use this tool for the DELETION / RETYPE / COMMENT
class, where the emitted code genuinely must not move.

    For a rename the gate is instead:
      * tools/unitfuzzy.py <unit> identical before and after, and
      * full tools/locked_ninja.sh EXIT=0.


SCOPE -- what this tool can and cannot gate
-------------------------------------------
It gates changes that must not alter codegen: deletions of dead declarations,
retypes, comment and formatting edits.

It CANNOT gate a rename.  A symbol name is *in* the object, so any rename
legitimately reports CHANGED -- gate those with `tools/pairing_check.py` plus
`tools/unitfuzzy.py`, per docs/rename_safety.md.

Baseline compiles use HEAD's include tree (`head_include_tree()`), not the
worktree's, so a change that also edits a header is compared like-for-like.
Before that fix such a change made the BASELINE fail to compile and the tool
printed `FAIL/<md5>` -- while exiting 0, i.e. silent success.  A failed compile
now always counts as `bad`, in every mode including `--md5`.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD_NINJA = REPO / "build.ninja"
WIBO = REPO / "build" / "tools" / "wibo"
SJIS = REPO / "build" / "tools" / "sjiswrap.exe"


class BuildInfo:
    """cflags / mw_version / object path, as build.ninja actually states them."""

    def __init__(self) -> None:
        self._lines = BUILD_NINJA.read_text(errors="replace").split("\n")
        self._by_src: dict[str, tuple[str, str, str]] = {}
        self._scan()

    def _scan(self) -> None:
        for i, line in enumerate(self._lines):
            if not line.startswith("build build/"):
                continue
            # A build statement may wrap anywhere, including immediately after
            # the ':' -- so join continuations before looking for the source.
            stmt, j = line, i
            while stmt.rstrip().endswith("$") and j + 1 < len(self._lines):
                j += 1
                stmt = stmt.rstrip()[:-1] + " " + self._lines[j].strip()
            m = re.match(r"^build (build/\S+\.o):\s*(.*)$", stmt)
            if not m:
                continue
            obj, rest = m.group(1), m.group(2)
            src = next((c for c in rest.split()
                        if c.endswith((".c", ".cpp"))), None)
            if src is None:
                continue
            cflags, mw = self._props(j)
            if cflags and src not in self._by_src:
                self._by_src[src] = (obj, cflags, mw or "GC/2.0")

    def _props(self, start: int) -> tuple[str, str]:
        cflags = mw = ""
        j = start + 1
        while j < len(self._lines) and not self._lines[j].startswith("build "):
            s = self._lines[j].strip()
            if s.startswith("mw_version = "):
                mw = s[len("mw_version = "):]
            elif s.startswith("cflags = "):
                buf = s[len("cflags = "):]
                while buf.endswith("$") and j + 1 < len(self._lines):
                    j += 1
                    buf = buf[:-1] + " " + self._lines[j].strip()
                cflags = buf
            j += 1
        return cflags, mw

    def get(self, src: str) -> tuple[str, str, str] | None:
        return self._by_src.get(src.replace("\\", "/"))


def compile_md5(content: bytes, src: str, info: BuildInfo, workdir: Path,
                head_includes: bool = False) -> str:
    """Compile `content` as if it were `src`. Returns an md5, or 'FAIL'.

    `head_includes` compiles against HEAD's include tree instead of the
    worktree's.  Without it the baseline compile of HEAD's .c resolves headers
    against your MODIFIED headers, so any change that renames a header field,
    macro or declaration makes the baseline fail to compile -- and the tool
    reports FAIL for a change that is in fact byte-neutral.
    """
    got = info.get(src)
    if got is None:
        return "NOBUILD"
    _obj, cflags, mw = got
    # trap 3: one fixed path for every version we compile
    cfile = workdir / ("bn" + Path(src).suffix)
    ofile = workdir / "bn.o"
    cfile.write_bytes(content)
    # trap 1: never let a previous object survive into this measurement
    if ofile.exists():
        ofile.unlink()
    # trap 2: the compiler build comes from build.ninja, not from an assumption
    cc = REPO / "build" / "compilers" / mw / "mwcceppc.exe"
    if not cc.is_file():
        return "NOCC"
    argv = [str(WIBO)]
    if SJIS.is_file():
        argv.append(str(SJIS))
    argv.append(str(cc))
    flags = shlex.split(cflags)
    if head_includes:
        tree = head_include_tree()
        if tree is not None:
            out = []
            i = 0
            while i < len(flags):
                if flags[i] == "-i" and i + 1 < len(flags) and \
                        flags[i + 1] in ("include", "./include"):
                    out += ["-i", str(tree / "include")]
                    i += 2
                else:
                    out.append(flags[i])
                    i += 1
            flags = out
    argv += flags
    argv += ["-c", "-o", str(ofile), str(cfile)]
    subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
    if not ofile.is_file() or ofile.stat().st_size == 0:
        return "FAIL"
    return hashlib.md5(ofile.read_bytes()).hexdigest()


_HEAD_TREE: list = []


def head_include_tree() -> Path | None:
    """Materialise HEAD's `include/` once, for like-for-like baseline compiles."""
    if _HEAD_TREE:
        return _HEAD_TREE[0]
    d = Path(tempfile.mkdtemp(prefix="bn_headinc_"))
    r = subprocess.run(f"git archive HEAD include | tar -x -C {shlex.quote(str(d))}",
                       cwd=REPO, shell=True, capture_output=True)
    if r.returncode != 0 or not (d / "include").is_dir():
        _HEAD_TREE.append(None)
        return None
    _HEAD_TREE.append(d)
    return d


def head_bytes(src: str) -> bytes | None:
    r = subprocess.run(["git", "show", f"HEAD:{src}"], cwd=REPO,
                       capture_output=True)
    return r.stdout if r.returncode == 0 else None


def modified_sources() -> list[str]:
    r = subprocess.run(["git", "diff", "--name-only"], cwd=REPO,
                       capture_output=True, text=True)
    return [f for f in r.stdout.split() if f.endswith((".c", ".cpp"))]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="*")
    ap.add_argument("--all", action="store_true",
                    help="check every modified .c in the worktree")
    ap.add_argument("--md5", action="store_true",
                    help="print the worktree object md5 instead of comparing")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    srcs = list(args.sources)
    if args.all:
        srcs += modified_sources()
    if not srcs:
        ap.error("give at least one source, or --all")

    info = BuildInfo()
    bad = 0
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        for src in dict.fromkeys(srcs):
            cur = (REPO / src).read_bytes()
            if args.md5:
                m = compile_md5(cur, src, info, work)
                print(f"{m}  {src}")
                if m in ("FAIL", "NOCC", "NOBUILD"):
                    bad += 1          # a failed compile is never a pass
                continue
            old = head_bytes(src)
            if old is None:
                print(f"?? not in HEAD          {src}")
                bad += 1
                continue
            a = compile_md5(old, src, info, work, head_includes=True)
            b = compile_md5(cur, src, info, work)
            # A gate may not flake. If the two versions disagree, re-compile the
            # baseline and require the compiler to have been deterministic before
            # believing the difference is real.
            if a != b and a not in ("FAIL", "NOCC", "NOBUILD"):
                if compile_md5(old, src, info, work, head_includes=True) != a:
                    print(f"NONDET   compiler not reproducible  {src}")
                    bad += 1
                    continue
            if a in ("FAIL", "NOCC", "NOBUILD") or b in ("FAIL", "NOCC", "NOBUILD"):
                print(f"!! {a}/{b}  {src}")
                bad += 1
            elif a != b:
                print(f"CHANGED  {a[:12]} -> {b[:12]}  {src}")
                bad += 1
            elif not args.quiet:
                print(f"identical  {src}")
    if not args.md5:
        print(f"-- {len(dict.fromkeys(srcs))} checked, {bad} not byte-neutral")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
