#!/usr/bin/env python3
"""gxt_lint.py -- G-M1 and G-M2 of the GX render thread (RT_THREAD.md 6.2, section 7).  Host-only; reads objects and the ELF, never touches a console.

The split is only as good as the claim "G runs these bodies, R runs those, and they share no mutable state".  This tool PROVES that claim from the linked
objects (`arm-vita-eabi-objdump -dr` of the HAL, the scaffold, the bridge and the backend) instead of trusting the table:

  G-M1 (the manifest)
    (i)   the nm T/W status of the 133 + 19 boundary symbols equals the committed manifest ([nm] of gxt_g_allow.txt), and no boundary-looking symbol
          (GX*, VI*, gxfifo_push_*) is defined that the table does not know.
    (ii)  RELOCATION CHECK: the transitive closure of every G-resident body (thr L rows, the E extras' real bodies, the T extras, gx_breakpoint_deliver) and the
          closure of everything R replays (thr R and X rows' real bodies, the P writer VBYTES uses, the R/X extras, ALL backend functions, gx_thread's R loop,
          the custom module's R half) must reference DISJOINT sets of mutable data symbols, except the instrument counters in [data-allow] (each with a reason).
          Re-classifying GXInitTexObjLOD as L MUST fail here: it writes gTexLod, which R's GXLoadTexObj reads.
    (iii) no G-resident closure reaches a function R replays as a root (the G side would run R's body), and nothing R-closure calls across objects through a
          --wrap'd symbol except the documented edges: an R row with edge=1 (GXSetTevIndirect) and the E rows (dropped on R).  A cross-object call from R to any other
          wrapped symbol would RECORD from R (or trap, or reach the custom module): that is the bug the edge flag exists for.
    (iv)  the data shared between the GAME and the HAL (defined in a HAL object and referenced by a non-HAL object, or defined in both) equals {gFogColor} + [shared].
  R2 (R2_STATE.md): the 42 S rows have TWO bodies in the link -- the public one (the legacy path: R replays it, so it is an R root like an R row) and the G variant gxh_g_<Name> (R2: G runs it
        on its own instance).  The G variants are checked as a class of their own: each must be DEFINED, none may reach a public S / R / X row (it would run R's instance), and the data
        they touch must not be touched by what R replays OUTSIDE the S rows' own bodies (the draw path, the backend, the ring loop, gxs_r_apply): the two instances gx_hal / gx_hal_g are
        different symbols, so only a side statistic can be shared, and one the draw path also touches must be in [data-allow] with its reason.
  G-M2 (the R closure)
    R's closure references no OS*/os_* symbol (os_ext_poll is dropped on R by its E wrapper) and nothing DEFINED under src_main_, src_dlls_, src_track_, src_xref_,
    src_musyx_ (game code).  Mutations: an OSSleepThread call added to a replayed body; the os_ext_poll E row deleted.

    python gxt_lint.py [lint]   [--objdir D] [--vobj D] [--extra-obj F ...] [--elf E] [--nm-file F] [--allow F] [--dup F]
    python gxt_lint.py report                    closure sizes, the cross-object edges R really uses, the shared data (informational)
    python gxt_lint.py snapshot-nm [--elf E]     print the [nm] section for gxt_g_allow.txt
    python gxt_lint.py selftest                  watch every rule FAIL on a mutation (synthetic fixtures, then the real objects when present)
exit 0 clean, 9 on any finding.
"""
import collections, fnmatch, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__)).replace('\\', '/')
sys.path.insert(0, HERE)
import gen_gx_rec as G                                    # the table, the disposition table, find_tool, winpath

OUT = G.winpath(G.devcfg.need('OUT', os.environ.get('OUT') or G.devcfg.dev('TOOLS_OUT')).replace('\\', '/'))
ALLOW_TXT = HERE + '/gxt_g_allow.txt'
GAME_PREFIXES = ('src_main_', 'src_dlls_', 'src_track_', 'src_xref_', 'src_musyx_')     # game code: R must reference nothing defined here
HAL_PREFIX = 'src_port_src_hal_'
# objects whose functions are analysed (everything else is a leaf).  The game objects that DEFINE a boundary symbol (pi_videoinit.c: GXFlush_) join them.
ANALYSED = ('src_port_src_hal_', 'src_port_src_scaffold_', 'src_port_src_bridge_', 'src_port_src_data_')
BACKEND_RE = re.compile(r'^(rhi_vita.*|rhi_gxm.*|backend_stubs|plat_window_vita)\.c\.o$')
TEXT_T = set('TtWwi')
DATA_T = set('bBdDCsSvV')          # mutable data (v/V: weak objects).  r/R (read-only) is not a race.
CALL_RELOCS = ('CALL', 'JUMP', 'PC24', 'PLT')
OS_RE = re.compile(r'^(OS[A-Z]\w*|os_\w+)$')
BR_RE = re.compile(r'^\s+[0-9a-f]+:\s+(?:b\w*(?:\.\w)?|cbn?z)\s+(?:\w+,\s*)?[0-9a-f]+ <([^>+]+)(?:\+0x[0-9a-f]+)?>')
FUNC_RE = re.compile(r'^([0-9a-f]{8}) <([^>]+)>:$')
RELOC_RE = re.compile(r'^\s+([0-9a-f]+):\s+R_ARM_(\w+)\s+(\S+)\s*$')
WORD_RE = re.compile(r'^\s+([0-9a-f]+):\s+\.word\s+0x([0-9a-f]+)')
DREL_RE = re.compile(r'^([0-9a-f]{8})\s+R_ARM_(\w+)\s+(\S+)\s*$')


def tool(n):
    return G.find_tool(n)


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit('%s failed: %s' % (os.path.basename(cmd[0]), r.stderr[-300:]))
    return r.stdout


# ---------------------------------------------------------------------------------------------------------------------------------------
# the committed lists (gxt_g_allow.txt): [nm] name status | [data-allow] [shared] [r-os-allow] [r-game-allow] [boot-only]  pattern | reason
# ---------------------------------------------------------------------------------------------------------------------------------------
def parse_allow(text):
    sec, out = None, {'nm': {}, 'data-allow': [], 'shared': [], 'r-os-allow': [], 'r-game-allow': [], 'boot-only': []}
    for ln in text.replace('\r\n', '\n').splitlines():
        t = ln.strip()
        if not t or t.startswith('#'):
            continue
        m = re.match(r'^\[([\w-]+)\]', t)
        if m:
            sec = m.group(1)
            if sec not in out:
                raise SystemExit('gxt_g_allow.txt: unknown section [%s]' % sec)
            continue
        if sec == 'nm':
            p = t.split()
            if len(p) != 2 or p[1] not in ('T', 'W'):
                raise SystemExit('gxt_g_allow.txt [nm]: want `name T|W`: ' + t)
            out['nm'][p[0]] = p[1]
        elif sec:
            pat, _, why = t.partition('|')
            if not why.strip():
                raise SystemExit('gxt_g_allow.txt: [%s] entry %r has no reason (every entry needs one, the planner approves additions)' % (sec, pat.strip()))
            out[sec].append((pat.strip(), why.strip()))
    return out


