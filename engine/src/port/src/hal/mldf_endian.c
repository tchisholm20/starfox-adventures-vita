// mldf_endian.c -- byte order for the game's numbered resident data files (MLDF ids),
// applied at the GAME's own load funnels (pi_dolphin.c, Vita/non-Windows builds only).
//
// Why here and not in the bridge: upstream's engine links 2 game TUs and swaps inside its own
// stand-in loaders (bridge/game_assetfile.c). The Vita build links the real game, whose
// loaders read big-endian disc data raw -- on the GameCube there was nothing to swap. Every
// resident file reaches the game through three funnels, all in pi_dolphin.c:
//   fileLoad / initLoadFileReadCb   a WHOLE buffer, kept resident in gResourceFileBuffers[id]
//   fileLoadToBuffer                a whole-file copy (from the resident buffer, or the disc)
//   fileLoadToBufferOffset          a slice copy (also getTabEntry), resident or disc
// so one table keyed by file id decides each file's byte order, once:
//   ARRAY16/ARRAY32  a uniform array: the resident buffer is swapped once when filled; a copy
//                    taken from it is already host order; a copy read from the DISC is swapped
//                    by the same width. Width from the CONSUMER'S declared type (docs/47 run 10).
//   RECORD           fixed-size records over the WHOLE file: same as ARRAY, per-record swapper
//                    (BITTABLE is consumed whole via loadAssetFileById, so resident must swap).
//   OBJDEF           variable-size defs located by OBJECTS.tab: resident stays RAW (never
//                    consumed whole), EVERY slice swapped (upstream's bswapObjDef).
//   RAW              no rule yet. The FIRST use of each such id is logged: an unknown format is
//                    a visible line in the log, never a silent pass.
#include "port/byteswap.h"
#include "main/gameloop_internal.h"   // GameBitDef -- the consumer's own record type
#include "mldf_swap_gen.h"             // DWARF-derived swappers (host/engine/gen_swappers.py)

#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern void bswapObjDef(void* p);     // bridge/game_objdef_swap.c
extern void bswapObjDefN(void* p, int len, int defNo);   // same, bounded by the slice length; defNo = OBJECTS.bin byte offset (for messages)

enum { RAW = 0, ARRAY16, ARRAY32, RECORD, OBJDEF, MAPSBIN, HITSREC };
typedef struct { uint8_t mode; uint8_t stride; void (*swap)(void*); const char* why; } MldfRule;

static void swapGameBitDef(void* p) { GameBitDef* d = (GameBitDef*)p; BE16(d->firstBit); }
// ObjSeqCastEntry (dlls/engine/2/2.c, its only consumer): s32 targetObjId, u16 flags, u16 objId.
// Raw, the title sequence read flags/objId byte-reversed and asked a NULL player for its health
// (Vita run 40: ObjSeq_start -> playerStatusIsPositive(NULL)).
static void swapObjSeqCast(void* p) { unsigned char* b = (unsigned char*)p; beFix32(b); beFix16(b + 4); beFix16(b + 6); }

// MapHitLine (main/map_block.h), 0x14 bytes: s16 x[2], y[2], z[2] @0..0xA, u8 endpointData[2] @0xC,
// u8 flags @0xE, s8 kind @0xF, s16 param @0x10, pad @0x12. HITS.bin holds NO header: each map block owns
// a slice [HITS.tab[blockId], HITS.tab[blockId+1]) of ceil32(n*0x14) bytes, so the records start at the
// slice start and the tail is padding. The game reads the slice natively (MapBlock_initHits, which also
// range-tests x/z against 0..0x280 and DISABLES a line outside it -- raw, 99% of lines failed that test).
// On the Rev 1 disc 19,344 of 19,348 records have x/z in [0,640] read big-endian, 286 read raw (2026-10-11).
static void swapMapHitLine(void* p) { unsigned char* b = (unsigned char*)p; for (int k = 0; k < 6; ++k) beFix16(b + k * 2); beFix16(b + 0x10); }

