/* scene_gpu.c -- PER-GXM-SCENE GPU TIME (2026-10-11). A measurement instrument: it draws nothing
 * and changes no GL or GXM state. Default OFF.
 *
 * THE QUESTION. At 640x368 in the scripted ShipBattle fight, where does the GPU time go PER SCENE?
 * ~8 EFB copies and 18 FBO switches a frame, M runs with a depth force-load/store, one copy is a
 * full-screen Z copy. Structural overhead (scene breaks, depth load/store, the copies) >= 40% of the
 * frame's GPU time -> a native GXM backend is justified; < 20% -> the lever is TEV shader cost and
 * overdraw; between -> report.
 *
 * HOW. No vitaGL patch: the link wraps sceGxmBeginScene and sceGxmEndScene
 * (build_game_vita.sh: -Wl,--wrap=sceGxmBeginScene -Wl,--wrap=sceGxmEndScene), and every scene
 * vitaGL makes passes through here. vitaGL has no sceGxmBeginSceneEx call (its only scene calls
 * are gxm.c:567/620/670 and the boot splash screen's own, splashscreen.c:6402/6447).
 *
 * THE NOTIFICATION IS vitaGL's OWN. vitaGL ends EVERY scene with
 *     query_fence.value++;  sceGxmEndScene(gxm_context, NULL, &query_fence);      (gxm.c:566-567)
 * where query_fence.address is word 0 of sceGxmGetNotificationRegion() (buffers.c:82-84): the GPU
 * writes the scene's number there when its FRAGMENT job completes, a monotonic completed-scene
 * counter (occlusion queries and the local GL_ARB_sync patch read it). So nothing is added, dropped
 * or replaced: the wrap reads the notification it is handed (address, value) and passes the call on
 * untouched; the scene is complete when *address has reached its value. A scene ended WITHOUT a
 * fragment notification (only the splash screen does that, at boot) is counted "untimed" and left
 * alone -- no notification of ours is put on it.
 *
 * COMPLETION TIME: POLLED, not waited. A thread at the port's one legal priority (0x10000100) reads
 * the counter every scene_gpu_poll.txt microseconds (default 100) against sceKernelGetProcessTimeWide.
 * sceGxmNotificationWait was NOT used: the notification is vitaGL's shared one, its value moves on
 * with every scene, and what a wait on a value that has already been passed does is not documented
 * -- a waiter that can miss its wake-up is not an instrument. The price is resolution: a completion
 * is stamped at the MIDDLE of the poll gap it fell in, +- half that gap, and the gaps are MEASURED
 * and printed with every block (p50 / p90 / max), together with how many completions shared a poll.
 *
 * SCENE TYPE = (target kind, depth loaded/stored, why it ended). The kind and the reason are tags the
 * backend sets at each glBindFramebuffer (rhi_vita.c RHI_SCENE_TAG): vitaGL begins and ends scenes
 * LAZILY, at the first draw after a bind, so the tag current at BeginScene names the target and the
 * tag current at EndScene says what the scene was left for. The display is cross-checked against a
 * fact the wrap sees itself (only the display scene carries a fragment sync object); disagreements
 * are counted and printed. Depth load/store is read from the depth-stencil surface handed to
 * BeginScene (sceGxmDepthStencilSurfaceGetForceLoadMode / ...StoreMode: what efb_try_hack sets on M).
 *
 * MARKERS (ux0:data/stairfax/, read once at boot):
 *   scene_gpu.txt        "1" on | "2 N" on + every scene of present N printed in order | "3" in-boot
 *                        A/B/A: the instrument alternates off/on every 60 presents (the [win] windows)
 *   scene_gpu_poll.txt   poll period in microseconds (default 100; 0 = spin, costs a core)
 *   scene_gpu_drain.txt  "1" = DRAIN ARM: sceGxmFinish after every scene, timed -- an independent
 *                        per-scene reference for the notification times (frame times are not the game's)
 *   scene_gpu_break.txt  "K [what]" = BREAK ARM, read by the backend (rhi_vita_efb.inc)
 *   vsync_off.txt        "1" = vglWaitVblankStart(GL_FALSE) after init (rhi_vita.c)
 * Absent scene_gpu.txt: the wraps are one load and one branch, no thread exists. */
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scene_gpu_book.h"
#include "scene_gpu.h"

/* the backend's side (rhi_vita.c) */
extern int rhi_vita_scene_kind, rhi_vita_scene_why;
extern int rhi_vita_scene_break_k, rhi_vita_scene_break_what, rhi_vita_vsync_off;
extern unsigned rhi_vita_scene_break_us, rhi_vita_scene_probe_us;
extern void rhi_vita_timing(SceUInt64 out[4]);
extern void rhi_vita_render_size(int *w, int *h);

