#!/usr/bin/env python3
"""Parse DOL headers and print section tables for the five SFA versions."""
import struct, sys, os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISOS = os.path.join(BASE, "isos")
VERS = ["US10", "US11", "EU10", "EU11", "JP10"]

def parse(path):
    with open(path, "rb") as f:
        hdr = f.read(0x100)
    offs = struct.unpack(">18I", hdr[0:0x48])
    addrs = struct.unpack(">18I", hdr[0x48:0x90])
    sizes = struct.unpack(">18I", hdr[0x90:0xD8])
    bss_addr, bss_size, entry = struct.unpack(">3I", hdr[0xD8:0xE4])
    secs = []
    for i in range(18):
        if sizes[i]:
            kind = "text" if i < 7 else "data"
            secs.append((kind, i, addrs[i], sizes[i], offs[i]))
    return secs, bss_addr, bss_size, entry

def main():
    data = {}
    for v in VERS:
        p = os.path.join(ISOS, v, "sys", "main.dol")
        data[v] = parse(p)
    for v in VERS:
        secs, ba, bs, e = data[v]
        print(f"=== {v}  entry=0x{e:08X}  bss=0x{ba:08X}+0x{bs:X}  filesize={os.path.getsize(os.path.join(ISOS,v,'sys','main.dol'))}")
        for kind, i, a, s, o in secs:
            print(f"  {kind}{i:2d}  addr=0x{a:08X}  size=0x{s:<8X} end=0x{a+s:08X}  off=0x{o:X}")
    # cross-version comparison per section index
    print("\n=== per-section size deltas (vs US10) ===")
    ref = {(k, i): (a, s) for k, i, a, s, o in data["US10"][0]}
    for v in VERS[1:]:
        print(f"--- {v}")
        for kind, i, a, s, o in data[v][0]:
            ra, rs = ref.get((kind, i), (0, 0))
            print(f"  {kind}{i:2d}  addr {ra:08X}->{a:08X} ({a-ra:+#x})  size {rs:#x}->{s:#x} ({s-rs:+#x})")

if __name__ == "__main__":
    main()
