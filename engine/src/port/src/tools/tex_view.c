// tex_view.c - decode and display a REAL Star Fox Adventures texture from the ISO.
//
// End-to-end integration of the lower stack: mount the ISO (dvd_shim), read a
// level's TEX1.tab/TEX1.bin, parse the SFA texture archive (format derived from the
// decomp: .tab = big-endian u32 per id, offset=(e&0xffffff)<<1, mips=(e>>24)&0x3f;
// the .bin record is the game's Texture layout - width@0xA, height@0xC, format@0x16,
// imageOffset@0x50, image at record+0x60+imageOffset), decode via tex_decode, and
// draw it on a quad through the GX shim -> RHI.
//
//   tex_view --iso <path> [--dir arwing] [--index N] [--gfx d3d11|d3d12|vk] [--capture out.bmp]

#include "port/gx_shim.h"
#include "port/tex_decode.h"
#include "port/dvd_shim.h"
#include "port/stfx_inflate.h"
#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#endif

static unsigned be16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int bppNum(int fmt) { // bits per texel * 8 (numerator over 8) for size estimate
    switch (fmt) {
        case GX_TF_I4: case GX_TF_CMPR: return 4;
        case GX_TF_I8: case GX_TF_IA4:  return 8;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: return 16;
        case GX_TF_RGBA8: return 32;
        default: return 0; // unknown/unsupported
    }
}
static const char* fmtName(int f) {
    switch (f) { case 0:return "I4"; case 1:return "I8"; case 2:return "IA4"; case 3:return "IA8";
        case 4:return "RGB565"; case 5:return "RGB5A3"; case 6:return "RGBA8"; case 14:return "CMPR"; default:return "?"; }
}

typedef struct { int id; unsigned off; int w, h, fmt, imgOff; } TexEntry;

// Decompress the ZLB block at bin+off into out. Returns decompressed size, or 0.
static size_t decompressZLB(const unsigned char* bin, unsigned off, int binSize,
                            unsigned char* out, size_t outCap) {
    if (off + 0x10 > (unsigned)binSize) return 0;
    const unsigned char* r = bin + off;
    if (memcmp(r, "ZLB", 3) != 0) return 0;
    unsigned usize = be32(r + 8), csize = be32(r + 0xc);
    if (usize == 0 || usize > outCap) return 0;
    if (off + 0x10 + csize > (unsigned)binSize) return 0;
    size_t got = 0;
    if (stfx_inflate_zlib(out, outCap, r + 0x10, csize, &got) != 0) return 0;
    return (got == usize) ? got : 0;
}

static RhiBackend parse_backend(const char* s) {
    if (!s) return RHI_BACKEND_D3D11;
    if (!strcmp(s,"vk")||!strcmp(s,"vulkan")) return RHI_BACKEND_VULKAN;
    if (!strcmp(s,"d3d12")) return RHI_BACKEND_D3D12;
    return RHI_BACKEND_D3D11;
}