int __real_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget,
                            const SceGxmValidRegion *validRegion, SceGxmSyncObject *vertexSyncObject,
                            SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil);
int __real_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification,
                          const SceGxmNotification *fragmentNotification);

#define SG_DIR   "ux0:data/stairfax/"
#define SG_RING  4096u            /* scenes submitted and not yet aggregated; its occupancy and drops are printed */
#define SG_MASK  (SG_RING - 1u)
#define SG_FRMS  64u
#define SG_LOST_US 2000000u       /* a scene whose notification has not come in 2 s is given up on, and counted */
#define SG_GAPB  41               /* poll-gap histogram: 25 us buckets up to 1 ms, then one for everything above */

typedef struct {
    uint64_t submit;              /* just before the real sceGxmEndScene */
    volatile uint64_t done;       /* 0 = pending; written by the poll thread */
    volatile uint32_t *addr; uint32_t token;
    uint32_t begin_us, end_us, drain_us;
    const void *rt;
    uint8_t kind, ls, why, lost;
} SgRec;
typedef struct { uint32_t first, n, wall, cpu, swap; int present; int skip; } SgFrm;

static volatile int g_on, g_active;   /* on = the marker asked; active = recording now (mode 3 toggles it) */
static int g_mode, g_drain, g_dump_present = -1;
static uint32_t g_poll_us = 100, g_clock_ns;
static SgRec *g_ring;
static volatile uint32_t g_w, g_p;    /* written (game thread), stamped (poll thread) */
static uint32_t g_a;                  /* aggregated (game thread) */
static volatile char g_lk;
static SgFrm g_frm[SG_FRMS]; static uint32_t g_fw, g_fa, g_frame_first;
static SgbWin *g_win;
static struct { SceGxmContext *ctx; const void *rt; uint32_t begin_us; uint8_t kind, ls, open; } g_cur;
static uint64_t g_prev_done, g_last_t, g_last_tm[2], g_wrap_us, g_wrap_scenes, g_agg_us;
static uint32_t g_dropped, g_untimed, g_mismatch, g_dropped_frames, g_skipped_frames;
static volatile uint32_t g_lost, g_coalesced, g_polls, g_gap_max, g_gap_h[SG_GAPB];
static int g_win_from, g_edge;
static SceUID g_thr = -1; static volatile int g_stop;

static inline uint64_t sg_now(void) { return sceKernelGetProcessTimeWide(); }

/* ---- the wraps ---------------------------------------------------------------------------------- */
int __wrap_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget,
                            const SceGxmValidRegion *validRegion, SceGxmSyncObject *vertexSyncObject,
                            SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil)
{
    if (!g_active)
        return __real_sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject, fragmentSyncObject, colorSurface, depthStencil);
    const uint64_t t0 = sg_now();
    uint8_t kind = (uint8_t) rhi_vita_scene_kind, ls = 0;
    /* Only the display scene has a fragment sync object (gxm.c:620-624; an FBO scene passes NULL,
     * gxm.c:670-675). That fact outranks the tag, and every disagreement is counted. */
    if (fragmentSyncObject) { if (kind != 'D') { ++g_mismatch; kind = 'D'; } }
    else if (kind == 'D')   { ++g_mismatch; kind = '?'; }
    if (depthStencil) {
        if (sceGxmDepthStencilSurfaceGetForceLoadMode(depthStencil) == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED) ls |= SGB_F_LOAD;
        if (sceGxmDepthStencilSurfaceGetForceStoreMode(depthStencil) == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED) ls |= SGB_F_STORE;
    }
    g_cur.ctx = context; g_cur.rt = renderTarget; g_cur.kind = kind; g_cur.ls = ls;
    rhi_vita_scene_why = 'o';   /* whatever ended the scene before this one has been used; a tag set from here on is this scene's */
    const uint64_t t1 = sg_now();
    int r = __real_sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject, fragmentSyncObject, colorSurface, depthStencil);
    g_cur.begin_us = (uint32_t) (sg_now() - t1);   /* a render target used for more scenes than it was made for WAITS in here */
    g_cur.open = 1;
    g_wrap_us += t1 - t0;
    return r;
}

