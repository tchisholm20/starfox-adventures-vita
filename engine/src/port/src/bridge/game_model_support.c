// game_model_support.c - support layer so the real src/main/model.c links and its
// resource-cache init runs. model.c's ModelList/anim helpers live in modelEngine.c, which
// CANNOT be compiled (it redefines gResourceDescriptors + Resource_Acquire already ported
// in game_resource.c, and would pull all ~600 DLLs). So the handful model.c needs are
// reimplemented here (allocModelStruct faithfully; ModelList_getHeader -> "not cached" so
// loads run fresh), alongside real PS matrix/vector math + OS fastcast, and stubs for the
// model file-loaders / render helpers not on the init path yet. Headerless (bare-symbol
// linkage), like the other *_stubs.c, to avoid signature conflicts.
//
// Milestone: model.c compiled + linked + ObjModel_InitResourceCaches runs in init. Full
// ObjModel_Load (model file load + ModelFileHeader byte-swap + relocation) is the next step.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "port/byteswap.h"
#include "port/stfx_inflate.h"
#include "port/dvd_shim.h"

extern void* mmAlloc(int size, int type, int flag);
extern void  loadAssetFileById(void* out, int fileId);
extern void  bswapModelFileHeader(void* p);
extern void  bswapModelRenderOps(void* p);
extern double acos(double);   // avoid <math.h> so we don't hit ucrt's inline acosf
extern float sinf(float), cosf(float);   // same reason: pull from the CRT without <math.h>

// Model texture resolution helpers (TEX1 decode + RHI upload). Forward-declared with struct
// tags so this C TU stays free of the renderer/asset headers; the linker matches by name.
struct RhiInstance; struct RhiTexture;
extern struct RhiInstance* vi_host_rhi(void);
extern struct RhiTexture*  rhi_createTexture(struct RhiInstance* rhi, int w, int h, int mipLevels,
                                             unsigned gxFmt, const void* data);
extern int  asset_loadTexRecord(const unsigned char* tab, int tabSize, const unsigned char* bin,
                                int binSize, int id, unsigned char* out, size_t cap,
                                int* w, int* h, int* fmt, unsigned* imgOff);
extern unsigned char* gxTexDecode(int fmt, int w, int h, const unsigned char* src);

// --- MODELS.tab / MODELS.bin -------------------------------------------------
// TWO model tables: the ROOT-level MODELS.tab/bin is the GLOBAL table objects load their
// character/prop models from (the OBJECTS.bin modelFileIds index it); each <dir>/MODELS.tab is
// the per-area terrain/prop set the SHOW_MODELS viewer browses. getTableFileEntry resolves
// against root by default (objects), or the per-dir set when the viewer opts in via
// stairfax_model_perdir(1). gModelSrcRoot records which source the last lookup used so
// loadModelsBin / loadAndDecompressDataFile read the matching .bin.
static unsigned char* gModelsTab; static int gModelsTabSize;   // per-dir (viewer)
static unsigned char* gModelsBin; static int gModelsBinSize;
static unsigned char* gRootModelsTab; static int gRootModelsTabSize;   // root/global (objects)
static unsigned char* gRootModelsBin; static int gRootModelsBinSize;
// The retail runtime MODELS table is a MERGE of slot A (current area) and slot B (a resident
// "common"/character map = warlock). Player/common models (ids ~1256/1257) are empty in every
// area table but flagged (0x10000000) in warlock/MODELS.tab, and the merge redirects their data
// read to warlock/MODELS.bin (fileId 0x46). The port models warlock as the slot-B fallback.
static unsigned char* gCommonModelsTab; static int gCommonModelsTabSize;
static unsigned char* gCommonModelsBin; static int gCommonModelsBinSize;
#define STAIRFAX_COMMON_MODEL_DIR "warlock"
static int gUsePerDir;      // viewer opts in; objects leave it 0 -> root
static int gModelSrc;   // set by getTableFileEntry: 0=perdir(area), 1=root/global, 2=common(warlock)
static const char* modelSrcName(void) { return gModelSrc==2?"common":gModelSrc==1?"root":"perdir"; }
void stairfax_model_perdir(int on) { gUsePerDir = on; }

// --- anim data pipeline (MODANIM/AMAP global, ANIM per-dir) ------------------
// modelLoadAnimations + modelGetAmapSize read these via fileLoadToBufferOffset (dvd_shim)
// and gModelAnimDataOffsetTable. On disc they are big-endian; register host-swapped copies
// so the recompiled game reads them natively. MODANIM.TAB/BIN + AMAP.TAB/BIN are ROOT-level
// (global model-id -> anim mapping); ANIM.TAB/BIN are per-area (the area's animation data).
//   MODANIM.TAB (0x2d) u16[]: model id -> MODANIM.BIN offset
//   MODANIM.BIN (0x2e) s16[]: per-model anim-id lists, -1 separated
//   AMAP.TAB   (0x31) u32[]: model id -> AMAP.BIN offset (size = next-this within a group)
//   AMAP.BIN   (0x32) u8[]:  per-anim bone maps (byte data, no swap)
//   ANIM.TAB   (0x2f) u32[]: anim id -> ANIM.BIN offset (0x10000000 = slot flag)
//   ANIM.BIN   (0x30) raw anim records (NOT ZLB; size via consecutive ANIM.TAB deltas)
static unsigned char* gAnimTab; static int gAnimTabSize;   // per-dir ANIM.TAB, u32-swapped
static unsigned char* gAnimBin; static int gAnimBinSize;   // per-dir ANIM.BIN, raw
// PREANIM (resident pre-loaded anim set): the player's real animations live here, NOT in ANIM.BIN.
// animLoadFromTable (model.c:2138) reads PREANIM.TAB[id]; if & 0x10000000, the record is in
// PREANIM.BIN at (word & 0x0fffffff) and ANIM.BIN's 0x20 entry is just a "look in PREANIM" stub.
// PREANIM.TAB/BIN are ROOT-level. TAB registered as fileId 0x52 (fileLoadToBufferOffset serves it);
// BIN (0x51) served raw by loadAndDecompressDataFile below (size via consecutive PREANIM.TAB deltas).
static unsigned char* gPreTab; static int gPreTabSize;     // PREANIM.TAB, u32-swapped
static unsigned char* gPreBin; static int gPreBinSize;     // PREANIM.BIN, raw
static int gAnimRegistered;

