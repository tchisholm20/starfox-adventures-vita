#!/usr/bin/env python3
"""Alignment-aware cross-version DOL section diff for SFA.

Normalizes PPC instruction words (masking branch displacements and
address-bearing immediates), builds unique-window anchors between the two
versions, extracts a monotonic alignment (LIS), and reports change regions
(replacements / insertions / deletions) with addresses on both sides.

Usage:
  python3 xdiff.py US10 US11 [--section text1] [--norm code|data|raw]
                   [--map-config GSAE01[:A|B]] [--json out.json] [-K 8]
"""
import argparse, json, os, re, struct, sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISOS = os.path.join(BASE, "isos")
REPO = "/Users/jackpriceburns/Code/sfa"

SEC_NAMES = {  # DOL section index -> conventional name (SFA layout)
    ("text", 0): ".init", ("text", 1): ".text",
    ("data", 7): "extab", ("data", 8): "extabindex",
    ("data", 9): ".ctors", ("data", 10): ".dtors",
    ("data", 11): ".rodata", ("data", 12): ".data",
    ("data", 13): ".sdata", ("data", 14): ".sdata2",
}

def parse_dol(path):
    with open(path, "rb") as f:
        raw = f.read()
    offs = struct.unpack(">18I", raw[0:0x48])
    addrs = struct.unpack(">18I", raw[0x48:0x90])
    sizes = struct.unpack(">18I", raw[0x90:0xD8])
    secs = {}
    for i in range(18):
        if sizes[i]:
            kind = "text" if i < 7 else "data"
            key = f"{kind}{i}" if kind == "text" else f"data{i}"
            secs[key] = (addrs[i], sizes[i], raw[offs[i]:offs[i]+sizes[i]])
    return secs

# ---------------- normalization ----------------

def norm_code(data):
    n = len(data) // 4
    words = struct.unpack(f">{n}I", data[:4*n])
    out = bytearray(4*n)
    pack_into = struct.pack_into
    for idx in range(n):
        w = words[idx]
        op = w >> 26
        if op == 18:                      # b/bl/ba: mask LI
            w &= 0xFC000003
        elif op == 16:                    # bc: mask BD
            w &= 0xFFFF0003
        elif op in (14, 15, 24, 25, 26, 27) or 32 <= op <= 55:
            # addi/addis/ori/oris/xori/xoris + all D-form loads/stores:
            # immediates can carry @ha/@l/@sda address halves
            w &= 0xFFFF0000
        elif op in (56, 57, 60, 61):      # psq_l/lu/st/stu: 12-bit displacement
            w &= 0xFFFFF000
        pack_into(">I", out, 4*idx, w)
    return bytes(out)

def norm_data(data):
    n = len(data) // 4
    words = struct.unpack(f">{n}I", data[:4*n])
    out = bytearray(4*n)
    pack_into = struct.pack_into
    for idx in range(n):
        w = words[idx]
        if 0x80003000 <= w < 0x803F0000:  # plausible pointer into the image
            w = 0
        pack_into(">I", out, 4*idx, w)
    return bytes(out)

def norm_raw(data):
    n = len(data) // 4
    return bytes(data[:4*n])

NORMS = {"code": norm_code, "data": norm_data, "raw": norm_raw}

# ---------------- anchors & alignment ----------------

def window_positions(nb, K):
    d = {}
    n = len(nb) // 4
    for i in range(n - K + 1):
        key = nb[4*i:4*(i+K)]
        if key in d:
            d[key] = -1
        else:
            d[key] = i
    return d

def lis_anchors(anchors):
    """anchors sorted by i; keep longest strictly-increasing subsequence in j."""
    import bisect
    tails = []      # tails[k] = smallest j ending an inc-subseq of length k+1
    tidx = []       # index into anchors for tails
    prev = [-1] * len(anchors)
    for idx, (i, j) in enumerate(anchors):
        p = bisect.bisect_left(tails, j)
        if p == len(tails):
            tails.append(j); tidx.append(idx)
        else:
            tails[p] = j; tidx[p] = idx
        prev[idx] = tidx[p-1] if p > 0 else -1
    out = []
    k = tidx[-1] if tidx else -1
    while k != -1:
        out.append(anchors[k]); k = prev[k]
    out.reverse()
    return out