def allowed(name, entries):
    base = name.split('::')[-1]
    return any(fnmatch.fnmatchcase(base, p) for p, _ in entries)


# ---------------------------------------------------------------------------------------------------------------------------------------
# (i) nm status
# ---------------------------------------------------------------------------------------------------------------------------------------
def nm_status(nm_text):
    st = {}
    for ln in nm_text.splitlines():
        p = ln.split()
        if len(p) == 3 and re.fullmatch(r'[0-9a-fA-F]+', p[0]):
            st[p[2]] = 'W' if p[1] in 'Ww' else ('T' if p[1] in 'Tt' else p[1])
    return st


def lint_nm_manifest(entries, nm_text, manifest):
    f = []
    st = nm_status(nm_text)
    names = {e['name'] for e in entries}
    for e in sorted(names):
        want = manifest.get(e)
        have = st.get(e)
        if want is None:
            f.append('(i) %s has no entry in the [nm] manifest of gxt_g_allow.txt (snapshot-nm)' % e)
        elif have is None:
            f.append('(i) %s is not defined in the ELF (manifest says %s)' % (e, want))
        elif have != want:
            f.append('(i) %s: nm says %s, the manifest says %s -- a weak stub became a strong definition (or the reverse): its row must be re-classified' % (e, have, want))
    for e in sorted(set(manifest) - names):
        f.append('(i) the manifest lists %s, which is in no table' % e)
    for n in sorted(st):
        if st[n] in ('T', 'W') and G.NM_RE.match('0 T ' + n) and n not in names:   # functions only: GXNtsc480Prog / GXWGFifo are data tables
            f.append('(i) %s is DEFINED in the ELF and looks like a boundary symbol but has no row in the table' % n)
    if len(st) < 1000:
        f.append('(i) nm returned only %d symbols: the ELF or the filter is wrong, refusing to call that a pass' % len(st))
    return f


# ---------------------------------------------------------------------------------------------------------------------------------------
# the object graph
# ---------------------------------------------------------------------------------------------------------------------------------------
class Func:
    __slots__ = ('id', 'obj', 'calls', 'data', 'ptrs')

    def __init__(self, fid, obj):
        self.id, self.obj = fid, obj
        self.calls = []          # (target symbol name, local-to-this-object function id or None)
        self.data = set()        # mutable data symbol ids referenced
        self.ptrs = set()        # data symbol ids whose initialiser holds function pointers (followed conservatively)


class Graph:
    def __init__(self):
        self.funcs = {}           # id -> Func
        self.gdef = {}            # global symbol -> (obj, type letter)
        self.objdefs = {}         # obj -> {name: type letter} (all defined, local and global)
        self.ptrs = collections.defaultdict(set)   # data symbol id -> function ids / symbols its initialiser points at
        self.objs = set()
        self.unresolved_sections = 0

    def fid(self, obj, name):
        """qualified id: a global symbol is its own name, a local one is obj::name"""
        t = self.objdefs.get(obj, {}).get(name)
        return name if (t is None or t.isupper()) else '%s::%s' % (obj, name)


def parse_object(g, path, objname):
    nmo = sh([tool('arm-vita-eabi-nm'), '-n', path])
    defs, sections = {}, {'b': [], 'd': [], 'r': []}
    for ln in nmo.splitlines():
        p = ln.split()
        if len(p) == 3:
            addr, t, n = int(p[0], 16), p[1], p[2]
            defs[n] = t
            k = t.lower()
            if k in ('b', 'd', 'r', 's', 'v'):
                sections['b' if k in ('b', 's') else 'd' if k in ('d', 'v') else 'r'].append((addr, n))
    g.objs.add(objname)
    g.objdefs[objname] = defs
    for t, n in defs.items():
        if t.isupper() and t not in 'U':
            old = g.gdef.get(n)
            if old is None or (old[1] in 'WV' and t not in 'WV'):
                g.gdef[n] = (objname, t)
    dis = sh([tool('arm-vita-eabi-objdump'), '-dr', '--no-show-raw-insn', path])
    cur, last_word, labels = None, {}, set()
    lines = dis.splitlines()
    for ln in lines:
        m = FUNC_RE.match(ln)
        if m:
            labels.add(m.group(2))
    sym_at = {k: sorted(v) for k, v in sections.items()}

    def by_addend(sec, addend):
        arr = sym_at.get(sec, [])
        best = None
        for a, n in arr:
            if a <= addend:
                best = n
            else:
                break
        return best

    funcs_here = {}
    for ln in lines:
        m = FUNC_RE.match(ln)
        if m:
            name = m.group(2)
            cur = Func(g.fid(objname, name), objname)
            funcs_here[name] = cur
            # a global defined by several objects (weak stubs in the scaffold, the bridge's copies): only the definition the linker keeps is the graph's
            d = pick_def(g, name) if '::' not in cur.id and hasattr(g, 'all_defs') else None
            if d is None or d[0] == objname:
                g.funcs[cur.id] = cur
            continue
        if cur is None:
            continue
        m = WORD_RE.match(ln)
        if m:
            last_word[m.group(1)] = int(m.group(2), 16)
            continue
        m = RELOC_RE.match(ln)
        if m:
            addr, rtype, tgt = m.group(1), m.group(2), m.group(3)
            is_call = any(c in rtype for c in CALL_RELOCS)
            if tgt.startswith('.'):
                sec = tgt.split('+')[0]
                if sec in ('.bss', '.data') and rtype == 'ABS32' and addr in last_word:
                    n = by_addend('b' if sec == '.bss' else 'd', last_word[addr])
                    if n:
                        cur.data.add(g.fid(objname, n))
                    else:
                        g.unresolved_sections += 1
                elif sec in ('.bss', '.data'):
                    g.unresolved_sections += 1
                continue
            if is_call:
                cur.calls.append((tgt, None))
            else:
                cur.calls.append((tgt, 'addr'))        # address taken: a function pointer or a datum; decided once every object is read
            continue
        m = BR_RE.match(ln)
        if m and m.group(1) != cur.id.split('::')[-1] and m.group(1) in labels:
            cur.calls.append((m.group(1), 'local'))
    # initialisers that hold pointers: `objdump -r` of the data sections
    try:
        rr = sh([tool('arm-vita-eabi-objdump'), '-r', path])
    except SystemExit:
        rr = ''
    sec = None
    for ln in rr.splitlines():
        m = re.match(r'^RELOCATION RECORDS FOR \[([^\]]+)\]', ln)
        if m:
            sec = m.group(1)
            continue
        if sec and sec.startswith(('.data', '.rodata')):
            m = DREL_RE.match(ln)
            if m and not m.group(3).startswith('.'):
                owner = by_addend('d' if sec.startswith('.data') else 'r', int(m.group(1), 16))
                if owner:
                    g.ptrs[g.fid(objname, owner)].add((objname, m.group(3)))
    return funcs_here


