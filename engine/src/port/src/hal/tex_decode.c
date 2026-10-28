// tex_decode.c - GX texture formats -> linear RGBA8. See tex_decode.h.

#include "port/tex_decode.h"
#include <stdlib.h>
#include <string.h>

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint8_t  x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }  // 5->8 bit
static inline uint8_t  x4(uint32_t v) { return (uint8_t)((v << 4) | v); }         // 4->8 bit
static inline uint8_t  x3(uint32_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); } // 3->8
static inline uint8_t  x6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }  // 6->8 bit

static void put(uint8_t* dst, int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = dst + ((size_t)y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

uint8_t* gxTexDecode(int fmt, int w, int h, const uint8_t* src) {
    if (w <= 0 || h <= 0 || !src) return NULL;
    uint8_t* dst = (uint8_t*)malloc((size_t)w * h * 4);
    if (!dst) return NULL;

    const uint8_t* s = src;
    switch (fmt) {
    // Intensity formats expand to (I, I, I, I) in GX -- alpha IS the intensity (the TEV sees TEXA = I).
    // 2026-09-27: I8 decoded alpha as 255, and I4 / IA4 were not decoded at all, so every game-text
    // glyph (I4 font pages) drew as a solid block with no texture (Vita run 77).
    case GX_TF_I8: // 8x4 tiles, 1 byte intensity
        // A whole tile is 4 rows of 8 texels; a row is 8 bytes in, each repeated four times, 32 bytes out:
        // two byte shuffles (2026-10-11: the THP movie's three planes come through here on the MAIN
        // thread for every decoded frame, 295,000 texels, and one bounds-checked put() per texel was
        // the cost). Integer vector code: GCC emits NEON for it as it stands (unlike float vectors).
        // Edge tiles keep the checked path. host/tests/thp_fast_test.c holds this to the old loop.
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 8) {
                if (tx + 8 <= w && ty + 4 <= h) {
                    typedef uint8_t V8u __attribute__((vector_size(8), aligned(1), may_alias));
                    typedef uint8_t V16u __attribute__((vector_size(16), aligned(1), may_alias));
                    for (int iy = 0; iy < 4; ++iy) {
                        uint8_t* d = dst + ((size_t)(ty + iy) * w + tx) * 4;
                        V8u v = *(const V8u*)s;
#ifdef TEX_I8_BREAK
                        *(V16u*)d = __builtin_shufflevector(v, v, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4);   // texel 3's alpha is texel 4's (the test must fail)
#else
                        *(V16u*)d = __builtin_shufflevector(v, v, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3);
#endif
                        *(V16u*)(d + 16) = __builtin_shufflevector(v, v, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7);
                        s += 8;
                    }
                    continue;
                }
                for (int iy = 0; iy < 4; ++iy)
                    for (int ix = 0; ix < 8; ++ix) {
                        uint8_t i = *s++;
                        put(dst, w, h, tx + ix, ty + iy, i, i, i, i);
                    }
            }
        return dst;

    case GX_TF_I4: // 8x8 tiles, 4 bits intensity, the HIGH nibble is the left texel
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int iy = 0; iy < 8; ++iy)
                    for (int ix = 0; ix < 8; ix += 2) {
                        uint8_t b = *s++, i0 = x4(b >> 4), i1 = x4(b & 0xf);
                        put(dst, w, h, tx + ix, ty + iy, i0, i0, i0, i0);
                        put(dst, w, h, tx + ix + 1, ty + iy, i1, i1, i1, i1);
                    }
        return dst;

    case GX_TF_IA4: // 8x4 tiles, 1 byte: alpha in the HIGH nibble, intensity in the low
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 8)
                for (int iy = 0; iy < 4; ++iy)
                    for (int ix = 0; ix < 8; ++ix) {
                        uint8_t b = *s++, a = x4(b >> 4), i = x4(b & 0xf);
                        put(dst, w, h, tx + ix, ty + iy, i, i, i, a);
                    }
        return dst;

    case GX_TF_IA8: // 4x4 tiles, 16-bit: ALPHA is the first byte, intensity the second. Run 258: this had them swapped (Dolphin
        // TextureDecoder_Generic.cpp DecodePixel_IA8 on the raw little-endian word: a = val & 0xFF = the first byte). The leaf sprites
        // (swaphol TEX0 289-295) decoded to an opaque quad with the silhouette as its colour: a leaf in a black box.
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int iy = 0; iy < 4; ++iy)
                    for (int ix = 0; ix < 4; ++ix) {
                        uint16_t v = be16(s); s += 2;
                        uint8_t a = (uint8_t)(v >> 8), i = (uint8_t)(v & 0xff);
                        put(dst, w, h, tx + ix, ty + iy, i, i, i, a);
                    }
        return dst;

    case GX_TF_RGB565: // 4x4 tiles, 16-bit
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int iy = 0; iy < 4; ++iy)
                    for (int ix = 0; ix < 4; ++ix) {
                        uint16_t v = be16(s); s += 2;
                        put(dst, w, h, tx + ix, ty + iy,
                            x5(v >> 11), x6((v >> 5) & 0x3f), x5(v & 0x1f), 255);
                    }
        return dst;

    case GX_TF_RGB5A3: // 4x4 tiles, 16-bit: top bit selects RGB555 (opaque) vs RGB4A3
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int iy = 0; iy < 4; ++iy)
                    for (int ix = 0; ix < 4; ++ix) {
                        uint16_t v = be16(s); s += 2;
                        uint8_t r, g, b, a;
                        if (v & 0x8000) { r = x5((v >> 10) & 0x1f); g = x5((v >> 5) & 0x1f); b = x5(v & 0x1f); a = 255; }
                        else            { a = x3((v >> 12) & 0x7); r = x4((v >> 8) & 0xf); g = x4((v >> 4) & 0xf); b = x4(v & 0xf); }
                        put(dst, w, h, tx + ix, ty + iy, r, g, b, a);
                    }
        return dst;

    case GX_TF_RGBA8: // 4x4 tiles, 64 bytes: 32 bytes AR pairs, then 32 bytes GB pairs
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4) {
                const uint8_t* ar = s;       // A,R for 16 texels
                const uint8_t* gb = s + 32;  // G,B for 16 texels
                for (int t = 0; t < 16; ++t) {
                    int ix = t & 3, iy = t >> 2;
                    put(dst, w, h, tx + ix, ty + iy, ar[t * 2 + 1], gb[t * 2 + 0], gb[t * 2 + 1], ar[t * 2 + 0]);
                }
                s += 64;
            }
        return dst;

    case GX_TF_CMPR: { // GC's DXT1 variant: 8x8 tiles of four 4x4 DXT1 sub-blocks
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx) {
                        // 8-byte DXT1 block: 2 big-endian RGB565 endpoints + 4 index bytes.
                        uint16_t c0 = be16(s), c1 = be16(s + 2);
                        uint8_t pr[4], pg[4], pb[4], pa[4];
                        int r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 0x3f), b0 = x5(c0 & 0x1f);
                        int r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 0x3f), b1 = x5(c1 & 0x1f);
                        pr[0]=(uint8_t)r0; pg[0]=(uint8_t)g0; pb[0]=(uint8_t)b0; pa[0]=255;
                        pr[1]=(uint8_t)r1; pg[1]=(uint8_t)g1; pb[1]=(uint8_t)b1; pa[1]=255;
                        if (c0 > c1) {
                            /* Run 255, checked against Dolphin (TextureDecoder_Generic.cpp DecodeDXTBlock, TextureDecoder_Util.h
                               DXTBlend): the GameCube blends 3/8 : 5/8, not DXT1's 1/3 : 2/3. */
                            pr[2]=(uint8_t)((r1*3+r0*5)>>3); pg[2]=(uint8_t)((g1*3+g0*5)>>3); pb[2]=(uint8_t)((b1*3+b0*5)>>3); pa[2]=255;
                            pr[3]=(uint8_t)((r0*3+r1*5)>>3); pg[3]=(uint8_t)((g0*3+g1*5)>>3); pb[3]=(uint8_t)((b0*3+b1*5)>>3); pa[3]=255;
                        } else {
                            pr[2]=(uint8_t)((r0+r1)/2); pg[2]=(uint8_t)((g0+g1)/2); pb[2]=(uint8_t)((b0+b1)/2); pa[2]=255;
                            /* ...and the transparent texel keeps the AVERAGE colour with alpha 0. This was DXT1's transparent
                               BLACK, so with GL_LINEAR every cut-out edge (foliage, sprites) filtered toward black. */
                            pr[3]=pr[2]; pg[3]=pg[2]; pb[3]=pb[2]; pa[3]=0;
                        }
                        for (int row = 0; row < 4; ++row) {
                            uint8_t bits = s[4 + row];
                            for (int col = 0; col < 4; ++col) {
                                int idx = (bits >> (6 - 2 * col)) & 3; // GC: MSB-first 2-bit pixels
                                put(dst, w, h, tx + sx * 4 + col, ty + sy * 4 + row,
                                    pr[idx], pg[idx], pb[idx], pa[idx]);
                            }
                        }
                        s += 8;
                    }
        return dst;
    }

    default:
        free(dst);
        return NULL; // unsupported (I4/IA4/palettized) - added later
    }
}

