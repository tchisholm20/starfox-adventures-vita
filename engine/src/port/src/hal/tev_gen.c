// tev_gen.c - a TevKey -> one Cg fragment program (vita fork, docs/47).
//
// One BRANCHLESS program per distinct key (docs/42 s8.3: a dynamic branch saves nothing on this GPU,
// and dead code costs), generated straight-line from the stages.
//
// EXACT: the program carries GX's integer CODES in fp32 (every value is an integer below 2^20, so
// fp32 holds it exactly and floor() is exact) and performs the combiner's own arithmetic -- 8-bit
// wrap of a/b/c, c' = c + (c >> 7), the +128 / +127 rounding, the bias-then-scale, the arithmetic
// right shifts, the [0,255] / [-1024,1023] clamps -- so its output equals tev_ref.c's bit for bit.
// A first version worked in normalised floats: the device's 48 census keys were within 2 codes, but
// random keys reached 255 codes wherever an unclamped register crossed the 8-bit wrap. Exactness is
// what lets the host test demand ZERO difference instead of arguing about a tolerance.
//
// The BODY is written in a dialect that is valid Cg AND valid C++ with host/tests/tev_cg_compat.h:
// float4 constructors (no swizzles -- they are spelled as constructors), .x/.y/.z/.w, + - *, floor,
// clamp, abs, sign, step, tex2D, clip. So every program the device will compile is first COMPILED
// AND RUN on the host against tev_ref.c (host/tests/tev_shader_test.cpp). A Cg compile error on the
// Vita is a process death and a console reboot, not an error message (CLAUDE.md).
//
// Uniforms (all in GX codes): uR0..uR3 = TEVPREV, TEVREG0..2 initial values (S10), uK0..uK3 = konst
// colours, uAref = (ref0, ref1). Varyings: vColor0/1 in 0..1 (rasterised channels), vT0..vT3.
#include "port/gx_tev_key.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct { char* p; int n, cap, bad; } Out;
static void emit(Out* o, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(o->p + o->n, o->cap > o->n ? (size_t)(o->cap - o->n) : 0, fmt, ap);
    va_end(ap);
    if (w < 0 || o->n + w >= o->cap) o->bad = 1; else o->n += w;
}
static const char* kReg[4] = { "rP", "r0", "r1", "r2" };
static const char kCh[4] = { 'x', 'y', 'z', 'w' };
static const int kFrac[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };

// A float4 whose channels come from `v` through swap table row `sw`.
static void swz(Out* o, const char* v, const uint8_t* sw) {
    emit(o, "float4(%s.%c, %s.%c, %s.%c, %s.%c)", v, kCh[sw[0] & 3], v, kCh[sw[1] & 3], v, kCh[sw[2] & 3], v, kCh[sw[3] & 3]);
}
static void konstC(Out* o, int sel) {
    if (sel < 8) { emit(o, "float4(%d.0, %d.0, %d.0, 0.0)", kFrac[sel], kFrac[sel], kFrac[sel]); return; }
    if (sel >= 12 && sel <= 15) { emit(o, "uK%d", sel - 12); return; }
    if (sel >= 16 && sel <= 31) { int k = (sel - 16) & 3; char c = kCh[(sel - 16) >> 2];
                                  emit(o, "float4(uK%d.%c, uK%d.%c, uK%d.%c, 0.0)", k, c, k, c, k, c); return; }
    emit(o, "float4(0.0, 0.0, 0.0, 0.0)");
}
static void konstA(Out* o, int sel) {
    if (sel < 8) { emit(o, "%d.0", kFrac[sel]); return; }
    if (sel >= 16 && sel <= 31) { emit(o, "uK%d.%c", (sel - 16) & 3, kCh[(sel - 16) >> 2]); return; }
    emit(o, "0.0");
}
static void colorArg(Out* o, int arg, int kcsel) {
    switch (arg) {
    case 0: emit(o, "rP"); break;
    case 1: emit(o, "float4(rP.w, rP.w, rP.w, 0.0)"); break;
    case 2: emit(o, "r0"); break;
    case 3: emit(o, "float4(r0.w, r0.w, r0.w, 0.0)"); break;
    case 4: emit(o, "r1"); break;
    case 5: emit(o, "float4(r1.w, r1.w, r1.w, 0.0)"); break;
    case 6: emit(o, "r2"); break;
    case 7: emit(o, "float4(r2.w, r2.w, r2.w, 0.0)"); break;
    case 8: emit(o, "tx"); break;
    case 9: emit(o, "float4(tx.w, tx.w, tx.w, 0.0)"); break;
    case 10: emit(o, "rs"); break;
    case 11: emit(o, "float4(rs.w, rs.w, rs.w, 0.0)"); break;
    case 12: emit(o, "float4(255.0, 255.0, 255.0, 0.0)"); break;
    case 13: emit(o, "float4(128.0, 128.0, 128.0, 0.0)"); break;
    case 14: konstC(o, kcsel); break;
    default: emit(o, "float4(0.0, 0.0, 0.0, 0.0)"); break;
    }
}
static void alphaArg(Out* o, int arg, int kasel) {
    switch (arg) {
    case 0: emit(o, "rP.w"); break;
    case 1: emit(o, "r0.w"); break;
    case 2: emit(o, "r1.w"); break;
    case 3: emit(o, "r2.w"); break;
    case 4: emit(o, "tx.w"); break;
    case 5: emit(o, "rs.w"); break;
    case 6: konstA(o, kasel); break;
    default: emit(o, "0.0"); break;
    }
}
static const char* kBiasS[4] = { "0.0", "128.0", "-128.0", "0.0" };
static const char* kSL[4] = { "1.0", "2.0", "4.0", "1.0" };     // << scaleL
static const char* kSR[4] = { "1.0", "1.0", "1.0", "0.5" };     // >> scaleR, as a multiply before floor
#ifdef TEVGEN_BREAK_ROUND   /* host negative control: the add rounding constant off by one */
static const char* kRnd(int op, int scale) { return scale == 3 ? "0.0" : op == 1 ? "127.0" : "127.0"; }
#else
static const char* kRnd(int op, int scale) { return scale == 3 ? "0.0" : op == 1 ? "127.0" : "128.0"; }
#endif

// The #defines every program starts with (so it is self-contained):
//   W8(v)   the low 8 bits of an integer code (GX a/b/c inputs), for any sign
//   CPR(c)  c + (c >> 7)
static void prelude(Out* o) {
    emit(o, "#define W8(v) ((v) - 256.0 * floor((v) * (1.0/256.0)))\n");
    emit(o, "#define CPR(c) ((c) + floor((c) * (1.0/128.0)))\n");
}

// FOG (2026-10-07, docs/47 run 116): after the alpha compare, so a discarded fragment stays discarded and alpha
// is never fogged. vW = the vertex program's clip w (= eye depth for a perspective projection; perspective-
// correct interpolation of w itself returns w exactly). uFog = (start, 1/(end-start), 0, 0), uFogColor rgb 0..1.
// The contract is tev_ref_fog (tev_ref.c); the host test holds this text to it within one code.
#ifdef TEVGEN_BREAK_FOG   /* host negative control: the wrong exponent constant */
#define FOGK "4.0"
#else
#define FOGK "8.0"
#endif
static void fogHead(Out* o, int type) {   // shared by the exact and the fast (normalised) fog tail
    emit(o, "    { float fz = clamp((vW - uFog.x) * uFog.y, 0.0, 1.0);\n");
    switch (type) {
    case 2: emit(o, "      float fg = fz;\n"); break;
    case 4: emit(o, "      float fg = 1.0 - exp2(-" FOGK " * fz);\n"); break;
    case 5: emit(o, "      float fg = 1.0 - exp2(-" FOGK " * fz * fz);\n"); break;
    case 6: emit(o, "      float fg = exp2(-" FOGK " * (1.0 - fz));\n"); break;
    default: emit(o, "      float fg = exp2(-" FOGK " * (1.0 - fz) * (1.0 - fz));\n"); break;
    }
}
static void fogCode(Out* o, int type) {
    fogHead(o, type);
    emit(o, "      return float4(rP.x + (uFogColor.x * 255.0 - rP.x) * fg, rP.y + (uFogColor.y * 255.0 - rP.y) * fg,\n"
            "                    rP.z + (uFogColor.z * 255.0 - rP.z) * fg, rP.w) * (1.0/255.0); }\n");
}

// ---- COMPARE-MODE STAGES (2026-10-12): GXSetTevColorOp / GXSetTevAlphaOp op 8..15 --------------------------------------------
// dolsdk GXTev.c: op > 1 stores bias = 3 (compare) and scale field = (op >> 1) & 3, op bit = op & 1. Compare id = (shift << 1) | opbit:
//   8 R8_GT, 9 R8_EQ (red of a vs red of b, one verdict for all three channels), 10 GR16_GT, 11 GR16_EQ (g*256+r), 12 BGR24_GT,
//   13 BGR24_EQ (b*65536+g*256+r), 14 RGB8_GT, 15 RGB8_EQ (per channel; the alpha ops 14/15 = A8_GT/A8_EQ: aa vs ab).
// out = d + (a OP b ? c : 0). No bias, no scale (the hardware ignores both); the stage's clamp flag still applies; a, b, c are the
// low 8 bits of their source (c is NOT weighted c' = c + (c >> 7)). Reference: tev_ref.c (after Dolphin's software TEV).
// The 16/24-bit compares go channel by channel, most significant first: exact at any float precision.
// Alpha ops 8..13 are NOT implemented (what the hardware reads for R8/GR16/BGR24 on the alpha combiner is not verified here; the
// title uses 14/15): such a key is refused (tev_cmp_unsupported), never approximated. The FAST variant refuses every compare
// (TF_COMPARE): its operands are normalised floats whose rounding differs by source (sampler unorm vs uniform * (1/255)), so == is
// not reliable; the exact program carries it.
static int isCmpOp(int op) { return op >= 8; }
int tev_cmp_unsupported(const TevKey* k) {
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) {
        const TevKeyStage* st = &k->st[s];
        if (st->cop[0] > 15 || (st->cop[0] >= 2 && st->cop[0] < 8) || (st->cop[0] < 2 && (st->cop[1] & 3) == 3)) return 1;
        if (st->aop[0] > 15 || (st->aop[0] >= 2 && st->aop[0] < 14) || (st->aop[0] < 2 && (st->aop[1] & 3) == 3)) return 1;
    }
    return 0;
}
int tev_has_compare(const TevKey* k) {
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) if (isCmpOp(k->st[s].cop[0]) || isCmpOp(k->st[s].aop[0])) return 1;
    return 0;
}
static void cmpColor(Out* o, int op) {   // defines cv (float4) from ca, cb, cc, cd
    int shift = (op - 8) >> 1, eq = op & 1;
#ifdef TEVGEN_BREAK_CMP   /* host negative control: a compare emitted as the add it used to be */
    emit(o, "      float4 cv = floor(((cd + float4(0.0, 0.0, 0.0, 0.0)) * 1.0 + floor((ca * (float4(256.0, 256.0, 256.0, 256.0) - cc) + cb * cc + float4(128.0, 128.0, 128.0, 128.0)) * (1.0/256.0))) * 1.0);\n");
    (void) shift; (void) eq; return;
#endif
    if (shift == 3) {
        if (eq) emit(o, "      float4 cm = float4(1.0, 1.0, 1.0, 1.0) - abs(sign(ca - cb));\n");
        else    emit(o, "      float4 cm = float4(step(cb.x + 1.0, ca.x), step(cb.y + 1.0, ca.y), step(cb.z + 1.0, ca.z), 0.0);\n");
        emit(o, "      float4 cv = cd + cm * cc;\n");
        return;
    }
    emit(o, "      float ex = 1.0 - abs(sign(ca.x - cb.x)); float gx = step(cb.x + 1.0, ca.x);\n");
    if (shift >= 1) emit(o, "      float ey = 1.0 - abs(sign(ca.y - cb.y)); float gy = step(cb.y + 1.0, ca.y);\n");
    if (shift >= 2) emit(o, "      float ez = 1.0 - abs(sign(ca.z - cb.z)); float gz = step(cb.z + 1.0, ca.z);\n");
    if (shift == 0)      emit(o, "      float cm = %s;\n", eq ? "ex" : "gx");
    else if (shift == 1) emit(o, "      float cm = %s;\n", eq ? "ey * ex" : "gy + ey * gx");
    else                 emit(o, "      float cm = %s;\n", eq ? "ez * ey * ex" : "gz + ez * (gy + ey * gx)");
    emit(o, "      float4 cv = cd + float4(cm, cm, cm, cm) * cc;\n");
}
static void cmpAlpha(Out* o, int op) {   // defines av (float) from aa, ab, ac, ad; op is 14 (GT) or 15 (EQ)
#ifdef TEVGEN_BREAK_CMP
    emit(o, "      float av = floor((ad + floor((aa * (256.0 - ac) + ab * ac + 128.0) * (1.0/256.0))) * 1.0);\n");
    (void) op; return;
#endif
    if (op & 1) emit(o, "      float av = ad + (1.0 - abs(sign(aa - ab))) * ac;\n");
    else        emit(o, "      float av = ad + step(ab + 1.0, aa) * ac;\n");
}

