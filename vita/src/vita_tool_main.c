/* vita_tool_main.c - runs an upstream viewer tool (model_view.c, map_view.cpp)
 * on the Vita. ONE wrapper, compiled once per tool with -DVT_TOOL=<name>_main
 * and the VT_* paths below; see CMakeLists.txt. The model_view build also
 * applies the build-time model_view fixes listed there.
 *
 * model_view.c decodes a real Star Fox Adventures model from the disc
 * (MODELS.bin + TEX1.bin, the render-instruction stream, joint bind poses) and
 * draws it through rhi_drawTextured -- exactly the four texture ops milestone 3
 * verified. It is compiled from the upstream worktree with `main` renamed to
 * model_view_main (a per-file -D), so a change upstream is picked up by a
 * rebuild and a break is real news.
 *
 * THE THREE PLACES THIS FILE STANDS BETWEEN UPSTREAM AND THE CONSOLE, all named:
 *
 *  1. argv. A Vita app gets no command line. The arguments come from
 *     ux0:data/stairfax/args.txt when it exists (whitespace-separated), else a
 *     default, and are ALWAYS printed so a report says which model it shows.
 *
 *  2. rhi_create is WRAPPED (-Wl,--wrap=rhi_create). model_view's
 *     parse_backend() maps every name except vk/d3d12 to D3D11 and rhi_create
 *     has no fallback, so unwrapped it asks for D3D11, gets NULL and exits 7.
 *     The Vita has one backend; the wrapper routes every request to AUTO (which
 *     is GL here) and logs what was asked for, so the substitution is visible.
 *
 *  3. rhi_present is WRAPPED to capture BEFORE the swap. model_view's own
 *     --capture runs AFTER rhi_present, and docs/47 records why that reads the
 *     wrong buffer: a plausible image of a different frame. It is therefore not
 *     passed; the capture here is taken pre-swap, on a fixed present count.
 *
 * Output: stdout/stderr -> ux0:data/stairfax_model.txt (unbuffered, so a crash
 * still leaves every line written before it), the capture ->
 * ux0:data/stairfax_model.bmp, and a coverage/colour-variety grade of that
 * capture in the same report.
 */

#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>   /* sceKernelGetCpuId, SCE_KERNEL_CPU_MASK_USER_n: the T0-d co-runner */
#include "log_async.h"
#include <psp2/power.h>
#include <vitaGL.h>
#include "gx_rt.h"   /* R8-MF: g_gxt.seq / r_presented / on for the GPU-skin retire tick */
#include "stfx_backend.h"   /* g_stfx_backend: the gxm backend path must never reach glReadPixels */
#ifdef VT_SETENV_ISO
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include "stfx_disc_msgs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int stfx_retail_load(const char *iso, char *why, int cap);   /* hal/dvd_shim.c */
/* The "bring your own disc" refusal: one of gen_disc_msgs.py's 1-bit screens, drawn into a CDRAM framebuffer and shown; then the app idles until the
 * player closes it (PS button). Never returns. Nothing else is initialised yet (no GXM, no game), so this owns the display. */
static void vt_disc_error_screen(int code)
{
    const unsigned w = STFX_DISC_MSG_W, h = STFX_DISC_MSG_H, bytes = (w * h * 4u + 0x3FFFFu) & ~0x3FFFFu;
    const unsigned char *bits = (code >= 1 && code <= 4) ? stfx_disc_msg[code] : stfx_disc_msg[4];
    SceUID mb = sceKernelAllocMemBlock("stfx_disc_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, NULL);
    void *base = NULL;
    if (mb >= 0 && sceKernelGetMemBlockBase(mb, &base) >= 0 && base) {
        uint32_t *px = (uint32_t *) base;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                px[y * w + x] = (bits[y * (w / 8u) + x / 8u] >> (7u - (x & 7u)) & 1u) ? 0xFFFFFFFFu : 0xFF201008u;   /* white on deep blue (A8B8G8R8) */
        SceDisplayFrameBuf fb; memset(&fb, 0, sizeof fb);
        fb.size = sizeof fb; fb.base = base; fb.pitch = w; fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = w; fb.height = h;
        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    }
    fflush(stdout);
    for (;;) {   /* the player closes it with the PS button; the lane's quit-app.ps1 drops ux0:data/STFX00001/QUIT -- honoured here too, so a test boot ends cleanly */
        SceUID q = sceIoOpen("ux0:data/STFX00001/QUIT", SCE_O_RDONLY, 0777);
        if (q >= 0) { sceIoClose(q); sceIoRemove("ux0:data/STFX00001/QUIT"); printf("[vita] disc error screen: QUIT sentinel, exiting\n"); fflush(stdout); sceKernelExitProcess(0); }
        sceKernelDelayThread(500000);
    }
}
#endif
void rhi_gxm_shutdown(void);   /* rhi_gxm.c: reverse-order teardown, bounded waits; a no-op unless the gxm backend is up */
/* THE GX RENDER THREAD (gx_thread.c, RT_THREAD.md 7.1 item 3). Every reference is WEAK: the viewers link this file without gx_thread.o, and a game link without a marker
 * (gx_thread.txt absent) is DIRECT, where gxt_is_active() is 0 and each hook below is one not-taken branch. */
struct RhiInstance; struct RhiSwapchain;
extern int  gxt_present(struct RhiInstance *rhi, struct RhiSwapchain *sc) __attribute__((weak));   /* DIRECT: __real_rhi_present; THREADED: PRESENT record + the frame-ahead wait */
extern void gxt_quit(const char *why) __attribute__((weak));                                       /* FIRST in every exit route; idempotent; bounded join of R */
extern volatile int gxt_r_unsafe __attribute__((weak));                                            /* 1 = R did not stop: the backend teardown must NOT run */
extern void gxt_stall_report(void) __attribute__((weak));
extern int  gxt_is_active(void) __attribute__((weak));

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VT_TOOL
#error "build with -DVT_TOOL=<tool>_main -DVT_NAME=... -DVT_LOG=... -DVT_SHOT=... -DVT_ARGS=..."
#endif
#define LOG      VT_LOG
#define ARGS     VT_ARGS
#ifdef VT_DISC
#define DISC     VT_DISC
#else
#define DISC     "ux0:data/stairfax/disc"
#endif
#define SHOT     VT_SHOT
#define VT_STR2(x) #x
#define VT_STR(x)  VT_STR2(x)
/* The tool's clear colour in 8-bit codes, so the coverage grade knows what
 * "background" is. model_view: (0.10,0.11,0.13); map_view: (0.06,0.07,0.10). */
#ifndef VT_CLEAR
#define VT_CLEAR 26, 28, 33
#endif
static const int kClear[3] = { VT_CLEAR };

/* The run ends on WALL-CLOCK time, not a frame count. A frame count assumed
 * 60 fps: the first map run (9,110 draw calls a frame) was still going minutes
 * later and held eboot.bin, so the next push 550'd "File not found" (the lane
 * refused the launch, correctly). RUN_FRAMES stays as a hard backstop.
 * START+SELECT still quits early. */
#ifndef RUN_SECONDS
#define RUN_SECONDS    20
#endif
#ifndef RUN_FRAMES
#define RUN_FRAMES     1200
#endif
#ifndef CAPTURE_AT
#define CAPTURE_AT     60
#endif
/* Frame time is measured over presents [TIME_FROM, CAPTURE_AT) -- after load
 * and warm-up, and BEFORE the capture frame, whose glReadPixels + 1.5 MB BMP
 * write would dominate any average it fell inside. */
#ifndef TIME_FROM
#define TIME_FROM      10
#endif

/* model_view mallocs 2 MB per texture group plus the whole MODELS/TEX1 files;
 * the host dry run of model 0 needs ~7 MB. 32 MB is headroom without starving
 * vitaGL, whose pools come from outside this heap. */
#ifndef VT_HEAP_MB
#define VT_HEAP_MB 32
#endif
unsigned int _newlib_heap_size_user = VT_HEAP_MB * 1024 * 1024;

/* ---- stdout -> LOG, at LINK time ------------------------------------------
 *
 * ⚠ The stdio route does NOT work on this toolchain, measured over three device
 * runs: freopen(LOG, "a", stdout) and `stdout = fopen(LOG, ...)` both left a
 * 0-byte file, while a sceIo breadcrumb in the same run proved fopen(LOG)
 * itself returned a valid FILE. The open works; printf simply never uses it.
 *
 * So the print family is --wrap'd and every call goes straight to sceIo,
 * append, one open/close per call (slow, and only ~40 lines are printed, so it
 * does not matter; it also means a crash cannot lose buffered lines). puts and
 * putchar are wrapped because gcc rewrites printf("text\n") -> puts("text")
 * and printf("c") -> putchar('c'); missing them would silently drop lines. */
#include <stdarg.h>

/* While the draw journal runs (one frame, ~7,000 lines), lines are HELD in RAM and written in one
 * go when it ends: at one open/close per line the frame took minutes (run 83 lost most of its
 * journal when the console left the network mid-frame). Outside the journal every line still goes
 * straight to the file, so a crash cannot lose it. */
static char *g_log_hold; static int g_log_hold_len, g_log_hold_cap;
static void log_write_now(const char *s, int n);
static void log_flush_all(void);
static void log_hold(int on)
{
    if (on && !g_log_hold) {
        log_flush_all();   /* keep the order: what was buffered goes out before the held block */
        g_log_hold_cap = 4 << 20;
        g_log_hold = (char *) malloc((size_t) g_log_hold_cap);
        g_log_hold_len = 0;
    } else if (!on && g_log_hold) {
        char *h = g_log_hold; int n = g_log_hold_len;
        g_log_hold = NULL;
        log_write_now(h, n);
        free(h);
    }
}
/* THE LOG IS BUFFERED ONCE THE GAME RUNS (run 196, docs/47). Run 195: 8% of gameplay frames were 83 ms or more, and most were
 * a normal frame plus n x ~35 ms -- one sceIoOpen/Write/Close per printed line (the game's own "LOADED OBJECT" / "FREED OBJECT"
 * lines, the [seq] lines), plus ~150 ms every 60 presents for the [win] report. From present LOG_BUF_FROM on, lines go to RAM
 * and reach the card every g_lbuf_every presents (ux0:data/stairfax/log_flush.txt, default 600; 1 = every line at once, as
 * before), when the buffer is nearly full, at exit, and at once when the watchdog reports a stall. A hard crash can lose up
 * to one interval of lines: set log_flush.txt to 1 for a run that is expected to die. Boot (the first presents) is unbuffered.
 * The lock is a spin flag: the watchdog thread prints too. */
#define LOG_BUF_FROM 300
static char g_lbuf[256 * 1024]; static int g_lbuf_n, g_lbuf_on, g_lbuf_every = 600; static volatile int g_lbuf_lock;
unsigned vt_log_flushes, vt_log_lines_buffered;
static void log_lock(void)   { while (__atomic_test_and_set(&g_lbuf_lock, __ATOMIC_ACQUIRE)) sceKernelDelayThread(50); }
static void log_unlock(void) { __atomic_clear(&g_lbuf_lock, __ATOMIC_RELEASE); }
/* TASK 2 (2026-10-12): the periodic flush moves off the main thread. log_async.h: the main thread swaps two buffers, the low-priority
 * writer `stfx_logw` does the sceIoOpen/Write/Close. ux0:data/stairfax/log_async_off.txt starting with 1 = the synchronous flush of run 196-237. */
static char g_lbuf2[256 * 1024];
static LogAsync g_la; static int g_la_on; static SceUID g_la_sema = -1;
static void la_sink(void *ud, const char *s, int n) { (void) ud; log_write_now(s, n); }
static void la_wake(void *ud) { (void) ud; sceKernelSignalSema(g_la_sema, 1); }
static void la_pause(void *ud) { (void) ud; sceKernelDelayThread(1000); }
static int la_thread(SceSize args, void *argp) { (void) args; (void) argp; for (;;) { if (sceKernelWaitSema(g_la_sema, 1, NULL) < 0) break; la_writer_once(&g_la); } return 0; }
static void log_async_start(void)   /* called once, at present LOG_BUF_FROM, before g_lbuf_on is set */
{
    FILE *af = fopen("ux0:data/stairfax/log_async_off.txt", "r"); int off = af && fgetc(af) == 49; if (af) fclose(af);
    if (off) { printf("[log] async writer: OFF (log_async_off.txt 1): the flush is synchronous on the main thread\n"); return; }
    g_la_sema = sceKernelCreateSema("stfx_logw", 0, 0, 255, NULL);
    SceUID t = g_la_sema >= 0 ? sceKernelCreateThread("stfx_logw", la_thread, 0x10000100, 0x4000, 0, 0, NULL) : -1;
    if (t < 0 && g_la_sema >= 0) { sceKernelDeleteSema(g_la_sema); g_la_sema = -1; }
    if (t < 0) { printf("[log] async writer: FAILED to start (sema %x, thread %x): synchronous flush\n", (unsigned) g_la_sema, (unsigned) t); return; }
    log_flush_all();   /* what the synchronous path buffered so far goes out first: order */
    la_init(&g_la, g_lbuf, g_lbuf2, (unsigned) sizeof g_lbuf, la_sink, la_wake, la_pause, NULL);
    sceKernelStartThread(t, 0, NULL);
    g_la_on = 1;
    printf("[log] async writer: ON (stfx_logw, 2 x %d KB; default priority 0x10000100 like every port thread; main thread only swaps buffers; exit / watchdog / stall flush synchronously)\n", (int) (sizeof g_lbuf / 1024));
}
void vt_log_async_report(void)   /* one line for the status block / exit */
{
    if (g_la_on) printf("[log] async: %llu swaps, %llu sync flushes, %llu lines dropped, %llu waits timed out, %llu lines appended\n", g_la.swaps, g_la.syncFlushes, g_la.dropLines, g_la.waitTimeouts, g_la.appended);
}
static void log_flush_all(void)
{
    if (g_la_on) { la_flush_sync(&g_la); return; }
    log_lock();
    if (g_lbuf_n) { log_write_now(g_lbuf, g_lbuf_n); g_lbuf_n = 0; ++vt_log_flushes; }
    log_unlock();
}
void vt_log_flush(void) { log_flush_all(); }   /* run 224: for a line that must be on the card before the next thing happens (rhi_vita_tev.inc: a refused GL handle, [mem] LOW) */
static void log_write(const char *s, int n)
{
    if (n <= 0) return;
    if (g_log_hold) {
        if (g_log_hold_len + n > g_log_hold_cap) { log_hold(0); log_write_now(s, n); log_hold(1); return; }
        memcpy(g_log_hold + g_log_hold_len, s, (size_t) n); g_log_hold_len += n;
        return;
    }
    if (g_lbuf_on && g_la_on) {
        if (n > (int) sizeof g_lbuf) { la_flush_sync(&g_la); log_write_now(s, n); return; }
        la_append(&g_la, s, n); ++vt_log_lines_buffered;
        return;
    }
    if (g_lbuf_on) {
        log_lock();
        if (g_lbuf_n + n > (int) sizeof g_lbuf) { if (g_lbuf_n) { log_write_now(g_lbuf, g_lbuf_n); g_lbuf_n = 0; ++vt_log_flushes; } }
        if (n > (int) sizeof g_lbuf) { log_unlock(); log_write_now(s, n); return; }
        memcpy(g_lbuf + g_lbuf_n, s, (size_t) n); g_lbuf_n += n; ++vt_log_lines_buffered;
        log_unlock();
        return;
    }
    log_write_now(s, n);
}
static void log_write_now(const char *s, int n)
{
    if (n <= 0) return;
    SceUID fd = sceIoOpen(LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, s, (unsigned) n);
    sceIoClose(fd);
}

int __wrap_vprintf(const char *fmt, va_list ap)
{
    /* 2026-10-20 (run 287a): a line over 511 bytes was CUT and lost its newline, so the next line ran into it. The [c3] report (~730 B)
     * lost its census/flush fields, and C2's [stall] line is as long. A long line now goes through a heap buffer, whole. */
    char b[512]; va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > (int) sizeof b - 1) {
        char *h = (char *) malloc((size_t) n + 1);
        if (h) { vsnprintf(h, (size_t) n + 1, fmt, ap2); va_end(ap2); log_write(h, n); free(h); return n; }
        n = (int) sizeof b - 1;
    }
    va_end(ap2);
    log_write(b, n);
    return n;
}
int __wrap_printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = __wrap_vprintf(fmt, ap);
    va_end(ap);
    return n;
}
#ifdef VT_WRAP_IO
/* THE GAME (vt_game.c): run 4 of the game logged nothing after VIInit and left no dump.
 * I first read that as a SILENT EXIT and wrote these wraps for it; RETRACTED: the app was
 * still RUNNING (hung -- see VT_WATCHDOG). The blind spots are real regardless: OSPanic
 * prints through vfprintf (not wrapped), and abort / exit / _exit / __assert_func end the
 * process silently. So on the game
 * target the stdio family aimed at stdout/stderr goes to the log (real FILEs keep their
 * real path), and every exit route logs WHO called it: a return address, plus &main so
 * the load delta can be taken out without a coredump. */
