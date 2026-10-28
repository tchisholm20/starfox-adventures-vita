// vi_shim.c - VI (video interface) on the host, backed by the RHI + platform window.
//
// The game rides the VI retrace as its frame heartbeat: VIInit() once, then a
// per-frame VIWaitForRetrace() that on hardware blocks until vblank and fires the
// registered pre/post-retrace callbacks (the game hangs its buffer swap on the
// pre-retrace one). Here VI owns the window/RHI/swapchain; VIWaitForRetrace pumps
// events, fires the callbacks, and presents a frame through the RHI.
//
// There is no GX draw path yet, so the presented frame is a flat clear: black when
// VISetBlack(1), otherwise a "VI alive" color. That is the honest state of the
// port - the real framebuffer contents arrive when gx_shim renders into it.

#include "port/vi_shim.h"
#include "port/plat_window.h"
#include "port/renderer/rhi.h"
extern void gx_draw_init(void) __attribute__((weak));
extern void gx_draw_setRhi(RhiInstance* rhi, RhiSwapchain* sc) __attribute__((weak));
extern void gx_draw_setRealSkin(int on) __attribute__((weak));

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct ViState {
    PlatWindow*      window;
    RhiInstance*     rhi;
    RhiSwapchain*    swapchain;
    int              width, height;
    bool             black;
    bool             initialized;
    uint32_t         retraceCount;
    VIRetraceCallback preCb;
    VIRetraceCallback postCb;
    void*            nextFrameBuffer;
    bool             frameOpen;
} ViState;

static ViState gVi;

// Open a frame for drawing: begin + clear to the current background. Game GX draws
// (issued before the next VIWaitForRetrace) accumulate into this open frame; the
// retrace then presents it. Clearing here - not at present - is what lets the
// geometry survive to the screen.
// Optional clear-color override (e.g. the sky day/night tint). Off until set.
static int   gClearSet;
static float gClearRGB[3];
void vi_set_clear_color(float r, float g, float b) {
    gClearRGB[0] = r; gClearRGB[1] = g; gClearRGB[2] = b; gClearSet = 1;
}

static void viOpenFrame(void) {
    if (!gVi.initialized || gVi.frameOpen) return;
    rhi_beginFrame(gVi.rhi);
    if (gVi.black)      rhi_clear(gVi.rhi, 0.0f, 0.0f, 0.0f, 1.0f);
    else if (gClearSet) rhi_clear(gVi.rhi, gClearRGB[0], gClearRGB[1], gClearRGB[2], 1.0f);
    else                rhi_clear(gVi.rhi, 0.10f, 0.18f, 0.10f, 1.0f);
    gVi.frameOpen = true;
}

static RhiBackend vi_pick_backend(void) {
    const char* g = getenv("STAIRFAX_GFX");
    if (g) {
        if (!strcmp(g, "vk") || !strcmp(g, "vulkan")) return RHI_BACKEND_VULKAN;
        if (!strcmp(g, "d3d12"))                      return RHI_BACKEND_D3D12;
        if (!strcmp(g, "d3d11"))                      return RHI_BACKEND_D3D11;
        if (!strcmp(g, "gl"))                         return RHI_BACKEND_GL;
    }
    return RHI_BACKEND_AUTO;
}

void VIInit(void) {
    if (gVi.initialized) return;
    memset(&gVi, 0, sizeof(gVi));
    gVi.width  = 1280;
    gVi.height = 720;
    gVi.black  = true;

    gVi.window = plat_window_create("Stairfax Temperatures (PC Port)", gVi.width, gVi.height);
    if (!gVi.window) { fprintf(stderr, "[vi] failed to create window\n"); return; }

    RhiCreateInfo ci = {0};
    ci.backend      = vi_pick_backend();
    ci.windowHandle = plat_window_native_handle(gVi.window);
    ci.width        = gVi.width;
    ci.height       = gVi.height;
    ci.vsync        = true;
    ci.debug        = false;
    ci.appName      = "Stairfax Temperatures";

    gVi.rhi = rhi_create(&ci);
    if (!gVi.rhi) { fprintf(stderr, "[vi] rhi_create failed\n"); return; }

    gVi.swapchain = rhi_swapchainCreate(gVi.rhi, ci.windowHandle, gVi.width, gVi.height, ci.vsync);
    if (!gVi.swapchain) { fprintf(stderr, "[vi] swapchain create failed\n"); return; }

    gVi.initialized = true;
    // The GX decoder (gx_draw.c) draws only once it has the RHI: upstream's engine wires it in
    // its bridge (game_scene.cpp), which the Vita game build does not link. Vita runs 22-23:
    // 128 display lists decoded per frame, ZERO backend draws, a black screen. Weak, so builds
    // without gx_draw link unchanged.
    if (gx_draw_init && gx_draw_setRhi) { gx_draw_init(); gx_draw_setRhi(gVi.rhi, gVi.swapchain); }
#ifdef __vita__
    // Real GX transform semantics: each vertex by the pos matrix its PNMTXIDX selects (else the
    // current one), then the projection. Only upstream's interim bridge renderer switched this on;
    // the game's own renderer (objprint_dolphin.c) needs it always. Off, every vertex of a
    // multi-bone character took the CURRENT matrix: the title's characters drew as huge spikes
    // (Vita runs 44-46).
    if (gx_draw_setRealSkin) gx_draw_setRealSkin(1);
    // The single-texture path samples the map of the first TEX0-texgen TEV stage, not GX_TEXMAP0
    // (gx_draw.c gStfxTexArm). Run 51 A/B, three arms in one run: map 0 is a normal-addressed
    // LIGHTING texture on characters and furniture, which drew as swimming rainbow noise; arm 1
    // textured the armchair correctly and put real colours on the characters.
    { extern int gStfxTexArm __attribute__((weak)); if (&gStfxTexArm) gStfxTexArm = 1; }
#endif
    printf("[vi] init: %s %dx%d\n", rhi_backendName(rhi_getBackend(gVi.rhi)), gVi.width, gVi.height);
    viOpenFrame(); // first frame ready to receive draws
}

