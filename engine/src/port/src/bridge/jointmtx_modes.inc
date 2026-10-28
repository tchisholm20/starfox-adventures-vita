/* jointmtx_modes_draft.c -- DRAFT: the paths of retail's modelAnimBuildJointMatrices that the port's C twin
 * (src/port/src/bridge/game_scene.cpp, modelAnimBuildJointMatrices) does not have:
 *
 *      mode 0x40          @cachePass: two frames of move 0 decoded uninterpolated, nlerped as quaternions into
 *                         the bank, then blended against channel 1                         (model.c:436)
 *      mode 0x04 / 0x08   blend two channels and STORE the quaternion + scale + translation in the bank
 *                         entry instead of building a matrix                               (model.c:473)
 *      mode 0x01 / 0x02   pose A / pose B is READ BACK from what a 0x04 / 0x08 call stored (model.c:507)
 *      0x10, 0x20         modifiers of @blendJoints (no translation blend; root rotation override)
 *
 * Translated instruction by instruction from the assembly, src/main/render.c:355-1676 of the same tree; every
 * block names the lines it translates. Nothing is "improved": the quirks are reproduced and marked QUIRK.
 * The map of the assembly is jointmtx_modes_notes.md; the Python reading is jointmtx_modes_model.py.
 *
 * WHAT IS VERIFIED (2026-10-10, host only):  this file == jointmtx_modes_model.py bit for bit on random
 * inputs, and that model == RETAIL'S INSTRUCTIONS (the function's bytes from the Rev 1 disc executed under
 * Unicorn, paired-single ops supplied by a hook) bit for bit. NOT verified: anything on a console or in
 * Dolphin; no retail memory vector exists for any of these modes (every captured call is mode 00).
 *
 * CONVENTIONS (the twin's): the bank is *out, 0x40 bytes per entry, host-order f32 / s16; the slot buffer is
 * the twin's gAnimSlots viewed as bytes (0x40 per slot: rotation ch0 @0 ch1 @6, scale ch0 @0xc ch1 @0x12,
 * translation ch0 @0x18 ch1 @0x1e; retail lbl_802C3564 + 0x1c); bones 0x1c each, head f32 host order; the
 * fixup table host order; descriptors and bit streams BIG-ENDIAN as on the disc.
 * A bank entry seen by the stored-quaternion passes: quaternion A (w x y z) @0x00, quaternion B @0x10, and
 * the SAME pose record laid over it at +0x1c, so scale A @0x28, scale B @0x2e, translation A @0x34, B @0x3a.
 *
 * BUILD: every float expression here is one PowerPC single-precision instruction per C operator. Compile
 * with -ffp-contract=off and with float evaluated as float (SSE2 / VFP; not x87).
 *      gcc -O2 -std=gnu11 -ffp-contract=off -DJM_HOST_TEST jointmtx_modes_draft.c -o jm_draft -lm
 * -DJM_BREAK=1..4 plants a one-line fault (the break arms of jointmtx_modes_model.py --break N).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef JM_BREAK
#define JM_BREAK 0
#endif
#ifndef JM_MAX_SLOTS
#define JM_MAX_SLOTS 128            /* = the twin's ANIM_MAX_SLOTS (retail's buffer holds 101) */
#endif

/* ---- STAND-INS so the file compiles alone. In the twin: delete this block, the names exist there. ---- */
#ifdef JM_HOST_TEST
int16_t gModelRootRotX, gModelRootRotY, gModelRootRotZ;     /* render.c:328-330 */
#elif !defined(JM_ROOT_ROT_DECLARED)   /* the twin declares them itself at file scope: a second declaration inside its
                                         anonymous namespace would name DIFFERENT variables (run 183: link failed) */
#ifdef __cplusplus
extern "C" {
#endif
extern int16_t gModelRootRotX, gModelRootRotY, gModelRootRotZ;
#ifdef __cplusplus
}
#endif
#endif
/* COPIED from the twin (game_scene.cpp:219): n bits MSB-first at bit offset bitPos. The assembly keeps a
   32-bit window per frame and refills it (render.c:969-981); it reads the same bits. */
static inline unsigned jm_animReadBits(const uint8_t* p, int bitPos, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; ++i) {
        int b = bitPos + i;
        r = (r << 1) | ((unsigned)(p[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    return r;
}
/* ---- end of stand-ins ---- */

/* render.c:337-352, the function's .sdata2 pool */
#define JM_PHASE_SCALE 16384.0f
#define JM_HALF_UNIT   0.001953125f       /* 1/512, translation */
#define JM_SCALE_UNIT  0.0009765625f      /* 1/1024, scale */
static const float jmC8 = 2.65546059e-42f, jmC6 = -2.63291104e-31f, jmC4 = 1.3751435e-20f, jmC2 = -2.87243285e-10f, jmC0 = 1.0f;
static const float jmS7 = -8.84440041e-37f, jmS5 = 6.59063581e-26f, jmS3 = -2.29492142e-15f, jmS1 = 2.39684487e-05f;

static inline int      jm_rs(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }       /* lha */
static inline unsigned jm_ru(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }      /* lhz */
static inline void     jm_wh(uint8_t* p, int v) { uint16_t w = (uint16_t)v; memcpy(p, &w, 2); } /* sth */
static inline float    jm_rf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }         /* lfs */
static inline void     jm_wf(uint8_t* p, float v) { memcpy(p, &v, 4); }                         /* stfs */
static inline unsigned jm_be16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }       /* lhz of disc data */
static inline int32_t  jm_mul_sra14(int32_t a, int32_t b) {                                     /* mullw ; srawi 14 */
    return (int32_t)((uint32_t)a * (uint32_t)b) >> 14;
}

/* fmadds frD = frA * frC + frB: FUSED, one rounding. The product of two singles is exact in a double; the
   sum is rounded to odd in double and then to single, which cannot double-round. (libm fmaf would do, where
   it is correct and fast; this is self-contained and uses only hardware double arithmetic.) */
static inline float jm_fmadds(float a, float c, float b) {
#if JM_BREAK == 4
    return a * c + b;                     /* BREAK 4: two roundings */
#else
    double p = (double)a * (double)c;
    double s = p + (double)b;
    if (!(s - s == 0.0)) return (float)s; /* inf / nan */
    double bb = s - p;
    double err = (p - (s - bb)) + ((double)b - bb);
    if (err != 0.0) {
        uint64_t u; memcpy(&u, &s, 8);
        if (!(u & 1)) s = nextafter(s, err > 0.0 ? HUGE_VAL : -HUGE_VAL);
    }
    return (float)s;
#endif
}

/* psq_st frS, d(rA), 1, 3 -- GQR3 = u16, scale 0 (the SDK start-up value): truncate toward zero, saturate */
static inline unsigned jm_sat_u16(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 65535.0f) return 65535;
    return (unsigned)v;
}

