/* plat_window_vita.c - the PlatWindow shim for PS Vita.
 *
 * Upstream's plat_window.h owns an OS window and hands its native handle to a
 * backend for swapchain creation. The Vita has neither: one fixed 960x544 panel,
 * no window manager, no HWND. So this is mostly an honest set of answers to
 * questions that do not apply -- with two parts that do real work.
 *
 * 1. plat_window_pump() is the only place a Vita build can be asked to quit.
 *    There is no window close button, so it reads the pad and returns false on
 *    START+SELECT. Without that the only exit is the PS button, which on this
 *    console has left an app holding memory that FTPVita then could not get a
 *    transfer buffer from ("550 Could not allocate memory" to a sixteen-byte
 *    upload). A clean exit path is a deploy-loop concern, not a nicety.
 *
 * 2. plat_window_capture_bmp() is implemented for real, via glReadPixels.
 *    ⚠ THIS IS THE ONLY VERIFICATION CHANNEL THAT SURVIVES HEADLESS RUNNING.
 *    Vita3K is driven with SDL's dummy video driver here (run_emu.sh), so there
 *    is no window to screenshot and "it rendered" would otherwise rest on the
 *    process not crashing. A BMP on disk can be checked by a script: the centre
 *    pixel either carries the clear colour or it does not.
 *
 *    ⚠ And it reads back what the GL pipeline actually produced, NOT the source
 *    colour. This repo has the scar: for a long time every capture read the
 *    composite BEFORE the blit, so a change to the blit, upscale or dither was
 *    invisible to it (CLAUDE.md records grab-panel.ps1 as the post-blit fix).
 *    glReadPixels here runs before the swap, so it reads the rendered buffer --
 *    which is the right target for a clear-and-present milestone, and is NOT a
 *    claim about what the panel scans out.
 */

#include "port/plat_window.h"
#include "stfx_backend.h"   /* the gxm backend path must never reach glReadPixels */

#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VITA_SCR_W 960
#define VITA_SCR_H 544

struct PlatWindow {
    int w, h;
    int pad_inited;
};

static struct PlatWindow g_win;

PlatWindow *plat_window_create(const char *title, int width, int height)
{
    (void) title;   /* no title bar exists to put it in */
    /* The requested size is kept for reporting only. The panel cannot change, so
     * honouring a 1280x720 request would be a fiction; see rhi_vita.c, which
     * clamps the viewport and records both numbers. */
    g_win.w = (width  > 0) ? width  : VITA_SCR_W;
    g_win.h = (height > 0) ? height : VITA_SCR_H;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    g_win.pad_inited = 1;
    return &g_win;
}

void plat_window_destroy(PlatWindow *w) { (void) w; }

void *plat_window_native_handle(PlatWindow *w)
{
    (void) w;
    /* NULL is the correct answer, not a failure. Every backend here ignores the
     * handle because there is no window to bind to; rhi_vita.c's
     * swapchainCreate takes (void)windowHandle deliberately. */
    return NULL;
}

/* 0 = no limit. Set by an entry point whose upstream loop only ends when the
 * window closes (model_view runs 100,000 frames), so a headless run still
 * exits through its NORMAL path rather than needing the PS button. */
static int g_frame_limit, g_frames_pumped;

void plat_window_vita_set_frame_limit(int frames)
{
    g_frame_limit   = frames;
    g_frames_pumped = 0;
}

/* The lane's QUIT sentinel (tools/quit-app.ps1, sent by `device.py release`).
 * ⚠ Until 2026-09-22 NO Stairfax build polled it, so every `release` was only a
 * lane handover: a map_view run that never exited held eboot.bin for ~10 min,
 * every later push 550'd "File not found", and it took a human pressing the PS
 * button to clear it (destroy is refused, by design). Polled every 30 pumps:
 * an sceIoOpen per frame is not free, and a half-second answer is plenty. */
#define QUIT_SENTINEL "ux0:data/STFX00001/QUIT"

/* THE POLLER THREAD (2026-10-28, hitch census on eboot 76c12fa3): the game thread used to sceIoOpen this sentinel every 30 pumps,
 * and vita_tool_main.c's shot_now.txt every 30 presents. A card open waits behind whatever else is on the card (the shader worker
 * writing GXPs, streaming): all 40 pump-dominated long frames of three sessions sat on that 30-frame beat, median 132-147 ms,
 * worst 6.0 s -- 47% of the long-frame excess of a 13.5-minute session. The opens now run on a low-priority thread every 500 ms
 * and the game thread reads flags. Any open slower than 20 ms is logged ([poll] slow open), which is the direct test of the
 * contention theory the census could only infer. */
static volatile int g_poll_quit;
volatile int g_stfx_shot_req;   /* vita_tool_main.c: a shot_now.txt request is waiting (the game thread takes the capture) */
static unsigned g_poll_slow, g_poll_n;
static uint32_t g_poll_max_us;

static int poll_open(const char *path)
{
    const uint32_t t0 = sceKernelGetProcessTimeLow();
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    const uint32_t us = sceKernelGetProcessTimeLow() - t0;
    ++g_poll_n;
    if (us > g_poll_max_us) g_poll_max_us = us;
    if (us > 20000u) { ++g_poll_slow; printf("[poll] slow open: %s took %.1f ms (slow %u of %u opens, max %.1f ms)\n", path, us / 1000.0, g_poll_slow, g_poll_n, g_poll_max_us / 1000.0); }
    if (f < 0) return 0;
    sceIoClose(f);
    return 1;
}