// PERSPECTIVE texgen, fragment side: a slot in projMask arrives as float3 vT (s, t, q) and is divided here, per pixel: pj<slot> = (s, t) / q, with q = 1 where |q| < 1e-6
// (what the per-vertex path did). The rasteriser interpolates (s, t, q) perspective-correctly, so s/q is the exact projective coordinate at every pixel; GX does the
// same (the texgen's s, t, q are interpolated, the divide is per pixel -- GX programming manual, "Texture coordinate generation"; the near-plane clip keeps q > 0 inside a visible triangle).
static const char* vtn(const TevKey* k, int slot, char* buf) {
    if (k->projMask >> slot & 1) snprintf(buf, 12, "pj%d", slot); else snprintf(buf, 12, "vT%d", slot);
    return buf;
}
static void projPrefix(Out* o, const TevKey* k) {
#ifdef TEVGEN_BREAK_PERSP   /* host negative control: the per-pixel divide left out (the quotient is taken as given) */
    for (int t = 0; t < k->numTex && t < TEVK_TEX; ++t) if (k->projMask >> t & 1) emit(o, "    float2 pj%d = float2(vT%d.x, vT%d.y);\n", t, t, t);
    return;
#endif
    for (int t = 0; t < k->numTex && t < TEVK_TEX; ++t) if (k->projMask >> t & 1) {
        emit(o, "    float qd%d = vT%d.z + step(abs(vT%d.z), 0.000001);\n    float2 pj%d = float2(vT%d.x / qd%d, vT%d.y / qd%d);\n", t, t, t, t, t, t, t, t);
    }
}
// ---- INDIRECT TEXTURING (2026-10-12) --------------------------------------------------------------------------------------------
// Per TEV stage with an indirect setup (TevIndKey), in texels of the stage's texture (the coordinate registers are texel counts):
//   (ux, uy) = wrap(regular coordinate = vT[slot] * size)           wrap: ITW_0 forces the axis to 0; ITW_OFF passes it. No texture: (0, 0).
//   (s, t, u) = the indirect texel at vT[indslot] / 2^scale, 8-bit components (A, B, G) -- dolsdk GXTev.c / Dolphin Tev.cpp: ITF_8 takes the
//              texel's ALPHA, BLUE and GREEN as S, T, U -- minus 128 for each selected by the bias (ITB_S = 1, T = 2, U = 4)
//   offset = M * (s, t, u)            M = uIM<m>a / uIM<m>b rows (the effective matrix: element / 1024 * 2^exp, CPU side); matrix OFF: no offset
//   (ux, uy) += offset;  += the previous stage's final coordinate when add-previous is set;  the stage samples at (ux, uy) / size.
//   bump alpha = the selected component's raw 8-bit value & 0xF8; GX_ALPHA_BUMP gives it to the stage's rasterised colour (all channels),
//   GX_ALPHA_BUMPN gives bump + (bump >> 5) (so 0xF8 reaches 0xFF).
// The fast variant refuses every key with an indirect stage (TF_IND): the exact program carries it.
// DYNAMIC matrices (ITM_S0..2 / ITM_T0..2, GXSetTevIndBumpST: the env-map bump): no elements of their own. GXSetIndTexMtx writes ITM_S<n> / ITM_T<n> to the same registers as ITM_<n>
// (dolsdk2004 GXBump.c: id = mtx_id - 1 / - 5 / - 9), and the hardware uses only that register's EXPONENT: the offset is the stage's own REGULAR texture coordinate (s, t) -- before the wrap,
// in texels -- times the indirect sample's S component (ITM_S) or T component (ITM_T), after the bias:
//   offset = (s, t) * (ind / 256) * 2^exp          [ind in -128..127: the S8 sample read as a /256 fraction]
// SOURCE: the SDK defines the stage set (GXSetTevIndBumpST = S on tev_stage, T on tev_stage + 1 with add-previous, then ITM_OFF + add-previous) and the register aliasing; the "/256" and
// "pre-wrap coordinate" are from memory of Dolphin's software TEV (not on this machine) and are UNVERIFIED -- they are invisible unless the stage's texgen is non-zero (BINRM / TANGENT texgens
// are unsupported in the port: their coordinate is (0,0), so the offset is 0 whatever the factor).
static int mtxIdx(int code) { return code >= 7 ? code - 7 : code >= 4 ? code - 4 : code - 1; }   /* matrix register 0..2 of a matrix code 1..9 */
unsigned tev_ind_matrix_mask(const TevKey* k) {
    unsigned m = 0;
    if (!k->ik.on) return 0;
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) if (k->ik.st[s][0] && k->ik.st[s][3] >= 1 && k->ik.st[s][3] <= 9) m |= 1u << mtxIdx(k->ik.st[s][3]);
    return m;
}
unsigned tev_ind_ts_mask(const TevKey* k) {
    unsigned m = 0;
    if (!k->ik.on) return 0;
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) {
        const TevKeyStage* st = &k->st[s];
        if (st->texSlot == 0xff) continue;
        if (k->ik.st[s][0]) m |= 1u << st->texSlot;                                               // the indirect stage converts texels <-> normalised through its texture's size
        else if (s + 1 < k->numStages && k->ik.st[s + 1][0] && (k->ik.st[s + 1][5] & 1)) m |= 1u << st->texSlot;   // a direct stage that feeds an add-previous
    }
    return m;
}
static void indSig(Out* o, const TevKey* k) {   // the extra uniforms of the program
    const unsigned mm = tev_ind_matrix_mask(k), tm = tev_ind_ts_mask(k);
    for (int m = 0; m < 3; ++m) if (mm >> m & 1) emit(o, ", uniform float4 uIM%da, uniform float4 uIM%db", m, m);
    for (int t = 0; t < TEVK_TEX; ++t) if (tm >> t & 1) emit(o, ", uniform float4 uTS%d", t);
}
static int indAnyPrev(const TevKey* k) {
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) if (k->ik.st[s][0] && (k->ik.st[s][5] & 1)) return 1;
    return 0;
}
static void indStage(Out* o, const TevKey* k, int s) {
    const uint8_t* q = k->ik.st[s]; const TevKeyStage* st = &k->st[s];
    const int slot = st->texSlot, mtx = q[3] <= 9 ? q[3] : 0, alpha = q[5] >> 1 & 3, dyn = mtx >= 4;
    const int bumpUsed = (st->ras == 2 || st->ras == 3) && alpha;
    char vn[16], vi[16];
    if (slot != 0xff) emit(o, "    float ux%d = %s.x * uTS%d.x; float uy%d = %s.y * uTS%d.y;\n", s, vtn(k, slot, vn), slot, s, vn, slot);
    else              emit(o, "    float ux%d = 0.0; float uy%d = 0.0;\n", s, s);
    if (dyn) emit(o, "    float rcx%d = ux%d; float rcy%d = uy%d;\n", s, s, s, s);   /* the dynamic matrices multiply the coordinate BEFORE the wrap */
#ifndef TEVGEN_BREAK_INDWRAP   /* host negative control: the ITW_0 wrap ignored (the regular coordinate is kept) */
    if (q[4] & 1) emit(o, "    ux%d = 0.0;\n", s);
    if (q[4] & 2) emit(o, "    uy%d = 0.0;\n", s);
#endif
    if (bumpUsed) emit(o, "    float bmp%d = 0.0;\n", s);
    if (mtx || (bumpUsed && alpha)) {
        const int is = q[1] & 3, sl = k->ik.slot[is];
        char sx[24], sy[24];
        snprintf(sx, sizeof sx, "%.9f", 1.0 / (double)(1 << (k->ik.sc[is][0] & 15)));
        snprintf(sy, sizeof sy, "%.9f", 1.0 / (double)(1 << (k->ik.sc[is][1] & 15)));
        emit(o, "    { float4 it%d = tex2D(tex%d, float2(%s.x * %s, %s.y * %s));\n", s, sl, vtn(k, sl, vi), sx, vi, sy);
        static const char kStu[3][4] = { { 'w', 'z', 'y', 0 }, { 'x', 'y', 'z', 0 }, { 'w', 'x', 'y', 0 } };   /* the texel channel each of S, T, U is read from (TevIndKey.stu) */
        const char* sc3 = kStu[k->ik.stu % 3];
        emit(o, "      float ia%d = floor(it%d.%c * 255.0 + 0.5); float ib%d = floor(it%d.%c * 255.0 + 0.5); float ig%d = floor(it%d.%c * 255.0 + 0.5);\n", s, s, sc3[0], s, s, sc3[1], s, s, sc3[2]);
        if (bumpUsed && alpha) {
#ifdef TEVGEN_BREAK_INDALPHA   /* host negative control: the bump alpha taken from the wrong component */
            static const char kC[4] = { 'a', 'g', 'a', 'b' };
#else
            static const char kC[4] = { 'a', 'a', 'b', 'g' };
#endif
            emit(o, "      bmp%d = floor(i%c%d * (1.0/8.0)) * 8.0;\n", s, kC[alpha], s);
            if (st->ras == 3) emit(o, "      bmp%d = bmp%d + floor(bmp%d * (1.0/32.0));\n", s, s, s);
        }
        if (mtx) {
#ifndef TEVGEN_BREAK_INDBIAS   /* host negative control: the -128 bias omitted */
            if (q[2] & 1) emit(o, "      ia%d = ia%d - 128.0;\n", s, s);
            if (q[2] & 2) emit(o, "      ib%d = ib%d - 128.0;\n", s, s);
            if (q[2] & 4) emit(o, "      ig%d = ig%d - 128.0;\n", s, s);
#endif
            if (dyn) {
#ifdef TEVGEN_BREAK_INDST   /* host negative control: S and T swapped */
                const char c = mtx >= 7 ? 'a' : 'b';
#else
                const char c = mtx >= 7 ? 'b' : 'a';
#endif
                emit(o, "      float dm%d = i%c%d * (1.0/256.0) * uIM%da.w;\n", s, c, s, mtxIdx(mtx));
#ifdef TEVGEN_BREAK_INDDYNWRAP   /* host negative control: the dynamic matrix multiplies the WRAPPED coordinate */
                emit(o, "      ux%d = ux%d + ux%d * dm%d;\n", s, s, s, s);
                emit(o, "      uy%d = uy%d + uy%d * dm%d; }\n", s, s, s, s);
#else
                emit(o, "      ux%d = ux%d + rcx%d * dm%d;\n", s, s, s, s);
                emit(o, "      uy%d = uy%d + rcy%d * dm%d; }\n", s, s, s, s);
#endif
            } else {
                emit(o, "      ux%d = ux%d + dot(uIM%da, float4(ia%d, ib%d, ig%d, 0.0));\n", s, s, mtx - 1, s, s, s);
                emit(o, "      uy%d = uy%d + dot(uIM%db, float4(ia%d, ib%d, ig%d, 0.0)); }\n", s, s, mtx - 1, s, s, s);
            }
        } else emit(o, "    }\n");
    }
#ifndef TEVGEN_BREAK_INDPREV   /* host negative control: add-previous dropped */
    if (q[5] & 1) emit(o, "    ux%d = ux%d + pcx; uy%d = uy%d + pcy;\n", s, s, s, s);
#endif
    if (indAnyPrev(k)) emit(o, "    pcx = ux%d; pcy = uy%d;\n", s, s);
}

