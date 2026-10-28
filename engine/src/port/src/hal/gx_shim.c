// gx_shim.c - GX immediate-mode -> RHI draws (colored + textured).
//
// GXBegin starts a batch; GXPosition/GXColor/GXTexCoord fill the current vertex in
// any order, committed when the next GXPosition begins or at GXEnd. GXEnd expands
// the primitive to a triangle list, applies MVP = proj*posmtx, and draws: textured
// (sampling the bound GXTexObj) if a texture has been loaded, else colored.

#include "port/gx_shim.h"
#include "port/renderer/rhi.h"
#include "port/tex_decode.h"

#include <stdlib.h>
#include <string.h>

static struct RhiInstance*  gRhi;
static struct RhiSwapchain* gSc;

#define GX_MAX_BATCH 8192
static RhiTexVertex gVerts[GX_MAX_BATCH]; // unified store (x,y,z,rgba,u,v)
static int          gCount;
static uint8_t      gPrim;
static RhiTexVertex gCur;
static bool         gHaveVertex;
static bool         gTexBound;

// --- transform state -------------------------------------------------------
#define GX_MTX_SLOTS 64
static float gProj[4][4];
static float gPosMtx[GX_MTX_SLOTS][3][4];
static int   gCurMtx;

static void mtxIdentity3x4(float m[3][4]) {
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}
static void mtxIdentity4x4(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

// MVP = proj(4x4) * posMtx(3x4 extended with [0,0,0,1]); row-major output.
static void computeMVP(float out[16]) {
    float P[4][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) P[i][j] = gPosMtx[gCurMtx][i][j];
    P[3][0] = P[3][1] = P[3][2] = 0.0f; P[3][3] = 1.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += gProj[i][k] * P[k][j];
            out[i * 4 + j] = s;
        }
}

void gx_shim_setRhi(struct RhiInstance* rhi, struct RhiSwapchain* sc) { gRhi = rhi; gSc = sc; }
struct RhiInstance* gx_shim_getRhi(void) { return gRhi; }

void GXInit_host(void) {
    gCount = 0; gHaveVertex = false; gTexBound = false; gCurMtx = 0;
    mtxIdentity4x4(gProj);
    for (int i = 0; i < GX_MTX_SLOTS; ++i) mtxIdentity3x4(gPosMtx[i]);
}
void GXSetViewport(float x, float y, float w, float h, float n, float f) { (void)x;(void)y;(void)w;(void)h;(void)n;(void)f; }
void GXSetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)x;(void)y;(void)w;(void)h; }
void GXSetCullMode(int mode) { (void)mode; }

void GXLoadPosMtxImm(float mtx[3][4], uint32_t id) {
    if (id >= GX_MTX_SLOTS) return;
    memcpy(gPosMtx[id], mtx, sizeof(float) * 12);
}
void GXSetCurrentMtx(uint32_t id) { if (id < GX_MTX_SLOTS) gCurMtx = (int)id; }
void GXSetProjection(float proj[4][4], int type) { (void)type; memcpy(gProj, proj, sizeof(float) * 16); }

// --- textures --------------------------------------------------------------
void GXInitTexObj(GXTexObj* obj, void* data, uint16_t w, uint16_t h, int fmt,
                  int wrapS, int wrapT, int mipmap) {
    (void)wrapS; (void)wrapT; (void)mipmap;
    if (!obj) return;
    obj->data = data; obj->width = w; obj->height = h; obj->fmt = fmt; obj->rhiTex = NULL;
}

void GXLoadTexObj(GXTexObj* obj, int mapId) {
    (void)mapId;
    if (!obj || !gRhi) return;
    if (!obj->rhiTex) { // decode GX -> RGBA8 and upload once, then cache
        uint8_t* rgba = gxTexDecode(obj->fmt, obj->width, obj->height, (const uint8_t*)obj->data);
        if (!rgba) return;
        obj->rhiTex = rhi_createTexture(gRhi, obj->width, obj->height, 1, (uint32_t)obj->fmt, rgba);
        free(rgba);
    }
    rhi_setTexture(gRhi, 0, (RhiTexture*)obj->rhiTex);
    gTexBound = (obj->rhiTex != NULL);
}