int __wrap_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification,
                          const SceGxmNotification *fragmentNotification)
{
    if (!g_active) return __real_sceGxmEndScene(context, vertexNotification, fragmentNotification);
    const uint64_t t0 = sg_now();
    SgRec *r = NULL; uint32_t w = 0;
    const int timed = fragmentNotification && fragmentNotification->address;
    if (!timed) ++g_untimed;
    else {
        while (__atomic_test_and_set(&g_lk, __ATOMIC_ACQUIRE)) { }
        w = g_w;
        if (w - g_a >= SG_RING - 1u) ++g_dropped;
        else {
            r = &g_ring[w & SG_MASK];
            r->done = 0; r->lost = 0; r->end_us = 0; r->drain_us = 0;
            r->addr = fragmentNotification->address; r->token = fragmentNotification->value;
            if (g_cur.open && g_cur.ctx == context) { r->kind = g_cur.kind; r->ls = g_cur.ls; r->rt = g_cur.rt; r->begin_us = g_cur.begin_us; }
            else { r->kind = '?'; r->ls = 0; r->rt = NULL; r->begin_us = 0; }   /* a scene begun while the instrument was off */
            r->why = (uint8_t) rhi_vita_scene_why;
        }
    }
    rhi_vita_scene_why = 'o';   /* consumed: the next scene's end is "other" until a bind says otherwise */
    g_cur.open = 0;
    const uint64_t t1 = sg_now();
    if (r) { r->submit = t1; __sync_synchronize(); g_w = w + 1u; }   /* published BEFORE the kick: the poller may see it complete at once */
    if (timed) __atomic_clear(&g_lk, __ATOMIC_RELEASE);
    int rc = __real_sceGxmEndScene(context, vertexNotification, fragmentNotification);   /* vitaGL's arguments, untouched */
    if (r) r->end_us = (uint32_t) (sg_now() - t1);
    if (g_drain) { sceGxmFinish(context); if (r) r->drain_us = (uint32_t) (sg_now() - t1); }
    g_wrap_us += t1 - t0; ++g_wrap_scenes;
    return rc;
}

/* ---- the poll thread ---------------------------------------------------------------------------- */
static int sg_poll_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    uint64_t last = sg_now();
    while (!g_stop) {
        const uint64_t t = sg_now();
        const uint32_t w = g_w; uint32_t p = g_p, k = 0;
        if (g_active) {
            const uint32_t gap = (uint32_t) (t - last); uint32_t b = gap / 25u;
            if (b >= SG_GAPB) b = SG_GAPB - 1;
            ++g_gap_h[b]; ++g_polls;
            if (gap > g_gap_max) g_gap_max = gap;
        }
        while (p != w) {
            SgRec *r = &g_ring[p & SG_MASK];
            if ((int32_t) (*r->addr - r->token) >= 0) {
                /* it completed somewhere in (max(last poll, its submit), now]: the middle of that */
                const uint64_t lo = last > r->submit ? last : r->submit;
                r->done = t > lo ? lo + (t - lo) / 2u : t;
            } else if (t > r->submit && t - r->submit > SG_LOST_US) { r->lost = 1; r->done = t; ++g_lost; }
            else break;
            ++p; ++k;
        }
        if (k > 1u) g_coalesced += k - 1u;
        if (k) { __sync_synchronize(); g_p = p; }
        last = t;
        if (p == w && !g_active) sceKernelDelayThread(2000);
        else if (g_poll_us) sceKernelDelayThread(g_poll_us);
    }
    return sceKernelExitThread(0);
}

/* ---- aggregation (game thread, at the present) -------------------------------------------------- */
static void sg_aggregate(void)
{
    while (g_fa != g_fw) {
        const SgFrm *f = &g_frm[g_fa % SG_FRMS];
        if ((int32_t) (g_p - (f->first + f->n)) < 0) break;   /* the GPU has not finished this frame yet */
        const int dump = f->present == g_dump_present;
        uint64_t base = 0;
        if (dump) printf("[scene] raw present %d: %u scenes, wall %.3f frame CPU %.3f swap %.3f ms | per scene: submit and done in ms after the frame's first submit; gpu = done - max(previous done, submit)\n",
                         f->present, (unsigned) f->n, f->wall / 1000.0, f->cpu / 1000.0, f->swap / 1000.0);
        for (uint32_t i = 0; i < f->n; ++i) {
            const SgRec *r = &g_ring[(f->first + i) & SG_MASK];
            const uint32_t gpu = sgb_scene_us(r->done, g_prev_done, r->submit);
            if (!i) base = r->submit;
            if (dump) printf("[scene] raw %3u %-8s %c%c %-12s rt %p | submit %9.3f done %9.3f prev-done %9.3f | gpu %8.3f ms | CPU in BeginScene %.3f EndScene %.3f%s | token %u%s\n",
                             (unsigned) i, sgb_kind_name(r->kind), r->ls & SGB_F_LOAD ? 'L' : '-', r->ls & SGB_F_STORE ? 'S' : '-', sgb_why_name(r->why), r->rt,
                             (double) (int64_t) (r->submit - base) / 1000.0, (double) (int64_t) (r->done - base) / 1000.0,
                             g_prev_done ? (double) (int64_t) (g_prev_done - base) / 1000.0 : 0.0, gpu / 1000.0,
                             r->begin_us / 1000.0, r->end_us / 1000.0, g_drain ? " (drain arm)" : "", (unsigned) r->token, r->lost ? " LOST" : "");
            if (!f->skip && !r->lost) sgb_add_scene(g_win, r->kind, r->ls, r->why, gpu, r->begin_us, r->end_us, r->drain_us);
            g_prev_done = r->done;
        }
        if (!f->skip) sgb_end_frame(g_win, f->wall, f->cpu, f->swap); else ++g_skipped_frames;
        g_a = f->first + f->n;
        ++g_fa;
    }
}