static void body(Out* o, const TevKey* k) {

    emit(o, "    float4 rP = uR0; float4 r0 = uR1; float4 r1 = uR2; float4 r2 = uR3;\n");
    emit(o, "    float4 tx = float4(0.0, 0.0, 0.0, 0.0); float4 rs = float4(0.0, 0.0, 0.0, 0.0);\n");
    projPrefix(o, k);
    if (k->ik.on && indAnyPrev(k)) emit(o, "    float pcx = 0.0; float pcy = 0.0;\n");
    for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) {
        const TevKeyStage* st = &k->st[s];
        emit(o, "    /* stage %d */\n", s);
        const int act = k->ik.on && k->ik.st[s][0];
        if (act) indStage(o, k, s);
        else if (k->ik.on && s + 1 < k->numStages && k->ik.st[s + 1][0] && (k->ik.st[s + 1][5] & 1)) {   // a direct stage in front of an add-previous: its regular coordinate is what is carried
            if (st->texSlot != 0xff) { char vp[16]; vtn(k, st->texSlot, vp); emit(o, "    pcx = %s.x * uTS%d.x; pcy = %s.y * uTS%d.y;\n", vp, st->texSlot, vp, st->texSlot); }
            else emit(o, "    pcx = 0.0; pcy = 0.0;\n");
        }
        if (st->texSlot != 0xff && !(act && (k->ik.st[s][4] & 4))) {   /* (GX_TEXMAP_DISABLE: the map only scales the coordinate) */
            char v[24]; snprintf(v, sizeof v, "tt%d", s);
            if (act) emit(o, "    { float4 %s = tex2D(tex%d, float2(ux%d * uTS%d.z, uy%d * uTS%d.w)); tx = floor(", v, st->texSlot, s, st->texSlot, s, st->texSlot);
            else     { char vp[16]; emit(o, "    { float4 %s = tex2D(tex%d, %s); tx = floor(", v, st->texSlot, vtn(k, st->texSlot, vp)); }
#ifdef TEVGEN_BREAK_SWAP   /* host negative control: the texture swap table ignored */
            swz(o, v, k->swap[0]); emit(o, " * 255.0 + float4(0.5, 0.5, 0.5, 0.5)); }\n");
#else
            swz(o, v, k->swap[st->texSwap & 3]); emit(o, " * 255.0 + float4(0.5, 0.5, 0.5, 0.5)); }\n");
#endif
        } else emit(o, "    tx = float4(0.0, 0.0, 0.0, 0.0);\n");
        if (st->ras == 0 || st->ras == 1) {
            emit(o, "    rs = floor("); swz(o, st->ras ? "vColor1" : "vColor0", k->swap[st->rasSwap & 3]);
            emit(o, " * 255.0 + float4(0.5, 0.5, 0.5, 0.5));\n");
        } else if ((st->ras == 2 || st->ras == 3) && act && (k->ik.st[s][5] >> 1 & 3)) emit(o, "    rs = float4(bmp%d, bmp%d, bmp%d, bmp%d);\n", s, s, s, s);
        else emit(o, "    rs = float4(0.0, 0.0, 0.0, 0.0);\n");
        // colour: a, b, c are 8-bit; d is the full S10 value
        emit(o, "    { float4 ca = W8("); colorArg(o, st->cin[0], st->kcsel);
        const int cmpC = isCmpOp(st->cop[0]), cmpA = isCmpOp(st->aop[0]);   // compare stages: c is the plain 8-bit value, not c' = c + (c >> 7)
        emit(o, "); float4 cb = W8(");     colorArg(o, st->cin[1], st->kcsel);
        emit(o, cmpC ? "); float4 cc = W8(" : "); float4 cc = CPR(W8("); colorArg(o, st->cin[2], st->kcsel);
        emit(o, cmpC ? ");\n      float4 cd = " : "));\n      float4 cd = ");  colorArg(o, st->cin[3], st->kcsel);
        emit(o, ";\n      float aa = W8(");  alphaArg(o, st->ain[0], st->kasel);
        emit(o, "); float ab = W8(");      alphaArg(o, st->ain[1], st->kasel);
        emit(o, cmpA ? "); float ac = W8(" : "); float ac = CPR(W8(");  alphaArg(o, st->ain[2], st->kasel);
        emit(o, cmpA ? ");\n      float ad = " : "));\n      float ad = ");   alphaArg(o, st->ain[3], st->kasel);
        emit(o, ";\n");
        int cs = st->cop[2] & 3, as = st->aop[2] & 3;
        const char* sgnC = st->cop[0] == 1 ? "-" : "+";
        const char* sgnA = st->aop[0] == 1 ? "-" : "+";
        if (cmpC) cmpColor(o, st->cop[0]);
        else {
        emit(o, "      float4 ct = floor(((ca * (float4(256.0, 256.0, 256.0, 256.0) - cc) + cb * cc) * %s + float4(%s, %s, %s, %s)) * (1.0/256.0));\n",
             kSL[cs], kRnd(st->cop[0], cs), kRnd(st->cop[0], cs), kRnd(st->cop[0], cs), kRnd(st->cop[0], cs));
        emit(o, "      float4 cv = floor(((cd + float4(%s, %s, %s, %s)) * %s %s ct) * %s);\n",
             kBiasS[st->cop[1] & 3], kBiasS[st->cop[1] & 3], kBiasS[st->cop[1] & 3], kBiasS[st->cop[1] & 3], kSL[cs], sgnC, kSR[cs]);
        }
        if (cmpA) cmpAlpha(o, st->aop[0]);
        else {
        emit(o, "      float at = floor(((aa * (256.0 - ac) + ab * ac) * %s + %s) * (1.0/256.0));\n", kSL[as], kRnd(st->aop[0], as));
        emit(o, "      float av = floor(((ad + %s) * %s %s at) * %s);\n", kBiasS[st->aop[1] & 3], kSL[as], sgnA, kSR[as]);
        }
        if (st->cop[3]) emit(o, "      cv = clamp(cv, 0.0, 255.0);\n");
        else            emit(o, "      cv = clamp(cv, -1024.0, 1023.0);\n");
        if (st->aop[3]) emit(o, "      av = clamp(av, 0.0, 255.0);\n");
        else            emit(o, "      av = clamp(av, -1024.0, 1023.0);\n");
        emit(o, "      %s = float4(cv.x, cv.y, cv.z, %s.w);\n", kReg[st->cop[4] & 3], kReg[st->cop[4] & 3]);
        emit(o, "      %s.w = av; }\n", kReg[st->aop[4] & 3]);
    }
    emit(o, "    rP = clamp(rP, 0.0, 255.0);\n");
    // alpha compare (GXSetAlphaCompare) on the 8-bit output alpha
    // the compare can be left out only when it passes for EVERY alpha: OR with an ALWAYS side, or
    // AND / XNOR of two ALWAYS -- NOT XOR, which fails there (the host test caught that: 296 trials)
    int both = k->acComp0 == 7 && k->acComp1 == 7;
    int always = ((k->acOp & 3) == 1 && (k->acComp0 == 7 || k->acComp1 == 7)) ||
                 (((k->acOp & 3) == 0 || (k->acOp & 3) == 3) && both);
    if (!always) {
        static const char* kCmp[8] = { "0.0", "step(a8 + 0.5, R)", "(1.0 - abs(sign(a8 - R)))", "step(a8, R)",
                                        "step(R + 0.5, a8)", "abs(sign(a8 - R))", "step(R, a8)", "1.0" };
        emit(o, "    { float a8 = rP.w; float p0; float p1;\n");
        emit(o, "      { float R = uAref.x; p0 = %s; }\n", kCmp[k->acComp0 & 7]);
        emit(o, "      { float R = uAref.y; p1 = %s; }\n", kCmp[k->acComp1 & 7]);
        switch (k->acOp & 3) {
        /* NOT `pass`: a Cg RESERVED word (CgFX technique/pass). Run 89-90: every program with an alpha
           compare declared `float pass`, and SceShaccCg took the process down inside glLinkProgram
           (keys 7425ce77, 2b62b6c7); the 14 programs without one compiled. host/tests/tev_reserved.c
           now scans every generated program for reserved words. */
#ifdef TEVGEN_BREAK_RESERVED
        case 0: emit(o, "      float pass = p0 * p1; float acOk = pass;\n"); break;
#else
        case 0: emit(o, "      float acOk = p0 * p1;\n"); break;
#endif
        case 1: emit(o, "      float acOk = clamp(p0 + p1, 0.0, 1.0);\n"); break;
        case 2: emit(o, "      float acOk = abs(p0 - p1);\n"); break;
        default: emit(o, "      float acOk = 1.0 - abs(p0 - p1);\n"); break;
        }
        emit(o, "      clip(acOk - 0.5); }\n");
    }
    if (k->fogType) fogCode(o, k->fogType);
    else emit(o, "    return rP * (1.0/255.0);\n");
}

static int hasCompare(const TevKey* k) { return tev_cmp_unsupported(k); }   // an op the generators cannot express (compare ops 8..15 now are expressed; see "COMPARE-MODE STAGES")

int tev_gen_prelude(char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    prelude(&o);
    return o.bad ? -1 : o.n;
}

int tev_gen_body(const TevKey* k, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    if (hasCompare(k)) return -2;
    body(&o, k);
    return o.bad ? -1 : o.n;
}

int tev_gen_fragment(const TevKey* k, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    if (hasCompare(k)) return -2;
    prelude(&o);
    /* colours travel as TEXCOORD varyings, as in the vita-renderer's working programs (gbuffer_shader.c) */
    emit(&o, "float4 main(float4 vColor0 : TEXCOORD0, float4 vColor1 : TEXCOORD1");
    for (int t = 0; t < k->numTex; ++t) emit(&o, k->projMask >> t & 1 ? ", float3 vT%d : TEXCOORD%d" : ", float2 vT%d : TEXCOORD%d", t, t + 2);
    if (k->fogType) emit(&o, ", float vW : TEXCOORD%d", 2 + k->numTex);
    for (int t = 0; t < k->numTex; ++t) emit(&o, ", uniform sampler2D tex%d", t);
    emit(&o, ",\n  uniform float4 uR0, uniform float4 uR1, uniform float4 uR2, uniform float4 uR3,\n"
             "  uniform float4 uK0, uniform float4 uK1, uniform float4 uK2, uniform float4 uK3, uniform float2 uAref%s",
             k->fogType ? ", uniform float4 uFog, uniform float4 uFogColor" : "");
    indSig(&o, k);
    emit(&o, ") : COLOR\n{\n");
    body(&o, k);
    emit(&o, "}\n");
    return o.bad ? -1 : o.n;
}

int tev_gen_vertex(int numTex, char* buf, int cap) { return tev_gen_vertex_ex(numTex, 0, buf, cap); }

int tev_gen_vertex_ex(int numTex, int fogType, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    emit(&o, "void main(float3 aPos : POSITION, float4 aColor0 : COLOR0, float4 aColor1 : COLOR1");
    for (int t = 0; t < numTex; ++t) emit(&o, ", float2 aT%d : TEXCOORD%d", t, t);
    /* the projection as four ROWS and dot products, never a float4x4: which way Cg and vitaGL's
       glUniformMatrix4fv read a matrix is two conventions to get right and both are silent when
       wrong (vita-renderer gbuffer_shader.c). rhi.h: row-major, clip = M * (x, y, z, 1). */
    emit(&o, ",\n  uniform float4 uM0, uniform float4 uM1, uniform float4 uM2, uniform float4 uM3,\n"
             "  out float4 vPos : POSITION, out float4 vColor0 : TEXCOORD0, out float4 vColor1 : TEXCOORD1");
    for (int t = 0; t < numTex; ++t) emit(&o, ", out float2 vT%d : TEXCOORD%d", t, t + 2);
    if (fogType) emit(&o, ", out float vW : TEXCOORD%d", 2 + numTex);
    emit(&o, ")\n{\n    float4 p = float4(aPos, 1.0);\n"
             "    vPos = float4(dot(uM0, p), dot(uM1, p), dot(uM2, p), dot(uM3, p));\n"
             "    vColor0 = aColor0;\n    vColor1 = aColor1;\n");
    for (int t = 0; t < numTex; ++t) emit(&o, "    vT%d = aT%d;\n", t, t);
    if (fogType) emit(&o, "    vW = dot(uM3, p);\n");   /* clip w, as vPos.w */
    emit(&o, "}\n");
    return o.bad ? -1 : o.n;
}

// ================================================================================================================
// GPU VERTEX PROGRAM (2026-10-08, docs/47 fast path stage 2). One BRANCHLESS Cg vertex program per VsKey that does
// what dlkCall does per DISTINCT vertex on the CPU: skinVertex (real-skin branch), pxChannelColour (+ pxLitRGBA),
// texgenVertex. The contract and the attribute / uniform / varying names are in gx_tev_key.h; the proof is
// host/tests/vs_shader_test.cpp, which compiles THIS body as C++ (tev_cg_compat.h) and holds it to the CPU path.
//
// EXACTNESS. Float expressions are written in the CPU functions' own operand order (no dot() in the lighting or the
// texgen: a different summation order is a different last bit, and the colour is compared code for code). Every
// value-dependent guard of the CPU code is arithmetic, never a branch:
//   d > 0            gt = 1 - step(d, 0);   inverse = 1 / (d + (1 - gt))      (d == 0: multiplies by 1, as the CPU leaves lx)
//   den != 0         dz = |sign(den)|;      attn = num / (den + (1 - dz)) * dz
//   l > 0 (normal)   same as d > 0, and the product is multiplied by gt so a zero-length vector stays 0
//   |q| > 1e-6       tz = 1 - step(|q|, 1e-6); divide by q * tz + (1 - tz)
//   ndl >= 0 (spec)  step(0, ndl)
// Literals carry an `f` (as the console-verified VSFAT probe does) so the C++ run is single precision like the CPU.
// The 8-bit rounding is per vertex, floor(m * lit + 0.5), output code / 255 (the fragment program multiplies back).
// A BLACK light is dropped by the packer instead of kept with colour 0 (the CPU skips it; 0 * inf would be NaN).
// Unsupported texgen source/function = (0,0) like texgenVertex; a TEXn the list does not carry reads (0,0,1,1).
// Refused (-2) rather than approximated: see tev_vs_check.
// Host negative controls: -DVSGEN_BREAK_{ROUND,SPOT,NRMMTX,POST,ALPHA,SLOT0,RESERVED,TMI,TMROW,NBT}.
// ================================================================================================================
static const char* const kVsWhy[VSW_N] = { "ok", "not real-skin", "gPxActive off", "light arm off", "uncacheable record", "!posOk vertex",
    "mixed have-normal", "matrix slot > 9", "more than 8 lights", "more than 8 input attributes", "more than TEVK_TEX texcoord slots", "inconsistent key",
    "mixed have-binormal/tangent", "texture-matrix index outside 30..57" };
const char* tev_vs_why_name(int why) { return why >= 0 && why < VSW_N ? kVsWhy[why] : "?"; }

