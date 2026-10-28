/* rhi_gxm_texture.c - the TEXTURE module of the native GXM backend, milestone M2 (design: host/engine/GXM_M2.md section 8; contract: rhi_gxm_int.h).
 *
 * OWNS: GxmTex (create / destroy / setTexture), the per-texture sampler state (wrap, min/mag/mip filter, lod bias, mip count), authored + generated mip
 * chains, and the COPY textures the target module renders EFB copies into (texture + colour surface + render target in one object).
 * DOES NOT: draw, begin or end a scene, or touch another module's state. Every GPU free goes through gxm_blk_free_deferred / gxm_frame_defer (R7).
 *
 * WHAT vitaGL SHIPS, AND THIS MATCHES (evidence: file:line, read 2026-10-20):
 *   - a glTexImage2D RGBA8 texture is a LINEAR (not strided) A8B8G8R8 GXM texture: control word 0x60000000 (utils/gxm_utils.h:115-121); row stride =
 *     ALIGN(w, 8) texels (utils/gpu_utils.c:386-440). sceGxmTextureInitLinear is the SDK spelling of the same thing.
 *   - mip chain memory (utils/gpu_utils.c gpu_alloc_mipmaps :808-905): level L starts after the sum of MAX(w>>k, 8) * (h>>k) * 4 for k < L (:829, :835);
 *     the chain stops when EITHER side reaches 1 (:828 loop, then the last level is added at :835). For power-of-two sides MAX(w,8) == ALIGN(w,8), which is the
 *     only case that reaches this code (rhi_vita.c:849 refuses NPOT). The pure helpers below are host-tested against a transcription of that loop.
 *   - filter mapping (textures.c:96-153) and lod bias raw = bias8 + GL_MAX_TEXTURE_LOD_BIAS (31) (textures.c:459).
 *   - vitaGL fills the levels the game did not author with sceGxmTransferDownscale (an ASYNC GPU transfer, 2x2 average). Here: a CPU 2x2 box filter, counted
 *     (mip_cpu_box). NOT bit-identical to the transfer unit's rounding (UNVERIFIED which rounding it uses): a picture difference in mips is expected, small.
 *
 * DIFFERENCES FROM THE vitaGL PATH, stated once:
 *   - a chain's SceGxmTexture mip count is applied only while the min filter is a *_MIPMAP_* mode (otherwise 1). vitaGL passes 0 to its own init macro for
 *     use_mips == false, which that macro turns into the field value 15; the intent is the same (base level only) and 1 is the SDK-documented spelling.
 *   - an allocation failure returns NULL (vitaGL: NULL only on malloc failure of the struct) and is counted (tex_fail).
 *   - texSetFilter that cannot build its chain (CPU box buffers / GPU memory) degrades the filter to its base mode and counts mip_fail; vitaGL's
 *     glGenerateMipmap had no visible failure.
 *
 * NEVER RUN ON THE CONSOLE. UNVERIFIED (GXM_M2.md section 15): sceGxmTextureInitLinear with mipCount > 1, MIRROR on a linear texture, the neutral lod bias raw value.
 */

#include "tex_wrap.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================================================================================== pure helpers
 * No SCE, no globals: rhi_gxm_texture host test (host/tests/gxm_tex_test.c) includes this file with GXMTEX_HOST_TEST and drives these.
 * GXMTEX_MUTANT=n (host test only) breaks exactly one rule each; the test must FAIL for every n. */
#define GXMTEX_MAXLV 14                      /* 4096 -> 13 halvings + the base */
#define GXMTEX_ALIGN8(x) (((uint32_t) (x) + 7u) & ~7u)

