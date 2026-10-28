// thp_decode.c - the THP (attract movie) VIDEO decoder in portable C (vita fork).
//
// The game's THP player (src/main/thp/*) is real C and is linked. Its decoder, the SDK's THPDec
// (upstream src/dolphin/thp/THPDec.c), is C for the marker parsing but Gekko paired-single
// assembly for the Huffman decode and the IDCT (15 asm functions). This file is that decoder in
// C, written against the retail code's own semantics:
//
//   - the stream is a baseline-JPEG subset: SOI, DQT, DHT, SOF0 (8-bit, 3 components, Y 2x2,
//     U/V 1x1), optional DRI, one SOS; APPn/COM skipped (retail THPVideoDecode's marker loop);
//   - NO byte stuffing: retail reads the entropy data as whole 32-bit words and never looks for
//     FF 00; a restart interval only rounds the bit position UP to the next byte and resets the
//     DC predictors (retail: cnt = 1 + ((cnt + 6) & ~7));
//   - quantisation tables are pre-scaled by the AAN factors (retail __THPReadQuantizationTable),
//     the IDCT is the AAN float IDCT, output = x/8 + 128 saturated to u8 (retail stores through
//     GQR6 = u8, scale 2^-3, after adding a 1024 bias);
//   - output planes are GX I8 TEXTURES: 8x4-pixel tiles of 32 bytes, tile rows contiguous
//     (retail writes a block at obase + xPos*4, its lower half at + wid*4). Y is W x H, U and V
//     are W/2 x H/2.
//
// Checked on the host against ffmpeg's THP decoder (host/tests/thp_decode_test.c).
#include "dolphin/types.h"
#include <string.h>
#include <stdlib.h>

unsigned thp_video_decode_calls, thp_video_decode_errors, thp_audio_decode_calls;
extern void gx_tex_invalidate(const void* addr, u32 n) __attribute__((weak));
extern void gx_tex_yuv_register(const void* y, const void* u, const void* v, int w, int h) __attribute__((weak));
// 2026-10-11 (docs/47 "movie upload"): the same, with the three planes already expanded to RGBA by the worker
// (hal/tex_decode.c gxTexDecode I8; ownership passes to gx_draw, which frees them). NULL = it is not linked (host tests).
extern void gx_tex_yuv_register_x(const void* y, const void* u, const void* v, int w, int h,
                                  unsigned char* ey, unsigned char* eu, unsigned char* ev) __attribute__((weak));
extern unsigned char* gxTexDecode(int fmt, int w, int h, const unsigned char* src) __attribute__((weak));

typedef struct {
    const u8* vij;          // symbol values, in code order
    s32 maxCode[18];        // largest code of each length, -1 if none (index 1..16; [17] sentinel)
    s32 valPtr[18];         // vij index of length l's first code, minus that code
} ThpHuff;

typedef struct {
    const u8* p;            // next byte of entropy data
    const u8* end;
    u32 bits;               // bit buffer, MSB-aligned
    s32 nbits;
} ThpBits;

typedef struct {
    u16 width, height;
    u8 quantSel[3], dcSel[3], acSel[3];
    s32 predDC[3];
    f32 quant[4][64];
    ThpHuff huff[4][2];     // [table id][class 0 = DC, 1 = AC]
    u32 validHuff;          // bit (id*2 + class)
    u16 restartInterval;
} ThpState;

