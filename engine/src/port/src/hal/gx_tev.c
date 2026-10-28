// gx_tev.c - TEV / texgen / indirect state: RECORDED, and censused per draw (vita fork, docs/47 run 50).
//
// Why (Vita runs 48-49): every TEV setter in the link was a do-nothing scaffold stub
// (scaffold/game_sky_stubs.c, game_objprint_stubs.c), so the port knew nothing of which texture
// map a stage samples, how stages combine, or where texcoords come from. The single-texture draw
// path samples whatever GX_TEXMAP0 last held: run 49's journal had 1,099 of ~2,540 textured
// draws -- the title's characters among them -- on ONE 32x32 RGBA8 texture, with sane UVs.
// The characters are SFA's fur: shell layers cut by a noise texture through indirect stages and
// TEV alpha (objprint_dolphin.c objFuzz*), which a one-texture modulate cannot express.
//
// The plan is docs/42 s8.3: no ubershader (a dynamic branch saves nothing on this GPU, docs/37;
// dead shader code costs 0.37 ms, docs/38) and no runtime compile (a Cg error reboots the
// console), so the TEV configurations the game uses must be COUNTED first, then one shader
// per configuration precompiled on the host. This file is step one: the setters record, and
// on the journal frame (rhi_vita_journal, vita_tool_main.c CAPTURE_AT) every draw's TEV key is
// hashed and censused. Nothing here changes what is drawn.
#include "dolphin/gx.h"
#include "port/renderer/rhi.h"
#include "port/gx_tev_key.h"
#include "port/gx_hal_state.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TEV_STAGES GXH_TEV_STAGES
#define TEX_GENS   GXH_TEX_GENS
#define IND_STAGES GXH_IND_STAGES

// R2 (docs/54, host/engine/R2_STATE.md): every static a TEV / texgen / indirect SETTER writes lives in the HAL state struct (port/gx_hal_state.h), which this object DEFINES
// (two instances: gx_hal = the one the draw path reads, gx_hal_g = G's while the render thread's state records are active). The readers below keep their old names through
// these macros; the setter bodies (gxh_b_*) use ONLY their `h` parameter (gxh_lint.py).
GxHal gx_hal = GXH_INIT, gx_hal_g = GXH_INIT;
const GxhGroup gxh_groups[GXH_NGROUPS] = { GXH_GROUP_TABLE };
_Static_assert(sizeof(GxHal) % 4 == 0, "GxHal is word-sized");
const char *const gxh_kind_names[GXH_NKINDS] = { "tev-stage", "tev-gen", "tev-counts", "ind-order", "tev-regs", "konst-regs", "swap-table", "ind-matrices", "vtx-desc", "vat",
    "projection", "pos-mtx", "nrm-mtx", "tex-mtx", "post-mtx", "light", "chan", "pixel", "copy-src", "tev-op" };
// R3a (host/engine/R3_DRAW.md): the cut switches and the generation counters of the draw path's memos (port/gx_hal_state.h "GENERATIONS").  DEFINED here, in the one object every HAL test links.
unsigned gx_r3a = GX_R3A_ALL & ~GX_R3A_C3;   // the release default: C1, C2, C4 ON, C3 OFF (hw_R3a, docs/54 2026-10-24: C3 measured harmful); gx_thread.c re-publishes it from the perf arm at every frame edge
void gx_r3a_set(unsigned mask) { gx_r3a = mask & (GX_R3A_ALL | GX_R3C_ALL); }
uint32_t gx_hal_gen[GXH_NKINDS_];
const uint16_t gxh_kflag_off[GXH_NKINDS_] = {   // indexed by kind (GXH_K_*)
    [GXH_K_STAGE] = offsetof(GxHal, kd_stage), [GXH_K_GEN] = offsetof(GxHal, kd_gen), [GXH_K_TEVCNT] = offsetof(GxHal, kd_tevcnt), [GXH_K_IND] = offsetof(GxHal, kd_ind),
    [GXH_K_TEVREG] = offsetof(GxHal, kd_tevreg), [GXH_K_KREG] = offsetof(GxHal, kd_kreg), [GXH_K_SWAP] = offsetof(GxHal, kd_swap), [GXH_K_INDM] = offsetof(GxHal, kd_indm),
    [GXH_K_DESC] = offsetof(GxHal, kd_desc), [GXH_K_VAT] = offsetof(GxHal, kd_vat), [GXH_K_PROJ] = offsetof(GxHal, kd_proj), [GXH_K_POS] = offsetof(GxHal, kd_pos),
    [GXH_K_NRM] = offsetof(GxHal, kd_nrm), [GXH_K_TEXM] = offsetof(GxHal, kd_texm), [GXH_K_POSTM] = offsetof(GxHal, kd_texm),   /* one shared flag */ [GXH_K_LIGHT] = offsetof(GxHal, kd_light),
    [GXH_K_CHAN] = offsetof(GxHal, kd_chan), [GXH_K_PIX] = offsetof(GxHal, kd_pix), [GXH_K_COPY] = offsetof(GxHal, kd_copy), [GXH_K_TEVOP] = offsetof(GxHal, kd_tevop),
};
void gxh_collect(void) {                     // flags -> generations; runs on the thread that owns gx_hal (G in DIRECT, R while threaded).  Two kinds may share one flag byte (TEXM / POSTM): read all, then clear
    uint8_t* const base = (uint8_t*)&gx_hal;
    unsigned any = 0, set = 0;
    for (unsigned k = 0; k < GXH_NKINDS_; ++k) if (base[gxh_kflag_off[k]]) set |= 1u << k;
    if (!set) return;
    for (unsigned k = 0; k < GXH_NKINDS_; ++k) { if (set >> k & 1u) { ++gx_hal_gen[k]; any = 1; } }
    if (any) for (unsigned k = 0; k < GXH_NKINDS_; ++k) base[gxh_kflag_off[k]] = 0;
}
void gxh_kdirty_all(GxHal* h) { for (unsigned k = 0; k < GXH_NKINDS_; ++k) ((uint8_t*)h)[gxh_kflag_off[k]] = 1; }
void gxh_kdirty_set(GxHal* h, unsigned kind) { if (kind < GXH_NKINDS_) ((uint8_t*)h)[gxh_kflag_off[kind]] = 1; }

#define gTev      (gx_hal.tev)
#define gx_tev_reg (gx_hal.tevReg)   // colour registers: recorded (a shader reads them as uniforms)
#define gx_tev_kreg (gx_hal.kreg)
#define gRegS10   (gx_hal.regS10)    // the same registers at full S10 range (-1024..1023): the TEV program's uniforms
#define gSwapTbl  (gx_hal.swapTbl)   // GXInit's tables are RGBA, RRRA, GGGA, BBBA
#define gIndMtx   (gx_hal.indMtx)
#define gIndE     (gx_hal.indE)      /* 2^scale_exp of ITM_0..2 */
#define gIndSc    (gx_hal.indSc)

