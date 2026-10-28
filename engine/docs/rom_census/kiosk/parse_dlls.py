#!/usr/bin/env python3
"""Parser for the SFA E3-2002 kiosk demo DLLS.bin (Dinosaur Planet DLL container format).

The container is a bare concatenation of DP-format DLL files (the per-DLL offset
table normally lives in DLLS.tab, which in the kiosk build is truncated to the
4-word bank header + terminator).  We recover DLL boundaries by walking:
parse a header, compute the minimum end (data_offset), then scan 4-byte-aligned
offsets for the next valid header.

DP DLL header (big-endian):
  0x00 u32 header_size
  0x04 u32 data_offset   (rel. to DLL start; 0xFFFFFFFF if none)
  0x08 u32 rodata_offset (rel. to DLL start; 0xFFFFFFFF if none) -> starts with
           GOT (until 0xFFFFFFFE), gp relocs (until 0xFFFFFFFD),
           data relocs (until 0xFFFFFFFF), then actual rodata
  0x0C u16 export_count, 0x0E u16 pad
  0x10 u32 ctor_offset (rel. to end of header)
  0x14 u32 dtor_offset
  0x18 u32 unknown
  0x1C u32 export_offsets[export_count]
  text begins at header_size
"""
import struct, sys, json, re

BIN = "/private/tmp/claude-501/-Users-jackpriceburns-Code-sfa/a9042ea5-4a78-464e-a394-bcba82f15363/scratchpad/isos/KIOSK/files/DLLS.bin"
OUT = "/private/tmp/claude-501/-Users-jackpriceburns-Code-sfa/a9042ea5-4a78-464e-a394-bcba82f15363/scratchpad/kioskdll"

data = open(BIN, "rb").read()
N = len(data)

def u32(o):
    return struct.unpack_from(">I", data, o)[0]

def u16(o):
    return struct.unpack_from(">H", data, o)[0]

def try_header(o):
    """Return parsed header dict if a plausible DP DLL header starts at o, else None."""
    if o + 0x20 > N:
        return None
    hsize = u32(o)
    if hsize < 0x20 or hsize > 0x8000 or hsize & 3:
        return None
    cnt = u16(o + 0xC)
    pad = u16(o + 0xE)
    if pad != 0:
        return None
    # header is exactly 0x20 + 4*cnt (observed invariant across all DLLs)
    if hsize != 0x20 + 4 * cnt:
        return None
    if o + hsize + 8 > N:
        return None
    doff = u32(o + 4)
    roff = u32(o + 8)
    for off in (doff, roff):
        if off != 0xFFFFFFFF:
            if off < hsize or off & 3 or o + off > N:
                return None
    if doff != 0xFFFFFFFF and roff != 0xFFFFFFFF and roff >= doff:
        return None
    ctor = u32(o + 0x10)
    dtor = u32(o + 0x14)
    limit = min(x for x in (doff, roff, 0x400000) if x != 0xFFFFFFFF)
    if ctor != 0xFFFFFFFF and (ctor & 3 or ctor >= limit):
        return None
    if dtor != 0xFFFFFFFF and (dtor & 3 or dtor >= limit):
        return None
    exports = []
    for i in range(cnt):
        e = u32(o + 0x1C + 4 * i)
        if e & 3 or e >= limit + 0x100000:
            return None
        exports.append(e)
    # validate the relocation table if rodata is present
    if roff != 0xFFFFFFFF:
        p = o + roff
        lim = o + doff if doff != 0xFFFFFFFF else N
        n_lists = 0
        sentinels = (0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFF)
        for s in sentinels:
            cnt2 = 0
            while p < lim and u32(p) != s:
                v = u32(p)
                ok = (v < 0x800000 and (v & 3) == 0) or (v >> 16) == 0x8000
                if not ok:
                    return None
                p += 4
                cnt2 += 1
                if cnt2 > 0x8000:
                    return None
            if p >= lim:
                return None
            p += 4
            n_lists += 1
        if n_lists != 3:
            return None
    return dict(offset=o, hsize=hsize, data_off=doff, rodata_off=roff,
                export_count=cnt, ctor=ctor, dtor=dtor, exports=exports)

