import sys, os, json, struct, re, bisect
from collections import defaultdict
from elftools.elf.elffile import ELFFile

ROOT = "/Users/zcanann/Documents/Projects/SFA-Decomp"
SPLITS = ROOT + "/config/GSAE01/splits.txt"
SYMBOLS = ROOT + "/config/GSAE01/symbols.txt"
DOL = ROOT + "/orig/GSAE01/sys/main.dol"

def parse_splits():
    units = {}
    cur = None
    for line in open(SPLITS):
        m = re.match(r'^(\S+\.c):', line)
        if m:
            cur = m.group(1); units[cur] = {}
            continue
        m = re.match(r'^\s+(\.\w+|\w+)\s+start:(0x[0-9A-Fa-f]+)\s+end:(0x[0-9A-Fa-f]+)', line)
        if m and cur:
            units[cur][m.group(1)] = (int(m.group(2),16), int(m.group(3),16))
    return units

def parse_symbols():
    name2addr = {}
    addr2name = {}
    addr2meta = {}
    for line in open(SYMBOLS, errors='replace'):
        m = re.match(r'^(\S+)\s*=\s*(\S+):(0x[0-9A-Fa-f]+);(.*)$', line)
        if not m: continue
        name, sect, addr = m.group(1), m.group(2), int(m.group(3),16)
        rest = m.group(4)
        sz = re.search(r'size:(0x[0-9A-Fa-f]+)', rest)
        sc = re.search(r'scope:(\w+)', rest)
        name2addr[name] = (sect, addr, int(sz.group(1),16) if sz else None)
        addr2name.setdefault(addr, name)
        if addr not in addr2meta:
            addr2meta[addr] = (name, sc.group(1) if sc else None, sect)
    return name2addr, addr2name, addr2meta

class Dol:
    def __init__(self, path):
        d = open(path,'rb').read()
        self.d = d
        offs = struct.unpack('>18I', d[0:0x48])
        addrs = struct.unpack('>18I', d[0x48:0x90])
        sizes = struct.unpack('>18I', d[0x90:0xd8])
        self.secs = [(addrs[i], sizes[i], offs[i]) for i in range(18) if sizes[i]]
    def read(self, addr, n):
        for a,s,o in self.secs:
            if a <= addr and addr+n <= a+s:
                return self.d[o+addr-a:o+addr-a+n]
        return None

def global_section_ranges(units):
    r = defaultdict(lambda: [0xffffffff, 0])
    for u in units.values():
        for s,(a,b) in u.items():
            r[s][0] = min(r[s][0], a); r[s][1] = max(r[s][1], b)
    return {s:(a,b) for s,(a,b) in r.items()}

def audit(unit_key, objpath, units, name2addr, gsr, dol):
    claims = units[unit_key]
    f = open(objpath,'rb')
    elf = ELFFile(f)
    symtab = elf.get_section_by_name('.symtab')
    secnames = {i: elf.get_section(i).name for i in range(elf.num_sections())}
    syms = list(symtab.iter_symbols())
    text_start = claims['.text'][0]
    # function table: STT_FUNC symbols in .text
    funcs = []
    for s in syms:
        if s['st_info']['type']=='STT_FUNC' and s['st_shndx'] not in ('SHN_UNDEF','SHN_ABS'):
            if secnames.get(s['st_shndx'])=='.text':
                funcs.append((s['st_value'], s['st_size'], s.name))
    funcs.sort()
    fstarts = [f0 for f0,_,_ in funcs]
    def off2func(off):
        i = bisect.bisect_right(fstarts, off)-1
        if i<0: return '?'
        return funcs[i][2]
    def classify(addr):
        for s,(a,b) in gsr.items():
            if a<=addr<b: return s
        return '?'
    # relocs
    rela = elf.get_section_by_name('.rela.text')
    perfunc = defaultdict(lambda: defaultdict(set))  # func -> sect -> set(addr)
    unknown = []
    for r in rela.iter_relocations():
        sym = syms[r['r_info_sym']]
        rtype = r['r_info_type']
        if sym['st_shndx'] not in ('SHN_UNDEF','SHN_ABS'):
            sect = secnames.get(sym['st_shndx'])
            if sect not in claims:
                continue
            addr = claims[sect][0] + sym['st_value'] + r['r_addend']
        else:
            info = name2addr.get(sym.name)
            if info is None:
                m = re.match(r'lbl_(80[0-9A-Fa-f]{6})$', sym.name)
                if m: addr = int(m.group(1),16) + r['r_addend']
                else:
                    unknown.append(sym.name); continue
            else:
                addr = info[1] + r['r_addend']
        sect = classify(addr)
        fn = off2func(r['r_offset'])
        perfunc[fn][sect].add(addr)
    return funcs, perfunc, unknown

def fmt_val(b):
    if b is None: return '????'
    u = struct.unpack('>I', b)[0]
    fv = struct.unpack('>f', b)[0]
    return f"{u:08x} ({fv!r})"

