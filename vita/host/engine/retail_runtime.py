#!/usr/bin/env python3
"""retail_runtime.py -- take the RETAIL BYTES out of the binary (2026-10-27, the public release).

gen_data.py and gen_pools.py emit game data read from the developer's own main.dol as initialised definitions, so an eboot built from
them CONTAINS Nintendo's data (~600 constants + ~1.5 KB of strings). Distributing that is what the release must not do. This rewrites
both generated files IN PLACE:
  - every definition keeps its TYPE, NAME and SIZE (layout unchanged) but becomes zero-filled;
  - gsae01_rev1_data.c gains `stfx_retail_table[]`: {destination, Rev 1 address, size, element size} per object, which
    dvd_shim.c stfx_retail_load() fills at boot from the PLAYER's disc (byte-swapped per element, as gen_data.py did on the host).
Run it after any re-run of gen_data.py / gen_pools.py. retail_gate.py (in build_game_vita.sh) refuses an ELF that contains the bytes.

    python retail_runtime.py [--src <engine tree>; default $STFX_SRC]      # idempotent: a converted file is left alone
"""
import argparse, os, re, sys
import devcfg                                    # dev_defaults.env: the developer's own paths (absent in the public tree)

TYPES = {'float': 4, 'double': 8, 'unsigned int': 4, 'unsigned short': 2, 'unsigned char': 1}
DEF = re.compile(r'^(float|double|unsigned int|unsigned short|unsigned char) (\w+)(?:\[(\d+)\])?(?: = .*?)?; /\* (\.\w+) rev1 (0x[0-9a-fA-F]+)(.*)\*/\s*$')
BSS = re.compile(r'^unsigned char \w+\[\d+\]; /\* \.s?bss2?: zero-initialised \*/\s*$')
POOL_HDR = re.compile(r'^/\* (\w+): rev1 (0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+) bytes')
MARK = '/* RETAIL-FREE (retail_runtime.py)'


def convert_data(path):
    src = open(path, newline='').read()
    if MARK in src: return None, 'already converted'
    out, table, refused = [], [], []
    for line in src.split('\n'):
        m = DEF.match(line)
        if m:
            ty, name, n, sec, addr, rest = m.groups()
            dims = f'[{n}]' if n else ''
            out.append(f'{ty} {name}{dims}; /* {sec} rev1 {addr}: filled at boot from the player\'s main.dol (stfx_retail_load) */')
            table.append((name, int(addr, 16), TYPES[ty], False))
            continue
        if ' = ' in line and not line.startswith((' ', '/*', '\t')) and line.strip():
            refused.append(line[:100])
        out.append(line)
    if refused: sys.exit('retail_runtime: initialised definitions it cannot parse (they would stay in the binary):\n  ' + '\n  '.join(refused))
    return out, table


def convert_pools(path):
    src = open(path, newline='').read()
    if MARK in src: return None, 'already converted'
    out, pools, cur = [], [], None
    for line in src.split('\n'):
        h = POOL_HDR.match(line)
        if h:
            cur = [h.group(1), int(h.group(2), 16), int(h.group(3), 16), 0]; pools.append(cur)
        s = line.strip()
        if s.startswith('.byte'):
            if cur is None: sys.exit('retail_runtime: .byte outside a pool block')
            k = len(s[5:].split(',')); cur[3] += k
            out.append(f'\t.zero {k}')
            continue
        if s.startswith(('.ascii', '.asciz', '.string', '.word', '.short', '.long', '.int', '.float')):
            sys.exit(f'retail_runtime: pool line it does not convert (retail bytes would stay): {s[:80]}')
        out.append(line)
    for name, addr, hdr_len, n in pools:
        if n > hdr_len: sys.exit(f'retail_runtime: pool {name} holds {n} bytes, its header says {hdr_len}')
    return out, pools


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--src', default=os.environ.get('STFX_SRC') or devcfg.dev('TOOLS_SRC')); a = ap.parse_args()
    devcfg.need('--src (or STFX_SRC)', a.src)
    d = os.path.join(a.src, 'src', 'port', 'src', 'data')
    pd, pp = os.path.join(d, 'gsae01_rev1_data.c'), os.path.join(d, 'gsae01_string_pools.S')
    rd, rp = convert_data(pd), convert_pools(pp)
    if rd[0] is None and rp[0] is None: print('retail_runtime: both files already retail-free'); return
    if rd[0] is None or rp[0] is None: sys.exit('retail_runtime: ONE file is converted and the other is not -- regenerate both (gen_data.py, gen_pools.py) and re-run')
    data_lines, table = rd; pool_lines, pools = rp
    ents = [f'    {{ (void*)&{n}, {addr:#010x}u, (unsigned)sizeof {n}, {el} }},' for n, addr, el, _ in table]
    externs = [f'extern unsigned char {name}[];' for name, *_ in pools]
    ents += [f'    {{ (void*){name}, {addr:#010x}u, {n}u, 1 }},   /* string pool */' for name, addr, _, n in pools]
    tail = [
        '',
        f'{MARK}: the definitions above are ZERO-filled; this table says where each one\'s bytes are in main.dol.',
        ' * dvd_shim.c stfx_retail_load() copies them from the PLAYER\'s disc at boot, swapping each `elem`-byte element to host order. */',
        'typedef struct { void* dst; unsigned addr, size; unsigned char elem; } StfxRetailEnt;',
        *externs,
        'const StfxRetailEnt stfx_retail_table[] = {',
        *ents,
        '};',
        f'const unsigned stfx_retail_n = {len(ents)}u;',
        '']
    open(pd, 'w', newline='\n').write('\n'.join(data_lines).rstrip('\n') + '\n' + '\n'.join(tail))
    pool_lines.insert(3, f'{MARK}: every .byte run is now .zero of the same length; the bytes come from the player\'s disc at boot. */')
    open(pp, 'w', newline='\n').write('\n'.join(pool_lines))
    print(f'retail_runtime: {len(table)} data objects + {len(pools)} pools ({sum(p[3] for p in pools)} bytes) now filled at boot; table {len(ents)} entries')


main()