void VIConfigure(GXRenderModeObj* rmode) {
    // The render mode carries the GC framebuffer dimensions (e.g. 640x480/528).
    // We present into a host-sized swapchain and will scale the framebuffer later;
    // for now the mode only marks that VI has been configured.
    (void)rmode;
}

void VISetNextFrameBuffer(void* fb) {
    gVi.nextFrameBuffer = fb;
}

void VISetBlack(bool black) {
    gVi.black = black;
}

void VIFlush(void) {
    // Commit pending VI register writes. No deferred state on the host yet.
}

// THE RETRACE, SPLIT (2026-10-08, docs/47 "Run 142"): microseconds in each part of VIWaitForRetrace, for the [win] line.
// [0] pump + AI delivery, [1] pre-retrace callback (+ the threads it wakes), [2] endFrame + present, [3] post-retrace callback
// (+ the threads it wakes), [4] reopening the frame.
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#define VIT_NOW() sceKernelGetProcessTimeWide()
#else
#define VIT_NOW() 0ull
#endif
unsigned long long vi_w_us[5];
#include "port/rt_timer.h"   /* Task 3: the retrace is the frame boundary and the WAIT category */
extern int gStfxPresented;
void VIWaitForRetrace(void) {
    if (!gVi.initialized) return;
    RT_EDGE(); RT_ENTER(RT_WAIT);   /* rt_stats.txt: close the frame that just ended, open the next if sampled; everything below is "wait" except the backend calls inside it */
    unsigned long long v0 = VIT_NOW(), v1;
    { extern void os_virtual_clock_retrace(void) __attribute__((weak)); if (os_virtual_clock_retrace) os_virtual_clock_retrace(); }   // os_shim.c: fixed_clock.txt

    // Window/OS events first; a close request stops the heartbeat cleanly.
    if (!plat_window_pump(gVi.window)) {
        // Leave initialized so callers can detect close via vi_shim_shouldClose.
    }

    // Interrupts, delivered here at a safe point (vita fork, 2026-09-27): the AI DMA ticks and the
    // retrace callbacks run inside os_irq_enter/exit, so a thread they wake preempts only after the
    // "handler" returns (hal/os_threads.c).
#if defined(TARGET_PC) && !defined(_WIN32)
    { extern void ai_shim_deliver(void); ai_shim_deliver(); }
    { extern void os_irq_enter(void); os_irq_enter(); }
#endif
    // THE GX FIFO BREAKPOINT (vita fork, 2026-10-09, docs/47 "Run 157"): an armed breakpoint has been "reached" by now (the
    // port draws synchronously), and on the hardware it precedes the frame's draw sync and copy, so it is delivered before
    // the pre-retrace callback. gx_draw.c owns it; 0 back = gx_bp.txt turned it off and the run-156 block below stands in.
    int bpDelivered = 0;
#if defined(TARGET_PC) && !defined(_WIN32)
    { extern int gx_breakpoint_deliver(void) __attribute__((weak)); if (gx_breakpoint_deliver) bpDelivered = gx_breakpoint_deliver(); }
#endif
    v1 = VIT_NOW(); vi_w_us[0] += v1 - v0; v0 = v1;
    // Pre-retrace callback: the game swaps display buffers here.
    if (gVi.preCb) gVi.preCb(gVi.retraceCount);
#if defined(TARGET_PC) && !defined(_WIN32)
    { extern void os_irq_exit(void); os_irq_exit(); }
#endif

    v1 = VIT_NOW(); vi_w_us[1] += v1 - v0; v0 = v1;
    // Present the frame the game just drew into (or a bare clear if it drew
    // nothing). The GX draw path (gx_draw) has been filling this open frame.
    if (!gVi.frameOpen) viOpenFrame();
    rhi_endFrame(gVi.rhi);
    rhi_present(gVi.rhi, gVi.swapchain);
    gVi.frameOpen = false;
    v1 = VIT_NOW(); vi_w_us[2] += v1 - v0; v0 = v1;

    // THE MOVIE'S TEXTURE SETS COME BACK HERE (vita fork, 2026-10-09, docs/47 "the frozen laptop"). The game returns a displayed THP
    // texture set to the decoder in videoBreakPointCallback (pi_dolphin.c:4390), which hangs on a GX FIFO BREAKPOINT interrupt. The
    // port has no FIFO and never raises one, so no set ever came back: three frames decoded, the decoder blocked on its free queue,
    // and after ten idle frames n_attractmode.c:427 tore the movie down and prepared it again -- ~49 times in 150 decoded frames
    // (run 154), the frozen, flashing laptop screen Tyler reported. The frame has just been presented: that is the moment the
    // breakpoint marks. Only this one duty of the callback is done here; its flip-queue and depth-read parts are untouched.
    // SINCE RUN 157 the whole callback is delivered above (bpDelivered) and this block is the gx_bp.txt 0 arm only.
#if defined(TARGET_PC) && !defined(_WIN32)
    {   extern int gAttractMovieState __attribute__((weak));
        extern void THPPlayerPostDrawDone(void) __attribute__((weak));
        extern void os_irq_enter(void); extern void os_irq_exit(void);
        if (!bpDelivered && THPPlayerPostDrawDone && &gAttractMovieState && (gAttractMovieState == 2 || gAttractMovieState == 3)) {
            os_irq_enter(); THPPlayerPostDrawDone(); os_irq_exit();
        }
    }
#endif

    // Post-retrace callback: the game runs its GPU error/metrics handler here (and the THP player's
    // PlayControl while a movie plays). An interrupt: see os_irq_enter above.
#if defined(TARGET_PC) && !defined(_WIN32)
    { extern void os_irq_enter(void); os_irq_enter(); }
#endif
    if (gVi.postCb) gVi.postCb(gVi.retraceCount);
#if defined(TARGET_PC) && !defined(_WIN32)
    { extern void os_irq_exit(void); os_irq_exit(); }
#endif

    gVi.retraceCount++;
    v1 = VIT_NOW(); vi_w_us[3] += v1 - v0; v0 = v1;

    // Reopen a fresh frame so the next iteration's GX draws have somewhere to land.
    viOpenFrame();
    vi_w_us[4] += VIT_NOW() - v0;
    gStfxPresented = 1;
    RT_LEAVE();
}