def resolve_reloc_addr(sym, r, claims, secnames, name2addr):
    if sym['st_shndx'] not in ('SHN_UNDEF', 'SHN_ABS'):
        sect = secnames.get(sym['st_shndx'])
        if sect not in claims:
            return None
        return claims[sect][0] + sym['st_value'] + r['r_addend']
    info = name2addr.get(sym.name)
    if info is None:
        m = re.match(r'lbl_(80[0-9A-Fa-f]{6})$', sym.name)
        if m:
            return int(m.group(1), 16) + r['r_addend']
        return None
    return info[1] + r['r_addend']

def scan_unit_pool(objpath, claims, name2addr, s2lo, s2hi):
    with open(objpath, 'rb') as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name('.symtab')
        if symtab is None:
            return None, 0
        secnames = {i: elf.get_section(i).name for i in range(elf.num_sections())}
        syms = list(symtab.iter_symbols())
        pool = set()
        nrel = 0
        for i in range(elf.num_sections()):
            name = elf.get_section(i).name
            if not name.startswith('.rela.'):
                continue
            if name[5:] not in ('.text', '.init'):
                continue
            for r in elf.get_section(i).iter_relocations():
                nrel += 1
                addr = resolve_reloc_addr(syms[r['r_info_sym']], r, claims, secnames, name2addr)
                if addr is not None and s2lo <= addr < s2hi:
                    pool.add(addr)
        return pool, nrel

def value_key(dol, addr, size=None):
    n = size if size in (4, 8) else 4
    b = dol.read(addr, n)
    if b is None:
        return None
    if n == 4 and b == b'\x43\x30\x00\x00':
        b2 = dol.read(addr, 8)
        if b2:
            return (8, b2)
    return (n, b)

def load_report_measures():
    path = ROOT + "/build/GSAE01/report.json"
    out = {}
    if not os.path.exists(path):
        return out
    rep = json.load(open(path))
    for u in rep.get('units', []):
        nm = u['name']
        nm = nm.split('/', 1)[1] if '/' in nm else nm
        ms = u.get('measures', {})
        out[nm + '.c'] = {
            'fuzzy': ms.get('fuzzy_match_percent'),
            'total_code': int(ms.get('total_code') or 0),
            'matched_code': int(ms.get('matched_code') or 0),
            'complete': (u.get('metadata') or {}).get('complete'),
        }
    return out

