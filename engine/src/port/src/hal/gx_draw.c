// gx_draw.c - GX write-gather-pipe interpreter -> RHI.
//
// The game submits geometry the console way: GXBegin(prim, vtxfmt, n) states the
// primitive, then n vertices are written straight to the GXWGFifo write-gather pipe
// in the layout described by the current vertex descriptor (GXSetVtxDesc) and the
// vertex-attribute table for that format (GXSetVtxAttrFmt). We buffer the pipe
// bytes, and once GXBegin's n vertices have arrived we decode them per the VAT,
// transform by MVP = proj * posmtx, and draw through the RHI. This is the real draw
// path - the same entry points compiled game drawing TUs call.

#include "port/gx_draw.h"
#include "port/rt_timer.h"   /* Task 3 (rt_stats.txt); here because the matrix-load entry points below use RT_SCOPE / RT_SUBS */
#include "port/gx_hal_state.h"   /* R2: the HAL state struct (two instances: gx_hal, gx_hal_g); see the header and host/engine/R2_STATE.md */

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"   /* GX_TF_* for GXGetTexBufferSize (vita fork) */
#include "dolphin/gx/GXBump.h"   /* GXSetTevIndirect, for GXSetTevIndRepeat (2026-10-13) */
/* port/tex_decode.h is NOT included: its "#ifndef GX_TF_I4" guard tests a MACRO, but
 * GX_TF_I4 is an enum constant, so it redeclares every GX_TF_* next to GXEnum.h. */
uint8_t* gxTexDecode(int fmt, int w, int h, const uint8_t* src);
uint8_t* gxTexDecodeYUV(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h);

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
/* R2: the statics GX state setters write now live in gx_hal (port/gx_hal_state.h, defined in gx_tev.c). The draw path keeps reading the old names through these macros; the
   setter bodies (gxh_b_*) use ONLY their `h` parameter (host/engine/gxh_lint.py enforces it). */
#define gDesc        (gx_hal.desc)
#define gVat         (gx_hal.vat)
#define gProj        (gx_hal.proj)
#define gProjOrtho   (gx_hal.projOrtho)
#define gPosMtx      (gx_hal.posMtx)
#define gNrmMtx      (gx_hal.nrmMtx)
#define gTexMtx      (gx_hal.texMtx)
#define gPostMtx     (gx_hal.postMtx)
#define gCurMtx      (gx_hal.px.curMtx)
#define gPxActive    (gx_hal.px.pxActive)
#define gNumChans    (gx_hal.px.numChans)
#define gColorUpdate (gx_hal.px.colorUpdate)
#define gBlendType   (gx_hal.px.blendType)
#define gBlendSrc    (gx_hal.px.blendSrc)
#define gBlendDst    (gx_hal.px.blendDst)
#define gAcComp0     (gx_hal.px.acComp0)
#define gAcComp1     (gx_hal.px.acComp1)
#define gAcOp        (gx_hal.px.acOp)
#define gAcRef0      (gx_hal.px.acRef0)
#define gAcRef1      (gx_hal.px.acRef1)
#define gCullMode    (gx_hal.px.cullMode)
#define gZCompare    (gx_hal.px.zCompare)
#define gZFunc       (gx_hal.px.zFunc)
#define gZUpdate     (gx_hal.px.zUpdate)
#define gChan        (gx_hal.chan)
#define gMat         (gx_hal.mat)
#define gAmb         (gx_hal.amb)
#define gCopySrc     (gx_hal.copySrc)
#define gx_light_regs (gx_hal.lightRegs)
#define gx_tev_op    (gx_hal.tevOp)
static int gDbg = -1;
static int dbg(void){ if(gDbg<0) gDbg = getenv("GXDRAW_DEBUG")?1:0; return gDbg; }

// --- RHI wiring ------------------------------------------------------------
static RhiInstance*  gRhi;
static RhiSwapchain* gSc;
static RhiTexture*   gTex;

void gx_draw_setRhi(RhiInstance* rhi, RhiSwapchain* sc) { gRhi = rhi; gSc = sc; }
void gx_draw_setTexture(RhiTexture* tex) { gTex = tex; }
void gx_draw_setAlphaMode(int mode) { if (gRhi) rhi_setAlphaMode(gRhi, mode); }

// --- vertex format state ---------------------------------------------------
// (VatAttr, gDesc and gVat: port/gx_hal_state.h)

// --- transform state -------------------------------------------------------
#define GX_MTX_SLOTS GXH_MTX_SLOTS

// Indexed vertex arrays: INDEX8/INDEX16 attributes carry an index into these bases
// (big-endian element data, `stride` bytes apart), set by the game before a draw.
typedef struct { const unsigned char* base; int stride; } ArrayDef;
static ArrayDef gArray[GX_VA_MAX_ATTR];

static void mtxIdentity3x4(float m[3][4]) { memset(m, 0, sizeof(float)*12); m[0][0]=m[1][1]=m[2][2]=1.0f; }
static void mtxIdentity4x4(float m[4][4]) { memset(m, 0, sizeof(float)*16); m[0][0]=m[1][1]=m[2][2]=m[3][3]=1.0f; }

// MVP = proj(4x4) * posMtx(3x4 extended with [0,0,0,1]); row-major output.
// gStfxGxZToGL (vita fork, docs/47 run 32): GX clip space puts z in [-w, 0] (near -1, far 0);
// GL's is [-w, w]. z' = 2z + w maps GX near -> -1, far -> +1: row2' = 2*row2 + row3.
int gStfxGxZToGL;
// THE VIEWPORT'S DEPTH RANGE (2026-10-14, docs/47 "Aurora diff"): GX maps clip z to depth = far + (far - near) * z/w (z/w in
// [-1, 0]; Aurora GXTransform.cpp:145-148, zmin = near * 2^24-1, oz = zmax). The game uses a NEGATIVE near as its z bias:
// Camera_ApplyDecalViewport -0.05 (ground shadows, decals), ..TransparentViewport -0.01 (every transparent map block),
// ..EffectDepthViewport -0.075. Ignoring it put shadows/decals AT the floor's depth: z-fighting that moves with the camera.
// GL's glDepthRange cannot go below 0, so the range is folded into the clip z row: z' = (f - n) z + (f - 1) w gives
// z'/w = (f - 1) + (f - n) z/w, the GX depth less one -- the same affine map the default [0, 1] viewport gets (identity).
// Applied before gStfxGxZToGL. ux0:data/stairfax/vp_depth_off.txt starting with 1 = ignore the range again (A/B; read once).
static float gVpZn = 0.0f, gVpZf = 1.0f;
static int gVpDepthOff = -1;
static int vpDepthOff(void) {
#ifdef BREAK_24   /* host negative control: the range is dropped (the pre-2026-10-14 behaviour) */
    return 1;
#endif
    if (gVpDepthOff < 0) {
        FILE* f = fopen("ux0:data/stairfax/vp_depth_off.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gVpDepthOff = c == 49; printf("[viewport] depth range %s\n", gVpDepthOff ? "IGNORED (vp_depth_off.txt = the old behaviour)" : "folded into the projection (GX near/far, incl. the game's negative-near z bias)");
    }
    return gVpDepthOff;
}
static void gxClipZ(float m[16]) {   /* the projection's z row: the viewport's depth range, then the optional GL remap */
    if ((gVpZn != 0.0f || gVpZf != 1.0f) && !vpDepthOff()) {
        const float a = gVpZf - gVpZn, b = gVpZf - 1.0f;
        for (int j = 0; j < 4; ++j) m[2*4+j] = a * m[2*4+j] + b * m[3*4+j];
    }
    if (gStfxGxZToGL) for (int j = 0; j < 4; ++j) m[2*4+j] = 2.0f * m[2*4+j] + m[3*4+j];
}
void gx_draw_clipProj(float out[16]) {   /* host-test hook (gx_port_test check 24): the projection as every draw path hands it to the backend */
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i*4+j] = gProj[i][j];
    gxClipZ(out);
}
static void computeMVP(float out[16]) {
    float P[4][4];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) P[i][j] = gPosMtx[gCurMtx][i][j];
    P[3][0]=P[3][1]=P[3][2]=0.0f; P[3][3]=1.0f;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0.0f; for (int k = 0; k < 4; ++k) s += gProj[i][k]*P[k][j];
        out[i*4+j] = s;
    }
    gxClipZ(out);
}

/* 2026-10-20: bumped by EVERY writer of what dlkKeyNow reads (gDesc, gArray base/stride, gSrcLo/gSrcHi); dlkCall reuses the last key and
   its hash while it is unchanged (hub: key + lookup was 3.4 ms/frame for ~540 calls). BREAK_KEYGEN: GXSetArray forgets to bump. */
static unsigned gVtxGen = 1;
void gxh_post_apply(unsigned hook) { if (hook == GXH_HOOK_VTXGEN) ++gVtxGen; }   /* R2 (D4): R's apply of the vertex-descriptor group; gVtxGen stays an R-side static (GXSetArray, setSourceBounds and init bump it on R too) */
void gx_draw_init(void) {
    ++gVtxGen;
    memset(gDesc, 0, sizeof(gDesc));
    memset(gVat, 0, sizeof(gVat));
    memset(gArray, 0, sizeof(gArray));
    gCurMtx = 0; gTex = NULL;
    mtxIdentity4x4(gProj);
    for (int i = 0; i < GX_MTX_SLOTS; ++i) mtxIdentity3x4(gPosMtx[i]);
    gxh_kdirty_all(&gx_hal);   // R3a C3: a writer of gx_hal that is not a setter body names every kind
}

// --- GX vertex format API --------------------------------------------------
GXH_INLINE void gxh_b_GXClearVtxDesc(GxHal* h, const int mark) { memset(h->desc, 0, sizeof(h->desc)); if (!mark) ++gVtxGen; GXH_MARK(h, mark, GXH_G_DESC); }
GXH_PAIR(GXClearVtxDesc, (void))
/* GX_VA_NBT IS THE NORMAL (2026-10-14, docs/47; found by reading encounter/aurora lib/gx/shader.cpp, confirmed by the SDK's GXAttr.c):
 * one descriptor field, one VAT field, one array, one place in the vertex (after POS, before CLR0) -- the VAT count (GX_NRM_NBT)
 * is what makes it nine components. The game declares its bump-mapped models with GX_VA_NBT (objprint_dolphin.c:1312,
 * pi_videoinit.c:190); stored as a 26th attribute it was decoded LAST, after the texcoords, so every colour / texcoord index
 * after it was read one slot early and the normal never: gx_port_test check 23. BREAK_23 = the old separate attribute. */
#ifdef BREAK_23
#define GX_NBT_ALIAS(a) (a)
#else
#define GX_NBT_ALIAS(a) ((a) == GX_VA_NBT ? GX_VA_NRM : (a))
#endif
GXH_INLINE void gxh_b_GXSetVtxDesc(GxHal* h, const int mark, GXAttr attr, GXAttrType type) {
    attr = GX_NBT_ALIAS(attr);
    if ((unsigned)attr < GX_VA_MAX_ATTR) { h->desc[attr] = type; if (!mark) ++gVtxGen; GXH_MARK(h, mark, GXH_G_DESC); }
}
GXH_PAIR(GXSetVtxDesc, (GXAttr attr, GXAttrType type), attr, type)
GXH_INLINE void gxh_b_GXSetVtxAttrFmt(GxHal* h, const int mark, GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    attr = GX_NBT_ALIAS(attr);
    if ((unsigned)vtxfmt < GX_MAX_VTXFMT && (unsigned)attr < GX_VA_MAX_ATTR) {
        h->vat[vtxfmt][attr].cnt  = (u8)cnt;
        h->vat[vtxfmt][attr].type = (u8)type;
        h->vat[vtxfmt][attr].frac = frac;
        GXH_MARK(h, mark, GXH_G_VAT0 + (int)vtxfmt);
    }
}
GXH_PAIR(GXSetVtxAttrFmt, (GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac), vtxfmt, attr, cnt, type, frac)

void GXSetArray(GXAttr attr, void* base, u8 stride) {
    attr = GX_NBT_ALIAS(attr);
#ifndef BREAK_KEYGEN
    if ((unsigned)attr < GX_VA_MAX_ATTR) { gArray[attr].base=(const unsigned char*)base; gArray[attr].stride=stride; ++gVtxGen; }
#else
    if ((unsigned)attr < GX_VA_MAX_ATTR) { gArray[attr].base=(const unsigned char*)base; gArray[attr].stride=stride; }
#endif
}

// Optional source-buffer bounds for indexed fetches. The GX API carries no array
// length, so a display list with an out-of-range index would read past the source
// buffer (usually the map block). When a caller sets bounds, the decoder skips any
// indexed element that falls outside [lo,hi) instead of faulting on unmapped memory.
static const unsigned char* gSrcLo;
static const unsigned char* gSrcHi;
void gx_draw_setSourceBounds(const void* lo, const void* hi) {
    gSrcLo = (const unsigned char*)lo; gSrcHi = (const unsigned char*)hi; ++gVtxGen;
}

// Bind-pose skinning: a per-matrix-slot POS translation added to each vertex by its
// PNMTXIDX (byte/3 -> slot). Set by the model renderer; cleared (n=0) for normal draws.
#define GX_MTX_IDX_SLOTS 16
static float gJointOff[GX_MTX_IDX_SLOTS][3];
static int   gHaveJointOff;
void gx_draw_setJointOffsets(const float off[][3], int n) {
    gHaveJointOff = (n > 0);
    for (int i = 0; i < GX_MTX_IDX_SLOTS; ++i)
        for (int c = 0; c < 3; ++c) gJointOff[i][c] = (i < n && off) ? off[i][c] : 0.0f;
}

// Full per-matrix-slot skinning: a 3x4 model-space transform applied to POS before the
// model->world->view matrix, selected by PNMTXIDX/3. Takes precedence over the offset path.
// Set by the model renderer (identity slots = bind pose; posed slots = animation).
static float gJointMtx[GX_MTX_IDX_SLOTS][3][4];
static int   gHaveJointMtx;
void gx_draw_setJointMatrices(const float m[][3][4], int n) {
    gHaveJointMtx = (n > 0);
    for (int i = 0; i < GX_MTX_IDX_SLOTS; ++i)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                gJointMtx[i][r][c] = (i < n && m) ? m[i][r][c]
                                   : (r == c ? 1.0f : 0.0f);   // identity fallback
}

// Apply the joint transform for this vertex's matrix slot (PNMTXIDX/3): full 3x4 matrix if
// set, else the bind-pose translation offset, else nothing.
static void applyJointSkin(RhiTexVertex* v, int pnmtx) {
    int s = (pnmtx / 3) & (GX_MTX_IDX_SLOTS - 1);
    if (gHaveJointMtx) {
        const float (*M)[4] = gJointMtx[s];
        float x = v->x, y = v->y, z = v->z;
        v->x = M[0][0]*x + M[0][1]*y + M[0][2]*z + M[0][3];
        v->y = M[1][0]*x + M[1][1]*y + M[1][2]*z + M[1][3];
        v->z = M[2][0]*x + M[2][1]*y + M[2][2]*z + M[2][3];
    } else if (gHaveJointOff) {
        v->x += gJointOff[s][0]; v->y += gJointOff[s][1]; v->z += gJointOff[s][2];
    }
}

// Explicit POS-only layout override: when a skinned model's descriptor-derived layout
// fails to validate against its DL, the model renderer autodetects the true per-vertex
// stride and POS offset (as model_view does) and forces it here. In force mode decodePrim
// reads only the POS index (at posOff, posSz bytes) plus the leading PNMTXIDX for skinning,
// ignoring the rest of the vertex, and advances by the fixed stride. Cleared with stride<=0.
static int gForceStride, gForcePosOff, gForcePosSz;
void gx_draw_setForceLayout(int stride, int posOff, int posSz) {
    gForceStride = stride; gForcePosOff = posOff; gForcePosSz = posSz;
}

// Real GX skinning mode: each vertex is transformed by the GX position matrix its PNMTXIDX
// selects (gPosMtx[PNMTXIDX/3], loaded via GXLoadPosMtxImm), then projected by gProj alone -
// i.e. the retail model render path. Off by default (the interim renderModel keeps using the
// gJointOff/gJointMtx offset path + full MVP). The real model renderer sets this around a draw.
static int gRealSkin;
void gx_draw_setRealSkin(int on) { gRealSkin = on; }
// Transform one vertex to view space by its per-vertex matrix (real mode) or the interim skin.
static void skinVertex(RhiTexVertex* v, int pnmtx, int havePnmtx) {
    if (gRealSkin) {
        int slot = havePnmtx ? (pnmtx/3) : gCurMtx;
        if ((unsigned)slot >= GX_MTX_SLOTS) slot = 0;
        const float (*M)[4] = gPosMtx[slot];
        float x=v->x, y=v->y, z=v->z;
        v->x = M[0][0]*x + M[0][1]*y + M[0][2]*z + M[0][3];
        v->y = M[1][0]*x + M[1][1]*y + M[1][2]*z + M[1][3];
        v->z = M[2][0]*x + M[2][1]*y + M[2][2]*z + M[2][3];
    } else {
        applyJointSkin(v, pnmtx);
    }
}

// --- GX transform API ------------------------------------------------------
// Journal (the Vita backend's per-frame draw journal, rhi_vita.c): log the transform state the
// game hands GX, to compare against the retail game's values (host/engine/stfx_oracle.py).
extern int rhi_vita_journal __attribute__((weak));
static int gxj(void) { return &rhi_vita_journal && rhi_vita_journal; }
static int gJournalLines;
// (gProjOrtho: the current projection's type (docs/47 run 116): perspective fog needs GX_PERSPECTIVE -- now gx_hal.projOrtho)
GXH_INLINE void gxh_b_GXSetProjection(GxHal* h, const int mark, const f32 mtx[4][4], GXProjectionType type) {
    RT_COUNT(RT_S_MTX);   /* Task 3: count only */
    memcpy(h->proj, mtx, sizeof(float)*16);
    h->projOrtho = type == GX_ORTHOGRAPHIC;
    GXH_MARK(h, mark, GXH_G_PROJ);
    if (gxj() && gJournalLines++ < 40)
        printf("[gx] proj %s  %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n",
               type == GX_ORTHOGRAPHIC ? "ORTHO" : "PERSP",
               mtx[0][0],mtx[0][1],mtx[0][2],mtx[0][3], mtx[1][0],mtx[1][1],mtx[1][2],mtx[1][3],
               mtx[2][0],mtx[2][1],mtx[2][2],mtx[2][3], mtx[3][0],mtx[3][1],mtx[3][2],mtx[3][3]);
}
GXH_PAIR(GXSetProjection, (const f32 mtx[4][4], GXProjectionType type), mtx, type)
GXH_INLINE void gxh_b_GXLoadPosMtxImm(GxHal* h, const int mark, const f32 mtx[3][4], u32 id) {
    RT_COUNT(RT_S_MTX);   /* Task 3: count only */
    u32 s = id/3;
    if (s >= GX_MTX_SLOTS) return;
    memcpy(h->posMtx[s], mtx, sizeof(float)*12);
    GXH_MARK(h, mark, GXH_G_POS0 + (int)s);
    if (gxj() && gJournalLines++ < 40)
        printf("[gx] posmtx slot %u  %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n", s,
               mtx[0][0],mtx[0][1],mtx[0][2],mtx[0][3], mtx[1][0],mtx[1][1],mtx[1][2],mtx[1][3],
               mtx[2][0],mtx[2][1],mtx[2][2],mtx[2][3]);
    // Safety on the skinned path: a NaN/inf or wildly out-of-range pos matrix (the not-yet-correct
    // vertex-group / extra-joint blend can still produce one) would fling its verts across the
    // screen. Push such a matrix's verts behind the far plane so they clip instead of spiking.
    if (gRealSkin) {
        float* M = &h->posMtx[s][0][0]; int bad = 0;
        for (int k=0;k<12;++k){ float a = M[k]<0?-M[k]:M[k]; if (a>1e6f || a!=a){ bad=1; break; } }
        // The skinned character is drawn under the follow-camera, which holds it ~120u away, so every
        // legit player pos matrix has a small view-space translation (well under a few hundred u). A
        // pos matrix whose bone sits ~2000u out is an extra vertex-group joint the not-yet-ported
        // blend path left at the WORLD ORIGIN (view-space ~(-1600,1900,-1760)); its verts draw a
        // degenerate triangle fan to that point - the diagonal spike. Clip those to the far plane.
#ifndef __vita__   /* the Vita runs the GAME's renderer, real mode always on: a distant object's pos
                     matrix legitimately sits > 900 u out, and this heuristic would cull it */
        if (!bad) { float tx=h->posMtx[s][0][3], ty=h->posMtx[s][1][3], tz=h->posMtx[s][2][3];
            if (tx*tx + ty*ty + tz*tz > 900.0f*900.0f) bad = 1; }
#endif
        if (bad) { for (int k=0;k<12;++k) M[k]=0; h->posMtx[s][2][3]=1e9f; }
    }
}
GXH_PAIR(GXLoadPosMtxImm, (const f32 mtx[3][4], u32 id), mtx, id)
// Normal matrices (vita fork, docs/47 run 58): recorded per slot for per-vertex GX lighting.
GXH_INLINE void gxh_b_GXLoadNrmMtxImm(GxHal* h, const int mark, const f32 mtx[3][4], u32 id) {
    RT_COUNT(RT_S_MTX);   /* Task 3: count only */
    u32 s = id / 3;
    if (s < GX_MTX_SLOTS) { GXH_MARK(h, mark, GXH_G_NRM0 + (int)s); memcpy(h->nrmMtx[s], mtx, sizeof(float) * 12); }
}
GXH_PAIR(GXLoadNrmMtxImm, (const f32 mtx[3][4], u32 id), mtx, id)
// Texture matrices (2026-09-28, the TEV path): GX_TEXMTX0..9 (ids 30..57) and the dual-texgen POST
// matrices GX_PTTEXMTX0..19 (ids 64..121). A 2x4 load fills two rows; its q row is (0,0,0,1).
GXH_INLINE void gxh_b_GXLoadTexMtxImm(GxHal* h, const int mark, const f32 mtx[][4], u32 id, GXTexMtxType type) {
    RT_COUNT(RT_S_MTX);   /* Task 3: count only */
    float (*d)[4] = NULL;
    int grp = 0;
    if (id >= 64 && id < 64 + 3 * 20) { d = h->postMtx[(id - 64) / 3]; grp = GXH_G_POSTM0 + (int)((id - 64) / 3); }
    else if (id >= 30 && id < 60) { d = h->texMtx[(id - 30) / 3]; grp = GXH_G_TEXM0 + (int)((id - 30) / 3); }
    if (!d) return;
    int rows = type == GX_MTX2x4 ? 2 : 3;
    for (int r = 0; r < rows; ++r) for (int c = 0; c < 4; ++c) d[r][c] = mtx[r][c];
    if (rows == 2) { d[2][0] = d[2][1] = d[2][2] = 0.0f; d[2][3] = 1.0f; }
    if (grp < GXH_G_TEXM0 || grp >= GXH_G_POSTM0 + GXH_NPOSTMTX) __builtin_unreachable();   /* tells gxh_kind_of_group the range: the kind is then one compare, not the whole chain */
    GXH_MARK(h, mark, grp);
}
GXH_PAIR(GXLoadTexMtxImm, (const f32 mtx[][4], u32 id, GXTexMtxType type), mtx, id, type)
GXH_INLINE void gxh_b_GXSetCurrentMtx(GxHal* h, const int mark, u32 id) { u32 s = id/3; if (s < GX_MTX_SLOTS) { h->px.curMtx = (int)s; GXH_MARK(h, mark, GXH_G_PIX); } }
GXH_PAIR(GXSetCurrentMtx, (u32 id), id)

// --- component sizing ------------------------------------------------------
static int compTypeSize(u8 t) { // GX_U8/S8=1, U16/S16=2, F32=4
    switch (t) { case GX_U8: case GX_S8: return 1; case GX_U16: case GX_S16: return 2; case GX_F32: return 4; }
    return 0;
}
static int colorSize(u8 t) { // GXCompType color enums
    switch (t) { case GX_RGB565: case GX_RGBA4: return 2; case GX_RGB8: case GX_RGBA6: return 3;
                 case GX_RGBX8: case GX_RGBA8: return 4; }
    return 0;
}
// Bytes one attribute occupies in the FIFO for the current descriptor/VAT.
static int attrSize(GXVtxFmt fmt, GXAttr a) {
    GXAttrType d = gDesc[a];
    if (d == GX_NONE) return 0;
    if (d == GX_INDEX8)  return 1;
    if (d == GX_INDEX16) return 2;
    // GX_DIRECT:
    VatAttr* v = &gVat[fmt][a];
    if (a <= GX_VA_TEX7MTXIDX) return 1;                 // matrix index attrs: 1 byte
    if (a == GX_VA_POS) return compTypeSize(v->type) * (v->cnt == GX_POS_XYZ ? 3 : 2);
    if (a == GX_VA_NRM) return compTypeSize(v->type) * (v->cnt == GX_NRM_XYZ ? 3 : 9);
    if (a == GX_VA_CLR0 || a == GX_VA_CLR1) return colorSize(v->type);
    if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) return compTypeSize(v->type) * (v->cnt == GX_TEX_ST ? 2 : 1);
    return 0;
}

// --- FIFO buffer -----------------------------------------------------------
#define GX_FIFO_CAP (1 << 20)
static unsigned char gFifo[GX_FIFO_CAP];
static int  gFifoLen;
static int  gCollecting;
static int  gExpectBytes;
static int  gPrimN;
static GXVtxFmt gPrimFmt;
static u8   gPrim;

static void fifoMaybeFlush(void); // draws once GXBegin's vertex count has arrived
static void fifoReset(void) { gFifoLen = 0; }
// Display-list RECORDING (vita fork): between GXBeginDisplayList and GXEndDisplayList
// the pipe writes go into the caller's buffer in the console's own list format -- the
// same format GXCallDisplayList above parses -- instead of being drawn.
static unsigned char* gList; static u32 gListCap, gListLen; static int gListOverflow;
unsigned gx_list_records, gx_list_overflows;
static void listByte(unsigned char b) { if (gListLen < gListCap) gList[gListLen++] = b; else gListOverflow = 1; }
static void fifoByte(unsigned char b) {
    if (gList) { listByte(b); return; }
    if (gFifoLen < GX_FIFO_CAP) gFifo[gFifoLen++] = b;
}

void gxfifo_push_u8 (unsigned char v)  { fifoByte(v); fifoMaybeFlush(); }
// R-VB (host/engine/R_VB.md): the render thread's replay of an immediate primitive's vertex bytes, in bulk.  Semantically `for i: gxfifo_push_u8(b[i])`:
// the same gFifo / gFifoLen, the same list bytes, the same decodeAndDraw calls at the same byte positions.  gx_vb_bulk (default 0) picks bulk vs per-byte in the generated VBYTES replay.
int gx_vb_bulk; unsigned gx_vb_bulk_calls;
void gxfifo_push_bytes(const unsigned char* b, unsigned n) {
    ++gx_vb_bulk_calls;
    while (n) {
        if (gList) { fifoByte(*b++); --n; fifoMaybeFlush(); continue; }   // recording a display list: the per-byte path, exactly
        unsigned take = n;
        if (gCollecting) {   // up to the byte that completes the primitive (that byte is drawn at, as fifoMaybeFlush does)
            int need = gExpectBytes - gFifoLen;
            if (need <= 0) take = 1; else if ((unsigned)need < take) take = (unsigned)need;
        }
        if (gFifoLen < GX_FIFO_CAP) {   // fifoByte: bytes past the cap are dropped but still consumed
            unsigned room = (unsigned)(GX_FIFO_CAP - gFifoLen), c = take < room ? take : room;
            memcpy(gFifo + gFifoLen, b, c); gFifoLen += (int)c;
        }
        b += take; n -= take;
        fifoMaybeFlush();
    }
}
void gxfifo_push_u16(unsigned short v) { fifoByte((unsigned char)(v>>8)); fifoByte((unsigned char)v); fifoMaybeFlush(); }
void gxfifo_push_u32(unsigned int v)   { fifoByte((unsigned char)(v>>24)); fifoByte((unsigned char)(v>>16));
                                         fifoByte((unsigned char)(v>>8));  fifoByte((unsigned char)v); fifoMaybeFlush(); }
void gxfifo_push_f32(float f)          { unsigned int u; memcpy(&u, &f, 4); gxfifo_push_u32(u); }
// signed forms (vita fork): the pipe takes bits; a signed store writes the same bytes
void gxfifo_push_s8 (signed char v)    { gxfifo_push_u8((unsigned char)v); }
void gxfifo_push_s16(short v)          { gxfifo_push_u16((unsigned short)v); }
void gxfifo_push_s32(int v)            { gxfifo_push_u32((unsigned int)v); }

// --- big-endian readers over an arbitrary byte source ----------------------
static unsigned rdU(const unsigned char* d, int* c, int n) { unsigned x=0; for(int i=0;i<n;++i) x=(x<<8)|d[(*c)++]; return x; }
static int      rdS(const unsigned char* d, int* c, int n) { unsigned x=rdU(d,c,n); unsigned s=1u<<(n*8-1); return (int)((x^s)-s); }
static float    rdF(const unsigned char* d, int* c) { unsigned u=rdU(d,c,4); float f; memcpy(&f,&u,4); return f; }

// 2^-frac, exact: x * 2^-k equals x / 2^k bit for bit, and a VFP multiply is far cheaper than a
// divide (docs/47 run 65: display-list decode was ~200 of the frame's 330 ms).
static const float kInvPow2[32] = {
    1.0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f,
    0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f,
    0x1p-21f, 0x1p-22f, 0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f };
static float compVal(const unsigned char* d, int* c, u8 type, u8 frac) { // one POS/TEX scalar
    const float k = kInvPow2[frac & 31];
    switch (type) {
        case GX_U8:  return (float)rdU(d,c,1) * k;
        case GX_S8:  return (float)rdS(d,c,1) * k;
        case GX_U16: return (float)rdU(d,c,2) * k;
        case GX_S16: return (float)rdS(d,c,2) * k;
        case GX_F32: return rdF(d,c);
    }
    return 0.0f;
}
static u32 readColorAt(const unsigned char* d, int* c, u8 type) { // -> r|g<<8|b<<16|a<<24
    u8 r=255,g=255,b=255,a=255;
    switch (type) {
        case GX_RGB565: { unsigned v=rdU(d,c,2); r=(u8)((v>>11)<<3); g=(u8)(((v>>5)&0x3f)<<2); b=(u8)((v&0x1f)<<3); } break;
        case GX_RGB8:   { r=(u8)rdU(d,c,1); g=(u8)rdU(d,c,1); b=(u8)rdU(d,c,1); } break;
        case GX_RGBX8:  { r=(u8)rdU(d,c,1); g=(u8)rdU(d,c,1); b=(u8)rdU(d,c,1); (void)rdU(d,c,1); } break;
        case GX_RGBA4:  { unsigned v=rdU(d,c,2); r=(u8)(((v>>12)&0xf)*17); g=(u8)(((v>>8)&0xf)*17); b=(u8)(((v>>4)&0xf)*17); a=(u8)((v&0xf)*17); } break;
        case GX_RGBA6:  { unsigned v=rdU(d,c,3); r=(u8)(((v>>18)&0x3f)<<2); g=(u8)(((v>>12)&0x3f)<<2); b=(u8)(((v>>6)&0x3f)<<2); a=(u8)((v&0x3f)<<2); } break;
        case GX_RGBA8:  { r=(u8)rdU(d,c,1); g=(u8)rdU(d,c,1); b=(u8)rdU(d,c,1); a=(u8)rdU(d,c,1); } break;
    }
    return (u32)r | ((u32)g<<8) | ((u32)b<<16) | ((u32)a<<24);
}

// Read attribute `a` from `src` (cursor `c`) into `out`; used for both DIRECT
// (src = the vertex stream) and INDEX (src = the array element).
static float gCurNrm[3]; static int gCurHaveNrm;   // the vertex being decoded (lighting, run 58)
static float gCurTex[8][2];                         // ... its raw TEX0..7 (the TEV path's texgen inputs)
static float gCurNbt[6]; static int gCurHaveNbt;    // ... its binormal + tangent: the 2nd and 3rd vectors of a 9-component (GX_NRM_NBT) normal (2026-10-13)
static unsigned char gCurTmi[8];                    // ... its TEX0MTXIDX..TEX7MTXIDX bytes: the XF matrix row each texgen reads instead of the one it names
static void readAttrValue(GXAttr a, VatAttr* v, const unsigned char* src, int* c,
                          RhiTexVertex* out, int* haveTex) {
    if (a == GX_VA_POS) {
        out->x = compVal(src,c,v->type,v->frac);
        out->y = compVal(src,c,v->type,v->frac);
        if (v->cnt == GX_POS_XYZ) out->z = compVal(src,c,v->type,v->frac);
    } else if (a == GX_VA_NRM) {
        int cc = (v->cnt == GX_NRM_XYZ) ? 3 : 9;
        u8 fr = (v->type == GX_S8) ? 6 : (v->type == GX_S16) ? 14 : 0;
        for (int i=0;i<cc;++i) { float n = compVal(src,c,v->type,fr); if (i < 3) gCurNrm[i] = n; else gCurNbt[i - 3] = n; }
        gCurHaveNrm = 1; if (cc == 9) gCurHaveNbt = 1;
    } else if (a == GX_VA_CLR0) {
        out->rgba = readColorAt(src,c,v->type);
    } else if (a == GX_VA_CLR1) {
        readColorAt(src,c,v->type);
    } else if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) {
        float s = compVal(src,c,v->type,v->frac);
        float t = (v->cnt == GX_TEX_ST) ? compVal(src,c,v->type,v->frac) : 0.0f;
        gCurTex[a - GX_VA_TEX0][0] = s; gCurTex[a - GX_VA_TEX0][1] = t;
        if (a == GX_VA_TEX0) { out->u=s; out->v=t; *haveTex=1; }
    }
}

// --- decode + draw ---------------------------------------------------------
#define GX_MAX_BATCH 16384
static RhiTexVertex gVerts[GX_MAX_BATCH];
static float gVertN[GX_MAX_BATCH][3]; static unsigned char gVertHaveN[GX_MAX_BATCH];
// Per-vertex validity: a vertex whose POS came from an out-of-bounds indexed fetch is
// left at the origin (garbage), which would stretch any triangle using it into a "spike".
// We mark such vertices invalid and drop every triangle that references one.
static unsigned char gVertOk[GX_MAX_BATCH];
// The TEV path's texgen inputs, per vertex: object-space position (before the pos matrix), raw
// normal, raw TEX0..7. Filled only while the TEV path is live (gTevLive), so the old path pays nothing.
static float gVObj[GX_MAX_BATCH][3], gVNrm0[GX_MAX_BATCH][3], gVTex[GX_MAX_BATCH][8][2];
static int gTevLive;
// 2026-10-13: the per-vertex binormal+tangent and TEXnMTXIDX bytes the texgens may read. Allocated on first use (a draw that has neither
// never pays the 0.5 MB); an allocation failure leaves them NULL and the texgens that need them read as unsupported, counted.
static float (*gVNbt)[6]; static unsigned char (*gVTmi)[8]; static int gAuxTried;
static int gTmiLive;   /* decodePrim: this primitive's gVTmi rows are valid (drawTev reads it) */
static int auxEnsure(void) {
    if (!gAuxTried) { gAuxTried = 1; gVNbt = (float (*)[6])calloc(GX_MAX_BATCH, sizeof *gVNbt); gVTmi = (unsigned char (*)[8])calloc(GX_MAX_BATCH, sizeof *gVTmi);
                      if (!gVNbt || !gVTmi) { free(gVNbt); free(gVTmi); gVNbt = NULL; gVTmi = NULL; } }
    return gVNbt != NULL;
}
#ifdef BREAK_18   /* host negative control: a vertex with no NBT normal reads (0,0,0) instead of Dolphin's (0,0,1) */
static const float kNbtDefault[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
#else
static const float kNbtDefault[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
#endif   /* a vertex with no NBT normal: Dolphin's coord default (0,0,1) for a BINRM / TANGENT source */

// Emit a triangle only if all three source vertices decoded cleanly.
static inline int emitTri(RhiTexVertex* out, int n, int cap, int a, int b, int c) {
    if (n+3 > cap) return n;
    if (!gVertOk[a] || !gVertOk[b] || !gVertOk[c]) return n;   // skip spike triangles
    out[n++]=gVerts[a]; out[n++]=gVerts[b]; out[n++]=gVerts[c];
    return n;
}

static int expandTriangles(RhiTexVertex* out, int cap, int nverts, u8 prim) {
    int n = 0;
    if (prim == GX_TRIANGLES) {
        for (int i = 0; i+3 <= nverts; i += 3) n = emitTri(out,n,cap,i,i+1,i+2);
    } else if (prim == GX_TRIANGLESTRIP) {
        for (int i = 2; i < nverts; ++i)
            n = (i&1) ? emitTri(out,n,cap,i-1,i-2,i) : emitTri(out,n,cap,i-2,i-1,i);
    } else if (prim == GX_TRIANGLEFAN) {
        for (int i = 2; i < nverts; ++i) n = emitTri(out,n,cap,0,i-1,i);
    } else if (prim == GX_QUADS) {
        for (int i = 0; i+4 <= nverts; i += 4) {
            n = emitTri(out,n,cap,i,i+1,i+2);
            n = emitTri(out,n,cap,i,i+2,i+3);
        }
    }
    return n;
}

// Decode nverts (per gDesc + VAT[fmt]) from `src`, expand `prim` to triangles, draw.
// Returns bytes consumed from `src`.
extern unsigned gx_px_skipped_noupdate;  // pixel state v1, below (gPxActive, gColorUpdate: gx_hal.px)
typedef GxHalLight PxLightRec;   /* (the struct's light register type: port/gx_hal_state.h) */
static u32 pxChannelColour(u32 vtx, int haveVtx, const RhiTexVertex* pos, const float* nrm, int haveN);
static void pxJournalLit(u32 v0in, int haveClr, u32 v0out, int haveN);
static int pxAlphaMode(void);
extern void rhi_vita_setDepthState(int test, int func, int write) __attribute__((weak));
static RhiTexture* gTexMap[8];   // (defined with the texture objects below)
void gx_tev_note_draw(unsigned tris, RhiTexture* const texMaps[8]);   // hal/gx_tev.c: TEV-key census
int gx_tev_tex0_stage_map(void); int gx_tev_uses_indirect(void); int gx_tev_num_stages(void); int gx_tev_stage_map(int s);
static unsigned gMovieMaps;   // GX_TEXMAPn holding a movie frame (bit n): see yuvIsMovieY
static unsigned gCopyMaps;    // GX_TEXMAPn holding an EFB-copy destination (bit n): see GXCopyTex
static unsigned gFilledCopyMaps;   // GX texture maps bound to an EFB copy the backend really made (mode 1)
unsigned gx_copy_draws_tevless;    // draws dropped: they read such a copy and the TEV path refused them
unsigned gx_movie_loads, gx_movie_draws; int gx_movie_last_map = -1;   // run 84: is the laptop reaching the movie?
static int inCopyDst(const void* p);
static RhiTexture* copyDstTex(const void* p);   // the backend copy texture of the destination holding p, or NULL
static void copyDstWanted(const void* p);       // the destination holding p was bound as a texture this present
static void copyDstRelease(const void* p, u32 n);
static const void* gMapImg[8]; static u16 gMapW[8], gMapH[8]; static u8 gMapFmt[8];   // journal: what each map holds
static void journalCopyHit(int k);
static int gCopyDstN;   // EFB-copy destinations recorded (GXCopyTex)
extern unsigned gx_copy_draws_skipped; unsigned gx_copy_draws_alt; unsigned gx_copy_draws_screenfx;
// gStfxTexArm (vita fork, docs/47 run 51), an A/B switch for the single-texture path:
//   0  sample GX_TEXMAP0 (the old behaviour; upstream's viewers stay here)
//   1  sample the map of the first TEV stage whose texcoord is TEX0 (gx_tev_tex0_stage_map)
//   2  as 1, and SKIP draws with an indirect stage (the fur shells need noise alpha to be right)
int gStfxTexArm;
// gStfxPerfArm (docs/47 run 64): where do ~260 ms/frame go? 0 = normal, 1 = decode + light every
// display list but submit nothing to the GPU, 2 = skip display lists entirely (game logic + the rest).
int gStfxPerfArm;
/* ARM 5 -- THE NULL-DRAW A/B (docs/53 T0-c, 2026-10-18). gStfxAb is the perf_ab.txt arm (projects/stairfax-vita/src/vita_tool_main.c;
 * weak, so a viewer build that does not link it reads 0 = off). Arm 5 = "the frame with every GX DRAW and every texture/EFB-copy
 * operation turned into a no-op, setters still running": wall(arm 5) is what the game thread G would cost if the whole render side R
 * were free, and wall(arm 0) - wall(arm 5) (+ the [win] "endFrame + present" column, which stays on G here) is R. EVERY early return
 * for arm 5, all in this file, all AFTER the counter bumps that perf_ab.py's scene check and the [win]/[rt] lines read:
 *   1. gxCallDisplayListImpl  after ++gStfxGxDlCalls / gStfxGxDlBytes          (no list is decoded, cached or drawn)
 *   2. decodeAndDrawImpl      after ++gx_imm_prims / gx_imm_verts              (no immediate primitive is decoded or drawn)
 *   3. gxLoadTexObjImpl       after ++gx_tex_loads                             (no cache lookup, decode, upload or bind; the
 *                              wrapper GXLoadTexObj has ALREADY run os_ext_poll(): that is a baton operation, not drawing)
 *   4. gxCopyTexImpl          after ++gx_copytex_calls                         (no EFB copy, no copy-destination bookkeeping)
 * The early return sits inside the *Impl bodies, not in the wrappers, so RT_SUBS/RT_ENTER/RT_LEAVE and the gx_w_*_us timers stay balanced.
 * Immediate mode needs nothing consumed: decodeAndDraw is entered with gCollecting already 0 and gFifoLen is re-zeroed by the next
 * GXBegin's fifoReset() (only GXBegin sets gCollecting), so skipping decodePrim leaves no half-open state; display-list recording
 * (gList) never reaches any of the four. SIDE EFFECTS OF ARM 5, so the settle windows matter: the per-list cache and the texture
 * cache are not fed while it holds (the first arm-0 window after it re-decodes/re-uploads), and a movie plane (yuvTakeEx) is not taken. */
extern int gStfxAb __attribute__((weak));
#define STFX_ARM5() (&gStfxAb && gStfxAb == 5)
unsigned gx_ind_lists_skipped;   /* run 243: lists not drawn under ind_skip.txt */
int gx_draw_static_hint;   /* set by the map-block renderer around its GXCallDisplayList (tex_dolphin.c) */
int gStfxDecodeArm = 1;   /* run 65: 1 = attribute plan (default), 0 = the old per-vertex slot walk */
static int pxNeedNormals(void);
static int gLastDecoded;   /* host-test hook: vertices the last primitive decoded */
const RhiTexVertex* gx_draw_lastVerts(int* n) { if (n) *n = gLastDecoded; return gVerts; }   /* a lit channel is active: only then are normals transformed */
// SAMPLED phase timers (2026-09-27): where the per-frame CPU goes inside GX drawing. Timing every
// primitive (~5,000/frame, 3 clock reads each) would itself cost ~15 ms, so 1 primitive in 8 is timed
// and the sums are scaled by 8 when read (vt_game.c prints them). Phases: decode (attribute fetch +
// transform), light (per-vertex GX lighting), submit (triangle expansion + RHI append).
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#define GXT_NOW() sceKernelGetProcessTimeWide()
#else
#define GXT_NOW() 0ull
#endif
/* GXAOT_HOOK_BEGIN C3 */
/* ---- GXAOT release track C3, arm 106/107/109 (patch 003) ------------------------------------------------------------------------------
 * The always-on timers below read the clock through GXT_NOW(). gx_c3_toff 1 (set by P's c3_frame_edge_h from the perf_ab arm) makes every read return 0 without the
 * syscall (sceKernelGetProcessTimeWide: 673 ns a read, run 277 "x 673 ns/read"), so each timed span adds 0 - 0. gx_c3_clk counts the reads that really happened:
 * the [c3] line's "gxclk reads/fr" proves the instrument was on (control) or off (arm 106). Host builds (GXT_NOW 0ull) keep the two variables for the link. */
unsigned gx_c3_clk; int gx_c3_toff;
#if defined(__vita__)
static inline unsigned long long gxtNow(void) { if (gx_c3_toff) return 0ull; ++gx_c3_clk; return sceKernelGetProcessTimeWide(); }
#undef GXT_NOW
#define GXT_NOW() gxtNow()
#endif
/* GXAOT_HOOK_END C3 */
#include "port/rt_timer.h"   /* 2026-10-12 Task 3: nesting-aware frame accounting; rt_stats.txt 1 turns it on (vita_tool_main.c reads the marker) */
RtState gRt;
unsigned long long gx_t_decode, gx_t_light, gx_t_submit;   // microseconds, sampled 1 in 8
extern unsigned long long gx_w_dl_us, gx_w_imm_us, gx_w_tex_us, gx_w_copy_us;   // exact, per entry point (defined at GXCallDisplayList)
extern unsigned gStfxGxDlCalls;
unsigned gx_prims, gx_prim_verts;
// The largest valid index + 1 for an indexed attribute's array (0 = no array: never in bounds;
// ~0u = no bound). gSrcLo/gSrcHi bound the POS vertex buffer (the spike guard); an array outside
// that window (NRM/CLR/TEX live in separate buffers) is not bounded by it -- dropping those left
// every skinned vertex with no UV, i.e. untextured.
static unsigned idxLimit(const ArrayDef* ar) {
    if (!ar->base || !ar->stride) return 0;
    if (!gSrcLo || !(ar->base >= gSrcLo && ar->base < gSrcHi)) return ~0u;
    return (unsigned)(gSrcHi - ar->base) / (unsigned)ar->stride;
}
// ---- the FAST attribute decoder (2026-09-28) -------------------------------------------------
// decodePrim's reference path reads every scalar through compVal (a switch per SCALAR) over rdU
// (a byte loop), and dispatches each attribute through readAttrValue's if-chain. Here each active
// attribute is resolved ONCE per primitive into an op (where it comes from, its component type and
// count, its scale), and a vertex is one switch per attribute. The reference path stays as
// gStfxDecodeArm 0; host/tests/gx_port_test.c check 9 fuzzes both arms to byte-identical vertices.
enum { OP_MTX, OP_POS, OP_NRM, OP_CLR0, OP_CLR1, OP_TEX0, OP_TEXN, OP_GENERIC };
typedef struct {
    const unsigned char* base; unsigned lim; int stride; VatAttr* vat;
    u8 attr, kind, idxBytes /* 0 = direct */, type, n /* components */;
    float k;
} DecOp;
static inline unsigned be16u(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
static inline float scal(const unsigned char* p, u8 type, float k) {
    switch (type) {
    case GX_U8:  return (float)p[0] * k;
    case GX_S8:  return (float)(signed char)p[0] * k;
    case GX_U16: return (float)be16u(p) * k;
    case GX_S16: return (float)(short)be16u(p) * k;
    default: { unsigned u = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; float f; memcpy(&f, &u, 4); return f; }
    }
}
static int buildOps(DecOp* ops, const GXAttr* plan, int planN, GXVtxFmt fmt, const unsigned* lim) {
    int n = 0;
    for (int pi = 0; pi < planN; ++pi) {
        GXAttr a = plan[pi]; GXAttrType d = gDesc[a];
        if (d == GX_NONE) continue;
        DecOp* o = &ops[n++];
        VatAttr* v = &gVat[fmt][a];
        o->vat = v; o->attr = (u8)a; o->base = gArray[a].base; o->stride = gArray[a].stride; o->lim = lim[a];
        o->idxBytes = d == GX_INDEX8 ? 1 : d == GX_INDEX16 ? 2 : 0;
        o->type = v->type; o->k = kInvPow2[v->frac & 31];
        if (a <= GX_VA_TEX7MTXIDX) { o->kind = OP_MTX; o->idxBytes = 0; continue; }   /* always 1 direct byte */
        int ts = compTypeSize(v->type);
        if (a == GX_VA_POS) { o->kind = OP_POS; o->n = v->cnt == GX_POS_XYZ ? 3 : 2; }
        else if (a == GX_VA_NRM) { o->kind = OP_NRM; o->n = v->cnt == GX_NRM_XYZ ? 3 : 9;
                                   o->k = kInvPow2[v->type == GX_S8 ? 6 : v->type == GX_S16 ? 14 : 0]; }
        else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) { o->kind = a == GX_VA_CLR0 ? OP_CLR0 : OP_CLR1; ts = 1; }
        else if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) { o->kind = a == GX_VA_TEX0 ? OP_TEX0 : OP_TEXN; o->n = v->cnt == GX_TEX_ST ? 2 : 1; }
        else { o->kind = OP_GENERIC; continue; }
        if (ts == 0) o->kind = OP_GENERIC;   /* an unknown component type: the reference reader decides */
    }
    return n;
}
// One vertex's attributes. Returns 0 if the POSITION index was out of bounds (vertex dropped).
static inline int decodeOps(const DecOp* ops, int nops, const unsigned char* src, int* c,
                            RhiTexVertex* out, int* haveTex, int* pnmtx) {
    int posOk = 1;
    for (int i = 0; i < nops; ++i) {
        const DecOp* o = &ops[i];
        if (o->kind == OP_MTX) { unsigned mb = src[(*c)++]; if (o->attr == GX_VA_PNMTXIDX) *pnmtx = (int)mb; else gCurTmi[o->attr - GX_VA_TEX0MTXIDX] = (unsigned char)mb; continue; }
        const unsigned char* p; int dc = 0; int* cur;
        if (o->idxBytes) {
            unsigned idx = o->idxBytes == 1 ? src[*c] : be16u(src + *c);
            *c += o->idxBytes;
            if (!(idx < o->lim)) { if (o->kind == OP_POS) posOk = 0; continue; }
            p = o->base + idx * (unsigned)o->stride; cur = &dc;
        } else { p = src; cur = c; }
        if (o->kind == OP_GENERIC) { readAttrValue((GXAttr)o->attr, o->vat, p, cur, out, haveTex); continue; }
        const unsigned char* q = p + *cur;
        const int ts = compTypeSize(o->type);
        switch (o->kind) {
        case OP_POS:
            out->x = scal(q, o->type, o->k); out->y = scal(q + ts, o->type, o->k);
            if (o->n == 3) out->z = scal(q + 2 * ts, o->type, o->k);
            *cur += ts * o->n; break;
        case OP_NRM:
            gCurNrm[0] = scal(q, o->type, o->k); gCurNrm[1] = scal(q + ts, o->type, o->k); gCurNrm[2] = scal(q + 2 * ts, o->type, o->k);
            if (o->n == 9) { for (int j = 0; j < 6; ++j) gCurNbt[j] = scal(q + (3 + j) * ts, o->type, o->k); gCurHaveNbt = 1; }
            gCurHaveNrm = 1; *cur += ts * o->n; break;
        case OP_CLR0: { int cc = *cur; out->rgba = readColorAt(p, &cc, o->type); *cur = cc; } break;
        case OP_CLR1: { int cc = *cur; (void)readColorAt(p, &cc, o->type); *cur = cc; } break;
        case OP_TEX0:
            out->u = scal(q, o->type, o->k); out->v = o->n == 2 ? scal(q + ts, o->type, o->k) : 0.0f;
            gCurTex[0][0] = out->u; gCurTex[0][1] = out->v;
            *haveTex = 1; *cur += ts * o->n; break;
        case OP_TEXN: {
            float* d = gCurTex[o->attr - GX_VA_TEX0];
            d[0] = scal(q, o->type, o->k); d[1] = o->n == 2 ? scal(q + ts, o->type, o->k) : 0.0f;
            *cur += ts * o->n; } break;
        }
    }
    return posOk;
}
// ---- the TEV path (2026-09-28, docs/47) ------------------------------------------------------------
#include "port/gx_tev_key.h"
extern int rhi_vita_drawTev(const TevKey*, const TevUniforms*, RhiTexture* const tex[TEVK_TEX],
                            const TevVertex*, uint32_t, const float mvp[16], int alphaMode) __attribute__((weak));
unsigned gx_tev_needed_sources(void);
int gx_tev_build(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX],
                 int acComp0, int acOp, int acComp1, int acRef0, int acRef1);
void gx_tev_texgen(int coord, u8* func, u8* src, u8* mtx, u8* norm, u8* post);
int gStfxTevArm = 1;   // 0: every draw takes the single-texture path (A/B in one binary)
unsigned gx_tev_draws, gx_tev_fb_state, gx_tev_fb_notex, gx_tev_fb_copytex, gx_tev_fb_backend, gx_tev_texgen_unsupported;
// One vertex's texcoord for a texgen (GX's texgen, on the CPU): source vector (x,y,z,1) from the
// object position, the raw normal, or a raw TEXn (s,t,1,1); through the texture matrix (identity
// for GX_IDENTITY); for MTX3x4, q is the third row; then the optional normalise + POST matrix;
// then the projective divide. Unsupported sources/functions (bump, SRTG, colour) read as (0,0), counted.
// THE MATRIX A TEXGEN NAMES (2026-10-11). The texgen's matrix argument indexes XF matrix memory: 0..27 (step 3) are the POSITION
// matrices (GX_PNMTX0 = 0, the ones GXLoadPosMtxImm fills), 30..57 the texture matrices, 60 (GX_IDENTITY) none. 19 game sites use
// GX_TG_MTX3x4 / GX_TG_POS / GX_PNMTX0 + a post matrix (projected lights, shadow falloff, caustics, heavy fog, cloud shadows): the
// view-space position, then the post matrix. The port treated every index outside 30..59 as identity, so those were computed from
// OBJECT space and slid as the camera moved. It is the texgen's own named matrix (the XF memory row), not the vertex's PNMTXIDX.
// A NORMAL source with a position matrix uses the position matrix too (w = 1), as XF does -- not the normal matrix.
// PER-VERTEX TEXTURE MATRIX (2026-10-13). When the vertex descriptor has TEXnMTXIDX, texgen n reads the XF matrix row that vertex's byte
// names INSTEAD of the matrix GXSetTexCoordGen2 named (Dolphin VertexShaderGen WriteTexCoordTransforms: `tmp = int(rawtex.z)`, rows
// tmp..tmp+2 of the transform memory; its CPU rasteriser takes the same index from srcVertex->texMtx). Same index space as the named
// matrix: 0..27 position matrices, 30..57 texture matrices, 60 identity. The skinned models rely on it (objprint_dolphin.c
// modelRenderFn_setVtxDescr / renderOpMatrix: each joint's matrix is loaded into texture slot 30 + 3 * joint). The BINRM (2) and TANGENT (3)
// sources read the 2nd and 3rd vector of a 9-component NBT normal (SDK GXSetTexCoordGen2: BINRM -> XF source row 3, TANGENT -> row 4;
// Dolphin's TransformUnit: row 3 = normal[1], row 4 = normal[2]); through the texgen matrix exactly like a normal. A vertex WITHOUT an NBT
// normal reads (0,0,1): VertexShaderGen starts every coord at vec4(0,0,1,1) and only overwrites it when the component exists.
// ux0:data/stairfax/texmtxidx_off.txt starting with 1 = neither (the per-vertex index is ignored, BINRM/TANGENT are unsupported again), in
// every path (A/B; read once, logged once).
// ux0:data/stairfax/texgen_pnmtx_old.txt starting with 1 = the old identity behaviour in both paths (A/B; read once).
unsigned gx_texgen_pn_cpu, gx_texgen_pn_gpu;   /* draws with a position-matrix texgen, per path (vt_game.c status block) */
unsigned gx_texgen_tmi_cpu, gx_texgen_tmi_gpu, gx_texgen_nbt;   /* 2026-10-13: draws whose texgen took its matrix from the vertex (TEXnMTXIDX), per path; draws with a BINRM / TANGENT source (the [texgen] line of gx_dlk_report) */
static int gTmiOld = -1;
void gx_draw_setTexMtxIdxOld(int on) { gTmiOld = on ? 1 : 0; }   /* host-test hook (gx_port_test check 16): the texmtxidx_off.txt switch, flipped in-process */
// RUN 267 A/B: ux0:data/stairfax/nbt_swap.txt starting with 1 = BINRM reads the 3rd NBT vector and TANGENT the 2nd (the order Dolphin's shader
// generator names them; the default follows its TransformUnit + the SDK). The Arwing's env bump streaks along the hull on run 264.
static int gNbtSwap = -1;
static int nbtSwap(void) {
    if (gNbtSwap < 0) { FILE* f = fopen("ux0:data/stairfax/nbt_swap.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
                        gNbtSwap = c == 49; printf("[texgen] NBT order: %s\n", gNbtSwap ? "SWAPPED (nbt_swap.txt): BINRM = 3rd vector, TANGENT = 2nd" : "BINRM = 2nd vector, TANGENT = 3rd"); }
    return gNbtSwap;
}
static int tmiOld(void) {
#ifdef BREAK_22   /* host negative control: the kill switch is not honoured (check 16's old-behaviour runs must fail) */
    return 0;
#endif
    if (gTmiOld < 0) {
        FILE* f = fopen("ux0:data/stairfax/texmtxidx_off.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gTmiOld = c == 49; printf("[texgen] per-vertex texture matrix index + binormal/tangent sources %s\n", gTmiOld ? "OFF (texmtxidx_off.txt = the old behaviour)" : "ON");
    }
    return gTmiOld;
}
static int gTgPnOld = -1;
static int tgPnOld(void) {
    if (gTgPnOld < 0) {
        FILE* f = fopen("ux0:data/stairfax/texgen_pnmtx_old.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gTgPnOld = c == 49; printf("[texgen] position-matrix texgens %s\n", gTgPnOld ? "OFF (texgen_pnmtx_old.txt = identity, the old behaviour)" : "ON");
    }
    return gTgPnOld;
}
#ifdef BREAK_14   /* host negative control: the old rule, a position-matrix texgen reads as identity (gx_port_test check 14) */
static int tgIsPn(u8 m) { (void)m; return 0; }
#else
static int tgIsPn(u8 m) { return m < 30 && !tgPnOld(); }
#endif
static const float (*tgMtx(u8 m))[4] {
    if (m >= 30 && m < 60) return gTexMtx[(m - 30) / 3];
    if (tgIsPn(m)) return gPosMtx[m / 3];
    return NULL;
}
// The matrix a vertex's TEXnMTXIDX byte names: a row of XF matrix memory (0..27 position matrices, 30..57 texture matrices, 60 and up identity).
static const float (*tgMtxRow(unsigned r))[4] {
    if (r >= 30 && r < 60) return gTexMtx[(r - 30) / 3];
    if (r < 30) return gPosMtx[r / 3];
    return NULL;
}
// tmi: that vertex's TEXnMTXIDX byte for this texgen's texcoord, or -1 = the descriptor has none (use the matrix the texgen names)
static void texgenVertex(int vi, u8 func, u8 src, u8 mtx, u8 norm, u8 post, int tmi, float out[2]) {
    float v[4] = { 0, 0, 0, 1 };
    if (src == 0) { v[0] = gVObj[vi][0]; v[1] = gVObj[vi][1]; v[2] = gVObj[vi][2]; }
    else if (src == 1) { v[0] = gVNrm0[vi][0]; v[1] = gVNrm0[vi][1]; v[2] = gVNrm0[vi][2]; }
#ifdef BREAK_17   /* host negative control: BINRM and TANGENT swapped */
    else if ((src == 2 || src == 3) && gVNbt && !tmiOld()) { const float* b = gVNbt[vi] + (src == 2 ? 3 : 0); v[0] = b[0]; v[1] = b[1]; v[2] = b[2]; }
#else
    else if ((src == 2 || src == 3) && gVNbt && !tmiOld()) { const float* b = gVNbt[vi] + ((src == 3) != nbtSwap() ? 3 : 0); v[0] = b[0]; v[1] = b[1]; v[2] = b[2]; }
#endif
    else if (src >= 4 && src <= 11) { v[0] = gVTex[vi][src - 4][0]; v[1] = gVTex[vi][src - 4][1]; v[2] = 1.0f; }
    else { ++gx_tev_texgen_unsupported; out[0] = out[1] = 0.0f; return; }
    if (func > 1) { ++gx_tev_texgen_unsupported; out[0] = out[1] = 0.0f; return; }
    float s, t, q;
#ifdef BREAK_16   /* host negative control: the per-vertex index is ignored, the texgen's named matrix is used (both paths) */
    tmi = -1;
#endif
    const float (*M)[4] = tmi >= 0 ? tgMtxRow((unsigned)tmi) : tgMtx(mtx);
    if (M) {
        s = M[0][0] * v[0] + M[0][1] * v[1] + M[0][2] * v[2] + M[0][3] * v[3];
        t = M[1][0] * v[0] + M[1][1] * v[1] + M[1][2] * v[2] + M[1][3] * v[3];
        q = func == 0 ? M[2][0] * v[0] + M[2][1] * v[1] + M[2][2] * v[2] + M[2][3] * v[3] : 1.0f;
    } else { s = v[0]; t = v[1]; q = func == 0 ? v[2] : 1.0f; }
    int proj = func == 0;
    if (post >= 64 && post < 64 + 3 * 20) {
        if (norm) { float l = sqrtf(s * s + t * t + q * q); if (l > 0.0f) { s /= l; t /= l; q /= l; } }
        const float (*P)[4] = gPostMtx[(post - 64) / 3];
        float s2 = P[0][0] * s + P[0][1] * t + P[0][2] * q + P[0][3];
        float t2 = P[1][0] * s + P[1][1] * t + P[1][2] * q + P[1][3];
        float q2 = P[2][0] * s + P[2][1] * t + P[2][2] * q + P[2][3];
        s = s2; t = t2; q = q2;
#ifdef BREAK_25   /* host negative control: the pre-2026-10-14 behaviour, a divide after ANY post matrix */
        proj = 1;
#endif
    }
    /* 2026-10-14 (Aurora shader.cpp:1260, Dolphin's texMtxInfo.projection): only a 3x4 (STQ) texgen is divided by q, post
     * matrix or not. A 2x4 + normalise + post (the game's fur/noise layer, objprint_dolphin.c:377/629/746, post = scale 37.5)
     * keeps (s, t) / |(s, t, 1)| * 37.5; dividing by the post's q (= 1/|..|) cancelled the normalisation. */
    if (proj && (q > 1e-6f || q < -1e-6f)) { s /= q; t /= q; }
    out[0] = s; out[1] = t;
}
static int expandIdx(int* out, int cap, int nverts, u8 prim) {   // expandTriangles' order, as indices
    int n = 0;
#define TRI(a, b, c) do { if (n + 3 <= cap && gVertOk[a] && gVertOk[b] && gVertOk[c]) { out[n++] = (a); out[n++] = (b); out[n++] = (c); } } while (0)
    if (prim == GX_TRIANGLES) { for (int i = 0; i + 3 <= nverts; i += 3) TRI(i, i + 1, i + 2); }
    else if (prim == GX_TRIANGLESTRIP) { for (int i = 2; i < nverts; ++i) { if (i & 1) TRI(i - 1, i - 2, i); else TRI(i - 2, i - 1, i); } }
    else if (prim == GX_TRIANGLEFAN) { for (int i = 2; i < nverts; ++i) TRI(0, i - 1, i); }
    else if (prim == GX_QUADS) { for (int i = 0; i + 4 <= nverts; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); } }
#undef TRI
    return n;
}
// SUBMIT, SPLIT (2026-10-07): submit is the largest phase (93 ms/frame at the title for 4,626
// primitives). The same 1-in-8 sampled primitives time its parts: the key build, the CPU texgen,
// the expansion to triangles, and the backend call (program lookup + batching + any flush).
unsigned long long gx_t_sub_key, gx_t_sub_texgen, gx_t_sub_expand, gx_t_sub_backend;
static int gSubTimed;
extern int rhi_vita_tev_timed __attribute__((weak));
// FOG on TEV-program draws (2026-10-07, docs/47 run 116). GXSetFog (below) records the request; a TEV draw under a
// PERSPECTIVE projection with a perspective fog type (2 lin, 4 exp, 5 exp2, 6 rev exp, 7 rev exp2) puts the SHAPE in
// the key (fogType) and the numbers in the uniforms; tev_gen's program fogs the final colour from the interpolated
// clip w. NOT built, and counted: orthographic fog types (>= 8) or an orthographic projection, and draws that do not
// go through a TEV program (the two fixed-function paths). Kill switch for the device A/B:
// ux0:data/stairfax/nofog.txt present = no fog at all (same pattern as tev_nomemo.txt).
extern int gFogType; extern float gFogStart, gFogEnd; extern GXColor gFogColor;
/* N1 (RT_THREAD.md D9): the render thread must not read the colour the GAME writes (gFogColor is shared with track/intersect_render.c).
   gx_fog_private == 0 (DIRECT, the default): gFogColor, exactly as before.  == 1 (threaded): R keeps its own copy, gx_fogColorR. */
extern GXColor gx_fogColorR; extern int gx_fog_private;
#ifdef BREAK_29   /* host test break (gx_port_test BREAK_29): the pre-N1 behaviour, the draw reads the game's variable even when private */
#define FOG_COLOR_SEL() (gFogColor)
#else
#define FOG_COLOR_SEL() (gx_fog_private ? gx_fogColorR : gFogColor)
#endif
unsigned gx_fog_drawn, gx_fog_nd_ortho, gx_fog_nd_notev, gx_fog_nd_off, gx_fog_nd_slots;   /* nd_slots: also counted in nd_ortho's bucket by the callers */
static int fogWant(void) { int t = gFogType; return t == 2 || (t >= 4 && t <= 7) || (t >= 10 && t <= 15); }
static int fogKilled(void) {
    static int s = -1;
    if (s < 0) { FILE* f = fopen("ux0:data/stairfax/nofog.txt", "r"); s = f ? 1 : 0; if (f) fclose(f);
                 printf("[fog] fog %s\n", s ? "OFF (nofog.txt)" : "ON"); }
    return s;
}
void gx_fog_report(void) {   // the [fog] line; also printed here at the 1st fog draw and every 16384th (vt_game.c's status block may call it too)
    printf("[fog] drawn %u | requested, NOT drawn: orthographic type/projection %u, not a TEV draw %u, kill switch %u\n",
           gx_fog_drawn, gx_fog_nd_ortho, gx_fog_nd_notev, gx_fog_nd_off);
}
static void fogCounted(void) {
    unsigned t = gx_fog_drawn + gx_fog_nd_ortho + gx_fog_nd_notev + gx_fog_nd_off;
    if (t == 1 || !(t & 0x3fff)) gx_fog_report();
}
static int gDlcOn = -1;   // the display-list census (end of this file): -1 not read, 0 off, 1 on
static int dlcOn(void); static void dlcOne(const unsigned char* dl, u32 nbytes); static void dlcVsKey(const TevKey* k, const u8* sc);
static int vertexStride(GXVtxFmt fmt);
extern unsigned gx_imm_prims, gx_imm_verts;
#ifdef GX_C3_SELFCHECK   /* R3a T2 (host tests only): every cached display-list call re-derives what the memos answered and compares BYTES (see c3Selfcheck_*, below the VS memo) */
unsigned gx_c3_chk_tev, gx_c3_chk_vs, gx_c3_chk_vs_ok, gx_c3_chk_bad, gx_c3_chk_hint, gx_c3_chk_hint_eq;
static void c3Selfcheck_tev(const TevKey* k, const TevUniforms* u, const u8* sc, const u8* sm, int bld);
struct DlkRec;
static void c3Selfcheck_vs(const struct DlkRec* r, const TevKey* tk, const u8* sc, const float* mvp, int fogWhy);
#define C3_CHK_TEV(k, u, sc, sm, bld) c3Selfcheck_tev(k, u, sc, sm, bld)
#define C3_CHK_VS(r, tk, sc, mvp, fw) c3Selfcheck_vs(r, tk, sc, mvp, fw)
#else
#define C3_CHK_TEV(k, u, sc, sm, bld) ((void) 0)
#define C3_CHK_VS(r, tk, sc, mvp, fw) ((void) 0)
#endif
// 1: the draw went through a TEV program. 0: the caller draws it the old way (every reason counted).
static int drawTev(int nverts, u8 prim, const float mvp[16]) {
    TevKey k; TevUniforms u; u8 sc[TEVK_TEX], sm[TEVK_TEX];
    unsigned long long s0 = gSubTimed ? GXT_NOW() : 0, s1 = 0, s2 = 0, s3 = 0;
    const int tevBld = gx_tev_build(&k, &u, sc, sm, gAcComp0, gAcOp, gAcComp1, gAcRef0, gAcRef1);
    C3_CHK_TEV(&k, &u, sc, sm, tevBld);
    if (!tevBld) { ++gx_tev_fb_state; return 0; }
    int fogWhy = 0;   // 0 no fog requested, 1 drawn, 2 orthographic, 3 kill switch
    if (fogWant()) {
        if ((gFogType & 8) || gProjOrtho) fogWhy = 2;
        else if (k.numTex >= 8) { fogWhy = 2; ++gx_fog_nd_slots; }   /* TEXCOORD0..9 are full: no varying left for the fog depth (gx_tev_key.h) */
        else if (fogKilled()) fogWhy = 3;
        else {
            fogWhy = 1; k.fogType = (uint8_t)gFogType;
            u.fogStart = gFogStart; u.fogScale = gFogEnd == gFogStart ? 0.0f : 1.0f / (gFogEnd - gFogStart);
            u.fogColor[0] = FOG_COLOR_SEL().r; u.fogColor[1] = FOG_COLOR_SEL().g; u.fogColor[2] = FOG_COLOR_SEL().b;
        }
    }
    RhiTexture* tx[TEVK_TEX] = { 0 };
    for (int sl = 0; sl < k.numTex; ++sl) {
        if (gCopyMaps >> sm[sl] & 1) { ++gx_tev_fb_copytex; return 0; }   // an EFB copy nobody filled
        tx[sl] = gTexMap[sm[sl]];
        if (!tx[sl]) { ++gx_tev_fb_notex; return 0; }
    }
    if (gDlcOn > 0) dlcVsKey(&k, sc);
    static float tc[GX_MAX_BATCH][TEVK_TEX][2];
    int pnSeen = 0;
    int tmiLive = 0, tmiSeen = 0, nbtSeen = 0;   /* the descriptor carries TEXnMTXIDX bytes and decodePrim kept them (gVTmi); counted once a draw */
    if (gTmiLive && !tmiOld() && gVTmi) tmiLive = 1;
    if (gSubTimed) { s1 = GXT_NOW(); gx_t_sub_key += s1 - s0; }
    for (int sl = 0; sl < k.numTex; ++sl) {
        u8 f, s, m, nm, pt; gx_tev_texgen(sc[sl], &f, &s, &m, &nm, &pt);
        if (tgIsPn(m)) { if (!pnSeen) { ++gx_texgen_pn_cpu; pnSeen = 1; } }
        const int tmiC = tmiLive && sc[sl] < 8 && gDesc[GX_VA_TEX0MTXIDX + sc[sl]] != GX_NONE ? sc[sl] : -1;   /* the vertex names this texgen's matrix */
        if (tmiC >= 0 && !tmiSeen) { ++gx_texgen_tmi_cpu; tmiSeen = 1; }
        if ((s == 2 || s == 3) && !nbtSeen && !tmiOld()) { ++gx_texgen_nbt; nbtSeen = 1; }
        if (f == 1 && s >= 4 && s <= 11 && !tgMtx(m) && !(pt >= 64 && pt < 64 + 3 * 20) && tmiC < 0) {
            /* TEXn through GX_IDENTITY, no post matrix (the census's commonest texgen): a copy */
            for (int vi = 0; vi < nverts; ++vi) { tc[vi][sl][0] = gVTex[vi][s - 4][0]; tc[vi][sl][1] = gVTex[vi][s - 4][1]; }
        } else
            for (int vi = 0; vi < nverts; ++vi) texgenVertex(vi, f, s, m, nm, pt, tmiC >= 0 ? gVTmi[vi][tmiC] : -1, tc[vi][sl]);
    }
    static int idx[GX_MAX_BATCH * 3];
    static TevVertex tv[GX_MAX_BATCH * 3];
    if (gSubTimed) { s2 = GXT_NOW(); gx_t_sub_texgen += s2 - s1; }
    int ni = expandIdx(idx, GX_MAX_BATCH * 3, nverts, prim);
    for (int i = 0; i < ni; ++i) {
        int v = idx[i];
        TevVertex* o = &tv[i];
        o->x = gVerts[v].x; o->y = gVerts[v].y; o->z = gVerts[v].z;
        o->c0 = gVerts[v].rgba; o->c1 = 0;   // COLOR1A1 is not computed: a stage that reads it sees zero
        for (int sl = 0; sl < k.numTex; ++sl) { o->t[sl][0] = tc[v][sl][0]; o->t[sl][1] = tc[v][sl][1]; }
    }
    if (gSubTimed) { s3 = GXT_NOW(); gx_t_sub_expand += s3 - s2; if (&rhi_vita_tev_timed) rhi_vita_tev_timed = 1; }
    int drawn = rhi_vita_drawTev(&k, &u, tx, tv, (uint32_t)ni, mvp, gPxActive ? pxAlphaMode() : RHI_ALPHA_OPAQUE);
    if (gSubTimed) { gx_t_sub_backend += GXT_NOW() - s3; if (&rhi_vita_tev_timed) rhi_vita_tev_timed = 0; }
    if (!drawn) { ++gx_tev_fb_backend; return 0; }
    ++gx_tev_draws;
    if (fogWhy) { if (fogWhy == 1) ++gx_fog_drawn; else if (fogWhy == 2) ++gx_fog_nd_ortho; else ++gx_fog_nd_off; fogCounted(); }
    return 1;
}
static int decodePrim(const unsigned char* src, GXVtxFmt fmt, u8 prim, int nverts, int maxBytes) {
    if (nverts > GX_MAX_BATCH) nverts = GX_MAX_BATCH;
    const int timed = ((++gx_prims) & 7) == 0;
    unsigned long long t0 = timed ? GXT_NOW() : 0, t1 = 0, t2 = 0;
    gx_prim_verts += (unsigned)nverts;
    int c = 0, haveTex = 0;
    int havePnmtx = (gDesc[GX_VA_PNMTXIDX] != GX_NONE) || gHaveJointOff;
    int needN = pxNeedNormals();
    /* the active attributes, once per primitive rather than 26 slot tests per vertex (run 65);
       gStfxDecodeArm 0 keeps the old every-slot walk for the A/B and the host equality test */
    GXAttr plan[GX_VA_MAX_ATTR]; int planN = 0;
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) if (gStfxDecodeArm == 0 || gDesc[a] != GX_NONE) plan[planN++] = a;
    /* Index bounds, once per primitive (2026-09-28): the per-vertex check divided by the array
       stride for every indexed attribute of every vertex -- a SOFTWARE divide on the Cortex-A9 (no
       hardware divider). Same verdicts: idx < lim, lim 0 = never, ~0u = unbounded. */
    unsigned lim[GX_VA_MAX_ATTR];
    for (int pi = 0; pi < planN; ++pi) lim[plan[pi]] = idxLimit(&gArray[plan[pi]]);
    DecOp ops[GX_VA_MAX_ATTR]; int nops = gStfxDecodeArm ? buildOps(ops, plan, planN, fmt, lim) : 0;
    gTevLive = gStfxTevArm && rhi_vita_drawTev && gRhi;
    /* keep only what the active texgens read (run 91: all of it, every vertex, cost ~9 ms/frame) */
    unsigned tevNeed = gTevLive ? gx_tev_needed_sources() : 0;
    /* 2026-10-13: the vertex's TEXnMTXIDX bytes (not on the forced layout, which parses no attributes) and the NBT vectors, kept only for the texgens that read them */
    int tmiAny = 0; for (int q = 0; q < 8; ++q) tmiAny |= gDesc[GX_VA_TEX0MTXIDX + q] != GX_NONE;
    const int auxOk = ((tevNeed & 12) || (gTevLive && tmiAny)) && auxEnsure();
    gTmiLive = auxOk && gTevLive && tmiAny && gForceStride <= 0 && !tmiOld();
    for (int vi = 0; vi < nverts; ++vi) {
        if (maxBytes > 0 && c >= maxBytes) { nverts = vi; break; }  // don't read past the DL
        RhiTexVertex out; out.x=out.y=out.z=0; out.rgba=0xFFFFFFFFu; out.u=out.v=0; gCurHaveNrm = 0; gCurHaveNbt = 0;
        if (tevNeed >> 4) for (int q = 0; q < 8; ++q) if (tevNeed >> (4 + q) & 1) gCurTex[q][0] = gCurTex[q][1] = 0.0f;
        int pnmtx = 0, posOk = 1;   // posOk=0 -> POS index was out of bounds, drop this vertex
        if (gForceStride > 0) {  // explicit POS-only layout (autodetected skinned fallback)
            const unsigned char* vp = src + c;
            if (gHaveJointOff) pnmtx = vp[0];
            unsigned idx = gForcePosSz==2 ? ((vp[gForcePosOff]<<8)|vp[gForcePosOff+1]) : vp[gForcePosOff];
            const ArrayDef* ar = &gArray[GX_VA_POS];
            int inBounds = 0;
            if (ar->base && ar->stride) {
                if (!gSrcLo) inBounds = 1;
                else if (ar->base >= gSrcLo && ar->base < gSrcHi) {
                    unsigned maxIdx = (unsigned)(gSrcHi - ar->base) / (unsigned)ar->stride;
                    inBounds = (idx < maxIdx);
                }
            }
            if (inBounds) { int ac = 0; readAttrValue(GX_VA_POS, &gVat[fmt][GX_VA_POS], ar->base + idx*ar->stride, &ac, &out, &haveTex); }
            else posOk = 0;
            c += gForceStride;
            if (tevNeed & 1) { gVObj[vi][0] = out.x; gVObj[vi][1] = out.y; gVObj[vi][2] = out.z; }
            if (tevNeed & 2) memset(gVNrm0[vi], 0, sizeof gVNrm0[vi]);
            if (tevNeed >> 4) memset(gVTex[vi], 0, sizeof gVTex[vi]);
            if (auxOk && (tevNeed & 12)) memcpy(gVNbt[vi], kNbtDefault, sizeof kNbtDefault);
            skinVertex(&out, pnmtx, havePnmtx);
            gVerts[vi] = out; gVertOk[vi] = (unsigned char)posOk;
            continue;
        }
        if (gStfxDecodeArm) posOk = decodeOps(ops, nops, src, &c, &out, &haveTex, &pnmtx);
        else for (int pi = 0; pi < planN; ++pi) {
            GXAttr a = plan[pi];
            GXAttrType d = gDesc[a];
            if (d == GX_NONE) continue;
            if (a <= GX_VA_TEX7MTXIDX) { unsigned mb = rdU(src,&c,1); if (a == GX_VA_PNMTXIDX) pnmtx = (int)mb; else gCurTmi[a - GX_VA_TEX0MTXIDX] = (unsigned char)mb; continue; }
            VatAttr* v = &gVat[fmt][a];
            if (d == GX_INDEX8 || d == GX_INDEX16) {
                unsigned idx = rdU(src, &c, d==GX_INDEX8?1:2);
                const ArrayDef* ar = &gArray[a];
                // Bound the index against the source buffer BEFORE computing the element
                // pointer, so a bad index can't overflow past the guard into wild memory
                // (idxLimit: the rules, computed once per primitive).
                int inBounds = idx < lim[a];
                if (inBounds) { int ac = 0; readAttrValue(a, v, ar->base + idx*ar->stride, &ac, &out, &haveTex); }
                else if (a == GX_VA_POS) posOk = 0;   // bad position -> drop vertex, not a spike
            } else { // GX_DIRECT: read inline from the stream
                readAttrValue(a, v, src, &c, &out, &haveTex);
            }
        }
        if (tevNeed) {
            if (tevNeed & 1) { gVObj[vi][0] = out.x; gVObj[vi][1] = out.y; gVObj[vi][2] = out.z; }
            if (tevNeed & 2) {
                if (gCurHaveNrm) { gVNrm0[vi][0] = gCurNrm[0]; gVNrm0[vi][1] = gCurNrm[1]; gVNrm0[vi][2] = gCurNrm[2]; }
                else memset(gVNrm0[vi], 0, sizeof gVNrm0[vi]);
            }
            for (int q = 0; q < 8; ++q) if (tevNeed >> (4 + q) & 1) { gVTex[vi][q][0] = gCurTex[q][0]; gVTex[vi][q][1] = gCurTex[q][1]; }
            if (auxOk && (tevNeed & 12)) memcpy(gVNbt[vi], gCurHaveNbt ? gCurNbt : kNbtDefault, sizeof kNbtDefault);
        }
        if (gTmiLive) memcpy(gVTmi[vi], gCurTmi, 8);
        skinVertex(&out, pnmtx, havePnmtx);
        gVerts[vi] = out; gVertOk[vi] = (unsigned char)posOk;
        gVertHaveN[vi] = (unsigned char)gCurHaveNrm;
        gLastDecoded = vi + 1;
        if (gCurHaveNrm && needN) {   // to view space by the normal matrix of this vertex's slot (real mode)
            int slot = havePnmtx ? (pnmtx / 3) : gCurMtx;
            if ((unsigned)slot >= GX_MTX_SLOTS) slot = 0;
            const float (*N)[4] = gNrmMtx[slot];
            float x = gCurNrm[0], y = gCurNrm[1], z = gCurNrm[2];
            gVertN[vi][0] = N[0][0]*x + N[0][1]*y + N[0][2]*z;
            gVertN[vi][1] = N[1][0]*x + N[1][1]*y + N[1][2]*z;
            gVertN[vi][2] = N[2][0]*x + N[2][1]*y + N[2][2]*z;
        }
    }
    if (timed) { t1 = GXT_NOW(); gx_t_decode += t1 - t0; }
    if (gPxActive) {   // the GAME's pixel state (v1): colour channels, blend, colour update
        if (!gColorUpdate) { ++gx_px_skipped_noupdate; return c; }
        int haveClr = gDesc[GX_VA_CLR0] != GX_NONE;
        u32 v0in = gVerts[0].rgba;
        for (int vi = 0; vi < nverts; ++vi)
            gVerts[vi].rgba = pxChannelColour(gVerts[vi].rgba, haveClr, &gVerts[vi], gVertN[vi], gVertHaveN[vi]);
        pxJournalLit(v0in, haveClr, gVerts[0].rgba, gVertHaveN[0]);
        if (gRhi) rhi_setAlphaMode(gRhi, pxAlphaMode());
        if (rhi_vita_setDepthState) rhi_vita_setDepthState(gZCompare, gZFunc, gZUpdate);
    }
    if (timed) { t2 = GXT_NOW(); gx_t_light += t2 - t1; }
    if (gRhi) {
        float mvp[16];
        if (gRealSkin) { for (int i=0;i<4;++i) for (int j=0;j<4;++j) mvp[i*4+j]=gProj[i][j];
                         gxClipZ(mvp); }
        else computeMVP(mvp);
        rhi_setColorTransform(gRhi, mvp);
        static RhiTexVertex tri[GX_MAX_BATCH*3];
        int n = expandTriangles(tri, (int)(sizeof(tri)/sizeof(tri[0])), nverts, prim);
        if (dbg()) fprintf(stderr, "[gxdraw] prim=%02x fmt=%d nv=%d tris=%d v0=(%.1f,%.1f,%.1f) rgba=%08x tex=%d\n",
                           prim, fmt, nverts, n/3, gVerts[0].x, gVerts[0].y, gVerts[0].z, gVerts[0].rgba, haveTex);
        if (n > 0 && gStfxPerfArm != 1) {   /* run 64 A/B arm 1: everything but the GPU submit */
            gx_tev_note_draw((unsigned)(n / 3), gTexMap);
            gSubTimed = timed;
            if (gTevLive && drawTev(nverts, prim, mvp)) { if (timed) gx_t_submit += GXT_NOW() - t2; return c; }
            if (fogWant()) { ++gx_fog_nd_notev; fogCounted(); }   /* fog requested, the draw is fixed-function: not fogged */
            RhiTexture* tex = gTex;
            if (gMovieMaps)   // a movie frame on a map this draw's stages read wins (see gMovieMaps)
                for (int s = 0; s < gx_tev_num_stages(); ++s) {
                    int m = gx_tev_stage_map(s);
                    if (m < 8 && (gMovieMaps >> m & 1) && gTexMap[m]) {
                        tex = gTexMap[m]; ++gx_movie_draws;
                        /* APPROXIMATION (run 84 journal, draw 1992): the laptop screen's lit vertex
                           colour is bright green (ff18ff0f); the single-texture path multiplied the
                           movie by it. Retail's five-stage combine (addYUVVideoTevStages) is not
                           emulated and shows the movie near full brightness: the movie draws unlit. */
                        for (int q = 0; q < n; ++q) tri[q].rgba |= 0x00FFFFFFu;
                        break;
                    }
                }
            if (gStfxTexArm >= 1) {
                if (gStfxTexArm >= 2 && gx_tev_uses_indirect()) return c;
                int m = gx_tev_tex0_stage_map();
                if (m >= 0) tex = gTexMap[m];
            }
            if (gCopyMaps && tex) {   // the sampled texture is an EFB copy nobody filled (see GXCopyTex)
                int isCopy = 0;
                for (int k = 0; k < 8; ++k) if ((gCopyMaps >> k & 1) && gTexMap[k] == tex) isCopy = 1;
                if (isCopy) {
                    /* another active stage's ordinary map, if there is one (a model whose first stage
                       reads a shadow copy still has its own texture); else the draw is the effect */
                    RhiTexture* alt = NULL;
                    for (int st = 0; st < gx_tev_num_stages() && !alt; ++st) {
                        int m = gx_tev_stage_map(st);
                        if (m < 8 && gTexMap[m] && gTexMap[m] != tex && !(gCopyMaps >> m & 1)) alt = gTexMap[m];
                    }
                    if (gxj()) {
                        int k = 0; while (k < 8 && !((gCopyMaps >> k & 1) && gTexMap[k] == tex)) ++k;
                        printf("[draw] copy-map draw: %s (tris %d, stages %d)", alt ? "ALT map" : "SKIPPED", n / 3, gx_tev_num_stages());
                        journalCopyHit(k);
                    }
                    /* run 95: the blocky overlay was ONE full-screen 6-stage quad whose stage 0 reads a 320x240
                       grab of the SCREEN; its ALT map is the effect's 64x64 noise map, which was drawn over the
                       whole frame. A draw that reads a screen-sized copy is a post effect on a picture already
                       there: without the copy it draws nothing. (Shadow copies are smaller than the screen grab.) */
                    {   int k = 0; while (k < 8 && !((gCopyMaps >> k & 1) && gTexMap[k] == tex)) ++k;
                        if (k < 8 && gMapW[k] * gMapH[k] >= 320u * 240u) { ++gx_copy_draws_screenfx; return c; } }
                    if (!alt) { ++gx_copy_draws_skipped; return c; }
                    tex = alt; ++gx_copy_draws_alt;
                }
            }
            /* Vita run 111: a draw that samples a REAL EFB copy and has lost its combiner must not be
               drawn at all. The blur filter (7 stages, refused by the TEV path for its 6 samples) came
               through here every frame and painted its Z8 copy over the screen, texel times white,
               blended: far = opaque white, the CloudRunner shots lost their whole sky. */
            if (gFilledCopyMaps && tex) {
                for (int k = 0; k < 8; ++k)
                    if ((gFilledCopyMaps >> k & 1) && gTexMap[k] == tex) { ++gx_copy_draws_tevless; return c; }
            }
            if (tex && haveTex) { rhi_setTexture(gRhi,0,tex); rhi_drawTextured(gRhi,tri,(u32)n); }
            else { static RhiColorVertex col[GX_MAX_BATCH*3];
                   for (int i=0;i<n;++i){ col[i].x=tri[i].x; col[i].y=tri[i].y; col[i].z=tri[i].z; col[i].rgba=tri[i].rgba; }
                   rhi_drawColored(gRhi, col, (u32)n); }
        }
    }
    if (timed) gx_t_submit += GXT_NOW() - t2;
    return c;
}

static void decodeAndDrawImpl(void);
static void decodeAndDraw(void) { RT_SUBS(RT_S_ENTRY_IMM); unsigned long long t = GXT_NOW(); RT_ENTER(RT_SHIM); decodeAndDrawImpl(); RT_LEAVE(); gx_w_imm_us += GXT_NOW() - t; }
static void decodeAndDrawImpl(void) { ++gx_imm_prims; gx_imm_verts += (unsigned)gPrimN; if (STFX_ARM5()) return;   /* arm 5 (see gStfxPerfArm): counters first */
    decodePrim(gFifo, gPrimFmt, gPrim, gPrimN, gExpectBytes); }

// --- docs/47 fast path, STAGE 1 (2026-10-08): the per-display-list cache --------------------------------
// WHY: per frame ~100 GXCallDisplayList calls carry ~5,000 primitives / ~36,000 strip vertices, and decodePrim
// re-decodes every strip vertex from big-endian bytes, skins it, lights it, runs CPU texgen, expands strips and
// calls the backend once per PRIMITIVE (hardware: decode 86 ms, light 104 ms, submit 118 ms per frame). The census
// (end of file) measured: list BYTES never change (0 of 413), the ARRAYS a list indexes change for 32 lists (13% of
// vertices, CPU-morphed models), only 65% of strip vertices are distinct records, and state cannot change INSIDE one
// call. So: per list, keep the DECODED-BUT-UNTRANSFORMED distinct vertices plus a strip->distinct map; per call
// transform/light/texgen each DISTINCT vertex once, expand through the map, make ONE backend call. The decode is
// decodeOps itself (the same code the per-primitive path runs), so the raw floats are bit-identical by construction;
// the transform is skinVertex / the decodePrim normal expression / pxChannelColour / texgenVertex, unchanged.
// A record is valid for: list pointer + size, gDesc, the VAT rows of the formats the list uses, and each indexed
// attribute's array base/stride/idxLimit. Two hashes guard it every call: the list bytes (changed -> rebuild) and the
// array bytes inside the [min,max] index span each indexed attribute uses (changed -> re-decode, keep the map).
// Anything the cache cannot reproduce bit for bit (a vertex that reads past the list end, a TEV refusal, a backend
// refusal, ...) falls back to the per-primitive loop for that call, untouched. gStfxDlCacheArm 0 = today's path.
int gStfxDlCacheArm = 1;
#define DLK_SETS 256   /* 2026-10-20: 512x4 -> 256x16: the hub evicted 12 records/frame (= its 12 rebuilds/frame) with the 8 MB budget half used */
#define DLK_WAYS 16
#define DLK_FMTS 8
#define DLK_BUDGET (8u << 20)      /* bytes of records alive; LRU eviction beyond it */
#define DLK_MAXD 65535u            /* distinct vertices per record (u16 map) */
#define DLK_MAXSPAN (4u << 20)     /* bytes one attribute's array span may hash */
#define DLK_MAXTRIV (1u << 20)     /* vertices one call may expand */
typedef unsigned long long dlk64;
typedef struct { u32 mapOff, nv, declared, ntri; u8 prim; } DlkPrim;
typedef struct { u8 desc[GX_VA_MAX_ATTR]; const unsigned char* base[GX_VA_MAX_ATTR]; int stride[GX_VA_MAX_ATTR]; unsigned lim[GX_VA_MAX_ATTR]; } DlkKey;
typedef struct DlkRec {
    const unsigned char* dl; u32 nbytes; dlk64 lastUse;
    u8 state, why;                 /* state 1 usable, 2 uncacheable (why = DLK_WHY_*) */
    DlkKey key; u8 fmtMask; VatAttr vat[DLK_FMTS][GX_VA_MAX_ATTR];
    u32 spanLo[GX_VA_MAX_ATTR], spanHi[GX_VA_MAX_ATTR], spanElem[GX_VA_MAX_ATTR]; u8 spanOn[GX_VA_MAX_ATTR];
    dlk64 hDl, hArr;
    u32 evSeq, dynGen; u8 okList, okArr, dyn;   /* run 165, validation by events: events seen up to; calls in a row the list / the arrays matched */
    u32 nprim, nstrip, ndist, ntri, stripVTri, declaredSum, bytes;
    u8 hasNrm, hasClr, hasPnm, ntex; u8 texAttr[8]; signed char texIdx[8];
    u8 tmiMask, ntmi, tmiBad; signed char tmiIdx[8];   /* 2026-10-13: TEXnMTXIDX in the descriptor (bit n), how many, bit n = some distinct vertex's byte is not a texture-matrix row 30,33..57; texcoord n -> its byte's column (-1 none) */
    DlkPrim* prim; u16* map; float* pos; float* nrm; float* nbt; u8* tmi; float* tex; u32* clr; u32* voff; u8* fl; u8* pnm;   /* nbt: binormal, tangent (6 floats a vertex; NULL when no distinct vertex has an NBT normal); tmi: ntmi bytes a vertex */
    u32* runs; u32 runOff[GX_VA_MAX_ATTR], runN[GX_VA_MAX_ATTR]; signed char runState;   /* the index runs dlkArrChanged hashes: 0 untried / 1 built / -1 whole spans */
    dlk64 hA[GX_VA_MAX_ATTR]; u32* fidx; signed char fastState; u8 fastChecked;   /* per-attribute span hashes; the fast refresh's indices (dlkFastIdx), 0 untried / 1 usable / -1 never */
    u8 vsScan, vsWhy; u32 vsNN, vsNB;   /* GXAOT_HOOK C3 arm 102: gx_vs_build's record scans, done once (vsScan 1 = valid) */
    u32 nprimTri;                       /* R3a C4: primitives with at least one triangle (the GPU path counted them with a loop every call) */
    u32 wrSeq, wrEpoch, c8Dirty; u8 c9Slow;   /* R6 C8: the write-ring position (and epoch) at which this record's hashes last matched the bytes; the attribute bits an announced write has changed since then; R6 C9: a refresh changed more than POS / NRM, so every GPU slot's image is stale */
    u32 vatGen; u8 vatGenOk;            /* R3c C7: the VAT kind's generation at which the record's copy of the VAT last matched the live one (valid when vatGenOk) */
    void* gpuMesh; u32 nRefresh; u8 gpuNo, meshStale;   /* stage 2: the backend mesh (built on first GPU draw), array refreshes seen, 1 = stays on the CPU path, 1 = the mesh's vertices are older than the record's */
    const struct GxSkinTable* skinT;    /* R8 GPU skinning: the table gpuMesh's aSk was built for (NULL = an unskinned image).  Only dereferenced right after it is (re)assigned from the BOUND table; never compared */
    uint32_t skinId;                    /* ... and its id (0 = unskinned): the mesh is rebuilt when a draw wants another id (gx_skin_mesh_reusable_id).  R8-MF A2: ids are never reused, a table address can be */
    uint32_t skinChkId; u8 skinChkOk;   /* R8: the id of the table dlkSkinPrep last judged this record against (0 = none yet), and its verdict */
} DlkRec;
/* R8 GPU skinning (host/engine/R8_GPUSKIN.md): the rules live in hal/gx_skin.c (host/tests/gx_skin_test.c holds them).  These WEAK defaults mean "no skin is ever bound" so a host test that links gx_draw.c without
   gx_skin.c still builds and behaves exactly as before; the real definitions override them. */
#define GXD_WEAK __attribute__((weak))
GXD_WEAK const struct GxSkinTable* gx_skin_hal_table(const void* posBase) { (void)posBase; return NULL; }
extern const unsigned char* gGxSkinChkCpu __attribute__((weak));   /* R8 HAL oracle (gx_skin.c); weak so host tests without gx_skin.c link */
GXD_WEAK void gx_skin_hal_oracle(const struct GxSkinTable* t, const float* pos, const uint32_t* fidx, uint32_t ndist, int hasNrm, const float* img) { (void)t; (void)pos; (void)fidx; (void)ndist; (void)hasNrm; (void)img; }
GXD_WEAK int gx_skin_hal_check(const struct GxSkinTable* t, const void* a, const void* b, int c, int d, int e, int f, int g) { (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return 1; }
GXD_WEAK int gx_skin_hal_attr(const struct GxSkinTable* t, uint32_t pi, uint32_t ni, int hasNrm, uint8_t out[8]) { (void)t; (void)pi; (void)ni; (void)hasNrm; (void)out; return 0; }
GXD_WEAK int gx_skin_mesh_reusable_id(uint32_t a, uint32_t b) { return a == b; }
GXD_WEAK uint32_t gx_skin_table_id(const struct GxSkinTable* t) { (void)t; return 0u; }
GXD_WEAK const float* gx_skin_vs_palette(void) { return NULL; }
GXD_WEAK void gx_skin_hal_ref(void) {}
GXD_WEAK int gx_skin_fallback_fill(const struct GxSkinTable* t, const void* nrmBase, const unsigned char** pos, const unsigned char** nrm) { (void)t; (void)nrmBase; (void)pos; (void)nrm; return 0; }
GXD_WEAK void gx_skin_hal_fallback(void) {}
GXD_WEAK void gx_skin_hal_why(int why) { (void)why; }
GXD_WEAK void gx_skin_hal_draw(void) {}
#if defined(__vita__)
extern const struct { const struct GxSkinTable* t; } gGxSkin;   /* gx_skin.h GxSkinBound: .t is its first member.  On the device an unskinned list pays this one load and branch (the host stand-ins call the helper) */
#define GXD_SKIN_BOUND() (gGxSkin.t != NULL)
#else
#define GXD_SKIN_BOUND() 1
#endif
static inline const struct GxSkinTable* gxdSkinWant(void);
/* stage 2, the device side (rhi_vita_tev.inc). Only the Vita backend has one; elsewhere these are null constants, not weak
   externs (a mingw host link does not null an undefined weak reference: gx_port_test.c line 16), so the branch is dead code. */
#if defined(__vita__)
extern void* rhi_vita_gx_mesh_create(const void* verts, uint32_t vbytes, const uint16_t* idx, uint32_t nidx, const GxMeshLayout* lay) __attribute__((weak));
extern void  rhi_vita_gx_mesh_destroy(void* mesh) __attribute__((weak));
extern void  rhi_vita_gx_mesh_update(void* mesh, const void* verts, uint32_t vbytes) __attribute__((weak));
extern int   rhi_vita_drawTevGx(const TevKey*, const TevUniforms*, RhiTexture* const tex[TEVK_TEX], const VsKey*, const VsUniforms*,
                                void* mesh, int alphaMode) __attribute__((weak));
/* R6 C9 (host/engine/R6_SKIN.md s2): the GXM backend's two-half update (rhi_gxm_draw.c).  Present in every Vita link; the begin call answers *fresh = -1 when the GXM backend is not the live one (vitaGL), and the caller then takes the old path. */
extern void* rhi_gxm_gx_mesh_update_begin(void* mesh, uint32_t vbytes, int* fresh) __attribute__((weak));
extern void  rhi_gxm_gx_mesh_update_commit(void* mesh) __attribute__((weak));
#elif defined(GX_C5_SELFCHECK)
/* R3c host tests (gx_rt_state_test.c): the GPU vertex path is LIVE on the host, against a recording stand-in for the backend (the test defines these four). */
extern void* gx_c5h_mesh_create(const void* verts, uint32_t vbytes, const uint16_t* idx, uint32_t nidx, const GxMeshLayout* lay);
extern void  gx_c5h_mesh_destroy(void* mesh);
extern void  gx_c5h_mesh_update(void* mesh, const void* verts, uint32_t vbytes);
extern int   gx_c5h_drawTevGx(const TevKey*, const TevUniforms*, RhiTexture* const tex[TEVK_TEX], const VsKey*, const VsUniforms*, void* mesh, int alphaMode);
extern void* gx_c9h_mesh_update_begin(void* mesh, uint32_t vbytes, int* fresh);   /* R6 C9: the host stand-ins of the two-half update */
extern void  gx_c9h_mesh_update_commit(void* mesh);
static void* (*const rhi_gxm_gx_mesh_update_begin)(void*, uint32_t, int*) = gx_c9h_mesh_update_begin;
static void  (*const rhi_gxm_gx_mesh_update_commit)(void*) = gx_c9h_mesh_update_commit;
static void* (*const rhi_vita_gx_mesh_create)(const void*, uint32_t, const uint16_t*, uint32_t, const GxMeshLayout*) = gx_c5h_mesh_create;
static void  (*const rhi_vita_gx_mesh_destroy)(void*) = gx_c5h_mesh_destroy;
static void  (*const rhi_vita_gx_mesh_update)(void*, const void*, uint32_t) = gx_c5h_mesh_update;
static int   (*const rhi_vita_drawTevGx)(const TevKey*, const TevUniforms*, RhiTexture* const tex[TEVK_TEX], const VsKey*, const VsUniforms*, void*, int) = gx_c5h_drawTevGx;
#else
static void* (*const rhi_vita_gx_mesh_create)(const void*, uint32_t, const uint16_t*, uint32_t, const GxMeshLayout*) = 0;
static void  (*const rhi_vita_gx_mesh_destroy)(void*) = 0;
static void  (*const rhi_vita_gx_mesh_update)(void*, const void*, uint32_t) = 0;
static int   (*const rhi_vita_drawTevGx)(const TevKey*, const TevUniforms*, RhiTexture* const tex[TEVK_TEX], const VsKey*, const VsUniforms*, void*, int) = 0;
static void* (*const rhi_gxm_gx_mesh_update_begin)(void*, uint32_t, int*) = 0;
static void  (*const rhi_gxm_gx_mesh_update_commit)(void*) = 0;
#endif
static int gx_vs_build(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvp);   /* mvp: the caller's clip-z'd projection (R3a C4), or NULL = compute it */
/* GXAOT_HOOK_BEGIN C3 */
/* ---- GXAOT release track C3, arm 102 (patch 003) ----------------------------------------------------------------------------
 * gx_vs_build runs once per GPU-path draw (run_t0b: 8.03M calls; ~13.6 us a call sampled, docs/47 run 277 "uniforms gx_vs_build"). Three of its loops
 * are O(distinct vertices) scans over properties of the RECORD, not of the GX state: every vertex posOk, the normal / NBT counts, the matrix-slot
 * range (avg 127 distinct vertices a call, run_t0b.txt:22582 verts/calls). Arm 102 (and 104, 105 -- gStfxAb, perf_ab.txt) does them once per record state
 * and keeps the answer (vsWhy / vsNN / vsNB) in the record; dlkRefresh (the only writer of fl[] / pnm[] after the build) clears it. The refusal
 * reason is the SAME one the loops would give first (POS_NOT_OK, then MIXED_NRM, then SLOT_RANGE) and a refusal still counts in gx_vs_refused every call.
 * gx_c3_vs_us / gx_c3_vs_n time 1 call in 16 in EVERY arm (the local micro-timer); gx_c3_vsb_* are the proof-of-effect counters the [c3] line prints. */
unsigned gx_c3_vsb_calls, gx_c3_vsb_hits, gx_c3_vsb_inval, gx_c3_vs_n; unsigned long long gx_c3_vs_us;
int gx_c3_vsb_force;   /* R3a C0: set by vita_tool_main.c from the EFFECTIVE C3 arm (every threaded perf arm runs the release cut set, arm 107), so an arm-mode boot measures the release path */
static int c3VsbOn(void) { return gx_c3_vsb_force || (&gStfxAb && (gStfxAb == 102 || gStfxAb == 104 || gStfxAb == 105 || gStfxAb == 107 || gStfxAb == 109)); }
static void c3VsScan(DlkRec* r) {
    const u32 nd = r->ndist; u32 nN = 0, nB = 0; int why = 0;
    for (u32 d = 0; d < nd; ++d) { if (!(r->fl[d] & 1)) { why = VSW_POS_NOT_OK; break; } nN += (r->fl[d] >> 1) & 1; nB += (r->fl[d] >> 3) & 1; }
    if (!why && nN && nN != nd) why = VSW_MIXED_NRM;
    if (!why && r->pnm) for (u32 d = 0; d < nd; ++d) if (r->pnm[d] / 3 > VS_MAX_SLOTS - 1) { why = VSW_SLOT_RANGE; break; }
    r->vsWhy = (u8)why; r->vsNN = nN; r->vsNB = nB; r->vsScan = 1;
}
static int c3VsBuildTimed(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvp) {
    static unsigned ctr; dlk64 t = (++ctr & 15u) == 0 ? GXT_NOW() : 0;
    const int ok = gx_vs_build(r, tk, sc, k, u, mvp);
    if (t) { gx_c3_vs_us += GXT_NOW() - t; ++gx_c3_vs_n; }
    return ok;
}
/* GXAOT_HOOK_END C3 */
int gStfxGpuArm;            /* ux0:data/stairfax/gx_gpu.txt starting with 1 = ON. Off by default until a device run has judged it */
static int gPxOldGuess = -1;   /* pxChannelColour's chan_oldguess A/B, once it is known: the vertex program does not reproduce the old rule */
static struct { unsigned calls, refused, dynamic, noMesh, backendNo, meshes, updates; unsigned long long meshBytes, updBytes, updUs, us, verts, tris;
                unsigned long long vsUs, meshUs, stUs, drUs; } gDlkGpu;   /* hw_Rsplit (docs/54 2026-10-24): the GPU path split by stage (GXT_NOW: 0 when the timers are off) */
static unsigned long long gDlkPre;   /* hw_Rsplit: dlkCall from the record lookup's end to the GPU/CPU path (TEV key build, fog, texture checks) */
enum { DLK_WHY_OVERRUN = 1, DLK_WHY_TOOBIG, DLK_WHY_SPAN, DLK_WHY_WALK, DLK_WHY_OOM };
enum { DLK_NE_REALSKIN, DLK_NE_FORCE, DLK_NE_JOINT, DLK_NE_DECARM, DLK_NE_PERF, DLK_NE_NOTEV, DLK_NE_JOURNAL, DLK_NE_CENSUS,
       DLK_FB_UNCACHEABLE, DLK_FB_TEV, DLK_FB_BACKEND, DLK_FB_OOM, DLK_NFB };
static const char* const kDlkFbName[DLK_NFB] = { "!realSkin", "forceStride", "jointSkin", "decodeArm!=1", "perfArm!=0", "noTevPath", "journal/dbg",
                                                 "census on", "uncacheable", "TEV refused", "backend refused", "oom/too big" };
static struct { unsigned calls, hits, builds, misses, rebuilds, refreshes, evictions, recs, fb[DLK_NFB], stripV, distV, whyN[8];
               unsigned long long bytes, t[7]; } gDlk;   /* t: validate, decode, transform, light, texgen, expand, backend (microseconds) */
unsigned gx_dlk_counter(int id) {   /* host-test hook */
    switch (id) { case 0: return gDlk.calls; case 1: return gDlk.hits; case 2: return gDlk.builds; case 3: return gDlk.misses;
                  case 4: return gDlk.rebuilds; case 5: return gDlk.refreshes; case 6: return gDlk.evictions; case 7: return gDlk.recs;
                  case 8: return (unsigned)gDlk.bytes; case 9: return gDlk.stripV; case 10: return gDlk.distV;
                  default: return id >= 16 && id < 16 + DLK_NFB ? gDlk.fb[id - 16] : 0; }
}
static DlkRec* gDlkTab[DLK_SETS * DLK_WAYS];
static uint32_t gDlkFreeGen;   /* R3c C7: bumped by dlkFree (a record left the table): the last-hit record cache (dlkCall) dies with it */
static unsigned gDlkKeyBuilt, gDlkKeyReused;
static dlk64 gDlkClock;
static int gDlkInit;
static int gDlkEvt;          /* dl_events.txt (run 165): 0 every call in full, 1 by events, 2 events ignored (break arm) */
static int gDlkHashOld;     /* dl_hash_old.txt 1 = the single-chain hash (run 164 A/B); set before any record exists */
static int gDlkDynOld;      /* dlk_dyn_old.txt 1 = the 512-slot never-forgetting dyn table (pre-2026-10-12 A/B) */
static void dlkInitOnce(void) {   /* ux0:data/stairfax/dl_nocache.txt present = the cache off (A/B on the device); read once, logged once */
    gDlkInit = 1;
    (void)tmiOld();   /* texmtxidx_off.txt: read and logged here with the other markers (it is also read lazily by the first draw that needs it) */
    { FILE* ef = fopen("ux0:data/stairfax/dl_events.txt", "r"); int c = ef ? fgetc(ef) : 0; if (ef) fclose(ef); gDlkEvt = c == 49 ? 1 : c == 50 ? 2 : 0; }
    { FILE* df = fopen("ux0:data/stairfax/dlk_dyn_old.txt", "r"); gDlkDynOld = df && fgetc(df) == 49;   /* default = the new set; dlk_dyn_old.txt starting with 1 = the 512-slot table of runs 165-237 */ if (df) fclose(df);
      printf("[dlk] dyn-buffer set: %s\n", gDlkDynOld ? "OLD 512-slot table, never retired (dlk_dyn_old.txt 1)" : "sorted intervals, retired on free, never full (cap 1024; free ring 256)"); }
    { FILE* hf = fopen("ux0:data/stairfax/dl_hash_old.txt", "r"); gDlkHashOld = hf && fgetc(hf) == 49; if (hf) fclose(hf);
      printf("[dlk] span hash: %s\n", gDlkHashOld ? "single chain (dl_hash_old.txt)" : "four lanes"); }
    /* both markers must START WITH 1 (a 0 written over them turns them off without a delete) */
    FILE* f = fopen("ux0:data/stairfax/dl_nocache.txt", "r"); int off = f && fgetc(f) == 49;
    if (f) fclose(f);
    if (off) gStfxDlCacheArm = 0;
    printf("[dlk] display-list cache %s\n", off ? "OFF (dl_nocache.txt)" : "ON");
    /* gx_gpu.txt: 1 = lists whose arrays never change; 2 = also the lists whose arrays DO change (the morphed models): their
       vertex buffer is re-uploaded whenever the array hash changes. 2 is the A/B arm for that upload; anything else = off. */
    f = fopen("ux0:data/stairfax/gx_gpu.txt", "r"); { int c = f ? fgetc(f) : 0; gStfxGpuArm = c == 49 ? 1 : c == 50 ? 2 : 0; } if (f) fclose(f);
    printf("[dlk] GPU vertex path %s\n", gStfxGpuArm == 2 ? "ON, changing arrays too (gx_gpu.txt 2)" : gStfxGpuArm ? "ON, static lists only (gx_gpu.txt 1)" : "off");
}
// Word-at-a-time hash, 64 bits of state (two 32-bit lanes): runs on every call over the list and the array spans.
#define DLK_STEP(w) do { a = (a ^ (w)) * 0x9E3779B1u; a ^= a >> 15; c = (c + (w)) * 0x85EBCA6Bu; c = ((c << 7) | (c >> 25)) ^ a; } while (0)
static dlk64 gDlkVal[3];   /* run 163: the validate timer split three ways (gx_dlk_report) */
/* Run 164 (docs/47): DLK_STEP is ONE dependent chain (c waits on a), ~50 MB/s on the device, and the hash was 8.8 ms a frame
   of gameplay (run 163: list 4.9, arrays 3.9). Spans of 32 bytes and more now go 16 bytes a step through four INDEPENDENT
   lanes, folded into the two-lane state at the end; shorter spans and the tail are as before. Every byte is still hashed:
   the validation stays exact. ux0:data/stairfax/dl_hash_old.txt starting with 1 = the single chain throughout (A/B). */
static dlk64 dlkHash(dlk64 seed, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p; u32 a = (u32)seed, c = (u32)(seed >> 32);
    if (n >= 32 && !gDlkHashOld) {
        u32 l0 = a ^ 0x243F6A88u, l1 = c ^ 0x85A308D3u, l2 = a + 0x13198A2Eu, l3 = c + 0x03707344u;
        for (; n >= 16; n -= 16, b += 16) {
            u32 w[4]; memcpy(w, b, 16);
            l0 = (l0 ^ w[0]) * 0x9E3779B1u; l0 = (l0 << 13) | (l0 >> 19);
            l1 = (l1 ^ w[1]) * 0x85EBCA6Bu; l1 = (l1 << 11) | (l1 >> 21);
            l2 = (l2 ^ w[2]) * 0xC2B2AE35u; l2 = (l2 << 15) | (l2 >> 17);
            l3 = (l3 ^ w[3]) * 0x27D4EB2Fu; l3 = (l3 << 9) | (l3 >> 23);
        }
        DLK_STEP(l0); DLK_STEP(l1); DLK_STEP(l2); DLK_STEP(l3);
    }
    for (; n >= 4; n -= 4, b += 4) { u32 w; memcpy(&w, b, 4); DLK_STEP(w); }
    if (n) { u32 w = 0; memcpy(&w, b, n); w ^= (u32)n << 24; DLK_STEP(w); }
    return (dlk64)c << 32 | a;
}
static void dlkKeyNow(DlkKey* k) {
    memset(k, 0, sizeof *k);
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) {
        k->desc[a] = (u8)gDesc[a];
        if (gDesc[a] == GX_INDEX8 || gDesc[a] == GX_INDEX16) { k->base[a] = gArray[a].base; k->stride[a] = gArray[a].stride; k->lim[a] = idxLimit(&gArray[a]); }
    }
}
static int dlkOps(DecOp* ops, GXVtxFmt fmt, const unsigned* lim) {
    GXAttr plan[GX_VA_MAX_ATTR]; int planN = 0;
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) if (gDesc[a] != GX_NONE) plan[planN++] = a;
    return buildOps(ops, plan, planN, fmt, lim);
}
// One vertex record, decoded by decodeOps and captured the way decodePrim captures it (gVObj / gVNrm0 / gVTex / out).
static int dlkDecodeOne(const DecOp* ops, int nops, const unsigned char* src, int* c, float pos[3], float nrm[3], u32* clr, int* pnm, int* haveN, float texv[8][2],
                        float nbt[6], unsigned char tmi[8], int* haveNbt) {
    RhiTexVertex out; out.x = out.y = out.z = 0; out.rgba = 0xFFFFFFFFu; out.u = out.v = 0; gCurHaveNrm = 0; gCurHaveNbt = 0;
    memset(gCurTex, 0, sizeof gCurTex); memset(gCurTmi, 0, sizeof gCurTmi);
    int pn = 0, ht = 0, ok = decodeOps(ops, nops, src, c, &out, &ht, &pn);
    pos[0] = out.x; pos[1] = out.y; pos[2] = out.z; *clr = out.rgba; *pnm = pn; *haveN = gCurHaveNrm;
    if (gCurHaveNrm) { nrm[0] = gCurNrm[0]; nrm[1] = gCurNrm[1]; nrm[2] = gCurNrm[2]; } else nrm[0] = nrm[1] = nrm[2] = 0.0f;
    memcpy(texv, gCurTex, sizeof gCurTex);
    memcpy(nbt, gCurHaveNbt ? gCurNbt : kNbtDefault, sizeof kNbtDefault); *haveNbt = gCurHaveNbt;
    memcpy(tmi, gCurTmi, 8);
    return ok;
}
static dlk64 dlkArrHash(const DlkRec* r) {
    dlk64 h = 0x2545F4914F6CDD1Dull;
    for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) {
        const unsigned char* b = r->key.base[a] + (size_t)r->spanLo[a] * (size_t)r->key.stride[a];
        h = dlkHash(h ^ (dlk64)a, b, (size_t)(r->spanHi[a] - r->spanLo[a]) * (size_t)r->key.stride[a] + r->spanElem[a]);
    }
    return h;
}
static int dlkElemRead(const DecOp* o) {   /* bytes decodeOps reads from one array element */
    int ts = compTypeSize(o->type);
#ifdef BREAK_20   /* host negative control: the array hash covers only the normal of an NBT element (a binormal / tangent change goes unseen) */
    switch (o->kind) { case OP_POS: return ts * o->n; case OP_NRM: return ts * 3; case OP_CLR0: return colorSize(o->type);
#else
    switch (o->kind) { case OP_POS: return ts * o->n; case OP_NRM: return ts * (o->n == 9 ? 9 : 3); case OP_CLR0: return colorSize(o->type);
#endif
                       case OP_TEX0: case OP_TEXN: return ts * o->n; default: return 0; }
}
// Expand one primitive through the map, in expandIdx's order, dropping triangles with a !posOk vertex. out NULL = count only.
static u32 dlkExpand(const DlkRec* r, const DlkPrim* P, const TevVertex* dtv, TevVertex* out) {
    const u16* m = r->map + P->mapOff; const u8* fl = r->fl; u32 n = 0, nv = P->nv;
#define DLK_TRI(a, b, c) do { u32 A = m[a], B = m[b], C = m[c]; if (fl[A] & fl[B] & fl[C] & 1) { if (out) { out[n] = dtv[A]; out[n + 1] = dtv[B]; out[n + 2] = dtv[C]; } n += 3; } } while (0)
    if (P->prim == GX_TRIANGLES) { for (u32 i = 0; i + 3 <= nv; i += 3) DLK_TRI(i, i + 1, i + 2); }
    else if (P->prim == GX_TRIANGLESTRIP) { for (u32 i = 2; i < nv; ++i) { if (i & 1) DLK_TRI(i - 1, i - 2, i); else DLK_TRI(i - 2, i - 1, i); } }
    else if (P->prim == GX_TRIANGLEFAN) { for (u32 i = 2; i < nv; ++i) DLK_TRI(0, i - 1, i); }
    else if (P->prim == GX_QUADS) { for (u32 i = 0; i + 4 <= nv; i += 4) { DLK_TRI(i, i + 1, i + 2); DLK_TRI(i, i + 2, i + 3); } }
#undef DLK_TRI
    return n;
}
// build scratch (grown, never shrunk)
static struct { float *pos, *nrm, *tex, *nbt; u32 *clr, *voff, *vlen, *next, *map; u8 *fl, *pnm, *fmt, *tmi; DlkPrim* prim;
                size_t capD, capS, capP; } gDs;
static u32 gDlkHead[1 << 16]; static unsigned short gDlkHeadGen[1 << 16], gDlkGenNow;
#define DLK_GROW(p, cap, need, esz) do { if ((size_t)(need) > (cap)) { size_t nc_ = (cap) ? (cap) : 512; while (nc_ < (size_t)(need)) nc_ *= 2; \
        void* q_ = realloc((p), nc_ * (esz)); if (!q_) { fail = 1; } else { (p) = q_; (cap) = nc_; } } } while (0)
static int dlkGrowD(size_t need) {   /* the per-distinct scratch arrays grow together */
    if (need <= gDs.capD) return 1;
    size_t nc = gDs.capD ? gDs.capD : 512; while (nc < need) nc *= 2;
#define DLK_G(p, esz) do { void* q_ = realloc((p), nc * (esz)); if (!q_) return 0; (p) = q_; } while (0)
    DLK_G(gDs.pos, 12); DLK_G(gDs.nrm, 12); DLK_G(gDs.tex, 64); DLK_G(gDs.clr, 4); DLK_G(gDs.voff, 4); DLK_G(gDs.vlen, 4); DLK_G(gDs.next, 4);
    DLK_G(gDs.fl, 1); DLK_G(gDs.pnm, 1); DLK_G(gDs.fmt, 1); DLK_G(gDs.nbt, 24); DLK_G(gDs.tmi, 8);
#undef DLK_G
    gDs.capD = nc; return 1;
}
static void dlkFree(DlkRec* r) {
    if (!r) return;
    if (r->gpuMesh && rhi_vita_gx_mesh_destroy) { rhi_vita_gx_mesh_destroy(r->gpuMesh); --gDlkGpu.meshes; }
    free(r->fidx); free(r->runs);
    ++gDlkFreeGen;   /* R3c C7: the last-hit record cache dies with any record */
    gDlk.bytes -= r->bytes; --gDlk.recs; free(r);
}
static DlkRec* dlkBuild(const unsigned char* dl, u32 nbytes) {
    DlkRec h; memset(&h, 0, sizeof h); h.dl = dl; h.nbytes = nbytes; dlkKeyNow(&h.key); h.hDl = dlkHash(0x9E3779B97F4A7C15ull, dl, nbytes);
    DecOp ops[DLK_FMTS][GX_VA_MAX_ATTR]; int nops[DLK_FMTS]; unsigned built = 0;
    h.hasNrm = gDesc[GX_VA_NRM] != GX_NONE; h.hasClr = gDesc[GX_VA_CLR0] != GX_NONE; h.hasPnm = gDesc[GX_VA_PNMTXIDX] != GX_NONE;
    for (int q = 0; q < 8; ++q) { if (gDesc[GX_VA_TEX0 + q] != GX_NONE) { h.texIdx[q] = (signed char)h.ntex; h.texAttr[h.ntex++] = (u8)q; } else h.texIdx[q] = -1; }
    for (int q = 0; q < 8; ++q) { if (gDesc[GX_VA_TEX0MTXIDX + q] != GX_NONE) { h.tmiIdx[q] = (signed char)h.ntmi++; h.tmiMask |= (u8)(1u << q); } else h.tmiIdx[q] = -1; }
    const int nt = h.ntex, ntmi = h.ntmi; int fail = 0, why = 0, anyNbt = 0;
    u32 nPrim = 0, nStrip = 0, nDist = 0, i = 0;
    if (!++gDlkGenNow) { memset(gDlkHeadGen, 0, sizeof gDlkHeadGen); gDlkGenNow = 1; }
    while (i < nbytes && !why && !fail) {
        u8 op = dl[i++];
        if (op == 0x00) continue;
        u8 prim = op & 0xF8, fmt = op & 0x07;
        if (!(prim == GX_QUADS || prim == GX_TRIANGLES || prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN || prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS)) break;
        if (i + 2 > nbytes) break;
        int nverts = (int)((dl[i] << 8) | dl[i + 1]); i += 2;
        if (nverts > GX_MAX_BATCH) nverts = GX_MAX_BATCH;
        const int declared = nverts, maxBytes = (int)(nbytes - i);
        if (maxBytes == 0 && nverts > 0) { why = DLK_WHY_OVERRUN; break; }   /* decodePrim skips its end test when maxBytes is 0 and reads past the list */
        if (!(built >> fmt & 1)) { nops[fmt] = dlkOps(ops[fmt], (GXVtxFmt)fmt, h.key.lim); built |= 1u << fmt; }
        h.fmtMask |= (u8)(1u << fmt);
        DLK_GROW(gDs.prim, gDs.capP, nPrim + 1, sizeof *gDs.prim); if (fail) break;
        DlkPrim* P = &gDs.prim[nPrim++]; memset(P, 0, sizeof *P); P->mapOff = nStrip; P->prim = prim; P->declared = (u32)declared;
        DLK_GROW(gDs.map, gDs.capS, nStrip + (size_t)nverts, sizeof *gDs.map); if (fail) break;
        int c = 0, vi;
        for (vi = 0; vi < nverts; ++vi) {
            if (maxBytes > 0 && c >= maxBytes) { nverts = vi; break; }   /* decodePrim: don't read past the DL */
            const int c0 = c; float pos[3], nrm[3], texv[8][2], nbtv[6]; unsigned char tmiv[8]; u32 clr; int pn, haveN, haveNbt;
            int posOk = dlkDecodeOne(ops[fmt], nops[fmt], dl + i, &c, pos, nrm, &clr, &pn, &haveN, texv, nbtv, tmiv, &haveNbt);
            if (i + (u32)c > nbytes) { why = DLK_WHY_OVERRUN; break; }   /* this vertex reads beyond the list: not reproducible from the list hash */
            const unsigned len = (unsigned)(c - c0); const unsigned char* vb = dl + i + c0;
            u32 hv = 2166136261u ^ fmt; for (unsigned q = 0; q < len; ++q) hv = (hv ^ vb[q]) * 16777619u;
            u32 bk = (hv ^ (hv >> 16)) & 0xffff, d = gDlkHeadGen[bk] == gDlkGenNow ? gDlkHead[bk] : 0xffffffffu;
            while (d != 0xffffffffu) { if (gDs.fmt[d] == fmt && gDs.vlen[d] == len && !memcmp(dl + gDs.voff[d], vb, len)) break; d = gDs.next[d]; }
            if (d == 0xffffffffu) {
                if (nDist >= DLK_MAXD) { why = DLK_WHY_TOOBIG; break; }
                if (!dlkGrowD(nDist + 1)) fail = 1;
                if (fail) break;
                d = nDist++;
                memcpy(gDs.pos + 3 * d, pos, sizeof pos); memcpy(gDs.nrm + 3 * d, nrm, sizeof nrm);
                for (int j = 0; j < nt; ++j) { gDs.tex[(size_t)d * 16 + 2 * j] = texv[h.texAttr[j]][0]; gDs.tex[(size_t)d * 16 + 2 * j + 1] = texv[h.texAttr[j]][1]; }
                gDs.clr[d] = clr; gDs.voff[d] = i + (u32)c0; gDs.vlen[d] = len; gDs.fmt[d] = fmt;
                gDs.fl[d] = (u8)((posOk ? 1 : 0) | (haveN ? 2 : 0) | (haveNbt ? 8 : 0) | (fmt << 4)); gDs.pnm[d] = (u8)pn;
                memcpy(gDs.nbt + 6 * (size_t)d, nbtv, sizeof nbtv); memcpy(gDs.tmi + 8 * (size_t)d, tmiv, 8); anyNbt |= haveNbt;
                for (int q = 0; q < 8; ++q) if (h.tmiMask >> q & 1) { const unsigned r_ = tmiv[q]; if (r_ < 30 || r_ > 57 || (r_ - 30) % 3) h.tmiBad |= (u8)(1u << q); }
                gDs.next[d] = gDlkHeadGen[bk] == gDlkGenNow ? gDlkHead[bk] : 0xffffffffu; gDlkHead[bk] = d; gDlkHeadGen[bk] = gDlkGenNow;
            }
            gDs.map[nStrip + (u32)vi] = d;
        }
        if (why || fail) break;
        P->nv = (u32)nverts; nStrip += (u32)nverts; i += (u32)c;
    }
    /* per-attribute index spans (what the array hash covers), re-walking each distinct record's raw bytes with the same ops;
       the walk must land exactly on the record's length, which proves attrSize and decodeOps agree for this format */
    if (!why && !fail) for (u32 d = 0; d < nDist && !why; ++d) {
        const unsigned char* vb = dl + gDs.voff[d]; int fmt = gDs.fmt[d], c = 0;
        for (int k = 0; k < nops[fmt]; ++k) {
            const DecOp* o = &ops[fmt][k];
            if (o->kind == OP_MTX) { c += 1; continue; }
            if (o->idxBytes) {
                unsigned idx = o->idxBytes == 1 ? vb[c] : be16u(vb + c); c += o->idxBytes;
                int el = dlkElemRead(o);
                if (idx < o->lim && el > 0) {
                    int a = o->attr;
                    if (!h.spanOn[a]) { h.spanOn[a] = 1; h.spanLo[a] = h.spanHi[a] = idx; }
                    if (idx < h.spanLo[a]) h.spanLo[a] = idx;
                    if (idx > h.spanHi[a]) h.spanHi[a] = idx;
                    if ((u32)el > h.spanElem[a]) h.spanElem[a] = (u32)el;
                }
            } else c += attrSize((GXVtxFmt)fmt, (GXAttr)o->attr);
        }
        if ((unsigned)c != gDs.vlen[d]) why = DLK_WHY_WALK;
    }
    if (!why && !fail) for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (h.spanOn[a]) {
        dlk64 sp = (dlk64)(h.spanHi[a] - h.spanLo[a]) * (dlk64)(unsigned)h.key.stride[a] + h.spanElem[a];
        if (sp > DLK_MAXSPAN) why = DLK_WHY_SPAN;
    }
    if (fail) { ++gDlk.fb[DLK_FB_OOM]; return NULL; }
    for (int f = 0; f < DLK_FMTS; ++f) if (h.fmtMask >> f & 1) memcpy(h.vat[f], gVat[f], sizeof h.vat[f]);
    h.state = why ? 2 : 1; h.why = (u8)why;
    size_t szHdr = (sizeof(DlkRec) + 7) & ~(size_t)7, total = szHdr;
    size_t oPrim = total; if (!why) total += nPrim * sizeof(DlkPrim);
    size_t oPos = total; if (!why) total += (size_t)nDist * 12;
    size_t oNrm = total; if (!why && h.hasNrm) total += (size_t)nDist * 12;
    size_t oTex = total; if (!why) total += (size_t)nDist * nt * 8;
    size_t oNbt = total; if (!why && anyNbt) total += (size_t)nDist * 24;
    size_t oClr = total; if (!why && h.hasClr) total += (size_t)nDist * 4;
    size_t oVoff = total; if (!why) total += (size_t)nDist * 4;
    size_t oMap = total; if (!why) total += ((size_t)nStrip * 2 + 3) & ~(size_t)3;
    size_t oFl = total; if (!why) total += ((size_t)nDist + 3) & ~(size_t)3;
    size_t oPnm = total; if (!why && h.hasPnm) total += (size_t)nDist;
    size_t oTmi = total; if (!why && ntmi) total += (size_t)nDist * ntmi;
    unsigned char* blob = (unsigned char*)malloc(total);
    if (!blob) { ++gDlk.fb[DLK_FB_OOM]; return NULL; }
    DlkRec* r = (DlkRec*)blob; *r = h; r->bytes = (u32)total;
    if (!why) {
        r->nprim = nPrim; r->nstrip = nStrip; r->ndist = nDist;
        r->prim = (DlkPrim*)(blob + oPrim); memcpy(r->prim, gDs.prim, nPrim * sizeof(DlkPrim));
        r->pos = (float*)(blob + oPos); memcpy(r->pos, gDs.pos, (size_t)nDist * 12);
        r->nrm = h.hasNrm ? (float*)(blob + oNrm) : NULL; if (r->nrm) memcpy(r->nrm, gDs.nrm, (size_t)nDist * 12);
        r->nbt = anyNbt ? (float*)(blob + oNbt) : NULL; if (r->nbt) memcpy(r->nbt, gDs.nbt, (size_t)nDist * 24);
        r->tmi = ntmi ? blob + oTmi : NULL;
        if (r->tmi) for (u32 d = 0; d < nDist; ++d) for (int q = 0; q < 8; ++q) if (h.tmiIdx[q] >= 0) r->tmi[(size_t)d * ntmi + h.tmiIdx[q]] = gDs.tmi[8 * (size_t)d + q];
        r->tex = (float*)(blob + oTex);
        for (u32 d = 0; d < nDist; ++d) for (int j = 0; j < nt; ++j) { r->tex[((size_t)d * nt + j) * 2] = gDs.tex[(size_t)d * 16 + 2 * j]; r->tex[((size_t)d * nt + j) * 2 + 1] = gDs.tex[(size_t)d * 16 + 2 * j + 1]; }
        r->clr = h.hasClr ? (u32*)(blob + oClr) : NULL; if (r->clr) memcpy(r->clr, gDs.clr, (size_t)nDist * 4);
        r->voff = (u32*)(blob + oVoff); memcpy(r->voff, gDs.voff, (size_t)nDist * 4);
        r->map = (u16*)(blob + oMap); for (u32 s = 0; s < nStrip; ++s) r->map[s] = (u16)gDs.map[s];
        r->fl = blob + oFl; memcpy(r->fl, gDs.fl, nDist);
        r->pnm = h.hasPnm ? blob + oPnm : NULL; if (r->pnm) memcpy(r->pnm, gDs.pnm, nDist);
        r->hArr = dlkArrHash(r);
        for (u32 p = 0; p < nPrim; ++p) {
            r->prim[p].ntri = dlkExpand(r, &r->prim[p], NULL, NULL) / 3;
            r->ntri += r->prim[p].ntri; r->declaredSum += r->prim[p].declared;
            if (r->prim[p].ntri) { r->stripVTri += r->prim[p].nv; ++r->nprimTri; }
        }
    }
    ++gDlk.recs; gDlk.bytes += r->bytes; ++gDlk.builds;
    if (why) ++gDlk.whyN[why & 7];
    return r;
}
// The array bytes changed under an unchanged list: re-decode the vertex attributes, keep the primitives and the map.
static void dlkRefresh(DlkRec* r) {
    r->vsScan = 0;   /* GXAOT_HOOK C3 arm 102: fl[] / pnm[] are rewritten below */
    DecOp ops[DLK_FMTS][GX_VA_MAX_ATTR]; int nops[DLK_FMTS]; unsigned built = 0; const int nt = r->ntex;
    for (u32 d = 0; d < r->ndist; ++d) {
        int fmt = r->fl[d] >> 4 & 7;
        if (!(built >> fmt & 1)) { nops[fmt] = dlkOps(ops[fmt], (GXVtxFmt)fmt, r->key.lim); built |= 1u << fmt; }
        float pos[3], nrm[3], texv[8][2], nbtv[6]; unsigned char tmiv[8]; u32 clr; int pn, haveN, haveNbt, c = 0;
        int posOk = dlkDecodeOne(ops[fmt], nops[fmt], r->dl + r->voff[d], &c, pos, nrm, &clr, &pn, &haveN, texv, nbtv, tmiv, &haveNbt);
        memcpy(r->pos + 3 * d, pos, sizeof pos); if (r->nrm) memcpy(r->nrm + 3 * d, nrm, sizeof nrm);
        if (r->nbt) memcpy(r->nbt + 6 * d, nbtv, sizeof nbtv);   /* (the TEXnMTXIDX bytes are list bytes: they cannot have changed under an unchanged list) */
        for (int j = 0; j < nt; ++j) { r->tex[((size_t)d * nt + j) * 2] = texv[r->texAttr[j]][0]; r->tex[((size_t)d * nt + j) * 2 + 1] = texv[r->texAttr[j]][1]; }
        if (r->clr) r->clr[d] = clr;
        if (r->pnm) r->pnm[d] = (u8)pn;
        r->fl[d] = (u8)((posOk ? 1 : 0) | (haveN ? 2 : 0) | (haveNbt ? 8 : 0) | (fmt << 4));
    }
}
static void dlkEvict(DlkRec** slot) { dlkFree(*slot); *slot = NULL; ++gDlk.evictions; }
static TevVertex* gDlkDtv; static size_t gDlkDtvCap;
static TevVertex* gDlkOut; static size_t gDlkOutCap;
static float* gDlkNrmT; static size_t gDlkNrmTCap;
// 1: the call was handled here (drawn, or deliberately nothing, exactly as the per-primitive loop would). 0: run today's loop.
// ---- the FAST array refresh (2026-10-08, docs/47 "Run 137") ---------------------------------------------------------------
// Run 137: with every list on the GPU path the cache's CPU side was still 23 ms a frame, 17 of it "decode" -- the morphed
// models' arrays change every frame and dlkRefresh re-decoded EVERY attribute of every distinct vertex through decodeOps.
// What changes is the POSITION and NORMAL array; the list bytes (so every index) and the other arrays do not. So: one hash per
// attribute's span (dlkArrChanged says WHICH arrays changed), and when only POS/NRM did, re-read just those through the indices
// saved per distinct vertex. Same scal() arithmetic as decodeOps, and the in-bounds verdicts cannot move (the indices and the
// limits are in the key), so the floats are the ones the full refresh would produce -- and the FIRST fast refresh of every record
// is checked against a full one (gDlkFastBad counts a mismatch and that record never takes the fast path again).
static unsigned gDlkFastN, gDlkFullN, gDlkFastBad;
// WHAT is hashed (2026-10-08, docs/47 "Run 139"). Each attribute's span was [lowest index, highest index] of its array. A model's
// lists SHARE its arrays and each list touches a scattered part, so every list hashed most of the model: run 139's prologue hashed
// ~4 MB a frame (20 ms) to validate 22K distinct vertices -- ~0.44 MB of referenced data. Now each record keeps, per attribute, the
// RUNS of indices it really references (neighbours closer than 8 elements merged) and hashes those. Still a superset of what the
// record reads, so a change it depends on cannot be missed. -DDLK_NORUNS = whole spans again (the A/B, and the host comparison).
static unsigned long long gDlkHashBytes; static unsigned gDlkRunRecs, gDlkRunNo;
static void dlkRunsBuild(DlkRec* r) {
    r->runState = -1;
#ifndef DLK_NORUNS
    size_t tot = 0; u32 off[GX_VA_MAX_ATTR];
    for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) { off[a] = (u32)tot; tot += (size_t)(r->spanHi[a] - r->spanLo[a]) + 1; }
    if (!tot || tot > (1u << 22)) { ++gDlkRunNo; return; }
    u8* mark = (u8*)calloc(tot, 1);
    if (!mark) { ++gDlkRunNo; return; }
    DecOp ops[DLK_FMTS][GX_VA_MAX_ATTR]; int nops[DLK_FMTS]; unsigned built = 0;
    for (u32 d = 0; d < r->ndist; ++d) {
        int fmt = r->fl[d] >> 4 & 7;
        if (!(built >> fmt & 1)) { nops[fmt] = dlkOps(ops[fmt], (GXVtxFmt)fmt, r->key.lim); built |= 1u << fmt; }
        const unsigned char* src = r->dl + r->voff[d]; int c = 0;
        for (int i = 0; i < nops[fmt]; ++i) {
            const DecOp* o = &ops[fmt][i];
            if (o->kind == OP_MTX) { c += 1; continue; }
            if (o->idxBytes) {
                u32 idx = o->idxBytes == 1 ? src[c] : be16u(src + c); c += o->idxBytes;
                const int a = o->attr;
                if (r->spanOn[a] && idx >= r->spanLo[a] && idx <= r->spanHi[a]) mark[off[a] + (idx - r->spanLo[a])] = 1;
                continue;
            }
            if (o->kind == OP_GENERIC) { free(mark); ++gDlkRunNo; return; }
            if (o->kind == OP_CLR0 || o->kind == OP_CLR1) c += colorSize(o->type);
            else c += compTypeSize(o->type) * o->n;
        }
    }
    /* two passes over the marks: count the runs, then write them as (first index, count) pairs */
    u32* runs = NULL; u32 nr = 0;
    for (int pass = 0; pass < 2; ++pass) {
        u32 n = 0;
        for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) {
            const u32 len = r->spanHi[a] - r->spanLo[a] + 1; const u8* m = mark + off[a]; u32 first = n, i = 0;
            while (i < len) {
                if (!m[i]) { ++i; continue; }
                u32 s = i, e = i;                                   /* e: the last marked element of this run */
                for (u32 j = i + 1; j < len && j - e <= 8; ++j) if (m[j]) e = j;
                if (pass) { runs[2 * n] = r->spanLo[a] + s; runs[2 * n + 1] = e - s + 1; }
                ++n; i = e + 1;
            }
            if (pass) { r->runOff[a] = first; r->runN[a] = n - first; }
        }
        if (!pass) { nr = n; runs = (u32*)malloc((size_t)nr * 2 * sizeof(u32) + 8); if (!runs) { free(mark); ++gDlkRunNo; return; } }
    }
    free(mark);
    r->runs = runs; r->runState = 1; ++gDlkRunRecs;
#endif
}
// ---- VALIDATION BY EVENTS (run 165, docs/47) ----------------------------------------------------------------------
// Run 163: re-reading every list and every array span on every call is ~8.5 ms a frame of gameplay, and a 4x faster hash
// took only 1.5 of it (run 164): the cost is touching the memory. A record whose list / arrays matched on DLK_EV_TRUST
// calls in a row is not re-read, UNTIL an EVENT overlaps what it reads: a freed heap block (mm_free) or a block written
// through the game's cache write-back (memcpyToCache: the morph and CPU-skinning outputs). What this does NOT see is a
// write by any other route into data that has sat still; for that, one call in DLK_EV_VERIFY is validated in full
// whatever its trust, and a difference found THERE is counted as a MISS and logged -- that count must stay 0.
// ux0:data/stairfax/dl_events.txt: 1 = on; 2 = on with the events IGNORED (the break arm: misses must appear on a
// morphing model); anything else / absent = every call validated in full (runs 132-164).
#define DLK_EV_TRUST  8
#define DLK_EV_VERIFY 16
#define DLK_EV_RING   256   /* was 64: 14,202 of 19,869 frees overflowed it in ThornTail (run 237); dlk_dyn_old.txt 1 keeps 64 */
static struct { const unsigned char *lo, *hi; } gDlkEvRing[DLK_EV_RING];
#define DLK_RING_N (gDlkDynOld ? 64u : (u32)DLK_EV_RING)
static u32 gDlkEvSeq, gDlkEvCall;
/* R6 C8 (host/engine/R6_SKIN.md s1): the WRITE ring.  gx_dlk_event(p, n, 1) appends (lo, hi) while C8 is on; gDlkWrSeq counts the appends.  A record keeps the sequence at which its hashes last matched (wrSeq, valid for gDlkWrEpoch: the epoch
   moves when C8 is switched, because writes announced while it was off are not in the ring).  Counters are cumulative, printed as the [r6] line. */
#define DLK_WR_RING 2048
static struct { const unsigned char *lo, *hi; } gDlkWrRing[DLK_WR_RING];
static u32 gDlkWrSeq, gDlkWrEpoch = 1;
unsigned gx_r6_c8_calls, gx_r6_c8_unch, gx_r6_c8_pred, gx_r6_c8_hash, gx_r6_c8_ovf, gx_r6_c8_miss;   /* dyn calls decided by C8 (not forced-verify), of them unchanged / predicted / hashed anyway, ring overflows, unannounced changes */
unsigned gx_r6_c9_part, gx_r6_c9_fresh, gx_r6_c9_ref;                                                  /* mesh updates written partially (POS / NRM) / in full / refused by the backend */
static struct { u32 frees, writes, overflow, touched, skipList, skipArr, forced, miss, dynCalls; } gDlkEv;
/* Run 165 put the write-backs in the ring too: ~99 a frame in gameplay against 64 slots, so the ring overflowed EVERY frame
   and every record was "touched" (398,705 overflows, nothing skipped). The write-backs hit the same buffers every frame, so
   they are a SET, not a stream: a buffer that has ever been written is DYNAMIC, a record reading one always hashes its
   arrays (its list is still trusted), and the ring carries the frees alone. A new buffer bumps gDlkDynGen and every
   record re-checks itself once. A full table makes every record dynamic (safe, and counted). */
#define DLK_DYN_SLOTS 512
static struct { const unsigned char *lo, *hi; } gDlkDyn[DLK_DYN_SLOTS];
static u32 gDlkDynN, gDlkDynGen = 1, gDlkDynFull;
#include "dlk_dyn.h"   /* 2026-10-12: the new set (sorted intervals, retired on free, never full) -- see the header for the failure it fixes */
static DlkDyn gDlkDyn2;
static u32 gDlkFreesDyn;   /* frees that retired or trimmed at least one dynamic interval */
void gx_dlk_event(const void* p, size_t n, int isWrite) {
    if (!gDlkEvt || !p || !n) return;
    const unsigned char *lo = (const unsigned char*)p, *hi = lo + n;
    if (!isWrite) {
        gDlkEvRing[gDlkEvSeq % DLK_RING_N].lo = lo; gDlkEvRing[gDlkEvSeq % DLK_RING_N].hi = hi;
        ++gDlkEvSeq; ++gDlkEv.frees;
        if (!gDlkDynOld) { const u32 r0 = gDlkDyn2.retired + gDlkDyn2.trimmed; dlkDynFree(&gDlkDyn2, lo, hi); if (gDlkDyn2.retired + gDlkDyn2.trimmed != r0) { ++gDlkFreesDyn; gDlkDynN = gDlkDyn2.n; } }
        return;
    }
    ++gDlkEv.writes;
#ifndef C8_BREAK_NOWRITE   /* host break arm (C8_BREAK_NOWRITE): the write ring drops every announced write, so a dynamic record is predicted unchanged for ever: the stream must go stale or misses must appear */
    if ((gx_r3a & GX_R3A_C8) && gDlkEvt == 1) { gDlkWrRing[gDlkWrSeq % DLK_WR_RING].lo = lo; gDlkWrRing[gDlkWrSeq % DLK_WR_RING].hi = hi; ++gDlkWrSeq; }
#endif
    if (!gDlkDynOld) { if (dlkDynWrite(&gDlkDyn2, lo, hi)) { ++gDlkDynGen; gDlkDynN = gDlkDyn2.n; } return; }
    u32 h = ((u32)((size_t)lo >> 5) * 2654435761u) >> 23;   /* 9 bits */
    for (u32 q = 0; q < DLK_DYN_SLOTS; ++q, h = (h + 1) & (DLK_DYN_SLOTS - 1)) {
        if (gDlkDyn[h].lo == lo) { if (hi > gDlkDyn[h].hi) { gDlkDyn[h].hi = hi; ++gDlkDynGen; } return; }
        if (!gDlkDyn[h].lo) { gDlkDyn[h].lo = lo; gDlkDyn[h].hi = hi; ++gDlkDynN; ++gDlkDynGen; return; }
    }
    if (!gDlkDynFull) { gDlkDynFull = 1; ++gDlkDynGen; }
}
/* host-test hooks (gx_port_test.c's events soak): turn validation-by-events on, pick the dyn set, read the counters */
void gx_dlk_setEvents(int mode, int dynOld) { gDlkEvt = mode; gDlkDynOld = dynOld; }
unsigned gx_dlk_evcounter(int id) { switch (id) { case 0: return gDlkEv.miss; case 1: return gDlkDyn2.n; case 2: return gDlkEv.dynCalls; case 3: return gDlkEv.skipArr;
                                                  case 4: return gDlkEv.skipList; case 5: return gDlkEv.overflow; case 6: return gDlkEv.touched; case 7: return gDlkDyn2.coarse;
                                                  case 8: return gx_r6_c8_unch; case 9: return gx_r6_c8_pred; case 10: return gx_r6_c8_hash; case 11: return gx_r6_c8_ovf; case 12: return gx_r6_c8_calls; case 13: return gx_r6_c8_miss;
                                                  case 14: return gx_r6_c9_part; case 15: return gx_r6_c9_fresh; case 16: return gx_r6_c9_ref; default: return 0; } }
static int dlkDynReads(const DlkRec* r) {   /* does this record read an array span inside a buffer that is ever written? */
    if (gDlkEvt == 2) return 0;
    if (gDlkDynFull) return 1;
    for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) {
        const unsigned char* b = r->key.base[a] + (size_t)r->spanLo[a] * (size_t)r->key.stride[a];
        const unsigned char* t = r->key.base[a] + (size_t)r->spanHi[a] * (size_t)r->key.stride[a] + r->spanElem[a];
        if (!gDlkDynOld) { if (dlkDynOverlaps(&gDlkDyn2, b, t)) return 1; continue; }
        for (u32 q = 0; q < DLK_DYN_SLOTS; ++q) if (gDlkDyn[q].lo && b < gDlkDyn[q].hi && gDlkDyn[q].lo < t) return 1;
    }
    return 0;
}
static int dlkEvTouched(DlkRec* r) {   /* has an event since this record's last call overlapped its list or an array span it reads? */
    const u32 since = gDlkEvSeq - r->evSeq;
    if (!since) return 0;
    r->evSeq = gDlkEvSeq;
    if (gDlkEvt == 2) return 0;
    if (since > DLK_RING_N) { ++gDlkEv.overflow; return 1; }
    for (u32 e = gDlkEvSeq - since; e != gDlkEvSeq; ++e) {
        const unsigned char *lo = gDlkEvRing[e % DLK_RING_N].lo, *hi = gDlkEvRing[e % DLK_RING_N].hi;
        if (r->dl < hi && lo < r->dl + r->nbytes) return 1;
        for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) {
            const unsigned char* b = r->key.base[a] + (size_t)r->spanLo[a] * (size_t)r->key.stride[a];
            const unsigned char* t = r->key.base[a] + (size_t)r->spanHi[a] * (size_t)r->key.stride[a] + r->spanElem[a];
            if (b < hi && lo < t) return 1;
        }
    }
    return 0;
}
static void dlkEvMiss(const DlkRec* r, const char* what, unsigned bits) {
    if (gDlkEv.miss++ < 24) printf("[dlk] EVENT MISS #%u: a trusted %s changed with no event (list %p, %u bytes, attrs %x, refreshes %u)\n", gDlkEv.miss, what, (void*)r->dl, r->nbytes, bits, r->nRefresh);
}
/* R6 C8: the attribute bits whose read span overlaps a write announced since sequence `from` (the caller has checked that the ring still holds them all) */
static unsigned dlkWrOverlap(const DlkRec* r, u32 from) {
    unsigned ch = 0;
    for (u32 e = from; e != gDlkWrSeq; ++e) {
        const unsigned char *lo = gDlkWrRing[e % DLK_WR_RING].lo, *hi = gDlkWrRing[e % DLK_WR_RING].hi;
        for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a] && !(ch >> a & 1)) {
            const unsigned char* b = r->key.base[a] + (size_t)r->spanLo[a] * (size_t)r->key.stride[a];
            const unsigned char* t = r->key.base[a] + (size_t)r->spanHi[a] * (size_t)r->key.stride[a] + r->spanElem[a];
            if (b < hi && lo < t) ch |= 1u << a;
        }
    }
    return ch;
}
static unsigned dlkArrChanged(DlkRec* r) {   /* bit a: the part of attribute a's array this record reads differs from the last call's (and remember it) */
    unsigned ch = 0;
    if (!r->runState) dlkRunsBuild(r);
    for (int a = 0; a < GX_VA_MAX_ATTR; ++a) if (r->spanOn[a]) {
        const unsigned char* base = r->key.base[a]; const size_t st = (size_t)r->key.stride[a];
        dlk64 h = 0x2545F4914F6CDD1Dull ^ (dlk64)a;
        if (r->runState > 0) {
            const u32* q = r->runs + 2 * r->runOff[a];
            for (u32 i = 0; i < r->runN[a]; ++i) { const size_t nb = (size_t)(q[2 * i + 1] - 1) * st + r->spanElem[a]; h = dlkHash(h, base + (size_t)q[2 * i] * st, nb); gDlkHashBytes += nb; }
        } else {
            const size_t nb = (size_t)(r->spanHi[a] - r->spanLo[a]) * st + r->spanElem[a];
            h = dlkHash(h, base + (size_t)r->spanLo[a] * st, nb); gDlkHashBytes += nb;
        }
        if (h != r->hA[a]) { ch |= 1u << a; r->hA[a] = h; }
    }
    return ch;
}
static int dlkFastIdx(DlkRec* r) {   /* per distinct vertex: its POS and NRM array indices (~0u = that attribute is not indexed) */
    if (r->fastState) return r->fastState > 0;
    r->fastState = -1;
    u32* f = (u32*)malloc((size_t)r->ndist * 2 * sizeof(u32) + 8);
    if (!f) return 0;
    DecOp ops[DLK_FMTS][GX_VA_MAX_ATTR]; int nops[DLK_FMTS]; unsigned built = 0;
    for (u32 d = 0; d < r->ndist; ++d) {
        int fmt = r->fl[d] >> 4 & 7;
        if (!(built >> fmt & 1)) { nops[fmt] = dlkOps(ops[fmt], (GXVtxFmt)fmt, r->key.lim); built |= 1u << fmt; }
        const unsigned char* src = r->dl + r->voff[d]; int c = 0; u32 pi = ~0u, ni = ~0u;
        for (int i = 0; i < nops[fmt]; ++i) {
            const DecOp* o = &ops[fmt][i];
            if (o->kind == OP_MTX) { c += 1; continue; }
            if (o->idxBytes) {
                u32 idx = o->idxBytes == 1 ? src[c] : be16u(src + c); c += o->idxBytes;
                if (o->kind == OP_POS) pi = idx; else if (o->kind == OP_NRM) ni = idx;
                continue;
            }
            if (o->kind == OP_GENERIC) { free(f); return 0; }                       /* a direct attribute only the reference reader sizes */
            if (o->kind == OP_CLR0 || o->kind == OP_CLR1) c += colorSize(o->type);
            else c += compTypeSize(o->type) * o->n;
        }
        f[2 * d] = pi; f[2 * d + 1] = ni;
    }
    r->fidx = f; r->fastState = 1;
    return 1;
}
static void dlkFastApply(DlkRec* r, unsigned ch) {
    DecOp ops[DLK_FMTS][GX_VA_MAX_ATTR]; int nops[DLK_FMTS]; unsigned built = 0; const DecOp* po[DLK_FMTS]; const DecOp* no[DLK_FMTS];
    const int doP = (ch >> GX_VA_POS) & 1, doN = ((ch >> GX_VA_NRM) & 1) && r->nrm;
    for (u32 d = 0; d < r->ndist; ++d) {
        int fmt = r->fl[d] >> 4 & 7;
        if (!(built >> fmt & 1)) {
            nops[fmt] = dlkOps(ops[fmt], (GXVtxFmt)fmt, r->key.lim); built |= 1u << fmt; po[fmt] = no[fmt] = NULL;
            for (int i = 0; i < nops[fmt]; ++i) { if (ops[fmt][i].kind == OP_POS) po[fmt] = &ops[fmt][i]; else if (ops[fmt][i].kind == OP_NRM) no[fmt] = &ops[fmt][i]; }
        }
        const u32 pi = r->fidx[2 * d], ni = r->fidx[2 * d + 1];
        if (doP && pi != ~0u && (r->fl[d] & 1) && po[fmt]) {
            const DecOp* o = po[fmt]; const unsigned char* q = o->base + pi * (unsigned)o->stride; const int ts = compTypeSize(o->type);
            r->pos[3 * d] = scal(q, o->type, o->k); r->pos[3 * d + 1] = scal(q + ts, o->type, o->k);
            if (o->n == 3) r->pos[3 * d + 2] = scal(q + 2 * ts, o->type, o->k);
        }
        if (doN && ni != ~0u && (r->fl[d] & 2) && no[fmt]) {
            const DecOp* o = no[fmt]; const unsigned char* q = o->base + ni * (unsigned)o->stride; const int ts = compTypeSize(o->type);
            r->nrm[3 * d] = scal(q, o->type, o->k); r->nrm[3 * d + 1] = scal(q + ts, o->type, o->k); r->nrm[3 * d + 2] = scal(q + 2 * ts, o->type, o->k);
        }
    }
}
static void dlkRefreshAny(DlkRec* r, unsigned ch) {
    const unsigned fastMask = 1u << GX_VA_POS | 1u << GX_VA_NRM;
#ifndef DLK_NOFAST
    if (!(ch & ~fastMask) && r->fastState >= 0 && !r->nbt && dlkFastIdx(r)) {   /* (a record with binormal/tangent re-reads them: the fast path updates POS and NRM only) */
        dlkFastApply(r, ch);
        if (!r->fastChecked) {   /* once per record: the fast result against the full refresh */
            r->fastChecked = 1;
            const size_t pb = (size_t)r->ndist * 3 * sizeof(float);
            float* keep = (float*)malloc(pb * 2 + 8);
            if (keep) {
                memcpy(keep, r->pos, pb); if (r->nrm) memcpy(keep + r->ndist * 3, r->nrm, pb);
                dlkRefresh(r);
                if (memcmp(keep, r->pos, pb) || (r->nrm && memcmp(keep + r->ndist * 3, r->nrm, pb))) { r->fastState = -1; ++gDlkFastBad; }
                free(keep);
            }
        }
        ++gDlkFastN;
        return;
    }
#endif
    dlkRefresh(r); ++gDlkFullN; r->c9Slow = 1;   /* R6 C9: more than POS / NRM may have moved: no GPU slot's image can be trusted for the rest */
}
// One distinct vertex's interleaved image (the layout L of dlkMesh): the ONE builder both the scratch path and the R6 C9 staging path use, so the two cannot write different bytes.
static void dlkMeshVert(const DlkRec* r, const GxMeshLayout* Lp, int nt, u32 d, unsigned char* v) {
    const GxMeshLayout L = *Lp;
    memcpy(v + L.offPos, r->pos + 3 * d, 12);
    if (r->nrm) memcpy(v + L.offNrm, r->nrm + 3 * d, 12);
    if (r->clr) { u32 c = r->clr[d]; v[L.offClr] = (u8)c; v[L.offClr + 1] = (u8)(c >> 8); v[L.offClr + 2] = (u8)(c >> 16); v[L.offClr + 3] = (u8)(c >> 24); }
    if (r->pnm) { float m = (float)(r->pnm[d] / 3); memcpy(v + L.offMtx, &m, 4); }
    if (r->nbt) { if (nbtSwap()) { memcpy(v + L.offBin, r->nbt + 6 * d + 3, 12); memcpy(v + L.offBin + 12, r->nbt + 6 * d, 12); }
                  else memcpy(v + L.offBin, r->nbt + 6 * d, 24); }   /* binormal then tangent: offTan = offBin + 12 */
    for (int g = 0; g < 2; ++g) if (L.offTmi[g] >= 0) {
        float tf[4]; for (int q = 0; q < 4; ++q) { const int cc = 4 * g + q; tf[q] = r->tmiIdx[cc] >= 0 ? (float)r->tmi[(size_t)d * r->ntmi + r->tmiIdx[cc]] : 0.0f; }
        memcpy(v + L.offTmi[g], tf, 16);
    }
    for (int j = 0; j < nt; ++j) memcpy(v + L.offTex[r->texAttr[j]], r->tex + ((size_t)d * nt + j) * 2, 8);
    if (L.offSk >= 0) { uint8_t sk[8] = { 255, 255, 0, 0, 255, 255, 0, 0 }; (void)gx_skin_hal_attr(r->skinT, r->fidx[2 * d], r->fidx[2 * d + 1], r->nrm != NULL, sk); float f[8]; for (int q = 0; q < 8; ++q) f[q] = (float)sk[q]; memcpy(v + L.offSk, f, 32); }   /* R8: dlkSkinPrep proved every index in range */
}
/* R8: may record r be drawn skinned by table t?  Once per (record, table).  Everything the vertex program assumes about the arrays is checked here, because the G side has already skipped the CPU blend and the
   model is wrong if drawn unskinned: the list reads t's model's FILE vertices (and normals) through indices, s16 positions at frac == posQ, s8 normals (decoded at 2^-6) with nrmQ == 6, no NBT, every index inside the table. */
/* R8 refusal reasons (instrument, 2026-10-26 hw_R8 smoke: ThornTails skipped): skinChkOk 1 = OK, >= 2 = refused with that reason --
   2 POS base, 3 NRM base, 4 POS format / frac, 5 NRM format / scale, 6 index outside the table, 7 NBT, 8 no POS / no fast index,
   9 tev_vs_check, 10 gx_vs_build refused, 11 no mesh, 12 backend refused, 13 an earlier dlkCall return (fallback, validation). */
static int sSkinWhy;
static int dlkSkinPrep(DlkRec* r, const struct GxSkinTable* t) {
    const uint32_t tid = gx_skin_table_id(t);
    if (r->skinChkId == tid) { if (r->skinChkOk != 1) sSkinWhy = r->skinChkOk; return r->skinChkOk == 1; }   /* by id: a new table at a freed one's address must be judged afresh (R8-MF A2) */
    r->skinChkId = tid; r->skinChkOk = 13;
    if (r->nbt || !r->pos || !dlkFastIdx(r)) { sSkinWhy = r->skinChkOk = r->nbt ? 7 : 8; return 0; }
    for (int f = 0; f < DLK_FMTS; ++f) if (r->fmtMask >> f & 1) {
        const VatAttr* vp = &r->vat[f][GX_VA_POS]; const VatAttr* vn = &r->vat[f][GX_VA_NRM];
        const int nrmExp = vn->type == GX_S8 ? 6 : vn->type == GX_S16 ? 14 : 0;   /* buildOps: a normal's scale comes from its type, not the VAT frac */
        const int c_ = gx_skin_hal_check(t, r->key.base[GX_VA_POS], r->key.base[GX_VA_NRM], r->nrm != NULL, vp->frac, vp->type == GX_S16 && vp->cnt == GX_POS_XYZ, nrmExp, vn->type == GX_S8 && vn->cnt == GX_NRM_XYZ);
        if (c_) { sSkinWhy = r->skinChkOk = (u8)(1 + c_); return 0; }
    }
    for (u32 d = 0; d < r->ndist; ++d) { uint8_t sk[8]; if (!gx_skin_hal_attr(t, r->fidx[2 * d], r->fidx[2 * d + 1], r->nrm != NULL, sk)) { sSkinWhy = r->skinChkOk = 6; return 0; } }
    r->skinChkOk = 1;
    return 1;
}
/* R8: the bound skin, when the list being drawn reads its model's file vertices */
static inline const struct GxSkinTable* gxdSkinWant(void) { return GXD_SKIN_BOUND() ? gx_skin_hal_table(gArray[GX_VA_POS].base) : NULL; }
// Stage 2: the record as a backend mesh. Every attribute the LIST carries, interleaved (GxMeshLayout), and the triangles in
// dlkExpand's order as u16 indices into the distinct vertices (gx_vs_build has already refused a list with a !posOk vertex,
// so no triangle is dropped here that the CPU path would draw).
static void dlkMesh(DlkRec* r) {
    GxMeshLayout L; int o = 0; const u32 nd = r->ndist; const int nt = r->ntex;
    L.offPos = o; o += 12;
    L.offNrm = r->nrm ? o : -1; if (r->nrm) o += 12;
    L.offClr = r->clr ? o : -1; if (r->clr) o += 4;
    L.offMtx = r->pnm ? o : -1; if (r->pnm) o += 4;
    L.offBin = L.offTan = L.offTmi[0] = L.offTmi[1] = -1;
    if (r->nbt) { L.offBin = o; o += 12; L.offTan = o; o += 12; }
    if (r->tmiMask & 0x0f) { L.offTmi[0] = o; o += 16; }
    if (r->tmiMask & 0xf0) { L.offTmi[1] = o; o += 16; }
    for (int n = 0; n < 8; ++n) L.offTex[n] = -1;
    for (int j = 0; j < nt; ++j) { L.offTex[r->texAttr[j]] = o; o += 8; }
    L.offSk = -1; if (r->skinT) { L.offSk = o; o += 32; }   /* R8: aSkP + aSkN (F32x4 each, small integers), static for the mesh */
    L.stride = o;
    const u32 ni = r->ntri * 3;
    const int upd = r->gpuMesh != NULL;   /* the arrays changed: same layout, same indices, new vertex bytes */
    if (upd && !rhi_vita_gx_mesh_update) { rhi_vita_gx_mesh_destroy(r->gpuMesh); r->gpuMesh = NULL; --gDlkGpu.meshes; return; }
    dlk64 m0 = GXT_NOW();
    if (upd && (gx_r3a & GX_R3A_C9) && !r->nbt && rhi_gxm_gx_mesh_update_begin && rhi_gxm_gx_mesh_update_commit) {
        /* R6 C9 (host/engine/R6_SKIN.md s2): the backend hands out the slot; a slot that already holds a full image of this mesh gets POS and NRM only (the rest of the image is what the last full write left there and cannot have moved:
           c9Slow tells the backend when it may have), any other slot gets the whole image through a small staging block.  NBT records keep the full path below (the fast refresh never handles them). */
        int fresh = r->c9Slow;   /* in: nonzero = every slot's image is stale; out: 1 = write the whole image, 0 = POS / NRM only, -1 = not the GXM backend */
        const u32 vb9 = nd * (u32)o;
        unsigned char* dst = (unsigned char*)rhi_gxm_gx_mesh_update_begin(r->gpuMesh, vb9, &fresh);
        if (fresh != -1) {
            r->c9Slow = 0;
            if (!dst) ++gx_r6_c9_ref;
            else if (fresh) {
                enum { BLK = 128 }; static unsigned char* sSt; static size_t sStCap; const size_t need = (size_t)BLK * (size_t)o;
                if (sStCap < need) { void* q = realloc(sSt, need); if (!q) return; sSt = (unsigned char*)q; sStCap = need; }   /* no staging block: no commit, the mesh keeps its current slot and the record stays stale (the retry is the next draw) */
                for (u32 d0 = 0; d0 < nd; d0 += BLK) {
                    const u32 n9 = nd - d0 < BLK ? nd - d0 : BLK;
                    for (u32 i = 0; i < n9; ++i) dlkMeshVert(r, &L, nt, d0 + i, sSt + (size_t)i * (size_t)o);
                    memcpy(dst + (size_t)d0 * (size_t)o, sSt, (size_t)n9 * (size_t)o);
                }
                rhi_gxm_gx_mesh_update_commit(r->gpuMesh); ++gx_r6_c9_fresh;
            } else {
                for (u32 d = 0; d < nd; ++d) {
                    unsigned char* v = dst + (size_t)d * (size_t)o;
                    memcpy(v + L.offPos, r->pos + 3 * d, 12);
                    if (r->nrm) memcpy(v + L.offNrm, r->nrm + 3 * d, 12);
                }
                rhi_gxm_gx_mesh_update_commit(r->gpuMesh); ++gx_r6_c9_part;
            }
#ifdef GX_C5_SELFCHECK
            if (dst) {   /* host self-check: the slot the backend now calls current must equal the full image today's path writes */
                extern int gx_c9h_check(void* mesh, const void* full, uint32_t bytes); extern unsigned gx_c5_chk_bad, gx_c5_chk;
                unsigned char* full = (unsigned char*)malloc((size_t)vb9 + 4);
                if (full) { for (u32 d = 0; d < nd; ++d) dlkMeshVert(r, &L, nt, d, full + (size_t)d * (size_t)o);
                            ++gx_c5_chk; if (!gx_c9h_check(r->gpuMesh, full, vb9)) { if (gx_c5_chk_bad++ < 6) fprintf(stderr, "FAIL: C9 the current slot differs from the full image (%s write)\n", fresh ? "full" : "partial"); } free(full); }
            }
#endif
            r->meshStale = 0;
            ++gDlkGpu.updates; gDlkGpu.updBytes += (unsigned long long)nd * (unsigned)(fresh ? o : (r->nrm ? 24 : 12)); gDlkGpu.updUs += GXT_NOW() - m0;
            return;
        }
    }
    static unsigned char* sVb; static size_t sVbCap;   /* one growing scratch buffer: this runs every frame for the morphed models */
    const size_t vneed = (size_t)nd * (size_t)o + 4;
    if (sVbCap < vneed) { size_t nc = sVbCap ? sVbCap : 65536; while (nc < vneed) nc *= 2; void* q = realloc(sVb, nc); if (!q) return; sVb = (unsigned char*)q; sVbCap = nc; }
    unsigned char* vb = sVb; u16* ib = upd ? NULL : (u16*)malloc((size_t)ni * 2 + 4);
    if (!upd && !ib) return;
    for (u32 d = 0; d < nd; ++d) dlkMeshVert(r, &L, nt, d, vb + (size_t)d * (size_t)o);
    if (upd) {
        rhi_vita_gx_mesh_update(r->gpuMesh, vb, nd * (u32)o); r->meshStale = 0;
        ++gDlkGpu.updates; gDlkGpu.updBytes += (unsigned long long)nd * (unsigned)o; gDlkGpu.updUs += GXT_NOW() - m0;
        return;
    }
    u32 n = 0;
    for (u32 p = 0; p < r->nprim && n <= ni; ++p) {
        const DlkPrim* P = &r->prim[p]; const u16* m = r->map + P->mapOff; const u8* fl = r->fl; const u32 nv = P->nv;
#define DLK_ITRI(a, b, c) do { u32 A = m[a], B = m[b], C = m[c]; if ((fl[A] & fl[B] & fl[C] & 1) && n + 3 <= ni) { ib[n] = (u16)A; ib[n + 1] = (u16)B; ib[n + 2] = (u16)C; n += 3; } } while (0)
        if (P->prim == GX_TRIANGLES) { for (u32 i = 0; i + 3 <= nv; i += 3) DLK_ITRI(i, i + 1, i + 2); }
        else if (P->prim == GX_TRIANGLESTRIP) { for (u32 i = 2; i < nv; ++i) { if (i & 1) DLK_ITRI(i - 1, i - 2, i); else DLK_ITRI(i - 2, i - 1, i); } }
        else if (P->prim == GX_TRIANGLEFAN) { for (u32 i = 2; i < nv; ++i) DLK_ITRI(0, i - 1, i); }
        else if (P->prim == GX_QUADS) { for (u32 i = 0; i + 4 <= nv; i += 4) { DLK_ITRI(i, i + 1, i + 2); DLK_ITRI(i, i + 2, i + 3); } }
#undef DLK_ITRI
    }
    if (n == ni && n) {
        r->gpuMesh = rhi_vita_gx_mesh_create(vb, nd * (u32)o, ib, n, &L);
        if (r->gpuMesh) { ++gDlkGpu.meshes; gDlkGpu.meshBytes += (unsigned long long)nd * (unsigned)o + (unsigned long long)n * 2; r->meshStale = 0; }
    }
    free(ib);
}
unsigned long long gx_prog_hint;                       /* R3a C3: see the draw call in dlkCall; read and cleared by the GXM backend (rhi_gxm_draw.c) */
extern uint32_t gx_tev_key_serial;                     /* gx_tev.c: moves when a rebuilt TevKey differs from the remembered one */
uint32_t gx_vs_key_serial;                             /* moves when a rebuilt VsKey differs from the remembered one (defined here; the memo is further down) */
/* ---- R3c (host/engine/R3C_CACHE.md): C5 PROGRAM-CLEAN DRAW and C7 DISPLAY-LIST CHECK TRIM, the HAL half ---------------------------------------------------------------------------------------------------
 * C5.  63% of the display-list GPU draws use the SAME raw (VsKey, TevKey) as the draw before.  The pair is a function of a SMALL set of inputs; the draw's other inputs (the matrices, the TEV / konst
 * colours, the fog numbers, the material / ambient colours, the light colours and positions, the alpha refs, the textures) are UNIFORMS or per-draw state and do not move it.  A "program epoch" counts the
 * moves of exactly the program inputs; a draw whose epoch equals the one of the draw its base was built at (and whose record has the same shape, and whose by-value non-struct inputs are the same) reuses
 * the base TevKey / sc / sm / VsKey and rebuilds ONLY the numbers (TevUniforms, VsUniforms).
 *
 * THE BUMP LIST (what moves the epoch), derived from the code that builds the pair, with the generation kind each input lives in (port/gx_hal_state.h):
 *   TevKey    gx_tev_build_ref (gx_tev.c)      TEV   gTev: numStages, st[] (orders, combiner ops, swap / konst selects, the indirect setup), numGens, numInd, indOrder   kinds STAGE TEVCNT IND (+ GEN: gTev.gen[] sits in the
 *                                                       same TevState and the VsKey reads it); gIndSc (IND); gSwapTbl (SWAP); and the three alpha-compare parameters acComp0 / acOp / acComp1 (PIX).
 *   VsKey     vsBuildKey (below)               gRealSkin, gStfxLightArm, tmiOld() (non-struct, compared by value), gPxActive and gNumChans (PIX), gChan[].enable / ambSrc / matSrc / diffFn / attnFn / lightMask (CHAN),
 *                                                       which lights have a non-zero colour (LIGHT: vsPackLights drops a black light), the texgen table gTev.gen[] at sc[] (GEN), and the RECORD (VsShape: hasPnm, hasClr, normals, NBT,
 *                                                       texIdx, tmiIdx, tmiBad).
 *   fog       fogWhy / k.fogType               gFogType (by value), gProjOrtho (PROJ, by value), k.numTex, fogKilled (a constant after the first read).
 *   NOT on the list (uniforms or per-draw state; they are rebuilt every draw):  POS NRM TEXM POSTM PROJ's matrix, gCurMtx, TEVREG KREG INDM, acRef0 / acRef1, gMat / gAmb, the light colours / positions / attenuation,
 *   gFogStart / End / colour, the viewport depth range and gStfxGxZToGL (they shape the caller's mvp), COPY, TEVOP (never read), the textures (gTexMap is read every draw).
 * The generation kinds are COARSE (one CHAN flag covers the mat / amb colours that change at every object): a kind whose generation moved is therefore re-examined BY VALUE (the fields above against the snapshot
 * the last examination took) and bumps the epoch only when a program input really changed.  A kind that is a pure program input (TEV, SWAP, IND, GEN) is examined the same way (a setter that stores the same
 * values again does not bump).  Over-bumping is safe (the full path runs); under-bumping is a wrong picture: GX_C5_SELFCHECK (host tests) rebuilds everything the plain way on every clean draw and compares BYTES. */
typedef struct { u8 state, why, haveNrm, nbAll, hasPnm, hasClr, hasNbt, tmiBad; signed char texIdx[8], tmiIdx[8]; } VsShape;   /* the parts of a record the VsKey reads (also keyed by the C3 memo, below) */
static int vsShapeOf(const DlkRec* r, VsShape* s);
extern int gStfxLightArm;   /* defined with the pixel state, below */
static void vsBuildUniforms(const VsKey* k, const TevKey* tk, const u8 sc[TEVK_TEX], const float* mvpIn, VsUniforms* u);
extern void gx_tev_build_uniforms(const TevKey* k, TevUniforms* u, int acRef0, int acRef1);   /* gx_tev.c: the numbers half of gx_tev_build */
extern void gx_tev_replay_stats(const TevKey* k);                                             /* gx_tev.c: the log counters a key build keeps */
unsigned gx_r3c_c5_total, gx_r3c_c5_clean, gx_r3c_c5_bumps;   /* the [r3c] census (cumulative): GPU-path draws that reached the tracker, of them clean (counted also when C5 is off), epoch moves */
unsigned gx_r3c_c7_vat, gx_r3c_c7_key, gx_r3c_c7_calls;       /* C7: calls whose VAT compare was skippable, whose record came from the last-hit cache, calls seen (counted also when C7 is off) */
#ifdef GX_C5_SELFCHECK
unsigned gx_c5_chk, gx_c5_chk_bad, gx_c5_chk_pred, gx_c5_chk_id, gx_c5_chk_c7;   /* host tests: clean draws re-derived, mismatches, predictions checked, hint-identity pairs checked, C7 shortcuts checked */
#endif
typedef struct {
    int init;
    uint32_t gen[GXH_NKINDS_];                                      /* the generations last examined */
    TevState tev; u8 indSc[GXH_IND_STAGES][2]; u8 swapTbl[4][4];     /* snapshots of the program inputs */
    u8 pxActive, numChans, acComp0, acOp, acComp1; PxChan chan[4]; u8 lightLive; int projOrtho;
    uint32_t epoch;                                                 /* the program epoch */
    int valid; uint32_t baseEpoch, keyId, keyIdSeq;                 /* the base: keys of the last successful full GPU-path build; keyId names the (VsKey, TevKey) pair for the backend's hint */
    int realSkin, lightArm, tmiOld, fogType;                        /* by-value non-struct inputs of the base */
    VsShape shape; TevKey k; u8 sc[TEVK_TEX], sm[TEVK_TEX];
#ifdef GX_C5_SELFCHECK
    VsKey vk;
#endif
} C5State;
static C5State gC5;
static u8 c5LightLive(void) { u8 m = 0; for (int i = 0; i < GXH_NLIGHTS; ++i) { const u32 col = gx_hal.lightRegs[i].color; if (col) m = (u8)(m | (1u << i)); } return m; }
static void c5Snap(C5State* c) {
    memcpy(&c->tev, &gx_hal.tev, sizeof c->tev); memcpy(c->indSc, gx_hal.indSc, sizeof c->indSc); memcpy(c->swapTbl, gx_hal.swapTbl, sizeof c->swapTbl);
    c->pxActive = (u8)gPxActive; c->numChans = gNumChans; c->acComp0 = gAcComp0; c->acOp = gAcOp; c->acComp1 = gAcComp1;
    memcpy(c->chan, gChan, sizeof c->chan); c->lightLive = c5LightLive(); c->projOrtho = gProjOrtho;
    for (unsigned k = 0; k < GXH_NKINDS_; ++k) c->gen[k] = gx_hal_gen[k];
}
/* Examine the program inputs; move the epoch when one moved.  Runs once per cached display-list call (GPU eligible) when any R3c cut / census is on. */
static void c5Track(void) {
    C5State* c = &gC5; int bump = 0;
    gxh_collect();
    if (!c->init) { c5Snap(c); c->init = 1; bump = 1; }
    else {
        static const uint8_t tevKinds[5] = { GXH_K_STAGE, GXH_K_TEVCNT, GXH_K_IND, GXH_K_SWAP, GXH_K_GEN };
        int tchg = 0;
        for (int i = 0; i < 5; ++i) if (c->gen[tevKinds[i]] != gx_hal_gen[tevKinds[i]]) { c->gen[tevKinds[i]] = gx_hal_gen[tevKinds[i]]; tchg = 1; }
        if (tchg && (memcmp(&c->tev, &gx_hal.tev, sizeof c->tev) || memcmp(c->indSc, gx_hal.indSc, sizeof c->indSc) || memcmp(c->swapTbl, gx_hal.swapTbl, sizeof c->swapTbl))) {
            memcpy(&c->tev, &gx_hal.tev, sizeof c->tev); memcpy(c->indSc, gx_hal.indSc, sizeof c->indSc); memcpy(c->swapTbl, gx_hal.swapTbl, sizeof c->swapTbl); bump = 1;
        }
        if (c->gen[GXH_K_PIX] != gx_hal_gen[GXH_K_PIX]) {
            c->gen[GXH_K_PIX] = gx_hal_gen[GXH_K_PIX];
            if (c->pxActive != (u8)gPxActive || c->numChans != gNumChans || c->acComp0 != gAcComp0 || c->acOp != gAcOp || c->acComp1 != gAcComp1) {
                c->pxActive = (u8)gPxActive; c->numChans = gNumChans; c->acComp0 = gAcComp0; c->acOp = gAcOp; c->acComp1 = gAcComp1; bump = 1;
            }
        }
        if (c->gen[GXH_K_CHAN] != gx_hal_gen[GXH_K_CHAN]) {
            c->gen[GXH_K_CHAN] = gx_hal_gen[GXH_K_CHAN];
            if (memcmp(c->chan, gChan, sizeof c->chan)) { memcpy(c->chan, gChan, sizeof c->chan); bump = 1; }
        }
        if (c->gen[GXH_K_LIGHT] != gx_hal_gen[GXH_K_LIGHT]) {
            c->gen[GXH_K_LIGHT] = gx_hal_gen[GXH_K_LIGHT];
            const u8 live = c5LightLive();
            if (live != c->lightLive) { c->lightLive = live; bump = 1; }
        }
        if (c->projOrtho != gProjOrtho) { c->projOrtho = gProjOrtho; bump = 1; }
    }
    if (bump) { ++c->epoch; ++gx_r3c_c5_bumps; }
}
/* 1 = the base keys are the keys a full build would produce NOW for this record (the epoch has not moved since the base was built, the by-value inputs and the record shape are the base's). */
static int c5Would(int shapeOk, const VsShape* shape) {
    const C5State* c = &gC5;
    if (!shapeOk) return 0;
    return c->valid && c->baseEpoch == c->epoch && c->realSkin == gRealSkin && c->lightArm == gStfxLightArm && c->tmiOld == tmiOld() && c->fogType == gFogType && !memcmp(&c->shape, shape, sizeof *shape);
}
static void c5Capture(const TevKey* k, const u8 sc[TEVK_TEX], const u8 sm[TEVK_TEX], const VsShape* shape, const VsKey* vk) {
    C5State* c = &gC5;
    c->valid = 1; c->baseEpoch = c->epoch; c->keyId = ++c->keyIdSeq;
    c->realSkin = gRealSkin; c->lightArm = gStfxLightArm; c->tmiOld = tmiOld(); c->fogType = gFogType;
    c->shape = *shape; memcpy(&c->k, k, sizeof c->k);
    memset(c->sc, 0, sizeof c->sc); memset(c->sm, 0, sizeof c->sm); memcpy(c->sc, sc, k->numTex); memcpy(c->sm, sm, k->numTex);
#ifdef GX_C5_SELFCHECK
    memcpy(&c->vk, vk, sizeof c->vk);
#else
    (void) vk;
#endif
}
#ifdef GX_C5_SELFCHECK
extern int gx_tev_build_refcall(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX], int acComp0, int acOp, int acComp1, int acRef0, int acRef1);
static int gx_vs_build_mono(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvpIn, const VsKey* prevKey);
static int c5RefFog(TevKey* k, TevUniforms* u) {   /* dlkCall's fog block, without its counters */
    if (!fogWant()) return 0;
    if ((gFogType & 8) || gProjOrtho) return 2;
    if (k->numTex >= 8) return 2;
    if (fogKilled()) return 3;
    k->fogType = (uint8_t)gFogType;
    u->fogStart = gFogStart; u->fogScale = gFogEnd == gFogStart ? 0.0f : 1.0f / (gFogEnd - gFogStart);
    u->fogColor[0] = FOG_COLOR_SEL().r; u->fogColor[1] = FOG_COLOR_SEL().g; u->fogColor[2] = FOG_COLOR_SEL().b;
    return 1;
}
static void c5Bad(const char* what) { if (gx_c5_chk_bad++ < 6) fprintf(stderr, "FAIL: C5 %s (check %u)\n", what, gx_c5_chk); }
/* Called in the GPU branch with the keys and numbers the draw is about to use.  used == 1: they came from the clean path -> they must equal a from-scratch build, byte for byte.  used == 0 and `would`:
 * the full path ran although the predictor said clean -> the BASE must equal the fresh keys (the predictor is checked even when the cut is off).  Always: equal keyId => equal raw keys (the backend hint contract). */
static void c5Selfcheck(const DlkRec* r, const TevKey* k, const TevUniforms* u, const u8* sc, const u8* sm, const VsKey* vk, const VsUniforms* vu, int used, int would, uint32_t keyId) {
    static VsKey lastVk; static TevKey lastTk; static uint32_t lastId; static int haveLast;
    TevKey rk; TevUniforms ru; u8 rsc[TEVK_TEX], rsm[TEVK_TEX]; VsKey rvk; VsUniforms rvu;
    memset(rsc, 0, sizeof rsc); memset(rsm, 0, sizeof rsm);
    if (haveLast && keyId == lastId) { ++gx_c5_chk_id; if (memcmp(&lastVk, vk, sizeof *vk) || memcmp(&lastTk, k, sizeof *k)) c5Bad("equal key ids, different raw (VsKey, TevKey)"); }
    lastVk = *vk; lastTk = *k; lastId = keyId; haveLast = 1;
    if (!used && !would) return;
    ++gx_c5_chk;
    if (!gx_tev_build_refcall(&rk, &ru, rsc, rsm, gAcComp0, gAcOp, gAcComp1, gAcRef0, gAcRef1)) { c5Bad("the plain TEV build refuses a draw predicted clean"); return; }
    (void) c5RefFog(&rk, &ru);
    if (!gx_vs_build_mono(r, &rk, rsc, &rvk, &rvu, NULL, NULL)) { c5Bad("the plain VS build refuses a draw predicted clean"); return; }
    if (used) {
        if (memcmp(k, &rk, sizeof rk)) c5Bad("clean TevKey bytes");
        if (memcmp(u, &ru, sizeof ru)) c5Bad("clean TevUniforms bytes");
        if (memcmp(sc, rsc, rk.numTex) || memcmp(sm, rsm, rk.numTex)) c5Bad("clean slot coord / map");
        if (memcmp(vk, &rvk, sizeof rvk)) c5Bad("clean VsKey bytes");
        if (memcmp(vu, &rvu, sizeof rvu)) c5Bad("clean VsUniforms bytes");
    } else {
        ++gx_c5_chk_pred;
        if (memcmp(&gC5.k, &rk, sizeof rk)) c5Bad("predicted clean but the TevKey differs from the base");
        if (memcmp(&gC5.vk, &rvk, sizeof rvk)) c5Bad("predicted clean but the VsKey differs from the base");
        if (memcmp(gC5.sc, rsc, rk.numTex) || memcmp(gC5.sm, rsm, rk.numTex)) c5Bad("predicted clean but the slot coord / map differ from the base");
    }
}
#endif
static int dlkCall(const unsigned char* dl, u32 nbytes, const struct GxSkinTable* skinT) {   /* skinT (R8): the bound skin this list reads the file vertices of, or NULL; a skinned list is NEVER drawn by the CPU path: every `return 0` becomes a counted skip at the call site */
    RT_SUBS(RT_S_DLK_ALL);   /* Task 3b: the whole cache attempt, every return path (sampled frames) */
    int why = !gRealSkin ? DLK_NE_REALSKIN : gForceStride > 0 ? DLK_NE_FORCE : (gHaveJointOff || gHaveJointMtx) ? DLK_NE_JOINT
            : gStfxDecodeArm != 1 ? DLK_NE_DECARM : gStfxPerfArm != 0 ? DLK_NE_PERF : !(gStfxTevArm && rhi_vita_drawTev && gRhi) ? DLK_NE_NOTEV
            : (gxj() || dbg()) ? DLK_NE_JOURNAL : gDlcOn > 0 ? DLK_NE_CENSUS : -1;
    if (why >= 0) { ++gDlk.fb[why]; return 0; }
    dlk64 t0 = GXT_NOW(), t1, t2, t3, t4, t5, t6, t7, t8;
    const unsigned r3a = gx_r3a;   /* R3a: the cut switches, read once per call */
    const unsigned r3c = r3a & GX_R3C_CUTS;   /* R3c: any of C5 / C6 / C7 on -> the trackers and the census run (the release default pays nothing for them) */
    const int c8 = (r3a & GX_R3A_C8) != 0 && gDlkEvt == 1;   /* R6 C8: dynamic records learn what changed from the announced writes */
    { static int c8Last; if (c8 != c8Last) { c8Last = c8; ++gDlkWrEpoch; } }   /* writes announced while C8 was off are not in the ring: every record's position is void */
    if (r3c) gxh_collect();   /* flags -> generations: the C7 VAT shortcut and the C5 tracker read them */
    // ---- look up, validate -------------------------------------------------------------------------------
    static DlkKey kn; static unsigned knGen; static u32 knHash;   /* reused while gVtxGen is unchanged (see gx_draw_init) */
    if (knGen != gVtxGen) { dlkKeyNow(&kn); knHash = (u32)dlkHash(0x51ED270B0F3D4C1Full, &kn, sizeof kn); knGen = gVtxGen; ++gDlkKeyBuilt; } else ++gDlkKeyReused;   /* the set is chosen by (list, KEY): run 134 had 2,577 evictions for 2,987 builds with the list alone -- one list under more than four keys */
    u32 hs = ((u32)((size_t)dl >> 4) * 2654435761u ^ nbytes * 40503u) + knHash * 0x9E3779B1u;
    DlkRec** set = &gDlkTab[((hs >> 8) & (DLK_SETS - 1)) * DLK_WAYS]; DlkRec** slot = NULL;
    /* Run 132 (docs/47): a record is (list, KEY), not (list). The morphed models draw ONE list from TWO vertex buffers on
       alternate frames, so matching on the list alone and then comparing the key threw the record away every call:
       114,976 rebuilds in 353,519 calls, 46 ms a frame of decode. Each buffer now keeps its own way in the set. */
    /* R3c C7: the last call's record, remembered by (list, size, the vertex-key generation, the free generation).  Nothing that decides the set search moved since (gVtxGen: descriptor / arrays / bounds;
       gDlkFreeGen: no record left the table), so the search would find the same slot: it is skipped.  Counted also when C7 is off; the host self-check runs the search and compares. */
    static struct { const unsigned char* dl; u32 nbytes, vtxGen, freeGen; DlkRec** slot; DlkRec* rec; } c7l;
    int c7key = 0;
    if (r3c) { ++gx_r3c_c7_calls; c7key = c7l.rec && c7l.dl == dl && c7l.nbytes == nbytes && c7l.vtxGen == gVtxGen && c7l.freeGen == gDlkFreeGen && *c7l.slot == c7l.rec; gx_r3c_c7_key += c7key; }
    if (c7key && (r3a & GX_R3A_C7)) slot = c7l.slot;
    else
    for (int w = 0; w < DLK_WAYS; ++w) if (set[w] && set[w]->dl == dl && set[w]->nbytes == nbytes && !memcmp(&kn, &set[w]->key, sizeof kn)) { slot = &set[w]; break; }
#ifdef GX_C5_SELFCHECK
    if (c7key) { DlkRec** s2 = NULL; for (int w = 0; w < DLK_WAYS; ++w) if (set[w] && set[w]->dl == dl && set[w]->nbytes == nbytes && !memcmp(&kn, &set[w]->key, sizeof kn)) { s2 = &set[w]; break; }
                 ++gx_c5_chk_c7; if (!s2 || s2 != c7l.slot) c5Bad("C7 last-hit shortcut disagrees with the set search"); }
#endif
    DlkRec* r = slot ? *slot : NULL; int hit = 1;
    dlk64 tvA = GXT_NOW(), tvB;   /* run 163: validate, split -- key + lookup | VAT + list hash | array hash */
    gDlkVal[0] += tvA - t0;
    int c8pred = 0; unsigned c8ch = 0;   /* R6 C8: 1 = this call's changed-attribute set is c8ch (no hash) */
    int evSkipList = 0, evSkipArr = 0;   /* run 165: validation by events (gDlkEvt); both 0 = this call is validated in full */
    if (r && gDlkEvt) {
        if (dlkEvTouched(r)) { r->okList = r->okArr = 0; ++gDlkEv.touched; }
        if (r->dynGen != gDlkDynGen) { r->dynGen = gDlkDynGen; r->dyn = (u8)dlkDynReads(r); }
        const int forced = ++gDlkEvCall % DLK_EV_VERIFY == 0;
        if (forced) ++gDlkEv.forced;
        else { evSkipList = r->okList >= DLK_EV_TRUST; evSkipArr = !r->dyn && r->okArr >= DLK_EV_TRUST; if (r->dyn) ++gDlkEv.dynCalls; }
        if (c8 && r->dyn && !forced && r->state == 1) {   /* R6 C8: not the verify call, and the arrays are in a written buffer: which of them changed is read off the write ring, not hashed */
            ++gx_r6_c8_calls;
            const u32 since = gDlkWrSeq - r->wrSeq;
            if (r->wrEpoch != gDlkWrEpoch) ++gx_r6_c8_hash;
            else if (since > DLK_WR_RING) { ++gx_r6_c8_ovf; ++gx_r6_c8_hash; }
            else {
                c8pred = 1; c8ch = dlkWrOverlap(r, r->wrSeq); r->wrSeq = gDlkWrSeq;
                if (c8ch) { ++gx_r6_c8_pred; r->c8Dirty |= c8ch; } else ++gx_r6_c8_unch;
            }
        }
    }
    if (r) {
        int ok = 1;
        /* R3c C7: the record's copy of the VAT matched the live VAT when it was last validated; GXSetVtxAttrFmt is the only writer of the VAT kind (flag -> generation), so an unchanged generation means an
           unchanged VAT and the per-format compare would pass again.  Counted also when C7 is off. */
        const int vatSkip = r3c && r->vatGenOk && r->vatGen == gx_hal_gen[GXH_K_VAT];
        gx_r3c_c7_vat += vatSkip;
        if (!(vatSkip && (r3a & GX_R3A_C7))) {
            for (int f = 0; ok && f < DLK_FMTS; ++f) if ((r->fmtMask >> f & 1) && memcmp(r->vat[f], gVat[f], sizeof r->vat[f])) ok = 0;
        }
#ifdef GX_C5_SELFCHECK
        else { int ok2 = 1; for (int f = 0; f < DLK_FMTS; ++f) if ((r->fmtMask >> f & 1) && memcmp(r->vat[f], gVat[f], sizeof r->vat[f])) ok2 = 0; ++gx_c5_chk_c7; if (!ok2) c5Bad("C7 VAT shortcut skipped a compare that would have failed"); }
        if (vatSkip && !(r3a & GX_R3A_C7)) { int ok2 = 1; for (int f = 0; f < DLK_FMTS; ++f) if ((r->fmtMask >> f & 1) && memcmp(r->vat[f], gVat[f], sizeof r->vat[f])) ok2 = 0; if (!ok2) c5Bad("C7 VAT prediction (cut off) would have skipped a failing compare"); }
#endif
        if (ok && r3c) { r->vatGen = gx_hal_gen[GXH_K_VAT]; r->vatGenOk = 1; }
#ifndef BREAK_12   /* host break arm: the list-hash validation is skipped, so a changed list is drawn stale (check 11 step e must FAIL) */
        if (ok && evSkipList) ++gDlkEv.skipList;
        else if (ok) {
            if (dlkHash(0x9E3779B97F4A7C15ull, dl, nbytes) != r->hDl) { ok = 0; if (gDlkEvt && r->okList >= DLK_EV_TRUST) dlkEvMiss(r, "list", 0); }
            else if (r->okList < 255) ++r->okList;
        }
#endif
        if (!ok) { dlkFree(r); *slot = NULL; r = NULL; ++gDlk.rebuilds; hit = 0; }
    }
    t1 = GXT_NOW(); tvB = t1; gDlkVal[1] += tvB - tvA;
    if (r && r->state == 1) {
#ifndef BREAK_11   /* host break arm: the array-hash validation is skipped, so changed attribute bytes are drawn stale (step d must FAIL) */
        unsigned ch = 0;
        if (evSkipArr) ++gDlkEv.skipArr;
        else if (c8pred) { ch = c8ch; if (ch) r->okArr = 0; }   /* R6 C8: the hashes (hA) are now older than the bytes; c8Dirty says for which attributes a change is EXPECTED at the next hashed call */
        else {
            unsigned c8exp = ~0u;   /* R6 C8: the attributes whose change an announced write explains (all of them when the ring cannot say) */
            if (c8 && r->wrEpoch == gDlkWrEpoch && gDlkWrSeq - r->wrSeq <= DLK_WR_RING) c8exp = r->c8Dirty | dlkWrOverlap(r, r->wrSeq);
            const unsigned c8hash = dlkArrChanged(r);   /* (the hashes match the bytes again from here) */
            /* R6 C8: an attribute refreshed from a PREDICTED change has a stale hash: bytes that changed and changed back compare equal while the decode holds the in-between values.  Those attributes are refreshed here
               whatever the hash says (an extra refresh at worst), and the stale mark is cleared. */
            ch = c8hash | r->c8Dirty; r->c8Dirty = 0;
            if (ch) { if (gDlkEvt && !r->dyn && r->okArr >= DLK_EV_TRUST) dlkEvMiss(r, "array", ch); r->okArr = 0; }
            else if (r->okArr < 255) ++r->okArr;
            if (c8) {
                if (r->dyn && (c8hash & ~c8exp)) { ++gx_r6_c8_miss; dlkEvMiss(r, "array (dynamic, C8)", c8hash & ~c8exp); }   /* a change no announced write explains: an EVENT MISS */
                r->wrSeq = gDlkWrSeq; r->wrEpoch = gDlkWrEpoch;
            }
        }
        t1 = GXT_NOW(); gDlkVal[2] += t1 - tvB;   /* validate = key + list hash + array hash; decode = a refresh or a build only (runs 132-139 counted the array hash as decode) */
        if (ch) { dlkRefreshAny(r, ch); ++gDlk.refreshes; hit = 0;
                  ++r->nRefresh; r->meshStale = 1; }   /* the mesh keeps its indices; its vertices are re-uploaded at the next GPU draw */
#endif
    } else if (!r) {
        t1 = GXT_NOW();
        hit = 0;
        r = dlkBuild(dl, nbytes);
        if (!r) return 0;
        if (r->state == 1) { (void)dlkArrChanged(r); if (c8) { r->wrSeq = gDlkWrSeq; r->wrEpoch = gDlkWrEpoch; r->c8Dirty = 0; } }   /* seed the per-attribute hashes with what was just decoded (R6 C8: and the ring position they belong to) */                                     /* (dlkBuild counted the oom) */
        if (r->bytes > DLK_BUDGET) { dlkFree(r); ++gDlk.fb[DLK_FB_OOM]; return 0; }
        while (gDlk.bytes > DLK_BUDGET) {                     /* over budget: evict the least recently used record (r is not in the table yet) */
            DlkRec** lru = NULL;
            for (int q = 0; q < DLK_SETS * DLK_WAYS; ++q) if (gDlkTab[q] && (!lru || gDlkTab[q]->lastUse < (*lru)->lastUse)) lru = &gDlkTab[q];
            if (!lru) break;
            dlkEvict(lru);
        }
        if (!slot) {
            ++gDlk.misses;
            for (int w = 0; w < DLK_WAYS; ++w) if (!set[w]) { slot = &set[w]; break; }
            if (!slot) { slot = &set[0]; for (int w = 1; w < DLK_WAYS; ++w) if (set[w]->lastUse < (*slot)->lastUse) slot = &set[w]; dlkEvict(slot); }
        }
        *slot = r;
        if (r3c) { r->vatGen = gx_hal_gen[GXH_K_VAT]; r->vatGenOk = 1; }   /* R3c C7: built from the live VAT */
    }
    r->lastUse = ++gDlkClock;
    if (r3c) { c7l.dl = dl; c7l.nbytes = nbytes; c7l.vtxGen = gVtxGen; c7l.freeGen = gDlkFreeGen; c7l.slot = slot; c7l.rec = r; }   /* R3c C7: remembered for the next call */
    t2 = GXT_NOW(); gDlk.t[0] += t1 - t0; gDlk.t[1] += t2 - t1;
    if (r->state != 1) { ++gDlk.fb[DLK_FB_UNCACHEABLE]; return 0; }
    const u32 nprim = r->nprim, nd = r->ndist;
    // ---- per-call state, exactly what the per-primitive loop does -------------------------------------------
    if (gPxActive && !gColorUpdate) {                         /* colour update off: nothing is drawn, one skip per primitive */
        gx_px_skipped_noupdate += nprim; gx_prims += nprim; gx_prim_verts += r->declaredSum;
        ++gDlk.calls; if (hit) ++gDlk.hits; return 1;
    }
    float mvp[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) mvp[i*4+j] = gProj[i][j];
    gxClipZ(mvp);
    const int am = gPxActive ? pxAlphaMode() : RHI_ALPHA_OPAQUE;
    if (r->ntri == 0 || nprim == 0) {                         /* lines/points/empty: no draw, but the state calls happen as before */
        if (nprim) {
            if (gPxActive) { rhi_setAlphaMode(gRhi, am); if (rhi_vita_setDepthState) rhi_vita_setDepthState(gZCompare, gZFunc, gZUpdate); }
            rhi_setColorTransform(gRhi, mvp);
        }
        gx_prims += nprim; gx_prim_verts += r->declaredSum;
        ++gDlk.calls; if (hit) ++gDlk.hits; gDlk.stripV += r->nstrip; gDlk.distV += nd; return 1;
    }
    // ---- the TEV key, fog and textures ONCE (drawTev's front half); any refusal -> today's loop counts it itself ----
    TevKey k; TevUniforms u; u8 sc[TEVK_TEX], sm[TEVK_TEX];
    /* R3c C5: is the (VsKey, TevKey) pair of the last GPU-path draw still the pair a full build would give now?  (the tracker, the record shape and the by-value inputs: see the block above dlkCall)  Counted also when C5
       is off; with C5 on a clean draw skips the TevKey build, the fog-free key build and the VsKey build and rebuilds only the numbers. */
    const int c5gpu = r3c && gStfxGpuArm > 0 && rhi_vita_drawTevGx && rhi_vita_gx_mesh_create && gPxOldGuess == 0 && !r->gpuNo;
    VsShape c5shape; int c5shapeOk = 0, c5would = 0, c5clean = 0;
    if (c5gpu) {
        c5Track();
        c5shapeOk = vsShapeOf(r, &c5shape);
        c5would = c5Would(c5shapeOk, &c5shape);
        ++gx_r3c_c5_total; gx_r3c_c5_clean += (unsigned)c5would;
        c5clean = c5would && (r3a & GX_R3A_C5) != 0;
    }
    if (c5clean) {
        dlk64 rtTb = RT_T0();
        memcpy(&k, &gC5.k, sizeof k); memcpy(sc, gC5.sc, sizeof sc); memcpy(sm, gC5.sm, sizeof sm);
        gx_tev_build_uniforms(&k, &u, gAcRef0, gAcRef1); gx_tev_replay_stats(&k);   /* the numbers of THIS draw; the key is the base's */
        RT_SUBSL(RT_S_TEVBUILD, rtTb);
    } else
    {   extern unsigned gx_tev_refused_indirect, gx_tev_refused_slots, gx_tev_refused_compare;
        unsigned ri = gx_tev_refused_indirect, rs = gx_tev_refused_slots, rc = gx_tev_refused_compare;
        dlk64 rtTb = RT_T0(); const int rtBld = gx_tev_build(&k, &u, sc, sm, gAcComp0, gAcOp, gAcComp1, gAcRef0, gAcRef1); RT_SUBSL(RT_S_TEVBUILD, rtTb);
        C3_CHK_TEV(&k, &u, sc, sm, rtBld);
        if (!rtBld) {
            gx_tev_refused_indirect = ri; gx_tev_refused_slots = rs; gx_tev_refused_compare = rc;   /* the loop's own build will count it */
            ++gDlk.fb[DLK_FB_TEV]; return 0;
        } }
    int fogWhy = 0; dlk64 rtFt = RT_T0();
    if (fogWant()) {
        if ((gFogType & 8) || gProjOrtho) fogWhy = 2;
        else if (k.numTex >= 8) { fogWhy = 2; ++gx_fog_nd_slots; }   /* TEXCOORD0..9 are full: no varying left for the fog depth (gx_tev_key.h) */
        else if (fogKilled()) fogWhy = 3;
        else {
            fogWhy = 1; k.fogType = (uint8_t)gFogType;
            u.fogStart = gFogStart; u.fogScale = gFogEnd == gFogStart ? 0.0f : 1.0f / (gFogEnd - gFogStart);
            u.fogColor[0] = FOG_COLOR_SEL().r; u.fogColor[1] = FOG_COLOR_SEL().g; u.fogColor[2] = FOG_COLOR_SEL().b;
        }
    }
    RhiTexture* tx[TEVK_TEX] = { 0 };
    for (int sl = 0; sl < k.numTex; ++sl) {
        if (gCopyMaps >> sm[sl] & 1) { ++gDlk.fb[DLK_FB_TEV]; return 0; }
        tx[sl] = gTexMap[sm[sl]];
        if (!tx[sl]) { ++gDlk.fb[DLK_FB_TEV]; return 0; }
    }
    RT_SUBSL(RT_S_FOGTEX, rtFt);
    gTevLive = 1;
    C3_CHK_VS(r, &k, sc, (r3a & GX_R3A_C4) ? mvp : NULL, fogWhy);
    // ---- stage 2 (2026-10-08, docs/47): the same list as ONE indexed draw, transform + light + texgen in a vertex program ----
    // Taken only when gx_vs_build can express the call exactly; a list whose arrays keep changing (the morphed models) stays
    // on the CPU path below (its mesh would be re-uploaded every frame: not measured, so not done). Anything refused falls
    // through to the CPU path of this same call, every reason counted (gx_vs_refused, gDlkGpu).
    { const dlk64 tp_ = GXT_NOW(); gDlkPre += tp_ - t2; }   /* hw_Rsplit */
    if (gStfxGpuArm > 0 && rhi_vita_drawTevGx && rhi_vita_gx_mesh_create && gPxOldGuess == 0 && !r->gpuNo) {
        dlk64 g0 = GXT_NOW();
        static VsKey vk; static VsUniforms vu; dlk64 rtT; int rtOk;
        int c5go = 1;   /* R3c C5: 1 = vk / vu are ready for this draw */
        if (r->nRefresh >= 2 && (gStfxGpuArm < 2 || !rhi_vita_gx_mesh_update)) { r->gpuNo = 1; ++gDlkGpu.dynamic; c5go = 0; }
        else if (c5clean) { rtT = RT_T0(); vsBuildUniforms(&vk, &k, sc, mvp, &vu); RT_SUB(vsUs, vsN, rtT); }   /* R3c C5: the key is vk's (the base's, untouched since); only the numbers are rebuilt, into the buffer that holds their zero regions */
#define gx_vs_build c3VsBuildTimed   /* GXAOT_HOOK C3: the next line's call goes through the sampled micro-timer */
        else if (!(gC5.valid = 0, rtT = RT_T0(), rtOk = gx_vs_build(r, &k, sc, &vk, &vu, (r3a & GX_R3A_C4) ? mvp : NULL), RT_SUB(vsUs, vsN, rtT), rtOk)) { ++gDlkGpu.refused; c5go = 0; sSkinWhy = 10; }
#undef gx_vs_build   /* GXAOT_HOOK C3 */
        if (c5go) {
            const dlk64 gv_ = GXT_NOW(); gDlkGpu.vsUs += gv_ - g0;   /* hw_Rsplit: gx_vs_build */
            if (r3c) {   /* R3c C5: the self-check (host tests), then this build becomes the base the next draw is compared with */
#ifdef GX_C5_SELFCHECK
                c5Selfcheck(r, &k, &u, sc, sm, &vk, &vu, c5clean, c5would, c5clean ? gC5.keyId : gC5.keyIdSeq + 1u);
#endif
                if (!c5clean && c5shapeOk) c5Capture(&k, sc, sm, &c5shape, &vk);
            }
            const VsKey* vkd = &vk; VsKey vks;   /* R8: the key the backend draws with: vk, or (skinned) vk + hasSkin; vk itself stays the C3/C5 memo's, unskinned */
            if (skinT || r->skinT) {
                if (skinT) {
                    if (!dlkSkinPrep(r, skinT)) return 0;
                    vks = vk; vks.hasSkin = 1;
                    if (&gGxSkinChkCpu && gGxSkinChkCpu) gx_skin_hal_oracle(skinT, r->pos, r->fidx, r->ndist, r->nrm != NULL, gx_skin_vs_palette());   /* R8 HAL oracle (direct + gx_skin_check.txt) */
                    { const int vc_ = tev_vs_check(&vks); if (vc_) { sSkinWhy = 9; gx_skin_hal_why(16 + vc_); return 0; } }   /* 16 + the VSW_* code: [r8-why] columns 16..31 */
                    vkd = &vks;
                }
                const uint32_t sid_ = gx_skin_table_id(skinT);
                if (r->gpuMesh && !gx_skin_mesh_reusable_id(r->skinId, sid_)) { rhi_vita_gx_mesh_destroy(r->gpuMesh); r->gpuMesh = NULL; --gDlkGpu.meshes; }
                r->skinT = skinT; r->skinId = sid_;
            }
            if (!r->gpuMesh || r->meshStale) { rtT = RT_T0(); dlkMesh(r); RT_SUB(meshUs, meshN, rtT); }
            const dlk64 gm_ = GXT_NOW(); gDlkGpu.meshUs += gm_ - gv_;   /* hw_Rsplit: mesh build / vertex re-pack + upload */
            if (!r->gpuMesh) { ++gDlkGpu.noMesh; sSkinWhy = 11; }
            else {
                rtT = RT_T0();
                if (gPxActive) { rhi_setAlphaMode(gRhi, am); if (rhi_vita_setDepthState) rhi_vita_setDepthState(gZCompare, gZFunc, gZUpdate); }
                rhi_setColorTransform(gRhi, mvp);
                RT_SUBSL(RT_S_STATE, rtT);
                const dlk64 gs_ = GXT_NOW(); gDlkGpu.stUs += gs_ - gm_;   /* hw_Rsplit: per-draw state to the backend */
                /* R3a C3 program hint: the TEV key serial, the VS key serial and the final fog type identify the (VsKey, TevKey) pair the backend is about to look a program up for; the GXM shader module
                 * skips its compare / hash when the hint equals the last call's (rhi_gxm_shader.c).  0 = no hint.  Consumed (and cleared) by rhi_gxm_drawTevGx. */
                gx_prog_hint = skinT ? 0ull : ((r3a & GX_R3A_C5) && gC5.valid) ? (1ull << 62) | (unsigned long long)gC5.keyId   /* R3c C5: bit 62 | the base's key id (it moves at every full build, so equal ids mean the same raw keys) */
                             : (r3a & GX_R3A_C3) ? (1ull << 63) | ((unsigned long long)(gx_tev_key_serial & 0xFFFFFFu) << 39) | ((unsigned long long)(gx_vs_key_serial & 0x7FFFFFFFu) << 8) | k.fogType : 0ull;   /* bit 63 | tev serial 24 bits | vs serial 31 bits | fog type 8 bits */
                rtT = RT_T0(); rtOk = rhi_vita_drawTevGx(&k, &u, tx, vkd, &vu, r->gpuMesh, am); RT_SUB(drawUs, drawN, rtT);
                gDlkGpu.drUs += GXT_NOW() - gs_;   /* hw_Rsplit: the backend draw (rhi_gxm_drawTevGx) */
                if (rtOk) {
                    if (skinT) gx_skin_hal_draw();   /* R8: GXSK_DRAWS */
                    u32 tp = 0; if (r3a & GX_R3A_C4) tp = r->nprimTri; else for (u32 p = 0; p < nprim; ++p) tp += r->prim[p].ntri != 0;   /* R3a C4: cached in the record */
                    gx_prims += nprim; gx_prim_verts += r->declaredSum;
                    gx_tev_note_draw(r->ntri, gTexMap);
                    gx_tev_draws += tp;
                    if (fogWhy) { if (fogWhy == 1) gx_fog_drawn += tp; else if (fogWhy == 2) gx_fog_nd_ortho += tp; else gx_fog_nd_off += tp; fogCounted(); }
                    ++gDlk.calls; if (hit) ++gDlk.hits; gDlk.stripV += r->nstrip; gDlk.distV += nd;
                    ++gDlkGpu.calls; gDlkGpu.verts += nd; gDlkGpu.tris += r->ntri; gDlkGpu.us += GXT_NOW() - g0;
                    return 1;
                }
                ++gDlkGpu.backendNo; sSkinWhy = 12;
            }
        }
    }
    if (skinT) return 0;   /* R8: the CPU path would draw the file vertices unskinned (the blend was skipped on purpose): the call site counts the skip */
    const size_t nOut = (size_t)r->ntri * 3;
    if (nOut > DLK_MAXTRIV) { ++gDlk.fb[DLK_FB_OOM]; return 0; }
    if (gDlkDtvCap < nd) { size_t nc = gDlkDtvCap ? gDlkDtvCap : 1024; while (nc < nd) nc *= 2; void* q = realloc(gDlkDtv, nc * sizeof *gDlkDtv); if (!q) { ++gDlk.fb[DLK_FB_OOM]; return 0; } gDlkDtv = (TevVertex*)q; gDlkDtvCap = nc; }
    if (gDlkOutCap < nOut) { size_t nc = gDlkOutCap ? gDlkOutCap : 4096; while (nc < nOut) nc *= 2; void* q = realloc(gDlkOut, nc * sizeof *gDlkOut); if (!q) { ++gDlk.fb[DLK_FB_OOM]; return 0; } gDlkOut = (TevVertex*)q; gDlkOutCap = nc; }
    const int needN = pxNeedNormals(), havePnmtx = r->hasPnm, haveClr = r->hasClr;
    if (needN && gDlkNrmTCap < nd) { size_t nc = gDlkNrmTCap ? gDlkNrmTCap : 1024; while (nc < nd) nc *= 2; void* q = realloc(gDlkNrmT, nc * 3 * sizeof(float)); if (!q) { ++gDlk.fb[DLK_FB_OOM]; return 0; } gDlkNrmT = (float*)q; gDlkNrmTCap = nc; }
    TevVertex* dtv = gDlkDtv;
    t3 = GXT_NOW();
    // ---- transform: position through the pos matrix of its slot (skinVertex), normal through the normal matrix --------
    for (u32 d = 0; d < nd; ++d) {
        TevVertex* o = &dtv[d]; memset(o, 0, sizeof *o);
        RhiTexVertex v; v.x = r->pos[3*d]; v.y = r->pos[3*d+1]; v.z = r->pos[3*d+2]; v.rgba = 0; v.u = v.v = 0;
        int pnmtx = r->pnm ? r->pnm[d] : 0;
        skinVertex(&v, pnmtx, havePnmtx);
        o->x = v.x; o->y = v.y; o->z = v.z;
        if (needN && (r->fl[d] & 2)) {   /* to view space by the normal matrix of this vertex's slot (decodePrim's expression) */
            int slot2 = havePnmtx ? (pnmtx / 3) : gCurMtx;
            if ((unsigned)slot2 >= GX_MTX_SLOTS) slot2 = 0;
            const float (*N)[4] = gNrmMtx[slot2];
            float x = r->nrm[3*d], y = r->nrm[3*d+1], z = r->nrm[3*d+2];
            gDlkNrmT[3*d]   = N[0][0]*x + N[0][1]*y + N[0][2]*z;
            gDlkNrmT[3*d+1] = N[1][0]*x + N[1][1]*y + N[1][2]*z;
            gDlkNrmT[3*d+2] = N[2][0]*x + N[2][1]*y + N[2][2]*z;
        }
    }
    t4 = GXT_NOW();
    // ---- light ----------------------------------------------------------------------------------------------
    {   static const float zero3[3] = { 0, 0, 0 };
        for (u32 d = 0; d < nd; ++d) {
            u32 in = r->clr ? r->clr[d] : 0xFFFFFFFFu;
            if (gPxActive) {
                RhiTexVertex pv; pv.x = dtv[d].x; pv.y = dtv[d].y; pv.z = dtv[d].z; pv.rgba = in; pv.u = pv.v = 0;
                dtv[d].c0 = pxChannelColour(in, haveClr, &pv, (needN && (r->fl[d] & 2)) ? gDlkNrmT + 3*d : zero3, (r->fl[d] >> 1) & 1);
            } else dtv[d].c0 = in;
        } }
    t5 = GXT_NOW();
    // ---- texgen: each slot's coordinate for each DISTINCT vertex ---------------------------------------------
    {   u8 sf[TEVK_TEX], ss[TEVK_TEX], sM[TEVK_TEX], sN[TEVK_TEX], sP[TEVK_TEX]; int ident[TEVK_TEX], tmiJ[TEVK_TEX], anyGen = 0, nt = r->ntex, pnSeenD = 0, tmiSeen = 0, needNbt = 0;
        for (int sl = 0; sl < k.numTex; ++sl) {
            gx_tev_texgen(sc[sl], &sf[sl], &ss[sl], &sM[sl], &sN[sl], &sP[sl]);
            tmiJ[sl] = sc[sl] < 8 && r->tmiIdx[sc[sl]] >= 0 && !tmiOld() ? r->tmiIdx[sc[sl]] : -1;   /* the vertex names this texgen's matrix: its byte's column */
            ident[sl] = sf[sl] == 1 && ss[sl] >= 4 && ss[sl] <= 11 && !tgMtx(sM[sl]) && !(sP[sl] >= 64 && sP[sl] < 64 + 3 * 20) && tmiJ[sl] < 0;
            if (!ident[sl]) anyGen = 1;
            if (tgIsPn(sM[sl]) && !pnSeenD) { ++gx_texgen_pn_cpu; pnSeenD = 1; }
            if (tmiJ[sl] >= 0 && !tmiSeen) { ++gx_texgen_tmi_cpu; tmiSeen = 1; }
            needNbt |= ss[sl] == 2 || ss[sl] == 3;
        }
        if (needNbt && !tmiOld()) { ++gx_texgen_nbt; auxEnsure(); }
        unsigned u0 = gx_tev_texgen_unsupported;
        if (anyGen) memset(gVTex[0], 0, sizeof gVTex[0]);   /* slot 0 of the per-vertex texgen inputs is the scratch texgenVertex reads */
        for (u32 d = 0; d < nd; ++d) {
            if (anyGen) {   /* texgenVertex reads gVObj/gVNrm0/gVTex[vi]: lend it index 0 (absent TEX attributes stay 0,0) */
                gVObj[0][0] = r->pos[3*d]; gVObj[0][1] = r->pos[3*d+1]; gVObj[0][2] = r->pos[3*d+2];
                if (gVNbt) memcpy(gVNbt[0], r->nbt ? r->nbt + 6 * d : kNbtDefault, sizeof kNbtDefault);
                if (r->nrm) { gVNrm0[0][0] = r->nrm[3*d]; gVNrm0[0][1] = r->nrm[3*d+1]; gVNrm0[0][2] = r->nrm[3*d+2]; } else gVNrm0[0][0] = gVNrm0[0][1] = gVNrm0[0][2] = 0.0f;
                for (int j = 0; j < nt; ++j) { gVTex[0][r->texAttr[j]][0] = r->tex[((size_t)d * nt + j) * 2]; gVTex[0][r->texAttr[j]][1] = r->tex[((size_t)d * nt + j) * 2 + 1]; }
            }
            for (int sl = 0; sl < k.numTex; ++sl) {
                if (ident[sl]) { int j = r->texIdx[ss[sl] - 4]; dtv[d].t[sl][0] = j >= 0 ? r->tex[((size_t)d * nt + j) * 2] : 0.0f; dtv[d].t[sl][1] = j >= 0 ? r->tex[((size_t)d * nt + j) * 2 + 1] : 0.0f; }
#ifdef BREAK_19   /* host negative control: the cached path forgets the per-vertex index (the per-primitive path keeps it) */
                else texgenVertex(0, sf[sl], ss[sl], sM[sl], sN[sl], sP[sl], -1, dtv[d].t[sl]);
#else
                else texgenVertex(0, sf[sl], ss[sl], sM[sl], sN[sl], sP[sl], tmiJ[sl] >= 0 ? (int)r->tmi[(size_t)d * r->ntmi + tmiJ[sl]] : -1, dtv[d].t[sl]);
#endif
            }
        }
        if (nd) gx_tev_texgen_unsupported = u0 + (gx_tev_texgen_unsupported - u0) / nd * r->stripVTri;   /* the counter's meaning: per STRIP vertex of a drawn primitive */
    }
    t6 = GXT_NOW();
    // ---- expand every primitive through its map into ONE buffer ------------------------------------------------
    u32 n = 0, triPrims = 0;
    for (u32 p = 0; p < nprim; ++p) { u32 m = dlkExpand(r, &r->prim[p], dtv, gDlkOut + n); n += m; triPrims += m != 0; }
    t7 = GXT_NOW();
    // ---- the state calls the per-primitive loop makes, then ONE backend call -----------------------------------
    if (gPxActive) { rhi_setAlphaMode(gRhi, am); if (rhi_vita_setDepthState) rhi_vita_setDepthState(gZCompare, gZFunc, gZUpdate); }
    rhi_setColorTransform(gRhi, mvp);
    int drawn = rhi_vita_drawTev(&k, &u, tx, gDlkOut, (uint32_t)n, mvp, am);
    t8 = GXT_NOW();
    gDlk.t[2] += t4 - t3; gDlk.t[3] += t5 - t4; gDlk.t[4] += t6 - t5; gDlk.t[5] += t7 - t6; gDlk.t[6] += t8 - t7;
    if (!drawn) { ++gDlk.fb[DLK_FB_BACKEND]; return 0; }      /* nothing was drawn: the loop repeats the whole call */
    gx_prims += nprim; gx_prim_verts += r->declaredSum;
    gx_tev_note_draw(r->ntri, gTexMap);
    gx_tev_draws += triPrims;
    if (fogWhy) { if (fogWhy == 1) gx_fog_drawn += triPrims; else if (fogWhy == 2) gx_fog_nd_ortho += triPrims; else gx_fog_nd_off += triPrims; fogCounted(); }
    ++gDlk.calls; if (hit) ++gDlk.hits; gDlk.stripV += r->nstrip; gDlk.distV += nd;
    return 1;
}
/* hw_Rsplit (docs/54 2026-10-24): R's display-list call split, cumulative microseconds (0 when the always-on timers are off: C3 arm 107).
 * 0 entry total | 1 key+lookup | 2 VAT+list hash | 3 array hash | 4 decode/build/refresh | 5 pre (TEV key, fog, tex checks) | 6 GPU path total | 7 vs_build | 8 mesh
 * | 9 state | 10 drawTevGx | 11 vertex re-upload (inside 8) | 12 cpu transform | 13 light | 14 texgen | 15 expand | 16 cpu backend | 17 immediate entry | 18 tex load | 19 copy
 * | 20 imm decode+transform x8 | 21 imm light x8 | 22 imm expand+submit x8 | 23 GPU-path calls | 24 dlk calls */
void gx_rsplit_times(unsigned long long o[25]) {
    extern unsigned long long gx_t_decode, gx_t_light, gx_t_submit;
    o[0] = gx_w_dl_us; o[1] = gDlkVal[0]; o[2] = gDlkVal[1]; o[3] = gDlkVal[2]; o[4] = gDlk.t[1]; o[5] = gDlkPre; o[6] = gDlkGpu.us; o[7] = gDlkGpu.vsUs;
    o[8] = gDlkGpu.meshUs; o[9] = gDlkGpu.stUs; o[10] = gDlkGpu.drUs; o[11] = gDlkGpu.updUs; o[12] = gDlk.t[2]; o[13] = gDlk.t[3]; o[14] = gDlk.t[4]; o[15] = gDlk.t[5];
    o[16] = gDlk.t[6]; o[17] = gx_w_imm_us; o[18] = gx_w_tex_us; o[19] = gx_w_copy_us; o[20] = gx_t_decode * 8; o[21] = gx_t_light * 8; o[22] = gx_t_submit * 8;
    o[23] = gDlkGpu.calls; o[24] = gDlk.calls;
}
void gx_win_times(unsigned long long out[8]) {   /* vita_tool_main.c's [win] line: cumulative microseconds / counts */
    out[0] = gx_w_dl_us; out[1] = gDlk.t[0] + gDlk.t[1] + gDlk.t[2] + gDlk.t[3] + gDlk.t[4] + gDlk.t[5] + gDlk.t[6]; out[2] = gDlkGpu.us;
    out[3] = gx_w_imm_us; out[4] = gx_w_tex_us; out[5] = gx_w_copy_us; out[6] = gStfxGxDlCalls; out[7] = gx_imm_prims;
}
void gx_dlk_report(void) {   /* the [dlk] lines (vt_game.c's status block, after gx_dlc_report) */
    if (!gDlkInit) return;
    unsigned fb = 0; for (int q = 0; q < DLK_NFB; ++q) fb += gDlk.fb[q];
    printf("[dlk] cache %s | calls taken %u (hits %u) | builds %u (new %u, rebuilt %u) refreshes %u | records %u, %llu bytes, evictions %u\n",
           gStfxDlCacheArm ? "ON" : "OFF", gDlk.calls, gDlk.hits, gDlk.builds, gDlk.misses, gDlk.rebuilds, gDlk.refreshes, gDlk.recs, gDlk.bytes, gDlk.evictions);
    printf("[dlk] validate split, cumulative ms: key + lookup %.1f | VAT + list hash %.1f | array hash %.1f\n", gDlkVal[0] / 1000.0, gDlkVal[1] / 1000.0, gDlkVal[2] / 1000.0);
    printf("[dlk] key reuse: built %u reused %u\n", gDlkKeyBuilt, gDlkKeyReused);
    printf("[dlk] validation by events: %s | events: %u frees (ring overflows %u, records touched %u) | %u writes into %u buffers%s, calls on records reading them %u | skipped: %u list hashes, %u array hashes | forced full checks %u | MISSES %u\n",
           gDlkEvt == 2 ? "ON, events IGNORED (dl_events.txt 2: the break arm)" : gDlkEvt ? "ON (dl_events.txt 1)" : "off: every call in full",
           gDlkEv.frees, gDlkEv.overflow, gDlkEv.touched, gDlkEv.writes, gDlkDynOld ? gDlkDynN : gDlkDyn2.n, gDlkDynFull ? " (TABLE FULL: every record dynamic)" : "", gDlkEv.dynCalls,
           gDlkEv.skipList, gDlkEv.skipArr, gDlkEv.forced, gDlkEv.miss);
    if (!gDlkDynOld) printf("[dlk] dyn set: %u intervals (peak %u) | grew %u times | frees that retired/trimmed %u (intervals retired %u, trimmed %u, kept whole %u) | coarse merges at capacity %u\n",
           gDlkDyn2.n, gDlkDyn2.peak, gDlkDyn2.grows, gDlkFreesDyn, gDlkDyn2.retired, gDlkDyn2.trimmed, gDlkDyn2.splitSkipped, gDlkDyn2.coarse);
    printf("[dlk] fell back %u:", fb);
    for (int q = 0; q < DLK_NFB; ++q) if (gDlk.fb[q]) printf(" %s %u,", kDlkFbName[q], gDlk.fb[q]);
    printf(" | uncacheable records: overrun %u, too-many-distinct %u, span %u, walk %u\n", gDlk.whyN[DLK_WHY_OVERRUN], gDlk.whyN[DLK_WHY_TOOBIG], gDlk.whyN[DLK_WHY_SPAN], gDlk.whyN[DLK_WHY_WALK]);
    printf("[dlk] vertices: %u strip -> %u distinct (%.1f%%) | cumulative ms: validate %.1f decode %.1f transform %.1f light %.1f texgen %.1f expand %.1f backend %.1f\n",
           gDlk.stripV, gDlk.distV, gDlk.stripV ? 100.0 * gDlk.distV / gDlk.stripV : 0.0, gDlk.t[0] / 1000.0, gDlk.t[1] / 1000.0, gDlk.t[2] / 1000.0,
           gDlk.t[3] / 1000.0, gDlk.t[4] / 1000.0, gDlk.t[5] / 1000.0, gDlk.t[6] / 1000.0);
    printf("[texgen] per-vertex texture matrix (TEXnMTXIDX) %s: draws with one on the CPU path %u, on the GPU vertex path %u | draws with a BINRM/TANGENT source %u\n",
           tmiOld() ? "OFF (texmtxidx_off.txt)" : "ON", gx_texgen_tmi_cpu, gx_texgen_tmi_gpu, gx_texgen_nbt);
    printf("[dlk] array refreshes: fast (POS/NRM only) %u, full %u, fast-vs-full self-check mismatches %u | array bytes hashed %llu, records hashing index runs %u, whole spans %u\n",
           gDlkFastN, gDlkFullN, gDlkFastBad, gDlkHashBytes, gDlkRunRecs, gDlkRunNo);
    printf("[dlg] GPU vertex path %s | calls %u (verts %llu, tris %llu), %.1f ms cumulative | stayed on the CPU: build refused %u, changing arrays %u records, no mesh %u, backend refused %u | meshes alive %u, %llu bytes uploaded | vertex re-uploads %u, %llu bytes, %.1f ms cumulative",
           gStfxGpuArm ? "ON" : "off", gDlkGpu.calls, gDlkGpu.verts, gDlkGpu.tris, gDlkGpu.us / 1000.0, gDlkGpu.refused, gDlkGpu.dynamic, gDlkGpu.noMesh, gDlkGpu.backendNo,
           gDlkGpu.meshes, gDlkGpu.meshBytes, gDlkGpu.updates, gDlkGpu.updBytes, gDlkGpu.updUs / 1000.0);
    printf(" | refusals:"); for (int q = 1; q < VSW_N; ++q) if (gx_vs_refused[q]) printf(" %s %u,", tev_vs_why_name(q), gx_vs_refused[q]);
    printf("\n");
}
void gx_rt_enable(int on) {
    gRt.on = on; gRt.act = 0;
    if (on) {   /* the cost of one clock read here, so the report can state its own overhead */
        unsigned long long t0 = GXT_NOW(), acc = 0; for (int i = 0; i < 2000; ++i) acc += GXT_NOW(); unsigned long long t1 = GXT_NOW();
        gRt.clkNs = (unsigned)((t1 - t0) * 1000ull / 2000ull); (void)acc;
    }
}
static unsigned rtFallbacks(void) { unsigned f = 0; for (int q = 0; q < DLK_NFB; ++q) f += gDlk.fb[q]; return f; }
/* The [rt] lines (vita_tool_main.c, every 60 presents when rt_stats.txt is 1).
   UNITS: every "ms/frame" below is per FRAME. "sampled" figures are totals over the sampled frames (1 in RT_SAMPLE) divided by the number of
   sampled frames, and a call count is the total over the sampled frames divided by the same number, so "calls/frame" and "ms/frame" and
   "us/call" (= ms/frame / calls/frame) are mutually consistent. "all frames" figures come from the always-on cumulative counters over the
   whole window. The two are comparable on average, not frame for frame. Slots overlap (a slot includes the backend calls made inside it). */
void gx_rt_report(int presents) {
    extern unsigned gx_copy_gpu, gx_copy_mem, gx_peekz_real, gx_peekz_calls;
    extern unsigned long long gx_t_decode, gx_t_light, gx_t_submit; extern unsigned gx_imm_prims;
    static int p_n; static dlk64 p_v[3], p_t[7], p_up, p_gpu, p_dec, p_lit, p_sub; static unsigned p_cg, p_cm, p_pz, p_pc, p_fb, p_prims;
    if (!gRt.on) return;
    const double nf = presents > p_n ? (double)(presents - p_n) : 1.0, fs = gRt.frames ? (double)gRt.frames : 1.0;
    if (p_n) {
        const double g = gRt.acc[RT_GAME] / 1000.0 / fs, sh = gRt.acc[RT_SHIM] / 1000.0 / fs, b = gRt.acc[RT_BACKEND] / 1000.0 / fs, w = gRt.acc[RT_WAIT] / 1000.0 / fs;
        unsigned reads = gRt.scopes; for (int i = 0; i < RT_NSUB; ++i) if (i != RT_S_MTX && i != RT_S_SETTERS) reads += gRt.subN[i]; reads += gRt.vsN + gRt.meshN + gRt.drawN + gRt.lookupN;
        reads *= 2;   /* a scope or a sub-timer = 2 clock reads */
        printf("[rt] game %.2f | shim %.2f | backend %.2f | wait %.2f | sum %.2f vs frame %.2f (ms/frame, %u sampled frames of %d presents; unbalanced leaves %u, depth overflows %u, other-thread calls %u) | instrument: %.0f scopes+timers/frame x %u ns/read = %.2f ms in a sampled frame, %.2f ms/frame averaged\n",
               g, sh, b, w, g + sh + b + w, gRt.wall / 1000.0 / fs, gRt.frames, presents - p_n, gRt.underflow, gRt.overflow, gRt.foreign,
               reads / 2.0 / fs, gRt.clkNs, reads / fs * gRt.clkNs / 1e6, reads / fs * gRt.clkNs / 1e6 / RT_SAMPLE);
        #define RTS(slot) (gRt.sub[slot] / 1000.0 / fs), (gRt.subN[slot] / fs), (gRt.subN[slot] ? (double)gRt.sub[slot] / gRt.subN[slot] : 0.0)
        printf("[rt] shim entries, inclusive of the backend calls inside, sampled (ms/frame | calls/frame | us/call): display lists %.2f | %.1f | %.1f  immediate %.2f | %.1f | %.1f  texture load %.2f | %.1f | %.1f  efb copy %.2f | %.1f | %.1f  [count only, no timer: the clock read costs more than the call] matrix loads %.1f/frame  setters %.1f/frame\n",
               RTS(RT_S_ENTRY_DL), RTS(RT_S_ENTRY_IMM), RTS(RT_S_ENTRY_TEX), RTS(RT_S_ENTRY_COPY), gRt.subN[RT_S_MTX] / fs, gRt.subN[RT_S_SETTERS] / fs);
        printf("[rt] inside display lists, sampled (ms/frame | calls/frame | us/call): cache attempt %.2f | %.1f | %.1f  of which tev key build %.2f | %.1f | %.1f, fog+texture checks %.2f | %.1f | %.1f, state to backend %.2f | %.1f | %.1f, uniforms gx_vs_build %.2f | %.1f | %.1f, mesh %.2f | %.1f | %.1f, backend draw %.2f | %.1f | %.1f (program lookup %.2f | %.1f | %.1f)  FALLBACK per-primitive loop %.2f | %.1f | %.1f\n",
               RTS(RT_S_DLK_ALL), RTS(RT_S_TEVBUILD), RTS(RT_S_FOGTEX), RTS(RT_S_STATE),
               gRt.vsUs / 1000.0 / fs, gRt.vsN / fs, gRt.vsN ? (double)gRt.vsUs / gRt.vsN : 0.0, gRt.meshUs / 1000.0 / fs, gRt.meshN / fs, gRt.meshN ? (double)gRt.meshUs / gRt.meshN : 0.0,
               gRt.drawUs / 1000.0 / fs, gRt.drawN / fs, gRt.drawN ? (double)gRt.drawUs / gRt.drawN : 0.0, gRt.lookupUs / 1000.0 / fs, gRt.lookupN / fs, gRt.lookupN ? (double)gRt.lookupUs / gRt.lookupN : 0.0,
               RTS(RT_S_FALLBACK));
        #undef RTS
        {   /* does the attribution close? display-list entry = cache attempt + fallback loop + residue (perf arms, census) */
            const double dl = gRt.sub[RT_S_ENTRY_DL] / 1000.0 / fs, at = gRt.sub[RT_S_DLK_ALL] / 1000.0 / fs, fb = gRt.sub[RT_S_FALLBACK] / 1000.0 / fs;
            const double inner = (gRt.sub[RT_S_TEVBUILD] + gRt.sub[RT_S_FOGTEX] + gRt.sub[RT_S_STATE] + gRt.vsUs + gRt.meshUs + gRt.drawUs) / 1000.0 / fs;
            const double valid = (double)((gDlkVal[0] - p_v[0]) + (gDlkVal[1] - p_v[1]) + (gDlkVal[2] - p_v[2])) / 1000.0 / nf;
            const double ent = dl + (gRt.sub[RT_S_ENTRY_IMM] + gRt.sub[RT_S_ENTRY_TEX] + gRt.sub[RT_S_ENTRY_COPY]) / 1000.0 / fs;
            printf("[rt] closure: display-list entry %.2f = cache attempt %.2f + fallback loop %.2f + residue %.2f | cache attempt %.2f = validation %.2f (all-frames counters) + timed parts %.2f + unattributed %.2f | all entries %.2f of shim+backend %.2f = %.0f%% (the rest is backend work outside any shim entry: present, movie texture uploads, vtable calls from the game)\n",
                   dl, at, fb, dl - at - fb, at, valid, inner, at - valid - inner, ent, sh + b, sh + b > 0 ? 100.0 * ent / (sh + b) : 0.0);
        }
        printf("[rt] per-primitive path (the immediate draws AND the fallback loop; its own 1-in-8 sampling x8, all frames, ms/frame): decode+transform %.2f | light %.2f | expand+submit %.2f | prims/frame %.0f | cache fallbacks/frame %.2f\n",
               (gx_t_decode - p_dec) * 8 / 1000.0 / nf, (gx_t_light - p_lit) * 8 / 1000.0 / nf, (gx_t_submit - p_sub) * 8 / 1000.0 / nf, (double)(gx_imm_prims - p_prims) / nf,
               (double)(rtFallbacks() - p_fb) / nf);
        printf("[rt] display-list split (ms/frame, all frames): key+lookup %.2f | VAT+list hash %.2f | array hash %.2f | decode %.2f | transform %.2f | light %.2f | texgen %.2f | expand %.2f | cpu-path backend %.2f || GPU path %.2f, of which vertex re-upload %.2f\n",
               (gDlkVal[0] - p_v[0]) / 1000.0 / nf, (gDlkVal[1] - p_v[1]) / 1000.0 / nf, (gDlkVal[2] - p_v[2]) / 1000.0 / nf,
               (gDlk.t[1] - p_t[1]) / 1000.0 / nf, (gDlk.t[2] - p_t[2]) / 1000.0 / nf, (gDlk.t[3] - p_t[3]) / 1000.0 / nf, (gDlk.t[4] - p_t[4]) / 1000.0 / nf,
               (gDlk.t[5] - p_t[5]) / 1000.0 / nf, (gDlk.t[6] - p_t[6]) / 1000.0 / nf, (gDlkGpu.us - p_gpu) / 1000.0 / nf, (gDlkGpu.updUs - p_up) / 1000.0 / nf);
        printf("[rt] readbacks this window: GPU copies %u | CPU readbacks (copy through memory) %u | PeekZ %u (of %u calls) | glReadPixels syncs %u\n",
               gx_copy_gpu - p_cg, gx_copy_mem - p_cm, gx_peekz_real - p_pz, gx_peekz_calls - p_pc, gRt.syncs);
    }
    p_n = presents; for (int i = 0; i < 3; ++i) p_v[i] = gDlkVal[i]; for (int i = 0; i < 7; ++i) p_t[i] = gDlk.t[i];
    p_up = gDlkGpu.updUs; p_gpu = gDlkGpu.us; p_cg = gx_copy_gpu; p_cm = gx_copy_mem; p_pz = gx_peekz_real; p_pc = gx_peekz_calls;
    p_dec = gx_t_decode; p_lit = gx_t_light; p_sub = gx_t_submit; p_prims = gx_imm_prims; p_fb = rtFallbacks();
    rt_window_reset(&gRt);
}


// --- GXCallDisplayList: replay a recorded GX command buffer -----------------
// A display list is a stream of primitive commands: an opcode byte (prim|fmt),
// a u16 vertex count, then that many vertices in the current descriptor layout.
// Non-draw opcodes (state loads) don't appear in SFA map block lists.
// Progress counters, read by the Vita tool's watchdog and per-present log (vita_tool_main.c):
// a stalled frame that is still calling lists is SLOW, one whose counters are flat is STUCK.
unsigned gStfxGxDlCalls, gStfxGxDlBytes;
// THE FRAME, SPLIT (2026-10-08, docs/47 "where the rest of the frame goes"): exact microseconds inside the four GX entry points that
// do real work, read per 60-present window by vita_tool_main.c ([win] line). Display lists (both cached paths + the per-primitive
// loop, and any shader compile they trigger), immediate GXBegin draws, GXLoadTexObj (decode + upload), GXCopyTex (EFB copies).
unsigned long long gx_w_dl_us, gx_w_imm_us, gx_w_tex_us, gx_w_copy_us;
static void gxCallDisplayListImpl(void* listv, u32 nbytes);
/* The per-primitive loop (decodePrim: CPU decode/transform/light/expand + drawTev) for a list the cache refused or is off for.  Was inline in
   gxCallDisplayListImpl; R8-FB needs to call it with swapped array bases. */
static void dlPrimLoop(const unsigned char* dl, unsigned nbytes) {
    unsigned i = 0;
    RT_SUBS(RT_S_FALLBACK);   /* Task 3b: from here to the end = the per-primitive loop for a list the cache refused or is off for (decodePrim: CPU decode/transform/light/expand + drawTev) */
    while (i < nbytes) {
        u8 op = dl[i++];
        if (op == 0x00) continue;              // NOP / padding
        u8 prim = op & 0xF8, fmt = op & 0x07;
        if (prim==GX_QUADS||prim==GX_TRIANGLES||prim==GX_TRIANGLESTRIP||prim==GX_TRIANGLEFAN||
            prim==GX_LINES||prim==GX_LINESTRIP||prim==GX_POINTS) {
            if (i+2 > nbytes) break;
            int nverts = (int)((dl[i]<<8)|dl[i+1]); i += 2;
            i += (unsigned)decodePrim(dl + i, (GXVtxFmt)fmt, prim, nverts, (int)(nbytes - i));
        } else {
            if (dbg()) fprintf(stderr, "[gxdraw] DL unknown op %02x at %u\n", op, i-1);
            break; // unknown opcode: stop rather than misparse
        }
    }
}

/* R8-FB: draw a skinned list that dlkCall(.., skinT) refused, exactly as the CPU blend would have: POS / NRM are pointed at a CPU-skinned copy of the
   model's arrays (gx_skin_fallback_fill: gx_skin_ref_pos / _nrm with the BOUND palette, in the file's format -- what ObjModel_Blend*Stream writes into
   vtxBuf), the list is drawn with NO skin (dlkCall(NULL), else the per-primitive loop), and the bases are put back.  gGxSkin is never touched, so the
   bound skin survives for the model's next list.  Returns 0 when the copy cannot serve this list (strides other than the file's 6 / 3, no memory). */
static int dlSkinFallback(const unsigned char* dl, unsigned nbytes, const struct GxSkinTable* t) {
    const unsigned char *cp = NULL, *cn = NULL;
    ArrayDef sp = gArray[GX_VA_POS], sn = gArray[GX_VA_NRM];
    if (sp.stride != 6) return 0;
    if (!gx_skin_fallback_fill(t, sn.stride == 3 ? sn.base : NULL, &cp, &cn) || !cp) return 0;   /* cn stays NULL unless the list's NRM array IS the model's normals */
    gArray[GX_VA_POS].base = cp;
    if (cn) gArray[GX_VA_NRM].base = cn;
    ++gVtxGen;
    gx_skin_hal_fallback();
    if (!(gStfxDlCacheArm && dlkCall(dl, nbytes, NULL))) dlPrimLoop(dl, nbytes);
    gArray[GX_VA_POS] = sp; gArray[GX_VA_NRM] = sn;
    ++gVtxGen;
    return 1;
}

void GXCallDisplayList(void* listv, u32 nbytes) { RT_SUBS(RT_S_ENTRY_DL); unsigned long long t = GXT_NOW(); RT_ENTER(RT_SHIM); gxCallDisplayListImpl(listv, nbytes); RT_LEAVE(); gx_w_dl_us += GXT_NOW() - t; }
static void gxCallDisplayListImpl(void* listv, u32 nbytes) {
    const unsigned char* dl = (const unsigned char*)listv;
    ++gStfxGxDlCalls; gStfxGxDlBytes += nbytes;
    if (STFX_ARM5()) return;   /* arm 5 (see gStfxPerfArm): the null-draw A/B; counters first */
    if (gStfxPerfArm == 2) return;   /* run 64 A/B: the frame without any display-list work */
    if (gStfxPerfArm == 3 && gx_draw_static_hint) return;   /* run 66 A/B: the frame without MAP BLOCK lists */
    if (gStfxPerfArm == 4 && gx_tev_uses_indirect()) return;   /* run 67 A/B: the frame without FUR-SHELL lists (indirect TEV) */
    /* Run 243: ~12 lists a frame in ThornTail Hollow are refused for an indirect stage and take the uncached per-primitive
       loop (est. ~25 ms/frame, and what they draw is the wrong single-texture picture). ux0:data/stairfax/ind_skip.txt
       starting with 1 = do not draw those lists at all: the A/B that measures the estimate. OFF by default (an object whose
       only pass is indirect would vanish). */
    {
        static int sIndSkip = -1;
        if (sIndSkip < 0) {
            FILE* sf = fopen("ux0:data/stairfax/ind_skip.txt", "r");
            sIndSkip = sf && fgetc(sf) == '1';
            if (sf) fclose(sf);
            fprintf(stderr, "[indskip] display lists with an indirect TEV stage: %s\n", sIndSkip ? "NOT DRAWN (ind_skip.txt 1)" : "drawn by the fallback (default)");
        }
        if (sIndSkip && gx_tev_uses_indirect()) { ++gx_ind_lists_skipped; return; }
    }
    if (dlcOn()) dlcOne(dl, nbytes);
    if (!gDlkInit) dlkInitOnce();   /* docs/47 fast path stage 1: the per-list cache, or today's loop below */
    const struct GxSkinTable* skinT = gxdSkinWant();   /* R8: unskinned lists pay one load and branch (device) */
    sSkinWhy = 13;
    if (gStfxDlCacheArm && dlkCall(dl, nbytes, skinT)) return;
    if (skinT) {   /* R8-FB: a skinned list the HAL refused is DRAWN from a CPU-skinned copy of the model's arrays (docs/54 hw_R8tour: 420 draws/60 frames were dropped and the geometry was missing); only a list the copy cannot serve is dropped */
        gx_skin_hal_why(sSkinWhy);
        if (!dlSkinFallback(dl, nbytes, skinT)) gx_skin_hal_ref();   /* R8: GXSK_REF_HAL -- drawing the file vertices unskinned would be wrong */
        return;
    }
    dlPrimLoop(dl, nbytes);
}

// --- GXBegin ---------------------------------------------------------------
static int vertexStride(GXVtxFmt fmt) {
    int s = 0;
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) s += attrSize(fmt, a);
    return s;
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (gList) {  // recording: the list header is the draw opcode + big-endian count
        listByte((unsigned char)(type | vtxfmt)); listByte((unsigned char)(nverts >> 8)); listByte((unsigned char)nverts);
        return;
    }
    gPrim = (u8)type; gPrimFmt = vtxfmt; gPrimN = nverts;
    int stride = vertexStride(vtxfmt);
    gExpectBytes = stride * nverts;
    fifoReset();
    gCollecting = (gExpectBytes > 0 && nverts > 0);
    if (dbg()) fprintf(stderr, "[gxdraw] GXBegin prim=%02x fmt=%d n=%d stride=%d expect=%d rhi=%p\n",
                       gPrim, vtxfmt, nverts, stride, gExpectBytes, (void*)gRhi);
    if (!gCollecting) decodeAndDraw(); // degenerate: nothing to read
}

// Called from each FIFO push: once a full primitive's bytes have landed, draw it.
static void fifoMaybeFlush(void) {
    if (gCollecting && gFifoLen >= gExpectBytes) { gCollecting = 0; decodeAndDraw(); }
}

// --- GX render-state + texture-object API used by intersect_render.c's 2D/text draw.
// Loose enum types (int/u8) match by symbol name (the GX enums are int-sized), as in
// gx_shim.c. Minimal bodies to link + a sane scissor; wire Z/TEV/texture state into the
// RHI when text-render CORRECTNESS is tackled (GXInitTexObj especially, for glyph textures).
// THE SCISSOR (2026-10-07, Vita run 111): GXSetScissor was an empty stub (scaffold/game_boot_gx.c,
// weak, and it won the link) and GXGetScissor answered the whole EFB whatever had been set. The
// cutscene letterbox IS a scissor (camera.c Camera_ApplyCurrentViewport: rows yOffset+6 .. 480 -
// (yOffset+6)), so the port drew the picture to both edges where retail is black; the callers that
// save, narrow and restore the scissor (gametext.c, dlls/engine 0/9/22) restored 640x480 over
// whatever was current. Now state: the getter returns what the setter was given.
static u32 gScis[4] = { 0, 0, 640, 480 };
unsigned gx_scissor_sets;
extern void rhi_vita_setScissor(int x, int y, int w, int h) __attribute__((weak));
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    gScis[0] = left; gScis[1] = top; gScis[2] = wd; gScis[3] = ht;
    ++gx_scissor_sets;
    if (rhi_vita_setScissor) rhi_vita_setScissor((int)left, (int)top, (int)wd, (int)ht);
}
// FOG (2026-10-07, Vita run 112): GXSetFog was an empty scaffold stub (scaffold/game_sky_stubs.c) and the
// port had no fog anywhere. It records what the game asks for and logs each distinct request (the first
// 24). Run 116: a PERSPECTIVE fog type is now DRAWN on TEV-program draws under a perspective projection
// (drawTev above); orthographic types and the fixed-function draws are not, and are counted ([fog] line).
// type: 0 none, 2 linear, 4 exp, 5 exp2, 6 rev exp, 7 rev exp2 (+8: orthographic).
int gFogType; float gFogStart, gFogEnd, gFogNear, gFogFar; GXColor gFogColor;
GXColor gx_fogColorR; int gx_fog_private;   /* N1 */
unsigned gx_fog_sets, gx_fog_distinct;
void GXSetFog(int type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    ++gx_fog_sets;
    if (type == gFogType && startz == gFogStart && endz == gFogEnd && nearz == gFogNear && farz == gFogFar &&
        color.r == FOG_COLOR_SEL().r && color.g == FOG_COLOR_SEL().g && color.b == FOG_COLOR_SEL().b) return;
    gFogType = type; gFogStart = startz; gFogEnd = endz; gFogNear = nearz; gFogFar = farz;
#ifdef BREAK_30   /* host test break (gx_port_test BREAK_30): GXSetFog never stores the game's variable, even in direct mode */
    gx_fogColorR = color;
#else
    gx_fogColorR = color; if (!gx_fog_private) gFogColor = color;   /* N1: threaded R must not write the game's variable */
#endif
    if (++gx_fog_distinct <= 24 || !(gx_fog_distinct & 1023))
        printf("[fog] request %u (set %u): type %d start %.1f end %.1f near %.2f far %.1f colour %02x%02x%02x -- %s\n",
               gx_fog_distinct, gx_fog_sets, type, startz, endz, nearz, farz, color.r, color.g, color.b,
               !fogWant() ? "no fog" : (type & 8) ? "orthographic: NOT DRAWN"
               : "drawn on TEV-program draws under a perspective projection; NOT drawn on fixed-function draws or under orthographic");
}
void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (left) *left = gScis[0]; if (top) *top = gScis[1]; if (wd) *wd = gScis[2]; if (ht) *ht = gScis[3];
}
// Z mode (vita fork, docs/47 run 32): was a no-op, so EVERY draw tested and wrote depth. The game
// draws a full-screen effect mesh first with z compare/update off; drawn with depth ON it sealed the
// depth buffer at near-z and every scene draw after it failed -- the frame was that mesh alone
// (run 24: white; runs 28-32: 1 colour with depth on, 108,994 with the depth test forced off).
// Passed per draw to the backend (weak: the viewers and builds without the Vita backend link as before).
// (gZCompare, gZFunc, gZUpdate: gx_hal.px, initial 1 / GX_LEQUAL / 1)
extern void rhi_vita_setDepthState(int test, int func, int write) __attribute__((weak));
GXH_INLINE void gxh_b_GXSetZMode(GxHal* h, const int mark, u8 compare_enable, int func, u8 update_enable) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    h->px.zCompare = compare_enable; h->px.zFunc = (u8)func; h->px.zUpdate = update_enable;
    h->px.pxActive = 1;
    GXH_MARK(h, mark, GXH_G_PIX);
}
GXH_PAIR(GXSetZMode, (u8 compare_enable, int func, u8 update_enable), compare_enable, func, update_enable)
void GXSetZCompLoc(u8 before_tex) { (void)before_tex; }
// GXSetTevSwapModeTable: recorded in gx_tev.c (the TEV programs read the tables)
// --- texture objects (vita fork) --------------------------------------------
// The game's GXTexObj is an opaque 32 bytes. The SDK packs it as hardware register
// images, with the image address as OSCachedToPhysical(ptr) >> 5 -- a GameCube
// PHYSICAL address, which cannot hold a host pointer. So the port keeps its OWN
// layout inside those 32 bytes: the real pointer, the geometry, and the RHI texture
// decoded on first load. Every GXInitTexObj*/GXGetTexObj*/GXLoadTexObj* here agrees
// on it; nothing else may read a GXTexObj's bytes (no game TU does).
typedef struct PortTexObj {
    void*       image;     // host pointer to big-endian GX texel data
    void*       userData;  // GXInitTexObjUserData
    RhiTexture* rhi;       // decoded + uploaded on first load; NULL until then
    u16 width, height;
    u8  fmt, wrap, mipmap; // wrap = wrap_s | wrap_t << 4; mipmap: bit 0 the game's mipmap flag, bits 1-3 (min filter + 1), bits 4-6 (mag filter + 1) from GXInitTexObjLOD (0 = never set = GX_LINEAR)
    u8  tag;               // PORT_TEXOBJ_TAG once GXInitTexObj has run
} PortTexObj;
#define PORT_TEXOBJ_TAG 0xA5
_Static_assert(sizeof(PortTexObj) <= 32, "PortTexObj must fit the game's 32-byte GXTexObj");

// Counters, read by the port's frame report: uploads growing without bound mean the
// per-object cache is leaking (re-init of a live object drops its RHI texture).
unsigned gx_tex_uploads, gx_tex_loads, gx_tex_load_uninit, gx_tex_lod_inits;
static RhiTexture* gTexMap[8];            // what each GX_TEXMAPn was last loaded with

void GXInitTexObj(void* obj, void* image, u16 width, u16 height, int fmt,
                  int wrap_s, int wrap_t, u8 mipmap) {
    PortTexObj* t = (PortTexObj*)obj;
    if (!t) return;
    memset(obj, 0, 32);                   // the game's whole object, not just our part
    t->image = image; t->width = width; t->height = height;
    t->fmt = (u8)fmt; t->wrap = (u8)((wrap_s & 15) | (wrap_t & 15) << 4); t->mipmap = mipmap ? 1 : 0;
    t->tag = PORT_TEXOBJ_TAG;
}
// STFX shadow-zbias (2026-10-12): the game says it is inside the shadow receiver (shadow_dolphin.c objDrawShadowCasterMesh); the backend applies a depth bias to
// draws made while it is set (ux0:data/stairfax/shadow_zbias.txt). Weak: absent on the host.
extern void rhi_vita_setShadowZBias(int on) __attribute__((weak));
void gx_draw_setShadowRecv(int on) { if (rhi_vita_setShadowZBias) rhi_vita_setShadowZBias(on); }
// Filtering/LOD: the RHI samples one level, bilinear, for every ordinary texture (counted, not applied). The min/mag FILTER is now
// RECORDED in the object (PortTexObj.mipmap bits 1-6) so that a GPU EFB copy -- the shadow maps -- can be sampled the way the game
// asked (gxLoadTexObjImpl -> rhi_vita_copyTexSetSampler). max_lod and the LOD bias reach the backend since run 273; min_lod, the
// bias clamp, edge LOD and anisotropy are still dropped.
// AUTHORED MIP LEVELS (run 264). The game's mipmapped textures carry their own smaller levels right after level 0 (GXGetTexBufferSize with
// mipmap = 1 sizes the whole chain; GXInitTexObjLOD's max_lod is the last LOD, so max_lod + 1 levels). Run 258 built every chain with
// glGenerateMipmap instead: a box filter over level 0, which on cut-out textures averages the leaf colour with the texels under alpha 0 --
// the black border Tyler saw on distant trees. The level count does not fit the 32-byte object (full on a 64-bit host), so it is kept here by
// image address; texUploadAuthoredMips decodes the levels and hands them to the backend. RETRACTED 2026-10-14: until run 273 the backend's
// upload never reached the GPU (vitaGL re-derives level > 0 from level 0; rhi_vita.c rhi_vita_texSetMips). ux0:data/stairfax/tex_mips_authored_off.txt
// starting with 1 = run 258's generated chains (A/B).
// 2026-10-14 (run 273): the entry also keeps the LOD BIAS, in 1/8 levels (GXM's unit), and a full probe run is COUNTED (gx_tex_lod_drops:
// the table never evicts, and a dropped note silently cost the texture its chain).
#define TEX_LOD_CAP 4096
static struct { const void* image; u8 levels; s8 bias8; } gTexLod[TEX_LOD_CAP];
unsigned gx_tex_lod_notes, gx_tex_lod_drops;
static void texLodNote(const void* image, int levels, int bias8) {
    unsigned h = (unsigned)(((size_t)image >> 5) * 2654435761u) & (TEX_LOD_CAP - 1);
    for (int i = 0; i < 16; ++i, h = (h + 1) & (TEX_LOD_CAP - 1))
        if (!gTexLod[h].image || gTexLod[h].image == image) {
            if (!gTexLod[h].image) ++gx_tex_lod_notes;
            gTexLod[h].image = image; gTexLod[h].levels = (u8)levels; gTexLod[h].bias8 = (s8)bias8; return;
        }
    ++gx_tex_lod_drops;
}
static int texLodLevels(const void* image, int* bias8) {
    unsigned h = (unsigned)(((size_t)image >> 5) * 2654435761u) & (TEX_LOD_CAP - 1);
    if (bias8) *bias8 = 0;
    for (int i = 0; i < 16; ++i, h = (h + 1) & (TEX_LOD_CAP - 1)) {
        if (gTexLod[h].image == image) { if (bias8) *bias8 = gTexLod[h].bias8; return gTexLod[h].levels; }
        if (!gTexLod[h].image) return 0;
    }
    return 0;
}
void GXInitTexObjLOD(void* obj, int min_filt, int mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, u8 bias_clamp, u8 do_edge_lod, int max_aniso) {
    (void)min_lod;
    {   PortTexObj* lt = (PortTexObj*)obj;
        float b8 = lod_bias * 8.0f; int bias8 = (int)(b8 < 0 ? b8 - 0.5f : b8 + 0.5f);
        if (bias8 < -31) bias8 = -31; else if (bias8 > 32) bias8 = 32;
        if (lt && lt->tag == PORT_TEXOBJ_TAG && lt->image && (lt->mipmap & 1) && max_lod >= 1.0f) texLodNote(lt->image, (int)max_lod + 1 > 11 ? 11 : (int)max_lod + 1, bias8);
    }
    (void)bias_clamp; (void)do_edge_lod; (void)max_aniso;
    ++gx_tex_lod_inits;
    PortTexObj* t = (PortTexObj*)obj;
    if (t && t->tag == PORT_TEXOBJ_TAG)
        t->mipmap = (u8)((t->mipmap & 1) | (((unsigned)(min_filt + 1) & 7) << 1) | (((unsigned)(mag_filt + 1) & 7) << 4));
}
static int texObjMinFilt(const PortTexObj* t) { unsigned v = (t->mipmap >> 1) & 7; return v ? (int)v - 1 : 1; }   // GX_NEAR 0, GX_LINEAR 1, NEAR_MIP_NEAR 2, LIN_MIP_NEAR 3, NEAR_MIP_LIN 4, LIN_MIP_LIN 5
static int texObjMagFilt(const PortTexObj* t) { unsigned v = (t->mipmap >> 4) & 7; return v ? (int)v - 1 : 1; }
void  GXInitTexObjUserData(void* obj, void* ud) { if (obj) ((PortTexObj*)obj)->userData = ud; }
void* GXGetTexObjUserData(const void* obj) { return obj ? ((const PortTexObj*)obj)->userData : NULL; }
int   GXGetTexObjFmt(const void* obj)    { return ((const PortTexObj*)obj)->fmt; }
u16   GXGetTexObjWidth(const void* obj)  { return ((const PortTexObj*)obj)->width; }
u16   GXGetTexObjHeight(const void* obj) { return ((const PortTexObj*)obj)->height; }
void* GXGetTexObjData(const void* obj)   { return ((const PortTexObj*)obj)->image; }

// ---- the texture CACHE (2026-09-27) ------------------------------------------------------------
// GXInitTexObj zeroes the object, so a per-object RHI texture is dropped on every re-init: the THP
// player re-inits its three I8 planes every frame, which created ~1.2 MB of textures per frame and
// never freed one. Textures are now cached by (image, fmt, w, h), shared by every object that
// names the same texels, and RE-DECODED when the texels are rewritten -- which the game announces
// (DCFlushRange / DCStoreRange after a CPU write, os_shim.c) and the THP decoder announces for its
// planes. Each entry is registered in every 64 KB page it spans, so an invalidation only looks at
// the pages it touches. Counters: gx_tex_cache_entries, gx_tex_redecodes.
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod);
#define TC_CAP 8192
#define TC_PAGES 4096
typedef struct { const u8* image; u32 size; u16 w, h; u8 fmt, dirty, wrap; RhiTexture* rhi; int next[4]; u32 pages[4]; u32 gen; } TexEnt;   /* gen: the movie decode this upload holds (yuvPlaneGen) */
static TexEnt gTc[TC_CAP + 1];            // [0] unused: slots are 1-based
static int gTcN, gTcPage[TC_PAGES];       // page bucket heads (entry index, 0 = empty)
unsigned gx_tex_cache_entries, gx_tex_redecodes, gx_tex_cache_full;
// TEXTURE WRAP (2026-10-11). GXInitTexObj records wrap_s | wrap_t << 4 in PortTexObj.wrap; nothing read it, and the backend made every
// texture GL_REPEAT, so a texture the game CLAMPS (the sky backdrop gradient, light falloff ramps, projected lights) wrapped.
// The cache entry now carries the wrap in its KEY: one image bound through two tex objs with different wraps is two textures
// (a few KB..MB of duplicate, counted by gx_tex_cache_entries) instead of a GL parameter change per bind -- no GL call at all
// at bind, and a draw that samples the same image under two modes in two maps still gets both right. The backend applies the
// modes once, right after creation (rhi_vita_texSetWrap; weak: absent on the host). Filter modes: GXInitTexObjLOD records
// nothing (the RHI samples one bilinear level) -- a separate change, not done here.
// ux0:data/stairfax/tex_wrap_old.txt starting with 1 = the old behaviour: key 0, no backend call, everything GL_REPEAT (A/B).
extern void rhi_vita_texSetWrap(RhiTexture*, int wrapS, int wrapT) __attribute__((weak));   /* rhi_vita.c */
static int gWrapOld = -1;
static u8 texWrapKey(const PortTexObj* t) {
    if (gWrapOld < 0) {
        FILE* f = fopen("ux0:data/stairfax/tex_wrap_old.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gWrapOld = c == 49; printf("[wrap] texture wrap modes %s\n", gWrapOld ? "OFF (tex_wrap_old.txt: everything REPEAT, the old behaviour)" : "ON");
    }
#ifdef BREAK_15   /* host negative control: the wrap is left out of the cache key and never reaches the backend */
    return 0x11;
#endif
    return gWrapOld ? 0x11 : t->wrap;   /* 0x11 = REPEAT/REPEAT: the old key meaning and the backend's creation state */
}
static void texWrapApply(RhiTexture* tex, u8 key) {
    if (tex && !gWrapOld && rhi_vita_texSetWrap)   /* also for REPEAT/REPEAT: the backend counts every texture by mode and makes no GL call when nothing changes */ rhi_vita_texSetWrap(tex, key & 15, key >> 4);
}
// TEXTURE FILTER (run 255). Every ordinary texture was GL_LINEAR whatever GXInitTexObjLOD asked (checked against Dolphin, whose
// sampler takes the texture's own min/mag filter: Rasterizer.cpp / TextureSampler.cpp). A map the game samples GX_NEAR -- an
// indirect or noise map above all -- was therefore smoothed. Applied at BIND, because GXInitTexObjLOD runs after GXInitTexObj and the
// cached texture is shared: the backend keeps the mode it last set and makes a GL call only on a change. No mip levels are
// uploaded, so the three *_MIP_* modes fall to their base filter (NEAR_MIP_* -> nearest, LIN_MIP_* -> linear).
// ux0:data/stairfax/tex_filter_old.txt starting with 1 = the old behaviour (everything linear, no backend call).
// MIPMAPS (run 258). Run 257's screenshot with indirect on: the fur/grass shells are a dense dot stipple and the Arwing is
// speckled. Both sample a small detail/noise map tiled many times (the fur noise 37.5x); the game asks for a mipmapped filter on
// those and the port uploaded level 0 only, so every minified sample aliased. The backend now builds the chain (once per
// texture, on the first bind that asks) and uses the game's min filter as given. ux0:data/stairfax/tex_mips_off.txt 1 = off.
extern void rhi_vita_texSetFilter(RhiTexture*, int minGX, int magNearest, int wantMips) __attribute__((weak));   /* rhi_vita.c */
static int gMipsOff = -1;
unsigned gx_tex_mip_binds;
static int gFilterOld = -1;
unsigned gx_tex_filter_nearest_binds;
extern void rhi_vita_texSetLodBias(RhiTexture*, int bias8) __attribute__((weak));   /* rhi_vita.c */
static int gLodBiasOff = -1;
unsigned gx_tex_lodbias_binds;
static void texFilterApply(RhiTexture* tex, const PortTexObj* t) {
    if (gFilterOld < 0) {
        FILE* f = fopen("ux0:data/stairfax/tex_filter_old.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gFilterOld = c == 49; printf("[filter] texture min/mag filter from the game %s\n", gFilterOld ? "OFF (tex_filter_old.txt: everything linear, the old behaviour)" : "ON");
    }
    if (!tex || gFilterOld || !rhi_vita_texSetFilter) return;
    if (gMipsOff < 0) {
        FILE* f = fopen("ux0:data/stairfax/tex_mips_off.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gMipsOff = c == 49; printf("[filter] mipmaps for textures the game filters with them %s\n", gMipsOff ? "OFF (tex_mips_off.txt)" : "ON (built by the backend on first use)");
    }
    int mn = texObjMinFilt(t), mg = texObjMagFilt(t);
    int mnNear = mn == 0 || mn == 2 || mn == 4, mgNear = mg == 0;
    /* 2026-10-14 (Aurora gfx/texture.cpp:306-340; the hardware's LOD clamp): a chain only for a texture the GAME made mipmapped.
     * texture.c:1014 hands a texture WITHOUT mips its own (mip) min filter with min_lod = max_lod = 0: the GameCube never leaves
     * level 0. A generated box-filtered chain averaged cut-out leaves into their alpha-0 texels (dark fringes) and blurred far
     * textures retail draws sharp. BREAK_26: the old rule (any mip min filter). */
#ifdef BREAK_26
    int wantMips = !gMipsOff && mn >= 2 && mn <= 5;
#else
    int wantMips = !gMipsOff && mn >= 2 && mn <= 5 && (t->mipmap & 1);
#endif
    /* 2026-10-14 (run 273; Aurora gfx/texture.cpp, the hardware's LOD clamp): a mipmapped texture whose max_lod is 0 (no note in the
     * LOD table) also stays on level 0. And the game's LOD BIAS reaches the sampler (it was dropped: retail picks finer levels than
     * we did -- the fur/grass noise maps are biased -2.0). ux0:data/stairfax/tex_lodbias_off.txt 1 = no bias (A/B).
     * BREAK_27: max_lod ignored. BREAK_28: the bias is never handed over. */
    int bias8 = 0;
    if (wantMips) {
        int levels = texLodLevels(t->image, &bias8);
#if !defined(BREAK_27) && !defined(BREAK_26)   /* BREAK_26's old rule had no clamp either */
        if (levels < 2) wantMips = 0;
#endif
    }
    if (mnNear || mgNear) ++gx_tex_filter_nearest_binds;
    if (wantMips) ++gx_tex_mip_binds;
    rhi_vita_texSetFilter(tex, wantMips ? mn : (mnNear ? 0 : 1), mgNear, wantMips);
    if (gLodBiasOff < 0) {
        FILE* f = fopen("ux0:data/stairfax/tex_lodbias_off.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
        gLodBiasOff = c == 49; printf("[filter] the game's texture LOD bias %s\n", gLodBiasOff ? "OFF (tex_lodbias_off.txt)" : "ON");
    }
#ifdef BREAK_28
    bias8 = 0;
#endif
    if (!wantMips || gLodBiasOff) bias8 = 0;
    if (bias8) ++gx_tex_lodbias_binds;
    if (rhi_vita_texSetLodBias) rhi_vita_texSetLodBias(tex, bias8);   /* the backend makes a GL call only on a change */
}
static int tcFind(const void* image, u16 w, u16 h, u8 fmt, u8 wrap) {
    u32 p = ((uintptr_t)image >> 16) & (TC_PAGES - 1);
    for (int e = gTcPage[p]; e; ) {
        TexEnt* x = &gTc[e];
        if (x->image == image && x->w == w && x->h == h && x->fmt == fmt && x->wrap == wrap) return e;
        int k = 0; while (k < 4 && x->pages[k] != p) k++;
        e = k < 4 ? x->next[k] : 0;
    }
    return 0;
}
// The table never reused a retired entry (image == NULL after gx_tex_release): the title movie retires and adds one every
// decoded frame (2026-10-09 play session: 4,281 entries after 4,200 frames, of TC_CAP 8,192), after which every new texture took
// the uncached per-object path. Compact when full: keep the live entries, rebuild the page chains. No index is held outside.
unsigned gx_tex_compactions;
static void tcCompact(void) {
    int n = 0;
    for (int e = 1; e <= gTcN; ++e) if (gTc[e].image) { ++n; if (n != e) gTc[n] = gTc[e]; }
    gTcN = n;
    memset(gTcPage, 0, sizeof gTcPage);
    for (int e = 1; e <= gTcN; ++e) {
        TexEnt* x = &gTc[e];
        for (int k = 0; k < 4; ++k) if (x->pages[k] != 0xFFFFFFFFu) { x->next[k] = gTcPage[x->pages[k]]; gTcPage[x->pages[k]] = e; }
    }
    ++gx_tex_compactions;
}
static int tcAdd(const void* image, u16 w, u16 h, u8 fmt, u8 wrap) {
    if (gTcN >= TC_CAP) tcCompact();   /* retired entries (gx_tex_release) are dropped first: the movie retires one a frame */
    if (gTcN >= TC_CAP) { ++gx_tex_cache_full; return 0; }
    int e = ++gTcN;
    TexEnt* x = &gTc[e];
    memset(x, 0, sizeof(*x));
    x->image = (const u8*)image; x->w = w; x->h = h; x->fmt = fmt; x->wrap = wrap; x->dirty = 1;
    x->size = GXGetTexBufferSize(w, h, fmt, 0, 0);
    u32 first = (uintptr_t)image >> 16, last = ((uintptr_t)image + (x->size ? x->size - 1 : 0)) >> 16;
    for (int k = 0; k < 4; k++) x->pages[k] = 0xFFFFFFFFu;
    for (u32 pg = first, k = 0; pg <= last && k < 4; pg++, k++) {   // textures up to 256 KB span <= 4 pages
        u32 b = pg & (TC_PAGES - 1);
        x->pages[k] = b; x->next[k] = gTcPage[b]; gTcPage[b] = e;
    }
    ++gx_tex_cache_entries;
    return e;
}
// The CPU rewrote [addr, addr+n): every cached texture overlapping it is re-decoded on next load.
void gx_tex_invalidate(const void* addr, u32 n) {
    uintptr_t a = (uintptr_t)addr, b = a + (n ? n : 1);
    if (!gTcN) return;
    for (uintptr_t pg = a >> 16; pg <= (b - 1) >> 16; pg++) {
        u32 p = pg & (TC_PAGES - 1);
        for (int e = gTcPage[p]; e; ) {
            TexEnt* x = &gTc[e];
            if ((uintptr_t)x->image < b && (uintptr_t)x->image + x->size > a) x->dirty = 1;
            int k = 0; while (k < 4 && x->pages[k] != p) k++;
            e = k < 4 ? x->next[k] : 0;
        }
    }
}

// The memory [addr, addr+n) was FREED (mm_shim.c mm_free): every cached texture inside it is
// destroyed and its entry retired (it can never match again; a new allocation at the same address
// gets a fresh entry). Run 73 leaked every movie cycle's planes this way until the movie's own
// allocation failed ("malloc for movie failed", 11.8 s frames). Bound slots are cleared too.
unsigned gx_tex_released;
void gx_tex_release(const void* addr, u32 n) {
    uintptr_t a = (uintptr_t)addr, b = a + (n ? n : 1);
    copyDstRelease(addr, n ? n : 1);
    if (!gTcN) return;
    for (uintptr_t pg = a >> 16; pg <= (b - 1) >> 16; pg++) {
        u32 p = pg & (TC_PAGES - 1);
        for (int e = gTcPage[p]; e; ) {
            TexEnt* x = &gTc[e];
            if (x->image && (uintptr_t)x->image >= a && (uintptr_t)x->image < b) {
                if (x->rhi) {
                    for (int m = 0; m < 8; m++) if (gTexMap[m] == x->rhi) gTexMap[m] = NULL;
                    if (gTex == x->rhi) gTex = NULL;
                    if (gRhi) rhi_destroyTexture(gRhi, x->rhi);
                    x->rhi = NULL;
                    ++gx_tex_released;
                }
                x->image = NULL;     // retired: tcFind never matches a NULL image
            }
            int k = 0; while (k < 4 && x->pages[k] != p) k++;
            e = k < 4 ? x->next[k] : 0;
        }
    }
}

// THP movie planes (2026-09-28): thp_decode.c registers each decoded frame's Y/U/V I8 planes. The
// movie's four TEV stages (YUV -> RGB) are not emulated, so the Y plane -- the one this path
// samples, TEXMAP0 -- is uploaded already converted (tex_decode.c gxTexDecodeYUV). Vita run 79 drew
// the attract laptop's screen flat green. The THP player rotates a few texture sets: 4 slots.
typedef struct { const void *y, *u, *v; int w, h; uint8_t* ex[3]; } YuvSet;   /* ex: the planes already expanded to RGBA by the decode worker (thp_decode.c), owned here until a load takes them */
static YuvSet gYuv[4]; static int gYuvNext;
unsigned gx_tex_yuv_frames;
// THE MOVIE DID NOT MOVE (2026-10-09, docs/47 "the frozen laptop"). Six consecutive title captures on the paced clock are
// byte-identical while `thp video decodes` advances, and `redecodes` is 0 in every run: the planes' cache entries are never
// re-decoded. Whatever the reason the address-range invalidation misses them, a decoded frame is a FACT the decoder reports
// here: each registration bumps the set's generation, and a cached texture whose image is one of the set's three planes is
// re-decoded when its generation is behind (gxLoadTexObjImpl). The first loads and decodes are logged ([mov]).
static unsigned gYuvGen[4]; unsigned gx_movie_regs, gx_movie_gen_redecodes;
static void yuvFreeEx(YuvSet* s) { for (int k = 0; k < 3; ++k) { free(s->ex[k]); s->ex[k] = NULL; } }
static void yuvRegister(const void* y, const void* u, const void* v, int w, int h, uint8_t* ey, uint8_t* eu, uint8_t* ev) {
    if (++gx_movie_regs <= 6) printf("[mov] decoded frame %u: planes y %p u %p v %p, %dx%d%s\n", gx_movie_regs, y, u, v, w, h, ey ? " (expanded by the worker)" : "");
    for (int i = 0; i < 4; ++i) if (gYuv[i].y == y) {
        yuvFreeEx(&gYuv[i]);   /* a frame decoded over one nobody drew: its expand is stale */
        gYuv[i].u = u; gYuv[i].v = v; gYuv[i].w = w; gYuv[i].h = h; gYuv[i].ex[0] = ey; gYuv[i].ex[1] = eu; gYuv[i].ex[2] = ev; ++gYuvGen[i]; return;
    }
    yuvFreeEx(&gYuv[gYuvNext]);
    gYuv[gYuvNext] = (YuvSet){ y, u, v, w, h, { ey, eu, ev } }; gYuvGen[gYuvNext] = gx_movie_regs; gYuvNext = (gYuvNext + 1) & 3;
}
void gx_tex_yuv_register(const void* y, const void* u, const void* v, int w, int h) { yuvRegister(y, u, v, w, h, NULL, NULL, NULL); }
void gx_tex_yuv_register_x(const void* y, const void* u, const void* v, int w, int h, uint8_t* ey, uint8_t* eu, uint8_t* ev) { yuvRegister(y, u, v, w, h, ey, eu, ev); }
/* The worker's expanded copy of plane `image` (w x h texels), taken: NULL if there is none, or it is another size. */
static uint8_t* yuvTakeEx(const void* image, int w, int h) {
    for (int i = 0; i < 4; ++i) {
        YuvSet* s = &gYuv[i];
        int k = s->y == image ? 0 : s->u == image ? 1 : s->v == image ? 2 : -1;
        if (k < 0 || !s->ex[k]) continue;
        if (w != (k ? s->w / 2 : s->w) || h != (k ? s->h / 2 : s->h)) return NULL;
        uint8_t* r = s->ex[k]; s->ex[k] = NULL; return r;
    }
    return NULL;
}
/* [movie-up] (2026-10-11, docs/47 "movie upload"): where the MAIN thread's time goes uploading the movie's planes.
   Per 5 s: planes uploaded, how many arrived pre-expanded, ms (sum per decoded frame = 3 planes) in the expand done
   here, in rhi_createTexture (split: the alpha-0 scan vs glTexImage2D, from rhi_vita.c), in rhi_destroyTexture. */
extern unsigned long long rhi_vita_tex_a0_us __attribute__((weak)), rhi_vita_tex_gl_us __attribute__((weak)), rhi_vita_tex_gen_us __attribute__((weak));   /* rhi_vita.c; absent on the host */
#define RV(x) (&x ? x : 0ull)
static struct { unsigned n, pre; unsigned long long exp, create, destroy, t0, a0, gl, gen; } gMU;
static void movieUpNote(int pre, unsigned long long exp, unsigned long long create, unsigned long long destroy) {
    unsigned long long now = GXT_NOW();
    if (!gMU.t0) gMU.t0 = now;
    ++gMU.n; gMU.pre += pre; gMU.exp += exp; gMU.create += create; gMU.destroy += destroy;
    if (now - gMU.t0 >= 5000000ull && gMU.n) {
        double fr = gMU.n / 3.0;   /* a decoded frame is three planes */
        printf("[movie-up] last %.1f s: %u plane uploads (%.1f frames, %u of them pre-expanded by the worker) | per frame on the MAIN thread: expand %.2f ms, createTexture %.2f ms"
               " (alpha scan %.2f, glTexImage2D %.2f, glGenTextures+params %.2f), destroyTexture %.2f ms\n",
               (now - gMU.t0) / 1e6, gMU.n, fr, gMU.pre, gMU.exp / 1000.0 / fr, gMU.create / 1000.0 / fr,
               (RV(rhi_vita_tex_a0_us) - gMU.a0) / 1000.0 / fr, (RV(rhi_vita_tex_gl_us) - gMU.gl) / 1000.0 / fr, (RV(rhi_vita_tex_gen_us) - gMU.gen) / 1000.0 / fr, gMU.destroy / 1000.0 / fr);
        memset(&gMU, 0, sizeof gMU); gMU.t0 = now; gMU.a0 = RV(rhi_vita_tex_a0_us); gMU.gl = RV(rhi_vita_tex_gl_us); gMU.gen = RV(rhi_vita_tex_gen_us);
    }
}
static int yuvPlaneGen(const void* image, unsigned* gen) {   /* 1 = image is a registered movie plane (Y, U or V) */
    for (int i = 0; i < 4; ++i) if (gYuv[i].y && (gYuv[i].y == image || gYuv[i].u == image || gYuv[i].v == image)) { *gen = gYuvGen[i] + 1u; return 1; }
    return 0;
}
static int isPlaneNow(const void* image) { unsigned g; return yuvPlaneGen(image, &g); }
static int yuvIsMovieY(const PortTexObj* t) {
    for (int i = 0; i < 4; ++i) if (gYuv[i].y == t->image && gYuv[i].w == t->width && gYuv[i].h == t->height) return 1;
    return 0;
}
// Which GX_TEXMAPn currently hold a movie frame (bit n). The attract LAPTOP appends the movie's
// stages after its own material (shader_dolphin.c addYUVVideoTevStages: Y at gRcpNextTexMap >= 1),
// so TEXMAP0 is the laptop's screen material; a draw whose stages read a movie map samples THAT.
// (gMovieMaps is declared with the other forward declarations, above the draw path)
static uint8_t* yuvDecodeIfMovie(const PortTexObj* t) {
    for (int i = 0; i < 4; ++i)
        if (gYuv[i].y == t->image && gYuv[i].w == t->width && gYuv[i].h == t->height) {
            ++gx_tex_yuv_frames;
            return gxTexDecodeYUV((const uint8_t*)gYuv[i].y, (const uint8_t*)gYuv[i].u, (const uint8_t*)gYuv[i].v, gYuv[i].w, gYuv[i].h);
        }
    return NULL;
}

// Decode (tex_decode.c) + upload through the cache, then bind. TEXMAP0 is what the
// port's single-texture draw path samples; the other maps are recorded only (the
// TEV multi-texture combine is not wired).
static void gxLoadTexObjImpl(void* obj, int id);
extern int rhi_vita_texSetMips(RhiTexture* t, int nlev, unsigned char* const* rgba, int maxLevel) __attribute__((weak));   /* rhi_vita.c */
unsigned gx_tex_authored_chains, gx_tex_authored_levels;
static void texUploadAuthoredMips(RhiTexture* tex, const PortTexObj* t) {
    static int sOff = -1;
    if (sOff < 0) { FILE* f = fopen("ux0:data/stairfax/tex_mips_authored_off.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
                    sOff = c == 49; printf("[filter] the game's own mip levels %s\n", sOff ? "OFF (tex_mips_authored_off.txt: generated chains, run 258)" : "ON (run 264)"); }
    if (sOff || !tex || !rhi_vita_texSetMips || !(t->mipmap & 1)) return;
    int levels = texLodLevels(t->image, NULL);
    if (levels < 2) return;
    unsigned char* lv[11] = { 0 };
    int n = 0, w = t->width, h = t->height;
    for (int L = 1; L < levels && n < 10; ++L) {
        int lw = w >> L, lh = h >> L;
        if (lw < 4 || lh < 4) break;   /* below 4 texels the backend's chain continues by downscale */
        u32 off = GXGetTexBufferSize((u16)w, (u16)h, (u32)t->fmt, 1, (u8)L);
        lv[n] = gxTexDecode(t->fmt, lw, lh, (const uint8_t*)t->image + off);
        if (!lv[n]) break;
        ++n;
    }
    /* n may be 0 (a small texture): the backend still ends the chain at max_lod (levels - 1) instead of 1x1 */
    if (rhi_vita_texSetMips(tex, n, lv, levels - 1)) { ++gx_tex_authored_chains; gx_tex_authored_levels += (unsigned)n; }
    for (int i = 0; i < n; ++i) free(lv[i]);
    if ((gx_tex_authored_chains & 255) == 1) printf("[filter] authored mip chains %u (%u levels)\n", gx_tex_authored_chains, gx_tex_authored_levels);
}
// FIX B (2026-10-12): a GPU EFB copy is sampled with the min/mag filter and wrap of the GXTexObj that binds it (it was GL_LINEAR/CLAMP
// whatever the game asked). The backend owns the on/off switch (ux0:data/stairfax/copyfilter_off.txt, one [copyfilter] boot line).
extern void rhi_vita_copyTexSetSampler(RhiTexture*, int gxMin, int gxMag, int gxWrapS, int gxWrapT) __attribute__((weak));
unsigned gx_copy_samplers;
static void copySamplerApply(RhiTexture* copyTex, const PortTexObj* t) {
    if (!rhi_vita_copyTexSetSampler) return;
    ++gx_copy_samplers;
    rhi_vita_copyTexSetSampler(copyTex, texObjMinFilt(t), texObjMagFilt(t), t->wrap & 15, t->wrap >> 4);
}
extern void os_ext_poll(void) __attribute__((weak));   /* os_threads.c: a finished off-baton movie decode wakes its thread HERE, not at the frame's end (movie_wake_off.txt 1: off) */
void GXLoadTexObj(void* obj, int id) { RT_SUBS(RT_S_ENTRY_TEX); if (os_ext_poll) os_ext_poll(); unsigned long long t = GXT_NOW(); RT_ENTER(RT_SHIM); gxLoadTexObjImpl(obj, id); RT_LEAVE(); gx_w_tex_us += GXT_NOW() - t; }
static void gxLoadTexObjImpl(void* obj, int id) {
    PortTexObj* t = (PortTexObj*)obj;
    ++gx_tex_loads;
    if (STFX_ARM5()) return;   /* arm 5 (see gStfxPerfArm): no cache lookup / decode / upload / bind (os_ext_poll already ran in GXLoadTexObj) */
    if (!t || t->tag != PORT_TEXOBJ_TAG || !t->image) { ++gx_tex_load_uninit; return; }
    // A GPU EFB copy (GXCopyTex, mode 1) lives in the backend, not in memory: bind THAT texture, never
    // the decode of whatever the destination's memory holds, and keep it out of the decode cache.
    RhiTexture* copyTex = gCopyDstN ? copyDstTex(t->image) : NULL;
    if (gCopyDstN) copyDstWanted(t->image);
    if (gRhi && !copyTex) {
        u8 wk = texWrapKey(t);
        int e = tcFind(t->image, t->width, t->height, t->fmt, wk);   // (no slot in the object: it must fit 32 bytes on a 64-bit host)
        if (!e) e = tcAdd(t->image, t->width, t->height, t->fmt, wk);
        TexEnt* x = e ? &gTc[e] : NULL;
        {   unsigned mg = 0; int isPlane = yuvPlaneGen(t->image, &mg);
            if (x && isPlane && x->gen != mg) { if (x->rhi && !x->dirty) ++gx_movie_gen_redecodes; x->dirty = 1; x->gen = mg; }
            static unsigned sMovLog;
            if (t->fmt == GX_TF_I8 && t->width >= 200 && sMovLog < 36) {
                ++sMovLog;
                printf("[mov] load map %d: image %p %ux%u | movie plane %d gen %u | cache entry %d dirty %d has texture %d | first bytes %02x %02x %02x %02x\n", id, t->image,
                       (unsigned)t->width, (unsigned)t->height, isPlane, mg, e, x ? x->dirty : -1, x ? x->rhi != NULL : -1,
                       ((const u8*)t->image)[0], ((const u8*)t->image)[1], ((const u8*)t->image)[2], ((const u8*)t->image)[3]);
            }
        }
        if (x && (x->dirty || !x->rhi)) {
            int mplane = t->fmt == GX_TF_I8 && isPlaneNow(t->image);
            unsigned long long m0 = mplane ? GXT_NOW() : 0, m1 = 0, m2 = 0, m3 = 0;
            uint8_t* rgba = (t->fmt == GX_TF_I8 && !(gStfxTevArm && rhi_vita_drawTev)) ? yuvDecodeIfMovie(t) : NULL;
            int pre = 0;
            if (!rgba && mplane && (rgba = yuvTakeEx(t->image, t->width, t->height)) != NULL) pre = 1;
            if (!rgba) rgba = gxTexDecode(t->fmt, t->width, t->height, (const uint8_t*)t->image);
            if (mplane) m1 = GXT_NOW();
            if (rgba) {
                if (x->rhi) { rhi_destroyTexture(gRhi, x->rhi); ++gx_tex_redecodes; }
                if (mplane) m2 = GXT_NOW();
                x->rhi = rhi_createTexture(gRhi, t->width, t->height, 1, (uint32_t)t->fmt, rgba); texWrapApply(x->rhi, wk);
                if (!mplane && !pre) texUploadAuthoredMips(x->rhi, t);
                if (mplane) m3 = GXT_NOW();
                free(rgba); ++gx_tex_uploads;
            }
            if (mplane) movieUpNote(pre, m1 - m0, m3 - m2, m2 - m1);
            x->dirty = 0;
        } else if (!x && !t->rhi) {   // cache full: the old per-object upload (counted)
            uint8_t* rgba = gxTexDecode(t->fmt, t->width, t->height, (const uint8_t*)t->image);
            if (rgba) { t->rhi = rhi_createTexture(gRhi, t->width, t->height, 1, (uint32_t)t->fmt, rgba); texWrapApply(t->rhi, wk); texUploadAuthoredMips(t->rhi, t); free(rgba); ++gx_tex_uploads; }
        }
        if (x) t->rhi = x->rhi;
    }
    if ((unsigned)id < 8) {
        if (!copyTex) texFilterApply(t->rhi, t);
        gTexMap[id] = t->rhi;
        if (t->fmt == GX_TF_I8 && yuvIsMovieY(t)) { gMovieMaps |= 1u << id; ++gx_movie_loads; gx_movie_last_map = id; }
        else gMovieMaps &= ~(1u << id);
        if (copyTex) { copySamplerApply(copyTex, t); gTexMap[id] = copyTex; gCopyMaps &= ~(1u << id); gFilledCopyMaps |= 1u << id; }   // drawn like any texture, TEV path included
        else { gFilledCopyMaps &= ~(1u << id); if (gCopyDstN && inCopyDst(t->image)) gCopyMaps |= 1u << id; else gCopyMaps &= ~(1u << id); }
        gMapImg[id] = t->image; gMapW[id] = t->width; gMapH[id] = t->height; gMapFmt[id] = t->fmt;
    }
    if (id == 0) gTex = copyTex ? copyTex : t->rhi;
}
// TMEM preloading is a GameCube cache-management scheme; the texels are the same,
// so a preloaded load is an ordinary load and a preload is nothing.
void GXLoadTexObjPreLoaded(void* obj, void* region, int id) { (void)region; GXLoadTexObj(obj, id); }
void GXPreLoadEntireTexture(void* obj, void* region) { (void)obj; (void)region; }

// Verbatim from the SDK (src/dolphin/gx/GXTexture.c), asserts dropped: pure size maths
// the game uses to allocate texture buffers, so it must match the console exactly.
static void portTexTileShift(u32 fmt, u32* rowTileS, u32* colTileS) {
    switch (fmt) {
    case GX_TF_I4: case 0x8: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
        *rowTileS = 3; *colTileS = 3; break;
    case GX_TF_I8: case GX_TF_IA4: case 0x9: case GX_TF_Z8: case GX_CTF_RA4: case GX_TF_A8:
    case GX_CTF_R8: case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        *rowTileS = 3; *colTileS = 2; break;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case 0xA:
    case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z16L:
        *rowTileS = 2; *colTileS = 2; break;
    default:
        *rowTileS = *colTileS = 0; break;
    }
}
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod) {
    u32 tileShiftX, tileShiftY, tileBytes, bufferSize, nx, ny, level;
    portTexTileShift(format, &tileShiftX, &tileShiftY);
    tileBytes = (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) ? 64 : 32;
    if (mipmap == 1) {
        bufferSize = 0;
        for (level = 0; level < max_lod; level++) {
            nx = (width + (1 << tileShiftX) - 1) >> tileShiftX;
            ny = (height + (1 << tileShiftY) - 1) >> tileShiftY;
            bufferSize += tileBytes * (nx * ny);
            if (width == 1 && height == 1) break;
            width = (width > 1) ? width >> 1 : 1;
            height = (height > 1) ? height >> 1 : 1;
        }
    } else {
        nx = (width + (1 << tileShiftX) - 1) >> tileShiftX;
        ny = (height + (1 << tileShiftY) - 1) >> tileShiftY;
        bufferSize = nx * ny * tileBytes;
    }
    return bufferSize;
}

void GXBeginDisplayList(void* list, u32 size) {
    gCollecting = 0; fifoReset();          // a primitive in flight is abandoned, as on hardware
    gList = (unsigned char*)list; gListCap = size; gListLen = 0; gListOverflow = 0;
}
// SDK contract: the size rounded up to 32 bytes with NOP padding, or 0 on overflow.
u32 GXEndDisplayList(void) {
    u32 n;
    if (!gList) return 0;
    while ((gListLen & 31) && gListLen < gListCap) gList[gListLen++] = 0;
    n = (gListOverflow || (gListLen & 31)) ? 0 : gListLen;
    gx_list_overflows += (n == 0 && gListLen); ++gx_list_records;
    gList = NULL;
    return n;
}
void GXResetWriteGatherPipe(void) { }       // the port's pipe has no gather buffer to drain

// --- draw sync (vita fork) -----------------------------------------------------
// The port draws SYNCHRONOUSLY: by the time GXSetDrawSync's token would reach the GP,
// everything before it has been drawn. So the token is readable back immediately.
// This is load-bearing: videoSwapFrameBuffers flips only when GXReadDrawSync returns
// the NEXT token, so a no-op GXSetDrawSync (the scaffold's) means no frame ever flips.
static u16 gDrawSyncToken;
void GXSetDrawSync(u16 token) { gDrawSyncToken = token; }
u16  GXReadDrawSync(void)     { return gDrawSyncToken; }
void GXPixModeSync(void) { }
void GXTexModeSync(void) { }
void __GXAbortWaitPECopyDone(void) { }     // GPU-hang recovery: the port's GP never hangs

// --- EFB readback / copies: NOT IMPLEMENTED, counted --------------------------------
// GXPeekZ reports the FAR plane (0xFFFFFF, 24-bit Z): nothing occludes the probe. The
// game's depth probes (pi_dolphin gDepthReadResults) will read every point as visible.
// EFB -> TEXTURE COPIES are not reproduced (2026-09-28, Vita run 82): a copy destination keeps
// whatever its memory held -- for a fresh mmAlloc, GARBAGE, which the prologue drew as rainbow
// streaks over the sky (distortion slots, rcp_dolphin.c) and speckle over the CloudRunner (shadow
// textures, newshadows.c). Until a real render-to-texture copy exists, the effect is made ABSENT:
// every copy destination is recorded, a draw whose SAMPLED texture lies in one is skipped
// (counted), and the copy's clear of its source rectangle is honoured (the shadow casters drawn
// into the EFB for the copy must not stay on screen).
unsigned gx_peekz_calls, gx_copytex_calls, gx_copy_draws_skipped, gx_copy_clears;
// GXPeekZ, REAL (vita fork, 2026-10-09, docs/47 "Run 157"). Until run 157 nothing ever CALLED it: the game peeks
// only inside videoBreakPointCallback, which the port never raised, so depthReadRequestPoll returned 0 for every
// probe -- every point OCCLUDED, not "visible" as the comment above claimed (sun flare, glow fades, particles).
// ux0:data/stairfax/peekz.txt: 0 or absent = the far plane (every probe visible); 1 = M's depth memory as it
// stands (may be a scene behind); 2 = flush + glFinish once per delivery, then the memory (exact, costs the wait).
extern int  rhi_vita_peekZ(int x, int y, unsigned* z24, unsigned* raw) __attribute__((weak));
extern void rhi_vita_peekZ_sync(void) __attribute__((weak));
static int gPeekZMode = -1;
static unsigned gBpSerial, gPeekZSynced;
unsigned gx_peekz_real, gx_peekz_near, gx_bp_delivered;
void GXPeekZ(u16 x, u16 y, u32* z) {
    unsigned v = 0xFFFFFFu, raw = 0;
    ++gx_peekz_calls;
    if (gPeekZMode < 0) { FILE* f = fopen("ux0:data/stairfax/peekz.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
                          gPeekZMode = c == 49 ? 1 : c == 50 ? 2 : 0;
                          printf("[peekz] mode %d (%s)\n", gPeekZMode, gPeekZMode == 2 ? "depth memory after a glFinish" : gPeekZMode == 1 ? "depth memory as it stands" : "far plane: every probe visible"); }
    if (gPeekZMode && rhi_vita_peekZ) {
        if (gPeekZMode == 2 && gPeekZSynced != gBpSerial && rhi_vita_peekZ_sync) { rhi_vita_peekZ_sync(); gPeekZSynced = gBpSerial; }
        if (rhi_vita_peekZ(x, y, &v, &raw)) {
            ++gx_peekz_real; gx_peekz_near += v < 0xFFFF00u;
            if (gx_peekz_real <= 40 || !(gx_peekz_real % 2000)) printf("[peekz] #%u (%u,%u) raw %08x z24 %06x\n", gx_peekz_real, x, y, raw, v);
        }
    }
    if (z) *z = v;
}

// --- the FIFO BREAKPOINT (vita fork, 2026-10-09, docs/47 "Run 157") -----------------------------------------------
// The game arms a breakpoint at the end of each frame's commands (GXFlush_, pi_videoinit.c:281) and does three things
// in its handler (videoBreakPointCallback, pi_dolphin.c:4390): returns the movie's displayed texture set, delivers the
// pending depth probes, and pops the flip queue (waking waitNextFrame). The port draws synchronously, so the GP has
// "reached" an armed breakpoint by the next retrace: vi_shim delivers it there, before the pre-retrace callback (the
// order the hardware gives: the breakpoint precedes the frame's draw sync and copy). One delivery per retrace.
// ux0:data/stairfax/gx_bp.txt starting with 0 = the run-156 behaviour (the movie duty alone, from vi_shim).
typedef void (*StfxBpCb)(void);
static StfxBpCb gBpCb;
static int gBpArmed, gBpMode = -1;
StfxBpCb GXSetBreakPtCallback(StfxBpCb cb) { StfxBpCb o = gBpCb; gBpCb = cb; return o; }
void GXEnableBreakPt(void* breakPt) { (void)breakPt; gBpArmed = 1; }
void GXDisableBreakPt(void) { gBpArmed = 0; }
int gx_breakpoint_deliver(void) {
    if (gBpMode < 0) { FILE* f = fopen("ux0:data/stairfax/gx_bp.txt", "r"); int c = f ? fgetc(f) : 0; if (f) fclose(f);
                       gBpMode = c == 48 ? 0 : 1;
                       printf("[bp] GX FIFO breakpoint: %s\n", gBpMode ? "DELIVERED at the retrace" : "not delivered (gx_bp.txt 0): the movie duty alone"); }
    if (!gBpMode) return 0;
    if (gBpArmed && gBpCb) { ++gBpSerial; ++gx_bp_delivered; gBpCb(); }
    return 1;
}
static u16 gCopyDstW, gCopyDstH; static int gCopyDstFmt;   // (gCopySrc: gx_hal.copySrc)
static GXColor gCopyClr; static u32 gCopyClrZ = 0xFFFFFF;
#define GX_COPY_DSTS 64
static struct { const unsigned char *lo, *hi; u16 w, h; int fmt; RhiTexture* tex; int want; } gCopyDst[GX_COPY_DSTS];   // tex: the GPU copy (mode 1), else NULL; want: the present it was last bound in   // (gCopyDstN: declared above the draw path)
extern void rhi_vita_clearRect(int x, int y, int w, int h, u32 rgba, float z) __attribute__((weak));
// The frame's background IS this colour: GXCopyDisp(clear) leaves the EFB holding it, and the game
// sets it once (black, pi_videoinit.c). The port's frame clear used vi_shim's placeholder green, so
// the rectangle that newshadows_drawReflectionTexture copies and clears at the START of every frame
// showed as a black box wherever the scene does not cover it (docs/47, 2026-10-07: the "post-frame
// copy" diagnosis is retracted).
extern void vi_set_clear_color(float r, float g, float b) __attribute__((weak));   // (weak: the host tests link no VI)
void GXSetCopyClear(GXColor clr, u32 z) { gCopyClr = clr; gCopyClrZ = z; if (vi_set_clear_color) vi_set_clear_color(clr.r / 255.0f, clr.g / 255.0f, clr.b / 255.0f); }
// THE VIEWPORT (2026-09-28, Vita run 92): GXSetViewport was a do-nothing scaffold stub, so passes
// the game confines to a small EFB rectangle -- the shadow casters (newshadows.c: a screenW square,
// 320x240 quadrants), the distortion slots (rcp_dolphin.c: 32x32) -- were drawn over the WHOLE
// screen, and the copy's clear then erased only the small rectangle: run 92's journal found 982
// untextured white caster draws per frame left on screen as blocky shapes. The rectangle is EFB
// pixels (640x480, top-left origin); the backend maps it. The depth range is folded into the projection's z row (gxClipZ, top of
// this file; GX allows a negative near, e.g. -0.075, that glDepthRange would clamp): counted when it differs from [0,1].
extern void rhi_vita_setViewport(float x, float y, float w, float h) __attribute__((weak));
unsigned gx_viewport_sets, gx_viewport_depth_ignored;
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    ++gx_viewport_sets;
    if (nearz != 0.0f || farz != 1.0f) ++gx_viewport_depth_ignored;   /* (name kept for the status log: now = non-default ranges seen) */
    gVpZn = nearz; gVpZf = farz;
    if (rhi_vita_setViewport) rhi_vita_setViewport(left, top, wd, ht);
}
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    (void)field;   // the half-line jitter of interlaced fields: nothing to do on a progressive screen
    GXSetViewport(left, top, wd, ht, nearz, farz);
}
GXH_INLINE void gxh_b_GXSetTexCopySrc(GxHal* h, const int mark, u16 left, u16 top, u16 wd, u16 ht) { GXH_MARK(h, mark, GXH_G_COPY); h->copySrc[0] = left; h->copySrc[1] = top; h->copySrc[2] = wd; h->copySrc[3] = ht; }
GXH_PAIR(GXSetTexCopySrc, (u16 left, u16 top, u16 wd, u16 ht), left, top, wd, ht)
void GXSetTexCopyDst(u16 wd, u16 ht, int fmt, u8 mipmap) { gCopyDstW = wd; gCopyDstH = ht; gCopyDstFmt = fmt; (void)mipmap; }
static int inCopyDst(const void* p) {
    for (int i = 0; i < gCopyDstN; ++i)
        if ((const unsigned char*)p >= gCopyDst[i].lo && (const unsigned char*)p < gCopyDst[i].hi) return 1;
    return 0;
}
static RhiTexture* copyDstTex(const void* p) {
    for (int i = 0; i < gCopyDstN; ++i)
        if ((const unsigned char*)p >= gCopyDst[i].lo && (const unsigned char*)p < gCopyDst[i].hi) return gCopyDst[i].tex;
    return NULL;
}
// DEMAND (2026-10-07): the game takes its whole-screen Z8 copy every frame (newshadows_capture
// ReflectionTextures) and samples it only while a blur / heat / distortion / snow-flash filter runs.
// A Z copy costs 256 depth tests per pixel on this backend (rhi_vita_efb.inc efb_z_resolve), so that
// one copy is made only while its destination is being bound: the first frame of an effect finds it
// absent (its draw is skipped, counted, as before run 97) and the next frame has it.
extern int rhi_vita_presents __attribute__((weak));
static int copyNow(void) { return &rhi_vita_presents ? rhi_vita_presents : 0; }
static void copyDstWanted(const void* p) {
    for (int i = 0; i < gCopyDstN; ++i)
        if ((const unsigned char*)p >= gCopyDst[i].lo && (const unsigned char*)p < gCopyDst[i].hi) { gCopyDst[i].want = copyNow(); return; }
}
// THE EFB MODE (docs/47 "EFB copies as GPU work"): the backend reads ux0:data/stairfax/efb_mode.txt once.
//   0 direct rendering, no real copies   1 (default) the game renders into an FBO, a copy is one GPU quad
//   2 direct rendering + the CPU readback (copyReal)
// Absent backend (the host tests) = mode 0 semantics: nothing is copied for real.
extern int rhi_vita_efbMode(void) __attribute__((weak));
extern RhiTexture* rhi_vita_copyEfb(RhiTexture* reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt) __attribute__((weak));
extern void rhi_vita_copyEfbFree(RhiTexture* t) __attribute__((weak));
static int efbMode(void) { return rhi_vita_efbMode ? rhi_vita_efbMode() : 0; }
// Nothing may keep pointing at a copy texture that is gone (or has been recreated at another address).
static void copyTexRemap(RhiTexture* from, RhiTexture* to) {
    if (!from) return;
    for (int m = 0; m < 8; ++m) if (gTexMap[m] == from) gTexMap[m] = to;
    if (gTex == from) gTex = to;
}
static int copyDstIndex(const void* p) {
    for (int i = 0; i < gCopyDstN; ++i)
        if ((const unsigned char*)p >= gCopyDst[i].lo && (const unsigned char*)p < gCopyDst[i].hi) return i;
    return -1;
}
static void copyDstFreeTex(int i) {
    if (!gCopyDst[i].tex) return;
    copyTexRemap(gCopyDst[i].tex, NULL);
    if (rhi_vita_copyEfbFree) rhi_vita_copyEfbFree(gCopyDst[i].tex);
    gCopyDst[i].tex = NULL;
}
// THE REAL COPY (2026-10-06, docs/47 "EFB copies, done right"): the rectangle is read back from the
// screen, resampled to the copy's size, encoded into `dest` in the copy format's texel layout, and
// the texture cache is told -- so the game's own texture path samples it, and its CPU passes over
// the texels (the shadow blur) see real data. Stage 1 limits, each counted: no Z format (vitaGL
// gave no usable depth texture on the probe), not the whole screen (the readback is a CPU copy that
// scales with pixels: 480x272 cost 14 ms). ⚠ Every readback is a scene split that LOSES the depth
// buffer (probe: depth_survived=0); the backend clears it so what follows is at least defined.
extern unsigned char* rhi_vita_readEfbRect(int x, int y, int w, int h, int* ow, int* oh) __attribute__((weak));
extern size_t gxTexEncodeCopy(int fmt, int w, int h, const unsigned char* rgba, unsigned char* dest);
extern void gxCopyResample(const unsigned char* src, int sw, int sh, int flip, unsigned char* dst, int dw, int dh);
unsigned gx_copy_real, gx_copy_zfmt, gx_copy_whole_screen, gx_copy_unsupported, gx_copy_real_kpx;
unsigned gx_copy_refused, gx_copy_dst_absent, gx_copy_dst_table_full;   // flicker counters: backend refusals / destinations left absent / destination table (GX_COPY_DSTS) full
unsigned gx_copy_gpu, gx_copy_mem;   // mode 1: copies done on the GPU / through memory (the B8 shadow texture)
unsigned gx_copy_z, gx_copy_z_idle;  // mode 1: Z8 copies done (also in gx_copy_gpu) / whole-screen ones skipped because nothing samples them
extern int rhi_vita_efbZMode(void) __attribute__((weak));
unsigned long long gx_copy_real_us;
static int copyReal(void* dest) {
    if (!rhi_vita_readEfbRect || !gCopyDstW || !gCopyDstH) return 0;
    if (gCopyDstFmt & 0x10) { ++gx_copy_zfmt; return 0; }
    if ((u32)gCopySrc[2] * gCopySrc[3] >= 640u * 480u) { ++gx_copy_whole_screen; return 0; }
    unsigned long long t0 = GXT_NOW();
    int sw = 0, sh = 0;
    unsigned char* rb = rhi_vita_readEfbRect(gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3], &sw, &sh);
    if (!rb) return 0;
    unsigned char* px = (unsigned char*)malloc((size_t)gCopyDstW * gCopyDstH * 4);
    size_t n = 0;
    if (px) { gxCopyResample(rb, sw, sh, 1, px, gCopyDstW, gCopyDstH); n = gxTexEncodeCopy(gCopyDstFmt, gCopyDstW, gCopyDstH, px, (unsigned char*)dest); }
    free(px); free(rb);
    if (!n) { ++gx_copy_unsupported; return 0; }
    gx_tex_invalidate(dest, (u32)n);
    ++gx_copy_real; gx_copy_real_kpx += (unsigned)(sw * sh) / 1000u; gx_copy_real_us += GXT_NOW() - t0;
    return 1;
}
// The "absent" bookkeeping: the destination is recorded, a draw that samples it is skipped (counted).
static void copyDstRecord(const unsigned char* lo) {
    u32 n = GXGetTexBufferSize(gCopyDstW, gCopyDstH, (u32)gCopyDstFmt, 0, 0);   // the SOURCE size bounds the destination (a mipmap copy is half of it)
    if (!inCopyDst(lo)) {
        if (gCopyDstN < GX_COPY_DSTS) { gCopyDst[gCopyDstN].lo = lo; gCopyDst[gCopyDstN].hi = lo + (n ? n : 32);
                                        gCopyDst[gCopyDstN].w = gCopyDstW; gCopyDst[gCopyDstN].h = gCopyDstH; gCopyDst[gCopyDstN].fmt = gCopyDstFmt;
                                        gCopyDst[gCopyDstN].tex = NULL; gCopyDst[gCopyDstN].want = -1000; ++gCopyDstN; }
    }
}
// Mode 1, a colour format: one GPU quad into the destination's own texture. The record keeps it and
// GXLoadTexObj binds it. A backend that refuses (NULL) leaves the destination "absent".
static int copyGpu(const unsigned char* dest) {
    if (!rhi_vita_copyEfb || !gCopyDstW || !gCopyDstH) return 0;
    copyDstRecord(dest);
    int i = copyDstIndex(dest);
    if (i < 0) { ++gx_copy_dst_table_full; return 0; }   // table full: absent, as before (counted)
    RhiTexture* old = gCopyDst[i].tex;
    RhiTexture* t = rhi_vita_copyEfb(old, gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3], gCopyDstW, gCopyDstH, gCopyDstFmt);
    if (!t) { ++gx_copy_refused; copyDstFreeTex(i); return 0; }   // (the backend has not touched `old` on a refusal; counted)
    if (old && t != old) copyTexRemap(old, t);  // recreated at another size: the backend freed the old one
    gCopyDst[i].tex = t; gCopyDst[i].w = gCopyDstW; gCopyDst[i].h = gCopyDstH; gCopyDst[i].fmt = gCopyDstFmt;
    ++gx_copy_gpu;
    return 1;
}
static void gxCopyTexImpl(void* dest, u8 clear);
void GXCopyTex(void* dest, u8 clear) { RT_SUBS(RT_S_ENTRY_COPY); unsigned long long t = GXT_NOW(); RT_ENTER(RT_SHIM); gxCopyTexImpl(dest, clear); RT_LEAVE(); gx_w_copy_us += GXT_NOW() - t; }
// COPY SOURCES ON DEMAND (run 260): while the census is armed (the shot_now.txt request) every small EFB copy's SOURCE rectangle is read back as it is
// taken and appended to ux0:data/stairfax_copies.bin: "CPY1", then 8 ints (dest address, src x, y, w, h, readback w, h, GX format), then w*h RGBA.
// The log line gives the destination's texture pointer, which is what the census prints per map. For the 32x32 sphere maps the game lights models with.
extern int rhi_vita_census_arm __attribute__((weak));
static void copyDbgDump(const void* dest) {
    static int sArmWas, sN;
    int arm = &rhi_vita_census_arm ? rhi_vita_census_arm : 0;
    if (arm > 0 && !sArmWas) { sN = 0; remove("ux0:data/stairfax_copies.bin"); }
    sArmWas = arm > 0;
    if (arm <= 0 || sN >= 48 || !rhi_vita_readEfbRect || (u32)gCopySrc[2] * gCopySrc[3] > 128u * 128u) return;
    int sw = 0, sh = 0;
    unsigned char* rb = rhi_vita_readEfbRect(gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3], &sw, &sh);
    if (!rb) { printf("[copydbg] %d: readback refused\n", sN); return; }
    FILE* f = fopen("ux0:data/stairfax_copies.bin", "ab");
    if (f) {
        int h[8] = { (int)(size_t)dest, gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3], sw, sh, gCopyDstFmt };
        fwrite("CPY1", 1, 4, f); fwrite(h, sizeof h, 1, f); fwrite(rb, 4, (size_t)sw * sh, f); fclose(f);
    }
    int i = copyDstIndex(dest);
    printf("[copydbg] %d: dest %p src %d,%d %dx%d -> dst %dx%d fmt %d | readback %dx%d | texture %p\n", sN, dest, gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3],
           gCopyDstW, gCopyDstH, gCopyDstFmt, sw, sh, i >= 0 ? (void*)gCopyDst[i].tex : NULL);
    ++sN; free(rb);
    {   /* run 261: is the readback upside down in the FBO mode? the SAME rectangle mirrored about the picture (EFB rows 480-y-h): marked by src y = -1-y */
        int mw = 0, mh = 0; unsigned char* mb = rhi_vita_readEfbRect(gCopySrc[0], 480 - gCopySrc[1] - gCopySrc[3], gCopySrc[2], gCopySrc[3], &mw, &mh);
        FILE* g = mb ? fopen("ux0:data/stairfax_copies.bin", "ab") : NULL;
        if (g) { int h2[8] = { (int)(size_t)dest, gCopySrc[0], -1 - gCopySrc[1], gCopySrc[2], gCopySrc[3], mw, mh, gCopyDstFmt };
                 fwrite("CPY1", 1, 4, g); fwrite(h2, sizeof h2, 1, g); fwrite(mb, 4, (size_t)mw * mh, g); fclose(g); }
        free(mb);
    }
}
static void gxCopyTexImpl(void* dest, u8 clear) {
    ++gx_copytex_calls;
    if (STFX_ARM5()) return;   /* arm 5 (see gStfxPerfArm): no EFB copy */
    if (dest) copyDbgDump(dest);
    if (dest) {
        int mode = efbMode(), done = 0;
        if (mode == 1) {
            if (gCopyDstFmt & 0x10) {
                // GX_TF_Z8: the backend resolves depth into the FBO's alpha and copies that. Every other
                // Z format (the game makes none), zmode 0, and the whole-screen copy nobody is sampling
                // stay absent, each counted.
                int zm = rhi_vita_efbZMode ? rhi_vita_efbZMode() : 0;
                int whole = (u32)gCopySrc[2] * gCopySrc[3] >= 640u * 480u;
                int i = copyDstIndex(dest);
                if (gCopyDstFmt != GX_TF_Z8 || !zm) ++gx_copy_zfmt;
                else if (whole && zm != 2 && !(i >= 0 && copyNow() - gCopyDst[i].want <= 2)) ++gx_copy_z_idle;
                else if ((done = copyGpu((const unsigned char*)dest))) ++gx_copy_z;
                else ++gx_copy_zfmt;
            }
            else if (gCopyDstFmt == 0x2A) {                  // GX_CTF_B8: the game blurs this texture on the CPU (boxBlurTexture): memory path, read from the FBO
                if (copyReal(dest)) {
                    ++gx_copy_mem; done = 1;
                    copyDstRelease(dest, 1);                 // no stale GPU copy may shadow the real texels
                }
            }
            else done = copyGpu((const unsigned char*)dest);
        }
        else if (mode == 2) done = copyReal(dest);           // today's behaviour: CPU readback
        if (!done) {
            if (!(gCopyDstFmt & 0x10)) ++gx_copy_dst_absent;   // (a Z copy left absent on purpose is gx_copy_z_idle / gx_copy_zfmt) this destination has NO valid copy until a later one succeeds: draws that sample it are skipped (gx_copy_draws_skipped)
            int i = copyDstIndex(dest); if (i >= 0) copyDstFreeTex(i);   // an older GPU copy would now be stale
            copyDstRecord((const unsigned char*)dest);
        }
    }
    if (clear && rhi_vita_clearRect) {
        ++gx_copy_clears;
        rhi_vita_clearRect(gCopySrc[0], gCopySrc[1], gCopySrc[2], gCopySrc[3],
                           (u32)gCopyClr.r | (u32)gCopyClr.g << 8 | (u32)gCopyClr.b << 16 | (u32)gCopyClr.a << 24,
                           (float)(gCopyClrZ & 0xFFFFFF) / 16777215.0f);
    }
}
// A freed block may be reused as an ordinary texture: forget copy destinations inside it.
static void journalCopyHit(int k) {
    if (k >= 8) { printf("\n"); return; }
    const unsigned char* im = (const unsigned char*)gMapImg[k];
    for (int i = 0; i < gCopyDstN; ++i)
        if (im >= gCopyDst[i].lo && im < gCopyDst[i].hi) {
            printf(" map %d image %p %ux%u fmt %u | copy range %p..%p (+%ld) dst %ux%u fmt %d\n", k, (const void*)im,
                   gMapW[k], gMapH[k], gMapFmt[k], (const void*)gCopyDst[i].lo, (const void*)gCopyDst[i].hi,
                   (long)(im - gCopyDst[i].lo), gCopyDst[i].w, gCopyDst[i].h, gCopyDst[i].fmt);
            return;
        }
    printf(" map %d image %p (no range?)\n", k, (const void*)im);
}
static void copyDstRelease(const void* p, u32 n) {
    const unsigned char* lo = (const unsigned char*)p; const unsigned char* hi = lo + n;
    for (int i = 0; i < gCopyDstN;)
        if (gCopyDst[i].lo < hi && lo < gCopyDst[i].hi) { copyDstFreeTex(i); gCopyDst[i] = gCopyDst[--gCopyDstN]; } else ++i;
}

// --- PIXEL STATE v1 (vita fork, docs/47 run 27) -------------------------------------
// Before this, GXSetChanCtrl / ChanMatColor / BlendMode / ColorUpdate were WEAK scaffold stubs:
// register-sourced colour came out as decodePrim's "no CLR0" default (opaque white), and a
// full-screen grid of such strips painted over the whole frame (run 25's draw journal).
// v1 models: the colour channels (register vs vertex source, material/ambient colour; LIT
// channels APPROXIMATED as mat * min(1, amb + 0.5) until lights are wired), the blend mode
// (none / alpha / additive), a discarding alpha compare (RHI TEST, fixed 0.5 threshold), and
// colour-update-off (the draw is skipped: depth/stencil-only passes must not paint).
// ⚠ Consumed only after the GAME calls one of these (gPxActive): the upstream viewers set the
// RHI alpha mode themselves and never call them, so their behaviour is unchanged.
enum { CH_COLOR0, CH_COLOR1, CH_ALPHA0, CH_ALPHA1 };
// (PxChan, gChan, gMat, gAmb, gNumChans, gColorUpdate, gBlend*, gAc*, gCullMode, gPxActive: gx_hal -- chan defaults: vertex colour, mat 255, one channel, colour update on)
unsigned       gx_px_skipped_noupdate;   // draws dropped because colour update was off
// The census's per-key state line (gx_tev.c, run 259): channel controls, material / ambient registers, blend, alpha compare, depth, cull, and the vertex layout
// (desc = bit per attribute that is present; nbt = the normal is 9 components; a TEXnMTXIDX bit means the vertex names its own texture matrix, which the port does NOT read).
void gx_draw_dbg_state(char* buf, int cap) {
    unsigned desc = 0; for (int a = 0; a < GX_VA_MAX_ATTR && a < 32; ++a) if (gDesc[a] != GX_NONE) desc |= 1u << a;
    snprintf(buf, (size_t)cap, "chans %u c0 en%u amb%u mat%u lm%02x df%u af%u | a0 en%u mat%u | mat %02x%02x%02x%02x amb %02x%02x%02x%02x | blend %u %u/%u upd%u | ac %u,%u op%u ref %u,%u | z %u,%u,%u cull %u | desc %06x",
             gNumChans, gChan[CH_COLOR0].enable, gChan[CH_COLOR0].ambSrc, gChan[CH_COLOR0].matSrc, gChan[CH_COLOR0].lightMask, gChan[CH_COLOR0].diffFn, gChan[CH_COLOR0].attnFn,
             gChan[CH_ALPHA0].enable, gChan[CH_ALPHA0].matSrc, gMat[0][0], gMat[0][1], gMat[0][2], gMat[0][3], gAmb[0][0], gAmb[0][1], gAmb[0][2], gAmb[0][3],
             gBlendType, gBlendSrc, gBlendDst, gColorUpdate, gAcComp0, gAcComp1, gAcOp, gAcRef0, gAcRef1, gZCompare, gZFunc, gZUpdate, gCullMode, desc);
}
static void chanIdx(int chan, int* c, int* a) {   // GX_COLOR0,1 / ALPHA0,1 / COLOR0A0,1A1
    *c = *a = -1;
    switch (chan) { case 0: *c = CH_COLOR0; break; case 1: *c = CH_COLOR1; break;
                    case 2: *a = CH_ALPHA0; break; case 3: *a = CH_ALPHA1; break;
                    case 4: *c = CH_COLOR0; *a = CH_ALPHA0; break; case 5: *c = CH_COLOR1; *a = CH_ALPHA1; break; }
}
GXH_INLINE void gxh_b_GXSetNumChans(GxHal* h, const int mark, u8 n) { h->px.numChans = n; h->px.pxActive = 1; GXH_MARK(h, mark, GXH_G_PIX); }
GXH_PAIR(GXSetNumChans, (u8 n), n)
GXH_INLINE void gxh_b_GXSetChanCtrl(GxHal* h, const int mark, int chan, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn) {
    h->px.pxActive = 1;                   /* first: with the store last the compiler duplicated this tail into every channel case (+5 instructions, A6) */
    GXH_MARK(h, mark, GXH_G_CHAN); GXH_MARK(h, mark, GXH_G_PIX);   /* pxActive lives in the PIX group; marked up front for the same reason (the flag stores were duplicated into the channel cases) */
    int c, a; chanIdx(chan, &c, &a);
    PxChan v = { enable, (u8)amb_src, (u8)mat_src, (u8)light_mask, (u8)diff_fn, (u8)attn_fn };
    if (c >= 0) h->chan[c] = v;
    if (a >= 0) h->chan[a] = v;
}
GXH_PAIR(GXSetChanCtrl, (int chan, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn), chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)
GXH_INLINE void gxh_b_GXSetChanMatColor(GxHal* h, const int mark, int chan, GXColor col) {
    int c, a; chanIdx(chan, &c, &a); int s = (chan == 1 || chan == 3 || chan == 5);
    if (c >= 0) { h->mat[s][0] = col.r; h->mat[s][1] = col.g; h->mat[s][2] = col.b; }
    if (a >= 0) h->mat[s][3] = col.a;
    h->px.pxActive = 1;
    GXH_MARK(h, mark, GXH_G_CHAN); GXH_MARK(h, mark, GXH_G_PIX);
}
GXH_PAIR(GXSetChanMatColor, (int chan, GXColor col), chan, col)
GXH_INLINE void gxh_b_GXSetChanAmbColor(GxHal* h, const int mark, int chan, GXColor col) {
    int c, a; chanIdx(chan, &c, &a); int s = (chan == 1 || chan == 3 || chan == 5);
    if (c >= 0) { h->amb[s][0] = col.r; h->amb[s][1] = col.g; h->amb[s][2] = col.b; }
    if (a >= 0) h->amb[s][3] = col.a;
    h->px.pxActive = 1;
    GXH_MARK(h, mark, GXH_G_CHAN); GXH_MARK(h, mark, GXH_G_PIX);
}
GXH_PAIR(GXSetChanAmbColor, (int chan, GXColor col), chan, col)
// BLEND FACTORS (2026-10-07, docs/47 run 122). Until now the backend knew three cases -- opaque, "blend"
// (always SRC_ALPHA, 1-SRC_ALPHA) and "add" (SRC_ALPHA, ONE) -- whatever factors the game asked for, and
// GX_BM_SUBTRACT was drawn opaque. pxAlphaMode now carries the real request in the mode word's upper bits
// (RHI_ALPHA_* class in the low byte, so every existing comparison still holds):
//   0x100 exact factors follow | 0x200 subtract (dst - src) | src factor << 12 | dst factor << 16
// Each distinct request is logged (the first 32). ux0:data/stairfax/blend_old.txt present = the old three
// cases, for the A/B.
unsigned gx_blend_sets, gx_blend_distinct;
static u32 gBlendSeen[64];
GXH_INLINE void gxh_b_GXSetBlendMode(GxHal* h, const int mark, int type, int src, int dst, int op) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    h->px.blendType = (u8)type; h->px.blendSrc = (u8)src; h->px.blendDst = (u8)dst; h->px.pxActive = 1;
    GXH_MARK(h, mark, GXH_G_PIX);
    ++gx_blend_sets;
    u32 key = ((u32)(type & 0xf) << 12) | ((u32)(src & 0xf) << 8) | ((u32)(dst & 0xf) << 4) | (u32)(op & 0xf), i;
    for (i = 0; i < gx_blend_distinct && i < 64; ++i) if (gBlendSeen[i] == key) return;
    if (gx_blend_distinct < 64) gBlendSeen[gx_blend_distinct] = key;
    if (++gx_blend_distinct <= 32)
        printf("[blend] request %u (set %u): type %d (0 none, 1 blend, 2 logic, 3 subtract) src %d dst %d op %d\n",
               gx_blend_distinct, gx_blend_sets, type, src, dst, op);
}
GXH_PAIR(GXSetBlendMode, (int type, int src, int dst, int op), type, src, dst, op)
GXH_INLINE void gxh_b_GXSetAlphaCompare(GxHal* h, const int mark, int comp0, u8 ref0, int op, int comp1, u8 ref1) {
    RT_COUNT(RT_S_SETTERS);   /* Task 3: count only */
    h->px.acComp0 = (u8)comp0; h->px.acRef0 = ref0; h->px.acOp = (u8)op; h->px.acComp1 = (u8)comp1; h->px.acRef1 = ref1; h->px.pxActive = 1;
    GXH_MARK(h, mark, GXH_G_PIX);
}
GXH_PAIR(GXSetAlphaCompare, (int comp0, u8 ref0, int op, int comp1, u8 ref1), comp0, ref0, op, comp1, ref1)
GXH_INLINE void gxh_b_GXSetColorUpdate(GxHal* h, const int mark, u8 enable) { h->px.colorUpdate = enable; h->px.pxActive = 1; GXH_MARK(h, mark, GXH_G_PIX); }
GXH_PAIR(GXSetColorUpdate, (u8 enable), enable)
void GXSetAlphaUpdate(u8 enable) { (void)enable; }
GXH_INLINE void gxh_b_GXSetCullMode(GxHal* h, const int mark, int mode) { h->px.cullMode = (u8)mode; GXH_MARK(h, mark, GXH_G_PIX); }
GXH_PAIR(GXSetCullMode, (int mode), mode)

// One vertex's colour through channel 0 (colour + alpha), GX style.
// LIGHTING (vita fork, docs/47 run 58): GX lights PER VERTEX, so this is the hardware model, not an
// approximation: out = mat * clamp(amb + sum over the channel's lights of colour * diffuse * attn).
// Formulas follow Dolphin's LightingShaderGen (the reference GX implementation); light registers are
// the ones GXLoadLightObjImm recorded (hal/gx_light.c), already in view space as the game loads them.
// Before this, a lit channel was "mat * min(1, amb + 0.5)": retail's attract laptop is green/cyan
// (lit by its screen), the Vita's was red (Tyler, run 57; retail frame from stfx_frame.py).
typedef PxLightRec PxLight;   /* color = r<<24|g<<16|b<<8|a (GXInitLightColor) -- as a VALUE, not bytes: on ARM the bytes are a,b,g,r */
int gStfxLightArm = 1;   /* run 62 A/B: 0 = the pre-run-58 guess (cost measurement) */
/* One pass over the channel's lights per vertex, all four components at once. Run 62 measured the
   first version (the whole light loop -- sqrt, divide, dots -- once PER COMPONENT) at 155 ms/frame:
   415 vs 260 ms in alternating windows of one run. The geometry is per light, not per component. */
static void pxLitRGBA(const PxChan* ch, const RhiTexVertex* P, const float* N, float acc[4]) {
    acc[0] = acc[1] = acc[2] = acc[3] = 0.0f;
    int df = ch->attnFn == 0 ? 0 : ch->diffFn;   /* SDK GXSetChanCtrl: diffuse fn is forced to NONE for GX_AF_SPEC */
    for (int i = 0; i < 8; ++i) {
        if (!(ch->lightMask & (1u << i))) continue;
        const PxLight* L = &gx_light_regs[i];
        if (!L->color) continue;                 /* a black light adds nothing */
        float lx = L->lpos[0] - P->x, ly = L->lpos[1] - P->y, lz = L->lpos[2] - P->z;
        float d2 = lx*lx + ly*ly + lz*lz, d = sqrtf(d2), attn = 1.0f;
        if (d > 0.0f) { float inv = 1.0f / d; lx *= inv; ly *= inv; lz *= inv; }
        float ndl = N[0]*lx + N[1]*ly + N[2]*lz;
        if (ch->attnFn == 1 /*GX_AF_SPOT*/) {
            float c = lx*L->ldir[0] + ly*L->ldir[1] + lz*L->ldir[2]; if (c < 0) c = 0;
            float num = L->a[0] + L->a[1]*c + L->a[2]*c*c; if (num < 0) num = 0;
            float den = L->k[0] + L->k[1]*d + L->k[2]*d2;
            attn = den != 0.0f ? num / den : 0.0f;
        } else if (ch->attnFn == 0 /*GX_AF_SPEC*/) {
            float c = ndl >= 0.0f ? N[0]*L->ldir[0] + N[1]*L->ldir[1] + N[2]*L->ldir[2] : 0.0f; if (c < 0) c = 0;
            float num = L->a[0] + L->a[1]*c + L->a[2]*c*c; if (num < 0) num = 0;
            float den = L->k[0] + L->k[1]*c + L->k[2]*c*c;
            attn = den != 0.0f ? num / den : 0.0f;
        }
        float f = attn * (df == 0 ? 1.0f : df == 1 ? ndl : (ndl > 0.0f ? ndl : 0.0f)) * (1.0f / 255.0f);
        if (f == 0.0f) continue;
        u32 c = L->color;
        acc[0] += f * (float)(c >> 24); acc[1] += f * (float)((c >> 16) & 0xff);
        acc[2] += f * (float)((c >> 8) & 0xff); acc[3] += f * (float)(c & 0xff);
    }
}
static int pxNeedNormals(void) {
    return gPxActive && gNumChans && gStfxLightArm && (gChan[CH_COLOR0].enable || gChan[CH_ALPHA0].enable);
}
static int pxSameLighting(const PxChan* a, const PxChan* b) {
    return a->lightMask == b->lightMask && a->diffFn == b->diffFn && a->attnFn == b->attnFn;
}
static u32 pxChannelColour(u32 vtx, int haveVtx, const RhiTexVertex* pos, const float* nrm, int haveN) {
    if (gNumChans == 0) return 0xFFFFFFFFu;
    u8 in[4] = { (u8)vtx, (u8)(vtx >> 8), (u8)(vtx >> 16), (u8)(vtx >> 24) }, out[4];
    float n[3] = { 0, 0, 0 };
    const PxChan* cc = &gChan[CH_COLOR0]; const PxChan* ca = &gChan[CH_ALPHA0];
    /* 2026-10-08 (docs/47 "Run 132"): an ENABLED channel is lit whether or not the vertex has a normal. Until now a
       vertex without one took the pre-run-58 placeholder (ambient + 0.5): the cloud layer (objprint_dolphin.c:880 --
       channel enabled, GX_LIGHT_NULL, material = vertex colour, ambient = the object's colour) came out as vertex colour
       x (ambient + 0.5) instead of vertex colour x ambient. With no normal N is zero: a light with a diffuse function
       adds nothing, one with GX_DF_NONE adds its colour, as in GX. ux0:data/stairfax/chan_oldguess.txt = the old rule (A/B). */
    static int sOldGuess = -1;
    if (sOldGuess < 0) { FILE* f = fopen("ux0:data/stairfax/chan_oldguess.txt", "r"); sOldGuess = f ? 1 : 0; if (f) fclose(f);
                         printf("[chan] enabled channel without a normal: %s\n", sOldGuess ? "OLD ambient + 0.5 (chan_oldguess.txt)" : "lit (ambient + lights)"); }
#ifdef BREAK_13
    sOldGuess = 1;   /* host break arm: check 13 must FAIL */
#endif
    const int litNoN = !sOldGuess; gPxOldGuess = sOldGuess;
    int lightC = cc->enable && (haveN || litNoN) && gStfxLightArm, lightA = ca->enable && (haveN || litNoN) && gStfxLightArm;
    if ((lightC || lightA) && haveN) {
        float l = sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
        if (l > 0.0f) { float inv = 1.0f / l; n[0] = nrm[0] * inv; n[1] = nrm[1] * inv; n[2] = nrm[2] * inv; }
    }
    float accC[4] = { 0, 0, 0, 0 }, accA[4] = { 0, 0, 0, 0 };
    if (lightC) pxLitRGBA(cc, pos, n, accC);
    if (lightA) { if (lightC && pxSameLighting(cc, ca)) { accA[3] = accC[3]; } else pxLitRGBA(ca, pos, n, accA); }
    for (int k = 0; k < 4; ++k) {
        const PxChan* ch = k < 3 ? cc : ca;
        int fromVtx = ch->matSrc == 1 /*GX_SRC_VTX*/ && haveVtx;
        int m = fromVtx ? in[k] : gMat[0][k];
        if (ch->enable) {
            int amb = ch->ambSrc == 1 && haveVtx ? in[k] : gAmb[0][k];
            int lit_ = k < 3 ? lightC : lightA;
            float lit = (float)amb / 255.0f + (lit_ ? (k < 3 ? accC[k] : accA[3]) : 0.5f);   // no normal (or arm 0): the old guess
            if (lit < 0.0f) lit = 0.0f; if (lit > 1.0f) lit = 1.0f;
            m = (int)((float)m * lit + 0.5f);
        }
        out[k] = (u8)m;
    }
    return (u32)out[0] | (u32)out[1] << 8 | (u32)out[2] << 16 | (u32)out[3] << 24;
}
static void pxJournalLit(u32 v0in, int haveClr, u32 v0out, int haveN) {   /* run 59: why are characters RED? lit draws' channel inputs, journal frame */
            static int sLitLines; static u32 sSeen[64]; static int sSeenN;
            if (gxj() && gChan[CH_COLOR0].enable && sLitLines < 60) {
                const PxChan* c0 = &gChan[CH_COLOR0];
                /* one line per DISTINCT channel setup (material, ambient, mask, fns, light colours) */
                u32 h = 2166136261u;
                #define MIX(v) (h = (h ^ (u32)(v)) * 16777619u)
                MIX(c0->ambSrc); MIX(c0->matSrc); MIX(c0->lightMask); MIX(c0->diffFn); MIX(c0->attnFn);
                for (int q = 0; q < 4; ++q) { MIX(gMat[0][q]); MIX(gAmb[0][q]); }
                for (int q = 0; q < 8; ++q) if (c0->lightMask & (1u << q)) MIX(gx_light_regs[q].color);
                #undef MIX
                for (int q = 0; q < sSeenN; ++q) if (sSeen[q] == h) return;
                if (sSeenN < 64) sSeen[sSeenN++] = h;
                sLitLines++;
                printf("[lit] src amb%u mat%u mask %02x df%u af%u | mat %02x%02x%02x%02x amb %02x%02x%02x%02x | vtx %08x clr%d nrm%d -> %08x |",
                       c0->ambSrc, c0->matSrc, c0->lightMask, c0->diffFn, c0->attnFn,
                       gMat[0][0], gMat[0][1], gMat[0][2], gMat[0][3], gAmb[0][0], gAmb[0][1], gAmb[0][2], gAmb[0][3],
                       (unsigned)v0in, haveClr, haveN, (unsigned)v0out);
                for (int i = 0; i < 8; ++i) if (c0->lightMask & (1u << i))
                    printf(" L%d %08x @(%.0f,%.0f,%.0f) a %.3g %.3g %.3g k %.3g %.3g %.3g dir %.2f %.2f %.2f", i, (unsigned)gx_light_regs[i].color,
                           gx_light_regs[i].lpos[0], gx_light_regs[i].lpos[1], gx_light_regs[i].lpos[2],
                           gx_light_regs[i].a[0], gx_light_regs[i].a[1], gx_light_regs[i].a[2], gx_light_regs[i].k[0], gx_light_regs[i].k[1], gx_light_regs[i].k[2],
                           gx_light_regs[i].ldir[0], gx_light_regs[i].ldir[1], gx_light_regs[i].ldir[2]);
                printf("\n");
            }
        }
// Host-test hook (host/tests/gx_port_test.c): one vertex through the channel, as a draw would.
u32 gx_draw_litColour(u32 vtx, int haveVtx, const float pos[3], const float nrm[3]) {
    RhiTexVertex v; v.x = pos[0]; v.y = pos[1]; v.z = pos[2]; v.rgba = vtx; v.u = v.v = 0;
    return pxChannelColour(vtx, haveVtx, &v, nrm, nrm != 0);
}
static int pxAlphaMode(void) {
    static int sOld = -1;   // blend_old.txt: the old three cases (A/B arm), read once and logged
    if (sOld < 0) { FILE* f = fopen("ux0:data/stairfax/blend_old.txt", "r"); sOld = f ? 1 : 0; if (f) fclose(f);
                    printf("[blend] %s\n", sOld ? "OLD three-case blend (blend_old.txt)" : "exact GX blend factors"); }
    if (gBlendType == 1 /*GX_BM_BLEND*/ || (gBlendType == 3 /*GX_BM_SUBTRACT*/ && !sOld)) {
        int cls = gBlendType == 3 || gBlendDst == 1 /*GX_BL_ONE*/ ? RHI_ALPHA_ADD : RHI_ALPHA_BLEND;
        if (sOld || !rhi_vita_drawTev) return cls;   // only the Vita backend reads the upper bits (the host viewers switch on the class)
        if (gBlendType == 3) return cls | 0x100 | 0x200 | (1 << 12) | (1 << 16);   // GX subtract ignores the factors: dst - src
        return cls | 0x100 | ((gBlendSrc & 7) << 12) | ((gBlendDst & 7) << 16);
    }
    int passAll0 = gAcComp0 == 7 /*ALWAYS*/, passAll1 = gAcComp1 == 7;
    int discards = gAcOp == 1 /*OR*/ ? !(passAll0 || passAll1) : !(passAll0 && passAll1);
    return discards ? RHI_ALPHA_TEST : RHI_ALPHA_OPAQUE;
}

// --- TEV / raster state: recorded, not yet consumed by the single-texture path -----
// (gx_tev_op[16]: gx_hal.tevOp)
extern void gx_tev_setop(int stage, int mode);   /* gx_tev.c: the SDK's canned combiners (the mode used to be only recorded) */
GXH_INLINE void gxh_b_GXSetTevOp(GxHal* h, const int mark, int stage, int mode) {
    if ((unsigned)stage < 16) { h->tevOp[stage] = (u8)mode; GXH_MARK(h, mark, GXH_G_TEVOP); }
    if (mark) gxh_g_tev_setop(stage, mode); else gx_tev_setop(stage, mode);   /* the composed setters: this instance's bodies (gx_tev.c) */
}
GXH_PAIR(GXSetTevOp, (int stage, int mode), stage, mode)
/* SDK GXBump.c:188 -- the stage repeats the previous stage's indirect offset: GXSetTevIndirect(stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF,
   GX_ITW_0, GX_ITW_0, add_prev TRUE, utc_lod FALSE, GX_ITBA_OFF). The recorded state is what gx_tev_build reads (it was an empty stub: the stage drew direct). */
#ifdef BREAK_21   /* host negative control: the old empty stub */
void GXSetTevIndRepeat(GXTevStageID stage) { (void)stage; }
void gxh_g_GXSetTevIndRepeat(GXTevStageID stage) { (void)stage; }
#else
void GXSetTevIndRepeat(GXTevStageID stage) { GXSetTevIndirect(stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_0, GX_ITW_0, GX_TRUE, GX_FALSE, GX_ITBA_OFF); }
void gxh_g_GXSetTevIndRepeat(GXTevStageID stage) { gxh_g_GXSetTevIndirect(stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_0, GX_ITW_0, GX_TRUE, GX_FALSE, GX_ITBA_OFF); }   /* the G variant of what it composes, never the wrapped public symbol */
#endif
void GXSetLineWidth(u8 width, GXTexOffset texOffsets) { (void)width; (void)texOffsets; }
void GXSetPointSize(u8 size, GXTexOffset texOffsets) { (void)size; (void)texOffsets; }

// --- debug ------------------------------------------------------------------------
unsigned gx_torus_calls;
void GXDrawTorus(f32 rc, u8 numc, u8 numt) { (void)rc; (void)numc; (void)numt; ++gx_torus_calls; }  // staff quake FX: not drawn

// ---- DISPLAY-LIST CENSUS (2026-10-08, docs/47 "GX fast path, stage 0") --------------------------------
// Before any display list is cached or handed to a vertex shader: which lists are STATIC? A list is
// (pointer, size, vertex descriptor, array bases). Every call hashes the list's bytes and, separately,
// every array element its indices reference; a hash that differs from the last call's is a change.
// Also counted: strip vertices vs distinct vertices per list (what an indexed cache saves), the
// matrix slots used, and the distinct vertex-program shapes (channel config + texgens) with the
// (vertex shape, TEV key) pairs a GPU path would have to compile.
// OFF unless ux0:data/stairfax/dl_census.txt exists (it hashes ~1 MB a frame). The table publishes its
// occupancy and DROPS; a self-test at enable mutates an array and a list and must see both.
#define DLC_N 4096
typedef struct { u32 key; const unsigned char* dl; u32 nbytes, hDl, hArr, calls, chgDl, chgArr, stripV, uniqV, tris;
                 u8 used, pnmtx, hint, maxSlot, forced; } DlcEnt;
static DlcEnt gDlc[DLC_N];
static unsigned gDlcN, gDlcDrops, gDlcBadWalk;
unsigned gx_imm_prims, gx_imm_verts;
static inline u32 dlcFnv(u32 h, const void* p, unsigned n) {
    const unsigned char* b = (const unsigned char*)p;
    for (unsigned i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
static int dlcElemSize(GXVtxFmt fmt, GXAttr a) {   // one ARRAY element of an indexed attribute
#ifdef DLC_BREAK
    return 0;   /* host break arm: the census hashes no array bytes, so its self-test must FAIL */
#endif
    VatAttr* v = &gVat[fmt][a];
    if (a == GX_VA_POS) return compTypeSize(v->type) * (v->cnt == GX_POS_XYZ ? 3 : 2);
    if (a == GX_VA_NRM) return compTypeSize(v->type) * (v->cnt == GX_NRM_XYZ ? 3 : 9);
    if (a == GX_VA_CLR0 || a == GX_VA_CLR1) return colorSize(v->type);
    if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) return compTypeSize(v->type) * (v->cnt == GX_TEX_ST ? 2 : 1);
    return 0;
}
static u32 gDlcSet[1 << 16]; static unsigned short gDlcGen[1 << 16], gDlcGenNow;
static void dlcOne(const unsigned char* dl, u32 nbytes) {
    u32 key = 2166136261u;
    key = dlcFnv(key, &dl, sizeof dl); key = dlcFnv(key, &nbytes, 4); key = dlcFnv(key, gDesc, sizeof gDesc);
    key = dlcFnv(key, &gForceStride, sizeof gForceStride);
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a)
        if (gDesc[a] == GX_INDEX8 || gDesc[a] == GX_INDEX16) { key = dlcFnv(key, &gArray[a].base, sizeof gArray[a].base); key = dlcFnv(key, &gArray[a].stride, 4); }
    DlcEnt* e = NULL; int isNew = 0;
    for (unsigned p = key & (DLC_N - 1), n = 0; n < DLC_N; ++n, p = (p + 1) & (DLC_N - 1)) {
        if (!gDlc[p].used) {
            if (gDlcN >= DLC_N * 3 / 4) break;
            e = &gDlc[p]; memset(e, 0, sizeof *e); e->used = 1; e->key = key; e->dl = dl; e->nbytes = nbytes; ++gDlcN; isNew = 1; break;
        }
        if (gDlc[p].key == key && gDlc[p].dl == dl && gDlc[p].nbytes == nbytes) { e = &gDlc[p]; break; }
    }
    if (!e) { ++gDlcDrops; return; }
    unsigned lim[GX_VA_MAX_ATTR];
    for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) lim[a] = gDesc[a] != GX_NONE ? idxLimit(&gArray[a]) : 0;
    u32 hDl = dlcFnv(2166136261u, dl, nbytes), hArr = 2166136261u, stripV = 0, tris = 0, uniq = 0;
    unsigned seenFmt = 0, i = 0; int maxSlot = 0, pn = gDesc[GX_VA_PNMTXIDX] != GX_NONE;
    if (isNew && !++gDlcGenNow) { memset(gDlcGen, 0, sizeof gDlcGen); gDlcGenNow = 1; }
    while (i < nbytes) {
        u8 op = dl[i++];
        if (op == 0x00) continue;
        u8 prim = op & 0xF8; GXVtxFmt fmt = (GXVtxFmt)(op & 0x07);
        if (!(prim==GX_QUADS||prim==GX_TRIANGLES||prim==GX_TRIANGLESTRIP||prim==GX_TRIANGLEFAN||prim==GX_LINES||prim==GX_LINESTRIP||prim==GX_POINTS)) { ++gDlcBadWalk; break; }
        if (i + 2 > nbytes) break;
        unsigned nv = (unsigned)dl[i] << 8 | dl[i + 1]; i += 2;
        int stride = gForceStride > 0 ? gForceStride : vertexStride(fmt);
        if (stride <= 0) { ++gDlcBadWalk; break; }
        if (!(seenFmt >> fmt & 1)) { seenFmt |= 1u << fmt; hDl = dlcFnv(hDl, gVat[fmt], sizeof gVat[fmt]); }
        unsigned done = 0;
        for (; done < nv && i + (unsigned)stride <= nbytes; ++done, i += (unsigned)stride) {
            const unsigned char* vp = dl + i; int c = 0;
            if (gForceStride > 0) {
                unsigned idx = gForcePosSz == 2 ? be16u(vp + gForcePosOff) : vp[gForcePosOff];
                int es = dlcElemSize(fmt, GX_VA_POS);
                if (idx < lim[GX_VA_POS] && es) hArr = dlcFnv(hArr, gArray[GX_VA_POS].base + idx * (unsigned)gArray[GX_VA_POS].stride, (unsigned)es);
            } else for (GXAttr a = 0; a < GX_VA_MAX_ATTR; ++a) {
                GXAttrType d = gDesc[a];
                if (d == GX_NONE) continue;
                if (a <= GX_VA_TEX7MTXIDX) { if (a == GX_VA_PNMTXIDX && vp[c] / 3 > maxSlot) maxSlot = vp[c] / 3; c += 1; continue; }
                if (d == GX_INDEX8 || d == GX_INDEX16) {
                    unsigned idx = d == GX_INDEX8 ? vp[c] : be16u(vp + c); c += d == GX_INDEX8 ? 1 : 2;
                    int es = dlcElemSize(fmt, a);
                    if (idx < lim[a] && es) hArr = dlcFnv(hArr, gArray[a].base + idx * (unsigned)gArray[a].stride, (unsigned)es);
                } else c += attrSize(fmt, a);
            }
            if (isNew && uniq < 49152u) {   // distinct vertices = distinct raw vertex records (a hash set: collisions undercount)
                u32 h = dlcFnv(2166136261u ^ (u32)fmt, vp, (unsigned)stride) | 1u;
                for (unsigned p = h & 0xffff;; p = (p + 1) & 0xffff) {
                    if (gDlcGen[p] != gDlcGenNow) { gDlcGen[p] = gDlcGenNow; gDlcSet[p] = h; ++uniq; break; }
                    if (gDlcSet[p] == h) break;
                }
            }
        }
        stripV += done;
        tris += prim == GX_TRIANGLES ? done / 3 : prim == GX_QUADS ? done / 4 * 2
              : ((prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN) && done >= 3) ? done - 2 : 0;
        if (done < nv) break;
    }
    if (isNew) { e->hDl = hDl; e->hArr = hArr; e->uniqV = uniq; e->stripV = stripV; e->tris = tris; }
    else {
        if (hDl != e->hDl) { ++e->chgDl; e->hDl = hDl; e->stripV = stripV; e->tris = tris; }
        if (hArr != e->hArr) { ++e->chgArr; e->hArr = hArr; }
    }
    ++e->calls; e->pnmtx = (u8)pn; e->hint |= (u8)(gx_draw_static_hint != 0); e->forced = (u8)(gForceStride > 0);
    if (maxSlot > e->maxSlot) e->maxSlot = (u8)maxSlot;
}
static void dlcSelfTest(void) {   // the census must SEE a changed array and a changed list
    GXAttrType sd[GX_VA_MAX_ATTR]; memcpy(sd, gDesc, sizeof sd);
    VatAttr sv = gVat[0][GX_VA_POS]; ArrayDef sa = gArray[GX_VA_POS]; int sf = gForceStride; gForceStride = 0;
    static unsigned char arr[18], dl[6] = { GX_TRIANGLES, 0, 3, 0, 1, 2 };
    memset(gDesc, 0, sizeof gDesc); gDesc[GX_VA_POS] = GX_INDEX8;
    gVat[0][GX_VA_POS].cnt = GX_POS_XYZ; gVat[0][GX_VA_POS].type = GX_S16; gVat[0][GX_VA_POS].frac = 0;
    gArray[GX_VA_POS].base = arr; gArray[GX_VA_POS].stride = 6;
    const unsigned char *slo = gSrcLo, *shi = gSrcHi; gSrcLo = gSrcHi = NULL; ++gVtxGen;
    arr[0] = 1; arr[6] = 2; arr[12] = 3;
    dlcOne(dl, 6); dlcOne(dl, 6);
    DlcEnt* e = NULL; for (unsigned p = 0; p < DLC_N; ++p) if (gDlc[p].used) e = &gDlc[p];
    int same = e && e->calls == 2 && !e->chgArr && !e->chgDl && e->stripV == 3 && e->uniqV == 3 && e->tris == 1;
    arr[7] ^= 1; dlcOne(dl, 6); int a1 = e && e->chgArr == 1 && e->chgDl == 0;
    dl[5] = 1;   dlcOne(dl, 6); int d1 = e && e->chgDl == 1 && e->chgArr == 2;   /* index 2 -> 1 also changes what is referenced */
    printf("[dlc] self-test: unchanged %s, array change seen %s, list change seen %s\n", same ? "ok" : "FAIL", a1 ? "ok" : "FAIL", d1 ? "ok" : "FAIL");
    if (!(same && a1 && d1)) gDlcOn = 0;
    dl[5] = 2; memset(arr, 0, sizeof arr);
    memset(gDlc, 0, sizeof gDlc); gDlcN = gDlcDrops = gDlcBadWalk = 0;
    memcpy(gDesc, sd, sizeof sd); gVat[0][GX_VA_POS] = sv; gArray[GX_VA_POS] = sa; gForceStride = sf; gSrcLo = slo; gSrcHi = shi; ++gVtxGen;
    gxh_kdirty_all(&gx_hal);   // R3a C3: it wrote gDesc / gVat directly
}
static int dlcOn(void) {
    if (gDlcOn < 0) {
        const char* env = getenv("STFX_DL_CENSUS");   /* the host test's switch */
        FILE* f = fopen("ux0:data/stairfax/dl_census.txt", "r"); int c1 = f ? fgetc(f) : 0; gDlcOn = (c1 == 49 || env) ? 1 : 0; if (f) fclose(f);   /* the file must START WITH 1: a 0 turns it off without a delete */
        printf("[dlc] display-list census %s\n", gDlcOn ? "ON (dl_census.txt)" : "off");
        if (gDlcOn) dlcSelfTest();
    }
    return gDlcOn;
}
// Vertex-program shapes: what a vertex shader would be specialised on, per TEV draw.
#define DLC_VSK 256
#define DLC_PAIR 2048
static struct { u32 h, n; } gDlcVsk[DLC_VSK], gDlcPair[DLC_PAIR];
static unsigned gDlcVskN, gDlcVskDrops, gDlcPairN, gDlcPairDrops, gDlcMaxLights, gDlcMaxTex;
static void dlcVsKey(const TevKey* k, const u8* sc) {
    u8 b[64]; int n = 0, nl = 0;
    b[n++] = gDesc[GX_VA_PNMTXIDX] != GX_NONE; b[n++] = gDesc[GX_VA_NRM] != GX_NONE; b[n++] = gDesc[GX_VA_CLR0] != GX_NONE;
    b[n++] = gNumChans;
    for (int q = 0; q < 2; ++q) {
        const PxChan* ch = &gChan[q ? CH_ALPHA0 : CH_COLOR0];
        int lights = 0; for (int i = 0; i < 8; ++i) if (ch->lightMask >> i & 1) ++lights;
        int en = gNumChans && ch->enable;
        if (en && lights > nl) nl = lights;
        b[n++] = (u8)en; b[n++] = gNumChans ? ch->matSrc : 0; b[n++] = en ? ch->ambSrc : 0;
        b[n++] = en ? (u8)lights : 0; b[n++] = en ? ch->diffFn : 0; b[n++] = en ? ch->attnFn : 0;
    }
    b[n++] = k->numTex;
    for (int sl = 0; sl < k->numTex; ++sl) {
        u8 f, s, m, nm, pt; gx_tev_texgen(sc[sl], &f, &s, &m, &nm, &pt);
        b[n++] = f; b[n++] = s; b[n++] = tgMtx(m) != NULL; b[n++] = nm; b[n++] = (pt >= 64 && pt < 64 + 3 * 20);
    }
    if ((unsigned)nl > gDlcMaxLights) gDlcMaxLights = (unsigned)nl;
    if (k->numTex > gDlcMaxTex) gDlcMaxTex = k->numTex;
    u32 h = dlcFnv(2166136261u, b, (unsigned)n) | 1u;
    unsigned p, c;
    for (p = h % DLC_VSK, c = 0; c < DLC_VSK; ++c, p = (p + 1) % DLC_VSK) {
        if (gDlcVsk[p].h == h) { ++gDlcVsk[p].n; break; }
        if (!gDlcVsk[p].h) {
            gDlcVsk[p].h = h; gDlcVsk[p].n = 1;
            if (++gDlcVskN <= 96) {
                printf("[vsk] #%u %08x pn %d nrm %d clr %d chans %d | C en %d mat %d amb %d lights %d df %d af %d | A en %d mat %d amb %d lights %d df %d af %d | tg",
                       gDlcVskN, (unsigned)h, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                for (int sl = 0; sl < k->numTex; ++sl) printf(" f%d s%d m%d n%d p%d", b[17 + sl * 5], b[18 + sl * 5], b[19 + sl * 5], b[20 + sl * 5], b[21 + sl * 5]);
                printf("\n");
            }
            break;
        }
    }
    if (c == DLC_VSK) ++gDlcVskDrops;
    static TevKey lastKey; static u32 lastT; static int haveLast;
    u32 t = (haveLast && !memcmp(k, &lastKey, sizeof *k)) ? lastT : dlcFnv(2166136261u, k, sizeof *k);
    lastKey = *k; lastT = t; haveLast = 1;
    u32 ph = (h * 0x9E3779B9u) ^ t; if (!ph) ph = 1;
    for (p = ph % DLC_PAIR, c = 0; c < DLC_PAIR; ++c, p = (p + 1) % DLC_PAIR) {
        if (gDlcPair[p].h == ph) { ++gDlcPair[p].n; break; }
        if (!gDlcPair[p].h) { if (gDlcPairN >= DLC_PAIR * 3 / 4) { c = DLC_PAIR; break; } gDlcPair[p].h = ph; gDlcPair[p].n = 1; ++gDlcPairN; break; }
    }
    if (c == DLC_PAIR) ++gDlcPairDrops;
}
void gx_dlc_report(void) {   // vt_game.c's status block
    if (gDlcOn <= 0) return;
    unsigned lists2 = 0, stat = 0, mutD = 0, mutA = 0, evD = 0, evA = 0, pnL = 0, hintL = 0, forcedL = 0, maxSlot = 0, calls = 0;
    unsigned long long vAll = 0, vStatic = 0, vMutA = 0, vMutD = 0, uAll = 0, tAll = 0, vPn = 0, vHint = 0, vHintMut = 0, vOnce = 0;
    for (unsigned p = 0; p < DLC_N; ++p) {
        const DlcEnt* e = &gDlc[p]; if (!e->used) continue;
        unsigned long long v = (unsigned long long)e->calls * e->stripV;
        calls += e->calls; vAll += v; uAll += (unsigned long long)e->calls * e->uniqV; tAll += (unsigned long long)e->calls * e->tris;
        if (e->pnmtx) { ++pnL; vPn += v; }
        if (e->forced) ++forcedL;
        if (e->hint) { ++hintL; vHint += v; if (e->chgDl || e->chgArr) vHintMut += v; }
        if (e->maxSlot > maxSlot) maxSlot = e->maxSlot;
        if (e->calls < 2) { vOnce += v; continue; }
        ++lists2;
        if (e->chgDl) { ++mutD; evD += e->chgDl; vMutD += v; }
        if (e->chgArr) { ++mutA; evA += e->chgArr; vMutA += v; }
        if (!e->chgDl && !e->chgArr) { ++stat; vStatic += v; }
    }
    printf("[dlc] lists %u of %u slots (DROPPED calls %u, bad walks %u), calls %u | called 2+ times: %u, never changed %u, list bytes changed %u (%u events), arrays changed %u (%u events)\n",
           gDlcN, DLC_N * 3 / 4, gDlcDrops, gDlcBadWalk, calls, lists2, stat, mutD, evD, mutA, evA);
    printf("[dlc] call-weighted strip verts %llu: never-changed lists %llu, array-changed %llu, list-changed %llu, seen once %llu | distinct verts %llu, tris %llu\n",
           vAll, vStatic, vMutA, vMutD, vOnce, uAll, tAll);
    printf("[dlc] matrix-indexed lists %u (verts %llu, max slot %u) | static-hint lists %u (verts %llu, of which changed %llu) | force-layout lists %u | immediate GXBegin prims %u verts %u\n",
           pnL, vPn, maxSlot, hintL, vHint, vHintMut, forcedL, gx_imm_prims, gx_imm_verts);
    printf("[vsk] vertex shapes %u (drops %u), (vertex shape, TEV key) pairs %u (drops %u) | max lights on a channel %u, max tex slots %u\n",
           gDlcVskN, gDlcVskDrops, gDlcPairN, gDlcPairDrops, gDlcMaxLights, gDlcMaxTex);
    /* the eight heaviest lists that changed: who are they? */
    const DlcEnt* top[8] = { 0 };
    for (unsigned p = 0; p < DLC_N; ++p) {
        const DlcEnt* e = &gDlc[p]; if (!e->used || !(e->chgDl || e->chgArr)) continue;
        for (int q = 0; q < 8; ++q) {
            if (!top[q] || (unsigned long long)e->calls * e->stripV > (unsigned long long)top[q]->calls * top[q]->stripV) {
                for (int r = 7; r > q; --r) top[r] = top[r - 1];
                top[q] = e; break;
            }
        }
    }
    for (int q = 0; q < 8 && top[q]; ++q)
        printf("[dlc] changed #%d: list %p %u bytes, calls %u, strip verts %u (distinct %u), array changes %u, list changes %u, pnmtx %d hint %d forced %d\n",
               q, (void*)top[q]->dl, (unsigned)top[q]->nbytes, (unsigned)top[q]->calls, (unsigned)top[q]->stripV, (unsigned)top[q]->uniqV,
               (unsigned)top[q]->chgArr, (unsigned)top[q]->chgDl, top[q]->pnmtx, top[q]->hint, top[q]->forced);
}


// ---- GPU VERTEX PATH, host side (2026-10-08, docs/47 fast path stage 2) -----------------------------------------------
// gx_vs_build derives the VsKey (the SHAPE of the generated vertex program, tev_gen.c tev_gen_vertex_gx) and the VsUniforms
// (its NUMBERS) from the CURRENT GX state for one display-list record. It draws nothing and changes no state, and NOTHING
// ON THE DEVICE CALLS IT YET: the draw path (dlkCall) is untouched; the device integration is a separate task. It reports 0
// with a VSW_* reason (gx_vs_why, counted in gx_vs_refused) when the list must stay on the CPU path:
//   not real-skin / gPxActive off / light arm off (chan_oldguess.txt is NOT reproduced: while it is active the shim must
//   not take this path), an unusable record, a vertex with !posOk, a list where only SOME distinct vertices carry a normal,
//   a matrix slot above 9, more than 8 lights' worth of uniforms or 8 input attributes. The CPU path is the specification:
//   host/tests/vs_shader_test.cpp compares the program's output with dlkCall's, vertex by vertex.
// Canonical keys: what the program cannot read is zeroed (a disabled channel's ambient/lights, a normal nothing reads, ...),
// so two draws that differ only in numbers share one program. Black lights are dropped from the packed list (the CPU skips
// them: pxLitRGBA's `if (!L->color) continue`); a light list is then ascending light index.
// The caller sets tk->fogType exactly as dlkCall does (perspective fog only): the key only needs "clip w goes on".
unsigned gx_vs_refused[VSW_N];
static int gx_vs_why;
#define VS_FAIL(w) do { gx_vs_why = (w); ++gx_vs_refused[w]; return 0; } while (0)
static int vsPackLights(const PxChan* ch, int idx[8]) {
    int n = 0;
    for (int i = 0; i < 8; ++i) if ((ch->lightMask & (1u << i)) && gx_light_regs[i].color) idx[n++] = i;
    return n;
}
static void vsPutLight(VsLight* o, const PxLightRec* L) {
    for (int q = 0; q < 3; ++q) { o->pos[q] = L->lpos[q]; o->dir[q] = L->ldir[q]; o->a[q] = L->a[q]; o->k[q] = L->k[q]; }
    o->col[0] = (float)(L->color >> 24); o->col[1] = (float)((L->color >> 16) & 0xff); o->col[2] = (float)((L->color >> 8) & 0xff); o->col[3] = (float)(L->color & 0xff);
}
/* R3c C5: gx_vs_build_ref is split in two so the program-clean draw can redo only the second half.  vsBuildKey is everything up to and including tev_vs_check (the VS_FAIL refusals and the VsKey);
 * vsBuildUniforms is the numbers (VsUniforms) and the texgen counters.  gx_vs_build_ref (below) is their composition, so every existing caller and test is unchanged. */
static int vsBuildKey(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k) {   /* k is zeroed by the caller */
    if (!gRealSkin) VS_FAIL(VSW_NOT_REAL_SKIN);
    if (!gPxActive) VS_FAIL(VSW_NO_PX);
    if (!gStfxLightArm) VS_FAIL(VSW_LIGHT_ARM_OFF);
    if (r->state != 1) VS_FAIL(VSW_UNCACHEABLE);
    if (tk->numTex > TEVK_TEX) VS_FAIL(VSW_TOO_MANY_TEX);
    const u32 nd = r->ndist; u32 nN = 0, nB = 0;
/* GXAOT_HOOK_BEGIN C3 */
    const int c3v = c3VsbOn();
    if (c3v) { DlkRec* rw = (DlkRec*)r; ++gx_c3_vsb_calls; if (!rw->vsScan) { c3VsScan(rw); ++gx_c3_vsb_inval; } else ++gx_c3_vsb_hits;
               if (rw->vsWhy) VS_FAIL(rw->vsWhy); nN = rw->vsNN; nB = rw->vsNB; }
    else {
/* GXAOT_HOOK_END C3 */
    for (u32 d = 0; d < nd; ++d) { if (!(r->fl[d] & 1)) VS_FAIL(VSW_POS_NOT_OK); nN += (r->fl[d] >> 1) & 1; nB += (r->fl[d] >> 3) & 1; }
    if (nN && nN != nd) VS_FAIL(VSW_MIXED_NRM);
    }   /* GXAOT_HOOK C3 arm 102: the original scans above run unchanged outside the memo */
    const int haveNrm = nd && nN == nd;
    if (c3v) goto c3_after_pnm;   /* GXAOT_HOOK C3 arm 102: the memo's vsWhy already covers the slot range */
    if (r->pnm) for (u32 d = 0; d < nd; ++d) if (r->pnm[d] / 3 > VS_MAX_SLOTS - 1) VS_FAIL(VSW_SLOT_RANGE);
    c3_after_pnm: ;   /* GXAOT_HOOK C3 */
    k->hasMtxIdx = r->hasPnm ? 1 : 0;
    k->numTex = tk->numTex; k->fog = tk->fogType ? 1 : 0;
    // ---- the colour / alpha channels -------------------------------------------------------------------------------
    int lidx[2][8], nl[2] = { 0, 0 };
    k->numChans = gNumChans ? 1 : 0;
    if (k->numChans) {
        const PxChan* pc[2] = { &gChan[CH_COLOR0], &gChan[CH_ALPHA0] };
        for (int c = 0; c < 2; ++c) {
            const PxChan* ch = pc[c]; VsChanKey* o = &k->ch[c];
            o->enable = ch->enable ? 1 : 0;
            o->matSrc = (ch->matSrc == 1 && r->hasClr) ? 1 : 0;     /* pxChannelColour: fromVtx = matSrc == GX_SRC_VTX && haveVtx */
            if (!o->enable) continue;
            o->ambSrc = (ch->ambSrc == 1 && r->hasClr) ? 1 : 0;
            o->attnFn = ch->attnFn == 0 ? 0 : ch->attnFn == 1 ? 1 : 2;
            o->diffFn = o->attnFn == 0 ? 0 : ch->diffFn == 0 ? 0 : ch->diffFn == 1 ? 1 : 2;   /* spec forces diffuse none; any other value clamps */
            nl[c] = vsPackLights(ch, lidx[c]); o->nLights = (uint8_t)nl[c];
        }
        if (k->ch[0].enable && k->ch[1].enable && k->ch[0].attnFn == k->ch[1].attnFn && k->ch[0].diffFn == k->ch[1].diffFn &&
            nl[0] == nl[1] && !memcmp(lidx[0], lidx[1], (size_t)nl[0] * sizeof(int))) {
            k->shareAlpha = 1; k->ch[1].nLights = 0;               /* same lighting: alpha takes the colour channel's accumulated alpha */
        }
        if ((k->ch[0].matSrc || k->ch[1].matSrc || (k->ch[0].enable && k->ch[0].ambSrc) || (k->ch[1].enable && k->ch[1].ambSrc))) k->hasClr0 = 1;
    }
    const int anyLights = k->ch[0].nLights + k->ch[1].nLights > 0;
    // ---- texgens ---------------------------------------------------------------------------------------------------
    int texNrm = 0;
    for (int sl = 0; sl < tk->numTex; ++sl) {
        u8 f, s, m, nm, pt; gx_tev_texgen(sc[sl], &f, &s, &m, &nm, &pt);
        VsTexKey* g = &k->tex[sl];
        if (!((s == 0 || s == 1 || ((s == 2 || s == 3) && !tmiOld()) || (s >= 4 && s <= 11)) && f <= 1)) continue;   /* texgenVertex: unsupported -> (0,0) */
        g->kind = 1; g->func = f;
        /* 2026-10-13: the vertex names this texgen's matrix (TEXnMTXIDX): the 10 texture matrices go up as a palette (uTMX) and the program picks the row */
        const int tmiS = sc[sl] < 8 && r->tmiIdx[sc[sl]] >= 0 && !tmiOld();
        if (tmiS) {
            if (r->tmiBad >> sc[sl] & 1) VS_FAIL(VSW_TMI_RANGE);   /* a row outside 30,33..57 (a position matrix, the identity, a stray byte): the CPU path takes it */
            k->tmiSlots |= (uint8_t)(1u << sl); k->tmiCoord[sl] = sc[sl];
        } else g->hasTM = tgMtx(m) ? 1 : 0;
        g->hasPost = (pt >= 64 && pt < 64 + 3 * 20) ? 1 : 0; g->norm = (g->hasPost && nm) ? 1 : 0;
        if (s == 0) g->src = 0;
        else if (s == 1) { g->src = 1; texNrm = 1; }
        else if (s == 2 || s == 3) {   /* binormal / tangent: the list's NBT vectors, or Dolphin's (0,0,1) when it has none */
            if (!r->nbt) g->src = 3;
            else { if (nB != nd) VS_FAIL(VSW_MIXED_NBT); g->src = s == 2 ? 4 : 5; if (s == 2) k->hasBin = 1; else k->hasTan = 1; }
        }
        else if (r->texIdx[s - 4] >= 0) { g->src = 2; g->texN = (uint8_t)(s - 4); k->texMask |= (uint8_t)(1u << (s - 4)); }
        else g->src = 3;                                                          /* a TEXn the list does not carry: (0,0,1,1) */
    }
    k->hasNrm = (haveNrm && (anyLights || texNrm)) ? 1 : 0;
    { int why = tev_vs_check(k); if (why) VS_FAIL(why); }
    return 1;
}
/* The numbers of a VS draw: a pure function of the VsKey (what to write), the TEV key's numTex, sc[] (the texcoord of each slot) and the HAL kinds POS / NRM / TEXM / POSTM / PROJ / CHAN (mat, amb) / LIGHT /
 * PIX (curMtx), plus the caller's mvp (NULL = from PROJ and the viewport depth range).  It writes only the regions the key names, so a caller that proves the key unchanged since the last build of the SAME
 * buffer may skip the 3.7 KB clear (R3a C4, R3c C5).  What the key phase derived locally (the packed light list, the matrix numbers of the texgens) is re-derived here from the key and the state: both are
 * pure functions of them (the light list from the channel's mask and the lights' colours; the texgen numbers from sc[] and the texgen table, the matrix palette flag from k->tmiSlots). */
static void vsBuildUniforms(const VsKey* k, const TevKey* tk, const u8 sc[TEVK_TEX], const float* mvpIn, VsUniforms* u) {
    int lidx[2][8];
    if (k->numChans) {
        const PxChan* pc[2] = { &gChan[CH_COLOR0], &gChan[CH_ALPHA0] };
        for (int c = 0; c < 2; ++c) if (k->ch[c].nLights) (void) vsPackLights(pc[c], lidx[c]);
    }
    float mvpL[16];
    if (!mvpIn) {
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) mvpL[i*4+j] = gProj[i][j];
        gxClipZ(mvpL);   /* as dlkCall's mvp: the Z remap (and the viewport's depth range) lives in the uniform */
        mvpIn = mvpL;
    }
    memcpy(u->proj, mvpIn, sizeof mvpL);
    if (k->hasMtxIdx) { for (int s = 0; s < VS_MAX_SLOTS; ++s) { memcpy(u->pm[s], gPosMtx[s], sizeof u->pm[s]); memcpy(u->nm[s], gNrmMtx[s], sizeof u->nm[s]); } }
    else { memcpy(u->pm[0], gPosMtx[gCurMtx], sizeof u->pm[0]); memcpy(u->nm[0], gNrmMtx[gCurMtx], sizeof u->nm[0]); }
    for (int q = 0; q < 4; ++q) { u->mat[q] = (float)gMat[0][q]; u->amb[q] = (float)gAmb[0][q]; }
    for (int c = 0; c < 2; ++c) for (int i = 0; i < k->ch[c].nLights; ++i) vsPutLight(&u->lt[c][i], &gx_light_regs[lidx[c][i]]);
    u8 tmId[TEVK_TEX], ptId[TEVK_TEX], tmPn[TEVK_TEX];
    for (int sl = 0; sl < tk->numTex; ++sl) {
        u8 f, s, m, nm, pt; gx_tev_texgen(sc[sl], &f, &s, &m, &nm, &pt); (void) f; (void) s; (void) nm;
        const VsTexKey* g = &k->tex[sl]; const int tmiS = (k->tmiSlots >> sl) & 1;
        tmId[sl] = ptId[sl] = tmPn[sl] = 0;
        if (!g->kind) continue;   /* the texgen the key could not express: nothing is written for it */
        if (g->hasTM) tmId[sl] = (u8)(m < 30 ? m / 3 : (m - 30) / 3);   /* tmSrc[sl] says which table: position matrices below 30 */
        tmPn[sl] = (u8)(!tmiS && tgIsPn(m));
        if (g->hasPost) ptId[sl] = (u8)((pt - 64) / 3);
    }
    for (int sl = 0; sl < tk->numTex; ++sl) {
        if (k->tex[sl].hasTM) memcpy(u->tm[sl],
#ifdef BREAK_VS_TGPN   /* host negative control: the uniform is filled from the TEXTURE matrices whatever the texgen names (vs_shader_test must fail) */
            (const void*)gTexMtx[tmId[sl] % 10],
#else
            tmPn[sl] ? (const void*)gPosMtx[tmId[sl]] : (const void*)gTexMtx[tmId[sl]],
#endif
            sizeof u->tm[sl]);
        if (k->tex[sl].hasPost) memcpy(u->pt[sl], gPostMtx[ptId[sl]], sizeof u->pt[sl]);
    }
    for (int sl = 0; sl < tk->numTex; ++sl) if (k->tex[sl].hasTM && tmPn[sl]) { ++gx_texgen_pn_gpu; break; }
    if (k->tmiSlots) { memcpy(u->tmx, gTexMtx, sizeof u->tmx); ++gx_texgen_tmi_gpu; }
    if (k->hasBin || k->hasTan) ++gx_texgen_nbt;
}
static int gx_vs_build_ref(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvpIn, const VsKey* prevKey) {
    /* prevKey (R3a C4): the VsKey that the SAME buffer's last successful build produced.  The uniform struct is written only where the key says (pm / nm slots, tm / pt rows, lights, tmx), so
     * when the new key equals it the unwritten bytes are already zero and the 3.7 KB memset can go.  NULL = the old behaviour, exactly (memset first, also on a refusal). */
    memset(k, 0, sizeof *k); if (!prevKey) memset(u, 0, sizeof *u);
    if (!vsBuildKey(r, tk, sc, k)) return 0;
    if (prevKey && memcmp(prevKey, k, sizeof *k)) memset(u, 0, sizeof *u);   /* R3a C4: a changed key re-zeroes the uniforms; an unchanged one keeps its zero regions */
    vsBuildUniforms(k, tk, sc, mvpIn, u);
    return 1;
}
#ifdef GX_C5_SELFCHECK
/* R3c C5: the monolithic gx_vs_build_ref as it was before the split, kept ONLY for the host self-check (c5Selfcheck compares the split against it, byte for byte, on every call it makes). */
#pragma push_macro("VS_FAIL")
#undef VS_FAIL
#define VS_FAIL(w) return 0   /* the self-check reference neither counts nor records a reason */
static int gx_vs_build_mono(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvpIn, const VsKey* prevKey) {
    /* prevKey (R3a C4): the VsKey that the SAME buffer's last successful build produced.  The uniform struct is written only where the key says (pm / nm slots, tm / pt rows, lights, tmx), so
     * when the new key equals it the unwritten bytes are already zero and the 3.7 KB memset can go.  NULL = the old behaviour, exactly (memset first, also on a refusal). */
    memset(k, 0, sizeof *k); if (!prevKey) memset(u, 0, sizeof *u);
    if (!gRealSkin) VS_FAIL(VSW_NOT_REAL_SKIN);
    if (!gPxActive) VS_FAIL(VSW_NO_PX);
    if (!gStfxLightArm) VS_FAIL(VSW_LIGHT_ARM_OFF);
    if (r->state != 1) VS_FAIL(VSW_UNCACHEABLE);
    if (tk->numTex > TEVK_TEX) VS_FAIL(VSW_TOO_MANY_TEX);
    const u32 nd = r->ndist; u32 nN = 0, nB = 0;
    for (u32 d = 0; d < nd; ++d) { if (!(r->fl[d] & 1)) VS_FAIL(VSW_POS_NOT_OK); nN += (r->fl[d] >> 1) & 1; nB += (r->fl[d] >> 3) & 1; }
    if (nN && nN != nd) VS_FAIL(VSW_MIXED_NRM);
    const int haveNrm = nd && nN == nd;
    if (r->pnm) for (u32 d = 0; d < nd; ++d) if (r->pnm[d] / 3 > VS_MAX_SLOTS - 1) VS_FAIL(VSW_SLOT_RANGE);
    k->hasMtxIdx = r->hasPnm ? 1 : 0;
    k->numTex = tk->numTex; k->fog = tk->fogType ? 1 : 0;
    // ---- the colour / alpha channels -------------------------------------------------------------------------------
    int lidx[2][8], nl[2] = { 0, 0 };
    k->numChans = gNumChans ? 1 : 0;
    if (k->numChans) {
        const PxChan* pc[2] = { &gChan[CH_COLOR0], &gChan[CH_ALPHA0] };
        for (int c = 0; c < 2; ++c) {
            const PxChan* ch = pc[c]; VsChanKey* o = &k->ch[c];
            o->enable = ch->enable ? 1 : 0;
            o->matSrc = (ch->matSrc == 1 && r->hasClr) ? 1 : 0;     /* pxChannelColour: fromVtx = matSrc == GX_SRC_VTX && haveVtx */
            if (!o->enable) continue;
            o->ambSrc = (ch->ambSrc == 1 && r->hasClr) ? 1 : 0;
            o->attnFn = ch->attnFn == 0 ? 0 : ch->attnFn == 1 ? 1 : 2;
            o->diffFn = o->attnFn == 0 ? 0 : ch->diffFn == 0 ? 0 : ch->diffFn == 1 ? 1 : 2;   /* spec forces diffuse none; any other value clamps */
            nl[c] = vsPackLights(ch, lidx[c]); o->nLights = (uint8_t)nl[c];
        }
        if (k->ch[0].enable && k->ch[1].enable && k->ch[0].attnFn == k->ch[1].attnFn && k->ch[0].diffFn == k->ch[1].diffFn &&
            nl[0] == nl[1] && !memcmp(lidx[0], lidx[1], (size_t)nl[0] * sizeof(int))) {
            k->shareAlpha = 1; k->ch[1].nLights = 0;               /* same lighting: alpha takes the colour channel's accumulated alpha */
        }
        if ((k->ch[0].matSrc || k->ch[1].matSrc || (k->ch[0].enable && k->ch[0].ambSrc) || (k->ch[1].enable && k->ch[1].ambSrc))) k->hasClr0 = 1;
    }
    const int anyLights = k->ch[0].nLights + k->ch[1].nLights > 0;
    // ---- texgens ---------------------------------------------------------------------------------------------------
    int texNrm = 0; u8 tmId[TEVK_TEX], ptId[TEVK_TEX], tmPn[TEVK_TEX];
    for (int sl = 0; sl < tk->numTex; ++sl) {
        u8 f, s, m, nm, pt; gx_tev_texgen(sc[sl], &f, &s, &m, &nm, &pt);
        VsTexKey* g = &k->tex[sl]; tmId[sl] = ptId[sl] = tmPn[sl] = 0;
        if (!((s == 0 || s == 1 || ((s == 2 || s == 3) && !tmiOld()) || (s >= 4 && s <= 11)) && f <= 1)) continue;   /* texgenVertex: unsupported -> (0,0) */
        g->kind = 1; g->func = f;
        /* 2026-10-13: the vertex names this texgen's matrix (TEXnMTXIDX): the 10 texture matrices go up as a palette (uTMX) and the program picks the row */
        const int tmiS = sc[sl] < 8 && r->tmiIdx[sc[sl]] >= 0 && !tmiOld();
        if (tmiS) {
            if (r->tmiBad >> sc[sl] & 1) VS_FAIL(VSW_TMI_RANGE);   /* a row outside 30,33..57 (a position matrix, the identity, a stray byte): the CPU path takes it */
            k->tmiSlots |= (uint8_t)(1u << sl); k->tmiCoord[sl] = sc[sl];
        } else g->hasTM = tgMtx(m) ? 1 : 0;
        g->hasPost = (pt >= 64 && pt < 64 + 3 * 20) ? 1 : 0; g->norm = (g->hasPost && nm) ? 1 : 0;
        if (g->hasTM) tmId[sl] = (u8)(m < 30 ? m / 3 : (m - 30) / 3);   /* tmSrc[sl] says which table: position matrices below 30 */
        tmPn[sl] = (u8)(!tmiS && tgIsPn(m));
        if (g->hasPost) ptId[sl] = (u8)((pt - 64) / 3);
        if (s == 0) g->src = 0;
        else if (s == 1) { g->src = 1; texNrm = 1; }
        else if (s == 2 || s == 3) {   /* binormal / tangent: the list's NBT vectors, or Dolphin's (0,0,1) when it has none */
            if (!r->nbt) g->src = 3;
            else { if (nB != nd) VS_FAIL(VSW_MIXED_NBT); g->src = s == 2 ? 4 : 5; if (s == 2) k->hasBin = 1; else k->hasTan = 1; }
        }
        else if (r->texIdx[s - 4] >= 0) { g->src = 2; g->texN = (uint8_t)(s - 4); k->texMask |= (uint8_t)(1u << (s - 4)); }
        else g->src = 3;                                                          /* a TEXn the list does not carry: (0,0,1,1) */
    }
    k->hasNrm = (haveNrm && (anyLights || texNrm)) ? 1 : 0;
    { int why = tev_vs_check(k); if (why) VS_FAIL(why); }
    if (prevKey && memcmp(prevKey, k, sizeof(*k))) memset(u, 0, sizeof(*u));   /* R3a C4: a changed key re-zeroes the uniforms; an unchanged one keeps its zero regions */
    // ---- uniforms ---------------------------------------------------------------------------------------------------
    float mvpL[16];
    if (!mvpIn) {
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) mvpL[i*4+j] = gProj[i][j];
        gxClipZ(mvpL);   /* as dlkCall's mvp: the Z remap (and the viewport's depth range) lives in the uniform */
        mvpIn = mvpL;
    }
    memcpy(u->proj, mvpIn, sizeof mvpL);
    if (k->hasMtxIdx) { for (int s = 0; s < VS_MAX_SLOTS; ++s) { memcpy(u->pm[s], gPosMtx[s], sizeof u->pm[s]); memcpy(u->nm[s], gNrmMtx[s], sizeof u->nm[s]); } }
    else { memcpy(u->pm[0], gPosMtx[gCurMtx], sizeof u->pm[0]); memcpy(u->nm[0], gNrmMtx[gCurMtx], sizeof u->nm[0]); }
    for (int q = 0; q < 4; ++q) { u->mat[q] = (float)gMat[0][q]; u->amb[q] = (float)gAmb[0][q]; }
    for (int c = 0; c < 2; ++c) for (int i = 0; i < k->ch[c].nLights; ++i) vsPutLight(&u->lt[c][i], &gx_light_regs[lidx[c][i]]);
    for (int sl = 0; sl < tk->numTex; ++sl) {
        if (k->tex[sl].hasTM) memcpy(u->tm[sl],
#ifdef BREAK_VS_TGPN   /* host negative control: the uniform is filled from the TEXTURE matrices whatever the texgen names (vs_shader_test must fail) */
            (const void*)gTexMtx[tmId[sl] % 10],
#else
            tmPn[sl] ? (const void*)gPosMtx[tmId[sl]] : (const void*)gTexMtx[tmId[sl]],
#endif
            sizeof u->tm[sl]);
        if (k->tex[sl].hasPost) memcpy(u->pt[sl], gPostMtx[ptId[sl]], sizeof u->pt[sl]);
    }
    for (int sl = 0; sl < tk->numTex; ++sl) if (k->tex[sl].hasTM && tmPn[sl]) { break; }
    if (k->tmiSlots) { memcpy(u->tmx, gTexMtx, sizeof u->tmx); }
    return 1;
}

#pragma pop_macro("VS_FAIL")
#endif

/* ---- R3a C3 (host/engine/R3_DRAW.md s2): THE MEMO OF gx_vs_build ------------------------------------------------------------------------------------------------------------------------
 * The result (VsKey + VsUniforms) is a pure function of the inputs below; it is left IN THE CALLER'S BUFFERS, so a hit costs no copy.  THE GENERATION TABLE -- every input, and how the memo keys on it:
 *   HAL kinds (generation counters, gx_hal_gen[]):
 *     PIX     gPxActive, gNumChans, gCurMtx (px)                  CHAN    gChan / gMat / gAmb (the channels, their lights masks, mat / amb colours)   LIGHT   gx_light_regs (colour != 0 packs a light)
 *     GEN     gTev.gen[] (gx_tev_texgen: func / src / mtx / norm / post per texcoord)                       TEXM    gTexMtx   POSTM   gPostMtx   POS   gPosMtx   NRM   gNrmMtx
 *     PROJ    gProj (the projection; the viewport range below completes the clip-z'd matrix)
 *   non-struct inputs, compared BY VALUE every call: gRealSkin, gStfxLightArm, tmiOld(), gVpZn, gVpZf, vpDepthOff(), gStfxGxZToGL (gxClipZ), tk->numTex, tk->fogType != 0, sc[0..numTex) (the texcoord of
 *     each slot), and the caller's mvp when it passes one (C4; it is derived from PROJ + the viewport range, so it can only differ when one of them did -- compared anyway: 64 bytes).
 *   the LIST (DlkRec), by value, as a shape: state, the scan results (vsWhy, "every distinct vertex has a normal", "every one has an NBT"), hasPnm, hasClr, nbt != NULL, texIdx[8], tmiIdx[8], tmiBad.
 *     Everything else a record holds (positions, indices, ...) is not read here.  A record rebuilt in place changes nothing the shape does not see.
 *   output identity: the memo remembers WHICH buffers hold the result (an external memset / reuse of the buffers can not be seen), so only the callers that own static buffers use it (dlkCall).
 * Only a SUCCESSFUL build is remembered (a refusal counts its reason on every call, as today).  C4 rides on the miss path: when the new VsKey equals the remembered one the uniform struct keeps its zero
 * regions (gx_vs_build_ref prevKey).  The key SERIAL is a cheap identity of the VsKey bytes for the program lookup (rhi_gxm_shader.c hint): it moves only when a rebuilt key differs. */
/* VsShape: typedef above dlkCall (R3c C5) */
typedef struct {
    int valid;
    const VsKey* kp; const VsUniforms* up;
    uint32_t gen[9];
    int realSkin, lightArm, tmiOld, zToGL, vpOff; float vpZn, vpZf;
    u8 numTex, fog; u8 sc[TEVK_TEX];
    int haveMvp; float mvp[16];
    VsShape shape;
    VsKey kcopy;
    u8 dPn, dTmi, dNbt;      /* what a full build adds to gx_texgen_pn_gpu / gx_texgen_tmi_gpu / gx_texgen_nbt: replayed on a hit */
} VsMemo;
static VsMemo gVm;
unsigned gx_vs_memo_hits, gx_vs_memo_miss;
static const uint8_t kVsMemoKinds[9] = { GXH_K_PIX, GXH_K_CHAN, GXH_K_LIGHT, GXH_K_GEN, GXH_K_TEXM, GXH_K_POSTM, GXH_K_POS, GXH_K_NRM, GXH_K_PROJ };
static int vsShapeOf(const DlkRec* r, VsShape* s) {   /* 0 = the list is not eligible (the full build will refuse it, or it is not usable) */
    DlkRec* rw = (DlkRec*)r;
    memset(s, 0, sizeof *s);
    if (r->state != 1) return 0;
    if (!rw->vsScan) c3VsScan(rw);                       /* a pure function of the record (the same scan arm 102 keeps); done here for every arm */
    if (rw->vsWhy) return 0;
    s->state = r->state; s->why = rw->vsWhy; s->haveNrm = r->ndist && rw->vsNN == r->ndist; s->nbAll = rw->vsNB == r->ndist;
    s->hasPnm = r->hasPnm; s->hasClr = r->hasClr; s->hasNbt = r->nbt != NULL; s->tmiBad = r->tmiBad;
    memcpy(s->texIdx, r->texIdx, 8); memcpy(s->tmiIdx, r->tmiIdx, 8);
    return 1;
}
static int gx_vs_build(const DlkRec* r, const TevKey* tk, const u8 sc[TEVK_TEX], VsKey* k, VsUniforms* u, const float* mvp) {
    if (!(gx_r3a & GX_R3A_C3)) { gVm.valid = 0; return gx_vs_build_ref(r, tk, sc, k, u, mvp, NULL); }
    gxh_collect();
    VsShape shape; const int eligible = vsShapeOf(r, &shape);
    if (eligible && tk->numTex <= TEVK_TEX) {
        VsMemo* m = &gVm;
        if (m->valid && m->kp == k && m->up == u
            && m->realSkin == gRealSkin && m->lightArm == gStfxLightArm && m->tmiOld == tmiOld() && m->zToGL == gStfxGxZToGL && m->vpOff == vpDepthOff() && m->vpZn == gVpZn && m->vpZf == gVpZf
            && m->numTex == tk->numTex && m->fog == (tk->fogType ? 1 : 0) && !memcmp(m->sc, sc, tk->numTex) && m->haveMvp == (mvp != NULL) && (!mvp || !memcmp(m->mvp, mvp, sizeof m->mvp))
            && !memcmp(&m->shape, &shape, sizeof shape)) {
            int same = 1;
            for (int i = 0; i < 9; ++i) same &= m->gen[i] == gx_hal_gen[kVsMemoKinds[i]];
            if (same) {
                if (c3VsbOn()) { ++gx_c3_vsb_calls; ++gx_c3_vsb_hits; }
                gx_texgen_pn_gpu += m->dPn; gx_texgen_tmi_gpu += m->dTmi; gx_texgen_nbt += m->dNbt;
                ++gx_vs_memo_hits;
                return 1;
            }
        }
        ++gx_vs_memo_miss;
        const unsigned pn0 = gx_texgen_pn_gpu, tmi0 = gx_texgen_tmi_gpu, nbt0 = gx_texgen_nbt;
        const VsKey* prev = (m->valid && m->kp == k && m->up == u && (gx_r3a & GX_R3A_C4)) ? &m->kcopy : NULL;
        const int ok = gx_vs_build_ref(r, tk, sc, k, u, mvp, prev);
        if (!ok) { m->valid = 0; return 0; }
        if (!m->valid || memcmp(&m->kcopy, k, sizeof *k)) ++gx_vs_key_serial;
        m->valid = 1; m->kp = k; m->up = u;
        for (int i = 0; i < 9; ++i) m->gen[i] = gx_hal_gen[kVsMemoKinds[i]];
        m->realSkin = gRealSkin; m->lightArm = gStfxLightArm; m->tmiOld = tmiOld(); m->zToGL = gStfxGxZToGL; m->vpOff = vpDepthOff(); m->vpZn = gVpZn; m->vpZf = gVpZf;
        m->numTex = tk->numTex; m->fog = tk->fogType ? 1 : 0; memset(m->sc, 0, sizeof m->sc); memcpy(m->sc, sc, tk->numTex);
        m->haveMvp = mvp != NULL; if (mvp) memcpy(m->mvp, mvp, sizeof m->mvp);
        m->shape = shape; m->kcopy = *k;
        m->dPn = (u8)(gx_texgen_pn_gpu - pn0); m->dTmi = (u8)(gx_texgen_tmi_gpu - tmi0); m->dNbt = (u8)(gx_texgen_nbt - nbt0);
        return 1;
    }
    gVm.valid = 0;
    return gx_vs_build_ref(r, tk, sc, k, u, mvp, NULL);
}
#ifdef GX_C3_SELFCHECK
/* R3a T2: the memos against the plain builds, on every cached display-list call of a host test.  Mismatches are counted (gx_c3_chk_bad) and the first few are described on stderr. */
extern int gx_tev_build_refcall(TevKey* k, TevUniforms* u, u8 slotCoord[TEVK_TEX], u8 slotMap[TEVK_TEX], int acComp0, int acOp, int acComp1, int acRef0, int acRef1);
static void c3Bad(const char* what) { if (gx_c3_chk_bad++ < 6) fprintf(stderr, "FAIL: C3 memo != reference: %s (check %u tev / %u vs)\n", what, gx_c3_chk_tev, gx_c3_chk_vs); }
static void c3Selfcheck_tev(const TevKey* k, const TevUniforms* u, const u8* sc, const u8* sm, int bld) {
    TevKey rk; TevUniforms ru; u8 rsc[TEVK_TEX], rsm[TEVK_TEX];
    memset(rsc, 0, sizeof rsc); memset(rsm, 0, sizeof rsm);
    const int rb = gx_tev_build_refcall(&rk, &ru, rsc, rsm, gAcComp0, gAcOp, gAcComp1, gAcRef0, gAcRef1);
    ++gx_c3_chk_tev;
    if (rb != bld) { c3Bad("gx_tev_build return"); return; }
    if (!rb) return;
    if (memcmp(k, &rk, sizeof rk)) c3Bad("TevKey bytes");
    if (memcmp(u, &ru, sizeof ru)) c3Bad("TevUniforms bytes");
    if (memcmp(sc, rsc, k->numTex) || memcmp(sm, rsm, k->numTex)) c3Bad("slot coord / map");   /* the build writes the slots the key uses, no more */
}
static void c3Selfcheck_vs(const DlkRec* r, const TevKey* tkIn, const u8* sc, const float* mvp, int fogWhy) {
    static VsKey mk; static VsUniforms mu;   /* the memoised build's own buffers: stable, like dlkCall's statics */
    static VsKey lastVk, lastRk; static TevKey lastTk; static unsigned long long lastHint; static int haveLast;
    VsKey rk; VsUniforms ru;
    (void) fogWhy;
    if (r->state != 1 || tkIn->numTex > TEVK_TEX) return;
    const unsigned r3a0 = gx_r3a;
    const int mb = gx_vs_build(r, tkIn, sc, &mk, &mu, mvp);                 /* as dlkCall (the memoised path when C3 is on) */
    const int rb = gx_vs_build_ref(r, tkIn, sc, &rk, &ru, mvp, NULL);       /* the plain build */
    (void) r3a0;
    ++gx_c3_chk_vs;
    if (mb != rb) { c3Bad("gx_vs_build return"); haveLast = 0; return; }
    if (!rb) { haveLast = 0; return; }
    ++gx_c3_chk_vs_ok;
    if (memcmp(&mk, &rk, sizeof rk)) c3Bad("VsKey bytes");
    if (memcmp(&mu, &ru, sizeof ru)) c3Bad("VsUniforms bytes");
    /* the program hint contract: equal hints between two consecutive draws mean equal raw keys (gx_prog_hint as dlkCall builds it) */
    {   const unsigned long long hint = (gx_r3a & GX_R3A_C3) ? (1ull << 63) | ((unsigned long long)(gx_tev_key_serial & 0xFFFFFFu) << 39) | ((unsigned long long)(gx_vs_key_serial & 0x7FFFFFFFu) << 8) | tkIn->fogType : 0ull;
        ++gx_c3_chk_hint;
        if (hint && haveLast && hint == lastHint) { ++gx_c3_chk_hint_eq; if (memcmp(&lastVk, &mk, sizeof mk) || memcmp(&lastTk, tkIn, sizeof *tkIn)) c3Bad("equal program hints, different raw keys"); }
        lastHint = hint; lastVk = mk; lastTk = *tkIn; haveLast = 1; (void) lastRk;
    }
}
#endif
// HOST-TEST HOOK (host/tests/vs_emit.c; precedents: gx_draw_lastVerts, gx_draw_litColour). After a GXCallDisplayList on the
// cached arm: finds the list's record, rebuilds the TEV key + fog exactly as dlkCall does, runs gx_vs_build on the current
// state, and hands back the record's DISTINCT RAW vertices and the triangle-corner index list in dlkExpand's order, so a
// test can run the generated program on them and compare with what the CPU path handed the backend.
int gx_draw_vsProbe(const unsigned char* dl, unsigned nbytes, GxVsProbe* o) {
    static VsRawVertex* sV; static size_t sVcap; static int* sI; static size_t sIcap;
    memset(o, 0, sizeof *o);
    DlkKey kn; dlkKeyNow(&kn);   /* the set is chosen by (list, KEY): run 134 had 2,577 evictions for 2,987 builds with the list alone -- one list under more than four keys */
    u32 hs = ((u32)((size_t)dl >> 4) * 2654435761u ^ nbytes * 40503u) + (u32)dlkHash(0x51ED270B0F3D4C1Full, &kn, sizeof kn) * 0x9E3779B1u;
    DlkRec** set = &gDlkTab[((hs >> 8) & (DLK_SETS - 1)) * DLK_WAYS]; const DlkRec* r = NULL;
    /* a record is (list, KEY) since run 132: the same list can sit in two ways under two vertex buffers */
    for (int w = 0; w < DLK_WAYS; ++w) if (set[w] && set[w]->dl == dl && set[w]->nbytes == nbytes && !memcmp(&kn, &set[w]->key, sizeof kn)) { r = set[w]; break; }
    if (!r || r->state != 1) return -1;
    TevKey tk; TevUniforms tu; u8 sc[TEVK_TEX], sm[TEVK_TEX];
    if (!gx_tev_build(&tk, &tu, sc, sm, gAcComp0, gAcOp, gAcComp1, gAcRef0, gAcRef1)) return -1;
    if (fogWant() && !((gFogType & 8) || gProjOrtho) && tk.numTex < 8 && !fogKilled()) tk.fogType = (uint8_t)gFogType;
    const u32 nd = r->ndist, nt = r->ntex;
    if (sVcap < nd) { size_t nc = sVcap ? sVcap : 256; while (nc < nd) nc *= 2; sV = (VsRawVertex*)realloc(sV, nc * sizeof *sV); sVcap = nc; }
    if (sIcap < (size_t)r->ntri * 3) { size_t nc = sIcap ? sIcap : 1024; while (nc < (size_t)r->ntri * 3) nc *= 2; sI = (int*)realloc(sI, nc * sizeof *sI); sIcap = nc; }
    for (u32 d = 0; d < nd; ++d) {
        VsRawVertex* v = &sV[d]; memset(v, 0, sizeof *v);
        for (int q = 0; q < 3; ++q) { v->pos[q] = r->pos[3*d+q]; v->nrm[q] = r->nrm ? r->nrm[3*d+q] : 0.0f; }
        u32 in = r->clr ? r->clr[d] : 0xFFFFFFFFu;
        for (int q = 0; q < 4; ++q) v->clr[q] = (float)((in >> (8 * q)) & 0xff) / 255.0f;
        v->mtx = r->pnm ? (float)(r->pnm[d] / 3) : 0.0f;
        if (r->nbt) for (int q = 0; q < 3; ++q) { const int sw = nbtSwap(); v->bin[q] = r->nbt[6*d+(sw?3:0)+q]; v->tan[q] = r->nbt[6*d+(sw?0:3)+q]; }
        for (int n = 0; n < 8; ++n) if (r->tmiIdx[n] >= 0) v->tmi[n] = (float)r->tmi[(size_t)d * r->ntmi + r->tmiIdx[n]];
        for (int n = 0; n < 8; ++n) if (r->texIdx[n] >= 0) { int j = r->texIdx[n]; v->tex[n][0] = r->tex[((size_t)d * nt + j) * 2]; v->tex[n][1] = r->tex[((size_t)d * nt + j) * 2 + 1]; }
    }
    int ni = 0;
    for (u32 p = 0; p < r->nprim; ++p) {
        const DlkPrim* P = &r->prim[p]; const u16* m = r->map + P->mapOff; const u8* fl = r->fl; u32 nv = P->nv;
#define VS_TRI(a, b, c) do { u32 A = m[a], B = m[b], C = m[c]; if (fl[A] & fl[B] & fl[C] & 1) { sI[ni++] = (int)A; sI[ni++] = (int)B; sI[ni++] = (int)C; } } while (0)
        if (P->prim == GX_TRIANGLES) { for (u32 i = 0; i + 3 <= nv; i += 3) VS_TRI(i, i + 1, i + 2); }
        else if (P->prim == GX_TRIANGLESTRIP) { for (u32 i = 2; i < nv; ++i) { if (i & 1) VS_TRI(i - 1, i - 2, i); else VS_TRI(i - 2, i - 1, i); } }
        else if (P->prim == GX_TRIANGLEFAN) { for (u32 i = 2; i < nv; ++i) VS_TRI(0, i - 1, i); }
        else if (P->prim == GX_QUADS) { for (u32 i = 0; i + 4 <= nv; i += 4) { VS_TRI(i, i + 1, i + 2); VS_TRI(i, i + 2, i + 3); } }
#undef VS_TRI
    }
    o->nv = (int)nd; o->ni = ni; o->v = sV; o->idx = sI; o->tk = tk;
    int ok = gx_vs_build_ref(r, &tk, sc, &o->key, &o->u, NULL, NULL);   /* the reference build: o's buffers are the caller's, never memoised */
    o->why = ok ? VSW_OK : gx_vs_why;
    return ok;
}