def load_graph(objdir, vobj=None, extra=(), dupfile=None, only_prefix=None):
    """read every analysed object (+ the backend, + extras) and the global definition index of the WHOLE objdir"""
    g = Graph()
    files = sorted(f for f in os.listdir(objdir) if f.endswith('.o'))
    # the global definition index of EVERY object in objdir (one nm call per 64 files)
    g.all_defs = collections.defaultdict(list)          # symbol -> [(obj, type)]
    for i in range(0, len(files), 64):
        chunk = [os.path.join(objdir, f) for f in files[i:i + 64]]
        out = sh([tool('arm-vita-eabi-nm'), '-A', '-g', '--defined-only'] + chunk)
        for ln in out.splitlines():
            m = re.match(r'^(.+\.o):[0-9a-f]+ (\w) (\S+)$', ln)
            if m:
                g.all_defs[m.group(3)].append((os.path.basename(m.group(1)), m.group(2)))
    kept = {}
    if dupfile and os.path.exists(dupfile):
        for ln in open(dupfile, encoding='utf-8'):
            p = ln.rstrip('\n').split('\t')
            if len(p) >= 2 and p[1].startswith('kept '):
                kept[p[0]] = p[1][5:].strip()
    g.kept = kept
    todo = []
    for f in files:
        if f.startswith(ANALYSED) or (only_prefix and f.startswith(only_prefix)):
            todo.append((os.path.join(objdir, f), f))
    # game objects that define a boundary symbol (pi_videoinit.c's GXFlush_): their bodies are G-resident roots
    g.game_def_objs = set()
    if vobj and os.path.isdir(vobj):
        for f in sorted(os.listdir(vobj)):
            if f.endswith('.o'):
                todo.append((os.path.join(vobj, f), f))
    for p in extra:
        todo.append((p, os.path.basename(p)))
    for p, f in todo:
        parse_object(g, p, f)
    return g


def pick_def(g, name):
    """the object the linker takes `name` from: the dup_resolution `kept` object if listed, else the strong definition, else the first"""
    ds = g.all_defs.get(name, [])
    if not ds:
        return None
    k = g.kept.get(name)
    for o, t in ds:
        if o == k:
            return o, t
    strong = [d for d in ds if d[1] not in 'WV']
    return (strong or ds)[0]


# ---------------------------------------------------------------------------------------------------------------------------------------
# closure
# ---------------------------------------------------------------------------------------------------------------------------------------
class Closure:
    def __init__(self):
        self.funcs, self.data, self.ext, self.wrapped, self.parent = set(), {}, {}, {}, {}


def resolve_call(g, fn, tgt, kind):
    """-> (kind, id, via_real).  kind: 'func' (a function of an analysed object, by id) | 'xfunc' (a global function, defined elsewhere) | 'data' | 'const' | 'ext'.
    `__real_X` is what --wrap makes of a call to X from the wrapper: the REAL body, never the wrapper."""
    obj = fn.obj
    via_real = False
    if tgt.startswith('__real_'):
        tgt, via_real = tgt[7:], True
    defs_here = g.objdefs.get(obj, {})
    if kind == 'local':
        return ('func', g.fid(obj, tgt), False)
    t_here = defs_here.get(tgt)
    if t_here is not None and not via_real:          # defined in this very object: the assembler resolved it, --wrap cannot touch it
        if t_here in TEXT_T:
            return ('func', g.fid(obj, tgt), False)
        if t_here in DATA_T:
            return ('data', g.fid(obj, tgt), False)
        return ('const', tgt, False)
    d = pick_def(g, tgt) if hasattr(g, 'all_defs') else None
    if d is None:
        return ('xfunc', tgt, via_real) if tgt in g.funcs else ('ext', tgt, via_real)
    o, t = d
    if t in TEXT_T:
        return ('xfunc', tgt, via_real)
    if t in DATA_T:
        return ('data', tgt, False)
    return ('const', tgt, False)


def closure(g, roots, wrapped, on_wrapped):
    """transitive closure over analysed objects.  `wrapped` = {symbol: entry}; on_wrapped(sym, entry, caller_id) -> 'follow' | 'stop' decides what a
    cross-object call to a --wrap'd symbol means for THIS side."""
    c = Closure()
    stack = []

    def add(fid, parent):
        if fid in g.funcs and fid not in c.funcs:
            c.funcs.add(fid); c.parent[fid] = parent; stack.append(fid)

    for r in roots:
        add(r, None)
    while stack:
        fid = stack.pop()
        fn = g.funcs[fid]
        for d in fn.data:
            c.data.setdefault(d, fid)
            for (po, ps) in g.ptrs.get(d, ()):
                add(g.fid(po, ps) if ps in g.objdefs.get(po, {}) else ps, fid)
        for tgt, kind in fn.calls:
            k, v, via_real = resolve_call(g, fn, tgt, kind)
            if k == 'ext':
                c.ext.setdefault(v, fid)
            elif k == 'data':
                c.data.setdefault(v, fid)
                for (po, ps) in g.ptrs.get(v, ()):               # an initialiser that points at functions: those are reachable
                    add(g.fid(po, ps) if ps in g.objdefs.get(po, {}) else ps, fid)
            elif k == 'xfunc':
                if v in wrapped and not via_real:
                    c.wrapped.setdefault(v, fid)
                    if on_wrapped(v, wrapped[v], fid) != 'follow':
                        continue
                if v in g.funcs:
                    add(v, fid)
                else:
                    c.ext.setdefault(v, fid)                      # defined in an object we do not analyse (game code, libc)
            elif k == 'func':
                if v in g.funcs:
                    add(v, fid)
    return c


