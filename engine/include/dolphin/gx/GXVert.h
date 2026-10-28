#ifndef _DOLPHIN_GX_GXVERT_H_
#define _DOLPHIN_GX_GXVERT_H_

#ifdef __REVOLUTION_SDK__
#include <revolution/gx/GXVert.h>
#else
#include <dolphin/types.h>
#include <dolphin/os.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GXFIFO_ADDR 0xCC008000

typedef union {
    u8  u8;
    u16 u16;
    u32 u32;
    u64 u64;
    s8  s8;
    s16 s16;
    s32 s32;
    s64 s64;
    f32 f32;
    f64 f64;
} PPCWGPipe;

#ifdef __MWERKS__
volatile PPCWGPipe GXWGFifo AT_ADDRESS(GXFIFO_ADDR);
#else
#define GXWGFifo (*(volatile PPCWGPipe*)GXFIFO_ADDR)
#endif

/* vita fork: with no fault trap (the Windows build catches 0xCC008000 stores with a
 * VEH handler; the Vita cannot), every write-gather-pipe store must be a CALL. */
#if defined(STAIRFAX_PORT) && !defined(_WIN32)
#define STFX_WGPIPE_CALLS 1
void gxfifo_push_u8(unsigned char v);  void gxfifo_push_s8(signed char v);
void gxfifo_push_u16(unsigned short v); void gxfifo_push_s16(short v);
void gxfifo_push_u32(unsigned int v);  void gxfifo_push_s32(int v);
void gxfifo_push_f32(float v);
#endif

#if defined(STFX_WGPIPE_CALLS)
#define FUNC_1PARAM(name, T) \
static inline void name##1##T(const T x) { gxfifo_push_##T(x); }
#define FUNC_2PARAM(name, T) \
static inline void name##2##T(const T x, const T y) { gxfifo_push_##T(x); gxfifo_push_##T(y); }
#define FUNC_3PARAM(name, T) \
static inline void name##3##T(const T x, const T y, const T z) { gxfifo_push_##T(x); gxfifo_push_##T(y); gxfifo_push_##T(z); }
#define FUNC_4PARAM(name, T) \
static inline void name##4##T(const T x, const T y, const T z, const T w) { gxfifo_push_##T(x); gxfifo_push_##T(y); gxfifo_push_##T(z); gxfifo_push_##T(w); }
#define FUNC_INDEX8(name) \
static inline void name##1x8(const u8 x) { gxfifo_push_u8(x); }
#define FUNC_INDEX16(name) \
static inline void name##1x16(const u16 x) { gxfifo_push_u16(x); }

#elif DEBUG

// external functions

#define FUNC_1PARAM(name, T) void name##1##T(const T x);
#define FUNC_2PARAM(name, T) void name##2##T(const T x, const T y);
#define FUNC_3PARAM(name, T) void name##3##T(const T x, const T y, const T z);
#define FUNC_4PARAM(name, T) void name##4##T(const T x, const T y, const T z, const T w);
#define FUNC_INDEX8(name)    void name##1x8(const u8 x);
#define FUNC_INDEX16(name)   void name##1x16(const u16 x);

#else

// inline functions

#define FUNC_1PARAM(name, T) \
static inline void name##1##T(const T x) { GXWGFifo.T = x; }

#define FUNC_2PARAM(name, T) \
static inline void name##2##T(const T x, const T y) { GXWGFifo.T = x; GXWGFifo.T = y; }

#define FUNC_3PARAM(name, T) \
static inline void name##3##T(const T x, const T y, const T z) { GXWGFifo.T = x; GXWGFifo.T = y; GXWGFifo.T = z; }

#define FUNC_4PARAM(name, T) \
static inline void name##4##T(const T x, const T y, const T z, const T w) { GXWGFifo.T = x; GXWGFifo.T = y; GXWGFifo.T = z; GXWGFifo.T = w; }

#define FUNC_INDEX8(name) \
static inline void name##1x8(const u8 x) { GXWGFifo.u8 = x; }

#define FUNC_INDEX16(name) \
static inline void name##1x16(const u16 x) { GXWGFifo.u16 = x; }

#endif

// GXCmd
FUNC_1PARAM(GXCmd, u8)
FUNC_1PARAM(GXCmd, u16)
FUNC_1PARAM(GXCmd, u32)

// GXParam
FUNC_1PARAM(GXParam, u8)
FUNC_1PARAM(GXParam, u16)
FUNC_1PARAM(GXParam, u32)
FUNC_1PARAM(GXParam, s8)
FUNC_1PARAM(GXParam, s16)
FUNC_1PARAM(GXParam, s32)
FUNC_1PARAM(GXParam, f32)
FUNC_3PARAM(GXParam, f32)
FUNC_4PARAM(GXParam, f32)

// GXPosition
FUNC_3PARAM(GXPosition, f32)
FUNC_3PARAM(GXPosition, u8)
FUNC_3PARAM(GXPosition, s8)
FUNC_3PARAM(GXPosition, u16)
FUNC_3PARAM(GXPosition, s16)
FUNC_2PARAM(GXPosition, f32)
FUNC_2PARAM(GXPosition, u8)
FUNC_2PARAM(GXPosition, s8)
FUNC_2PARAM(GXPosition, u16)
FUNC_2PARAM(GXPosition, s16)
FUNC_INDEX16(GXPosition)
FUNC_INDEX8(GXPosition)

// GXNormal
FUNC_3PARAM(GXNormal, f32)
FUNC_3PARAM(GXNormal, s16)
FUNC_3PARAM(GXNormal, s8)
FUNC_INDEX16(GXNormal)
FUNC_INDEX8(GXNormal)

// GXColor
FUNC_4PARAM(GXColor, u8)
FUNC_1PARAM(GXColor, u32)
FUNC_3PARAM(GXColor, u8)
FUNC_1PARAM(GXColor, u16)
FUNC_INDEX16(GXColor)
FUNC_INDEX8(GXColor)

// GXTexCoord
FUNC_2PARAM(GXTexCoord, f32)
FUNC_2PARAM(GXTexCoord, s16)
FUNC_2PARAM(GXTexCoord, u16)
FUNC_2PARAM(GXTexCoord, s8)
FUNC_2PARAM(GXTexCoord, u8)
FUNC_1PARAM(GXTexCoord, f32)
FUNC_1PARAM(GXTexCoord, s16)
FUNC_1PARAM(GXTexCoord, u16)
FUNC_1PARAM(GXTexCoord, s8)
FUNC_1PARAM(GXTexCoord, u8)
FUNC_INDEX16(GXTexCoord)
FUNC_INDEX8(GXTexCoord)

// GXMatrixIndex
FUNC_1PARAM(GXMatrixIndex, u8)

#undef FUNC_1PARAM
#undef FUNC_2PARAM
#undef FUNC_3PARAM
#undef FUNC_4PARAM
#undef FUNC_INDEX8
#undef FUNC_INDEX16

#ifdef __cplusplus
}
#endif

#endif
#endif