static inline uint32_t gxmtex_row_bytes(int w)           /* row stride of a LINEAR texture of width w: ALIGN(w, 8) texels of 4 bytes */
{
#if defined(GXMTEX_MUTANT) && GXMTEX_MUTANT == 1
    return (uint32_t) w * 4u;
#else
    return GXMTEX_ALIGN8(w) * 4u;
#endif
}
static inline uint32_t gxmtex_level_bytes(int w, int h, int L)   /* bytes of level L of a w x h chain */
{
    int lw = w >> L, lh = h >> L;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    return gxmtex_row_bytes(lw) * (uint32_t) lh;
}
static inline int gxmtex_top(int w, int h)               /* levels above the base: vitaGL's chain stops when EITHER side reaches 1 */
{
    int n = 0;
#if defined(GXMTEX_MUTANT) && GXMTEX_MUTANT == 2
    for (; w > 1 || h > 1; w >>= 1, h >>= 1) { ++n; if (n >= GXMTEX_MAXLV - 1) break; }
#else
    for (; w > 1 && h > 1; w >>= 1, h >>= 1) ++n;
#endif
    return n;
}
static inline uint32_t gxmtex_level_off(int w, int h, int L)     /* byte offset of level L from the start of the chain */
{
    uint32_t o = 0;
    for (int k = 0; k < L; ++k) o += gxmtex_level_bytes(w, h, k);
    return o;
}
static inline uint32_t gxmtex_chain_bytes(int w, int h, int maxLevel) { return gxmtex_level_off(w, h, maxLevel + 1); }

/* The game's lod bias in 1/8 levels -> the GXM raw field. rhi_vita.c:889-891 clamps to [-31, 32]; raw = bias8 + 31 (GL_MAX_TEXTURE_LOD_BIAS, textures.c:459). */
static inline int gxmtex_bias_clamp(int b8) { return b8 < -31 ? -31 : (b8 > 32 ? 32 : b8); }
static inline unsigned gxmtex_bias_raw(int b8)
{
#if defined(GXMTEX_MUTANT) && GXMTEX_MUTANT == 3
    return (unsigned) (gxmtex_bias_clamp(b8) + 30);
#else
    return (unsigned) (gxmtex_bias_clamp(b8) + 31);
#endif
}

/* The min-filter resolution of rhi_vita_texSetFilter (rhi_vita.c:905-909): out of range -> 1; a mip request without wantMips, or on a side that is not a
 * power of two or is 1 texel, degrades to the base filter of the same family (NEAR family 2, 4 -> 0; LINEAR family 3, 5 -> 1). */
static inline int gxmtex_filter_resolve(int minGX, int wantMips, int w, int h)
{
    if (minGX < 0 || minGX > 5) minGX = 1;
#if defined(GXMTEX_MUTANT) && GXMTEX_MUTANT == 4
    if (minGX >= 2 && !wantMips) minGX = (minGX == 2 || minGX == 3) ? 0 : 1;
#else
    if (minGX >= 2 && !wantMips) minGX = (minGX == 2 || minGX == 4) ? 0 : 1;
#endif
    if (minGX >= 2 && ((w & (w - 1)) || (h & (h - 1)) || w < 2 || h < 2)) minGX = (minGX == 2 || minGX == 4) ? 0 : 1;
    return minGX;
}

/* 2x2 box filter of two source rows (tight RGBA8, sw texels each, sw even) into sw / 2 texels, rounding to nearest per channel. */
static inline void gxmtex_box_rows(const uint8_t *r0, const uint8_t *r1, int sw, uint8_t *dst)
{
    for (int x = 0; x < sw / 2; ++x) {
        const uint8_t *a = r0 + (size_t) x * 8u, *b = r1 + (size_t) x * 8u;
        for (int c = 0; c < 4; ++c) {
#if defined(GXMTEX_MUTANT) && GXMTEX_MUTANT == 5
            dst[(size_t) x * 4u + (size_t) c] = (uint8_t) ((a[c] + a[c + 4] + b[c] + b[c + 4]) >> 2);
#else
            dst[(size_t) x * 4u + (size_t) c] = (uint8_t) ((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) >> 2);
#endif
        }
    }
}

#ifndef GXMTEX_HOST_TEST
/* ================================================================================================================================== the module */
#include "rhi_gxm_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vitaGL path's own counters (rhi_vita.c) feed vt_game.c's [wrap] / [filter] / [mips] lines. They are bumped here too when linked (weak), so those
 * lines mean the same thing under this backend; the module's own copies are what the [gxm-acct] tokens report. */
