#!/usr/bin/env python3
"""Screen the tree for calls made through an IMPLICIT function declaration.

The normal build is silent about these: with no `-W` flag MWCC accepts a call
to an undeclared function, assumes `int` return and applies only the default
argument promotions.  The emitted code is usually right by luck, but a
float-returning callee, or an integer actual landing in a floating parameter
slot, is silently wrong -- and nothing in the build log, the byte gate or the
DOL gate can see it.

This tool re-runs every unit's exact ninja compile command with `-W all`
added, throws the object away, and keeps the diagnostics.  Under `-W all`
MWCC reports "function has no prototype" twice over:

  Warning, caret on the enclosing definition -- noise, one per function
  Error,   caret underlining the CALLEE NAME  -- a genuinely undeclared call

Only the second form is reported here.  Each hit is then cross-referenced
against the real prototype declared elsewhere in include/, and classified:

  A  return type is not int-width      -> the implicit `int` return is wrong
  B  callee has a floating parameter   -> an integer actual lands in a GPR
  C  callee has a narrow integer value -> the callee expects an explicit conversion
  D  no prototype anywhere in the tree

Usage:
    python3 tools/proto_screen.py                 # screen everything
    python3 tools/proto_screen.py --raw out.txt   # also dump the raw log
    python3 tools/proto_screen.py --jobs 8
"""
import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def unescape(value):
    value = re.sub(r"\$\n\s*", " ", value)
    return value.replace("$ ", " ").replace("$:", ":").replace("$$", "$").strip()


def parse_ninja(path):
    """Yield (output, rule, source, vars) for every mwcc* edge building a .c."""
    lines = open(path).read().split("\n")
    edges = []
    i = 0
    while i < len(lines):
        stmt = lines[i]
        if not stmt.startswith("build "):
            i += 1
            continue
        while stmt.endswith("$"):
            i += 1
            stmt = stmt[:-1] + lines[i]
        i += 1
        match = re.match(r"^build ([^:]+):\s+(mwcc\w*)\s+(.*)$", stmt)
        variables = {}
        while i < len(lines) and lines[i].startswith("  "):
            vstmt = lines[i]
            while vstmt.endswith("$"):
                i += 1
                vstmt = vstmt[:-1] + lines[i]
            i += 1
            vmatch = re.match(r"^\s*(\w+) = (.*)$", vstmt)
            if vmatch:
                variables[vmatch.group(1)] = unescape(vmatch.group(2))
        if not match:
            continue
        sources = unescape(match.group(3)).split("|")[0].split()
        # every C-shaped mwcc edge, .cpp included: the build compiles one
        # C++ unit (Runtime.PPCEABI.H/__init_cpp_exceptions.cpp) and a .c-only
        # filter silently dropped its prototypes from the screen.  The .s
        # units use the `as` rule and never reach this regex.
        if not sources or not sources[0].endswith((".c", ".cp", ".cpp")):
            continue
        edges.append((unescape(match.group(1)), match.group(2), sources[0], variables))
    return edges


def compile_with_warnings(edge):
    _, rule, source, variables = edge
    version = variables.get("mw_version")
    if not version:
        return source, ""
    argv = []
    if os.name != "nt":
        argv.append(os.path.join(ROOT, "build/tools/wibo"))
    if "sjis" in rule:
        argv.append(os.path.join(ROOT, "build/tools/sjiswrap.exe"))
    argv.append(os.path.join(ROOT, "build/compilers", version, "mwcceppc.exe"))
    flags = shlex.split(variables.get("cflags", ""), posix=os.name != "nt")
    if os.name == "nt":
        flags = [flag[1:-1] if len(flag) >= 2 and flag[0] == flag[-1] == '"'
                 else flag for flag in flags]
    kept = []
    j = 0
    while j < len(flags):
        if flags[j] == "-maxerrors":
            j += 2
            continue
        kept.append(flags[j])
        j += 1
    output = os.devnull
    temp_output = None
    if os.name == "nt":
        handle, temp_output = tempfile.mkstemp(suffix=".o")
        os.close(handle)
        os.unlink(temp_output)
        output = temp_output
    argv += kept + ["-maxerrors", "400", "-W", "all", "-c", source, "-o", output]
    try:
        done = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                              timeout=900, errors="replace")
    except subprocess.TimeoutExpired:
        return source, "TIMEOUT"
    finally:
        if temp_output and os.path.exists(temp_output):
            os.unlink(temp_output)
    return source, (done.stdout or "") + (done.stderr or "")


HIT_RE = re.compile(
    r"^#\s+(\d+): (.*)\n#   Error: ( *)(\^+)\n#   function has no prototype", re.M)

PROTO_RE = re.compile(
    r"(?:^|[;{}\n])\s*(?:extern\s+)?"
    r"((?:const\s+|unsigned\s+|signed\s+|struct\s+|union\s+|enum\s+)*"
    r"[A-Za-z_]\w*\s*\**)\s*"
    r"([A-Za-z_]\w*)\s*\(([^;{)]*(?:\([^)]*\)[^;{)]*)*)\)\s*;")