// ---- THP movie frames (2026-09-28) ------------------------------------------------------------
// The game draws a movie frame as THREE I8 textures (Y at TEXMAP0, U at 1, V at 2) combined by four
// TEV stages (thp/n_options.c THPPlayerDrawCurrentFrame; shader_dolphin.c addYUVVideoTevStages for
// the attract laptop's screen). The port's fixed-function path samples one map, so Vita run 79 drew
// the laptop screen flat GREEN. Here are those stages, in TEV integer arithmetic: a lerp is
// (a*(256-c') + b*c') >> 8 with c' = c + (c >> 7), unclamped stages keep a signed range.
//   stage 0 (U): c  = C0 + U*K0        a  = A0 - U*K0a        C0 = {-90, 0, -114, 135}, K0 = {0,0,0xE2,0x58}
//   stage 1 (V): c  = (c + V*K1) * 2   a -= V*K1a             K1 = {0xB3,0,0,0xB6}
//   stage 2 (Y): c  = clamp(c + Y)     a  = clamp(a + Y)
//   stage 3:     out = lerp(a, c, K2)  K2 = {0xFF,0,0xFF}: R, B from c; G from a
static inline int tevMul(int b, int c) { return (b * (c + (c >> 7))) >> 8; }
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline uint8_t i8At(const uint8_t* p, int wid, int x, int y) {   // GX I8: 8x4 tiles of 32 bytes
    return p[(y >> 2) * (wid * 4) + (x >> 3) * 32 + (y & 3) * 8 + (x & 7)];
}
uint8_t* gxTexDecodeYUV(const uint8_t* yp, const uint8_t* up, const uint8_t* vp, int w, int h) {
    if (!yp || !up || !vp || w <= 0 || h <= 0) return NULL;
    uint8_t* dst = (uint8_t*)malloc((size_t)w * h * 4);
    if (!dst) return NULL;
    const int hw = w / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int Y = i8At(yp, w, x, y), U = i8At(up, hw, x >> 1, y >> 1), V = i8At(vp, hw, x >> 1, y >> 1);
            int r = (-90 + tevMul(V, 0xB3)) * 2 + Y;
            int b = (-114 + tevMul(U, 0xE2)) * 2 + Y;
            int g = 135 - tevMul(U, 0x58) - tevMul(V, 0xB6) + Y;
            uint8_t* p = dst + ((size_t)y * w + x) * 4;
            p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b); p[3] = 255;
        }
    return dst;
}

