/* gxm_stall.c -- WHERE DOES THE GAME THREAD WAIT FOR THE GPU?  (docs/54 R15, task C2-scenestall, 2026-10-19). A measurement instrument:
 * it draws nothing, changes no GL or GXM state, and passes every call through with the caller's own arguments. Default OFF.
 *
 * THE QUESTION. docs/54 R15: "the chapter-2 frame exceeds the GPU's 8-scenes-per-render-target limit, so part of the 'CPU-bound' 85 ms
 * may be the CPU WAITING". scene_gpu.c already prices sceGxmBeginScene/EndScene ("CPU inside BeginScene 0.37 ms/frame", run279.txt:1426 etc.);
 * what it cannot see is a stall INSIDE a draw (a full VDM / vertex ring or parameter buffer makes sceGxmDraw wait for the GPU to free
 * space), inside an explicit sync (sceGxmFinish = glFinish / glReadPixels), or in a notification wait (occlusion queries). Those sit in the
 * "GPU path 29.8 ms / 526 draws = 57 us a draw" column of [win] and are not separated from vitaGL's own CPU work there. This file times
 * exactly the four calls that can block on the GPU and prints, per 60-present window, how many calls, how long, and how many were long.
 *
 * HOW. Link-time wraps, the mechanism scene_gpu.c uses (build_game_vita.sh -Wl,--wrap=...): vitaGL's calls to sceGxmDraw / sceGxmDrawInstanced /
 * sceGxmFinish / sceGxmNotificationWait land in __wrap_X, which reads sceKernelGetProcessTimeWide() before and after the __real_X call.
 *   * RESOLUTION. The clock is 1 us. A normal draw is a few us, so per-call quantisation is +-1 us: the MEAN and the totals are sound (the
 *     quantisation is unbiased over many calls); a single call is not. The question here is the TAIL, and the tail is >= 200 us.
 *   * COST. Two clock reads a call, ~673 ns each on the device (scene_gpu.c measured it: "6 clock reads a scene at 673 ns each"); this file
 *     calibrates its own at init and prints the figure, and prints the reads' total (2 x timed calls x ns) so the window's wall can be
 *     corrected. At ~526 draws a frame that is ~0.7 ms/frame, inside the numbers below (they are not corrected for it).
 *   * SINGLE WRITER. The counters are plain (not atomic): vitaGL draws on the game thread; the boot splash draws on its own thread until the
 *     first scene, so the first window can lose a count. perf_ab drops the first windows (settle) and the first [stall] line is flagged warm-up.
 *
 * MARKER (ux0:data/stairfax/gxm_stall.txt, read once at boot):
 *     absent / anything else   OFF: each wrap is one load, one branch, a tail call
 *     "1"                      ON in every window
 *     "2"                      ON only while perf_ab arm 108 holds (gxm_stall_arm(present, arm) is called every present): the in-boot A/B/A
 *
 * OUTPUT, every 60 presents while on (one line, `[stall]`, never inside the [win] line: perf_ab.py's [win] regex is anchored on its columns):
 *   [stall] presents P..Q arm A mode M frames F mixed X | draw calls/frame N us/frame U mean u | big>200us n/frame c us/frame b | huge>1000us n/frame h | max us m |
 *           drawinst calls/frame ... | finish calls/frame ... | notifwait calls/frame ... | routes/frame tevdraw A tevflush B efbcopy C | hist(draw,us) <5:.. <10:.. <25:.. <50:.. <100:.. <200:.. <500:.. <1000:.. >=1000:.. |
 *           timed T calls, N ns/read, clock reads 2 x T x N = X ms/frame (inside the figures above)
 * `big` = calls longer than GS_BIG_US: the STALL MASS is the us/frame column on that group. A stall found here (mass >> 0.3 ms/frame) is
 * what R15 feared; mass ~0 closes it for the fragment-independent classes too. */
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gxm_stall.h"

#define GS_BIG_US   200u
#define GS_HUGE_US  1000u
#define GS_DIR      "ux0:data/stairfax/"
#define GS_ARM      108
#define GS_B        9                      /* histogram buckets: <5 <10 <25 <50 <100 <200 <500 <1000 >=1000 (us) */

/* The game's OWN counts of the GL draws it issues, read as weak externs (an unlinked one reads 0 and perf_ab calls the window INVALID). They let the [stall] line carry,
 * for the SAME 60 presents, the routes that each end in >= 1 sceGxmDraw, so a reader can see how much of the wrap's call count the game explains:
 *   rhi_vita_ab_draws         ++ per rhi_vita_drawTevGx (rhi_vita_tev.inc:1592), then one glDrawElements (tev.inc:1657) = vitaGL draw.c:381-387 = 1 sceGxmDraw (+ <= 2 hidden
 *                             scissor-mask draws, tests.c:314/336 via update_scissor_test, when the scissor is dirty)
 *   rhi_vita_tev_batches      ++ per tev_flush (tev.inc:1175), one glDrawArrays (tev.inc:1170) = vitaGL draw.c:214 = 1 sceGxmDraw (+ the same hidden draws)
 *   rhi_vita_efb_gpu_copies   ++ per EFB copy (efb.inc:941/951); most draw one quad (efb.inc:268), a re-used z-late copy draws none, so it is reported but never a lower bound
 * NOT counted by any of them: glClear's depth/stencil quad (misc.c:720), the UI batches (rhi_vita.c:372/726/1187), the present blit. */
