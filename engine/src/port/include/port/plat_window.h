#pragma once
// plat_window.h - minimal host window abstraction.
//
// Owns an OS window and hands out its native handle (HWND on Windows) for a
// backend to bind a swapchain to. vi_shim will sit on top of this later; for now
// the renderer smoketest drives it directly. Kept backend-agnostic on purpose so
// D3D11/D3D12/Vulkan all consume the same handle.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlatWindow PlatWindow;

PlatWindow* plat_window_create(const char* title, int width, int height);
void        plat_window_destroy(PlatWindow* w);

// Native handle for swapchain creation: HWND on Windows.
void*       plat_window_native_handle(PlatWindow* w);

// Pump the OS message queue. Returns false once the window has been closed.
bool        plat_window_pump(PlatWindow* w);

void        plat_window_size(PlatWindow* w, int* outW, int* outH);

// Capture the window's client area to a 24-bit BMP file. Uses DWM-composited
// readback so it works with flip-model D3D and Vulkan swapchains. Returns true
// on success. Purely a diagnostic for the bring-up milestone.
bool        plat_window_capture_bmp(PlatWindow* w, const char* path);

#ifdef __cplusplus
}
#endif
