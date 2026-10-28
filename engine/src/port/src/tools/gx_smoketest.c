// gx_smoketest.c - render a triangle through the GX shim -> RHI draw path.
//
// Proves GX immediate-mode calls (GXBegin/GXPosition/GXColor/GXEnd) become real
// RHI draws. Sets up a window + RHI + swapchain, hands them to gx_shim, then each
// frame clears and submits one RGB triangle via GX. --gfx picks the backend
// (default d3d11 - the first backend with the draw path). --capture writes a BMP.

#include "port/gx_shim.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#endif

// Left-handed perspective, clip z in [0,1] (works on D3D and Vulkan; a subset for
// GL). Row-major to match the RHI's dot(row, p) convention.
static void perspectiveLH(float m[4][4], float fovyRad, float aspect, float zn, float zf) {
    float ys = 1.0f / tanf(fovyRad * 0.5f);
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = ys / aspect;
    m[1][1] = ys;
    m[2][2] = zf / (zf - zn);
    m[2][3] = -zn * zf / (zf - zn);
    m[3][2] = 1.0f;
}
// Modelview 3x4: rotate about Y then translate +Z (left-handed).
static void modelviewYT(float m[3][4], float angle, float dist) {
    float c = cosf(angle), s = sinf(angle);
    m[0][0] = c;  m[0][1] = 0; m[0][2] = s; m[0][3] = 0;
    m[1][0] = 0;  m[1][1] = 1; m[1][2] = 0; m[1][3] = 0;
    m[2][0] = -s; m[2][1] = 0; m[2][2] = c; m[2][3] = dist;
}

static RhiBackend parse_backend(const char* s) {
    if (!s) return RHI_BACKEND_D3D11;
    if (!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if (!strcmp(s,"d3d12"))                   return RHI_BACKEND_D3D12;
    if (!strcmp(s,"d3d11"))                   return RHI_BACKEND_D3D11;
    if (!strcmp(s,"auto"))                    return RHI_BACKEND_AUTO;
    return RHI_BACKEND_D3D11;
}

int main(int argc, char** argv) {
    const char* gfx = "d3d11";
    const char* capturePath = NULL;
    int frames = 240, captureFrame = 30;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--gfx") && i+1 < argc)     gfx = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i+1 < argc) capturePath = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i+1 < argc)  frames = atoi(argv[++i]);
    }

    const int W = 1280, H = 720;
    char title[128];
    snprintf(title, sizeof(title), "Stairfax Temperatures - GX triangle [%s]", gfx);
    PlatWindow* win = plat_window_create(title, W, H);
    if (!win) { fprintf(stderr, "[gx] window failed\n"); return 2; }

    RhiCreateInfo ci = {0};
    ci.backend = parse_backend(gfx);
    ci.windowHandle = plat_window_native_handle(win);
    ci.width = W; ci.height = H; ci.vsync = true; ci.appName = "Stairfax Temperatures";
    RhiInstance* rhi = rhi_create(&ci);
    if (!rhi) { fprintf(stderr, "[gx] rhi_create failed\n"); return 3; }
    RhiSwapchain* sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);

    gx_shim_setRhi(rhi, sc);
    GXInit_host();
    GXSetViewport(0, 0, (float)W, (float)H, 0, 1);
    printf("[gx] backend=%s\n", rhi_backendName(rhi_getBackend(rhi)));

    int captured = 0;
    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) break;

        rhi_beginFrame(rhi);
        rhi_clear(rhi, 0.08f, 0.10f, 0.14f, 1.0f);

        // Set up the GX transform: perspective projection + a rotating modelview,
        // exactly as the game would (GXSetProjection / GXLoadPosMtxImm).
        float proj[4][4], mv[3][4];
        perspectiveLH(proj, 60.0f * 3.14159265f / 180.0f, (float)W / (float)H, 0.1f, 100.0f);
        modelviewYT(mv, (float)f * 0.02f, 3.0f);
        GXSetProjection(proj, GX_PERSPECTIVE);
        GXLoadPosMtxImm(mv, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        // One RGB triangle in MODEL space; the MVP transforms it to the screen.
        GXBegin(GX_TRIANGLES, 0, 3);
        GXPosition3f32( 0.0f,  0.8f, 0.0f); GXColor1u32(0xFF0000FF); // red   apex
        GXPosition3f32( 0.8f, -0.7f, 0.0f); GXColor1u32(0x00FF00FF); // green
        GXPosition3f32(-0.8f, -0.7f, 0.0f); GXColor1u32(0x0000FFFF); // blue
        GXEnd();

        rhi_endFrame(rhi);
        rhi_present(rhi, sc);

        if (capturePath && !captured && f >= captureFrame) {
            sleep_ms(60);
            if (plat_window_capture_bmp(win, capturePath))
                printf("[gx] captured frame %d -> %s\n", f, capturePath);
            captured = 1;
            frames = f + 3;
        }
    }

    rhi_swapchainDestroy(rhi, sc);
    rhi_destroy(rhi);
    plat_window_destroy(win);
    printf("[gx] done\n");
    return 0;
}
