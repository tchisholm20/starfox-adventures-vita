#!/usr/bin/env python3
"""Source-level dangling externs -- the half the nm screen cannot see.

tools/dangling_extern_check.py works from UND relocs, so it only sees an
extern that is actually REFERENCED. An `extern T foo;` that no code touches
emits no reloc at all, so a declaration naming a symbol that exists nowhere
is invisible to it -- and equally invisible to the compiler, the linker and
the DOL gate. synth_internal.h carried `extern const f32 lbl_803E8430;` for
an address absent from symbols.txt and from every split.

Definitions come from symbols.txt, from our own objects and from the retail
split objects, matching the nm screen's authority.

COVERAGE: the scan roots are ALL of game code. They used to be src/main,
src/track and their headers only -- so src/dlls, 724 .c files and 305 headers,
better than half the game tree, was outside the instrument entirely, and the
report of zero it produced said nothing about them. The library trees stay out
by policy, not by omission: their headers legitimately declare SDK subsystems
this game never links (THP, GBA, AX) and the EABI linker-generated symbols
(_SDA_BASE_, _e_bss), which are dangling by construction and would be nothing
but noise. Sweeping the full tree scores 94 such vendor declarations and, in
game code, 0.
"""
import glob
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = os.path.join(ROOT, 'build/binutils/powerpc-eabi-nm')

SCAN = ('src/main/**/*.c', 'src/main/**/*.h',
        'src/track/**/*.c', 'src/track/**/*.h',
        'src/dlls/**/*.c', 'src/dlls/**/*.h',
        'include/main/**/*.h', 'include/track/**/*.h',
        'include/dlls/**/*.h', 'include/game/**/*.h',
        'include/sys/**/*.h', 'include/util/**/*.h',
        'include/global.h', 'include/types.h')

EXTERN = re.compile(
    r'^extern\s+(?!"C")((?:const\s+|volatile\s+|unsigned\s+|signed\s+|struct\s+|union\s+|enum\s+)*'
    r'[A-Za-z_]\w*(?:\s*\*)*)\s+(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)\s*;',
    re.M)


def defined_names():
    names = set()
    for line in open(os.path.join(ROOT, 'config/GSAE01/symbols.txt')):
        m = re.match(r'\s*([A-Za-z_@$][\w@$.]*)\s*=', line)
        if m:
            names.add(m.group(1))
    objs = sorted(glob.glob(os.path.join(ROOT, 'build/GSAE01/obj/**/*.o'), recursive=True))
    objs += sorted(glob.glob(os.path.join(ROOT, 'build/GSAE01/src/**/*.o'), recursive=True))
    for i in range(0, len(objs), 200):
        out = subprocess.run([NM] + objs[i:i + 200], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 3 and p[1] not in ('U', 'w'):
                names.add(p[2])
    return names


def main():
    defined = defined_names()
    print('[known definitions %d]' % len(defined))
    hits = []
    seen = set()
    for pat in SCAN:
        for path in sorted(glob.glob(os.path.join(ROOT, pat), recursive=True)):
            if path in seen:
                continue
            seen.add(path)
            text = open(path, 'rb').read().decode('latin-1')
            text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
            text = re.sub(r'//[^\n]*', '', text)
            for m in EXTERN.finditer(text):
                name = m.group(3)
                # A function declaration carries a parameter list, not a ';'
                # right after the declarator, so EXTERN never matches one.
                if name not in defined:
                    hits.append((name, os.path.relpath(path, ROOT),
                                 m.group(0).strip()))
    print('[dangling declarations %d]' % len(hits))
    for name, path, decl in sorted(hits):
        print('%-30s %-46s %s' % (name, path, decl))


if __name__ == '__main__':
    main()