extern unsigned rhi_vita_wrap_tex[2][3] __attribute__((weak));
extern unsigned rhi_vita_wrap_changes __attribute__((weak)), rhi_vita_wrap_npot_downgrades __attribute__((weak));
extern unsigned rhi_vita_filter_changes __attribute__((weak)), rhi_vita_mip_chains __attribute__((weak));
extern unsigned rhi_vita_mip_authored __attribute__((weak)), rhi_vita_lodbias_changes __attribute__((weak));
#define BUMP(sym) do { if (&(sym) != NULL) ++(sym); } while (0)

#define TEX_MAX_DIM 4096            /* vita_createTexture refuses nothing; the design (section 8) refuses > 4096, the largest GXM texture side */
#define TEX_ALIGN 256u
#define LOG_FIRST 8                 /* failure reasons logged: the first LOG_FIRST of each kind, then counted only */

/* every GxmTex this module makes lives in a node, so shutdown can free what the game leaked (GxmTex has no link field) */
typedef struct TexNode { GxmTex t; struct TexNode *prev, *next; } TexNode;

static struct {
    TexNode *head;
    unsigned alive, copy_alive;
    unsigned long long bytes, copy_bytes;
    unsigned tex_fail, tex_reject, copy_fail, copy_lpddr, tex_created, tex_destroyed;
    unsigned wrap_tex[2][3], wrap_changes, wrap_npot_downgrades;
    unsigned filter_changes, mip_chains, mip_authored, mip_cpu_box, mip_fail, mip_clamped, lodbias_changes;
    unsigned cf_calls, cf_nearest, cf_changes;
    unsigned leak_freed;
    int lodlog_done;
} S;

static unsigned s_logged[4];
static int log_ok(int kind) { return s_logged[kind]++ < LOG_FIRST; }   /* 0 create, 1 mips, 2 copy, 3 other */

static void node_link(TexNode *n) { n->prev = NULL; n->next = S.head; if (S.head) S.head->prev = n; S.head = n; }
static void node_unlink(TexNode *n)
{
    if (n->prev) n->prev->next = n->next; else S.head = n->next;
    if (n->next) n->next->prev = n->prev;
    n->prev = n->next = NULL;
}

/* ---------------------------------------------------------------------------------------------------------------- sampler state onto the SceGxmTexture */
static void tex_apply_wrap(GxmTex *t)
{
    static const SceGxmTextureAddrMode am[3] = { SCE_GXM_TEXTURE_ADDR_CLAMP, SCE_GXM_TEXTURE_ADDR_REPEAT, SCE_GXM_TEXTURE_ADDR_MIRROR };   /* TW_CLAMP, TW_REPEAT, TW_MIRROR */
    GXM_CHK(sceGxmTextureSetUAddrMode(&t->t, am[t->wrapS > 2 ? TW_REPEAT : t->wrapS]));
    GXM_CHK(sceGxmTextureSetVAddrMode(&t->t, am[t->wrapT > 2 ? TW_REPEAT : t->wrapT]));
}
static void tex_apply_filter(GxmTex *t)   /* textures.c:96-153 */
{
    static const uint8_t pointMin[6] = { 1, 0, 1, 0, 1, 0 }, mipOn[6] = { 0, 0, 0, 0, 1, 1 };
    const unsigned f = t->filtMin > 5 ? 1u : t->filtMin;
    GXM_CHK(sceGxmTextureSetMinFilter(&t->t, pointMin[f] ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR));
    GXM_CHK(sceGxmTextureSetMagFilter(&t->t, t->filtMagNear ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR));
    GXM_CHK(sceGxmTextureSetMipFilter(&t->t, mipOn[f] ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED));
    if (t->hasMips && t->nlev > 1)   /* a chain exists: levels are visible only for the mip modes */
        GXM_CHK(sceGxmTextureSetMipmapCount(&t->t, f >= 2 ? t->nlev : 1u));
}
static void tex_apply_lod(GxmTex *t) { GXM_CHK(sceGxmTextureSetLodBias(&t->t, gxmtex_bias_raw(t->lodBias8))); }
static void tex_apply_all(GxmTex *t) { tex_apply_wrap(t); tex_apply_filter(t); tex_apply_lod(t); }

/* ---------------------------------------------------------------------------------------------------------------- release */
static void rt_destroy_cb(void *p) { GXM_CHK(sceGxmDestroyRenderTarget((SceGxmRenderTarget *) p)); }

