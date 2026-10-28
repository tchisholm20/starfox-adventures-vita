#pragma once
// gx_shim.h - GX -> RHI translation (first brick: immediate-mode colored draws).
//
// GX is the GameCube's fixed-function graphics API. The full translation (TEV
// ubershaders, textures, matrices, the WGPIPE vertex stream) is large; this first
// slice implements immediate-mode geometry: GXBegin/GXPosition3f32/GXColor.../GXEnd
// accumulate into an RHI colored-vertex batch that is drawn on GXEnd. Enough to
// prove the GX->RHI draw path end to end. State setters are accepted and mostly
// recorded/ignored for now.
//
// This is the port's own immediate-mode subset, not yet the full dolphin/gx.h
// shadow (that arrives when real GX-using TUs are compiled).

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// GX primitive types (subset; values match dolphin/gx).
enum {
    GX_POINTS        = 0xB8,
    GX_LINES         = 0xA8,
    GX_LINESTRIP     = 0xB0,
    GX_TRIANGLES     = 0x90,
    GX_TRIANGLESTRIP = 0x98,
    GX_TRIANGLEFAN   = 0xA0,
    GX_QUADS         = 0x80,
};

struct RhiInstance;
struct RhiSwapchain;

// Plumbing: give GX the RHI it renders into (VI owns these).
void gx_shim_setRhi(struct RhiInstance* rhi, struct RhiSwapchain* sc);
struct RhiInstance* gx_shim_getRhi(void);

// Projection type + position-matrix slot ids (subset of dolphin/gx).
enum { GX_PERSPECTIVE = 0, GX_ORTHOGRAPHIC = 1 };
enum { GX_PNMTX0 = 0, GX_PNMTX1 = 3, GX_PNMTX2 = 6, GX_PNMTX3 = 9 };

// Minimal lifecycle.
void GXInit_host(void);           // host init (real GXInit returns a GXFifoObj*)
void GXSetViewport(float x, float y, float w, float h, float nearz, float farz);
void GXSetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void GXSetCullMode(int mode);

// Transform: position (modelview) matrix memory + projection. The combined
// proj*posmtx is uploaded to the RHI as the MVP before each draw.
void GXLoadPosMtxImm(float mtx[3][4], uint32_t id);  // load 3x4 into matrix slot
void GXSetCurrentMtx(uint32_t id);                    // select current pos matrix
void GXSetProjection(float proj[4][4], int type);     // 4x4 projection

// Textures. GXTexObj describes a GX-format texture; GXLoadTexObj decodes it to
// RGBA8, uploads it to the RHI (cached in the obj), and binds it for drawing.
typedef struct GXTexObj {
    void*    data;
    uint16_t width, height;
    int      fmt;      // GX_TF_* (see tex_decode.h)
    void*    rhiTex;   // cached RhiTexture* after first load
} GXTexObj;
enum { GX_TEXMAP0 = 0, GX_TEXMAP1 = 1, GX_TEXMAP2 = 2, GX_TEXMAP3 = 3 };

void GXInitTexObj(GXTexObj* obj, void* data, uint16_t w, uint16_t h, int fmt,
                  int wrapS, int wrapT, int mipmap);
void GXLoadTexObj(GXTexObj* obj, int mapId);

// Immediate mode.
void GXBegin(uint8_t primitive, uint8_t vtxfmt, uint16_t nverts);
void GXEnd(void);
void GXPosition3f32(float x, float y, float z);
void GXPosition2f32(float x, float y);
void GXColor1u32(uint32_t rgba);          // 0xRRGGBBAA
void GXColor4u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void GXTexCoord2f32(float s, float t);

#ifdef __cplusplus
}
#endif
