#!/usr/bin/env python3
"""closure.py -- grow upstream's game_engine object set until it links (or stalls).

Uses the objects compile_all.sh produced. Each round: link, parse `undefined
reference`, look each symbol up in an exact nm definition index, add its object,
repeat. Reports what it could not resolve, split by WHY:
  - defined only in a source that does not compile yet  -> fix that source
  - defined nowhere                                        -> genuine gap (stub/implement)
  - multiple definitions                                   -> two providers; pick one
Nothing is stubbed here; this only measures and grows the set.
"""
import os, re, subprocess, sys, collections

import stfx_paths as P_
OUT = P_.OUT
MSYS = os.environ.get('MSYS_BASH') or 'C:/dev/msys64/usr/bin/bash.exe'   # MSYS2's bash; MSYS_BASH overrides
R = P_.SRC_POSIX
V = P_.VITA_POSIX            # projects/stairfax-vita, from this file's location (STFX_VITA overrides)
C3 = os.environ.get('STFX_C3') or (V + '/../gxaot/release/c3')   # c3_cuts.h / .inc (rhi_vita_tev.inc includes them)
LIBS = ('-lvitaGL -lvitashark -lSceShaccCgExt -lstdc++ -lSceShaccCg_stub -ltaihen_stub -lmathneon '
        '-lSceGxm_stub -lSceCommonDialog_stub -lSceAppMgr_stub -lSceSysmodule_stub -lSceKernelDmacMgr_stub '
        '-lSceDisplay_stub -lSceCtrl_stub -lSceSysmem_stub -lSceLibKernel_stub -lSceKernelThreadMgr_stub '
        '-lSceAudio_stub -lScePower_stub -lm')

def sh(cmd):
    return subprocess.run([MSYS, '-lc', 'export VITASDK=/usr/local/vitasdk PATH=/usr/local/vitasdk/bin:$PATH; ' + cmd],
                          capture_output=True, text=True)

def obj_id(src):   # same naming as compile_all.sh
    rel = src.replace(R + '/', '')
    return re.sub(r'[/.]', '_', rel) + '.o'

# --- the Vita platform objects (backend, window, backend stubs) ---------------
os.makedirs(OUT + '/vobj', exist_ok=True)
F = ('-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DTARGET_PC=1 -DSTAIRFAX_PORT=1 '
     f'-I{R}/src/port/include -I{V}/src -I{C3} -idirafter {R}/include -idirafter {R} -w')
vobjs = []
for f in ('rhi_vita.c', 'plat_window_vita.c', 'backend_stubs.c', 'rhi_gxm.c', 'rhi_gxm_frame.c', 'rhi_gxm_shader.c', 'rhi_gxm_texture.c', 'rhi_gxm_draw.c', 'rhi_gxm_target.c', 'rhi_gxm_split.c'):   # rhi_gxm_split.c = R5 (host/engine/R5_SPLIT.md): the GXM table of the render-thread split. rhi_gxm.c: the native GXM backend skeleton (milestone M1); rhi_vita.c dispatches to it when backend.txt says gxm
    o = f'{OUT}/vobj/{f}.o'
    r = sh(f'arm-vita-eabi-gcc -std=gnu11 {F} -c {V}/src/{f} -o {o}')
    if r.returncode: sys.exit('vita obj failed: ' + f + r.stderr[-400:])
    vobjs.append(o)

# --- exact definition index over every compiled object ------------------------
objdir = OUT + '/obj'
allobjs = sorted(os.listdir(objdir))
# A response file, not `*.o`: at 899 objects the glob passed Windows' 32K command-line
# limit, nm never ran ("Argument list too long", rc 126), and the index came back EMPTY
# -- the closure then reported 203 undefined as if that were a result. So: fail loudly.
open(f'{OUT}/nm_objs.rsp', 'w').write('\n'.join(allobjs) + '\n')
r = sh(f'cd {OUT}/obj && arm-vita-eabi-nm -A --defined-only -g @{OUT}/nm_objs.rsp')
if r.returncode != 0: sys.exit(f'index nm FAILED rc={r.returncode}: {r.stderr[-400:]}')
defs = collections.defaultdict(set)
for line in r.stdout.splitlines():
    m = re.match(r'^([^:]+\.o):[0-9a-fA-F]*\s+([A-Za-z])\s+(\S+)$', line.strip())
    if m and m.group(2) not in ('U', 'w', 'v'):
        defs[m.group(3)].add(m.group(1))
