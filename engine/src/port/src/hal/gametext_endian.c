// gametext_endian.c - a loaded GAME TEXT file (gametext/<dir>/<name>.bin) to host byte order
// (vita fork, 2026-09-27).
//
// Game-text files are loaded by PATH (gametext.c: DVDReadAsync into a load slot), not through the
// MLDF resource ids hal/mldf_endian.c covers, and gameTextFinalizeLoad reads their big-endian
// structure natively. Vita run 73: the title menu drew its panels and highlight bars but NO
// LETTERS. This walks the file exactly as gameTextFinalizeLoad does and swaps every multi-byte
// field IN PLACE, before that function reads anything:
//   s32 glyphCount; TextGlyph[glyphCount] {u32 key; u16 u, v; 8 bytes};
//   u16 entryCount, stringDataSize; GameTextDef[entryCount] {u16 identifier, count; 4 bytes; s32 strings};
//   s32 stringCount; s32 offsets[stringCount]; u8 stringData[stringDataSize];
//   s32 paddingBytes; u8 padding[paddingBytes];
//   { u16 format, bitsPerPixel, width, height; texels (area * bpp / 8 bytes, left big-endian: the GX
//     texture decoder reads them that way) } ... until width == height == 0.
// Every count and offset is checked against the file size as it is read; a file that does not
// close exactly is REFUSED (logged, left untouched) rather than half-swapped.
#include "dolphin/types.h"
#include <stdio.h>

unsigned gametext_swapped, gametext_refused;

static inline u32 rd32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u16 rd16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static inline void sw32(u8* p) { u8 t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; }
static inline void sw16(u8* p) { u8 t = p[0]; p[0] = p[1]; p[1] = t; }

#define NEED(n) do { if ((u32)(q - b) + (u32)(n) > size) { why = #n; goto refuse; } } while (0)

// Returns 1 if swapped, 0 if refused (the buffer is then untouched).
int stfx_gametext_loaded(void* buf, u32 size) {
    u8* const b = (u8*)buf;
    const u8* q = b;
    const char* why = "";
    u32 glyphs, entries, strData, strings, pad, i, textures = 0;
    // pass 1: VALIDATE (reading big-endian), so a refusal leaves the file untouched
    NEED(4); glyphs = rd32(q); q += 4;
    if (glyphs > 20000) { why = "glyph count"; goto refuse; }
    NEED(glyphs * 16); q += glyphs * 16;
    if (glyphs == 0) { gametext_swapped++; sw32(b); return 1; }   // the game stops at count 0
    NEED(4); entries = rd16(q); strData = rd16(q + 2); q += 4;
    NEED(entries * 12); q += entries * 12;
    NEED(4); strings = rd32(q); q += 4;
    if (strings > 100000) { why = "string count"; goto refuse; }
    NEED(strings * 4); q += strings * 4;
    NEED(strData); q += strData;
    NEED(4); pad = rd32(q); q += 4;
    NEED(pad); q += pad;
    for (;;) {
        u32 w, h, bpp;
        NEED(8); bpp = rd16(q + 2); w = rd16(q + 4); h = rd16(q + 6); q += 8;
        if (w == 0 && h == 0) break;
        if ((bpp != 4 && bpp != 16) || w > 1024 || h > 1024) { why = "texture header"; goto refuse; }
        NEED(w * h * bpp / 8); q += w * h * bpp / 8;
        textures++;
    }
    // pass 2: SWAP, walking the same structure
    q = b;
    sw32((u8*)q); q += 4;
    for (i = 0; i < glyphs; i++, q += 16) { sw32((u8*)q); sw16((u8*)q + 4); sw16((u8*)q + 6); }
    sw16((u8*)q); sw16((u8*)q + 2); q += 4;
    for (i = 0; i < entries; i++, q += 12) { sw16((u8*)q); sw16((u8*)q + 2); sw32((u8*)q + 8); }
    sw32((u8*)q); q += 4;
    for (i = 0; i < strings; i++, q += 4) sw32((u8*)q);
    q += strData;
    sw32((u8*)q); q += 4 + pad;
    for (;;) {
        u32 w = rd16(q + 4), h = rd16(q + 6), bpp = rd16(q + 2);
        sw16((u8*)q); sw16((u8*)q + 2); sw16((u8*)q + 4); sw16((u8*)q + 6);
        q += 8;
        if (w == 0 && h == 0) break;
        q += w * h * bpp / 8;
    }
    gametext_swapped++;
    printf("[gametext] to host order: %u glyphs, %u entries, %u strings (%u bytes), %u textures, %u of %u bytes\n",
           glyphs, entries, strings, strData, textures, (unsigned)(q - b), size);
    return 1;
refuse:
    gametext_refused++;
    printf("[gametext] REFUSED (left big-endian): %s runs past the %u-byte file at +%u\n", why, size, (unsigned)(q - b));
    return 0;
}