INT_WIDTH = re.compile(
    r"^(const\s+)?(void|int|long|unsigned|unsigned int|unsigned long|signed|"
    r"u8|s8|u16|s16|u32|s32|size_t|char|short|BOOL|\w+\s*\*+)$")
FLOATING = re.compile(r"\b(f32|f64|float|double)\b")
NARROW_VALUE = re.compile(
    r"(?:^|,)\s*(?:const\s+)?"
    r"(?:u8|s8|u16|s16|char|short|unsigned char|signed char|"
    r"unsigned short|signed short)\b(?!\s*\*)")


def collect_hits(log):
    hits = []
    for match in HIT_RE.finditer(log):
        code = match.group(2)
        column = len(match.group(3))
        name = re.match(r"[A-Za-z_]\w*", code[column:])
        if name:
            hits.append((int(match.group(1)), name.group(0), code.strip()))
    return hits


def collect_prototypes():
    protos = collections.defaultdict(set)
    where = collections.defaultdict(set)
    for dirpath, _, filenames in os.walk(os.path.join(ROOT, "include")):
        for filename in filenames:
            if not filename.endswith(".h"):
                continue
            path = os.path.join(dirpath, filename)
            text = open(path, errors="replace").read()
            for match in PROTO_RE.finditer(text):
                name = match.group(2)
                if name in ("if", "while", "for", "switch", "return", "sizeof",
                            "defined", "else", "do"):
                    continue
                protos[name].add((" ".join(match.group(1).split()),
                                  " ".join(match.group(3).split())))
                where[name].add(os.path.relpath(path, ROOT))
    return protos, where


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=10)
    parser.add_argument("--raw", help="write the full MWCC log here")
    parser.add_argument("--json", help="write the hit list here")
    args = parser.parse_args()

    edges = parse_ninja(os.path.join(ROOT, "build.ninja"))
    seen = set()
    unique = []
    for edge in edges:
        if edge[0] in seen:
            continue
        seen.add(edge[0])
        unique.append(edge)
    sys.stderr.write("screening %d units\n" % len(unique))

    logs = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for count, result in enumerate(pool.map(compile_with_warnings, unique)):
            logs.append(result)
            if count % 100 == 0:
                sys.stderr.write("  %d\n" % count)

    if args.raw:
        with open(args.raw, "w") as handle:
            for source, log in logs:
                if log.strip():
                    handle.write("### %s\n%s\n" % (source, log))

    hits = []
    for source, log in logs:
        for line, name, code in collect_hits(log):
            hits.append({"unit": source, "line": line, "name": name, "code": code})
    if args.json:
        json.dump(hits, open(args.json, "w"), indent=1)

    protos, where = collect_prototypes()
    by_name = collections.defaultdict(list)
    for hit in hits:
        by_name[hit["name"]].append(hit)

    bad_return, float_param, narrow_param, unknown = [], [], [], []
    for name, group in sorted(by_name.items()):
        found = protos.get(name)
        if not found:
            unknown.append((name, group))
            continue
        returns = sorted({p[0] for p in found})
        arglists = sorted({p[1] for p in found})
        if any(FLOATING.search(r) for r in returns) or \
                not any(INT_WIDTH.match(r) for r in returns):
            bad_return.append((name, returns, arglists, group))
        if any(FLOATING.search(a) for a in arglists):
            float_param.append((name, returns, arglists, group))
        if any(NARROW_VALUE.search(a) for a in arglists):
            narrow_param.append((name, returns, arglists, group))

    print("implicit-declaration call sites: %d in %d units, %d distinct callees"
          % (len(hits), len({h["unit"] for h in hits}), len(by_name)))

    def dump(title, rows):
        print("\n" + "=" * 78)
        print(title)
        print("=" * 78)
        if not rows:
            print("  (none)")
        for name, returns, arglists, group in rows:
            print("%-38s ret=%s" % (name, returns))
            print("      args:    %s" % (arglists,))
            print("      declared in: %s" % sorted(where[name])[:4])
            for hit in group:
                print("   >> %s:%d  %s" % (hit["unit"], hit["line"], hit["code"][:110]))

    dump("A. RETURN TYPE IS NOT INT-WIDTH (the implicit 'int' return is wrong)",
         bad_return)
    dump("B. CALLEE HAS A FLOATING PARAMETER (an integer actual lands in a GPR)",
         float_param)
    dump("C. CALLEE HAS A NARROW INTEGER VALUE PARAMETER (conversion may differ)",
         narrow_param)

    print("\n" + "=" * 78)
    print("D. NO PROTOTYPE ANYWHERE IN include/: %d" % len(unknown))
    print("=" * 78)
    for name, group in unknown:
        print("   %-36s %s:%d" % (name, group[0]["unit"], group[0]["line"]))

    print("\nper-unit counts")
    for unit, count in collections.Counter(h["unit"] for h in hits).most_common():
        print("%5d %s" % (count, unit))


if __name__ == "__main__":
    main()