// GXSetTevSwapModeTable (was a no-op in gx_draw.c)
GXH_INLINE void gxh_b_GXSetTevSwapModeTable(GxHal* h, const int mark, GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a) {
    if ((unsigned)table >= 4) return;
    h->swapTbl[table][0] = (u8)r; h->swapTbl[table][1] = (u8)g; h->swapTbl[table][2] = (u8)b; h->swapTbl[table][3] = (u8)a;
    GXH_MARK(h, mark, GXH_G_SWAP);
}
GXH_PAIR(GXSetTevSwapModeTable, (GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a), table, r, g, b, a)

GXH_INLINE void gxh_b_GXSetNumTevStages(GxHal* h, const int mark, u8 n) { h->tev.numStages = n; GXH_MARK(h, mark, GXH_G_TEVCNT); }
GXH_PAIR(GXSetNumTevStages, (u8 n), n)
GXH_INLINE void gxh_b_GXSetTevOrder(GxHal* h, const int mark, GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    if ((unsigned)s >= TEV_STAGES) return;
    h->tev.st[s].coord = (u8)coord; h->tev.st[s].map = (u8)map; h->tev.st[s].color = (u8)color;
    h->tev.st[s].dis = (u8)((unsigned)map >> 8 & 1);
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevOrder, (GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color), s, coord, map, color)
GXH_INLINE void gxh_b_GXSetTevColorIn(GxHal* h, const int mark, GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    if ((unsigned)s >= TEV_STAGES) return;
    u8* p = h->tev.st[s].cin; p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)c; p[3] = (u8)d;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevColorIn, (GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d), s, a, b, c, d)
GXH_INLINE void gxh_b_GXSetTevAlphaIn(GxHal* h, const int mark, GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    if ((unsigned)s >= TEV_STAGES) return;
    u8* p = h->tev.st[s].ain; p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)c; p[3] = (u8)d;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevAlphaIn, (GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d), s, a, b, c, d)
GXH_INLINE void gxh_b_GXSetTevColorOp(GxHal* h, const int mark, GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    if ((unsigned)s >= TEV_STAGES) return;
    u8* p = h->tev.st[s].cop; p[0] = (u8)op; p[1] = (u8)bias; p[2] = (u8)scale; p[3] = (u8)clamp; p[4] = (u8)out;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevColorOp, (GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out), s, op, bias, scale, clamp, out)
GXH_INLINE void gxh_b_GXSetTevAlphaOp(GxHal* h, const int mark, GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out) {
    if ((unsigned)s >= TEV_STAGES) return;
    u8* p = h->tev.st[s].aop; p[0] = (u8)op; p[1] = (u8)bias; p[2] = (u8)scale; p[3] = (u8)clamp; p[4] = (u8)out;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevAlphaOp, (GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out), s, op, bias, scale, clamp, out)
GXH_INLINE void gxh_b_GXSetTevSwapMode(GxHal* h, const int mark, GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex) {
    if ((unsigned)s >= TEV_STAGES) return;
    h->tev.st[s].rasSwap = (u8)ras; h->tev.st[s].texSwap = (u8)tex;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
}
GXH_PAIR(GXSetTevSwapMode, (GXTevStageID s, GXTevSwapSel ras, GXTevSwapSel tex), s, ras, tex)
GXH_INLINE void gxh_b_GXSetTevKColorSel(GxHal* h, const int mark, GXTevStageID s, GXTevKColorSel sel) { if ((unsigned)s < TEV_STAGES) { h->tev.st[s].kcsel = (u8)sel; GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s); } }
GXH_PAIR(GXSetTevKColorSel, (GXTevStageID s, GXTevKColorSel sel), s, sel)
GXH_INLINE void gxh_b_GXSetTevKAlphaSel(GxHal* h, const int mark, GXTevStageID s, GXTevKAlphaSel sel) { if ((unsigned)s < TEV_STAGES) { h->tev.st[s].kasel = (u8)sel; GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s); } }
GXH_PAIR(GXSetTevKAlphaSel, (GXTevStageID s, GXTevKAlphaSel sel), s, sel)
GXH_INLINE void gxh_b_GXSetTevColor(GxHal* h, const int mark, GXTevRegID id, GXColor c) {
    if ((unsigned)id >= 4) return;
    h->tevReg[id] = c;
    h->regS10[id][0] = c.r; h->regS10[id][1] = c.g; h->regS10[id][2] = c.b; h->regS10[id][3] = c.a;
    GXH_MARK(h, mark, GXH_G_TEVREG);
}
GXH_PAIR(GXSetTevColor, (GXTevRegID id, GXColor c), id, c)
GXH_INLINE void gxh_b_GXSetTevColorS10(GxHal* h, const int mark, GXTevRegID id, GXColorS10 c) {
    if ((unsigned)id >= 4) return;   // S10 range clamped to u8: the recorder's view, not the shader's
    h->regS10[id][0] = c.r; h->regS10[id][1] = c.g; h->regS10[id][2] = c.b; h->regS10[id][3] = c.a;
    #define CL(v) (u8)((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    h->tevReg[id].r = CL(c.r); h->tevReg[id].g = CL(c.g); h->tevReg[id].b = CL(c.b); h->tevReg[id].a = CL(c.a);
    #undef CL
    GXH_MARK(h, mark, GXH_G_TEVREG);
}
GXH_PAIR(GXSetTevColorS10, (GXTevRegID id, GXColorS10 c), id, c)
GXH_INLINE void gxh_b_GXSetTevKColor(GxHal* h, const int mark, GXTevKColorID id, GXColor c) { if ((unsigned)id < 4) { GXH_MARK(h, mark, GXH_G_KREG); h->kreg[id] = c; } }
GXH_PAIR(GXSetTevKColor, (GXTevKColorID id, GXColor c), id, c)
GXH_INLINE void gxh_b_GXSetTevDirect(GxHal* h, const int mark, GXTevStageID s) { if ((unsigned)s < TEV_STAGES) { memset(h->tev.st[s].ind, 0, sizeof h->tev.st[s].ind); GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s); } }
GXH_PAIR(GXSetTevDirect, (GXTevStageID s), s)
GXH_INLINE void gxh_b_GXSetTevIndirect(GxHal* h, const int mark, GXTevStageID s, GXIndTexStageID ind, GXIndTexFormat fmt, GXIndTexBiasSel bias,
                      GXIndTexMtxID mtx, GXIndTexWrap ws, GXIndTexWrap wt, GXBool addPrev, GXBool utcLod,
                      GXIndTexAlphaSel alpha) {
    if ((unsigned)s >= TEV_STAGES) return;
    GXH_MARK(h, mark, GXH_G_STAGE0 + (int)s);
    u8* p = h->tev.st[s].ind;
    p[0] = 1; p[1] = (u8)ind; p[2] = (u8)fmt; p[3] = (u8)bias; p[4] = (u8)mtx;
    p[5] = (u8)ws; p[6] = (u8)wt; p[7] = (u8)addPrev; p[8] = (u8)utcLod; p[9] = (u8)alpha;
}
GXH_PAIR(GXSetTevIndirect, (GXTevStageID s, GXIndTexStageID ind, GXIndTexFormat fmt, GXIndTexBiasSel bias, GXIndTexMtxID mtx, GXIndTexWrap ws, GXIndTexWrap wt, GXBool addPrev, GXBool utcLod, GXIndTexAlphaSel alpha),
         s, ind, fmt, bias, mtx, ws, wt, addPrev, utcLod, alpha)
