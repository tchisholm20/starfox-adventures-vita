#!/usr/bin/env python3
"""Name-agnostic symbol reader census.

Decodes every D-form memory access in the linked DOL image, resolving r2/r13
small-data displacements and lis+lo address pairs to absolute addresses, then
attributes each access to the symbols.txt symbol whose [addr, addr+size) range
contains it.

The census keys on section, size and access opcode/width only -- never on the
shape of a symbol name -- so retail names such as ``Prepared_803DEAD8`` are
treated exactly like ``lbl_*`` or a hand-written identifier.

Usage:
    python3 tools/reader_census.py --section .sdata2 --no-readers
    python3 tools/reader_census.py --width-conflicts
"""

import argparse
import bisect
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(ROOT, "config", "GSAE01", "symbols.txt")
ELF = os.path.join(ROOT, "build", "GSAE01", "main.elf")
NM = os.path.join(ROOT, "build", "binutils", "powerpc-eabi-nm")
OBJDUMP = os.path.join(ROOT, "build", "binutils", "powerpc-eabi-objdump")

SYM_RE = re.compile(
    r"\s*([A-Za-z_\$][\w\$\.@]*)\s*=\s*([\w\.]+):0x([0-9A-Fa-f]+);\s*(.*)")

# opcode -> (mnemonic, width in bytes, kind)
#   kind: 'i' integer, 'f' float, 'a' address-only (addi/addis/ori)
DFORM = {
    32: ("lwz", 4, "i"), 33: ("lwzu", 4, "i"),
    34: ("lbz", 1, "i"), 35: ("lbzu", 1, "i"),
    36: ("stw", 4, "i"), 37: ("stwu", 4, "i"),
    38: ("stb", 1, "i"), 39: ("stbu", 1, "i"),
    40: ("lhz", 2, "i"), 41: ("lhzu", 2, "i"),
    42: ("lha", 2, "i"), 43: ("lhau", 2, "i"),
    44: ("sth", 2, "i"), 45: ("sthu", 2, "i"),
    46: ("lmw", 4, "i"), 47: ("stmw", 4, "i"),
    48: ("lfs", 4, "f"), 49: ("lfsu", 4, "f"),
    50: ("lfd", 8, "f"), 51: ("lfdu", 8, "f"),
    52: ("stfs", 4, "f"), 53: ("stfsu", 4, "f"),
    54: ("stfd", 8, "f"), 55: ("stfdu", 8, "f"),
}
STORES = {"stw", "stwu", "stb", "stbu", "sth", "sthu", "stmw",
          "stfs", "stfsu", "stfd", "stfdu", "psq_st", "psq_stu"}
# gekko paired-single: 12-bit displacement
PSFORM = {56: ("psq_l", 8), 57: ("psq_lu", 8),
          60: ("psq_st", 8), 61: ("psq_stu", 8)}


def parse_symbols(path=SYMBOLS):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = SYM_RE.match(line)
        if not m:
            continue
        name, sect, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        size = re.search(r"size:0x([0-9A-Fa-f]+)", rest)
        align = re.search(r"align:(\d+)", rest)
        data = re.search(r"data:(\w+)", rest)
        scope = re.search(r"scope:(\w+)", rest)
        out.append({
            "name": name, "section": sect, "addr": addr,
            "size": int(size.group(1), 16) if size else 0,
            "align": int(align.group(1)) if align else None,
            "data": data.group(1) if data else None,
            "scope": scope.group(1) if scope else None,
            "line": line.rstrip("\n"),
        })
    out.sort(key=lambda s: (s["addr"], s["section"]))
    return out


def sda_bases():
    out = subprocess.run([NM, ELF], capture_output=True, text=True).stdout
    bases = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("_SDA_BASE_", "_SDA2_BASE_"):
            bases[parts[2]] = int(parts[0], 16)
    return bases["_SDA_BASE_"], bases["_SDA2_BASE_"]


def elf_code_sections():
    """Yield (vma, bytes) for every executable section of the linked image."""
    hdr = subprocess.run([OBJDUMP, "-h", ELF], capture_output=True, text=True).stdout
    secs = []
    lines = hdr.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if not m:
            continue
        flags = lines[i + 1] if i + 1 < len(lines) else ""
        if "CODE" not in flags:
            continue
        secs.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(5), 16)))
    blob = open(ELF, "rb").read()
    for name, size, vma, off in secs:
        yield name, vma, blob[off:off + size]


