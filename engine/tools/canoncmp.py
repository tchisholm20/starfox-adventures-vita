#!/usr/bin/env python3
"""Canonical .o comparator v2.

Verdicts:
  IDENTICAL   raw md5 equal
  NEUT_ATN    all section contents identical; relocs identical after @N resolution
  NEUT_META   all sections EXCEPT .comment identical; relocs canonical-identical;
              symbol multiset identical (order-insensitive, @N canonicalized)
  DIFF        anything else
"""
import re
import subprocess
import sys
import hashlib

OBJDUMP = "build/binutils/powerpc-eabi-objdump"


def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def run(args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def contents(path):
    out = run([OBJDUMP, "-s", path])
    data = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r"Contents of section (\S+):", line)
        if m:
            cur = m.group(1)
            data[cur] = []
            continue
        if cur and re.match(r"^ [0-9a-f]+ ", line):
            hexpart = line[1:].split("  ")[0]
            data[cur].append("".join(hexpart.split()[1:5]))
    return {k: "".join(v) for k, v in data.items()}


def symtab(path):
    out = run([OBJDUMP, "-t", path])
    syms = {}
    rows = []
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+)\s+(.*?)\s+(\S+)\s+([0-9a-f]+)\s+(?:\.hidden\s+)?(\S+)\s*$", line)
        if m:
            val, fl, sec, size, name = m.groups()
            syms[name] = (sec, val, size)
            rows.append((name, fl, sec, val, size))
    return syms, rows


def canon_rows(rows, syms):
    res = []
    for name, fl, sec, val, size in rows:
        if re.match(r"^@\d+$", name):
            name = "@ANON"
        res.append((name, fl, sec, val, size))
    return sorted(res)


def relocs(path, syms):
    out = run([OBJDUMP, "-r", path])
    res = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]", line)
        if m:
            cur = m.group(1)
            res[cur] = []
            continue
        m = re.match(r"^([0-9a-f]+)\s+(\S+)\s+(\S+)\s*$", line)
        if m and cur:
            off, typ, tgt = m.groups()
            tm = re.match(r"^(@\d+)([+-].*)?$", tgt)
            if tm:
                name, add = tm.group(1), tm.group(2) or ""
                info = syms.get(name)
                tgt = (f"CANON<{info[0]}:{info[1]}:{info[2]}>" if info else name) + add
            res[cur].append((off, typ, tgt))
    return res


def main():
    a, b = sys.argv[1], sys.argv[2]
    if md5(a) == md5(b):
        print("IDENTICAL")
        return
    ca, cb = contents(a), contents(b)
    noncomment_equal = True
    comment_differs = False
    for k in sorted(set(ca) | set(cb)):
        if ca.get(k) != cb.get(k):
            if k == ".comment":
                comment_differs = True
            else:
                print(f"  section {k} differs")
                noncomment_equal = False
    if not noncomment_equal:
        print("DIFF (contents)")
        sys.exit(1)
    sa, ra_rows = symtab(a)
    sb, rb_rows = symtab(b)
    ra = relocs(a, sa)
    rb = relocs(b, sb)
    if ra != rb:
        for k in sorted(set(ra) | set(rb)):
            la, lb = ra.get(k, []), rb.get(k, [])
            if la != lb:
                for x, y in zip(la, lb):
                    if x != y:
                        print(f"  {k}: {x} vs {y}")
                if len(la) != len(lb):
                    print(f"  {k}: count {len(la)} vs {len(lb)}")
        print("DIFF (relocs)")
        sys.exit(1)
    if canon_rows(ra_rows, sa) != canon_rows(rb_rows, sb):
        seta = set(canon_rows(ra_rows, sa))
        setb = set(canon_rows(rb_rows, sb))
        for x in sorted(seta ^ setb):
            print(f"  sym {'A' if x in seta else 'B'}: {x}")
        print("DIFF (symbols)")
        sys.exit(1)
    if comment_differs:
        print("NEUT_META (.comment/symtab-order only)")
    else:
        print("NEUT_ATN")


if __name__ == "__main__":
    main()