def path_of(c, fid):
    out = []
    while fid is not None and len(out) < 12:
        out.append(fid.split('::')[-1])
        fid = c.parent.get(fid)
    return ' <- '.join(out)


# ---------------------------------------------------------------------------------------------------------------------------------------
# the rules
# ---------------------------------------------------------------------------------------------------------------------------------------
def classify(rows, extras):
    """name -> entry (thr, edge) for every boundary symbol"""
    return {e['name']: e for e in G.all_entries(rows, extras)}


def r_special_roots(g):
    """the R half of the custom module, when its object is loaded: what R REALLY calls for an X row (gxts_R_X may be a no-op: GXPeekZ, GXGetScissor and
    GXReadDrawSync are answered on G), instead of the over-approximation 'the real body of every X row'"""
    return [fid for fid, fn in g.funcs.items() if fn.obj == 'gx_rt_special.o' and fn.id.split('::')[-1].startswith(('gxts_R_', 'gxts_r_', 'gxts_audit_r'))]


def analyse(g, rows, extras, allow, r_extra_roots=(), r_extra_objs=(), shared_refs=None):
    """-> (findings, info dict).  A pure function of the graph and the table: the selftest feeds it mutated tables.
    shared_refs = {data symbol: why} for (iv): HAL-defined data that game code references (or defines too); only those R's closure touches can race."""
    ent = classify(rows, extras)
    f, info = [], {}
    boot_only = {p for p, _ in allow.get('boot-only', [])}
    special = r_special_roots(g)
    g_roots = [n for n, e in ent.items() if (e['thr'] in ('L', 'E') or (e['thr'] == 'X' and n in G.G_RESIDENT_X)) and n not in boot_only]
    t_roots = [n for n, e in ent.items() if e['thr'] == 'T']
    # X rows: the real body is a root of R unless the custom module is loaded (then its gxts_R_* are the roots, and they say what R calls)
    x_real = [n for n, e in ent.items() if e['thr'] == 'X' and n not in G.G_RESIDENT_X]
    r_roots = [n for n, e in ent.items() if e['thr'] in ('R', 'S')] + ['gxfifo_push_u8']          # S rows: the legacy path replays the public body on R
    s_rows = sorted(n for n, e in ent.items() if e['thr'] == 'S')
    s_g_roots = ['gxh_g_' + n for n in s_rows]
    r_roots += special if special else x_real
    r_roots += list(r_extra_roots)
    r_roots += [fid for fid, fn in g.funcs.items() if BACKEND_RE.match(fn.obj) or fn.obj in r_extra_objs]
    info['r_special'] = len(special)
    r_roots_nons = [r for r in r_roots if r not in s_rows]       # R's roots without the S rows' public bodies (the legacy path): what R2 leaves on R

    def r_side(sym, e, caller):
        th = e['thr']
        if th == 'E':
            return 'stop'                                      # dropped on R by its E wrapper
        if th in ('R', 'S') and e['edge']:
            return 'follow'                                    # the wrapper calls __real_X on R
        if th == 'L':
            return 'follow'                                    # the wrapper's mode-3 line is `return __real_X(...)`: it runs on R
        return 'stop'                                          # R/X/P/T: reported below

    def g_side(sym, e, caller):
        return 'follow' if e['thr'] in ('L', 'T', 'E') else 'stop'    # an R/X/P call from G RECORDS: that is the design

    cr = closure(g, r_roots, ent, r_side)
    cg = closure(g, g_roots, ent, g_side)
    cr_nons = closure(g, r_roots_nons, ent, r_side)
    cgs = closure(g, [r for r in s_g_roots if r in g.funcs], ent, g_side)
    info['s_g_funcs'], info['s_g_data'] = len(cgs.funcs), len(cgs.data)
    for r_ in s_g_roots:                                       # (S) each S row's G variant exists in the analysed objects
        if r_ not in g.funcs:
            f.append('(S) %s is not defined in the analysed HAL objects: the S row has no G variant (a stale object, or R2_STATE.md 4.2 not met)' % r_)
    ct = closure(g, t_roots, ent, g_side)
    info['r_funcs'], info['g_funcs'], info['t_funcs'] = len(cr.funcs), len(cg.funcs), len(ct.funcs)
    info['r_data'], info['g_data'] = len(cr.data), len(cg.data)
    # (ii) disjoint mutable data.  T rows (viewer-only) write R's state BY NATURE: reaching one in threaded mode is counted by gxt_viewer_trap and fails the run's
    # gates, so their data is reported (info) but is not a finding.
    both = sorted(set(cg.data) & set(cr.data))
    info['t_vs_r'] = sorted(set(ct.data) & set(cr.data))
    for d in both:
        if allowed(d, allow['data-allow']):
            info.setdefault('shared_allowed', []).append(d)
        else:
            f.append('(ii) data symbol %s is referenced by a G-resident body (%s) AND by what R replays (%s): not in [data-allow]' %
                     (d, path_of(cg, cg.data[d]), path_of(cr, cr.data[d])))
    # (ii-S) the S rows' G variants against what R replays outside the S rows' own bodies
    for d in sorted(set(cgs.data) & set(cr_nons.data)):
        if allowed(d, allow['data-allow']):
            info.setdefault('shared_allowed', []).append(d)
        else:
            f.append('(ii) data symbol %s is referenced by an S row\'s G variant (%s) AND by what R replays outside the S bodies (%s): not in [data-allow]' %
                     (d, path_of(cgs, cgs.data[d]), path_of(cr_nons, cr_nons.data[d])))
    # (iii) G reaches an R root; R reaches a wrapped symbol it must not
    r_root_set = {n for n, e in ent.items() if e['thr'] in ('R', 'S', 'X') and n not in G.G_RESIDENT_X}
    for n in sorted(r_root_set & cg.funcs):
        f.append('(iii) a G-resident body reaches %s, which R replays (%s): G would run R\'s body' % (n, path_of(cg, n)))
    for n in sorted(r_root_set & cgs.funcs):
        f.append('(iii) an S row\'s G variant reaches %s, which R replays (%s): G would run R\'s instance of the state' % (n, path_of(cgs, n)))
    for sym, caller in sorted(cgs.wrapped.items()):
        if ent[sym]['thr'] in ('R', 'S', 'X', 'P'):
            f.append('(iii) an S row\'s G variant calls the boundary symbol %s across objects (%s): its wrapper would record / divert on G (a G variant composes G variants, never a public symbol)' % (sym, path_of(cgs, caller)))
    edges_used = {}
    for sym, caller in cr.wrapped.items():
        e = ent[sym]
        th = e['thr']
        edges_used[sym] = caller
        if th in ('R', 'S') and not e['edge']:
            f.append('(iii) R reaches %s across objects (%s) but its row has edge=0: the wrapper would RECORD from R. Set edge=1 in gx_thr_table.txt (and regenerate), or re-think the row' % (sym, path_of(cr, caller)))
        elif th in ('X', 'P', 'T'):
            f.append('(iii) R reaches %s (thr %s) across objects (%s): its wrapper runs a G-side function on R' % (sym, th, path_of(cr, caller)))
    info['r_edges'] = sorted(edges_used)
    for n, e in sorted(ent.items()):
        if e['edge'] and n not in cr.wrapped:
            f.append('(iii) %s is marked edge=1 but nothing R replays calls it across objects: a stale flag (or the analysis lost the edge)' % n)
        if e['thr'] == 'E' and n not in cr.wrapped:
            f.append('(iii) %s is an E row (dropped on R) but nothing R replays calls it across objects: a stale row' % n)
    # G-M2
    for sym in sorted(cr.ext):
        if OS_RE.match(sym) and not allowed(sym, allow['r-os-allow']):
            f.append('(M2) R\'s closure references %s (%s): OS code takes the baton / the game\'s locks and R must never run it' % (sym, path_of(cr, cr.ext[sym])))
    for sym in sorted(set(cr.ext) | set(cr.data)):
        base = sym.split('::')[-1]
        d = pick_def(g, base) if hasattr(g, 'all_defs') else None
        if d and d[0].startswith(GAME_PREFIXES) and not allowed(base, allow['r-game-allow']):
            f.append('(M2) R\'s closure references %s, defined in game code %s (%s)' % (base, d[0], path_of(cr, cr.ext.get(sym) or cr.data.get(sym))))
    for fid in sorted(cr.funcs):                           # an OS-layer FUNCTION inside R's closure (the symbol is defined in an analysed object, so it is not in cr.ext)
        base = fid.split('::')[-1]
        if OS_RE.match(base) and not allowed(base, allow['r-os-allow']):
            f.append("(M2) R's closure contains the OS-layer function %s (%s): it takes the baton / the game's locks and R must never run it" % (base, path_of(cr, fid)))
    for fid in sorted(cr.funcs):
        if g.funcs[fid].obj.startswith(GAME_PREFIXES):
            f.append('(M2) R\'s closure contains %s from game object %s' % (fid, g.funcs[fid].obj))
    info['r_ext'] = sorted(cr.ext)
    # (iv) the data the GAME shares with what R touches
    if shared_refs is not None:
        touched = {d.split('::')[-1] for d in cr.data} | {s.split('::')[-1] for s in cr.ext}
        shared = {n: why for n, why in shared_refs.items() if n in touched}
        info['shared_with_game'] = sorted(shared)
        f += lint_shared(shared, allow)
    return f, info


