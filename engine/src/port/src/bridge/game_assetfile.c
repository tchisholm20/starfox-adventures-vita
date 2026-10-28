// game_assetfile.c - the port's MLDF resident-data-file loader.
//
// The game loads numbered resident data files via loadAssetFileById(out, fileId) (async
// through loadAsset on console). Here we load them synchronously by id -> filename from the
// ISO, cache by id, and byte-swap to host order per the file's layout (port/byteswap.h) so
// the recompiled game code reads them natively. getDataFileSize(id) returns the byte size,
// matching the real pi_dolphin.c (gResourceFileSizes[id]).
//
// Add a row to gMldf as each subsystem needs another data file, with the right swapper:
//   u16 array (sw_u16), -1-terminated u32 array (sw_u32), record swap (sw_bittable), or
//   none (sw_none) for heterogeneous blobs read via typed accessors later.

#include "port/dvd_shim.h"
#include "port/byteswap.h"
#include "main/mm.h"     // mmAlloc - loaded buffers the game later mm_free's must be mm-owned

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*SwapFn)(void* buf, int size);

static void sw_none(void* b, int s) { (void)b; (void)s; }
static void sw_u16(void* b, int s)  { beFixArray16(b, (size_t)(s / 2)); }
static void sw_u32(void* b, int s)  { beFixArray32(b, (size_t)(s / 4)); }

// BITTABLE.bin GameBitDef {u16 firstBit; u8 flags; u8 taskHintId}: only firstBit swaps.
typedef struct { uint16_t firstBit; uint8_t flags; uint8_t taskHintId; } GameBitDefLE;
static void bswapGameBitDef(void* p) { GameBitDefLE* e = (GameBitDefLE*)p; BE16(e->firstBit); }
static void sw_bittable(void* b, int s) { beFixRecords(b, (size_t)(s / 4), 4, bswapGameBitDef); }

static const struct { int id; const char* name; SwapFn sw; } gMldf[] = {
    { 0x33, "BITTABLE.bin", sw_bittable }, // gamebits
    { 0x16, "TABLES.bin",   sw_none     }, // object tables data (typed access later)
    { 0x17, "TABLES.tab",   sw_u32      }, // -1-terminated u32 index
    { 0x3d, "OBJECTS.tab",  sw_u32      }, // -1-terminated u32 offsets
    { 0x3e, "OBJECTS.bin",  sw_none     }, // object defs (typed access later)
    { 0x3f, "OBJINDEX.bin", sw_u16      }, // s16 seq->objId table
    { 0x1e, "MAPS.tab",     sw_u32      }, // per-map 7-word section offsets into MAPS.bin
    { 0x1d, "MAPS.bin",     sw_none     }, // romlist/map data (typed access via offsets)
    { 0x27, "TRKBLK.tab",   sw_u16      }, // cumulative per-romlist block-id bases (0xffff-term)
    { 0x35, "VOXOBJ.tab",   sw_u32      }, // voxmaps_initialise map list (int*, -1-term); real camera
};

#define MLDF_MAX_ID 0x100
static void* gBuf[MLDF_MAX_ID];
static int   gSize[MLDF_MAX_ID];

void loadAssetFileById(void* out, int fileId) {
    for (unsigned i = 0; i < sizeof(gMldf) / sizeof(gMldf[0]); ++i) {
        if (gMldf[i].id != fileId) continue;
        if (fileId >= 0 && fileId < MLDF_MAX_ID && !gBuf[fileId]) {
            int sz = 0;
            void* b = loadFileByPath((char*)gMldf[i].name, &sz, 0);
            if (b) {
                gMldf[i].sw(b, sz);       // BE -> host per layout
                gBuf[fileId] = b; gSize[fileId] = sz;
            } else if (getenv("STAIRFAX_TRACE")) {
                fprintf(stderr, "[asset] %s (id 0x%x) not found in ISO\n", gMldf[i].name, fileId);
            }
        }
        if (fileId >= 0 && fileId < MLDF_MAX_ID && gBuf[fileId]) *(void**)out = gBuf[fileId];
        return;
    }
    // Unknown id: leave *out unchanged (matches the prior stub - no regression).
}

int getDataFileSize(int idx) {
    return (idx >= 0 && idx < MLDF_MAX_ID) ? gSize[idx] : 0;
}

// The game's by-path async loader, adapted to synchronous host IO. Callers read *outSize
// immediately after the call and later mm_free the returned buffer, so the buffer MUST be
// mm-owned (a plain malloc corrupts the mm heap on free - that was the text-load crash).
// We load the whole file up front, mmAlloc it, fire the callback with a success result,
// and return it. (Lives here, not in the DVD HAL, because only this layer links mm.)
void* loadFileByPathAsync(char* path, int* outSize, int prio, DVDCallback callback) {
    (void)prio;
    DVDFileInfo info;
    if (!DVDOpen(path, &info)) {
        if (outSize) *outSize = 0;
        if (callback) callback(-1, 0);
        return 0;
    }
    int len = info.length;
    void* buf = mmAlloc(len > 0 ? len : 1, 0xE, 0);
    if (buf && len > 0) DVDRead(&info, buf, len, 0);
    if (outSize) *outSize = len;
    if (callback) callback(len, &info);
    DVDClose(&info);
    return buf;
}