int main(int argc, char** argv) {
    const char* iso = NULL; const char* dir = "arwing"; const char* gfx = "d3d11";
    const char* capturePath = NULL; int wantIndex = -1;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--iso") && i+1 < argc)     iso = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i+1 < argc)     dir = argv[++i];
        else if (!strcmp(argv[i], "--index") && i+1 < argc)   wantIndex = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gfx") && i+1 < argc)     gfx = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i+1 < argc) capturePath = argv[++i];
    }
    if (!iso) { fprintf(stderr, "usage: tex_view --iso <path> [--dir arwing] [--index N]\n"); return 2; }

    dvd_shim_init(iso);
    char tabPath[256], binPath[256];
    snprintf(tabPath, sizeof(tabPath), "%s/TEX1.tab", dir);
    snprintf(binPath, sizeof(binPath), "%s/TEX1.bin", dir);
    int tabSize = 0, binSize = 0;
    unsigned char* tab = (unsigned char*)loadFileByPath(tabPath, &tabSize, 0);
    unsigned char* bin = (unsigned char*)loadFileByPath(binPath, &binSize, 0);
    if (!tab || !bin) { fprintf(stderr, "[texview] could not read %s / %s\n", tabPath, binPath); return 3; }
    printf("[texview] %s: tab=%d bytes (%d entries), bin=%d bytes\n", dir, tabSize, tabSize/4, binSize);

    // Scan the tab: each distinct block is a ZLB-compressed Texture record.
    static unsigned char scratch[8 * 1024 * 1024];
    int nEntries = tabSize / 4;
    TexEntry* valid = (TexEntry*)malloc(sizeof(TexEntry) * nEntries);
    int nValid = 0;
    unsigned lastOff = 0xffffffff;
    for (int id = 0; id < nEntries; ++id) {
        unsigned e = be32(tab + id * 4);
        int mips = (e >> 24) & 0x3f;
        unsigned off = (e & 0xffffff) << 1;
        if (mips < 1 || off == lastOff) continue;
        lastOff = off;
        size_t dsz = decompressZLB(bin, off, binSize, scratch, sizeof scratch);
        if (dsz < 0x60) continue;
        int w = (int)be16(scratch + 0xA), h = (int)be16(scratch + 0xC);
        int fmt = scratch[0x16];
        int imgOff = (int)be32(scratch + 0x50);
        int bn = bppNum(fmt);
        if (bn == 0 || w < 4 || h < 4 || w > 1024 || h > 1024) continue;
        unsigned imgStart = 0x60 + (unsigned)imgOff;
        unsigned imgBytes = (unsigned)w * h * bn / 8;
        if (imgStart + imgBytes > dsz) continue;
        valid[nValid].id = id; valid[nValid].off = off; valid[nValid].w = w; valid[nValid].h = h;
        valid[nValid].fmt = fmt; valid[nValid].imgOff = imgOff;
        ++nValid;
    }
    printf("[texview] %d decodable textures. First few:\n", nValid);
    for (int i = 0; i < nValid && i < 16; ++i)
        printf("   [%2d] id=%d  %dx%d  %s\n", i, valid[i].id, valid[i].w, valid[i].h, fmtName(valid[i].fmt));
    if (nValid == 0) { fprintf(stderr, "[texview] no decodable textures found\n"); return 4; }

    // Pick: explicit index, else the largest-area texture (a real image, not a glyph).
    int pick = 0;
    if (wantIndex >= 0 && wantIndex < nValid) pick = wantIndex;
    else { long best = 0; for (int i = 0; i < nValid; ++i) { long a=(long)valid[i].w*valid[i].h; if(a>best){best=a;pick=i;} } }
    TexEntry T = valid[pick];
    printf("[texview] showing [%d] id=%d %dx%d %s\n", pick, T.id, T.w, T.h, fmtName(T.fmt));

    // Window + RHI.
    const int W = 1280, H = 720;
    char title[160];
    snprintf(title, sizeof(title), "Stairfax Temperatures - %s TEX1 id=%d %dx%d %s [%s]",
             dir, T.id, T.w, T.h, fmtName(T.fmt), gfx);
    PlatWindow* win = plat_window_create(title, W, H);
    RhiCreateInfo ci = {0};
    ci.backend = parse_backend(gfx); ci.windowHandle = plat_window_native_handle(win);
    ci.width = W; ci.height = H; ci.vsync = true; ci.appName = "Stairfax Temperatures";
    RhiInstance* rhi = rhi_create(&ci);
    if (!rhi) { fprintf(stderr, "[texview] rhi_create failed\n"); return 5; }
    RhiSwapchain* sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);
    gx_shim_setRhi(rhi, sc);
    GXInit_host();

    // Decompress the chosen texture's ZLB record into a kept buffer.
    unsigned char* recBuf = (unsigned char*)malloc(sizeof(scratch));
    size_t recSz = decompressZLB(bin, T.off, binSize, recBuf, sizeof(scratch));
    if (recSz == 0) { fprintf(stderr, "[texview] decompress failed\n"); return 6; }
    unsigned char* image = recBuf + 0x60 + T.imgOff;

    GXTexObj tex;
    GXInitTexObj(&tex, image, (uint16_t)T.w, (uint16_t)T.h, T.fmt, 0, 0, 0);

    // Quad sized to the texture's aspect ratio (fits within +/-0.85 in clip space).
    float ar = (float)T.w / (float)T.h;
    float qx = 0.85f, qy = 0.85f;
    if (ar > 1.0f) qy = qx / ar; else qx = qy * ar;

    int captured = 0, frames = 240, captureFrame = 20;
    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) break;
        rhi_beginFrame(rhi);
        rhi_clear(rhi, 0.06f, 0.06f, 0.08f, 1.0f);
        GXLoadTexObj(&tex, GX_TEXMAP0);
        GXBegin(GX_QUADS, 0, 4);
        GXPosition3f32(-qx,  qy, 0.0f); GXColor1u32(0xFFFFFFFF); GXTexCoord2f32(0.0f, 0.0f);
        GXPosition3f32( qx,  qy, 0.0f); GXColor1u32(0xFFFFFFFF); GXTexCoord2f32(1.0f, 0.0f);
        GXPosition3f32( qx, -qy, 0.0f); GXColor1u32(0xFFFFFFFF); GXTexCoord2f32(1.0f, 1.0f);
        GXPosition3f32(-qx, -qy, 0.0f); GXColor1u32(0xFFFFFFFF); GXTexCoord2f32(0.0f, 1.0f);
        GXEnd();
        rhi_endFrame(rhi);
        rhi_present(rhi, sc);
        if (capturePath && !captured && f >= captureFrame) {
            sleep_ms(60);
            plat_window_capture_bmp(win, capturePath);
            printf("[texview] captured -> %s\n", capturePath);
            captured = 1; frames = f + 3;
        }
    }

    free(tab); free(bin); free(valid); free(recBuf);
    rhi_swapchainDestroy(rhi, sc); rhi_destroy(rhi); plat_window_destroy(win);
    dvd_shim_shutdown();
    return 0;
}
