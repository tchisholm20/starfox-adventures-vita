/* rhi_vita.c - a PS Vita backend for Stairfax Temperatures' RHI.
 *
 * WHY THIS FILE EXISTS, AND WHY IT IS SHAPED LIKE THIS
 *
 * The PC port (Cassieandstuff/SFA-Decomp @ pc-port-continued) puts every GPU
 * call behind `RhiOps`, a 14-entry vtable in port/renderer/rhi_internal.h, and
 * `rhi.c` dispatches every public rhi_* call through it. Selecting a backend is
 * therefore just picking which ops table an instance carries -- so a Vita
 * renderer is a new ops table and nothing else. No upstream file is edited.
 *
 * It registers as the GL/GLES backend rather than a new RhiBackend enum, because
 * `rhi.c` already routes RHI_BACKEND_GL -> rhi_gl_create() and its AUTO path on
 * a non-_WIN32 target falls through to GL once rhi_vulkan_available() is false.
 * Upstream's own rhi_gl.c is a 369-byte stub returning NULL, so there is nothing
 * to lose and no conflict: that file is simply not in this project's build.
 *
 * THE VTABLE WAS FILLED IN ONE VERIFIED BRICK AT A TIME. rhi.c null-checks
 * every op, so each entry stayed NULL until it had been proven on hardware:
 * clear/present (milestone 1), drawColored + setColorTransform (2), then the
 * four texture ops (3). Filling entries with plausible stubs that draw nothing
 * is how a renderer comes to "work" while showing an empty screen.
 *
 * WHAT IS PROVEN AND WHAT IS COPIED
 *
 * The vitaGL sequence below is not invented; it is the one projects/vita-renderer
 * ships at a locked 60, with its hard-won details carried across:
 *
 *   - vglUseCachedMem(GL_TRUE) BEFORE init. vitaGL allocates uncached by
 *     default, which is right for GPU-only buffers and catastrophic for anything
 *     the CPU reads back (gpu.c measured 24 ms just READING two small textures).
 *   - a 4 MB pool. gpu.c records that 8 MB plus a large newlib heap exceeded
 *     what the app could actually get, and the failure mode was a SILENT HANG
 *     inside init rather than an error.
 *   - NO vglSetupRuntimeShaderCompiler. The renderer needs it for runtime Cg;
 *     this backend is fixed-function only, so skipping it drops the
 *     libshacccg.suprx dependency entirely. That matters beyond packaging: a Cg
 *     compile error on device is not an error message, it takes the process down
 *     and the console reboots.
 *
 * ⚠ vglInitExtended's RETURN VALUE IS NOT SUCCESS. A successful init returns
 * GL_FALSE -- it reports whether a resolution fallback happened. Branching on it
 * produced an invisible app that would not exit, plus three confidently wrong
 * diagnoses. It is recorded here and never tested.
 *
 * ⚠ THERE IS NO vglEnd/vglTerm in the installed header (checked; gpu.c:116 says
 * the same). So destroy() cannot release vitaGL, and pretending otherwise would
 * be a lie in the one place a reader would trust. vitaGL is initialised at most
 * once per process and the instance is a file-scope singleton to match, because
 * the Vita has exactly one display.
 */

#include "port/renderer/rhi.h"
#include "port/renderer/rhi_internal.h"
#include "port/renderer/rhi_gl.h"

#include <vitaGL.h>
#include <psp2/io/fcntl.h>      /* vtrace's breadcrumb file */
#include <psp2/gxm.h>           /* sceGxmTransferFinish (rhi_vita_texSetMips) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   /* offsetof */
#include "geom_stats.h"   /* per-frame geometry fingerprint, shared with the oracle */
#include "tex_wrap.h"     /* GX wrap mode -> applied mode (pure; host/tests/build_tex_wrap_test.sh) */
#include "zbias_marker.h" /* shadow_zbias.txt parser (pure) */
#include "port/rt_timer.h"  /* Task 3: RT_SCOPE(RT_BACKEND) at the backend entry points (rt_stats.txt) */
RtState gRt __attribute__((weak));   /* the strong one is hal/gx_draw.c; the standalone tools (vt_model, vt_map) link no gx_draw and use this zero one (act stays 0) */
#include "efb_rect.h"     /* EFB line -> target pixel mapping, 528-line target, copy quantiser model (pure; host/tests/build_efb_rect_test.sh) */
#include "stfx_backend.h" /* which backend this process runs (vitaGL, the default; or the native GXM skeleton, rhi_gxm.c) */
#include "gx_rt_split_be.h" /* R5 (host/engine/R5_SPLIT.md): g_gxsp_cap / g_gxsp_api.  A split-CAPABLE process (gx_thread_split.txt = 1) holds proxies for every texture / mesh and routes the shims below through the split's front
                               (record when the split is ON, unwrap and call the backend when OFF); any other process pays one load and one predicted branch per shim */
#define GXSP_CAP() __builtin_expect(g_gxsp_cap, 0)
/* rhi_gxm.c (milestone M1). Every entry point below that the game reaches starts with `if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_<name>(...)`. */
RhiInstance *rhi_gxm_create(const RhiCreateInfo *info);
void rhi_gxm_setDepthState(int test, int func, int write);
void rhi_gxm_setViewport(float x, float y, float w, float h);
void rhi_gxm_setScissor(int x, int y, int w, int h);
void rhi_gxm_setShadowZBias(int on);
unsigned char *rhi_gxm_readEfbRect(int x, int y, int w, int h, int *ow, int *oh);
void rhi_gxm_clearRect(int x, int y, int w, int h, uint32_t rgba, float z);
void rhi_gxm_texInfo(RhiTexture *h, int out[4]);
void rhi_gxm_texSetWrap(RhiTexture *h, int gxS, int gxT);
int rhi_gxm_texSetMips(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel);
void rhi_gxm_texSetLodBias(RhiTexture *h, int bias8);
void rhi_gxm_texSetFilter(RhiTexture *h, int minGX, int magNearest, int wantMips);
unsigned rhi_gxm_uploadStatic(const RhiTexVertex *v, uint32_t n);
void rhi_gxm_deleteStatic(unsigned id);
void rhi_gxm_drawStatic(RhiInstance *r, unsigned vbo, uint32_t first, uint32_t count);

#define VITA_SCR_W 960
#define VITA_SCR_H 544

/* The RENDER size: the size of the target the game draws into (M in EFB mode 1; the default framebuffer
 * in modes 0/2, where it is forced to the panel). VITA_SCR_W/H stay the PANEL. Set ONCE, in efb_init,
 * before M exists, from ux0:data/stairfax/render_res.txt; absent/invalid = the panel size. */
static int g_rw = VITA_SCR_W, g_rh = VITA_SCR_H;
/* g_rh is M's ALLOCATED height. g_vh is the VISIBLE height: the pixel rows GX lines 0..480 occupy (y * g_vh / 480). They are equal
 * unless the 528-line EFB is on (efb528_off.txt not 1, EFB mode 1): then g_rh = efb528_total_h(g_vh) and the extra rows lie BELOW the
 * picture, so GX lines 480..528 (the shadow-map passes use down to 512) have pixels. See efb_rect.h. */
static int g_vh = VITA_SCR_H;
static int g_efb528;   /* 1 = M is taller than the picture (set once in efb_res_init) */
unsigned rhi_vita_efb_vp_below480, rhi_vita_efb_clears_below480;   /* viewports / clear rects that extend below GX line 480 */
void rhi_vita_render_size(int *w, int *h) { *w = g_rw; *h = g_vh; }

/* ---- SCENE TAGS for the per-scene GPU-time instrument (scene_gpu.c, 2026-10-11) -------------------
 * Two plain stores at every glBindFramebuffer; nothing reads them unless scene_gpu.txt turned the
 * instrument on. vitaGL begins and ends GXM scenes LAZILY, at the first draw after a bind, so:
 *   kind = the target the NEXT scene is on   ('D' display, 'M' main, 'C' colour-copy texture,
 *          'Z' Z-copy texture, 'S' Z-save, 'B' break-arm scratch, 'm' break-arm probe scene on M)
 *   why  = what the OPEN scene is being left for ('c' colour copy, 'z' Z copy, 'r' back to M,
 *          'p' present, 'x' readback, 'b' break arm); the wrap resets it to 'o' (other) once used.
 * This backend calls nothing in scene_gpu.c: closure.py links it without that object. */
int rhi_vita_scene_kind = 'D', rhi_vita_scene_why = 'o';
#define RHI_SCENE_TAG(kind, why) (rhi_vita_scene_kind = (kind), rhi_vita_scene_why = (why))
int rhi_vita_vsync_off;                                  /* vsync_off.txt: vglWaitVblankStart(GL_FALSE) after init */
int rhi_vita_scene_break_k, rhi_vita_scene_break_what;   /* scene_gpu_break.txt "K [what]" (rhi_vita_efb.inc) */
unsigned rhi_vita_scene_break_us, rhi_vita_scene_probe_us;   /* the break scenes' cost DRAINED, from the boot self-test */

