#!/usr/bin/env python3
"""wgpipe_rewrite.py -- turn write-gather-pipe STORES into CALLS, at build time.

Game code writes vertices straight into the GameCube's write-gather pipe:
    GXWGFifo.s16 = x1 << 2;
On the console GXWGFifo is hardware at 0xCC008000. The Windows port catches those stores
with a fault handler (VEH, wgpipe_trap.c); the Vita has NO user-mode fault trap, so each
store is a data abort -- the game's first device run died on exactly this, in hudDrawRect
(PC at `strb r0, [ip]` with ip = 0xCC008000, dump 2026-09-22).

So every such STATEMENT becomes a call into gx_draw's FIFO interpreter:
    gxfifo_push_s16(x1 << 2);
The argument converts to the member type exactly as the assignment did. Source files are
NOT modified: compile_all.sh compiles the rewritten copy (with -iquote to the original
directory, and #line so diagnostics name the original). Anything that mentions GXWGFifo
and is NOT a plain store statement is REFUSED (exit 2) -- nothing is skipped silently.
The inline GXPosition*/GXColor* functions and GX_WRITE_* macros are fixed in the headers
(GXVert.h / GXPriv.h, vita fork), not here.

    python wgpipe_rewrite.py IN.c OUT.c
"""
import re, sys

TYPES = ('u8', 's8', 'u16', 's16', 'u32', 's32', 'f32')
# `GXWGFifo.T =` and the cast form `(*(PPCWGPipe2*)&GXWGFifo).T =` (skystars.c)
STORE = re.compile(r'(?:GXWGFifo|\(\s*\*\s*\(\s*\w+\s*\*\s*\)\s*&\s*GXWGFifo\s*\))\s*\.\s*('
                   + '|'.join(TYPES) + r')\s*=(?!=)')
# ...and the RAW form, `*(volatile f32*)0xCC008000 = x;` (rcp_dolphin.c). The link gate
# (wgpipe_gate.py) found it AFTER the first rewrite: this scanner had only looked for the
# NAME GXWGFifo, so a store through the bare address walked past it.
RAW_T = {'f32': 'f32', 'float': 'f32', 'u8': 'u8', 's8': 's8', 'u16': 'u16', 's16': 's16',
         'u32': 'u32', 's32': 's32', 'unsigned char': 'u8', 'unsigned short': 'u16',
         'unsigned int': 'u32', 'short': 's16', 'int': 's32', 'signed char': 's8'}
RAW = re.compile(r'\*\s*\(\s*volatile\s+(' + '|'.join(sorted(RAW_T, key=len, reverse=True))
                 + r')\s*\*\s*\)\s*0[xX][cC][cC]008000\s*=(?!=)')
# Mentions that are NOT stores and stay harmless once every store is a call: a local
# `extern ... GXWGFifo AT_ADDRESS(...);` declaration, or a local `#define GXWGFifo (...)`.
DECL = re.compile(r'^([ \t]*)(?:extern[ \t]+)?((?:volatile[ \t]+)?\w+[ \t]+GXWGFifo)[ \t]*:[ \t]*\([ \t]*'
                  r'0[xX][cC][cC]008000[ \t]*\)[ \t]*;', re.M)
ALLOWED = re.compile(r'^[ \t]*(?:extern\b[^;\n]*\bGXWGFifo\b[^;\n]*;[ \t\r]*|#[ \t]*define[ \t]+GXWGFifo\b.*)$', re.M)
PROTO = ('void gxfifo_push_u8(unsigned char v); void gxfifo_push_s8(signed char v); '
         'void gxfifo_push_u16(unsigned short v); void gxfifo_push_s16(short v); '
         'void gxfifo_push_u32(unsigned int v); void gxfifo_push_s32(int v); '
         'void gxfifo_push_f32(float v);   /* wgpipe_rewrite.py */\n')

