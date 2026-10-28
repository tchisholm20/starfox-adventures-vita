#ifndef DOLPHIN_GX_GXLEGACY_H_
#define DOLPHIN_GX_GXLEGACY_H_

#include "types.h"

typedef union PPCWGPipe2 {
    u8 u8;
    u16 u16;
    u32 u32;
    s8 s8;
    s16 s16;
    s32 s32;
    f32 f32;
    f64 f64;
} PPCWGPipe2;

#ifdef STAIRFAX_PORT
extern volatile PPCWGPipe2 GXWGFifo;   /* port: the write-gather pipe is a real object defined in
                                          gx_wgpipe.cpp (append-proxy -> RHI); MSVC can't parse the
                                          Metrowerks hardware-address placement below. */
#else
PPCWGPipe2 GXWGFifo : (0xCC008000);
#endif

#endif /* DOLPHIN_GX_GXLEGACY_H_ */