def shared_with_game(g, game_objs_nm, dup_lines):
    """(iv): data symbols defined in a HAL object and referenced by game code, or defined in both a HAL and a non-HAL object"""
    shared = {}
    hal_data = {n for n, ds in g.all_defs.items() if any(o.startswith(HAL_PREFIX) and t in DATA_T for o, t in ds)}
    for n, refs in game_objs_nm.items():
        if n in hal_data:
            shared[n] = 'referenced by ' + sorted(refs)[0]
    for ln in dup_lines:
        p = ln.rstrip('\n').split('\t')
        if len(p) < 3:
            continue
        sym = p[0]
        objs = [p[1].replace('kept ', '').strip(), p[2].replace('weakened in ', '').strip()]
        if any(o.startswith(HAL_PREFIX) for o in objs) and any(not o.startswith(HAL_PREFIX) for o in objs) and any(t in DATA_T for o, t in g.all_defs.get(sym, [])):
            shared[sym] = 'defined in both ' + ' and '.join(objs)
    return shared


def game_refs(objdir):
    """symbol -> set of game objects that reference it undefined"""
    refs = collections.defaultdict(set)
    files = sorted(f for f in os.listdir(objdir) if f.endswith('.o') and f.startswith(GAME_PREFIXES))
    for i in range(0, len(files), 64):
        chunk = [os.path.join(objdir, f) for f in files[i:i + 64]]
        for ln in sh([tool('arm-vita-eabi-nm'), '-A', '-u'] + chunk).splitlines():
            m = re.match(r'^(.+\.o):\s+U (\S+)$', ln)
            if m:
                refs[m.group(2)].add(os.path.basename(m.group(1)))
    return refs


def lint_shared(shared, allow):
    f = []
    want = {'gFogColor'}
    for n, why in sorted(shared.items()):
        if n not in want and not allowed(n, allow['shared']):
            f.append('(iv) %s is shared between the game and the HAL (%s) and is not in [shared]: R reads/writes it while G\'s game code does too' % (n, why))
    lit = [p for p, _ in allow['shared'] if not any(ch in p for ch in '*?[')]
    for n in sorted(want | set(lit)):
        if n not in shared:
            f.append('(iv) %s is listed as shared but is not (stale entry, or the analysis lost it)' % n)
    return f