_indexed = {o for os_ in defs.values() for o in os_}
if len(_indexed) < 0.9 * len(allobjs):
    sys.exit(f'index covers {len(_indexed)} of {len(allobjs)} objects -- refusing to trust it')
print(f'index: {len(defs)} symbols over {len(_indexed)} of {len(allobjs)} objects')

# --- candidates that must NEVER be pulled in ---------------------------------
# src/dolphin/*: the real Dolphin SDK. The port REPLACES it with the HAL shims
#   (os_shim, dvd_shim, gx_draw...); pulling it in duplicated DVDRead/OSReport/GX*.
# hal/gx_shim.c: the model viewer's GX layer; the engine's is gx_draw.c.
# port tools: separate executables with their own main().
# EXCEPT the game's own MSL math (math_8029*.c): portable C polynomial trig/pow that
#   the SDK does not have and no shim replaces (math_float_helpers.c stays out: asm).
ALLOWED_DOLPHIN = ('src_dolphin_MSL_C_PPCEABI_bare_H_math_802927a4_c.o',
                   'src_dolphin_MSL_C_PPCEABI_bare_H_math_80293da4_c.o',
                   'src_dolphin_MSL_C_PPCEABI_bare_H_math_8029454c_c.o',
                   # AXFX reverb setup: portable C (the wet path is asm; hal/dsp_ai_shim.c)
                   'src_dolphin_axfx_reverb_std_create_c.o')
# src/MSL_C/* (upstream's 2026-09 layout): MSL libc is newlib's job on the Vita; only the game's
#   portable math may come from here, one file at a time, each named with the reason.
ALLOWED_MSL = tuple('src_MSL_C_PPCEABI_bare_H_%s_c.o' % f for f in (
    # the game's portable C math, which lived in src/main/ before upstream moved it here (acosf.c ->
    # inverse_trig.c etc.); each provides names the first merged link had NO provider for (2026-09-27)
    'trig16',                 # fsin16/fcos16 and their Approx/Precise/HighPrecision variants
    'angle_vectors',          # angleToVec2*
    'sincos_approximations',  # mathSinf*/mathCosf*
    'power_helpers',          # powfCore*, Vec_normalize
    'tanf_log2',              # mathTanf, log2fBitEstimate
    'inverse_trig',           # acosf_fast, atan2fHighPrecision (also the game's acosf/atan2f, as pre-merge)
    'sqrtf',                  # sqrtfHighPrecision, invSqrt (and the game's sqrtf: pre-merge math_8029312c.c)
    'reciprocal',             # fastReciprocal (pre-merge trig_float_helpers.c)
    'trig_reduce',            # trigReduceQuadrantHighPrecision (pre-merge math_80292d3c.c)
    # rand/srand (2026-10-07, run 124): the game's 32-bit LCG (state * 0x19660D + 0x3C6EF35F). Without it
    # the link took NEWLIB's rand, which clears bit 31 -- and randomGetRange (vecmath.c) divides by 2^32,
    # so every random range gave only its LOWER HALF and randomGetRange(0, 1) was always 0; engine/7's
    # save/restore of the seed through rand()/srand() did not round-trip either. Verify in the ELF:
    # `rand` must be ~0x14 bytes among game code and carry 0x19660D, not sit beside putc/realloc.
    'rand',
))
def banned(o):
    return ((o.startswith('src_dolphin_') and o not in ALLOWED_DOLPHIN)
            or (o.startswith('src_MSL_C_') and o not in ALLOWED_MSL) or o.startswith('src_port_src_hal_gx_shim')
            or o.startswith('src_port_src_tools_')
            # hal/pad_shim.c: upstream's all-zero PAD stub, superseded by hal/pad_input.c
            # (the Vita backend). Out ON PURPOSE, not because it happens not to compile.
            or o.startswith('src_port_src_hal_pad_shim')
            # hal/thp_shim.c: upstream stubs THPPlayerPlay(void*, int) / THPPlayerStop
            # returning 0 -- a SIGNATURE MISMATCH with the game's real BOOL THPPlayerPlay(void)
            # in thp/dll_3e.c, which is compiled and linked instead.
            or o.startswith('src_port_src_hal_thp_shim'))
for sym in list(defs):
    defs[sym] = {o for o in defs[sym] if not banned(o)}