unsigned tev_vs_tmi_groups(const VsKey* k) {
    unsigned g = 0;
    for (int s = 0; s < k->numTex && s < TEVK_TEX; ++s) if (k->tmiSlots >> s & 1) g |= 1u << (k->tmiCoord[s] >> 2 & 1);
    return g;
}
int tev_vs_check(const VsKey* k) {
    if (k->numTex > TEVK_TEX) return VSW_TOO_MANY_TEX;
    int nl = k->ch[0].nLights + k->ch[1].nLights;
    if (k->ch[0].nLights > VS_MAX_LIGHTS || k->ch[1].nLights > VS_MAX_LIGHTS || nl > VS_MAX_LIGHTS) return VSW_TOO_MANY_LIGHTS;
    int na = 1 + (k->hasNrm ? 1 : 0) + (k->hasClr0 ? 1 : 0) + (k->hasMtxIdx ? 1 : 0) + (k->hasBin ? 1 : 0) + (k->hasTan ? 1 : 0);
    for (int n = 0; n < 8; ++n) if (k->texMask >> n & 1) ++na;
    { const unsigned tg = tev_vs_tmi_groups(k); na += (int)(tg & 1) + (int)(tg >> 1 & 1); }
    if (k->hasSkin) { na += k->hasNrm ? 2 : 1; if (k->hasBin || k->hasTan) return VSW_BAD_KEY; }   /* R8: aSkP, and aSkN when a normal is read; the raw binormal / tangent are not skinned (NBT models never get a table) */
    if (na > VS_MAX_ATTRS) return VSW_TOO_MANY_ATTRS;
    for (int c = 0; c < 2; ++c) if (k->ch[c].diffFn > 2 || k->ch[c].attnFn > 2 || k->ch[c].matSrc > 1 || k->ch[c].ambSrc > 1) return VSW_BAD_KEY;
    if (k->shareAlpha && (!k->ch[0].enable || !k->ch[1].enable || k->ch[1].nLights)) return VSW_BAD_KEY;
    unsigned mask = 0; int bin = 0, tan = 0;
    if (k->tmiSlots >> (k->numTex < 8 ? k->numTex : 8)) return VSW_BAD_KEY;   /* a bit past the slots the key has */
    for (int s = 0; s < k->numTex; ++s) {
        const VsTexKey* g = &k->tex[s];
        if (g->kind > 1 || g->func > 1 || g->src > 5 || g->texN > 7) return VSW_BAD_KEY;
        if (g->kind && g->src == 2) mask |= 1u << g->texN;
        if (g->kind && g->src == 4) bin = 1;
        if (g->kind && g->src == 5) tan = 1;
        if (k->tmiSlots >> s & 1) { if (!g->kind || g->hasTM || k->tmiCoord[s] > 7) return VSW_BAD_KEY; }   /* the vertex's matrix replaces the uniform one: never both */
    }
    if (mask != k->texMask || bin != (k->hasBin ? 1 : 0) || tan != (k->hasTan ? 1 : 0)) return VSW_BAD_KEY;
    return VSW_OK;
}
uint32_t tev_vs_key_hash(const VsKey* k) {
    const unsigned char* p = (const unsigned char*)k; uint32_t h = 2166136261u;
    for (size_t i = 0, n = tev_vs_key_len(k); i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
unsigned tev_vs_proj_mask(const VsKey* k) {
    unsigned m = 0;
#ifdef VSGEN_BREAK_POST   /* the pre-2026-10-14 rule (see vsTexgen): a post matrix made the slot projective */
    for (int s = 0; s < k->numTex && s < TEVK_TEX; ++s) if (k->tex[s].kind && (k->tex[s].func == 0 || k->tex[s].hasPost)) m |= 1u << s;
#else   /* only a 3x4 (STQ) texgen is divided by q, post matrix or not (Aurora shader.cpp:1260) */
    for (int s = 0; s < k->numTex && s < TEVK_TEX; ++s) if (k->tex[s].kind && k->tex[s].func == 0) m |= 1u << s;
#endif
    return m;
}
static int vsUsesN(const VsKey* k) { return k->hasNrm && (k->ch[0].nLights + k->ch[1].nLights) > 0; }
static int vsPosN(const VsKey* k) {
#ifdef VSGEN_BREAK_SLOT0
    if (!k->hasMtxIdx) return 6;   /* the break reads slot 1, so the array must reach it */
#endif
    return k->hasMtxIdx ? 3 * VS_MAX_SLOTS : 3;
}
int tev_vs_uniform_vec4(const VsKey* k) {
    int n = 4 + vsPosN(k) + (vsUsesN(k) ? (k->hasMtxIdx ? 3 * VS_MAX_SLOTS : 3) : 0) + (k->numChans ? 2 : 0) + 5 * (k->ch[0].nLights + k->ch[1].nLights);
    for (int s = 0; s < k->numTex; ++s) if (k->tex[s].kind) n += (k->tex[s].hasTM ? 3 : 0) + (k->tex[s].hasPost ? 3 : 0);
    if (k->tmiSlots) n += 30;   /* uTMX: the 10 texture matrices */
    if (k->hasSkin) n += VS_SKIN_PAL_VEC4;   /* uBone: the bone palette + the scale row (R8) */
    return n;
}
static void vsSig(Out* o, int* n, int cpp, int kind /* 0 in, 1 uniform, 2 out */, const char* type, const char* name, const char* sem) {
    emit(o, *n ? ",\n  " : "  "); ++*n;
    if (cpp) { if (kind == 2) emit(o, "%s& %s", type, name); else emit(o, "%s %s", type, name); return; }
    if (kind == 1) emit(o, "uniform %s %s", type, name);
    else if (kind == 2) emit(o, "out %s %s : %s", type, name, sem);
    else emit(o, "%s %s : %s", type, name, sem);
}
static void vsParams(Out* o, const VsKey* k, int cpp) {
    int n = 0; char nm[32], sem[32];
    vsSig(o, &n, cpp, 0, "float3", "aPos", "POSITION");
    if (k->hasNrm) vsSig(o, &n, cpp, 0, "float3", "aNrm", "NORMAL");
    if (k->hasClr0) vsSig(o, &n, cpp, 0, "float4", "aColor0", "COLOR0");
    if (k->hasMtxIdx) vsSig(o, &n, cpp, 0, "float", "aMtx", "TEXCOORD0");
    for (int t = 0, rank = 0; t < 8; ++t) if (k->texMask >> t & 1) {
        snprintf(nm, sizeof nm, "aT%d", t); snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++);
        vsSig(o, &n, cpp, 0, "float2", nm, sem);
    }
    { int rank = 0; for (int t = 0; t < 8; ++t) rank += k->texMask >> t & 1;   /* the 2026-10-13 attributes follow the aT: the next TEXCOORD indices (and, in the shim, locations) */
      const unsigned tg = tev_vs_tmi_groups(k);
      if (k->hasBin) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float3", "aBin", sem); }
      if (k->hasTan) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float3", "aTan", sem); }
      if (tg & 1) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float4", "aTmA", sem); }
      if (tg & 2) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float4", "aTmB", sem); }
      if (k->hasSkin) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float4", "aSkP", sem);
                        if (k->hasNrm) { snprintf(sem, sizeof sem, "TEXCOORD%d", 1 + rank++); vsSig(o, &n, cpp, 0, "float4", "aSkN", sem); } } }   /* R8: after aTmB, the last attribute (the shim / GXM binder add it in the same place) */
    vsSig(o, &n, cpp, 1, "float4", "uM0", 0); vsSig(o, &n, cpp, 1, "float4", "uM1", 0);
    vsSig(o, &n, cpp, 1, "float4", "uM2", 0); vsSig(o, &n, cpp, 1, "float4", "uM3", 0);
    snprintf(nm, sizeof nm, "uPos[%d]", vsPosN(k)); vsSig(o, &n, cpp, 1, "float4", nm, 0);
    if (vsUsesN(k)) { snprintf(nm, sizeof nm, "uNrm[%d]", k->hasMtxIdx ? 3 * VS_MAX_SLOTS : 3); vsSig(o, &n, cpp, 1, "float4", nm, 0); }
    if (k->numChans) { vsSig(o, &n, cpp, 1, "float4", "uMat", 0); vsSig(o, &n, cpp, 1, "float4", "uAmb", 0); }
    for (int c = 0; c < 2; ++c) if (k->ch[c].nLights) {
        static const char* const f[5] = { "pos", "dir", "a", "k", "col" };
        for (int q = 0; q < 5; ++q) { snprintf(nm, sizeof nm, "u%cL%s[%d]", c ? 'A' : 'C', f[q], k->ch[c].nLights); vsSig(o, &n, cpp, 1, "float4", nm, 0); }
    }
    for (int s = 0; s < k->numTex; ++s) if (k->tex[s].kind) {
        if (k->tex[s].hasTM) { snprintf(nm, sizeof nm, "uTM%d[3]", s); vsSig(o, &n, cpp, 1, "float4", nm, 0); }
        if (k->tex[s].hasPost) { snprintf(nm, sizeof nm, "uPT%d[3]", s); vsSig(o, &n, cpp, 1, "float4", nm, 0); }
    }
    if (k->tmiSlots) vsSig(o, &n, cpp, 1, "float4", "uTMX[30]", 0);
    if (k->hasSkin) { snprintf(nm, sizeof nm, "uBone[%d]", VS_SKIN_PAL_VEC4); vsSig(o, &n, cpp, 1, "float4", nm, 0); }   /* R8: the palette + scale row, their own uniform */
    vsSig(o, &n, cpp, 2, "float4", "vPos", "POSITION");
    vsSig(o, &n, cpp, 2, "float4", "vColor0", "TEXCOORD0");
    vsSig(o, &n, cpp, 2, "float4", "vColor1", "TEXCOORD1");
    for (int s = 0; s < k->numTex; ++s) { snprintf(nm, sizeof nm, "vT%d", s); snprintf(sem, sizeof sem, "TEXCOORD%d", 2 + s); vsSig(o, &n, cpp, 2, k->projFrag && (tev_vs_proj_mask(k) >> s & 1) ? "float3" : "float2", nm, sem); }
    if (k->fog) { snprintf(sem, sizeof sem, "TEXCOORD%d", 2 + k->numTex); vsSig(o, &n, cpp, 2, "float", "vW", sem); }
}
// one channel's light loop: accumulators A<c>R/G/B (rgb) and/or A<c>W (alpha), exactly pxLitRGBA's arithmetic
static void vsLights(Out* o, const VsKey* k, int c, int rgb, int alpha) {
    const VsChanKey* ch = &k->ch[c];
    if (rgb) emit(o, "    float A%dR = 0.0f; float A%dG = 0.0f; float A%dB = 0.0f;\n", c, c, c);
    if (alpha) emit(o, "    float A%dW = 0.0f;\n", c);
    for (int i = 0; i < ch->nLights; ++i) {
        char t[16]; snprintf(t, sizeof t, "%d_%d", c, i); const char P = c ? 'A' : 'C';
        emit(o, "    /* %s light %d */\n", c ? "alpha" : "colour", i);
        emit(o, "    float4 lp%s = u%cLpos[%d]; float4 ld%s = u%cLdir[%d]; float4 la%s = u%cLa[%d]; float4 lk%s = u%cLk[%d]; float4 lc%s = u%cLcol[%d];\n",
             t, P, i, t, P, i, t, P, i, t, P, i, t, P, i);
        emit(o, "    float lx%s = lp%s.x - vx; float ly%s = lp%s.y - vy; float lz%s = lp%s.z - vz;\n", t, t, t, t, t, t);
        emit(o, "    float d2%s = lx%s * lx%s + ly%s * ly%s + lz%s * lz%s;\n", t, t, t, t, t, t, t);
        emit(o, "    float dd%s = sqrt(d2%s);\n", t, t);
        emit(o, "    float dg%s = 1.0f - step(dd%s, 0.0f);\n", t, t);
        emit(o, "    float di%s = 1.0f / (dd%s + (1.0f - dg%s));\n", t, t, t);
        emit(o, "    lx%s = lx%s * di%s; ly%s = ly%s * di%s; lz%s = lz%s * di%s;\n", t, t, t, t, t, t, t, t, t);
        emit(o, "    float ndl%s = n0 * lx%s + n1 * ly%s + n2 * lz%s;\n", t, t, t, t);
        if (ch->attnFn == 1) {            /* GX_AF_SPOT */
            emit(o, "    float cs%s = max(lx%s * ld%s.x + ly%s * ld%s.y + lz%s * ld%s.z, 0.0f);\n", t, t, t, t, t, t, t);
            emit(o, "    float nu%s = max(la%s.x + la%s.y * cs%s + la%s.z * cs%s * cs%s, 0.0f);\n", t, t, t, t, t, t, t);
#ifdef VSGEN_BREAK_SPOT   /* host negative control: the spot denominator takes d where it needs d^2 */
            emit(o, "    float de%s = lk%s.x + lk%s.y * dd%s + lk%s.z * dd%s;\n", t, t, t, t, t, t);
#else
            emit(o, "    float de%s = lk%s.x + lk%s.y * dd%s + lk%s.z * d2%s;\n", t, t, t, t, t, t);
#endif
        } else if (ch->attnFn == 0) {     /* GX_AF_SPEC */
            emit(o, "    float cs%s = max(step(0.0f, ndl%s) * (n0 * ld%s.x + n1 * ld%s.y + n2 * ld%s.z), 0.0f);\n", t, t, t, t, t);
            emit(o, "    float nu%s = max(la%s.x + la%s.y * cs%s + la%s.z * cs%s * cs%s, 0.0f);\n", t, t, t, t, t, t, t);
            emit(o, "    float de%s = lk%s.x + lk%s.y * cs%s + lk%s.z * cs%s * cs%s;\n", t, t, t, t, t, t, t);
        }
        if (ch->attnFn != 2) {
            emit(o, "    float dz%s = abs(sign(de%s));\n", t, t);
            emit(o, "    float at%s = nu%s / (de%s + (1.0f - dz%s)) * dz%s;\n", t, t, t, t, t);
        } else emit(o, "    float at%s = 1.0f;\n", t);
        if (ch->diffFn == 0) emit(o, "    float f%s = at%s * (1.0f / 255.0f);\n", t, t);
        else if (ch->diffFn == 1) emit(o, "    float f%s = at%s * ndl%s * (1.0f / 255.0f);\n", t, t, t);
        else emit(o, "    float f%s = at%s * max(ndl%s, 0.0f) * (1.0f / 255.0f);\n", t, t, t);
        if (rgb) emit(o, "    A%dR = A%dR + f%s * lc%s.x; A%dG = A%dG + f%s * lc%s.y; A%dB = A%dB + f%s * lc%s.z;\n", c, c, t, t, c, c, t, t, c, c, t, t);
        if (alpha) emit(o, "    A%dW = A%dW + f%s * lc%s.w;\n", c, c, t, t);
    }
}
/* R8: the position / normal the rest of the program reads: the raw attribute, or (hasSkin) the skinned, re-quantised local of vsSkin */
static const char* vsPn(const VsKey* k, int c) { static const char* const a[3] = { "aPos.x", "aPos.y", "aPos.z" }, * const b[3] = { "spx", "spy", "spz" }; return k->hasSkin ? b[c] : a[c]; }
static const char* vsNn(const VsKey* k, int c) { static const char* const a[3] = { "aNrm.x", "aNrm.y", "aNrm.z" }, * const b[3] = { "snx", "sny", "snz" }; return k->hasSkin ? b[c] : a[c]; }
static void vsTexgen(Out* o, const VsKey* k, int sl) {
    const VsTexKey* g = &k->tex[sl];
    if (!g->kind) { emit(o, "    vT%d = float2(0.0f, 0.0f);   /* unsupported texgen: (0,0) like texgenVertex */\n", sl); return; }
    char v0[24], v1[24], v2[24];
    switch (g->src) {
    case 0: snprintf(v0, sizeof v0, "%s", vsPn(k, 0)); snprintf(v1, sizeof v1, "%s", vsPn(k, 1)); snprintf(v2, sizeof v2, "%s", vsPn(k, 2)); break;
    case 1: if (k->hasNrm) { snprintf(v0, sizeof v0, "%s", vsNn(k, 0)); snprintf(v1, sizeof v1, "%s", vsNn(k, 1)); snprintf(v2, sizeof v2, "%s", vsNn(k, 2)); }
            else { snprintf(v0, sizeof v0, "0.0f"); snprintf(v1, sizeof v1, "0.0f"); snprintf(v2, sizeof v2, "0.0f"); } break;
    case 2: snprintf(v0, sizeof v0, "aT%d.x", g->texN); snprintf(v1, sizeof v1, "aT%d.y", g->texN); snprintf(v2, sizeof v2, "1.0f"); break;
#ifdef VSGEN_BREAK_NBT   /* host negative control: the binormal's components are read in reverse order */
    case 4: snprintf(v0, sizeof v0, "aBin.z"); snprintf(v1, sizeof v1, "aBin.y"); snprintf(v2, sizeof v2, "aBin.x"); break;
#else
    case 4: snprintf(v0, sizeof v0, "aBin.x"); snprintf(v1, sizeof v1, "aBin.y"); snprintf(v2, sizeof v2, "aBin.z"); break;
#endif
    case 5: snprintf(v0, sizeof v0, "aTan.x"); snprintf(v1, sizeof v1, "aTan.y"); snprintf(v2, sizeof v2, "aTan.z"); break;
    default: snprintf(v0, sizeof v0, "0.0f"); snprintf(v1, sizeof v1, "0.0f"); snprintf(v2, sizeof v2, "1.0f"); break;
    }
    const int tmi = k->tmiSlots >> sl & 1;
    emit(o, "    /* texcoord %d: %s, source %s%s%s%s */\n", sl, g->func ? "MTX2x4" : "MTX3x4", g->src == 0 ? "pos" : g->src == 1 ? "nrm" : g->src == 2 ? "TEXn" : g->src == 4 ? "binormal" : g->src == 5 ? "tangent" : "TEXn(absent)",
         g->hasTM ? ", texmtx" : tmi ? ", texmtx from the vertex (TEXnMTXIDX)" : "", g->hasPost ? ", post" : "", g->hasPost && g->norm ? ", normalise" : "");
    if (g->hasTM || tmi) {   /* M[i][0]*v0 + M[i][1]*v1 + M[i][2]*v2 + M[i][3]*1 */
        char m0[24], m1[24], m2[24];
        if (tmi) {   /* the vertex's row r (30..57) -> uTMX[r - 30 + 0..2]: the same row triple the CPU path takes from gTexMtx[(r - 30) / 3] */
            const int c = k->tmiCoord[sl]; const char* grp = c < 4 ? "aTmA" : "aTmB";
#ifdef VSGEN_BREAK_TMI   /* host negative control: the program reads the NEXT component of the TEXnMTXIDX group (another texcoord's index) */
            emit(o, "    float tk%d = floor(%s.%c + 0.5f) - 30.0f;\n    int tj%d_0 = (int)tk%d;\n    int tj%d_1 = (int)(tk%d + 1.0f);\n    int tj%d_2 = (int)(tk%d + 2.0f);\n",
                 sl, grp, "xyzw"[(c + 1) & 3], sl, sl, sl, sl, sl, sl);
#elif defined(VSGEN_BREAK_TMROW)   /* host negative control: the second row of the palette matrix is the first row again */
            emit(o, "    float tk%d = floor(%s.%c + 0.5f) - 30.0f;\n    int tj%d_0 = (int)tk%d;\n    int tj%d_1 = (int)(tk%d);\n    int tj%d_2 = (int)(tk%d + 2.0f);\n",
                 sl, grp, "xyzw"[c & 3], sl, sl, sl, sl, sl, sl);
#else
            emit(o, "    float tk%d = floor(%s.%c + 0.5f) - 30.0f;\n    int tj%d_0 = (int)tk%d;\n    int tj%d_1 = (int)(tk%d + 1.0f);\n    int tj%d_2 = (int)(tk%d + 2.0f);\n",
                 sl, grp, "xyzw"[c & 3], sl, sl, sl, sl, sl, sl);
#endif
            emit(o, "    float4 tx%d_0 = uTMX[tj%d_0]; float4 tx%d_1 = uTMX[tj%d_1]; float4 tx%d_2 = uTMX[tj%d_2];\n", sl, sl, sl, sl, sl, sl);
            snprintf(m0, sizeof m0, "tx%d_0", sl); snprintf(m1, sizeof m1, "tx%d_1", sl); snprintf(m2, sizeof m2, "tx%d_2", sl);
        } else { snprintf(m0, sizeof m0, "uTM%d[0]", sl); snprintf(m1, sizeof m1, "uTM%d[1]", sl); snprintf(m2, sizeof m2, "uTM%d[2]", sl); }
        emit(o, "    float ts%d = %s.x * %s + %s.y * %s + %s.z * %s + %s.w;\n", sl, m0, v0, m0, v1, m0, v2, m0);
        emit(o, "    float tt%d = %s.x * %s + %s.y * %s + %s.z * %s + %s.w;\n", sl, m1, v0, m1, v1, m1, v2, m1);
        if (g->func == 0) emit(o, "    float tq%d = %s.x * %s + %s.y * %s + %s.z * %s + %s.w;\n", sl, m2, v0, m2, v1, m2, v2, m2);
        else emit(o, "    float tq%d = 1.0f;\n", sl);
    } else {
        emit(o, "    float ts%d = %s;\n    float tt%d = %s;\n", sl, v0, sl, v1);
        if (g->func == 0) emit(o, "    float tq%d = %s;\n", sl, v2); else emit(o, "    float tq%d = 1.0f;\n", sl);
    }
    int proj = g->func == 0;
    if (g->hasPost) {
        if (g->norm) {
            emit(o, "    float tl%d = sqrt(ts%d * ts%d + tt%d * tt%d + tq%d * tq%d);\n", sl, sl, sl, sl, sl, sl, sl);
            emit(o, "    float tw%d = tl%d + step(tl%d, 0.0f);\n", sl, sl, sl);   /* l > 0: divide by l; else by 1 */
            emit(o, "    ts%d = ts%d / tw%d; tt%d = tt%d / tw%d; tq%d = tq%d / tw%d;\n", sl, sl, sl, sl, sl, sl, sl, sl, sl);
        }
        emit(o, "    float ps%d = uPT%d[0].x * ts%d + uPT%d[0].y * tt%d + uPT%d[0].z * tq%d + uPT%d[0].w;\n", sl, sl, sl, sl, sl, sl, sl, sl);
        emit(o, "    float pt%d = uPT%d[1].x * ts%d + uPT%d[1].y * tt%d + uPT%d[1].z * tq%d + uPT%d[1].w;\n", sl, sl, sl, sl, sl, sl, sl, sl);
        emit(o, "    float pq%d = uPT%d[2].x * ts%d + uPT%d[2].y * tt%d + uPT%d[2].z * tq%d + uPT%d[2].w;\n", sl, sl, sl, sl, sl, sl, sl, sl);
        emit(o, "    ts%d = ps%d; tt%d = pt%d; tq%d = pq%d;\n", sl, sl, sl, sl, sl, sl);
#ifdef VSGEN_BREAK_POST   /* host negative control: the pre-2026-10-14 bug, a divide after ANY post matrix (only 3x4 divides: Aurora
                              * shader.cpp:1260, Dolphin texMtxInfo.projection; see gx_draw.c texgenVertex) */
        proj = 1;
#endif
    }
    if (proj && k->projFrag) {   /* PERSPECTIVE-correct: (s, t, q) go out un-divided; the fragment program divides after the rasteriser's perspective-correct interpolation (docs/47 "persp") */
        emit(o, "    vT%d = float3(ts%d, tt%d, tq%d);\n", sl, sl, sl, sl);
    } else if (proj) {
        emit(o, "    float tz%d = 1.0f - step(abs(tq%d), 0.000001f);\n", sl, sl);
        emit(o, "    float tr%d = tq%d * tz%d + (1.0f - tz%d);\n", sl, sl, sl, sl);
        emit(o, "    vT%d = float2(ts%d / tr%d, tt%d / tr%d);\n", sl, sl, sl, sl, sl);
    } else emit(o, "    vT%d = float2(ts%d, tt%d);\n", sl, sl, sl);
}
/* R8 GPU skinning (host/engine/R8_GPUSKIN.md design 4; the C model is hal/gx_skin.c gx_skin_ref_pos / gx_skin_ref_nrm, op for op).  One table entry: its two palette bone rows,
 * its weights / 128 and its "skinned" flag, decoded from the packed pair (a * 256 + b, w0 * 256 + w1) of aSk (component ca, cw).  The palette index of an UNSKINNED entry (a = 255) is forced to
 * row 0 (a valid row), and its result is discarded by the select below -- arithmetic, never a branch, like the rest of this program. */
