// gx_light.c - GX light-object API for the port (vita fork).
//
// The GXInitLight* / GXGetLightAttnK / GXInitSpecularDir bodies below are the SDK's
// (src/dolphin/gx/GXLight.c), extracted MECHANICALLY by the fork's mk_light step:
// debug ASSERTMSGLINE/CHECK_GXBEGIN lines dropped, nothing else changed. They are
// pure struct arithmetic on the game's opaque 64-byte GXLightObj.
// One deliberate difference: sqrtf is libm's, not the SDK's 3-step __frsqrte Newton
// refinement (that is the PPC estimate instruction; libm is at least as exact).
//
// GXLoadLightObjImm: on hardware this writes the object to XF light registers. Here
// it is RECORDED into gx_light_regs[] (+ a load counter) -- the port's renderer does
// not light yet, so nothing consumes it. Recorded, not dropped, so the state is
// inspectable when lighting is wired.
#include <math.h>
#include <string.h>
#include "dolphin/gx/GXStruct.h"
#include "dolphin/gx/GXEnum.h"
#include "dolphin/gx/GXLighting.h"
#include "port/gx_hal_state.h"   /* R2: gx_light_regs[] moved into the HAL state struct (gx_hal.lightRegs) */

// GXLightObj private data
typedef struct {
    u32 reserved[3];
    u32 Color;
    f32 a[3];
    f32 k[3];
    f32 lpos[3];
    f32 ldir[3];
} __GXLightObjInt_struct;

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;
    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightAttnA(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;
    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
}

void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;
    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXGetLightAttnK(const GXLightObj* lt_obj, f32* k0, f32* k1, f32* k2) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;
    *k0 = obj->k[0];
    *k1 = obj->k[1];
    *k2 = obj->k[2];
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    f32 cr;
    f32 d;
    f32 a0, a1, a2;
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    if (cutoff <= 0.0f || cutoff > 90.0f)
        spot_func = GX_SP_OFF;

    cr = cosf((3.1415927f * cutoff) / 180.0f);

    switch (spot_func) {
    case GX_SP_FLAT:
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case GX_SP_COS:
        a2 = 0.0f;
        a0 = -cr / (1.0f - cr);
        a1 = 1.0f / (1.0f - cr);
        break;
    case GX_SP_COS2:
        a0 = 0.0f;
        a1 = -cr / (1.0f - cr);
        a2 = 1.0f / (1.0f - cr);
        break;
    case GX_SP_SHARP: {
        f32 u = 1.0f - cr;
        d = u * u;
        a1 = 2.0f / d;
        a0 = (cr * (cr - 2.0f)) / d;
        a2 = -1.0f / d;
        break;
    }
    case GX_SP_RING1: {
        f32 u = 1.0f - cr;
        d = u * u;
        a0 = (-4.0f * cr) / d;
        a1 = (4.0f * (1.0f + cr)) / d;
        a2 = -4.0f / d;
        break;
    }
    case GX_SP_RING2: {
        f32 u = 1.0f - cr;
        f32 two_cr = 2.0f * cr;
        d = u * u;
        a0 = 1.0f - (two_cr * cr) / d;
        a1 = (4.0f * cr) / d;
        a2 = -2.0f / d;
        break;
    }
    case GX_SP_OFF:
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }
    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    f32 k0, k1, k2;
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    if (ref_dist < 0.0f)
        dist_func = GX_DA_OFF;
    if (ref_br <= 0.0f || ref_br >= 1.0f)
        dist_func = GX_DA_OFF;

    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f;
        k1 = (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_OFF:
    default:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = 0.0f;
        break;
    }

    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    obj->lpos[0] = x;
    obj->lpos[1] = y;
    obj->lpos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    obj->ldir[0] = -nx;
    obj->ldir[1] = -ny;
    obj->ldir[2] = -nz;
}

void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    f32 mag;
    f32 vx;
    f32 vy;
    f32 vz;
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    vx = -nx;
    vy = -ny;
    vz = 1.0f - nz;

    mag = 1.0f / sqrtf((vx * vx) + (vy * vy) + (vz * vz));

    obj->ldir[0] = vx * mag;
    obj->ldir[1] = vy * mag;
    obj->ldir[2] = vz * mag;
    obj->lpos[0] = vx * 1048576.0f;
    obj->lpos[1] = vy * 1048576.0f;
    obj->lpos[2] = -nz * 1048576.0f;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color) {
    __GXLightObjInt_struct* obj;

    obj = (__GXLightObjInt_struct*)lt_obj;

    obj->Color = ((u32)color.r << 24) | ((u32)color.g << 16) | ((u32)color.b << 8) | color.a;
}

unsigned gx_light_loads;   // a statistic, outside the state struct: while R2 is active only G runs this body

GXH_INLINE void gxh_b_GXLoadLightObjImm(GxHal* h, const int mark, const GXLightObj* lt_obj, GXLightID light) {
    // GXLightID is a bit mask (GX_LIGHT0 = 1 << 0 ... GX_LIGHT7 = 1 << 7); the SDK's
    // ConvLightID2Num maps anything else to light 0 -- kept.
    unsigned idx = 0;
    if (light && !(light & (light - 1)) && light <= 0x80) idx = (unsigned)__builtin_ctz((unsigned)light);
    memcpy(&h->lightRegs[idx], lt_obj, sizeof(__GXLightObjInt_struct));
    ++gx_light_loads;
    GXH_MARK(h, mark, GXH_G_LIGHT0 + (int)idx);
}
GXH_PAIR(GXLoadLightObjImm, (const GXLightObj* lt_obj, GXLightID light), lt_obj, light)
_Static_assert(sizeof(GxHalLight) == sizeof(__GXLightObjInt_struct), "the HAL's light register mirrors the SDK's private light object");
_Static_assert(sizeof(__GXLightObjInt_struct) == sizeof(GXLightObj), "light layout must fill GXLightObj");