typedef struct VitaTexture {
    GLuint id;
    int    w, h;
    uint32_t fmt;   /* the GX format it was decoded from (journal only) */
    uint32_t a0;    /* texels with alpha 0 at upload (journal only; 2026-09-28 HUD alpha) */
    uint8_t  filtMin, filtMagNear, hasMips;   /* filtMin: the GX min filter now applied (0 NEAR, 1 LINEAR, 2..5 the mip modes; run 258); filtMagNear 1 = GL_NEAREST; hasMips: the chain was built. Creation: 1, 0, 0 */
    uint8_t  wrapS, wrapT;   /* the EFFECTIVE wrap modes now applied (tex_wrap.h TW_*); vita_createTexture sets REPEAT, rhi_vita_texSetWrap changes them */
    int8_t   lodBias8;       /* the LOD bias now applied, in 1/8 levels (GXM's unit); creation 0 (rhi_vita_texSetLodBias) */
} VitaTexture;

typedef struct RhiVita {
    RhiInstance base;
    int  w, h;
    bool vsync;
    /* The MVP, stored COLUMN-major because that is what glLoadMatrixf wants.
     * See vita_setColorTransform for why that is not a detail. */
    float mvp_col[16];
    /* The texture rhi_setTexture bound for slot 0; NULL until one is. */
    VitaTexture *cur_tex;
    /* drawTextured calls skipped because nothing was bound. Traced once, then
     * counted, so a silent skip cannot pass for a draw. */
    unsigned untextured_skips;
} RhiVita;

static void vtrace(const char *what, int v);

/* One display, one instance. See the header comment: vitaGL has no teardown, so
 * a second create must not re-init it. */
static RhiVita g_inst;
static int     g_created;   /* an instance has been handed out */
static int     g_vgl_up;    /* vitaGL has been initialised in this process */

/* The swapchain sentinel. On Vita the display IS the swapchain -- there is
 * nothing to allocate -- but rhi_swapchainCreate's contract is a non-NULL handle
 * on success, and callers test it. Returning NULL would read as failure; a
 * sentinel says "there is one, and it is implicit". */
static int g_swapchain_token;

static void vita_destroy(RhiInstance *r)
{
    (void) r;
    /* Deliberately does NOT tear vitaGL down: no vglEnd/vglTerm exists. Marking
     * the instance free is the whole of what can honestly be done, and a
     * subsequent create reuses the live GL context. */
    g_created = 0;
}

static RhiSwapchain *vita_swapchainCreate(RhiInstance *r, void *windowHandle,
                                          int w, int h, bool vsync)
{
    RhiVita *v = (RhiVita *) r;
    (void) windowHandle;          /* no window system on Vita */
    if (w > 0) v->w = w;
    if (h > 0) v->h = h;
    v->vsync = vsync;
    /* The caller asks for its own size (the smoketest wants 1280x720). The panel
     * is 960x544 and cannot change, so the viewport is clamped to the panel
     * rather than honouring a request the hardware cannot meet. Reported below
     * so a size mismatch is visible instead of silently letterboxed. */
    glViewport(0, g_rh - g_vh, g_rw, g_vh);   /* the bound target (its picture rows: all of it unless the 528-line EFB is on): M in EFB mode 1 (the panel in modes 0/2, where g_rw/g_rh are the panel) */
    return (RhiSwapchain *) &g_swapchain_token;
}

static void vita_swapchainDestroy(RhiInstance *r, RhiSwapchain *sc)
{
    (void) r; (void) sc;          /* implicit; nothing was allocated */
}

static void vita_swapchainResize(RhiInstance *r, RhiSwapchain *sc, int w, int h)
{
    (void) r; (void) sc; (void) w; (void) h;
    /* The panel is fixed. A resize request is not an error, it is simply not
     * actionable, so the viewport stays as init left it. */
}

/* ---- Draw batching ---------------------------------------------------------
 *
 * WHY: gx_draw issues one rhi_drawTextured per GX primitive (strip, fan), so the
 * desert map submitted 9,110 draws a frame and ran at 270-276 ms/frame (docs/47
 * m6). Consecutive textured draws that share texture + MVP + alpha mode are
 * the same GL state, so they are appended to one staging buffer and issued as
 * ONE glDrawArrays when that state changes. Predicted on the host oracle before
 * this was written: 9,110 draws -> 216 batches (geom_stats.h `batches`).
 *
 * ⚠ ORDER IS PRESERVED: a batch only ever holds a contiguous run of draws, and
 * anything that could observe or change the framebuffer between them (a
 * colored draw, a clear, endFrame, present, a texture being destroyed) flushes
 * first. So the image is the same as unbatched; only the call count changes.
 *
 * ⚠ The vertices are COPIED: the caller (gx_draw) reuses its buffer at once.
 *
 * Kill switch for A/B measurement in ONE binary: if ux0:data/STFX00001/NOBATCH
 * exists at create, every draw flushes immediately (the old behaviour). */
static struct {
    RhiTexVertex *buf;
    uint32_t      n, cap;
    VitaTexture  *tex;
    float         mvp_col[16];
    int           mode;
    int           depth;     /* the draws' depth state (rhi_vita_setDepthState), part of the batch key */
    int           enabled;
} g_batch;

/* GX z mode per draw (gx_draw.c GXSetZMode -> rhi_vita_setDepthState; docs/47 run 32). -1 = the
 * reference behaviour (test + write, LEQUAL): the viewers never call it. Packed: bit0 test,
 * bits1-3 GX compare func, bit4 write. */
static int g_depth = -1, g_depth_apply = -1;
void rhi_vita_setDepthState(int test, int func, int write)
{
    if (GXSP_CAP()) { g_gxsp_api->setDepthState(test, func, write); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_setDepthState(test, func, write); return; }
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    g_depth = (test & 1) | ((func & 7) << 1) | ((write & 1) << 4);
}

static void vita_loadMvpCols(const float *mvp_col)
{
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(mvp_col);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void vita_applyAlphaMode(int mode);

/* ---- Frame timing ----------------------------------------------------------
 * Cumulative microseconds per span, read by the tool wrapper at the same two
 * presents that bound its ms/frame window, so the parts can be checked against
 * the whole. [0] frame CPU (beginFrame -> endFrame: everything upstream does to
 * build the frame, incl. gx_draw's decode), [1] batch append (inside [0]),
 * [2] flush GL calls (inside [0]), [3] vglSwapBuffers. */
#include <psp2/kernel/processmgr.h>
static SceUInt64 g_time[4];
/* Per-DRAW instruments (fingerprint sums + the append clock reads) are OPT-IN.
 * MEASURED on the desert map (docs/47): they cost ~32 ms of a 76.6 ms frame --
 * 42% -- so every map frame time taken with them on overstated the real cost.
 * The ux0:data/STFX00001/STATS marker turns them on for oracle-comparison runs;
 * the cheap counters (calls, triangles, GL draws) are always on. */
static int g_stats = 0;

/* ---- DRAW JOURNAL: one frame's draws, for "what covers the screen?" (docs/47 run 24) ----
 * vita_tool_main.c turns it on for the frame it captures. Per draw: kind, triangles, the first
 * vertex colour, the NDC bounding box of vertices with w > 0 (and how many had w <= 0), the
 * alpha mode, the texture. A full-screen draw is then a line, not a guess. */
int rhi_vita_journal = 0;
int rhi_vita_census_arm = 0;   /* > 0: the TEV census runs without the per-draw journal (run 259; set by the shot_now.txt request, counted down per present) */
int rhi_vita_colored_honors_alpha = 0;   /* 1 in the game build: see vita_drawColored */
static int g_journal_n;
static void journal(char kind, const float *xyz, size_t stride, uint32_t count, uint32_t rgba0,
                    int mode, const void *tex, const float *mc /* column-major MVP */)
{
    if (!rhi_vita_journal) return;
    if (g_journal_n >= 8000) { g_journal_n++; return; }
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }; unsigned behind = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float *p = (const float *) ((const char *) xyz + i * stride);
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = mc[0 * 4 + r] * p[0] + mc[1 * 4 + r] * p[1] + mc[2 * 4 + r] * p[2] + mc[3 * 4 + r];
        if (c[3] <= 1e-6f) { behind++; continue; }
        for (int a = 0; a < 2; ++a) { float n = c[a] / c[3]; if (n < lo[a]) lo[a] = n; if (n > hi[a]) hi[a] = n; }
    }
    printf("[draw] %3d %c tris %4u rgba %08x ndc x[%7.2f,%7.2f] y[%7.2f,%7.2f] w<=0 %u/%u mode %d tex %p v0 (%.1f,%.1f,%.1f)\n",
           g_journal_n, kind, count / 3u, (unsigned) rgba0, lo[0], hi[0], lo[1], hi[1], behind, count, mode, tex,
           xyz[0], xyz[1], xyz[2]);
    if (kind == 'T' && tex) {
        /* run 48: the characters' textures read as NOISE -- wild UVs (texgens are stubs) or wrong
           texels? The UV range over the draw separates them: a sane skin spans ~[0,1]. */
        const VitaTexture *t = (const VitaTexture *) tex;
        float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
        for (uint32_t i = 0; i < count; ++i) {
            const RhiTexVertex *q = (const RhiTexVertex *) ((const char *) xyz + i * stride);
            if (q->u < ulo) ulo = q->u;
            if (q->u > uhi) uhi = q->u;
            if (q->v < vlo) vlo = q->v;
            if (q->v > vhi) vhi = q->v;
        }
        printf("[draw]     tex %dx%d fmt %u alpha0 %u uv u[%.3g,%.3g] v[%.3g,%.3g]\n", t->w, t->h, (unsigned) t->fmt,
               (unsigned) t->a0, ulo, uhi, vlo, vhi);
    }
    g_journal_n++;
}
void rhi_vita_journal_end(void)
{
    printf("[draw] journal: %d draws this frame (%d listed)\n", g_journal_n, g_journal_n < 8000 ? g_journal_n : 8000);
    rhi_vita_journal = 0; g_journal_n = 0;
}
static SceUInt64 g_frame_t0;
void rhi_vita_timing(SceUInt64 out[4]) { for (int i = 0; i < 4; ++i) out[i] = g_time[i]; }

