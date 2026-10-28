// tev_ref.c - the GX TEV combiner in exact integer arithmetic: the host ORACLE for the generated
// fragment programs (tev_gen.c; host/tests/tev_shader_test.cpp). Follows Dolphin's software
// renderer (VideoBackends/Software/Tev.cpp), the reference GX implementation:
//   a, b, c are 8-bit (the low 8 bits of their source), d is 11-bit signed;
//   c' = c + (c >> 7);  t = (a*(256-c') + b*c') << scaleL;  t += divide2 ? 0 : (sub ? 127 : 128);
//   t >>= 8;  result = ((d + bias) << scaleL) +- t;  result >>= scaleR;
//   clamp ? [0,255] : [-1024,1023].  Every stage reads its inputs BEFORE any output is written.
// COMPARE-MODE stages (op 8..15; dolsdk GXTev.c stores bias 3 and shift = (op >> 1) & 3 for them; Dolphin's software TEV, DrawColorCompare):
//   out = d + ((a OP b) ? c : 0), no bias, no scale, the clamp flag still applies; a, b, c are the low 8 bits (c NOT weighted c' = c + (c >> 7)).
//   op 8 R8_GT 9 R8_EQ (r of a vs r of b, one verdict for r, g, b)  10 GR16_GT 11 GR16_EQ (g*256+r)  12 BGR24_GT 13 BGR24_EQ (b*65536+g*256+r)
//   14 RGB8_GT 15 RGB8_EQ (per channel). Alpha: ops 14/15 only (A8_GT / A8_EQ: aa vs ab); alpha 8..13 are refused by tev_gen (unverified).
// Ops 2..7, >15 and bias 3 on an add/sub are refused by tev_gen and not evaluated here.
#include "port/gx_tev_key.h"
#include <math.h>
#include <string.h>

static const int kBias[4] = { 0, 128, -128, 0 };
static const int kScaleL[4] = { 0, 1, 2, 0 };
static const int kScaleR[4] = { 0, 0, 0, 1 };
static const int kFrac[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };   // GX_TEV_KCSEL_1 .. _1_8

static int s11(int v) { v &= 0x7ff; return v >= 0x400 ? v - 0x800 : v; }
static int konstC(const TevUniforms* u, int sel, int ch) {
    if (sel < 8) return kFrac[sel];
    if (sel >= 12 && sel <= 15) return u->konst[sel - 12][ch];      // K0..K3 rgb
    if (sel >= 16 && sel <= 31) return u->konst[(sel - 16) & 3][(sel - 16) >> 2];   // Kn_R/G/B/A
    return 0;
}
static int konstA(const TevUniforms* u, int sel) {
    if (sel < 8) return kFrac[sel];
    if (sel >= 16 && sel <= 31) return u->konst[(sel - 16) & 3][(sel - 16) >> 2];
    return 0;
}
static int cmp(int f, int a, int ref) {
    switch (f) {
    case 0: return 0; case 1: return a < ref; case 2: return a == ref; case 3: return a <= ref;
    case 4: return a > ref; case 5: return a != ref; case 6: return a >= ref; default: return 1;
    }
}

// INDIRECT texturing (2026-10-12) -- the oracle. Per TEV stage with an indirect setup (TevIndKey), in TEXELS of the stage's texture:
//   rc = vt[slot] * size  (0 without a texture), wrap: ITW_0 zeroes an axis
//   (S, T, U) = the indirect texel (nearest sample of slot ik.slot[ind] at vt / 2^scale) as (A, B, G) -- or the order tev_ind_stu selects --
//               minus 128 for each bias bit;   offset = indM[m] * (S, T, U)  (indM already holds element / 1024 * 2^exp)
//   coordinate = rc + offset (+ the previous stage's final coordinate with add-previous); the stage samples at coordinate / size.
//   bump alpha = the selected component's raw value & 0xF8; GX_ALPHA_BUMP: ras = bump in every channel; _BUMPN: bump + (bump >> 5).
// A stage without an indirect setup carries its regular coordinate (in texels) as "previous".
// Sources: dolsdk2004 src/gx/GXBump.c (register packing: bias bits S=1 T=2 U=4, matrix elements s1.10 + shared exponent, scale = 2^-n);
// Dolphin's software TEV for the A, B, G channel order and the 0xF8 bump mask -- NOT available on this machine, so those two are the
// documented assumptions (tev_ind_stu.txt switches the order on the device; see tev_gen.c).
/* ik.stu: 0: S,T,U <- A,B,G (assumed); 1: R,G,B; 2: A,R,G */
static const int kStuCh[3][3] = { { 3, 2, 1 }, { 0, 1, 2 }, { 3, 0, 1 } };   /* channel indices into r,g,b,a for S, T, U */
static void constTexFn(void* ctx, int slot, double uu, double vv, uint8_t rgba[4]) { (void) uu; (void) vv; memcpy(rgba, ((const uint8_t (*)[4]) ctx)[slot], 4); }

