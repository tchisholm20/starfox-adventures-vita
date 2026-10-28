// gx_wgpipe.cpp - the single definition of the write-gather pipe object.
//
// GXWGFifo is where compiled game code writes vertex data (GXWGFifo.s16 = x, ...).
// The PPCWGPipe proxy (host shadow of ppcwgpipe_struct.h) turns each store into a
// gxfifo_push_* call, so this object needs no real storage - it just has to exist
// at link time as the symbol every drawing TU references. Built as C++ so the proxy
// operator= overloads are in effect.

#include "main/dll/ppcwgpipe_struct.h"

// Definition of the write-gather pipe (declared extern "C" volatile in the header).
extern "C" { volatile PPCWGPipe GXWGFifo; }
