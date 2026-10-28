#!/usr/bin/env python3
"""Sound object fingerprint: disasm + allocatable section bytes, with @N normalized.

Usage: objfp.py <obj> [<obj> ...]        -> print sha1 per object
       objfp.py --save <tag> <obj>...    -> store fingerprints under .objfp/<tag>
       objfp.py --cmp <tag> <obj>...     -> compare against stored
"""
import hashlib
import os
import re
import subprocess
import sys

OBJDUMP = "build/binutils/powerpc-eabi-objdump"
SECS = [".text", ".data", ".rodata", ".sdata", ".sdata2", ".sbss", ".sbss2", ".bss",
        ".ctors", ".dtors", ".init", ".fini", ".rodata1", ".data1"]
AT = re.compile(rb"@\d+")


def fp(obj):
    args = [OBJDUMP, "-M", "gekko", "-drz", obj]
    a = subprocess.run(args, capture_output=True).stdout
    args2 = [OBJDUMP, "-s"] + sum([["-j", s] for s in SECS], []) + [obj]
    b = subprocess.run(args2, capture_output=True).stdout
    blob = AT.sub(b"@N", a) + b"\n---\n" + AT.sub(b"@N", b)
    # strip the leading "file format" / path line which contains the obj name
    blob = b"\n".join(l for l in blob.split(b"\n") if b"file format" not in l)
    return hashlib.sha1(blob).hexdigest()


def main():
    args = sys.argv[1:]
    mode, tag = None, None
    if args and args[0] in ("--save", "--cmp"):
        mode, tag, args = args[0], args[1], args[2:]
    d = os.path.join(".objfp", tag) if tag else None
    if mode == "--save":
        os.makedirs(d, exist_ok=True)
    bad = 0
    for o in args:
        h = fp(o)
        key = o.replace("/", "_")
        if mode == "--save":
            open(os.path.join(d, key), "w").write(h)
            print(f"SAVE {h}  {o}")
        elif mode == "--cmp":
            p = os.path.join(d, key)
            if not os.path.exists(p):
                print(f"MISS ????  {o}")
                bad += 1
                continue
            old = open(p).read().strip()
            ok = old == h
            bad += 0 if ok else 1
            print(f"{'SAME' if ok else 'DIFF'} {h}  {o}")
        else:
            print(f"{h}  {o}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
