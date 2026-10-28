/* vt_map.c - vita_tool_main.c configured for upstream's map_view.cpp. */
#define VT_TOOL          map_view_main
#define VT_NAME          "map_view"
#define VT_LOG           "ux0:data/stairfax_map.txt"
#define VT_SHOT          "ux0:data/stairfax_map.bmp"
#define VT_ARGS          "ux0:data/stairfax/map_args.txt"
/* upstream's defaults (desert, mod29), plus enough frames to outlast the
 * pump's RUN_FRAMES limit so the Vita-side limit is the one that ends it. */
#define VT_DEFAULT_ARGS  "--dir", "desert", "--mod", "29", "--frames", "100000"
#define VT_CLEAR         15, 18, 26      /* rhi_clear(0.06, 0.07, 0.10) */
#include "vita_tool_main.c"

/* ============================================================================
 * DISPLAY-LIST RECORD / REPLAY CACHE (map_view only; all symbols --wrap'd in
 * CMakeLists.txt, upstream untouched).
 *
 * WHY: measured on device (docs/47), 200.27 of 201.21 ms/frame is inside
 * GXCallDisplayList -- gx_draw decoding the SAME static display lists every
 * frame, ~100k vertices through generic attribute readers. The GPU is idle.
 *
 * HOW: the first call of a display list runs the real gx_draw and RECORDS what
 * it sends the backend (texture, alpha mode, model-space vertices, one entry per
 * rhi_drawTextured). Later calls REPLAY those straight to the backend. The only
 * per-frame input is the MVP; it is recomputed from shadowed GXSetProjection /
 * GXLoadPosMtxImm / GXSetCurrentMtx with gx_draw's own computeMVP arithmetic.
 *
 * ⚠ SELF-VALIDATING. While recording, every MVP gx_draw actually sends is
 * compared with the shadow's. Any mismatch (a display list that loads matrices
 * itself, a safety clamp inside GXLoadPosMtxImm), or any untextured draw, marks
 * that list UNCACHEABLE forever: it is always decoded for real. Correctness is
 * then checked end to end by geom_stats: tri count, sum_xyz and sum_mvp must be
 * identical to an uncached run, and the frame pixel-identical.
 *
 * ⚠ VALID ONLY FOR STATIC DATA. The key is (list pointer, size, bound texture):
 * map blocks are loaded once and never change. A caller that edits vertex data
 * or display lists in place must not use this.
 *
 * Kill switch: ux0:data/STFX00001/NOCACHE present at the first call -> every
 * call decodes for real (the A/B arm), timing still recorded.
 * ========================================================================== */
#include <psp2/kernel/processmgr.h>
#include "port/renderer/rhi_internal.h"   /* RhiInstance::alphaMode */

void __real_GXCallDisplayList(void *list, unsigned int nbytes);
void __real_GXSetProjection(const float mtx[4][4], int type);
void __real_GXLoadPosMtxImm(const float mtx[3][4], unsigned int id);
void __real_GXSetCurrentMtx(unsigned int id);
void __real_gx_draw_setTexture(RhiTexture *tex);
void __real_rhi_setColorTransform(RhiInstance *r, const float m[16]);
void __real_rhi_setTexture(RhiInstance *r, int slot, RhiTexture *t);
void __real_rhi_setAlphaMode(RhiInstance *r, int mode);
void __real_rhi_drawTextured(RhiInstance *r, const RhiTexVertex *v, uint32_t n);
void __real_rhi_drawColored(RhiInstance *r, const RhiColorVertex *v, uint32_t n);

/* ---- shadow of gx_draw's transform state (same slot arithmetic: id / 3) ---- */
#define SH_SLOTS 64
static float sh_proj[4][4] = { {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1} };
static float sh_pos[SH_SLOTS][3][4];
static int   sh_cur;
static RhiTexture *sh_tex;   /* gx_draw_setTexture's argument */

void __wrap_GXSetProjection(const float mtx[4][4], int type)
{
    memcpy(sh_proj, mtx, sizeof sh_proj);
    __real_GXSetProjection(mtx, type);
}
void __wrap_GXLoadPosMtxImm(const float mtx[3][4], unsigned int id)
{
    unsigned s = id / 3u;
    if (s < SH_SLOTS) memcpy(sh_pos[s], mtx, sizeof sh_pos[s]);
    __real_GXLoadPosMtxImm(mtx, id);
}
void __wrap_GXSetCurrentMtx(unsigned int id)
{
    unsigned s = id / 3u;
    if (s < SH_SLOTS) sh_cur = (int) s;
    __real_GXSetCurrentMtx(id);
}
void __wrap_gx_draw_setTexture(RhiTexture *tex) { sh_tex = tex; __real_gx_draw_setTexture(tex); }