GXH_INLINE void gxh_b_GXSetNumTexGens(GxHal* h, const int mark, u8 n) { h->tev.numGens = n; GXH_MARK(h, mark, GXH_G_TEVCNT); }
GXH_PAIR(GXSetNumTexGens, (u8 n), n)
GXH_INLINE void gxh_b_GXSetTexCoordGen2(GxHal* h, const int mark, GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool norm, u32 post) {
    if ((unsigned)dst >= TEX_GENS) return;
    TexGen* g = &h->tev.gen[dst];
    g->func = (u8)func; g->src = (u8)src; g->mtx = (u8)mtx; g->norm = (u8)norm; g->post = (u8)post;
    GXH_MARK(h, mark, GXH_G_GEN0 + (int)dst);
}
GXH_PAIR(GXSetTexCoordGen2, (GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool norm, u32 post), dst, func, src, mtx, norm, post)
GXH_INLINE void gxh_b_GXSetNumIndStages(GxHal* h, const int mark, u8 n) { h->tev.numInd = n; GXH_MARK(h, mark, GXH_G_TEVCNT); }
GXH_PAIR(GXSetNumIndStages, (u8 n), n)
GXH_INLINE void gxh_b_GXSetIndTexOrder(GxHal* h, const int mark, GXIndTexStageID s, GXTexCoordID coord, GXTexMapID map) {
    if ((unsigned)s >= IND_STAGES) return;
    h->tev.indOrder[s][0] = (u8)coord; h->tev.indOrder[s][1] = (u8)map;
    GXH_MARK(h, mark, GXH_G_IND);
}
GXH_PAIR(GXSetIndTexOrder, (GXIndTexStageID s, GXTexCoordID coord, GXTexMapID map), s, coord, map)
// INDIRECT matrices and coordinate scales (2026-10-12). They were scaffold no-ops (game_objprint_stubs.c, weak); now recorded. SDK source:
// dolsdk2004 src/gx/GXBump.c GXSetIndTexMtx: each element is (int)(1024.0f * f) & 0x7FF in an 11-bit field (s1.10: -1..+1), the exponent is
// stored +17; the hardware applies  offset = (M * (s, t, u)) * 2^scale_exp  with s, t, u the indirect texel's 8-bit values after the bias.
// What is kept is the EFFECTIVE float matrix (quantised element / 1024 * 2^exp) of ITM_0..2; the dynamic ITM_S/T matrices are not recorded
// (a stage that names one is refused). A matrix id that is not ITM_0..2 is ignored here, as the SDK would write other registers.
GXH_INLINE void gxh_b_GXSetIndTexMtx(GxHal* h, const int mark, GXIndTexMtxID id, const f32 offset[2][3], s8 scale_exp) {
    if ((unsigned)id < 1 || (unsigned)id > 3 || !offset) return;
#ifdef TEVIND_BREAK_EXP   /* host negative control: the matrix exponent off by one */
    const float sc = ldexpf(1.0f, (int)scale_exp + 1);
#else
    const float sc = ldexpf(1.0f, (int)scale_exp);
#endif
    GXH_MARK(h, mark, GXH_G_INDM);   /* before the call and the loop: the base register is still live */
    h->indE[id - 1] = sc;
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 3; ++c) {
        int v = (int)(1024.0f * offset[r][c]) & 0x7FF;
        if (v & 0x400) v -= 0x800;
        h->indMtx[id - 1][r][c] = (float)v / 1024.0f * sc;
    }
}
GXH_PAIR(GXSetIndTexMtx, (GXIndTexMtxID id, const f32 offset[2][3], s8 scale_exp), id, offset, scale_exp)
GXH_INLINE void gxh_b_GXSetIndTexCoordScale(GxHal* h, const int mark, GXIndTexStageID s, GXIndTexScale ss, GXIndTexScale st) {
    if ((unsigned)s >= IND_STAGES) return;
    h->indSc[s][0] = (u8)ss; h->indSc[s][1] = (u8)st;
    GXH_MARK(h, mark, GXH_G_IND);
}
GXH_PAIR(GXSetIndTexCoordScale, (GXIndTexStageID s, GXIndTexScale ss, GXIndTexScale st), s, ss, st)

