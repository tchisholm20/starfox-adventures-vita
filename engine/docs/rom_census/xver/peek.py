#!/usr/bin/env python3
"""Dump words from a version's DOL with symbol annotation from a config.
Usage: inspect.py VER 0xLO 0xHI [CONFIG]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdiff import parse_dol, load_symbols, lookup, ISOS
import struct

def dump(ver, lo, hi, cfg=None):
    secs = parse_dol(os.path.join(ISOS, ver, "sys", "main.dol"))
    syms = {}
    allsyms = []
    if cfg:
        syms = load_symbols(cfg)
        for sec, lst in syms.items():
            allsyms.extend(lst)
        allsyms.sort()
    for key, (addr, size, data) in secs.items():
        if addr <= lo < addr + size:
            for a in range(lo, min(hi, addr + size), 4):
                w = struct.unpack(">I", data[a-addr:a-addr+4])[0]
                note = ""
                if cfg:
                    for sec, lst in syms.items():
                        hit = [s for s in lst if s[0] == a]
                        if hit:
                            note += f"  <== {hit[0][2]}"
                    if 0x80003000 <= w < 0x803F0000:
                        t = lookup(allsyms, w)
                        if t:
                            note += f"   -> {t[2]}+{w-t[0]:#x}"
                try:
                    txt = data[a-addr:a-addr+4].decode("ascii")
                    txt = "".join(c if 32 <= ord(c) < 127 else "." for c in txt)
                except Exception:
                    txt = "...."
                print(f"{a:08X}: {w:08X}  {txt}{note}")
            return
    print("not found")

if __name__ == "__main__":
    dump(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16),
         sys.argv[4] if len(sys.argv) > 4 else None)
