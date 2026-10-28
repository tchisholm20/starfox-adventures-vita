/* rhi_gxm_split.c -- R5 (host/engine/R5_SPLIT.md): the GXM backend's side of the render-thread split.  This file only BINDS: the table of real calls R-back executes (GxspBackend, gx_rt_split_be.h) to the rhi_gxm_*
 * entry points, plus the pure queries the front answers without touching the backend.  Nothing here calls sceGxm*.
 *
 * Split-CAPABLE is decided once, at rhi_gxm_create (before the first texture / mesh exists): ux0:data/stairfax/gx_thread_split.txt starts with '1'.  Absent or anything else: this function returns at once, the
 * process never makes a proxy, and every shim costs one load and one predicted branch.  Whether the split is ON is gx_thread.c's decision (the arm, at a frame edge). */
#include "rhi_gxm_int.h"
#include "gx_rt_split_be.h"

#include <psp2/io/fcntl.h>
#include <string.h>

void gxsp_host_config(uint32_t ring_bytes, uint32_t arena_bytes, int audit) __attribute__((weak));   /* ditto */
int gxsp_cap_init(const GxspBackend *be) __attribute__((weak));   /* gx_rt_split.c; weak so the closure's trial link (which has not got it) resolves -- build_game_vita.sh names it in its symbol gate */

static RhiGxm s_exec;            /* R-back's own generic instance: the alpha mode a draw is issued with and the bound texture are per-thread state while the split is on */
static GxspBackend s_be;

static int s_audit;   /* the marker's second character 'a': every draw carries a hash of its key / uniform structs and the back compares its mirror (gx_rt_split.c audit) */
static int cap_marker(void)
{
    char b[8];
    SceUID fd = sceIoOpen("ux0:data/stairfax/gx_thread_split.txt", SCE_O_RDONLY, 0777);
    int n = 0;
    memset(b, 0, sizeof b);
    if (fd >= 0) { n = sceIoRead(fd, b, sizeof b - 1); sceIoClose(fd); }
    s_audit = n > 1 && b[1] == 'a';
    return fd >= 0 && n > 0 && b[0] == '1';
}

void gxsp_gxm_init(RhiInstance *inst)
{
    if (g_stfx_backend != STFX_BK_GXM || !cap_marker()) return;
    if (!gxsp_cap_init) { gxm_log("[gxsp] gx_thread_split.txt = 1 but gx_rt_split.o is not linked: NOT split-capable"); return; }
    s_exec = g_gxm_inst;
    s_exec.base.ops = inst->ops;
    s_be.inst = inst; s_be.ops = inst->ops; s_be.exec_inst = &s_exec.base;
    s_be.tex_max_dim = rhi_gxm_tex_max_dim();
    s_be.layout_ok = rhi_gxm_pure_layout_ok; s_be.mesh_check = rhi_gxm_pure_mesh_check; s_be.copy_ok = rhi_gxm_copy_precheck;
    s_be.setDepthState = rhi_gxm_setDepthState; s_be.setViewport = rhi_gxm_setViewport; s_be.setScissor = rhi_gxm_setScissor; s_be.setShadowZBias = rhi_gxm_setShadowZBias;
    s_be.clearRect = rhi_gxm_clearRect; s_be.texInfo = rhi_gxm_texInfo; s_be.texSetWrap = rhi_gxm_texSetWrap; s_be.texSetFilter = rhi_gxm_texSetFilter; s_be.texSetLodBias = rhi_gxm_texSetLodBias;
    s_be.texSetMips = rhi_gxm_texSetMips; s_be.copyEfb = rhi_gxm_copyEfb; s_be.copyTexSetFilter = rhi_gxm_copyTexSetFilter; s_be.copyTexSetSampler = rhi_gxm_copyTexSetSampler;
    s_be.copyEfbFree = rhi_gxm_copyEfbFree; s_be.readEfbRect = rhi_gxm_readEfbRect; s_be.drawTev = rhi_gxm_drawTev;
    s_be.meshCreate = rhi_gxm_gx_mesh_create; s_be.meshUpdate = rhi_gxm_gx_mesh_update; s_be.meshDestroy = rhi_gxm_gx_mesh_destroy;
    s_be.meshBegin = rhi_gxm_gx_mesh_update_begin_real; s_be.meshCommit = rhi_gxm_gx_mesh_update_commit_real;
    s_be.drawTevGx = rhi_gxm_drawTevGx_h; s_be.verdictGx = rhi_gxm_verdict_gx; s_be.verdictTev = rhi_gxm_verdict_tev; s_be.setR3a = rhi_gxm_set_r3a_ovr;
    if (s_audit && gxsp_host_config) gxsp_host_config(0u, 0u, 1);
    if (gxsp_cap_init(&s_be)) gxm_log("[gxsp] split-capable: gx_thread_split.txt = 1, handles are proxies (the split itself is switched on by the arm / threaded mode)");
    else gxm_log("[gxsp] split-capable REFUSED by gxsp_cap_init");
}