static void tex_release(TexNode *n)
{
    GxmTex *t = &n->t;
    node_unlink(n);
    if (t->copy) {
        if (S.copy_alive) --S.copy_alive;
        S.copy_bytes -= t->mem.size < S.copy_bytes ? t->mem.size : S.copy_bytes;
        if (t->rt) gxm_frame_defer(rt_destroy_cb, t->rt);    /* an in-flight scene may still target it */
    } else {
        if (S.alive) --S.alive;
        ++S.tex_destroyed;
        S.bytes -= t->mem.size < S.bytes ? t->mem.size : S.bytes;
    }
    if (t->mem.p) gxm_blk_free_deferred(&t->mem);            /* the GPU may still sample / write it */
    free(n);
}

int gxm_tex_init(void)
{
    memset(&S, 0, sizeof S);
    memset(s_logged, 0, sizeof s_logged);
    return 0;
}

void gxm_tex_shutdown(void)
{
    unsigned n = 0;
    while (S.head) { tex_release(S.head); ++n; }
    S.leak_freed += n;
    gxm_log("[gxm] tex shutdown: %u texture(s) were still alive and are freed (deferred)", n);
}

/* ---------------------------------------------------------------------------------------------------------------- ordinary textures */
int rhi_gxm_tex_max_dim(void) { return TEX_MAX_DIM; }   /* R5: the render-thread split's front refuses an oversize texture from its arguments, as gxm_op_createTexture does */

RhiTexture *gxm_op_createTexture(RhiInstance *r, int w, int h, int mips, uint32_t fmt, const void *rgba8)
{
    (void) r; (void) mips;
    if (w <= 0 || h <= 0 || !rgba8) return NULL;            /* vita_createTexture :765 */
    if (w > TEX_MAX_DIM || h > TEX_MAX_DIM) {
        ++S.tex_reject;
        if (log_ok(0)) gxm_log("[gxm] tex: %dx%d refused (> %d)", w, h, TEX_MAX_DIM);
        return NULL;
    }
    TexNode *n = (TexNode *) calloc(1, sizeof *n);
    if (!n) { ++S.tex_fail; return NULL; }
    GxmTex *t = &n->t;
    const uint32_t stride = gxmtex_row_bytes(w), size = stride * (uint32_t) h;
    if (gxm_blk_alloc(&t->mem, GXM_MEM_CDRAM, size, TEX_ALIGN) != 0 || !t->mem.p) {
        ++S.tex_fail;
        if (log_ok(0)) gxm_log("[gxm] tex: %dx%d (%u bytes) allocation FAILED (tex_fail %u)", w, h, size, S.tex_fail);
        free(n);
        return NULL;
    }
    const uint32_t rowb = (uint32_t) w * 4u;
    const unsigned char *src = (const unsigned char *) rgba8;
    unsigned char *dst = (unsigned char *) t->mem.p;
    if (stride == rowb) memcpy(dst, src, (size_t) rowb * (size_t) h);
    else for (int y = 0; y < h; ++y) {
        memcpy(dst + (size_t) y * stride, src + (size_t) y * rowb, rowb);
        memset(dst + (size_t) y * stride + rowb, 0, stride - rowb);
    }
    if (GXM_CHK(sceGxmTextureInitLinear(&t->t, t->mem.p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned) w, (unsigned) h, 1)) != 0) {
        ++S.tex_fail;
        gxm_blk_free_now(&t->mem);                           /* never submitted */
        free(n);
        return NULL;
    }
    if (!S.lodlog_done) {                                    /* the neutral raw value, read back once (GXM_M2.md section 15: 31 is ASSUMED) */
        S.lodlog_done = 1;
        gxm_log("[gxm] tex: sceGxmTextureGetLodBias after InitLinear = %u (31 assumed neutral; this module sets raw %u for bias 0)",
                (unsigned) sceGxmTextureGetLodBias(&t->t), gxmtex_bias_raw(0));
    }
    t->w = w; t->h = h; t->fmt = fmt;
    t->filtMin = 1; t->filtMagNear = 0; t->hasMips = 0; t->lodBias8 = 0; t->nlev = 1;
    t->wrapS = t->wrapT = TW_REPEAT;
    tex_apply_all(t);
    node_link(n);
    ++S.alive; S.bytes += t->mem.size; ++S.tex_created;
    return (RhiTexture *) t;
}