def find_regions(na, nb, K=8):
    """Return list of (a_lo, a_hi, b_lo, b_hi) word-index change regions."""
    nA, nB = len(na)//4, len(nb)//4
    da = window_positions(na, K)
    db = window_positions(nb, K)
    anchors = []
    for key, ia in da.items():
        if ia < 0:
            continue
        jb = db.get(key, -1)
        if jb >= 0:
            anchors.append((ia, jb))
    anchors.sort()
    anchors = lis_anchors(anchors)
    if not anchors:
        return [(0, nA, 0, nB)]

    # group anchors into constant-delta runs; run span = [first_i, last_i+K)
    runs = []
    ci, cj = anchors[0]
    run_start = ci
    last_i = ci
    cur_d = cj - ci
    for i, j in anchors[1:]:
        d = j - i
        if d == cur_d and i <= last_i + K:  # contiguous-ish coverage
            last_i = i
        else:
            runs.append((run_start, last_i + K, cur_d))
            run_start, last_i, cur_d = i, i, d
    runs.append((run_start, last_i + K, cur_d))

    regions = []

    def wa(i):  # word at A idx
        return na[4*i:4*i+4]
    def wb(j):
        return nb[4*j:4*j+4]

    def scan_same_delta(lo, hi, d):
        """find mismatch intervals inside [lo,hi) under delta d"""
        if na[4*lo:4*hi] == nb[4*(lo+d):4*(hi+d)]:
            return
        x = lo
        while x < hi:
            if wa(x) == wb(x + d):
                x += 1
                continue
            y = x
            while y < hi and wa(y) != wb(y + d):
                y += 1
            regions.append((x, y, x + d, y + d))
            x = y

    # verify run interiors (same-length replacements hide inside runs)
    for lo, hi, d in runs:
        scan_same_delta(lo, hi, d)

    def refine_gap(a_lo, a_hi, b_lo, b_hi):
        """trim matching prefix (left delta) and suffix (right delta)"""
        if a_hi < a_lo:          # anchor runs overlap in A: pure insertion
            excess = a_lo - a_hi
            a_lo -= excess; b_lo -= excess
        if b_hi < b_lo:          # anchor runs overlap in B: pure deletion
            excess = b_lo - b_hi
            b_lo -= excess; a_lo -= excess
        while a_lo < a_hi and b_lo < b_hi and wa(a_lo) == wb(b_lo):
            a_lo += 1; b_lo += 1
        while a_hi > a_lo and b_hi > b_lo and wa(a_hi-1) == wb(b_hi-1):
            a_hi -= 1; b_hi -= 1
        if a_lo != a_hi or b_lo != b_hi:
            regions.append((a_lo, a_hi, b_lo, b_hi))

    # head
    f_lo, f_hi, f_d = runs[0]
    refine_gap(0, f_lo, 0, f_lo + f_d)
    # between runs
    for (lo1, hi1, d1), (lo2, hi2, d2) in zip(runs, runs[1:]):
        refine_gap(hi1, lo2, hi1 + d1, lo2 + d2)
    # tail
    l_lo, l_hi, l_d = runs[-1]
    refine_gap(l_hi, nA, l_hi + l_d, nB)

    regions.sort()
    # merge overlapping/adjacent
    merged = []
    for r in regions:
        if merged and r[0] <= merged[-1][1] and r[2] <= merged[-1][3]:
            m = merged[-1]
            merged[-1] = (m[0], max(m[1], r[1]), m[2], max(m[3], r[3]))
        else:
            merged.append(r)
    return merged

# ---------------- TU / symbol mapping ----------------

def load_splits(cfg):
    """{secname: [(start,end,unit)]} sorted"""
    path = os.path.join(REPO, "config", cfg, "splits.txt")
    out = {}
    unit = None
    for line in open(path):
        m = re.match(r"^(\S+):\s*$", line)
        if m and "/" in m.group(1):
            unit = m.group(1); continue
        m = re.match(r"\s+(\S+)\s+start:(0x[0-9A-Fa-f]+)\s+end:(0x[0-9A-Fa-f]+)", line)
        if m and unit:
            out.setdefault(m.group(1), []).append((int(m.group(2), 16), int(m.group(3), 16), unit))
    for v in out.values():
        v.sort()
    return out

def load_symbols(cfg):
    """{secname: [(addr,size,name)]} sorted"""
    path = os.path.join(REPO, "config", cfg, "symbols.txt")
    out = {}
    rx = re.compile(r"^(\S+) = (\.\w+|\w+):(0x[0-9A-Fa-f]+);(?: // (.*))?")
    for line in open(path):
        m = rx.match(line)
        if not m:
            continue
        name, sec, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4) or ""
        sm = re.search(r"size:(0x[0-9A-Fa-f]+)", rest)
        size = int(sm.group(1), 16) if sm else 0
        out.setdefault(sec, []).append((addr, size, name))
    for v in out.values():
        v.sort()
    return out

