// plat_window_win32.c - Win32 implementation of plat_window.h.

#include "port/plat_window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct PlatWindow {
    HWND hwnd;
    int  width, height;
    bool closed;
};

static LRESULT CALLBACK plat_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlatWindow* w = (PlatWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CLOSE:
            if (w) w->closed = true;
            return 0;
        case WM_DESTROY:
            return 0;
        case WM_SIZE:
            if (w) { w->width = LOWORD(lp); w->height = HIWORD(lp); }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

PlatWindow* plat_window_create(const char* title, int width, int height) {
    PlatWindow* w = (PlatWindow*)calloc(1, sizeof(PlatWindow));
    if (!w) return NULL;
    w->width = width;
    w->height = height;

    HINSTANCE hinst = GetModuleHandleW(NULL);
    static const wchar_t* kClass = L"StairfaxRhiWindow";
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = plat_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // ignore "already registered" on repeat

    // Convert title to wide.
    wchar_t wtitle[256];
    if (!title) title = "Stairfax Temperatures";
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);

    RECT r = {0, 0, width, height};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kClass, wtitle, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, hinst, NULL);
    if (!hwnd) { free(w); return NULL; }

    w->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return w;
}

void plat_window_destroy(PlatWindow* w) {
    if (!w) return;
    if (w->hwnd) DestroyWindow(w->hwnd);
    free(w);
}

void* plat_window_native_handle(PlatWindow* w) {
    return w ? (void*)w->hwnd : NULL;
}

bool plat_window_pump(PlatWindow* w) {
    if (!w) return false;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !w->closed;
}

void plat_window_size(PlatWindow* w, int* outW, int* outH) {
    if (!w) return;
    RECT rc;
    if (GetClientRect(w->hwnd, &rc)) {
        if (outW) *outW = rc.right - rc.left;
        if (outH) *outH = rc.bottom - rc.top;
    } else {
        if (outW) *outW = w->width;
        if (outH) *outH = w->height;
    }
}

bool plat_window_capture_bmp(PlatWindow* w, const char* path) {
    if (!w || !w->hwnd) return false;

    RECT rc;
    if (!GetClientRect(w->hwnd, &rc)) return false;
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;

    HDC hdcWin = GetDC(w->hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWin, cw, ch);
    HGDIOBJ old = SelectObject(hdcMem, bmp);

    // PW_RENDERFULLCONTENT (0x2) pulls DWM-composited content, so it captures
    // GPU-rendered (D3D/Vulkan flip-model) client area rather than black.
    PrintWindow(w->hwnd, hdcMem, 0x00000002 /*PW_RENDERFULLCONTENT*/);

    BITMAPINFOHEADER bih = {0};
    bih.biSize = sizeof(bih);
    bih.biWidth = cw;
    bih.biHeight = ch; // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    int rowStride = ((cw * 3 + 3) & ~3);
    int imgSize = rowStride * ch;
    unsigned char* pixels = (unsigned char*)malloc(imgSize);
    bool result = false;
    if (pixels) {
        if (GetDIBits(hdcMem, bmp, 0, ch, pixels, (BITMAPINFO*)&bih, DIB_RGB_COLORS)) {
            BITMAPFILEHEADER bfh = {0};
            bfh.bfType = 0x4D42; // 'BM'
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            bfh.bfSize = bfh.bfOffBits + imgSize;

            FILE* f = NULL;
            fopen_s(&f, path, "wb");
            if (f) {
                fwrite(&bfh, sizeof(bfh), 1, f);
                fwrite(&bih, sizeof(bih), 1, f);
                fwrite(pixels, imgSize, 1, f);
                fclose(f);
                result = true;
            }
        }
        free(pixels);
    }

    SelectObject(hdcMem, old);
    DeleteObject(bmp);
    DeleteDC(hdcMem);
    ReleaseDC(w->hwnd, hdcWin);
    return result;
}
