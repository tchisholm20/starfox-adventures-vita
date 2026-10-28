#!/usr/bin/env python3
"""retail_gate.py -- REFUSE an ELF that carries retail bytes (2026-10-27, the public release).

For every stfx_retail_table entry (gsae01_rev1_data.c, written by retail_runtime.py) of >= 8 bytes, the bytes are read from the
developer's main.dol and searched in the ELF in BOTH orders (disc big-endian, and host order after the per-element swap). Any hit:
exit 9. Entries under 8 bytes (single floats / ints such as 1.0f) cannot be told apart from ordinary constants and are NOT gated --
the coverage line says how much is. Watched failing on eboot 2dbc5504's ELF (built before retail_runtime.py).

    python retail_gate.py <elf> [--iso <rev1 iso>] [--src <engine tree>]
"""
import argparse, re, struct, sys
import stfx_paths as P_

ap = argparse.ArgumentParser(); ap.add_argument('elf'); ap.add_argument('--iso', default=P_.ISO); ap.add_argument('--src', default=P_.dev('TOOLS_SRC') or P_.SRC)
a = ap.parse_args()
tab = open(a.src + '/src/port/src/data/gsae01_rev1_data.c').read()
if 'stfx_retail_table' not in tab: sys.exit('retail_gate: the data file has no stfx_retail_table -- run retail_runtime.py (the generated data still EMBEDS retail bytes)')
sizes = {m.group(2): {'float': 4, 'double': 8, 'unsigned int': 4, 'unsigned short': 2, 'unsigned char': 1}[m.group(1)] * int(m.group(3) or 1)
         for m in re.finditer(r'^(float|double|unsigned int|unsigned short|unsigned char) (\w+)(?:\[(\d+)\])?;', tab, re.M)}
ents = []
for m in re.finditer(r'\{ \(void\*\)&?(\w+), (0x[0-9a-fA-F]+)u, (?:\(unsigned\)sizeof \w+|(\d+)u), (\d) \}', tab):
    name, addr, n, el = m.group(1), int(m.group(2), 16), m.group(3), int(m.group(4))
    ents.append((name, addr, int(n) if n else sizes[name], el))
f = open(a.iso, 'rb'); f.seek(0x420); dol = struct.unpack('>I', f.read(4))[0]; f.seek(dol); h = f.read(0x100)
OFFS, ADDRS, SIZES = struct.unpack('>18I', h[0:72]), struct.unpack('>18I', h[0x48:0x90]), struct.unpack('>18I', h[0x90:0xD8])
def read(addr, n):
    for o, s0, sz in zip(OFFS, ADDRS, SIZES):
        if sz and s0 <= addr and addr + n <= s0 + sz: f.seek(dol + o + addr - s0); return f.read(n)
    sys.exit(f'retail_gate: {addr:#x}+{n} in no DOL section')
# Runs that the DECOMP'S OWN SOURCE defines (upstream commits them, as it commits all its code): not bytes our extraction added. Each is
# checked mechanically -- the named source must still DEFINE the symbol with the string -- so an exception cannot outlive its evidence.
ALLOW_SOURCE = {
    'sSampleBufferSLoadedCallbackLoadError': ('src/main/audio.c', 'sampleBufferSLoadedCallback load error', 'upstream 151a306a52 (2026-08-21)'),
    'sVoxmapsRouteNodesListOverflow':        ('src/main/voxmaps.c', 'VOXMAPS: route nodes list overflow', 'upstream a274f5ae299 (2026-09-07)'),
}
allowed = {}
for name, (rel, text, ev) in ALLOW_SOURCE.items():
    body = open(a.src + '/' + rel, encoding='utf-8', errors='replace').read()
    if not re.search(r'^char ' + name + r'\[\d*\] = "' + re.escape(text), body, re.M):
        sys.exit(f'retail_gate: the allowance for {name} names {rel}, which no longer defines it with "{text}" -- re-examine it')
    allowed[name] = f'{rel} ({ev})'
elf = open(a.elf, 'rb').read()
gated = skipped = 0; hits = []
for name, addr, n, el in ents:
    b = read(addr, n)
    if n < 8 or len(set(b)) <= 1: skipped += n; continue
    sw = bytes(x for k in range(0, n, el) for x in reversed(b[k:k + el]))
    gated += n
    for form, pat in (('disc order', b), ('host order', sw)):
        if pat in elf:
            if name in allowed: print(f'retail_gate: allowed {name} ({n} B, {form}): defined by the decomp source {allowed[name]}')
            else: hits.append(f'{name} ({n} B, {form})')
print(f'retail_gate: {len(ents)} table entries; {gated} bytes gated, {skipped} bytes not gatable (< 8 B or constant)')
if hits:
    print(f'retail_gate: FAIL -- {len(hits)} retail byte runs found in {a.elf}:'); [print('   ', x) for x in hits[:15]]; sys.exit(9)
print('retail_gate: PASS -- no gated retail bytes in the ELF')