static void evalCore(const TevKey* k, const TevUniforms* u, TevTexFn fn, void* ctx, const double vt[TEVK_TEX][2], const double texSz[TEVK_TEX][2],
                     const uint8_t ras[2][4], int16_t out[4], int* discarded);
void tev_ref_eval(const TevKey* k, const TevUniforms* u, const uint8_t tex[TEVK_TEX][4],
                  const uint8_t ras[2][4], int16_t out[4], int* discarded) {
    static const double z[TEVK_TEX][2];
    evalCore(k, u, constTexFn, (void*) tex, z, z, ras, out, discarded);
}
void tev_ref_eval_ex(const TevKey* k, const TevUniforms* u, TevTexFn fn, void* ctx, const double vt[TEVK_TEX][2], const double texSz[TEVK_TEX][2],
                     const uint8_t ras[2][4], int16_t out[4], int* discarded) {
    evalCore(k, u, fn, ctx, vt, texSz, ras, out, discarded);
}
static void evalCore(const TevKey* k, const TevUniforms* u, TevTexFn fn, void* ctx, const double vt[TEVK_TEX][2], const double texSz[TEVK_TEX][2],
                     const uint8_t ras[2][4], int16_t out[4], int* discarded) {
    double pcx = 0, pcy = 0;   // the previous stage's final coordinate, texels
    int R[4][4];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) R[r][c] = u->reg[r][c];
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) {
        const TevKeyStage* st = &k->st[s];
        int T[4] = { 0, 0, 0, 0 }, A[4] = { 0, 0, 0, 0 };   // swizzled texel, rasterised colour
        const int ind = k->ik.on && k->ik.st[s][0];
        double ux = 0, uy = 0; int bump = 0;
        if (st->texSlot != 0xff) { ux = vt[st->texSlot][0] * texSz[st->texSlot][0]; uy = vt[st->texSlot][1] * texSz[st->texSlot][1]; }
        const double rcx = ux, rcy = uy;   // the regular coordinate BEFORE the wrap: what the dynamic matrices multiply
        if (ind) {
            const uint8_t* q = k->ik.st[s];
            if (q[4] & 1) ux = 0;
            if (q[4] & 2) uy = 0;
            const int mtx = q[3] <= 9 ? q[3] : 0, alpha = q[5] >> 1 & 3;
            if (mtx || alpha) {
                const int is = q[1] & 3, sl = k->ik.slot[is];
                uint8_t it[4];
                fn(ctx, sl, vt[sl][0] / (double) (1 << k->ik.sc[is][0]), vt[sl][1] / (double) (1 << k->ik.sc[is][1]), it);
                double stu[3]; int raw[3];
                for (int i = 0; i < 3; ++i) { raw[i] = it[kStuCh[k->ik.stu % 3][i]]; stu[i] = raw[i] - ((q[2] >> i & 1) ? 128 : 0); }
                if (alpha) bump = raw[alpha - 1] & 0xF8;
                if (mtx >= 4) {   // dynamic: (s, t) * (ind / 256) * 2^exp, ITM_S takes the S component, ITM_T the T component (GXSetTevIndBumpST)
                    const int reg = mtx >= 7 ? mtx - 7 : mtx - 4;
                    const double f = stu[mtx >= 7 ? 1 : 0] / 256.0 * (double) u->indE[reg];
                    ux += rcx * f; uy += rcy * f;
                } else if (mtx) {
                    const float* m0 = u->indM[mtx - 1][0]; const float* m1 = u->indM[mtx - 1][1];
                    ux += (double) m0[0] * stu[0] + (double) m0[1] * stu[1] + (double) m0[2] * stu[2];
                    uy += (double) m1[0] * stu[0] + (double) m1[1] * stu[1] + (double) m1[2] * stu[2];
                }
            }
            if (q[5] & 1) { ux += pcx; uy += pcy; }
        }
        pcx = ux; pcy = uy;
        if (st->texSlot != 0xff && !(ind && (k->ik.st[s][4] & 4))) {   // (GX_TEXMAP_DISABLE: no sampling)
            const uint8_t* sw = k->swap[st->texSwap & 3];
            uint8_t tx4[4];
            if (ind) fn(ctx, st->texSlot, ux / texSz[st->texSlot][0], uy / texSz[st->texSlot][1], tx4);
            else     fn(ctx, st->texSlot, vt[st->texSlot][0], vt[st->texSlot][1], tx4);
            for (int c = 0; c < 4; ++c) T[c] = tx4[sw[c] & 3];
        }
        if (st->ras == 0 || st->ras == 1) {
            const uint8_t* sw = k->swap[st->rasSwap & 3];
            for (int c = 0; c < 4; ++c) A[c] = ras[st->ras][sw[c] & 3];
        } else if ((st->ras == 2 || st->ras == 3) && ind && (k->ik.st[s][5] >> 1 & 3)) {
            const int b = st->ras == 3 ? bump + (bump >> 5) : bump;
            for (int c = 0; c < 4; ++c) A[c] = b;
        }
        int in[4][4];   // [arg a,b,c,d][r,g,b,a]
        for (int arg = 0; arg < 4; ++arg) {
            for (int c = 0; c < 3; ++c) {
                int v;
                switch (st->cin[arg]) {
                case 0: v = R[0][c]; break;  case 1: v = R[0][3]; break;
                case 2: v = R[1][c]; break;  case 3: v = R[1][3]; break;
                case 4: v = R[2][c]; break;  case 5: v = R[2][3]; break;
                case 6: v = R[3][c]; break;  case 7: v = R[3][3]; break;
                case 8: v = T[c]; break;     case 9: v = T[3]; break;
                case 10: v = A[c]; break;    case 11: v = A[3]; break;
                case 12: v = 255; break;     case 13: v = 128; break;
                case 14: v = konstC(u, st->kcsel, c); break;
                default: v = 0; break;
                }
                in[arg][c] = v;
            }
            {
                int v;
                switch (st->ain[arg]) {
                case 0: v = R[0][3]; break;  case 1: v = R[1][3]; break;
                case 2: v = R[2][3]; break;  case 3: v = R[3][3]; break;
                case 4: v = T[3]; break;     case 5: v = A[3]; break;
                case 6: v = konstA(u, st->kasel); break;
                default: v = 0; break;
                }
                in[arg][3] = v;
            }
        }
        int res[4];
        for (int c = 0; c < 4; ++c) {
            const uint8_t* op = c < 3 ? st->cop : st->aop;
            int a = in[0][c] & 255, b = in[1][c] & 255, cc = in[2][c] & 255, d = s11(in[3][c]);
            #ifdef TEVREF_BREAK_CMP   /* host negative control: compare ops evaluated as the add they used to be */
            if (0) {
#else
            if (op[0] >= 8) {   // compare mode
#endif
                int gt = !(op[0] & 1), shift = c == 3 ? 3 : (op[0] - 8) >> 1, ch = shift == 3 ? c : 0, hit;
                int av = 0, bv = 0;
                if (shift == 3) { av = a; bv = b; }
                else { int nb = shift + 1;   // channels r (0), g, b: most significant is the highest
                       for (int i = nb - 1; i >= 0; --i) { av = av * 256 + (in[0][i] & 255); bv = bv * 256 + (in[1][i] & 255); } }
                (void) ch;
                hit = gt ? av > bv : av == bv;
                int r = d + (hit ? cc : 0);
                if (op[3]) r = r < 0 ? 0 : r > 255 ? 255 : r;
                else       r = r < -1024 ? -1024 : r > 1023 ? 1023 : r;
                res[c] = r;
                continue;
            }
            int sc = op[2] & 3, sub = op[0] == 1;
            int c2 = cc + (cc >> 7);
            int t = (a * (256 - c2) + b * c2) << kScaleL[sc];
            t += sc == 3 ? 0 : (sub ? 127 : 128);
            t >>= 8;
            if (sub) t = -t;
            int r = ((d + kBias[op[1] & 3]) << kScaleL[sc]) + t;
            r >>= kScaleR[sc];
            if (op[3]) r = r < 0 ? 0 : r > 255 ? 255 : r;
            else       r = r < -1024 ? -1024 : r > 1023 ? 1023 : r;
            res[c] = r;
        }
        for (int c = 0; c < 3; ++c) R[st->cop[4] & 3][c] = res[c];
        R[st->aop[4] & 3][3] = res[3];
    }
    for (int c = 0; c < 4; ++c) { int v = R[0][c]; out[c] = (int16_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    if (discarded) {
        int a = out[3], p0 = cmp(k->acComp0, a, u->acRef0), p1 = cmp(k->acComp1, a, u->acRef1), pass;
        switch (k->acOp) { case 0: pass = p0 && p1; break; case 1: pass = p0 || p1; break;
                           case 2: pass = p0 != p1; break; default: pass = p0 == p1; break; }
        *discarded = !pass;
    }
}

// FOG (2026-10-07, docs/47 run 116): the contract tev_gen's fog code is tested against. Floats.
//   f = clamp((ze - start) * scale, 0, 1); linear f; exp 1-2^(-8f); exp2 1-2^(-8f^2);
//   rev exp 2^(-8(1-f)); rev exp2 2^(-8(1-f)^2);  rgb = rgb + (fogColor - rgb) * fog, alpha untouched.
void tev_ref_fog(const TevKey* k, const TevUniforms* u, float ze, int16_t rgba[4]) {
    if (!k->fogType) return;
    float f = (ze - u->fogStart) * u->fogScale; f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    float g;
    switch (k->fogType) {
    case 2: g = f; break;
    case 4: g = 1.0f - exp2f(-8.0f * f); break;
    case 5: g = 1.0f - exp2f(-8.0f * f * f); break;
    case 6: g = exp2f(-8.0f * (1.0f - f)); break;
    case 7: g = exp2f(-8.0f * (1.0f - f) * (1.0f - f)); break;
    default: return;
    }
    for (int c = 0; c < 3; ++c) {
        float v = rgba[c] + ((float)u->fogColor[c] - rgba[c]) * g;
        rgba[c] = (int16_t)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f));
    }
}