/* The EFB-as-FBO hooks (rhi_vita_efb.inc, included at the end): all no-ops unless mode 1 is live. */
static void efb_begin_frame(void);
static void efb_end_frame(void);      /* composite M onto the default framebuffer, once per frame */
static void efb_after_swap(void);
static int  efb_read_prep(void);
static int  efb_keeps_depth(void);
static void efb_init(void);
static void tev_flush(void);   /* the TEV-program batch (below): one of the two batches is open at a time */
/* ---- THE GPU PROBE (2026-10-10, docs/47 "Run 180") ------------------------------------------------------------------
 * Gameplay at 640x368 is GPU-bound (run 168) and nobody knows which draws the fill belongs to. With
 * ux0:data/stairfax/gpu_probe.txt starting with 1, every PROBE_LEN presents the backend moves to the next ARM and stops
 * issuing one class of draws; the [win] line (every 60 presents) then prices that class in the SAME scene, bracketed by
 * two baseline arms (their difference is the noise floor). The game is unaware: nothing but the draw call is skipped.
 *   0 baseline | 1 no GX-path opaque/test | 2 no GX-path blended | 3 no batched-TEV opaque/test | 4 no batched-TEV blended
 *   5 no fixed-function draws (coloured / textured: UI, full-screen quads) | 6 baseline */
#define PROBE_LEN  120
#define PROBE_ARMS 7
#define PROBE2_BASE 8                 /* every 8th arm is a baseline */
#define PROBE2_ARMS 96                /* 84 program indices + 12 baselines: one cycle = 11,520 presents */
static int g_probe_on = -1, g_probe_arm, g_probe2_arm, g_probe2_prog = -1;
unsigned rhi_vita_probe_skipped;
static void probe_tick(int presents)
{
    if (g_probe_on < 0) { FILE *pf = fopen("ux0:data/stairfax/gpu_probe.txt", "r"); int c = pf ? fgetc(pf) : 0; if (pf) fclose(pf);
                          g_probe_on = c == 49 ? 1 : c == 50 ? 2 : 0;
                          if (g_probe_on == 1) printf("[probe] GPU probe ON: %d arms of %d presents\n", PROBE_ARMS, PROBE_LEN);
                          if (g_probe_on == 2) printf("[probe] GPU probe 2 ON: one batched-path TEV program skipped per arm of %d presents, a baseline every %d arms\n", PROBE_LEN, PROBE2_BASE); }
    if (!g_probe_on) return;
    if (g_probe_on == 2) {   /* run 189: arm 0, PROBE2_BASE, 2*PROBE2_BASE... are baselines; the others skip ONE program of the batched path */
        const int a2 = (presents / PROBE_LEN) % PROBE2_ARMS;
        if (a2 != g_probe2_arm) {
            printf("[probe2] present %d: arm %d ends (program index %d, %u draws skipped) -> arm %d\n", presents, g_probe2_arm, g_probe2_prog, rhi_vita_probe_skipped, a2);
            rhi_vita_probe_skipped = 0; g_probe2_arm = a2;
            g_probe2_prog = (a2 % PROBE2_BASE) == 0 ? -1 : a2 - 1 - a2 / PROBE2_BASE;
        }
        return;
    }
    const int arm = (presents / PROBE_LEN) % PROBE_ARMS;
    if (arm != g_probe_arm) { printf("[probe] present %d: arm %d -> %d (skipped in the arm that ends: %u draws)\n", presents, g_probe_arm, arm, rhi_vita_probe_skipped); rhi_vita_probe_skipped = 0; g_probe_arm = arm; }
}
/* cls: 1 GX path, 2 batched TEV, 3 fixed function; blended: the alpha mode blends */
static int probe_skip(int cls, int blended)
{
    const int a = g_probe_arm;
    const int hit = (a == 1 && cls == 1 && !blended) || (a == 2 && cls == 1 && blended) || (a == 3 && cls == 2 && !blended)
                 || (a == 4 && cls == 2 && blended) || (a == 5 && cls == 3);
    if (hit) ++rhi_vita_probe_skipped;
    return hit;
}

static void vita_flush(void)
{
    tev_flush();
    if (!g_batch.n || !g_batch.tex) { g_batch.n = 0; return; }
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    vita_loadMvpCols(g_batch.mvp_col);
    g_depth_apply = g_batch.depth;
    vita_applyAlphaMode(g_batch.mode);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_batch.tex->id);
    /* texel * vertex colour == the reference shader's `i.col * uTex.Sample()`. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(RhiTexVertex), &g_batch.buf[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(RhiTexVertex), &g_batch.buf[0].rgba);
    glTexCoordPointer(2, GL_FLOAT, sizeof(RhiTexVertex), &g_batch.buf[0].u);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) g_batch.n);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
    gs_cur.backend_draws++;
    g_batch.n = 0;
    g_time[2] += sceKernelGetProcessTimeWide() - t0;
}

static bool vita_present(RhiInstance *r, RhiSwapchain *sc)
{
    (void) r; (void) sc;
    vita_flush();
    efb_end_frame();   /* mode 1: a present with no endFrame still shows the picture (idempotent) */
    /* GL_FALSE = do not have vitaGL clear for us; this backend owns its clear.
     * vglSwapBuffers is also where the vblank wait happens. */
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    rhi_vita_scene_why = 'p';   /* scene_gpu.c: the open scene ends in the swap */
    vglSwapBuffers(GL_FALSE);
    g_time[3] += sceKernelGetProcessTimeWide() - t0;
    efb_after_swap();
    gs_frame_end();
    return true;
}

static void vita_beginFrame(RhiInstance *r) { (void) r; g_frame_t0 = sceKernelGetProcessTimeWide(); efb_begin_frame(); }
/* Flush here too: every read-back in these tools (the smoketest's checks, the
 * wrapper's pre-swap grade + capture) happens AFTER endFrame, so the pixels
 * they read must already be drawn. */
static void vita_endFrame  (RhiInstance *r)
{
    (void) r;
    vita_flush();
    efb_end_frame();   /* mode 1: the finished picture must be in the DEFAULT framebuffer for the tools' readbacks */
    if (g_frame_t0) g_time[0] += sceKernelGetProcessTimeWide() - g_frame_t0;
}

static void vita_clear(RhiInstance *r, float cr, float cg, float cb, float ca)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    (void) r;
    vita_flush();
    efb_begin_frame();   /* a clear opens a frame: after an endFrame with no present, back into M */
    glClearColor(cr, cg, cb, ca);
    /* ⚠ glClear honours the write masks. The BLEND/ADD path leaves glDepthMask(GL_FALSE); when a
     * frame ended on a blended draw, the next clear left DEPTH untouched, stale near depth piled
     * up, and every draw failed the test -- the game's frame came out as the clear colour alone
     * (docs/47 run 30: 1 colour with depth on, 108,994 with it off, same frame). */
    glDisable(GL_SCISSOR_TEST);   /* glClear honours the scissor too: the frame clear is the whole target */
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* The CLEAR of a GX EFB->texture copy (gx_draw.c GXCopyTex, 2026-09-28 run 82): the game draws
 * shadow casters etc. into a rectangle of the EFB, copies it, and the copy clears that rectangle.
 * GX coordinates are the 640x480 EFB, top-left origin; the viewport maps them onto the whole
 * 960x544 screen. Every later flush re-applies its own depth/alpha state, so forcing the masks
 * here cannot leak into the next draw. */
/* GXSetViewport (gx_draw.c, run 92): an EFB rectangle (640x480, top-left origin) onto the 960x544
 * screen (GL: bottom-left origin). Both batches flush first: a draw keeps the viewport it was issued
 * under. */