void scene_gpu_present(int present)
{
    if (!g_on) return;
    const uint64_t t = sg_now();
    SceUInt64 tm[4]; rhi_vita_timing(tm);
    const uint32_t wall = g_last_t ? (uint32_t) (t - g_last_t) : 0u;
    const uint32_t cpu = (uint32_t) (tm[0] - g_last_tm[0]), swap = (uint32_t) (tm[3] - g_last_tm[1]);
    g_last_t = t; g_last_tm[0] = tm[0]; g_last_tm[1] = tm[3];
    int skip = !wall || g_edge > 0;
    if (g_edge > 0) --g_edge;
    if (g_mode == 3 && present > 0) {
        /* the frame that just ended ran under the state set at the previous present: phase (present-1)/60.
         * The first two frames of every phase are left out of both sides (the GPU still holds the other's work). */
        if (wall && (present - 1) % 60 >= 2) sgb_ab_frame(g_win, (unsigned) ((present - 1) / 60), wall, cpu, swap);
        const int want = (present / 60) & 1;
        if (want != g_active) { g_cur.open = 0; g_active = want; g_edge = 2; }
    }
    const uint32_t w = g_w;
    if (w != g_frame_first || g_active) {
        if (g_fw - g_fa >= SG_FRMS) ++g_dropped_frames;   /* the GPU is > 64 frames behind: this frame's scenes are skipped over */
        else {
            SgFrm *f = &g_frm[g_fw % SG_FRMS];
            f->first = g_frame_first; f->n = w - g_frame_first; f->wall = wall; f->cpu = cpu; f->swap = swap; f->present = present; f->skip = skip;
            ++g_fw;
        }
    }
    g_frame_first = w;
    sg_aggregate();
    g_agg_us += sg_now() - t;
}

static void sg_emit(const char *line) { printf("%s\n", line); }

void scene_gpu_report(int present)
{
    if (!g_on) return;
    SgbCtx c; memset(&c, 0, sizeof c);
    c.present_from = g_win_from; c.present_to = present; g_win_from = present;
    c.clk[0] = scePowerGetArmClockFrequency(); c.clk[1] = scePowerGetBusClockFrequency();
    c.clk[2] = scePowerGetGpuClockFrequency(); c.clk[3] = scePowerGetGpuXbarClockFrequency();
    rhi_vita_render_size(&c.rw, &c.rh);
    c.vsync_off = rhi_vita_vsync_off; c.drain = g_drain; c.mode = g_mode;
    c.break_k = rhi_vita_scene_break_k; c.break_what = rhi_vita_scene_break_what;
    c.break_drained_us = rhi_vita_scene_break_us; c.probe_drained_us = rhi_vita_scene_probe_us;
    c.poll_us = g_poll_us;
    {   uint32_t h[SG_GAPB], n = 0, acc = 0; int got50 = 0, got90 = 0;
        for (int i = 0; i < SG_GAPB; ++i) { h[i] = g_gap_h[i]; g_gap_h[i] = 0; n += h[i]; }
        for (int i = 0; i < SG_GAPB && n; ++i) {
            acc += h[i];
            if (!got50 && acc * 2u >= n) { c.gap_p50 = (uint32_t) (i + 1) * 25u; got50 = 1; }   /* the bucket's upper edge */
            if (!got90 && acc * 10u >= n * 9u) { c.gap_p90 = (uint32_t) (i + 1) * 25u; got90 = 1; }
        }
        c.polls = g_polls; g_polls = 0; c.gap_max = g_gap_max; g_gap_max = 0;
    }
    c.coalesced = g_coalesced; g_coalesced = 0; c.lost = g_lost; g_lost = 0;
    c.dropped = g_dropped; g_dropped = 0; c.untimed = g_untimed; g_untimed = 0; c.mismatch = g_mismatch; g_mismatch = 0;
    c.pending_frames = g_fw - g_fa; c.dropped_frames = g_dropped_frames; g_dropped_frames = 0;
    c.skipped_frames = g_skipped_frames; g_skipped_frames = 0;
    c.wrap_us = g_wrap_us; g_wrap_us = 0; c.wrap_scenes = g_wrap_scenes; g_wrap_scenes = 0; c.agg_us = g_agg_us; g_agg_us = 0;
    c.clock_ns = g_clock_ns;
    sgb_report(g_win, &c, sg_emit);
}