static const u8 kNatural[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const double kAAN[8] = {1.0, 1.387039845, 1.306562965, 1.175875602,
                               1.0, 0.785694958, 0.541196100, 0.275899379};

static inline u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

static int readDQT(ThpState* s, const u8* p) {
    s32 len = be16(p) - 2;
    p += 2;
    while (len >= 65) {
        u8 id = *p++ & 3;
        f32 q[64];
        int i, r, c;
        for (i = 0; i < 64; i++) q[kNatural[i]] = (f32)*p++;
        for (r = 0, i = 0; r < 8; r++)
            for (c = 0; c < 8; c++, i++) s->quant[id][i] = (f32)((double)q[i] * kAAN[r] * kAAN[c]);
        len -= 65;
    }
    return len == 0 ? 0 : 13;
}

static int readDHT(ThpState* s, const u8* p) {
    s32 len = be16(p) - 2;
    p += 2;
    while (len > 17) {
        u8 tc = *p >> 4, id = *p & 15;
        const u8* bits = p + 1;
        int l, n = 0, code = 0, k = 0;
        ThpHuff* h;
        if (id > 3 || tc > 1) return 14;
        h = &s->huff[id][tc];
        for (l = 0; l < 16; l++) n += bits[l];
        h->vij = p + 17;
        for (l = 1; l <= 16; l++) {         // canonical codes (JPEG Annex C), length by length
            int cnt = bits[l - 1];
            if (cnt) {
                h->valPtr[l] = k - code;
                code += cnt;
                k += cnt;
                h->maxCode[l] = code - 1;
            } else {
                h->maxCode[l] = -1;
                h->valPtr[l] = -1;
            }
            code <<= 1;
        }
        h->maxCode[17] = 0x7fffffff;
        s->validHuff |= 1u << (id * 2 + tc);
        p += 17 + n;
        len -= 17 + n;
    }
    return len == 0 ? 0 : 14;
}

static inline void fill(ThpBits* b) {
    while (b->nbits <= 24) {
        u32 byte = b->p < b->end ? *b->p++ : 0;
        b->bits |= byte << (24 - b->nbits);
        b->nbits += 8;
    }
}

static inline u32 getBits(ThpBits* b, int n) {
    u32 v;
    if (n == 0) return 0;
    fill(b);
    v = b->bits >> (32 - n);
    b->bits <<= n;
    b->nbits -= n;
    return v;
}

static inline int decodeHuff(ThpBits* b, const ThpHuff* h) {
    s32 code, l = 1;
    fill(b);
    code = (s32)(b->bits >> 31);
    while (code > h->maxCode[l]) {
        if (++l > 16) return -1;
        code = (s32)(b->bits >> (32 - l));
    }
    b->bits <<= l;
    b->nbits -= l;
    return h->vij[h->valPtr[l] + code];
}

static inline s32 extend(u32 v, int s) { return (s && v < (1u << (s - 1))) ? (s32)v - (1 << s) + 1 : (s32)v; }

static int decodeBlock(ThpState* s, ThpBits* b, int comp, s16* blk) {
    const ThpHuff* dc = &s->huff[s->dcSel[comp]][0];
    const ThpHuff* ac = &s->huff[s->acSel[comp]][1];
    int t, k;
    memset(blk, 0, 64 * sizeof(s16));
    t = decodeHuff(b, dc);
    if (t < 0) return 16;
    s->predDC[comp] += extend(getBits(b, t), t);
    blk[0] = (s16)s->predDC[comp];
    for (k = 1; k < 64;) {
        int rs = decodeHuff(b, ac), r, sz;
        if (rs < 0) return 16;
        r = rs >> 4;
        sz = rs & 15;
        if (sz == 0) {
            if (r != 15) break;             // EOB
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return 17;
        blk[kNatural[k]] = (s16)extend(getBits(b, sz), sz);
        k++;
    }
    return 0;
}

static inline u8 clampU8(f32 v) {
    s32 i = (s32)v;                         // truncation toward zero, like the u8 store
    return (u8)(i < 0 ? 0 : i > 255 ? 255 : i);
}

// AAN float IDCT (IJG jidctflt.c structure) on a dequantised block, written as an 8x8 block into
// an I8-tiled plane of width `wid` at pixel (x0, y0).
static void idctStore(const s16* in, const f32* q, u8* plane, u32 wid, u32 x0, u32 y0) {
    f32 ws[64];
    int i;
    for (i = 0; i < 8; i++) {               // columns
        const s16* c = in + i;
        f32* w = ws + i;
        const f32* qq = q + i;
        f32 t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z5, z10, z11, z12, z13;
        if (!c[8] && !c[16] && !c[24] && !c[32] && !c[40] && !c[48] && !c[56]) {
            f32 dc = c[0] * qq[0];
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
            continue;
        }
        t0 = c[0] * qq[0];  t1 = c[16] * qq[16]; t2 = c[32] * qq[32]; t3 = c[48] * qq[48];
        t10 = t0 + t2; t11 = t0 - t2; t13 = t1 + t3; t12 = (t1 - t3) * 1.414213562f - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;
        t4 = c[8] * qq[8];  t5 = c[24] * qq[24]; t6 = c[40] * qq[40]; t7 = c[56] * qq[56];
        z13 = t6 + t5; z10 = t6 - t5; z11 = t4 + t7; z12 = t4 - t7;
        t7 = z11 + z13; t11 = (z11 - z13) * 1.414213562f;
        z5 = (z10 + z12) * 1.847759065f;
        t10 = z12 * 1.082392200f - z5; t12 = z10 * -2.613125930f + z5;
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 + t5;
        w[0] = t0 + t7; w[56] = t0 - t7; w[8] = t1 + t6; w[48] = t1 - t6;
        w[16] = t2 + t5; w[40] = t2 - t5; w[32] = t3 + t4; w[24] = t3 - t4;
    }
    for (i = 0; i < 8; i++) {               // rows, +1024 bias, /8
        f32* w = ws + i * 8;
        f32 t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z5, z10, z11, z12, z13;
        u32 y = y0 + i;
        u8* row = plane + (y >> 2) * (wid * 4) + (y & 3) * 8;
        u8 o[8];
        int k;
        t10 = w[0] + w[4] + 1024.0f; t11 = w[0] - w[4] + 1024.0f;
        t13 = w[2] + w[6]; t12 = (w[2] - w[6]) * 1.414213562f - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;
        z13 = w[5] + w[3]; z10 = w[5] - w[3]; z11 = w[1] + w[7]; z12 = w[1] - w[7];
        t7 = z11 + z13; t11 = (z11 - z13) * 1.414213562f;
        z5 = (z10 + z12) * 1.847759065f;
        t10 = z12 * 1.082392200f - z5; t12 = z10 * -2.613125930f + z5;
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 + t5;
        o[0] = clampU8((t0 + t7) * 0.125f); o[7] = clampU8((t0 - t7) * 0.125f);
        o[1] = clampU8((t1 + t6) * 0.125f); o[6] = clampU8((t1 - t6) * 0.125f);
        o[2] = clampU8((t2 + t5) * 0.125f); o[5] = clampU8((t2 - t5) * 0.125f);
        o[4] = clampU8((t3 + t4) * 0.125f); o[3] = clampU8((t3 - t4) * 0.125f);
        for (k = 0; k < 8; k++) row[((x0 + k) >> 3) * 32 + ((x0 + k) & 7)] = o[k];
    }
}

// ---- the frame's markers (both decoders) -------------------------------------------------------
// Error codes follow retail THPVideoDecode's (3 bad syntax, 11 unsupported marker, 25/27 no
// input/output); 16/17 are this decoder's own (bad Huffman code / coefficient overrun), 30 = the
// decode was told to stop (thpWorkerCancel: the movie is being torn down).
static int parseHeaders(ThpState* s, const u8** pc) {
    const u8* c = *pc;
    int err = 0, haveSOF = 0, done = 0;
    while (!done) {
        u8 m;
        if (*c++ != 0xFF) return 3;
        while (*c == 0xFF) c++;
        m = *c++;
        switch (m) {
        case 0xD8: break;                                   // SOI
        case 0xDB: if ((err = readDQT(s, c))) return err; c += be16(c); break;
        case 0xC4: if ((err = readDHT(s, c))) return err; c += be16(c); break;
        case 0xC0: {                                        // SOF0
            int i;
            if (c[2] != 8) return 10;
            s->height = be16(c + 3);
            s->width = be16(c + 5);
            if (c[7] != 3) return 12;
            for (i = 0; i < 3; i++) {
                u8 hv = c[8 + i * 3 + 1];
                if ((i == 0 && hv != 0x22) || (i > 0 && hv != 0x11)) return 19;
                s->quantSel[i] = c[8 + i * 3 + 2] & 3;
            }
            haveSOF = 1;
            c += be16(c);
            break;
        }
        case 0xDD: s->restartInterval = be16(c + 2); c += be16(c); break;   // DRI
        case 0xDA: {                                        // SOS: the entropy data follows
            int i;
            if (c[2] != 3) return 12;
            for (i = 0; i < 3; i++) {
                u8 t = c[3 + i * 2 + 1];
                s->dcSel[i] = t >> 4;
                s->acSel[i] = t & 15;
                if (s->dcSel[i] > 3 || s->acSel[i] > 3 ||
                    !(s->validHuff & (1u << (s->dcSel[i] * 2))) || !(s->validHuff & (1u << (s->acSel[i] * 2 + 1))))
                    return 15;
            }
            c += be16(c);
            done = 1;
            break;
        }
        default:
            if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE) { c += be16(c); break; }
            return 11;
        }
    }
    if (!haveSOF) return 3;
    *pc = c;
    return 0;
}

// ---- the REFERENCE decoder: bit-serial Huffman, the float IDCT one pixel at a time. It is what
// ran until 2026-10-11 and what movie_fastdec_off.txt selects; the fast decoder below is held to
// its output byte for byte (host/tests/thp_fast_test.c, all 1797 frames of the disc movie).
static s32 decodeRef(const u8* c, u8* tileY, u8* tileU, u8* tileV, int* w, int* h, const volatile int* stop) {
    ThpState s;
    ThpBits b;
    int err;
    s16 blk[64];
    u32 mcuX, mcuY, mcusPerRow, mcuRows, sinceRestart = 0;
    memset(&s, 0, sizeof(s));
    if ((err = parseHeaders(&s, &c))) return err;
    b.p = c;
    b.end = c + (u32)s.width * s.height * 2;   // a bound past any real frame: never read wild memory
    b.bits = 0;
    b.nbits = 0;
    mcusPerRow = (s.width + 15) / 16;
    mcuRows = (s.height + 15) / 16;
    for (mcuY = 0; mcuY < mcuRows; mcuY++) {
        if (stop && *stop) return 30;
        for (mcuX = 0; mcuX < mcusPerRow; mcuX++) {
            u32 x = mcuX * 16, y = mcuY * 16;
            if ((err = decodeBlock(&s, &b, 0, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[0]], tileY, s.width, x, y);
            if ((err = decodeBlock(&s, &b, 0, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[0]], tileY, s.width, x + 8, y);
            if ((err = decodeBlock(&s, &b, 0, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[0]], tileY, s.width, x, y + 8);
            if ((err = decodeBlock(&s, &b, 0, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[0]], tileY, s.width, x + 8, y + 8);
            if ((err = decodeBlock(&s, &b, 1, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[1]], tileU, s.width / 2, x / 2, y / 2);
            if ((err = decodeBlock(&s, &b, 2, blk))) return err;
            idctStore(blk, s.quant[s.quantSel[2]], tileV, s.width / 2, x / 2, y / 2);
            if (s.restartInterval && ++sinceRestart == s.restartInterval) {
                // retail: round the bit position up to a byte (no marker bytes), reset predictors
                int drop = b.nbits & 7;
                b.bits <<= drop;
                b.nbits -= drop;
                s.predDC[0] = s.predDC[1] = s.predDC[2] = 0;
                sinceRestart = 0;
            }
        }
    }
    *w = s.width;
    *h = s.height;
    return 0;
}

// ---- the FAST decoder (2026-10-11, docs/47 "the movie decode off the baton") ---------------------
// Same stream, same arithmetic, the same bytes out -- only the work is organised differently:
//   - Huffman: one 9-bit table lookup per symbol -- for an AC coefficient whose code and value bits
//     together fit 9 bits, the lookup IS the coefficient (value, run, bits used); a code longer than
//     9 bits (about 1 in 100 in the disc movie) takes the reference's canonical search. Code and
//     value bits come out of ONE 32-bit big-endian window read at the bit position (no refill loop);
//   - coefficients are dequantised as they are decoded (coef * q is the reference's first IDCT
//     multiply, done only for the ~8 that are not zero), and the block keeps masks of the rows and
//     columns that hold any;
//   - a DC-only block is one value, a block with only its first row or first column is one 1-D
//     pass, and the rest take the float AAN IDCT four lanes at a time (GCC vector extensions: NEON
//     on the Vita, SSE on the host -- the same C, so the host comparison covers the Vita's code);
//   - an 8x8 block at a multiple of 8 is exactly two half GX tiles: rows are stored 8 bytes at a
//     time, never a pixel at a time.
// WHY THE IDCT STAYED FLOAT (the brief was fixed point): every operation is the reference's, in the
// reference's order, on IEEE single floats -- a lane-wise add, subtract or multiply has no rounding
// freedom, the Cortex-A9 has no FUSED multiply-add (its vmla rounds the product first), and
// -ffast-math is not among the project's flags -- so the planes are IDENTICAL to the reference's,
// not "within a bound". A fixed-point IDCT would differ by +-1 somewhere in most frames, and a
// 16-bit NEON one would be at most ~2x faster on a part that is now a minority of the decode.
#define THP_LUT_BITS 9
typedef struct {
    u16 e[1 << THP_LUT_BITS];       // (code length << 8) | symbol; 0 = not in the table
    s16 ac[1 << THP_LUT_BITS];      // AC tables: a whole coefficient whose code AND value bits fit the index --
                                    // (value << 8) | (run << 4) | bits consumed; 0 = take the two-step path
} ThpLut;
typedef struct { const u8* base; const u8* end; u32 pos; } ThpFastBits;   // pos: BITS consumed from base
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(THP_NO_NEON)
#include <arm_neon.h>
typedef float32x4_t thp_v4f;
#else
typedef f32 thp_v4f __attribute__((vector_size(16)));
#endif
typedef int thp_v4i __attribute__((vector_size(16)));
typedef f32 thp_v4fu __attribute__((vector_size(16), aligned(4), may_alias));
typedef int thp_v4iu __attribute__((vector_size(16), aligned(1), may_alias));
unsigned thp_fast_blocks[5];    // blocks by path: 0 DC only, 1 first row only, 2 first column only, 3 full IDCT, 4 Huffman codes off the table

static void buildLut(ThpLut* t, const ThpHuff* h, int isAc) {
    const u8* bits = h->vij - 16;                           // the DHT's 16 counts sit just before its symbols
    u32 code = 0, k = 0;
    int l;
    memset(t, 0, sizeof(*t));
    for (l = 1; l <= THP_LUT_BITS; l++) {
        u32 cnt = bits[l - 1], i;
        if (code + cnt > (1u << l)) return;                 // not a prefix code: what is left takes the reference search
        for (i = 0; i < cnt; i++, code++, k++) {
            u32 first = code << (THP_LUT_BITS - l), n = 1u << (THP_LUT_BITS - l), j;
            u16 e = (u16)(l << 8 | h->vij[k]);
#ifdef THP_FAST_BREAK_1
            if (l == 5 && i == 0) e ^= 1;                   // ONE symbol of ONE code length wrong (the test must fail)
#endif
            for (j = 0; j < n; j++) t->e[first + j] = e;
        }
        code <<= 1;
    }
    if (isAc) {
        u32 i;
        for (i = 0; i < (1u << THP_LUT_BITS); i++) {
            u32 e = t->e[i], len = e >> 8, run = (e >> 4) & 15, sz = e & 15;
            if (e && sz && len + sz <= THP_LUT_BITS) {
                u32 v = ((i << len) & ((1u << THP_LUT_BITS) - 1)) >> (THP_LUT_BITS - sz);
                s32 val = extend(v, (int)sz);
                if (val >= -128 && val <= 127) t->ac[i] = (s16)(val * 256 + (s32)(run * 16 + len + sz));
            }
        }
    }
}

// The next 32 bits of the stream, MSB first; at least 25 of them are real (the reference's fill()).
static inline u32 fastPeek(const ThpFastBits* b) {
    const u8* p = b->base + (b->pos >> 3);
    u32 w;
    if (__builtin_expect(p + 4 <= b->end, 1)) {
        memcpy(&w, p, 4);
        w = __builtin_bswap32(w);
    } else {                                                // past the bound: zeros, as the reference reads
        int i;
        for (w = 0, i = 0; i < 4; i++) w = w << 8 | (p + i < b->end ? p[i] : 0u);
    }
    return w << (b->pos & 7);
}

static int slowHuff(u32 w, const ThpHuff* h, int* len) {    // the reference's search, on a window
    s32 code = (s32)(w >> 31), l = 1;
    while (code > h->maxCode[l]) {
        if (++l > 16) return -1;
        code = (s32)(w >> (32 - l));
    }
    *len = (int)l;
    ++thp_fast_blocks[4];
    return h->vij[h->valPtr[l] + code];
}

#define THP_C1 1.414213562f
#define THP_C2 1.847759065f
#define THP_C3 1.082392200f
#ifdef THP_FAST_BREAK_3
#define THP_C4 -2.6131265f                                  // ONE constant two float steps off (the test must fail)
#else
#define THP_C4 -2.613125930f
#endif
// LANE OPERATIONS. S_ = one f32. V_ = four f32, one per lane, each lane the same IEEE operation.
// On the Vita the V_ set is NEON INTRINSICS, not operators: GCC's ARM backend lowers `a + b` on a
// float vector to four scalar VFP operations unless -funsafe-math-optimizations is given (NEON has no
// denormals), and that flag would also licence reassociation. Measured, not assumed: the first
// version used operators and the Vita object held 370 scalar float operations and no vector one.
// The intrinsics are not what the host runs, so the Vita OBJECT is run too, in a CPU emulator
// (host/tests/thp_arm_emu.py), against the same baseline planes.
#define S_ADD(a, b)  ((a) + (b))
#define S_SUB(a, b)  ((a) - (b))
#define S_MULC(a, c) ((a) * (c))
#define S_ADDC(a, c) ((a) + (c))
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(THP_NO_NEON)
#define V_ADD(a, b)  vaddq_f32(a, b)
#define V_SUB(a, b)  vsubq_f32(a, b)
#define V_MULC(a, c) vmulq_f32(a, vdupq_n_f32(c))
#define V_ADDC(a, c) vaddq_f32(a, vdupq_n_f32(c))
#define V_LOAD(p)    vld1q_f32(p)
#define V_TRUNC(v)   ((thp_v4i)vcvtq_s32_f32(v))            /* toward zero, like (s32)v */
#define V_TR4(a, b, c, d) do { /* transpose four rows of four */ \
    float32x4x2_t p_ = vtrnq_f32(a, b), q_ = vtrnq_f32(c, d);    /* p_.val[0] = a0 b0 a2 b2, p_.val[1] = a1 b1 a3 b3 */ \
    a = vcombine_f32(vget_low_f32(p_.val[0]), vget_low_f32(q_.val[0])); \
    b = vcombine_f32(vget_low_f32(p_.val[1]), vget_low_f32(q_.val[1])); \
    c = vcombine_f32(vget_high_f32(p_.val[0]), vget_high_f32(q_.val[0])); \
    d = vcombine_f32(vget_high_f32(p_.val[1]), vget_high_f32(q_.val[1])); } while (0)
#else
#define V_ADD  S_ADD
#define V_SUB  S_SUB
#define V_MULC S_MULC
#define V_ADDC S_ADDC
#define V_LOAD(p)  (*(const thp_v4fu*)(p))
#define V_TRUNC(v) __builtin_convertvector(v, thp_v4i)
#define V_TR4(a, b, c, d) do { \
    thp_v4f p_ = __builtin_shuffle(a, b, (thp_v4i){0, 4, 1, 5}), q_ = __builtin_shuffle(a, b, (thp_v4i){2, 6, 3, 7}), \
            r_ = __builtin_shuffle(c, d, (thp_v4i){0, 4, 1, 5}), s_ = __builtin_shuffle(c, d, (thp_v4i){2, 6, 3, 7}); \
    a = __builtin_shuffle(p_, r_, (thp_v4i){0, 1, 4, 5}); b = __builtin_shuffle(p_, r_, (thp_v4i){2, 3, 6, 7}); \
    c = __builtin_shuffle(q_, s_, (thp_v4i){0, 1, 4, 5}); d = __builtin_shuffle(q_, s_, (thp_v4i){2, 3, 6, 7}); } while (0)
#endif
// The reference's 1-D pass (idctStore above), operation for operation and in its order, over a lane
// type T with the operation set OP (S or V). BIASED: the row pass adds 1024 to the two even sums,
// where the reference does; SCALED: the row pass's * 0.125.
#define THP_IDCT_1D(T, OP, i0, i1, i2, i3, i4, i5, i6, i7, o0, o1, o2, o3, o4, o5, o6, o7, BIASED, SCALED) do { \
    T t0_, t1_, t2_, t3_, t4_, t5_, t6_, t7_, t10_, t11_, t12_, t13_, z5_, z10_, z11_, z12_, z13_; \
    t10_ = OP##_ADD(i0, i4); t11_ = OP##_SUB(i0, i4); \
    if (BIASED) { t10_ = OP##_ADDC(t10_, 1024.0f); t11_ = OP##_ADDC(t11_, 1024.0f); } \
    t13_ = OP##_ADD(i2, i6); t12_ = OP##_SUB(OP##_MULC(OP##_SUB(i2, i6), THP_C1), t13_); \
    t0_ = OP##_ADD(t10_, t13_); t3_ = OP##_SUB(t10_, t13_); t1_ = OP##_ADD(t11_, t12_); t2_ = OP##_SUB(t11_, t12_); \
    z13_ = OP##_ADD(i5, i3); z10_ = OP##_SUB(i5, i3); z11_ = OP##_ADD(i1, i7); z12_ = OP##_SUB(i1, i7); \
    t7_ = OP##_ADD(z11_, z13_); t11_ = OP##_MULC(OP##_SUB(z11_, z13_), THP_C1); \
    z5_ = OP##_MULC(OP##_ADD(z10_, z12_), THP_C2); \
    t10_ = OP##_SUB(OP##_MULC(z12_, THP_C3), z5_); t12_ = OP##_ADD(OP##_MULC(z10_, THP_C4), z5_); \
    t6_ = OP##_SUB(t12_, t7_); t5_ = OP##_SUB(t11_, t6_); t4_ = OP##_ADD(t10_, t5_); \
    o0 = OP##_ADD(t0_, t7_); o7 = OP##_SUB(t0_, t7_); o1 = OP##_ADD(t1_, t6_); o6 = OP##_SUB(t1_, t6_); \
    o2 = OP##_ADD(t2_, t5_); o5 = OP##_SUB(t2_, t5_); o4 = OP##_ADD(t3_, t4_); o3 = OP##_SUB(t3_, t4_); \
    if (SCALED) { \
        o0 = OP##_MULC(o0, 0.125f); o1 = OP##_MULC(o1, 0.125f); o2 = OP##_MULC(o2, 0.125f); o3 = OP##_MULC(o3, 0.125f); \
        o4 = OP##_MULC(o4, 0.125f); o5 = OP##_MULC(o5, 0.125f); o6 = OP##_MULC(o6, 0.125f); o7 = OP##_MULC(o7, 0.125f); \
    } } while (0)

// The two half tiles an 8x8 block at (x0, y0) covers: rows y0..y0+3 are 32 contiguous bytes at
// tile[0], rows y0+4..y0+7 at tile[1], 8 bytes a row (x0 and y0 are multiples of 8).
static inline u8* blockTile(u8* plane, u32 wid, u32 x0, u32 y0, int half) {
    return plane + ((y0 >> 2) + (u32)half) * (wid * 4) + (x0 >> 3) * 32;
}

static inline thp_v4i cvtClamp(thp_v4f v) {                 // the reference's clampU8, four at a time
    thp_v4i i = V_TRUNC(v);
    thp_v4i m = i < 0;
    i &= ~m;
    m = i > 255;
    return (i & ~m) | (m & 255);
}

// The full IDCT of a dequantised block (natural order, +0 where there is no coefficient).
static void idctFull(const f32* in, u8* plane, u32 wid, u32 x0, u32 y0) {
    thp_v4f W[8][2];                                        // after the columns: W[r][h] = row r, columns 4h..4h+3
    int h, g;
    for (h = 0; h < 2; h++) {
        const f32* c = in + h * 4;
        thp_v4f i0 = V_LOAD(c), i1 = V_LOAD(c + 8), i2 = V_LOAD(c + 16), i3 = V_LOAD(c + 24), i4 = V_LOAD(c + 32),
                i5 = V_LOAD(c + 40), i6 = V_LOAD(c + 48), i7 = V_LOAD(c + 56);
        THP_IDCT_1D(thp_v4f, V, i0, i1, i2, i3, i4, i5, i6, i7, W[0][h], W[1][h], W[2][h], W[3][h], W[4][h], W[5][h],
                    W[6][h], W[7][h], 0, 0);
    }
    for (g = 0; g < 2; g++) {                               // rows 4g..4g+3, one per lane
        thp_v4f a0 = W[4 * g][0], a1 = W[4 * g + 1][0], a2 = W[4 * g + 2][0], a3 = W[4 * g + 3][0];
        thp_v4f a4 = W[4 * g][1], a5 = W[4 * g + 1][1], a6 = W[4 * g + 2][1], a7 = W[4 * g + 3][1];
        thp_v4f o0, o1, o2, o3, o4, o5, o6, o7;
        thp_v4i lo, hi;
        u8* tile = blockTile(plane, wid, x0, y0, g);
        V_TR4(a0, a1, a2, a3);                              // a_j: lane k = w[4g + k][j]
        V_TR4(a4, a5, a6, a7);
        THP_IDCT_1D(thp_v4f, V, a0, a1, a2, a3, a4, a5, a6, a7, o0, o1, o2, o3, o4, o5, o6, o7, 1, 1);
        // four pixels of a row in one little-endian word per lane: lane k of lo = row k, columns 0..3
        lo = cvtClamp(o0) | cvtClamp(o1) << 8 | cvtClamp(o2) << 16 | cvtClamp(o3) << 24;
        hi = cvtClamp(o4) | cvtClamp(o5) << 8 | cvtClamp(o6) << 16 | cvtClamp(o7) << 24;
        *(thp_v4iu*)tile = __builtin_shuffle(lo, hi, (thp_v4i){0, 4, 1, 5});            // rows 0 and 1 of the half tile
        *(thp_v4iu*)(tile + 16) = __builtin_shuffle(lo, hi, (thp_v4i){2, 6, 3, 7});     // rows 2 and 3
    }
}

// One block: decode, dequantise, transform, store. `blk` is 64 floats of +0 on entry and on exit.
// (The bit position lives in a local for the whole block: the byte stores below may alias anything.)
static inline int fastBlock(ThpState* s, ThpFastBits* bits, const ThpLut* dcl, const ThpLut* acl, int comp, f32* blk,
                            u8* plane, u32 wid, u32 x0, u32 y0) {
    const ThpHuff* dch = &s->huff[s->dcSel[comp]][0];
    const ThpHuff* ach = &s->huff[s->acSel[comp]][1];
    const f32* q = s->quant[s->quantSel[comp]];
    ThpFastBits b = *bits;
    u8 idx[64];
    u32 w, v, rows = 1, cols = 1, e;
    int t, k, n = 0, len, nat;
    f32 dc;

    w = fastPeek(&b);
    e = dcl->e[w >> (32 - THP_LUT_BITS)];
    if (e) { len = (int)(e >> 8); t = (int)(e & 0xFF); }
    else if ((t = slowHuff(w, dch, &len)) < 0) return 16;
    if (t > 16) return 16;                                  // (no DC category is wider than 11 bits)
    if (t) {
        if (len + t > 24) { b.pos += (u32)len; w = fastPeek(&b); len = 0; }
        v = (w << len) >> (32 - t);
        s->predDC[comp] += extend(v, t);
    }
    b.pos += (u32)(len + t);
    dc = (f32)(s16)s->predDC[comp] * q[0];

    for (k = 1; k < 64;) {
        int rs, r, sz, fa;
        w = fastPeek(&b);
        fa = acl->ac[w >> (32 - THP_LUT_BITS)];
        if (fa) {                                           // code + value in one lookup
            k += (fa >> 4) & 15;
            if (k > 63) { while (n) blk[idx[--n]] = 0.0f; return 17; }
            b.pos += (u32)(fa & 15);
            nat = kNatural[k];
            blk[nat] = (f32)(fa >> 8) * q[nat];
        } else {
            e = acl->e[w >> (32 - THP_LUT_BITS)];
            if (e) { len = (int)(e >> 8); rs = (int)(e & 0xFF); }
            else if ((rs = slowHuff(w, ach, &len)) < 0) { while (n) blk[idx[--n]] = 0.0f; return 16; }
            r = rs >> 4;
            sz = rs & 15;
            if (sz == 0) {
                b.pos += (u32)len;
                if (r != 15) break;                         // EOB
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) { while (n) blk[idx[--n]] = 0.0f; return 17; }
            if (len + sz > 24) { b.pos += (u32)len; w = fastPeek(&b); len = 0; }
            v = (w << len) >> (32 - sz);
            b.pos += (u32)(len + sz);
            nat = kNatural[k];
            blk[nat] = (f32)(s16)extend(v, sz) * q[nat];
        }
        idx[n++] = (u8)nat;
        rows |= 1u << (nat >> 3);
        cols |= 1u << (nat & 7);
        k++;
    }
    bits->pos = b.pos;

    if (n == 0) {                                           // DC only: every pixel is (dc + 1024) / 8
#ifdef THP_FAST_BREAK_2
        u8 val = clampU8((dc + 1032.0f) * 0.125f);          // one grey level too bright (the test must fail)
#else
        u8 val = clampU8((dc + 1024.0f) * 0.125f);
#endif
        memset(blockTile(plane, wid, x0, y0, 0), val, 32);
        memset(blockTile(plane, wid, x0, y0, 1), val, 32);
        ++thp_fast_blocks[0];
        return 0;
    }
    blk[0] = dc;
    if (rows == 1) {                                        // only the first row: the columns are constant, all 8 rows are one row pass
        f32 o0, o1, o2, o3, o4, o5, o6, o7;
        u8 o[8];
        u8* t0 = blockTile(plane, wid, x0, y0, 0);
        u8* t1 = blockTile(plane, wid, x0, y0, 1);
        int i;
        THP_IDCT_1D(f32, S, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7], o0, o1, o2, o3, o4, o5, o6, o7, 1, 1);
        o[0] = clampU8(o0); o[1] = clampU8(o1); o[2] = clampU8(o2); o[3] = clampU8(o3);
        o[4] = clampU8(o4); o[5] = clampU8(o5); o[6] = clampU8(o6); o[7] = clampU8(o7);
        for (i = 0; i < 4; i++) { memcpy(t0 + i * 8, o, 8); memcpy(t1 + i * 8, o, 8); }
        ++thp_fast_blocks[1];
    } else if (cols == 1) {                                 // only the first column: one column pass, every row is one value
        f32 o0, o1, o2, o3, o4, o5, o6, o7;
        u8* t0 = blockTile(plane, wid, x0, y0, 0);
        u8* t1 = blockTile(plane, wid, x0, y0, 1);
        THP_IDCT_1D(f32, S, blk[0], blk[8], blk[16], blk[24], blk[32], blk[40], blk[48], blk[56], o0, o1, o2, o3, o4, o5, o6, o7, 0, 0);
        memset(t0, clampU8((o0 + 1024.0f) * 0.125f), 8);      memset(t0 + 8, clampU8((o1 + 1024.0f) * 0.125f), 8);
        memset(t0 + 16, clampU8((o2 + 1024.0f) * 0.125f), 8); memset(t0 + 24, clampU8((o3 + 1024.0f) * 0.125f), 8);
        memset(t1, clampU8((o4 + 1024.0f) * 0.125f), 8);      memset(t1 + 8, clampU8((o5 + 1024.0f) * 0.125f), 8);
        memset(t1 + 16, clampU8((o6 + 1024.0f) * 0.125f), 8); memset(t1 + 24, clampU8((o7 + 1024.0f) * 0.125f), 8);
        ++thp_fast_blocks[2];
    } else {
        idctFull(blk, plane, wid, x0, y0);
        ++thp_fast_blocks[3];
    }
    blk[0] = 0.0f;
    while (n) blk[idx[--n]] = 0.0f;
    return 0;
}

static s32 decodeFast(const u8* c, u8* tileY, u8* tileU, u8* tileV, int* w, int* h, const volatile int* stop) {
    ThpState s;
    ThpLut lut[4][2];
    ThpFastBits b;
    f32 blk[64] __attribute__((aligned(16)));
    const ThpLut *dcl[3], *acl[3];
    int err, i;
    u32 mcuX, mcuY, mcusPerRow, mcuRows, sinceRestart = 0, wid, cwid;
    const u8* file = c;
    memset(&s, 0, sizeof(s));
    if ((err = parseHeaders(&s, &c))) return err;
    if ((s.width | s.height) & 15) return decodeRef(file, tileY, tileU, tileV, w, h, stop);   // (never: the movie is 512x384)
    for (i = 0; i < 8; i++)
        if (s.validHuff & (1u << i)) buildLut(&lut[i >> 1][i & 1], &s.huff[i >> 1][i & 1], i & 1);
    for (i = 0; i < 3; i++) { dcl[i] = &lut[s.dcSel[i]][0]; acl[i] = &lut[s.acSel[i]][1]; }
    memset(blk, 0, sizeof(blk));
    b.base = c;
    b.end = c + (u32)s.width * s.height * 2;                // the reference's bound
    b.pos = 0;
    wid = s.width;
    cwid = wid / 2;
    mcusPerRow = wid / 16;
    mcuRows = s.height / 16;
    for (mcuY = 0; mcuY < mcuRows; mcuY++) {
        if (stop && *stop) return 30;
        for (mcuX = 0; mcuX < mcusPerRow; mcuX++) {
            u32 x = mcuX * 16, y = mcuY * 16;
            if ((err = fastBlock(&s, &b, dcl[0], acl[0], 0, blk, tileY, wid, x, y))) return err;
            if ((err = fastBlock(&s, &b, dcl[0], acl[0], 0, blk, tileY, wid, x + 8, y))) return err;
            if ((err = fastBlock(&s, &b, dcl[0], acl[0], 0, blk, tileY, wid, x, y + 8))) return err;
            if ((err = fastBlock(&s, &b, dcl[0], acl[0], 0, blk, tileY, wid, x + 8, y + 8))) return err;
            if ((err = fastBlock(&s, &b, dcl[1], acl[1], 1, blk, tileU, cwid, x / 2, y / 2))) return err;
            if ((err = fastBlock(&s, &b, dcl[2], acl[2], 2, blk, tileV, cwid, x / 2, y / 2))) return err;
            if (s.restartInterval && ++sinceRestart == s.restartInterval) {
                b.pos = (b.pos + 7) & ~7u;                  // retail: up to the next byte, predictors reset
                s.predDC[0] = s.predDC[1] = s.predDC[2] = 0;
                sinceRestart = 0;
            }
        }
    }
    *w = s.width;
    *h = s.height;
    return 0;
}

// A frame's bytes -> its three planes, and nothing else (no counter, no GX call): what the decode
// thread runs, and what the host tests call. fast 0 = the reference decoder.
s32 thp_decode_planes(int fast, const void* file, void* tileY, void* tileU, void* tileV, int* w, int* h,
                      const volatile int* stop) {
    return fast ? decodeFast((const u8*)file, (u8*)tileY, (u8*)tileU, (u8*)tileV, w, h, stop)
                : decodeRef((const u8*)file, (u8*)tileY, (u8*)tileU, (u8*)tileV, w, h, stop);
}

// ---- OFF THE BATON (2026-10-11, docs/47 "the movie decode off the baton") ------------------------
// The game's video decoder (thp/THPVideoDecode.c AttractMovieVideo_Decoder) is an OSThread: it holds
// the baton (hal/os_threads.c) for as long as THPVideoDecode takes, ~80 ms a frame on the Vita, and
// it outranks main -- hardware run 211: frames alternate 33 and 83 ms. The decode itself touches
// nothing of the game's (frame bytes in, three planes out), so it runs on a Vita thread of its own
// that is NOT an OSThread and never holds the baton, on another core:
//
//   game decoder thread (baton)                    stfx_thp (no baton, its own core)
//   THPVideoDecode: fill the job, signal jobSema -> wakes, decodes into the planes
//   os_ext_wait(doneSema): BLOCKED, baton passed   done = 1, signal doneSema
//   ... main runs its frame ...
//   main, end of an interrupt window (os_irq_exit): polls doneSema, makes the decoder runnable;
//   it outranks main, so it resumes at once, invalidates/registers the planes (GX state: baton
//   only) and returns the frame to the game.
//
// WHO OWNS WHAT. The frame bytes (a read buffer) and the planes (a texture set) belong to the game
// decoder thread from the moment it took them off their queues until it sends them on, which is
// after THPVideoDecode returns; nothing else reads or writes them in between (the game draws only
// curTextureSet, and a set reaches the free queue only after PlayControl replaced it AND the frame
// that drew it was presented). For the length of one job the worker borrows exactly those two, and
// the job struct: the game thread writes it before jobSema, the worker until doneSema, the game
// thread after. One job at a time -- the game thread waits for each.
// STOP / LOOP / TEARDOWN. A loop is just the next frame. THPPlayerStop cancels the decoder thread
// (OSCancelThread) and the game then frees the buffers: os_threads.c calls thpWorkerCancel first,
// which raises `stop` (checked once per macroblock row) and waits for the worker to report done, so
// the worker is idle before any buffer can be freed. AT EXIT thp_worker_shutdown asks the thread to
// leave and waits <= 1.5 s; it is never killed.
// ux0:data/stairfax/movie_thread_off.txt starting with 1 = decode on the game thread, as before;
// movie_fastdec_off.txt starting with 1 = the reference decoder; movie_thread_core.txt: 0/1/2 = the
// worker's core (default 2), x = not pinned.
int thp_arm_fast = 1;           // host tests set these; on the Vita the marker files do
static int gArmsRead, gThreadOff;
extern unsigned stfx_mv_skipped __attribute__((weak));     // hal/stfx_audio.c (THPVideoDecode.c's skip arm)

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <stdio.h>
#define THP_NOW() sceKernelGetProcessTimeWide()
extern void os_ext_wait(int sema, void (*onCancel)(void));             // hal/os_threads.c
extern unsigned os_ext_held, os_ext_polled, os_ext_midpolled;
extern int os_ext_midframe;
extern unsigned long long os_ext_held_us;

static struct {
    SceUID thr, jobSema, doneSema;
    int state;                      // 0 not started, 1 running, -1 failed or shut down (decode on the game thread)
    int coreMask, affinityResult;
    volatile int quit, stop, done;
    // the job: written by the game thread before jobSema, by the worker until doneSema
    const u8* file; u8 *y, *u, *v; int fast;
    s32 err; int w, h;
    unsigned long long decUs;       // this job's decode time, measured on the worker
    unsigned long long expUs;       // ... and the I8 -> RGBA expand of the three planes that followed (movie_upload_off.txt 1: none)
    unsigned char* ex[3];           // the expanded planes Y, U, V (malloc): the worker writes them before done, the game thread takes them after
    int preExpand;
    unsigned cancels;
} gW;

static int thpWorker(SceSize args, void* argp) {
    (void)args; (void)argp;
    for (;;) {
        unsigned long long t0;
        sceKernelWaitSema(gW.jobSema, 1, NULL);
        if (gW.quit) break;
#ifdef THP_WORKER_TEST_HOOK         /* host/tests/os_ext_test.c makes a decode take as long as it needs */
        THP_WORKER_TEST_HOOK();
#endif
        t0 = THP_NOW();
        gW.err = thp_decode_planes(gW.fast, gW.file, gW.y, gW.u, gW.v, &gW.w, &gW.h, &gW.stop);
        gW.decUs = THP_NOW() - t0;
        gW.expUs = 0;
        if (gW.preExpand && !gW.err && !gW.stop && gxTexDecode && gx_tex_yuv_register_x && gW.w > 0 && gW.h > 0) {
            // The expand reads the planes this worker just wrote and nothing else; the planes belong to the game decoder
            // thread until THPVideoDecode returns (see WHO OWNS WHAT), so the same window covers this. It runs on the
            // worker so the MAIN thread, which used to do it inside the first draw of the frame, only uploads.
            unsigned long long e0 = THP_NOW();
            gW.ex[0] = gxTexDecode(1 /* GX_TF_I8 */, gW.w, gW.h, gW.y);
            gW.ex[1] = gxTexDecode(1, gW.w / 2, gW.h / 2, gW.u);
            gW.ex[2] = gxTexDecode(1, gW.w / 2, gW.h / 2, gW.v);
            gW.expUs = THP_NOW() - e0;
        }
        __atomic_store_n(&gW.done, 1, __ATOMIC_SEQ_CST);
        sceKernelSignalSema(gW.doneSema, 1);
    }
    return 0;
}

// hal/os_threads.c OSCancelThread, on the cancelling thread (it holds the baton), when the thread
// being cancelled is waiting for a job: the worker must be idle before the caller frees anything.
static void thpWorkerCancel(void) {
    SceUInt to = 2000000;
    gW.stop = 1;
    if (sceKernelWaitSema(gW.doneSema, 1, &to) < 0)
        printf("[movie-dec] CANCEL: the decode thread did not report done in 2 s -- it may still be writing a plane\n");
    gW.stop = 0;
    ++gW.cancels;
}

static int workerStart(void) {
    if (gW.state) return gW.state > 0;
    gW.jobSema = sceKernelCreateSema("stfx_thp_job", 0, 0, 1, NULL);
    gW.doneSema = sceKernelCreateSema("stfx_thp_done", 0, 0, 1, NULL);
    // the default priority, as every other thread of the port (rhi_vita_tev.inc: +32 was refused)
    gW.thr = sceKernelCreateThread("stfx_thp", thpWorker, 0x10000100, 0x10000, 0, 0, NULL);
    if (gW.jobSema < 0 || gW.doneSema < 0 || gW.thr < 0) {
        printf("[movie-dec] NO decode thread (thread %08x, semas %08x %08x): decoding on the game thread\n",
               (unsigned)gW.thr, (unsigned)gW.jobSema, (unsigned)gW.doneSema);
        gW.state = -1;
        return 0;
    }
    gW.affinityResult = gW.coreMask ? sceKernelChangeThreadCpuAffinityMask(gW.thr, gW.coreMask) : 0;
    if (sceKernelStartThread(gW.thr, 0, NULL) < 0) { printf("[movie-dec] the decode thread did not start\n"); gW.state = -1; return 0; }
    gW.state = 1;
    return 1;
}

static int markerChar(const char* path) {
    FILE* f = fopen(path, "r");
    int c = f ? fgetc(f) : -1;
    if (f) fclose(f);
    return c;
}
static void armsRead(void) {
    int core = markerChar("ux0:data/stairfax/movie_thread_core.txt");
    gArmsRead = 1;
    gThreadOff = markerChar("ux0:data/stairfax/movie_thread_off.txt") == '1';
    thp_arm_fast = markerChar("ux0:data/stairfax/movie_fastdec_off.txt") != '1';
    gW.preExpand = markerChar("ux0:data/stairfax/movie_upload_off.txt") != '1';
    os_ext_midframe = markerChar("ux0:data/stairfax/movie_wake_off.txt") != '1';
    gW.coreMask = core == 'x' ? 0 : core == '0' ? SCE_KERNEL_CPU_MASK_USER_0 : core == '1' ? SCE_KERNEL_CPU_MASK_USER_1
                                                                            : SCE_KERNEL_CPU_MASK_USER_2;
    if (!gThreadOff) workerStart();
    printf("[movie-dec] planes expanded %s | decoder woken %s\n",
           gW.preExpand ? "on the WORKER" : "on the main thread (movie_upload_off.txt)",
           os_ext_midframe ? "as soon as it is done, at the next texture load (and at the retrace)" : "at the retrace only (movie_wake_off.txt)");
    printf("[movie-dec] decode thread %s | decoder %s | core mask %05x (set: %08x)\n",
           gThreadOff ? "OFF (movie_thread_off.txt): on the game thread, holding the baton"
                      : gW.state > 0 ? "on: off the baton" : "FAILED to start: on the game thread",
           thp_arm_fast ? "fast (table Huffman, sparse blocks, 4-lane IDCT)" : "REFERENCE (movie_fastdec_off.txt)",
           (unsigned)gW.coreMask, (unsigned)gW.affinityResult);
}

// The [movie-dec] line. WORKING (movie playing, thread on): decoded + skipped = 29.97/s with decoded
// >= 15; "waited" about one or two game frames (the wake is polled at the retrace) while "baton held"
// stays 0 after the first frame of each movie start (the prepare, where main itself is waiting);
// game frames back at ~33 ms ([win]). Thread OFF: waited = decode, and all of it is baton time.
static struct { unsigned n, skipped0, held0, polled0, mid0, expN; unsigned long long expUs, decUs, decMax, waitUs, waitMax, heldUs0, t0;
                unsigned total; unsigned long long totDec, totWait, totDecMax, totWaitMax; } gS;
static void statsLine(int final) {
    unsigned long long now = THP_NOW();
    unsigned skipped = &stfx_mv_skipped ? stfx_mv_skipped : 0u;
    double s;
    if (!gS.t0) { gS.t0 = now; gS.skipped0 = skipped; gS.held0 = os_ext_held; gS.heldUs0 = os_ext_held_us; gS.polled0 = os_ext_polled; gS.mid0 = os_ext_midpolled; }
    s = (double)(now - gS.t0) / 1000000.0;
    if (gS.n && (s >= 5.0 || final))
        printf("[movie-dec] last %.1f s: decoded %.2f/s skipped %.2f/s | decode avg %.2f ms max %.2f (on the %s) | game thread waited avg %.2f ms max %.2f"
               " | baton HELD through a wait %u times, %.1f ms; woken at a retrace %u times, mid-frame %u times | worker plane expand avg %.2f ms (%u) | thread %s, decoder %s\n",
               s, gS.n / s, (skipped - gS.skipped0) / s, gS.decUs / 1000.0 / gS.n, gS.decMax / 1000.0, gW.state > 0 ? "worker" : "game thread",
               gS.waitUs / 1000.0 / gS.n, gS.waitMax / 1000.0, os_ext_held - gS.held0, (os_ext_held_us - gS.heldUs0) / 1000.0,
               os_ext_polled - gS.polled0, os_ext_midpolled - gS.mid0, gS.expN ? gS.expUs / 1000.0 / gS.expN : 0.0, gS.expN,
               gW.state > 0 ? "on" : "OFF", thp_arm_fast ? "fast" : "REFERENCE");
    if (final && gS.total)
        printf("[movie-dec] totals: decoded %u | decode avg %.2f ms max %.2f | game thread waited avg %.2f ms max %.2f | baton HELD through a wait %u times, %.1f ms"
               " | cancelled mid-decode %u | fast paths: DC %u, row %u, column %u, full %u blocks; long codes %u\n",
               gS.total, gS.totDec / 1000.0 / gS.total, gS.totDecMax / 1000.0, gS.totWait / 1000.0 / gS.total, gS.totWaitMax / 1000.0,
               os_ext_held, os_ext_held_us / 1000.0, gW.cancels, thp_fast_blocks[0], thp_fast_blocks[1], thp_fast_blocks[2], thp_fast_blocks[3],
               thp_fast_blocks[4]);
    if (s >= 5.0 || final) {
        gS.n = 0; gS.decUs = gS.decMax = gS.waitUs = gS.waitMax = 0; gS.expN = 0; gS.expUs = 0;
        gS.t0 = now; gS.skipped0 = skipped; gS.held0 = os_ext_held; gS.heldUs0 = os_ext_held_us; gS.polled0 = os_ext_polled; gS.mid0 = os_ext_midpolled;
    }
}
static void statsAdd(unsigned long long dec, unsigned long long wait) {
    ++gS.n; gS.decUs += dec; gS.waitUs += wait;
    if (dec > gS.decMax) gS.decMax = dec;
    if (wait > gS.waitMax) gS.waitMax = wait;
    ++gS.total; gS.totDec += dec; gS.totWait += wait;
    if (dec > gS.totDecMax) gS.totDecMax = dec;
    if (wait > gS.totWaitMax) gS.totWaitMax = wait;
    statsLine(0);
}

// vita_tool_main.c, at exit (main thread), next to stfx_audio_shutdown: the totals, then the worker
// is ASKED to leave and waited for. Never killed: a thread killed inside a kernel call is how a
// console stops launching apps.
void thp_worker_shutdown(void) {
    if (gArmsRead) statsLine(1);
    if (gW.state != 1) return;
    gW.state = -1;
    gW.stop = 1;
    gW.quit = 1;
    sceKernelSignalSema(gW.jobSema, 1);     // (already 1 = a job it has not taken: it sees quit first)
    {   SceUInt to = 1500000;
        int r = sceKernelWaitThreadEnd(gW.thr, NULL, &to);
        if (r >= 0) sceKernelDeleteThread(gW.thr);
        printf("[movie-dec] decode thread %s\n", r >= 0 ? "stopped" : "did NOT stop in 1.5 s (left running)");
    }
}
#else
void thp_worker_shutdown(void) {}
#endif

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    s32 err;
    int w = 0, h = 0;
    int takeEx = 0; unsigned long long expUs = 0;
    (void)work;
    ++thp_video_decode_calls;
    if (!file) return 25;
    if (!tileY || !tileU || !tileV) return 27;
#if defined(__vita__)
    {   unsigned long long t0, dec;
        if (!gArmsRead) armsRead();
        t0 = THP_NOW();
        if (!gThreadOff && gW.state > 0) {
            gW.file = (const u8*)file; gW.y = (u8*)tileY; gW.u = (u8*)tileU; gW.v = (u8*)tileV;
            gW.fast = thp_arm_fast;
            for (int k = 0; k < 3; ++k) { if (gW.ex[k]) free(gW.ex[k]); gW.ex[k] = NULL; }   // (a cancelled job's, never taken)
            __atomic_store_n(&gW.done, 0, __ATOMIC_SEQ_CST);
            sceKernelSignalSema(gW.jobSema, 1);
            // sleeps WITHOUT the baton until the worker signals; hal/os_threads.c says how it is woken
            while (!__atomic_load_n(&gW.done, __ATOMIC_SEQ_CST)) os_ext_wait(gW.doneSema, thpWorkerCancel);
            err = gW.err; w = gW.w; h = gW.h; dec = gW.decUs;
            takeEx = !err && gW.ex[0] && gW.ex[1] && gW.ex[2];
            expUs = gW.expUs;
        } else {
            err = thp_decode_planes(thp_arm_fast, file, tileY, tileU, tileV, &w, &h, NULL);
            dec = THP_NOW() - t0;
        }
        statsAdd(dec, THP_NOW() - t0);
    }
#else
    (void)gArmsRead; (void)gThreadOff;
    err = thp_decode_planes(thp_arm_fast, file, tileY, tileU, tileV, &w, &h, NULL);
#endif
    if (err) { ++thp_video_decode_errors; return err; }
    // the planes are GX textures the game re-inits every frame: their texels changed (gx_draw.c's
    // texture cache re-decodes them on the next load; weak -- the host test links without gx_draw)
    if (gx_tex_invalidate) {
        gx_tex_invalidate(tileY, (u32)w * h);
        gx_tex_invalidate(tileU, (u32)w * h / 4);
        gx_tex_invalidate(tileV, (u32)w * h / 4);
    }
    // ...and they are a movie frame: gx_draw uploads the Y plane converted to RGB (the movie TEV)
#if defined(__vita__)
    if (takeEx && gx_tex_yuv_register_x) {
        unsigned char *a = gW.ex[0], *b = gW.ex[1], *c = gW.ex[2];
        gW.ex[0] = gW.ex[1] = gW.ex[2] = NULL;          // gx_draw owns them now
        gx_tex_yuv_register_x(tileY, tileU, tileV, w, h, a, b, c);
        gS.expUs += expUs; ++gS.expN;
        return 0;
    }
    for (int k = 0; k < 3; ++k) { if (gW.ex[k]) free(gW.ex[k]); gW.ex[k] = NULL; }
#endif
    if (gx_tex_yuv_register) gx_tex_yuv_register(tileY, tileU, tileV, w, h);
    return 0;
}

// ---- audio: the SDK's THPAudioDecode (upstream src/dolphin/thp/THPAudio.c, C) over a big-endian
// record header. Needed even with no audio OUTPUT: the movie's DMA-callback mixer
// (thp/attractmovie.c AttractMovieAudio_Mix) loops until it gets samples, so "0 valid samples" hung
// the AI interrupt (Vita run 70, watchdog at AttractMovieAudio_Mix+0x7e). Checked against ffmpeg's
// adpcm_thp on the disc movie (host/tests/thp_decode_test.c).
typedef struct { u32 offsetNextChannel, sampleSize; s16 lCoef[8][2], rCoef[8][2]; s16 lYn1, lYn2, rYn1, rYn2; } ThpAHdr;
typedef struct { const u8* p; u32 nib; u8 pred, scale; } ThpADec;
static inline void adInit(ThpADec* d, const u8* ptr) {
    d->p = ptr; d->nib = 2; d->pred = (u8)((*d->p & 0x70) >> 4); d->scale = (u8)(*d->p & 0xF); d->p++;
}
static inline s32 adSample(ThpADec* d) {
    s32 s;
    if (!(d->nib & 0x0f)) { d->pred = (u8)((*d->p & 0x70) >> 4); d->scale = (u8)(*d->p & 0xF); d->p++; d->nib += 2; }
    if (d->nib & 1) { s = (s32)((u32)(*d->p & 0xF) << 28) >> 28; d->p++; }
    else s = (s32)((u32)(*d->p & 0xF0) << 24) >> 28;
    d->nib++;
    return s;
}
static void adChannel(const u8* data, const s16 coef[8][2], s16 yn1, s16 yn2, u32 n, s16* out, int step,
                      s16* out2) {
    ThpADec d;
    u32 i;
    adInit(&d, data);
    for (i = 0; i < n; i++) {
        s32 sample = adSample(&d);
        s64 yn = (s64)coef[d.pred][1] * yn2;
        yn += (s64)coef[d.pred][0] * yn1;
        yn += (s64)((sample << d.scale) << 11);
        yn <<= 5;
        if ((u16)(yn & 0xffff) > 0x8000) yn += 0x10000;
        else if ((u16)(yn & 0xffff) == 0x8000 && (yn & 0x10000)) yn += 0x10000;
        if (yn > 2147483647LL) yn = 2147483647LL;
        if (yn < -2147483648LL) yn = -2147483648LL;
        *out = (s16)(yn >> 16); out += step;
        if (out2) { *out2 = (s16)(yn >> 16); out2 += step; }
        yn2 = yn1;
        yn1 = (s16)(yn >> 16);
    }
}
u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag) {
    ThpAHdr h;
    const u8* f = audioFrame;
    const u8 *left, *right;
    s16 *decL, *decR;
    int step, i, j;
    ++thp_audio_decode_calls;
    if (!audioBuffer || !audioFrame) return 0;
    h.offsetNextChannel = (u32)f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3];
    h.sampleSize = (u32)f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7];
    for (i = 0; i < 8; i++)
        for (j = 0; j < 2; j++) {
            h.lCoef[i][j] = (s16)(f[8 + (i * 2 + j) * 2] << 8 | f[9 + (i * 2 + j) * 2]);
            h.rCoef[i][j] = (s16)(f[40 + (i * 2 + j) * 2] << 8 | f[41 + (i * 2 + j) * 2]);
        }
    h.lYn1 = (s16)(f[72] << 8 | f[73]); h.lYn2 = (s16)(f[74] << 8 | f[75]);
    h.rYn1 = (s16)(f[76] << 8 | f[77]); h.rYn2 = (s16)(f[78] << 8 | f[79]);
    left = audioFrame + 80;
    right = left + h.offsetNextChannel;
    if (flag == 1) { decR = audioBuffer; decL = audioBuffer + h.sampleSize; step = 1; }
    else { decR = audioBuffer; decL = audioBuffer + 1; step = 2; }
    if (h.offsetNextChannel == 0) {                     // mono: the one channel to both sides
        adChannel(left, h.lCoef, h.lYn1, h.lYn2, h.sampleSize, decL, step, decR);
    } else {
        adChannel(left, h.lCoef, h.lYn1, h.lYn2, h.sampleSize, decL, step, NULL);
        adChannel(right, h.rCoef, h.rYn1, h.rYn2, h.sampleSize, decR, step, NULL);
    }
    return h.sampleSize;
}

// THPInit: TRUE -- the decoder above needs none of retail THPInit's locked-cache / GQR setup.
// (Before 2026-09-27 this returned FALSE to decline the movie; the title menu reads input only
// once the movie is PREPARED, so declining it kept the game on the title screen for ever.)
BOOL THPInit(void) { return TRUE; }