// model.c global: anim id -> ANIM.BIN offset. Normally set by ObjModel_InitResourceCaches,
// but that returns early in the port (its getCurrentDataFile(MODELS_TAB_A) is null since we
// load MODELS.tab per-dir), so set it here when the swapped ANIM.TAB is ready.
extern unsigned* gModelAnimDataOffsetTable;

static void animFilesEnsureLoaded(const char* dir) {
    if (gAnimRegistered) return;
    gAnimRegistered = 1;
    int sz; unsigned char* b; char p[128];
    // root-level, host-swapped, handed to dvd_shim so fileLoadToBufferOffset serves slices
    if ((b = (unsigned char*)loadFileByPath("MODANIM.TAB", &sz, 0))) { beFixArray16(b, sz/2); dvd_register_buffer(0x2d, b, sz); }
    if ((b = (unsigned char*)loadFileByPath("MODANIM.BIN", &sz, 0))) { beFixArray16(b, sz/2); dvd_register_buffer(0x2e, b, sz); }
    if ((b = (unsigned char*)loadFileByPath("AMAP.TAB",    &sz, 0))) { beFixArray32(b, sz/4); dvd_register_buffer(0x31, b, sz); }
    if ((b = (unsigned char*)loadFileByPath("AMAP.BIN",    &sz, 0))) {                        dvd_register_buffer(0x32, b, sz); }
    // per-area
    snprintf(p, sizeof p, "%s/ANIM.TAB", dir);
    if ((gAnimTab = (unsigned char*)loadFileByPath(p, &gAnimTabSize, 0))) {
        beFixArray32(gAnimTab, gAnimTabSize/4);
        dvd_register_buffer(0x2f, gAnimTab, gAnimTabSize);
        gModelAnimDataOffsetTable = (unsigned*)gAnimTab;
    }
    snprintf(p, sizeof p, "%s/ANIM.BIN", dir);
    gAnimBin = (unsigned char*)loadFileByPath(p, &gAnimBinSize, 0);
    // PREANIM (root-level): where the player's real animations live.
    if ((gPreTab = (unsigned char*)loadFileByPath("PREANIM.TAB", &gPreTabSize, 0))) {
        beFixArray32(gPreTab, gPreTabSize/4);
        dvd_register_buffer(0x52, gPreTab, gPreTabSize);
    }
    gPreBin = (unsigned char*)loadFileByPath("PREANIM.BIN", &gPreBinSize, 0);
}
// Size of PREANIM id `idx` = delta of consecutive (host-order) PREANIM.TAB offsets.
static unsigned preanimEntrySize(int idx) {
    if (!gPreTab || idx < 0 || (idx + 2) * 4 > gPreTabSize) return 0x1000;
    unsigned a = ((unsigned*)gPreTab)[idx]     & 0x0fffffff;
    unsigned b = ((unsigned*)gPreTab)[idx + 1] & 0x0fffffff;
    return (b > a && b - a < 0x40000) ? (b - a) : 0x1000;
}

static void modelsEnsureLoaded(void) {
    if (gModelsTab && gModelsBin) return;
    const char* dir = getenv("STAIRFAX_MODEL_DIR"); if (!dir) dir = "desert";
    char p[128];
    snprintf(p, sizeof p, "%s/MODELS.tab", dir); gModelsTab = (unsigned char*)loadFileByPath(p, &gModelsTabSize, 0);
    snprintf(p, sizeof p, "%s/MODELS.bin", dir); gModelsBin = (unsigned char*)loadFileByPath(p, &gModelsBinSize, 0);
    // root/global model table - where objects' modelFileIds resolve
    if (!gRootModelsTab) gRootModelsTab = (unsigned char*)loadFileByPath((char*)"MODELS.tab", &gRootModelsTabSize, 0);
    if (!gRootModelsBin) gRootModelsBin = (unsigned char*)loadFileByPath((char*)"MODELS.bin", &gRootModelsBinSize, 0);
    // slot-B / common (warlock) models - holds the flagged player/common records
    if (!gCommonModelsTab) gCommonModelsTab = (unsigned char*)loadFileByPath((char*)STAIRFAX_COMMON_MODEL_DIR "/MODELS.tab", &gCommonModelsTabSize, 0);
    if (!gCommonModelsBin) gCommonModelsBin = (unsigned char*)loadFileByPath((char*)STAIRFAX_COMMON_MODEL_DIR "/MODELS.bin", &gCommonModelsBinSize, 0);
    animFilesEnsureLoaded(dir);
}
// Find the "ZLB" block within maxScan bytes (models have a small metadata prefix).
static int findZLB(const unsigned char* p, int maxScan) {
    for (int i = 0; i + 4 <= maxScan; ++i)
        if (p[i]=='Z' && p[i+1]=='L' && p[i+2]=='B') return i;
    return -1;
}