int __real_vfprintf(FILE *f, const char *fmt, va_list ap);
size_t __real_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int __real_fputs(const char *s, FILE *f);
int __real_fputc(int c, FILE *f);
static int is_console(FILE *f) { return f == stdout || f == stderr; }
int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!is_console(f)) return __real_vfprintf(f, fmt, ap);
    return __wrap_vprintf(fmt, ap);
}
int __wrap_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = __wrap_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
size_t __wrap_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (!is_console(f)) return __real_fwrite(p, sz, n, f);
    log_write((const char *) p, (int) (sz * n));
    return n;
}
int __wrap_fputs(const char *s, FILE *f)
{
    if (!is_console(f)) return __real_fputs(s, f);
    log_write(s, (int) strlen(s));
    return 1;
}
int __wrap_fputc(int c, FILE *f)
{
    if (!is_console(f)) return __real_fputc(c, f);
    char ch = (char) c; log_write(&ch, 1);
    return c;
}
int main(void);
void vt_cr_quit_nowait(void);   /* the T0-d co-runner: a flag and a signal only */
static void log_exit(const char *how, int code, void *caller)
{
    char b[160];
    vt_cr_quit_nowait();
    int n = snprintf(b, sizeof b, "[vita] %s(%d) called from %p (runtime; &main = %p)\n",
                     how, code, caller, (void *) &main);
    log_write(b, n);
    log_flush_all();
}
void __real_exit(int code) __attribute__((noreturn));
void __real__exit(int code) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));
void __wrap_exit(int code)  { if (gxt_quit) gxt_quit("exit"); log_exit("exit", code, __builtin_return_address(0)); __real_exit(code); }   /* RT_THREAD 3.7: R is stopped FIRST (bounded), before anything shuts down */
void __wrap__exit(int code) { log_exit("_exit", code, __builtin_return_address(0)); __real__exit(code); }
void __wrap_abort(void)     { log_exit("abort", 0, __builtin_return_address(0)); __real_abort(); }
void __real___assert_func(const char *file, int line, const char *fn, const char *expr) __attribute__((noreturn));
void __wrap___assert_func(const char *file, int line, const char *fn, const char *expr)
{
    char b[256];
    int n = snprintf(b, sizeof b, "[vita] ASSERT %s:%d %s: %s\n", file ? file : "?", line, fn ? fn : "?", expr ? expr : "?");
    log_write(b, n);
    __real___assert_func(file, line, fn, expr);
}
#else
int __wrap_fprintf(FILE *f, const char *fmt, ...)
{
    (void) f;   /* model_view only ever fprintf's to stderr */
    va_list ap; va_start(ap, fmt);
    int n = __wrap_vprintf(fmt, ap);
    va_end(ap);
    return n;
}
#endif
int __wrap_puts(const char *s)
{
    log_write(s, (int) strlen(s));
    log_write("\n", 1);
    return 1;
}
int __wrap_putchar(int c)
{
    char ch = (char) c;
    log_write(&ch, 1);
    return c;
}

#include "geom_stats.h"
const GeomStats *rhi_vita_frame_stats(void);
int VT_TOOL(int argc, char **argv);
void plat_window_vita_set_frame_limit(int frames);
void rhi_vita_timing(SceUInt64 out[4]);
/* Optional tool-specific timer (vt_map.c defines it; absent -> NULL). */
__attribute__((weak)) void vt_extra_timing(SceUInt64 out[2]);

RhiInstance *__real_rhi_create(const RhiCreateInfo *info);
static RhiInstance *g_vt_rhi;   /* the instance the tool created */
bool         __real_rhi_present(RhiInstance *rhi, RhiSwapchain *sc);

RhiInstance *__wrap_rhi_create(const RhiCreateInfo *info)
{
    RhiCreateInfo ci = *info;
    printf("[vita] rhi_create: requested %s -> routed to AUTO (the one Vita backend)\n",
           rhi_backendName(info->backend));
    ci.backend = RHI_BACKEND_AUTO;
    RhiInstance *r = __real_rhi_create(&ci);
    printf("[vita] rhi_create: got %s\n", r ? rhi_backendName(rhi_getBackend(r)) : "NULL");
    g_vt_rhi = r;   /* for tool-specific instruments (vt_map.c's display-list cache) */
    return r;
}

/* Grade the frame about to be presented. model_view clears to (0.10, 0.11,
 * 0.13), i.e. about (26, 28, 33). "It drew" means pixels that are NOT that
 * colour, and "it is textured" means those pixels are many colours rather than
 * one flat fill -- a missing texture under GL_MODULATE draws white or black,
 * which this separates from a real texel image. */
static void grade_frame(void)
{
    if (g_stfx_backend == STFX_BK_GXM) { printf("[vita] grade: skipped (gxm backend M1: no glReadPixels path)\n"); return; }
    const int W = 960, H = 544;
    unsigned char *px = (unsigned char *) malloc((size_t) W * H * 4);
    if (!px) { printf("[vita] grade: no memory\n"); return; }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

    long cover = 0;
    int  minx = W, maxx = -1, miny = H, maxy = -1;
    /* distinct 15-bit colours among covered pixels, via a bitmap */
    static unsigned char seen[32768 / 8];
    memset(seen, 0, sizeof seen);
    int distinct = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const unsigned char *p = px + ((size_t) y * W + x) * 4;
            int dr = p[0] - kClear[0], dg = p[1] - kClear[1], db = p[2] - kClear[2];
            if (dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2)
                continue;
            ++cover;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            unsigned k = (unsigned) (p[0] >> 3) << 10 | (unsigned) (p[1] >> 3) << 5 | (p[2] >> 3);
            if (!(seen[k >> 3] & (1u << (k & 7)))) { seen[k >> 3] |= (unsigned char) (1u << (k & 7)); ++distinct; }
        }
    free(px);

    printf("[vita] grade: covered %ld px (%.2f%% of frame)\n", cover, 100.0 * cover / (W * H));
    if (cover)
        printf("[vita] grade: bbox x %d..%d  y %d..%d (lower-left origin)\n", minx, maxx, miny, maxy);
    printf("[vita] grade: %d distinct 15-bit colours among covered pixels\n", distinct);
    printf("%s model covers part of the frame (0.5%% .. 90%%)\n",
           (cover > W * H / 200 && cover < (long) W * H * 9 / 10) ? "PASS" : "FAIL");
    printf("%s covered pixels are textured (>= 64 distinct colours)\n",
           distinct >= 64 ? "PASS" : "FAIL");
}

static int g_presents;
static SceUInt64 g_t_first, g_t_from, g_t_after_capture;
/* GX progress counters (hal/gx_draw.c in the game build; weak, so the viewers link unchanged). */
extern unsigned gStfxGxDlCalls __attribute__((weak)), gStfxGxDlBytes __attribute__((weak));
static unsigned vt_dl_calls(void) { return &gStfxGxDlCalls ? gStfxGxDlCalls : 0; }
static unsigned vt_dl_bytes(void) { return &gStfxGxDlBytes ? gStfxGxDlBytes : 0; }

/* T0-d: THE CORE-1 CO-RUNNER, arms 8 and 9 (docs/53 section 10 T0 (d), 2026-10-18). DEFAULT OFF: no thread, no buffer, no cost unless a
 * perf_ab.txt arm list contains 8 or 9, and nothing at all when perf_ab.txt is absent.
 * WHY: the render-thread design assumes a free third core and a free L2/DRAM bus for R (the A9 cores share ONE L2). This is the
 * cheapest check of both, before any consumer exists: a thread pinned to core 1 at the port's only legal priority (0x10000100,
 * run 171), 16 KB stack, created LAZILY the first time arm 8 or 9 is active, blocked on a semaphore (zero CPU) in every other arm.
 *   arm 8  ALU spin only (a dependent multiply chain: no memory traffic) for as long as the arm holds. Reads "is core 1 free?".
 *   arm 9  per present: stream up to 20 MB (80 x 256 KB memcpy chunks between two 4 MB port-owned buffers, alternating direction
 *          so the 4 MB working set never sits in the 512 KB L2), then ALU spin until the next present. Reads "does R-sized
 *          traffic on core 1 slow the game thread on core 0?". 20 MB = the target; what really moved is LOGGED, not assumed.
 * The reading is G's wall (perf_ab.py --wall) -- and it is only a reading if the window says the co-runner really ran on core 1:
 * every arm-8/9 window prints a "[cr] ..." line with the co-runner's sceKernelGetCpuId() samples (1 per ms), the main thread's
 * (1 per present), its on-CPU milliseconds and the MB it actually streamed per frame; perf_ab.py calls the window INVALID
 * if the co-runner was not on core 1, did not get the CPU, or (arm 9) streamed under 50% of the target. A co-runner that never
 * got core 1 is an INVALID window, not a null result.
 * TEARDOWN: the co-runner is never waited on unboundedly. vt_cr_quit_nowait() (exit/_exit/abort wraps) only sets a flag and
 * signals; vt_cr_shutdown() (vt_stop, the post-VT_TOOL epilogue) adds ONE join bounded to 300 ms and never kills, never deletes.
 * The two buffers are freed never (the process ends). The thread makes no libc, malloc or printf call except memcpy: syscalls + memcpy only. */
#define CR_PRIO         0x10000100
#define CR_STACK        0x4000
#define CR_BUF_BYTES    (4u << 20)
#define CR_CHUNK_BYTES  (256u << 10)
#define CR_TARGET_KB    20480u
static SceUID g_cr_sema = -1, g_cr_thr = -1;
static int g_cr_state;                      /* 0 not started, 1 running, -1 refused (logged once) */
static volatile int g_cr_arm, g_cr_quit;
static volatile unsigned g_cr_seq;          /* arm 9: bumped once per present by the main thread */
static volatile unsigned g_cr_cpu[4], g_cr_kb, g_cr_run_us, g_cr_samples, g_cr_sink;   /* written by the co-runner ONLY */
static unsigned g_m_cpu[4];                 /* the main thread's own samples (main thread only) */
static unsigned char *g_cr_a, *g_cr_b;
static void cr_sample(SceUInt64 *last)      /* one sample per >= 1 ms of wall time: which core, and how much of the time was ours */
{
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - *last < 1000u) return;
    const unsigned dt = (unsigned) (now - *last);
    int id = sceKernelGetCpuId(); if (id < 0 || id > 3) id = 3;
    g_cr_cpu[id] = g_cr_cpu[id] + 1; g_cr_samples = g_cr_samples + 1;
    if (dt <= 1500u) g_cr_run_us = g_cr_run_us + dt;   /* a longer gap = descheduled: not counted as on-CPU time. 1.5 ms (was 2.5): an ALU step is ~1.0-1.3 ms between
                                                         * samples, so a gap past 1.5 ms is a preemption, or a memcpy chunk slower than ~170 MB/s -- either way the window
                                                         * must NOT read as 100% on-CPU. Still approximate: a gap under 1.5 ms that included a preemption counts in full.
                                                         * The kernel's own figure (runClocks) is logged beside it in the [cr] line; its UNIT is unverified, so the judge
                                                         * does not use it until a device run has compared the two (docs/53 T0 section). */
    *last = now;
}
static void cr_alu(unsigned n)              /* registers only: a dependent LCG chain, ~0.3 ms for 32768 at 444 MHz */
{
    unsigned x = g_cr_samples + 1u;
    for (unsigned i = 0; i < n; ++i) { x = x * 1664525u + 1013904223u; __asm__ volatile ("" : "+r" (x)); }
    g_cr_sink = x;
}
static int cr_active(void) { const int a = g_cr_arm; return !g_cr_quit && (a == 8 || a == 9); }
static int cr_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    for (;;) {
        if (sceKernelWaitSema(g_cr_sema, 1, NULL) < 0 || g_cr_quit) break;   /* zero CPU while the arm is anything else */
        SceUInt64 last = sceKernelGetProcessTimeWide(); unsigned off = 0; int dir = 0;
        while (cr_active()) {
            if (g_cr_arm == 8) { cr_alu(32768); cr_sample(&last); continue; }
            const unsigned seq = g_cr_seq; unsigned kb = 0;
            while (cr_active() && g_cr_arm == 9 && g_cr_seq == seq && kb < CR_TARGET_KB) {   /* the traffic */
                if (dir) memcpy(g_cr_a + off, g_cr_b + off, CR_CHUNK_BYTES); else memcpy(g_cr_b + off, g_cr_a + off, CR_CHUNK_BYTES);
                off += CR_CHUNK_BYTES; if (off >= CR_BUF_BYTES) { off = 0; dir ^= 1; }
                kb += CR_CHUNK_BYTES >> 10; g_cr_kb = g_cr_kb + (CR_CHUNK_BYTES >> 10);
                cr_sample(&last);
            }
            while (cr_active() && g_cr_arm == 9 && g_cr_seq == seq) { cr_alu(32768); cr_sample(&last); }   /* then spin until the next present */
        }
    }
    return 0;
}
static void cr_start(void)                  /* main thread, the first present of the first arm-8/9 window: the settle window absorbs it */
{
    g_cr_state = -1;
    g_cr_a = (unsigned char *) malloc(CR_BUF_BYTES); g_cr_b = (unsigned char *) malloc(CR_BUF_BYTES);
    if (!g_cr_a || !g_cr_b) { printf("[cr] co-runner NOT started: could not allocate 2 x %u MB\n", CR_BUF_BYTES >> 20); return; }
    memset(g_cr_a, 0x5a, CR_BUF_BYTES); memset(g_cr_b, 0xa5, CR_BUF_BYTES);   /* touch every page now, not in the measured loop */
    g_cr_sema = sceKernelCreateSema("stfx_cr", 0, 0, 255, NULL);
    g_cr_thr = g_cr_sema >= 0 ? sceKernelCreateThread("stfx_cr", cr_thread, CR_PRIO, CR_STACK, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL) : -1;
    if (g_cr_thr < 0 || sceKernelStartThread(g_cr_thr, 0, NULL) < 0) {
        printf("[cr] co-runner NOT started: sema %x thread %x\n", (unsigned) g_cr_sema, (unsigned) g_cr_thr); g_cr_thr = -1; return;
    }
    g_cr_state = 1;
    printf("[cr] co-runner thread started (arms 8/9): prio 0x%x, affinity mask 0x%x = core 1 only, stack %d KB, 2 x %u MB buffers, stream target %u MB/frame in %u KB chunks; blocks on a semaphore in every other arm\n",
           CR_PRIO, (unsigned) SCE_KERNEL_CPU_MASK_USER_1, CR_STACK >> 10, CR_BUF_BYTES >> 20, CR_TARGET_KB >> 10, CR_CHUNK_BYTES >> 10);
}
static void vt_cr_arm(int arm)              /* called with the arm that holds from this present on */
{
    const int was = g_cr_arm == 8 || g_cr_arm == 9, now = arm == 8 || arm == 9;
    if (now && g_cr_state == 0 && !g_cr_quit) cr_start();
    g_cr_arm = arm;
    if (arm == 9) g_cr_seq = g_cr_seq + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (now && !was && g_cr_state == 1) sceKernelSignalSema(g_cr_sema, 1);
}
static void vt_main_cpu_sample(void)        /* every present, every arm: G's placement baseline (vt_perf_ab). Was arms 8/9 only: no baseline to compare against. */
{
    const int id = sceKernelGetCpuId();
    g_m_cpu[id < 0 || id > 3 ? 3 : id]++;
}
void vt_cr_quit_nowait(void)                /* exit / _exit / abort: a flag and a signal, nothing that can wait */
{
    g_cr_quit = 1;
    if (g_cr_sema >= 0) sceKernelSignalSema(g_cr_sema, 1);
}
static void vt_cr_shutdown(void)            /* vt_stop and the epilogue: ONE bounded join (300 ms), never a kill or a delete */
{
    vt_cr_quit_nowait();
    if (g_cr_state == 1 && g_cr_thr >= 0) { SceUInt to = 300000; int st = 0; int rc = sceKernelWaitThreadEnd(g_cr_thr, &st, &to); printf("[cr] co-runner joined (rc %x)\n", (unsigned) rc); }
}
static void vt_cr_window(int from, int to, int arm)   /* the window just printed; always snapshots, prints only for arms 8/9 */
{
    static unsigned p_cpu[4], p_m[4], p_kb, p_run, p_smp;
    unsigned c[4], m[4];
    for (int i = 0; i < 4; ++i) { c[i] = g_cr_cpu[i] - p_cpu[i]; m[i] = g_m_cpu[i] - p_m[i]; p_cpu[i] = g_cr_cpu[i]; p_m[i] = g_m_cpu[i]; }
    const unsigned kb = g_cr_kb - p_kb, run = g_cr_run_us - p_run, smp = g_cr_samples - p_smp;
    p_kb = g_cr_kb; p_run = g_cr_run_us; p_smp = g_cr_samples;
    static unsigned long long p_rc;
    long long krc = -1;                     /* the kernel's runClocks delta for the co-runner (UNIT UNVERIFIED), -1 = unreadable */
    if (g_cr_state == 1 && g_cr_thr >= 0) {
        SceKernelThreadInfo ti; memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        if (sceKernelGetThreadInfo(g_cr_thr, &ti) == 0) { const unsigned long long rc = (unsigned long long) ti.runClocks; krc = (long long) (rc - p_rc); p_rc = rc; }
    }
    if (arm != 8 && arm != 9) return;
    printf("[cr] presents %d..%d arm %d | co-runner cpu id samples c0=%u c1=%u c2=%u c3=%u (n=%u, 1 per ms, pinned to core 1) | main thread cpu id c0=%u c1=%u c2=%u c3=%u (1 per present)"
           " | co-runner on-CPU %.1f ms | streamed %.2f MB/frame (target %.2f) | kernel runClocks %+lld (unit unverified)%s\n",
           from, to, arm, c[0], c[1], c[2], c[3], smp, m[0], m[1], m[2], m[3], run / 1000.0,
           kb / 1024.0 / (double) (to - from > 0 ? to - from : 1), arm == 9 ? CR_TARGET_KB / 1024.0 : 0.0, krc, g_cr_state == 1 ? "" : " | CO-RUNNER NOT RUNNING");
}
/* gx_rec.c (another agent, the T0-a recorder) supplies these two; weak, so a build without it links unchanged. */
extern void gx_rec_window(int presents_from, int presents_to, int arm) __attribute__((weak));
extern void gx_rec_frame_edge(int arm) __attribute__((weak));
/* GXAOT_HOOK_BEGIN C3 */
/* GXAOT release track C3 (arms 100-107 and 109 (108 is C2's), patches/C3-perdraw/002): the pacing lock + the [c3] window line; rhi_vita_tev.inc via c3_cuts.inc supplies them, weak */
extern void rhi_vita_c3_frame_edge(int arm, unsigned long long now_us) __attribute__((weak));
extern void rhi_vita_c3_window(int from, int to, int arm) __attribute__((weak));
/* GXAOT_HOOK_END C3 */