def lookup(sorted_list, addr, lo_i=0):
    """last entry with start <= addr"""
    import bisect
    i = bisect.bisect_right(sorted_list, (addr, float("inf"), "￿")) - 1
    return sorted_list[i] if i >= 0 else None

# ---------------- driver ----------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va"); ap.add_argument("vb")
    ap.add_argument("--section", default="text1")
    ap.add_argument("--norm", default=None, choices=[None, "code", "data", "raw"])
    ap.add_argument("-K", type=int, default=8)
    ap.add_argument("--map-config", default=None, help="config dir, optionally :A or :B for which side to map (default A)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    sa = parse_dol(os.path.join(ISOS, args.va, "sys", "main.dol"))
    sb = parse_dol(os.path.join(ISOS, args.vb, "sys", "main.dol"))
    addr_a, size_a, data_a = sa[args.section]
    addr_b, size_b, data_b = sb[args.section]
    secname = SEC_NAMES.get(("text" if args.section.startswith("text") else "data",
                             int(args.section.replace("text", "").replace("data", ""))),
                            args.section)
    norm = args.norm or ("code" if args.section.startswith("text") else "data")
    na = NORMS[norm](data_a)
    nb = NORMS[norm](data_b)

    regions = find_regions(na, nb, K=args.K)

    splits = symbols = None
    map_side = "A"
    if args.map_config:
        cfg = args.map_config
        if ":" in cfg:
            cfg, map_side = cfg.split(":")
        splits = load_splits(cfg).get(secname, [])
        symbols = load_symbols(cfg).get(secname, [])

    print(f"# {args.va} ({secname} @ {addr_a:08X}+{size_a:#x}) vs {args.vb} ({addr_b:08X}+{size_b:#x})  norm={norm} K={args.K}")
    print(f"# section size delta {size_b - size_a:+#x}")
    total_delta = 0
    out_rows = []
    for a_lo, a_hi, b_lo, b_hi in regions:
        aa0, aa1 = addr_a + 4*a_lo, addr_a + 4*a_hi
        ba0, ba1 = addr_b + 4*b_lo, addr_b + 4*b_hi
        alen, blen = aa1 - aa0, ba1 - ba0
        total_delta += blen - alen
        if alen == 0:
            kind = "INSERT"
        elif blen == 0:
            kind = "DELETE"
        elif alen == blen:
            kind = "REPLACE"
        else:
            kind = "RESIZE"
        row = dict(kind=kind, a_start=aa0, a_end=aa1, b_start=ba0, b_end=ba1,
                   a_len=alen, b_len=blen)
        if splits is not None:
            side_start, side_end = (aa0, aa1) if map_side == "A" else (ba0, ba1)
            probe_end = max(side_start, side_end - 4)
            u1 = lookup(splits, side_start)
            u2 = lookup(splits, probe_end)
            s1 = lookup(symbols, side_start)
            s2 = lookup(symbols, probe_end)
            row["unit"] = u1[2] if u1 and side_start < u1[1] else "?"
            row["unit_end"] = u2[2] if u2 and probe_end < u2[1] else "?"
            row["sym"] = f"{s1[2]}+{side_start - s1[0]:#x}" if s1 else "?"
            row["sym_end"] = f"{s2[2]}+{probe_end - s2[0]:#x}" if s2 else "?"
        out_rows.append(row)
        loc = ""
        if splits is not None:
            straddle = " *** STRADDLES TU BOUNDARY ***" if row["unit"] != row["unit_end"] else ""
            loc = f"  {row['unit']}  {row['sym']}" + (f" .. {row['sym_end']}" if row["sym"].split("+")[0] != row["sym_end"].split("+")[0] else "") + straddle
        print(f"{kind:8s} A:{aa0:08X}-{aa1:08X} ({alen:#7x})  B:{ba0:08X}-{ba1:08X} ({blen:#7x}){loc}")
    print(f"# regions={len(out_rows)}  net delta from regions {total_delta:+#x} (section delta {size_b-size_a:+#x}) {'OK' if total_delta == size_b-size_a else 'MISMATCH!'}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(va=args.va, vb=args.vb, section=secname, norm=norm,
                           addr_a=addr_a, addr_b=addr_b, regions=out_rows), f, indent=1)

if __name__ == "__main__":
    main()