# --- scaffold stubs are WEAK -----------------------------------------------------
# Upstream's rule: a scaffold stub is deleted once real code replaces it. Weakening
# the scaffold objects does that at link time: a real definition wins, the stub
# only fills what is still missing. Which stubs are still in use is reported.
import shutil
WEAK = OUT + '/weakobj'
os.makedirs(WEAK, exist_ok=True)
PATCHED = OUT + '/patchobj'   # game objects with specific symbols weakened (see resolve_dups)
os.makedirs(PATCHED, exist_ok=True)
for f in os.listdir(PATCHED): os.remove(os.path.join(PATCHED, f))
def objpath(o):
    if 'scaffold' in o: return f'{WEAK}/{o}'
    if os.path.exists(f'{PATCHED}/{o}'): return f'{PATCHED}/{o}'
    return f'{OUT}/obj/{o}'
for o in allobjs:
    if 'scaffold' in o:
        r = sh(f'arm-vita-eabi-objcopy --weaken {OUT}/obj/{o} {WEAK}/{o}')
        if r.returncode: sys.exit('weaken failed: ' + o)

# which sources failed to compile, and what do they (textually) define?
failed = [l.strip() for l in open(OUT + '/fails.txt') if l.strip()]

# --- seed: upstream's game_engine list + its HAL libs ---------------------------
# engine_seed.txt: upstream's game_engine source list (includes the game's main(): gameloop_main.c
# before upstream's 2026-09 rewrite, gameloop.c after -- both listed; a missing one only warns)
seed_srcs = [P_.rebase(l.strip()).replace(P_.SRC, R) for l in
             open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'engine_seed.txt')) if l.strip()]
for h in ('hal/os_shim.c', 'hal/mm_shim.c', 'hal/vi_shim.c', 'hal/dvd_shim.c', 'hal/stfx_inflate.c',
          'hal/assets.c', 'hal/tex_decode.c', 'hal/gx_draw.c', 'hal/gx_wgpipe.cpp', 'hal/stfx_debug.c'):
    seed_srcs.append(f'{R}/src/port/src/{h}')
seed_srcs.append(f'{R}/src/port/src/hal/renderer/rhi.c')
# OVERRIDES must be seeded: the closure only pulls objects for UNDEFINED symbols, and an
# override's names are all defined already (by the game's own copies). Without this line the
# retail string pools never entered the link -- and pool_gate.py refused the build (watched).
seed_srcs.append(f'{R}/src/port/src/data/gsae01_string_pools.S')
# Retail-layout .bss blocks (gen_retail_blocks.py, device run 9): overrides, so seeded too.
seed_srcs.append(f'{R}/src/port/src/data/gsae01_retail_blocks.S')
# The game's rand/srand (run 124; see ALLOWED_MSL): seeded for the same reason -- newlib defines `rand`
# too, so the name is never undefined and the allowlist alone changed nothing (watched: the ELF's rand
# was still newlib's, the eboot sha unchanged).
seed_srcs.append(f'{R}/src/MSL_C/PPCEABI/bare/H/rand.c')
# The retail loading-screen data (boot_logo.c + gen_embedded.py's .inc), seeded for the SAME reason (2026-10-10, the
# platform-layer audit): scaffold/game_boot_externs.c defines gLoadingScreenTextures too (0x40000 zero bytes, weak), so the
# name is never undefined and the real object -- compiled every build -- never entered the link.
# NOT SEEDED YET (run 182, 2026-10-10): with the real data linked, initLoadingScreenTextures (dlls/engine/50/50.c:136) took a
# data abort at boot (+0x2a, the first store through OSGetArenaHi() - 0x40000). The blob is three retail Texture headers
# (big-endian) plus texels; the parser needs a byte-order rule and the arena copy (pi_videoinit.c:122) needs checking first.
# The SHADOWED report below keeps listing it.
# seed_srcs.append(f'{R}/src/main/boot_logo.c')
have = []
for s in seed_srcs:
    o = obj_id(s)
    if o in allobjs: have.append(o)
    else: print('seed source has no object (compile failed?):', s.replace(R + '/', ''))
have = list(dict.fromkeys(have))

def link(objs):
    lst = OUT + '/link_objs.txt'
    open(lst, 'w').write('\n'.join(objpath(o) for o in objs) + '\n' + '\n'.join(vobjs) + '\n')
    r = sh(f'arm-vita-eabi-g++ -Wl,-q @{lst} -o {OUT}/game_engine.elf {LIBS} 2>&1')
    und = set(re.findall(r"undefined reference to `([^']+)'", r.stdout))
    dup = set(re.findall(r"multiple definition of `([^']+)'", r.stdout))
    return r.returncode, und, dup, r.stdout

