/* gx_rt_special.h -- the GX-typed prototypes of the CUSTOM rows (thr = X in host/engine/gx_thr_table.txt).  WRITTEN BY THE PLANNER from that table; module "gen" re-derives it (lint) and must
 * reproduce its non-comment lines identically; module "special" (src/gx_rt_special.c) DEFINES every function here; module "gen" CALLS them from the generated wrappers / the dispatch switch.
 *   gxts_G_<name>(params)      runs on G in threaded mode INSTEAD of the real function: records (gxt_rec_alloc ...) and/or does G-local work; returns what the real function returns
 *   gxts_R_<name>(p, nw)       runs on R: p = payload words of the record gxts_G_<name> wrote (the layout is PRIVATE to special: gen never looks inside); calls __real_<name> or the HAL
 * A custom row's record is opened with gxt_rec_alloc(GXT_OP_<name>, nwords). */
#ifndef GX_RT_SPECIAL_H
#define GX_RT_SPECIAL_H
#include <stdint.h>
#include <stddef.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/gx/GXCpu2Efb.h>
#include <main/pi_flush_api.h>
#include "port/renderer/rhi.h"
#include "gx_rt.h"
#ifdef __cplusplus
extern "C" {
#endif

void gxts_G_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void gxts_R_GXBegin(const uint32_t *p, uint32_t nw);
void gxts_G_GXBeginDisplayList(void* list, u32 size);
void gxts_R_GXBeginDisplayList(const uint32_t *p, uint32_t nw);
void gxts_G_GXCopyTex(void* dest, GXBool clear);
void gxts_R_GXCopyTex(const uint32_t *p, uint32_t nw);
u32 gxts_G_GXEndDisplayList(void);
void gxts_R_GXEndDisplayList(const uint32_t *p, uint32_t nw);
void gxts_G_GXGetScissor(u32* left, u32* top, u32* wd, u32* ht);
void gxts_R_GXGetScissor(const uint32_t *p, uint32_t nw);
void gxts_G_GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso);
void gxts_R_GXInitTexObjLOD(const uint32_t *p, uint32_t nw);
void gxts_G_GXLoadTexObj(GXTexObj* obj, GXTexMapID id);
void gxts_R_GXLoadTexObj(const uint32_t *p, uint32_t nw);
void gxts_G_GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region, GXTexMapID id);
void gxts_R_GXLoadTexObjPreLoaded(const uint32_t *p, uint32_t nw);
void gxts_G_GXPeekZ(u16 x, u16 y, u32* z);
void gxts_R_GXPeekZ(const uint32_t *p, uint32_t nw);
u16 gxts_G_GXReadDrawSync(void);
void gxts_R_GXReadDrawSync(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetArray(GXAttr attr, void* base_ptr, u8 stride);
void gxts_R_GXSetArray(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetCopyClear(GXColor clear_clr, u32 clear_z);
void gxts_R_GXSetCopyClear(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetDrawSync(u16 token);
void gxts_R_GXSetDrawSync(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);
void gxts_R_GXSetFog(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht);
void gxts_R_GXSetScissor(const uint32_t *p, uint32_t nw);
void gxts_G_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap);
void gxts_R_GXSetTexCopyDst(const uint32_t *p, uint32_t nw);

/* [extra] custom rows */
int gxts_G_gx_breakpoint_deliver(void);
void gxts_R_gx_breakpoint_deliver(const uint32_t *p, uint32_t nw);
void gxts_G_gx_dlk_event(const void* p, size_t n, int isWrite);
void gxts_R_gx_dlk_event(const uint32_t *p, uint32_t nw);
void gxts_G_gx_tex_invalidate(const void* addr, u32 n);
void gxts_R_gx_tex_invalidate(const uint32_t *p, uint32_t nw);
void gxts_G_gx_tex_release(const void* addr, u32 n);
void gxts_R_gx_tex_release(const uint32_t *p, uint32_t nw);
void gxts_G_gx_tex_yuv_register(const void* y, const void* u, const void* v, int w, int h);
void gxts_R_gx_tex_yuv_register(const uint32_t *p, uint32_t nw);
void gxts_G_gx_tex_yuv_register_x(const void* y, const void* u, const void* v, int w, int h, uint8_t* ey, uint8_t* eu, uint8_t* ev);
void gxts_R_gx_tex_yuv_register_x(const uint32_t *p, uint32_t nw);

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_SPECIAL_H */