#define A16(why)  { ARRAY16, 2, 0, why }
#define A32(why)  { ARRAY32, 4, 0, why }
static const MldfRule gRules[0x5a] = {
    [0x17] = A32("TABLES.tab: int* gObjTablesBinIndex"),
    [0x16] = A32("TABLES.bin: s32 texture-id lists, -1 terminated (294.c triggerTexture*, texscroll2.c); rest of file unread"),
    [0x34] = A16("WEAPONDA.bin: s16[6] StaffWeaponSample (226.c:517)"),
    [0x40] = A16("OBJEVENT.bin: s16 packed events (objanim.c:193; object.c ObjAnim_LoadMoveEvents)"),
    [0x41] = A32("OBJHITS.bin: u32 hit-mask words (model.c:1692; objhits.c ObjHitReact_LoadMoveEntries)"),
    [0x19] = A32("SCREENS.tab: int* asset, -1 terminated (17.c screens_show)"),
    [0x1e] = A32("MAPS.tab: *(int*)(gMapsTab + off) (shader.c)"),
    [0x22] = A16("TEXTABLE.bin: u16* gRcpTexIdRemap"),
    [0x27] = A16("TRKBLK.tab: u16* gTrkBlkTab"),
    [0x29] = A32("HITS.tab: (int*)gHitsTab (tex_dolphin.c)"),
    [0x28] = { HITSREC, 0x14, swapMapHitLine, "HITS.bin: MapHitLine slices located by HITS.tab (MapBlock_initHits)" },
    [0x37] = { HITSREC, 0x14, swapMapHitLine, "MODLINES.bin: per-object MapHitLine slices located by MODLINES.tab (loadModLines; intersectModLineBuild)" },
    [0x2c] = A16("MODELIND.bin: s16 slices idc*2 (model.c)"),
    [0x2d] = A16("MODANIM.TAB: *(s16*)tbl, slices id<<1 (model.c)"),
    [0x2e] = A16("MODANIM.BIN: per-model s16 anim-id lists, -1 terminated: *(s16*)animationHeaderBuffer, s16* gModelResourceBuffer (model.c; Vita run 15)"),
    [0x31] = A32("AMAP.TAB: int* gModelAnimOffsetTable"),
    [0x33] = { RECORD, sizeof(GameBitDef), swapGameBitDef, "BITTABLE.bin: GameBitDef* gGameBitTable" },
    [0x15] = { RECORD, STFX_SIZEOF_GlobalMapEntry, stfx_swap_GlobalMapEntry, "GLOBALMA.bin: GlobalMapEntry* data (initMaps; run 11)" },
    [0x1c] = { RECORD, STFX_SIZEOF_WarpDestination, stfx_swap_WarpDestination, "WARPTAB.bin: WarpDestination, getTabEntry idx<<4" },
    [0x1f] = { RECORD, STFX_SIZEOF_MapInfoRecord, stfx_swap_MapInfoRecord, "MAPINFO.bin: MapInfoRecord, getTabEntry mapId<<5" },
    [0x35] = A32("VOXOBJ.tab: int* gVoxMapsMapList"),
    [0x38] = A32("MODLINES.tab: int hdr[] pairs (object.c)"),
    [0x3b] = { RECORD, 8, swapObjSeqCast, "OBJSEQ.bin: ObjSeqCastEntry[], getTabEntry first*8 (2.c ObjSeq_start)" },
    [0x3c] = A16("OBJSEQ.tab: *(s16*)hdr, slices seqIdx*2 (2.c)"),
    [0x0f] = A16("OBJSEQ2C.tab: s16 baseAnimId, getTabEntry idx*2 (2.c objLoadAnimdata; run 81)"),
    [0x3d] = A32("OBJECTS.tab: int* gObjFileOffsetTable"),
    [0x3e] = { OBJDEF, 0, 0, "OBJECTS.bin: one ObjDef per slice (bswapObjDef)" },
    [0x3f] = A16("OBJINDEX.bin: s16* gObjSeqToObjIdTable"),
    [0x1d] = { MAPSBIN, 0, 0, "MAPS.bin: per-map sections located by MAPS.tab (swapMapsSection)" },
    [0x50] = A32("TEXPRE.tab: ((int*)tabPtr)[i] (pi_dolphin.c), int* bankEntry (texture.c)"),
    [0x52] = A32("PREANIM.tab: ((u32*)slotScratch)[i] (pi_dolphin.c)"),
    // Map-owned tabs, slot A and slot B: int* in mergeTableFiles (p1/p2) and getCurrentDataFile
    // consumers (loadTextureBank: int* p, -1 terminated); MldfTables sizes each merge buffer
    // as entries * 4.
    [0x0e] = A32("ANIMCURV.tab A"), [0x56] = A32("ANIMCURV.tab B"),
    [0x1a] = A32("VOXMAP.tab A"),   [0x53] = A32("VOXMAP.tab B"),
    [0x21] = A32("TEX1.tab A"),     [0x4c] = A32("TEX1.tab B"),
    [0x24] = A32("TEX0.tab A"),     [0x4e] = A32("TEX0.tab B"),
    [0x26] = A32("BLOCKS.tab A"),   [0x48] = A32("BLOCKS.tab B"),
    [0x2a] = A32("MODELS.tab A"),   [0x45] = A32("MODELS.tab B"),
    [0x2f] = A32("ANIM.TAB A"),     [0x49] = A32("ANIM.TAB B"),
};

static uint8_t gRawLogged[0x5a];
static void noteRaw(int id, const char* how, int size) {
    if (id < 0 || id >= 0x5a || gRawLogged[id]) return;
    gRawLogged[id] = 1;
    fprintf(stderr, "[endian] id 0x%02x %s: %d bytes with NO byte-order rule (raw big-endian)\n", id, how, size);
}

