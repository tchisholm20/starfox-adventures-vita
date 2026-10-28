// game_boot_shims.c - globals the real game boot code needs that normally live in
// hardware or the Dolphin SDK: the write-gather pipe and the video render mode.

#include "dolphin/gx/GXStruct.h"

// GXWGFifo (the write-gather pipe) is defined once in gx_wgpipe.cpp / stairfax_gxdraw,
// where the C++ write-proxy routes vertex stores into the GX FIFO interpreter.

// NTSC 480i, 640x480 - enough for videoInit() to configure VI + the copy pipeline.
GXRenderModeObj gHostRenderMode = {
    0,          // viTVmode
    640,        // fbWidth
    480,        // efbHeight
    480,        // xfbHeight
    0, 0,       // viXOrigin, viYOrigin
    640, 480,   // viWidth, viHeight
    0,          // xFBmode
    0,          // field_rendering
    0,          // aa
    {{0}},      // sample_pattern
    {0}         // vfilter
};
GXRenderModeObj* gRenderModeObj = 0;
