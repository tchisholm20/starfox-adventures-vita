#pragma once
// rhi_internal.h - shared contract between rhi_common and the backend libraries.
//
// Game code and shims only ever see rhi.h (opaque handles). The backends see this
// header, which completes `struct RhiInstance` as a base object carrying a vtable
// (RhiOps). Each backend defines its own instance struct with `RhiInstance base;`
// as the first member, sets base.ops / base.backend, and returns it upcast to
// RhiInstance*. rhi_common dispatches every public call through base.ops.
//
// This is the seam that makes renderers swappable: selecting a backend just picks
// which ops table the instance carries. No #ifdef in the call path.

#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RhiOps {
    // Lifetime
    void (*destroy)(RhiInstance*);

    // Swapchain (owned by VI)
    RhiSwapchain* (*swapchainCreate)(RhiInstance*, void* windowHandle, int w, int h, bool vsync);
    void          (*swapchainDestroy)(RhiInstance*, RhiSwapchain*);
    void          (*swapchainResize)(RhiInstance*, RhiSwapchain*, int w, int h);
    bool          (*present)(RhiInstance*, RhiSwapchain*);

    // Frame
    void (*beginFrame)(RhiInstance*);
    void (*endFrame)(RhiInstance*);
    void (*clear)(RhiInstance*, float r, float g, float b, float a);

    // Draw (first GX brick: immediate-mode colored triangles)
    void (*drawColored)(RhiInstance*, const RhiColorVertex*, uint32_t count);
    void (*setColorTransform)(RhiInstance*, const float m[16]); // row-major MVP

    // Textures
    RhiTexture* (*createTexture)(RhiInstance*, int w, int h, int mips, uint32_t fmt, const void* rgba8);
    void        (*destroyTexture)(RhiInstance*, RhiTexture*);
    void        (*setTexture)(RhiInstance*, int slot, RhiTexture*);
    void        (*drawTextured)(RhiInstance*, const RhiTexVertex*, uint32_t count);
} RhiOps;

struct RhiInstance {
    const RhiOps* ops;
    RhiBackend    backend;
    int           alphaMode;   // RhiAlphaMode: 0 opaque, 1 alpha-test cutout, 2 alpha blend
};

#ifdef __cplusplus
}
#endif