# ---------------------------------------------------------------------------------------------------------------------------------------
# synthetic fixtures for the selftest: a graph small enough to read, every rule failing on one edit
# ---------------------------------------------------------------------------------------------------------------------------------------
def synth_graph(mut=None):
    """HAL: GXSetFoo (R) writes sFoo; GXGetBar (L) reads gBar; GXInitLod (X) writes gLod, which GXLoadTex (X) reads; GXSetCopyClear (X) -> vi_set_clear_color (E)
    across objects; GXSetTevIndRepeat (R) -> GXSetTevIndirect (R, edge) across objects; rhi_vita_draw (backend) -> sceGxmDraw."""
    mut = mut or set()
    g = Graph()
    g.all_defs = collections.defaultdict(list)
    g.kept = {}

    def obj(name, funcs, data=(), glob=()):
        g.objs.add(name)
        defs = {}
        for fn in funcs:
            defs[fn] = 'T' if fn in glob or not fn.startswith('_') else 't'
        for d in data:
            defs[d] = 'B' if d in glob else 'b'
        g.objdefs[name] = defs
        for n, t in defs.items():
            if t.isupper():
                g.all_defs[n].append((name, t))
                g.gdef.setdefault(n, (name, t))

    obj('src_port_src_hal_gx_draw_c.o', ['GXSetFoo', 'GXGetBar', 'GXInitLod', 'GXLoadTex', 'GXSetCopyClear', 'GXSetTevIndRepeat', 'gxfifo_push_u8', 'GXFlushLike'],
        ['sFoo', 'gBar', 'gLod', 'gCounter'], glob=['GXSetFoo', 'GXGetBar', 'GXInitLod', 'GXLoadTex', 'GXSetCopyClear', 'GXSetTevIndRepeat', 'gxfifo_push_u8', 'GXFlushLike', 'gBar', 'gLod', 'gCounter'])
    obj('src_port_src_hal_gx_tev_c.o', ['GXSetTevIndirect'], ['sInd'], glob=['GXSetTevIndirect'])
    obj('src_port_src_hal_gx_light_c.o', ['GXSetBaz'] + ([] if 'no_g_variant' in mut else ['gxh_g_GXSetBaz']), ['gHalR', 'gHalG', 'gBazStat'],
        glob=['GXSetBaz'] + ([] if 'no_g_variant' in mut else ['gxh_g_GXSetBaz']) + ['gHalR', 'gHalG', 'gBazStat'])
    obj('src_port_src_hal_vi_shim_c.o', ['vi_set_clear_color'], ['gClear'], glob=['vi_set_clear_color'])
    obj('src_port_src_hal_os_threads_c.o', ['os_ext_poll'], ['gBaton'], glob=['os_ext_poll'])
    obj('rhi_vita.c.o', ['rhi_vita_draw'], ['gGx'], glob=['rhi_vita_draw'])
    obj('src_main_game_c.o', ['gameThing'], ['gGameVar'], glob=['gameThing', 'gGameVar'])
    obj('src_port_src_hal_os_shim_c.o', ['OSSleepThread', 'OSReport'], [], glob=['OSSleepThread', 'OSReport'])
    g.all_defs['gFogColor'] = [('src_port_src_hal_gx_draw_c.o', 'B'), ('src_main_game_c.o', 'B')]

    def fn(o, name, calls=(), data=()):
        f = Func(g.fid(o, name), o)
        f.calls = list(calls)
        f.data = {g.fid(o, d) for d in data}
        g.funcs[f.id] = f
        return f

    H = 'src_port_src_hal_gx_draw_c.o'
    fn(H, 'GXSetFoo', data=['sFoo'] if 'foo_global' not in mut else ['gCounter'])
    fn(H, 'GXGetBar', data=['gBar'] + (['gLod'] if 'getbar_lod' in mut else []) + (['gCounter'] if 'foo_global' in mut else []))
    fn(H, 'GXInitLod', data=['gLod'])
    fn(H, 'GXLoadTex', data=['gLod'], calls=[('os_ext_poll', None)])
    fn(H, 'GXSetCopyClear', calls=[('vi_set_clear_color', None)])
    fn(H, 'GXSetTevIndRepeat', calls=[('GXSetTevIndirect', None)])
    fn(H, 'gxfifo_push_u8', data=['gCounter'])
    fn(H, 'GXFlushLike', calls=[('GXSetFoo', 'local')] if 'g_calls_r' in mut else [])
    fn('src_port_src_hal_gx_tev_c.o', 'GXSetTevIndirect', data=['sInd'])
    L = 'src_port_src_hal_gx_light_c.o'
    fn(L, 'GXSetBaz', data=['gHalR', 'gBazStat'])                     # the S row's public body: the legacy path, R replays it
    if 'no_g_variant' not in mut:
        fn(L, 'gxh_g_GXSetBaz', data=['gHalG', 'gBazStat'] + (['gHalR'] if 'g_writes_r_instance' in mut else []), calls=[('GXSetBaz', None)] if 'g_calls_public' in mut else [])
    fn('src_port_src_hal_vi_shim_c.o', 'vi_set_clear_color', data=['gClear'])
    fn('src_port_src_hal_os_threads_c.o', 'os_ext_poll', data=['gBaton'])
    fn('rhi_vita.c.o', 'rhi_vita_draw', calls=([('OSSleepThread', None)] if 'r_os' in mut else []) + ([('gameThing', None)] if 'r_game' in mut else []), data=['gGx'] + (['gBazStat'] if 'baz_stat_in_backend' in mut else []) + (['gHalG'] if 'backend_reads_g_instance' in mut else []))
    if 'r_calls_r' in mut:
        g.funcs['rhi_vita_draw'] = g.funcs.pop('rhi_vita_draw')
        g.funcs['rhi_vita_draw'].calls.append(('GXSetFoo', None))
    return g


def synth_table(mut=None):
    mut = mut or set()
    rows = [dict(name=n, thr=t, edge=e, cls='A', rec=1, ret='void', params='void', inline='') for n, t, e in [
        ('GXSetFoo', 'R', 0), ('GXGetBar', 'L', 0), ('GXInitLod', 'L' if 'lod_as_L' in mut else 'X', 0), ('GXLoadTex', 'X', 0),
        ('GXSetCopyClear', 'X', 0), ('GXSetTevIndRepeat', 'R', 0), ('GXSetTevIndirect', 'R', 0 if 'no_edge' in mut else 1), ('gxfifo_push_u8', 'P', 0), ('GXFlushLike', 'L', 0), ('GXSetBaz', 'S', 0)]]
    extras = [dict(name=n, thr=t, edge=0, cls='H', rec=0, ret='void', params='void', inline='', extra=True) for n, t in
              [('vi_set_clear_color', 'E')] + ([] if 'no_poll_row' in mut else [('os_ext_poll', 'E')])]
    return rows, extras


EMPTY_ALLOW = {'nm': {}, 'data-allow': [('gCounter', 'a 32-bit instrument counter, torn reads are benign')], 'shared': [], 'r-os-allow': [], 'r-game-allow': [], 'boot-only': []}