extern unsigned rhi_vita_ab_draws __attribute__((weak));
extern unsigned rhi_vita_tev_batches __attribute__((weak));
extern unsigned rhi_vita_efb_gpu_copies __attribute__((weak));
static unsigned g_p_ab, g_p_fl, g_p_efb;   /* the counters' values at the window's start */
static unsigned gs_rd(const unsigned *p) { return p ? *p : 0u; }

enum { GS_DRAW, GS_DRAWI, GS_FINISH, GS_NOTIF, GS_N };
static const char *const k_name[GS_N] = { "draw", "drawinst", "finish", "notifwait" };
static const uint32_t k_edge[GS_B - 1] = { 5, 10, 25, 50, 100, 200, 500, 1000 };

typedef struct { uint64_t n, us, nbig, usbig, nhuge, max; uint64_t h[GS_B]; } GsStat;

static GsStat g_s[GS_N];
static volatile int g_on;            /* the marker asked */
static volatile int g_active;        /* timing now */
static int g_mode, g_win_arm = -2, g_arm_changed, g_frames, g_warm = 1;   /* g_win_arm: the arm seen at the window's interior presents (-2 = none yet) */
static uint32_t g_clock_ns;

int __real_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount);
int __real_sceGxmDrawInstanced(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData,
                               unsigned int indexCount, unsigned int indexWrap);
void __real_sceGxmFinish(SceGxmContext *context);
int __real_sceGxmNotificationWait(const SceGxmNotification *notification);

static inline uint64_t gs_now(void) { return sceKernelGetProcessTimeWide(); }

static inline void gs_rec(int k, uint32_t us)
{
    GsStat *s = &g_s[k];
    ++s->n; s->us += us;
    if (us > s->max) s->max = us;
    if (us > GS_BIG_US) { ++s->nbig; s->usbig += us; }
    if (us > GS_HUGE_US) ++s->nhuge;
    int b = 0;
    while (b < GS_B - 1 && us >= k_edge[b]) ++b;
    ++s->h[b];
}

/* ---- the wraps ---------------------------------------------------------------------------------- */
int __wrap_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount)
{
    if (!g_active) return __real_sceGxmDraw(context, primType, indexType, indexData, indexCount);
    const uint64_t t0 = gs_now();
    const int r = __real_sceGxmDraw(context, primType, indexType, indexData, indexCount);
    gs_rec(GS_DRAW, (uint32_t) (gs_now() - t0));
    return r;
}

int __wrap_sceGxmDrawInstanced(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData,
                               unsigned int indexCount, unsigned int indexWrap)
{
    if (!g_active) return __real_sceGxmDrawInstanced(context, primType, indexType, indexData, indexCount, indexWrap);
    const uint64_t t0 = gs_now();
    const int r = __real_sceGxmDrawInstanced(context, primType, indexType, indexData, indexCount, indexWrap);
    gs_rec(GS_DRAWI, (uint32_t) (gs_now() - t0));
    return r;
}

void __wrap_sceGxmFinish(SceGxmContext *context)
{
    if (!g_active) { __real_sceGxmFinish(context); return; }
    const uint64_t t0 = gs_now();
    __real_sceGxmFinish(context);
    gs_rec(GS_FINISH, (uint32_t) (gs_now() - t0));
}

int __wrap_sceGxmNotificationWait(const SceGxmNotification *notification)
{
    if (!g_active) return __real_sceGxmNotificationWait(notification);
    const uint64_t t0 = gs_now();
    const int r = __real_sceGxmNotificationWait(notification);
    gs_rec(GS_NOTIF, (uint32_t) (gs_now() - t0));
    return r;
}

