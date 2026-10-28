// rhi_smoketest.c - milestone harness: open a window, clear it to a color, present.
//
// Backend picked with --gfx d3d11|d3d12|vk|gl|auto. Proves the swappable RHI end to
// end: rhi_create -> swapchain -> beginFrame/clear/endFrame -> present. Optional
// --capture writes a BMP of the window for headless verification.
//
//   rhi_smoketest --gfx d3d12 --frames 240
//   rhi_smoketest --gfx d3d11 --capture out.bmp

#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms) { struct timespec t = { ms/1000, (ms%1000)*1000000L }; nanosleep(&t, NULL); }
#endif

static RhiBackend parse_backend(const char* s) {
    if (!s)                      return RHI_BACKEND_AUTO;
    if (!strcmp(s, "vk") ||
        !strcmp(s, "vulkan"))    return RHI_BACKEND_VULKAN;
    if (!strcmp(s, "d3d12") ||
        !strcmp(s, "dx12"))      return RHI_BACKEND_D3D12;
    if (!strcmp(s, "d3d11") ||
        !strcmp(s, "dx11"))      return RHI_BACKEND_D3D11;
    if (!strcmp(s, "gl"))        return RHI_BACKEND_GL;
    return RHI_BACKEND_AUTO;
}

int main(int argc, char** argv) {
    const char* gfx = "auto";
    const char* capturePath = NULL;
    int frames = 240;
    int captureFrame = 45;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--gfx") && i + 1 < argc)          gfx = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)  frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capturePath = argv[++i];
    }

    RhiBackend want = parse_backend(gfx);
    const int W = 1280, H = 720;

    char title[128];
    snprintf(title, sizeof(title), "Stairfax Temperatures (PC Port) - RHI smoketest [%s]", gfx);

    PlatWindow* win = plat_window_create(title, W, H);
    if (!win) { fprintf(stderr, "[smoketest] failed to create window\n"); return 2; }

    RhiCreateInfo ci = {0};
    ci.backend      = want;
    ci.windowHandle = plat_window_native_handle(win);
    ci.width        = W;
    ci.height       = H;
    ci.vsync        = true;
    ci.debug        = false;
    ci.appName      = "Stairfax Temperatures";

    RhiInstance* rhi = rhi_create(&ci);
    if (!rhi) {
        fprintf(stderr, "[smoketest] rhi_create failed for backend '%s'\n", gfx);
        plat_window_destroy(win);
        return 3;
    }
    RhiBackend got = rhi_getBackend(rhi);
    printf("[smoketest] backend=%s%s window=%dx%d\n",
           rhi_backendName(got), rhi_isBestEffort(got) ? " (best-effort)" : "", W, H);

    // If the backend didn't auto-create a swapchain in rhi_create, make one.
    RhiSwapchain* sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);

    int captured = 0;
    int rc = 0;
    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) break;

        // Distinctive clear color with a slow pulse so the loop is visibly live.
        float t = (float)f / 60.0f;
        float r = 0.15f + 0.10f * (float)sin(t * 1.7f);
        float g = 0.45f + 0.15f * (float)sin(t * 1.1f + 1.0f);
        float b = 0.80f + 0.15f * (float)sin(t * 0.9f + 2.0f);

        rhi_beginFrame(rhi);
        rhi_clear(rhi, r, g, b, 1.0f);
        rhi_endFrame(rhi);
        if (!rhi_present(rhi, sc)) { fprintf(stderr, "[smoketest] present failed\n"); rc = 4; break; }

        if (capturePath && !captured && f >= captureFrame) {
            sleep_ms(60); // let DWM compose the presented frame
            if (plat_window_capture_bmp(win, capturePath)) {
                printf("[smoketest] captured frame %d -> %s\n", f, capturePath);
            } else {
                fprintf(stderr, "[smoketest] capture failed\n");
            }
            captured = 1;
            frames = f + 3; // wrap up shortly after capturing
        }
    }

    rhi_swapchainDestroy(rhi, sc);
    rhi_destroy(rhi);
    plat_window_destroy(win);
    printf("[smoketest] done (%s)\n", rhi_backendName(got));
    return rc;
}