int gxTexDecodeSelfTest(void) {
    // RGBA8: one 4x4 tile. texel 0 = (R,G,B,A)=(10,20,30,40), texel 5 = (1,2,3,4).
    uint8_t rgba8[64];
    memset(rgba8, 0, sizeof(rgba8));
    // AR pairs (bytes 0..31): [A,R] per texel; GB pairs (32..63): [G,B].
    rgba8[0] = 40; rgba8[1] = 10;   rgba8[32] = 20; rgba8[33] = 30;   // texel 0
    rgba8[10] = 4; rgba8[11] = 1;   rgba8[42] = 2;  rgba8[43] = 3;    // texel 5
    uint8_t* d = gxTexDecode(GX_TF_RGBA8, 4, 4, rgba8);
    if (!d) return 1;
    int ok = d[0] == 10 && d[1] == 20 && d[2] == 30 && d[3] == 40 &&
             d[5 * 4 + 0] == 1 && d[5 * 4 + 1] == 2 && d[5 * 4 + 2] == 3 && d[5 * 4 + 3] == 4;
    free(d);
    if (!ok) return 2;

    // RGB5A3 opaque white (0xFFFF -> 555 all ones -> 255,255,255,255).
    uint8_t rgb5a3[32];
    memset(rgb5a3, 0, sizeof(rgb5a3));
    rgb5a3[0] = 0xFF; rgb5a3[1] = 0xFF; // texel 0 = 0xFFFF
    d = gxTexDecode(GX_TF_RGB5A3, 4, 4, rgb5a3);
    if (!d) return 3;
    ok = d[0] == 255 && d[1] == 255 && d[2] == 255 && d[3] == 255;
    free(d);
    if (!ok) return 4;

    // CMPR: an 8x8 texture (4 sub-blocks of 8 bytes). Endpoints red/blue, indices
    // 0x1B -> per row columns pick palette 0,1,2,3. Check pixel(0,0)=red,(1,0)=blue.
    uint8_t cmpr[32];
    for (int blk = 0; blk < 4; ++blk) {
        uint8_t* p = cmpr + blk * 8;
        p[0] = 0xF8; p[1] = 0x00; // c0 = RGB565 red (0xF800), big-endian
        p[2] = 0x00; p[3] = 0x1F; // c1 = RGB565 blue (0x001F)
        p[4] = p[5] = p[6] = p[7] = 0x1B;
    }
    d = gxTexDecode(GX_TF_CMPR, 8, 8, cmpr);
    if (!d) return 5;
    ok = d[0] == 255 && d[1] == 0 && d[2] == 0 &&                 // (0,0) red
         d[4 + 0] == 0 && d[4 + 1] == 0 && d[4 + 2] == 255;       // (1,0) blue
    free(d);
    if (!ok) return 6;

    return 0;
}