static void vsSkinEntry(Out* o, const char* pre, const char* at) {
    emit(o, "    float %sa = %s.x; float %sb = %s.y;\n", pre, at, pre, at);   /* the U8x4 attribute holds the integers themselves */
    emit(o, "    float %sw0 = %s.z; float %sw1 = %s.w;\n", pre, at, pre, at);
    emit(o, "    float %son = step(%sa, 254.5f);\n", pre, pre);
    emit(o, "    float %sab = %sa * %son * 3.0f; float %sbb = %sb * %son * 3.0f;\n", pre, pre, pre, pre, pre, pre);
    emit(o, "    int %sia0 = (int)%sab; int %sia1 = (int)(%sab + 1.0f); int %sia2 = (int)(%sab + 2.0f);\n", pre, pre, pre, pre, pre);
    emit(o, "    int %sib0 = (int)%sbb; int %sib1 = (int)(%sbb + 1.0f); int %sib2 = (int)(%sbb + 2.0f);\n", pre, pre, pre, pre, pre);
    emit(o, "    float4 %sA0 = uBone[%sia0]; float4 %sA1 = uBone[%sia1]; float4 %sA2 = uBone[%sia2];\n", pre, pre, pre, pre, pre, pre);
    emit(o, "    float4 %sB0 = uBone[%sib0]; float4 %sB1 = uBone[%sib1]; float4 %sB2 = uBone[%sib2];\n", pre, pre, pre, pre, pre, pre);
    emit(o, "    float %sk0 = %sw0 * (1.0f / 128.0f); float %sk1 = %sw1 * (1.0f / 128.0f);\n", pre, pre, pre, pre);
}
static void vsSkin(Out* o, const VsKey* k) {
    static const char c3[3] = { 'x', 'y', 'z' };
    emit(o, "    /* R8 GPU skin: p = (MA * p) * w0/128 + (MB * p) * w1/128, then the CPU kernel's integer: clamp to s16, truncate toward zero, at 2^posQ (uBone[%d].x / .y) */\n", VS_SKIN_PAL_VEC4 - 1);
    emit(o, "    float4 sks = uBone[%d];\n", VS_SKIN_PAL_VEC4 - 1);
    vsSkinEntry(o, "skp", "aSkP");
    for (int c = 0; c < 3; ++c)
        emit(o, "    float skpo%c = (skpA%d.x * aPos.x + skpA%d.y * aPos.y + skpA%d.z * aPos.z + skpA%d.w) * skpk0 + (skpB%d.x * aPos.x + skpB%d.y * aPos.y + skpB%d.z * aPos.z + skpB%d.w) * skpk1;\n",
             c3[c], c, c, c, c, c, c, c, c);
    for (int c = 0; c < 3; ++c) {
        emit(o, "    float skpq%c = clamp(skpo%c * sks.x, -32768.0f, 32767.0f);\n", c3[c], c3[c]);
        emit(o, "    float sp%c = skpon * (sign(skpq%c) * floor(abs(skpq%c)) * sks.y) + (1.0f - skpon) * aPos.%c;\n", c3[c], c3[c], c3[c], c3[c]);
    }
    if (!k->hasNrm) return;
    emit(o, "    /* the normal: its own table entry, no translation, s8 at 2^nrmQ (uBone[%d].z / .w) */\n", VS_SKIN_PAL_VEC4 - 1);
    vsSkinEntry(o, "skn", "aSkN");
    for (int c = 0; c < 3; ++c)
        emit(o, "    float skno%c = (sknA%d.x * aNrm.x + sknA%d.y * aNrm.y + sknA%d.z * aNrm.z) * sknk0 + (sknB%d.x * aNrm.x + sknB%d.y * aNrm.y + sknB%d.z * aNrm.z) * sknk1;\n",
             c3[c], c, c, c, c, c, c);
    for (int c = 0; c < 3; ++c) {
        emit(o, "    float sknq%c = clamp(skno%c * sks.z, -128.0f, 127.0f);\n", c3[c], c3[c]);
        emit(o, "    float sn%c = sknon * (sign(sknq%c) * floor(abs(sknq%c)) * sks.w) + (1.0f - sknon) * aNrm.%c;\n", c3[c], c3[c], c3[c], c3[c]);
    }
}
static void vsBody(Out* o, const VsKey* k) {
    static const char kc[4] = { 'x', 'y', 'z', 'w' };
    const int nl0 = k->ch[0].nLights, nl1 = k->ch[1].nLights;
    int shareEff = k->shareAlpha;
#ifdef VSGEN_BREAK_ALPHA   /* host negative control: alpha takes the colour channel's lighting even when it is NOT shared */
    if (k->ch[0].enable && k->ch[1].enable) shareEff = 1;
#endif
#ifdef VSGEN_BREAK_RESERVED   /* host negative control for the reserved-word scan: `pass` is a CgFX keyword (docs/47 runs 89-90) */
    emit(o, "    float pass = 0.0f;\n");
#endif
    if (k->hasSkin) vsSkin(o, k);   /* R8: spx.. / snx.. replace aPos / aNrm everywhere below (vsPn / vsNn) */
    // ---- position: skinVertex, real-skin branch --------------------------------------------------------------
    int r0 = 0;
#ifdef VSGEN_BREAK_SLOT0   /* host negative control: a non-indexed list reads matrix slot 1 instead of slot 0 */
    r0 = 3;
#endif
    if (k->hasMtxIdx) {
        emit(o, "    float mi = floor(aMtx + 0.5f);\n    float bi = mi * 3.0f;\n    int i0 = (int)bi;\n    int i1 = (int)(bi + 1.0f);\n    int i2 = (int)(bi + 2.0f);\n");
        emit(o, "    float4 pm0 = uPos[i0]; float4 pm1 = uPos[i1]; float4 pm2 = uPos[i2];\n");
    } else emit(o, "    float4 pm0 = uPos[%d]; float4 pm1 = uPos[%d]; float4 pm2 = uPos[%d];\n", r0, r0 + 1, r0 + 2);
    emit(o, "    float vx = pm0.x * %s + pm0.y * %s + pm0.z * %s + pm0.w;\n", vsPn(k, 0), vsPn(k, 1), vsPn(k, 2));
    emit(o, "    float vy = pm1.x * %s + pm1.y * %s + pm1.z * %s + pm1.w;\n", vsPn(k, 0), vsPn(k, 1), vsPn(k, 2));
    emit(o, "    float vz = pm2.x * %s + pm2.y * %s + pm2.z * %s + pm2.w;\n", vsPn(k, 0), vsPn(k, 1), vsPn(k, 2));
    // ---- the view-space normal, normalised with the zero-length guard ----------------------------------------
    if (nl0 + nl1 > 0) {
        if (vsUsesN(k)) {
#ifdef VSGEN_BREAK_NRMMTX   /* host negative control: the normal goes through the POSITION matrix rows */
            emit(o, "    float4 nm0 = pm0; float4 nm1 = pm1; float4 nm2 = pm2;\n");
#else
            if (k->hasMtxIdx) emit(o, "    float4 nm0 = uNrm[i0]; float4 nm1 = uNrm[i1]; float4 nm2 = uNrm[i2];\n");
            else emit(o, "    float4 nm0 = uNrm[0]; float4 nm1 = uNrm[1]; float4 nm2 = uNrm[2];\n");
#endif
            emit(o, "    float nx = nm0.x * %s + nm0.y * %s + nm0.z * %s;\n", vsNn(k, 0), vsNn(k, 1), vsNn(k, 2));
            emit(o, "    float ny = nm1.x * %s + nm1.y * %s + nm1.z * %s;\n", vsNn(k, 0), vsNn(k, 1), vsNn(k, 2));
            emit(o, "    float nz = nm2.x * %s + nm2.y * %s + nm2.z * %s;\n", vsNn(k, 0), vsNn(k, 1), vsNn(k, 2));
            emit(o, "    float nlen = sqrt(nx * nx + ny * ny + nz * nz);\n");
            emit(o, "    float ngt = 1.0f - step(nlen, 0.0f);\n");
            emit(o, "    float ninv = 1.0f / (nlen + (1.0f - ngt));\n");
            emit(o, "    float n0 = nx * ninv * ngt; float n1 = ny * ninv * ngt; float n2 = nz * ninv * ngt;\n");
        } else emit(o, "    float n0 = 0.0f; float n1 = 0.0f; float n2 = 0.0f;   /* a lit channel on a list without normals: N = 0 */\n");
    }
    // ---- lit colour: pxChannelColour --------------------------------------------------------------------------
    if (k->numChans) {
        if (k->ch[0].enable && nl0) vsLights(o, k, 0, 1, shareEff);
        if (k->ch[1].enable && nl1) vsLights(o, k, 1, 0, 1);
        for (int q = 0; q < 4; ++q) {
            const VsChanKey* ch = &k->ch[q < 3 ? 0 : 1];
            char m[48], a[48], acc[16];
            if (ch->matSrc) snprintf(m, sizeof m, "floor(aColor0.%c * 255.0f + 0.5f)", kc[q]); else snprintf(m, sizeof m, "uMat.%c", kc[q]);
            if (!ch->enable) { emit(o, "    float oc%d = (%s) / 255.0f;\n", q, m); continue; }
            if (ch->ambSrc) snprintf(a, sizeof a, "floor(aColor0.%c * 255.0f + 0.5f)", kc[q]); else snprintf(a, sizeof a, "uAmb.%c", kc[q]);
            if (q < 3) { if (nl0) snprintf(acc, sizeof acc, "A0%c", "RGB"[q]); else snprintf(acc, sizeof acc, "0.0f"); }
            else if (shareEff) { if (nl0) snprintf(acc, sizeof acc, "A0W"); else snprintf(acc, sizeof acc, "0.0f"); }
            else { if (nl1) snprintf(acc, sizeof acc, "A1W"); else snprintf(acc, sizeof acc, "0.0f"); }
            emit(o, "    float lt%d = clamp((%s) / 255.0f + %s, 0.0f, 1.0f);\n", q, a, acc);
#ifdef VSGEN_BREAK_ROUND   /* host negative control: the per-vertex 8-bit rounding is dropped */
            emit(o, "    float oc%d = ((%s) * lt%d) / 255.0f;\n", q, m, q);
#else
            emit(o, "    float oc%d = floor((%s) * lt%d + 0.5f) / 255.0f;\n", q, m, q);
#endif
        }
        emit(o, "    vColor0 = float4(oc0, oc1, oc2, oc3);\n");
    } else emit(o, "    vColor0 = float4(1.0f, 1.0f, 1.0f, 1.0f);\n");
    emit(o, "    vColor1 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n");
    // ---- texcoords: texgenVertex --------------------------------------------------------------------------------
    for (int s = 0; s < k->numTex; ++s) vsTexgen(o, k, s);
    // ---- clip position (the projection with gStfxGxZToGL already folded into uM2) and fog ----------------------
    emit(o, "    float4 vp = float4(vx, vy, vz, 1.0f);\n");
    emit(o, "    vPos = float4(dot(uM0, vp), dot(uM1, vp), dot(uM2, vp), dot(uM3, vp));\n");
    if (k->fog) emit(o, "    vW = dot(uM3, vp);\n");
}
int tev_gen_vertex_gx_prelude(char* buf, int cap) { if (cap > 0) buf[0] = 0; return 0; }
int tev_gen_vertex_gx_params(const VsKey* k, int cpp, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    if (tev_vs_check(k)) return -2;
    vsParams(&o, k, cpp);
    return o.bad ? -1 : o.n;
}
int tev_gen_vertex_gx_body(const VsKey* k, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    if (tev_vs_check(k)) return -2;
    vsBody(&o, k);
    return o.bad ? -1 : o.n;
}
int tev_gen_vertex_gx(const VsKey* k, char* buf, int cap) {
    Out o = { buf, 0, cap, 0 };
    if (cap > 0) buf[0] = 0;
    if (tev_vs_check(k)) return -2;
    emit(&o, "void main(\n");
    vsParams(&o, k, 0);
    emit(&o, ")\n{\n");
    vsBody(&o, k);
    emit(&o, "}\n");
    return o.bad ? -1 : o.n;
}

