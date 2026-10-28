/* compat/windows.h - map_view.cpp includes <windows.h> unconditionally and uses
 * exactly one thing from it: Sleep(), in its post-present --capture path, which
 * the Vita wrapper never enables (it captures PRE-swap instead). Anything else
 * reaching for Win32 should fail to compile, so nothing else is declared. */
#pragma once
#include <psp2/kernel/threadmgr.h>
static inline void Sleep(unsigned ms) { sceKernelDelayThread(ms * 1000u); }