// GXSetTevOp (dolsdk2004 src/gx/GXTev.c): the five canned modes are just GXSetTevColorIn / AlphaIn / Op calls. The port's GXSetTevOp (gx_draw.c) only RECORDED the mode, so the two stages
// the game builds this way (addEnvMapBumpStages, shader_dolphin.c) kept whatever combiner the previous object left behind. gx_draw.c's GXSetTevOp now calls this.
// (R2: the composed setters are the BODIES above with the same instance and mark, so the G variant never reaches a wrapped public symbol.)
GXH_INLINE void gxh_b_tev_setop(GxHal* h, const int mark, int id, int mode) {
    if ((unsigned)id >= TEV_STAGES) return;
    int carg = GX_CC_RASC, aarg = GX_CA_RASA;
    if (id != 0) { carg = GX_CC_CPREV; aarg = GX_CA_APREV; }
    switch (mode) {
    case 0: gxh_b_GXSetTevColorIn(h, mark, (GXTevStageID)id, GX_CC_ZERO, GX_CC_TEXC, (GXTevColorArg)carg, GX_CC_ZERO); gxh_b_GXSetTevAlphaIn(h, mark, (GXTevStageID)id, GX_CA_ZERO, GX_CA_TEXA, (GXTevAlphaArg)aarg, GX_CA_ZERO); break;   /* MODULATE */
    case 1: gxh_b_GXSetTevColorIn(h, mark, (GXTevStageID)id, (GXTevColorArg)carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO); gxh_b_GXSetTevAlphaIn(h, mark, (GXTevStageID)id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, (GXTevAlphaArg)aarg); break;   /* DECAL */
    case 2: gxh_b_GXSetTevColorIn(h, mark, (GXTevStageID)id, (GXTevColorArg)carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO); gxh_b_GXSetTevAlphaIn(h, mark, (GXTevStageID)id, GX_CA_ZERO, GX_CA_TEXA, (GXTevAlphaArg)aarg, GX_CA_ZERO); break;   /* BLEND */
    case 3: gxh_b_GXSetTevColorIn(h, mark, (GXTevStageID)id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC); gxh_b_GXSetTevAlphaIn(h, mark, (GXTevStageID)id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA); break;   /* REPLACE */
    case 4: gxh_b_GXSetTevColorIn(h, mark, (GXTevStageID)id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, (GXTevColorArg)carg); gxh_b_GXSetTevAlphaIn(h, mark, (GXTevStageID)id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, (GXTevAlphaArg)aarg); break;   /* PASSCLR */
    default: return;
    }
    gxh_b_GXSetTevColorOp(h, mark, (GXTevStageID)id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    gxh_b_GXSetTevAlphaOp(h, mark, (GXTevStageID)id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void gx_tev_setop(int id, int mode) { gxh_b_tev_setop(&gx_hal, 0, id, mode); }
void gxh_g_tev_setop(int id, int mode) { gxh_b_tev_setop(&gx_hal_g, 1, id, mode); }

// --- what a draw path may consume --------------------------------------------------------------
int gx_tev_num_stages(void) { return gTev.numStages; }
int gx_tev_stage_map(int s) { return (unsigned)s < TEV_STAGES ? gTev.st[s].map : 0xff; }
// The texture map of the first active stage whose texcoord is generated straight from the TEX0
// vertex attribute -- the one texture the single-texture path can sample with the UVs it has.
// Run 50's census: characters/objects put a normal-addressed LIGHTING texture on map 0 and the
// diffuse on a later stage (key 6: s2 m2 via g1 = TEX0); the map is one stage, map 0. -1: none.
int gx_tev_tex0_stage_map(void) {
    for (int s = 0; s < gTev.numStages && s < TEV_STAGES; ++s) {
        const TevStage* t = &gTev.st[s];
        if (t->map >= 8 || t->coord >= gTev.numGens || t->coord >= TEX_GENS) continue;
        if (gTev.gen[t->coord].src == GX_TG_TEX0) return t->map;
    }
    return -1;
}
// Any active stage indirect? (run 50: key 11, the fur shells -- cut by noise via two ind stages)
int gx_tev_uses_indirect(void) {
    for (int s = 0; s < gTev.numStages && s < TEV_STAGES; ++s) if (gTev.st[s].ind[0]) return 1;
    return 0;
}

// --- the TEV PROGRAM key (docs/47): what tev_gen turns into one fragment program ------------------
// Texture SLOTS are the distinct (texcoord, map) pairs the active stages sample, in stage order;
// slotCoord/slotMap tell the draw path which texgen and which bound texture each slot is. Returns 0
// when this state cannot go through a TEV program -- an indirect stage, compare mode, more than
// TEVK_TEX slots, a texcoord past numGens -- and the caller falls back (and counts it).
unsigned gx_tev_refused_indirect, gx_tev_refused_slots, gx_tev_refused_compare;
// INDIRECT texturing counters ([tevind] line) and the kill switch ux0:data/stairfax/tev_ind_off.txt starting with 1 (= refuse every indirect stage, as before).
unsigned gx_tev_ind_draws, gx_tev_ind_refused;
// EVERY refusal reason, counted separately (2026-10-12): a refused build sends its display list (or primitive) down the uncached per-primitive fallback. gx_tev_build is
// called once by dlkCall's front half for a list and once per primitive by the per-primitive loop, so a refused list of n primitives counts 1 + n builds here; the
// caller can attribute lists and primitives exactly with gx_tev_last_refusal() (NULL when the last build succeeded). Logged on the first build refused for a reason and
// every 8192nd refused build: [tevref] ...
static const char* gWhyName[24]; static unsigned gWhyN[24]; static int gWhyUsed; static unsigned gWhyTotal;
static const char* gLastWhy;
const char* gx_tev_last_refusal(void) { return gLastWhy; }
static void whyCount(const char* why) {
    int i = 0; while (i < gWhyUsed && gWhyName[i] != why) ++i;
    if (i == gWhyUsed) { if (gWhyUsed == 24) i = 23; else { gWhyName[gWhyUsed++] = why; } }
    const int first = gWhyN[i] == 0; ++gWhyN[i]; ++gWhyTotal; gLastWhy = why;
    if (first || !(gWhyTotal & 8191)) {
        printf("[tevref] builds refused by reason (a refused list = 1 + its primitives):");
        for (int q = 0; q < gWhyUsed; ++q) printf(" | %s: %u", gWhyName[q], gWhyN[q]);
        printf("\n");
    }
}
static uint32_t gIndKeys[64]; static int gIndKeysN;
static uint32_t gIndRefHash[48]; static int gIndRefN;
static int gIndOn = -1, gIndStu;   /* gIndStu: ux0:data/stairfax/tev_ind_stu.txt, the channel order of S, T, U (TevIndKey.stu) */
int gx_tev_ind_force;   /* host tests: 1 = indirect on whatever the marker files say */
static int indEnabled(void) {
    if (gx_tev_ind_force) return 1;
    if (gIndOn < 0) {
        FILE* f = fopen("ux0:data/stairfax/tev_ind_off.txt", "r"); int off = f && fgetc(f) == 49; if (f) fclose(f);   /* the kill switch: 1 = refuse every indirect stage, as before 2026-10-12 */
        gIndOn = !off;
        { FILE* g = fopen("ux0:data/stairfax/tev_ind_stu.txt", "r"); int c = g ? fgetc(g) : -1; if (g) fclose(g); gIndStu = c >= '0' && c <= '2' ? c - '0' : 0; }
        printf("[tevind] S,T,U channel order %d (%s)\n", gIndStu, gIndStu == 0 ? "A,B,G (assumed)" : gIndStu == 1 ? "R,G,B" : "A,R,G");
        printf("[tevind] indirect texturing %s%s", gIndOn ? "ON (default; tev_ind_off.txt starting with 1 turns it off)" : "OFF (tev_ind_off.txt 1): every indirect stage is refused, the draw falls back as before", "\n");
    }
    return gIndOn;
}
int gx_tev_ind_enabled(void) { return indEnabled(); }   /* the game-side water setup follows the same runtime switch (intersect_render.c STFX_WATER_IND) */
// A stage whose indirect setup is outside the supported subset: counted, and logged ONCE per distinct setup.
static int indRefuse(const TevStage* t, const char* why) {
    uint32_t h = 2166136261u; for (int i = 0; i < 10; ++i) h = (h ^ t->ind[i]) * 16777619u; for (const char* c = why; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
    ++gx_tev_ind_refused; ++gx_tev_refused_indirect; whyCount(why);
    int j = 0; while (j < gIndRefN && gIndRefHash[j] != h) ++j;
    if (j == gIndRefN && gIndRefN < 48) {
        gIndRefHash[gIndRefN++] = h;
        printf("[tevind] refused (%s): stage ind st%u fmt%u bias%u mtx%u wrap%u/%u addprev%u utc%u alpha%u\n", why, t->ind[1], t->ind[2], t->ind[3], t->ind[4], t->ind[5], t->ind[6], t->ind[7], t->ind[8], t->ind[9]);
    }
    return 0;
}
// The slot of a (texcoord, map) pair in the key (allocated on first use), or -1 when the key already has TEVK_TEX of them (counted, as ever).
static int slotOf(TevKey* k, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX], int coord, int map, int nst) {
    int sl = 0;
    while (sl < k->numTex && !(slotCoord[sl] == coord && slotMap[sl] == map)) ++sl;
    if (sl == k->numTex) {
        if (k->numTex == TEVK_TEX) {
            /* run 127: WHICH state asks for more than TEVK_TEX pairs (the title's 12K "slots"
               refusals)? Its stages' (coord, map), the first 4 times and then every 4096th. */
            if (gx_tev_refused_slots < 4 || !(gx_tev_refused_slots & 4095)) {
                printf("[tevslots] refusal %u: %d stages, gens %d, (coord/map):", gx_tev_refused_slots, gTev.numStages, gTev.numGens);
                for (int q = 0; q < nst; ++q) printf(" %d/%d", gTev.st[q].coord, gTev.st[q].map);
                printf("\n");
            }
            ++gx_tev_refused_slots; whyCount("slots: more than 8 (texcoord, map) pairs"); return -1;
        }
        slotCoord[sl] = (u8)coord; slotMap[sl] = (u8)map; k->numTex++;
    }
    return sl;
}
unsigned gx_tev_cmp_draws;   /* gx_tev_build calls whose key has a compare stage (a draw, or a cached-list rebuild) */
// The numbers of a TEV draw (TevUniforms), a pure function of the HAL kinds TEVREG / KREG / INDM and the two alpha refs (and of the key: the indirect matrices only when it is indirect).
static void tev_fill_uniforms(const TevKey* k, TevUniforms* u, int acRef0, int acRef1) {
    memset(u, 0, sizeof *u);   /* the struct is memcmp'd to batch draws: padding and the fog fields start at zero (docs/47 run 116) */
    memcpy(u->reg, gRegS10, sizeof u->reg);
    for (int r = 0; r < 4; ++r) { u->konst[r][0] = gx_tev_kreg[r].r; u->konst[r][1] = gx_tev_kreg[r].g;
                                  u->konst[r][2] = gx_tev_kreg[r].b; u->konst[r][3] = gx_tev_kreg[r].a; }
    u->acRef0 = (uint8_t)acRef0; u->acRef1 = (uint8_t)acRef1;
    if (k->ik.on) { memcpy(u->indM, gIndMtx, sizeof u->indM); memcpy(u->indE, gIndE, sizeof u->indE); }   // the effective matrices (tev_gen declares only the ones the key uses)
}
static uint32_t gCmpKeys[64]; static int gCmpKeysN;
static int gx_tev_build_ref(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX],
                            int acComp0, int acOp, int acComp1, int acRef0, int acRef1) {
    gLastWhy = NULL;
    memset(k, 0, sizeof *k);
    int n = gTev.numStages > TEVK_STAGES ? TEVK_STAGES : gTev.numStages;
    if (n < 1) n = 1;
    k->numStages = (uint8_t)n;
    memcpy(k->swap, gSwapTbl, sizeof k->swap);
    k->acComp0 = (uint8_t)acComp0; k->acOp = (uint8_t)acOp; k->acComp1 = (uint8_t)acComp1;
    for (int s = 0; s < n; ++s) {
        const TevStage* t = &gTev.st[s];
        TevKeyStage* d = &k->st[s];
        // INDIRECT: a stage naming an indirect stage the state has (numInd) is indirect; any other is direct, as the hardware does
        if (t->ind[0] && !indEnabled()) { ++gx_tev_refused_indirect; whyCount("indirect: kill switch tev_ind_off.txt"); return 0; }   /* today's behaviour: ANY indirect stage refuses the key */
        int active = t->ind[0] && t->ind[1] < gTev.numInd && t->ind[1] < IND_STAGES;
        if (active) {
            if (t->ind[2] != 0)                                   return indRefuse(t, "format (only ITF_8)");
            if (t->ind[4] == 4 || t->ind[4] == 8 || t->ind[4] > 11) return indRefuse(t, "indirect matrix id");
            if ((t->ind[5] != 0 && t->ind[5] != 6) || (t->ind[6] != 0 && t->ind[6] != 6)) return indRefuse(t, "wrap (only OFF and ITW_0)");
            if (t->ind[8])                                        return indRefuse(t, "utc lod");
            if (t->ind[9] > 3 || t->ind[3] > 7)                   return indRefuse(t, "bias/alpha select");
        }
        /* compare-mode ops (8..15) are expressed by tev_gen (2026-10-12); what it cannot express is refused after the loop (tev_cmp_unsupported) */
        d->texSlot = 0xff;
        if (t->map < 8 && t->coord < gTev.numGens && t->coord < TEX_GENS) {
            int sl = slotOf(k, slotCoord, slotMap, t->coord, t->map, n);
            if (sl < 0) return 0;
            d->texSlot = (uint8_t)sl;
        } else if (active && t->coord < gTev.numGens && t->coord < TEX_GENS) return indRefuse(t, "texcoord without a texture map");
        if (active) {
            // matrix code: 0 OFF, 1..3 ITM_0..2, 4..6 ITM_S0..2, 7..9 ITM_T0..2 (the dynamic ones multiply the stage's own coordinate: tev_gen.c "INDIRECT TEXTURING")
            const int mtx = t->ind[4] >= 9 ? t->ind[4] - 2 : t->ind[4] >= 5 ? t->ind[4] - 1 : t->ind[4], alpha = t->ind[9], is = t->ind[1];
            uint8_t* q = k->ik.st[s];
            q[0] = 1; q[1] = t->ind[1]; q[2] = t->ind[3]; q[3] = (uint8_t)mtx;
            q[4] = (uint8_t)((t->ind[5] == 6 ? 1 : 0) | (t->ind[6] == 6 ? 2 : 0) | (t->dis && d->texSlot != 0xff ? 4 : 0));
            q[5] = (uint8_t)((t->ind[7] ? 1 : 0) | (alpha << 1));
            k->ik.on = 1; k->ik.stu = (uint8_t)gIndStu;
            if (mtx || alpha) {   // the indirect texel is looked up: its own texcoord and map are a slot of the key
                const int oc = gTev.indOrder[is][0], om = gTev.indOrder[is][1];
                if (om >= 8 || oc >= gTev.numGens || oc >= TEX_GENS) return indRefuse(t, "indirect stage without a valid texcoord / map");
                int sl = slotOf(k, slotCoord, slotMap, oc, om, n);
                if (sl < 0) return 0;
                k->ik.slot[is] = (uint8_t)sl; k->ik.sc[is][0] = gIndSc[is][0]; k->ik.sc[is][1] = gIndSc[is][1];
            }
        }
        d->ras = t->color == 4 ? 0 : t->color == 5 ? 1 : 0xff;   // GX_COLOR0A0, GX_COLOR1A1; ZERO/NULL read as 0
        if ((t->color == 7 || t->color == 8) && active && (t->ind[9] & 3)) d->ras = (uint8_t)(t->color == 7 ? 2 : 3);   // GX_ALPHA_BUMP / _BUMPN: the bump alpha of THIS stage's indirect setup (zero without one)
        memcpy(d->cin, t->cin, 4); memcpy(d->ain, t->ain, 4); memcpy(d->cop, t->cop, 5); memcpy(d->aop, t->aop, 5);
        d->rasSwap = t->rasSwap; d->texSwap = t->texSwap; d->kcsel = t->kcsel; d->kasel = t->kasel;
    }
    if (tev_cmp_unsupported(k)) { ++gx_tev_refused_compare; whyCount("combiner op: ops 2..7 / >15, bias 3 on add/sub, or alpha compare ops 8..13"); return 0; }
    if (k->ik.on) {
        for (int i = 0; i < 4; ++i) { int used = 0; for (int s = 0; s < n; ++s) if (k->ik.st[s][0] && k->ik.st[s][1] == i && (k->ik.st[s][3] || (k->ik.st[s][5] >> 1))) used = 1; if (!used) k->ik.slot[i] = 0xff; }
        uint32_t h = 2166136261u; for (size_t i = 0; i < sizeof *k; ++i) h = (h ^ ((const uint8_t*)k)[i]) * 16777619u;
        int j = 0, fresh = 0; while (j < gIndKeysN && gIndKeys[j] != h) ++j;
        if (j == gIndKeysN && gIndKeysN < 64) { gIndKeys[gIndKeysN++] = h; fresh = 1; }
        if (!(gx_tev_ind_draws++ & 1023) || fresh)
            printf("[tevind] keys with indirect stages: %d, draws: %u, refused (unsupported feature): %u\n", gIndKeysN, gx_tev_ind_draws, gx_tev_ind_refused);
    }
    if (tev_has_compare(k)) {   /* hardware evidence that the compare path is taken: distinct keys and builds, logged on the first and every 1024th */
        uint32_t h = 2166136261u; for (size_t i = 0; i < sizeof *k; ++i) h = (h ^ ((const uint8_t*)k)[i]) * 16777619u;
        int j = 0, fresh = 0; while (j < gCmpKeysN && gCmpKeys[j] != h) ++j;
        if (j == gCmpKeysN && gCmpKeysN < 64) { gCmpKeys[gCmpKeysN++] = h; fresh = 1; }
        if (!(gx_tev_cmp_draws++ & 1023) || fresh)
            printf("[tevcmp] keys with a compare stage: %d, draws: %u\n", gCmpKeysN, gx_tev_cmp_draws);
    }
    if (u) tev_fill_uniforms(k, u, acRef0, acRef1);
    return 1;
}
/* R3c C5: the uniform half of gx_tev_build_ref, callable alone (the program-clean draw keeps the TevKey and rebuilds only these numbers).  gx_tev_build_ref is the composition: key, then this. */
void gx_tev_build_uniforms(const TevKey* k, TevUniforms* u, int acRef0, int acRef1) { tev_fill_uniforms(k, u, acRef0, acRef1); }
/* R3c C5: the statistics gx_tev_build_ref keeps for a key it is NOT rebuilding (logs only; the same replay the C3 memo does). */
void gx_tev_replay_stats(const TevKey* k) {
    gLastWhy = NULL;
    if (k->ik.on && !(gx_tev_ind_draws++ & 1023)) printf("[tevind] keys with indirect stages: %d, draws: %u, refused (unsupported feature): %u\n", gIndKeysN, gx_tev_ind_draws, gx_tev_ind_refused);
    if (tev_has_compare(k) && !(gx_tev_cmp_draws++ & 1023)) printf("[tevcmp] keys with a compare stage: %d, draws: %u\n", gCmpKeysN, gx_tev_cmp_draws);
}

/* ---- R3a C3 (host/engine/R3_DRAW.md s2): THE MEMO OF gx_tev_build ------------------------------------------------------------------------------------------------------------------------
 * gx_tev_build is a pure function of: the HAL kinds it reads, its nine arguments, and nothing else.  Its generation table (what the memo keys on, and why):
 *     STAGE   gTev.st[]: the stage orders, combiner ops, swap/konst selects, the indirect setup of each stage
 *     TEVCNT  gTev.numStages / numGens / numInd
 *     IND     gTev.indOrder, gIndSc (the indirect stage's texcoord / map, the coordinate scale)
 *     SWAP    gSwapTbl
 *     TEVREG  gRegS10 (the TevUniforms registers)
 *     KREG    gx_tev_kreg (the TevUniforms konst colours)
 *     INDM    gIndMtx, gIndE (copied into TevUniforms when the key is indirect)
 *   arguments by value: acComp0, acOp, acComp1 (the key), acRef0, acRef1 (the uniforms); and whether the caller wants TevUniforms at all.
 *   NOT keyed on, with the reason: gen[] / tevOp[] / tevReg[] / COPY / PIX (never read); indEnabled() and gIndStu (read once from files at the first indirect stage, constants afterwards -- the first
 *   build that reaches them is always a miss); the counters and the logs (replayed below).
 * Only a SUCCESSFUL build is remembered (a refusal counts a reason and returns 0 on every call, as today).  The memo is one entry: the draw path asks for the same state many times in a row
 * (a model's display lists share a material). */
typedef struct {
    int valid, hasU;
    uint32_t gen[7];
    int args[5];
    TevKey k; TevUniforms u; u8 sc[TEVK_TEX], sm[TEVK_TEX];
} TevMemo;
static TevMemo gTm;
unsigned gx_tev_memo_hits, gx_tev_memo_miss;
uint32_t gx_tev_key_serial;                /* bumped when a (re)computed key differs from the remembered one: a cheap identity for the program lookup (rhi_gxm_shader.c hint) */
static const uint8_t kTevMemoKinds[7] = { GXH_K_STAGE, GXH_K_TEVCNT, GXH_K_IND, GXH_K_SWAP, GXH_K_TEVREG, GXH_K_KREG, GXH_K_INDM };
int gx_tev_build(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX],
                 int acComp0, int acOp, int acComp1, int acRef0, int acRef1) {
    if (!(gx_r3a & GX_R3A_C3)) { gTm.valid = 0; return gx_tev_build_ref(k, u, slotCoord, slotMap, acComp0, acOp, acComp1, acRef0, acRef1); }
    gxh_collect();
    const int args[5] = { acComp0, acOp, acComp1, acRef0, acRef1 };
    if (gTm.valid && (u == NULL || gTm.hasU) && !memcmp(gTm.args, args, sizeof args)) {
        int same = 1;
        for (int i = 0; i < 7; ++i) same &= gTm.gen[i] == gx_hal_gen[kTevMemoKinds[i]];
        if (same) {
            gLastWhy = NULL;
            memcpy(k, &gTm.k, sizeof *k); if (u) memcpy(u, &gTm.u, sizeof *u);
            memcpy(slotCoord, gTm.sc, gTm.k.numTex); memcpy(slotMap, gTm.sm, gTm.k.numTex);   /* the build writes only the slots the key uses; the rest of the caller's arrays is left as it was */
            /* the statistics the full build keeps (logs only; the key table behind "fresh" is not replayed): */
            if (k->ik.on && !(gx_tev_ind_draws++ & 1023)) printf("[tevind] keys with indirect stages: %d, draws: %u, refused (unsupported feature): %u\n", gIndKeysN, gx_tev_ind_draws, gx_tev_ind_refused);
            if (tev_has_compare(k) && !(gx_tev_cmp_draws++ & 1023)) printf("[tevcmp] keys with a compare stage: %d, draws: %u\n", gCmpKeysN, gx_tev_cmp_draws);
            ++gx_tev_memo_hits;
            return 1;
        }
    }
    ++gx_tev_memo_miss;
    const int r = gx_tev_build_ref(k, u, slotCoord, slotMap, acComp0, acOp, acComp1, acRef0, acRef1);
    if (!r) { gTm.valid = 0; return 0; }
    if (!gTm.valid || memcmp(&gTm.k, k, sizeof *k)) ++gx_tev_key_serial;
    gTm.valid = 1; gTm.hasU = u != NULL;
    memcpy(gTm.args, args, sizeof args);
    for (int i = 0; i < 7; ++i) gTm.gen[i] = gx_hal_gen[kTevMemoKinds[i]];
    memcpy(&gTm.k, k, sizeof *k); if (u) memcpy(&gTm.u, u, sizeof *u);
    memset(gTm.sc, 0, TEVK_TEX); memset(gTm.sm, 0, TEVK_TEX); memcpy(gTm.sc, slotCoord, k->numTex); memcpy(gTm.sm, slotMap, k->numTex);
    return 1;
}
#ifdef GX_C3_SELFCHECK   /* host tests only (gx_rt_state_test.c): the reference build, never memoised, for the dlkCall self-check */
int gx_tev_build_refcall(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX], int acComp0, int acOp, int acComp1, int acRef0, int acRef1) {
    return gx_tev_build_ref(k, u, slotCoord, slotMap, acComp0, acOp, acComp1, acRef0, acRef1);
}
#endif
// Which texgen SOURCES the active stages need, as a mask: bit 0 position, bit 1 normal, bit 2 binormal, bit 3 tangent (2026-10-13), bit 4+n
// TEXn. gx_draw keeps only these per vertex (run 91: keeping all of them cost ~9 ms/frame of decode).
unsigned gx_tev_needed_sources(void) {
    unsigned m = 0;
    for (int s = 0; s < gTev.numStages && s < TEV_STAGES; ++s) {
        const TevStage* t = &gTev.st[s];
        if (t->map >= 8 || t->coord >= gTev.numGens || t->coord >= TEX_GENS) continue;
        unsigned src = gTev.gen[t->coord].src;
        if (src == 0) m |= 1u; else if (src == 1) m |= 2u; else if (src == 2 || src == 3) m |= 1u << src; else if (src >= 4 && src <= 11) m |= 1u << src;
    }
    for (int s = 0; s < gTev.numStages && s < TEV_STAGES; ++s) {   // an indirect stage looks its offset up through ITS OWN texcoord (GXSetIndTexOrder): the source it reads is needed too
        const TevStage* t = &gTev.st[s];
        if (!t->ind[0] || t->ind[1] >= gTev.numInd || t->ind[1] >= IND_STAGES) continue;
        const unsigned oc = gTev.indOrder[t->ind[1]][0], om = gTev.indOrder[t->ind[1]][1];
        if (om >= 8 || oc >= gTev.numGens || oc >= TEX_GENS) continue;
        unsigned src = gTev.gen[oc].src;
        if (src == 0) m |= 1u; else if (src == 1) m |= 2u; else if (src == 2 || src == 3) m |= 1u << src; else if (src >= 4 && src <= 11) m |= 1u << src;
    }
    return m;
}
// The texgen a texcoord index uses (gx_draw computes the coordinates on the CPU).
void gx_tev_texgen(int coord, u8* func, u8* src, u8* mtx, u8* norm, u8* post) {
    const TexGen* g = &gTev.gen[(unsigned)coord < TEX_GENS ? coord : 0];
    *func = g->func; *src = g->src; *mtx = g->mtx; *norm = g->norm; *post = g->post;
}

