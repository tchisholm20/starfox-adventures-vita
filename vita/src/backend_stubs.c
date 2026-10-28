/* backend_stubs.c - the three desktop backends, declared absent on Vita.
 *
 * rhi.c references all four backends unconditionally: its AUTO path calls
 * rhi_vulkan_available() before falling through to GL, and its switch names
 * rhi_d3d12_create / rhi_d3d11_create in cases that can never be selected here.
 * C still needs them to link, so they exist and answer honestly.
 *
 * ⚠ THESE ARE NOT PLATFORM IMPORTS, AND THE DISTINCTION IS LOAD-BEARING. This
 * repo's rule is never to stub a platform import -- one `extern int
 * sceKernelCreateThread(...)` once made a generator fabricate `return 0` and
 * replace the real OS call for a whole binary, with no instrument output at all
 * as the only symptom. What is stubbed here is the opposite case: upstream's own
 * optional backends, whose contract explicitly includes "not available on this
 * platform". `available() == false` is the designed answer, not a fabrication,
 * and `create()` returning NULL is what rhi.c already handles.
 *
 * If a Vulkan backend is ever wanted on this console it would be real work
 * against SceGxm, not a matter of flipping these to true.
 */

#include "port/renderer/rhi.h"
#include "port/renderer/rhi_vulkan.h"
#include "port/renderer/rhi_d3d12.h"
#include "port/renderer/rhi_d3d11.h"

#include <stddef.h>

bool         rhi_vulkan_available(void)                   { return false; }
RhiInstance *rhi_vulkan_create(const RhiCreateInfo *info) { (void) info; return NULL; }

bool         rhi_d3d12_available(void)                    { return false; }
RhiInstance *rhi_d3d12_create(const RhiCreateInfo *info)  { (void) info; return NULL; }

bool         rhi_d3d11_available(void)                    { return false; }
RhiInstance *rhi_d3d11_create(const RhiCreateInfo *info)  { (void) info; return NULL; }
