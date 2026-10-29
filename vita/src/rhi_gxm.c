/* rhi_gxm.c - the native GXM backend: THE SHELL (milestone M2). Everything that does work lives in the five modules (design: host/engine/GXM_M2.md):
 *   rhi_gxm_frame.c   init/teardown order, memory, rings, fences, counters lines        rhi_gxm_shader.c   programs
 *   rhi_gxm_texture.c textures + copy targets                                           rhi_gxm_draw.c     draws, meshes, per-draw state
 *   rhi_gxm_target.c  M, scenes, EFB copy, present, display blit, SHOT
 * This file keeps only: the backend choice (ux0:data/stairfax/backend.txt, "gxm"), the RhiOps table, the instance, create/shutdown.
 * WHERE IT IS CALLED FROM: rhi_vita.c. rhi_gl_create() asks stfx_backend_select() BEFORE any vitaGL call; on gxm it returns rhi_gxm_create()'s
 * instance, whose ops never reach a vitaGL function. Every direct rhi_vita_* entry point starts with `if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_*(...)`.
 * NO TIMING CLAIMS: nothing here is measured. */

#include "rhi_gxm_int.h"
#include "gx_rt_split.h"

#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#define BACKEND_FILE "ux0:data/stairfax/backend.txt"

int g_stfx_backend;               /* STFX_BK_VITAGL until stfx_backend_select() says otherwise */
RhiGxm g_gxm_inst;
/* R5 (R5_SPLIT.md): the render-thread split's switches, defined here because every link that has rhi_vita.c has this file (the viewers never have gx_rt_split.o).  g_gxsp_cap: handles are proxies (the marker
 * gx_thread_split.txt = 1, read by gxsp_gxm_init before the first handle exists); g_gxsp_on: the HAL's backend calls are being recorded (gx_thread.c switches it at a frame edge). */
volatile int g_gxsp_cap, g_gxsp_on;
const struct GxspApi *g_gxsp_api;
void gxsp_gxm_init(RhiInstance *inst) __attribute__((weak));   /* rhi_gxm_split.c (the game build only) */

/* ---- the choice ------------------------------------------------------------------------------------------------ */
void stfx_backend_select(void)
{
    static int done;
    if (done) return;
    done = 1;
    char b[32];
    memset(b, 0, sizeof b);
    SceUID fd = sceIoOpen(BACKEND_FILE, SCE_O_RDONLY, 0777);
    if (fd >= 0) { sceIoRead(fd, b, sizeof b - 1); sceIoClose(fd); }
    /* GXM IS THE DEFAULT (2026-10-28, the public release): the native backend is the shipping renderer, so a player who installs the vpk without
     * the ux0 folder's backend.txt must still get it. Only an explicit first word "vitagl" selects the old vitaGL path (a dev comparison arm). */
    g_stfx_backend = (fd >= 0 && !strncmp(b, "vitagl", 6) && (b[6] == 0 || b[6] == '\n' || b[6] == '\r' || b[6] == ' ')) ? STFX_BK_VITAGL : STFX_BK_GXM;
    if (g_stfx_backend == STFX_BK_GXM) printf("[backend] gxm\n");
    else printf("[backend] vitagl\n");
    printf("[backend] chosen once, before any vitaGL call, from %s (%s)\n", BACKEND_FILE,
           fd < 0 ? "absent: the default, gxm" : g_stfx_backend == STFX_BK_VITAGL ? "first word vitagl" : "present, not 'vitagl': the default, gxm");
}

/* ---- ops local to the shell ----------------------------------------------------------------------------------- */
static int g_created;
static int g_swapchain_token;

static void shell_destroy(RhiInstance *r) { (void) r; g_created = 0; }
static RhiSwapchain *shell_swapchainCreate(RhiInstance *r, void *wh, int w, int h, bool vsync) { (void) r; (void) wh; (void) w; (void) h; (void) vsync; return (RhiSwapchain *) &g_swapchain_token; }
static void shell_swapchainDestroy(RhiInstance *r, RhiSwapchain *sc) { (void) r; (void) sc; }
static void shell_swapchainResize(RhiInstance *r, RhiSwapchain *sc, int w, int h) { (void) r; (void) sc; (void) w; (void) h; }

static const RhiOps kGxmOps = {
    shell_destroy, shell_swapchainCreate, shell_swapchainDestroy, shell_swapchainResize, gxm_op_present, gxm_op_beginFrame, gxm_op_endFrame,
    gxm_op_clear, gxm_op_drawColored, gxm_op_setColorTransform, gxm_op_createTexture, gxm_op_destroyTexture, gxm_op_setTexture, gxm_op_drawTextured,
};

/* ---- creation (called by rhi_gl_create after the backend choice) -------------------------------------------------- */
RhiInstance *rhi_gxm_create(const RhiCreateInfo *info)
{
    if (g_created) { gxm_log("[gxm] create refused: an instance is already out"); return NULL; }
    g_gxm_inst.base.ops = &kGxmOps;
    g_gxm_inst.base.backend = RHI_BACKEND_GL;      /* the slot rhi.c routes to; the log says gxm */
    g_gxm_inst.base.alphaMode = RHI_ALPHA_OPAQUE;
    g_gxm_inst.w = (info && info->width > 0) ? info->width : GXM_DISP_W;
    g_gxm_inst.h = (info && info->height > 0) ? info->height : GXM_DISP_H;
    g_gxm_inst.cur_tex = NULL;
    memset(g_gxm_inst.mvp, 0, sizeof g_gxm_inst.mvp);
    g_gxm_inst.mvp[0] = g_gxm_inst.mvp[5] = g_gxm_inst.mvp[10] = g_gxm_inst.mvp[15] = 1.0f;
    if (gxm_frame_init(info) != 0) return NULL;
    if (gxsp_gxm_init) gxsp_gxm_init(&g_gxm_inst.base);   /* R5: split-capable iff the marker says so; installs the proxies' ops table over the real one */
    g_created = 1;
    return &g_gxm_inst.base;
}

/* The render-thread trace line (gx_thread.c, RT_THREAD.md 7.1 item 3g): every sceGxmDraw the process issued that succeeded (gxm_sce_draw's counter).
 * Read by whichever thread executes the present: R when threaded, G when direct. One 32-bit load; torn reads are impossible on ARMv7. */
unsigned gxm_issued_draws(void) { return g_gx.st.sce_draw_ok; }

/* ---- teardown (the QUIT sentinel path: vt_stop in vita_tool_main.c) ----------------------------------------------- */
void rhi_gxm_shutdown(void)
{
    if (g_stfx_backend != STFX_BK_GXM) return;
    gxm_frame_shutdown();
}