// ---- MAPS.bin (0x1d) --------------------------------------------------------------------
// Per map k, MAPS.tab (0x1e, swapped A32) gives 8 offsets w0..w7 into MAPS.bin. The model,
// VALIDATED on the Rev 1 disc for all 117 maps (docs/47 run 12):
//   [w0,w1) page header, 0x38 bytes (or 0: maps 104/114 have none). Fields the game READS from
//           disc: s16 sizeX,sizeZ,originX,originZ @0..6, u16 objectDataSize @8 (MapRomListPage),
//           s16 @0x1c/0x1e (mapsBinGetRomlistSize). The pointer slots, worldX/Z and mapLayer are
//           overwritten at runtime (shader.c 3042-3099) -- left as they are.
//   [w1,w2) cells, u32 (w2-w1 == sizeX*sizeZ*4 on 115/115 maps with a header)
//   [w2,w6) cellRects, visCellRects, layerRects, visLayerRects: u32 (`v >> 28`, shader.c 1983)
//   [w6,w7) PackHeader {u32 magic, int decompressedSize, auxSize, compressedSize} (117/117 carry
//           0xFACEFEED or 0xE0E0E0E0); bytes after its 16 stay raw (auxSize: meaning not known).
int gStfxMapsSectionsSwapped, gStfxMapsAuxBytesRaw;
static void swapMapsSection(unsigned char* sec, const int* w, int avail) {
    // sec points at w[0]'s bytes; avail = bytes present from there (clips every sub-section)
    #define IN(o, n) ((o) >= 0 && (o) + (n) <= avail)
    if (w[1] - w[0] >= 0x20) {
        static const int s16at[] = { 0, 2, 4, 6, 8, 0x1c, 0x1e };
        for (unsigned k = 0; k < sizeof s16at / sizeof s16at[0]; ++k)
            if (IN(s16at[k], 2)) beFix16(sec + s16at[k]);
    }
    for (int o = w[1] - w[0]; o < w[6] - w[0]; o += 4) if (IN(o, 4)) beFix32(sec + o);
    if (w[7] - w[6] >= 16)
        for (int o = w[6] - w[0]; o < w[6] - w[0] + 16; o += 4) if (IN(o, 4)) beFix32(sec + o);
    if (w[7] - w[6] > 16) gStfxMapsAuxBytesRaw += w[7] - w[6] - 16;
    gStfxMapsSectionsSwapped++;
    #undef IN
}
static unsigned char* gMapsBinPending; static int gMapsBinPendingSize;
static void swapMapsBinWhole(unsigned char* bin, int size, const int* tab, int tabSize) {
    int maps = (tabSize / 4 - 1) / 7;
    for (int k = 0; k < maps; ++k) {
        const int* w = tab + k * 7;
        int okOrder = 1;
        for (int i = 0; i < 7; ++i) if (w[i] > w[i + 1]) okOrder = 0;
        if (!okOrder || w[7] > size) {
            fprintf(stderr, "[endian] MAPS.bin map %d: MAPS.tab offsets out of order/range -- section left raw\n", k);
            continue;
        }
        swapMapsSection(bin + w[0], w, size - w[0]);
    }
    fprintf(stderr, "[endian] MAPS.bin: %d map sections swapped (%d aux bytes left raw)\n",
            gStfxMapsSectionsSwapped, gStfxMapsAuxBytesRaw);
}

// Counters, readable from a debugger or a final report.
int gStfxMldfSwappedResident, gStfxMldfSwappedSlices, gStfxMldfMisaligned;

void stfx_mldf_resident(int id, void* buf, int size) {
    if (id < 0 || id >= 0x5a || !buf || size <= 0) return;
    const MldfRule* r = &gRules[id];
    if (r->mode == ARRAY16) beFixArray16(buf, (size_t)size / 2);
    else if (r->mode == ARRAY32) beFixArray32(buf, (size_t)size / 4);
    else if (r->mode == RECORD) {
        if (size % r->stride) { gStfxMldfMisaligned++;
            fprintf(stderr, "[endian] id 0x%02x resident size %d not a multiple of %d\n", id, size, r->stride); }
        beFixRecords(buf, (size_t)size / r->stride, r->stride, r->swap);
    }
    else if (r->mode == MAPSBIN) {
        extern u32 gResourceFileBuffers[]; extern u32 gResourceFileSizes[];
        if (gResourceFileBuffers[0x1e]) swapMapsBinWhole(buf, size, (int*)gResourceFileBuffers[0x1e], gResourceFileSizes[0x1e]);
        else { gMapsBinPending = buf; gMapsBinPendingSize = size; return; }   // MAPS.tab not yet here
    }
    else { if (r->mode == RAW) noteRaw(id, "resident", size); return; }
    gStfxMldfSwappedResident++;
    if (id == 0x1e && gMapsBinPending) {             // MAPS.bin arrived first: do it now
        swapMapsBinWhole(gMapsBinPending, gMapsBinPendingSize, (int*)buf, size);
        gMapsBinPending = 0; gStfxMldfSwappedResident++;
    }
}

