#!/usr/bin/env python3
"""Disassemble an address range from a version's DOL. Usage:
   python3 disasm.py VER 0xADDR 0xENDADDR"""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdiff import parse_dol, ISOS

OBJDUMP = "/Users/jackpriceburns/Code/sfa/build/binutils/powerpc-eabi-objdump"

def disasm(ver, lo, hi):
    secs = parse_dol(os.path.join(ISOS, ver, "sys", "main.dol"))
    for key, (addr, size, data) in secs.items():
        if addr <= lo < addr + size:
            chunk = data[lo-addr:hi-addr]
            tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"xd_{ver}_{lo:x}.bin")
            with open(tmp, "wb") as f:
                f.write(chunk)
            out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "powerpc",
                                  "-M", "gekko", "-EB", f"--adjust-vma=0x{lo:x}", tmp],
                                 capture_output=True, text=True)
            lines = [l for l in out.stdout.splitlines() if "\t" in l]
            return "\n".join(lines) + (("\nSTDERR: " + out.stderr) if out.stderr.strip() else "")
    return "address not in any section"

if __name__ == "__main__":
    ver = sys.argv[1]
    lo = int(sys.argv[2], 16)
    hi = int(sys.argv[3], 16)
    print(disasm(ver, lo, hi))
