#!/usr/bin/env python3
"""wgpipe_gate.py -- FAIL if any code still builds the write-gather-pipe address.

The Vita has no fault trap for the GameCube's pipe at 0xCC008000; one store there is a
data abort (the game's first device runs, 2026-09-22: hudDrawRect, `strb r0, [ip]`,
ip = 0xCC008000). The fix turns stores into calls (wgpipe_rewrite.py + GXVert.h /
GXPriv.h). This gate proves it on the LINKED image: it disassembles the ELF and looks for
a register loaded with the address the way gcc builds it on ARMv7 -- movw #0x8000 then
movt #0xcc00 on the same register -- and prints each function that does.

    python wgpipe_gate.py ELF        exit 0 = none, 1 = found (listed), 2 = tool error
"""
import re, subprocess, sys

OBJDUMP = 'C:/dev/msys64/usr/local/vitasdk/bin/arm-vita-eabi-objdump.exe'
elf = sys.argv[1]
r = subprocess.run([OBJDUMP, '-d', elf], capture_output=True, text=True)
if r.returncode or not r.stdout:
    sys.exit(2)
fn, low, hits, insns = '?', {}, [], 0
# The low half comes as movw OR mov.w (a modified immediate). The first version of this
# gate matched only movw, and PASSED the very ELF that crashed (watched: 0 hits, exit 0).
mov = re.compile(r'\t(movw|movt|mov)(?:\.w)?\s+(\w+), #(\d+)')
for line in r.stdout.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
    if m: fn, low = m.group(1), {}; continue
    if '\t' not in line: continue
    insns += 1
    m = mov.search(line)
    if not m: continue
    op, reg, imm = m.group(1), m.group(2), int(m.group(3))
    if op in ('movw', 'mov'): low[reg] = imm
    elif imm == 0xCC00 and low.get(reg) == 0x8000:
        hits.append((fn, line.split(':')[0].strip()))
if insns < 100000:                      # a truncated disassembly must not read as "clean"
    print(f'only {insns} instructions disassembled -- refusing'); sys.exit(2)
print(f'{insns} instructions scanned; {len(hits)} loads of 0xCC008000')
for f, a in hits[:40]: print(f'  {a}  {f}')
sys.exit(1 if hits else 0)
