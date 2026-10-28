#pragma once
// gx_draw.h - the game-facing GX draw path: real GX vertex API + write-gather pipe,
// decoded through the vertex-attribute table and submitted to the RHI.
//
// Unlike gx_shim.h (the viewer's custom immediate-mode helper), this library
// implements the REAL Dolphin GX signatures (GXBegin/GXSetVtxDesc/GXSetVtxAttrFmt/
// GXLoadPosMtxImm/...) and consumes vertices from GXWGFifo exactly as compiled game
// code does, so real drawing TUs can link straight against it.

#include "port/renderer/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wiring: hand the interpreter the RHI it should draw into (once, at startup).
void gx_draw_setRhi(RhiInstance* rhi, RhiSwapchain* sc);
void gx_draw_init(void);
// Bind/unbind the texture sampled by subsequent primitives that carry TEX0.
void gx_draw_setTexture(RhiTexture* tex);
// Alpha handling for subsequent textured primitives (RhiAlphaMode: 0 opaque, 1 test, 2 blend).
void gx_draw_setAlphaMode(int mode);
void gx_draw_setSourceBounds(const void* lo, const void* hi);
void gx_draw_setJointOffsets(const float off[][3], int n);
void gx_draw_setJointMatrices(const float m[][3][4], int n);
void gx_draw_setForceLayout(int stride, int posOff, int posSz);
// Real GX skinning: transform each vertex by gPosMtx[PNMTXIDX/3] then project by gProj alone
// (the retail model render path). Off = interim gJointOff/gJointMtx path. Set around a draw.
void gx_draw_setRealSkin(int on);

// Real GX entry point (also in dolphin/gx/GXDispList.h): replay a recorded command
// buffer of primitive draws. Indexed attributes read from GXSetArray bases.
void GXCallDisplayList(void* list, unsigned int nbytes);

// The write-gather pipe sink (called by the PPCWGPipe C++ proxy, one per store).
// Bytes are buffered big-endian, matching the console FIFO, and a primitive is
// decoded+drawn as soon as GXBegin's advertised vertex count has arrived.
void gxfifo_push_u8(unsigned char v);
void gxfifo_push_u16(unsigned short v);
void gxfifo_push_u32(unsigned int v);
void gxfifo_push_f32(float v);

#ifdef __cplusplus
}
#endif
