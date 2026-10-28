#!/usr/bin/env python3
"""gen_embedded.py -- build-time extraction of copyrighted embedded assets.

The decomp never commits these: dtk extracts them from the user's own retail DOL
at build time (config.yml `extract:`). This does the same for the host build,
into a BUILD-OUTPUT directory (never the fork's history), from the Rev 1 disc.

gLoadingScreenTextures (Rev 1 .data 0x802CD260, 0x40000 bytes): the address is the
Rev 1 map's, CHECKED BY STRUCTURE, not trusted (a Rev 1 entry was proven stale this
session): the doc says 0xFF padding from +0x1F520 to the end -- all 133,856 bytes
are 0xFF -- and the next bytes begin a string, i.e. the region ends where the map
says. A 0x400-byte TAIL of the following data is included because diagnostic code
reads format strings at base+0x4002c, past the region (docs/orig/embedded_assets.md).

    python gen_embedded.py OUTDIR
"""
import os, struct, sys
import stfx_paths as P_
ISO = P_.ISO
ADDR, SIZE, TAIL = 0x802CD260, 0x40000, 0x400

f = open(ISO, 'rb'); f.seek(0x420); dol = struct.unpack('>I', f.read(4))[0]; f.seek(dol); h = f.read(0x100)
O = struct.unpack('>18I', h[0:72]); A = struct.unpack('>18I', h[0x48:0x90]); S = struct.unpack('>18I', h[0x90:0xD8])
def rd(a, n):
    for o, s0, sz in zip(O, A, S):
        if sz and s0 <= a and a + n <= s0 + sz:
            f.seek(dol + o + a - s0); return f.read(n)
    sys.exit(f'{a:#x}+{n:#x} not inside one DOL section')

b = rd(ADDR, SIZE + TAIL)
if set(b[0x1F520:SIZE]) != {0xFF}:
    sys.exit('STRUCTURE CHECK FAILED: 0xFF padding not found at +0x1F520..+0x40000 -- wrong address or disc')
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
with open(os.path.join(out, 'gLoadingScreenTextures.inc'), 'w', newline='\n') as w:
    w.write('/* GENERATED at build time from the user\'s own GSAE01 Rev 1 disc by\n'
            ' * projects/stairfax-vita/host/engine/gen_embedded.py. Copyrighted: NEVER commit.\n'
            f' * {SIZE:#x} bytes + a {TAIL:#x}-byte tail of the following .data (read by diagnostics\n'
            ' * at base+0x4002c on GameCube, where the data is contiguous). */\n')
    w.write(f'unsigned char gLoadingScreenTextures[{SIZE + TAIL:#x}] __attribute__((aligned(32))) = {{\n')
    for i in range(0, len(b), 32):
        w.write(','.join(str(x) for x in b[i:i + 32]) + ',\n')
    w.write('};\n')
print(f'wrote gLoadingScreenTextures.inc ({SIZE + TAIL} bytes; padding check passed) -> {out}')