/* PERF A/B IN ONE BOOT (2026-10-14, docs/47 perf phase). ux0:data/stairfax/perf_ab.txt, read once at present 1:
 *     from <present> period <presents> arms <a> <b> [<c> <d> ...]
 * From present `from` on, gStfxAb takes the listed arms in turn for `period` presents each, cycling. Both are rounded UP to
 * multiples of 60, so every [win] window (60 presents) lies inside ONE arm; each window is tagged with an "[ab] presents P..Q
 * arm A" line printed right after it. Code under test reads gStfxAb (0 = control by convention). Arm 99 is the POSITIVE
 * CONTROL, built in: a 2.00 ms busy-wait on the main thread every present. A harness that does not report +2.0 ms for it
 * cannot see anything. Arm 97 is the SAME busy-wait at 12.00 ms (the scaled control, for runs whose wall noise floor exceeds 2 ms).
 * host/engine/perf_ab.py groups the windows by arm and judges. Absent file = gStfxAb stays 0. */
int gStfxAb;
/* R3a C0 (host/engine/R3_DRAW.md s2, 2026-10-23): THE ARM THE C3 MACHINERY SEES.  The release default (fixed mode) sets gStfxAb = 107 = the C3 cuts + the always-on timers OFF; an arm-mode boot
 * used to leave gStfxAb = the perf arm (6, 132, ...), so the timers (a syscall per read) stayed ON and every arm-mode R number measured them.  gStfxC3Arm = 107 for every threaded perf arm
 * (gxt_c3_arm, gx_thread.c) and = gStfxAb otherwise; c3_cuts.h's C3_ON / c3_clk, the c3 frame edge / window and gx_draw.c's c3VsbOn read it (rhi_vita_tev.inc maps the header's gStfxAb to it). */
int gStfxC3Arm;
extern int gxt_c3_arm(int arm) __attribute__((weak));
extern int gxt_skin_wanted(int arm) __attribute__((weak));   /* gx_thread.c: R8 GPU skinning wanted for this perf arm (149) / gx_skin.txt */
extern int gGxSkinOn __attribute__((weak));            /* hal/gx_skin.c: the switch objprint_dolphin.c reads on G */
extern void gxt_r3a_edge(int arm) __attribute__((weak));   /* gx_thread.c: publishes the R3a cut switches (gx_r3a) for this perf arm */
extern int gx_c3_toff __attribute__((weak));
extern int gx_c3_vsb_force __attribute__((weak));   /* gx_draw.c: 1 = the record-scan memo (arm 102/104/105/107/109's cut) is on whatever gStfxAb says (R3a C0) */
/* R8-MF A1: free the retired GPU-skin tables R has passed -- EVERY present, in EVERY mode.  hw_R8tour2 (docs/54 2026-10-26): the tick sat after
 * the perf-A/B-off early return below, so in the RELEASE config (no perf_ab.txt) it never ran and retired tables never freed ([r8-tick] read all
 * zeros); every A/B boot took the other path.  Called on both paths, after gx_rec_frame_edge has settled g_gxt.on. */
static void vt_skin_tick(void)
{
    extern void gx_skin_frame_edge(uint32_t gseq, uint32_t rdone, int ringOn) __attribute__((weak));
    if (gx_skin_frame_edge) gx_skin_frame_edge(g_gxt.seq, GXT_LOAD_ACQ(&g_gxt.r_presented), g_gxt.on);
}
static void vt_c3_arm_sync(void)
{
    gStfxC3Arm = gxt_c3_arm ? gxt_c3_arm(gStfxAb) : gStfxAb;
    if (&gGxSkinOn) gGxSkinOn = gxt_skin_wanted ? gxt_skin_wanted(gStfxAb) : 0;   /* R8: GPU skinning follows arm 149 / gx_skin.txt, written at the frame edge by G only */
    if (gxt_r3a_edge) gxt_r3a_edge(gStfxAb);   /* R3a: the four cut switches follow the perf arm (arm 6 / fixed mode = all ON, 133 = none, 134..137 = all but one) */
    { extern int gx_vb_bulk __attribute__((weak)); extern int gxt_vb_bulk_wanted(int arm) __attribute__((weak)); if (&gx_vb_bulk) gx_vb_bulk = gxt_vb_bulk_wanted ? gxt_vb_bulk_wanted(gStfxAb) : 0; }   /* R-VB: perf arm 155 = 149 + R replays VBYTES in bulk (hal/gx_draw.c); G writes, byte-exact either way */
    { extern int g_gxs_mm_v2 __attribute__((weak)); if (&g_gxs_mm_v2) g_gxs_mm_v2 = gStfxAb == 140; }   /* hw_Mm2: the 4-way program memo (rhi_gxm_shader.c), perf arm 140 only until judged */
    if (&gx_c3_vsb_force) gx_c3_vsb_force = gStfxC3Arm == 102 || gStfxC3Arm == 104 || gStfxC3Arm == 105 || gStfxC3Arm == 107 || gStfxC3Arm == 109;
}
static void vt_c3_arm_report(void)   /* one line per change of (perf arm, C3 arm): the effective C3 state, with the switch the timers actually read */
{
    static int s_ab = -1, s_c3 = -1, s_toff = -1;
    const int toff = &gx_c3_toff ? gx_c3_toff : -1;
    if (gStfxAb == s_ab && gStfxC3Arm == s_c3 && toff == s_toff) return;
    s_ab = gStfxAb; s_c3 = gStfxC3Arm; s_toff = toff;
    printf("[c3map] perf arm %d runs C3 arm %d | always-on timers %s (gx_c3_toff %d)\n", gStfxAb, gStfxC3Arm, toff > 0 ? "OFF" : toff == 0 ? "ON" : "n/a", toff);
}
static int g_ab_from = -1, g_ab_period, g_ab_n, g_ab_arm[8];
static void vt_perf_ab(SceUInt64 now)
{
    (void) now;
    if (g_presents == 1) {
        FILE *f = fopen("ux0:data/stairfax/perf_ab.txt", "r");
        if (f) {
            char w[16]; int from = -1, per = 0;
            while (fscanf(f, "%15s", w) == 1) {
                if (!strcmp(w, "from")) { if (fscanf(f, "%d", &from) != 1) break; }
                else if (!strcmp(w, "period")) { if (fscanf(f, "%d", &per) != 1) break; }
                else if (!strcmp(w, "arms")) { while (g_ab_n < 8 && fscanf(f, "%d", &g_ab_arm[g_ab_n]) == 1) ++g_ab_n; }
            }
            fclose(f);
            if (from >= 0 && per > 0 && g_ab_n > 0) {
                g_ab_from = (from + 59) / 60 * 60; g_ab_period = (per + 59) / 60 * 60;
                printf("[ab] perf A/B ON: from present %d, %d presents per arm, %d arms:", g_ab_from, g_ab_period, g_ab_n);
                for (int i = 0; i < g_ab_n; ++i) printf(" %d", g_ab_arm[i]);
                printf("\n");
            } else { g_ab_n = 0; printf("[ab] perf_ab.txt present but not understood (want: from <n> period <n> arms <a> <b> ...): OFF\n"); }
        } else printf("[ab] perf A/B off (no perf_ab.txt)\n");
        /* RELEASE DEFAULT (2026-10-20, docs/54 visit 3): with no A/B running, play runs arm 107 = the C3 per-draw cuts + the always-on perf
         * timers off (indicative -11 ms wall at the ch2 hub, run 288b). Inside an A/B, arm 0 stays the old behaviour (the control).
         * ux0:data/stairfax/c3_off.txt (any content) = the old behaviour in play too. */
        if (g_ab_from < 0) {
            FILE *o = fopen("ux0:data/stairfax/c3_off.txt", "r");
            if (o) fclose(o);
            gStfxAb = o ? 0 : 107;
            printf("[release] C3 cuts + timers off: %s (perf A/B off; c3_off.txt %s)\n", gStfxAb == 107 ? "ON (arm 107)" : "OFF (arm 0)", o ? "present" : "absent");
        }
    }
    vt_c3_arm_sync();   /* R3a C0 */
    if (g_ab_from < 0 && rhi_vita_c3_frame_edge) { rhi_vita_c3_frame_edge(gStfxC3Arm, now); vt_c3_arm_report(); }   /* GXAOT_HOOK C3: perf A/B off: the release default (107, or 0 with c3_off.txt) */
    if (g_ab_from < 0) { if (gx_rec_frame_edge) gx_rec_frame_edge(gStfxAb); vt_skin_tick(); return; }   /* perf A/B off: still one edge per present, arm 0 */
    vt_main_cpu_sample();   /* G's placement, every present, every arm: the [ab] line prints it so an arm-8/9 window has a baseline */
    if (g_presents % 60 == 0) {
        extern unsigned rhi_vita_ab_draws __attribute__((weak)), rhi_vita_ab_tmx_draws __attribute__((weak)), rhi_vita_ab_blend_flips __attribute__((weak));
        extern unsigned long long rhi_vita_ab_vs_vec4 __attribute__((weak));
        static unsigned p_d, p_t, p_b; static unsigned long long p_v;
        const unsigned d = &rhi_vita_ab_draws ? rhi_vita_ab_draws : 0, t = &rhi_vita_ab_tmx_draws ? rhi_vita_ab_tmx_draws : 0,
                       b = &rhi_vita_ab_blend_flips ? rhi_vita_ab_blend_flips : 0;
        const unsigned long long v = &rhi_vita_ab_vs_vec4 ? rhi_vita_ab_vs_vec4 : 0;
        /* vitaGL vprog memo (tools/vitagl-vprog-memo.patch): hits/misses per frame; weak so an unpatched libvitaGL still links */
        extern unsigned int vgl_vprog_memo_hits __attribute__((weak)), vgl_vprog_memo_misses __attribute__((weak));
        static unsigned p_mh, p_mm, p_am[4];
        unsigned am[4];
        for (int i = 0; i < 4; ++i) { am[i] = g_m_cpu[i] - p_am[i]; p_am[i] = g_m_cpu[i]; }
        const unsigned mh = &vgl_vprog_memo_hits ? vgl_vprog_memo_hits : 0, mm = &vgl_vprog_memo_misses ? vgl_vprog_memo_misses : 0;
        if (g_presents > g_ab_from)   /* the arm that held for the window just printed; the counters are the GPU-path draws in it */
            printf("[ab] presents %d..%d arm %d | gpu draws/frame %.1f, with uTMX %.1f | vertex uniform vec4/draw %.1f | blend flips/frame %.1f"
                   " | vprog memo hits/frame %.1f, misses/frame %.1f | main thread cpu id c0=%u c1=%u c2=%u c3=%u (1 per present)%s\n",
                   g_presents - 60, g_presents, gStfxAb, (d - p_d) / 60.0, (t - p_t) / 60.0, d != p_d ? (double) (v - p_v) / (d - p_d) : 0.0, (b - p_b) / 60.0,
                   (mh - p_mh) / 60.0, (mm - p_mm) / 60.0, am[0], am[1], am[2], am[3], &vgl_vprog_memo_hits ? "" : " (libvitaGL WITHOUT the memo patch)");
        p_d = d; p_t = t; p_b = b; p_v = v; p_mh = mh; p_mm = mm;
        if (g_presents > g_ab_from) {   /* the same window, for T0-d (arms 8/9: the [cr] line) and T0-a (gx_rec.c's per-window report) */
            vt_cr_window(g_presents - 60, g_presents, gStfxAb);
            if (gx_rec_window) gx_rec_window(g_presents - 60, g_presents, gStfxAb);
            if (rhi_vita_c3_window) rhi_vita_c3_window(g_presents - 60, g_presents, gStfxC3Arm);   /* GXAOT_HOOK C3: the arm that held for the window just printed (R3a C0: its EFFECTIVE C3 arm; the [c3map] line pairs it with the perf arm) */
        }
    }
    if (g_presents >= g_ab_from) gStfxAb = g_ab_arm[((g_presents - g_ab_from) / g_ab_period) % g_ab_n];
    vt_cr_arm(gStfxAb);   /* arms 8/9: the core-1 co-runner (lazy start; any other arm parks it) */
    if (gx_rec_frame_edge) gx_rec_frame_edge(gStfxAb);   /* once per present, after the arm is updated */
    vt_c3_arm_sync();   /* R3a C0: after the arm is updated, before anything reads the C3 state */
    vt_skin_tick();   /* R8-MF A1: perf A/B on (the A/B-off path calls it before its early return) */
    if (rhi_vita_c3_frame_edge) { rhi_vita_c3_frame_edge(gStfxC3Arm, now); vt_c3_arm_report(); }   /* GXAOT_HOOK C3: arms 103/105/109 pacing lock, 106, 107, 109 timers switch, frame-time buckets */
    { /* arm 4: vitaGL's vprog memo ON (control = upstream: patch the vertex program on every draw) */
        extern int vgl_vprog_memo __attribute__((weak));
        if (&vgl_vprog_memo) vgl_vprog_memo = gStfxAb == 4;
    }
    if (gStfxAb == 99) { const SceUInt64 t = sceKernelGetProcessTimeWide(); while (sceKernelGetProcessTimeWide() - t < 2000u) { } }
    /* Arm 97: the SCALED positive control (docs/53 T0, 2026-10-18). The 2 ms spin of arm 99 is smaller than the wall noise floor of a 35-85 ms
     * frame (5.4-6.2 ms in runs 279/282), so "SEEN" on it proves the harness can see 2 ms only where the floor is under 2 ms. The GO rule needs
     * gains of max(2 x floor, 5) ms; this arm is the same busy-wait at 12 ms, a size the harness MUST resolve before it may judge an effect that big. */
    if (gStfxAb == 97) { const SceUInt64 t = sceKernelGetProcessTimeWide(); while (sceKernelGetProcessTimeWide() - t < 12000u) { } }
    /* Arm 96 (RT_THREAD.md 6.1): THREADED + the same 12 ms spin on G. With R the long pole it must NOT move wall beyond the floor (G has 28 ms of slack to absorb it);
     * arm 98 (the same spin on R) is in gx_thread.c's PRESENT handler. Arm 97 stays the DIRECT control. gx_thread.c makes arm 96 threaded; run direct it is arm 97 plus a label. */
    if (gStfxAb == 96) { const SceUInt64 t = sceKernelGetProcessTimeWide(); while (sceKernelGetProcessTimeWide() - t < 12000u) { } }
    /* Arms that read or write R-side state from G cannot run beside a replaying R (RT_THREAD D12): the null-draw arm 5 and the gxdump arm 114 are REFUSED while threaded.
     * They are treated as arm 0 for the frame, and the substitution is printed once (a judge that sees "[gxt] arm 5 skipped" knows what the window was). */
    if ((gStfxAb == 5 || gStfxAb == 114) && gxt_is_active && gxt_is_active()) {
        static int s_said;
        if (!s_said++) printf("[gxt] arm %d skipped (threaded): it reads R-side state from G; this window ran as arm 0\n", gStfxAb);
        gStfxAb = 0;
    }
}