/* ---- init / arm / window ------------------------------------------------------------------------ */
void gxm_stall_init(void)
{
    char b[8] = { 0 };
    if (g_on) return;
    SceUID f = sceIoOpen(GS_DIR "gxm_stall.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) { int n = sceIoRead(f, b, 1); sceIoClose(f); if (n < 1) b[0] = 0; }
    if (b[0] != '1' && b[0] != '2') { printf("[stall] sceGxm call timers: off (gxm_stall.txt absent or not 1/2)\n"); return; }
    g_mode = b[0] - '0';
    {   uint64_t t0 = gs_now(), t1 = t0;
        for (int i = 0; i < 2000; ++i) t1 = gs_now();
        g_clock_ns = (uint32_t) ((t1 - t0) * 1000u / 2000u);
    }
    g_on = 1;
    g_p_ab = gs_rd(&rhi_vita_ab_draws); g_p_fl = gs_rd(&rhi_vita_tev_batches); g_p_efb = gs_rd(&rhi_vita_efb_gpu_copies);
    g_active = g_mode == 1;    /* mode 2 starts OFF and follows the perf_ab arm */
    printf("[stall] sceGxm call timers ON (gxm_stall.txt mode %d%s): sceGxmDraw, sceGxmDrawInstanced, sceGxmFinish, sceGxmNotificationWait are timed, nothing added or replaced | "
           "one clock read %u ns | big > %u us, huge > %u us\n",
           g_mode, g_mode == 2 ? ", on only while perf_ab arm 108 holds" : "", (unsigned) g_clock_ns, GS_BIG_US, GS_HUGE_US);
}

void gxm_stall_arm(int present, int arm)
{
    if (!g_on) return;
    /* The arm that holds for window P..Q is set AT present P (vt_perf_ab), the same present that closes the window before it, and which of the two hooks runs
     * first at that present is not something to depend on. So the window's arm is read from its INTERIOR presents only (P+1 .. Q-1), and an arm that differs
     * between two interior presents makes the window `mixed`. The gate itself follows every call: draws only happen between presents. */
    if (present % 60 != 0) {
        if (g_win_arm == -2) g_win_arm = arm;
        else if (arm != g_win_arm) g_arm_changed = 1;
    }
    if (g_mode == 2) g_active = arm == GS_ARM;
}

static double gs_per(uint64_t v, int frames) { return frames ? (double) v / frames : 0.0; }

static void gs_line(char *buf, size_t cap, int from, int to, int frames)
{
    const GsStat *d = &g_s[GS_DRAW];
    uint64_t timed = 0;
    for (int k = 0; k < GS_N; ++k) timed += g_s[k].n;
    int o = snprintf(buf, cap, "[stall] presents %d..%d arm %d mode %d frames %d mixed %d%s | ", from, to, g_win_arm == -2 ? -1 : g_win_arm, g_mode, frames, g_arm_changed ? 1 : 0, g_warm ? " warmup" : "");
    o += snprintf(buf + o, cap - (size_t) o, "draw calls/frame %.1f us/frame %.0f mean %.2f | big>%uus n/frame %.2f us/frame %.0f | huge>%uus n/frame %.2f | max us %u | ",
                  gs_per(d->n, frames), gs_per(d->us, frames), d->n ? (double) d->us / d->n : 0.0,
                  GS_BIG_US, gs_per(d->nbig, frames), gs_per(d->usbig, frames), GS_HUGE_US, gs_per(d->nhuge, frames), (unsigned) d->max);
    for (int k = GS_DRAWI; k < GS_N; ++k) {
        const GsStat *s = &g_s[k];
        o += snprintf(buf + o, cap - (size_t) o, "%s calls/frame %.2f us/frame %.0f max us %u big n/frame %.2f | ", k_name[k], gs_per(s->n, frames), gs_per(s->us, frames), (unsigned) s->max, gs_per(s->nbig, frames));
    }
    o += snprintf(buf + o, cap - (size_t) o, "routes/frame tevdraw %.2f tevflush %.2f efbcopy %.2f | ",
                  frames ? (double) (gs_rd(&rhi_vita_ab_draws) - g_p_ab) / frames : 0.0, frames ? (double) (gs_rd(&rhi_vita_tev_batches) - g_p_fl) / frames : 0.0,
                  frames ? (double) (gs_rd(&rhi_vita_efb_gpu_copies) - g_p_efb) / frames : 0.0);
    o += snprintf(buf + o, cap - (size_t) o, "hist(draw,us)");
    for (int b = 0; b < GS_B; ++b) {
        if (b < GS_B - 1) o += snprintf(buf + o, cap - (size_t) o, " <%u:%llu", (unsigned) k_edge[b], (unsigned long long) d->h[b]);
        else              o += snprintf(buf + o, cap - (size_t) o, " >=%u:%llu", (unsigned) k_edge[b - 1], (unsigned long long) d->h[b]);
    }
    o += snprintf(buf + o, cap - (size_t) o, " | timed %llu calls, %u ns/read, clock reads 2 x %llu x %u = %.2f ms/frame (inside the figures above)",
                  (unsigned long long) timed, (unsigned) g_clock_ns, (unsigned long long) timed, (unsigned) g_clock_ns,
                  frames ? 2.0 * (double) timed * g_clock_ns / 1e6 / frames : 0.0);
}

void gxm_stall_present(int present)
{
    if (!g_on) return;
    ++g_frames;
    if (present % 60 != 0) return;
    {   /* window [present-60, present] closed */
        uint64_t any = 0;
        for (int k = 0; k < GS_N; ++k) any += g_s[k].n;
        if (any) {                       /* a mode-2 window with the timers off prints nothing: its absence is what perf_ab reads as "arm 108 did not run" */
            char line[1600];
            gs_line(line, sizeof line, present - 60, present, g_frames);
            printf("%s\n", line);
        }
    }
    memset(g_s, 0, sizeof g_s);
    g_p_ab = gs_rd(&rhi_vita_ab_draws); g_p_fl = gs_rd(&rhi_vita_tev_batches); g_p_efb = gs_rd(&rhi_vita_efb_gpu_copies);
    g_frames = 0; g_arm_changed = 0; g_win_arm = -2; g_warm = 0;
}