void gxm_op_destroyTexture(RhiInstance *r, RhiTexture *h)
{
    RhiGxm *v = (RhiGxm *) r;
    if (!h) return;
    if (v && v->cur_tex == h) v->cur_tex = NULL;             /* no dangling bind */
    if (g_gxm_inst.cur_tex == h) g_gxm_inst.cur_tex = NULL;
    tex_release((TexNode *) h);                              /* GxmTex is the first member of its node */
}

void gxm_op_setTexture(RhiInstance *r, int slot, RhiTexture *h)
{
    RhiGxm *v = (RhiGxm *) r;
    (void) slot;                                             /* single-texture path (rhi_vita.c:938-944) */
    if (v) v->cur_tex = h;
}

void rhi_gxm_texInfo(RhiTexture *h, int out[4])              /* the census: w, h, the GX format, min filter + 16 if the mip chain exists */
{
    const GxmTex *t = (const GxmTex *) h;
    if (!t) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    out[0] = t->w; out[1] = t->h; out[2] = (int) t->fmt; out[3] = t->filtMin + (t->hasMips ? 16 : 0);
}

void rhi_gxm_texSetWrap(RhiTexture *h, int gxS, int gxT)
{
    GxmTex *t = (GxmTex *) h;
    if (!t) return;
    unsigned down = 0;
    const int s = tw_effective(gxS, t->w, &down), q = tw_effective(gxT, t->h, &down);
    S.wrap_npot_downgrades += down;
    { unsigned *np = &rhi_vita_wrap_npot_downgrades; if (down && np) *np += down; }   /* weak, as above; down is 0..2 */
    ++S.wrap_tex[0][s]; ++S.wrap_tex[1][q];
    { unsigned (*wt)[3] = rhi_vita_wrap_tex; if (wt) { ++wt[0][s]; ++wt[1][q]; } }   /* weak: absent when rhi_vita.c is not linked */
    if (s == t->wrapS && q == t->wrapT) return;
    t->wrapS = (uint8_t) s; t->wrapT = (uint8_t) q;
    tex_apply_wrap(t);
    ++S.wrap_changes; BUMP(rhi_vita_wrap_changes);
}

void rhi_gxm_texSetLodBias(RhiTexture *h, int bias8)
{
    GxmTex *t = (GxmTex *) h;
    if (!t) return;
    bias8 = gxmtex_bias_clamp(bias8);
    if (bias8 == t->lodBias8) return;
    t->lodBias8 = (int8_t) bias8;
    tex_apply_lod(t);
    ++S.lodbias_changes; BUMP(rhi_vita_lodbias_changes);
}

/* ---------------------------------------------------------------------------------------------------------------- mip chains */
/* Build the whole chain 0..maxLevel in ONE new block and make it the texture's storage. Levels 1..nlev come from rgba[L-1] (tight RGBA8, (w>>L) x (h>>L));
 * levels nlev+1..maxLevel are a CPU 2x2 box of the level above (counted). Level 0 is copied from the old block ROW BY ROW through a two-row scratch (the
 * old block is CDRAM: read once, never twice). The texture is untouched on any failure. Returns 1 on success.
 * Preconditions (checked by the callers): power-of-two sides, 1 <= maxLevel <= gxmtex_top, 0 <= nlev <= maxLevel, !hasMips, !copy, mem.p != NULL. */
