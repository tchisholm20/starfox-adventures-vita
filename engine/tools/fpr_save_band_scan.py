#!/usr/bin/env python3
"""List retail functions whose saved-FPR set is not a contiguous band ending at f31.

MWCC's prologue emitter only produces f(32-N)..f31 (docs/foreign/model_skinning_provenance.md),
so any other save set in a retail object came from function-level assembly.
"""
import subprocess,re,glob,collections
objs=glob.glob('build/GSAE01/obj/**/*.o',recursive=True)
odd=[];total=0
for o in objs:
    out=subprocess.run(['build/binutils/powerpc-eabi-objdump','-M','gekko','-d',o],capture_output=True,text=True).stdout
    for m in re.finditer(r'\n[0-9a-f]+ <([^>]+)>:\n((?:.*\n){0,60})',out):
        name,body=m.group(1),m.group(2)
        if not re.search(r'stwu\s+r1,-',body.split('\n\n')[0][:600]): continue
        saves=[int(x) for x in re.findall(r'stfd\s+f(\d+),-?\d+\(r1\)',body)]
        saves=[s for s in saves if s>=14]
        if not saves: continue
        total+=1
        if max(saves)!=31 or sorted(set(saves))!=list(range(min(saves),32)):
            odd.append((o.split('obj/')[1],name,sorted(set(saves))))
print('functions with FPR saves:',total)
for r in odd: print(r)