/* render.c:949-955 (and 446-452): the sub-frame fraction of a phase. *frac = the float (f28 in @cachePass),
   returns it x16384 as the u16 the stack slot holds. The twin's floorf() form agrees for 0 <= phase < 65536. */
static inline unsigned jm_phase_frac(float phase, float* frac) {
    float fr = phase - (float)jm_sat_u16(phase);          /* psq_st 1,3 ; psq_l 1,3 ; fsubs */
    if (frac) *frac = fr;
    return jm_sat_u16(fr * JM_PHASE_SCALE);                 /* fmuls ; psq_st 1,3 */
}

/* The fixup table (r8, gModelJointScratchBuffer): 8-byte rows {u16 off0, u16 off1, s16 add0, s16 add1},
   a column ends at off == 0x1000. render.c:1017-1028 (one frame) and 1172-1186 (both frames, +6). */
static void jm_fixups(uint8_t* base, const uint8_t* fix, int col, int paired) {
    if (!fix) return;                                       /* PORT GUARD (the twin's) */
    for (int guard = 0; guard < JM_MAX_SLOTS * 12; ++guard, fix += 8) {    /* PORT GUARD: bounded walk */
        unsigned off = jm_ru(fix + 2 * col);
        if (off == 0x1000) break;
        if (off + 8 > JM_MAX_SLOTS * 0x40 - 6) continue;    /* PORT GUARD (the twin's): stay inside gAnimSlots */
        int add = jm_rs(fix + 4 + 2 * col);
        jm_wh(base + off, jm_rs(base + off) + add);
        if (paired) jm_wh(base + off + 6, jm_rs(base + off + 6) + add);
    }
}

/* @decodeInterpolated, render.c:944-1103. One channel: frames k and k+1 interpolated by the phase fraction
   into the record at `base` (S for channel 0, S+6 for channel 1). `col` = which fixup column (r8 or r8+2).
   The twin has this twice already (stage 1 and the fade's channel 1); same arithmetic. */
static void jm_decode_interpolated(uint8_t* base, const uint8_t* mfd, const uint8_t* cur, int stride,
                                   float phase, const uint8_t* fix, int col) {
    int n = mfd[0];                                         /* 947 */
    if (n > JM_MAX_SLOTS) n = JM_MAX_SLOTS;                 /* PORT GUARD (the twin's). Retail with n == 0 underflows its counter. */
    const uint8_t* dp = mfd + 4;                            /* 946 */
    const uint8_t* nxt = cur + stride;                      /* 948 */
    int frac = (int)jm_phase_frac(phase, 0);                /* 949-955, lhz */
    int pos = 0;
    uint8_t* p = base;
    for (int k = 0; k < n * 3; ++k) {                       /* 956-957, 963-1016 */
        unsigned d = jm_be16(dp); dp += 2;                  /* 964, 998 */
        int bw = (int)(d & 0xf), val = (int)(d & 0xfff0);   /* 965-966 */
        if (bw) {                                           /* 969-995: rotation. delta sign-extended from 14 bits, x4 */
            int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
            pos += bw;
            int dl = (int32_t)((uint32_t)(x - c) << 18) >> 18;              /* 986-988 */
            val += (c + jm_mul_sra14(dl, frac)) << 2;                       /* 989-993 */
        }
        jm_wh(p, val);                                      /* 997 */
        jm_wh(p + 0xc, 0); jm_wh(p + 0x18, 0);              /* 1000-1002 */
        if (d & 0x10) {                                     /* 1004-1005 -> @L_80007624 */
            unsigned d2 = jm_be16(dp);                      /* 1030 */
            int trans = 1;
            if (d2 & 0x10) {                                /* 1031-1032: this one is a SCALE */
                dp += 2;                                    /* 1064 */
                int v2 = (int)(d2 & 0xffc0);                /* 1033 */
                bw = (int)(d2 & 0xf);
                if (bw) {                                   /* 1037-1061: delta NOT sign-extended, x2 */
                    int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
                    pos += bw;
                    v2 += (c + jm_mul_sra14(x - c, frac)) << 1;
                }
                jm_wh(p + 0xc, v2);                         /* 1063 */
                trans = (d2 & 0x20) != 0;                   /* 1034, 1066-1068 */
                if (trans) d2 = jm_be16(dp);                /* 1065 */
            }
            if (trans) {                                    /* @L_800076B8, 1070-1103: translation, 16-bit delta, x1 */
                dp += 2;
                int v3 = (int)(d2 & 0xfff0);
                bw = (int)(d2 & 0xf);
                if (bw) {
                    int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
                    pos += bw;
                    v3 += c + jm_mul_sra14((int16_t)(x - c), frac);         /* 1091-1097 */
                }
                jm_wh(p + 0x18, v3);                        /* 1101 */
            }
        }
        p += (k % 3 == 2) ? 0x3c : 2;                       /* 1007-1013 */
    }
    jm_fixups(base, fix, col, 0);                           /* 1017-1028 */
}