static int tex_build_chain(GxmTex *t, int nlev, unsigned char *const *rgba, int maxLevel)
{
    const int w = t->w, h = t->h;
    const uint32_t total = gxmtex_chain_bytes(w, h, maxLevel);
    GxmBlk nb;
    memset(&nb, 0, sizeof nb);
    if (gxm_blk_alloc(&nb, GXM_MEM_CDRAM, total, TEX_ALIGN) != 0 || !nb.p) {
        ++S.mip_fail;
        if (log_ok(1)) gxm_log("[gxm] mips: %dx%d chain of %u bytes allocation FAILED", w, h, total);
        return 0;
    }
    unsigned char *base = (unsigned char *) nb.p;
    unsigned char *rows = (unsigned char *) malloc((size_t) w * 8u);            /* two source rows, tight */
    const int need_l1 = !(nlev >= 1 && rgba && rgba[0]);
    unsigned char *gen = need_l1 ? (unsigned char *) malloc((size_t) (w >> 1) * (size_t) (h >> 1) * 4u) : NULL;
    int ok = rows != NULL && (!need_l1 || gen != NULL);
    unsigned boxed = 0;

    if (ok) {                                                                    /* level 0 (+ level 1 box when level 1 is not authored) */
        const unsigned char *old = (const unsigned char *) t->mem.p;
        const uint32_t os = gxmtex_row_bytes(w), ns = gxmtex_row_bytes(w), rowb = (uint32_t) w * 4u;
        for (int y = 0; y < h; ++y) {
            unsigned char *rr = rows + (size_t) (y & 1) * rowb;
            memcpy(rr, old + (size_t) y * os, rowb);
            memcpy(base + (size_t) y * ns, rr, rowb);
            if (ns > rowb) memset(base + (size_t) y * ns + rowb, 0, ns - rowb);
            if (need_l1 && (y & 1)) gxmtex_box_rows(rows, rows + rowb, w, gen + (size_t) (y >> 1) * (size_t) (w >> 1) * 4u);
        }
        if (need_l1) ++boxed;
    }

    const unsigned char *cur = NULL;                 /* tight pixels of the level above the one being written */
    unsigned char *curOwned = NULL;
    for (int L = 1; ok && L <= maxLevel; ++L) {
        const int lw = w >> L, lh = h >> L;
        const unsigned char *lp;
        unsigned char *own = NULL;
        if (L <= nlev && rgba && rgba[L - 1]) lp = rgba[L - 1];
        else if (L == 1) { lp = gen; own = gen; gen = NULL; }
        else {
            own = (unsigned char *) malloc((size_t) lw * (size_t) lh * 4u);
            if (!own) { ok = 0; break; }
            const size_t sb = (size_t) (lw * 2) * 4u;
            for (int y = 0; y < lh; ++y) gxmtex_box_rows(cur + (size_t) (2 * y) * sb, cur + (size_t) (2 * y + 1) * sb, lw * 2, own + (size_t) y * (size_t) lw * 4u);
            lp = own; ++boxed;
        }
        {
            unsigned char *d = base + gxmtex_level_off(w, h, L);
            const uint32_t ls = gxmtex_row_bytes(lw), lrow = (uint32_t) lw * 4u;
            for (int y = 0; y < lh; ++y) {
                memcpy(d + (size_t) y * ls, lp + (size_t) y * lrow, lrow);
                if (ls > lrow) memset(d + (size_t) y * ls + lrow, 0, ls - lrow);
            }
        }
        free(curOwned);
        cur = lp; curOwned = own;
    }
    free(curOwned); free(gen); free(rows);
    if (!ok) {
        ++S.mip_fail;
        if (log_ok(1)) gxm_log("[gxm] mips: %dx%d CPU scratch allocation FAILED", w, h);
        gxm_blk_free_now(&nb);                                                   /* never submitted */
        return 0;
    }

    SceGxmTexture saved = t->t;
    if (GXM_CHK(sceGxmTextureInitLinear(&t->t, nb.p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned) w, (unsigned) h, (unsigned) maxLevel + 1u)) != 0) {
        t->t = saved;
        ++S.mip_fail;
        gxm_blk_free_now(&nb);
        return 0;
    }
    GxmBlk old = t->mem;
    S.bytes += nb.size; S.bytes -= old.size < S.bytes ? old.size : S.bytes;
    t->mem = nb;
    gxm_blk_free_deferred(&old);                                                 /* draws already recorded may still sample the old one */
    t->hasMips = 1; t->nlev = (uint8_t) (maxLevel + 1);
    S.mip_cpu_box += boxed;
    tex_apply_all(t);                                                            /* InitLinear reset the sampler state */
    return 1;
}

