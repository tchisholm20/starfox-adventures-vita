// ar_shim.c - the GameCube's 16 MB auxiliary RAM (ARAM) as host memory (vita fork).
//
// Replaces upstream's stub, whose ARQPostRequest neither copied nor completed. MusyX
// cannot survive that: aramInit (musyx/runtime/aram_init.c) posts one upload and
// spins on `while (*pendingCount != 0)`, which only the request's completion callback
// clears -- so sndInit would never return.
//
// Here a request is performed IMMEDIATELY (memcpy in its direction) and its callback
// runs before ARQPostRequest returns. MusyX's queue tolerates that ordering: the
// callback finds its slot by pointer and does count -= 1 before the poster's count
// += 1, and both are u8, so 0 -> 255 -> 0 is exact.
//
// ARAM addresses are offsets into gAram. MRAM addresses are host pointers carried in
// u32 fields (ARQRequest.source/dest) -- valid on the 32-bit Vita, NOT on a 64-bit host.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dolphin/types.h"
#include "dolphin/ar.h"

#define ARAM_SIZE (16u * 1024u * 1024u)
#define ARAM_BASE 0x4000u              // SDK: ARInit reserves the first 16 KB for the OS
static unsigned char* gAram;
static u32 gAramTop = ARAM_BASE;
unsigned ar_requests, ar_bytes, ar_rejects;   // read by the port's report

static int aram_ok(void) {
    if (!gAram) gAram = (unsigned char*)calloc(1, ARAM_SIZE);
    return gAram != NULL;
}
// The software DSP (hal/stfx_audio.c) reads sample data here: PB addresses are offsets into this buffer.
unsigned char* ar_shim_base(void) { return aram_ok() ? gAram : NULL; }
u32            ar_shim_size(void) { return ARAM_SIZE; }

u32  ARInit(u32* stack_index_addr, u32 num_entries) {
    (void)stack_index_addr; (void)num_entries;
    aram_ok();
    return ARAM_BASE;
}
BOOL ARCheckInit(void) { return gAram != NULL; }
u32  ARGetBaseAddress(void) { return ARAM_BASE; }
u32  ARGetSize(void) { return ARAM_SIZE; }
u32  ARAlloc(u32 length) {               // SDK: a stack; returns the old top
    u32 a = gAramTop;
    gAramTop += length;
    return a;
}
u32  ARFree(u32* length) { if (length) *length = 0; return gAramTop; }   // the stack's pop is not modelled
void ARQInit(void) { aram_ok(); }

// One transfer. Out-of-range requests are REFUSED (counted), never clipped silently.
static int ar_copy(u32 type, u32 mram, u32 aram, u32 length) {
    if (!aram_ok() || aram > ARAM_SIZE || length > ARAM_SIZE - aram || !mram) { ++ar_rejects; return 0; }
    if (type == ARQ_TYPE_MRAM_TO_ARAM) memcpy(gAram + aram, (const void*)(uintptr_t)mram, length);
    else                               memcpy((void*)(uintptr_t)mram, gAram + aram, length);
    ++ar_requests; ar_bytes += length;
    return 1;
}

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority,
                    u32 source, u32 dest, u32 length, ARQCallback callback) {
    request->next = NULL; request->owner = owner; request->type = type; request->priority = priority;
    request->source = source; request->dest = dest; request->length = length; request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) ar_copy(type, source, dest, length);
    else                               ar_copy(type, dest, source, length);
    if (callback) callback((u32)(uintptr_t)request);
}

// ARStartDMA is the raw engine under ARQ; synchronous here too (no DMA callback exists to run).
void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    ar_copy(type, mainmem_addr, aram_addr, length);
}
u32 ARGetDMAStatus(void) { return 0; }   // never busy