def strip_comments_keep_len(s):
    """Blank out comments and string/char literals (same length) so the scanner
    never matches inside them."""
    out, i, n = list(s), 0, len(s)
    while i < n:
        if s.startswith('//', i):
            j = s.find('\n', i); j = n if j < 0 else j
            for k in range(i, j): out[k] = ' '
            i = j
        elif s.startswith('/*', i):
            j = s.find('*/', i + 2); j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n': out[k] = ' '
            i = j
        elif s[i] in '"\'':
            q, j = s[i], i + 1
            while j < n and s[j] != q:
                j += 2 if s[j] == '\\' else 1
            for k in range(i + 1, min(j, n)): out[k] = ' '
            i = j + 1
        else:
            i += 1
    return ''.join(out)

# ---- MWCC inline assembly (upstream's 100%-matching tree, 2026-09) -------------------------------
# Function-level `asm T name(...) { ... }` bodies and their `asm T name(...);` prototypes are removed
# from the build copy (newlines kept, so #line stays true). The port supplies each one in C
# (src/port/src/hal/model_asm_twins.c, bridge/game_scene.cpp); a missing twin is an undefined symbol
# at link, never a silent no-op. Statement-level `asm { ... }` blocks inside C functions are replaced
# ONLY from this table, keyed by the block's instructions (whitespace-normalised). Any other block
# is REFUSED.
INLINE_ASM = {
    # model.c setGQR6/setGQR7: the quantisation registers the psq_l/psq_st skinning kernels read.
    # The C twins read this RAM shadow instead (retail semantics: the LAST value written).
    'mtspr GQR6, config': '{ extern unsigned int gStfxGQR[8]; gStfxGQR[6] = config; }',
    'mtspr GQR7, config': '{ extern unsigned int gStfxGQR[8]; gStfxGQR[7] = config; }',
    # pi_videoinit.c videoConfigureProcessor: MSR[PM] + HID0[SPD] (performance monitor, speculative
    # cache access). No meaning on the Vita's Cortex-A9.
    'mfmsr control ori control, control, MSR_PM mtmsr control mfspr control, HID0 '
    'ori control, control, HID0_SPD mtspr HID0, control': '{ (void)control; }',
}
ASM_FN = re.compile(r'^[ \t]*asm[ \t]+[A-Za-z_][^;{()]*\(', re.M)
ASM_BLOCK = re.compile(r'\basm[ \t\r\n]*\{')


def _blank(s, a, b):
    """s with [a, b) replaced by its own newlines only."""
    return s[:a] + '\n' * s.count('\n', a, b) + s[b:]


def _match_brace(code, i):
    """index just past the '}' matching code[i] == '{'"""
    depth = 0
    for j in range(i, len(code)):
        if code[j] == '{':
            depth += 1
        elif code[j] == '}':
            depth -= 1
            if depth == 0:
                return j + 1
    raise SystemExit('REFUSED: unbalanced braces after offset %d' % i)


def strip_mwcc_asm(src):
    """-> (src', [names of removed asm functions], n inline blocks replaced)"""
    names, n_inline = [], 0
    while True:
        code = strip_comments_keep_len(src)
        m = ASM_FN.search(code)
        if not m:
            break
        name = re.search(r'([A-Za-z_]\w*)\s*\($', code[m.start():m.end()]).group(1)
        k = code.find(')', m.end() - 1)
        depth, k = 0, m.end() - 1
        while k < len(code):                       # the parameter list may span lines
            if code[k] == '(':
                depth += 1
            elif code[k] == ')':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        rest = code[k + 1:].lstrip()
        if rest.startswith(';'):
            end = code.index(';', k) + 1
        elif rest.startswith('{'):
            end = _match_brace(code, code.index('{', k))
            names.append(name)
        else:
            raise SystemExit('REFUSED: asm function %s is neither a prototype nor a definition' % name)
        # leave the signature as an ordinary C prototype, so C callers in this TU keep a declaration
        # (model.c calls modelBlendMorphTargetChunk); pad with the removed region's other newlines
        proto = re.sub(r'^[ \t]*asm[ \t]+', '', src[m.start():k + 1]) + ';'
        src = src[:m.start()] + proto + '\n' * (src.count('\n', m.start(), end) - proto.count('\n')) + src[end:]
    while True:
        code = strip_comments_keep_len(src)
        m = ASM_BLOCK.search(code)
        if not m:
            break
        end = _match_brace(code, m.end() - 1)
        key = ' '.join(code[m.end():end - 1].split())
        if key not in INLINE_ASM:
            line = src.count('\n', 0, m.start()) + 1
            raise SystemExit('REFUSED line %d: inline asm block with no port replacement: %r' % (line, key[:80]))
        repl = INLINE_ASM[key]
        src = src[:m.start()] + repl + '\n' * src.count('\n', m.start(), end) + src[end:]
        n_inline += 1
    return src, names, n_inline


