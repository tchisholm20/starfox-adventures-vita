#!/usr/bin/env python3
"""Compare every built object's per-section symbol ORDER against the retail carve.

Section 11 of docs/priced_classes.md measures that `.bss` layout is set by first-use
order and that declaration order is completely inert, which makes the carve's `.bss`
order a free oracle for the source text's use order. Nothing scores it: `.bss` has no
bytes, so objdiff's data score never sees a permutation, and neither does md5-of-every-`.o`
when the unit is not linked.

Prints the sections whose SHARED symbols appear in a different order than the retail
carve puts them. Each row is a claim that our TU touches those objects in the wrong
order -- a missing use, a use spelled against the wrong object, or a wrong TU boundary.

  python3 tools/bss_order_scan.py
"""

import os, subprocess, re
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OD=ROOT+'/build/binutils/powerpc-eabi-objdump'
SRC=ROOT+'/build/GSAE01/src'; OBJ=ROOT+'/build/GSAE01/obj'
def secs(p,raw=False):
    out=subprocess.run([OD,'-t',p],capture_output=True,text=True).stdout
    d={}
    for ln in out.splitlines():
        m=re.match(r'^([0-9a-f]{8}) (.{7}) +(\S+)\t([0-9a-f]{8}) (?:\.hidden )?(.+)$',ln)
        if not m: continue
        off,flags,sec,size,name=m.groups()
        if 'O' not in flags or sec.startswith('.debug') or sec=='*ABS*': continue
        if name.startswith('@') or name.startswith('.') or '$' in name: continue
        d.setdefault(sec,[]).append((int(off,16),int(size,16),name))
    return {k:sorted(v) for k,v in d.items()}


if __name__ == "__main__":
    targets=set()
    try:
        out=subprocess.run(['ninja','-C',ROOT,'-t','targets','all'],capture_output=True,text=True).stdout
        for ln in out.splitlines():
            t=ln.split(':',1)[0].strip()
            if t.endswith('.o'): targets.add(os.path.abspath(os.path.join(ROOT,t)))
    except OSError:
        pass
    bad=[]; names=[]
    for dp,_,fs in os.walk(SRC):
        for f in fs:
            if not f.endswith('.o'): continue
            ours=os.path.join(dp,f); rel=os.path.relpath(ours,SRC)
            if targets and os.path.abspath(ours) not in targets: continue
            th=os.path.join(OBJ,rel)
            if not os.path.isfile(th): continue
            a=secs(ours,True); b=secs(th,True)
            for sec in sorted(set(a)&set(b)):
                am={(o,z):n for o,z,n in a[sec]}; bm={(o,z):n for o,z,n in b[sec]}
                for k in sorted(set(am)&set(bm)):
                    if am[k]!=bm[k]: names.append((rel,sec,hex(k[0]),k[1],am[k],bm[k]))
            a={k:[x[2] for x in v] for k,v in a.items()}; b={k:[x[2] for x in v] for k,v in b.items()}
            for sec in sorted(set(a)&set(b)):
                an,bn=a[sec],b[sec]
                common=[n for n in an if n in bn]
                bcommon=[n for n in bn if n in an]
                if common!=bcommon and len(common)>1:
                    bad.append((rel,sec,common,bcommon))
    print(len(names),'symbols whose NAME differs from the retail carve at the same section/offset/size')
    for r in names: print('  %-52s %-8s %-6s %4d  ours=%-34s retail=%s'%r)
    print()
    print(len(bad),'sections whose SHARED symbol ORDER differs from the retail carve')
    for r in bad:
        print('\n%s  %s'%(r[0],r[1])); print('  ours  :',r[2]); print('  retail:',r[3])