# --- duplicate resolution: PORT (hal/bridge) > GAME > SCAFFOLD ----------------------
# A port file that defines the same symbol as a game file REPLACES it on purpose
# (pad_input.c for pad.c, dvd_shim for fileio's DVD layer, game_model_support for
# modelEngine). So the symbol is weakened in the GAME object's copy and the port's
# strong definition wins. Game-vs-game duplicates keep the alphabetically-first
# object and weaken the rest. Every weakening is logged in dup_resolution.txt.
# RETRACTED 2026-09-22: "PORT > GAME" for all of src/port. It conflated two different
# things: hal/ is a PLATFORM replacement (memory, input, disc I/O) and must beat the game's
# console code; bridge/ is upstream's STAND-IN layer (the viewer harness) that real game
# code is meant to replace. Under the old rule 47 bridge definitions overrode real game
# code -- modelEngine.c ended up half real, half stand-in. Now:
#   HAL (port platform) 3  >  GAME 2  >  BRIDGE 1  >  SCAFFOLD 0
def prio(o):
    if 'scaffold' in o: return 0
    if o.startswith('src_port_src_bridge_'): return 1
    if o.startswith('src_port_'): return 3
    return 2
CLASS = {'SCAFFOLD': 0, 'BRIDGE': 1, 'GAME': 2, 'HAL': 3}
SYMPRIO = {}
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'symbol_priority.txt')):
    l = l.split('#')[0].split()
    if l: SYMPRIO[l[0]] = CLASS[l[1]]
dup_log = []
def resolve_dups(dups):
    changed = False
    for sym in sorted(dups):
        owners = sorted(o for o in have if 'scaffold' not in o and o in defs.get(sym, ()))
        if len(owners) < 2: continue
        top = max(prio(o) for o in owners)
        if sym in SYMPRIO:        # a decided case (symbol_priority.txt) beats the directory rule
            if not any(prio(o) == SYMPRIO[sym] for o in owners):
                sys.exit(f'symbol_priority.txt: {sym} has no {SYMPRIO[sym]}-class provider among {owners}')
            top = SYMPRIO[sym]
        keep = sorted(o for o in owners if prio(o) == top)[0]
        for o in owners:
            if o == keep: continue
            pending.setdefault(o, []).append(sym)
            dup_log.append(f'{sym}\tkept {keep}\tweakened in {o}')
            changed = True
    # ONE objcopy per object (all its symbols at once): the whole-.bss retail layout
    # (gen_retail_blocks.py --all) made ~1850 duplicates, and one call per symbol took ~20 min.
    for o, syms_ in sorted(pending.items()):
        src = objpath(o); dst = f'{PATCHED}/{o}'
        flags = ' '.join(f'--weaken-symbol={s}' for s in syms_)
        r = sh(f'arm-vita-eabi-objcopy {flags} {src} {dst}.tmp && mv {dst}.tmp {dst}')
        if r.returncode: sys.exit('weaken-symbol failed: ' + o + ' ' + ' '.join(syms_[:5]))
    pending.clear()
    return changed
pending = {}

rnd = 0
while True:
    rnd += 1
    rc, und, dup, log = link(have)
    add, nodef = [], []
    for sym in sorted(und):
        cands = sorted(defs.get(sym, ()))
        # the highest-priority provider (same order as duplicate resolution)
        pick = sorted(cands, key=lambda c: (-prio(c), c))[:1]
        if pick: add.append(pick[0])
        else: nodef.append(sym)
    add = [a for a in dict.fromkeys(add) if a not in have]
    print(f'round {rnd}: objects {len(have)+len(vobjs)}, undefined {len(und)}, duplicates {len(dup)}, adding {len(add)}')
    if rc == 0:
        break
    if not add:
        if dup and resolve_dups(dup):
            continue          # relink with the duplicates resolved
        break
    have += add

