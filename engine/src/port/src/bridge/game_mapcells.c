// game_mapcells.c - decode a map's terrain cell grid for real block placement.
//
// This is the faithful port of the real cell-grid logic (mapGetRomListAndOffsets +
// mapFillCellEntry in shader.c). A map's MAPS.bin section (MAPS.tab words 0..7) begins
// with a MapRomListPage header {s16 sizeX, sizeZ, originX, originZ; u16 objectDataSize}
// and holds the cell grid at word1 (== the runtime `page->cells`, set to
// base + (word1 - word0)). Each cell is a big-endian u32:
//   cellIndex    = (cell >> 0x11) & 0x3f    // block within the romlist group
//   romListIndex = (cell >> 0x17) & 0xff    // 0xff = empty cell (no block)
//   blockId      = cellIndex + gTrkBlkTab[romListIndex]   // global block id
// gTrkBlkTab (TRKBLK.tab, 0xffff-terminated u16) holds the cumulative block-id base per
// romlist group; blockId is the global index into the map's BLOCKS.tab (the modNN.tab).
// The cell array is addressed [gridX-minX + (gridZ-minZ)*sizeX] with minX/minZ == the
// page origin, so cell (cx,cz) is world grid ((originX+cx),(originZ+cz)); dropping the
// common origin, the block sits at world ((cx-originX)*640, (cz-originZ)*640) and object
// romlist coords (which the game shifts by +origin*640) then land on it 1:1.

#include "port/byteswap.h"
#include <stdint.h>

extern void loadAssetFileById(void* out, int fileId);

#define MLDF_MAPS_TAB   0x1e
#define MLDF_MAPS_BIN   0x1d
#define MLDF_TRKBLK_TAB 0x27
#define WORDS_PER_MAP   7

typedef struct { int cellX, cellZ, blockId; } StairfaxMapCell;

// Fill out[] with one entry per populated cell (up to maxOut) and return the count;
// negative on error. sizeX/sizeZ/originX/originZ are the grid dimensions and origin.
int stairfax_mapcells_decode(int mapId, StairfaxMapCell* out, int maxOut,
                             int* pSizeX, int* pSizeZ, int* pOriginX, int* pOriginZ) {
    int32_t* tab = 0; unsigned char* bin = 0; unsigned char* trk = 0;
    loadAssetFileById(&tab, MLDF_MAPS_TAB);    // host-order words (sw_u32)
    loadAssetFileById(&bin, MLDF_MAPS_BIN);    // raw big-endian section data
    loadAssetFileById(&trk, MLDF_TRKBLK_TAB);  // host-order u16 (sw_u16)
    if (!tab || !bin || !trk || mapId < 0) return -1;

    int base = mapId * WORDS_PER_MAP;
    int w0 = tab[base + 0], w1 = tab[base + 1];
    unsigned char* sec = bin + w0;
    int sizeX   = (int16_t)beRead16(sec + 0);
    int sizeZ   = (int16_t)beRead16(sec + 2);
    int originX = (int16_t)beRead16(sec + 4);
    int originZ = (int16_t)beRead16(sec + 6);
    if (sizeX <= 0 || sizeZ <= 0 || sizeX > 64 || sizeZ > 64) return -2;

    uint16_t* trkv = (uint16_t*)trk;
    int entries = 0; while (trkv[entries] != 0xffff) entries++;
    int trkTabCount = entries - 1;             // == gTrkBlkTabCount
    if (trkTabCount < 1) return -3;

    unsigned char* cells = bin + w1;
    int n = 0;
    for (int cz = 0; cz < sizeZ; ++cz) {
        for (int cx = 0; cx < sizeX; ++cx) {
            uint32_t cell = beRead32(cells + (cx + cz * sizeX) * 4);
            int rli = (int)((cell >> 0x17) & 0xff);
            if (rli == 0xff) continue;
            int ci = (int)((cell >> 0x11) & 0x3f);
            if (rli >= trkTabCount) rli = trkTabCount - 1;
            int blockId = ci + (int)trkv[rli];
            if (blockId >= (int)trkv[trkTabCount]) blockId = (int)trkv[trkTabCount] - 1;
            if (n < maxOut) { out[n].cellX = cx; out[n].cellZ = cz; out[n].blockId = blockId; }
            n++;
        }
    }
    if (pSizeX)   *pSizeX   = sizeX;
    if (pSizeZ)   *pSizeZ   = sizeZ;
    if (pOriginX) *pOriginX = originX;
    if (pOriginZ) *pOriginZ = originZ;
    return n;
}