void stfx_mldf_slice(int id, void* dst, int offset, int size, int fromResident) {
    if (id < 0 || id >= 0x5a || !dst || size <= 0) return;
    const MldfRule* r = &gRules[id];
    switch (r->mode) {
    case ARRAY16: case ARRAY32:
        if (fromResident) return;                         // resident copy is host order already
        if (offset % r->stride || size % r->stride) {
            gStfxMldfMisaligned++;
            fprintf(stderr, "[endian] id 0x%02x slice off=%d size=%d not %d-aligned\n", id, offset, size, r->stride);
        }
        if (r->mode == ARRAY16) beFixArray16(dst, (size_t)size / 2); else beFixArray32(dst, (size_t)size / 4);
        break;
    case RECORD:
        if (fromResident) return;
        if (offset % r->stride || size % r->stride) {
            gStfxMldfMisaligned++;
            fprintf(stderr, "[endian] id 0x%02x slice off=%d size=%d not %d-record aligned\n", id, offset, size, r->stride);
        }
        beFixRecords(dst, (size_t)size / r->stride, r->stride, r->swap);
        break;
    case OBJDEF:
        bswapObjDefN(dst, size, offset);
        break;
    case HITSREC:   // resident stays RAW (never consumed whole); EVERY slice is swapped; 32-byte tail padding is skipped
        beFixRecords(dst, (size_t)size / r->stride, r->stride, r->swap);
        break;
    case MAPSBIN: {
        if (fromResident) return;                          // resident MAPS.bin is host order
        extern u32 gResourceFileBuffers[]; extern u32 gResourceFileSizes[];
        const int* tab = (const int*)gResourceFileBuffers[0x1e];
        int maps = tab ? ((int)gResourceFileSizes[0x1e] / 4 - 1) / 7 : 0, k;
        for (k = 0; k < maps && tab[k * 7] != offset; ++k) {}
        if (k == maps) {
            gStfxMldfMisaligned++;
            fprintf(stderr, "[endian] MAPS.bin disc slice off=%d matches no map section -- left raw\n", offset);
            return;
        }
        swapMapsSection((unsigned char*)dst, tab + k * 7, size);
        break;
    }
    default:
        noteRaw(id, fromResident ? "slice(resident)" : "slice(disc)", size);
        return;
    }
    gStfxMldfSwappedSlices++;
}

// A whole-file copy is a slice at offset 0.
void stfx_mldf_copy(int id, void* dst, int size, int fromResident) {
    stfx_mldf_slice(id, dst, 0, size, fromResident);
}

// Called by the HAL's DVDRead for every read at offset 0. The game assigns
// gResourceFileBuffers[id] BEFORE it reads (fileLoad, initLoadFiles, mapLoadDataFile), so a read
// INTO a resident slot's buffer is that file arriving whole -- sync or async alike.
extern u32 gResourceFileBuffers[];
extern u32 gResourceFileSizes[];
void stfx_mldf_on_read(void* buf, int size) {
    for (int id = 0; id < 0x58; ++id) {
        if ((void*)gResourceFileBuffers[id] != buf) continue;
        int n = (int)gResourceFileSizes[id];
        if (n <= 0 || n > size) n = size;
        stfx_mldf_resident(id, buf, n);
        return;
    }
}

// ---- decompressed model files (MODELS.bin entries) ---------------------------------------
// Called by ObjModel_LoadModelData right after decompression, BEFORE its native header writes.
// Upstream's swappers (bridge/game_model_swap.c) name every field through the game's own
// ModelFileHeader/Shader structs. Vertex/normal/colour/texcoord arrays and the display lists stay
// big-endian: the GX layer (hal/gx_draw) decodes them as the GameCube's GPU would.
extern void bswapModelFileHeader(void* p);
extern void bswapModelRenderOps(void* m);
int gStfxModelsSwapped;
void stfx_model_file_loaded(void* model, int size) {
    if (!model || size < 0x100) {
        fprintf(stderr, "[endian] model file %p size %d: too small to be a ModelFileHeader -- left raw\n", model, size);
        return;
    }
    bswapModelFileHeader(model);
    bswapModelRenderOps(model);
    gStfxModelsSwapped++;
}