static int g_vp[4] = { 0, 0, VITA_SCR_W, VITA_SCR_H };
unsigned rhi_vita_viewport_changes;
static void vita_sc_recalc(void);
void rhi_vita_setViewport(float x, float y, float w, float h)
{
    if (GXSP_CAP()) { g_gxsp_api->setViewport(x, y, w, h); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_setViewport(x, y, w, h); return; }
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    int vp[4];
    if (!efb_vp_px(x, y, w, h, g_rw, g_vh, g_rh, vp)) return;
    if (y + h > 480.0f) ++rhi_vita_efb_vp_below480;
    if (!memcmp(vp, g_vp, sizeof vp)) return;
    vita_flush();
    memcpy(g_vp, vp, sizeof vp);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    vita_sc_recalc();
    ++rhi_vita_viewport_changes;
}
/* GXSetScissor (gx_draw.c, run 111). Until 2026-10-07 it was an EMPTY stub (the weak scaffold one
 * won the link), so nothing the game scissored was scissored: the cutscene LETTERBOX above all --
 * camera.c sets it as a scissor of rows 71..409 of 480, retail shows black there, the port drew sky
 * to both edges (docs/47 "Runs 108-110", the first retail comparison). An EFB rectangle like the
 * viewport's; kept as state and re-applied by every draw path (vita_applyAlphaMode), because the
 * clears, the copies and the Z resolve all set their own scissor and leave it off.
 * ux0:data/stairfax/noscissor.txt = the old behaviour (the A/B arm; read once). */
static int g_sc[4] = { 0, 0, VITA_SCR_W, VITA_SCR_H };
static int g_sc_full = 1, g_sc_off = -1;
unsigned rhi_vita_scissor_changes, rhi_vita_scissor_empty;
void rhi_vita_setScissor(int x, int y, int w, int h)
{
    if (GXSP_CAP()) { g_gxsp_api->setScissor(x, y, w, h); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_setScissor(x, y, w, h); return; }
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    if (g_sc_off < 0) {
        SceUID f = sceIoOpen("ux0:data/stairfax/noscissor.txt", SCE_O_RDONLY, 0777);
        g_sc_off = f >= 0;
        if (f >= 0) sceIoClose(f);
        printf("[scissor] GXSetScissor is %s\n", g_sc_off ? "IGNORED (noscissor.txt)" : "applied");
    }
    int sc[4];
    efb_sc_px(x, y, w, h, g_rw, g_vh, g_rh, sc);
    if (!memcmp(sc, g_sc, sizeof sc)) return;
    vita_flush();   /* a draw keeps the scissor it was issued under */
    memcpy(g_sc, sc, sizeof sc);
    vita_sc_recalc();
    if (!sc[2] || !sc[3]) ++rhi_vita_scissor_empty;
    if (++rhi_vita_scissor_changes <= 24 || !(rhi_vita_scissor_changes & 4095))
        printf("[scissor] change %u: EFB %d,%d %dx%d -> GL %d,%d %dx%d%s (empty so far %u)\n", rhi_vita_scissor_changes,
               x, y, w, h, sc[0], sc[1], sc[2], sc[3], g_sc_full ? " (whole screen: test off)" : "", rhi_vita_scissor_empty);
}
/* "No scissor test needed": the box is all of M -- or, with the extra 528-line rows, the whole PICTURE while the viewport is inside
 * it (the viewport already confines the draw, so the test would be redundant: the game's ordinary 640x480 scissor stays free). */
