#!/usr/bin/env python3
"""named_diff_gate.py -- G-D1, the NAMED-DIFF REGISTER of the GX render thread (RT_THREAD.md 7.2).

The claim "direct mode is the build before, except for N1 and N2" is only as good as the proof that nothing else in the HAL or the backend moved.  This hashes the compiled
objects and compares them with the baseline (base_obj.sha: the HAL objects of the GXM build that preceded the render-thread work, hashed BEFORE the first rebuild; base_vobj_rt.sha: the backend objects
rebuilt from the BASELINE COMMIT's sources at the rt-copy path with closure.py's exact flags. The first baseline of the backend was built from the ORIGINAL tree's path
and differs by __FILE__ strings alone (found 2026-10-20: five rhi_gxm_*.o 'differed' with no source change), so a path-insensitive claim needs a same-path baseline).

    must be byte-identical : hal gx_tev, gx_light, vi_shim, os_threads, os_shim, renderer/rhi; the backend objects rhi_vita, rhi_gxm_frame/shader/texture/draw/target, plat_window_vita, backend_stubs
    may differ (NAMED)     : hal gx_draw (hunk N1: private fog colour), hal mm_shim (hunk N2: gxt_free in mm_free_now), vobj rhi_gxm.c.o (integrator item 3g: gxm_issued_draws)
    any other object that exists in the baseline and differs is REPORTED (the HAL's other objects are not in the register's must-list, but a change is still news).

    python named_diff_gate.py [--obj D] [--vobj D] [--base-obj F] [--base-vobj F]          exit 0 held, 14 broken, 2 cannot judge
    python named_diff_gate.py selftest                                                     watches the gate fail (a changed must-object, a missing baseline, an unnamed diff)
"""
import hashlib, os, sys, tempfile

import devcfg                                   # dev_defaults.env (developer machine only)
OUT = devcfg.need('OUT', os.environ.get('OUT') or devcfg.dev('TOOLS_OUT')).replace(chr(92), '/')
H = 'src_port_src_hal_'
MUST = [H + n + '_c.o' for n in ('gx_tev', 'gx_light', 'vi_shim', 'os_threads', 'os_shim', 'renderer_rhi')]
MUST_V = ['rhi_vita.c.o', 'rhi_gxm_frame.c.o', 'rhi_gxm_shader.c.o', 'rhi_gxm_texture.c.o', 'rhi_gxm_draw.c.o', 'rhi_gxm_target.c.o', 'plat_window_vita.c.o', 'backend_stubs.c.o']
NAMED = {H + 'gx_draw_c.o': 'N1 private fog colour', H + 'mm_shim_c.o': 'N2 gxt_free in mm_free_now + gxt_reclaim retry in mm_alloc_from', 'rhi_gxm.c.o': '3g gxm_issued_draws'}


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def read_base(p):
    d = {}
    for l in open(p, encoding='utf-8'):
        t = l.split()
        if len(t) == 2:
            d[t[1].lstrip('*')] = t[0]
    return d


def judge(objdir, vobjdir, base_obj, base_vobj, emit=print):
    bad, note = [], []
    for kind, d, base, must in (('obj', objdir, base_obj, MUST), ('vobj', vobjdir, base_vobj, MUST_V)):
        for n in must:
            if n not in base:
                bad.append('%s/%s is in the must-be-identical list but not in the baseline' % (kind, n)); continue
            p = os.path.join(d, n)
            if not os.path.exists(p):
                bad.append('%s/%s is missing' % (kind, n)); continue
            h = sha(p)
            emit('  %-9s %-40s %s' % ('IDENTICAL' if h == base[n] else 'DIFFERS', n, h[:12]))
            if h != base[n]:
                bad.append('%s/%s differs from the baseline (%s... vs %s...) and is not a named diff' % (kind, n, h[:12], base[n][:12]))
        for n, why in NAMED.items():
            if n in base and os.path.exists(os.path.join(d, n)):
                h = sha(os.path.join(d, n))
                emit('  %-9s %-40s %s  (named: %s)' % ('same' if h == base[n] else 'DIFFERS', n, h[:12], why))
        for n in sorted(base):   # everything else in the baseline
            if n in must or n in NAMED:
                continue
            p = os.path.join(d, n)
            if os.path.exists(p) and sha(p) != base[n] and n.startswith(H):
                note.append('%s/%s' % (kind, n))
    if note:
        emit('  note: HAL objects outside the register that also differ from the baseline (not a failure of the register, but news): ' + ', '.join(note))
    return bad


