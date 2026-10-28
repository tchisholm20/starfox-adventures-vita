#include "port/renderer/rhi_vulkan.h"
#include <stddef.h>

bool rhi_vulkan_available(void) { return false; } // TODO: check loader
RhiInstance* rhi_vulkan_create(const RhiCreateInfo* info) { (void)info; return NULL; }
