#pragma once
// rhi_vulkan.h - Vulkan backend (primary)
// Windows/Linux/Android. Requires Vulkan SDK + Volk or SDL Vulkan loader.
#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

bool rhi_vulkan_available(void); // runtime check (loader present + ICD)
RhiInstance* rhi_vulkan_create(const RhiCreateInfo* info);

#ifdef __cplusplus
}
#endif