/* @decodePaired, render.c:1104-1265. Move 0 WITHOUT interpolation: frame k into channel 0's fields, frame
   k+1 into channel 1's fields of the same record. */
static void jm_decode_paired(uint8_t* base, const uint8_t* mfd, const uint8_t* cur, int stride, const uint8_t* fix) {
    int n = mfd[0];                                         /* 1107 */
    if (n > JM_MAX_SLOTS) n = JM_MAX_SLOTS;                 /* PORT GUARD */
    const uint8_t* dp = mfd + 4;                            /* 1106 */
    const uint8_t* nxt = cur + stride;                      /* 1108 */
    int pos = 0;
    uint8_t* p = base;
    for (int k = 0; k < n * 3; ++k) {                       /* 1109-1110, 1116-1171 */
        unsigned d = jm_be16(dp); dp += 2;                  /* 1117, 1151 */
        int bw = (int)(d & 0xf);
        int a = (int)d, b = (int)d;                         /* 1119-1122: width 0, the word itself (its low 4 bits are 0) */
        if (bw) {                                           /* 1124-1149 */
            int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
            pos += bw;
            a = (int)(d & 0xfff0) + (c << 2); b = (int)(d & 0xfff0) + (x << 2);
        }
        jm_wh(p, a); jm_wh(p + 6, b);
        jm_wh(p + 0xc, 0); jm_wh(p + 0x12, 0); jm_wh(p + 0x18, 0); jm_wh(p + 0x1e, 0);     /* 1153-1157 */
        if (d & 0x10) {                                     /* 1159-1160 -> @L_80007864 */
            unsigned d2 = jm_be16(dp);                      /* 1188 */
            int trans = 1;
            if (d2 & 0x10) {                                /* 1189-1190: scale */
                dp += 2;                                    /* 1225 */
                bw = (int)(d2 & 0xf);
#if JM_BREAK == 2
                a = b = (int)(d2 & 0xffc0);                 /* BREAK 2: what @decodeInterpolated does */
#else
                a = b = (int)d2;                            /* 1193-1196 QUIRK: a constant scale keeps its 0x10 / 0x20 FLAG bits */
#endif
                if (bw) {                                   /* 1198-1223 */
                    int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
                    pos += bw;
                    a = (int)(d2 & 0xffc0) + (c << 1); b = (int)(d2 & 0xffc0) + (x << 1);
                }
                jm_wh(p + 0xc, a); jm_wh(p + 0x12, b);
                trans = (d2 & 0x20) != 0;                   /* 1191, 1227-1229 */
                if (trans) d2 = jm_be16(dp);                /* 1226 */
            }
            if (trans) {                                    /* @L_80007900, 1231-1265 */
                dp += 2;
                bw = (int)(d2 & 0xf);
                a = b = (int)d2;                            /* 1232-1234 */
                if (bw) {                                   /* 1237-1262 */
                    int c = (int)jm_animReadBits(cur, pos, bw), x = (int)jm_animReadBits(nxt, pos, bw);
                    pos += bw;
                    a = (int)(d2 & 0xfff0) + c; b = (int)(d2 & 0xfff0) + x;
                }
                jm_wh(p + 0x18, a); jm_wh(p + 0x1e, b);
            }
        }
        p += (k % 3 == 2) ? 0x3c : 2;                       /* 1162-1168 */
    }
    jm_fixups(base, fix, 0, 1);                             /* 1172-1186: column 0, the same addend to both frames */
}

/* The sine / cosine of one angle register: render.c:817-850 (and the two repeats to 922; 1319-1423 in
   @singlePose). r16 = the register AFTER the caller's optional srawi 1. Polynomials in x = (s16)(r16 << 2),
   i.e. the angle reduced to +-1/8 turn; the quadrant comes from (r16 + 0x2000) & 0xc000. */
static inline void jm_sincos(int r16, float* cs, float* sn) {
    float x = (float)(int16_t)(uint16_t)((uint32_t)r16 << 2);      /* slwi 2 ; sth ; psq_l 1,5 (s16, scale 0) */
    float x2 = x * x;                                               /* fmuls */
    float s = jm_fmadds(x2, jmS7, jmS5);
    s = jm_fmadds(x2, s, jmS3);
    s = jm_fmadds(x2, s, jmS1);
    s = x * s;                                                      /* fmuls */
    float c = jm_fmadds(x2, jmC8, jmC6);
    c = jm_fmadds(x2, c, jmC4);
    c = jm_fmadds(x2, c, jmC2);
    c = jm_fmadds(x2, c, jmC0);
    switch ((unsigned)(r16 + 0x2000) & 0xc000u) {
    case 0x0000: *sn = s;  *cs = c;  break;
    case 0x4000: *sn = c;  *cs = -s; break;                         /* fneg: a true sign flip */
    case 0x8000: *sn = -s; *cs = -c; break;                         /* unreachable for a half angle */
    default:     *sn = -c; *cs = s;  break;
    }
}

/* @buildRotation (render.c:793-943) and the eight instructions after each call (591-598, 618-625): three s16
   angles, each HALVED (srawi 1), to a quaternion w x y z. Not normalised beyond what the polynomials give. */
