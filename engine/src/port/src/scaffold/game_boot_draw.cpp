// game_boot_draw.cpp - a draw issued through the game's OWN GX state.
//
// videoInit() (real game code) already programmed GX_VTXFMT0 into the FIFO
// interpreter via its GXSetVtxDesc / GXSetVtxAttrFmt calls: PNMTXIDX(direct) +
// POS(s16 xyz) + CLR0(rgba8) + TEX0(s16 st, frac7). We deliberately do NOT re-declare
// that format here - we just submit a primitive in it. If the routing works, the
// interpreter decodes it using the game-configured VAT and it renders; if it didn't,
// the stride would be wrong and nothing would appear. This is the proof that
// game_boot's GX calls now flow through gx_draw.
//
// Built as C++ so GXWGFifo.* stores go through the write-gather-pipe proxy.

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"
#include "main/dll/ppcwgpipe_struct.h" // declares extern "C" volatile PPCWGPipe GXWGFifo

#include <cmath>
#include <cstring>

// Per-vertex writers in the game-configured VTXFMT0 layout (matrix index byte,
// position s16 x3, colour rgba8, texcoord s16 x2). Same idiom as the drawing TUs.
static inline void vtx(s16 x, s16 y, s16 z, u8 r, u8 g, u8 b,
                       s16 s = 0, s16 t = 0, u8 mtx = 0) {
    GXWGFifo.u8  = mtx;
    GXWGFifo.s16 = x; GXWGFifo.s16 = y; GXWGFifo.s16 = z;
    GXWGFifo.u8  = r; GXWGFifo.u8 = g; GXWGFifo.u8 = b; GXWGFifo.u8 = 255;
    GXWGFifo.s16 = s; GXWGFifo.s16 = t;
}

extern "C" void game_boot_draw_test(int frame) {
    // Projection + a slow spin (the game sets these per view; the vertex FORMAT is
    // what came from videoInit).
    float proj[4][4]; std::memset(proj, 0, sizeof proj);
    const float aspect = 1280.0f / 720.0f, zn = 0.1f, zf = 1000.0f;
    float ys = 1.0f / std::tan((60.0f * 3.14159265f / 180.0f) * 0.5f);
    proj[0][0] = ys / aspect; proj[1][1] = ys;
    proj[2][2] = zf / (zf - zn); proj[2][3] = -zn * zf / (zf - zn); proj[3][2] = 1.0f;

    float a = (float)frame * 0.02f, c = std::cos(a), s = std::sin(a);
    float mv[3][4] = {
        {  c, 0.0f,   s, 0.0f },
        {0.0f, 1.0f, 0.0f, 0.0f },
        { -s, 0.0f,   c, 400.0f },
    };

    GXSetProjection(proj, GX_PERSPECTIVE);
    GXLoadPosMtxImm(mv, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    vtx(   0,  120, 0, 255,  40,  40);
    vtx( 120, -100, 0,  40, 255,  40);
    vtx(-120, -100, 0,  40,  40, 255);
    GXEnd();
}