int rhi_gxm_texSetMips(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel)   /* rhi_vita.c:846-880 */
{
    GxmTex *t = (GxmTex *) h;
    if (!t || nlev < 0 || t->hasMips || t->copy || !t->mem.p || (t->w & (t->w - 1)) || (t->h & (t->h - 1))) return 0;
    if (nlev > 0 && !rgba) return 0;
    const int top = gxmtex_top(t->w, t->h);
    if (maxLevel > top) maxLevel = top;
    if (nlev > maxLevel) nlev = maxLevel;
    if (maxLevel < 1) return 0;
    if (!tex_build_chain(t, nlev, rgba, maxLevel)) return 0;
    if (maxLevel < top) ++S.mip_clamped;
    ++S.mip_authored; BUMP(rhi_vita_mip_authored);
    return 1;
}

void rhi_gxm_texSetFilter(RhiTexture *h, int minGX, int magNearest, int wantMips)   /* rhi_vita.c:898-923 */
{
    GxmTex *t = (GxmTex *) h;
    if (!t) return;
    magNearest = magNearest != 0;
    minGX = gxmtex_filter_resolve(minGX, wantMips, t->w, t->h);
    if (minGX == t->filtMin && magNearest == t->filtMagNear) return;
    if (minGX >= 2 && !t->hasMips) {                                             /* glGenerateMipmap: the full box chain */
        if (t->copy || !t->mem.p || !tex_build_chain(t, 0, NULL, gxmtex_top(t->w, t->h)))
            minGX = (minGX == 2 || minGX == 4) ? 0 : 1;                          /* no chain: the base filter (counted as mip_fail by the builder) */
        else { ++S.mip_chains; BUMP(rhi_vita_mip_chains); }
    }
    t->filtMin = (uint8_t) minGX; t->filtMagNear = (uint8_t) magNearest;
    tex_apply_filter(t);
    ++S.filter_changes; BUMP(rhi_vita_filter_changes);
}

/* ---------------------------------------------------------------------------------------------------------------- copy textures (EFB copies) */
static GxmTex *copy_fail_out(TexNode *n, const char *why, int dw, int dh)
{
    ++S.copy_fail;
    if (log_ok(2)) gxm_log("[gxm] copy texture %dx%d FAILED: %s (copy_fail %u)", dw, dh, why, S.copy_fail);
    if (n) {
        if (n->t.rt) { GXM_CHK(sceGxmDestroyRenderTarget(n->t.rt)); }           /* created, never used by a scene */
        if (n->t.mem.p) gxm_blk_free_now(&n->t.mem);
        free(n);
    }
    return NULL;
}

GxmTex *gxm_tex_new_copy(int dw, int dh, int gxfmt)       /* rhi_vita_efb.inc:644-674 */
{
    if (dw <= 0 || dh <= 0) return copy_fail_out(NULL, "bad size", dw, dh);
    const int aw = (dw + 7) & ~7, ah = (dh + 7) & ~7;
    if (aw > TEX_MAX_DIM || ah > TEX_MAX_DIM) return copy_fail_out(NULL, "larger than 4096", dw, dh);
    TexNode *n = (TexNode *) calloc(1, sizeof *n);
    if (!n) return copy_fail_out(NULL, "out of host memory", dw, dh);
    GxmTex *t = &n->t;
    const uint32_t size = (uint32_t) aw * (uint32_t) ah * 4u;
    if (gxm_blk_alloc(&t->mem, GXM_MEM_CDRAM, size, TEX_ALIGN) != 0 || !t->mem.p) return copy_fail_out(n, "GPU memory", dw, dh);
    if (t->mem.kind != GXM_MEM_CDRAM) ++S.copy_lpddr;      /* a render surface that fell back to LPDDR: counted; the first run that shows it decides */
    memset(t->mem.p, 0, size);                              /* deterministic first sample (vitaGL left it undefined) */
    t->w = dw; t->h = dh; t->fmt = (uint32_t) gxfmt; t->copy = 1;
    t->aw = aw; t->ah = ah;
    t->filtMin = 1; t->filtMagNear = 0; t->hasMips = 0; t->lodBias8 = 0; t->nlev = 1;
    t->wrapS = t->wrapT = TW_CLAMP;                         /* GL_CLAMP_TO_EDGE (:660-661) */
    t->smin = t->smag = 0;                                  /* LINEAR / LINEAR */
    if (GXM_CHK(sceGxmTextureInitLinear(&t->t, t->mem.p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned) aw, (unsigned) ah, 1)) != 0)
        return copy_fail_out(n, "TextureInitLinear", dw, dh);
    tex_apply_all(t);
    if (GXM_CHK(sceGxmColorSurfaceInit(&t->cs, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                       SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, (unsigned) aw, (unsigned) ah, (unsigned) aw, t->mem.p)) != 0)
        return copy_fail_out(n, "ColorSurfaceInit", dw, dh);
    SceGxmRenderTargetParams rp;
    memset(&rp, 0, sizeof rp);
    rp.flags = 0;
    rp.width = (uint16_t) aw; rp.height = (uint16_t) ah;
    rp.scenesPerFrame = GXM_COPY_RT_SCENES;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.multisampleLocations = 0;
    rp.driverMemBlock = -1;
    if (GXM_CHK(sceGxmCreateRenderTarget(&rp, &t->rt)) != 0 || !t->rt) { t->rt = NULL; return copy_fail_out(n, "CreateRenderTarget", dw, dh); }
    node_link(n);
    ++S.copy_alive; S.copy_bytes += t->mem.size;
    return t;
}