static inline void jm_quat(const uint8_t* rot, float q[4]) {
    float c0, s0, c1, s1, c2, s2;
    jm_sincos(jm_rs(rot) >> 1, &c0, &s0);                           /* 815-816: lha ; srawi 1 */
    jm_sincos(jm_rs(rot + 2) >> 1, &c1, &s1);
    jm_sincos(jm_rs(rot + 4) >> 1, &c2, &s2);
    float f6 = c0 * c1, f7 = c0 * s1, f8 = s0 * c1, f9 = s0 * s1;   /* 923-926 */
    float f10 = f6 * c2, f11 = f9 * s2, f12 = f8 * c2, f13 = f7 * s2;  /* 927-930 */
    float f14 = f7 * c2, f15 = f8 * s2, f16 = f6 * s2, f17 = f9 * c2;
    q[0] = f10 + f11; q[1] = f12 - f13; q[2] = f14 + f15; q[3] = f16 - f17;
}

typedef struct JmCtx {
    uint8_t* bank;              /* r3 = *out: model->jointMatrices[buf] */
    const uint8_t* dst;         /* r4: 12 f32, the parent of bone 0 */
    uint8_t* slots;             /* retail lbl_802C3564 + 0x1c; in the twin (uint8_t*)gAnimSlots */
    const uint8_t* jd;          /* r6: ModelBone[jc], 0x1c each */
    int jc;                     /* r7 */
    const uint8_t* fix;         /* r8: gModelJointScratchBuffer */
    int flags;                  /* r9 */
    const uint8_t* mfd[2];      /* work+0x34 / +0x38  frameData (descriptors: count @0, u16 BE from +4) */
    const uint8_t* cur[2];      /* work+0x2c / +0x30  frameStreamCursors */
    int16_t stride[2];          /* work+0x4c / +0x4e  frameStreamStrides (lha) */
    float phase[2];             /* work+0x04 / +0x08  framePhases */
    int16_t countdown;          /* work+0x58          eventCountdown, read SIGNED (lha, psq_l 1,5) */
} JmCtx;

/* @blendJoints, render.c:477-792. A / B = the two pose records' bases (slot buffer, or the bank + 0x1c / + 0x22),
   w16 = the integer weight (r16), w = the float weight (f28). Returns 0 when it left by blr (686-687: only under
   mode & 0xC with the LAST bone not masked) and 1 when it left by `b @hierarchy` (655, 757). */