def scan(sda, sda2):
    """Return {addr: set((mnemonic, width, kind))} over all decoded accesses."""
    hits = {}

    def record(addr, mn, width, kind):
        hits.setdefault(addr, set()).add((mn, width, kind))

    for _name, vma, data in elf_code_sections():
        high = {}  # reg -> high 16 bits already shifted
        for off in range(0, len(data) & ~3, 4):
            insn = struct.unpack_from(">I", data, off)[0]
            op = insn >> 26
            ra = (insn >> 16) & 31
            rd = (insn >> 21) & 31
            simm = insn & 0xFFFF
            if simm >= 0x8000:
                simm -= 0x10000
            if op == 15:  # addis
                if ra == 0:
                    high[rd] = (insn & 0xFFFF) << 16
                else:
                    high.pop(rd, None)
                continue
            if op in DFORM:
                mn, width, kind = DFORM[op]
                base = None
                if ra == 2:
                    base = sda2
                elif ra == 13:
                    base = sda
                elif ra in high:
                    base = high[ra]
                if base is not None:
                    record(base + simm, mn, width, kind)
                # update forms clobber rA; loads clobber rD
                if mn.endswith("u"):
                    high.pop(ra, None)
                if kind == "i" and not mn.startswith("st"):
                    high.pop(rd, None)
                continue
            if op in PSFORM:
                mn, width = PSFORM[op]
                d = insn & 0xFFF
                if d >= 0x800:
                    d -= 0x1000
                base = None
                if ra == 2:
                    base = sda2
                elif ra == 13:
                    base = sda
                elif ra in high:
                    base = high[ra]
                if base is not None:
                    record(base + d, mn, width, "f")
                continue
            if op == 14:  # addi / li
                base = None
                if ra == 2:
                    base = sda2
                elif ra == 13:
                    base = sda
                elif ra in high:
                    base = high[ra]
                if base is not None and ra != 0:
                    record(base + simm, "addi", 0, "a")
                    high[rd] = base + simm
                else:
                    high.pop(rd, None)
                continue
            if op == 24:  # ori
                if ra in high:
                    high[rd] = high[ra] | (insn & 0xFFFF)
                    record(high[rd], "ori", 0, "a")
                else:
                    high.pop(rd, None)
                continue
            # any other instruction: conservatively kill its destination
            if op in (7, 8, 12, 13, 28, 29):
                high.pop(rd, None)
            elif op == 31:
                high.pop(ra if (insn >> 1) & 0x3FF in (444, 124, 24, 536, 28) else rd, None)
                high.pop(rd, None)
            elif op in (16, 18, 19):  # branches invalidate everything
                high.clear()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--section", default=None)
    ap.add_argument("--no-readers", action="store_true")
    ap.add_argument("--width-conflicts", action="store_true")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--addr", default=None)
    args = ap.parse_args()

    syms = parse_symbols()
    sda, sda2 = sda_bases()
    hits = scan(sda, sda2)
    addrs = sorted(hits)

    # attribute each access to the containing symbol
    starts = [s["addr"] for s in syms]
    for s in syms:
        s["acc"] = set()
    for a in addrs:
        i = bisect.bisect_right(starts, a) - 1
        while i >= 0:
            s = syms[i]
            if s["addr"] + max(s["size"], 1) > a:
                s["acc"] |= hits[a]
                break
            if starts[i] != starts[max(i - 1, 0)]:
                break
            i -= 1

    sel = [s for s in syms if args.section is None or s["section"] == args.section]

    if args.addr:
        want = int(args.addr, 16)
        for s in sel:
            if s["addr"] == want:
                print(s["line"])
                print("  accesses:", sorted(s["acc"]) or "NONE")
        return

    if args.summary:
        tot = len(sel)
        withr = sum(1 for s in sel if s["acc"])
        print(f"section={args.section or 'ALL'} total={tot} with_reader={withr} "
              f"no_reader={tot - withr}")
        return

    if args.no_readers:
        for s in sel:
            if not s["acc"]:
                print(f"{s['addr']:08X} {s['section']:9s} size=0x{s['size']:X} "
                      f"data={s['data']} scope={s['scope']} {s['name']}")
        return

    if args.width_conflicts:
        declared = {"byte": 1, "2byte": 2, "4byte": 4, "float": 4, "double": 8,
                    "string": 1, "string_table": 1}
        for s in sel:
            if not s["acc"]:
                continue
            d = declared.get(s["data"] or "", None)
            if d is None:
                continue
            widths = {w for (_m, w, _k) in s["acc"] if w}
            kinds = {k for (_m, _w, k) in s["acc"] if k != "a"}
            bad = []
            if widths and d not in widths:
                bad.append(f"declared {s['data']}({d}B) vs access widths {sorted(widths)}")
            if s["data"] in ("float", "double") and kinds == {"i"}:
                bad.append("declared float but only integer accesses")
            if s["data"] in ("4byte", "2byte", "byte") and kinds == {"f"}:
                bad.append(f"declared {s['data']} but only float accesses")
            if bad:
                print(f"{s['addr']:08X} {s['section']:9s} size=0x{s['size']:X} "
                      f"{s['name']}: {'; '.join(bad)} "
                      f"[{','.join(sorted(m for m, _w, _k in s['acc']))}]")
        return

    for s in sel:
        print(f"{s['addr']:08X} {s['name']} {sorted(s['acc'])}")


if __name__ == "__main__":
    sys.exit(main())