// --- the census --------------------------------------------------------------------------------
// The KEY is the active part of the state only: stages < numStages, texgens < numGens, indirect
// orders < numInd. Stale entries past the counts must not split one configuration into many.
typedef struct { u8 numStages, numGens, numInd; TevStage st[TEV_STAGES]; TexGen gen[TEX_GENS]; u8 indOrder[IND_STAGES][2]; } CensusKey;
static void keyOf(CensusKey* k) {
    memset(k, 0, sizeof *k);
    k->numStages = gTev.numStages; k->numGens = gTev.numGens; k->numInd = gTev.numInd;
    for (int i = 0; i < gTev.numStages && i < TEV_STAGES; ++i) k->st[i] = gTev.st[i];
    for (int i = 0; i < gTev.numGens && i < TEX_GENS; ++i) k->gen[i] = gTev.gen[i];
    for (int i = 0; i < gTev.numInd && i < IND_STAGES; ++i) { k->indOrder[i][0] = gTev.indOrder[i][0]; k->indOrder[i][1] = gTev.indOrder[i][1]; }
}
#define CENSUS_MAX 512
// CENSUS ON DEMAND (run 259): rhi_vita_census_arm > 0 (set with the shot_now.txt screenshot request, counted down per present) runs the census
// for the frames that follow the screenshot WITHOUT the per-draw journal. Each entry is then also split by the object that set the state
// (gStfxDbgObjTag = romDefNo * 16 + render op, written by objSetupRenderOpGxState; it is the LAST object: map geometry carries a stale tag) and
// records what a key alone does not say: the textures' size / GX format, the colour and konst registers, the channel controls and the vertex layout.
int gStfxDbgObjTag = -1;
extern int rhi_vita_census_arm __attribute__((weak));
extern void rhi_vita_texInfo(RhiTexture* t, int out[4]) __attribute__((weak));
extern void gx_draw_dbg_state(char* buf, int cap) __attribute__((weak));
static struct { CensusKey key; unsigned draws, tris; const void* map0tex[8]; int tag; int texInfo[8][4]; GXColor reg[4], kreg[4]; char st[240]; } gCensus[CENSUS_MAX];
static int gCensusN, gCensusOverflow, gCensusWasOn;
static unsigned gCensusDraws;
extern int rhi_vita_journal __attribute__((weak));