// ---- decompressed map blocks (BLOCKS.bin entries) ----------------------------------------
// Called once per freshly decompressed block (shader.c, after MapBlock_loadFromFile, BEFORE
// MapBlock_init relocates its offsets). Header and the leaf tables the CPU reads natively are put
// in host order; every swapper is generated from DWARF (gen_swappers.py). The GX vertex arrays
// (vertices/vertexColors/vertexTexCoords) and the display lists stay big-endian for the GX layer.
// ⚠ OPEN (docs/47 run 16): a few CPU readers take the vertex arrays natively too (model.c:2234,
// GroundAnima.c:27) -- a dual view to settle before those paths run. GroundAnima SETTLED 2026-10-14 (run 276): its accessors
// (shader.c trackUnpackVector / trackPackVector) read and write BE under STAIRFAX_PORT. model.c:2234 is still open. XYZAnimator SETTLED 2026-10-18 (its own BE accessors, build_xyz_anim_test.sh).
#include "main/map_block.h"
int gStfxBlocksSwapped;
void stfx_mapblock_loaded(void* p) {
    unsigned char* b = (unsigned char*)p;
    MapBlockData* blk = (MapBlockData*)p;
    stfx_swap_MapBlockData(p);
    #define LEAF(field, count, T) do { u32 off = (u32)(uintptr_t)blk->field; \
        if (off) for (int i_ = 0; i_ < (int)(count); ++i_) stfx_swap_##T(b + off + i_ * STFX_SIZEOF_##T); } while (0)
    LEAF(displayLists,  blk->displayListCount, MapBlockBoundsRec);
    LEAF(shaders,       blk->shaderCount,      Shader);
    LEAF(gcPolygons,    blk->nPolygons,        MapTriIndex);
    /* polyGroupCount + 1: the table ends with a TERMINATOR group whose firstTri == nPolygons closes the last
       range (group[1].firstTri in trackBuildBlockTriangles / mapBlockCountTrianglesByType). Verified on the
       disc: 10 of 10 sampled blocks (shipbattle/mod14, linka/mod65). Left raw, the last group's end read
       byte-reversed (106 -> 27136) and its triangle loop ran off the end of the polygon array. */
    LEAF(polygonGroups, blk->polyGroupCount ? blk->polyGroupCount + 1 : 0, MapTriGroup);
    LEAF(textures,      blk->textureCount,     MapTextureRef);
    #undef LEAF
    gStfxBlocksSwapped++;
}

// ---- inflated romlists (object placements), piRomLoadSection -----------------------------
// ObjPlacement (game/objects/object_setup.h): objectId s16 @0, size u8 @2 (record length / 4),
// map-act/load bytes @3..7, pos f32 @8/@0xC/@0x10, ident s32 @0x14, then per-object-type setup data.
// The header is swapped by hand (its unions mix byte and s16 views, which gen_swappers.py
// refuses); the byte view of @2..7 is the one the loaders use, as upstream's romlist spawner
// chose. ⚠ The per-object-type data after @0x18 is still RAW: each object DLL reads it with its
// own struct -- the next endian class (docs/47 run 39).
int gStfxRomlistRecords, gStfxRomlistsSwapped;
static void stfx_romlist_load_swaps(unsigned char* b, unsigned char* e);
void stfx_romlist_loaded(void* buf, int size) {
    unsigned char* p = (unsigned char*)buf; unsigned char* e = p + size; int n = 0;
    if (!buf || size <= 0) return;
    while (p + 0x18 <= e) {
        unsigned rec = p[2] * 4u;
        if (rec < 0x18 || p + rec > e) break;
        beFix16(p + 0); beFix32(p + 0x08); beFix32(p + 0x0C); beFix32(p + 0x10); beFix32(p + 0x14);
        p += rec; ++n;
    }
    gStfxRomlistRecords += n; gStfxRomlistsSwapped++;
    stfx_romlist_load_swaps((unsigned char*)buf, p);
    fprintf(stderr, "[endian] romlist: %d placements swapped (%d of %d bytes walked)\n", n, (int)(p - (unsigned char*)buf), size);
}

// ---- per-object-type placement data (past the 0x18 header), at the object's init --------
// Which struct a record's tail is depends on the object DLL that inits it, which the romlist
// loader does not know; so the tail is swapped lazily, once, just before that init
// (object.c Obj_RunInitCallback), by the DWARF-generated table in placement_swap_gen.c.
// Only records the loader registered as PENDING (see the pending set below) are swapped: code-built placements
// (Obj_AllocObjectSetup, copies, stack structs) are host order and never are. A record is swapped once per load.
typedef struct { void* init; void (*swap)(unsigned char*); unsigned short size; } StfxPlacementSwap;
extern const StfxPlacementSwap gStfxPlacementSwaps[]; extern const int gStfxPlacementSwapCount;
#define STFX_PL_PEND   32768   /* power of two; 20,566 placements on the whole disc, a few thousand resident */
static void* sPlUnknown[256]; static int sPlUnknownN;
int gStfxPlacementsSwapped, gStfxPlacementsUnknown, gStfxPlacementsShort, gStfxPlMarksFull, gStfxPlacementsClipped, gStfxPlacementsStale;

// Hand-written entries, consulted when the DWARF-generated table has none. gen_placement_swappers.py emitted nothing for
// these inits (their placement structs carry byte arrays / are shared between two DLLs), yet each reads multi-byte tail
// fields natively -- raw, Trigger_init armed or disabled every trigger from a byte-reversed game bit (docs/47 run 218:
// 14 TrigPln on the galleon, objectId 0x4c, 80-byte records). Layouts: include/dlls/objects/294.h (TriggerPlacement,
// asserted there), include/dlls/objects/texscroll_types.h (TexScrollPlacement, 0x24 bytes, "all 226 retail EN placements").
extern void Trigger_init(), TexScroll2_init(), TexScroll_init();
static void plTrigger(unsigned char* b)    { beFix16(b + 0x38); beFix16(b + 0x44); beFix16(b + 0x46);
                                             beFix16(b + 0x48); beFix16(b + 0x4a); beFix16(b + 0x4c); beFix16(b + 0x4e); }
