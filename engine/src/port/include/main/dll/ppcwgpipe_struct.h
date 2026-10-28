#ifndef MAIN_DLL_PPCWGPIPE_STRUCT_H_
#define MAIN_DLL_PPCWGPIPE_STRUCT_H_
// HOST SHADOW of the write-gather-pipe union.
//
// On the console GXWGFifo is a single MMIO register at 0xCC008000; each store to it
// pushes bytes into the GP FIFO (a hardware side effect - successive writes append,
// they do not overwrite). A plain C union can't reproduce that "append on write"
// behaviour, so for C++ translation units we make PPCWGPipe a set of write proxies
// whose operator= forwards each store to the GX FIFO interpreter (gx_draw.c), in the
// console's big-endian order. C translation units (the boot path, which only writes
// GP registers we ignore) keep the plain union - same source, harmless storage.

#include "types.h"

#ifdef __cplusplus

extern "C" {
    void gxfifo_push_u8(unsigned char v);
    void gxfifo_push_u16(unsigned short v);
    void gxfifo_push_u32(unsigned int v);
    void gxfifo_push_f32(float v);
}

struct PPCWGPipe {
    // Integer stores take (int): every 8/16-bit typed value and every int literal
    // the game writes promotes to int, so this one overload is unambiguous for all
    // of them. 32-bit takes both signednesses so large unsigned literals fit. All
    // operators are `volatile`-qualified because GXWGFifo is a volatile object
    // (the write-gather pipe), so stores go through volatile member calls.
    struct P8  { void operator=(int v) volatile { gxfifo_push_u8((unsigned char)v); } };
    struct P16 { void operator=(int v) volatile { gxfifo_push_u16((unsigned short)v); } };
    struct P32 { void operator=(unsigned int v) volatile { gxfifo_push_u32(v); }
                 void operator=(int v)          volatile { gxfifo_push_u32((unsigned int)v); } };
    struct PF  { void operator=(float v)  volatile { gxfifo_push_f32(v); }
                 void operator=(double v) volatile { gxfifo_push_f32((float)v); } };
    union {
        P8 u8;  P8 s8;
        P16 u16; P16 s16;
        P32 u32; P32 s32;
        PF f32;
        unsigned char _storage[8]; // keep the object >= any store a C TU may make
    };
};

// The write-gather pipe object itself (defined in gx_wgpipe.cpp). Declared extern "C"
// so it is the plain _GXWGFifo symbol compiled C game TUs reference via AT_ADDRESS;
// a later `extern volatile PPCWGPipe GXWGFifo;` in a drawing TU inherits this linkage.
extern "C" volatile PPCWGPipe GXWGFifo;

#else

typedef union {
    u8 u8;
    u16 u16;
    u32 u32;
    s8 s8;
    s16 s16;
    s32 s32;
    f32 f32;
} PPCWGPipe;

#endif

#endif