/* gx_draw.c computeMVP, verbatim arithmetic (float, same loop order). */
static void sh_mvp(float out[16])
{
    float P[4][4];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) P[i][j] = sh_pos[sh_cur][i][j];
    P[3][0] = P[3][1] = P[3][2] = 0.0f; P[3][3] = 1.0f;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0.0f; for (int k = 0; k < 4; ++k) s += sh_proj[i][k] * P[k][j];
        out[i * 4 + j] = s;
    }
}

/* ---- the cache ---------------------------------------------------------------*/
typedef struct { RhiTexture *tex; int mode; uint32_t off, count; } CDraw;
typedef struct {
    const void *list; unsigned nbytes; RhiTexture *key_tex;
    int state;                    /* 0 empty, 1 recorded, 2 uncacheable */
    CDraw *draws; int ndraws, capdraws;
    CDraw *runs;  int nruns;      /* draws merged: same tex+mode AND contiguous */
} CEntry;

/* ---- static VBO replay (rhi_vita.c extension) -------------------------------
 * The whole cache pool is uploaded ONCE to a GL buffer; each list replays as a
 * few RUNS (consecutive draws with the same texture + alpha mode, which are
 * contiguous in the pool because recording appends in order) straight from it.
 * Removes the per-frame batch memcpy and vitaGL's client-array copy.
 * NOVBO marker -> the previous per-draw replay, for A/B in one binary. */
unsigned rhi_vita_uploadStatic(const RhiTexVertex *v, uint32_t n);
void     rhi_vita_deleteStatic(unsigned id);
void     rhi_vita_drawStatic(RhiInstance *r, unsigned vbo, uint32_t first, uint32_t count);
static unsigned g_vbo, g_vbo_n;
static int      g_vbo_enabled = -1;
static SceUInt64 g_vbo_uploads;
#define C_SLOTS 2048
static CEntry        c_tab[C_SLOTS];
static RhiTexVertex *c_pool; static uint32_t c_pool_n, c_pool_cap;
static CEntry       *c_rec;       /* entry being recorded, else NULL */
static float         c_rec_mvp[16];
static RhiTexture   *c_rec_tex; static int c_rec_mode;
static int           c_enabled = -1;
static SceUInt64     g_dl_us, g_dl_calls, c_hits, c_records, c_uncacheable;

static CEntry *c_find(const void *list, unsigned nbytes, RhiTexture *tex)
{
    unsigned h = (unsigned) ((uintptr_t) list >> 3) * 2654435761u;
    for (unsigned i = 0; i < C_SLOTS; ++i) {
        CEntry *e = &c_tab[(h + i) % C_SLOTS];
        if (!e->list) { e->list = list; e->nbytes = nbytes; e->key_tex = tex; return e; }
        if (e->list == list && e->nbytes == nbytes && e->key_tex == tex) return e;
    }
    return NULL;   /* table full: caller decodes for real */
}

void __wrap_rhi_setColorTransform(RhiInstance *r, const float m[16])
{
    if (c_rec && c_rec->state == 0 && memcmp(m, c_rec_mvp, sizeof c_rec_mvp) != 0)
        c_rec->state = 2;   /* gx_draw used an MVP the shadow cannot reproduce */
    __real_rhi_setColorTransform(r, m);
}
void __wrap_rhi_setTexture(RhiInstance *r, int slot, RhiTexture *t)
{
    if (c_rec) c_rec_tex = t;
    __real_rhi_setTexture(r, slot, t);
}
void __wrap_rhi_setAlphaMode(RhiInstance *r, int mode)
{
    if (c_rec) c_rec_mode = mode;
    __real_rhi_setAlphaMode(r, mode);
}
void __wrap_rhi_drawColored(RhiInstance *r, const RhiColorVertex *v, uint32_t n)
{
    if (c_rec) c_rec->state = 2;   /* not recorded: keep the cache textured-only */
    __real_rhi_drawColored(r, v, n);
}
void __wrap_rhi_drawTextured(RhiInstance *r, const RhiTexVertex *v, uint32_t n)
{
    if (c_rec && c_rec->state == 0) {
        if (c_pool_n + n > c_pool_cap) {
            uint32_t cap = c_pool_cap ? c_pool_cap : 65536u;
            while (cap < c_pool_n + n) cap *= 2u;
            RhiTexVertex *np = (RhiTexVertex *) realloc(c_pool, cap * sizeof *np);
            if (!np) c_rec->state = 2; else { c_pool = np; c_pool_cap = cap; }
        }
        if (c_rec->state == 0 && c_rec->ndraws == c_rec->capdraws) {
            int cap = c_rec->capdraws ? c_rec->capdraws * 2 : 16;
            CDraw *nd = (CDraw *) realloc(c_rec->draws, (size_t) cap * sizeof *nd);
            if (!nd) c_rec->state = 2; else { c_rec->draws = nd; c_rec->capdraws = cap; }
        }
        if (c_rec->state == 0) {
            memcpy(c_pool + c_pool_n, v, n * sizeof *v);
            CDraw d = { c_rec_tex, c_rec_mode, c_pool_n, n };
            c_rec->draws[c_rec->ndraws++] = d;
            c_pool_n += n;
        }
    }
    __real_rhi_drawTextured(r, v, n);
}