static int jm_blend_joints(const JmCtx* c, uint8_t* A, uint8_t* B, int mode, int w16, float w) {
    uint8_t* bank = c->bank;
    const int jc = c->jc, flags = c->flags;
    if (mode & 0x20) {                      /* 480-487: the rotation of SLOT 0 of pose B := the root rotation. */
        jm_wh(B + 0, gModelRootRotX);       /* QUIRK: under mode & 2, B is bank + 0x22, so this lands on bytes */
        jm_wh(B + 2, gModelRootRotY);       /* 0x22-0x27 of bank entry 0 and is read by nothing (B's rotation  */
        jm_wh(B + 4, gModelRootRotZ);       /* comes from the stored quaternion).                              */
    }
    uint8_t* D = A;                         /* 489: where the blended scale / translation go */
    if (mode & 0x0c) D = bank + 0x1c + ((mode & 8) ? 6 : 0);    /* 492-498 */
    const float iw = 1.0f - w;              /* 504 */

    /* loop 1, 507-565: scale and translation, integer lerps */
    for (int j = 0; j < jc; ++j) {
        const uint8_t* b = c->jd + j * 0x1c;
        uint8_t* pb = B + b[3] * 0x40;      /* 508-510 */
        uint8_t* pa = A + b[2] * 0x40;      /* 511-513 */
        uint8_t* pd = D + b[2] * 0x40;      /* 514 */
        if (mode & 0x0f) {                  /* 515-529 */
            int o = (b[1] & 0x7f) * 0x40;
            pd = D + o;
            if (mode & 3) {
                if (mode & 1) pa = A + o;   /* 524-527. QUIRK: with 1 AND 2 set only A is re-indexed; pose B's */
                else pb = B + o;            /* record stays at bone byte 3, not at the output index.           */
#if JM_BREAK == 3
                if (mode & 2) pb = B + o;   /* BREAK 3 */
#endif
            }
        }
        for (int k = 0; k < 3; ++k) {       /* 530-562 */
            int sb = (int)jm_ru(pb + 0xc + 2 * k); if (!sb) sb = 0x400;    /* 531-534 */
            int sa = (int)jm_ru(pa + 0xc + 2 * k); if (!sa) sa = 0x400;    /* 536-539 */
            jm_wh(pd + 0xc + 2 * k, jm_mul_sra14(sb - sa, w16) + sa);       /* 541-545 */
            int t = jm_rs(pa + 0x18 + 2 * k);                               /* 546 */
            if (!(mode & 0x10))                                             /* 548-555 */
                t += jm_mul_sra14(jm_rs(pb + 0x18 + 2 * k) - t, w16);
            jm_wh(pd + 0x18 + 2 * k, t);                                    /* 557 */
        }
    }

    /* loop 2, 571-792: rotation */
    for (int j = 0; j < jc; ++j) {
        const uint8_t* b = c->jd + j * 0x1c;
        const int last = (j == jc - 1);
        const int o = (b[1] & 0x7f) * 0x40;
        const uint8_t* pp = D + b[2] * 0x40;    /* 572-574: r24. QUIRK: loop 1 wrote at the OUTPUT index whenever
                                                   mode & 0xF; this reads at bone byte 2 unless mode & 1. */
        float qa[4], qb[4];
        if (mode & 1) {                         /* 576-587: quaternion A as a 0x04 call stored it */
            pp = D + o;
            for (int i = 0; i < 4; ++i) qa[i] = jm_rf(bank + o + 4 * i);
        } else jm_quat(A + b[2] * 0x40, qa);    /* 589-598 */
        if (mode & 2)                           /* 603-614: quaternion B as a 0x08 call stored it */
            for (int i = 0; i < 4; ++i) qb[i] = jm_rf(bank + o + 0x10 + 4 * i);
        else jm_quat(B + b[3] * 0x40, qb);      /* 616-625 */

        float dot = qa[0] * qb[0] + qa[1] * qb[1];      /* 627-630 */
        dot = dot + qa[2] * qb[2];                      /* 629, 632 */
        dot = dot + qa[3] * qb[3];                      /* 631, 634 */
        for (int i = 0; i < 4; ++i) qa[i] = qa[i] * iw; /* 633, 635, 637, 644 */
#if JM_BREAK != 1
        if (dot < 0.0f)                                 /* 636-642: fsubs from 0.0 (not fneg); no flip on NaN */
            for (int i = 0; i < 4; ++i) qb[i] = 0.0f - qb[i];
#endif
        int idx = (int)(int8_t)b[1] & flags;            /* 646-650 */
        if (idx < 0) {
            if (last) return 1;                         /* 651-655: `b @hierarchy`, NOT blr */
            continue;
        }
        idx &= 0x7f;                                    /* 657 */
        float q[4];
        for (int i = 0; i < 4; ++i) q[i] = qa[i] + qb[i] * w;   /* 645, 658-665: nlerp, NOT renormalised */

        if (mode & 0x0c) {                              /* 666-687: store it; the index is NOT masked by flags */
            uint8_t* qo = bank + o + ((mode & 8) ? 0x10 : 0);
            for (int i = 0; i < 4; ++i) jm_wf(qo + 4 * i, q[i]);
            continue;
        }
        /* @L_8000713C, 689-792: quaternion -> 3x3, column scale, translation, into bank[idx] */
        uint8_t* M = bank + idx * 0x40;                 /* 690-692 */
        const float x2 = q[1] * 2.0f, y2 = q[2] * 2.0f, z2 = q[3] * 2.0f;  /* 664, 689, 709 */
        int tr[3]; unsigned sc[3];                      /* 694-707: EVERY load before the first store (M may be pp - 0x1c) */
        for (int k = 0; k < 3; ++k) tr[k] = jm_rs(pp + 0x18 + 2 * k);
        for (int k = 0; k < 3; ++k) sc[k] = jm_ru(pp + 0xc + 2 * k);
        float t[3];
        for (int k = 0; k < 3; ++k) {
            float a = (float)tr[k] * JM_HALF_UNIT;      /* psq_l 1,5 ; fmuls */
            t[k] = a + jm_rf(b + 4 + 4 * k);            /* fadds with the bone's head */
        }
        const float wx = q[0] * x2, wy = q[0] * y2, wz = q[0] * z2;        /* 711, 713, 715 */
        const float xx = q[1] * x2, xy = q[1] * y2, xz = q[1] * z2;        /* 716-718 */
        const float zz = q[3] * z2, yy = q[2] * y2, yz = q[2] * z2;        /* 720, 722, 724 */
        float c0[3], c1[3], c2[3], s;
        s = yy + zz; c0[0] = 1.0f - s; c0[1] = xy + wz; c0[2] = xz - wy;   /* f19 f20 f21 */
        s = xx + zz; c1[0] = xy - wz; c1[1] = 1.0f - s; c1[2] = yz + wx;   /* f2 f3 f4 */
        s = xx + yy; c2[0] = xz + wy; c2[1] = yz - wx; c2[2] = 1.0f - s;   /* f25 f26 f10 */
        jm_wf(M + 0x0c, t[0]); jm_wf(M + 0x2c, t[2]); jm_wf(M + 0x1c, t[1]);   /* 708, 712, 714 */
        if (sc[0] == 0) {                               /* 735-739 QUIRK: the unscaled X column is stored across */
            jm_wf(M + 0x0, c0[0]);                      /* ROW 0 (+0, +4, +8); +4 and +8 are overwritten just     */
            jm_wf(M + 0x4, c0[1]);                      /* below, +0x10 and +0x20 are NOT WRITTEN and keep what   */
            jm_wf(M + 0x8, c0[2]);                      /* the bank held. Reached only when the scale read is 0.  */
        } else {                                        /* 758-767 */
            float f = (float)sc[0] * JM_SCALE_UNIT;     /* sth ; psq_l 1,3 ; fmuls */
            jm_wf(M + 0x00, c0[0] * f); jm_wf(M + 0x10, c0[1] * f); jm_wf(M + 0x20, c0[2] * f);
        }
        if (sc[1] == 0) {                               /* 742-745 */
            jm_wf(M + 0x04, c1[0]); jm_wf(M + 0x14, c1[1]); jm_wf(M + 0x24, c1[2]);
        } else {                                        /* 770-779 */
            float f = (float)sc[1] * JM_SCALE_UNIT;
            jm_wf(M + 0x04, c1[0] * f); jm_wf(M + 0x14, c1[1] * f); jm_wf(M + 0x24, c1[2] * f);
        }
        if (sc[2] == 0) {                               /* 748-751 */
            jm_wf(M + 0x08, c2[0]); jm_wf(M + 0x18, c2[1]); jm_wf(M + 0x28, c2[2]);
        } else {                                        /* 782-791 */
            float f = (float)sc[2] * JM_SCALE_UNIT;
            jm_wf(M + 0x08, c2[0] * f); jm_wf(M + 0x18, c2[1] * f); jm_wf(M + 0x28, c2[2] * f);
        }
    }
    return (mode & 0x0c) ? 0 : 1;                       /* 686-687 blr / 757 b @hierarchy */
}

