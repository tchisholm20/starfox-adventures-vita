#!/usr/bin/env python3
"""pool_gate.py -- do the game's STRING-POOL OFFSETS land on the same strings as retail?

Decompiled code indexes ACROSS separately-defined string arrays from the first one:
    char* base = sSampleBufferSLoadedCallbackLoadError;   // a 40-byte array
    loadFileByPathAsync(base + 0x1b4, ...);                // "/audio/data/Music.bin"
MWCC laid those arrays out contiguously, each padded to 4 bytes; GCC need not. Device
run 6 of the game logged DVD "misses" for `allback load error` and `io/data/Music.bin` --
offsets landing mid-string.

Ground truth is the retail DOL: for every (pool, offset) the source uses, the C string at
pool+offset in the LINKED ELF must equal the C string at the pool's retail address+offset.
Exit 0 = all equal, 1 = mismatches (listed), 2 = tool error.

    python pool_gate.py ELF
"""
import os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rev_map as R

import stfx_paths as P_
SRC = P_.SRC + '/'
NM = 'C:/dev/msys64/usr/local/vitasdk/bin/arm-vita-eabi-nm.exe'
RELF = 'C:/dev/msys64/usr/local/vitasdk/bin/arm-vita-eabi-readelf.exe'
# (the TU that USES the offsets, the pool symbol it starts from)
POOLS = [('src/main/audio.c', 'sSampleBufferSLoadedCallbackLoadError'),
         ('src/dlls/engine/2/maketex.c', 'sMemoryCardFileNameString'),
         ('src/main/gameloop_main.c', 'sGameLoopResetMessages'),
         ('src/main/voxmaps.c', 'sVoxmapsRouteNodesListOverflow')]

# Pools whose retail address rev_map cannot decide. Listed, printed as OPEN on every run,
# never silently passed. Remove an entry only by placing its address from Rev 1 code.
KNOWN_OPEN = {'sGameLoopResetMessages':
              'AMBIGUOUS retail address; its offsets only feed OSReport text in the reset handler'}

ISO = P_.ISO
f = open(ISO, 'rb'); f.seek(0x420); dol = struct.unpack('>I', f.read(4))[0]; f.seek(dol); h = f.read(0x100)
O = struct.unpack('>18I', h[0:72]); A = struct.unpack('>18I', h[0x48:0x90]); S = struct.unpack('>18I', h[0x90:0xD8])
def dol_read(a, n):
    for o, s0, sz in zip(O, A, S):
        if sz and s0 <= a < s0 + sz:
            f.seek(dol + o + a - s0); return f.read(min(n, s0 + sz - a))
    return None

def offsets(tu, pool):
    s = open(SRC + tu, encoding='utf-8', errors='replace').read()
    aliases = re.findall(r'\b(\w+)\s*=\s*(?:\(\s*char\s*\*\s*\)\s*)?&?' + pool + r'\s*;', s)
    names = '|'.join([pool] + aliases)
    return sorted({int(o, 0) for o in re.findall(r'\b(?:' + names + r')\s*\+\s*(0x[0-9a-fA-F]+|\d+)\b', s)})

def cstr(b):
    return b.split(b'\0')[0] if b else None

def main():
    elf = sys.argv[1]
    syms = {}
    for line in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    segs = []
    for line in subprocess.run([RELF, '-lW', elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if p and p[0] == 'LOAD':
            segs.append((int(p[2], 16), int(p[1], 16), int(p[4], 16)))   # vaddr, file off, filesz
    raw = open(elf, 'rb').read()
    def elf_read(a, n):
        for va, off, fs in segs:
            if va <= a < va + fs: return raw[off + a - va: off + a - va + n]
        return None
    # RETAIL-FREE build (2026-10-27, retail_runtime.py): the pools are ZERO in the ELF and stfx_retail_load copies each whole block from the
    # player's main.dol at boot. Read them THROUGH that fill: a pool covered by stfx_retail_table returns the disc bytes at the table's address,
    # so a misplaced label or a wrong table address is still a MISMATCH. Uncovered addresses read the ELF, as before.
    fill = []
    tabf = P_.SRC + '/src/port/src/data/gsae01_rev1_data.c'
    tab = open(tabf, encoding='utf-8', errors='replace').read() if os.path.exists(tabf) else ''
    for m in re.finditer(r'\{ \(void\*\)(\w+), (0x[0-9a-fA-F]+)u, (\d+)u, 1 \},   /\* string pool \*/', tab):
        if m.group(1) in syms: fill.append((syms[m.group(1)], int(m.group(2), 16), int(m.group(3))))
    print(f'  pools filled at boot from the disc (stfx_retail_table): {len(fill)}')
    elf_only = elf_read
    def elf_read(a, n):
        for s0, ra, sz in fill:
            if s0 <= a < s0 + sz: return dol_read(ra + (a - s0), min(n, s0 + sz - a))
        return elf_only(a, n)
    bad = checked = 0
    # ⚠ The first version of this gate checked ONLY the bytes at the pool symbol, and passed
    # (25/25) a build whose game code never used that symbol: GCC's ARM section anchors reach
    # a TU's own globals as `.LANCHOR0 + k`, so overriding the symbol changed nothing (device
    # run 7 logged the same garbage paths). So each using object must also REFERENCE the pool
    # through its symbol -- a relocation naming it.
    OBJDUMP = NM.replace('-nm.exe', '-objdump.exe')
    for tu, pool in POOLS:
        obj = P_.OUT + '/obj/' + re.sub(r'[/.]', '_', tu) + '.o'
        if pool in KNOWN_OPEN: pass
        elif not os.path.exists(obj):
            print(f'  {tu}: object missing -- cannot judge'); bad += 1
        else:
            rel = subprocess.run([OBJDUMP, '-r', obj], capture_output=True, text=True).stdout
            if not re.search(r'\s' + re.escape(pool) + r'(\+0x[0-9a-f]+)?\s*$', rel, re.M):
                bad += 1
                print(f'  NOT REACHED {tu}: its code has no relocation naming {pool} '
                      f'(section anchors?) -- the override cannot take effect')
    for tu, pool in POOLS:
        if pool not in syms: print(f'  {pool}: not in ELF'); bad += 1; continue
        p, conf, _ = R.predict(pool, exclude_self=False)
        if p is None or conf.startswith('AMBIGUOUS'):
            if pool in KNOWN_OPEN:
                print(f'  OPEN (known, not judged): {pool} -- {KNOWN_OPEN[pool]}'); continue
            print(f'  {pool}: retail address not decidable ({conf}) -- cannot judge'); bad += 1; continue
        for o in offsets(tu, pool):
            checked += 1
            want, got = cstr(dol_read(p + o, 256)), cstr(elf_read(syms[pool] + o, 256))
            if want != got:
                bad += 1
                print(f'  MISMATCH {pool}+{o:#x}: retail {want!r}  vita {got!r}')
    print(f'{checked} pool offsets checked, {bad} wrong')
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