/* the first time a G-side backend reader is skipped because R is replaying, say so once per reader */
static int thr_skip(int thr, const char *what)
{
    static const char *said[16]; static int n;
    if (!thr) return 0;
    for (int i = 0; i < n; ++i) if (said[i] == what) return 1;
    if (n < 16) said[n++] = what;
    printf("[gxt] %s skipped (threaded)\n", what);
    return 1;
}
static unsigned long long vt_status_us;   /* inside the per-60-presents status block (the game-state log) */
unsigned vt_ft_hist[5], vt_ft_dev50;   /* per-window frame-time histogram (run 194) */
bool __wrap_rhi_present(RhiInstance *rhi, RhiSwapchain *sc)
{
    SceUInt64 now = sceKernelGetProcessTimeWide();   /* microseconds */
    ++g_presents;
    { extern int rhi_vita_presents; rhi_vita_presents = g_presents; }
    { extern void rhi_vita_asc_tick(void) __attribute__((weak)); if (rhi_vita_asc_tick) rhi_vita_asc_tick(); }   /* run 171: a compile finished off-thread becomes a shader object here */
    if (g_presents == 1) g_t_first = now;
    if (g_presents == 1) {   /* Task 3 (2026-10-12): ux0:data/stairfax/rt_stats.txt starting with 1 = the [rt] frame accounting (hal/gx_draw.c, vi_shim.c, rhi_vita*) */
        extern void gx_rt_enable(int on) __attribute__((weak));
        FILE *rf = fopen("ux0:data/stairfax/rt_stats.txt", "r"); int on = rf && fgetc(rf) == 49; if (rf) fclose(rf);
        if (on && gx_rt_enable) gx_rt_enable(1);
        {   /* 2026-10-14 perf phase, step 3: phase_stats.txt 1 = the [ph] split of REST (game tree hal/stfx_phase.c, main/gameloop.c STFX_PH) */
            extern void stfx_ph_enable(int on) __attribute__((weak));
            FILE *pf = fopen("ux0:data/stairfax/phase_stats.txt", "r"); int pon = pf && fgetc(pf) == 49; if (pf) fclose(pf);
            if (pon && stfx_ph_enable) stfx_ph_enable(1);
            printf("[ph] game-loop phase split: %s (phase_stats.txt 1 = ON)\n", pon && stfx_ph_enable ? "ON" : "off");
        }
        printf("[rt] stats: %s (rt_stats.txt 1 = ON: game/shim/backend/wait split over 1 frame in 8, DL-path split, readback counts; every 60 presents)\n", on && gx_rt_enable ? "ON" : "off");
    }
    if (g_presents == LOG_BUF_FROM) {
        FILE *lf = fopen("ux0:data/stairfax/log_flush.txt", "r"); int v = 0;
        if (lf) { if (fscanf(lf, "%d", &v) != 1) v = 0; fclose(lf); }
        if (v > 0) g_lbuf_every = v;
        printf("[log] buffered from here: written to the card every %d presents%s\n", g_lbuf_every, g_lbuf_every == 1 ? " (every line at once: log_flush.txt 1)" : "");
        g_lbuf_on = g_lbuf_every != 1;
        if (g_lbuf_on) log_async_start();
    }
    if (g_lbuf_on && g_la_on) {   /* the hand-off: two pointer swaps; retried every present while the writer is still busy */
        if (g_presents % g_lbuf_every == 0) g_la.want = 1;
        if (g_la.want && la_swap(&g_la)) g_la.want = 0;
    } else if (g_lbuf_on && g_presents % g_lbuf_every == 0) log_flush_all();
    {   /* FRAME-TIME HISTOGRAM (run 194, docs/47): is the frame GPU-bound, or stepping in vblanks? The averages of runs 168-193
           cannot tell. One bucket per 16.67 ms step (edges halfway between steps): 16.7 | 33.3 | 50.0 | 66.7 | 83.3+ ms, plus the
           spread inside the 50 ms bucket (how far from 50.0 its frames sit). Printed with every [win] line, then cleared. */
        static SceUInt64 s_last;
        if (s_last) {
            const unsigned us = (unsigned) (now - s_last);
            int b = (int) ((us + 8333u) / 16667u) - 1; if (b < 0) b = 0; if (b > 4) b = 4;
            ++vt_ft_hist[b];
            if (b == 2) { const int d = (int) us - 50000; vt_ft_dev50 += (unsigned) (d < 0 ? -d : d); }
            {   /* THE LONG FRAMES (run 195): run 194 found 8% of gameplay frames at 83 ms or more, ~5 a window. Each one is named
                   here with its own split, from the cumulative timers' per-frame deltas (at most 8 lines a window). */
                extern void gx_win_times(unsigned long long out[8]) __attribute__((weak));
                extern unsigned long long vi_w_us[5] __attribute__((weak)), rhi_vita_gx_compile_us __attribute__((weak)),
                    rhi_vita_tev_compile_us __attribute__((weak)), rhi_vita_shc_load_us __attribute__((weak)), mm_tick_us __attribute__((weak));
                static SceUInt64 p_s[4]; static unsigned long long p_g[8], p_v[5], p_c[3]; static int s_have, s_lines, s_win;
                SceUInt64 c_s[4]; unsigned long long c_g[8] = {0}, c_v[5] = {0}, c_c[3];
                rhi_vita_timing(c_s); if (gx_win_times) gx_win_times(c_g);
                if (&vi_w_us[0]) for (int q = 0; q < 5; ++q) c_v[q] = vi_w_us[q];
                c_c[0] = &rhi_vita_gx_compile_us ? rhi_vita_gx_compile_us : 0; c_c[1] = &rhi_vita_tev_compile_us ? rhi_vita_tev_compile_us : 0;
                c_c[2] = &rhi_vita_shc_load_us ? rhi_vita_shc_load_us : 0;
                if (g_presents / 60 != s_win) { s_win = g_presents / 60; s_lines = 0; }
                if (s_have && us > 75000u && g_presents > 1200 && s_lines++ < 8)
                    printf("[long] present %d: %.1f ms | frame CPU %.1f swap %.1f | display lists %.1f immediate %.1f texture load %.1f efb copy %.1f | retrace: pump %.1f pre %.1f present %.1f post %.1f reopen %.1f | shader pair %.1f tev %.1f cache loads %.1f\n",
                           g_presents, us / 1000.0, (double) (c_s[0] - p_s[0]) / 1000.0, (double) (c_s[3] - p_s[3]) / 1000.0,
                           (double) (c_g[0] - p_g[0]) / 1000.0, (double) (c_g[3] - p_g[3]) / 1000.0, (double) (c_g[4] - p_g[4]) / 1000.0, (double) (c_g[5] - p_g[5]) / 1000.0,
                           (double) (c_v[0] - p_v[0]) / 1000.0, (double) (c_v[1] - p_v[1]) / 1000.0, (double) (c_v[2] - p_v[2]) / 1000.0, (double) (c_v[3] - p_v[3]) / 1000.0, (double) (c_v[4] - p_v[4]) / 1000.0,
                           (double) (c_c[0] - p_c[0]) / 1000.0, (double) (c_c[1] - p_c[1]) / 1000.0, (double) (c_c[2] - p_c[2]) / 1000.0);
                for (int q = 0; q < 4; ++q) p_s[q] = c_s[q];
                for (int q = 0; q < 8; ++q) p_g[q] = c_g[q];
                for (int q = 0; q < 5; ++q) p_v[q] = c_v[q];
                for (int q = 0; q < 3; ++q) p_c[q] = c_c[q];
                s_have = 1;
            }
        }
        s_last = now;
    }
#ifdef VT_PRESENT_LOG
    {   /* every present for the first 30, then every 60th: frame time + GX work since the last line */
        static SceUInt64 s_prev; static unsigned s_calls, s_bytes;
        /* THE STATUS BLOCK IS NOT FREE (2026-10-08, docs/47 "Run 141"): ~70 lines every 60 presents, and every printf reopens the
         * log. Its time is measured (vt_status_us, in the [win] line), and ux0:data/stairfax/quiet.txt starting with 1 prints it
         * every 600 presents instead of every 60 -- the [win] line stays at 60. */
        static int s_quiet = -1;
        if (s_quiet < 0) { FILE *qf = fopen("ux0:data/stairfax/quiet.txt", "r"); s_quiet = qf && fgetc(qf) == 49; if (qf) fclose(qf);
                           printf("[vita] status block every %d presents%s\n", s_quiet ? 600 : 60, s_quiet ? " (quiet.txt)" : ""); }
        if ((!s_quiet && g_presents <= 30) || g_presents % (s_quiet ? 600 : 60) == 0) {   /* quiet: not the first 30 presents either (each block is ~1 s: run 149's boot took 30 s of them) */
            SceUInt64 sl0 = sceKernelGetProcessTimeWide();
            { extern void vt_game_state_log(void) __attribute__((weak)); if (vt_game_state_log) vt_game_state_log(); }
            vt_status_us += sceKernelGetProcessTimeWide() - sl0;
            unsigned c = vt_dl_calls(), by = vt_dl_bytes();
            static SceUInt64 s_line_t; static int s_line_n;
            extern int gStfxLightArm __attribute__((weak));
            printf("[frame] %d dt %.1f ms avg %.1f ms over %d | light %d | dl calls %u bytes %u (since last line)\n", g_presents,
                   s_prev ? (double) (now - s_prev) / 1000.0 : 0.0,
                   s_line_t ? (double) (now - s_line_t) / 1000.0 / (double) (g_presents - s_line_n) : 0.0, g_presents - s_line_n,
                   &gStfxLightArm ? gStfxLightArm : -1, c - s_calls, by - s_bytes);
            s_calls = c; s_bytes = by; s_line_t = now; s_line_n = g_presents;
        }
        s_prev = now;
    }
#endif
    {   /* THE FRAME, SPLIT, every 60 presents (2026-10-08, docs/47): wall, frame CPU, and the exact time inside each GX entry
         * point (hal/gx_draw.c gx_win_times). REST = frame CPU minus all of them = the game's own logic, audio, the movie,
         * everything that is not GX. Shader builds are INSIDE the display-list / immediate figures and shown beside them. */
        extern void gx_win_times(unsigned long long out[8]) __attribute__((weak));
        extern unsigned long long rhi_vita_gx_compile_us __attribute__((weak)), rhi_vita_tev_compile_us __attribute__((weak)),
                                  rhi_vita_shc_load_us __attribute__((weak));
        static SceUInt64 w_t, w_s[4]; static unsigned long long w_g[8], w_c[3]; static int w_n;
        static unsigned long long w_st, w_vi[5], w_thr[2];
        extern unsigned long long vi_w_us[5] __attribute__((weak));
        extern unsigned long long os_thread_times(unsigned long long *mainUs) __attribute__((weak));
        if (gx_win_times && g_presents % 60 == 0) {
            SceUInt64 s1[4]; unsigned long long g1[8], c1[3];
            rhi_vita_timing(s1); gx_win_times(g1);
            c1[0] = &rhi_vita_gx_compile_us ? rhi_vita_gx_compile_us : 0; c1[1] = &rhi_vita_tev_compile_us ? rhi_vita_tev_compile_us : 0;
            c1[2] = &rhi_vita_shc_load_us ? rhi_vita_shc_load_us : 0;
            if (w_t) {
                const double nf = (double) (g_presents - w_n) * 1000.0;
                double cpu = (double) (s1[0] - w_s[0]) / nf, dl = (double) (g1[0] - w_g[0]) / nf, imm = (double) (g1[3] - w_g[3]) / nf,
                       tex = (double) (g1[4] - w_g[4]) / nf, cp = (double) (g1[5] - w_g[5]) / nf;
                printf("[win] presents %d..%d: wall %.1f | frame CPU %.1f, swap %.1f | display lists %.1f (cache CPU path %.1f, GPU path %.1f; %.0f calls) |"
                       " immediate %.1f (%.0f prims) | texture load %.1f | efb copy %.1f | REST %.1f || shader builds inside those: pair %.1f, tev %.1f (of which cache loads %.1f)  (ms/frame)\n",
                       w_n, g_presents, (double) (now - w_t) / nf, cpu, (double) (s1[3] - w_s[3]) / nf, dl, (double) (g1[1] - w_g[1]) / nf,
                       (double) (g1[2] - w_g[2]) / nf, (double) (g1[6] - w_g[6]) / (nf / 1000.0), imm, (double) (g1[7] - w_g[7]) / (nf / 1000.0), tex, cp,
                       cpu - dl - imm - tex - cp, (double) (c1[0] - w_c[0]) / nf, (double) (c1[1] - w_c[1]) / nf, (double) (c1[2] - w_c[2]) / nf);
            }
            if (w_t) printf("[win] ... of the wall time outside frame CPU and swap: status log %.1f ms/frame\n", (double) (vt_status_us - w_st) / ((double) (g_presents - w_n) * 1000.0));
            { extern void gx_rt_report(int presents) __attribute__((weak)); if (gx_rt_report) gx_rt_report(g_presents); }   /* Task 3: only prints when rt_stats.txt is 1 */
            {   /* hw_Rsplit (docs/54 2026-10-24): R's display-list call split + the GXM backend's draw steps, per 60-present window, ms/frame. Prints only while
                 * the HAL's always-on timers are ON (gx_c3_toff 0: C3 arm 104 = perf arm 139); the backend steps need gxm_prof.txt. Counters are R's, read from G (racy, diagnostic). */
                extern void gx_rsplit_times(unsigned long long o[25]) __attribute__((weak)); extern int gx_c3_toff __attribute__((weak));
                extern unsigned long long g_gxm_ptc[2][8] __attribute__((weak)); extern unsigned g_gxm_pnc[2] __attribute__((weak));
                static unsigned long long rp[25], rb[2][8]; static unsigned rbn[2]; static int rn = -1, rarm = -1;
                if (gx_rsplit_times && &gx_c3_toff && gx_c3_toff == 0) {
                    unsigned long long o[25], b[2][8] = { { 0 } }; unsigned bn[2] = { 0, 0 }; gx_rsplit_times(o);
                    if (&g_gxm_ptc) { memcpy(b, g_gxm_ptc, sizeof b); bn[0] = g_gxm_pnc[0]; bn[1] = g_gxm_pnc[1]; }
                    if (rn >= 0 && rarm == gStfxAb && g_presents > rn) {
                        const double f = (double) (g_presents - rn) * 1000.0; double d[25]; for (int i = 0; i < 23; ++i) d[i] = (double) (o[i] - rp[i]) / f;
                        printf("[rsplit] presents %d..%d arm %d | dl entry %.2f = key+lookup %.2f VAT+listhash %.2f arrhash %.2f decode %.2f pre(tevkey,fog,tex) %.2f GPU %.2f [vs_build %.2f mesh %.2f (reupload %.2f) state %.2f drawTevGx %.2f]"
                               " CPU [xform %.2f light %.2f texgen %.2f expand %.2f backend %.2f] | imm %.2f [dec+xf %.2f light %.2f exp+submit %.2f] | tex %.2f | copy %.2f | calls gpu %.0f dlk %.0f /frame\n",
                               rn, g_presents, gStfxAb, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[11], d[9], d[10], d[12], d[13], d[14], d[15], d[16], d[17], d[20], d[21], d[22], d[18], d[19],
                               (double) (o[23] - rp[23]) * 1000.0 / f, (double) (o[24] - rp[24]) * 1000.0 / f);
                        for (int k = 0; k < 2; ++k) { const unsigned nd = bn[k] - rbn[k]; if (!nd) continue;
                            printf("[rsplit] presents %d..%d arm %d backend %s: %.0f draws/frame, ms/frame lookup %.2f vp %.2f fp %.2f stage %.2f scene %.2f state %.2f submit %.2f\n", rn, g_presents, gStfxAb, k ? "list" : "imm",
                                   nd * 1000.0 / f, (b[k][0] - rb[k][0]) / f, (b[k][1] - rb[k][1]) / f, (b[k][2] - rb[k][2]) / f, (b[k][3] - rb[k][3]) / f, (b[k][4] - rb[k][4]) / f, (b[k][5] - rb[k][5]) / f, (b[k][6] - rb[k][6]) / f); }
                    }
                    {   extern unsigned g_gxs_mm_st[6] __attribute__((weak)); extern void gxs_td_stats(unsigned o[4]) __attribute__((weak)); static unsigned pm[6]; static int pmn = -1;
                        if (&g_gxs_mm_st && gxs_td_stats) { unsigned td[4]; gxs_td_stats(td); const double f2 = (double) (g_presents - pmn);
                            if (pmn >= 0 && rn >= 0 && pmn == rn && rarm == gStfxAb && g_presents > pmn) printf("[rsplit] presents %d..%d arm %d memo: per frame one-key hits %.1f memo hits %.1f misses %.1f (empty %.1f same-program %.1f other-program %.1f) | td raw keys seen %u aliased %u over %u raw %u\n",
                                rn, g_presents, gStfxAb, (g_gxs_mm_st[0] - pm[0]) / f2, (g_gxs_mm_st[1] - pm[1]) / f2, (g_gxs_mm_st[2] - pm[2]) / f2, (g_gxs_mm_st[3] - pm[3]) / f2, (g_gxs_mm_st[4] - pm[4]) / f2, (g_gxs_mm_st[5] - pm[5]) / f2, td[0], td[1], td[2], td[3]);
                            memcpy(pm, g_gxs_mm_st, sizeof pm); pmn = g_presents; } }
                    memcpy(rp, o, sizeof rp); memcpy(rb, b, sizeof rb); rbn[0] = bn[0]; rbn[1] = bn[1]; rn = g_presents; rarm = gStfxAb;
                } else rn = -1;
            }
            {   /* R3c (host/engine/R3C_CACHE.md s2): the census of the three cuts, ONE line, cumulative counters (the judge takes deltas).  Counted whether or not a cut is on, where that is meaningful; all zero while every R3c cut is off. */
                extern unsigned gx_r3c_c5_total __attribute__((weak)), gx_r3c_c5_clean __attribute__((weak)), gx_r3c_c5_bumps __attribute__((weak));
                extern unsigned gx_r3c_c7_vat __attribute__((weak)), gx_r3c_c7_key __attribute__((weak)), gx_r3c_c7_calls __attribute__((weak));
                extern void rhi_gxm_c6_census(unsigned o[4]) __attribute__((weak));
                if (&gx_r3c_c5_total && &gx_r3c_c7_calls) {
                    unsigned c6[4] = { 0, 0, 0, 0 }; if (rhi_gxm_c6_census) rhi_gxm_c6_census(c6);
                    printf("[r3c] arm %d | C5 clean draws %u of %u (bumps %u) | C6 imm draws %u merged into %u draws (max run %u) | C7 vat-skips %u key-skips %u of %u calls\n", gStfxAb,
                           gx_r3c_c5_clean, gx_r3c_c5_total, gx_r3c_c5_bumps, c6[0], c6[1], c6[2], gx_r3c_c7_vat, gx_r3c_c7_key, gx_r3c_c7_calls);
                }
            }
            {   /* R6 (host/engine/R6_SKIN.md s3): the skinned-model refresh census, ONE line, cumulative counters; printed while C8 or C9 is on (the counters only move then) */
                extern unsigned gx_r3a __attribute__((weak));
                extern unsigned gx_r6_c8_calls __attribute__((weak)), gx_r6_c8_unch __attribute__((weak)), gx_r6_c8_pred __attribute__((weak)), gx_r6_c8_hash __attribute__((weak)), gx_r6_c8_ovf __attribute__((weak)), gx_r6_c8_miss __attribute__((weak));
                extern unsigned gx_r6_c9_part __attribute__((weak)), gx_r6_c9_fresh __attribute__((weak)), gx_r6_c9_ref __attribute__((weak));
                if (&gx_r3a && &gx_r6_c8_calls && &gx_r6_c9_part && (gx_r3a & 384u))
                    printf("[r6] arm %d | C8 dyn calls %u: unchanged %u, predicted %u, hashed %u (overflow %u), misses %u | C9 updates %u: partial %u, fresh %u, refused %u\n", gStfxAb,
                           gx_r6_c8_calls, gx_r6_c8_unch, gx_r6_c8_pred, gx_r6_c8_hash, gx_r6_c8_ovf, gx_r6_c8_miss, gx_r6_c9_part + gx_r6_c9_fresh + gx_r6_c9_ref, gx_r6_c9_part, gx_r6_c9_fresh, gx_r6_c9_ref);
            }
            {   /* R8 (host/engine/R8_GPUSKIN.md): the GPU-skin census, ONE line, window deltas of gGxSkinCtr (hal/gx_skin.h GXSK_*: 0 binds, 1 blends skipped, 2 refused table, 3 refused morph, 4 refused hal, 5 draws, 6 palette bytes); printed while skinning is on or any counter moved */
                extern unsigned gGxSkinCtr[7] __attribute__((weak));
                extern int gGxSkinOn __attribute__((weak));
                static unsigned r8p[7]; static int r8n = -1;
                if (&gGxSkinCtr && &gGxSkinOn) {
                    unsigned d[7], moved = 0; int i;
                    for (i = 0; i < 7; ++i) { d[i] = gGxSkinCtr[i] - r8p[i]; r8p[i] = gGxSkinCtr[i]; moved |= d[i]; }
                    unsigned r8fb = 0;   /* R8-FB: fallback draws (gx_skin.c gGxSkinFb[0], cumulative -> window delta); appended LAST so readers that parse the line's start still match */
                    {   extern unsigned gGxSkinFb[2] __attribute__((weak)); static unsigned fbp; if (&gGxSkinFb) { r8fb = gGxSkinFb[0] - fbp; fbp = gGxSkinFb[0]; moved |= r8fb; } }
                    if (r8n >= 0 && (gGxSkinOn || moved) && g_presents > r8n)
                        printf("[r8] arm %d | skin on %d | binds %u draws %u | blends skipped %u | refused table %u morph %u hal %u | palette %.1f KB/frame | hal fallback %u\n", gStfxAb, gGxSkinOn,
                               d[0], d[5], d[1], d[2], d[3], d[4], (double) d[6] / 1024.0 / (double) (g_presents - r8n), r8fb);
                    {   /* R8 refusal reasons, cumulative (gx_draw.c dlkSkinPrep comment: 2 POS base 3 NRM base 4 POS fmt 5 NRM fmt 6 index 7 NBT 8 no POS/idx 9 vs_check 10 vs_build 11 no mesh 12 backend 13 earlier return) */
                        extern unsigned gGxSkinWhy[32] __attribute__((weak));
                        if (&gGxSkinWhy && d[4]) { printf("[r8-why]"); for (i = 2; i < 14; ++i) printf(" %d:%u", i, gGxSkinWhy[i]); printf(" | vs_check(16+VSW):"); for (i = 16; i < 32; ++i) if (gGxSkinWhy[i]) printf(" %d:%u", i - 16, gGxSkinWhy[i]); printf("\n"); }
                    }
                    {   /* R8 per-model census (objprint_dolphin.c stfxSkc; marker gx_skin_census.txt): why a skinned model does / does not take the GPU path */
                        extern void stfxSkinCensusFlush(int arm, unsigned frames) __attribute__((weak));
                        if (stfxSkinCensusFlush && r8n >= 0 && g_presents > r8n) stfxSkinCensusFlush(gStfxAb, (unsigned)(g_presents - r8n));
                    }
                    {   /* R8-MF lifetime census (gx_skin.c gGxSkinLife, before the R8 default flip): did the deferred-free path run with a LIVE table? cumulative */
                        extern unsigned gGxSkinLife[4] __attribute__((weak));
                        extern unsigned gx_skin_retired_pending(void) __attribute__((weak));
                        static unsigned lifep[4];
                        if (&gGxSkinLife && (gGxSkinLife[0] != lifep[0] || gGxSkinLife[2] != lifep[2] || gGxSkinLife[3] != lifep[3])) {
                            printf("[r8-life] released %u (with a live table %u) | tables retired %u freed %u pending %u\n", (unsigned) gGxSkinLife[0], (unsigned) gGxSkinLife[1],
                                   (unsigned) gGxSkinLife[2], (unsigned) gGxSkinLife[3], gx_skin_retired_pending ? (unsigned) gx_skin_retired_pending() : 0u);
                            memcpy(lifep, gGxSkinLife, sizeof lifep);
                        }
                        {   /* hw_R8tour2: while anything is pending, the last tick's inputs (a retired table never freed on the device in hw_R8tour) */
                            extern unsigned gGxSkinTick[4] __attribute__((weak));
                            if (&gGxSkinTick && gx_skin_retired_pending && gx_skin_retired_pending())
                                printf("[r8-tick] gseq %u rdone %u ringOn %u | oldest pending stamp %u (frees when rdone - stamp >= 0, or ringOn 0)\n", gGxSkinTick[0], gGxSkinTick[1], gGxSkinTick[2], gGxSkinTick[3]);
                        }
                    }
                    r8n = g_presents;
                }
            }
            {   /* IMM census (gx_rt_special.c gxts_imm_census_flush; marker gx_imm_census.txt): the immediate GXBegin draws by game call site, window deltas */
                extern void gxts_imm_census_flush(int arm, unsigned frames) __attribute__((weak));
                static int imn = -1;
                extern unsigned gStfxGlowCtr[6] __attribute__((weak));   /* expgfx.c drawGlow (hw_KO2): calls, quads, texture switches, same-state runs, max quads/call; 5 = batched GXBegins (hw_GB) */
                static unsigned glp[6];
                if (gxts_imm_census_flush) { if (imn >= 0 && g_presents > imn) gxts_imm_census_flush(gStfxAb, (unsigned)(g_presents - imn)); }
                if (&gStfxGlowCtr) {
                    unsigned d[6] = { 0 }; int i;
                    for (i = 0; i < 6; ++i) if (i != 4) { d[i] = gStfxGlowCtr[i] - glp[i]; glp[i] = gStfxGlowCtr[i]; }   /* 4 is a running max, printed as is */
                    if (imn >= 0 && g_presents > imn)
                        printf("[glow] arm %d /%u fr: calls %u quads %u tex switches %u runs %u | max quads/call %u | batched GXBegins %u\n", gStfxAb, (unsigned)(g_presents - imn), d[0], d[1], d[2], d[3], gStfxGlowCtr[4], d[5]);
                }
                {   /* R-VB (host/engine/R_VB.md): proof the bulk VBYTES replay ran on the device -- gx_draw.c counts gxfifo_push_bytes calls (written by R; read here as a counter) */
                    extern unsigned gx_vb_bulk_calls __attribute__((weak));
                    static unsigned vbp;
                    if (&gx_vb_bulk_calls && imn >= 0 && g_presents > imn) { const unsigned v = gx_vb_bulk_calls; printf("[vb] arm %d /%u fr: bulk VBYTES calls %u\n", gStfxAb, (unsigned)(g_presents - imn), v - vbp); vbp = v; }
                }
                imn = g_presents;
            }
            {   /* R7 census (rhi_gxm_draw.c): consecutive drawTevGx calls that could be one instanced draw; cumulative; only while gxm_r7.txt is present (counters move only then) */
                extern void rhi_gxm_r7_census(unsigned o[8]) __attribute__((weak));
                if (rhi_gxm_r7_census) { unsigned r7[8]; rhi_gxm_r7_census(r7);
                    if (r7[0]) printf("[r7] arm %d | tevgx issued %u | joinable to the previous: A matrices-only %u, B +texmtx %u, C +colours %u | D same state other mesh %u | A-runs %u (max %u)\n",
                                      gStfxAb, r7[0], r7[1], r7[2], r7[3], r7[4], r7[5], r7[6]); }
            }
            {   /* R9 replay census (rhi_gxm_draw.c): each drawTevGx against its match one frame earlier; cumulative; only while gxm_r9.txt is present. Reader: host/engine/r9_read.py */
                extern void rhi_gxm_r9_census(unsigned o[12]) __attribute__((weak));
                if (rhi_gxm_r9_census) { unsigned r9[12]; rhi_gxm_r9_census(r9);
                    if (r9[0]) printf("[r9] arm %d presents %d | frames %u brk %u | tevgx issued %u | vs previous frame: NEW %u MESH %u STATE %u COLOUR %u TEXMTX %u MTX %u SAME %u | dropped %u | max/frame %u\n",
                                      gStfxAb, g_presents, r9[9], r9[11], r9[0], r9[1], r9[2], r9[3], r9[4], r9[5], r9[6], r9[7], r9[8], r9[10]); }
            }
            { extern void stfx_ph_report(int frames) __attribute__((weak)); if (stfx_ph_report && w_t) stfx_ph_report(g_presents - w_n); }   /* step 3: only prints when phase_stats.txt is 1 */
            vt_log_async_report();   /* Task 2: swaps / drops, one line per window */
            { extern unsigned rhi_vita_mip_chains, rhi_vita_filter_changes; extern unsigned long long rhi_vita_mip_us; printf("[filter] mip chains built %u (%.1f ms cumulative) | filter changes %u\n", rhi_vita_mip_chains, (double) rhi_vita_mip_us / 1000.0, rhi_vita_filter_changes); }   /* run 258 */
            { extern unsigned rhi_vita_mip_authored, rhi_vita_mip_authored_fail, rhi_vita_mip_clamped, rhi_vita_lodbias_changes; extern int rhi_vita_mip_verify;
              extern unsigned gx_tex_lod_notes, gx_tex_lod_drops, gx_tex_lodbias_binds;
              printf("[mips] authored chains %u (fail %u, clamped below 1x1 by max_lod %u) | verify %d | lod notes %u, DROPPED %u | biased binds %u, bias changes %u\n",
                     rhi_vita_mip_authored, rhi_vita_mip_authored_fail, rhi_vita_mip_clamped, rhi_vita_mip_verify, gx_tex_lod_notes, gx_tex_lod_drops, gx_tex_lodbias_binds, rhi_vita_lodbias_changes); }   /* run 273 */
            printf("[clk] now arm %d bus %d gpu %d xbar %d\n", scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
            printf("[ft] frames by vblank step: 16.7ms %u | 33.3ms %u | 50.0ms %u | 66.7ms %u | 83.3ms+ %u || mean |t - 50.0| inside the 50 ms bucket: %.2f ms\n",
                   vt_ft_hist[0], vt_ft_hist[1], vt_ft_hist[2], vt_ft_hist[3], vt_ft_hist[4], vt_ft_hist[2] ? vt_ft_dev50 / (vt_ft_hist[2] * 1000.0) : 0.0);
            vt_ft_hist[0] = vt_ft_hist[1] = vt_ft_hist[2] = vt_ft_hist[3] = vt_ft_hist[4] = 0; vt_ft_dev50 = 0;
            w_st = vt_status_us;
            {   unsigned long long thr[2] = { 0, 0 }; if (os_thread_times) thr[0] = os_thread_times(&thr[1]);
                if (w_t && &vi_w_us[0]) {
                    const double nf = (double) (g_presents - w_n) * 1000.0;
                    printf("[win] ... the retrace: pump + audio delivery %.1f | pre-retrace callback %.1f | endFrame + present %.1f | post-retrace callback %.1f | reopen %.1f || other threads held the CPU %.1f  (ms/frame)\n",
                           (double) (vi_w_us[0] - w_vi[0]) / nf, (double) (vi_w_us[1] - w_vi[1]) / nf, (double) (vi_w_us[2] - w_vi[2]) / nf,
                           (double) (vi_w_us[3] - w_vi[3]) / nf, (double) (vi_w_us[4] - w_vi[4]) / nf, (double) (thr[0] - w_thr[0]) / nf);
                }
                if (&vi_w_us[0]) memcpy(w_vi, vi_w_us, sizeof w_vi);
                w_thr[0] = thr[0]; w_thr[1] = thr[1];
                if (g_presents % 600 == 0) { extern void os_thread_report(void) __attribute__((weak)); if (os_thread_report) os_thread_report(); }
            }
            w_t = now; w_n = g_presents; memcpy(w_s, s1, sizeof w_s); memcpy(w_g, g1, sizeof w_g); memcpy(w_c, c1, sizeof w_c);
        }
    }
    vt_perf_ab(now);
    /* THREADED (RT_THREAD D12): decided AFTER vt_perf_ab, which is where gxt_frame_edge switches mode, so a transition at THIS present is already seen. G-side readers of backend state
     * (the captures, grade_frame, the draw journal, shot_now) must not run beside a replaying R: each is skipped with ONE line the first time it would have run. */
    const int thr = gxt_is_active && gxt_is_active();
    static SceUInt64 s_span0[4], s_x0[2];
    if (g_presents == TIME_FROM) {
        g_t_from = now; rhi_vita_timing(s_span0);
        if (vt_extra_timing) vt_extra_timing(s_x0);
    }
    if (g_presents == CAPTURE_AT && g_t_from) {
        const double nf = (double) (CAPTURE_AT - TIME_FROM);
        double ms = (double) (now - g_t_from) / 1000.0 / nf;
        printf("[time] presents %d..%d: %.2f ms/frame (%.1f fps), measured on device\n",
               TIME_FROM, CAPTURE_AT, ms, 1000.0 / ms);
        /* The same window, split. Both snapshots are taken here, BEFORE the real
         * present, so the window holds exactly nf swaps and nf frame builds. */
        SceUInt64 s1[4]; rhi_vita_timing(s1);
        double part[4];
        for (int i = 0; i < 4; ++i) part[i] = (double) (s1[i] - s_span0[i]) / 1000.0 / nf;
        { SceUID ns = sceIoOpen("ux0:data/STFX00001/STATS", SCE_O_RDONLY, 0777);
          printf("[stats] per-draw instruments %s\n", ns >= 0 ? "ON (STATS marker; frame time inflated ~42%%)" : "OFF (default)");
          if (ns >= 0) sceIoClose(ns); }
        printf("[split] frame CPU %.2f ms (of which append %.2f, GL flush %.2f) | swap %.2f | other %.2f  (ms/frame)\n",
               part[0], part[1], part[2], part[3], ms - part[0] - part[3]);
        if (vt_extra_timing) {
            SceUInt64 x1[2]; vt_extra_timing(x1);
            double dl = (double) (x1[0] - s_x0[0]) / 1000.0 / nf;
            printf("[split] inside GXCallDisplayList %.2f ms/frame over %.0f calls/frame "
                   "(incl. backend append+flush) | outside it, rest of frame CPU %.2f\n",
                   dl, (double) (x1[1] - s_x0[1]) / nf, part[0] - dl);
        }
    }
    /* ⚠ ONCE. set_frame_limit() RESETS the pump counter; calling it on every
     * present past the deadline zeroed the counter each frame, so the limit was
     * never exceeded and the map run never ended (2026-09-22: ~10 min holding
     * eboot.bin). */
    static int s_deadline_hit;
    if (!s_deadline_hit && g_t_first && now - g_t_first > (SceUInt64) RUN_SECONDS * 1000000u) {
        s_deadline_hit = 1;
        printf("[vita] run deadline (%d s) reached at present %d\n", RUN_SECONDS, g_presents);
        plat_window_vita_set_frame_limit(1);   /* next-but-one pump returns false: normal exit */
    }
#ifdef VT_PRESENT_LOG
    if (!thr_skip(thr, "draw journal")) {   /* The per-draw journal is OPT-IN (2026-09-27): ~4,000 draws x 2 lines through the per-call log
         * writer took over a minute of the capture frame (run 72: 3,475 draws logged 60 s after present
         * 599), which reads as a hang. ux0:data/STFX00001/JOURNAL turns it on. */
        extern int rhi_vita_journal;
        /* The marker may hold a present number (2026-09-28): the journal then covers THAT present,
         * so a gameplay frame can be journalled without moving CAPTURE_AT. Empty = CAPTURE_AT. */
        static int s_journal = -1, s_journal_at;
        if (s_journal < 0) {
            SceUID jf = sceIoOpen("ux0:data/STFX00001/JOURNAL", SCE_O_RDONLY, 0777);
            s_journal = jf >= 0;
            s_journal_at = CAPTURE_AT;
            if (jf >= 0) {
                char b[16] = {0};
                int n = sceIoRead(jf, b, sizeof b - 1);
                if (n > 0 && atoi(b) > 1) s_journal_at = atoi(b);
                sceIoClose(jf);
            }
            printf("[vita] draw journal at present %d: %s\n", s_journal_at, s_journal ? "ON (JOURNAL marker)" : "off");
        }
        if (s_journal && g_presents == s_journal_at - 1) { rhi_vita_journal = 1; log_hold(1); }
        if (s_journal && s_journal_at != CAPTURE_AT && g_presents == s_journal_at) {
            extern void rhi_vita_journal_end(void);
            rhi_vita_journal_end();
            log_hold(0);
            {   const GeomStats *st = rhi_vita_frame_stats();
                printf("[geom] present %d: tex %u calls %u tris | col %u calls %u tris | max_abs %.3f | batches %u | backend_draws %u\n",
                       g_presents, st->tex_calls, st->tex_tris, st->col_calls, st->col_tris, st->max_abs, st->batches, st->backend_draws);
            }
        }
    }
#endif
    if (g_presents == CAPTURE_AT && !thr_skip(thr, "CAPTURE_AT capture + grade_frame")) {
        /* BEFORE the swap: the buffer just drawn is still the one bound. */
#ifdef VT_PRESENT_LOG
        { extern void rhi_vita_journal_end(void); rhi_vita_journal_end(); log_hold(0); }
#endif
        grade_frame();
        {   const GeomStats *st = rhi_vita_frame_stats();
            printf("[geom] tex %u calls %u tris | col %u calls %u tris | sum_xyz %.6e | sum_mvp %.6e | max_abs %.3f | batches %u | backend_draws %u\n",
                   st->tex_calls, st->tex_tris, st->col_calls, st->col_tris, st->sum_xyz, st->sum_mvp, st->max_abs, st->batches, st->backend_draws);
        }
        bool ok = plat_window_capture_bmp(NULL, SHOT);
        printf("[vita] capture (pre-swap, present #%d) -> %s: %s\n",
               CAPTURE_AT, SHOT, ok ? "ok" : "FAILED");
    }
    /* A capture ON DEMAND (run 256): put any file at ux0:data/stairfax/shot_now.txt while the game runs and the next check
     * (every 30 presents) writes ux0:data/stairfax_shot.bmp, logs the present number and deletes the request -- that delete is
     * the proof it was seen. For "I am looking at the fault right now": the player stands still, the host asks, nobody has to
     * know a present number in advance (captures.txt below needs one before the boot). */
    {   extern int rhi_vita_census_arm;   /* run 259: the request below also runs the TEV census over the next two frames (printed when it ends) */
        if (rhi_vita_census_arm > 0) --rhi_vita_census_arm;
    }
    if ((g_presents % 30) == 0 && !thr_skip(thr, "shot_now poll")) {
        extern volatile int g_stfx_shot_req __attribute__((weak));   /* plat_window_vita.c's poller thread opens the file, not this thread (2026-10-28 hitch fix) */
        if (&g_stfx_shot_req && g_stfx_shot_req) {
            extern int rhi_vita_census_arm;
            rhi_vita_census_arm = 2;
            sceIoRemove("ux0:data/stairfax/shot_now.txt");   /* before the flag clears: the poller must not see the file again */
            g_stfx_shot_req = 0;
            printf("[vita] capture ON DEMAND (pre-swap, present #%d) -> ux0:data/stairfax_shot.bmp: %s\n", g_presents,
                   plat_window_capture_bmp(NULL, "ux0:data/stairfax_shot.bmp") ? "ok" : "FAILED");
        }
    }
    if (!thr_skip(thr, "captures.txt")) {   /* Extra captures (2026-09-27): ux0:data/stairfax/captures.txt lists present numbers, one per
         * line; each is captured pre-swap to ux0:data/stairfax_cap_<present>.bmp. To follow the game
         * through menus in ONE run. Absent file = none. The list is read once and logged. */
        static int s_caps[32], s_ncaps = -1;
        if (s_ncaps < 0) {
            FILE *cf = fopen("ux0:data/stairfax/captures.txt", "r");
            s_ncaps = 0;
            if (cf) { while (s_ncaps < 32 && fscanf(cf, "%d", &s_caps[s_ncaps]) == 1) s_ncaps++; fclose(cf); }
            printf("[vita] extra captures: %d listed\n", s_ncaps);
        }
        for (int i = 0; i < s_ncaps; i++)
            if (g_presents == s_caps[i]) {
                char p[64];
                snprintf(p, sizeof p, "ux0:data/stairfax_cap_%d.bmp", g_presents);
                printf("[vita] capture (pre-swap, present #%d) -> %s: %s\n", g_presents, p,
                       plat_window_capture_bmp(NULL, p) ? "ok" : "FAILED");
                /* The game's camera at this capture, in the form stfx_frame.py --mem reads it from
                 * retail (gCameraFovY, gCameraProjectionMatrix, gCameraViewMatrix,
                 * gCameraInverseViewMatrix): a shot that is framed differently from retail's is
                 * either the camera or the pose, and this says which (docs/47 run 112). */
                {   extern float Camera_GetFovY(void) __attribute__((weak));
                    extern float *Camera_GetProjectionMatrix(void) __attribute__((weak));
                    extern float *Camera_GetViewMatrix(void) __attribute__((weak));
                    extern float *Camera_GetInverseViewMatrix(void) __attribute__((weak));
                    /* Every live object's transform, as stfx_frame.py --objs prints retail's (OBJ lines):
                     * index classId romDefNo rot(3) scale localPos(3) worldPos(3) hasParent moveProgress.
                     * Run 116-117: camera, sequence frame and subtitles equal retail's, the actors in the
                     * picture do not -- this says which object and which field. Raw offsets, as the
                     * game's own STATIC_ASSERTs fix them (objanim_internal.h). */
                    {   extern void **ObjList_GetObjects(int *, int *) __attribute__((weak));
                        if (ObjList_GetObjects) {
                            int first = 0, cnt = 0; void **ol = ObjList_GetObjects(&first, &cnt);
                            for (int oi = 0; ol && oi < cnt; ++oi) {
                                const unsigned char *o = (const unsigned char *) ol[oi]; if (!o) continue;
                                const short *s = (const short *) o; const float *f = (const float *) o;
                                printf("[obj] %d %d %d %d %d %d %d %g %g %g %g %g %g %g %d %g %d %d %g %d\n", g_presents, oi, s[0x44 / 2], s[0x46 / 2], s[0], s[1], s[2],
                                       (double) f[2], (double) f[3], (double) f[4], (double) f[5], (double) f[6], (double) f[7], (double) f[8],
                                       *(const unsigned *) (o + 0x30) != 0, (double) f[0x98 / 4], s[0xa0 / 2], s[0xa2 / 2], (double) f[0x9c / 4], (signed char) o[0xad]);
                                /* the CloudRunner's and the player's first 6 joint matrices, both matrix buffers, as
                                 * stfx_frame.py --objs prints retail's JNT lines (run 123: object state equal, picture not) */
                                if (s[0x46 / 2] == 140 || s[0x46 / 2] == 31) {
                                    /* obj+0x7c is `banks`, a POINTER to the bank array: model = banks[bankIndex] */
                                    const unsigned char *const *bk = *(const unsigned char *const *const *) (o + 0x7c);
                                    int bi = (signed char) o[0xad];
                                    const unsigned char *m = bk && bi >= 0 && bi < 4 ? bk[bi] : NULL;
                                    for (int w = 0; m && w < 2; ++w) {   /* the two ObjAnimState blocks, raw bytes (ANS in retail's dump) */
                                        const unsigned char *st = *(const unsigned char *const *) (m + 0x2c + 4 * w);
                                        printf("[ans] %d %d %d ", g_presents, s[0x46 / 2], w);
                                        for (int k = 0; st && k < 0x68; ++k) printf("%02x", st[k]);
                                        printf("\n");
                                    }
                                    for (int b = 0; m && b < 2; ++b) {
                                        const float *jm = *(const float *const *) (m + 0xc + 4 * b);
                                        /* ModelFileHeader.jointCount + extraJointCount (run 130: the bones equal retail's, some
                                           of the bird's pieces are still misplaced -- the vertex-group slots past jointCount) */
                                        int nj = *(const unsigned char *const *) m ? (*(const unsigned char *const *) m)[0xf3] + (*(const unsigned char *const *) m)[0xf4] : 0;
                                        if (nj > 80) nj = 80;
                                        for (int j = 0; jm && j < nj; ++j) {
                                            const float *q = jm + 16 * j;
                                            printf("[jnt] %d %d %d %d %g %g %g %g %g %g %g %g %g %g %g %g\n", g_presents, s[0x46 / 2], b, j,
                                                   (double) q[0], (double) q[1], (double) q[2], (double) q[3], (double) q[4], (double) q[5],
                                                   (double) q[6], (double) q[7], (double) q[8], (double) q[9], (double) q[10], (double) q[11]);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    /* The sky DLL's state (engine/5, sky.h SkyTimeBlend) as stfx_frame.py --pmem gSkyState:0:7 and
                     * gSkyState:0x20c:19 read retail's: textures + ids, time of day, palettes, the override blend,
                     * phase. Run 122: the port's sky is 2-3x brighter than retail's and fog and blend are ruled out. */
                    {   extern unsigned char *gSkyState __attribute__((weak)); extern unsigned char gSkyCurrentLightColor[] __attribute__((weak));
                        if (&gSkyState && gSkyState) {
                            const unsigned char *k = gSkyState; const int *w = (const int *) k; const float *kf = (const float *) k;
                            printf("[sky] present %d texAId %d texBId %d | time %g | palettes %d %d %d %d %d %d %d %d | blend %g | phase %d prev %d sel %d | tex %p %p %p %p %p\n",
                                   g_presents, w[5], w[6], (double) kf[0x20c / 4], w[0x21c / 4], w[0x220 / 4], w[0x224 / 4], w[0x228 / 4], w[0x22c / 4], w[0x230 / 4],
                                   w[0x234 / 4], w[0x238 / 4], (double) kf[0x23c / 4], k[0x24f], (signed char) k[0x250], k[0x251],
                                   (void *) w[0], (void *) w[1], (void *) w[2], (void *) w[3], (void *) w[4]);
                            printf("[sky] present %d words 0x240.. %08x %08x %08x %08x %08x | light colour %02x%02x%02x%02x\n", g_presents,
                                   (unsigned) w[0x240 / 4], (unsigned) w[0x244 / 4], (unsigned) w[0x248 / 4], (unsigned) w[0x24c / 4], (unsigned) w[0x250 / 4],
                                   gSkyCurrentLightColor ? gSkyCurrentLightColor[0] : 0, gSkyCurrentLightColor ? gSkyCurrentLightColor[1] : 0,
                                   gSkyCurrentLightColor ? gSkyCurrentLightColor[2] : 0, gSkyCurrentLightColor ? gSkyCurrentLightColor[3] : 0);
                        }
                    }
                    if (Camera_GetFovY) printf("[cam] present %d fovY %.5g\n", g_presents, (double) Camera_GetFovY());
                    const char *nm[3] = { "proj", "view", "invview" };
                    float *m[3] = { Camera_GetProjectionMatrix ? Camera_GetProjectionMatrix() : NULL,
                                    Camera_GetViewMatrix ? Camera_GetViewMatrix() : NULL,
                                    Camera_GetInverseViewMatrix ? Camera_GetInverseViewMatrix() : NULL };
                    for (int k = 0; k < 3; ++k) if (m[k])
                        printf("[cam] present %d %s %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g\n", g_presents, nm[k],
                               (double) m[k][0], (double) m[k][1], (double) m[k][2], (double) m[k][3], (double) m[k][4], (double) m[k][5], (double) m[k][6], (double) m[k][7],
                               (double) m[k][8], (double) m[k][9], (double) m[k][10], (double) m[k][11], (double) m[k][12], (double) m[k][13], (double) m[k][14], (double) m[k][15]);
                }
            }
    }
    {   /* The object sequences' own frame counters (docs/47 run 116), in the form stfx_frame.py --mem
         * reads them from retail: per slot the sequence id and the slot's master frame
         * (gObjSeqSlotStreamTimeTable = last frame's slot frame, engine/2 ObjSeq_update), plus the
         * stream the sequence may be waiting on. Every 25 presents and whenever the set of running
         * slots changes, so a sequence's START present is known to the frame. */
        extern float gObjSeqSlotStreamTimeTable[] __attribute__((weak));
        extern short gObjSeqSlotSeqIdTable[] __attribute__((weak));
        extern int gObjSeqPreparingStreamSlot __attribute__((weak)), gObjSeqTimedStreamSlot __attribute__((weak));
        extern float gObjSeqStreamRemainingTime __attribute__((weak));
        extern unsigned char curSeqNo __attribute__((weak)), framesThisStep __attribute__((weak));
        extern unsigned char gObjSeqPreviousSlotResults[] __attribute__((weak));   /* 1 = an object's seq fn HOLDS the slot */
        static unsigned s_seq_sig = ~0u;
        {   /* the player's data table (bridge/game_player.c), once: built 1 and vehicle[5] 140 (SB_Cloudrun)
             * or the player's `table + offset` reads are zeros (run 118: Krystal never mounted) */
            extern int stairfax_player_table_built __attribute__((weak)); extern int lbl_80332EC0[] __attribute__((weak));
            static int s_pt;
            if (!s_pt) { s_pt = 1; printf("[player] data table built %d, vehicle[5] %d (want 1, 140)\n",
                                          &stairfax_player_table_built ? stairfax_player_table_built : -1,
                                          lbl_80332EC0 ? lbl_80332EC0[(0x13c + 5 * 4) / 4] : -1); }
        }
        if (gObjSeqSlotStreamTimeTable && gObjSeqSlotSeqIdTable && &gObjSeqPreparingStreamSlot) {
            unsigned sig = 0;
            for (int i = 0; i < 0x55; i++)
                if (gObjSeqSlotStreamTimeTable[i] > 0.0f) sig = sig * 31u + (unsigned) (i + 1) * 131u + (unsigned short) gObjSeqSlotSeqIdTable[i]
                                                                + (gObjSeqPreviousSlotResults ? gObjSeqPreviousSlotResults[i] * 7919u : 0u);
            if (sig != s_seq_sig || g_presents % 25 == 0) {
                char b[512]; int n = 0;
                for (int i = 0; i < 0x55 && n < (int) sizeof b - 40; i++)
                    if (gObjSeqSlotStreamTimeTable[i] > 0.0f)
                        n += snprintf(b + n, sizeof b - n, " s%d:id%d:f%.0f:r%d", i, gObjSeqSlotSeqIdTable[i], (double) gObjSeqSlotStreamTimeTable[i],
                                      gObjSeqPreviousSlotResults ? gObjSeqPreviousSlotResults[i] : -1);
                b[n] = 0;
                printf("[seq] present %d%s cur %d step %d | stream preparing %d timed %d remaining %.2f |%s\n", g_presents,
                       sig != s_seq_sig ? " CHANGE" : "", &curSeqNo ? curSeqNo : -1, &framesThisStep ? framesThisStep : -1,
                       gObjSeqPreparingStreamSlot, &gObjSeqTimedStreamSlot ? gObjSeqTimedStreamSlot : -9,
                       &gObjSeqStreamRemainingTime ? (double) gObjSeqStreamRemainingTime : -9.0, n ? b : " (no slot running)");
                s_seq_sig = sig;
            }
        }
    }
    if (g_presents == CAPTURE_AT + 1) g_t_after_capture = now;
#ifdef VT_AB_NODEPTH
    /* A/B in ONE run (docs/47 run 30): present CAPTURE_AT is captured as-is; presents
     * CAPTURE_AT+1.. draw with the depth test OFF and CAPTURE_AT+2 is captured to VT_AB_NODEPTH.
     * If the scene appears only in the second image, depth is what hides it. */
    /* Run 32: a THIRD arm between them -- GX clip z [-w,0] converted to GL's [-w,w]
     * (gx_draw.c gStfxGxZToGL), depth ON, captured at CAPTURE_AT+2; depth-off at CAPTURE_AT+4. */
    { extern int rhi_vita_force_nodepth; extern int gStfxGxZToGL __attribute__((weak));
      if (g_presents == CAPTURE_AT && &gStfxGxZToGL) gStfxGxZToGL = 1;
      if (g_presents == CAPTURE_AT + 2) {
          bool okz = plat_window_capture_bmp(NULL, VT_AB_ZGL);
          printf("[vita] A/B capture, GX z -> GL z (present #%d) -> %s: %s\n", g_presents, VT_AB_ZGL, okz ? "ok" : "FAILED");
          if (&gStfxGxZToGL) gStfxGxZToGL = 0;
          rhi_vita_force_nodepth = 1;
      }
      if (g_presents == CAPTURE_AT + 4) {
          bool ok2 = plat_window_capture_bmp(NULL, VT_AB_NODEPTH);
          printf("[vita] A/B capture, depth test OFF (present #%d) -> %s: %s\n", g_presents, VT_AB_NODEPTH, ok2 ? "ok" : "FAILED");
          rhi_vita_force_nodepth = 0;
      } }
#endif
#ifdef VT_AB_MAP
#ifndef VT_AB_MAP_ARM
#define VT_AB_MAP_ARM 3   /* run 66: map blocks; run 67 sets 4 (fur) */
#endif
    /* Run 66: what share of the frame are the MAP BLOCK lists (the only static, cacheable kind)?
     * gStfxPerfArm alternates 0 / 3 (3 = skip lists called under gx_draw_static_hint). */
    { extern int gStfxPerfArm __attribute__((weak));
      if (&gStfxPerfArm && g_presents > CAPTURE_AT + 4) gStfxPerfArm = (((g_presents - CAPTURE_AT) / 120) & 1) ? VT_AB_MAP_ARM : 0;
      if (&gStfxPerfArm && g_presents % 60 == 0) printf("[perf] arm %d at present %d\n", gStfxPerfArm, g_presents); }
#endif
#ifdef VT_AB_DECODE
    /* Run 65: the attribute-plan decoder vs the old per-vertex slot walk (gx_draw.c gStfxDecodeArm),
     * alternating every 120 presents after the capture; host-tested byte-identical (gx_port_test 7). */
    { extern int gStfxDecodeArm __attribute__((weak));
      if (&gStfxDecodeArm && g_presents > CAPTURE_AT + 4) gStfxDecodeArm = ((g_presents - CAPTURE_AT) / 120) & 1 ? 0 : 1;
      if (&gStfxDecodeArm && g_presents % 60 == 0) printf("[decode] arm %d at present %d\n", gStfxDecodeArm, g_presents); }
#endif
#ifdef VT_AB_PERF
    /* Run 64: where does the frame go? After the capture, gx_draw.c gStfxPerfArm cycles 0/1/2 every
     * 120 presents (0 normal, 1 no GPU submit, 2 no display lists); the [frame] averages are the arms. */
    { extern int gStfxPerfArm __attribute__((weak));
      if (&gStfxPerfArm && g_presents > CAPTURE_AT + 4) gStfxPerfArm = ((g_presents - CAPTURE_AT) / 120) % 3;
      if (&gStfxPerfArm && g_presents % 60 == 0) printf("[perf] arm %d at present %d\n", gStfxPerfArm, g_presents); }
#endif
#ifdef VT_AB_LIGHT
    /* Run 62: what does per-vertex lighting cost? After the capture, gx_draw.c gStfxLightArm alternates
     * 1/0 every 120 presents; the [frame] lines' 60-present averages are the two arms, interleaved. */
    { extern int gStfxLightArm __attribute__((weak));
      if (&gStfxLightArm && g_presents > CAPTURE_AT + 4) gStfxLightArm = ((g_presents - CAPTURE_AT) / 120) & 1 ? 0 : 1; }
#endif
#ifdef VT_AB_ANIM1
    /* Run 55, THREE arms in one run (bridge/game_scene.cpp gStfxAnimArm): CAPTURE_AT = translation +
     * scale (SHOT, with a raw-term [anim] trace of the frame's first models); CAPTURE_AT+2 =
     * translation only -> VT_AB_ANIM1; CAPTURE_AT+4 = neither (run 52's stage 3) -> VT_AB_ANIM2. */
    { extern int gStfxAnimArm __attribute__((weak)), gStfxAnimTrace __attribute__((weak));
      /* run 55's first build: the switch sat in a C++ anonymous namespace, the weak ref bound to
         nothing and the A/B silently did not run. Say so instead. */
      if (!&gStfxAnimArm && g_presents == CAPTURE_AT) printf("[vita] A/B anim: gStfxAnimArm NOT LINKED -- no arms captured\n");
      if (&gStfxAnimArm) {
          if (g_presents == CAPTURE_AT - 1 && &gStfxAnimTrace) gStfxAnimTrace = 3;
          if (g_presents == CAPTURE_AT) gStfxAnimArm = 1;
          if (g_presents == CAPTURE_AT + 2) {
              bool ok1 = plat_window_capture_bmp(NULL, VT_AB_ANIM1);
              printf("[vita] A/B capture, anim translation only (present #%d) -> %s: %s\n", g_presents, VT_AB_ANIM1, ok1 ? "ok" : "FAILED");
              gStfxAnimArm = 0;
          }
          if (g_presents == CAPTURE_AT + 4) {
              bool ok2 = plat_window_capture_bmp(NULL, VT_AB_ANIM2);
              printf("[vita] A/B capture, anim neither (present #%d) -> %s: %s\n", g_presents, VT_AB_ANIM2, ok2 ? "ok" : "FAILED");
          }
      } }
#endif
#ifdef VT_AB_TEXSEL1
    /* Run 51, THREE arms in one run (gx_draw.c gStfxTexArm): present CAPTURE_AT is arm 0 (GX_TEXMAP0,
     * the control, SHOT); CAPTURE_AT+2 is arm 1 (the map of the first TEX0-texgen TEV stage) to
     * VT_AB_TEXSEL1; CAPTURE_AT+4 is arm 2 (arm 1, indirect-stage draws skipped) to VT_AB_TEXSEL2.
     * Arm 1 stays on afterwards. The game animates between presents: compare regions, not bytes. */
    { extern int gStfxTexArm __attribute__((weak));
      if (&gStfxTexArm) {
          if (g_presents == CAPTURE_AT) gStfxTexArm = 1;
          if (g_presents == CAPTURE_AT + 2) {
              bool ok1 = plat_window_capture_bmp(NULL, VT_AB_TEXSEL1);
              printf("[vita] A/B capture, arm 1 tex0-stage texture (present #%d) -> %s: %s\n", g_presents, VT_AB_TEXSEL1, ok1 ? "ok" : "FAILED");
              gStfxTexArm = 2;
          }
          if (g_presents == CAPTURE_AT + 4) {
              bool ok2 = plat_window_capture_bmp(NULL, VT_AB_TEXSEL2);
              printf("[vita] A/B capture, arm 2 + indirect draws skipped (present #%d) -> %s: %s\n", g_presents, VT_AB_TEXSEL2, ok2 ? "ok" : "FAILED");
              gStfxTexArm = 1;
          }
      } }
#endif
    {   /* PER-SCENE GPU TIME (scene_gpu.c, 2026-10-11; off unless ux0:data/stairfax/scene_gpu.txt asks): the frame ends at
         * the real present, and the [scene] block is printed every 600 presents whatever quiet.txt says. Weak: the
         * viewers link this file without scene_gpu.o. */
        extern void scene_gpu_present(int present) __attribute__((weak));
        extern void scene_gpu_report(int present) __attribute__((weak));
        /* THE PRESENT (RT_THREAD D2): gxt_present is the real present when DIRECT (and counts the frame), a PRESENT record + the frame-ahead wait when THREADED. Weak: absent = the real one. */
        bool pr = gxt_present ? (gxt_present(rhi, sc) != 0) : __real_rhi_present(rhi, sc);
        if (scene_gpu_present) scene_gpu_present(g_presents);
        if (scene_gpu_report && g_presents % 600 == 0) scene_gpu_report(g_presents);
        {   /* gxm_stall.c (docs/54 R15). Same function, same present as vt_perf_ab(), which ran above: gStfxAb already holds this present's arm. The two calls are
             * order-independent by design (the window's arm is read from its interior presents). _present prints "[stall] presents P..Q ..." every 60th, only if something was timed. */
            extern void gxm_stall_arm(int present, int arm) __attribute__((weak));
            extern void gxm_stall_present(int present) __attribute__((weak));
            if (gxm_stall_arm) gxm_stall_arm(g_presents, gStfxAb);
            if (gxm_stall_present) gxm_stall_present(g_presents);
        }
        return pr;
    }
}

/* argv from ARGS, else the default. Tokens point into a static buffer. */
static int build_argv(char **argv, int cap)
{
    static char buf[512];
    /* No --index: upstream's own default, the largest UNSKINNED model. The first
     * run forced --index 0 (a 36-joint character), which upstream's viewer
     * cannot pose on ANY platform -- the host oracle collapses it identically. */
    static char *defaults[] = { VT_DEFAULT_ARGS };
    int argc = 0;
    argv[argc++] = "model_view";
    argv[argc++] = "--iso";
    argv[argc++] = DISC;

    FILE *f = fopen(ARGS, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        for (char *t = strtok(buf, " \t\r\n"); t && argc < cap; t = strtok(NULL, " \t\r\n"))
            argv[argc++] = t;
        printf("[vita] args from %s\n", ARGS);
    } else {
        for (unsigned i = 0; i < sizeof defaults / sizeof defaults[0]; ++i)
            argv[argc++] = defaults[i];
        printf("[vita] args: default (no %s)\n", ARGS);
    }
    return argc;
}

#ifdef VT_WATCHDOG
/* Run 4 of the game HUNG after VIInit: no frame was ever presented, so the pump (which
 * polls the lane's QUIT sentinel) never ran, `release` could not end it, and `destroy` is
 * refused by design -- a human had to press PS. This thread turns a hang into DATA:
 * if no present arrives for VT_WATCHDOG seconds it logs so and FAULTS ON PURPOSE. The
 * Vita coredump records EVERY thread's registers, so the dump names where the main thread
 * is spinning, and the fault ends the app through the system's own crash path. */
#include <psp2/kernel/threadmgr.h>
static int vt_watchdog(SceSize args, void *argp)
{
    (void) args; (void) argp;
    int last = -1, still = 0;
    unsigned c5 = 0, b5 = 0;   /* GX counters 5 s before the fault: SLOW (moving) vs STUCK (flat) */
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        if (g_presents != last) { last = g_presents; still = 0; continue; }
        ++still;
        if (still == 20 || still == 60) {
            /* A STALL REPORT with no fault (2026-09-27): the fault below leaves the crash dialog up and
             * blocks the next deploy until a human presses OK. Many stalls are wait loops that keep
             * calling padUpdate (run 71: pad reads continued, presents stopped), so pad_input.c records
             * padUpdate's caller; that address minus (&main's runtime - its link address) is in the ELF. */
            extern void *gStfxPadCaller __attribute__((weak));
            extern unsigned gStfxPadReads __attribute__((weak));
            char b[256];
            int n = snprintf(b, sizeof b, "[vita] stall: no present for %d s (presents=%d); padUpdate caller %p "
                             "(reads %u) | main at %p (subtract main's link address from both)\n",
                             still, g_presents, &gStfxPadCaller ? gStfxPadCaller : NULL,
                             &gStfxPadReads ? gStfxPadReads : 0u, (void *) &main);
            log_write(b, n);
            log_flush_all();   /* the main thread is not presenting: nobody else will write the buffer out */
            if (gxt_stall_report && gxt_is_active && gxt_is_active()) { gxt_stall_report(); log_flush_all(); }   /* the ring: head / pub / tail, R's last op, who waits on whom (RT_THREAD 7.1 3e) */
        }
        if (still == VT_WATCHDOG - 5) { c5 = vt_dl_calls(); b5 = vt_dl_bytes(); }
        if (still < VT_WATCHDOG) continue;
        char b[256];
        int n = snprintf(b, sizeof b, "[vita] watchdog: no present for %d s (presents=%d); last 5 s: "
                         "%u display lists, %u bytes (%s) -- deliberate fault for an all-thread snapshot\n",
                         still, g_presents, vt_dl_calls() - c5, vt_dl_bytes() - b5,
                         vt_dl_calls() != c5 ? "SLOW: still drawing" : "STUCK: no GX work");
        log_write(b, n);
        log_flush_all();
        if (gxt_stall_report && gxt_is_active && gxt_is_active()) { gxt_stall_report(); log_flush_all(); }   /* before the deliberate fault: the crash dump names threads, this names the RING's state */
        { extern void rhi_vita_shc_flush(unsigned) __attribute__((weak)); if (rhi_vita_shc_flush) rhi_vita_shc_flush(1500); }   /* queued program binaries reach the card before the deliberate fault */
        *(volatile int *) 0 = 0x57A11;   /* STALL */
    }
    return 0;
}
static void vt_watchdog_start(void)
{
    SceUID t = sceKernelCreateThread("stfx_watchdog", vt_watchdog, 0x10000100, 0x4000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    printf("[vita] watchdog %s (%d s without a present -> snapshot fault)\n", t >= 0 ? "armed" : "FAILED", VT_WATCHDOG);
}
#endif

#ifdef VT_EXIT_ON_STOP
void plat_window_vita_set_stop_hook(void (*fn)(const char *why));
/* The game never returns from main(): end the process from the pump, with the same
 * summary a returning tool prints. sceKernelExitProcess is the app's own clean exit,
 * not the lane-refused `destroy`. */
static void vt_stop(const char *why)
{
    if (gxt_quit) gxt_quit(why);   /* RT_THREAD 3.7: FIRST. QUIT record + bounded join of R; on failure gxt_r_unsafe is set and the backend teardown below is skipped */
    printf("[vita] stop (%s) after %d presents\n", why, g_presents);
    if (g_t_after_capture && g_presents > CAPTURE_AT + 2) {
        double ms = (double) (sceKernelGetProcessTimeWide() - g_t_after_capture) / 1000.0
                  / (double) (g_presents - CAPTURE_AT - 1);
        printf("[time] presents %d..%d: %.2f ms/frame (%.1f fps), whole post-capture run\n",
               CAPTURE_AT + 1, g_presents, ms, 1000.0 / ms);
    }
    vt_cr_shutdown();   /* T0-d co-runner (arms 8/9): flag + signal + one join bounded to 300 ms; a no-op if it never started */
    { extern void stfx_audio_shutdown(void) __attribute__((weak)); if (stfx_audio_shutdown) stfx_audio_shutdown(); }   /* asks the audio thread to stop and waits <= 1.5 s; never kills it */
    { extern void rhi_vita_shc_shutdown(void) __attribute__((weak)); if (rhi_vita_shc_shutdown) rhi_vita_shc_shutdown(); }   /* rhi_vita_tev.inc: flush the queued program-binary saves (<= 3 s), stop the writer and join it; never kills it */
    { extern void scene_gpu_shutdown(void) __attribute__((weak)); if (scene_gpu_shutdown) scene_gpu_shutdown(); }   /* scene_gpu.c: the poll thread, asked to stop and waited for <= 1.5 s; never killed */
    { extern void thp_worker_shutdown(void) __attribute__((weak)); if (thp_worker_shutdown) thp_worker_shutdown(); }   /* hal/thp_decode.c: the [movie-dec] totals; the decode thread is asked to stop, waited for <= 1.5 s, never killed */
    if (g_stfx_backend == STFX_BK_GXM) {   /* the native GXM backend: reverse-order teardown, bounded waits; the vitaGL path has none */
        if (&gxt_r_unsafe && gxt_r_unsafe) printf("[gxt] rhi_gxm_shutdown SKIPPED: R did not stop, the backend is not idle\n");   /* it assumes an idle backend owner */
        else rhi_gxm_shutdown();
    }
    vt_log_async_report();
    printf("done\n");
    log_flush_all();   /* the buffered log (run 196) */
    sceKernelExitProcess(0);
}
#endif

int main(void)
{
    /* THE LOG IS ROTATED, NOT REMOVED (run 246). Runs 243-245: the `[mapbank] mapUnload ... caller` line of three staff attempts
     * was lost, each time because the next launch deleted the log before it was pulled. The last three runs stay on the card as
     * LOG.1 (the previous run) .. LOG.3; the return codes are printed so a rotation that did not happen is visible in the log. */
    sceIoRemove(LOG ".3");
    int rot2 = sceIoRename(LOG ".2", LOG ".3");
    int rot1 = sceIoRename(LOG ".1", LOG ".2");
    int rot0 = sceIoRename(LOG, LOG ".1");
    if (rot0 < 0) sceIoRemove(LOG);   /* could not be kept: start clean, as before, and say so below */

    printf("stairfax %s (Vita)\n", VT_NAME);
#ifdef STFX_BUILD_ID
    printf("[build] id=%s (build_game_vita.sh stamps it; build_id.txt next to the eboot holds the same id and the eboot's sha256)\n", STFX_BUILD_ID);
#endif
    printf("[log] rotate: previous log -> %s.1 %s (rc %x; .1->.2 rc %x, .2->.3 rc %x; 80010002 = there was none)\n", LOG, rot0 < 0 ? "NOT KEPT" : "kept",
           (unsigned) rot0, (unsigned) rot1, (unsigned) rot2);
    /* CLOCKS (run 217+). The port asked for nothing, so every frame time before this ran at whatever the console
     * happened to be set to: Tyler's overlay toggles an overclock by hand, and the same build measured 20 fps
     * GPU-bound (runs 212/213) and 30-60 fps (play217). Request the maximum the OS grants and PRINT what was read
     * back, before and after. ux0:data/stairfax/clock.txt: "0" = request nothing; "ARM BUS GPU XBAR" = those. */
    {
        int want[4] = { 444, 222, 222, 166 }, rc[4] = { 0, 0, 0, 0 }, ask = 1;
        int b4[4] = { scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency() };
        FILE *cf = fopen("ux0:data/stairfax/clock.txt", "r");
        if (cf) { int v[4], n = fscanf(cf, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]); fclose(cf);
                  if (n >= 1 && v[0] == 0) ask = 0; else if (n == 4) { want[0] = v[0]; want[1] = v[1]; want[2] = v[2]; want[3] = v[3]; } }
        if (ask) { rc[0] = scePowerSetArmClockFrequency(want[0]); rc[1] = scePowerSetBusClockFrequency(want[1]);
                   rc[2] = scePowerSetGpuClockFrequency(want[2]); rc[3] = scePowerSetGpuXbarClockFrequency(want[3]); }
        printf("[clk] at boot arm %d bus %d gpu %d xbar %d | %s %d %d %d %d (rc %x %x %x %x) | actual arm %d bus %d gpu %d xbar %d\n",
               b4[0], b4[1], b4[2], b4[3], ask ? "requested" : "requested NOTHING (clock.txt 0), default would be", want[0], want[1], want[2], want[3],
               (unsigned) rc[0], (unsigned) rc[1], (unsigned) rc[2], (unsigned) rc[3],
               scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    }
    { extern void scene_gpu_init(void) __attribute__((weak)); if (scene_gpu_init) scene_gpu_init(); }   /* scene_gpu.c: reads scene_gpu.txt once, before vitaGL is up; absent = off */
    { extern void gxm_stall_init(void) __attribute__((weak)); if (gxm_stall_init) gxm_stall_init(); }   /* gxm_stall.c (docs/54 R15): reads gxm_stall.txt once; absent = off */
#ifdef VT_COLORED_HONORS_ALPHA
    { extern int rhi_vita_colored_honors_alpha; rhi_vita_colored_honors_alpha = 1; }
#endif
    char *argv[16];
    int argc = build_argv(argv, 16);
    printf("[vita] argv:");
    for (int i = 0; i < argc; ++i) printf(" %s", argv[i]);
    printf("\n");

#ifdef VT_SETENV_ISO
    /* The game's own DVDInit() takes no path: it mounts STAIRFAX_ISO (a directory or
     * an image). Set it before main so the unmodified boot path finds the disc. */
    setenv("STAIRFAX_ISO", DISC, 1);
    printf("[vita] STAIRFAX_ISO=%s\n", DISC);
    /* BRING YOUR OWN DISC (2026-10-27, the public release): the binary carries no retail bytes; they come from the player's image here, BEFORE
     * the game's main can read any of them. A missing / wrong / modified image stops on a full-screen message (no game code runs). */
    {   char why[200];
        const int dr = stfx_retail_load(DISC, why, (int) sizeof why);
        if (dr) { printf("[vita] disc check refused (%d): %s\n", dr, why); vt_disc_error_screen(dr); }
    }
#endif
#ifdef VT_EXIT_ON_STOP
    plat_window_vita_set_stop_hook(vt_stop);
#endif
#ifdef VT_WATCHDOG
    vt_watchdog_start();
#endif
    plat_window_vita_set_frame_limit(RUN_FRAMES);
    int rc = VT_TOOL(argc, argv);

    if (gxt_quit) gxt_quit("main returned");   /* RT_THREAD 3.7: FIRST in the epilogue as well */
    printf("[vita] %s returned %d after %d presents\n", VT_STR(VT_TOOL), rc, g_presents);
    if (g_t_after_capture && g_presents > CAPTURE_AT + 2) {
        double ms = (double) (sceKernelGetProcessTimeWide() - g_t_after_capture) / 1000.0
                  / (double) (g_presents - CAPTURE_AT - 1);
        printf("[time] presents %d..%d: %.2f ms/frame (%.1f fps), whole post-capture run\n",
               CAPTURE_AT + 1, g_presents, ms, 1000.0 / ms);
    }
    vt_cr_shutdown();   /* T0-d co-runner: bounded, see the block above gStfxAb */
    { extern void stfx_audio_shutdown(void) __attribute__((weak)); if (stfx_audio_shutdown) stfx_audio_shutdown(); }
    { extern void rhi_vita_shc_shutdown(void) __attribute__((weak)); if (rhi_vita_shc_shutdown) rhi_vita_shc_shutdown(); }   /* rhi_vita_tev.inc: flush the queued program-binary saves (<= 3 s), stop the writer and join it; never kills it */
    { extern void scene_gpu_shutdown(void) __attribute__((weak)); if (scene_gpu_shutdown) scene_gpu_shutdown(); }
    { extern void thp_worker_shutdown(void) __attribute__((weak)); if (thp_worker_shutdown) thp_worker_shutdown(); }   /* hal/thp_decode.c: the [movie-dec] totals; the decode thread is asked to stop, waited for <= 1.5 s, never killed */
    vt_log_async_report();
    printf("done\n");
    log_flush_all();
    sceKernelExitProcess(rc);
    return rc;
}
