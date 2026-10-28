#!/usr/bin/env python3
"""Census of unreferenced local declarations, taken from the compiler itself.

MWCC already knows which locals are dead: `-w unusedvar` emits

    #     702: int zzzDeadCanary;
    # Warning:     ^^^^^^^^^^^^^
    #   variable / argument 'zzzDeadCanary' is not used in function

Asking the compiler beats parsing C.  A hand-rolled scanner mistakes struct
members for locals (anonymous-struct fields), `} name;` struct terminators, and
macro-argument continuation lines; the compiler makes none of those mistakes,
and it sees through macros, so a local that is only referenced from inside a
macro expansion is correctly reported as USED.

    python3 tools/unused_locals.py                       # census the whole tree
    python3 tools/unused_locals.py src/main/object.c     # one file
    python3 tools/unused_locals.py --json out.json

Parameters are excluded: the warning covers "variable / argument", but deleting
an argument changes the ABI, so only standalone declaration statements are
reported.  Everything reported still has to pass tools/byteneutral.py before it
is deleted -- this tool proposes, it does not prove.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from byteneutral import REPO, SJIS, WIBO, BuildInfo  # noqa: E402

WARN = re.compile(r"variable / argument '([^']+)' is not used in function")
SRCLINE = re.compile(r"^#\s+(\d+):\s?(.*)$")
# a standalone declaration statement: ends in ';', is not a parameter list
DECL = re.compile(r"^\s*[A-Za-z_][\w \t\*&,\[\]]*\b\w+\s*(\[[^\]]*\])?\s*;\s*$")


def census_file(src: str, info: BuildInfo, work: Path) -> list[dict]:
    got = info.get(src)
    if got is None:
        return []
    _obj, cflags, mw = got
    cc = REPO / "build" / "compilers" / mw / "mwcceppc.exe"
    if not cc.is_file():
        return []
    argv = [str(WIBO)]
    if SJIS.is_file():
        argv.append(str(SJIS))
    argv.append(str(cc))
    flags = shlex.split(cflags)
    # -maxerrors 1 truncates the warning list; raise it just for the census
    for i, f in enumerate(flags):
        if f == "-maxerrors" and i + 1 < len(flags):
            flags[i + 1] = "9999"
    argv += flags + ["-w", "unusedvar", "-c", "-o", str(work / "c.o"), src]
    r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")

    hits: list[dict] = []
    lines = out.split("\n")
    for i, line in enumerate(lines):
        m = WARN.search(line)
        if not m:
            continue
        name = m.group(1)
        # walk back to the quoted source line that carries the line number
        lineno, text = None, ""
        for j in range(i - 1, max(-1, i - 6), -1):
            s = SRCLINE.match(lines[j])
            if s:
                lineno, text = int(s.group(1)), s.group(2)
                break
        if lineno is None or not DECL.match(text):
            continue  # parameter, or a shape we will not touch blind
        if not re.search(r"\b%s\b" % re.escape(name), text):
            continue
        hits.append({"file": src, "line": lineno, "name": name,
                     "decl": text.strip()})
    return hits


def multi_name(decl: str) -> bool:
    """`int a, b;` -- deleting the line would take a live name with it."""
    return "," in decl.split(";")[0]


def prune(hits: list[dict], info: BuildInfo, work: Path) -> int:
    """Delete each candidate, keeping only those that leave the object identical."""
    from byteneutral import compile_md5

    removed = 0
    by_file: dict[str, list[dict]] = {}
    for h in hits:
        by_file.setdefault(h["file"], []).append(h)

    for src, ents in sorted(by_file.items()):
        path = REPO / src
        orig = path.read_text(errors="surrogateescape").split("\n")
        base = compile_md5(path.read_bytes(), src, info, work)
        if base in ("FAIL", "NOCC", "NOBUILD"):
            print(f"  !! cannot baseline {src} ({base})")
            continue

        def with_deleted(lines_to_cut: list[int]) -> str:
            out = list(orig)
            for ln in sorted(lines_to_cut, reverse=True):
                del out[ln - 1]
            return "\n".join(out)

        cands = []
        for e in sorted({h["line"]: h for h in ents}.values(), key=lambda x: x["line"]):
            if multi_name(e["decl"]):
                print(f"  -- skip (multi-name) {src}:{e['line']} {e['decl']}")
                continue
            cands.append(e["line"])
        if not cands:
            continue

        keep = []
        if compile_md5(with_deleted(cands).encode(errors="surrogateescape"),
                       src, info, work) == base:
            keep = cands
        else:  # bisect: accept each line that is individually neutral
            for ln in cands:
                if compile_md5(with_deleted([ln]).encode(errors="surrogateescape"),
                               src, info, work) == base:
                    keep.append(ln)
            while keep and compile_md5(with_deleted(keep).encode(errors="surrogateescape"),
                                       src, info, work) != base:
                keep.pop()
        if keep:
            path.write_text(with_deleted(keep), errors="surrogateescape")
            removed += len(keep)
        print(f"  {src}: removed {len(keep)}/{len(cands)}"
              + (f"  (rejected {[l for l in cands if l not in keep]})"
                 if len(keep) != len(cands) else ""))
    return removed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="*", help="default: every source in build.ninja")
    ap.add_argument("--json", help="write the census to this path")
    ap.add_argument("--prune", action="store_true",
                    help="delete every candidate whose removal leaves the object identical")
    args = ap.parse_args()

    info = BuildInfo()
    srcs = args.sources or sorted(info._by_src)
    all_hits: list[dict] = []
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        for n, src in enumerate(srcs, 1):
            if not (REPO / src).is_file():
                continue
            hits = census_file(src, info, work)
            all_hits += hits
            if hits:
                print(f"{src}: {len(hits)}")
            if len(srcs) > 20 and n % 100 == 0:
                print(f"  ... {n}/{len(srcs)} files, {len(all_hits)} found",
                      file=sys.stderr)
    print(f"-- {len(all_hits)} unreferenced local declarations over {len(srcs)} files")
    if args.json:
        Path(args.json).write_text(json.dumps(all_hits, indent=1))
        print(f"-- wrote {args.json}")
    if args.prune and all_hits:
        with tempfile.TemporaryDirectory() as td:
            n = prune(all_hits, info, Path(td))
        print(f"-- pruned {n} declarations (every remaining object byte-identical)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
