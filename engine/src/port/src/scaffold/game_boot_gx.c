// game_boot_gx.c - minimal GX entry points for the boot path.
//
// videoInit()/waitNextFrame() call ~40 GX functions to program the GameCube GP:
// FIFO setup, vertex formats, the copy pipeline, viewport/scissor. On the boot
// milestone there is no GP hardware and we present a cleared framebuffer through
// the VI shim, so these are host no-ops that only need to satisfy the link and
// return sane values. They are defined WITHOUT the real GX prototypes on purpose:
// C links by bare symbol name, so the caller's real signatures bind to these
// arg-ignoring definitions with no header conflict. Real geometry submission will
// replace this file with a GX->RHI translator (see gx_shim.c for the draw path).

#include "dolphin/types.h"

// A scratch FIFO object handed back by GXInit; the boot code stores the pointer
// but the host never dereferences GP state.
static u8 gBootFifo[0x100];

void* GXInit()               { return gBootFifo; }

void GXSetDispCopySrc()      { }
void GXSetDispCopyDst()      { }
u32  GXSetDispCopyYScale()   { return 1; }
void GXSetDispCopyGamma()    { }
void GXSetCopyClear()        { }
void GXSetCopyFilter()       { }
void GXCopyDisp()            { }

void GXInitFifoBase()        { }
void GXInitFifoLimits()      { }
void GXSetCPUFifo()          { }
void GXSetGPFifo()           { }
void GXGetFifoPtrs()         { }
void GXSetBreakPtCallback()  { }
void GXEnableBreakPt()       { }
void GXSetDrawSync()         { }
void GXFlush()               { }

void GXSetViewport()         { }
void GXSetScissor()          { }
void GXSetFieldMode()        { }
void GXSetPixelFmt()         { }
void GXSetDither()           { }
void GXSetCullMode()         { }
void GXSetBlendMode()        { }
void GXSetAlphaUpdate()      { }
void GXSetNumChans()         { }
void GXSetChanCtrl()         { }
void GXSetMisc()             { }

void GXInvalidateVtxCache()  { }
void GXInvalidateTexAll()    { }
void GXEnableTexOffsets()    { }

// GXClearVtxDesc / GXSetVtxDesc / GXSetVtxAttrFmt / GXLoadPosMtxImm /
// GXLoadTexMtxImm / GXSetCurrentMtx are now the REAL implementations in gx_draw.c
// (stairfax_gxdraw) - videoInit's calls program the actual vertex-format/matrix
// state the FIFO interpreter decodes against.

u32  GXReadXfRasMetric()     { return 0; }
void GXGetGPStatus()         { }
void GXSetGPMetric()         { }