def selftest():
    bad = 0

    def expect(label, findings, want_fail, key=None):
        nonlocal bad
        hit = [x for x in findings if key in x] if key else findings
        ok = bool(hit) == want_fail
        print('%-78s %s%s' % (label, 'FAILS as it must' if hit and want_fail else ('passes' if not hit else 'FAILS'), '' if ok else '   <== SELFTEST BROKEN'))
        if hit and want_fail:
            print('      e.g. ' + hit[0][:230])
        if not ok:
            bad += 1

    # ---- synthetic fixtures: one edit per rule
    def run(gm=None, tm=None, allow=None):
        rows, extras = synth_table(tm)
        for e in G.all_entries(rows, extras):
            pass
        return analyse(synth_graph(gm), rows, extras, allow or EMPTY_ALLOW)

    f, info = run()
    expect('synthetic: the baseline is clean', f, False)
    expect('synthetic (ii): GXInitLod re-classified X -> L (it writes gLod, which R reads)', run(tm={'lod_as_L'})[0], True, '(ii)')
    expect('synthetic (ii): a G-resident getter starts reading gLod', run(gm={'getbar_lod'})[0], True, '(ii)')
    expect('synthetic (ii): ...but the same getter with gLod in [data-allow] passes', run(gm={'getbar_lod'}, allow=dict(EMPTY_ALLOW, **{'data-allow': EMPTY_ALLOW['data-allow'] + [('gLod', 'x')]}))[0], False)
    expect('synthetic (ii): a replayed R row now writes the counter G also uses, with [data-allow] empty', run(gm={'foo_global'}, allow=dict(EMPTY_ALLOW, **{'data-allow': []}))[0], True, '(ii)')
    expect('synthetic (iii): a G-resident body calls an R row\'s real body', run(gm={'g_calls_r'})[0], True, '(iii)')
    expect('synthetic (iii): the backend (R) calls GXSetFoo across objects (an R row, edge 0)', run(gm={'r_calls_r'})[0], True, '(iii)')
    expect('synthetic (iii): GXSetTevIndirect edge flag removed', run(tm={'no_edge'})[0], True, '(iii)')
    expect('synthetic (iii): the os_ext_poll E row deleted -> GXLoadTex\'s call is a bare cross-object call', run(tm={'no_poll_row'})[0], True, '(M2)')
    expect('synthetic (S): an S row whose G variant is missing from the objects', run(gm={'no_g_variant'})[0], True, '(S)')
    expect('synthetic (iii): an S row\'s G variant calls the public (R-instance) body', run(gm={'g_calls_public'})[0], True, '(iii)')
    expect('synthetic (ii-S): the backend (R) touches a side statistic the G variant also touches', run(gm={'baz_stat_in_backend'})[0], True, '(ii)')
    expect('synthetic (ii-S): ...with [data-allow] naming it, with its reason', run(gm={'baz_stat_in_backend'}, allow=dict(EMPTY_ALLOW, **{'data-allow': EMPTY_ALLOW['data-allow'] + [('gBazStat', 'a statistic: one writer per mode')]}))[0], False)
    expect('synthetic (ii-S): the S row\'s public body and its G variant share a statistic (by design: one writer per mode)', run()[0], False)
    expect('synthetic (ii-S): the backend touches the G instance', run(gm={'backend_reads_g_instance'})[0], True, '(ii)')
    expect('synthetic (M2): an OSSleepThread call added to the backend', run(gm={'r_os'})[0], True, '(M2)')
    expect('synthetic (M2): the backend calls game code', run(gm={'r_game'})[0], True, '(M2)')
    # (i)
    ent = [dict(name=n) for n in ('GXBegin', 'GXFlush')]
    nm = ''.join('%08x %s %s\n' % (0x1000 + 16 * i, 'T', 'sym%d' % i) for i in range(1100)) + '00002000 T GXBegin\n00002010 W GXFlush\n'
    man = {'GXBegin': 'T', 'GXFlush': 'W'}
    expect('(i) nm status equals the manifest', lint_nm_manifest(ent, nm, man), False)
    expect('(i) a weak stub flipped to strong', lint_nm_manifest(ent, nm.replace('W GXFlush', 'T GXFlush'), man), True)
    expect('(i) a strong definition flipped to weak', lint_nm_manifest(ent, nm.replace('T GXBegin', 'W GXBegin'), man), True)
    expect('(i) a fake GXFoo defined', lint_nm_manifest(ent, nm + '00003000 T GXFoo\n', man), True)
    expect('(i) a DATA symbol with a boundary-looking name (GXWGFifo) is not a function', lint_nm_manifest(ent, nm + '00003000 D GXWGFifo\n', man), False)
    expect('(i) a boundary symbol removed from the ELF', lint_nm_manifest(ent, nm.replace('GXFlush', 'GXFlushX'), man), True)
    expect('(i) a stale manifest entry', lint_nm_manifest(ent, nm, dict(man, GXGone='T')), True)
    expect('(i) nm filter broken (3 symbols)', lint_nm_manifest(ent, '00000000 T GXBegin\n00000010 W GXFlush\n', man), True)
    # (iv)
    expect('(iv) {gFogColor} only', lint_shared({'gFogColor': 'x'}, EMPTY_ALLOW), False)
    expect('(iv) a new shared symbol', lint_shared({'gFogColor': 'x', 'gNewShared': 'y'}, EMPTY_ALLOW), True)
    expect('(iv) gFogColor stopped being shared (stale)', lint_shared({}, EMPTY_ALLOW), True)
    expect('(iv) an allow-listed pattern covers it', lint_shared({'gFogColor': 'x', 'gx_draw_count': 'y'}, dict(EMPTY_ALLOW, shared=[('gx_*', 'counters')])), False)
    # the allow file itself
    try:
        parse_allow('[shared]\ngx_foo\n')
        expect('allow file: an entry without a reason', [], True)
    except SystemExit as e:
        expect('allow file: an entry without a reason is refused', ['refused: %s' % e], True)
    # ---- the real objects, when this machine has them
    objdir = OUT + '/obj'
    if os.path.isdir(objdir) and os.path.exists(ALLOW_TXT):
        allow = parse_allow(open(ALLOW_TXT, encoding='utf-8').read())
        rows = G.parse_table(); thr0, extras = G.parse_thr()
        g = real_graph()
        f, info = analyse(g, rows, extras, allow, *real_extras())
        expect('REAL objects: G-M1 (ii)(iii) + G-M2 on the committed table', f, False)
        print('      (%d G funcs / %d R funcs, %d / %d data symbols, R edges %s)' % (info['g_funcs'], info['r_funcs'], info['g_data'], info['r_data'], ' '.join(info['r_edges'])))
        t2, e2 = G.parse_thr(G.read_text(G.THR_TXT).replace('GXInitTexObjLOD|X|0', 'GXInitTexObjLOD|L|0'))
        r2 = G.attach_thr(G.parse_table(attach=False), t2)
        f2, _ = analyse(g, r2, e2, allow, *real_extras())
        expect('REAL objects (ii): GXInitTexObjLOD re-classified as L (writes R\'s gTexLod)', f2, True, '(ii)')
        t3, e3 = G.parse_thr(G.read_text(G.THR_TXT).replace('os_ext_poll|', 'os_ext_pollX|').replace('H|0|void|os_ext_poll|void||E|0|', 'H|0|void|os_ext_poll|void||E|0|'))
        txt4 = G.read_text(G.THR_TXT)
        line = [l for l in txt4.splitlines() if l.startswith('H|0|void|os_ext_poll|')][0]
        t4, e4 = G.parse_thr(txt4.replace(line + '\n', ''))
        r4 = G.attach_thr(G.parse_table(attach=False), t4)
        f4, _ = analyse(g, r4, e4, allow, *real_extras())
        expect('REAL objects (M2): the os_ext_poll E row deleted', f4, True, 'contains the OS-layer function os_ext_poll')
        t5, e5 = G.parse_thr(txt4.replace('GXSetTevIndirect|S|1', 'GXSetTevIndirect|S|0'))
        r5 = G.attach_thr(G.parse_table(attach=False), t5)
        f5, _ = analyse(g, r5, e5, allow, *real_extras())
        expect('REAL objects (iii): the edge flag of GXSetTevIndirect removed', f5, True, 'GXSetTevIndirect')
        # an OSSleepThread call added to a replayed body: patch one function of the real graph
        g.funcs['GXSetCullMode'].calls.append(('OSSleepThread', None))
        f6, _ = analyse(g, rows, extras, allow, *real_extras())
        expect('REAL objects (M2): OSSleepThread called from GXSetCullMode (a replayed body)', f6, True, 'contains the OS-layer function OSSleepThread')
        g.funcs['GXSetCullMode'].calls.pop()
        n = 'gx_draw_count_selftest'
        g.funcs['GXSetCullMode'].data.add(n)
        g.funcs['GXFlush_'].data.add(n) if 'GXFlush_' in g.funcs else g.funcs['GXGetTexObjFmt'].data.add(n)
        f7, _ = analyse(g, rows, extras, dict(allow, **{'data-allow': [(p, w) for p, w in allow['data-allow'] if p != n]}), *real_extras())
        expect('REAL objects (ii): a data symbol touched by a G-resident body and a replayed body', f7, True, n)
    else:
        print('(the real objects are not under %s: the REAL-object mutations were NOT run)' % objdir)
    print('SELFTEST %s' % ('OK: every rule was watched failing' if not bad else 'BROKEN: %d expectation(s) wrong' % bad))
    return 1 if bad else 0