static void vita_sc_recalc(void)
{
    int whole = g_sc[0] == 0 && g_sc[1] == 0 && g_sc[2] == g_rw && g_sc[3] == g_rh;
    int pic = g_rh > g_vh && g_sc[0] == 0 && g_sc[1] == g_rh - g_vh && g_sc[2] == g_rw && g_sc[3] == g_vh
              && g_vp[0] >= 0 && g_vp[0] + g_vp[2] <= g_rw && g_vp[1] >= g_rh - g_vh && g_vp[1] + g_vp[3] <= g_rh;
    g_sc_full = whole || pic;
}
static void vita_applyScissor(void)
{
    if (g_sc_full || g_sc_off > 0) { glDisable(GL_SCISSOR_TEST); return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(g_sc[0], g_sc[1], g_sc[2], g_sc[3]);
}
/* An EFB rectangle (640x480, top-left origin) as the screen holds it now: malloc'd RGBA8, BOTTOM
 * row first (glReadPixels), *ow x *oh screen pixels. For GXCopyTex (gx_draw.c copyReal). Measured
 * on the probe (docs/47): it captures this frame's draws, colour survives, and the DEPTH buffer
 * does not -- so depth is cleared here, to make what follows defined instead of arbitrary. */
unsigned rhi_vita_efb_reads;
unsigned char *rhi_vita_readEfbRect(int x, int y, int w, int h, int *ow, int *oh)
{
    if (GXSP_CAP()) return g_gxsp_api->readEfbRect(x, y, w, h, ow, oh);
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_readEfbRect(x, y, w, h, ow, oh);
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    ++gRt.syncs;   /* glReadPixels sync, counted in the [rt] readback line */
    if (w <= 0 || h <= 0) return NULL;
    vita_flush();
    const int fromM = efb_read_prep();   /* mode 1: read from M (a scene boundary first); mode 0/2: the default framebuffer, as before */
    rhi_vita_scene_why = 'x';   /* scene_gpu.c: glReadPixels ends the open scene */
    EfbRectPx rp = efb_rect_px(x, y, w, h, g_rw, g_vh, g_rh);
    int x0 = rp.x0, x1 = rp.x1, y0 = rp.y0, y1 = rp.y1;
    if (x1 <= x0 || y1 <= y0) return NULL;
    unsigned char *buf = (unsigned char *) malloc((size_t) (x1 - x0) * (size_t) (y1 - y0) * 4u);
    if (!buf) return NULL;
    /* RUN 262: with M (an FBO) bound, vitaGL addresses the rectangle from the TOP of the picture; the rows still come back bottom-up. Measured in run 261
     * (copy-source dump): the rectangle at g_rh - y1 read back black for all 14 copies while the same rectangle mirrored about the picture held the image the
     * game had just drawn (the specular sphere map: mean 29,98,15 against retail's 29,96,15). The default framebuffer (modes 0 / 2) keeps the GL origin.
     * Until now every memory-path copy in mode 1 (the GX_CTF_B8 shadow texture) read the wrong rows. ux0:data/stairfax/efb_read_old.txt = the old origin (A/B). */
    static int s_old = -1;
    if (s_old < 0) { FILE *f = fopen("ux0:data/stairfax/efb_read_old.txt", "r"); s_old = f ? 1 : 0; if (f) fclose(f);
                     printf("[efb] readback origin with the FBO bound: %s\n", s_old ? "OLD, GL bottom-left (efb_read_old.txt)" : "top-left (run 262)"); }
    glReadPixels(x0, fromM && !s_old ? y0 : g_rh - y1, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    if (!efb_keeps_depth()) {   /* with the force-load/store hack the depth survives the readback: keep it */
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    *ow = x1 - x0; *oh = y1 - y0;
    ++rhi_vita_efb_reads;
    return buf;
}
unsigned rhi_vita_clear_rects;
void rhi_vita_clearRect(int x, int y, int w, int h, uint32_t rgba, float z)
{
    if (GXSP_CAP()) { g_gxsp_api->clearRect(x, y, w, h, rgba, z); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_clearRect(x, y, w, h, rgba, z); return; }
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    if (w <= 0 || h <= 0) return;
    vita_flush();
    /* NOT clamped, as before: with the 528-line target every rect the game clears (<= line 512) lies inside M; with efb528_off.txt it
     * is the old arithmetic, including a box that runs off the bottom (counted). */
    int x0 = x * g_rw / 640, x1 = (x + w) * g_rw / 640;
    int y0 = y * g_vh / 480, y1 = (y + h) * g_vh / 480;
    if (g_efb528) { EfbRectPx rp = efb_rect_px(x, y, w, h, g_rw, g_vh, g_rh); x0 = rp.x0; x1 = rp.x1; y0 = rp.y0; y1 = rp.y1; }   /* the SAME rect the copy took */
    if (y + h > 480) ++rhi_vita_efb_clears_below480;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, g_rh - y1, x1 - x0, y1 - y0);
    glClearColor((rgba & 0xff) / 255.0f, ((rgba >> 8) & 0xff) / 255.0f, ((rgba >> 16) & 0xff) / 255.0f,
                 (rgba >> 24) / 255.0f);
    glClearDepthf(z);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glClearDepthf(1.0f);
    ++rhi_vita_clear_rects;
}

/* ⚠ ROW-MAJOR IN, COLUMN-MAJOR OUT, AND THIS IS THE ONE THING THAT WILL SILENTLY
 * RUIN EVERY DRAW. rhi.h is explicit: "16 floats, ROW-MAJOR: m[0..3] = row 0",
 * and clip = M * (x,y,z,1). OpenGL's glLoadMatrixf reads COLUMN-major. Feeding
 * it a row-major matrix loads the transpose, which for a pure
 * translate/perspective matrix is not a wild error -- it produces geometry that
 * is present, plausible and wrong, which is far worse than nothing on screen.
 * So transpose here, once, at the boundary where the convention changes. */
static void vita_setColorTransform(RhiInstance *r, const float m[16])
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    RhiVita *v = (RhiVita *) r;
    if (!m) return;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            v->mvp_col[col * 4 + row] = m[row * 4 + col];
}

/* Load the stored (already column-major) MVP. */
static void vita_loadMvp(const RhiVita *v) { vita_loadMvpCols(v->mvp_col); }

/* Per-mode blend/depth/alpha-test state, matching the reference backends
 * (rhi_d3d11.cpp drawTextured; rhi_vulkan.cpp does the same):
 *
 *   OPAQUE  blend off, alpha test off,    depth test+write  ("ignore texel alpha")
 *   TEST    blend off, discard a < 0.5,   depth test+write
 *   BLEND   src-over,  alpha test off,    depth test, write OFF
 *
 * TEST is GL_GEQUAL, not GL_GREATER: the reference is HLSL clip(a - 0.5), which
 * keeps a == 0.5. Depth is LEQUAL like the reference, so coplanar draws later in
 * the frame win -- which is also what keeps the z=0 bring-up geometry layering
 * in draw order. */
int rhi_vita_force_nodepth = 0;   /* A/B diagnostic (vita_tool_main.c VT_AB_NODEPTH): depth test OFF */
unsigned rhi_vita_blend_exact, rhi_vita_blend_subtract;
/* SHADOW RECEIVER DEPTH BIAS (2026-10-12). The shadow receiver re-draws floor triangles coplanar with the floor already in the depth buffer
 * (Z LEQUAL, no write, no bias); the port gives the two passes different float paths, so LEQUAL can fail on a scatter of pixels. While gx_draw.c
 * says the game is inside objDrawShadowCasterMesh (rhi_vita_setShadowZBias), every flush applies glPolygonOffset(factor, units) with
 * GL_POLYGON_OFFSET_FILL (vitaGL: sceGxmSetFront/BackDepthBias((int) factor, (int) units), misc.c update_polygon_offset). ux0:data/stairfax/shadow_zbias.txt
 * = "factor units"; "0 0" = off. The flag changes only after a flush, so earlier draws keep their state. */
static int g_zb_on, g_zb_applied, g_zb_f = ZB_DEFAULT_FACTOR, g_zb_u = ZB_DEFAULT_UNITS, g_zb_loaded;
unsigned rhi_vita_zbias_draw_flushes;
static void zbias_load(void)
{
    if (g_zb_loaded) return;
    g_zb_loaded = 1;
    char b[64] = { 0 }; int len = 0;
    SceUID f = sceIoOpen("ux0:data/stairfax/shadow_zbias.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) { len = sceIoRead(f, b, sizeof b - 1); sceIoClose(f); }
    int ok = zb_parse(b, len, &g_zb_f, &g_zb_u);
    printf("[shadow-zbias] factor %d units %d%s%s\n", g_zb_f, g_zb_u, (g_zb_f || g_zb_u) ? "" : " (OFF: today's behaviour)",
           f < 0 ? " (no shadow_zbias.txt: default)" : ok ? " (shadow_zbias.txt)" : " (shadow_zbias.txt unparsable: default)");
}
void rhi_vita_setShadowZBias(int on)
{
    if (GXSP_CAP()) { g_gxsp_api->setShadowZBias(on); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_setShadowZBias(on); return; }
    zbias_load();
    on = on && (g_zb_f || g_zb_u);
    if (on == g_zb_on) return;
    vita_flush();   /* what is batched was drawn under the old state */
    g_zb_on = on;
}
static void vita_applyZBias(void)
{
    if (g_zb_on == g_zb_applied) return;
    g_zb_applied = g_zb_on;
    if (g_zb_on) { glPolygonOffset((GLfloat) g_zb_f, (GLfloat) g_zb_u); glEnable(GL_POLYGON_OFFSET_FILL); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
}
static void vita_applyAlphaMode(int mode)
{
    vita_applyZBias();
    /* The game's own blend factors (2026-10-07, docs/47 run 122; gx_draw.c pxAlphaMode): bit 0x100 =
     * exact factors in bits 12-14 (src) and 16-18 (dst), GX numbering; 0x200 = subtract (dst - src).
     * The class stays in the low byte. GX's EFB is RGB8_Z24 here (no destination alpha, and M's alpha
     * holds the Z resolve): DSTALPHA reads as 1. ⚠ Not checked: a scene that switches the EFB to
     * RGBA6_Z24 and blends on real destination alpha. */
    int exact = mode & 0x100, sub = mode & 0x200, bs = (mode >> 12) & 7, bd = (mode >> 16) & 7;
    mode &= 0xff;
    vita_applyScissor();   /* every draw path comes through here */
    /* vitaGL re-sends depth state to GXM on every call, changed or not (docs/51): when the game's z
     * mode is applied below, the defaults here would only be overwritten. */
    const int zmode = g_depth_apply >= 0 && !rhi_vita_force_nodepth;
    if (!zmode) {
        if (rhi_vita_force_nodepth) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
    glBlendEquation(sub ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    if (mode == RHI_ALPHA_BLEND || mode == RHI_ALPHA_ADD) {
        glEnable(GL_BLEND);
        if (exact) {
            /* GX_BL_*: 0 ZERO 1 ONE 2 DSTCLR(src)/SRCCLR(dst) 3 INV of 2 4 SRCALPHA 5 INVSRCALPHA 6 DSTALPHA 7 INVDSTALPHA */
            static const GLenum kSrc[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO };
            static const GLenum kDst[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO };
            glBlendFunc(kSrc[bs], kDst[bd]);
            ++rhi_vita_blend_exact; if (sub) ++rhi_vita_blend_subtract;
        } else
        glBlendFunc(GL_SRC_ALPHA, mode == RHI_ALPHA_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_ALPHA_TEST);
        if (!zmode) glDepthMask(GL_FALSE);
    } else {
        glDisable(GL_BLEND);
        if (mode == RHI_ALPHA_TEST) {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GEQUAL, 0.5f);
        } else {
            glDisable(GL_ALPHA_TEST);
        }
        if (!zmode) glDepthMask(GL_TRUE);
    }
    if (zmode) {   /* the game's GX z mode, this draw */
        static const GLenum kFunc[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                         GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
        if (g_depth_apply & 1) { glEnable(GL_DEPTH_TEST); glDepthFunc(kFunc[(g_depth_apply >> 1) & 7]); }
        else glDisable(GL_DEPTH_TEST);
        int blended = (mode == RHI_ALPHA_BLEND || mode == RHI_ALPHA_ADD);
        glDepthMask(((g_depth_apply >> 4) & 1) && !blended ? GL_TRUE : GL_FALSE);
    }
}

static void vita_drawColored(RhiInstance *r, const RhiColorVertex *verts,
                            uint32_t count)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    RhiVita *v = (RhiVita *) r;
    if (!verts || count < 3) return;
    if (g_probe_arm && probe_skip(3, 0)) return;
    /* GL_TRIANGLES consumes vertices in threes. A caller handing over a partial
     * triangle is a bug upstream, but dropping the remainder is better than
     * letting the driver read past the array. */
    count -= count % 3u;

    gs_cur.col_calls++; gs_cur.col_tris += count / 3u;
    journal('C', &verts[0].x, sizeof verts[0], count, verts[0].rgba, rhi_vita_colored_honors_alpha ? r->alphaMode : RHI_ALPHA_OPAQUE, NULL, v->mvp_col);
    if (g_stats) {
        gs_mvp(v->mvp_col);
        for (uint32_t i = 0; i < count; ++i) gs_vert(verts[i].x, verts[i].y, verts[i].z);
    }

    vita_flush();   /* textured draws queued before this one must land first */
    gs_cur.backend_draws++;
    vita_loadMvp(v);
    /* The reference colored path draws with blending OFF and depth test+write
     * ON, regardless of alphaMode. Set per draw, because drawTextured changes
     * this state and it must not leak from one path into the other.
     * THE GAME build opts out (rhi_vita_colored_honors_alpha, set by vt_game.c): there the
     * alpha mode comes from the game's own GXSetBlendMode (gx_draw.c pixel state v1), and a
     * forced-opaque coloured draw is how 48 full-screen strips painted the frame white (run 25). */
    g_depth_apply = g_depth;
    vita_applyAlphaMode(rhi_vita_colored_honors_alpha ? r->alphaMode : RHI_ALPHA_OPAQUE);
    glDisable(GL_TEXTURE_2D);

    /* Interleaved client-side arrays straight out of the caller's buffer. The
     * RhiColorVertex layout is float x,y,z then a u32 whose MEMORY BYTE ORDER is
     * R,G,B,A (rhi.h says so), which is exactly what GL_UNSIGNED_BYTE + 4
     * components reads -- so no swizzle, and no repacking pass.
     *
     * ⚠ A note for when this grows: vitaGL must finish with these pointers
     * before the call returns, because the caller owns the memory and upstream's
     * gx_shim reuses its accumulation buffer between batches. It holds for
     * glDrawArrays with client arrays; it would NOT hold if this were changed to
     * a deferred or mapped path. */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(RhiColorVertex), &verts[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(RhiColorVertex), &verts[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) count);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* ---- Textures ------------------------------------------------------------
 *
 * rhi.h's contract is that createTexture receives LINEAR RGBA8 -- gx_shim has
 * already decoded the GX format (CMPR, I4, RGB5A3, ...) on the CPU -- so `fmt` is
 * informational and `mips` is ignored, exactly as the D3D11 reference does (it
 * creates MipLevels=1 and never reads fmt). Sampling is LINEAR with REPEAT wrap,
 * also matching the reference sampler.
 *
 * ⚠ UNVERIFIED: REPEAT on a NON-POWER-OF-TWO texture. The bring-up test uses
 * 64x64 and says nothing about NPOT wrap on GXM. Game textures are mostly POT,
 * but the first NPOT one is where to look if tiling goes wrong.
 *
 * UV CONVENTION: none is imposed here. GL's t=0 and D3D's v=0 both address the
 * FIRST ROW IN MEMORY, so passing the caller's UVs straight through reproduces
 * the reference backends; "flipping to fix GL" would be the bug. The bring-up
 * test pins this with four distinct quadrant colours, which also catch a U flip
 * and an R/B channel swap. */
unsigned long long rhi_vita_tex_a0_us, rhi_vita_tex_gl_us, rhi_vita_tex_gen_us;   /* movie I8 planes: alpha scan / glTexImage2D / glGenTextures+params, microseconds (gx_draw.c [movie-up]) */
unsigned rhi_vita_textures_alive; unsigned long long rhi_vita_texture_bytes;   /* run 224: textures created here and not destroyed, and their RGBA8 bytes (the [mem] line, rhi_vita_tev.inc) */
static uint32_t vita_texA0(const void *rgba8, size_t texels)
{
    const uint32_t *p = (const uint32_t *) rgba8;   /* little-endian: R,G,B,A bytes -> alpha is the top byte */
    uint32_t n = 0;
    size_t i = 0;
    for (; i + 4 <= texels; i += 4)
        n += ((p[i] >> 24) == 0) + ((p[i + 1] >> 24) == 0) + ((p[i + 2] >> 24) == 0) + ((p[i + 3] >> 24) == 0);
    for (; i < texels; ++i) n += (p[i] >> 24) == 0;
    return n;
}
static RhiTexture *vita_createTexture(RhiInstance *r, int w, int h, int mips,
                                      uint32_t fmt, const void *rgba8)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    (void) r; (void) mips;
    if (w <= 0 || h <= 0 || !rgba8) return NULL;
    VitaTexture *t = (VitaTexture *) malloc(sizeof *t);
    if (!t) return NULL;
    t->fmt = fmt;
    t->w = w;
    t->h = h;
    t->filtMin = 1; t->filtMagNear = 0; t->hasMips = 0; t->lodBias8 = 0;   /* GL_LINEAR below, level 0 only, no bias; malloc does not zero */
    t->wrapS = t->wrapT = TW_REPEAT;   /* what the lines below set; gx_draw.c refines it through rhi_vita_texSetWrap */
    /* Per-plane timers for the movie's I8 planes only (gx_draw.c prints them as [movie-up]); the upload below is untouched. */
    int mv = fmt == 1 /* GX_TF_I8 */ && w >= 200;
    SceUInt64 tm0 = mv ? sceKernelGetProcessTimeWide() : 0, tm1, tm2;
    /* The alpha-0 count (journal text only) was one byte load and compare per texel -- 295,000 for a movie frame's three planes,
     * on every texture upload. Same count, a whole texel per load, unrolled by four: vita_texA0_ref is the old loop (host test). */
    /* Run 218: still 9.73 ms of the 14.6 ms a movie frame costs on the main thread. The movie's planes are I8 (alpha = the
     * luminance), the count means nothing for them and only journal text reads it: not scanned (a0 = 0 for those three). */
    t->a0 = mv ? 0 : vita_texA0(rgba8, (size_t) w * (size_t) h);
    tm1 = mv ? sceKernelGetProcessTimeWide() : 0;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    /* vitaGL copies the pixels during this call, so the caller may free rgba8
     * straight afterwards -- which every upstream caller does. */
    tm2 = mv ? sceKernelGetProcessTimeWide() : 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba8);
    if (mv) {
        SceUInt64 tm3 = sceKernelGetProcessTimeWide();
        rhi_vita_tex_a0_us += tm1 - tm0; rhi_vita_tex_gen_us += tm2 - tm1; rhi_vita_tex_gl_us += tm3 - tm2;
    }
    ++rhi_vita_textures_alive; rhi_vita_texture_bytes += (unsigned long long) w * (unsigned) h * 4u;
    return (RhiTexture *) t;
}

/* TEXTURE WRAP (2026-10-11; tex_wrap.h has the rules and the vitaGL/GXM facts). gx_draw.c calls this right after it creates a
 * cached texture, with the GX wrap modes of the texture object the cache entry was keyed on (the key includes the wrap, so
 * one texture never serves two modes and nothing needs setting at bind time). No GL call when the modes are already applied
 * (a REPEAT/REPEAT texture, the common case, costs nothing here). The open batch is flushed first: it may still draw with
 * this texture's old state. Counters (vt_game.c's [wrap] line): textures by applied mode per axis, GL changes, NPOT downgrades. */
unsigned rhi_vita_wrap_tex[2][3], rhi_vita_wrap_changes, rhi_vita_wrap_npot_downgrades;
void rhi_vita_texInfo(RhiTexture *h, int out[4])   /* the census (gx_tev.c): w, h, the GX format, min filter + 16 if the mip chain exists */
{
    if (GXSP_CAP()) { g_gxsp_api->texInfo(h, out); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_texInfo(h, out); return; }
    const VitaTexture *t = (const VitaTexture *)h;
    out[0] = t->w; out[1] = t->h; out[2] = (int)t->fmt; out[3] = t->filtMin + (t->hasMips ? 16 : 0);
}
void rhi_vita_texSetWrap(RhiTexture *h, int gxS, int gxT)
{
    if (GXSP_CAP()) { g_gxsp_api->texSetWrap(h, gxS, gxT); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_texSetWrap(h, gxS, gxT); return; }
    VitaTexture *t = (VitaTexture *) h;
    if (!t) return;
    unsigned down = 0;
    int s = tw_effective(gxS, t->w, &down), q = tw_effective(gxT, t->h, &down);
    rhi_vita_wrap_npot_downgrades += down;
    ++rhi_vita_wrap_tex[0][s]; ++rhi_vita_wrap_tex[1][q];
    if (s == t->wrapS && q == t->wrapT) return;
    static const GLint gl[3] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT };
    vita_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    if (s != t->wrapS) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl[s]);
    if (q != t->wrapT) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl[q]);
    t->wrapS = (uint8_t) s; t->wrapT = (uint8_t) q;
    ++rhi_vita_wrap_changes;
}

