// game_romlist.c - read a map's object placements from the real romlist (MAPS.bin).
//
// The romlist for map N is a section of MAPS.bin located by MAPS.tab: 7 u32 words per
// map (word0 = section start, words 1..5 = cell/rect sub-sections, word6 = objects
// offset, and the NEXT map's word0 = this section's end). This mirrors the real
// mapGetRomListAndOffsets (shader.c): objects run [word6, word7) as a packed array of
// ObjPlacement records, each `size*4` bytes. Fields are big-endian, so we byte-swap on
// read (port/byteswap.h). This proves the romlist DATA pipeline; actual spawning
// (objSetupObject -> loadCharacter) is gated on the model subsystem (loadCharacter
// dereferences the model bank's file pointer, which needs a real ObjModel_Load).

#include "port/byteswap.h"
#include "port/stfx_inflate.h"
#include "port/dvd_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <windows.h>
#endif

// Spawn one placement, guarded: object types that hit a not-yet-ported subsystem (a
// stubbed helper returning NULL that loadCharacter writes through) fault - catch it and
// skip so the valid objects still spawn. Returns the created object, or NULL on skip.
extern void* objSetupObject(void* data, int flags, int mapLayer, int objIndex, void* parent);
static void* spawnGuarded(void* p, int mapId, int idx) {
#if defined(_MSC_VER)
    __try { return objSetupObject(p, 1, mapId, idx, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return (void*)-1; }  // -1 = faulted/skipped
#else
/* vita fork: SEH is MSVC-only. Elsewhere this runs UNGUARDED -- a fault here
 * crashes instead of being skipped, which is deliberate: it points at the
 * missing subsystem instead of hiding it. docs/47. */
    return objSetupObject(p, 1, mapId, idx, 0);
#endif
}

// Guarded full model load: ObjModel_Load runs modelLoadAnimations, which faults when the
// current area lacks the model's ANIM data. Catch it and return NULL so the caller falls back
// to the bind-pose-only loader. Used for spawned-object models (which want animations).
extern void* ObjModel_Load(int id, int loadFlag, int* outSize);
void* stairfax_objmodel_load_guarded(int fid) {
    int sz = 0;
#if defined(_MSC_VER)
    __try { return ObjModel_Load(-fid, 0, &sz); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return (void*)0; }
#else
    return ObjModel_Load(-fid, 0, &sz);   /* unguarded off MSVC: see spawnGuarded */
#endif
}

extern void loadAssetFileById(void* out, int fileId);
extern int  getDataFileSize(int fileId);

// Decompress a map's <name>.romlist.zlb into `out` using the FACEFEED PackHeader that
// lives in MAPS.bin at the map's objects offset. Returns decompressed size, 0 on failure.
// PackHeader (BE): magic@0, decompressedSize@4, auxSize@8, compressedSize@0xc.
static int romlistDecompress(const unsigned char* packHdr, const char* name,
                             unsigned char* out, size_t cap) {
    if (beRead32(packHdr) != 0xFACEFEEDu) return 0;
    unsigned decompSize = beRead32(packHdr + 4);
    unsigned compSize   = beRead32(packHdr + 0xc);
    char path[128];
    snprintf(path, sizeof path, "%s.romlist.zlb", name);
    int fsz = 0;
    unsigned char* file = (unsigned char*)loadFileByPath(path, &fsz, 0);
    if (!file) { fprintf(stderr, "[romlist] %s not found\n", path); return 0; }
    if (decompSize > cap) return 0;
    size_t outLen = 0;
    // payload is a zlib stream at file+0x10, compSize bytes
    if (stfx_inflate_zlib(out, cap, file + 0x10, compSize, &outLen) != 0) {
        fprintf(stderr, "[romlist] inflate failed for %s (comp=0x%x)\n", path, compSize);
        return 0;
    }
    return (int)outLen;
}

#define MLDF_MAPS_TAB 0x1e
#define MLDF_MAPS_BIN 0x1d
#define WORDS_PER_MAP 7
#define OBJPLACEMENT_MIN 0x1C   // ident@0x14 + 4

// ObjPlacement (game/objects/object_setup.h): objectId s16@0, size u8@2, mapActLo u8@3,
// loadFlags u8@4, mapActHi u8@5, loadRange u8@6, unk07 u8@7, posX@8 posY@C posZ@10, ident@14.
// Real spawn: decompress a map's romlist, byte-swap each ObjPlacement, and hand it to the
// real objSetupObject (object.c) -> loadCharacter -> ObjModel_Load. Objects get created in
// gObjList. Requires OBJECTS.bin registered so loadObjectFile can read the def by id.
extern int   gObjCount;
extern void  dvd_register_buffer(int id, void* buf, int size);

int stairfax_romlist_spawn(int mapId, const char* mapName, int maxCount) {
    int32_t* tab = 0; unsigned char* bin = 0;
    loadAssetFileById(&tab, MLDF_MAPS_TAB); loadAssetFileById(&bin, MLDF_MAPS_BIN);
    if (!tab || !bin) { fprintf(stderr, "[spawn] MAPS not loaded\n"); return -1; }
    // Register OBJECTS.bin so the real loadObjectFile (fileLoadToBufferOffset) can read defs.
    { void* ob = 0; loadAssetFileById(&ob, 0x3e);
      if (ob) dvd_register_buffer(0x3e, ob, getDataFileSize(0x3e));
      else { fprintf(stderr, "[spawn] OBJECTS.bin not loaded\n"); return -1; } }

    int objOff = tab[mapId * WORDS_PER_MAP + 6];
    static unsigned char objbuf[0x20000];
    int dsz = romlistDecompress(bin + objOff, mapName, objbuf, sizeof objbuf);
    if (dsz <= 0) { fprintf(stderr, "[spawn] romlist decompress failed\n"); return -1; }

    int before = gObjCount, spawned = 0, tried = 0;
    unsigned char* p = objbuf; unsigned char* e = objbuf + dsz;
    for (int idx = 0; p + OBJPLACEMENT_MIN <= e && tried < maxCount; ++idx) {
        unsigned size = p[2]; if (size == 0) break;
        // ObjPlacement -> host order: objectId(u16)@0, posXYZ(f32)@8/0xC/0x10, ident(s32)@0x14
        beFix16(p + 0); beFix32(p + 0x08); beFix32(p + 0x0C); beFix32(p + 0x10); beFix32(p + 0x14);
        void* obj = spawnGuarded(p, mapId, idx);
        tried++;
        if (obj && obj != (void*)-1) spawned++;
        if (tried <= 12) fprintf(stderr, "[spawn]  placement %d id=%d -> %s\n", idx, *(short*)p,
                                 obj == (void*)-1 ? "SKIP(fault)" : (obj ? "OBJECT" : "null"));
        p += size * 4;
    }
    fprintf(stderr, "[spawn] map %d (%s): tried %d, spawned %d, gObjCount %d -> %d\n",
            mapId, mapName, tried, spawned, before, gObjCount);
    return spawned;
}

// Spawn the player character (Sabre seq 0x00 / Krystal seq 0x1F) the way the real
// mapSetupPlayer does: fabricate a CharSpawn (same byte layout as an ObjPlacement, but
// constructed in host order - no byte-swap) and hand it to the real loadCharacter with
// flags&1, which loads the real character model/anims AND appends the object to gObjList.
// Guarded: some character-DLL setup hooks are stubbed in the port, so catch any fault and
// return NULL rather than take down the frame. Returns the GameObject, or NULL.
extern void* loadCharacter(short* data, int flags, int a2, int a3, void* parent, int unused);
#if defined(_MSC_VER)
static DWORD gPlayerFaultCode; static void* gPlayerFaultAddr;
static int playerFaultFilter(EXCEPTION_POINTERS* ep) {
    gPlayerFaultCode = ep->ExceptionRecord->ExceptionCode;
    gPlayerFaultAddr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
void* stairfax_spawn_player(int seq, float x, float y, float z) {
    // Ensure OBJECTS.bin is registered so loadObjectFile can read the character def by id.
    { void* ob = 0; loadAssetFileById(&ob, 0x3e);
      if (ob) dvd_register_buffer(0x3e, ob, getDataFileSize(0x3e));
      else { fprintf(stderr, "[player] OBJECTS.bin not loaded\n"); return 0; } }

    unsigned char spawn[0x18];
    memset(spawn, 0, sizeof spawn);
    // CharSpawn: id s16@0, size u8@2=0x18, unk4@4=1, unk5@5=4, unk6@6=0xff, unk7@7=0xff,
    // x@8, y@0xC, z@0x10, mapId@0x14=-1 (mirrors object.c mapSetupPlayer).
    *(short*)(spawn + 0) = (short)seq;
    spawn[2] = 0x18; spawn[4] = 1; spawn[5] = 4; spawn[6] = 0xff; spawn[7] = 0xff;
    *(float*)(spawn + 8) = x; *(float*)(spawn + 0xC) = y; *(float*)(spawn + 0x10) = z;
    *(int*)(spawn + 0x14) = -1;

    extern void* Obj_GetActiveModel(void* obj);
    extern void  stairfax_bswap_model_moves(void* header);
    void* obj = 0;
#if defined(_MSC_VER)
    __try { obj = loadCharacter((short*)spawn, 1, -1, -1, 0, 0); }
    __except (playerFaultFilter(GetExceptionInformation())) {
        HMODULE hm = GetModuleHandleA(NULL);
        fprintf(stderr, "[player] loadCharacter FAULT code=0x%08lx addr=%p rva=0x%tx\n",
                gPlayerFaultCode, gPlayerFaultAddr, (char*)gPlayerFaultAddr - (char*)hm);
        obj = 0;
    }
#else
    obj = loadCharacter((short*)spawn, 1, -1, -1, 0, 0);   /* unguarded off MSVC: see spawnGuarded */
#endif
    // The move atlas loaded by ObjModel_Load is big-endian on disc; the real loaders don't swap it.
    // Swap it to host order so the anim eval (modelAnimEvalChannels -> modelAnimBuildJointMatrices)
    // reads valid offsets - same as the interim spawned-object path does.
    if (obj) {
        void* mdl = Obj_GetActiveModel(obj);
        void* file = mdl ? *(void**)mdl : 0;
        if (file) stairfax_bswap_model_moves(file);
    }
    fprintf(stderr, "[player] spawn seq=0x%x at (%.0f,%.0f,%.0f) -> %p\n", seq, x, y, z, obj);
    return obj;
}

// Render the player through the REAL model render path (objRenderModel -> modelDoRenderInstrs),
// with real GX skinning enabled. SEH-guarded: the real path pulls light/anim state that may not
// be fully set up yet, so catch a fault rather than take down the frame.
extern void objRenderModel(void* obj);
extern void gx_draw_setRealSkin(int on);
extern void gx_draw_setSourceBounds(const void* lo, const void* hi);
extern void gx_draw_setForceLayout(int stride, int posOff, int posSz);
extern int  Object_ObjAnimSetMove(void* objAnim, int move, float progress, unsigned char flags);
extern void* Obj_GetActiveModel(void* obj);
void stairfax_render_player(void* obj, int move, float progress) {
    gx_draw_setRealSkin(1);
    gx_draw_setForceLayout(0, 0, 0);   // clear any interim POS-only layout the object pass left set
    // Diagnostic: STAIRFAX_PLAYER_BINDPOSE sets ModelFileHeader.flags (@0x02) bit 1, which makes
    // modelDoRenderInstrs skip ObjModel_UpdateAnimMatrices - renders bind pose, proving the real
    // interpreter decodes the geometry without needing the (not-yet-wired) character anim data.
    int bindpose = getenv("STAIRFAX_PLAYER_BINDPOSE") != 0;
    // Bound the POS-array index reads to the model's own working vertex buffer so a bad
    // display-list index (or trailing junk misread as a draw) is dropped instead of faulting
    // on a wild read. The real render path (objprint) sets GX_VA_POS to ObjModel.vtxBuf[buf]
    // (0x1c[(bufferFlags>>1)&1]); file->vertexCount (@0xE4) * 6 bytes bounds it.
    // Which of the two double-buffered working vertex buffers the render binds to GX_VA_POS depends
    // on ObjModel.bufferFlags, which the render path itself toggles - so bound BOTH so the index
    // guard doesn't reject the live buffer (rejecting it left every POS at 0 -> mesh collapsed to
    // its bone origins, i.e. the spike).
    {
        unsigned char* am = (unsigned char*)Obj_GetActiveModel(obj);
        if (am) {
            // Re-arm the per-frame animation update. ObjModel_UpdateAnimMatrices (in the render) only
            // re-evaluates the skeleton when ObjModel.bufferFlags bit 3 (@0x18) is CLEAR, and sets it
            // after each update; the real Obj_UpdateAllObjects clears it every frame (object.c:2379).
            // The port drives playerUpdate directly and never runs that loop, so the flag stayed set
            // and the anim evaluated exactly ONCE (frozen bind/T-pose) even as currentMoveProgress
            // advanced. Clear it here so the pose tracks the move each frame.
            *(unsigned short*)(am + 0x18) &= (unsigned short)~0x8;
            unsigned char* v0 = *(unsigned char**)(am + 0x1c);       // vtxBuf[0]
            unsigned char* v1 = *(unsigned char**)(am + 0x20);       // vtxBuf[1]
            unsigned char* file = *(unsigned char**)(am + 0);
            if (file && (v0 || v1)) {
                unsigned vc = *(unsigned short*)(file + 0xE4);       // vertexCount
                unsigned char* lo = v0, * hi = v1;
                if (!lo || (v1 && v1 < v0)) { lo = v1; hi = v0; }
                gx_draw_setSourceBounds(lo, hi + (size_t)vc * 6);
            }
        }
    }
#if defined(_MSC_VER)
    __try
#endif
    {   /* unguarded off MSVC: see spawnGuarded */
        if (bindpose) {
            void* mdl = Obj_GetActiveModel(obj);
            void* file = mdl ? *(void**)mdl : 0;   // ObjModel.file @ offset 0
            if (file) *(unsigned short*)((char*)file + 2) |= 2;
        } else if (getenv("STAIRFAX_PLAYER_SETMOVE")) {
            // The player's ObjAnimComponent is the GameObject itself (anim at offset 0). Setting a
            // move here loads it into the ACTIVE anim channel, but the render evaluates the CURRENT
            // channel (animStateA); the port doesn't yet run the per-frame ObjAnim update that
            // promotes/blends active->current, so this alone doesn't pose the player (see notes).
            Object_ObjAnimSetMove(obj, move, progress, 0);
        }
        objRenderModel(obj);
    }
#if defined(_MSC_VER)
    __except (playerFaultFilter(GetExceptionInformation())) {
        HMODULE hm = GetModuleHandleA(NULL);
        fprintf(stderr, "[player] render FAULT code=0x%08lx addr=%p rva=0x%tx\n",
                gPlayerFaultCode, gPlayerFaultAddr, (char*)gPlayerFaultAddr - (char*)hm);
    }
#endif
    gx_draw_setRealSkin(0);
}

int stairfax_romlist_dump(int mapId, int maxLog) {
    const char* mapName = getenv("STAIRFAX_ROMLIST_NAME");
    if (!mapName) mapName = "frontend";
    int32_t* tab = 0;
    unsigned char* bin = 0;
    loadAssetFileById(&tab, MLDF_MAPS_TAB);   // u32-swapped by game_assetfile
    loadAssetFileById(&bin, MLDF_MAPS_BIN);
    if (!tab || !bin) { fprintf(stderr, "[romlist] MAPS.tab/bin not loaded\n"); return -1; }

    int tabWords = getDataFileSize(MLDF_MAPS_TAB) / 4;
    int binSize  = getDataFileSize(MLDF_MAPS_BIN);
    int mapCount = (tabWords / WORDS_PER_MAP) - 1;   // last group is the trailing end word

    // Scan mode (mapId < 0): report every map's object-section size.
    if (mapId < 0) {
        fprintf(stderr, "[romlist] scan: tabWords=%d mapCount=%d binSize=0x%x\n",
                tabWords, mapCount, binSize);
        for (int m = 0; m < mapCount; ++m) {
            int o = tab[m * WORDS_PER_MAP + 6], en = tab[m * WORDS_PER_MAP + 7];
            if (en - o > 0x40) fprintf(stderr, "[romlist]  map %d objSection=0x%x bytes\n", m, en - o);
        }
        return mapCount;
    }

    if (mapId >= mapCount) {
        fprintf(stderr, "[romlist] mapId %d out of range (0..%d)\n", mapId, mapCount - 1);
        return -1;
    }

    int base   = mapId * WORDS_PER_MAP;
    int objOff = tab[base + 6];                 // FACEFEED PackHeader offset in MAPS.bin
    if (objOff < 0 || objOff + 16 > binSize) {
        fprintf(stderr, "[romlist] map %d bad objOff 0x%x (bin=0x%x)\n", mapId, objOff, binSize);
        return -1;
    }

    // The objects section in MAPS.bin is a FACEFEED PackHeader; the real placements are
    // in <name>.romlist.zlb, decompressed via that header.
    static unsigned char objbuf[0x20000];
    int dsz = romlistDecompress(bin + objOff, mapName, objbuf, sizeof objbuf);
    if (dsz <= 0) { fprintf(stderr, "[romlist] map %d (%s): decompress failed\n", mapId, mapName); return -1; }

    unsigned char* p = objbuf;
    unsigned char* e = objbuf + dsz;
    int count = 0, logged = 0;
    while (p + OBJPLACEMENT_MIN <= e) {
        unsigned size = p[2];                   // record size in 4-byte units (byte, no swap)
        if (size == 0) break;
        int16_t objectId = (int16_t)beRead16(p);
        float x = beReadF32(p + 0x08), y = beReadF32(p + 0x0C), z = beReadF32(p + 0x10);
        if (logged < maxLog) {
            fprintf(stderr, "[romlist]   obj[%d] id=%d pos=(%.0f,%.0f,%.0f) sz=%u\n",
                    count, objectId, x, y, z, size);
            logged++;
        }
        count++;
        p += size * 4;
    }
    fprintf(stderr, "[romlist] map %d (%s): %d bytes -> %d placements\n",
            mapId, mapName, dsz, count);
    return count;
}