static void plTexScroll(unsigned char* b)  { beFix16(b + 0x18); beFix16(b + 0x1a); }
/* Records the ENGINE reads with no object init in the way. shader.c (the map-section loader) registers every romlist
   record whose objectId is 110 with RomCurve_add and every one with objectId 5 with the Checkpoint interface's
   addRouteEntry as the page loads; the path / route code then reads their tails natively, whether or not the
   curve or checkpoint OBJECT ever exists. So these two are keyed by objectId and swapped at load, not by init.
   Layouts (DWARF; placement_swap_test.py re-derives and compares them): include/main/dll/rom_curve_def.h RomCurveDef
   (68 bytes, asserted) -- linkIds[4] @0x1c.., then by curve type: requiredBit @0x30 / forbiddenBit @0x32 (type 0x24), roll @0x38 (8, 0x1a); and
   include/main/checkpoint_route.h CheckpointRouteEntry (64 bytes) -- four s32 links @0x18.. . The disc's curve
   records are 52..68 bytes: a field past the record's end is not swapped. curve_init / checkpoint4_init are in the
   init table too (a curve object may spawn), where the marks below make them a no-op. NB checkpoint4_init's own
   view (Checkpoint4Placement) pads the links as bytes, so the generated table deliberately has no entry for it. */
extern void curve_init(), checkpoint4_init();
static void plRomCurveRec(unsigned char* b, unsigned rec) {
    int i; unsigned ty = b[0x19];                            /* RomCurveDef.type */
    for (i = 0; i < 4; ++i) if (0x1c + (unsigned)i * 4 + 4 <= rec) beFix32(b + 0x1c + i * 4);
    /* The tail past the links is TYPE-dependent. requiredBit / forbiddenBit are game bits only for Tricky route nodes
       (type 0x24: Objfsa_FindNearestEnabledCurveType24, tricky.c); for the other types the same bytes are byte pairs
       (type 0x26 holds 0x0403, 0x1c05 -- swapped as s16 they would be wrecked), and roll is read for types 8 / 0x1a only
       (curve_init). Disc check: placement_swap_test.py. */
    if (ty == 0x24) { if (rec >= 0x32) beFix16(b + 0x30); if (rec >= 0x34) beFix16(b + 0x32); }
    if ((ty == 0x08 || ty == 0x1a) && rec >= 0x3a) beFix16(b + 0x38);
    /* STFX cutscene-camera: the path-camera mode (dlls/engine/71, pathcam_buildWindowSamples) reads RomCurvePathNode
       sampleA / sampleB / sampleC (s16 @0x34 / 0x36 / 0x38 = camera pitch / yaw / roll) from the MOVE nodes (types 8 and
       0x1a). Only @0x38 was swapped (curve_init reads it as `roll`, and placement_swap_test.py derives its list from
       curve_init, so it cannot see this engine-side reader): a cutscene camera riding a path had a right position and a
       byte-reversed orientation. sampleD (s8 @0x3a) and cameraFlags (u8 @0x3b) are bytes. */
    if ((ty == 0x08 || ty == 0x1a) && rec >= 0x36) beFix16(b + 0x34);
    if ((ty == 0x08 || ty == 0x1a) && rec >= 0x38) beFix16(b + 0x36);
}
static void plRomCurve(unsigned char* b)   { plRomCurveRec(b, 0x44); }
static void plCheckpoint(unsigned char* b) { beFix32(b + 0x18); beFix32(b + 0x1c); beFix32(b + 0x20); beFix32(b + 0x24); }
#define STFX_OBJID_ROMCURVE    110
#define STFX_OBJID_CHECKPOINT  5
/* objlib.c ObjHitRegion_FindContainingId walks every loaded romlist page for objectId 0x130 records and reads
   ObjHitRegionPlacement natively (id / halfX / halfY / halfZ, four u16 @0x18..0x1e; yaw, pitch are bytes) with no object
   init in the way -- the same class as the curves. Rev 1's disc has NO record of that id (placement_swap_test.py counts
   them and the harness exercises a synthetic one), so this is for completeness: it can not be seen on this disc. */
#define STFX_OBJID_HITREGION   0x130
static void plHitRegion(unsigned char* b) { beFix16(b + 0x18); beFix16(b + 0x1a); beFix16(b + 0x1c); beFix16(b + 0x1e); }
int gStfxEngineReadSwapped;
static const StfxPlacementSwap sStfxExtraSwaps[] = {
    { (void*)Trigger_init,     plTrigger,    0x50 },
    { (void*)TexScroll2_init,  plTexScroll,  0x24 },
    { (void*)TexScroll_init,   plTexScroll,  0x24 },
    { (void*)curve_init,       plRomCurve,   0x44 },
    { (void*)checkpoint4_init, plCheckpoint, 0x40 },
};