def main():
    a = sys.argv[1:]
    if a and a[0] == 'selftest':
        return selftest()
    opt = lambda k, d: (a[a.index(k) + 1] if k in a else d)
    bo, bv = opt('--base-obj', OUT + '/rt_integ/base_obj.sha'), opt('--base-vobj', OUT + '/rt_integ/base_vobj_rt.sha')
    if not (os.path.exists(bo) and os.path.exists(bv)):
        print('named_diff_gate: no baseline (%s, %s): cannot judge' % (bo, bv)); return 2
    bad = judge(opt('--obj', OUT + '/obj'), opt('--vobj', OUT + '/vobj'), read_base(bo), read_base(bv))
    for b in bad:
        print('NAMED-DIFF FAIL ' + b)
    print('named_diff_gate: %s' % ('FAIL (%d)' % len(bad) if bad else 'HELD (only N1, N2 and 3g may differ)'))
    return 14 if bad else 0


def selftest():
    t = tempfile.mkdtemp(prefix='ndg_')
    od, vd = os.path.join(t, 'o'), os.path.join(t, 'v'); os.makedirs(od); os.makedirs(vd)
    bo, bv = {}, {}
    for n in MUST + list(NAMED)[:2]:
        open(os.path.join(od, n), 'wb').write(n.encode()); bo[n] = sha(os.path.join(od, n))
    for n in MUST_V + ['rhi_gxm.c.o']:
        open(os.path.join(vd, n), 'wb').write(n.encode()); bv[n] = sha(os.path.join(vd, n))
    q = lambda *x: None
    ok = True
    def expect(name, bad, want_bad):
        nonlocal ok
        good = bool(bad) == want_bad
        ok &= good
        print('  %-58s %s' % (name, 'ok' if good else 'WRONG: ' + str(bad)))
    expect('unchanged tree holds', judge(od, vd, bo, bv, q), False)
    open(os.path.join(od, H + 'gx_draw_c.o'), 'wb').write(b'N1'); open(os.path.join(od, H + 'mm_shim_c.o'), 'wb').write(b'N2'); open(os.path.join(vd, 'rhi_gxm.c.o'), 'wb').write(b'3g')
    expect('the three NAMED diffs are allowed', judge(od, vd, bo, bv, q), False)
    p = os.path.join(od, H + 'vi_shim_c.o'); s = open(p, 'rb').read(); open(p, 'wb').write(s + b'x')
    expect('MUTANT: vi_shim.o changed -> must FAIL', judge(od, vd, bo, bv, q), True)
    open(p, 'wb').write(s)
    p = os.path.join(vd, 'rhi_gxm_draw.c.o'); s = open(p, 'rb').read(); open(p, 'wb').write(s + b'x')
    expect('MUTANT: backend rhi_gxm_draw.o changed -> must FAIL', judge(od, vd, bo, bv, q), True)
    open(p, 'wb').write(s)
    os.remove(os.path.join(od, H + 'gx_tev_c.o'))
    expect('MUTANT: gx_tev.o missing -> must FAIL', judge(od, vd, bo, bv, q), True)
    bo2 = dict(bo); del bo2[H + 'os_shim_c.o']
    open(os.path.join(od, H + 'gx_tev_c.o'), 'wb').write(H.encode() and ('gx_tev_c.o').encode())
    expect('MUTANT: must-object absent from the baseline -> must FAIL', judge(od, vd, bo2, bv, q), True)
    print('selftest: %s' % ('every mutant FAILED as it must' if ok else 'BROKEN'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