void __wrap_GXCallDisplayList(void *list, unsigned int nbytes)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    if (c_enabled < 0) {
        SceUID m = sceIoOpen("ux0:data/STFX00001/NOCACHE", SCE_O_RDONLY, 0777);
        c_enabled = (m < 0);
        if (m >= 0) sceIoClose(m);
        printf("[cache] display-list cache %s\n", c_enabled ? "ENABLED" : "disabled (NOCACHE marker)");
    }
    CEntry *e = (c_enabled && g_vt_rhi) ? c_find(list, nbytes, sh_tex) : NULL;

    if (g_vbo_enabled < 0) {
        SceUID m = sceIoOpen("ux0:data/STFX00001/NOVBO", SCE_O_RDONLY, 0777);
        g_vbo_enabled = (m < 0);
        if (m >= 0) sceIoClose(m);
        printf("[cache] static VBO replay %s\n", g_vbo_enabled ? "ENABLED" : "disabled (NOVBO marker)");
    }
    if (e && e->state == 1 && g_vbo_enabled) {     /* REPLAY from the static VBO */
        if (!e->runs) {                             /* merge once */
            e->runs = (CDraw *) malloc((size_t) e->ndraws * sizeof *e->runs);
            for (int i = 0; e->runs && i < e->ndraws; ++i) {
                const CDraw *d = &e->draws[i];
                CDraw *last = e->nruns ? &e->runs[e->nruns - 1] : NULL;
                if (last && last->tex == d->tex && last->mode == d->mode
                         && last->off + last->count == d->off)
                    last->count += d->count;
                else
                    e->runs[e->nruns++] = *d;
            }
        }
        if (g_vbo_n != c_pool_n) {                  /* pool grew: (re)upload once */
            if (g_vbo) rhi_vita_deleteStatic(g_vbo);
            g_vbo = rhi_vita_uploadStatic(c_pool, c_pool_n);
            g_vbo_n = c_pool_n;
            g_vbo_uploads++;
        }
        float mvp[16]; sh_mvp(mvp);
        RhiInstance *r = g_vt_rhi;
        int saved_mode = r->alphaMode;
        __real_rhi_setColorTransform(r, mvp);
        for (int i = 0; e->runs && i < e->nruns; ++i) {
            const CDraw *d = &e->runs[i];
            __real_rhi_setTexture(r, 0, d->tex);
            __real_rhi_setAlphaMode(r, d->mode);
            rhi_vita_drawStatic(r, g_vbo, d->off, d->count);
        }
        __real_rhi_setAlphaMode(r, saved_mode);
        c_hits++;
    } else if (e && e->state == 1) {                /* REPLAY per draw (NOVBO arm) */
        float mvp[16]; sh_mvp(mvp);
        RhiInstance *r = g_vt_rhi;
        int saved_mode = r->alphaMode;
        for (int i = 0; i < e->ndraws; ++i) {
            const CDraw *d = &e->draws[i];
            __real_rhi_setColorTransform(r, mvp);   /* gx_draw sets it per primitive too */
            __real_rhi_setTexture(r, 0, d->tex);
            __real_rhi_setAlphaMode(r, d->mode);
            __real_rhi_drawTextured(r, c_pool + d->off, d->count);
        }
        __real_rhi_setAlphaMode(r, saved_mode);
        c_hits++;
    } else if (e && e->state == 0) {                /* RECORD */
        c_rec = e;
        sh_mvp(c_rec_mvp);
        c_rec_tex = NULL; c_rec_mode = g_vt_rhi->alphaMode;
        __real_GXCallDisplayList(list, nbytes);
        c_rec = NULL;
        if (e->state == 0) { e->state = 1; c_records++; } else c_uncacheable++;
    } else {                                        /* uncacheable / disabled */
        __real_GXCallDisplayList(list, nbytes);
    }
    g_dl_us += sceKernelGetProcessTimeWide() - t0;
    g_dl_calls++;
}
void vt_extra_timing(SceUInt64 out[2])
{
    out[0] = g_dl_us; out[1] = g_dl_calls;
    static int said;
    if (!said && g_dl_calls > 1000) {
        said = 1;
        printf("[cache] lists recorded %llu, uncacheable %llu, replays so far %llu, pool %u verts (%u KB), VBO uploads %llu\n",
               (unsigned long long) c_records, (unsigned long long) c_uncacheable,
               (unsigned long long) c_hits, (unsigned) c_pool_n,
               (unsigned) (c_pool_n * sizeof(RhiTexVertex) / 1024u),
               (unsigned long long) g_vbo_uploads);
    }
}