/* PENDING SET: the only records the init-time hook may swap are ones this loader put here -- a record start inside a
   romlist page it just header-swapped, with a fingerprint (objectId, size byte, ident) taken at that moment.
   Not an address range: romlist pages are mmAlloc'd and FREED on a map change, and the allocator then hands the same
   memory to Obj_AllocObjectSetup (crates, explosions, triggers built at runtime, native byte order, size byte in BYTES).
   A range test swapped those; a mark test could not tell them apart either. A runtime record is not pending, or if it
   sits on a stale pending address its fingerprint does not match (ident is -1 in Obj_AllocObjectSetup records), so it is
   left alone. An entry leaves the set when its record is swapped (once per load: respawns re-init from the same bytes)
   or when a new load overlaps it (map reload over the same memory). */
#define STFX_PEND_TOMB ((void*)1)
static void* sPend[STFX_PL_PEND]; static unsigned sPendSig[STFX_PL_PEND];
static unsigned plHash(void* p) { return ((unsigned)(uintptr_t)p >> 2) * 2654435761u & (STFX_PL_PEND - 1); }
static unsigned plSig(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[0x14] << 24 ^ (unsigned)p[0x15] * 0x9e3779b1u ^ (unsigned)p[0x16] * 0x85ebca6bu ^ (unsigned)p[0x17] * 0xc2b2ae35u; }
static int plFind(void* p) {        /* slot of a pending record, or -1 */
    unsigned h, n = 0;
    for (h = plHash(p); sPend[h]; h = (h + 1) & (STFX_PL_PEND - 1)) {
        if (sPend[h] == p) return (int)h;
        if (++n >= STFX_PL_PEND) break;
    }
    return -1;
}
static void plAdd(void* p, unsigned sig) {
    unsigned h, n = 0;
    for (h = plHash(p); sPend[h] && sPend[h] != STFX_PEND_TOMB && sPend[h] != p; h = (h + 1) & (STFX_PL_PEND - 1))
        if (++n >= STFX_PL_PEND) { gStfxPlMarksFull++; return; }
    sPend[h] = p; sPendSig[h] = sig;
}
/* A new load over memory that held an old page: drop every pending record in [b,e), then rebuild without tombstones. */
static void plRetire(unsigned char* b, unsigned char* e) {
    static void* keepP[STFX_PL_PEND]; static unsigned keepS[STFX_PL_PEND]; int n = 0, i;
    for (i = 0; i < STFX_PL_PEND; ++i) {
        unsigned char* m = (unsigned char*)sPend[i];
        if (sPend[i] && sPend[i] != STFX_PEND_TOMB && !(m >= b && m < e)) { keepP[n] = sPend[i]; keepS[n++] = sPendSig[i]; }
        sPend[i] = 0;
    }
    for (i = 0; i < n; ++i) plAdd(keepP[i], keepS[i]);
}
/* After the page is header-swapped: swap + (for the engine-read kinds) finish the records described above, and make
   every other record pending for its object's init. */
static void stfx_romlist_load_swaps(unsigned char* b, unsigned char* e) {
    unsigned char* p = b; int nc = 0, nk = 0;
    plRetire(b, e);
    while (p + 0x18 <= e) {
        unsigned rec = p[2] * 4u; int id = *(short*)p;       /* objectId: already host order (stfx_romlist_loaded) */
        if (rec < 0x18 || p + rec > e) break;
        if (id == STFX_OBJID_ROMCURVE) { plRomCurveRec(p, rec); ++nc; }
        else if (id == STFX_OBJID_CHECKPOINT && rec >= 0x28) { plCheckpoint(p); ++nk; }
        else if (id == STFX_OBJID_HITREGION && rec >= 0x20) { plHitRegion(p); ++nk; }
        else plAdd(p, plSig(p));
        p += rec;
    }
    gStfxPlacementsSwapped += nc + nk; gStfxEngineReadSwapped += nc + nk;
    if (nc + nk) fprintf(stderr, "[endian] romlist: %d rom-curve + %d checkpoint-route tails swapped at load\n", nc, nk);
}
void stfx_placement_before_init(void* initFn, void* placement) {
    unsigned char* p = (unsigned char*)placement; int i, slot;
    if (!p || !initFn) return;
    slot = plFind(p);
    if (slot < 0) return;                   /* not a pending disc record: runtime-built, already swapped, or engine-read */
    if (sPendSig[slot] != plSig(p)) {       /* stale entry: that page was freed and the memory reused by a native record */
        sPend[slot] = STFX_PEND_TOMB; gStfxPlacementsStale++; return;
    }
    const StfxPlacementSwap* sw = 0;
    for (i = 0; i < gStfxPlacementSwapCount; ++i) if (gStfxPlacementSwaps[i].init == initFn) { sw = &gStfxPlacementSwaps[i]; break; }
    if (!sw) for (i = 0; i < (int)(sizeof sStfxExtraSwaps / sizeof sStfxExtraSwaps[0]); ++i)
        if (sStfxExtraSwaps[i].init == initFn) { sw = &sStfxExtraSwaps[i]; break; }
    if (!sw) {
        int k; gStfxPlacementsUnknown++;
        for (k = 0; k < sPlUnknownN; ++k) if (sPlUnknown[k] == initFn) break;
        if (k == sPlUnknownN && sPlUnknownN < 256) {
            sPlUnknown[sPlUnknownN++] = initFn;
            fprintf(stderr, "[endian] placement: no swapper for init %p (objectId 0x%04x, %u bytes) -- tail left RAW\n",
                    initFn, *(unsigned short*)p, p[2] * 4u);
        }
        return;   /* stays pending: nothing was done */
    }
    if (sw->size > p[2] * 4u) {
        /* The struct is longer than this record: swapping in place would swap the NEXT record's bytes. Run the
           swapper on a zero-padded SCRATCH copy and take back only the record's own bytes: every field wholly inside
           the record comes out right, and what lies past its end is dropped. (A field STRADDLING the end would be
           half-swapped; the decomp's structs are padded out to a size, not given fields across the real end.)
           docs/47 run 218: WarpPointPlacement is 40 bytes, the disc record 36, its one swapped field at 0x20 --
           before this the whole record was left raw ("struct 40 > record 36 -- NOT swapped"). */
        unsigned char scratch[512]; unsigned rec = p[2] * 4u;
        if (sw->size > sizeof scratch) {
            gStfxPlacementsShort++;
            fprintf(stderr, "[endian] placement: init %p struct %u > record %u bytes -- NOT swapped\n", initFn, sw->size, rec);
            return;
        }
        memset(scratch, 0, sizeof scratch); memcpy(scratch, p, rec);
        sw->swap(scratch); memcpy(p, scratch, rec);
        sPend[slot] = STFX_PEND_TOMB; gStfxPlacementsSwapped++; gStfxPlacementsClipped++;
        return;
    }
    sw->swap(p);
    sPend[slot] = STFX_PEND_TOMB; gStfxPlacementsSwapped++;
}