/* ---- init / shutdown ---------------------------------------------------------------------------- */
static int sg_read(const char *name, char *b, int cap)
{
    char path[96];
    snprintf(path, sizeof path, SG_DIR "%s", name);
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (f < 0) return 0;
    int n = sceIoRead(f, b, (SceSize) (cap - 1));
    sceIoClose(f);
    if (n < 0) n = 0;
    b[n] = 0;
    return 1;
}

void scene_gpu_init(void)
{
    char b[32];
    if (g_on || g_ring) return;
    if (!sg_read("scene_gpu.txt", b, sizeof b) || b[0] < '1' || b[0] > '3') {
        printf("[scene] per-scene GPU time: off (scene_gpu.txt absent or not 1/2/3)\n");
        return;
    }
    g_mode = b[0] - '0';
    if (g_mode == 2) { int m = 0, n = -1; if (sscanf(b, "%d %d", &m, &n) == 2 && n > 0) g_dump_present = n; }
    if (sg_read("scene_gpu_poll.txt", b, sizeof b)) { int v = atoi(b); if (v >= 0 && v <= 5000) g_poll_us = (uint32_t) v; }
    if (sg_read("scene_gpu_drain.txt", b, sizeof b) && b[0] == '1') g_drain = 1;
    g_ring = (SgRec *) calloc(SG_RING, sizeof *g_ring);
    g_win = (SgbWin *) calloc(1, sizeof *g_win);
    if (!g_ring || !g_win) { printf("[scene] per-scene GPU time: OFF, out of memory (%u + %u bytes)\n", (unsigned) (SG_RING * sizeof *g_ring), (unsigned) sizeof *g_win); return; }
    g_win->rng = 12345u;
    {   uint64_t t0 = sg_now(), t1 = t0;
        for (int i = 0; i < 2000; ++i) t1 = sg_now();
        g_clock_ns = (uint32_t) ((t1 - t0) * 1000u / 2000u);
    }
    g_thr = sceKernelCreateThread("stfx_scene_gpu", sg_poll_thread, 0x10000100, 0x4000, 0, 0, NULL);   /* the port's one legal priority (run 171) */
    int rs = g_thr >= 0 ? sceKernelStartThread(g_thr, 0, NULL) : g_thr;
    if (rs < 0) { printf("[scene] per-scene GPU time: OFF, the poll thread did not start (create %08x start %08x)\n", (unsigned) g_thr, (unsigned) rs); g_thr = -1; return; }
    g_on = 1;
    g_active = g_mode != 3;   /* mode 3 starts in an OFF phase (presents 0..59) */
    printf("[scene] per-scene GPU time ON (scene_gpu.txt mode %d%s): vitaGL's own fragment notification is timed, nothing added or replaced | poll thread prio 0x10000100, period %u us%s | one clock read %u ns | ring %u scenes, %u bytes | drain arm %s\n",
           g_mode, g_mode == 3 ? ", in-boot A/B/A by 60-present phases" : "", (unsigned) g_poll_us, g_poll_us ? "" : " (SPIN: a core)", (unsigned) g_clock_ns,
           (unsigned) SG_RING, (unsigned) (SG_RING * sizeof *g_ring + sizeof *g_win), g_drain ? "ON (scene_gpu_drain.txt)" : "off");
    if (g_dump_present > 0) printf("[scene] every scene of present %d will be printed\n", g_dump_present);
}

/* Asks the poll thread to stop and waits <= 1.5 s for it; it is never killed. */
void scene_gpu_shutdown(void)
{
    if (g_thr < 0) return;
    g_active = 0; g_stop = 1;
    SceUInt to = 1500000; int st = 0;
    int r = sceKernelWaitThreadEnd(g_thr, &st, &to);
    if (r >= 0) sceKernelDeleteThread(g_thr);
    printf("[scene] poll thread %s (wait %08x)\n", r >= 0 ? "stopped" : "did NOT stop in 1.5 s: left alone", (unsigned) r);
    g_thr = -1; g_on = 0;
}
