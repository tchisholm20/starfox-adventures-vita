#include "port/renderer/rhi_gl.h"
// Best-effort GL - required for Android GLES, fallback on desktop
bool rhi_gl_available(void) { return false; }
RhiInstance* rhi_gl_create(const RhiCreateInfo* info) { (void)info; return NULL; }
bool rhi_gles_available(void) { return false; }
RhiInstance* rhi_gles_create(const RhiCreateInfo* info) { (void)info; return NULL; }