/* @singlePose, render.c:1266-1538: one pose, FULL angles (lhz, no halving), euler matrix. The twin has this
   path (eulerMat34 + stage 3, with sinf/cosf, proven to 2e-7 on retail vectors); it is here because modes
   0x02 / 0x04 / 0x08 also exit through it when the countdown is <= 0 (render.c:406-409). */
static void jm_single_pose(const JmCtx* c) {
    for (int j = 0; j < c->jc; ++j) {
        const uint8_t* b = c->jd + j * 0x1c;
        int idx = (int)(int8_t)b[1] & c->flags;         /* 1283-1287 */
        if (idx < 0) continue;
        uint8_t* M = c->bank + idx * 0x40;              /* 1294, 1296 (no & 0x7f here) */
        const uint8_t* p = c->slots + b[2] * 0x40;      /* 1295-1298 */
        float c0, s0, c1, s1, c2, s2;
        jm_sincos((int)jm_ru(p), &c0, &s0);             /* 1319: lhz */
        jm_sincos((int)jm_ru(p + 2), &c1, &s1);
        jm_sincos((int)jm_ru(p + 4), &c2, &s2);
        for (int k = 0; k < 3; ++k) {                   /* 1433-1448 */
            float a = (float)jm_rs(p + 0x18 + 2 * k) * JM_HALF_UNIT;
            jm_wf(M + 0x0c + 0x10 * k, jm_rf(b + 4 + 4 * k) + a);
        }
        const float f7 = c0 * s2, f6 = s0 * s2, f8 = s0 * c2, f9 = c0 * c2;    /* 1447-1451 */
        float col[3][3], m;
        col[0][0] = c1 * c2; col[0][1] = c1 * s2; col[0][2] = 0.0f - s1;        /* 1452-1458 */
        m = f8 * s1; col[1][0] = m - f7; m = f6 * s1; col[1][1] = m + f9; col[1][2] = s0 * c1;  /* 1461-1470 */
        m = f9 * s1; col[2][0] = m + f6; m = f7 * s1; col[2][1] = m - f8; col[2][2] = c0 * c1;  /* 1473-1482 */
        for (int k = 0; k < 3; ++k) {                   /* 1453-1538: a zero scale leaves the column as is */
            unsigned sc = jm_ru(p + 0xc + 2 * k);
            if (sc) {
                float f = (float)sc * JM_SCALE_UNIT;
                for (int i = 0; i < 3; ++i) jm_wf(M + 4 * k + 0x10 * i, col[k][i] * f);
            } else
                for (int i = 0; i < 3; ++i) jm_wf(M + 4 * k + 0x10 * i, col[k][i]);
        }
    }
}

/* @hierarchy, render.c:1539-1651: bank[bone] = parent * bank[bone], in bone order; bone 0's parent is dst.
   The twin does this as its stages 4 + 5 (model-space FK, then dst prepended to every bone), which is the same
   product associated differently and with unfused arithmetic: it matched retail to 8e-6, not bit for bit.
   This is retail's order with retail's fused multiply-adds. Use ONE of the two, not both. */
static void jm_hierarchy(const JmCtx* c, int mode) {
    if (mode & 0x0c) return;                            /* 1543-1545 */
    for (int j = 0; j < c->jc; ++j) {
        const uint8_t* b = c->jd + j * 0x1c;
        int m = (int)(int8_t)b[1] & c->flags;
        if (m < 0) continue;                            /* 1563-1570, 1578-1585 */
        uint8_t* M;
        float P[12], C[12], R[12];
        if (j == 0) {
            M = c->bank + (b[1] & 0x7f) * 0x40;         /* 1556, 1561-1562 QUIRK: bone 0's index is not masked by flags */
            for (int i = 0; i < 12; ++i) P[i] = jm_rf(c->dst + 4 * i);     /* 1546, 1598-1603 */
        } else {
            M = c->bank + m * 0x40;                     /* 1587-1588 */
            const uint8_t* pm = c->bank + b[0] * 0x40;  /* 1590, 1595-1596: parent = a u8 bank index (lbz), never "none".
                                                           When it equals the previous bone's index retail reuses the
                                                           registers (1591-1593, 1606-1611): the same values. */
            for (int i = 0; i < 12; ++i) P[i] = jm_rf(pm + 4 * i);
        }
        for (int i = 0; i < 12; ++i) C[i] = jm_rf(M + 4 * i);              /* 1613-1618 */
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 4; ++col) {
                float acc = C[col] * P[r * 4];                              /* ps_muls0, 1619-1624 */
                acc = jm_fmadds(C[4 + col], P[r * 4 + 1], acc);             /* ps_madds1, 1625-1630 */
                acc = jm_fmadds(C[8 + col], P[r * 4 + 2], acc);             /* ps_madds0, 1631-1636 */
                if (col >= 2)                                               /* ps_madds1 by the pair (0, 1), 1637-1639 */
                    acc = jm_fmadds(col == 2 ? 0.0f : 1.0f, P[r * 4 + 3], acc);
                R[r * 4 + col] = acc;
            }
        for (int i = 0; i < 12; ++i) jm_wf(M + 4 * i, R[i]);               /* 1640-1645 */
    }
}

/* ============================== the paths ============================== */