def run_all(json_out=None):
    units = parse_splits()
    name2addr, addr2name, addr2meta = parse_symbols()
    gsr = global_section_ranges(units)
    dol = Dol(DOL)
    s2syms = sorted((a for n, (s, a, z) in name2addr.items() if s == '.sdata2'))
    s2lo = min([gsr.get('.sdata2', (0xffffffff, 0))[0]] + s2syms[:1])
    s2hi = max(gsr.get('.sdata2', (0, 0))[1], (s2syms[-1] + 8) if s2syms else 0)
    claims2 = sorted((rng[0], rng[1], u) for u, c in units.items() for s, rng in c.items() if s == '.sdata2')
    def owner_of(addr):
        i = bisect.bisect_right(claims2, (addr, 0xffffffff, '\xff')) - 1
        if i >= 0 and claims2[i][0] <= addr < claims2[i][1]:
            return claims2[i][2]
        return None
    unit_pool = {}
    pool_silent = []
    missing = []
    for u, claims in sorted(units.items()):
        if '.text' not in claims and '.init' not in claims:
            continue
        objpath = ROOT + '/build/GSAE01/obj/' + u[:-2] + '.o'
        if not os.path.exists(objpath):
            missing.append(u)
            continue
        pool, nrel = scan_unit_pool(objpath, claims, name2addr, s2lo, s2hi)
        if pool is None:
            missing.append(u)
            continue
        unit_pool[u] = pool
        if not pool:
            pool_silent.append(u)
    addr_units = defaultdict(set)
    for u, pool in unit_pool.items():
        for a in pool:
            addr_units[a].add(u)
    multi = {a: us for a, us in addr_units.items() if len(us) > 1}
    parent = {}
    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    for a, us in multi.items():
        us = sorted(us)
        for u in us[1:]:
            union(us[0], u)
    groups = defaultdict(list)
    for u in {u for us in multi.values() for u in us}:
        groups[find(u)].append(u)
    measures = load_report_measures()
    addr2size = {a: z for n, (s, a, z) in name2addr.items() if s == '.sdata2'}
    addr2dtype = {}
    for line in open(SYMBOLS, errors='replace'):
        m = re.match(r'^\S+\s*=\s*\.sdata2:(0x[0-9A-Fa-f]+);.*data:(\w+)', line)
        if m:
            addr2dtype[int(m.group(1), 16)] = m.group(2)
    internal = {}
    for u, pool in unit_pool.items():
        own = [a for a in pool if owner_of(a) == u or len(addr_units[a]) == 1]
        byval = defaultdict(list)
        for a in own:
            sz = addr2size.get(a)
            if sz is not None and sz not in (4, 8):
                continue
            dt = addr2dtype.get(a)
            if dt is not None and dt not in ('float', 'double'):
                continue
            k = value_key(dol, a, sz)
            if k is not None:
                byval[k].append(a)
        dups = {}
        for k, v in byval.items():
            v = sorted(v)
            if len(v) > 1 and any(b - a > 4 for a, b in zip(v, v[1:])):
                dups[k] = v
        if dups:
            internal[u] = dups
    result = {
        'multi': multi, 'groups': groups, 'internal': internal,
        'pool_silent': pool_silent, 'missing': missing,
        'unit_pool': unit_pool, 'addr_units': addr_units,
        'owner_of': owner_of, 'measures': measures,
        'addr2meta': addr2meta, 'dol': dol,
    }
    print(f"units scanned: {len(unit_pool)}  pool-silent: {len(pool_silent)}  missing carve: {len(missing)}")
    print(f"multi-unit .sdata2 addresses: {len(multi)}  neighborhoods: {len(groups)}")
    def unmatched(u):
        m = measures.get(u)
        return ((m.get('total_code') or 0) - (m.get('matched_code') or 0)) if m else 0
    ranked = sorted(groups.values(), key=lambda g: -sum(unmatched(u) for u in g))
    for g in ranked:
        g = sorted(g)
        shared = sorted(a for a, us in multi.items() if us & set(g))
        w = sum(unmatched(u) for u in g)
        print(f"\n== NEIGHBORHOOD ({len(g)} units, unmatched code {w} B, {len(shared)} shared addrs)")
        for u in g:
            m = measures.get(u, {})
            comp = 'C' if m.get('complete') else ' '
            print(f"  [{comp}] {u:52s} fuzzy {m.get('fuzzy', 0):8.3f}  unmatched {unmatched(u):6d} B")
        for a in shared:
            meta = addr2meta.get(a, ('?', '?', '?'))
            ow = owner_of(a) or 'UNCLAIMED'
            print(f"    {a:08x} {fmt_val(dol.read(a,4)):26s} owner={ow:40s} sym={meta[0]} scope={meta[1]} refs={','.join(sorted(addr_units[a]))}")
    if internal:
        print("\n== INTERNAL DUPLICATE VALUES (same value, distinct addrs, one claim) ==")
        for u in sorted(internal, key=lambda u: -unmatched(u)):
            m = measures.get(u, {})
            print(f"  {u} (fuzzy {m.get('fuzzy', 0):.3f}, unmatched {unmatched(u)} B):")
            for k, addrs in sorted(internal[u].items(), key=lambda kv: kv[1]):
                print(f"    [{k[0]}B] {fmt_val(k[1][:4]):26s} at {' '.join(f'{a:08x}' for a in addrs)}")
    if json_out:
        payload = {
            'multi': {f'{a:08x}': sorted(us) for a, us in multi.items()},
            'pool_silent': pool_silent, 'missing': missing,
            'internal': {u: {' '.join(f'{a:08x}' for a in v): None for v in d.values()} for u, d in internal.items()},
        }
        json.dump(payload, open(json_out, 'w'), indent=1)
    return result

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--all':
        run_all(sys.argv[2] if len(sys.argv) > 2 else None)
        sys.exit(0)
    unit_key = sys.argv[1]
    objpath = sys.argv[2]
    units = parse_splits()
    name2addr, addr2name, addr2meta = parse_symbols()
    gsr = global_section_ranges(units)
    dol = Dol(DOL)
    funcs, perfunc, unknown = audit(unit_key, objpath, units, name2addr, gsr, dol)
    text_start = units[unit_key]['.text'][0]
    print(f"== {unit_key} .text 0x{units[unit_key]['.text'][0]:08x}-0x{units[unit_key]['.text'][1]:08x}")
    print(f"global .sdata2 range: {gsr.get('.sdata2')}")
    if unknown: print("UNKNOWN SYMS:", sorted(set(unknown))[:20])
    # per-function pool refs in text order
    all_pool = set()
    for off, sz, name in funcs:
        refs = perfunc.get(name, {})
        pool = sorted(refs.get('.sdata2', set()))
        all_pool.update(pool)
        if pool:
            print(f"0x{text_start+off:08x} {name:44s} sdata2: {' '.join(f'{a:08x}' for a in pool)}")
    print("\n-- pool words (addr value):")
    vals = defaultdict(list)
    for a in sorted(all_pool):
        b = dol.read(a,4)
        print(f"  {a:08x}  {fmt_val(b)}")
        if b: vals[b].append(a)
    print("\n-- duplicated values at distinct addresses:")
    for b, addrs in sorted(vals.items(), key=lambda kv: kv[1]):
        if len(addrs)>1:
            print(f"  {fmt_val(b):30s} at {' '.join(f'{a:08x}' for a in addrs)}")