/* Run 255: the game's min/mag filter for an ordinary texture (gx_draw.c texFilterApply). A GL call only when the mode changes. */
unsigned rhi_vita_filter_changes, rhi_vita_mip_chains; unsigned long long rhi_vita_mip_us;
/* THE GAME'S OWN MIP LEVELS (gx_draw.c texUploadAuthoredMips): levels 1..nlev as decoded RGBA (each half the previous), and the chain
 * ends at maxLevel (GXInitTexObjLOD's max_lod: the hardware never samples past it). Marks the texture so texSetFilter does not replace
 * it with glGenerateMipmap. Returns 0 (texture as it was, no chain) on a GL error.
 * RETRACTION (2026-10-14, run 273) of run 264's version, which handed each level to glTexImage2D(level L): vitaGL's glTexImage2D at
 * level > 0 (textures.c _glTexImage2D -> gpu_utils.c gpu_alloc_mipmaps) IGNORES the pixels and re-derives levels 1..L from level 0
 * with sceGxmTransferDownscale. No authored texel ever reached the GPU: runs 264-272 drew the same box chain as run 258, to 1x1.
 * Now ONE glTexImage2D at maxLevel allocates the chain and sets its level count, the downscale (an ASYNC transfer, which would land
 * on top of the writes) is waited for, and glTexSubImage2D -- which does honour level > 0 -- writes levels 1..nlev. Levels past
 * nlev keep the downscale (gx_draw decodes nothing under 4 texels). nlev 0 = the max_lod clamp alone. The first chain of each boot
 * is read back from the GXM texture memory ([mips] verify: mismatching bytes of level 1; 0 = the authored texels are there). */