/* render.c:391-435: the entry for every mode WITHOUT 0x40. The three named paths below are this function. */
static void jm_entry_blend(const JmCtx* c, int mode) {
    uint8_t* A = c->slots;                              /* 385-387: r11 */
    uint8_t* B = c->slots + 6;                          /* 388: r15 */
    if (mode & 1) {                                     /* 391-396: pose A = the bank (and NO countdown test) */
        A = c->bank + 0x1c;
    } else {                                            /* 397-409 */
        jm_decode_interpolated(c->slots, c->mfd[0], c->cur[0], c->stride[0], c->phase[0], c->fix, 0);
        if (c->countdown <= 0) {                        /* 406-409 -> @singlePose, under ANY of 0x02 0x04 0x08 0x10 0x20 */
            jm_single_pose(c);
            jm_hierarchy(c, mode);                      /* 1539-1545: skipped under 0xC, so a 0x04 / 0x08 call leaves */
            return;                                     /* LOCAL MATRICES where the next call expects quaternions      */
        }
    }
    if (mode & 2) {                                     /* 411-416: pose B = the bank */
        B = c->bank + 0x22;
    } else {                                            /* 417-426: channel 1 into S+6, fixup column 1 (r8 += 2) */
        jm_decode_interpolated(c->slots + 6, c->mfd[1], c->cur[1], c->stride[1], c->phase[1], c->fix, 1);
    }
    const float w = (float)c->countdown / JM_PHASE_SCALE;   /* 428-431: psq_l 1,5 ; fdivs */
    jm_blend_joints(c, A, B, mode, c->countdown, w);    /* 432-434 */
    jm_hierarchy(c, mode);                              /* 435 / 757: under 0xC nothing more happens */
}

/* PATH: mode 0x04 / 0x08 (model.c:473, blendMask = 4 << i). Channel pair (i, i+2) is blended by
   eventState / prevEventState and STORED: quaternion at bank[idx] + 0 (0x04) or + 0x10 (0x08), scale and
   translation in the pose record laid over the entry (+0x28 / +0x34 for 0x04, +0x2e / +0x3a for 0x08). No
   matrix, no hierarchy. The caller's bits 0x10 / 0x20 are never present here (model.c:474 passes blendMask). */
static void jm_path_store_quaternion(const JmCtx* c, int mode) { jm_entry_blend(c, mode); }

/* PATH: mode 0x01 / 0x02 / 0x03 (| 0x10 | 0x20) (model.c:507, outFlags). Pose A and / or pose B come from what
   the store path left in the bank; the result is the joint matrices and the hierarchy. */
static void jm_path_blend_stored(const JmCtx* c, int mode) { jm_entry_blend(c, mode); }

/* PATH: mode 0x40 (model.c:436, models whose file flags & 8), @cachePass, render.c:436-476. */
static void jm_path_cache(const JmCtx* c) {
    uint8_t* S = c->slots;
    /* 437-443: move 0, frames k and k+1, uninterpolated */
    jm_decode_paired(S, c->mfd[0], c->cur[0], c->stride[0], c->fix);
    /* 446-452: the weight between the two frames = the phase fraction; r16 is read back SIGNED (lha) */
    float fr;
    int w16 = (int16_t)jm_phase_frac(c->phase[0], &fr);
    /* 453-455: mode := 4. Frame k x frame k+1 -> quaternion + scale + translation in the bank (the 0x04 form).
       QUIRK: pose B's record is taken at bone byte 3 (channel 1's slot map), though it holds move 0's frame. */
    if (jm_blend_joints(c, S, S + 6, 4, w16, fr))
        return;     /* QUIRK (651-655): the LAST bone is masked by flags -> @hierarchy with r10 = 4 -> @epilogue.
                       The whole call ends here: channel 1 is not decoded and no matrix is built. */
    /* 456-465: channel 1, interpolated, into S+6. r8 was not advanced: fixup COLUMN 0, applied at S+6. */
    jm_decode_interpolated(S + 6, c->mfd[1], c->cur[1], c->stride[1], c->phase[1], c->fix, 0);
    /* 466-475: mode := 1. The caller's 0x10 / 0x20 are DROPPED on this path (r10 is overwritten at 454, 471). */
    const float w = (float)c->countdown / JM_PHASE_SCALE;
    jm_blend_joints(c, c->bank + 0x1c, S + 6, 1, c->countdown, w);
    jm_hierarchy(c, 1);
}

/* The whole retail entry, render.c:389-390 then one of the above. Used by the host test for every mode. */
static void jm_build(const JmCtx* c, int mode) {
    if (mode & 0x40) jm_path_cache(c);
    else jm_entry_blend(c, mode);
}

/* PORT GUARD, not retail: every index the paths will dereference, checked once. Retail trusts its data. */
static int jm_inputs_ok(const JmCtx* c, int bankEntries) {
    if (!c->bank || !c->slots || !c->jd || c->jc <= 0) return 0;
    for (int j = 0; j < c->jc; ++j) {
        const uint8_t* b = c->jd + j * 0x1c;
        if (b[2] >= JM_MAX_SLOTS || b[3] >= JM_MAX_SLOTS) return 0;
        if ((b[1] & 0x7f) >= bankEntries) return 0;
        if (j > 0 && b[0] >= bankEntries && ((int)(int8_t)b[1] & c->flags) >= 0) return 0;  /* a parent outside the bank */
    }
    return 1;
}

/* ============================== how it goes into the twin ==============================
 *
 * game_scene.cpp, modelAnimBuildJointMatrices. REPLACE the line
 *
 *      if (mode & 0x0C) return;                        // blend path not yet ported; leave bank as-is
 *
 * (it sits after the [jmode] census and BEFORE `uint8_t* mfd = ...` / the stage-1 decode) with
 *
 *      if (jm_twin_dispatch(bank, dst, work, jd, jc, scratch, flags, mode, (uint8_t*)gAnimSlots, MAX_JOINTS)) return;
 *
 * and nothing else changes: modes 0x00 / 0x10 / 0x20 fall through to the twin's own stages exactly as today
 * (single pose, and the cross-fade under `fade = fadeCd > 0 && !(mode & 0x4f)`), which are the ones proven
 * against retail memory. The dispatch must come before the twin's `memset(gAnimSlots, 0, ...)` and its
 * channel-0 decode: under mode & 1 retail does not decode channel 0 at all (work+0x34 may be stale), and
 * retail never clears the slot buffer.
 *   - jm_twin_dispatch returns 1 when it handled the call (mode & 0x4f != 0), 0 for the twin's own modes.
 *   - `!mfd` / `!cur` returns in the twin are port guards; the equivalents are in jm_twin_dispatch.
 *   - The twin's [jvec] dumps (JSLOTS / JBANK) are not reached on these paths; add them after the call if
 *     vectors are wanted from the device.
 */
