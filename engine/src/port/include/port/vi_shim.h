#pragma once
// vi_shim.h - VI -> SDL_Window/swapchain
// Shadows include/dolphin/vi.h

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GXRenderModeObj GXRenderModeObj; // from dolphin/gx (GXStruct.h)
typedef struct RhiInstance RhiInstance;         // from renderer/rhi.h
typedef struct PlatWindow  PlatWindow;          // from plat_window.h

void VIInit(void);
void VIConfigure(GXRenderModeObj* rmode);
void VISetNextFrameBuffer(void* fb);
void VISetBlack(bool black);
void VIFlush(void);
void VIWaitForRetrace(void);
int VIGetNextField(void);
int VIGetTvFormat(void);
int VIGetDTVStatus(void);

typedef void (*VIRetraceCallback)(uint32_t count);
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb);
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb);

// Host helpers - expose the objects VI owns so pad/gx shims and tools can reach them.
PlatWindow*  vi_host_window(void);   // the host window VI created
RhiInstance* vi_host_rhi(void);      // the RHI instance VI created
void*        vi_shim_getWindow(void); // native handle (HWND) for input shims
void         vi_shim_pollEvents(void);
int          vi_shim_shouldClose(void);
void         vi_set_clear_color(float r, float g, float b); // frame clear tint (e.g. sky)

#ifdef __cplusplus
}
#endif
