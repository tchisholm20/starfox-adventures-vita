// vi_smoketest.c - drive the VI shim the way the game does, without the game.
//
// Mirrors pi_videoinit.c / gameloop_main.c: VIInit, register pre/post-retrace
// callbacks, blank, then spin VIWaitForRetrace as the frame heartbeat. Proves the
// VI interface owns the window and drives the RHI, and that the retrace callbacks
// fire once per frame (the hook the game's buffer swap depends on).
//
// Backend via env STAIRFAX_GFX=vk|d3d12|d3d11. Optional --capture writes a BMP.

#include "port/vi_shim.h"
#include "port/plat_window.h"
#include "port/renderer/rhi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#endif

static uint32_t gPreCount  = 0;
static uint32_t gPostCount = 0;
static uint32_t gLastRetrace = 0;

static void onPreRetrace(uint32_t count)  { gPreCount++;  gLastRetrace = count; }
static void onPostRetrace(uint32_t count) { gPostCount++; (void)count; }

int main(int argc, char** argv) {
    const char* capturePath = NULL;
    int frames = 240;
    int captureFrame = 60;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--frames")  && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capturePath = argv[++i];
    }

    // --- video bring-up, as pi_videoinit.c::videoInit does ---
    VIInit();
    if (!vi_host_rhi()) { fprintf(stderr, "[vi_smoketest] VIInit failed\n"); return 2; }

    VISetPreRetraceCallback(onPreRetrace);
    VISetPostRetraceCallback(onPostRetrace);
    VIConfigure(NULL);
    VISetBlack(true);
    VIFlush();
    VIWaitForRetrace();   // pi_videoinit.c:143
    VIWaitForRetrace();   // pi_videoinit.c:144

    printf("[vi_smoketest] backend=%s after-setup retrace=%u pre=%u post=%u\n",
           rhi_backendName(rhi_getBackend(vi_host_rhi())), gLastRetrace, gPreCount, gPostCount);

    // --- main loop heartbeat ---
    VISetBlack(false); // reveal the "VI alive" color
    int captured = 0;
    for (int f = 0; f < frames; ++f) {
        if (vi_shim_shouldClose()) break;
        VIWaitForRetrace();

        if (capturePath && !captured && f >= captureFrame) {
            sleep_ms(60);
            if (plat_window_capture_bmp(vi_host_window(), capturePath))
                printf("[vi_smoketest] captured frame %d -> %s\n", f, capturePath);
            else
                fprintf(stderr, "[vi_smoketest] capture failed\n");
            captured = 1;
            frames = f + 3;
        }
    }

    printf("[vi_smoketest] done: total retrace=%u preCb=%u postCb=%u (callbacks fired once/frame: %s)\n",
           gPostCount, gPreCount, gPostCount, (gPreCount == gPostCount) ? "yes" : "NO");
    return 0;
}
