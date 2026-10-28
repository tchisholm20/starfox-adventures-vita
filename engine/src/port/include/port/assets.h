#pragma once
// assets.h - reusable Star Fox Adventures asset readers (disc-format layer).
//
// Shared by the port tools (map_view, model_view, tex_view, ...): big-endian helpers,
// ZLB/DIR decompression, TEX0/TEX1 texture records, and MapBlockData parsing. Keeps the
// on-disc format knowledge in one place instead of copied per tool. Pure data parsing -
// no RHI/GX dependency; callers decode textures with gxTexDecode and draw via gx_draw.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- big-endian readers over GameCube data --------------------------------
static inline unsigned asset_be16(const uint8_t* p){ return ((unsigned)p[0]<<8)|p[1]; }
static inline int      asset_s16be(const uint8_t* p){ int v=((int)p[0]<<8)|p[1]; return v>=0x8000?v-0x10000:v; }
static inline unsigned asset_be32(const uint8_t* p){ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static inline float    asset_f32be(const uint8_t* p){ union{unsigned u; float f;} x; x.u=asset_be32(p); return x.f; }

// --- ZLB compressed blocks -------------------------------------------------
// Offset of the "ZLB\0" tag within the first `maxScan` bytes of p, or -1.
int    asset_findZLB(const uint8_t* p, int maxScan);
// Inflate a ZLB wrapper (16-byte header at `zlb`, deflate payload at +0x10) into
// `out` (cap bytes). `avail` = bytes available from `zlb`. Returns decoded size or 0.
size_t asset_inflateZLB(const uint8_t* zlb, int avail, uint8_t* out, size_t cap);

// --- textures (TEX0/TEX1 records) ------------------------------------------
// Load a texture record by id from a TEX{0,1}.tab/.bin pair into `out`, which then
// holds the standard texture header (width@0xA, height@0xC, format@0x16) followed by
// the image at out+*imgOff. Handles both wrappers at the tab offset:
//   "ZLB" -> inflate;   "DIR" -> stored raw, texture begins at block+0x20.
// Returns 1 on success. Decode with gxTexDecode(fmt,w,h, out+*imgOff).
int asset_loadTexRecord(const uint8_t* tab, int tabSize, const uint8_t* bin, int binSize, int id,
                        uint8_t* out, size_t cap, int* w, int* h, int* fmt, unsigned* imgOff);

// --- map blocks (MapBlockData) ---------------------------------------------
typedef struct AssetMapBlock {
    uint8_t* data;    // OWNED decompressed block (free with asset_freeMapBlock)
    size_t   size;
    int  id, vertCount, colorCount, texCoordCount, nPolys, dlCount, texCount, shCount, yOff, fmt;
    int  posSz, clrSz, texSz;                 // per-attr index sizes (1=INDEX8, 2=INDEX16)
    unsigned vertOff, colOff, texOff, polyOff, dlBase, texArrOff, shOff;
} AssetMapBlock;

// Grid-tab entry -> block offset in the .zlb.bin (low 24 bits of the entry; the high
// bits are buffer-slot flags). Offset 0 is VALID (the first block). Returns the
// sentinel ASSET_NO_BLOCK for an empty slot (tab entry == 0).
#define ASSET_NO_BLOCK 0xFFFFFFFFu
unsigned asset_mapBlockOffset(const uint8_t* tab, int tabSize, int blockId);
// Inflate + parse the block at tab[blockId]; on success mallocs blk->data and fills
// the descriptor (offsets are relative to blk->data). Returns 1, or 0 if empty/bad.
int  asset_loadMapBlock(const uint8_t* bin, int binSize, const uint8_t* tab, int tabSize,
                        int blockId, AssetMapBlock* blk);
void asset_freeMapBlock(AssetMapBlock* blk);

// POS fixed-point fraction per VTXFMT (2^-frac scale). Index by AssetMapBlock.fmt.
extern const int ASSET_POS_FRAC[8];

#ifdef __cplusplus
}
#endif
