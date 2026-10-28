// lightmap_smoketest.cpp - render real game geometry through a REAL game function.
//
// This calls lightmapDrawTriangleList() compiled straight from src/main/lightmap_draw.c
// (as C++, so its GXWGFifo.* stores go through the write-gather-pipe proxy). We hand it
// a LightmapVertex array + triangle list in the game's own on-disc layout; the function
// emits the primitive to the FIFO exactly as it does in-game, gx_draw decodes it against
// VTXFMT0, and the RHI draws it. Nothing about the draw is reimplemented - only the
// vertex data is synthesized (a real map would supply it from the ISO).

#include "port/gx_draw.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"
#include "main/lightmap_api.h"   // LightmapVertex + lightmapDrawTriangleList decl

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <windows.h>

// lightmapDrawTriangleList is declared in lightmap_api.h and defined in
// src/main/lightmap_draw.c, compiled into this target as C++.

static RhiBackend parseBackend(const char* s) {
    if (!s) return RHI_BACKEND_D3D11;
    if (!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if (!strcmp(s,"d3d12")) return RHI_BACKEND_D3D12;
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
    char title[128]; snprintf(title, sizeof title, "Stairfax Temperatures - lightmap_draw.c [%s]", gfx);
    PlatWindow* win = plat_window_create(title, W, H);
    if (!win) return 2;
    RhiCreateInfo ci = {};
    ci.backend = parseBackend(gfx); ci.windowHandle = plat_window_native_handle(win);
    ci.width = W; ci.height = H; ci.vsync = true; ci.appName = "Stairfax Temperatures";
    RhiInstance* rhi = rhi_create(&ci);
    if (!rhi) return 3;
    RhiSwapchain* sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);

    gx_draw_init();
    gx_draw_setRhi(rhi, sc);
    // VTXFMT0 attribute formats, as videoInit programs them (lightmapDrawTriangleList
    // sets only the vertex DESCRIPTOR, relying on these having been set).
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);

    // A cube's worth of LightmapVertex data (game on-disc layout: s16 x,y,z,pad;
    // s16 s,t; u8 r,g,b,a). 8 corners, 12 triangles.
    const short P = 140;
    LightmapVertex v[8];
    std::memset(v, 0, sizeof v);
    const short px[8] = {-P,+P,+P,-P,-P,+P,+P,-P};
    const short py[8] = {-P,-P,+P,+P,-P,-P,+P,+P};
    const short pz[8] = {-P,-P,-P,-P,+P,+P,+P,+P};
    const unsigned char cr[8]={255, 60, 60,255,255,255, 60, 60};
    const unsigned char cg[8]={ 60,255, 60,255, 60,255,255, 60};
    const unsigned char cb[8]={ 60, 60,255, 60,255,255,255,255};
    for (int i = 0; i < 8; ++i) {
        v[i].x = px[i]; v[i].y = py[i]; v[i].z = pz[i];
        v[i].s = 0; v[i].t = 0;
        v[i].r = cr[i]; v[i].g = cg[i]; v[i].b = cb[i]; v[i].a = 255;
    }
    // Triangle list: lightmapDrawTriangleList reads 3 vertex indices at list[1..3] per
    // 0x10-byte entry (list[0] unused by the emit loop).
    static const unsigned char idx[12][3] = {
        {0,1,2},{0,2,3}, {4,6,5},{4,7,6}, {0,5,1},{0,4,5},
        {1,6,2},{1,5,6}, {2,7,3},{2,6,7}, {3,4,0},{3,7,4},
    };
    unsigned char triList[12 * 0x10];
    std::memset(triList, 0, sizeof triList);
    for (int t = 0; t < 12; ++t) {
        triList[t*0x10 + 1] = idx[t][0];
        triList[t*0x10 + 2] = idx[t][1];
        triList[t*0x10 + 3] = idx[t][2];
    }

    printf("[lm] backend=%s\n", rhi_backendName(rhi_getBackend(rhi)));

    int captured = 0;
    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) break;
        rhi_beginFrame(rhi);
        rhi_clear(rhi, 0.08f, 0.10f, 0.14f, 1.0f);

        // Perspective + spin, then the REAL game draw function.
        float proj[4][4]; std::memset(proj, 0, sizeof proj);
        float ys = 1.0f/std::tan((55.0f*3.14159265f/180.0f)*0.5f);
        proj[0][0]=ys/((float)W/H); proj[1][1]=ys; proj[2][2]=1000.0f/(1000.0f-0.1f);
        proj[2][3]=-0.1f*1000.0f/(1000.0f-0.1f); proj[3][2]=1.0f;
        float a=(float)f*0.02f, c=std::cos(a), s=std::sin(a);
        float mv[3][4]={{c,0,s,0},{ s*0.5f,0.87f,-c*0.5f,0},{ -s*0.87f,0.5f,c*0.87f,520.0f}};
        GXSetProjection(proj, GX_PERSPECTIVE);
        GXLoadPosMtxImm(mv, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        lightmapDrawTriangleList(v, triList, 12);

        rhi_endFrame(rhi);
        rhi_present(rhi, sc);
        if (capturePath && !captured && f >= captureFrame) {
            Sleep(60);
            if (plat_window_capture_bmp(win, capturePath))
                printf("[lm] captured frame %d -> %s\n", f, capturePath);
            captured = 1; frames = f + 3;
        }
    }
    rhi_swapchainDestroy(rhi, sc); rhi_destroy(rhi); plat_window_destroy(win);
    printf("[lm] done\n");
    return 0;
}