#if UINTPTR_MAX == 0xffffffffu || defined(JM_HOST_TEST)
static int jm_twin_dispatch(uint8_t* bank, const uint8_t* dst, const void* work, const uint8_t* jd, int jc,
                            const uint8_t* scratch, int flags, int mode, uint8_t* slots, int bankEntries) {
    if (!(mode & 0x4f)) return 0;                       /* 0x00 / 0x10 / 0x20: the twin's own, proven code */
    JmCtx c;
    c.bank = bank; c.dst = dst; c.slots = slots; c.jd = jd; c.jc = jc; c.fix = scratch; c.flags = flags;
#if UINTPTR_MAX == 0xffffffffu
    {   /* the work struct as the twin reads it (ObjAnimState, 32-bit pointers) */
        const uint8_t* w = (const uint8_t*)work;
        memcpy(&c.phase[0], w + 0x04, 4); memcpy(&c.phase[1], w + 0x08, 4);
        memcpy(&c.cur[0], w + 0x2c, 4);   memcpy(&c.cur[1], w + 0x30, 4);
        memcpy(&c.mfd[0], w + 0x34, 4);   memcpy(&c.mfd[1], w + 0x38, 4);
        memcpy(&c.stride[0], w + 0x4c, 2); memcpy(&c.stride[1], w + 0x4e, 2);
        memcpy(&c.countdown, w + 0x58, 2);
    }
#else
    c = *(const JmCtx*)work;                            /* host test: the caller hands over a filled JmCtx */
#endif
    if (!jm_inputs_ok(&c, bankEntries)) return 1;       /* PORT GUARD: leave the bank as it is */
    /* PORT GUARD: which channels this mode byte will actually decode (work+0x38 is the caller's uninitialised
       stack when one channel is in use: docs/47 run 126). Retail dereferences without looking. */
    {
        int need0 = (mode & 0x40) || !(mode & 1);
        int need1 = (mode & 0x40) ? 1 : (!(mode & 2) && ((mode & 1) || c.countdown > 0));
        if (need0 && (!c.mfd[0] || !c.cur[0])) return 1;
        if (need1 && (!c.mfd[1] || !c.cur[1])) return 1;
    }
    if (mode & 0x40) jm_path_cache(&c);                 /* model.c:436 */
    else if (mode & 0x0c) jm_path_store_quaternion(&c, mode);   /* model.c:473 */
    else jm_path_blend_stored(&c, mode);                /* model.c:507 with outFlags & 3 */
    return 1;
}
#endif

/* ============================== host test runner ============================== */
#ifdef JM_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
/* cases as jointmtx_modes_model.py pack_case() writes them; output = bank then slot buffer, per case */
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s cases.bin out.bin\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* in = (uint8_t*)malloc((size_t)n + 64);
    if (fread(in, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    FILE* o = fopen(argv[2], "wb");
    if (!o) return 2;
    static uint8_t bank[128 * 0x40], slots[JM_MAX_SLOTS * 0x40];
    const uint8_t* p = in;
    while (p < in + n) {
        int32_t h[7], cur[2], len[5]; int16_t sh[6]; float ph[2];
        memcpy(h, p, 28); p += 28; memcpy(sh, p, 12); p += 12; memcpy(ph, p, 8); p += 8;
        memcpy(cur, p, 8); p += 8; memcpy(len, p, 20); p += 20;
        if ((uint32_t)h[0] != 0x4a4d4f44u) { fprintf(stderr, "bad magic\n"); return 3; }
        int mode = h[1], nb = h[4], nsl = h[5], chain = h[6];
        JmCtx c;
        c.flags = h[2]; c.jc = h[3];
        c.countdown = sh[0]; c.stride[0] = sh[1]; c.stride[1] = sh[2];
        gModelRootRotX = sh[3]; gModelRootRotY = sh[4]; gModelRootRotZ = sh[5];
        c.phase[0] = ph[0]; c.phase[1] = ph[1];
        c.jd = p; p += c.jc * 0x1c;
        c.fix = p; p += len[0];
        c.mfd[0] = p; p += len[1];
        c.cur[0] = p + cur[0]; p += len[2];
        c.mfd[1] = p; p += len[3];
        c.cur[1] = p + cur[1]; p += len[4];
        c.dst = p; p += 48;
        if (!chain) { memcpy(bank, p, (size_t)nb * 0x40); memcpy(slots, p + nb * 0x40, (size_t)nsl * 0x40); }
        p += (nb + nsl) * 0x40;
        c.bank = bank; c.slots = slots;
        /* through the twin's dispatcher for the modes it takes; the twin's own modes (0x00 / 0x10 / 0x20) through
           the full retail entry, as the control group */
        if (!jm_twin_dispatch(bank, c.dst, &c, c.jd, c.jc, c.fix, c.flags, mode, slots, nb)) jm_build(&c, mode);
        fwrite(bank, 1, (size_t)nb * 0x40, o); fwrite(slots, 1, (size_t)nsl * 0x40, o);
    }
    fclose(o);
    return 0;
}
#endif