unsigned rhi_vita_mip_authored, rhi_vita_mip_authored_fail, rhi_vita_mip_clamped;
int rhi_vita_mip_verify = -1;
int rhi_vita_texSetMips(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel)
{
    if (GXSP_CAP()) return g_gxsp_api->texSetMips(h, nlev, rgba, maxLevel);
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_texSetMips(h, nlev, rgba, maxLevel);
    VitaTexture *t = (VitaTexture *) h;
    if (!t || nlev < 0 || t->hasMips || (t->w & (t->w - 1)) || (t->h & (t->h - 1))) return 0;
    int top = 0;
    for (int w = t->w, hh = t->h; w > 1 && hh > 1; w >>= 1, hh >>= 1) ++top;   /* vitaGL's chain stops when EITHER side reaches 1 */
    if (maxLevel > top) maxLevel = top;
    if (nlev > maxLevel) nlev = maxLevel;
    if (maxLevel < 1) return 0;
    vita_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_2D, maxLevel, GL_RGBA, t->w >> maxLevel, t->h >> maxLevel, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    sceGxmTransferFinish();
    for (int L = 1; L <= nlev; ++L)
        glTexSubImage2D(GL_TEXTURE_2D, L, 0, 0, t->w >> L, t->h >> L, GL_RGBA, GL_UNSIGNED_BYTE, rgba[L - 1]);
    if (glGetError() != GL_NO_ERROR) { ++rhi_vita_mip_authored_fail; return 0; }
    if (rhi_vita_mip_verify < 0 && nlev >= 1) {   /* level 1 sits after level 0 (rows of max(w, 8) texels), rows padded to 8 texels */
        const unsigned char *p = (const unsigned char *) vglGetTexDataPointer(GL_TEXTURE_2D);
        int lw = t->w >> 1, lh = t->h >> 1, bad = 0;
        if (p) {
            p += (size_t) (t->w > 8 ? t->w : 8) * t->h * 4u;
            for (int y = 0; y < lh; ++y)
                for (int x = 0; x < lw * 4; ++x) bad += p[(size_t) y * ((lw + 7) & ~7) * 4u + x] != rgba[0][(size_t) y * lw * 4u + x];
            rhi_vita_mip_verify = bad;
            printf("[mips] verify: first authored chain %dx%d, levels 1..%d of %d, level 1 mismatching bytes %d of %d (%s)\n",
                   t->w, t->h, nlev, maxLevel, bad, lw * lh * 4, bad ? "NOT the authored texels" : "the authored texels are on the GPU");
        }
    }
    if (maxLevel < top) ++rhi_vita_mip_clamped;
    t->hasMips = 1; ++rhi_vita_mip_authored;
    rhi_vita_texture_bytes += (unsigned long long) t->w * (unsigned) t->h * 4u / 3u;
    return 1;
}
/* The game's LOD BIAS (GXInitTexObjLOD lod_bias, dropped until 2026-10-14; Aurora applies it). In 1/8 levels: GXM's unit (Vita3K
 * vulkan/texture.cpp: mipLodBias = (lod_bias - 31) / 8), and vitaGL's integer GL_TEXTURE_LOD_BIAS is that raw step around 31.
 * A GL call only on a change. */
unsigned rhi_vita_lodbias_changes;
void rhi_vita_texSetLodBias(RhiTexture *h, int bias8)
{
    if (GXSP_CAP()) { g_gxsp_api->texSetLodBias(h, bias8); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_texSetLodBias(h, bias8); return; }
    VitaTexture *t = (VitaTexture *) h;
    if (!t) return;
    if (bias8 < -31) bias8 = -31;
    if (bias8 > 32) bias8 = 32;
    if (bias8 == t->lodBias8) return;
    vita_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias8);
    t->lodBias8 = (int8_t) bias8; ++rhi_vita_lodbias_changes;
}
void rhi_vita_texSetFilter(RhiTexture *h, int minGX, int magNearest, int wantMips)
{
    if (GXSP_CAP()) { g_gxsp_api->texSetFilter(h, minGX, magNearest, wantMips); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_texSetFilter(h, minGX, magNearest, wantMips); return; }
    VitaTexture *t = (VitaTexture *) h;
    if (!t) return;
    static const GLint glMin[6] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
    magNearest = magNearest != 0;
    if (minGX < 0 || minGX > 5) minGX = 1;
    if (minGX >= 2 && !wantMips) minGX = (minGX == 2 || minGX == 4) ? 0 : 1;
    /* a chain needs power-of-two sides here; anything else keeps its base filter */
    if (minGX >= 2 && ((t->w & (t->w - 1)) || (t->h & (t->h - 1)) || t->w < 2 || t->h < 2)) minGX = (minGX == 2 || minGX == 4) ? 0 : 1;
    if (minGX == t->filtMin && magNearest == t->filtMagNear) return;
    vita_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    if (minGX >= 2 && !t->hasMips) {
        SceUInt64 m0 = sceKernelGetProcessTimeWide();
        glGenerateMipmap(GL_TEXTURE_2D);
        rhi_vita_mip_us += sceKernelGetProcessTimeWide() - m0;
        t->hasMips = 1; ++rhi_vita_mip_chains;
        rhi_vita_texture_bytes += (unsigned long long) t->w * (unsigned) t->h * 4u / 3u;
    }
    if (minGX != t->filtMin) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glMin[minGX]);
    if (magNearest != t->filtMagNear) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magNearest ? GL_NEAREST : GL_LINEAR);
    t->filtMin = (uint8_t) minGX; t->filtMagNear = (uint8_t) magNearest;
    ++rhi_vita_filter_changes;
}

static void vita_destroyTexture(RhiInstance *r, RhiTexture *h)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    RhiVita *v = (RhiVita *) r;
    VitaTexture *t = (VitaTexture *) h;
    if (!t) return;
    vita_flush();                             /* the open batch may draw with it */
    if (v->cur_tex == t) v->cur_tex = NULL;   /* no dangling bind */
    glDeleteTextures(1, &t->id);
    --rhi_vita_textures_alive; rhi_vita_texture_bytes -= (unsigned long long) t->w * (unsigned) t->h * 4u;
    free(t);
}

static void vita_setTexture(RhiInstance *r, int slot, RhiTexture *h)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    RhiVita *v = (RhiVita *) r;
    (void) slot;   /* single-texture path, as in every reference backend */
    v->cur_tex = (VitaTexture *) h;
}

static void vita_drawTextured(RhiInstance *r, const RhiTexVertex *verts,
                              uint32_t count)
{
    RT_SCOPE(RT_BACKEND);   /* Task 3 (rt_stats.txt): backend entry */
    RhiVita *v = (RhiVita *) r;
    if (!verts || count < 3) return;
    if (g_probe_arm && probe_skip(3, 0)) return;
    count -= count % 3u;
    if (!v->cur_tex) {
        /* Every upstream caller guards on having a texture, so this firing is a
         * bug upstream. Skipped rather than drawn untextured, which would be a
         * plausible-looking wrong image. */
        if (v->untextured_skips++ == 0) vtrace("drawTextured_no_tex", 1);
        return;
    }

    gs_cur.tex_calls++; gs_cur.tex_tris += count / 3u;
    journal('T', &verts[0].x, sizeof verts[0], count, verts[0].rgba, r->alphaMode, v->cur_tex, v->mvp_col);
    if (g_stats) {
        gs_mvp(v->mvp_col);
        gs_batch(v->cur_tex, v->mvp_col, r->alphaMode);
        for (uint32_t i = 0; i < count; ++i) gs_vert(verts[i].x, verts[i].y, verts[i].z);
    }

    /* Same state as the open batch? Append. Otherwise close it and start one. */
    int same = g_batch.n && g_batch.tex == v->cur_tex && g_batch.mode == r->alphaMode
            && g_batch.depth == g_depth
            && memcmp(g_batch.mvp_col, v->mvp_col, sizeof g_batch.mvp_col) == 0;
    if (!same) {
        vita_flush();
        g_batch.tex  = v->cur_tex;
        g_batch.mode = r->alphaMode;
        g_batch.depth = g_depth;
        memcpy(g_batch.mvp_col, v->mvp_col, sizeof g_batch.mvp_col);
    }
    if (g_batch.n + count > g_batch.cap) {
        uint32_t cap = g_batch.cap ? g_batch.cap : 4096u;
        while (cap < g_batch.n + count) cap *= 2u;
        RhiTexVertex *nb = (RhiTexVertex *) realloc(g_batch.buf, cap * sizeof *nb);
        if (!nb) {   /* out of memory: draw what we have, then this one alone */
            vita_flush();
            if (count > g_batch.cap) { vtrace("batch_oom", (int) count); return; }
        } else {
            g_batch.buf = nb;
            g_batch.cap = cap;
        }
    }
    SceUInt64 ta = g_stats ? sceKernelGetProcessTimeWide() : 0;
    memcpy(g_batch.buf + g_batch.n, verts, count * sizeof *verts);
    if (g_stats) g_time[1] += sceKernelGetProcessTimeWide() - ta;
    g_batch.n += count;
    if (!g_batch.enabled) vita_flush();   /* NOBATCH arm: one draw per call */
}

/* The vtable is complete: no entry is NULL any more. */
static const RhiOps kVitaOps = {
    vita_destroy,
    vita_swapchainCreate,
    vita_swapchainDestroy,
    vita_swapchainResize,
    vita_present,
    vita_beginFrame,
    vita_endFrame,
    vita_clear,
    vita_drawColored,
    vita_setColorTransform,
    vita_createTexture,
    vita_destroyTexture,
    vita_setTexture,
    vita_drawTextured,
};

/* Breadcrumbs. A failed launch on this console leaves no coredump, and once
 * vitaGL owns the display there is no console either -- so the only way to learn
 * where init died is a file written before each step. Same technique as
 * vita-renderer's gtrace, and it is what turns "black screen" into a line
 * number. */
