#!/usr/bin/env python3
"""Census of decompiler-artifact identifiers still in the source.

Ghidra leaves a signature vocabulary behind: `uVar3`, `iStack_28`, `param_1`,
`DAT_803dd124`, `FUN_800c1234`. None of it is source a person wrote, so this is
pure recovery. It is NOT mechanisable: a good name needs the function read, so
this tool ranks the work rather than doing it.

⚠️ CORRECTION -- an earlier version of this docstring claimed renaming is
"byte-neutral by construction because identifiers do not reach codegen". That is
true of a C identifier and FALSE of the rename as a whole. A non-static
function's name is in its object's symbol table, and more importantly
config/GSAE01/symbols.txt is BUILD INPUT that regenerates the retail target
objects; objdiff pairs target to source BY NAME, so a rename applied to one side
only -- or measured against a stale object -- collapses the unit's score while
`locked_ninja.sh` still reports EXIT=0. Gate every rename with
tools/pairing_check.py (0 retail-only symbols) plus unitfuzzy equality on every
unit the name reaches -- see docs/rename_safety.md -- never with
tools/byteneutral.py.

    python3 tools/junk_names.py src                  # ranked census
    python3 tools/junk_names.py src --kind param     # one class
    python3 tools/junk_names.py src --list src/main/model.c

⚠️ SKIPPED ON PURPOSE (recorded as not provably layout-neutral):
  * positional stack-slot locals inside `0x43300000` int->float conversion
    blobs and FP-save spills -- their declaration ORDER is load-bearing;
  * `u64 param_1..8` vararg-pivot chains.
Both are reported under their own kind so they stay visible, but they are
flagged `HOLD` rather than offered as work.

Whatever you rename, gate it per docs/rename_safety.md: get the blast radius with
`pairing_check.py --refs <name>` FIRST, rename both the C source and
config/GSAE01/symbols.txt, rebuild (DLL objects by name -- the bare gate skips
them), then require 0 retail-only symbols and unitfuzzy equality on every unit in
that radius.
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from byteneutral import REPO  # noqa: E402
from source_coverage_audit import live_files_under  # noqa: E402

KINDS = {
    # Ghidra value temporaries: uVar1, iVar12, fVar3, auVar2, bVar1, cVar4 ...
    "var": re.compile(r"\b([abcdfilpsu]{1,2}(?:Var|Stack)_?\d+)\b"),
    # positional stack slots: local_28, fStack_1c, auStack_40
    "stack": re.compile(r"\b((?:[a-z]{1,3})?[Ss]tack_[0-9a-fA-F]+|local_[0-9a-fA-F]+)\b"),
    # unnamed parameters
    "param": re.compile(r"\bparam_(\d+)\b"),
    # address-derived data and function names
    "addr": re.compile(r"\b((?:DAT|PTR|UNK)_[0-9a-fA-F]{6,}|lbl_[0-9A-Fa-f]{6,})\b"),
    "func": re.compile(r"\b((?:FUN|fn)_[0-9a-fA-F]{6,})\b"),
}
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# int->float conversion blob and FP-save spill: declaration order is load-bearing
HOLD_CTX = re.compile(r"0x43300000|4503599627370496|psq_st|__save_fpr")


def scan(path: Path) -> dict[str, collections.Counter]:
    try:
        raw = path.read_text(errors="surrogateescape")
    except OSError:
        return {}
    text = COMMENT.sub(" ", raw)
    hold = bool(HOLD_CTX.search(text))
    out: dict[str, collections.Counter] = {}
    for kind, rx in KINDS.items():
        c = collections.Counter(m.group(1) for m in rx.finditer(text))
        if c:
            key = kind
            if hold and kind in ("stack", "param"):
                key = kind + "/HOLD"
            out[key] = out.get(key, collections.Counter()) + c
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--kind", choices=sorted(KINDS))
    ap.add_argument("--list", metavar="FILE", help="list every distinct name in one file")
    args = ap.parse_args()

    files: list[Path] = []
    for r in args.roots:
        p = REPO / r
        if p.is_dir():
            files += [Path(f) for f in
                      live_files_under(str(p), exts=(".c", ".cp", ".cpp"))]
        else:
            files.append(p)

    if args.list:
        p = REPO / args.list
        for kind, c in sorted(scan(p).items()):
            print(f"{kind}: {', '.join(n for n, _ in c.most_common(40))}")
        return 0

    per_file: list[tuple[int, str, dict]] = []
    totals: collections.Counter = collections.Counter()
    for f in files:
        got = scan(f)
        if args.kind:
            got = {k: v for k, v in got.items() if k.split("/")[0] == args.kind}
        if not got:
            continue
        n = sum(sum(c.values()) for c in got.values())
        per_file.append((n, str(f.relative_to(REPO)), got))
        for k, c in got.items():
            totals[k] += sum(c.values())

    for n, rel, got in sorted(per_file, reverse=True)[:40]:
        parts = " ".join(f"{k}={sum(c.values())}" for k, c in sorted(got.items()))
        print(f"{n:6d}  {rel:60s} {parts}")
    print(f"-- {len(per_file)} files; " +
          ", ".join(f"{k}={v}" for k, v in sorted(totals.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