// ---- EFB -> TEXTURE COPY: the encoder (2026-10-06, docs/47 "EFB copies, done right") ----
// GXCopyTex writes real texels into main memory: the game samples them AND reads them on the CPU
// (newshadows.c blurs its shadow copies). So a copy is encoded into `dest` in the layout of the
// texture format the copy format lands in, big-endian like every texel store here, and the
// ordinary decode/upload path takes it from there. Returns the bytes written, 0 for a format this
// does not encode (every Z format: there is no depth source yet) -- the caller counts those.
// The copy formats (GXEnum.h): 0x20|k takes ONE channel (R4 0x20, A8 0x27,
// R8 0x28, G8 0x29, B8 0x2A); the plain I formats take luma, in the EFB's studio range like the
// hardware (Y = 0.257R + 0.504G + 0.098B + 16).
static uint8_t copyLuma(const uint8_t* p) { return (uint8_t)((66 * p[0] + 129 * p[1] + 25 * p[2] + 128 + (16 << 8)) >> 8); }
size_t gxTexEncodeCopy(int fmt, int w, int h, const uint8_t* rgba /* top row first */, uint8_t* dest) {
    if (w <= 0 || h <= 0 || !rgba || !dest) return 0;
    int kind, bw, bh;   // kind: 0 I4-layout, 1 I8-layout, 4 RGB565, 5 RGB5A3, 6 RGBA8 (the game copies no IA format)
    int chan = -1;      // the single channel a CTF format takes (-1: luma)
    switch (fmt) {
        case 0x00: kind = 0; break;                 // I4
        case 0x20: kind = 0; chan = 0; break;       // R4
        case 0x01: kind = 1; break;                 // I8
        case 0x27: kind = 1; chan = 3; break;       // A8
        case 0x28: kind = 1; chan = 0; break;       // R8
        case 0x29: kind = 1; chan = 1; break;       // G8
        case 0x2A: kind = 1; chan = 2; break;       // B8
        case 0x04: kind = 4; break;                 // RGB565
        case 0x05: kind = 5; break;                 // RGB5A3
        case 0x06: kind = 6; break;                 // RGBA8
        default: return 0;
    }
    bw = (kind <= 1) ? 8 : 4; bh = (kind == 0) ? 8 : 4;
    uint8_t* d = dest;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            uint8_t blk[64]; int n = 0;
            if (kind == 6) memset(blk, 0, sizeof blk);
            for (int ty = 0; ty < bh; ++ty)
                for (int tx = 0; tx < bw; ++tx, ++n) {
                    int x = bx + tx, y = by + ty;
                    static const uint8_t zero[4] = { 0, 0, 0, 0 };
                    const uint8_t* p = (x < w && y < h) ? rgba + ((size_t)y * w + x) * 4 : zero;
                    uint8_t v = chan >= 0 ? p[chan] : copyLuma(p);
                    switch (kind) {
                        case 0: if (n & 1) d[n >> 1] |= v >> 4; else d[n >> 1] = (uint8_t)(v & 0xF0); break;
                        case 1: d[n] = v; break;
                        case 4: { unsigned c = (unsigned)(p[0] >> 3) << 11 | (unsigned)(p[1] >> 2) << 5 | (unsigned)(p[2] >> 3);
                                  d[n * 2] = (uint8_t)(c >> 8); d[n * 2 + 1] = (uint8_t)c; break; }
                        case 5: { unsigned c = p[3] >= 0xE0
                                      ? 0x8000u | (unsigned)(p[0] >> 3) << 10 | (unsigned)(p[1] >> 3) << 5 | (unsigned)(p[2] >> 3)
                                      : (unsigned)(p[3] >> 5) << 12 | (unsigned)(p[0] >> 4) << 8 | (unsigned)(p[1] >> 4) << 4 | (unsigned)(p[2] >> 4);
                                  d[n * 2] = (uint8_t)(c >> 8); d[n * 2 + 1] = (uint8_t)c; break; }
                        default: blk[n * 2] = p[3]; blk[n * 2 + 1] = p[0]; blk[32 + n * 2] = p[1]; blk[32 + n * 2 + 1] = p[2]; break;
                    }
                }
            if (kind == 6) memcpy(d, blk, 64);
            d += kind == 6 ? 64 : 32;
        }
    return (size_t)(d - dest);
}
// The rectangle as read back (sw x sh, `flip`: bottom row first, as glReadPixels gives it) -> the
// copy's size (dw x dh, top row first), each destination texel the AREA AVERAGE of the source
// pixels it covers (the hardware's 2x2 box for a mipmap copy; here the screen is also 1.5 x 1.13
// the EFB, so the box is whatever the ratio makes it).
void gxCopyResample(const uint8_t* src, int sw, int sh, int flip, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw; if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = { 0, 0, 0, 0 }, n = (unsigned)((x1 - x0) * (y1 - y0));
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = src + (size_t)(flip ? sh - 1 - yy : yy) * sw * 4;
                for (int xx = x0; xx < x1; ++xx) for (int c = 0; c < 4; ++c) acc[c] += row[xx * 4 + c];
            }
            for (int c = 0; c < 4; ++c) dst[((size_t)y * dw + x) * 4 + c] = (uint8_t)((acc[c] + n / 2) / n);
        }
    }
}
