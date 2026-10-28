"""Screen for the "inlined helper return" signature.

Retail compiles some loops with an UNCONDITIONAL back-edge where our build
emits a conditional one, because the original was a static-inline helper whose
`return` is the loop exit -- a shape no do/while can express. gameTextWrapLines
was found and fixed this way (98.039 -> 98.845, all 32 structural bytes).

Validated on that case: it fires pre-fix (ub=+2) and is silent post-fix.
`ub` is the reliable column; `cb` was NEGATIVE on the true positive, so do not
gate on it.

Reports, per function present in both the retail and our object:
  ub   = retail unconditional back-branches minus ours   (>0 == signature)
  cb   = ours conditional back-branches minus retail's    (>0 == signature)
  dlen = len(ours) - len(retail) instructions
Only functions whose bytes differ (i.e. sub-100) are reported.
"""
import re, subprocess, sys, os, glob
OBJDUMP = 'build/binutils/powerpc-eabi-objdump'
FN = re.compile(r'^([0-9a-f]{8}) <([^>]+)>:\n(.*?)(?=^[0-9a-f]{8} <|\Z)', re.S | re.M)
INS = re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} ){4})\t(.*)$')
BR  = re.compile(r'^(b|ba|bl|bdnz\+?|bdnz-?|b[a-z]{2,3}[+-]?)\s+([0-9a-f]+) <')

def parse(path):
    try:
        t = subprocess.run([OBJDUMP, '-M', 'gekko', '-drz', path],
                           capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return {}
    out = {}
    for m in FN.finditer(t):
        ins = []
        for l in m.group(3).split('\n'):
            k = INS.match(l)
            if k:
                ins.append((int(k.group(1), 16), k.group(2).replace(' ', ''), k.group(3).strip()))
        if ins:
            out[m.group(2)] = ins
    return out

def edges(ins):
    if not ins: return 0, 0
    lo, hi = ins[0][0], ins[-1][0]
    ub = cb = 0
    for addr, _, txt in ins:
        m = BR.match(txt)
        if not m: continue
        op, tgt = m.group(1), int(m.group(2), 16)
        if op == 'bl' or not (lo <= tgt <= hi): continue
        if tgt >= addr: continue                       # backward only
        if op == 'b': ub += 1
        else: cb += 1
    return ub, cb


if __name__ == "__main__":
    rows = []
    for ro in glob.glob('build/GSAE01/obj/**/*.o', recursive=True):
        co = ro.replace('build/GSAE01/obj/', 'build/GSAE01/src/', 1)
        if not os.path.exists(co): continue
        T, C = parse(ro), parse(co)
        if not T or not C: continue
        unit = ro[len('build/GSAE01/obj/'):-2]
        for name, ti in T.items():
            ci = C.get(name)
            if not ci: continue
            if [x[1] for x in ti] == [x[1] for x in ci]: continue   # byte-identical
            tub, tcb = edges(ti); cub, ccb = edges(ci)
            ub, cb = tub - cub, ccb - tcb
            if ub > 0 or cb > 0:
                rows.append((ub, cb, len(ci) - len(ti), len(ti), unit, name))
    rows.sort(key=lambda r: (-(r[0] + r[1]), -abs(r[2])))
    print("%3s %3s %5s %6s  %-42s %s" % ("ub", "cb", "dlen", "size", "unit", "function"))
    for r in rows:
        print("%3d %3d %5d %6d  %-42s %s" % (r[0], r[1], r[2], r[3], r[4], r[5]))
    print("\n-- %d candidate functions" % len(rows))