static void censusPrint(void) {
    printf("[tev] census: %d distinct TEV keys over %u draws%s\n", gCensusN, gCensusDraws,
           gCensusOverflow ? " (TABLE FULL: more keys were dropped)" : "");
    for (int i = 0; i < gCensusN; ++i) {
        const CensusKey* k = &gCensus[i].key;
        printf("[tev] key %d: draws %u tris %u | stages %u gens %u ind %u | obj %d op %d\n", i, gCensus[i].draws, gCensus[i].tris,
               k->numStages, k->numGens, k->numInd, gCensus[i].tag >> 4, gCensus[i].tag & 15);
        printf("[tev]   reg");
        for (int r = 0; r < 4; ++r) printf(" %02x%02x%02x%02x", gCensus[i].reg[r].r, gCensus[i].reg[r].g, gCensus[i].reg[r].b, gCensus[i].reg[r].a);
        printf(" | k");
        for (int r = 0; r < 4; ++r) printf(" %02x%02x%02x%02x", gCensus[i].kreg[r].r, gCensus[i].kreg[r].g, gCensus[i].kreg[r].b, gCensus[i].kreg[r].a);
        printf(" | %s\n", gCensus[i].st);
        for (int m = 0; m < 8; ++m) if (gCensus[i].map0tex[m])
            printf("[tev]   map%d tex %p %dx%d fmt %d filt %d\n", m, gCensus[i].map0tex[m], gCensus[i].texInfo[m][0], gCensus[i].texInfo[m][1], gCensus[i].texInfo[m][2], gCensus[i].texInfo[m][3]);
        for (int s = 0; s < k->numStages && s < TEV_STAGES; ++s) {
            const TevStage* t = &k->st[s];
            printf("[tev]   s%d ord c%u m%u%s ch%u | C %u,%u,%u,%u op %u b%u s%u cl%u ->%u | A %u,%u,%u,%u op %u b%u s%u cl%u ->%u | sw %u/%u k %u/%u",
                   s, t->coord, t->map, t->dis ? "(DIS)" : "", t->color, t->cin[0], t->cin[1], t->cin[2], t->cin[3], t->cop[0], t->cop[1], t->cop[2], t->cop[3], t->cop[4],
                   t->ain[0], t->ain[1], t->ain[2], t->ain[3], t->aop[0], t->aop[1], t->aop[2], t->aop[3], t->aop[4],
                   t->rasSwap, t->texSwap, t->kcsel, t->kasel);
            if (t->ind[0]) printf(" | IND st%u f%u b%u m%u w%u/%u add%u a%u", t->ind[1], t->ind[2], t->ind[3], t->ind[4], t->ind[5], t->ind[6], t->ind[7], t->ind[9]);
            if (t->map < 8) printf(" | tex %p", gCensus[i].map0tex[t->map]);
            printf("\n");
        }
        for (int g = 0; g < k->numGens && g < TEX_GENS; ++g)
            printf("[tev]   g%d func %u src %u mtx %u norm %u post %u\n", g, k->gen[g].func, k->gen[g].src, k->gen[g].mtx, k->gen[g].norm, k->gen[g].post);
        for (int n = 0; n < k->numInd && n < IND_STAGES; ++n)
            printf("[tev]   ind%d coord %u map %u\n", n, k->indOrder[n][0], k->indOrder[n][1]);
    }
    gCensusN = 0; gCensusOverflow = 0; gCensusDraws = 0;
}
// Called by gx_draw for every draw it submits; texMaps = what each GX_TEXMAPn holds now.
void gx_tev_note_draw(unsigned tris, RhiTexture* const texMaps[8]) {
    int on = (&rhi_vita_journal && rhi_vita_journal) || (&rhi_vita_census_arm && rhi_vita_census_arm > 0);
    if (!on) { if (gCensusWasOn) { censusPrint(); gCensusWasOn = 0; } return; }
    gCensusWasOn = 1; ++gCensusDraws;
    CensusKey k; keyOf(&k);
    int i;
    for (i = 0; i < gCensusN; ++i) if (gCensus[i].tag == gStfxDbgObjTag && !memcmp(&gCensus[i].key, &k, sizeof k)) break;
    if (i == gCensusN) {
        if (gCensusN == CENSUS_MAX) { gCensusOverflow = 1; return; }
        gCensus[i].key = k; gCensus[i].draws = gCensus[i].tris = 0; gCensus[i].tag = gStfxDbgObjTag;
        memcpy(gCensus[i].reg, gx_tev_reg, sizeof gx_tev_reg); memcpy(gCensus[i].kreg, gx_tev_kreg, sizeof gx_tev_kreg);
        gCensus[i].st[0] = 0; if (&gx_draw_dbg_state) gx_draw_dbg_state(gCensus[i].st, (int)sizeof gCensus[i].st);
        for (int m = 0; m < 8; ++m) { memset(gCensus[i].texInfo[m], 0, sizeof gCensus[i].texInfo[m]); if (texMaps[m] && &rhi_vita_texInfo) rhi_vita_texInfo(texMaps[m], gCensus[i].texInfo[m]); }
        for (int m = 0; m < 8; ++m) gCensus[i].map0tex[m] = texMaps[m];   // first draw's textures
        ++gCensusN;
    }
    gCensus[i].draws++; gCensus[i].tris += tris;
}
