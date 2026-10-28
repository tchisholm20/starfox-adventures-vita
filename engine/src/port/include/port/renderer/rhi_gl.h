#pragma once
// rhi_gl.h - OpenGL 4.6 / GLES 3.1 backend (best-effort, Android required)
// Dusklight keeps GL for Android; we do the same. Desktop GL is fallback only.
#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

bool rhi_gl_available(void);
RhiInstance* rhi_gl_create(const RhiCreateInfo* info);

// GLES variant (Android)
bool rhi_gles_available(void);
RhiInstance* rhi_gles_create(const RhiCreateInfo* info);

#ifdef __cplusplus
}
#endif
