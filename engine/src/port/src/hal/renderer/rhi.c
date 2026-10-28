// rhi.c - common RHI groundwork: backend selection + vtable dispatch.
//
// This file links into rhi_common and knows nothing about any GPU API. It picks a
// backend (via the per-backend `*_create` entry points, which live in their own
// libraries) and then forwards every public rhi_* call through instance->ops.

#include "port/renderer/rhi.h"
#include "port/renderer/rhi_internal.h"
#include "port/renderer/rhi_vulkan.h"
#include "port/renderer/rhi_d3d12.h"
#include "port/renderer/rhi_d3d11.h"
#include "port/renderer/rhi_gl.h"
#include <stddef.h>

const char* rhi_backendName(RhiBackend b) {
    switch (b) {
        case RHI_BACKEND_VULKAN: return "vulkan";
        case RHI_BACKEND_D3D12:  return "d3d12";
        case RHI_BACKEND_D3D11:  return "d3d11";
        case RHI_BACKEND_GL:     return "gl";
        default:                 return "auto";
    }
}

bool rhi_isBestEffort(RhiBackend b) {
    return b == RHI_BACKEND_D3D11 || b == RHI_BACKEND_GL;
}

RhiInstance* rhi_create(const RhiCreateInfo* info) {
    RhiBackend want = info->backend;
    if (want == RHI_BACKEND_AUTO) {
#ifdef _WIN32
        if      (rhi_vulkan_available()) want = RHI_BACKEND_VULKAN;
        else if (rhi_d3d12_available())  want = RHI_BACKEND_D3D12;
        else if (rhi_d3d11_available())  want = RHI_BACKEND_D3D11;
        else                             want = RHI_BACKEND_GL;
#else
        if (rhi_vulkan_available()) want = RHI_BACKEND_VULKAN;
        else                        want = RHI_BACKEND_GL;
#endif
    }

    RhiInstance* inst = NULL;
    switch (want) {
        case RHI_BACKEND_VULKAN: inst = rhi_vulkan_create(info); break;
        case RHI_BACKEND_D3D12:  inst = rhi_d3d12_create(info);  break;
        case RHI_BACKEND_D3D11:  inst = rhi_d3d11_create(info);  break;
        case RHI_BACKEND_GL:     inst = rhi_gl_create(info);     break;
        default: return NULL;
    }
    return inst; // backend set inst->ops and inst->backend, or returned NULL
}

RhiBackend rhi_getBackend(RhiInstance* r) {
    return r ? r->backend : RHI_BACKEND_AUTO;
}

void rhi_destroy(RhiInstance* r) {
    if (r && r->ops && r->ops->destroy) r->ops->destroy(r);
}

// --- Swapchain -------------------------------------------------------------
RhiSwapchain* rhi_swapchainCreate(RhiInstance* r, void* windowHandle, int w, int h, bool vsync) {
    return (r && r->ops && r->ops->swapchainCreate)
         ? r->ops->swapchainCreate(r, windowHandle, w, h, vsync) : NULL;
}
void rhi_swapchainDestroy(RhiInstance* r, RhiSwapchain* sc) {
    if (r && r->ops && r->ops->swapchainDestroy) r->ops->swapchainDestroy(r, sc);
}
void rhi_swapchainResize(RhiInstance* r, RhiSwapchain* sc, int w, int h) {
    if (r && r->ops && r->ops->swapchainResize) r->ops->swapchainResize(r, sc, w, h);
}
bool rhi_present(RhiInstance* r, RhiSwapchain* sc) {
    return (r && r->ops && r->ops->present) ? r->ops->present(r, sc) : false;
}

// --- Frame -----------------------------------------------------------------
void rhi_beginFrame(RhiInstance* r) {
    if (r && r->ops && r->ops->beginFrame) r->ops->beginFrame(r);
}
void rhi_endFrame(RhiInstance* r) {
    if (r && r->ops && r->ops->endFrame) r->ops->endFrame(r);
}
void rhi_clear(RhiInstance* r, float cr, float cg, float cb, float ca) {
    if (r && r->ops && r->ops->clear) r->ops->clear(r, cr, cg, cb, ca);
}
void rhi_drawColored(RhiInstance* r, const RhiColorVertex* verts, uint32_t count) {
    if (r && r->ops && r->ops->drawColored) r->ops->drawColored(r, verts, count);
}
void rhi_setColorTransform(RhiInstance* r, const float m[16]) {
    if (r && r->ops && r->ops->setColorTransform) r->ops->setColorTransform(r, m);
}
void rhi_drawTextured(RhiInstance* r, const RhiTexVertex* verts, uint32_t count) {
    if (r && r->ops && r->ops->drawTextured) r->ops->drawTextured(r, verts, count);
}
void rhi_setAlphaMode(RhiInstance* r, int mode) {
    if (r) r->alphaMode = mode;   // read by each backend's drawTextured
}

// --- Not yet implemented (this stage is window+clear only) -----------------
// Declared in rhi.h; routed through ops once the draw/pipeline path lands.
RhiTevKey    rhi_buildTevKey(void) { RhiTevKey k = {0}; return k; }
RhiPipeline* rhi_getOrCreatePipeline(RhiInstance* r, RhiTevKey key) { (void)r; (void)key; return NULL; }
RhiTexture*  rhi_createTexture(RhiInstance* r, int w, int h, int mip, uint32_t fmt, const void* d) {
    return (r && r->ops && r->ops->createTexture) ? r->ops->createTexture(r, w, h, mip, fmt, d) : NULL;
}
void         rhi_destroyTexture(RhiInstance* r, RhiTexture* t) {
    if (r && r->ops && r->ops->destroyTexture) r->ops->destroyTexture(r, t);
}
RhiBuffer*   rhi_createBuffer(RhiInstance* r, size_t s, const void* d, bool dyn) {
    (void)r; (void)s; (void)d; (void)dyn; return NULL;
}
void         rhi_destroyBuffer(RhiInstance* r, RhiBuffer* b) { (void)r; (void)b; }
void         rhi_setPipeline(RhiInstance* r, RhiPipeline* p) { (void)r; (void)p; }
void         rhi_setTexture(RhiInstance* r, int slot, RhiTexture* t) {
    if (r && r->ops && r->ops->setTexture) r->ops->setTexture(r, slot, t);
}
void         rhi_setVertexBuffer(RhiInstance* r, RhiBuffer* vb, size_t stride) { (void)r; (void)vb; (void)stride; }
void         rhi_setIndexBuffer(RhiInstance* r, RhiBuffer* ib) { (void)r; (void)ib; }
void         rhi_drawIndexed(RhiInstance* r, uint32_t n, uint32_t first) { (void)r; (void)n; (void)first; }
void         rhi_draw(RhiInstance* r, uint32_t n, uint32_t first) { (void)r; (void)n; (void)first; }
void         rhi_setViewport(RhiInstance* r, float x, float y, float w, float h) { (void)r; (void)x; (void)y; (void)w; (void)h; }
bool         rhi_supports(RhiInstance* r, const char* feature) { (void)r; (void)feature; return false; }