def parse_relocs(o, hdr):
    """Parse GOT/gp/data reloc lists at rodata_offset. Returns (got, gp, datarel, end_off)."""
    if hdr["rodata_off"] == 0xFFFFFFFF:
        return [], [], [], None
    p = o + hdr["rodata_off"]
    got = []
    while p < N and u32(p) != 0xFFFFFFFE:
        got.append(u32(p)); p += 4
    p += 4
    gp = []
    while p < N and u32(p) != 0xFFFFFFFD:
        gp.append(u32(p)); p += 4
    p += 4
    dr = []
    while p < N and u32(p) != 0xFFFFFFFF:
        dr.append(u32(p)); p += 4
    p += 4
    return got, gp, dr, p - o

dlls = []
pos = 0
while pos < N:
    hdr = try_header(pos)
    if hdr is None:
        print(f"WALK BREAK: no valid header at 0x{pos:x} (dll #{len(dlls)})")
        break
    got, gp, dr, reloc_end = parse_relocs(pos, hdr)
    hdr["got_n"], hdr["gp_n"], hdr["datarel_n"] = len(got), len(gp), len(dr)
    hdr["reloc_end"] = reloc_end
    # minimum end of this DLL
    ends = [hdr["hsize"]]
    if reloc_end: ends.append(reloc_end)
    if hdr["data_off"] != 0xFFFFFFFF: ends.append(hdr["data_off"])
    min_end = pos + max(ends)
    # scan for next header
    nxt = None
    p = (min_end + 3) & ~3
    while p < N:
        if try_header(p):
            nxt = p
            break
        p += 4
    end = nxt if nxt is not None else N
    hdr["end"] = end
    hdr["size"] = end - pos
    hdr["first16"] = data[pos + hdr["hsize"]:pos + hdr["hsize"] + 16].hex()
    dlls.append(hdr)
    pos = end

print(f"Parsed {len(dlls)} DLLs, coverage 0x{pos:x} / 0x{N:x}")

# bank assignment from DLLS.tab header [0x5B, 0xAE, 0x00, 0xC6]
E_LAST, M_LAST, P_LAST = 0x5B, 0xAE, 0xC6
def bank(i):
    if i <= E_LAST: return "engine"
    if i <= M_LAST: return "modgfx"
    if i <= P_LAST: return "projgfx"
    return "objects"

with open(f"{OUT}/dll_table.txt", "w") as f:
    f.write("idx  bank     offset    size    hsize  exports ctor     dtor     data_off rodata_off got gp  datarel text_first16\n")
    for i, d in enumerate(dlls):
        f.write(f"{i:<4d} {bank(i):<8s} 0x{d['offset']:06x}  {d['size']:<7d} 0x{d['hsize']:<4x} {d['export_count']:<7d} "
                f"{d['ctor']:<8x} {d['dtor']:<8x} "
                f"{'-' if d['data_off']==0xFFFFFFFF else hex(d['data_off']):<8s} "
                f"{'-' if d['rodata_off']==0xFFFFFFFF else hex(d['rodata_off']):<10s} "
                f"{d['got_n']:<3d} {d['gp_n']:<3d} {d['datarel_n']:<7d} {d['first16']}\n")

json.dump([{k: v for k, v in d.items() if k != "exports"} | {"exports": d["exports"]} for d in dlls],
          open(f"{OUT}/dll_table.json", "w"), indent=0)

# ---- strings per DLL ----
STR = re.compile(rb"[\x20-\x7e]{4,}")
with open(f"{OUT}/dll_strings.txt", "w") as f:
    for i, d in enumerate(dlls):
        seg = data[d["offset"]:d["end"]]
        hits = [(m.start(), m.group().decode("ascii")) for m in STR.finditer(seg)]
        if not hits:
            continue
        f.write(f"=== DLL {i} ({bank(i)}) offset=0x{d['offset']:x} size={d['size']} exports={d['export_count']} ===\n")
        for off, s in hits:
            f.write(f"  +0x{off:05x}  {s}\n")
print("wrote dll_table.txt / dll_table.json / dll_strings.txt")