_REAL = {}


def real_graph():
    if 'g' not in _REAL:
        _REAL['g'] = load_graph(OUT + '/obj', vobj=OUT + '/vobj', extra=real_extra_files(), dupfile=OUT + '/dup_resolution.txt')
    return _REAL['g']


def real_extra_files():
    """gx_thread.o / gx_rt_special.o / gx_rec.o if built (the game dir or scratch_gen)"""
    out = []
    for d in (OUT + '/game', OUT + '/scratch_gen'):
        for n in ('gx_thread.o', 'gx_rt_special.o', 'gx_rt_state.o'):
            p = '%s/%s' % (d, n)
            if os.path.exists(p) and not any(os.path.basename(x) == n for x in out):
                out.append(p)
    return out


def real_extras():
    """-> (r_extra_roots, r_extra_objs): gx_thread's R loop and the custom module's R half join R's roots"""
    names = [os.path.basename(p) for p in real_extra_files()]
    roots, objs = [], []
    if 'gx_thread.o' in names:
        roots.append('gx_thread.o::r_main')                # the R loop; the custom module's R half is picked up by r_special_roots()
    if 'gx_rt_state.o' in names:
        roots.append('gx_rt_state.o::gxs_r_apply')         # R2: the R half of the STATE record (it copies into gx_hal and runs the post-apply hook)
    return roots, objs


def main():
    a = sys.argv[1:]
    cmd = a[0] if a and not a[0].startswith('--') else 'lint'
    opt = lambda k, d=None: (a[a.index(k) + 1] if k in a else d)
    multi = lambda k: [x for i, x in enumerate(a) if i and a[i - 1] == k]
    if cmd == 'selftest':
        return selftest()
    rows = G.parse_table()
    thr, extras = G.parse_thr()
    ent = G.all_entries(rows, extras)
    elf = opt('--elf', OUT + '/game/stairfax_game.elf')
    if cmd == 'snapshot-nm':
        st = nm_status(G.run_nm(elf))
        print('[nm]    # name status (T strong, W weak): `gxt_lint.py snapshot-nm`, from %s' % elf)
        for e in sorted(ent, key=lambda e: e['name']):
            print('%s %s' % (e['name'], st.get(e['name'], '?')))
        return 0
    allow = parse_allow(open(opt('--allow', ALLOW_TXT), encoding='utf-8').read())
    findings = []
    nm_text = open(opt('--nm-file'), encoding='utf-8').read() if opt('--nm-file') else (G.run_nm(elf) if os.path.exists(elf) else None)
    if nm_text is None:
        print('(i) NOT RUN: no ELF at %s (pass --elf or --nm-file)' % elf)
    else:
        findings += lint_nm_manifest(ent, nm_text, allow['nm'])
    objdir = opt('--objdir', OUT + '/obj')
    dupfile = opt('--dup', OUT + '/dup_resolution.txt')
    g = load_graph(objdir, vobj=opt('--vobj', OUT + '/vobj'), extra=multi('--extra-obj') or real_extra_files(), dupfile=dupfile)
    re_roots, re_objs = real_extras()
    dup = open(dupfile, encoding='utf-8').read().splitlines() if os.path.exists(dupfile) else []
    fa, info = analyse(g, rows, extras, allow, multi('--r-root') or re_roots, re_objs, shared_with_game(g, game_refs(objdir), dup))
    findings += fa
    if cmd == 'report':
        print('G-resident closure: %d functions, %d data symbols | viewer-only (T) closure: %d functions' % (info['g_funcs'], info['g_data'], info['t_funcs']))
        print('R closure: %d functions, %d data symbols (custom-module R roots: %d)' % (info['r_funcs'], info['r_data'], info['r_special']))
        print('cross-object --wrap edges R really uses: %s' % ' '.join(info['r_edges']))
        print('data touched by a G-resident body AND by R (all covered by [data-allow]): %s' % ' '.join(info.get('shared_allowed', [])))
        print('viewer-only (T) bodies touch R state by nature (counted by gxt_viewer_trap): %s' % ' '.join(info['t_vs_r']))
        print('data the GAME shares with what R touches: %s' % ' '.join(info.get('shared_with_game', [])))
        print('external symbols R references (not analysed): %s' % ' '.join(info['r_ext']))
        print('unresolved section-relative relocations: %d' % g.unresolved_sections)
    for x in findings:
        print('LINT FAIL ' + x)
    print('gxt_lint: %s' % ('FAIL (%d findings)' % len(findings) if findings else 'PASS'))
    return 9 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