void gxm_tex_free(GxmTex *t)
{
    if (!t) return;
    if (t->copy) gxm_target_copyedge_forget(t);             /* opt-in copy-edge diagnostic: scan/drop a pending entry before the memory goes */
    tex_release((TexNode *) t);
}

void rhi_gxm_copyEfbFree(RhiTexture *t)
{
    if (!t) return;
    if (g_gxm_inst.cur_tex == t) g_gxm_inst.cur_tex = NULL;
    gxm_tex_free((GxmTex *) t);
}

void rhi_gxm_copyTexSetFilter(RhiTexture *h, int gxMin, int gxMag)   /* rhi_vita_efb.inc:1046-1061 */
{
    GxmTex *t = (GxmTex *) h;
    if (!t || !t->copy || !g_gx.copyfilter_on) return;
    const uint8_t nmin = gxMin == 0 || gxMin == 2 || gxMin == 4, nmag = gxMag == 0;
    ++S.cf_calls;
    if (nmin || nmag) ++S.cf_nearest;
    if (nmin == t->smin && nmag == t->smag) return;
    if (nmin != t->smin) GXM_CHK(sceGxmTextureSetMinFilter(&t->t, nmin ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR));
    if (nmag != t->smag) GXM_CHK(sceGxmTextureSetMagFilter(&t->t, nmag ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR));
    t->smin = nmin; t->smag = nmag;
    ++S.cf_changes;
}

void rhi_gxm_copyTexSetSampler(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT)   /* :1065-1072 */
{
    GxmTex *t = (GxmTex *) h;
    if (!t || !t->copy || !g_gx.copyfilter_on) return;
    rhi_gxm_copyTexSetFilter(h, gxMin, gxMag);
    if (t->w == t->aw && t->h == t->ah) rhi_gxm_texSetWrap(h, gxWrapS, gxWrapT);   /* REPEAT on a padded copy would wrap through the padding */
}

/* ---------------------------------------------------------------------------------------------------------------- report */
int gxm_tex_report(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    const int n = snprintf(buf, (size_t) cap,
        "tex_alive=%u tex_bytes=%llu tex_fail=%u wrap_changes=%u wrap_npot_downgrades=%u filter_changes=%u mip_chains=%u mip_authored=%u mip_cpu_box=%u "
        "lodbias_changes=%u copy_alive=%u tex_reject=%u mip_fail=%u mip_clamped=%u copy_fail=%u copy_lpddr=%u copy_bytes=%llu cf_calls=%u cf_nearest=%u cf_changes=%u "
        "tex_leak_freed=%u tex_create=%u tex_destroy=%u ",
        S.alive, S.bytes, S.tex_fail, S.wrap_changes, S.wrap_npot_downgrades, S.filter_changes, S.mip_chains, S.mip_authored, S.mip_cpu_box,
        S.lodbias_changes, S.copy_alive, S.tex_reject, S.mip_fail, S.mip_clamped, S.copy_fail, S.copy_lpddr, S.copy_bytes, S.cf_calls, S.cf_nearest, S.cf_changes,
        S.leak_freed, S.tex_created, S.tex_destroyed);
    if (n < 0) return 0;
    return n >= cap ? cap - 1 : n;
}
#endif /* GXMTEX_HOST_TEST */