def rewrite(src):
    code = strip_comments_keep_len(src)
    edits, pos = [], 0
    found = [(m, m.group(1)) for m in STORE.finditer(code)] + \
            [(m, RAW_T[' '.join(m.group(1).split())]) for m in RAW.finditer(code)]
    found.sort(key=lambda mt: mt[0].start())
    for m, t in found:
        # the expression runs to the ';' at bracket depth 0
        depth, j = 0, m.end()
        while j < len(code):
            c = code[j]
            if c in '([{': depth += 1
            elif c in ')]}': depth -= 1
            elif c == ';' and depth == 0: break
            if depth < 0: raise SystemExit(f'REFUSED: store at offset {m.start()} is not a statement')
            j += 1
        # must be a statement: preceded (ignoring space) by ; { } or a label/else/)
        k = m.start() - 1
        while k >= 0 and code[k] in ' \t\r\n': k -= 1
        if k >= 0 and code[k] not in ';{}):' and not code[:k + 1].endswith('else'):
            line = src.count('\n', 0, m.start()) + 1
            raise SystemExit(f'REFUSED line {line}: GXWGFifo store is not a plain statement')
        edits.append((m.start(), j, t, src[m.end():j].strip()))
    out, last = [], 0
    for a, b, t, expr in edits:
        out.append(src[last:a]); out.append(f'gxfifo_push_{t}({expr})'); last = b
    out.append(src[last:])
    res = ''.join(out)
    # MWCC's absolute-address DECLARATION `[extern] [volatile] T GXWGFifo : (0xCC008000);` is not C:
    # in the build copy it becomes a plain extern (one host definition, gx_wgpipe.cpp). The vita
    # fork used to edit each upstream file to AT_ADDRESS by hand; upstream's 2026-09 rewrite moved
    # those declarations into new files (shader.c's has no `extern` at all), so the class is fixed
    # here instead. One line in, one line out: #line numbering is unchanged.
    res = DECL.sub(r'\1extern \2;', res)
    scan = ALLOWED.sub(lambda mm: ' ' * len(mm.group(0)), strip_comments_keep_len(res))
    left = [res.count('\n', 0, mm.start()) + 1 for mm in re.finditer(r'GXWGFifo|0[xX][cC][cC]008000', scan)]
    if left:
        raise SystemExit(f'REFUSED: GXWGFifo / 0xCC008000 still used on lines {left[:10]} (not a plain store)')
    return res, len(edits)

def main():
    src_path, out_path = sys.argv[1], sys.argv[2]
    src = open(src_path, encoding='utf-8', errors='surrogateescape').read()
    src, asm_names, n_inline = strip_mwcc_asm(src)
    res, n = rewrite(src)
    with open(out_path, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(PROTO + f'#line 1 "{src_path}"\n' + res)
    print(f'{n} stores -> calls: {src_path}')
    if asm_names or n_inline:
        print(f'asm functions removed (port twins must define them): {" ".join(asm_names) or "-"}; '
              f'inline asm blocks replaced: {n_inline}')

if __name__ == '__main__':
    main()
