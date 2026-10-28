#pragma once
// rhi_d3d11.h - D3D11 backend (best-effort fallback)
// Like Dusklight's d3d11: not performance-targeted, for older GPUs/handhelds.
// Feature level 11_0, no mesh shaders, TEV ubershader may be simplified.
#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

bool rhi_d3d11_available(void);
RhiInstance* rhi_d3d11_create(const RhiCreateInfo* info);

#ifdef __cplusplus
}
#endif
