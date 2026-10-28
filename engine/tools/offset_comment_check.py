#!/usr/bin/env python3
"""Verify /* 0xNN: */ field-offset comments against the real struct layout.

No gate sees these claims: STATIC_ASSERT anchors only a few fields per struct,
so a padding edit silently falsifies every offset comment below it. This uses
the real compiler as the oracle (never a hand-rolled type-size model) by
emitting STATIC_ASSERT(offsetof(S, f) == CLAIMED) for every commented field.

Usage:  python3 tools/offset_comment_check.py [file ...]
        (no args = sweep every file under include/ AND src/ carrying a claim)

Exit 0 = every claim verified; exit 1 = at least one file failed.

COVERAGE, stated because two thirds of this tool's blind spots were structural
rather than logical, and a claim it never parses is indistinguishable from a
claim it verified:
  - Population is `include/**/*.h` AND `src/**/*.c`. A struct declared inside a
    game .c carries offset claims too -- 64 of them -- and scanning include/
    alone left every one unread. A .c is probed by appending the assertions to
    a copy of the unit, so the oracle is the same compiler either way.
  - Aggregates are found by brace MATCHING. The former non-greedy regex
    (`\\{(.*?)\\}\\s*(\\w+)\\s*;`) stopped at the first `}` followed by a name and
    a semicolon, which is exactly what a nested anonymous MEMBER looks like, so
    on any struct carrying one it took the member's name for a type name and
    silently dropped every field below it. It also read only
    `typedef struct {..} Name;` -- a plain `struct Name {..};` and a
    `typedef union` were outside the grammar entirely.
  - Fields of a nested anonymous member are probed through a scoped path
    (`offsetof(T, outer.inner)`) instead of being discarded.

Two known false-positive shapes, both requiring a human read of the hit:
  - a sub-location note on a padding block: `u8 pad04[0x38]; /* 0x18: ... */`
    documents an offset INSIDE the pad, not the pad's own offset. Legitimate
    when start <= claim < start+size; a claim outside that span is a real bug.
  - a file that will not compile standalone (missing type dep) reports as
    FAIL_BUILD. That is a harness gap and not a false comment -- but it is also
    how a header that genuinely fails to declare its own dependencies shows up,
    so read it before dismissing it.

Bitfields are skipped on purpose: a bit position is not a byte offset.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from source_coverage_audit import live_files_under  # noqa: E402

CC = [
    "build/tools/wibo", "build/tools/sjiswrap.exe",
    "build/compilers/GC/2.0/mwcceppc.exe",
    "-nodefaults", "-proc", "gekko", "-align", "powerpc", "-enum", "int",
    "-fp", "hardware", "-Cpp_exceptions", "off", "-O4,p", "-inline", "auto",
    "-pragma", "cats off", "-pragma", "warn_notinlined off",
    "-maxerrors", "200", "-nosyspath", "-RTTI", "off", "-fp_contract", "on",
    "-str", "reuse", "-multibyte", "-i", "include", "-i", "build/GSAE01/include",
    "-DBUILD_VERSION=0", "-DVERSION_GSAE01", "-DNDEBUG=1", "-lang=c",
]

# Aggregate heads, found by brace MATCHING rather than by a non-greedy regex.
# The regex form (`\{(.*?)\}\s*(\w+)\s*;`) stops at the first closing brace that
# is followed by a name and a semicolon -- which is what a nested anonymous
# member looks like -- so on any struct carrying one it took the MEMBER's name
# for the type name and dropped every field past it. That is how DSPvoice's
# `} playInfo;` member became a phantom type and took the whole header's claims
# out of the census with it.
HEAD_RE = re.compile(r"\b(typedef\s+)?(struct|union)\b[ \t]*(\w+)?\s*\{")
# Only a COLON form is an offset claim. `/* 0x80 = caught in beam */` is a BIT
# mask and `/* obj+0xC0 swap slot */` names the value's source, not this field.
FIELD_RE = re.compile(
    r"\s*(?:const\s+)?([A-Za-z_][\w ]*?[\w*])\s+(\*?\w+)\s*(\[[^;]*\])?\s*;"
    r"\s*/\*\s*0x([0-9A-Fa-f]{1,4})\s*:"
)
# `} name;` / `} name[2];` closing an anonymous nested aggregate: its fields are
# reachable, but only through a scoped path.
NESTED_TAIL_RE = re.compile(r"^\s*\}\s*(\w+)\s*(\[[^\]]*\])?\s*;")


def _match_brace(src, i):
    """Index just past the '}' matching the '{' at src[i]."""
    depth = 0
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def _walk_body(body, tag, prefix, found):
    """Collect claims in one aggregate body, descending into nested members.

    A nested anonymous member contributes its fields under a scoped path
    (`offsetof(T, outer.inner)`), which is what makes them checkable at all;
    the previous line-oriented pass discarded them wholesale.
    """
    i = 0
    while i < len(body):
        j = body.find("{", i)
        line_end = body.find("\n", i)
        if j < 0 or (line_end >= 0 and False):
            pass
        if j < 0:
            chunk, i = body[i:], len(body)
        else:
            chunk, nxt = body[i:j], _match_brace(body, j)
            if nxt < 0:
                chunk, i = body[i:], len(body)
            else:
                inner = body[j + 1:nxt - 1]
                tail = body[nxt:body.find("\n", nxt) if body.find("\n", nxt) >= 0
                            else len(body)]
                tm = NESTED_TAIL_RE.match("}" + tail)
                if tm and not tm.group(2):
                    _walk_body(inner, tag, prefix + tm.group(1) + ".", found)
                i = nxt
        for line in chunk.split("\n"):
            fm = FIELD_RE.match(line)
            if not fm or ":" in line.split("/*")[0]:  # skip bitfields
                continue
            found.append((tag, prefix + fm.group(2).lstrip("*"),
                          int(fm.group(4), 16)))


def probes_for(header):
    src = re.sub(r"//.*", "", open(header, encoding="utf-8", errors="replace").read())
    found = []
    for m in HEAD_RE.finditer(src):
        end = _match_brace(src, m.end() - 1)
        if end < 0:
            continue
        tail = src[end:src.find(";", end) + 1] if ";" in src[end:end + 200] else ""
        name = re.match(r"\s*(\w+)\s*;", tail)
        if m.group(1) and name:                 # typedef struct {..} Name;
            tag = name.group(1)
        elif m.group(3) and not m.group(1):     # struct Name {..};
            tag = "%s %s" % (m.group(2), m.group(3))
        else:
            continue                            # anonymous member: reached below
        _walk_body(src[m.end():end - 1], tag, "", found)
    return found


def check(header):
    probes = probes_for(header)
    if not probes:
        return "SKIP", 0, ""
    if header.endswith(".c"):
        # A struct declared inside a .c is reachable only from that TU, so the
        # probes ride along in a copy of the unit itself rather than in a
        # synthetic includer. Same compiler, same oracle.
        body = open(header, "rb").read().decode("latin-1")
        lines = [body]
    else:
        inc = header.split("include/", 1)[-1]
        lines = ['#include "global.h"', '#include "game/objects/object.h"',
                 f'#include "{inc}"']
    for i, (tag, name, off) in enumerate(probes):
        lines.append(
            f"char probe_{i}[(offsetof({tag},{name})==0x{off:X})?1:-1]; "
            f"/* {tag}.{name} @0x{off:X} */"
        )
    with tempfile.TemporaryDirectory() as td:
        cf = os.path.join(td, "probe.c")
        open(cf, "wb").write(("\n".join(lines) + "\n").encode("latin-1"))
        r = subprocess.run(
            CC + ["-c", cf, "-o", os.path.join(td, "probe.o")],
            cwd=ROOT, capture_output=True, text=True,
        )
        out = r.stdout + r.stderr
    if "Error" not in out:
        return "CLEAN", len(probes), ""
    bad = re.findall(r"offsetof\(([A-Za-z_]\w*),(\w+)\)==0x([0-9A-Fa-f]+)", out)
    if not bad:
        return "FAIL_BUILD", len(probes), out.strip().split("\n")[-1]
    return "FAIL", len(probes), "; ".join(f"{s}.{f} claims 0x{o}" for s, f, o in bad)


def main():
    argv = sys.argv[1:]
    if argv:
        headers = argv
    else:
        # `include/` is not the whole population: a struct declared inside a
        # game .c carries offset claims that no gate and, until this root was
        # added, no instrument ever read.  Sources come from build.ninja's
        # answer (plus group-included files), not a filesystem walk: an offset
        # claim in a source the build never compiles is about nothing.
        cands = [
            os.path.join(dp, fn)
            for base in ("include", "src")
            for dp, _, fns in os.walk(os.path.join(ROOT, base))
            for fn in fns
            if fn.endswith(".h")
        ] + live_files_under("src", exts=(".c", ".cp", ".cpp"))
        headers = sorted(
            p for p in cands
            if re.search(
                r"/\* ?0x[0-9A-Fa-f]{1,4} ?:",
                open(p, encoding="utf-8", errors="replace").read(),
            )
        )
    total, bad = 0, 0
    for h in headers:
        rel = os.path.relpath(h, ROOT)
        status, n, detail = check(h)
        if status == "CLEAN":
            total += n
        elif status != "SKIP":
            bad += 1
            print(f"{status:10s} {rel} ({n} probes)  {detail}")
    print(f"\n{total} offset claims verified TRUE; {bad} header(s) need a look.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