/* Objects whose descriptor has NO init slot (DLL 234 Sideload) never reach stfx_placement_before_init, yet read their
   tail in update. object.c's Obj_RunInitCallback offers such a record here, keyed by the descriptor's UPDATE function
   (the table lists those under the update symbol: placement_overrides.txt `noinit`, validated by the generator against
   modelEngine.c's descriptors). Nothing happens unless the table has that key, so the hundreds of init-less
   level-control / null-descriptor objects cost one table scan and add no "unknown init" noise; when it has it, the
   SAME pending-set / fingerprint / exactly-once path as an init-time swap decides whether the record is swapped. */
void stfx_placement_noinit(void* updateFn, void* placement) {
    int i;
    if (!placement || !updateFn) return;
    for (i = 0; i < gStfxPlacementSwapCount; ++i)
        if (gStfxPlacementSwaps[i].init == updateFn) { stfx_placement_before_init(updateFn, placement); return; }
}

// ---- animation moves (ANIM.BIN 0x30/0x4a, PREANIM.bin 0x51 entries) -----------------------
// One ObjAnimMoveData blob, freshly decompressed (model.c: loadAnimation, animLoadFromTable).
// The game reads two s16 natively: frameStreamOffset @2 (modelAnimUpdateChannels, the frame
// cursor) and rootCurveOffset @4, and the root curve behind it (f32 scale, s16 sampleCount, s16
// axis data to the end of the blob: objanim.c). Left raw, the CloudRunner's cursor sat 7,974
// bytes BEFORE its descriptors where retail's is 952 after them: 0x00DE read as 0xDE00 (docs/47
// run 127) -- every move that reads stream bits decoded some other memory, and a move of
// constants (Krystal's seat) came out right.
// NOT swapped: the u16 descriptors from @10 (the joint-matrix builder twin reads them big-endian,
// run 56) and the bit stream (read MSB-first). A blob is swapped once: each call site is a
// fresh decompression.
int gStfxAnimMovesSwapped, gStfxAnimMovesShort;
void stfx_anim_move_loaded(void* blob, int size) {
    unsigned char* a = (unsigned char*)blob;
    if (!a || size < 10) { gStfxAnimMovesShort++; return; }
    beFix16(a + 2); beFix16(a + 4);
    int streamOff = *(short*)(a + 2), rco = *(short*)(a + 4);
    if (rco != 0 && rco >= streamOff && rco + 6 <= size) {
        beFix32(a + rco); beFix16(a + rco + 4);
        beFixArray16(a + rco + 6, (size_t)(size - (rco + 6)) / 2);
    }
    if (gStfxAnimMovesSwapped < 4 || !(gStfxAnimMovesSwapped & 255))
        fprintf(stderr, "[endian] anim move %d: %d bytes, stream @%d, root curve @%d, joints %d, frames %d, stride %d\n",
                gStfxAnimMovesSwapped, size, streamOff, rco, a[6], a[7], a[8]);
    gStfxAnimMovesSwapped++;
}