// ================================================================================================================
// FAST VARIANT (2026-10-08, docs/47 "fast TEV"). The exact generator above carries GX integer codes in fp32 and
// reproduces every wrap, rounding, shift and clamp per stage and per channel: 0 codes, and heavy (one 6-stage,
// 6-sample full-screen filter is ~12 ms a frame at 640x368). The FAST variant works in NORMALISED floats (code / 255;
// registers may leave 0..1 and are signed) and per stage does the combiner formula directly:
//     result = clamp( (d + bias +- lerp(a, b, c)) * scale )         lerp = a + (b - a) * c
// no floor, no 8-bit wrap, no +128, one clamp at most. The channels are PACKED into one float4 (alpha in .w) whenever the
// stage's colour and alpha ops agree, with the common shapes (zero a / b / c / d, no bias, scale 1) specialised away.
//
// WHEN IT IS SAFE. The float formula differs from GX in two ways, and the generator proves the key is on the safe side of
// both, else REFUSES (-2) and the device uses the exact program:
//  (1) THE 8-BIT WRAP. GX masks a, b, c to their low 8 bits. A source outside 0..255 therefore wraps (-1 reads 255). The
//      sources tex / ras / konst / constants are 0..255. A register is, as an a/b/c input, safe if
//        - it still holds its INITIAL value (a uniform of unknown range): the fast program reads a once-wrapped copy
//          (wP, w0, w1, w2 = the 8-bit wrap of uR*, exact for an integer), so any initial value is fine;
//        - or the stage that last wrote it clamped (exact value in 0..255, as in GX);
//        - or the stage wrote it with clamp OFF but interval analysis (below) proves the value, widened by the worst-case
//          error, stays inside 0..255, so GX would not have wrapped it either.
//      Otherwise (a clamp-off register that may leave 0..255, read as a/b/c by a later stage): REFUSED, reason "wrap".
//      Read as d it is fine: d is the full S10 value, no wrap, and the fast variant clamps at -1024/1023 like GX.
//  (2) ROUNDING. GX rounds the lerp (+128 / +127 then >> 8), shifts right with a floor, and weights by c' = c + (c >> 7)
//      (c = 128 weighs 129/256, the float uses 128/255); the fast variant has none of that. Each stage's difference from GX is
//      bounded in codes:  e = gain * (e_d + max(e_a, e_b) + e_c + [c not constant 0/255] * 0.498) + rho
//      with gain = 1, 2, 4, 0.5 for the scale, rho = the rounding GX adds (0.5 per rounded lerp; 1.0 for the >>1 scale; 0
//      where the lerp is exact: no lerp, c = 0 or c = 255). The bound is propagated stage to stage through the registers.
//      If the worst case on PREV exceeds TEVFAST_MAXERR (2.45: after rounding the output to a code, <= 2 codes) the key is
//      REFUSED, reason "error". The bound is conservative on purpose (an observed difference is far below it).
//  Compare-mode ops (bias 3) are refused as the exact generator does.
// Uniform and varying names, the samplers and the signature are those of the exact generator, so the same vertex programs
// and uniform uploads serve both. FOG is the exact generator's own text (fogHead) in normalised units.
// Host negative controls: -DTEVFAST_BREAK_{LERP,CLAMP,SAFE}.
// ================================================================================================================
#ifndef TEVFAST_MAXERR
#define TEVFAST_MAXERR 2.45
#endif
enum { TF_OK = 0, TF_COMPARE, TF_WRAP, TF_ERR, TF_BAD, TF_IND, TF_N };
static const char* const kFastWhy[TF_N] = { "ok", "compare-mode op", "wrap: a clamp-off register that may leave 0..255 is read as a/b/c",
                                            "error: worst-case bound over 2 codes", "bad key", "indirect texturing: exact program only" };
const char* tev_fast_why_name(int why) { return why >= 0 && why < TF_N ? kFastWhy[why] : "?"; }

typedef struct { double lo, hi, err; int clamped, init; } FGrp;     // one register's rgb or alpha: value range / worst error (codes)
typedef struct { char e[120], s[40]; int plain, zero, exact, isConst; double lo, hi, err; } FArg;   // one combiner input
typedef struct { int useR[4], needW[4], needK[4]; } FFlags;
static const char* const kWReg[4] = { "wP", "w0", "w1", "w2" };
static const double kFGain[4] = { 1.0, 2.0, 4.0, 0.5 };
static const int kBiasI[4] = { 0, 128, -128, 0 };