print('\n=== RESULT ===')
print(f'link rc={rc}  objects={len(have)+len(vobjs)}  undefined={len(und)}  duplicates={len(dup)}')
open(OUT + '/closure_objs.txt', 'w').write('\n'.join(have) + '\n')
open(OUT + '/dup_resolution.txt', 'w').write('\n'.join(dup_log) + '\n')
applied = {l.split('\t')[0] for l in dup_log}
for sym in sorted(SYMPRIO):   # an entry that never met a duplicate decided nothing: say so
    print(('symbol_priority: %-24s -> %s' % (sym, [k for k, v in CLASS.items() if v == SYMPRIO[sym]][0]))
          + ('' if sym in applied else '   NEVER APPLIED (no duplicate) -- stale entry?'))
print('duplicate symbols resolved by weakening:', len(dup_log))
# classify what is left
def textual_definers(sym):
    r = sh(f'grep -lw "{sym}" ' + ' '.join(failed[:200]) + ' 2>/dev/null')
    return [x.replace(R + '/', '') for x in r.stdout.split()]
rep = []
for sym in sorted(nodef):
    t = textual_definers(re.sub(r'\W', '', sym.split('(')[0])) if failed else []
    rep.append(('in-failed-source' if t else 'nowhere', sym, t[:2]))
open(OUT + '/unresolved.txt', 'w').write('\n'.join(f'{k}\t{s}\t{",".join(t)}' for k, s, t in rep) + '\n')
c = collections.Counter(k for k, _, _ in rep)
print('unresolved:', dict(c))
print('duplicates:', ' '.join(sorted(dup)[:30]))

# which scaffold stubs are still the ONLY provider of something the link uses?
r = sh(f'cd {WEAK} && ls *.o > {OUT}/nm_weak.rsp && arm-vita-eabi-nm -A --defined-only @{OUT}/nm_weak.rsp')
if r.returncode != 0: sys.exit(f'weak nm FAILED rc={r.returncode}: {r.stderr[-400:]}')   # same 32K trap
weak_syms = collections.defaultdict(set)
for line in r.stdout.splitlines():
    m = re.match(r'^(?:.*/)?([^/:]+\.o):[0-9a-fA-F]*\s+([VWvw])\s+(\S+)$', line.strip())
    if m and m.group(1) in have: weak_syms[m.group(1)].add(m.group(3))
strong_elsewhere = set()
for o in have:
    if 'scaffold' in o: continue
    strong_elsewhere |= {s for s, objs in defs.items() if o in objs}
still = {o: sorted(v - strong_elsewhere) for o, v in weak_syms.items()}
# ...and which of those SHADOW a real definition that was compiled but never linked? (the rand / gLoadingScreenTextures trap:
# the closure pulls objects only for UNDEFINED names, and a weak stub already in the link defines the name.)
shadowed = []
for o_, syms_ in still.items():
    for s_ in syms_:
        real = sorted(x for x in defs.get(s_, ()) if 'scaffold' not in x and x not in have and x.startswith(('src_main_', 'src_dlls_', 'src_track_')))
        if real: shadowed.append(f'{s_}  (stub in {o_}; real, compiled, NOT linked: {real[0]})')
print('scaffold stubs SHADOWING a compiled game definition:', len(shadowed))
for l_ in shadowed: print('  SHADOWED ', l_)
print('scaffold stubs still providing symbols:', sum(len(v) for v in still.values()),
      ' overridden by real code:', sum(len(weak_syms[o]) - len(still[o]) for o in still))

# --- MIXED objects: partly replaced by a HAL file, still supplying live symbols ------
# A HAL replacement is meant to take a game file WHOLE (mm_shim for mm.c, pad_input for
# pad.c). If it misses some names, the closure pulls the real file in for them, and that
# file's internals then run against state the HAL never built. The first device run of
# the game crashed exactly so (mm.c's heap walker, 2026-09-22). Reported, never fixed here.
weakened = collections.defaultdict(set)     # game object -> symbols a HAL winner took
for line in dup_log:
    sym, kept, lost = line.split('\t')
    kept, lost = kept.replace('kept ', ''), lost.replace('weakened in ', '')
    if prio(kept) == 3 and prio(lost) == 2: weakened[lost].add(sym)
mixed = []
for o, took in sorted(weakened.items()):
    live = sorted(s for s, objs in defs.items() if o in objs and s not in took)
    if live: mixed.append((o, len(took), live))
print(f'MIXED objects (HAL took some names, the rest still link): {len(mixed)}')
for o, n, live in mixed:
    print(f'  {o}: HAL took {n}; still live: {" ".join(live[:12])}{" ..." if len(live) > 12 else ""}')