// --- real matrix/vector math (Mtx = f32[3][4] row-major, Vec = f32[3]) ------
void  PSVECAdd(const float* a, const float* b, float* o) { o[0]=a[0]+b[0]; o[1]=a[1]+b[1]; o[2]=a[2]+b[2]; }
void  PSVECSubtract(const float* a, const float* b, float* o) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
float PSVECDotProduct(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
void  PSMTXCopy(const float* s, float* d) { for (int i=0;i<12;++i) d[i]=s[i]; }
void  PSMTXTrans(float* m, float x, float y, float z) {
    memset(m,0,12*sizeof(float)); m[0]=m[5]=m[10]=1.0f; m[3]=x; m[7]=y; m[11]=z;
}
void  PSMTXTranspose(const float* s, float* d) {   // 3x3 transpose, translation cleared
    d[0]=s[0]; d[1]=s[4]; d[2]=s[8];  d[3]=0.0f;
    d[4]=s[1]; d[5]=s[5]; d[6]=s[9];  d[7]=0.0f;
    d[8]=s[2]; d[9]=s[6]; d[10]=s[10];d[11]=0.0f;
}
void  PSMTXReorder(const float* s, float* d) { PSMTXCopy(s,d); }  // approx; used off the load path
// ab = a * b for 3x4 matrices (b's implicit 4th row = [0 0 0 1]); alias-safe. This is THE model
// render-path concat: renderOpMatrix does PSMTXConcat(modelView, jointBank[idx], posMtx) and
// modelInitBones builds the bind pose with it. It was previously a return-0 stub, so every joint
// matrix (bind AND animated) was garbage - the skinned player rendered as a spike.
void  PSMTXConcat(const float* a, const float* b, float* ab) {
    float t[12];
    t[0]  = a[0]*b[0] + a[1]*b[4] + a[2]*b[8];
    t[1]  = a[0]*b[1] + a[1]*b[5] + a[2]*b[9];
    t[2]  = a[0]*b[2] + a[1]*b[6] + a[2]*b[10];
    t[3]  = a[0]*b[3] + a[1]*b[7] + a[2]*b[11] + a[3];
    t[4]  = a[4]*b[0] + a[5]*b[4] + a[6]*b[8];
    t[5]  = a[4]*b[1] + a[5]*b[5] + a[6]*b[9];
    t[6]  = a[4]*b[2] + a[5]*b[6] + a[6]*b[10];
    t[7]  = a[4]*b[3] + a[5]*b[7] + a[6]*b[11] + a[7];
    t[8]  = a[8]*b[0] + a[9]*b[4] + a[10]*b[8];
    t[9]  = a[8]*b[1] + a[9]*b[5] + a[10]*b[9];
    t[10] = a[8]*b[2] + a[9]*b[6] + a[10]*b[10];
    t[11] = a[8]*b[3] + a[9]*b[7] + a[10]*b[11] + a[11];
    for (int i = 0; i < 12; ++i) ab[i] = t[i];
}
// m = scale(x,y,z): a pure diagonal scale matrix (matches dolphin PSMTXScale, which SETS m).
void  PSMTXScale(float* m, float x, float y, float z) {
    m[0]=x; m[1]=0; m[2]=0;  m[3]=0;
    m[4]=0; m[5]=y; m[6]=0;  m[7]=0;
    m[8]=0; m[9]=0; m[10]=z; m[11]=0;
}
// out = 3x3(m) * in  (rotation/scale only, no translation).
void  PSMTXMultVecSR(const float* m, const float* in, float* out) {
    float x=in[0], y=in[1], z=in[2];
    out[0]=m[0]*x+m[1]*y+m[2]*z;
    out[1]=m[4]*x+m[5]*y+m[6]*z;
    out[2]=m[8]*x+m[9]*y+m[10]*z;
}
// m = rotation of `rad` radians about principal axis ('x'/'y'/'z'). SETS m (matches dolphin
// C_MTXRotRad). It was previously a no-op stub, which left the caller's Mtx as stack garbage -
// PSMTXMultVecSR(garbage, vec) then NaN'd the player's motion/tail vectors -> NaN position.
void  PSMTXRotRad(float* m, char axis, float rad) {
    float s = sinf(rad), c = cosf(rad);
    for (int i = 0; i < 12; ++i) m[i] = 0.0f;
    switch (axis) {
        case 'x': case 'X':
            m[0]=1.0f; m[5]=c; m[6]=-s; m[9]=s; m[10]=c; break;
        case 'y': case 'Y':
            m[0]=c; m[2]=s; m[5]=1.0f; m[8]=-s; m[10]=c; break;
        default: /* 'z' */
            m[0]=c; m[1]=-s; m[4]=s; m[5]=c; m[10]=1.0f; break;
    }
}

// --- OS fast casts (shadow OSFastCast.h declares these extern) --------------
// SATURATING (vita fork, 2026-10-10, docs/47 "the platform-layer audit"): on the GameCube these are psq_st quantised stores,
// which clamp to the destination's range and truncate toward zero. A plain C cast of an out-of-range float is undefined and
// on ARM wraps, so a skinned vertex past +-32767 (or a normal past +-127) flipped to the opposite sign.
int16_t __OSf32tos16(float f)        { return f >= 32767.0f ? (int16_t)32767 : f <= -32768.0f ? (int16_t)-32768 : f != f ? (int16_t)0 : (int16_t)f; }
int8_t  __OSf32tos8(float f)         { return f >= 127.0f ? (int8_t)127 : f <= -128.0f ? (int8_t)-128 : f != f ? (int8_t)0 : (int8_t)f; }
float   __OSs16tof32(const int16_t* p){ return (float)*p; }
float   __OSs8tof32(const int8_t* p)  { return (float)*p; }
float   __OSu8tof32(const uint8_t* p) { return (float)*p; }

// --- modelEngine ModelList (reimplemented; layout from main/model_engine.h) --
typedef struct { int16_t* entries; int16_t* end; int16_t* capacityEnd;
                 uint8_t dataSize; uint8_t strideShorts; uint8_t pad[2]; int16_t* iter; } ML;
void* allocModelStruct(int capacity, int dataSize) {
    int entryBytes = dataSize + 2;
    ML* list = (ML*)mmAlloc(capacity*entryBytes + (int)sizeof(ML), 0x1a, 0);
    list->entries = (int16_t*)((uint8_t*)list + sizeof(ML));
    list->dataSize = (uint8_t)dataSize;
    list->strideShorts = (uint8_t)((unsigned)entryBytes >> 1);
    list->end = list->entries;
    list->capacityEnd = list->entries + capacity * list->strideShorts;
    memset(list->entries, -1, (size_t)capacity * (list->strideShorts * 2));
    return list;
}
int  ModelList_getHeader(void* list, int index, void* outHeader) { (void)list;(void)index;(void)outHeader; return 0; }
void modelInitModelList(void) { }
int  model_adjustModelList(void) { return 0; }
int  model_findIdxInModelList(void) { return 0; }

// --- data-file loaders (getCurrentDataFile bridges to the MLDF asset loader) --
void* getCurrentDataFile(int id) {
    if (id == 0x2f) { modelsEnsureLoaded(); return gAnimTab; }  // ANIM.TAB (per-dir, host-swapped)
    void* p = 0; loadAssetFileById(&p, id); return p;
}
// textureIds[i] holds whatever textureLoad returned (a ModelTex handle) reinterpreted as s32;
// textureIdxToPtr converts it back to the pointer (ObjModel_GetTexture's render-time resolve).
void* stairfax_modeltex_rhi(void* p);   // fwd (defined below)
extern void gx_draw_setTexture(struct RhiTexture* tex);
void* textureIdxToPtr(int index) {
    void* p = (void*)(intptr_t)index;
    // The real render path resolves each render op's textures through here right before binding
    // them to GX. The port's TEV/texture-stage setup is stubbed, so bind the base texture to the
    // RHI sampler as a side effect: any handle that is one of our ModelTex records decodes+binds.
    struct RhiTexture* rt = (struct RhiTexture*)stairfax_modeltex_rhi(p);
    if (rt) gx_draw_setTexture(rt);
    return p;
}

// --- model texture cache (TEX1: area slot A + warlock/common slot B) ---------
// ObjModel_Load calls textureLoad(-(rawId | 0x8000), 1) per model texture and patches the return
// into ModelFileHeader.textureIds[i]; ObjModel_ResolveRenderOpTextures then copies it into each
// render op's layers[].texture (Shader+0x24). We return a small ModelTex handle (cached by rawId);
// renderModel later calls stairfax_modeltex_rhi to lazily decode the TEX1 record into an RhiTexture
// and bind it. Decode is deferred to render time so the RHI is guaranteed up.
typedef struct ModelTex { int rawId; int tried; struct RhiTexture* rt; } ModelTex;
static ModelTex** gModelTex; static int gModelTexN, gModelTexCap;
static ModelTex* modelTexGet(int rawId) {
    for (int i = 0; i < gModelTexN; ++i) if (gModelTex[i]->rawId == rawId) return gModelTex[i];
    if (gModelTexN == gModelTexCap) {
        int nc = gModelTexCap ? gModelTexCap * 2 : 64;
        ModelTex** n = (ModelTex**)realloc(gModelTex, (size_t)nc * sizeof(ModelTex*));
        if (!n) return 0; gModelTex = n; gModelTexCap = nc;
    }
    ModelTex* m = (ModelTex*)calloc(1, sizeof(ModelTex)); if (!m) return 0;
    m->rawId = rawId; gModelTex[gModelTexN++] = m; return m;
}
void* textureLoad(int texId, unsigned char flag) {
    (void)flag;
    return modelTexGet((-texId) & 0x7fff);   // texId arrives as -(rawId | 0x8000)
}

// TEX1 sources: per-area (slot A) + warlock/common (slot B, where character textures live).
static unsigned char* gTex1Tab; static int gTex1TabSize;
static unsigned char* gTex1Bin; static int gTex1BinSize;
static unsigned char* gCTex1Tab; static int gCTex1TabSize;
static unsigned char* gCTex1Bin; static int gCTex1BinSize;
static int gTex1Registered;
static void tex1EnsureLoaded(void) {
    if (gTex1Registered) return; gTex1Registered = 1;
    const char* dir = getenv("STAIRFAX_MODEL_DIR"); if (!dir) dir = "desert";
    char p[128];
    snprintf(p, sizeof p, "%s/TEX1.tab", dir); gTex1Tab = (unsigned char*)loadFileByPath(p, &gTex1TabSize, 0);
    snprintf(p, sizeof p, "%s/TEX1.bin", dir); gTex1Bin = (unsigned char*)loadFileByPath(p, &gTex1BinSize, 0);
    gCTex1Tab = (unsigned char*)loadFileByPath((char*)STAIRFAX_COMMON_MODEL_DIR "/TEX1.tab", &gCTex1TabSize, 0);
    gCTex1Bin = (unsigned char*)loadFileByPath((char*)STAIRFAX_COMMON_MODEL_DIR "/TEX1.bin", &gCTex1BinSize, 0);
}

// Lazily decode a ModelTex's TEX1 record into an RhiTexture and return it (NULL until the RHI is
// up or on decode failure). Called from renderModel with a render op's layers[].texture pointer.
static unsigned char gModelTexBuf[4 * 1024 * 1024];
void* stairfax_modeltex_rhi(void* p) {
    if (!p) return 0;
    ModelTex* m = 0;
    for (int i = 0; i < gModelTexN; ++i) if ((void*)gModelTex[i] == p) { m = gModelTex[i]; break; }
    if (!m) return 0;
    if (m->tried) return m->rt;
    struct RhiInstance* rhi = vi_host_rhi();
    if (!rhi) return 0;                       // RHI not up yet: retry next frame
    m->tried = 1;
    tex1EnsureLoaded();
    int aw=0, ah=0, af=0; unsigned aio=0; int aok = 0;
    int cw=0, ch=0, cf=0; unsigned cio=0; int cok = 0;
    if (gCTex1Tab && asset_loadTexRecord(gCTex1Tab, gCTex1TabSize, gCTex1Bin, gCTex1BinSize, m->rawId,
                                         gModelTexBuf, sizeof gModelTexBuf, &cw, &ch, &cf, &cio)) cok = 1;
    // common (warlock) holds the character textures; probe it first and prefer it. area is the
    // fallback for area-local props whose ids the common set doesn't carry.
    int w, h, fmt; unsigned io; const char* srcName;
    if (cok) { w=cw; h=ch; fmt=cf; io=cio; srcName="common"; }
    else if (gTex1Tab && asset_loadTexRecord(gTex1Tab, gTex1TabSize, gTex1Bin, gTex1BinSize, m->rawId,
                                             gModelTexBuf, sizeof gModelTexBuf, &aw, &ah, &af, &aio)) {
        w=aw; h=ah; fmt=af; io=aio; srcName="area"; aok=1;
    } else { srcName=0; }
    if (srcName) {
        unsigned char* rgba = gxTexDecode(fmt, w, h, gModelTexBuf + io);
        if (rgba) { m->rt = rhi_createTexture(rhi, w, h, 1, (unsigned)fmt, rgba); free(rgba); }
        if (getenv("STAIRFAX_MODEL_TEX"))
            fprintf(stderr, "[modeltex] id=%d -> %s %dx%d fmt=%d %s (common=%d area=%d)\n",
                    m->rawId, srcName, w, h, fmt, m->rt ? "ok" : "decode-fail", cok, aok);
    } else if (getenv("STAIRFAX_MODEL_TEX")) {
        fprintf(stderr, "[modeltex] id=%d NOT in area/common TEX1\n", m->rawId);
    }
    return m->rt;
}

// selectTexture(texture, mapId): the real objRenderModel/modelDoRenderInstrs path binds each
// render op's textures through this (objprint_dolphin.c). `texture` is the ModelTex handle
// textureLoad returned (via textureIdxToPtr). The RHI has a single sampler, so bind only the
// base color layer (mapId 0); a null base (untextured op) naturally clears the binding. Other
// stages (aux/indirect/noise/shadow) are ignored. This is what textures the default player.
extern void gx_draw_setTexture(struct RhiTexture* tex);
void selectTexture(void* texture, int mapId) {
    if (mapId != 0) return;
    gx_draw_setTexture((struct RhiTexture*)stairfax_modeltex_rhi(texture));
}

// Size of anim id `idx` in ANIM.BIN = delta of consecutive (host-order) ANIM.TAB offsets.
static unsigned animEntrySize(int idx) {
    if (!gAnimTab || idx < 0 || (idx + 2) * 4 > gAnimTabSize) return 0x1000;
    unsigned a = ((unsigned*)gAnimTab)[idx]     & 0x0fffffff;
    unsigned b = ((unsigned*)gAnimTab)[idx + 1] & 0x0fffffff;
    return (b > a && b - a < 0x40000) ? (b - a) : 0x1000;
}

// Scan MODELS.tab for non-zero entries (real models), logging the first `want`.
int stairfax_model_scan(int want) {
    modelsEnsureLoaded();
    if (!gModelsTab) { fprintf(stderr, "[model] no MODELS.tab\n"); return -1; }
    int n = gModelsTabSize / 4, found = 0, first = -1, firstStatic = -1;
    for (int i = 0; i < n; ++i) {
        unsigned e = beRead32(gModelsTab + i * 4);
        if (e & 0x0fffffff) {
            unsigned off = e & 0x0fffffff;
            int animCount = (gModelsBin && off + 0x24 <= (unsigned)gModelsBinSize) ? (int)beRead32(gModelsBin + off + 0x1c) : -1;
            if (first < 0) first = i;
            if (animCount == 0 && firstStatic < 0) firstStatic = i;
            if (found < want) fprintf(stderr, "[model] tab[%d]=0x%08x animCount=%d\n", i, e, animCount);
            found++;
        }
    }
    fprintf(stderr, "[model] first real=%d, first STATIC (animCount 0)=%d\n", first, firstStatic);
    if (firstStatic >= 0) return firstStatic;
    fprintf(stderr, "[model] MODELS.tab: %d entries, %d non-zero, first=%d\n", n, found, first);
    return first;
}

// Look up `index` in one MODELS.tab; return the entry word (with 0x10000000 flag) or 0.
static unsigned modelsTabEntry(const unsigned char* tab, int tabSize, int index) {
    if (!tab || index < 0 || (index + 1) * 4 > tabSize) return 0;
    unsigned e = beRead32(tab + index * 4);
    return (e & 0x0fffffff) ? e : 0;
}
// The .bin matching the source the last getTableFileEntry resolved.
static const unsigned char* curModelsBin(int* sz) {
    if (gModelSrc == 2) { *sz = gCommonModelsBinSize; return gCommonModelsBin; }
    if (gModelSrc == 1) { *sz = gRootModelsBinSize;   return gRootModelsBin; }
    *sz = gModelsBinSize; return gModelsBin;
}

// getTableFileEntry(MODELS_TAB_A, index) -> the model's MODELS.bin entry word (BE). Objects
// resolve against the ROOT/global table; the SHOW_MODELS viewer opts into per-dir first.
int getTableFileEntry(int fileId, int index, int* out) {
    if (fileId != 0x2a) { if (out) *out = 0; return 0; }
    modelsEnsureLoaded();
    unsigned e = 0;
    // Merge order (mirrors mergeTableFiles' A-then-B rule): area/root first, then the resident
    // common (warlock) slot B for the player/common records the area tables leave empty.
    if (gUsePerDir && (e = modelsTabEntry(gModelsTab, gModelsTabSize, index)))     gModelSrc = 0;
    else if ((e = modelsTabEntry(gRootModelsTab, gRootModelsTabSize, index)))       gModelSrc = 1;
    else if ((e = modelsTabEntry(gModelsTab, gModelsTabSize, index)))               gModelSrc = 0;
    else if ((e = modelsTabEntry(gCommonModelsTab, gCommonModelsTabSize, index)))   gModelSrc = 2;
    if (!e) {
        if (getenv("STAIRFAX_MODEL_TEST"))
            fprintf(stderr, "[model] tab[%d] MISS (root[i]=0x%x common[i]=0x%x)\n", index,
                    (gRootModelsTab && (index+1)*4<=gRootModelsTabSize) ? (unsigned)beRead32(gRootModelsTab+index*4) : 0,
                    (gCommonModelsTab && (index+1)*4<=gCommonModelsTabSize) ? (unsigned)beRead32(gCommonModelsTab+index*4) : 0);
        return 0;
    }
    if (out) *out = (int)e;
    if (getenv("STAIRFAX_MODEL_TEST"))
        fprintf(stderr, "[model] tab[%d]=0x%08x (off=0x%x) src=%s\n",
                index, (int)e, e & 0x0fffffff, modelSrcName());
    return 1;
}

// loadModelsBin reads the model entry's size metadata (BE) from MODELS.bin[offset].
void loadModelsBin(int offsetFlags, int* pAnimCount, int* pHeaderSize, int* pAmapFlag, int* pDataLen, int id) {
    (void)id;
    modelsEnsureLoaded();
    unsigned off = (unsigned)offsetFlags & 0x0fffffff;
    int binSize; const unsigned char* bin = curModelsBin(&binSize);
    if (!bin || off + 0x24 > (unsigned)binSize) {
        *pAnimCount = *pHeaderSize = *pAmapFlag = *pDataLen = 0; return;
    }
    const unsigned char* e = bin + off;
    *pDataLen    = (int)beRead32(e + 0x04);
    *pAmapFlag   = (int)beRead32(e + 0x18);
    *pAnimCount  = (int)beRead32(e + 0x1c);
    *pHeaderSize = (int)beRead32(e + 0x20);
}

// loadAndDecompressDataFile: fileId-aware.
//   MODELS.bin (0x2b/0x46): inflate the model's ZLB block into dst, then swap the header.
//   ANIM.bin   (0x30/0x4a): raw anim records (NOT ZLB). flags&1 = size probe (return the
//     entry size via *sizeOut, no copy); else copy `length` bytes into dst. The anim record
//     content stays big-endian for now (playback byte-swap is the next step); the load path
//     only needs a correctly-sized, non-crashing buffer.
void* loadAndDecompressDataFile(int fileId, void* dst, int offsetFlags, unsigned length, int* sizeOut, int idx, unsigned flags) {
    modelsEnsureLoaded();
    unsigned off = (unsigned)offsetFlags & 0x0fffffff;

    if (fileId == 0x30 || fileId == 0x4a) {           // ANIM.BIN (per-area, raw)
        if (flags & 1) { if (sizeOut) *sizeOut = (int)animEntrySize(idx); return dst; }
        if (!gAnimBin || !dst) return dst;
        unsigned n = length;
        if (off >= (unsigned)gAnimBinSize) return dst;
        if (off + n > (unsigned)gAnimBinSize) n = (unsigned)gAnimBinSize - off;
        memcpy(dst, gAnimBin + off, n);
        return dst;
    }

    if (fileId == 0x51) {                             // PREANIM.BIN (resident, raw) - real player anims
        if (flags & 1) { if (sizeOut) *sizeOut = (int)preanimEntrySize(idx); return dst; }
        if (!gPreBin || !dst) return dst;
        unsigned n = length;
        if (off >= (unsigned)gPreBinSize) return dst;
        if (off + n > (unsigned)gPreBinSize) n = (unsigned)gPreBinSize - off;
        memcpy(dst, gPreBin + off, n);
        // The PREANIM record is big-endian on disc. Unlike the per-area moveData (swapped once by
        // stairfax_bswap_model_moves at spawn), cached moves load lazily down this path and are never
        // swapped - so the decoder read BE rotation descriptors as host order -> collapsed/spinning
        // joints. Swap the fields it reads natively: streamOff@2, rootCurveOffset@4, and the u16
        // frame-command descriptors [10..streamOff). The packed delta STREAM stays BE (read MSB-first).
        if (n > 10) {
            unsigned char* a = (unsigned char*)dst;
            unsigned streamOff = beRead16(a + 2);
            beFix16(a + 2); beFix16(a + 4);
            if (streamOff > 10 && streamOff < 0x8000 && (unsigned)streamOff <= n)
                beFixArray16(a + 10, (streamOff - 10) / 2);
            // Root curve (ObjAnimMoveData.rootCurveOffset @+4, now host order): a run/blend move's
            // speed->phase curve, at moveData + rootCurveOffset, which sits PAST the frame stream so
            // the descriptor swap above never reaches it. Layout: f32 scale, s16 sampleCount, then s16
            // axis samples (to end of record). Left BE, ObjAnim_SampleRootCurvePhase reads a
            // non-monotonic distance curve and its do/while(!foundPhase) (objanim.c:645) SPINS - the
            // gait-8 sprint hang. Swap it here so the curve is a valid ascending distance table.
            int rco = *(short*)(a + 4);
            if (rco >= (int)streamOff && rco + 6 <= (int)n) {
                beFix32(a + rco);                                   // f32 scale
                beFix16(a + rco + 4);                               // s16 sampleCount
                beFixArray16(a + rco + 6, (n - (rco + 6)) / 2);     // s16 axis samples
            }
        }
        return dst;
    }

    // MODELS.bin (default): FACEFEED metadata prefix + ZLB block, from the source getTableFileEntry
    // resolved (root for objects, per-dir for the viewer).
    int binSize; const unsigned char* bin = curModelsBin(&binSize);
    if (!bin || !dst || off >= (unsigned)binSize) return dst;
    int z = findZLB(bin + off, 0x40);
    if (z < 0) { if (getenv("STAIRFAX_MODEL_TEST")) fprintf(stderr, "[model] no ZLB at 0x%x (src=%s)\n", off, modelSrcName()); return dst; }
    const unsigned char* zlb = bin + off + z;
    unsigned csize = beRead32(zlb + 0xc);
    size_t got = 0;
    if (stfx_inflate_zlib((unsigned char*)dst, length ? length : 0x200000, zlb + 0x10, csize, &got) != 0) {
        if (getenv("STAIRFAX_MODEL_TEST")) fprintf(stderr, "[model] inflate failed (csize=0x%x)\n", csize);
        return dst;
    }
    bswapModelFileHeader(dst);   // BE -> host so RelocateModelData reads valid offsets
    bswapModelRenderOps(dst);    // + renderOps/textureIds leaf so ResolveRenderOpTextures is safe
    return dst;
}

// Load a model's renderable header only, exactly as ObjModel_Load does up to (but not
// including) modelLoadAnimations - which needs the MODANIM/AMAP/ANIM data subsystem that
// isn't up yet and faults on animated models. This yields a fully relocated header with
// render-op textures resolved (bind pose), enough to render skinned models statically.
extern void* ObjModel_LoadModelData(int id);
extern void  ObjModel_RelocateModelData(unsigned char* m);
extern void  ObjModel_ResolveRenderOpTextures(unsigned char* m);
void* stairfax_model_load_static(int id) {
    int realId = id < 0 ? -id : id;
    unsigned char* h = (unsigned char*)ObjModel_LoadModelData(realId);
    if (!h) return 0;
    ObjModel_RelocateModelData(h);
    int off = 0;
    for (int i = 0; i < h[0xf2]; i++) {           // textureLoad is stubbed -> null slots
        int base = *(int*)(h + 0x20);
        *(void**)(base + off) = 0;
        off += 4;
    }
    ObjModel_ResolveRenderOpTextures(h);
    return h;
}

// Byte-swap a model's animation move data (BE-on-disc -> host). The model header IS the
// ObjAnimDef (ObjModel.file/animDef union): moveCount@0xEC, moveData@0x64 = a union with
// animationModelPtrs, an array of moveCount pointers to the loaded ANIM.BIN atlas entries.
// Each entry is a self-contained ObjAnimMoveData/ObjAnimFrameCommand blob, still big-endian:
//   [0]=refcount [1]=frameControl [2..3]=streamOff(s16) [4..5]=rootCurveOffset(s16)
//   [6]=jointCount [7]=frameLength [8]=frameStride [9]=? [10..streamOff)=u16 descriptors
//   [streamOff..)=packed per-frame delta stream (a big-endian bitstream - left BE; the
//   reimplemented decoder reads it MSB-first). RelocateModelData already relocated the
//   pointers; only these multi-byte scalar/array fields need swapping so Object_ObjAnimSetMove
//   + modelAnimUpdateChannels read them natively. Idempotency: dedups aliased entries; call once.
void stairfax_bswap_model_moves(void* header) {
    unsigned char* h = (unsigned char*)header;
    int moveCount = *(unsigned short*)(h + 0xEC);
    unsigned char** moveData = *(unsigned char***)(h + 0x64);
    if (!moveData || moveCount <= 0 || moveCount > 256) return;
    unsigned char* seen[256]; int ns = 0;
    for (int i = 0; i < moveCount; ++i) {
        unsigned char* a = moveData[i];
        if (!a) continue;
        int dup = 0; for (int k = 0; k < ns; ++k) if (seen[k] == a) { dup = 1; break; }
        if (dup) continue;
        if (ns < 256) seen[ns++] = a;
        unsigned streamOff = beRead16(a + 2);   // read BE before swapping in place
        beFix16(a + 2);                          // streamOff (s16)
        beFix16(a + 4);                          // rootCurveOffset (s16)
        if (streamOff > 10 && streamOff < 0x8000)
            beFixArray16(a + 10, (streamOff - 10) / 2);  // frame-command descriptors (u16)
    }
}

// --- anim / render helpers (not on the init path yet) -----------------------
// The real move-frame loader. objanim's ObjAnim_LoadCachedMove -> animationLoad streams a cached
// move's keyframes into the model's move cache (the real gameloop.c routes this through a type-7
// loadAsset -> loadAnimation; the port has no gameloop.c, so call loadAnimation directly). Left as
// the return-0 stub, no move ever loaded -> the anim decode read a placeholder (nSlots=1) and every
// character stayed in bind/T-pose. loadAnimation(animDef, animId, moveIndex, cache) reads the move
// from PREANIM/ANIM/AMAP via the same asset loaders the model/map paths use.
extern void* loadAnimation(void* hdr, short id, int moveIndex, unsigned char* bufout);
void animationLoad(void** out, int animId, int moveIndex, unsigned char* cache, void* animDef) {
    if (out) *out = loadAnimation(animDef, (short)animId, moveIndex, cache);
}
// modelAnimBuildJointMatrices + modelRenderInterpolateRootTransform are now the reversed real
// implementations in game_scene.cpp.
void  modelRenderDecodeAdpcm(void) { }
// Real render-instruction bitstream state (from modelEngine.c, which the port can't compile).
// The interpreter (objprint_dolphin.c) needs this to actually initialise the bit cursor.
typedef struct { unsigned char* instrs; int byteCount; int bitCount; int fieldC; int bit; } RIState;
void modelRenderInstrsState_init(void* st, void* instrs, int bitCount, int fieldC) {
    RIState* s = (RIState*)st;
    s->byteCount = (bitCount >> 3) + ((bitCount & 7) ? 1 : 0);
    s->bitCount = bitCount; s->fieldC = fieldC; s->instrs = (unsigned char*)instrs; s->bit = 0;
}
int  modelRenderInstrsState_getBit(void* st) { return ((RIState*)st)->bit; }
void modelRenderInstrsState_setBit(void* st, int bit) { ((RIState*)st)->bit = bit; }
void  ShaderDef_free(void) { }
// Real shaderInit (rcp_dolphin.c) records a render op's texture references into the
// ObjModel's per-op ModelRenderOpTextureRefs. The port renders untextured, but the
// character display lists are baked with a fixed vertex layout whose matrix-index
// prefix INCLUDES a texture-matrix index (GX_VA_TEX0MTXIDX) whenever the op is
// textured - modelRenderFn_setVtxDescr gates that attribute on textureRefs[0]/[1]
// being non-null. Leaving refs null (the old no-op stub) made the runtime descriptor
// one attribute short of the baked DL, so every vertex drifted by a byte and the
// draw walked off the list. Reproduce the invariant: a render op that references any
// texture layer gets a non-null texture0 sentinel, so the descriptor matches the
// baked layout. The sentinel is never dereferenced on the untextured path (the TEV /
// selectTexture stages are stubbed). Layout: def = Shader* (render op); texture0 @ +0.
static unsigned char gShaderTexSentinel[4];
void  shaderInit(unsigned char* def, void* textures, void* obj, int shaderFlags) {
    (void)obj; (void)shaderFlags;
    if (!def || !textures) return;
    unsigned char layerCount = def[0x41];   // Shader.layerCount
    void* reg1 = *(void**)(def + 0x08);      // Shader.reg1Texture
    void* reg2 = *(void**)(def + 0x14);      // Shader.reg2Texture
    void** texRefs = (void**)textures;       // [0]=texture0, [1]=texture1
    if (reg1)              texRefs[0] = reg1;
    else if (layerCount)   texRefs[0] = gShaderTexSentinel;
    if (reg2)              texRefs[1] = reg2;
}
// objFrozenRenderCb now real in intersect_render.c (text/2D render bring-up) - removed.

// --- misc ------------------------------------------------------------------
void     GXSetAlphaCompare(void) { }
unsigned PPCMfhid2(void) { return 0; }
float    acosf(float x) { return (float)acos((double)x); }  // shadow math_api declares extern

// --- data globals ----------------------------------------------------------
float gModelRootRotX, gModelRootRotY, gModelRootRotZ, gModelVertexScale;

// --- UI-DLL loader (faithful port of modelEngine.c's front-end DLL spine) ----
// The title screen / menus / attract mode are "UI DLLs": one is current at a time,
// Resource_Acquire'd by id from gModelEngineUiDllResourceIds[], run each frame via its
// UiDllVTable (frameStart/frameEnd/draw). The real loader lives in modelEngine.c, which
// the port cannot compile, so it is reimplemented here (like the rest of this file).
// Safe as a spine on its own: curUiDll starts 0 -> id -1 -> nothing loaded -> no vtable
// calls, until front-end code calls loadUiDll(index) with a registered UI DLL.
typedef struct UiDllVTable {
    void* field0;
    int  (*frameStart)(void);
    void (*frameEnd)(void);
    void (*draw)(int arg0, int arg1, int arg2);
    void (*setState)(int state);
} UiDllVTable;

extern void* Resource_Acquire(unsigned short id, int unused);
extern int   Resource_Release(void* handleSlot);

int gModelEnginePrevUiDll;
int curUiDll;
int gModelEnginePendingUiDll;
UiDllVTable** gModelEngineCurUiDllRes;
int gModelEngineUiDllResourceIds[] = {
    -1, 16, 50, 51, 52, 53, 54, 55, 56, 57, -1, -1, 58, -1, 63, 64, 65, -1,
};

void curUiDllDraw(int a, int b, int c, int d) {
    (void)d;
    if (gModelEngineCurUiDllRes != NULL) {
        UiDllVTable* cb = *gModelEngineCurUiDllRes;
        cb->draw(a, b, c);
    }
}

void uiDll_runFrameEndAndLoadNext(void) {
    int resourceId;
    if (gModelEngineCurUiDllRes != NULL) {
        UiDllVTable* cb = *gModelEngineCurUiDllRes;
        cb->frameEnd();
    }
    if (gModelEnginePendingUiDll != 0) {
        gModelEnginePendingUiDll--;
        gModelEnginePrevUiDll = curUiDll;
        if (gModelEngineCurUiDllRes != NULL) {
            Resource_Release(gModelEngineCurUiDllRes);
            gModelEngineCurUiDllRes = NULL;
        }
        resourceId = gModelEngineUiDllResourceIds[gModelEnginePendingUiDll];
        if (resourceId != -1) {
            gModelEngineCurUiDllRes = Resource_Acquire((unsigned short)resourceId, 1);
        } else {
            gModelEngineCurUiDllRes = NULL;
            gModelEnginePendingUiDll = 0;
        }
        curUiDll = gModelEnginePendingUiDll;
        gModelEnginePendingUiDll = 0;
    }
}

int uiDll_runFrameStartAndLoadNext(void) {
    int result = 0;
    int resourceId;
    if (gModelEngineCurUiDllRes != NULL) {
        UiDllVTable* cb = *gModelEngineCurUiDllRes;
        result = cb->frameStart();
    }
    if (gModelEnginePendingUiDll != 0) {
        gModelEnginePendingUiDll--;
        gModelEnginePrevUiDll = curUiDll;
        if (gModelEngineCurUiDllRes != NULL) {
            Resource_Release(gModelEngineCurUiDllRes);
            gModelEngineCurUiDllRes = NULL;
        }
        resourceId = gModelEngineUiDllResourceIds[gModelEnginePendingUiDll];
        if (resourceId != -1) {
            gModelEngineCurUiDllRes = Resource_Acquire((unsigned short)resourceId, 1);
        } else {
            gModelEngineCurUiDllRes = NULL;
            gModelEnginePendingUiDll = 0;
        }
        curUiDll = gModelEnginePendingUiDll;
        gModelEnginePendingUiDll = 0;
    }
    return result;
}

void setCurUiDll(int idx) { curUiDll = idx; }
int  getPrevUiDll(void) { return gModelEnginePrevUiDll; }
UiDllVTable** getCurUiDllInterface(void) { return gModelEngineCurUiDllRes; }
int  getCurUiDll(void) { return curUiDll; }

void loadUiDll(int index) {
    int next, current, resourceId;
    current = curUiDll;
    if (index != current) {
        next = index + 1;
        gModelEnginePendingUiDll = next;
        if (gModelEngineCurUiDllRes == NULL && next != 0) {
            gModelEnginePendingUiDll = next - 1;
            gModelEnginePrevUiDll = current;
            if (gModelEngineCurUiDllRes != NULL) {
                Resource_Release(gModelEngineCurUiDllRes);
                gModelEngineCurUiDllRes = NULL;
            }
            resourceId = gModelEngineUiDllResourceIds[gModelEnginePendingUiDll];
            if (resourceId != -1) {
                gModelEngineCurUiDllRes = Resource_Acquire((unsigned short)resourceId, 1);
            } else {
                gModelEngineCurUiDllRes = NULL;
                gModelEnginePendingUiDll = 0;
            }
            curUiDll = gModelEnginePendingUiDll;
            gModelEnginePendingUiDll = 0;
        }
    }
}