static void fZero(FArg* a, int alpha) {
    memset(a, 0, sizeof *a); a->zero = a->exact = a->isConst = 1;
    if (alpha) snprintf(a->e, sizeof a->e, "0.0"); else { snprintf(a->e, sizeof a->e, "float4(0.0, 0.0, 0.0, 0.0)"); snprintf(a->s, sizeof a->s, "0.0"); }
}
static void fConst(FArg* a, double code, int alpha) {
    memset(a, 0, sizeof *a); a->lo = a->hi = code; a->exact = code == 255.0; a->isConst = 1;
    if (alpha) snprintf(a->e, sizeof a->e, "%.6f", code / 255.0);
    else { snprintf(a->s, sizeof a->s, "%.6f", code / 255.0); snprintf(a->e, sizeof a->e, "float4(%s, %s, %s, 0.0)", a->s, a->s, a->s); }
}
static void fPlain(FArg* a, const char* id) { memset(a, 0, sizeof *a); a->plain = 1; a->hi = 255.0; snprintf(a->e, sizeof a->e, "%s", id); }
static void fScalar(FArg* a, const char* sc, int alpha, int bcast) {   // tx.w etc.: alpha, or broadcast into rgb
    memset(a, 0, sizeof *a); a->hi = 255.0;
    if (alpha) { snprintf(a->e, sizeof a->e, "%s", sc); return; }
    snprintf(a->s, sizeof a->s, "%s", sc); (void)bcast;
    snprintf(a->e, sizeof a->e, "float4(%s, %s, %s, 0.0)", sc, sc, sc);
}
// a register as an input. role 0: d (signed, no wrap); role 1: a/b/c (8-bit wrapped in GX). Returns 0 or TF_WRAP.
static int fReg(FArg* a, FGrp g[4][2], FFlags* f, int reg, int grp, int role, int bcast, int alpha) {
    FGrp* q = &g[reg][grp]; const char* nm = kReg[reg]; double lo = q->lo, hi = q->hi, err = q->err; int why = 0, useW = 0;
    if (role == 1) {
        if (q->init) { nm = kWReg[reg]; f->needW[reg] = 1; lo = 0; hi = 255; err = 0; useW = 1; }
        else if (!(q->clamped || (q->lo - q->err >= 0.0 && q->hi + q->err <= 255.0))) why = TF_WRAP;
    }
    if (!useW) f->useR[reg] = 1;
    char sc[24];
    if (alpha) { snprintf(sc, sizeof sc, "%s.w", nm); fScalar(a, sc, 1, 0); }
    else if (grp == 1) { snprintf(sc, sizeof sc, "%s.w", nm); fScalar(a, sc, 0, 1); }
    else fPlain(a, nm);
    a->lo = lo; a->hi = hi; a->err = err;
    return why;
}
static int fColArg(FArg* a, FGrp g[4][2], FFlags* f, const TevKeyStage* st, int arg, int role, int txZero, int rsZero) {
    switch (arg) {
    case 0: case 2: case 4: case 6: return fReg(a, g, f, arg >> 1, 0, role, 0, 0);
    case 1: case 3: case 5: case 7: return fReg(a, g, f, arg >> 1, 1, role, 1, 0);
    case 8: if (txZero) fZero(a, 0); else fPlain(a, "tx"); return 0;
    case 9: if (txZero) fZero(a, 0); else fScalar(a, "tx.w", 0, 1); return 0;
    case 10: if (rsZero) fZero(a, 0); else fPlain(a, "rs"); return 0;
    case 11: if (rsZero) fZero(a, 0); else fScalar(a, "rs.w", 0, 1); return 0;
    case 12: fConst(a, 255.0, 0); return 0;
    case 13: fConst(a, 128.0, 0); return 0;
    case 14: {
        int sel = st->kcsel; char nm[16];
        if (sel < 8) { fConst(a, kFrac[sel], 0); return 0; }
        if (sel >= 12 && sel <= 15) { snprintf(nm, sizeof nm, "kn%d", sel - 12); fPlain(a, nm); f->needK[sel - 12] = 1; return 0; }
        if (sel >= 16 && sel <= 31) { snprintf(nm, sizeof nm, "kn%d.%c", (sel - 16) & 3, kCh[(sel - 16) >> 2]); fScalar(a, nm, 0, 1); f->needK[(sel - 16) & 3] = 1; return 0; }
        fZero(a, 0); return 0; }
    default: fZero(a, 0); return 0;
    }
}
static int fAlphaArg(FArg* a, FGrp g[4][2], FFlags* f, const TevKeyStage* st, int arg, int role, int txZero, int rsZero) {
    switch (arg) {
    case 0: case 1: case 2: case 3: return fReg(a, g, f, arg, 1, role, 0, 1);
    case 4: if (txZero) fZero(a, 1); else fScalar(a, "tx.w", 1, 0); return 0;
    case 5: if (rsZero) fZero(a, 1); else fScalar(a, "rs.w", 1, 0); return 0;
    case 6: {
        int sel = st->kasel; char nm[16];
        if (sel < 8) { fConst(a, kFrac[sel], 1); return 0; }
        if (sel >= 16 && sel <= 31) { snprintf(nm, sizeof nm, "kn%d.%c", (sel - 16) & 3, kCh[(sel - 16) >> 2]); fScalar(a, nm, 1, 0); f->needK[(sel - 16) & 3] = 1; return 0; }
        fZero(a, 1); return 0; }
    default: fZero(a, 1); return 0;
    }
}
static double fMax(double a, double b) { return a > b ? a : b; }
static double fMin(double a, double b) { return a < b ? a : b; }
// one channel group's value range and worst-case error after a stage; *need = whether the stage's clamp can change anything
static int fExactW(const FArg* c, int wmode) { return c->isConst || (wmode == 2 && c->err == 0.0); }
static void fCalc(const FArg* a, const FArg* b, const FArg* c, const FArg* d, int sub, int bias, int sc, int clampOn, int wmode, FGrp* r, int* need) {
    double dlo = d->zero ? 0 : d->lo, dhi = d->zero ? 0 : d->hi, derr = d->zero ? 0 : d->err;
    int hasL = 1, lexact = 1; double Llo = 0, Lhi = 0, eL = 0;
    if (c->zero) { hasL = !a->zero; if (hasL) { Llo = a->lo; Lhi = a->hi; eL = a->err; } }
    else if (a->zero && b->zero) hasL = 0;
    else {
        Llo = fMin(a->lo, b->lo); Lhi = fMax(a->hi, b->hi); eL = fMax(a->err, b->err);
        if (!c->exact) {   // the weight error (c's own error, + c' = c + (c >> 7) unless the weight is made exact) times the lerp's span
            // GX's weight c' = c + (c >> 7) is a STEP of the integer c: the exact-weight form (floor) is only safe where c is an integer
            // the fast program holds exactly (err == 0); a c with error would straddle the step and be a whole code of weight out
            double dw = c->err + (fExactW(c, wmode) ? 0.0 : 0.498);
            eL += dw * (Lhi - Llo) / 255.0; lexact = 0;
        }
    }
    double g = kFGain[sc & 3], k = kBiasI[bias & 3], lo, hi;
    if (sub) { lo = dlo + k - Lhi; hi = dhi + k - Llo; } else { lo = dlo + k + Llo; hi = dhi + k + Lhi; }
    lo *= g; hi *= g;
    double rho = (sc & 3) == 3 ? (lexact ? 0.5 : 1.0) : (lexact ? 0.0 : 0.5);
    r->err = g * (derr + eL) + rho; r->init = 0; r->clamped = clampOn;
    double cl = clampOn ? 0.0 : -1024.0, ch = clampOn ? 255.0 : 1023.0;
    *need = !(lo >= cl && hi <= ch);
    r->lo = fMin(fMax(lo, cl), ch); r->hi = fMax(fMin(hi, ch), cl);
}
static void fSub(char* out, size_t n, const char* lit, int alpha) {   // constant literal as a scalar or a float4
    if (alpha) snprintf(out, n, "%s", lit); else snprintf(out, n, "float4(%s, %s, %s, %s)", lit, lit, lit, lit);
}
// the expression for one stage's result (alpha: scalar; else float4 -- or the packed float4 with alpha in .w)
static void fStr(char* out, size_t n, int alpha, const FArg* a, const FArg* b, const FArg* c, const FArg* d, int sub, int bias, int sc, int needClamp, int clampOn, int wmode) {
#ifdef TEVFAST_BREAK_LERP   /* host negative control: a and b swapped in the lerp */
    { const FArg* t = a; a = b; b = t; }
#endif
    char L[400], P[900], K[96], one[96], cw[300];
    fSub(one, sizeof one, "1.0", alpha);
    // the lerp weight: c as it is, or GX's c' / 256 = (c * 255 + (c >= 128 codes)) / 256 (exact; constants fold at compile time)
    if (!c->zero && !c->exact && fExactW(c, wmode)) {
        char h[96]; fSub(h, sizeof h, "0.5", alpha);
        snprintf(cw, sizeof cw, "(%s * 0.996094 + floor(%s * 1.99219) * 0.003906)", c->e, c->e); (void)h;
    } else snprintf(cw, sizeof cw, "%s", c->e);
    int hasL = 1;
    if (c->zero) { if (a->zero) hasL = 0; else snprintf(L, sizeof L, "%s", a->e); }
    else if (a->zero && b->zero) hasL = 0;
    else if (a->zero) snprintf(L, sizeof L, "%s * %s", b->e, cw);
    else if (b->zero) snprintf(L, sizeof L, "%s * (%s - %s)", a->e, one, cw);
    else snprintf(L, sizeof L, "%s + (%s - %s) * %s", a->e, b->e, a->e, cw);
    if (hasL && strchr(L, ' ')) { char t[400]; snprintf(t, sizeof t, "(%s)", L); strcpy(L, t); }
    int has = 0, neg = 0; P[0] = 0;
    if (!d->zero) { snprintf(P, sizeof P, "%s", d->e); has = 1; }
    if (bias == 1 || bias == 2) {
        fSub(K, sizeof K, bias == 1 ? "0.501961" : "-0.501961", alpha);
        if (!has) snprintf(P, sizeof P, "%s", K);
        else if (bias == 1) { char t[900]; snprintf(t, sizeof t, "%s + %s", P, K); strcpy(P, t); }
        else { fSub(K, sizeof K, "0.501961", alpha); char t[900]; snprintf(t, sizeof t, "%s - %s", P, K); strcpy(P, t); }
        has = 1;
    }
    if (hasL) {
        if (has) { char t[900]; snprintf(t, sizeof t, "%s %s %s", P, sub ? "-" : "+", L); strcpy(P, t); }
        else { snprintf(P, sizeof P, "%s", L); neg = sub; }
        has = 1;
    }
    if (!has) fSub(P, sizeof P, "0.0", alpha);
    double gg = kFGain[sc & 3] * (neg ? -1.0 : 1.0);
    char X[1000];
    if (gg != 1.0) snprintf(X, sizeof X, strchr(P, ' ') ? "(%s) * %.1f" : "%s * %.1f", P, gg); else snprintf(X, sizeof X, "%s", P);
#ifdef TEVFAST_BREAK_CLAMP   /* host negative control: the per-stage clamp dropped */
    needClamp = 0;
#endif
    if (needClamp) snprintf(out, n, "clamp(%s, %s)", X, clampOn ? "0.0, 1.0" : "-4.015686, 4.011765");
    else snprintf(out, n, "%s", X);
}
// the input a and the alpha input as ONE float4 (colour in xyz, alpha in w)
static void fPack(FArg* o, const FArg* c, const FArg* a) {
    memset(o, 0, sizeof *o); o->zero = c->zero && a->zero; o->exact = c->exact && a->exact; o->isConst = c->isConst && a->isConst; o->err = fMax(c->err, a->err);
    char id[120];
    if (o->zero) { snprintf(o->e, sizeof o->e, "float4(0.0, 0.0, 0.0, 0.0)"); return; }
    snprintf(id, sizeof id, "%s.w", c->e);
    if (c->plain && !strcmp(a->e, id)) { snprintf(o->e, sizeof o->e, "%s", c->e); o->plain = 1; return; }
    if (c->s[0]) snprintf(o->e, sizeof o->e, "float4(%s, %s, %s, %s)", c->s, c->s, c->s, a->e);
    else snprintf(o->e, sizeof o->e, "float4(%s.x, %s.y, %s.z, %s)", c->e, c->e, c->e, a->e);   /* c->plain */
}
static int hasCompareFast(const TevKey* k) { return hasCompare(k) || tev_has_compare(k); }   // the fast variant refuses every compare stage (see "COMPARE-MODE STAGES")
static const char* const kCmpFast[8] = { "0.0", "step(a8 + 0.5, R)", "(1.0 - abs(sign(a8 - R)))", "step(a8, R)",
                                         "step(R + 0.5, a8)", "abs(sign(a8 - R))", "step(R, a8)", "1.0" };

// pass 1 (final = 0) only analyses and fills fl; pass 2 (final = 1) emits with fl known. Returns 0 or a TF_ reason.
static int fastBody(Out* o, const TevKey* k, FFlags* fl, int final, int wmode) {
    if (k->numStages < 1 || k->numStages > TEVK_STAGES) return TF_BAD;
    if (k->ik.on) return TF_IND;
    if (hasCompareFast(k)) return TF_COMPARE;
    int why = 0;
    FGrp g[4][2];
    for (int r = 0; r < 4; ++r) for (int q = 0; q < 2; ++q) { g[r][q].lo = -1024; g[r][q].hi = 1023; g[r][q].err = 0; g[r][q].clamped = 0; g[r][q].init = 1; }
    FFlags own; if (!final) memset(fl, 0, sizeof *fl);
    (void)own;
    // pre-scan, so the header (pass 2) is first: the flags come from pass 1, so only emit the header when final
    if (final) {
        projPrefix(o, k);
        for (int r = 0; r < 4; ++r) if (fl->useR[r]) emit(o, "    float4 %s = uR%d * (1.0/255.0);\n", kReg[r], r);
        for (int r = 0; r < 4; ++r) if (fl->needW[r])
            emit(o, "    float4 %s = (uR%d - 256.0 * floor(uR%d * (1.0/256.0))) * (1.0/255.0);\n", kWReg[r], r, r);
        for (int q = 0; q < 4; ++q) if (fl->needK[q]) emit(o, "    float4 kn%d = uK%d * (1.0/255.0);\n", q, q);
    }
#ifdef TEVFAST_BREAK_RESERVED   /* host negative control for the reserved-word scan of the fast programs */
    if (final) emit(o, "    float pass = 0.0;\n");
#endif
    for (int s = 0; s < k->numStages; ++s) {
        const TevKeyStage* st = &k->st[s];
        int usesTx = 0, usesRs = 0;
        for (int i = 0; i < 4; ++i) {
            if (st->cin[i] == 8 || st->cin[i] == 9) usesTx = 1;
            if (st->cin[i] == 10 || st->cin[i] == 11) usesRs = 1;
            if (st->ain[i] == 4) usesTx = 1;
            if (st->ain[i] == 5) usesRs = 1;
        }
        int txZero = st->texSlot == 0xff, rsZero = !(st->ras == 0 || st->ras == 1);
        if (final) emit(o, "    /* stage %d */\n    {\n", s);
        if (final && usesTx && !txZero) {
            const uint8_t* sw = k->swap[st->texSwap & 3];
            if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3) { char vp[16]; emit(o, "      float4 tx = tex2D(tex%d, %s);\n", st->texSlot, vtn(k, st->texSlot, vp)); }
            else { char vp[16]; emit(o, "      float4 tt = tex2D(tex%d, %s);\n      float4 tx = ", st->texSlot, vtn(k, st->texSlot, vp)); swz(o, "tt", sw); emit(o, ";\n"); }
        }
        if (final && usesRs && !rsZero) {
            const uint8_t* sw = k->swap[st->rasSwap & 3];
            if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3) emit(o, "      float4 rs = %s;\n", st->ras ? "vColor1" : "vColor0");
            else { emit(o, "      float4 rs = "); swz(o, st->ras ? "vColor1" : "vColor0", sw); emit(o, ";\n"); }
        }
        FArg ca[4], aa[4], cd, ad;
        int w;
        for (int i = 0; i < 3; ++i) { w = fColArg(&ca[i], g, fl, st, st->cin[i], 1, txZero, rsZero); if (w && !why) why = w;
                                      w = fAlphaArg(&aa[i], g, fl, st, st->ain[i], 1, txZero, rsZero); if (w && !why) why = w; }
        fColArg(&cd, g, fl, st, st->cin[3], 0, txZero, rsZero); fAlphaArg(&ad, g, fl, st, st->ain[3], 0, txZero, rsZero);
        int csub = st->cop[0] == 1, asub = st->aop[0] == 1, cbias = st->cop[1] & 3, abias = st->aop[1] & 3, csc = st->cop[2] & 3, asc = st->aop[2] & 3;
        FGrp rc, ra; int needC, needA;
        fCalc(&ca[0], &ca[1], &ca[2], &cd, csub, cbias, csc, st->cop[3] != 0, wmode, &rc, &needC);
        fCalc(&aa[0], &aa[1], &aa[2], &ad, asub, abias, asc, st->aop[3] != 0, wmode, &ra, &needA);
        int cdst = st->cop[4] & 3, adst = st->aop[4] & 3;
        if (final) {
            int pack = csub == asub && cbias == abias && csc == asc && (st->cop[3] != 0) == (st->aop[3] != 0);
            if (pack) {
                FArg m[4]; char X[1400];
                for (int i = 0; i < 3; ++i) fPack(&m[i], &ca[i], &aa[i]);
                fPack(&m[3], &cd, &ad);
                fStr(X, sizeof X, 0, &m[0], &m[1], &m[2], &m[3], csub, cbias, csc, needC || needA, st->cop[3] != 0, wmode);
                emit(o, "      float4 sv = %s;\n", X);
                if (cdst == adst) emit(o, "      %s = sv;\n", kReg[cdst]);
                else { emit(o, "      %s = float4(sv.x, sv.y, sv.z, %s.w);\n", kReg[cdst], kReg[cdst]); emit(o, "      %s.w = sv.w;\n", kReg[adst]); }
            } else {
                char X[1400], Y[1400];
                fStr(X, sizeof X, 0, &ca[0], &ca[1], &ca[2], &cd, csub, cbias, csc, needC, st->cop[3] != 0, wmode);
                fStr(Y, sizeof Y, 1, &aa[0], &aa[1], &aa[2], &ad, asub, abias, asc, needA, st->aop[3] != 0, wmode);
                emit(o, "      float4 cv = %s;\n      float av = %s;\n", X, Y);
                emit(o, "      %s = float4(cv.x, cv.y, cv.z, %s.w);\n", kReg[cdst], kReg[cdst]);
                emit(o, "      %s.w = av;\n", kReg[adst]);
            }
            emit(o, "    }\n");
        }
        fl->useR[cdst] = 1; fl->useR[adst] = 1;
        g[cdst][0] = rc; g[adst][1] = ra;
    }
    fl->useR[0] = 1;
    FGrp* pc = &g[0][0]; FGrp* pa = &g[0][1];
    double e = fMax(pc->init ? 0 : pc->err, pa->init ? 0 : pa->err);
    if (!why && e >= TEVFAST_MAXERR) why = TF_ERR;
    int needClamp = pc->init || pa->init || pc->lo < 0 || pc->hi > 255 || pa->lo < 0 || pa->hi > 255;
    if (final) {
        if (needClamp) emit(o, "    rP = clamp(rP, 0.0, 1.0);\n");
        int both = k->acComp0 == 7 && k->acComp1 == 7;
        int always = ((k->acOp & 3) == 1 && (k->acComp0 == 7 || k->acComp1 == 7)) || (((k->acOp & 3) == 0 || (k->acOp & 3) == 3) && both);
        if (!always) {
            emit(o, "    { float a8 = floor(rP.w * 255.0 + 0.5); float p0; float p1;\n");
            emit(o, "      { float R = uAref.x; p0 = %s; }\n", kCmpFast[k->acComp0 & 7]);
            emit(o, "      { float R = uAref.y; p1 = %s; }\n", kCmpFast[k->acComp1 & 7]);
            switch (k->acOp & 3) {
            case 0: emit(o, "      float acOk = p0 * p1;\n"); break;
            case 1: emit(o, "      float acOk = clamp(p0 + p1, 0.0, 1.0);\n"); break;
            case 2: emit(o, "      float acOk = abs(p0 - p1);\n"); break;
            default: emit(o, "      float acOk = 1.0 - abs(p0 - p1);\n"); break;
            }
            emit(o, "      clip(acOk - 0.5); }\n");
        }
        if (k->fogType) {
            fogHead(o, k->fogType);
            emit(o, "      return float4(rP.x + (uFogColor.x - rP.x) * fg, rP.y + (uFogColor.y - rP.y) * fg,\n"
                    "                    rP.z + (uFogColor.z - rP.z) * fg, rP.w); }\n");
        } else emit(o, "    return rP;\n");
    }