// THE GAME'S OWN FRAME LOOPS (vita fork, 2026-10-10, docs/47 "Runs 175/179"). On the GameCube the retrace is an interrupt, so
// any loop that calls waitNextFrame() is a frame loop: 36 call sites (memory-card screens, loading and file waits, a boss,
// audio streaming). The port presented and delivered the retrace ONLY from main()'s loop (the hook of runs 19-21), so inside
// every other loop the game ran, read the pad, and showed NOTHING -- runs 175 and 179: the idle title stops at present 3597,
// exactly, inside showMemCardError (intersect_memcard.c:514) with the pad still being read. waitNextFrame() now calls this
// first: if no retrace has happened since the last frame start, this loop is not main's, and the retrace happens here.
// ux0:data/stairfax/nested_present_off.txt starting with 1 = the old behaviour (A/B).
int gStfxPresented;
unsigned stfx_nested_presents;
void stfx_frame_start(void) {
    static int started, off = -1;
    if (off < 0) { off = 0;
#if defined(__vita__)
        { FILE* f = fopen("ux0:data/stairfax/nested_present_off.txt", "r"); off = f && fgetc(f) == 49; if (f) fclose(f); }
        printf("[vi] the game's own frame loops present: %s\n", off ? "OFF (nested_present_off.txt)" : "on");
#endif
    }
    if (started && !gStfxPresented && !off) { ++stfx_nested_presents; VIWaitForRetrace(); }
    gStfxPresented = 0; started = 1;
}

int VIGetNextField(void)  { return (int)(gVi.retraceCount & 1); }
int VIGetTvFormat(void)   { return 0; /* NTSC */ }
int VIGetDTVStatus(void)  { return 0; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = gVi.preCb;
    gVi.preCb = cb;
    return prev;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = gVi.postCb;
    gVi.postCb = cb;
    return prev;
}

PlatWindow*  vi_host_window(void) { return gVi.window; }
RhiInstance* vi_host_rhi(void)    { return gVi.rhi; }

void* vi_shim_getWindow(void) {
    return gVi.window ? plat_window_native_handle(gVi.window) : NULL;
}
void vi_shim_pollEvents(void) {
    if (gVi.window) plat_window_pump(gVi.window);
}
int vi_shim_shouldClose(void) {
    if (!gVi.window) return 1;
    // plat_window_pump returns false once closed; probe without consuming more.
    return plat_window_pump(gVi.window) ? 0 : 1;
}