// --- immediate mode --------------------------------------------------------
static void commitVertex(void) {
    if (!gHaveVertex || gCount >= GX_MAX_BATCH) { gHaveVertex = false; return; }
    gVerts[gCount++] = gCur;
    gHaveVertex = false;
}

void GXBegin(uint8_t primitive, uint8_t vtxfmt, uint16_t nverts) {
    (void)vtxfmt; (void)nverts;
    gPrim = primitive;
    gCount = 0;
    gHaveVertex = false;
}

void GXPosition3f32(float x, float y, float z) {
    commitVertex();                 // finalize the previous vertex
    gCur.x = x; gCur.y = y; gCur.z = z;
    gCur.rgba = 0xFFFFFFFFu;         // defaults until GXColor/GXTexCoord set them
    gCur.u = 0.0f; gCur.v = 0.0f;
    gHaveVertex = true;
}
void GXPosition2f32(float x, float y) { GXPosition3f32(x, y, 0.0f); }

void GXColor1u32(uint32_t c) {
    uint8_t r = (uint8_t)(c >> 24), g = (uint8_t)(c >> 16), b = (uint8_t)(c >> 8), a = (uint8_t)c;
    gCur.rgba = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
void GXColor4u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    gCur.rgba = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
void GXTexCoord2f32(float s, float t) { gCur.u = s; gCur.v = t; }

// Expand the primitive into a triangle list (unified verts), then draw.
static int expandTriangles(RhiTexVertex* out, int cap) {
    int n = 0;
    if (gPrim == GX_TRIANGLES) {
        for (int i = 0; i + 3 <= gCount && n + 3 <= cap; i += 3) {
            out[n++] = gVerts[i]; out[n++] = gVerts[i+1]; out[n++] = gVerts[i+2];
        }
    } else if (gPrim == GX_TRIANGLESTRIP) {
        for (int i = 2; i < gCount && n + 3 <= cap; ++i) {
            if (i & 1) { out[n++] = gVerts[i-1]; out[n++] = gVerts[i-2]; out[n++] = gVerts[i]; }
            else       { out[n++] = gVerts[i-2]; out[n++] = gVerts[i-1]; out[n++] = gVerts[i]; }
        }
    } else if (gPrim == GX_TRIANGLEFAN) {
        for (int i = 2; i < gCount && n + 3 <= cap; ++i) {
            out[n++] = gVerts[0]; out[n++] = gVerts[i-1]; out[n++] = gVerts[i];
        }
    } else if (gPrim == GX_QUADS) {
        for (int i = 0; i + 4 <= gCount && n + 6 <= cap; i += 4) {
            out[n++] = gVerts[i];   out[n++] = gVerts[i+1]; out[n++] = gVerts[i+2];
            out[n++] = gVerts[i];   out[n++] = gVerts[i+2]; out[n++] = gVerts[i+3];
        }
    }
    return n;
}

static void flushBatch(void) {
    if (!gRhi || gCount == 0) return;

    float mvp[16];
    computeMVP(mvp);
    rhi_setColorTransform(gRhi, mvp);

    static RhiTexVertex tri[GX_MAX_BATCH * 3];
    int n = expandTriangles(tri, (int)(sizeof(tri) / sizeof(tri[0])));
    if (n == 0) return;

    if (gTexBound) {
        rhi_drawTextured(gRhi, tri, (uint32_t)n);
    } else {
        static RhiColorVertex col[GX_MAX_BATCH * 3];
        for (int i = 0; i < n; ++i) {
            col[i].x = tri[i].x; col[i].y = tri[i].y; col[i].z = tri[i].z; col[i].rgba = tri[i].rgba;
        }
        rhi_drawColored(gRhi, col, (uint32_t)n);
    }
}

void GXEnd(void) {
    commitVertex();
    flushBatch();
    gCount = 0;
    gHaveVertex = false;
}