#ifdef TEVFAST_BREAK_SAFE   /* host negative control: the safety rule accepts everything it can emit */
    why = 0;
#endif
    return why;
}
// the cheapest mode that is safe: 1 = lerp weight c as is; 2 = GX's exact weight c'/256 (a few more ops). Returns 0 (refused,
// *why = the reason of the better attempt) or the mode.
static int fastPick(const TevKey* k, int* why) {
    FFlags fl; char d[2]; Out o = { d, 0, 0, 0 };
    int w1 = fastBody(&o, k, &fl, 0, 1);
    if (!w1) { *why = 0; return 1; }
    if (w1 != TF_ERR) { *why = w1; return 0; }
    int w2 = fastBody(&o, k, &fl, 0, 2);
    if (!w2) { *why = 0; return 2; }
    *why = w2; return 0;
}
int tev_fast_check(const TevKey* k) { int why; fastPick(k, &why); return why; }
int tev_fast_safe(const TevKey* k) { return tev_fast_check(k) == 0; }
static int fastGen(const TevKey* k, char* buf, int cap, int withSig) {
    Out o = { buf, 0, cap, 0 }; FFlags fl; int why;
    if (cap > 0) buf[0] = 0;
    int mode = fastPick(k, &why);
    if (!mode) return -2;
    fastBody(&o, k, &fl, 0, mode);
    o.n = 0; o.bad = 0;
    if (withSig) {
        emit(&o, "float4 main(float4 vColor0 : TEXCOORD0, float4 vColor1 : TEXCOORD1");
        for (int t = 0; t < k->numTex; ++t) emit(&o, k->projMask >> t & 1 ? ", float3 vT%d : TEXCOORD%d" : ", float2 vT%d : TEXCOORD%d", t, t + 2);
        if (k->fogType) emit(&o, ", float vW : TEXCOORD%d", 2 + k->numTex);
        for (int t = 0; t < k->numTex; ++t) emit(&o, ", uniform sampler2D tex%d", t);
        emit(&o, ",\n  uniform float4 uR0, uniform float4 uR1, uniform float4 uR2, uniform float4 uR3,\n"
                 "  uniform float4 uK0, uniform float4 uK1, uniform float4 uK2, uniform float4 uK3, uniform float2 uAref%s) : COLOR\n{\n",
             k->fogType ? ", uniform float4 uFog, uniform float4 uFogColor" : "");
    }
    fastBody(&o, k, &fl, 1, mode);
    if (withSig) emit(&o, "}\n");
    return o.bad ? -1 : o.n;
}
int tev_gen_body_fast(const TevKey* k, char* buf, int cap) { return fastGen(k, buf, cap, 0); }
int tev_gen_fragment_fast(const TevKey* k, char* buf, int cap) { return fastGen(k, buf, cap, 1); }

// ============================================================================================================
// KEY CANONICALISATION (2026-10-12, docs/47 "Run 233" follow-up). tev_key_canon maps a TevKey to the SHORTEST key whose
// generated fragment program is BYTE-IDENTICAL, so equal programs get equal keys and the device keeps ONE GL program for them.
// Run 233 built 1,091 pair programs over 829 TEV keys but compiled only 125 fragment sources (+64 vertex): the raw key is a
// memcmp of the whole struct, and gx_tev_build copies GX state that the generator never reads -- GXSetTevKColorSel /
// GXSetTevKAlphaSel selectors left over from an earlier draw on a stage that reads no konst (the census corpus holds
// pairs of keys that differ only there: "... 0 28" vs "... 12 28"), swap-table rows no stage points at, a texture swap index on
// a stage with no texture, an alpha compare that cannot reject (ALWAYS/ALWAYS with any op, OR with an ALWAYS side), argument
// codes past the last valid one (all read as zero), op/bias/scale fields carrying bits above the ones the generator masks.
// Every rule below is the generator's own read of the field, mirrored; host/tests/tev_canon_test.c holds each one to the
// generator (byte-identical source over random keys and over single-field perturbations), and its -DTEVCANON_BREAK_* arms put a
// WRONG rule in and must fail. fast = 1 canonicalises for tev_gen_fragment_fast (it ignores more: the texture / raster row
// when no argument reads tx / rs); the result is only valid for the generator it was made for.
// ============================================================================================================
static int canonUsesTx(const TevKeyStage* s) {
    for (int i = 0; i < 4; ++i) if (s->cin[i] == 8 || s->cin[i] == 9 || s->ain[i] == 4) return 1;
    return 0;
}
static int canonUsesRs(const TevKeyStage* s) {
    for (int i = 0; i < 4; ++i) if (s->cin[i] == 10 || s->cin[i] == 11 || s->ain[i] == 5) return 1;
    return 0;
}
void tev_key_canon(const TevKey* in, TevKey* out, int fast) {
    *out = *in;
    int n = in->numStages; if (n > TEVK_STAGES) n = TEVK_STAGES;
    if (n < 0) n = 0;
    for (int s = n; s < TEVK_STAGES; ++s) memset(&out->st[s], 0, sizeof out->st[s]);   // past the count: never read
    // alpha compare: dropped from the program when it cannot reject (the generators' own `always`)
    {
        int both = in->acComp0 == 7 && in->acComp1 == 7;
        int always = ((in->acOp & 3) == 1 && (in->acComp0 == 7 || in->acComp1 == 7)) ||
                     (((in->acOp & 3) == 0 || (in->acOp & 3) == 3) && both);
        if (always) { out->acComp0 = 7; out->acComp1 = 7; out->acOp = 0; }
        else { out->acComp0 = in->acComp0 & 7; out->acComp1 = in->acComp1 & 7; out->acOp = in->acOp & 3; }
    }
    if (in->fogType && in->fogType != 2 && in->fogType != 4 && in->fogType != 5 && in->fogType != 6) out->fogType = 7;   // fogHead's default arm
    int txRef[TEVK_STAGES], rsRef[TEVK_STAGES];
    for (int s = 0; s < n; ++s) {
        TevKeyStage* st = &out->st[s];
        for (int i = 0; i < 4; ++i) { if (st->cin[i] > 14) st->cin[i] = 15; if (st->ain[i] > 6) st->ain[i] = 7; }   // read as zero
        // op: 8..15 are the compare ops (kept distinct; the hardware ignores bias and scale for them, so both are zeroed); 1 SUB; anything else is
        // read as ADD. Out-of-range ops (2..7, >15) are refused by the generators; they are left alone here.
        if (st->cop[0] > 15 || (st->cop[0] >= 2 && st->cop[0] < 8)) { /* refused: untouched */ }
        else if (st->cop[0] >= 8) { st->cop[1] = 0; st->cop[2] = 0; }
        else { st->cop[0] = st->cop[0] == 1; st->cop[1] &= 3; st->cop[2] &= 3; }
        if (st->aop[0] > 15 || (st->aop[0] >= 2 && st->aop[0] < 8)) { /* refused: untouched */ }
        else if (st->aop[0] >= 8) { st->aop[1] = 0; st->aop[2] = 0; }
        else { st->aop[0] = st->aop[0] == 1; st->aop[1] &= 3; st->aop[2] &= 3; }
        st->cop[3] = st->cop[3] != 0; st->cop[4] &= 3;
        st->aop[3] = st->aop[3] != 0; st->aop[4] &= 3;
        int useC = 0, useA = 0;
        for (int i = 0; i < 4; ++i) { if (st->cin[i] == 14) useC = 1; if (st->ain[i] == 6) useA = 1; }
#ifdef TEVCANON_BREAK_KCSEL   /* host negative control: the konst selector dropped even where a stage reads it */
        useC = 0;
#endif
        if (!useC) st->kcsel = 0;
        else if ((st->kcsel >= 8 && st->kcsel <= 11) || st->kcsel > 31) st->kcsel = 8;   // konstC / fColArg: not a constant, not a uK -> zero
        if (!useA) st->kasel = 0;
        else if ((st->kasel >= 8 && st->kasel <= 15) || st->kasel > 31) st->kasel = 8;   // konstA: no uK as a whole colour -> zero
        if (st->ras > 3) st->ras = 0xff;
        if ((st->ras == 2 || st->ras == 3) && !(in->ik.on && in->ik.st[s][0] && (in->ik.st[s][5] >> 1 & 3))) st->ras = 0xff;   // bump alpha without a bump select is zero
        if (fast) {   // the fast generator reads tx / rs only where an argument names it
            if (!canonUsesTx(st)) st->texSlot = 0xff;
            if (!canonUsesRs(st)) st->ras = 0xff;
        }
#ifdef TEVCANON_BREAK_TEXSLOT   /* host negative control: the texture slot dropped on every stage */
        st->texSlot = st->texSlot == 0xff ? 0xff : 0;
#endif
        txRef[s] = st->texSlot != 0xff ? (st->texSwap & 3) : -1;
        rsRef[s] = st->ras <= 1 ? (st->rasSwap & 3) : -1;
    }
    // swap tables: only the rows a stage points at are read (their CONTENT, masked as swz does); every other row is set to GXInit's
    // default (RGBA, RRRA, GGGA, BBBA). The row INDEX is kept, not renumbered: a draw whose key is already clean (default tables) is its own
    // canonical form, so a program binary saved under that key's hash on an earlier boot is still found (shc: key hash + key bytes).
    static const uint8_t kDefSwap[4][4] = { {0,1,2,3}, {0,0,0,3}, {1,1,1,3}, {2,2,2,3} };
    int refd[4] = { 0, 0, 0, 0 };
    for (int s = 0; s < n; ++s) {
        TevKeyStage* st = &out->st[s];
        if (txRef[s] >= 0) { st->texSwap = (uint8_t)txRef[s]; refd[txRef[s]] = 1; } else st->texSwap = 0;
        if (rsRef[s] >= 0) { st->rasSwap = (uint8_t)rsRef[s]; refd[rsRef[s]] = 1; } else st->rasSwap = 0;
    }
    for (int t = 0; t < 4; ++t) {
        if (refd[t]) for (int i = 0; i < 4; ++i) out->swap[t][i] = in->swap[t][i] & 3;
        else memcpy(out->swap[t], kDefSwap[t], 4);
    }
#ifdef TEVCANON_BREAK_SWAP   /* host negative control: the CONTENT of a row in use is ignored */
    for (int t = 0; t < 4; ++t) memcpy(out->swap[t], kDefSwap[t], 4);
#endif
    out->projMask = (uint8_t)(in->projMask & (in->numTex >= 8 ? 0xffu : (1u << in->numTex) - 1u));   // perspective slots: only the slots the key has
    // INDIRECT: keep every field the generator reads, zero the rest. A stage's bias only matters with a matrix; the indirect stage index, slot and
    // scale only with a lookup (matrix or bump alpha used); the wrap only where the stage has a texture; unused stages and indirect stages are zero.
    {
        TevIndKey* ik = &out->ik;
        memset(ik, 0, sizeof *ik);
        if (in->ik.on) {
            int any = 0, used[4] = { 0, 0, 0, 0 };
            for (int s = 0; s < n; ++s) {
                const uint8_t* r = in->ik.st[s]; uint8_t* q = ik->st[s];
                if (!r[0]) continue;
                const int bump = out->st[s].ras == 2 || out->st[s].ras == 3;
                const int mtx = r[3] <= 9 ? r[3] : 0, alpha = bump ? (r[5] >> 1 & 3) : 0, look = mtx || alpha;
                q[0] = 1; q[1] = look ? (uint8_t)(r[1] & 3) : 0; q[2] = mtx ? (uint8_t)(r[2] & 7) : 0; q[3] = (uint8_t)mtx;
                q[4] = out->st[s].texSlot == 0xff ? 0 : (uint8_t)(r[4] & 7);
                q[5] = (uint8_t)((r[5] & 1) | (alpha << 1));
                if (look) used[r[1] & 3] = 1;
                any = 1;
            }
            if (any) {
                ik->on = 1; ik->stu = (uint8_t)(in->ik.stu % 3);
                for (int i = 0; i < 4; ++i) {
                    ik->slot[i] = used[i] ? in->ik.slot[i] : 0xff;
                    if (used[i]) { ik->sc[i][0] = in->ik.sc[i][0]; ik->sc[i][1] = in->ik.sc[i][1]; }
                }
            } else memset(ik, 0, sizeof *ik);
        }
    }
#ifdef TEVCANON_BREAK_CMPOP   /* host negative control: the old canonicaliser, op collapsed to `== 1` (a compare becomes an add) */
    for (int s = 0; s < n; ++s) { out->st[s].cop[0] = out->st[s].cop[0] == 1; out->st[s].aop[0] = out->st[s].aop[0] == 1; }
#endif
#ifdef TEVCANON_BREAK_BIAS   /* host negative control: the colour bias dropped */
    for (int s = 0; s < n; ++s) out->st[s].cop[1] = 0;
#endif
}