static int poll_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    for (;;) {
        if (!g_poll_quit && poll_open(QUIT_SENTINEL)) g_poll_quit = 1;
        if (!g_stfx_shot_req && poll_open("ux0:data/stairfax/shot_now.txt")) g_stfx_shot_req = 1;
        sceKernelDelayThread(500 * 1000);
    }
    return 0;
}

static bool quit_requested(void)
{
    static int started;
    if (!started) {
        started = 1;
        /* 0x10000100 / 0x4000 = the watchdog's parameters, known to work (0x10000100 + 32 returned an error on hw, eboot 0691d8c2).
         * Priority barely matters: the thread sleeps 500 ms and blocks only itself inside sceIoOpen. */
        const SceUID t = sceKernelCreateThread("stfx_poll", poll_thread, 0x10000100, 0x4000, 0, 0, NULL);
        const int rs = t >= 0 ? sceKernelStartThread(t, 0, NULL) : -1;
        printf("[vita] poller thread %s (QUIT sentinel + shot_now.txt off the game thread) create 0x%08x start 0x%08x\n",
               t >= 0 && rs >= 0 ? "started" : "FAILED", (unsigned) t, (unsigned) rs);
    }
    if (!g_poll_quit) return false;
    sceIoRemove(QUIT_SENTINEL);   /* consumed: that delete is the proof it was seen */
    printf("[vita] pump: QUIT sentinel consumed, returning false\n");
    return true;
}

/* An entry point whose loop IGNORES the pump's false -- the game's own main() is
 * `do { gameLoop(); } while (1)`, and vi_shim only records the close -- registers a
 * hook that ends the process. Called with the reason, instead of returning false.
 * Without it neither the run limit nor the lane's QUIT could stop the game. */
static void (*g_stop_hook)(const char *why);
void plat_window_vita_set_stop_hook(void (*fn)(const char *why)) { g_stop_hook = fn; }
static bool stop(const char *why)
{
    if (g_stop_hook) g_stop_hook(why);   /* does not return */
    return false;
}

bool plat_window_pump(PlatWindow *w)
{
    if (!w) return false;
    if (quit_requested()) return stop("QUIT sentinel");
    if (g_frame_limit > 0 && ++g_frames_pumped > g_frame_limit) {
        /* Say so ONCE: a map run whose log showed neither the tool's own
         * exit line nor the wrapper's could not tell "the loop never saw
         * false" from "it hung on the way out". This line separates them. */
        static int said;
        if (!said) { said = 1; printf("[vita] pump: run limit reached, returning false\n"); }
        return stop("run limit");
    }
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT))
            return stop("Start+Select");   /* the deliberate quit gesture */
    }
    return true;
}

void plat_window_size(PlatWindow *w, int *outW, int *outH)
{
    /* Reports the PANEL, not the requested size, because this is what a caller
     * sizing a viewport or a readback needs to be right about. */
    (void) w;
    if (outW) *outW = VITA_SCR_W;
    if (outH) *outH = VITA_SCR_H;
}

static void put32le(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char) (v      & 0xFF);
    p[1] = (unsigned char) (v >> 8 & 0xFF);
    p[2] = (unsigned char) (v >> 16 & 0xFF);
    p[3] = (unsigned char) (v >> 24 & 0xFF);
}

bool plat_window_capture_bmp(PlatWindow *w, const char *path)
{
    (void) w;
    if (!path) return false;
    if (g_stfx_backend == STFX_BK_GXM) { printf("[vita] capture %s skipped: the gxm backend (M1) has no glReadPixels path\n", path); return false; }

    const int W = VITA_SCR_W, H = VITA_SCR_H;
    unsigned char *rgba = (unsigned char *) malloc((size_t) W * H * 4);
    if (!rgba) return false;

    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    /* 24-bit BMP, rows bottom-up. glReadPixels' origin is the LOWER-left, which
     * is already BMP's row order -- so no flip. Getting this backwards yields a
     * vertically mirrored image that still "looks rendered", which is exactly
     * the kind of plausible-but-wrong artefact worth naming. */
    const int rowRaw  = W * 3;
    const int rowPad  = (4 - (rowRaw % 4)) % 4;
    const int rowSize = rowRaw + rowPad;
    const unsigned int pixBytes = (unsigned int) rowSize * (unsigned int) H;
    const unsigned int fileSize = 54u + pixBytes;

    unsigned char hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put32le(hdr + 2, fileSize);
    put32le(hdr + 10, 54);              /* pixel data offset */
    put32le(hdr + 14, 40);              /* BITMAPINFOHEADER  */
    put32le(hdr + 18, (unsigned) W);
    put32le(hdr + 22, (unsigned) H);
    hdr[26] = 1;                        /* planes            */
    hdr[28] = 24;                       /* bits per pixel    */
    put32le(hdr + 34, pixBytes);

    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { free(rgba); return false; }
    sceIoWrite(fd, hdr, sizeof hdr);

    unsigned char *row = (unsigned char *) malloc((size_t) rowSize);
    if (!row) { sceIoClose(fd); free(rgba); return false; }
    memset(row, 0, (size_t) rowSize);

    for (int y = 0; y < H; ++y) {
        const unsigned char *src = rgba + (size_t) y * W * 4;
        for (int x = 0; x < W; ++x) {
            /* BMP stores B,G,R; glReadPixels gave R,G,B,A. */
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        sceIoWrite(fd, row, (unsigned) rowSize);
    }

    sceIoClose(fd);
    free(row);
    free(rgba);
    return true;
}
