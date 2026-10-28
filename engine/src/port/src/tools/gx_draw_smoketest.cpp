// gx_draw_smoketest.cpp - drive the REAL GX draw path exactly as a game TU does.
//
// This mirrors the idiom in src/main/lightmap_draw.c: set up the VTXFMT0 vertex
// format (as videoInit does), then GXBegin + write vertices straight to GXWGFifo +
// GXEnd. Nothing here calls a custom helper - it's the same source shape the game
// compiles, so a green triangle on screen proves the WGPIPE interpreter + VAT decode
// + transform + RHI submit all work end to end. --gfx picks the backend.

#include "port/gx_draw.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"
#include "main/dll/ppcwgpipe_struct.h" // declares extern "C" volatile PPCWGPipe GXWGFifo

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <windows.h>

// The game's own per-vertex writers (see lightmap_draw.c), verbatim in spirit:
// each stores to the write-gather pipe, which our proxy forwards to the FIFO.
static inline void wgPosition3s16(s16 x, s16 y, s16 z) { GXWGFifo.s16 = x; GXWGFifo.s16 = y; GXWGFifo.s16 = z; }
static inline void wgPosition1x8(u8 i)                 { GXWGFifo.u8  = i; }
static inline void wgColor4u8(u8 r, u8 g, u8 b, u8 a)  { GXWGFifo.u8 = r; GXWGFifo.u8 = g; GXWGFifo.u8 = b; GXWGFifo.u8 = a; }
static inline void wgTexCoord2s16(s16 s, s16 t)        { GXWGFifo.s16 = s; GXWGFifo.s16 = t; }

static void perspectiveLH(float m[4][4], float fovy, float aspect, float zn, float zf) {
    float ys = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, sizeof(float)*16);
    m[0][0] = ys/aspect; m[1][1] = ys; m[2][2] = zf/(zf-zn); m[2][3] = -zn*zf/(zf-zn); m[3][2] = 1.0f;
}
static void modelviewYT(float m[3][4], float a, float d) {
    float c = cosf(a), s = sinf(a);
    m[0][0]=c; m[0][1]=0; m[0][2]=s; m[0][3]=0;
    m[1][0]=0; m[1][1]=1; m[1][2]=0; m[1][3]=0;
    m[2][0]=-s; m[2][1]=0; m[2][2]=c; m[2][3]=d;
}

static RhiBackend parseBackend(const char* s) {
    if (!s) return RHI_BACKEND_D3D11;
    if (!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if (!strcmp(s,"d3d12")) return RHI_BACKEND_D3D12;
    if (!strcmp(s,"d3d11")) return RHI_BACKEND_D3D11;
    return RHI_BACKEND_D3D11;
}

int main(int argc, char** argv) {
    const char* gfx = "d3d11"; const char* capturePath = nullptr;
    int frames = 240, captureFrame = 30;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--gfx") && i+1<argc) gfx = argv[++i];
        else if (!strcmp(argv[i],"--capture") && i+1<argc) capturePath = argv[++i];
        else if (!strcmp(argv[i],"--frames") && i+1<argc) frames = atoi(argv[++i]);
    }

    const int W = 1280, H = 720;
    char title[128]; snprintf(title, sizeof title, "Stairfax Temperatures - GX FIFO draw [%s]", gfx);
    PlatWindow* win = plat_window_create(title, W, H);
    if (!win) { fprintf(stderr, "[gxd] window failed\n"); return 2; }

    RhiCreateInfo ci = {};
    ci.backend = parseBackend(gfx);
    ci.windowHandle = plat_window_native_handle(win);
    ci.width = W; ci.height = H; ci.vsync = true; ci.appName = "Stairfax Temperatures";
    RhiInstance* rhi = rhi_create(&ci);
    if (!rhi) { fprintf(stderr, "[gxd] rhi_create failed\n"); return 3; }
    RhiSwapchain* sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);

    gx_draw_init();
    gx_draw_setRhi(rhi, sc);

    // Exactly the vertex format videoInit() programs for GX_VTXFMT0.
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);

    printf("[gxd] backend=%s\n", rhi_backendName(rhi_getBackend(rhi)));

    int captured = 0;
    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) break;

        rhi_beginFrame(rhi);
        rhi_clear(rhi, 0.08f, 0.10f, 0.14f, 1.0f);

        float proj[4][4], mv[3][4];
        perspectiveLH(proj, 60.0f*3.14159265f/180.0f, (float)W/(float)H, 0.1f, 1000.0f);
        modelviewYT(mv, (float)f*0.02f, 400.0f);
        GXSetProjection(proj, GX_PERSPECTIVE);
        GXLoadPosMtxImm(mv, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        // One triangle, S16 model-space coords (like terrain/HUD geometry). The
        // PNMTXIDX byte + POS + CLR0 stream is decoded straight from the FIFO.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        wgPosition1x8(0); wgPosition3s16(   0,  120, 0); wgColor4u8(255,  40,  40, 255);
        wgPosition1x8(0); wgPosition3s16( 120, -100, 0); wgColor4u8( 40, 255,  40, 255);
        wgPosition1x8(0); wgPosition3s16(-120, -100, 0); wgColor4u8( 40,  40, 255, 255);
        GXEnd();

        rhi_endFrame(rhi);
        rhi_present(rhi, sc);

        if (capturePath && !captured && f >= captureFrame) {
            Sleep(60);
            if (plat_window_capture_bmp(win, capturePath))
                printf("[gxd] captured frame %d -> %s\n", f, capturePath);
            captured = 1; frames = f + 3;
        }
    }

    rhi_swapchainDestroy(rhi, sc);
    rhi_destroy(rhi);
    plat_window_destroy(win);
    printf("[gxd] done\n");
    return 0;
}
