// game_boot.c - run REAL Star Fox Adventures boot code natively against the port HAL.
//
// This is the "it's a port, not an emulator" milestone: our host main() calls the
// game's own videoInit() (compiled from src/main/pi_videoinit.c to native x86),
// which programs VI + the copy pipeline through our shims, then we spin the game's
// own waitNextFrame() as the frame heartbeat. The CPU is running the game's actual
// compiled functions - only the hardware peripherals (VI/GX) are our reimplementation.

#include "dolphin/os.h"
#include "dolphin/gx/GXStruct.h"
#include "port/vi_shim.h"
#include "port/plat_window.h"
#include "port/renderer/rhi.h"
#include "port/gx_draw.h"
#include <stdio.h>
#include <string.h>

// Real game functions (compiled from src/main/pi_videoinit.c).
extern void videoInit(void* unusedRenderMode, int unusedArg);
extern void waitNextFrame(void);
extern GXRenderModeObj  gHostRenderMode;
extern GXRenderModeObj* gRenderModeObj;

// A draw submitted in the VTXFMT0 format videoInit programmed into gx_draw.
extern void game_boot_draw_test(int frame);

int main(int argc, char** argv) {
    const char* capturePath = NULL; int frames = 180;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--capture") && i+1 < argc) capturePath = argv[++i];
        else if (!strcmp(argv[i], "--frames")  && i+1 < argc) frames = atoi(argv[++i]);
    }

    setbuf(stdout, NULL); // unbuffered: keep step markers if the game code faults
    printf("[boot] Stairfax Temperatures - booting real game video init\n");

    // The prologue of the game's init(): OSInit / VIInit are our shims.
    printf("[boot] OSInit()...\n");   OSInit();
    printf("[boot] VIInit()...\n");   VIInit();
    gRenderModeObj = &gHostRenderMode;
    // Hand the GX draw path the VI-owned RHI so the game's GX calls render.
    gx_draw_init();
    gx_draw_setRhi(vi_host_rhi(), NULL);

    // Real game code: configure video + the GX copy pipeline.
    static unsigned char videoWork[0x3E8];
    printf("[boot] videoInit()...\n"); videoInit(videoWork, 0);
    printf("[boot] videoInit() returned; entering game frame loop\n");

    // Spin the game's own frame-pacing function. waitNextFrame() -> VIWaitForRetrace
    // -> our present, and fires the game's registered retrace callbacks.
    int captured = 0;
    for (int f = 0; f < frames; ++f) {
        if (vi_shim_shouldClose()) break;
        // Draw through the game's GX state, then present + pace exactly as the game
        // does: VIWaitForRetrace() flips the frame (firing the game's retrace
        // callbacks), waitNextFrame() is the game's own frame-timing step.
        // game_boot_draw_test submits geometry in the VTXFMT0 format videoInit
        // configured in gx_draw.
        game_boot_draw_test(f);
        VIWaitForRetrace();
        waitNextFrame();
        if (capturePath && !captured && f >= 10) {
            if (vi_host_window()) plat_window_capture_bmp(vi_host_window(), capturePath);
            printf("[boot] captured frame %d -> %s\n", f, capturePath);
            captured = 1; frames = f + 3;
        }
    }
    printf("[boot] done\n");
    return 0;
}
