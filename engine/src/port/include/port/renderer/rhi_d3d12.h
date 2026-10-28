#pragma once
// rhi_d3d12.h - D3D12 backend (primary on Windows)
#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

bool rhi_d3d12_available(void);
RhiInstance* rhi_d3d12_create(const RhiCreateInfo* info);

#ifdef __cplusplus
}
#endif