static void vtrace(const char *what, int v)
{
    SceUID fd = sceIoOpen("ux0:data/stairfax_boot.txt",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    char b[128];
    int n = snprintf(b, sizeof b, "  rhi_vita:%-20s %d\n", what, v);
    if (n > 0) sceIoWrite(fd, b, (unsigned) n);
    sceIoClose(fd);
}

bool rhi_gl_available(void)
{
    return true;    /* vitaGL is linked in; there is no runtime probe to fail */
}

static unsigned g_patcher_kb[3] = { 1024, 1024, 1024 };   /* the shader patcher's buffer / vertex USSE / fragment USSE areas, KB (vitaGL's defaults; set below, read by the [mem] line and the trim in rhi_vita_tev.inc) */
RhiInstance *rhi_gl_create(const RhiCreateInfo *info)
{
    stfx_backend_select();   /* backend.txt, read ONCE, before any vitaGL call; prints "[backend] gxm" / "[backend] vitagl" */
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_create(info);
    if (g_created) {
        vtrace("create_refused_busy", 1);
        return NULL;    /* one display, one instance -- see the header comment */
    }

    if (!g_vgl_up) {
        vtrace("before_vglInit", 0);
        {   /* THE SHADER PATCHER'S VERTEX USSE AREA (run 226). vitaGL gives the patcher three fixed 1 MB areas. Run 226's [mem]
             * line (the first run that had one) read vertex USSE 898 of 1024 KB with 672 programs alive -- about 1.3 KB a
             * program, so the area ends near 770 programs, well inside the 896-program budget, and vitaGL does not check the
             * patcher's result. The program trim was holding it down by accident, at 422 evictions a session. So: 2 MB for the
             * vertex area (it comes out of vitaGL's own RAM pool, 45 MB free in run 226); buffer and fragment stay (0 and 363
             * of 1024 KB used). ux0:data/stairfax/patcher_small.txt starting with 1 = vitaGL's 1 MB (the break arm).
             * NOT YET SEEN ON HARDWARE: a boot that does not come up after this change is this line. */
            SceUID pf = sceIoOpen("ux0:data/stairfax/patcher_small.txt", SCE_O_RDONLY, 0777); char pb = 0;
            if (pf >= 0) { sceIoRead(pf, &pb, 1); sceIoClose(pf); }
            if (pb != '1') { g_patcher_kb[1] = 2048; vglSetupShaderPatcher(g_patcher_kb[0] * 1024u, g_patcher_kb[1] * 1024u, g_patcher_kb[2] * 1024u); }
            vtrace("patcher_vertex_kb", (int) g_patcher_kb[1]);
        }
        vglUseCachedMem(GL_TRUE);           /* MUST precede vglInit* */
        GLboolean ok = vglInitExtended(0, VITA_SCR_W, VITA_SCR_H,
                                       4 * 1024 * 1024,
                                       SCE_GXM_MULTISAMPLE_NONE);
        /* ⚠ RECORDED, NEVER BRANCHED ON: a SUCCESSFUL init returns GL_FALSE.
         * This value reports a resolution fallback, not success. */
        vtrace("vglInitExtended_flag", (int) ok);
        g_vgl_up = 1;
        {   /* vsync_off.txt starting with 1 (2026-10-11, for scene_gpu.c's sum check): the swap no longer waits for the
             * vblank, so the time spent in it is time spent waiting for the GPU. Default: vitaGL's own (vsync on). */
            SceUID vf = sceIoOpen("ux0:data/stairfax/vsync_off.txt", SCE_O_RDONLY, 0777); char vb = 0;
            if (vf >= 0) { sceIoRead(vf, &vb, 1); sceIoClose(vf); }
            rhi_vita_vsync_off = vb == '1';
            if (rhi_vita_vsync_off) vglWaitVblankStart(GL_FALSE);
            printf("[vsync] %s\n", rhi_vita_vsync_off ? "OFF: vglWaitVblankStart(GL_FALSE) (vsync_off.txt)" : "on (vitaGL's default; vsync_off.txt absent or not 1)");
        }

        /* Cull off, like the reference raster state. Depth, blend and alpha
         * test are set per draw by vita_applyAlphaMode. */
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, VITA_SCR_W, VITA_SCR_H);
        vtrace("gl_state_set", 1);
    }

    g_inst.base.ops       = &kVitaOps;
    g_inst.base.backend   = RHI_BACKEND_GL;
    g_inst.base.alphaMode = RHI_ALPHA_OPAQUE;
    g_inst.w     = (info && info->width  > 0) ? info->width  : VITA_SCR_W;
    g_inst.h     = (info && info->height > 0) ? info->height : VITA_SCR_H;
    g_inst.vsync = info ? info->vsync : true;
    g_inst.cur_tex          = NULL;
    g_inst.untextured_skips = 0;
    {   /* Batching on unless the NOBATCH marker exists (A/B in one binary). */
        SceUID nb = sceIoOpen("ux0:data/STFX00001/NOBATCH", SCE_O_RDONLY, 0777);
        g_batch.enabled = (nb < 0);
        if (nb >= 0) sceIoClose(nb);
        g_batch.n = 0;
        vtrace("batching_enabled", g_batch.enabled);
        SceUID ns = sceIoOpen("ux0:data/STFX00001/STATS", SCE_O_RDONLY, 0777);
        g_stats = (ns >= 0);
        if (ns >= 0) sceIoClose(ns);
        vtrace("stats_enabled", g_stats);
    }
    /* Identity, i.e. clip-space passthrough, which rhi.h specifies as the state
     * until setColorTransform is called. ⚠ Leaving mvp_col zeroed would collapse
     * every vertex onto the origin -- a blank screen that reads as "the draw path
     * does not work" rather than as "the matrix was never initialised". */
    for (int i = 0; i < 16; ++i) g_inst.mvp_col[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    g_created    = 1;
    zbias_load();   /* shadow_zbias.txt: the one boot line */
    efb_init();   /* the EFB mode file, M, the copy programs and the boot self-test (once) */

    /* The requested size is recorded next to the real panel size, because the
     * smoketest asks for 1280x720 and gets 960x544. An unreported clamp is how
     * a letterboxed or cropped frame gets mistaken for a renderer bug. */
    vtrace("requested_w", g_inst.w);
    vtrace("panel_w", VITA_SCR_W);
    vtrace("create_ok", 1);
    return &g_inst.base;
}

/* GLES entry points. Upstream treats "gl" and "gles" as separate probes (desktop
 * GL 4.6 vs Android GLES 3.1). vitaGL is a GLES2-class implementation, so both
 * names resolve here rather than leaving rhi_gles_create undefined at link time
 * for any caller that prefers it. */
bool         rhi_gles_available(void)                        { return rhi_gl_available(); }
RhiInstance *rhi_gles_create(const RhiCreateInfo *info)      { return rhi_gl_create(info); }

/* The frame currently being drawn (read by the tool wrapper just BEFORE present). */
const GeomStats *rhi_vita_frame_stats(void) { return &gs_cur; }

/* ---- Static geometry: a Vita-only extension, NOT part of RhiOps ---------------
 *
 * WHY: with the display-list cache (vt_map.c) the map's ~101k vertices never
 * change, yet every frame they were memcpy'd into the batch buffer AND re-copied
 * by vitaGL's client-array path -- ~16 ms/frame of GL flush alone (docs/47).
 * Uploading them ONCE into a GL buffer object removes both copies.
 *
 * This is deliberately outside rhi.h: upstream's RHI has no notion of static
 * buffers on this path, so a caller must know it is on Vita to use it. The draw
 * honours the CURRENT texture / MVP / alpha mode exactly like drawTextured, and
 * flushes any open batch first so draw order is preserved. */
unsigned rhi_vita_uploadStatic(const RhiTexVertex *v, uint32_t n)
{
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_uploadStatic(v, n);
    vita_flush();
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferData(GL_ARRAY_BUFFER, (GLsizei) (n * sizeof *v), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    vtrace("static_upload_verts", (int) n);
    return id;
}

void rhi_vita_deleteStatic(unsigned id)
{
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_deleteStatic(id); return; }
    GLuint g = id;
    vita_flush();
    if (g) glDeleteBuffers(1, &g);
}

void rhi_vita_drawStatic(RhiInstance *r, unsigned vbo, uint32_t first, uint32_t count)
{
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_drawStatic(r, vbo, first, count); return; }
    RhiVita *v = (RhiVita *) r;
    count -= count % 3u;
    if (!vbo || !count || !v->cur_tex) return;
    vita_flush();   /* anything batched before this draw lands first */
    gs_cur.tex_calls++; gs_cur.tex_tris += count / 3u;
    vita_loadMvpCols(v->mvp_col);
    vita_applyAlphaMode(r->alphaMode);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, v->cur_tex->id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    /* With a buffer bound, the "pointers" are byte offsets into it. */
    glVertexPointer(3, GL_FLOAT, sizeof(RhiTexVertex), (const void *) offsetof(RhiTexVertex, x));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(RhiTexVertex), (const void *) offsetof(RhiTexVertex, rgba));
    glTexCoordPointer(2, GL_FLOAT, sizeof(RhiTexVertex), (const void *) offsetof(RhiTexVertex, u));
    glDrawArrays(GL_TRIANGLES, (GLint) first, (GLsizei) count);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_TEXTURE_2D);
    gs_cur.backend_draws++;
}

/* TEV programs: the multi-texture combiner path (docs/47) */
#include "rhi_vita_tev.inc"
/* The EFB as an FBO: GPU copies for GXCopyTex (docs/47) */
#include "rhi_vita_efb.inc"
