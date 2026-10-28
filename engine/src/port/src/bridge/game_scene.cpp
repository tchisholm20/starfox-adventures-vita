// game_scene.cpp - interim port-side sceneRender for the live engine loop.
//
// The real src/main/lightmap.c sceneRender() drives camera/lights/visible-geometry
// then sceneDraw(), which walks REAL loaded map+object data through renderSceneGeometry
// -> mapBlockRenderMain (the matched interpreter in lightmap_draw.c/tex_dolphin.c/...).
// That whole path is gated on the resource/DLL system + romlist loading real map data
// into memory, which is still stubbed. Until those subsystems come online, this stands
// in for sceneRender: it loads a real map straight from the ISO via stairfax_assets and
// draws it through the SAME gx_draw -> RHI path map_view proved, with a free-fly camera
// driven by the now-real host input (pad_input.c). It is deliberately NOT decomp-native;
// it will be replaced by the real sceneDraw one subsystem at a time.
//
// Present model: the real waitNextFrame() cannot present in our HAL (it sleeps on the
// video flip queue, which needs the retrace interrupt + video thread we don't have yet),
// so nothing downstream flips the frame. Everything gameLoop draws AFTER sceneRender
// (screens/UI/minimap/subtitles) is currently stubbed to no-ops, so the frame's visible
// content is complete when sceneRender returns - we present here via VIWaitForRetrace().
// When those subsystems become real, the present moves to a proper retrace model.

#include "port/gx_draw.h"
#include "port/renderer/rhi.h"
#include "port/vi_shim.h"
#include "port/dvd_shim.h"
#include "port/assets.h"
#include "port/plat_window.h"

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTransform.h"
#include "dolphin/gx/GXEnum.h"

#include <cstdio>
extern "C" int rhi_vita_presents __attribute__((weak));   /* run 126 [jmode]; global scope for the same reason */
#ifdef __vita__
extern "C" short gModelRootRotX, gModelRootRotY, gModelRootRotZ;   /* the game's (render.c), s16: mode 0x20 */
#endif
extern "C" int gStfxAnimArm, gStfxAnimTrace;   /* run 55 A/B arms; declared HERE, at global scope: the
   anim code below sits in an anonymous namespace, which hid them from vita_tool_main.c */
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>

struct StairfaxMapCell { int cellX, cellZ, blockId; };

extern "C" {
    int stairfax_mapcells_decode(int mapId, StairfaxMapCell* out, int maxOut,
                                 int* sizeX, int* sizeZ, int* originX, int* originZ);
    uint8_t* gxTexDecode(int fmt, int w, int h, const uint8_t* src);
    void VIWaitForRetrace(void);
    void stairfax_gamebit_selftest(void);
    void  stairfax_sky_init(void);
    float stairfax_sky_tick(void);
    void  vi_set_clear_color(float r, float g, float b);
    // spawned object system (object.c)
    void** gObjList; int gObjCount;
    void* Obj_GetActiveModel(void* obj);
    void* stairfax_spawn_player(int seq, float x, float y, float z);  // port-side player spawn
    void  stairfax_render_player(void* obj, int move, float progress); // real objRenderModel path
    extern float gPortViewMatrix[3][4];                              // port->real camera bridge
    float* Camera_GetViewMatrix(void);                               // real camera.c view matrix
    void*  Camera_GetCurrent(void);                                  // real view Camera (pos@0x0C..0x14)
    extern int gStairfaxRealCamActive;                               // 1 once the real follow cam is driving
    // host input (pad_input.c)
    int padGetStickX(int); int padGetStickY(int);
    int padGetCX(int);     int padGetCY(int);
    unsigned char padGetLTrigger(int); unsigned char padGetRTrigger(int);
    unsigned int getButtonsHeld(int);
}

#define MAP_BLOCK_WORLD_SIZE 640.0f

// vita fork: C-linkage OBJECTS must be declared OUTSIDE the anonymous namespace. Inside
// it a name has internal linkage (C++11 [basic.link]), which beats extern "C": GCC emits
// `(anonymous namespace)::gPlayerObj` and references `(anonymous namespace)::
// gModelAnimOffsetTable`, so neither links to the C side (game_player.c, model.c).
// MSVC evidently gives them C linkage anyway -- why the Windows build links.
extern "C" { uint8_t* gPlayerObj = nullptr; extern int* gModelAnimOffsetTable; }

namespace {

struct Block { AssetMapBlock mb; std::vector<RhiTexture*> texCache; int gid=-1; };
struct Placement { int block; float wx, wy, wz; };

RhiInstance* gRhi = nullptr;
std::vector<Block> gBlocks;
std::vector<Placement> gPlaces;
bool gInited = false;
bool gInitFailed = false;

// world bbox + a scale for camera speed
float gWorldCtr[3] = {0,0,0};
float gWorldR = 1000.0f;

// free-fly camera
float gCamPos[3];
float gCamYaw = 0.0f;
float gCamPitch = 0.35f;
bool  gCamPlaced = false;

// playable character (interim, port-side controller; real player.c not compiled yet)
// gPlayerObj: the spawned Sabre/Krystal GameObject -- defined above the namespace (vita fork)
static int gSceneBaseObjCount = -1;   // gObjCount before the player DLL spawns children (render-loop cap)
extern "C" int  stairfax_player_dll_enabled(void);   // game_player.c - real player DLL harness
extern "C" void stairfax_player_dll_tick(void);
extern "C" void stairfax_camera_set_pose(short yaw, short pitch, float x, float y, float z);  // camera shim
float    gFollowYaw = 0.0f;      // third-person camera orbit yaw (C-stick)
float    gFollowPitch = -0.28f;  // look slightly down at the character
float    gPlayerAnimPhase = 0.0f;// walk-cycle phase, advanced by movement speed
bool     gPlayerMoving = false;

void setupVtxFormats() {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   2);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   8);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT3, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX0, GX_TEX_ST,   GX_S16,   7);
    GXSetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   3);
    GXSetVtxAttrFmt(GX_VTXFMT5, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT5, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 8);
    GXSetVtxAttrFmt(GX_VTXFMT6, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   8);
    GXSetVtxAttrFmt(GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT6, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS,  GX_POS_XYZ,  GX_S16,   0);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA4, 0);
    for (int t=0;t<4;++t) GXSetVtxAttrFmt(GX_VTXFMT7, (GXAttr)(GX_VA_TEX0+t), GX_TEX_ST, GX_S16, 10);
}

// --- real object-model display (loaded via the real ObjModel_Load) ----------
extern "C" { void* ObjModel_Load(int id, int loadFlag, int* outSize); int stairfax_model_scan(int want);
    void ObjModel_InitResourceCaches(void); void ObjModel_InitScratchBuffers(void);
    void* stairfax_model_load_static(int id);
    void* stairfax_objmodel_load_guarded(int fid);   // full ObjModel_Load (anims), SEH-guarded
    void stairfax_bswap_model_moves(void* header);
    void stairfax_model_perdir(int on);   // viewer: resolve MODELS.tab per-dir (else root/global)
    int dvd_shim_findFile(const char* dir, const char* prefix, const char* suffix, char* out, int outSize);
    int Object_ObjAnimSetMove(void* objAnimHandle, int moveId, float moveProgress, unsigned char flags);
    void modelAnimEvalChannels(uint8_t* dst, void* model, void* channel, float blend, int flags);
    void PSMTXConcat(const float* a, const float* b, float* ab);
    void* getCache(void);
    void* stairfax_modeltex_rhi(void* modelTexHandle);   // resolve a render op's TEX1 texture
    }   // gModelAnimOffsetTable: declared above the namespace (vita fork)

// Minimal ObjModel/ObjAnimComponent/ObjAnimState harness wired to a model header, so the REAL
// objanim move API (Object_ObjAnimSetMove) selects a move + computes framePhase - the same path a
// spawned GameObject uses. Field offsets: ObjModel.file@0, currentState@0x2C, activeState@0x30;
// ObjAnimComponent.jointPoseData@0x6C, banks@0x7C, activeMoveProgress@0x9C, activeMove@0xA2,
// bankIndex@0xAD. The model header IS the ObjAnimDef (union), so bank->animDef = the header.
struct AnimHarness { uint8_t objModel[0x64]; uint8_t state[0x68]; uint8_t state2[0x68];
                     uint8_t objAnim[0xC0]; void* banks[1]; uint8_t poseData[0x400];
                     float dst[64][3][4]; };  // 64 = MAX_JOINTS (defined below)
static void harnessSetup(AnimHarness* H, uint8_t* header) {
    memset(H, 0, sizeof *H);
    *(void**)(H->objModel + 0x00) = header;      // file / animDef (union)
    *(void**)(H->objModel + 0x30) = H->state;    // activeState  (Object_ObjAnimSetMove writes here)
    *(void**)(H->objModel + 0x2C) = H->state2;   // currentState
    H->banks[0] = H->objModel;
    *(void**)(H->objAnim + 0x7C) = H->banks;     // banks
    *(void**)(H->objAnim + 0x6C) = H->poseData;  // jointPoseData
    H->objAnim[0xAD] = 0;                         // bankIndex
    *(int16_t*)(H->objAnim + 0xA2) = -1;          // activeMove (force move-change on first set)
}
// Advance the move through the real objanim API and return the resulting frame command + framePhase.
static uint8_t* harnessSetMove(AnimHarness* H, int moveId, float progress, float* outFramePhase) {
    Object_ObjAnimSetMove(H->objAnim, moveId, progress, 0);
    if (outFramePhase) *outFramePhase = *(float*)(H->state + 0x04);   // framePhase
    return *(uint8_t**)(H->state + 0x34);                             // moveFrameData
}
// modelLoadAnimations() dereferences gModelAnimOffsetTable, allocated only by
// ObjModel_InitResourceCaches. The port's model loads can run before the engine's own
// init reaches it, so ensure it once here before any ObjModel_Load.
static void ensureModelCaches() {
    if (!gModelAnimOffsetTable) { ObjModel_InitResourceCaches(); ObjModel_InitScratchBuffers(); }
}
static inline unsigned mdlBE32(const uint8_t* p){ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static inline unsigned mdlBE16(const uint8_t* p){ return ((unsigned)p[0]<<8)|p[1]; }
static inline float    mdlBEF32(const uint8_t* p){ union{unsigned u; float f;} x; x.u=mdlBE32(p); return x.f; }
#define MAX_JOINTS 64
static unsigned gFrameCounter = 0;   // advances once per sceneRender; drives the anim-test pose
// 3x4 model-space transform helpers (implicit [0 0 0 1] bottom row).
static inline void mat34Copy(const float s[3][4], float d[3][4]) {
    for (int r=0;r<3;++r) for (int c=0;c<4;++c) d[r][c]=s[r][c];
}
static inline void mat34Mul(const float a[3][4], const float b[3][4], float o[3][4]) {
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) o[r][c]=a[r][0]*b[0][c]+a[r][1]*b[1][c]+a[r][2]*b[2][c];
        o[r][3]=a[r][0]*b[0][3]+a[r][1]*b[1][3]+a[r][2]*b[2][3]+a[r][3];
    }
}
// Reversed from modelAnimBuildJointMatrices (stage 2, see src/port/docs/anim_playback_RE.md):
// three joint rotation angles -> a quaternion via the game's exact euler combine, then the
// standard quaternion -> 3x3 rotation, with `head` as the local translation. The retail code
// uses sin/cos polynomials of the half-angles; sinf/cosf is the same function. The angle SOURCE
// (real per-frame keyframe deltas) is the remaining prerequisite; here angles are passed in.
static void jointLocalMat34(float a0, float a1, float a2, const float head[3], float out[3][4]) {
    float c0=cosf(a0*0.5f), s0=sinf(a0*0.5f);
    float c1=cosf(a1*0.5f), s1=sinf(a1*0.5f);
    float c2=cosf(a2*0.5f), s2=sinf(a2*0.5f);
    float w = c0*c1*c2 + s0*s1*s2;
    float x = s0*c1*c2 - c0*s1*s2;
    float y = c0*s1*c2 + s0*c1*s2;
    float z = c0*c1*s2 - s0*s1*c2;
    out[0][0]=1-2*(y*y+z*z); out[0][1]=2*(x*y-w*z);   out[0][2]=2*(x*z+w*y);   out[0][3]=head[0];
    out[1][0]=2*(x*y+w*z);   out[1][1]=1-2*(x*x+z*z); out[1][2]=2*(y*z-w*x);   out[1][3]=head[1];
    out[2][0]=2*(x*z-w*y);   out[2][1]=2*(y*z+w*x);   out[2][2]=1-2*(x*x+y*y); out[2][3]=head[2];
}
// Read n bits MSB-first from byte stream p starting at bit offset bitPos (GC bitstream order).
static inline unsigned animReadBits(const uint8_t* p, int bitPos, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; ++i) {
        int b = bitPos + i;
        r = (r << 1) | ((p[b >> 3] >> (7 - (b & 7))) & 1);
    }
    return r;
}
// Reversed modelAnimBuildJointMatrices stages 1-3 (see anim_playback_RE.md), functional port:
// decode move `moveIdx` at fractional frame `frameF` from the (host-swapped) atlas, per joint
// producing 3 euler angles, then euler->quat->3x4 (jointLocalMat34) + FK. Fills jmtx[slot].
// Each of a joint's 3 rotation components = a u16 descriptor (base<<4|bitWidth): bitWidth 0 =
// constant (angle=base); else read bitWidth bits from the current + next frame streams in
// parallel, interpolate by the subframe fraction, scale (comp 0/1/2 -> x4/x2/x1), add to base.
static void decodeAtlas(uint8_t* a, float frameF, int moveIdx,
                        uint8_t* jd, int jc, float accum[][3], float jmtx[][3][4]) {
    if (!a) return;
    int streamOff = *(uint16_t*)(a + 2);
    int frameLen  = a[7];
    int frameStride = a[8];
    const uint16_t* desc = (const uint16_t*)(a + 10);   // host-order after the move swap
    if (frameLen < 1) frameLen = 1;
    int frame = (int)frameF; if (frame < 0) frame = 0; if (frame > frameLen - 1) frame = frameLen - 1;
    float frac = frameF - (float)frame; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    int fracFixed = (int)(frac * 16384.0f);
    const uint8_t* cur  = a + streamOff + frameStride * frame;
    const uint8_t* next = (frame + 1 < frameLen) ? cur + frameStride : cur;
    if (getenv("STAIRFAX_DESC_DUMP")) { static int once=0; if(!once){once=1;
        int nDesc=(streamOff-10)/2; fprintf(stderr,"[desc] move %d jc=%d nDesc=%d (jc*3=%d):\n",moveIdx,jc,nDesc,jc*3);
        for(int k=0;k<nDesc;++k){ unsigned dd=desc[k]; fprintf(stderr," %04x(b%d f%x)",dd,dd&0xf,(dd>>4)&3);
            if((k&7)==7)fprintf(stderr,"\n"); } fprintf(stderr,"\n"); }}
    int bitPos = 0, di = 0, nDesc = (streamOff - 10) / 2;
    int dbgAnim = 0; float dbgMin = 1e9f, dbgMax = -1e9f;
    // Decode one descriptor: bitWidth 0 = constant (angle=base); else read bitWidth bits from
    // the current + next frame streams in parallel, interpolate, scale, add base. Advances
    // di + bitPos. `scale` = channel weight (primary axis x4; extra channels x2 / x1).
    auto decodeOne = [&](int scale) -> int {
        if (di >= nDesc) return 0;
        unsigned d = desc[di++];
        int bw = d & 0xf;
        if (bw == 0) return (int16_t)d;
        int cv = (int)animReadBits(cur,  bitPos, bw);
        int nv = (int)animReadBits(next, bitPos, bw);
        bitPos += bw;
        int delta = (int)((unsigned)(nv - cv) << 18) >> 18;
        int interp = cv + ((delta * fracFixed) >> 14);
        ++dbgAnim;
        return (int16_t)(d & 0xfff0) + interp * scale;
    };
    static float A[MAX_JOINTS][3][4];
    for (int j = 0; j < jc; ++j) {
        float ang[3];
        // Three primary rotation axes (all x4). A primary descriptor with bit 0x10 set pulls one
        // extra secondary-channel descriptor (x2), which if it has 0x20 pulls a third (x1); those
        // are consumed to stay bit-aligned but don't drive the single-channel pose. (Flag-based
        // variable component encoding, reversed from modelAnimBuildJointMatrices .L_80007738.)
        for (int c = 0; c < 3; ++c) {
            unsigned d = (di < nDesc) ? desc[di] : 0;
            int sample = decodeOne(4);
            if (d & 0x10) { unsigned d1 = (di < nDesc) ? desc[di] : 0; decodeOne(2);
                if (d1 & 0x20) decodeOne(1); }
            ang[c] = (float)(int16_t)sample * (6.2831853f / 65536.0f);
            if (ang[c] < dbgMin) dbgMin = ang[c]; if (ang[c] > dbgMax) dbgMax = ang[c];
        }
        const uint8_t* b = jd + j * 0x1c; int parent = (int8_t)b[0];
        float head[3]; for (int c = 0; c < 3; ++c) head[c] = *(const float*)(b + 4 + c*4);
        float L[3][4]; jointLocalMat34(ang[0], ang[1], ang[2], head, L);
        if (parent >= 0 && parent < j) mat34Mul(A[parent], L, A[j]); else mat34Copy(L, A[j]);
        for (int r = 0; r < 3; ++r) { for (int cc = 0; cc < 3; ++cc) jmtx[j][r][cc] = A[j][r][cc];
            jmtx[j][r][3] = A[j][r][3] - (A[j][r][0]*accum[j][0] + A[j][r][1]*accum[j][1] + A[j][r][2]*accum[j][2]); }
    }
    if (getenv("STAIRFAX_MODEL_DBG")) { static int once=0; if(!once){once=1;
        fprintf(stderr, "[play] move %d: stride=%d bitsUsed=%d (of %d) desc=%d/%d animComps=%d "
                "angleRange=[%.1f,%.1f]deg\n", moveIdx, frameStride, bitPos, frameStride*8,
                di, nDesc, dbgAnim, dbgMin*57.2958f, dbgMax*57.2958f); }}
}

// Reversed modelRenderInterpolateRootTransform (render.c:407) - the ROOT-MOTION decode, the keystone
// that lets a move's root track drive real displacement (move distance / warp / height cues consumed
// by ObjModel_SampleJointTransform + player.c). Same interpolated-bitstream machine as decodeAtlas,
// pointed at the move's root slot: 3 channels, each a PRIMARY descriptor (base<<4|bitWidth) whose
// interpolated value -> outRotation[i] (14-bit sign-extended delta, x4), and, gated by the primary's
// 0x10 flag, a terminal SECONDARY/tertiary in the 0x10/0x20 chain -> outPosition[i] (natural signed
// delta, x1). bitWidth 0 -> 0 (retail sample-init; NOT base, unlike the joint path). The caller
// (model.c ObjModel_SampleJointTransform, compiled) set anim->frameStreamCursor/Stride/framePhase and
// swapped moveFrameData to the root slot; it post-scales outPos by 1/512 + root bind head * scale.
// GC packed-memory helpers become plain reads here: descriptors are host-swapped at load, the two
// parallel frame streams are read MSB-first big-endian via animReadBits. ObjAnimState offsets:
// framePhase@0x04, frameStreamCursor@0x2C, moveFrameData@0x34, frameStreamStride@0x4E.
extern "C" void modelRenderInterpolateRootTransform(void* animv, int16_t* outPosition, int16_t* outRotation) {
    uint8_t* anim = (uint8_t*)animv;
    for (int i = 0; i < 3; ++i) { outPosition[i] = 0; outRotation[i] = 0; }
    if (!anim) return;
    float framePhase = *(float*)(anim + 0x04);
    int16_t stride   = *(int16_t*)(anim + 0x4E);
    const uint8_t* cur = *(const uint8_t**)(anim + 0x2C);
    uint8_t* mfd       = *(uint8_t**)(anim + 0x34);
    if (!cur || !mfd) return;
    const uint16_t* desc = (const uint16_t*)(mfd + 4);           // = atlas+10, the descriptor table
    int streamOff = *(uint16_t*)(mfd - 4);                        // atlas+2 (host-swapped)
    int nDesc = (streamOff > 10) ? (streamOff - 10) / 2 : 0;
    const uint8_t* next = cur + stride;
    float fp = framePhase - floorf(framePhase);
    int fracFixed = (int)(fp * 16384.0f);
    int bitPos = 0, di = 0;
    // Decode one descriptor. bitWidth 0 -> 0. Else read bitWidth bits from the current+next frame
    // streams (MSB-first) and interpolate by the subframe fraction; sext = 14-bit sign-extend of the
    // delta (rotation), else natural signed (position); scale = channel weight (rotation x4, pos x1).
    auto readCh = [&](int scale, int sext) -> int {
        if (di >= nDesc) { ++di; return 0; }
        unsigned d = desc[di++];
        int bw = d & 0xf;
        if (bw == 0) return 0;
        int cv = (int)animReadBits(cur,  bitPos, bw);
        int nv = (int)animReadBits(next, bitPos, bw);
        bitPos += bw;
        int delta = nv - cv;
        if (sext) delta = (int)((unsigned)delta << 18) >> 18;   // 14-bit sign-extend
        int interp = cv + ((delta * fracFixed) >> 14);
        return (int)(int16_t)(d & 0xfff0) + interp * scale;
    };
    for (int i = 0; i < 3; ++i) {
        unsigned d = (di < nDesc) ? desc[di] : 0;
        outRotation[i] = (int16_t)readCh(4, 1);                  // primary -> rotation
        int pos = 0;
        if (d & 0x10) {
            /* Run 236 (ladder: "a step, then jitters down and back up"): as render.c:1781-1826. A second descriptor WITH
               0x10 is not a value: its bits are skipped, and without 0x20 the position is 0; with 0x20 the third one is
               the terminal. A second descriptor WITHOUT 0x10 is itself the terminal, and its 0x20 is part of its base.
               The old code read the second as a value in both cases and took a set 0x20 as "one more follows". */
            unsigned d1 = (di < nDesc) ? desc[di] : 0;
            int terminal = 1;
            if (d1 & 0x10) {
                bitPos += d1 & 0xf; ++di;
                if (!(d1 & 0x20)) terminal = 0;
            }
            if (terminal) pos = readCh(1, 0);
        }
        outPosition[i] = (int16_t)pos;
    }
}

// The reversed modelAnimBuildJointMatrices, with the REAL retail signature, so the real eval
// chain (modelAnimEvalChannels -> modelAnimUpdateChannels -> here, all in model.c) drives it.
// Reads the ObjAnimState `work`: moveFrameData@0x34 (= atlas+6), framePhase@0x04. Produces the
// per-joint 3x4 skinning matrices (M_j = A_j * T(-accum_j)) into `dst` (0x30 stride), which is
// exactly what the port's gx_draw_setJointMatrices consumes. Delegates to decodeAtlas.
// s16 slot angle -> radians (full circle = 65536), then the direct euler->3x4 rotation matrix
// exactly as retail's non-blend build (.L_80007980): rows below are the retail store order.
static void eulerMat34(int16_t a0s, int16_t a1s, int16_t a2s, const float head[3], float m[3][4]) {
    const float k = 6.28318530718f / 65536.0f;
    float c0=cosf(a0s*k), s0=sinf(a0s*k), c1=cosf(a1s*k), s1=sinf(a1s*k), c2=cosf(a2s*k), s2=sinf(a2s*k);
    m[0][0]=c1*c2;              m[0][1]=s0*c2*s1 - c0*s2;  m[0][2]=c0*c2*s1 + s0*s2;  m[0][3]=head[0];
    m[1][0]=c1*s2;              m[1][1]=s0*s2*s1 + c0*c2;  m[1][2]=c0*s2*s1 - s0*c2;  m[1][3]=head[1];
    m[2][0]=-s1;               m[2][1]=s0*c1;             m[2][2]=c0*c1;             m[2][3]=head[2];
}
// Faithful port of the retail modelAnimBuildJointMatrices non-blend path (mode 0, eventCountdown<=0),
// which is what a spawned character's steady render uses (verified: mode=0 flags=0x7f). Stages:
//   1. decode this channel's packed keyframes into a per-slot buffer (.L_800074EC)
//   2. add the base pose from the scratch fixup table (.L_800075FC, filled by BuildAnimBlendTable)
//   3. per bone, build a local matrix from its idx-selected slot (.L_80007980) into bank[idx0&0x7f]
//   4. forward kinematics: bank[bone] = bank[parent] * bank[bone]  (.L_80007D74)
// ModelBone (stride 0x1c): parent s8@0, idx0 u8@1 (bank slot, & flags), idx1 u8@2 (rotation slot),
// idx2 u8@3, head f32[3]@4 (BE on disc). Slot buffer: 0x40 stride, rot s16 @0/2/4, secondary @0xc..,
// translation @0x18.. . Descriptors: moveFrameData[0]=slotCount, u16 descriptors from +4.
#define ANIM_MAX_SLOTS 128
static int16_t gAnimSlots[ANIM_MAX_SLOTS][0x20];   // 0x40 bytes = 0x20 s16 per slot
/* run 55 A/B (docs/47): bit 0 = retail slot translation, bit 1 = retail column scale. The trace
   prints the raw decoded terms for one model when armed (vita_tool_main.c, capture frame). */
#define JM_ROOT_ROT_DECLARED 1   /* line 34 of this file */
#define JM_MAX_SLOTS ANIM_MAX_SLOTS
#include "jointmtx_modes.inc"   /* the retail routine's other paths (host/tests/jointmtx_modes_draft.c is the tested copy) */
extern "C" { unsigned stfx_jm_other_modes; }
extern "C" void modelAnimBuildJointMatrices(int* out, uint8_t* dst, void* work, uint8_t* jd,
                                            int jc, uint8_t* scratch, int flags, uint8_t mode) {
    if (!out || !jd || jc <= 0 || jc > MAX_JOINTS) return;
    uint8_t* bank = *(uint8_t**)out;               // model->jointMatrices[buf], stride 0x40
    if (!bank) bank = dst;
    // The render builds gPosMtx from getCache() pos-matrix slots (renderOpMatrix fills only slots
    // 0..jointCount+extra); a render-op posMtx index past that reads a STALE slot from a prior
    // object's render (seen as a ~1e27 matrix -> the player vertices shoot off). Reset the pos
    // region to identity so any unrebuilt slot is inert instead of garbage. Region [0,0x2700) is
    // the pos/normal matrices; the joint bank staged at +0x2700 is untouched.
    /* run 159 (docs/47): retail has no such wipe, and a joint build that lands INSIDE another model's draw (a mounted rider)
       resets the matrices that draw is still reading. ux0:data/stairfax/nocachewipe.txt starting with 1 = no wipe (A/B). */
    static int sNoWipe = -1;
    if (sNoWipe < 0) { sNoWipe = 0;
#ifdef __vita__
        FILE* wf = fopen("ux0:data/stairfax/nocachewipe.txt", "r"); sNoWipe = wf && fgetc(wf) == 49; if (wf) fclose(wf);
        printf("[cachewipe] pos-matrix staging wipe at every joint build: %s\n", sNoWipe ? "OFF (nocachewipe.txt)" : "on");
#endif
    }
    if (uint8_t* cache = sNoWipe ? (uint8_t*)0 : (uint8_t*)getCache()) {
        for (int i = 0; i < 0x2700; i += 0x30) { float* M=(float*)(cache+i);
            for (int k=0;k<12;++k) M[k]=0; M[0]=M[5]=M[10]=1.0f; }
    }
    // Clear every joint-bank slot to identity before decoding: a bone whose bank slot no bone
    // writes (an idx gap), and the extra vertex-group slots that modelCalcVtxGroupMtxs blends
    // from, must be inert instead of stale garbage (a stale slot rendered as a ~1e27 matrix ->
    // spike). total = jointCount + extraJointCount from the model header.
    int total = jc;
    if (gPlayerObj) { uint8_t* am=(uint8_t*)Obj_GetActiveModel(gPlayerObj);
        if (am) { uint8_t* file=*(uint8_t**)am; if (file) total = file[0xF3] + file[0xF4]; } }
    if (total < jc || total > 200) total = jc;
#ifndef __vita__   /* the Vita runs the game's own renderer: retail does NOT clear the bank. A bone whose
                      (s8)idx0 & flags is negative is SKIPPED (its slot keeps what other code wrote,
                      e.g. a look-at head); clearing it to identity broke that (Vita run 52). */
    for (int j = 0; j < total; ++j) { float* M=(float*)(bank+j*0x40);
        for (int k=0;k<12;++k) M[k]=0; M[0]=M[5]=M[10]=1.0f; }
#endif
#ifdef __vita__
    /* run 126 (docs/47): WHICH path does each model ask for? One line per distinct (bones, mode,
       flags, fade running) -- retail's builder branches on mode 0x40 / 0x1 / 0x2 / 0xC / 0x10 /
       0x20 and on eventCountdown (work+0x58) > 0, and this twin does the single pose only.
       `n12` = bones whose idx1 != idx2 (the two blend sources index differently there). The
       table's occupancy and drops are printed, so silence is not "not reached". */
    {
        static struct { const uint8_t* jd; int flags; uint8_t mode, fade; } s_seen[96];
        static int s_n, s_drop;
        int16_t cd = *(int16_t*)((uint8_t*)work + 0x58);
        uint8_t fade = cd > 0;
        int i = 0;
        /* 2026-10-12: with the census table full (run 237: 163,841 drops in ThornTail) every call used to scan all 96 entries and
           still find nothing; diagnostic only, so once full it costs one compare, and says so every 65,536th call. */
        if (s_n >= 96) { if (!(s_drop++ & 0xFFFF)) printf("[jmode] table FULL, drops %d\n", s_drop); i = s_n + 1; }
        for (; i < s_n; ++i)
            if (s_seen[i].jd == jd && s_seen[i].mode == mode && s_seen[i].flags == flags && s_seen[i].fade == fade) break;
        if (i == s_n) {
            if (s_n < 96) {
                int n12 = 0;
                for (int j = 0; j < jc; ++j) if (jd[j * 0x1c + 2] != jd[j * 0x1c + 3]) ++n12;
                uint8_t* m0 = *(uint8_t**)((uint8_t*)work + 0x34);
                uint8_t* m1 = *(uint8_t**)((uint8_t*)work + 0x38);
                s_seen[s_n].jd = jd; s_seen[s_n].mode = mode; s_seen[s_n].flags = flags; s_seen[s_n].fade = fade; ++s_n;
                /* run 126 crashed HERE reading m1[0]: with one channel in use, work+0x38 is the caller's
                   uninitialised stack (retail's too). Pointers only; channel 1 is real only if cd > 0. */
                printf("[jmode] %d/96 jd %p jc %d mode %02x flags %x countdown %d n12 %d data %p %p phase %.3f %.3f present %d drops %d\n",
                       s_n, (void*)jd, jc, mode, flags, cd, n12, (void*)m0, (void*)m1,
                       *(float*)((uint8_t*)work + 0x04), *(float*)((uint8_t*)work + 0x08), rhi_vita_presents, s_drop);
            } else if (!(s_drop++ & 1023)) printf("[jmode] table FULL, drops %d\n", s_drop);
        }
    }
#endif
    /* THE OTHER MODES (2026-10-10, docs/47 "the blend modes"): 0x04/0x08 store quaternion poses in the bank, 0x01/0x02/0x03
       blend stored poses, 0x40 is the cache pass (model.c:436/473/507 and the 0x14 -> 0x18 -> 7 -> 1 chain of
       ObjModel_UpdateAnimMatrices, model.c:2100). Until now 0x04/0x08 returned with the bank untouched and the rest fell
       into the single-pose decode. jointmtx_modes.inc is the retail routine's other paths, checked instruction for
       instruction against the Rev 1 function under emulation (host/tests/jointmtx_modes_model.py); NOT yet against a
       console or Dolphin, and no content reached so far asks for them. Modes 0x00/0x10/0x20 are not touched.
       ux0:data/stairfax/jm_modes_off.txt starting with 1 = the old behaviour. */
    {   static int sOff = -1;
        if (sOff < 0) { sOff = 0;
#ifdef __vita__
            FILE* jf = fopen("ux0:data/stairfax/jm_modes_off.txt", "r"); sOff = jf && fgetc(jf) == 49; if (jf) fclose(jf);
            printf("[jm] joint-builder modes other than 00/10/20: %s\n", sOff ? "OFF (jm_modes_off.txt): as before" : "retail paths (jointmtx_modes.inc)");
#endif
        }
        if (sOff) { if (mode & 0x0C) return; }
        else if (jm_twin_dispatch(bank, dst, work, jd, jc, scratch, flags, mode, (uint8_t*)gAnimSlots, MAX_JOINTS)) { ++stfx_jm_other_modes; return; }
    }
    uint8_t* mfd = *(uint8_t**)((uint8_t*)work + 0x34);   // moveFrameData (descriptors)
    const uint8_t* cur = *(const uint8_t**)((uint8_t*)work + 0x2c);  // frame stream cursor
    int16_t stride = *(int16_t*)((uint8_t*)work + 0x4c);            // frame stream stride (bytes)
    float framePhase = *(float*)((uint8_t*)work + 0x04);
    if (!mfd) return;
#ifdef __vita__
    /* run 127 (docs/47): the 35-bone model's (the CloudRunner's) INPUTS and stages at the capture
       presents, raw memory bytes, in the blocks stfx_frame.py --jcensus takes from retail (JWORK
       JBONES JFIX JDESC JSTREAM JDST, then JSLOTS and JBANK below). host/tests/jointmtx_model.py
       replays them: run 126 showed retail's single pose reproduced exactly from retail's inputs,
       so what differs here is an input. Armed for a present in captures.txt and the one before. */
    int jvec = 0;
    {
        static int s_caps[32], s_n = -1;
        if (s_n < 0) { s_n = 0; FILE* cf = fopen("ux0:data/stairfax/captures.txt", "r");
            if (cf) { while (s_n < 32 && fscanf(cf, "%d", &s_caps[s_n]) == 1) s_n++; fclose(cf); } }
        int p = &rhi_vita_presents ? rhi_vita_presents : -1;
        if (jc == 35) for (int i = 0; i < s_n; ++i) if (p == s_caps[i] || p == s_caps[i] - 1) jvec = 1;
    }
    auto jdump = [&](const char* kind, const void* ptr, int n) {
        /* 64 bytes a line, ONE printf a line: run 127 printed a byte per printf (each one a file
           open/close on the device: the frame took minutes) and the log layer split the long lines */
        const uint8_t* q = (const uint8_t*)ptr;
        static const char hx[] = "0123456789abcdef";
        for (int o = 0; q && o < n; o += 64) {
            char line[130]; int m = n - o < 64 ? n - o : 64;
            for (int i = 0; i < m; ++i) { line[i * 2] = hx[q[o + i] >> 4]; line[i * 2 + 1] = hx[q[o + i] & 15]; }
            line[m * 2] = 0;
            printf("[jvec] %d %s %d %s\n", &rhi_vita_presents ? rhi_vita_presents : -1, kind, o, line);
        }
    };
    if (jvec) {
        printf("[jvec] %d JCALL mode %02x flags %x jd %p bank %p dst %p\n", rhi_vita_presents, mode, flags, (void*)jd, (void*)bank, (void*)dst);
        jdump("JWORK", work, 0x68); jdump("JBONES", jd, jc * 0x1c); jdump("JFIX", scratch, 0x140);
        jdump("JDESC", mfd, 800); jdump("JSTREAM", cur, 256); jdump("JDST", dst, 48);
    }
#endif
    const uint8_t* nxt = cur ? cur + stride : cur;
    float frac = framePhase - floorf(framePhase); if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    int fracFixed = (int)(frac * 16384.0f);
#ifdef __vita__
    /* Vita run 56: the move descriptors are BIG-ENDIAN as on the disc (retail reads them with lhz,
       0x80007534). Read little-endian (this probe unset) they decoded to scales of 42982/1024 and
       translations of -25403/512 where retail's slot buffer (Dolphin, 0x802C3D00) holds 0 and a few
       units; values like 0x0900/0x1800/0x7200 were the byte-swap's fingerprint. */
    int be = 1;
#else
    int be = getenv("STAIRFAX_ANIM_BE") != 0;      // descriptor endianness probe
#endif

    // Stage 1: decode N slots into gAnimSlots (cleared first so the base-pose fixup lands cleanly
    // on un-animated slots, and un-decoded components stay identity/zero).
    memset(gAnimSlots, 0, sizeof gAnimSlots);
    int nSlots = mfd[0]; if (nSlots > ANIM_MAX_SLOTS) nSlots = ANIM_MAX_SLOTS;
    const uint8_t* dp = mfd + 4;
    int bitPos = 0;
    auto rdDesc = [&]() -> unsigned { unsigned v = be ? ((dp[0]<<8)|dp[1]) : (dp[0]|(dp[1]<<8)); dp += 2; return v; };
    auto sample = [&](int bw) -> int {
        if (!cur) return 0;
        int cv = (int)animReadBits(cur, bitPos, bw);
        int nv = (int)animReadBits(nxt, bitPos, bw);
        bitPos += bw;
        return cv + (((((int)((unsigned)(nv - cv) << 18)) >> 18) * fracFixed) >> 14);
    };
    auto sample16 = [&](int bw) -> int {   // retail's translation path: 16-bit delta sign extension
        if (!cur) return 0;
        int cv = (int)animReadBits(cur, bitPos, bw);
        int nv = (int)animReadBits(nxt, bitPos, bw);
        bitPos += bw;
        return cv + (((((int)((unsigned)(nv - cv) << 16)) >> 16) * fracFixed) >> 14);
    };
    for (int s = 0; s < nSlots; ++s) {
        int16_t* slot = gAnimSlots[s];
        for (int c = 0; c < 3; ++c) {
            unsigned d = rdDesc(); int bw = d & 0xf;
            int val = (int16_t)(d & 0xfff0);
            if (bw) val = (int16_t)(d & 0xfff0) + (sample(bw) << 2);   // primary: x4
            slot[c] = (int16_t)val;                                    // rotation @ +0/+2/+4
#ifdef __vita__
            /* Retail Rev 1 0x800075CC-0x80007734 (docs/47 run 54): after a rotation descriptor with
               0x10, the NEXT descriptor is a SCALE only if IT has 0x10 (then its 0x20 says a
               translation follows); WITHOUT 0x10 it is directly the TRANSLATION. The code below
               (#else) always took it as a scale, so a translation-only component landed in the
               scale slot. Translation deltas sign-extend from 16 bits, not 18. */
            if (d & 0x10) {
                unsigned d2 = rdDesc();
                int transDesc = -1;
                if (d2 & 0x10) {                                       // scale @ +0xc..
                    int bw2 = d2 & 0xf;
                    int v2 = (int16_t)(d2 & 0xffc0);
                    if (bw2) v2 = (int16_t)(d2 & 0xffc0) + (sample(bw2) << 1);   // x2
                    slot[6 + c] = (int16_t)v2;
                    if (d2 & 0x20) transDesc = (int)rdDesc();
                } else transDesc = (int)d2;
                if (transDesc >= 0) {                                  // translation @ +0x18..
                    unsigned d3 = (unsigned)transDesc; int bw3 = d3 & 0xf;
                    int v3 = (int16_t)(d3 & 0xfff0);
                    if (bw3) v3 = (int16_t)(d3 & 0xfff0) + sample16(bw3);    // x1
                    slot[12 + c] = (int16_t)v3;
                }
            }
#else
            if (d & 0x10) {                                            // secondary (scale) @ +0xc..
                unsigned d2 = rdDesc(); int bw2 = d2 & 0xf;
                int v2 = (int16_t)(d2 & 0xffc0);
                if (bw2) v2 = (int16_t)(d2 & 0xffc0) + (sample(bw2) << 1);   // x2
                slot[6 + c] = (int16_t)v2;
                if (d2 & 0x20) {                                       // tertiary (translation) @ +0x18..
                    unsigned d3 = rdDesc(); int bw3 = d3 & 0xf;
                    int v3 = (int16_t)(d3 & 0xfff0);
                    if (bw3) v3 = (int16_t)(d3 & 0xfff0) + sample(bw3);      // x1
                    slot[12 + c] = (int16_t)v3;
                }
            }
#endif
        }
    }

    // Stage 2: add the base pose (scratch fixup): entries {u16 slotByteOffset, pad, s16 addend, pad},
    // stride 8, terminated by slotByteOffset == 0x1000.
    if (scratch) {
        const uint8_t* e = scratch;
        for (int guard = 0; guard < ANIM_MAX_SLOTS * 12; ++guard) {
            unsigned off = *(const uint16_t*)(e + 0);
            if (off == 0x1000) break;
            if (off < sizeof gAnimSlots - 1)
                *(int16_t*)((uint8_t*)gAnimSlots + off) += *(const int16_t*)(e + 4);
            e += 8;
        }
    }

#ifdef __vita__
    /* THE CROSS-FADE (run 129; retail asm @L_80006D38 -> @blendJoints, mode 0 with eventCountdown > 0).
       Modelled first on the host from the assembly and exact against retail's own inputs and outputs
       (host/tests/jointmtx_model.py: 0 slot fields, 7e-6 on the bank, hits 4900/4901/4931). Channel 1
       decodes into the SAME slots 6 bytes on (s16 index +3: rot 3..5, scale 9..11, translation 15..17);
       its fixups are the table's second columns (offset @2, addend @6); scale and translation lerp in
       integers back into channel 0's slot; rotation is a quaternion nlerp, built in stage 3.
       NOT ported (and logged by [jmode]): modes 0x1/0x2/0x4/0x8 (stored-quaternion passes) and 0x40. */
    int16_t fadeCd = *(int16_t*)((uint8_t*)work + 0x58);
    bool fade = fadeCd > 0 && !(mode & 0x4f);
    {
        static int s_nofade = -1;   /* break arm: ux0:data/stairfax/jnt_nofade.txt = the old single pose */
        if (s_nofade < 0) { FILE* nf = fopen("ux0:data/stairfax/jnt_nofade.txt", "r"); s_nofade = nf ? 1 : 0; if (nf) fclose(nf);
            printf("[jfade] cross-fade %s\n", s_nofade ? "OFF (jnt_nofade.txt)" : "ON"); }
        if (s_nofade) fade = false;
    }
    if (fade) {
        uint8_t* mfd1 = *(uint8_t**)((uint8_t*)work + 0x38);
        const uint8_t* cur1 = *(const uint8_t**)((uint8_t*)work + 0x30);
        int16_t stride1 = *(int16_t*)((uint8_t*)work + 0x4e);
        float ph1 = *(float*)((uint8_t*)work + 0x08);
        if (!mfd1 || !cur1) fade = false;
        else {
            const uint8_t* nxt1 = cur1 + stride1;
            int frac1 = (int)((ph1 - floorf(ph1)) * 16384.0f);
            int n1 = mfd1[0]; if (n1 > ANIM_MAX_SLOTS) n1 = ANIM_MAX_SLOTS;
            const uint8_t* d1 = mfd1 + 4; int bit1 = 0;
            auto rd1 = [&]() -> unsigned { unsigned v = (unsigned)((d1[0] << 8) | d1[1]); d1 += 2; return v; };
            auto smp = [&](int bw, int sx) -> int {   /* sx: sign-extend the frame delta from this many bits (0: none) */
                int cv = (int)animReadBits(cur1, bit1, bw), nv = (int)animReadBits(nxt1, bit1, bw);
                bit1 += bw;
                int dl = nv - cv; if (sx) dl = (int)((unsigned)dl << (32 - sx)) >> (32 - sx);
                return cv + ((dl * frac1) >> 14);
            };
            for (int s = 0; s < n1; ++s) {
                int16_t* sl = gAnimSlots[s] + 3;
                for (int c = 0; c < 3; ++c) {
                    unsigned d = rd1(); int bw = d & 0xf; int val = (int)(d & 0xfff0);
                    if (bw) val += smp(bw, 14) << 2;
                    sl[c] = (int16_t)val;
                    if (d & 0x10) {
                        unsigned d2 = rd1(); int td = -1;
                        if (d2 & 0x10) {
                            int bw2 = d2 & 0xf; int v2 = (int)(d2 & 0xffc0);
                            if (bw2) v2 += smp(bw2, 0) << 1;
                            sl[6 + c] = (int16_t)v2;
                            if (d2 & 0x20) td = (int)rd1();
                        } else td = (int)d2;
                        if (td >= 0) {
                            int bw3 = td & 0xf; int v3 = td & 0xfff0;
                            if (bw3) v3 += smp(bw3, 16);
                            sl[12 + c] = (int16_t)v3;
                        }
                    }
                }
            }
            if (scratch) {
                const uint8_t* e = scratch;
                for (int guard = 0; guard < ANIM_MAX_SLOTS * 12; ++guard) {
                    unsigned off = *(const uint16_t*)(e + 2);
                    if (off == 0x1000) break;
                    if (off + 6 < sizeof gAnimSlots - 1)
                        *(int16_t*)((uint8_t*)gAnimSlots + 6 + off) += *(const int16_t*)(e + 6);
                    e += 8;
                }
            }
            if (mode & 0x20) { gAnimSlots[0][3] = gModelRootRotX; gAnimSlots[0][4] = gModelRootRotY; gAnimSlots[0][5] = gModelRootRotZ; }
            for (int j = 0; j < jc; ++j) {          /* scale + translation, in place in channel 0's slot */
                const uint8_t* b = jd + j * 0x1c;
                if (b[2] >= ANIM_MAX_SLOTS || b[3] >= ANIM_MAX_SLOTS) continue;
                int16_t* sa = gAnimSlots[b[2]]; const int16_t* sb = gAnimSlots[b[3]] + 3;
                for (int c = 0; c < 3; ++c) {
                    int scA = (uint16_t)sa[6 + c], scB = (uint16_t)sb[6 + c];
                    if (!scA) scA = 0x400; if (!scB) scB = 0x400;
                    sa[6 + c] = (int16_t)(scA + (((scB - scA) * fadeCd) >> 14));
                    int tA = sa[12 + c];
                    if (!(mode & 0x10)) tA += ((sb[12 + c] - tA) * fadeCd) >> 14;
                    sa[12 + c] = (int16_t)tA;
                }
            }
        }
    }
    if (jvec) jdump("JSLOTS", gAnimSlots, jc * 0x40);
#endif
    // Stage 3: per bone, build local matrix into bank[idx0 & 0x7f].
    for (int j = 0; j < jc; ++j) {
        const uint8_t* b = jd + j * 0x1c;
        int outSlot = b[1] & 0x7f;
        int rotSlot = b[2];
        if (rotSlot >= ANIM_MAX_SLOTS) rotSlot = 0;
        const int16_t* slot = gAnimSlots[rotSlot];
        float head[3]; for (int c = 0; c < 3; ++c) head[c] = *(const float*)(b + 4 + c * 4);  // host order (swapped at load)
#ifdef __vita__
        /* Retail non-blend build, Rev 1 0x80007980-0x80007D70 (docs/47 run 53), which the lines
           below had only partly: (1) a bone with (s8)idx0 & flags < 0 is skipped, slot untouched;
           (2) translation = head + slot s16 @+0x18/0x1a/0x1c * 1/512 (r2-32744); (3) each rotation
           COLUMN is scaled by slot u16 @+0xc/0xe/0x10 * 1/1024 (r2-32740), zero = unscaled.
           The rotation itself (eulerMat34) matches retail term for term. */
        if (((int)(int8_t)b[1] & flags) < 0) continue;
        {
            float (*M)[4] = (float(*)[4])(bank + ((int)(int8_t)b[1] & flags) * 0x40);
            if (fade && b[3] < ANIM_MAX_SLOTS) {
                /* euler -> quaternion per channel at HALF angles (asm @buildRotation: srawi 1), sign-matched,
                   lerped by countdown/16384 and NOT renormalised; channel 1's slot is the bone's byte 3 */
                const float kh = 6.28318530718f / 65536.0f;
                auto quat = [&](const int16_t* r, float q[4]) {
                    float h0 = (float)(r[0] >> 1) * kh, h1 = (float)(r[1] >> 1) * kh, h2 = (float)(r[2] >> 1) * kh;
                    float c0 = cosf(h0), s0 = sinf(h0), c1 = cosf(h1), s1 = sinf(h1), c2 = cosf(h2), s2 = sinf(h2);
                    q[0] = c0 * c1 * c2 + s0 * s1 * s2; q[1] = s0 * c1 * c2 - c0 * s1 * s2;
                    q[2] = c0 * s1 * c2 + s0 * c1 * s2; q[3] = c0 * c1 * s2 - s0 * s1 * c2;
                };
                float qa[4], qb[4]; quat(slot, qa); quat(gAnimSlots[b[3]] + 3, qb);
                float w = (float)fadeCd * (1.0f / 16384.0f);
                if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0.0f) for (int i = 0; i < 4; ++i) qb[i] = -qb[i];
                float qw = qa[0] * (1.0f - w) + qb[0] * w, qx = qa[1] * (1.0f - w) + qb[1] * w;
                float qy = qa[2] * (1.0f - w) + qb[2] * w, qz = qa[3] * (1.0f - w) + qb[3] * w;
                M[0][0] = 1 - 2 * (qy * qy + qz * qz); M[0][1] = 2 * (qx * qy - qw * qz);     M[0][2] = 2 * (qx * qz + qw * qy);
                M[1][0] = 2 * (qx * qy + qw * qz);     M[1][1] = 1 - 2 * (qx * qx + qz * qz); M[1][2] = 2 * (qy * qz - qw * qx);
                M[2][0] = 2 * (qx * qz - qw * qy);     M[2][1] = 2 * (qy * qz + qw * qx);     M[2][2] = 1 - 2 * (qx * qx + qy * qy);
                for (int c = 0; c < 3; ++c) {
                    M[c][3] = head[c] + (float)slot[12 + c] * (1.0f / 512.0f);
                    uint16_t sc = (uint16_t)slot[6 + c];   /* never 0 after the lerp above (0 reads 0x400) */
                    if (sc) { float f = (float)sc * (1.0f / 1024.0f); M[0][c] *= f; M[1][c] *= f; M[2][c] *= f; }
                }
                continue;
            }
            float th[3];
            for (int c = 0; c < 3; ++c) th[c] = head[c] + ((gStfxAnimArm & 1) ? (float)slot[12 + c] * (1.0f / 512.0f) : 0.0f);
            eulerMat34(slot[0], slot[1], slot[2], th, M);
            if (gStfxAnimArm & 2) for (int c = 0; c < 3; ++c) {
                uint16_t sc = (uint16_t)slot[6 + c];
                if (sc) { float f = (float)sc * (1.0f / 1024.0f); M[0][c] *= f; M[1][c] *= f; M[2][c] *= f; }
            }
            if (gStfxAnimTrace > 0 && j < 24)   /* run 55: the raw terms, first model of the frame */
                printf("[anim] jc %d j %d par %d idx %u/%u rot %d %d %d | scale %u %u %u | trans %d %d %d | head %.2f %.2f %.2f | flags %x mode %x\n",
                       jc, j, (int8_t)b[0], b[1], b[2], slot[0], slot[1], slot[2], (uint16_t)slot[6], (uint16_t)slot[7], (uint16_t)slot[8],
                       slot[12], slot[13], slot[14], head[0], head[1], head[2], flags, mode);
        }
#else
        eulerMat34(slot[0], slot[1], slot[2], head, (float(*)[4])(bank + outSlot * 0x40));
#endif
    }
    if (gStfxAnimTrace > 0) gStfxAnimTrace--;
#ifdef __vita__
    /* THE OVERLAY PASS (run 255). ObjModel_UpdateAnimMatrices runs this builder a second time with flags -1 for a secondary
       move (staff draw / sheath: anim.activeMove, model.c:2111): bit 7 of a bone's idx0 then MASKS it, so only the upper body
       is rebuilt. Stages 4 + 5 below are "model-space FK, then dst prepended to every bone", which equals retail only when
       every bone is rebuilt: on the overlay the first unmasked bone's parent is a masked bone that the first pass already left
       in WORLD space, so stage 4 gave world * local and stage 5 multiplied dst in again -- the upper body placed by the
       object's position and yaw twice (Tyler: "the top half of Fox's body stretches to oblivion" on staff out / away).
       Retail's order (@hierarchy, jointmtx_modes.inc jm_hierarchy): in bone order, a masked bone is skipped, bone 0's parent
       is dst, every other bone's parent is its u8 bank index. dst therefore enters once, through bone 0, or not at all.
       Only the masked pass takes this path; the full pass (flags 0x7f) keeps the stages that were checked against retail. */
    bool stfxOverlayDone = false;
    if (flags & 0x80) {
        for (int j = 0; j < jc; ++j) {
            const uint8_t* b = jd + j * 0x1c;
            int m = (int)(int8_t)b[1] & flags;
            if (m < 0) continue;
            float* M = (float*)(bank + (j == 0 ? (b[1] & 0x7f) : m) * 0x40);
            const float* P = (j == 0) ? (const float*)dst : (const float*)(bank + b[0] * 0x40);
            if (!P) continue;
            float t[12]; PSMTXConcat((float*)P, M, t);
            for (int i = 0; i < 12; ++i) M[i] = t[i];
        }
        stfxOverlayDone = true;
        { static int sSaid; if (!sSaid) { sSaid = 1; printf("[jm] overlay pass (flags %x, mode %x, %d bones): retail hierarchy order, dst through bone 0 only\n", (unsigned)flags, (unsigned)mode, jc); } }
    }
    if (!stfxOverlayDone) {
#endif
    // Stage 4: forward kinematics in place (bones ordered parent-before-child). ModelBone.parent
    // (@0) is the parent's BANK SLOT index directly (retail: parent<<6 into the bank), not an array
    // index; a negative parent marks a root (local matrix already world).
    for (int j = 0; j < jc; ++j) {
        const uint8_t* b = jd + j * 0x1c;
        int parent = (int8_t)b[0];
        int outSlot = b[1] & 0x7f;
        if (parent < 0 || parent == outSlot) continue;
#ifdef __vita__
        if (((int)(int8_t)b[1] & flags) < 0) continue;   /* retail FK skips it too (0x80007DF8) */
#endif
        float* M = (float*)(bank + outSlot * 0x40);
        float* P = (float*)(bank + parent * 0x40);
        float t[12]; PSMTXConcat(P, M, t);
        for (int i = 0; i < 12; ++i) M[i] = t[i];
    }
    // Stage 5: place the (model-space) skeleton into the world. dst is the object world matrix wm
    // (Obj_BuildWorldTransformMatrix -> setMatrixFromObjectTransposed), passed as arg2 by the real
    // modelAnimEvalChannels/ObjModel_UpdateAnimMatrices. Without this the bones stay at the model
    // origin and the character renders ~2000u from the camera (tiny, off-screen). Prepend wm to each
    // bone once: worldBone = wm * modelBone. (extra vtx-group slots inherit it via modelCalcVtxGroupMtxs.)
    if (dst && (uint8_t*)dst != bank) {
        unsigned char applied[MAX_JOINTS] = {0};
        for (int j = 0; j < jc; ++j) {
            int slot = jd[j * 0x1c + 1] & 0x7f;
            if (slot >= MAX_JOINTS || applied[slot]) continue;
#ifdef __vita__
            if (((int)(int8_t)jd[j * 0x1c + 1] & flags) < 0) continue;
#endif
            applied[slot] = 1;
            float* M = (float*)(bank + slot * 0x40);
            float t[12]; PSMTXConcat((float*)dst, M, t);
            for (int i = 0; i < 12; ++i) M[i] = t[i];
        }
    }
#ifdef __vita__
    }   /* !stfxOverlayDone */
    if (jvec) jdump("JBANK", bank, jc * 0x40);
#endif
    // The extra vertex-group joints (bank[jointCount..]) are filled by the real modelCalcVtxGroupMtxs
    // in modelInitMtxs after this returns (world * inverse-bind blends of two main joints); the bank
    // clear above leaves them identity until then.
}

struct Bits { const uint8_t* d; int pos; int bitLen; };
static int mdlBits(Bits* b,int n){ int pos=b->pos,off=pos>>3; unsigned w=b->d[off]|(b->d[off+1]<<8)|(b->d[off+2]<<16); b->pos=pos+n; return (int)((w>>(pos&7))&((1u<<n)-1)); }

struct DispModel { uint8_t* h; float wx,wy,wz,scale; };
static std::vector<DispModel> gDispModels;

// Validate a POS layout (posOff/posSz/stride) against a DL: walk it as GX command stream,
// require every POS index < vertexCount and the stream to end exactly aligned. Mirrors
// model_view's validateLayout - a skinned model whose descriptor-derived stride is wrong
// fails this, and detectStride finds the true stride.
static bool validateStride(const uint8_t* dl, int dlSize, int posOff, int posSz, int stride, int vc) {
    int p = 0, saw = 0;
    while (p < dlSize) {
        unsigned op = dl[p]; if (op == 0) { ++p; continue; }
        if ((op & 0x80) == 0) return false;
        if (p + 3 > dlSize) return false;
        int cnt = (int)mdlBE16(dl + p + 1); p += 3;
        if (cnt == 0 || p + cnt*stride > dlSize) return false;
        for (int v = 0; v < cnt; ++v) {
            const uint8_t* vp = dl + p + v*stride;
            int pi = posSz==2 ? (int)mdlBE16(vp+posOff) : vp[posOff];
            if (pi >= vc) return false;
        }
        p += cnt*stride; saw = 1;
    }
    return saw != 0;
}
static int detectStride(const uint8_t* dl, int dlSize, int vc, int posOff, int posSz) {
    for (int st = posOff + posSz; st <= 64; ++st)
        if (validateStride(dl, dlSize, posOff, posSz, st, vc)) return st;
    return 0;
}

// Draw one loaded model's display lists through gx_draw at (wx,wy,wz), scaled.
static void renderModel(const DispModel& dm, const float camView[3][4],
                        const float (*animMtx)[3][4] = nullptr) {
    uint8_t* h = dm.h;
    uint8_t* verts   = *(uint8_t**)(h + 0x28);
    uint8_t* normals = *(uint8_t**)(h + 0x2c);
    uint8_t* colors  = *(uint8_t**)(h + 0x30);
    uint8_t* texc    = *(uint8_t**)(h + 0x34);
    uint8_t* renderOps = *(uint8_t**)(h + 0x38);
    uint8_t* dlTable = *(uint8_t**)(h + 0xD0);
    uint8_t* instrs  = *(uint8_t**)(h + 0xD4);
    int instrBits = (int)(*(uint16_t*)(h + 0xD8)) << 3;
    int renderOpCount = h[0xF8], dlCount = h[0xF5], jointCount = h[0xF3], texMtxCount = h[0xFA];
    int vc = *(uint16_t*)(h + 0xE4);
    if (!instrs || !dlTable || !verts || instrBits <= 0) return;
    // Skinned models prepend a PNMTXIDX byte + one TEXnMTXIDX byte per texMtxCount to each
    // vertex; consume them and apply per-joint bind-pose offsets (below) so skinned verts land
    // at their bone positions instead of collapsing to the origin.
    int pnmtx = (jointCount > 1) ? (1 + texMtxCount) : 0;
    if (pnmtx && getenv("STAIRFAX_MODEL_PNMTX")) pnmtx = atoi(getenv("STAIRFAX_MODEL_PNMTX")); // diag override
    if (getenv("STAIRFAX_MODEL_DBG"))
        fprintf(stderr, "[rm] jc=%d texMtx=%d pnmtx=%d renderOps=%d dlCount=%d\n",
                jointCount, texMtxCount, pnmtx, renderOpCount, dlCount);

    // model-local -> world (scale + translate), composed with the camera view
    float mv[3][4];
    for (int r=0;r<3;++r){ for(int c=0;c<3;++c) mv[r][c]=camView[r][c]*dm.scale;
        mv[r][3]=camView[r][0]*dm.wx + camView[r][1]*dm.wy + camView[r][2]*dm.wz + camView[r][3]; }
    GXLoadPosMtxImm(mv, GX_PNMTX0); GXSetCurrentMtx(GX_PNMTX0);
    gx_draw_setSourceBounds(h, h + 0x80000);
    gx_draw_setTexture(nullptr);   // default; each render op rebinds its own TEX1 texture below

    // Bind-pose joint offsets: worldPos = localPos + (accumHead - tail), accumHead summing
    // bone head translations down the parent chain (matches model_view). Extra joints blend.
    float gOff[MAX_JOINTS][3]; for (int j=0;j<MAX_JOINTS;++j) gOff[j][0]=gOff[j][1]=gOff[j][2]=0;
    int slotMap[16]; for (int i=0;i<16;++i) slotMap[i]=i;
    // Full per-joint skinning matrices M_j (bind-pose = identity). STAIRFAX_ANIM_TEST fills them
    // with a procedural FK pose to prove the matrix path deforms the mesh; real keyframe decode
    // will replace the procedural local rotations. animTest gates whether matrices (vs the plain
    // bind-pose offset path) drive the skin.
    static float jmtx[MAX_JOINTS][3][4];
    const char* animPlay = getenv("STAIRFAX_ANIM_PLAY");   // moveIdx to play from real keyframes
    bool animTest = getenv("STAIRFAX_ANIM_TEST") != nullptr || animPlay != nullptr || animMtx != nullptr;
    if (pnmtx) {
        uint8_t* jd = *(uint8_t**)(h + 0x3C); int jc = jointCount;
        static float accum[MAX_JOINTS][3];
        if (jd && jc > 0 && jc <= MAX_JOINTS) {
            for (int j=0;j<jc;++j){ const uint8_t* b=jd+j*0x1c; int parent=(int8_t)b[0];
                for (int c=0;c<3;++c){ float head=*(const float*)(b+4+c*4), tail=*(const float*)(b+0x10+c*4);
                    accum[j][c]=head + ((parent>=0&&parent<j)?accum[parent][c]:0.0f);
                    gOff[j][c]=accum[j][c]-tail; } }
            uint8_t* ed = *(uint8_t**)(h + 0x54); int ec = h[0xF4];
            for (int i=0;i<ec && jc+i<MAX_JOINTS;++i){ const uint8_t* e=ed+i*3;
                int g0=e[0],g1=e[1]; float w=e[2]/4.0f, wi=1.0f-w;
                if (g0<jc+i && g1<jc+i) for (int c=0;c<3;++c) gOff[jc+i][c]=w*gOff[g0][c]+wi*gOff[g1][c]; }
            if (animMtx) {
                // Externally-supplied joint matrices (spawned object driven via the real eval chain).
                for (int j=0;j<jc && j<MAX_JOINTS;++j) mat34Copy(animMtx[j], jmtx[j]);
                for (int i=0;i<ec && jc+i<MAX_JOINTS;++i){ const uint8_t* e=ed+i*3; int g0=e[0];
                    if (g0<jc+i) mat34Copy(jmtx[g0], jmtx[jc+i]); }
            }
            else if (animPlay) {
                // REAL playback: decode move animPlay's keyframes (reversed stages 1-3).
                int moveCount = *(uint16_t*)(h+0xEC);
                int moveIdx = atoi(animPlay); if (moveIdx < 0 || moveIdx >= moveCount) moveIdx = 0;
                float speed = getenv("STAIRFAX_ANIM_SPEED") ? atof(getenv("STAIRFAX_ANIM_SPEED")) : 0.3f;
                uint8_t** md = *(uint8_t***)(h+0x64); uint8_t* atl = md ? md[moveIdx] : nullptr;
                float flen = atl ? (float)atl[7] : 1.0f; if (flen < 1) flen = 1;
                if (getenv("STAIRFAX_ANIM_EVALCHAIN")) {
                    // Fullest routing: the REAL eval chain drives the reversed build.
                    // Object_ObjAnimSetMove -> modelAnimEvalChannels -> modelAnimUpdateChannels
                    // -> modelAnimBuildJointMatrices (reimplemented) -> H.dst.
                    static AnimHarness H; static uint8_t* hHdr = nullptr;
                    if (hHdr != h) { harnessSetup(&H, h); hHdr = h; }
                    float progress = fmodf(gFrameCounter * speed, flen) / flen;
                    Object_ObjAnimSetMove(H.objAnim, moveIdx, progress, 0);
                    *(uint16_t*)(H.state + 0x58) = 0;   // eventCountdown=0 -> single slot (no null prev-slot)
                    modelAnimEvalChannels((uint8_t*)H.dst, H.objModel, H.state, progress, 0x7f);
                    for (int j = 0; j < jc && j < MAX_JOINTS; ++j) mat34Copy(H.dst[j], jmtx[j]);
                } else if (getenv("STAIRFAX_ANIM_HARNESS")) {
                    // Route move selection + framePhase through the REAL objanim API.
                    static AnimHarness H; static uint8_t* hHdr = nullptr;
                    if (hHdr != h) { harnessSetup(&H, h); hHdr = h; }
                    float progress = fmodf(gFrameCounter * speed, flen) / flen;
                    float framePhase = 0; uint8_t* mfd = harnessSetMove(&H, moveIdx, progress, &framePhase);
                    if (getenv("STAIRFAX_MODEL_DBG")) { static int once=0; if(!once){once=1;
                        fprintf(stderr,"[harness] move %d: Object_ObjAnimSetMove -> moveFrameData=%p (atlas+6=%p, %s) framePhase=%.2f (direct=%.2f)\n",
                            moveIdx, (void*)mfd, (void*)(atl+6), mfd==atl+6?"MATCH":"DIFFER", framePhase, fmodf(gFrameCounter*speed, flen)); }}
                    decodeAtlas(mfd ? mfd - 6 : atl, framePhase, moveIdx, jd, jc, accum, jmtx);
                } else {
                    float frameF = fmodf(gFrameCounter * speed, flen);
                    decodeAtlas(atl, frameF, moveIdx, jd, jc, accum, jmtx);
                }
                for (int i=0;i<ec && jc+i<MAX_JOINTS;++i){ const uint8_t* e=ed+i*3; int g0=e[0];
                    if (g0<jc+i) mat34Copy(jmtx[g0], jmtx[jc+i]); }
            }
            else if (animTest) {
                // Forward kinematics with a procedural per-joint local rotation:
                //   A_j = A_parent * ( T(head_j) * R(angle_j) ),  M_j = A_j * T(-accum_j)
                static float A[MAX_JOINTS][3][4];
                float t = gFrameCounter * 0.05f;
                for (int j=0;j<jc;++j){ const uint8_t* b=jd+j*0x1c; int parent=(int8_t)b[0];
                    float head[3]; for(int c=0;c<3;++c) head[c]=*(const float*)(b+4+c*4);
                    float a0=0.25f*sinf(t+j*0.6f), a1=0.18f*sinf(t*1.3f+j*0.4f), a2=0.22f*sinf(t*0.7f+j);
                    float L[3][4]; jointLocalMat34(a0, a1, a2, head, L);
                    if (parent>=0 && parent<j) mat34Mul(A[parent], L, A[j]); else mat34Copy(L, A[j]);
                    for (int r=0;r<3;++r){ for(int c=0;c<3;++c) jmtx[j][r][c]=A[j][r][c];
                        jmtx[j][r][3]=A[j][r][3] - (A[j][r][0]*accum[j][0]+A[j][r][1]*accum[j][1]+A[j][r][2]*accum[j][2]); }
                }
                for (int i=0;i<ec && jc+i<MAX_JOINTS;++i){ const uint8_t* e=ed+i*3; int g0=e[0];
                    if (g0<jc+i) mat34Copy(jmtx[g0], jmtx[jc+i]); }
            }
        }
    }

    Bits bs = { instrs, 0, instrBits };
    const uint8_t* curOp = nullptr;
    int posSz=2,nrmP=0,nrmSz=1,clrP=0,clrSz=1,texSz=1,layerCount=0, guard=0;
    while (bs.pos + 4 <= bs.bitLen && guard++ < 100000) {
        int op = mdlBits(&bs, 4);
        if (op == 1) { int idx = mdlBits(&bs, 6); curOp = (idx < renderOpCount) ? renderOps + idx*0x44 : nullptr; }
        else if (op == 3) {
            posSz = mdlBits(&bs,1)?2:1;
            nrmP=0; if(curOp&&(curOp[0x40]&1)){ nrmP=1; nrmSz=mdlBits(&bs,1)?2:1; }
            clrP=0; if(curOp&&(curOp[0x40]&2)){ clrP=1; clrSz=mdlBits(&bs,1)?2:1; }
            texSz = mdlBits(&bs,1)?2:1; layerCount = curOp?curOp[0x41]:0;
        }
        else if (op == 4) { int cnt=mdlBits(&bs,4); for(int i=0;i<cnt;++i){ int mi=mdlBits(&bs,8); if(i<16) slotMap[i]=mi; } }
        else if (op == 2) {
            int dlIdx = mdlBits(&bs, 8);
            if (dlIdx >= dlCount) continue;
            const uint8_t* de = dlTable + dlIdx*0x1c;
            const uint8_t* dl = *(const uint8_t* const*)(de + 0);  // de+0 relocated to a host ptr on load
            int dls = (int)mdlBE16(de + 4);                        // de+4 (u16 size) is leaf, still BE
            // Reject an implausible DL pointer/size: an object whose model DL table wasn't
            // relocated (e.g. a child the player DLL spawned via a load path the port doesn't
            // fully wire) yields a garbage, often mis-aligned pointer here. Relocated host DL
            // pointers are >=4-byte aligned; skip anything else rather than fault in the decode.
            if (!dl || dls <= 0 || dls > 0x80000 || ((uintptr_t)dl & 3)) continue;

            int posOff = pnmtx;
            int descStride = pnmtx + posSz + (nrmP?nrmSz:0) + (clrP?clrSz:0) + layerCount*texSz;
            if (getenv("STAIRFAX_MODEL_DBG") && jointCount>=20) { static int nlog=0; if(nlog<40){ nlog++;
                int okv = validateStride(dl, dls, posOff, posSz, descStride, vc);
                int det = okv ? descStride : detectStride(dl, dls, vc, posOff, posSz);
                fprintf(stderr, "[rm] DL %d: dls=%d posOff=%d posSz=%d nrmP=%d clrP=%d layerCount=%d texSz=%d descStride=%d valid=%d detected=%d vc=%d\n",
                        dlIdx, dls, posOff, posSz, nrmP, clrP, layerCount, texSz, descStride, okv, det, vc); }}

            // Per-slot skin: full FK matrices under the anim test, else the bind-pose offset.
            if (pnmtx && animTest) {
                float slotMtx[16][3][4];
                for (int s=0;s<16;++s){ int j=slotMap[s]; if(j<0||j>=MAX_JOINTS)j=0; mat34Copy(jmtx[j], slotMtx[s]); }
                gx_draw_setJointMatrices(slotMtx, 16);
            } else if (pnmtx) {
                float slotOff[16][3];
                for (int s=0;s<16;++s){ int j=slotMap[s]; if(j<0||j>=MAX_JOINTS)j=0;
                    slotOff[s][0]=gOff[j][0]; slotOff[s][1]=gOff[j][1]; slotOff[s][2]=gOff[j][2]; }
                gx_draw_setJointOffsets(slotOff, 16);
            }

            // Bind this render op's TEX1 texture + alpha mode (mirrors the terrain path). After
            // load, layers[0].texture (Shader+0x24) holds the ModelTex handle textureLoad returned;
            // stairfax_modeltex_rhi lazily decodes it to an RhiTexture. Untextured ops stay null.
            { RhiTexture* mtex=nullptr; int amode=RHI_ALPHA_OPAQUE;
              if (curOp && curOp[0x41] > 0) {
                  mtex = (RhiTexture*)stairfax_modeltex_rhi(*(void* const*)(curOp + 0x24));
                  unsigned shFlags = *(const unsigned*)(curOp + 0x3C);
                  if (shFlags & 0x60000000u)      amode = RHI_ALPHA_BLEND;
                  else if (shFlags & 0x400u)      amode = RHI_ALPHA_TEST; }
              gx_draw_setTexture(mtex); gx_draw_setAlphaMode(amode); }

            if (validateStride(dl, dls, posOff, posSz, descStride, vc)) {
                // Descriptor-derived layout is correct: full attribute path.
                gx_draw_setForceLayout(0,0,0);
                GXClearVtxDesc();
                if (pnmtx) { GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
                    for (int t=0;t<texMtxCount;++t) GXSetVtxDesc((GXAttr)(GX_VA_TEX0MTXIDX+t), GX_DIRECT); }
                GXSetVtxDesc(GX_VA_POS, posSz==2?GX_INDEX16:GX_INDEX8);
                if (nrmP) GXSetVtxDesc(GX_VA_NRM, nrmSz==2?GX_INDEX16:GX_INDEX8);
                if (clrP) GXSetVtxDesc(GX_VA_CLR0, clrSz==2?GX_INDEX16:GX_INDEX8);
                for (int t=0;t<layerCount;++t) GXSetVtxDesc((GXAttr)(GX_VA_TEX0+t), texSz==2?GX_INDEX16:GX_INDEX8);
                GXSetArray(GX_VA_POS, verts, 6);
                if (nrmP && normals) GXSetArray(GX_VA_NRM, normals, (h[0x24]&0x80)?9:3);
                if (clrP && colors)  GXSetArray(GX_VA_CLR0, colors, 2);
                if (layerCount && texc) GXSetArray(GX_VA_TEX0, texc, 4);
                GXCallDisplayList((void*)dl, (unsigned)dls);
            } else {
                // Descriptor stride wrong (common on skinned DLs): autodetect the true
                // per-vertex stride and draw POS-only, like model_view's fallback.
                int ps = posSz, st = detectStride(dl, dls, vc, posOff, ps);
                if (!st) { ps = (posSz==2?1:2); st = detectStride(dl, dls, vc, posOff, ps); }
                if (st) {
                    GXClearVtxDesc();
                    if (pnmtx) GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
                    GXSetVtxDesc(GX_VA_POS, ps==2?GX_INDEX16:GX_INDEX8);
                    GXSetArray(GX_VA_POS, verts, 6);
                    gx_draw_setForceLayout(st, posOff, ps);
                    GXCallDisplayList((void*)dl, (unsigned)dls);
                    gx_draw_setForceLayout(0,0,0);
                }
            }
        }
        else if (op == 5) break;
        else break;
    }
    gx_draw_setSourceBounds(nullptr, nullptr);
    gx_draw_setJointOffsets(nullptr, 0);
    gx_draw_setJointMatrices(nullptr, 0);
    gx_draw_setForceLayout(0, 0, 0);
    gx_draw_setTexture(nullptr); gx_draw_setAlphaMode(RHI_ALPHA_OPAQUE);   // don't leak into next model
}

// SEH-guarded wrapper: the interim renderModel autodetects vertex stride by scanning display
// lists, and a spawned object whose model the port doesn't fully load/relocate (e.g. once the
// player DLL shifts the heap and exposes a latent DL over-read) can fault mid-scan. Skip that
// one object's draw instead of taking down the frame. POD-only scope so SEH needs no C++ unwind.
static void safeRenderModel(const DispModel& dm, const float camView[3][4], const float (*am)[3][4]) {
#if defined(_MSC_VER)
    __try { renderModel(dm, camView, am); }
    __except (1 /* EXCEPTION_EXECUTE_HANDLER */) { }
#else
/* vita fork: SEH is MSVC-only. Elsewhere this runs UNGUARDED -- a fault here
 * crashes instead of being skipped, which is deliberate: it points at the
 * missing subsystem instead of hiding it. docs/47. */
    renderModel(dm, camView, am);
#endif
}

void loadScene() {
    gInited = true;
    // Terrain dir: STAIRFAX_MAP_DIR, else the model dir (so one dir setting drives both), else desert.
    const char* dir = getenv("STAIRFAX_MAP_DIR");
    if (!dir) dir = getenv("STAIRFAX_MODEL_DIR");
    if (!dir) dir = "desert";
    const char* modenv = getenv("STAIRFAX_MAP_MOD");

    char tabPath[256], binPath[256];
    if (modenv) {
        int mod = atoi(modenv);
        snprintf(tabPath, sizeof tabPath, "%s/mod%d.tab", dir, mod);
        snprintf(binPath, sizeof binPath, "%s/mod%d.zlb.bin", dir, mod);
    } else if (!dvd_shim_findFile(dir, "mod", ".zlb.bin", binPath, sizeof binPath) ||
               !dvd_shim_findFile(dir, "mod", ".tab", tabPath, sizeof tabPath)) {
        // no mod file in this dir - fall back to the desert default
        snprintf(tabPath, sizeof tabPath, "desert/mod29.tab");
        snprintf(binPath, sizeof binPath, "desert/mod29.zlb.bin");
    }
    int tabSize=0, binSize=0;
    unsigned char* tab=(unsigned char*)loadFileByPath(tabPath,&tabSize,0);
    unsigned char* bin=(unsigned char*)loadFileByPath(binPath,&binSize,0);
    if (!tab || !bin) { fprintf(stderr,"[scene] cannot read %s / %s (ISO mounted?)\n",tabPath,binPath); gInitFailed=true; return; }

    unsigned lastOff=0xffffffff;
    for (int i=0;i<tabSize/4;++i){
        unsigned off=asset_mapBlockOffset(tab,tabSize,i); if(off==ASSET_NO_BLOCK||off==lastOff) continue; lastOff=off;
        Block b; b.gid=i; if(asset_loadMapBlock(bin,binSize,tab,tabSize,i,&b.mb)) gBlocks.push_back(std::move(b));
    }
    if (gBlocks.empty()) { fprintf(stderr,"[scene] no valid blocks in %s\n",binPath); gInitFailed=true; return; }

    // Real per-cell placement: decode the map's cell grid (MAPS.bin) and place each block
    // at its true world cell. Falls back to a square grid when no map id is given or the
    // decode fails. mapId from STAIRFAX_MAP_ID, else STAIRFAX_ROMLIST (same map the objects
    // spawn from), so terrain and objects share one coordinate frame.
    int placedByCells=0;
    const char* mapIdEnv = getenv("STAIRFAX_MAP_ID"); if(!mapIdEnv) mapIdEnv=getenv("STAIRFAX_ROMLIST");
    if (mapIdEnv) {
        static StairfaxMapCell cells[64*64]; int sx=0,sz=0,orgX=0,orgZ=0;
        int nc=stairfax_mapcells_decode(atoi(mapIdEnv),cells,64*64,&sx,&sz,&orgX,&orgZ);
        if (nc>0) {
            std::unordered_map<int,int> byGid;                 // global block id -> gBlocks index
            for (int i=0;i<(int)gBlocks.size();++i) byGid[gBlocks[i].gid]=i;
            for (int c=0;c<nc;++c){ auto it=byGid.find(cells[c].blockId); if(it==byGid.end()) continue;
                Placement pl; pl.block=it->second;
                pl.wx=(cells[c].cellX-orgX)*MAP_BLOCK_WORLD_SIZE;
                pl.wz=(cells[c].cellZ-orgZ)*MAP_BLOCK_WORLD_SIZE;
                pl.wy=(float)gBlocks[it->second].mb.yOff;
                gPlaces.push_back(pl); placedByCells++; }
            fprintf(stderr,"[scene] cell grid map %s: %dx%d origin=(%d,%d) %d cells, %d placed\n",
                    mapIdEnv,sx,sz,orgX,orgZ,nc,placedByCells);
        } else fprintf(stderr,"[scene] cell decode failed for map %s (rc=%d)\n",mapIdEnv,nc);
    }
    if (!placedByCells) {                                      // fallback: square grid
        int cols=(int)ceilf(sqrtf((float)gBlocks.size()));
        for (int i=0;i<(int)gBlocks.size();++i){ Placement pl; pl.block=i;
            pl.wx=(i%cols)*MAP_BLOCK_WORLD_SIZE; pl.wz=(i/cols)*MAP_BLOCK_WORLD_SIZE; pl.wy=(float)gBlocks[i].mb.yOff;
            gPlaces.push_back(pl); }
    }

    float wmn[3]={1e18f,1e18f,1e18f}, wmx[3]={-1e18f,-1e18f,-1e18f};
    for (auto& pl : gPlaces) {
        AssetMapBlock& b=gBlocks[pl.block].mb; const uint8_t* d=b.data;
        float scl=1.0f/(float)(1<<ASSET_POS_FRAC[b.fmt&7]); float t[3]={pl.wx,pl.wy,pl.wz};
        for (int v=0;v<b.vertCount;++v){ const uint8_t* vp=d+b.vertOff+v*6;
            float w[3]={asset_s16be(vp)*scl+t[0], asset_s16be(vp+2)*scl+t[1], asset_s16be(vp+4)*scl+t[2]};
            for(int k=0;k<3;++k){ if(w[k]<wmn[k])wmn[k]=w[k]; if(w[k]>wmx[k])wmx[k]=w[k]; } }
    }
    for (int k=0;k<3;++k) gWorldCtr[k]=(wmn[k]+wmx[k])*0.5f;
    gWorldR=1; for(int k=0;k<3;++k){ float e=(wmx[k]-wmn[k])*0.5f; if(e>gWorldR)gWorldR=e; }

    // textures from TEX1 (same route as map_view)
    char p1t[256],p1b[256]; snprintf(p1t,256,"%s/TEX1.tab",dir); snprintf(p1b,256,"%s/TEX1.bin",dir);
    int s1t=0,s1b=0; unsigned char* t1tab=(unsigned char*)loadFileByPath(p1t,&s1t,0);
    unsigned char* t1bin=(unsigned char*)loadFileByPath(p1b,&s1b,0);
    int texOK=0,texTot=0;
    if (t1tab&&t1bin){ static unsigned char tbuf[4*1024*1024];
        for (auto& blk : gBlocks){ AssetMapBlock& b=blk.mb; blk.texCache.assign(b.texCount,nullptr);
            for (int k=0;k<b.texCount;++k){ int id=(int)(asset_be32(b.data+b.texArrOff+k*4)&0x7fff);
                int w,h,fmt; unsigned io; texTot++;
                if (asset_loadTexRecord(t1tab,s1t,t1bin,s1b,id,tbuf,sizeof tbuf,&w,&h,&fmt,&io)){
                    uint8_t* rgba=gxTexDecode(fmt,w,h,tbuf+io);
                    if(rgba){ blk.texCache[k]=rhi_createTexture(gRhi,w,h,1,(uint32_t)fmt,rgba); free(rgba); if(blk.texCache[k])texOK++; } } } }
    }
    printf("[scene] %s: %zu blocks, %d/%d textures, worldR=%.0f\n",
           binPath, gBlocks.size(), texOK, texTot, gWorldR);
}

// Port-side player controller: move the spawned GameObject camera-relative and face it
// toward motion. Y is left at the spawn height (no collision yet - ground-clamp is next).
void updatePlayer() {
    if (!gPlayerObj) return;
    uint8_t* o = gPlayerObj;
    float lx = padGetStickX(0)/100.0f, ly = padGetStickY(0)/100.0f;
    float mag = sqrtf(lx*lx + ly*ly);
    gPlayerMoving = (mag > 0.25f);
    if (gPlayerMoving) {
        float sy=sinf(gFollowYaw), cyw=cosf(gFollowYaw);
        // camera-relative basis on the yaw plane (matches the view-forward convention below)
        float fwd[3]={-sy,0,cyw}, rgt[3]={cyw,0,sy};
        float dx = fwd[0]*ly + rgt[0]*lx, dz = fwd[2]*ly + rgt[2]*lx;
        float dl = sqrtf(dx*dx+dz*dz); if (dl>1e-4f){ dx/=dl; dz/=dl; }
        float speed = getenv("STAIRFAX_PLAYER_SPEED") ? (float)atof(getenv("STAIRFAX_PLAYER_SPEED")) : 14.0f;
        float step = speed * (mag>1.0f?1.0f:mag);
        *(float*)(o+0x0C) += dx*step;   // anim.localPosX
        *(float*)(o+0x14) += dz*step;   // anim.localPosZ
        // face movement direction; SFA anim.rotX is s16 (full turn = 65536). Tunable offset
        // (degrees) since the model's neutral forward axis isn't known a priori.
        float faceOff = getenv("STAIRFAX_PLAYER_FACE") ? (float)atof(getenv("STAIRFAX_PLAYER_FACE")) : 0.0f;
        float heading = atan2f(dx, dz) + faceOff*(3.14159265f/180.0f);
        *(int16_t*)(o+0x00) = (int16_t)lroundf(heading/(2.0f*3.14159265f)*65536.0f);
        float aspd = getenv("STAIRFAX_PLAYER_ANIMSPEED") ? (float)atof(getenv("STAIRFAX_PLAYER_ANIMSPEED")) : 0.6f;
        gPlayerAnimPhase += aspd * (mag>1.0f?1.0f:mag);
    }
}

void updateCamera() {
    // Third-person follow of the spawned player character.
    if (gPlayerObj && getenv("STAIRFAX_PLAYER")) {
        float cx = padGetCX(0)/100.0f, cy = padGetCY(0)/100.0f;
        if (fabsf(cx) < 0.2f) cx = 0.0f;   // C-stick deadzone: idle drift was accumulating pitch/yaw
        if (fabsf(cy) < 0.2f) cy = 0.0f;   // to the clamp, pinning the camera looking at the sky
        // Freeze orbit accumulation during the ~2.5s scene load: transient pad state while the map
        // streams in was swinging the pitch off its (look-down) default, so the camera "moved up on
        // its own" and framed the sky before the player was even visible.
        static int camWarm = 0;
        if (camWarm < 150) { camWarm++; cx = 0.0f; cy = 0.0f; }
        gFollowYaw   += cx * 0.045f;
        gFollowPitch += cy * 0.03f;
        if (getenv("STAIRFAX_CAM_TRACE")) { static int c=0; if((c++%60)==0)
            fprintf(stderr,"[cam] pitch=%.3f yaw=%.3f cx=%.2f cy=%.2f\n", gFollowPitch, gFollowYaw, cx, cy); }
        if (gFollowPitch >  0.55f) gFollowPitch =  0.55f;
        if (gFollowPitch < -0.90f) gFollowPitch = -0.90f;
        float dist  = getenv("STAIRFAX_PLAYER_CAMDIST") ? (float)atof(getenv("STAIRFAX_PLAYER_CAMDIST")) : 160.0f;
        float lookH = getenv("STAIRFAX_PLAYER_CAMHEIGHT") ? (float)atof(getenv("STAIRFAX_PLAYER_CAMHEIGHT")) : 45.0f;
        float px=*(float*)(gPlayerObj+0x0C), py=*(float*)(gPlayerObj+0x10), pz=*(float*)(gPlayerObj+0x14);
        float tgt[3]={px, py+lookH, pz};
        float cp=cosf(gFollowPitch), sp=sinf(gFollowPitch), sy=sinf(gFollowYaw), cyw=cosf(gFollowYaw);
        // view-forward for sceneRender's Rot=Rx(pitch)*Ry(yaw) convention (row 2 of Rot).
        float viewFwd[3]={ -sy*cp, sp, cyw*cp };
        for (int k=0;k<3;++k) gCamPos[k]=tgt[k]-viewFwd[k]*dist;
        gCamYaw=gFollowYaw; gCamPitch=gFollowPitch; gCamPlaced=true;
        if (getenv("STAIRFAX_CAM_TRACE")) { static int c=0; if((c++%60)==0)
            fprintf(stderr,"[cam] player=(%.1f,%.1f,%.1f) lookH=%.1f dist=%.1f pitch=%.3f camPos=(%.1f,%.1f,%.1f)\n",
                    px,py,pz,lookH,dist,gFollowPitch, gCamPos[0],gCamPos[1],gCamPos[2]); }
        return;
    }
    if (!gCamPlaced) {
        float dist = gWorldR * 2.0f;
        gCamPos[0]=gWorldCtr[0];
        gCamPos[1]=gWorldCtr[1]+gWorldR*0.55f;
        gCamPos[2]=gWorldCtr[2]-dist;
        gCamYaw=0.0f;
        // look down at the map center: camera-forward world = (0,sin p,cos p) must
        // point along (center - pos) = (0, -0.55R, +dist).
        gCamPitch=atan2f(-gWorldR*0.55f, dist);
        gCamPlaced=true;
    }
    // ~1/60s cadence; scale motion to world size
    float moveSpd = gWorldR * 0.012f;
    float lookSpd = 0.03f;
    float lx = padGetStickX(0)/100.0f, ly = padGetStickY(0)/100.0f;   // WASD / left stick
    float cx = padGetCX(0)/100.0f,     cy = padGetCY(0)/100.0f;       // IJKL / right stick
    float up = (padGetRTrigger(0)?1.0f:0.0f) - (padGetLTrigger(0)?1.0f:0.0f);

    gCamYaw   += cx * lookSpd;
    gCamPitch += cy * lookSpd;
    if (gCamPitch >  1.5f) gCamPitch =  1.5f;
    if (gCamPitch < -1.5f) gCamPitch = -1.5f;

    float sy=sinf(gCamYaw), cyw=cosf(gCamYaw);
    // forward on yaw plane, right perpendicular
    float fwd[3]={ sy, 0.0f, cyw };
    float rgt[3]={ cyw, 0.0f, -sy };
    for (int k=0;k<3;++k) gCamPos[k] += (fwd[k]*ly + rgt[k]*lx) * moveSpd;
    gCamPos[1] += up * moveSpd;
}

} // namespace

extern "C" void sceneRender(int a, int b, int c, int d, int e, int f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;

    if (!gInited) {
        gRhi = vi_host_rhi();
        if (!gRhi) return;                 // VI not up yet
        gx_draw_init(); gx_draw_setRhi(gRhi, nullptr); setupVtxFormats();
        loadScene();
        stairfax_gamebit_selftest();   // one-shot: verify gamebits after init (env-gated)
        stairfax_sky_init();           // bring up the real sky DLL's time-of-day clock
        if (getenv("STAIRFAX_SHOW_MODELS")) {
            ensureModelCaches();
            // per-dir terrain/prop set by default; STAIRFAX_MODEL_GLOBAL views root/global object models
            if (!getenv("STAIRFAX_MODEL_GLOBAL")) stairfax_model_perdir(1);
            int one[] = { getenv("STAIRFAX_MODEL_ONE") ? atoi(getenv("STAIRFAX_MODEL_ONE")) : 0 };
            int many[] = {136,141,294,296,477,528,532,533};
            int* ids = one[0] ? one : many;
            int nids = one[0] ? 1 : (int)(sizeof(many)/sizeof(many[0]));
            int ncols = 4;
            for (int i = 0; i < nids; ++i) {
                // Real ObjModel_Load now runs the full anim pipeline (MODANIM/AMAP/ANIM);
                // STAIRFAX_MODEL_STATIC forces the bind-pose-only loader (skips animations).
                int msz = 0;
                uint8_t* mh = getenv("STAIRFAX_MODEL_STATIC")
                    ? (uint8_t*)stairfax_model_load_static(-ids[i])
                    : (uint8_t*)ObjModel_Load(-ids[i], 0, &msz);
                if (!mh) continue;
                stairfax_bswap_model_moves(mh);   // BE anim move data -> host order
                if (getenv("STAIRFAX_MODEL_DBG")) {
                    // The model header IS the ObjAnimDef (ObjModel.file/animDef union): moveCount
                    // @0xEC, moveData@0x64 -> atlas entries (post-swap, host order).
                    uint8_t* h = mh;
                    uint8_t** moveData = *(uint8_t***)(h+0x64); int mc = *(uint16_t*)(h+0xEC);
                    fprintf(stderr, "[animdef] id=%d moveCount=%d moveData=%p jc=%d\n",
                            ids[i], mc, (void*)moveData, h[0xF3]);
                    if (moveData && moveData[0]) { uint8_t* a=moveData[0];
                        int streamOff=*(int16_t*)(a+2), frameLen=a[7], stride=a[8], jc=a[6];
                        fprintf(stderr, "[animdef] move0 streamOff=%d rootCurve=%d jc=%d frameLen=%d stride=%d desc[0..5]=%04x %04x %04x %04x %04x %04x\n",
                                streamOff, *(int16_t*)(a+4), jc, frameLen, stride,
                                *(uint16_t*)(a+10),*(uint16_t*)(a+12),*(uint16_t*)(a+14),
                                *(uint16_t*)(a+16),*(uint16_t*)(a+18),*(uint16_t*)(a+20)); }
                }
                DispModel dm; dm.h = mh;
                float s = atof(getenv("STAIRFAX_MODEL_SCALE") ? getenv("STAIRFAX_MODEL_SCALE") : "4");
                if (nids == 1) {
                    // off to the left of the terrain, at look height, in open space
                    dm.wx = gWorldCtr[0] - gWorldR * 0.75f;
                    dm.wy = gWorldCtr[1];
                    dm.wz = gWorldCtr[2];
                } else {
                    dm.wx = gWorldCtr[0] + ((i % ncols) - 1.5f) * (gWorldR * 0.35f);
                    dm.wy = gWorldCtr[1] + gWorldR * 0.30f;
                    dm.wz = gWorldCtr[2] + (i / ncols) * (gWorldR * 0.35f);
                }
                dm.scale = s;
                gDispModels.push_back(dm);
            }
            fprintf(stderr, "[models] %zu display models loaded\n", gDispModels.size());
            stairfax_model_perdir(0);   // objects resolve against root/global again
        }
        if (const char* mid = getenv("STAIRFAX_ROMLIST")) {
            extern int stairfax_romlist_dump(int mapId, int maxLog);
            stairfax_romlist_dump(atoi(mid), 8);   // read real object placements from MAPS.bin
        }
        if (getenv("STAIRFAX_SPAWN")) {
            extern int stairfax_romlist_spawn(int mapId, const char* mapName, int maxCount);
            const char* nm = getenv("STAIRFAX_ROMLIST_NAME"); if (!nm) nm = "frontend";
            int mid = getenv("STAIRFAX_ROMLIST") ? atoi(getenv("STAIRFAX_ROMLIST")) : 0;
            stairfax_romlist_spawn(mid, nm, 40);
            // Reframe the camera on the spawned objects' bounds (they live in the romlist
            // map's space, not the desert map we loaded for framing).
            float mn[3]={1e18f,1e18f,1e18f}, mx[3]={-1e18f,-1e18f,-1e18f}; int nf=0;
            for (int i=0;i<gObjCount;++i){ uint8_t* o=(uint8_t*)gObjList[i];
                float x=*(float*)(o+0x0C), y=*(float*)(o+0x10), z=*(float*)(o+0x14);
                if(x<mn[0])mn[0]=x; if(x>mx[0])mx[0]=x; if(y<mn[1])mn[1]=y; if(y>mx[1])mx[1]=y;
                if(z<mn[2])mn[2]=z; if(z>mx[2])mx[2]=z; nf++; }
            if(nf>0){ for(int k=0;k<3;++k) gWorldCtr[k]=(mn[k]+mx[k])*0.5f;
                gWorldR=1; for(int k=0;k<3;++k){ float e=(mx[k]-mn[k])*0.5f; if(e>gWorldR)gWorldR=e; }
                const char* z=getenv("STAIRFAX_SPAWN_ZOOM"); if(z) gWorldR*=(float)atof(z); // pull camera in
                gCamPlaced=false; }
            fprintf(stderr,"[spawn] reframed on %d objects, worldR=%.0f\n", nf, gWorldR);
        }
        // Playable character: spawn the real Sabre/Krystal object at the object-cloud centroid
        // (interim - the real per-map save spawn point needs the save/map-event subsystem). It
        // joins gObjList and renders through the spawn loop; a port-side controller drives it.
        if (getenv("STAIRFAX_PLAYER")) {
            ensureModelCaches();   // loadCharacter's internal ObjModel_Load needs the caches up
            int seq = getenv("STAIRFAX_PLAYER_CHAR") ? atoi(getenv("STAIRFAX_PLAYER_CHAR")) : 0x1F; // Krystal
            float px=gWorldCtr[0], py=gWorldCtr[1], pz=gWorldCtr[2];
            if (const char* ppos = getenv("STAIRFAX_PLAYER_POS"))
                sscanf(ppos, "%f,%f,%f", &px, &py, &pz);
            gPlayerObj = (uint8_t*)stairfax_spawn_player(seq, px, py, pz);
            gFollowYaw = 0.0f; gCamPlaced = false;
            // Objects present before the player DLL runs. playerUpdate spawns children (staff,
            // effects) into gObjList whose models the port doesn't fully load/relocate; the
            // generic render loop must not draw those, so cap it at this base count.
            gSceneBaseObjCount = gObjCount;
        }
        if (const char* mm = getenv("STAIRFAX_MODEL_TEST")) {
            extern void* ObjModel_Load(int id, int loadFlag, int* outSize);
            extern int stairfax_model_scan(int want);
            int realId = atoi(mm), sz = 0;
            if (realId == 0) realId = stairfax_model_scan(12);   // 0 = find first real model
            unsigned char* h = (unsigned char*)ObjModel_Load(-realId, 0, &sz);  // negative id = direct realId
            if (!h) { fprintf(stderr, "[model] ObjModel_Load(%d) -> NULL\n", realId); }
            else {
                int dataSize = *(int*)(h + 0x0c);
                unsigned char* verts = *(unsigned char**)(h + 0x28);
                int inRange = verts >= h && verts < h + dataSize;
                unsigned short vtxCount = *(unsigned short*)(h + 0xE4);
                unsigned short dlBitLen = *(unsigned short*)(h + 0xD8);
                fprintf(stderr, "[model] id=%d loaded: dataSize=0x%x texCount=%d jointCount=%d "
                        "vtxCount=%d dlBits=%d vertsPtr=%p (in-range=%d) outSize=0x%x\n",
                        realId, dataSize, h[0xF2], h[0xF3], vtxCount, dlBitLen, (void*)verts, inRange, sz);
            }
        }
    }
    if (gInitFailed) { VIWaitForRetrace(); return; }
    ++gFrameCounter;   // drives the procedural anim-test pose

    // Real sky DLL advances the time-of-day clock; tint the frame clear by day/night.
    float skyB = stairfax_sky_tick();
    vi_set_clear_color(0.02f + skyB * 0.28f, 0.03f + skyB * 0.47f, 0.06f + skyB * 0.74f);

    // Real player DLL (Phase A): run the recompiled playerUpdate state machine over the
    // spawned character. When enabled it owns the character's motion/animation, so the
    // interim hand-driven updatePlayer is skipped. Default stays the interim path.
    if (stairfax_player_dll_enabled()) stairfax_player_dll_tick();
    else                               updatePlayer();   // interim camera-relative move
    updateCamera();

    // Camera shim (Milestone 2): push the port follow-cam pose into the view struct the REAL
    // playerUpdate reads (Camera_GetCurrent), so its camera-relative controls track the on-screen
    // view. gFollowYaw/gFollowPitch are the port's orbit angles in RADIANS; the Camera struct wants
    // GameCube s16 angles (full turn = 65536). gCamPos is the world-space eye position.
    if (stairfax_player_dll_enabled() && gPlayerObj) {
        short camYawS16, camPitchS16;
        float k = 65536.0f / (2.0f * 3.14159265f);

        camYawS16 = (short)(-gFollowYaw * k);   // positive sign = first guess; correct empirically
        camPitchS16 = 0;
        stairfax_camera_set_pose(camYawS16, camPitchS16, gCamPos[0], gCamPos[1], gCamPos[2]);
    }

    const int W=1280, H=720;
    float proj[4][4]; memset(proj,0,sizeof proj);
    float ys=1.0f/tanf((60.0f*3.14159265f/180.0f)*0.5f);
    float zn=fmaxf(gWorldR*0.01f,1.0f), zf=gWorldR*8.0f+1.0f;
    proj[0][0]=ys/((float)W/H); proj[1][1]=ys; proj[2][2]=zf/(zf-zn);
    proj[2][3]=-zn*zf/(zf-zn); proj[3][2]=1.0f;
    GXSetProjection(proj, GX_PERSPECTIVE);

    // world->view from yaw/pitch/pos: Rot = Rx(pitch)*Ry(yaw)
    float cyw=cosf(gCamYaw), sy=sinf(gCamYaw), cp=cosf(gCamPitch), sp=sinf(gCamPitch);
    float Ry[3][3]={{cyw,0,sy},{0,1,0},{-sy,0,cyw}};
    float Rx[3][3]={{1,0,0},{0,cp,-sp},{0,sp,cp}};
    float Rot[3][3];
    for(int r=0;r<3;++r)for(int cc=0;cc<3;++cc){float s2=0;for(int k=0;k<3;++k)s2+=Rx[r][k]*Ry[k][cc];Rot[r][cc]=s2;}
    float camView[3][4];
    for(int r=0;r<3;++r){ float t=0; for(int k=0;k<3;++k){ camView[r][k]=Rot[r][k]; t-=Rot[r][k]*gCamPos[k]; } camView[r][3]=t; }

    // Real camera: once the follow cam is activated (gStairfaxRealCamActive, set when the player spawns
    // and setMode(0x42) runs), the engine/1 camcontrol + engine/66 cam produces a verified follow POSE
    // (Camera_GetCurrent()->pos: ~90u behind, ~104u above the player). camcontrol never calls
    // Camera_UpdateViewMatrices in the port's path though, so the real gCameraViewMatrix stays zero -
    // build the view matrix here as a LOOK-AT from the real eye to the player, in the port's proven LH
    // convention (right = up x fwd, +Z into screen). Feed it to BOTH terrain (camView) and the player
    // render (write gCameraViewMatrix, which objprint's Camera_GetViewMatrix returns) so they are one
    // coherent view. Keyed on the runtime flag so non-player dev runs keep their interim view.
    if (gStairfaxRealCamActive && gPlayerObj) {
        unsigned char* vc = (unsigned char*)Camera_GetCurrent();
        if (vc) {
            // Build the render from the real view Camera's SMOOTHED orientation (Camera_GetCurrent:
            // yaw@0x00, pitch@0x02 s16, pos@0x0C) - NOT a per-frame look-at at Fox, which would always
            // pin him dead-centre and remove engine/66's stable-when-still / centre-on-move smoothing.
            // Port convention: look dir = (-sin(gCamYaw),0,cos(gCamYaw)); verified gCamYaw = +viewYaw
            // (straight: viewYaw 180deg -> look -Z; diagonal: matches Fox's heading). Rot = Rx(pitch)*Ry(yaw).
            float krad = 2.0f*3.14159265f/65536.0f;
            float yaw = (float)(*(short*)(vc+0x00)) * krad;
            float pit = -(float)(*(short*)(vc+0x02)) * krad;
            float pos[3]={ *(float*)(vc+0x0C), *(float*)(vc+0x10), *(float*)(vc+0x14) };
            float cY=cosf(yaw), sY=sinf(yaw), cP=cosf(pit), sP=sinf(pit);
            float Ry2[3][3]={{cY,0,sY},{0,1,0},{-sY,0,cY}};
            float Rx2[3][3]={{1,0,0},{0,cP,-sP},{0,sP,cP}};
            float Rt[3][3];
            for(int r=0;r<3;++r)for(int c2=0;c2<3;++c2){float s2=0;for(int k2=0;k2<3;++k2)s2+=Rx2[r][k2]*Ry2[k2][c2];Rt[r][c2]=s2;}
            for(int r=0;r<3;++r){ float t=0; for(int k2=0;k2<3;++k2){ camView[r][k2]=Rt[r][k2]; t-=Rt[r][k2]*pos[k2]; } camView[r][3]=t; }
            float* gvm=Camera_GetViewMatrix();   // player render bridge (objprint reads this)
            if(gvm) for(int r=0;r<3;++r)for(int cc=0;cc<4;++cc) gvm[r*4+cc]=camView[r][cc];
        }
    }

    bool drawMap = !getenv("STAIRFAX_NO_MAP");
    for (auto& pl : gPlaces) {
        if (!drawMap) break;
        Block& blk=gBlocks[pl.block]; AssetMapBlock& bb=blk.mb; const uint8_t* dd=bb.data;
        gx_draw_setSourceBounds(dd, dd + bb.size);  // clamp indexed fetches to this block
        GXAttrType posIdx = bb.posSz==2?GX_INDEX16:GX_INDEX8;
        GXAttrType clrIdx = bb.clrSz==2?GX_INDEX16:GX_INDEX8;
        GXAttrType texIdx = bb.texSz==2?GX_INDEX16:GX_INDEX8;
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  posIdx);
        GXSetVtxDesc(GX_VA_CLR0, clrIdx);
        GXSetArray(GX_VA_POS, (void*)(dd+bb.vertOff), 6);
        if(bb.colOff) GXSetArray(GX_VA_CLR0, (void*)(dd+bb.colOff), 2);
        // Every TEX layer of a multi-textured block indexes the SAME texcoord array
        // (real setupToRenderMapBlock binds TEX0..TEXn all to vertexTexCoords, stride 4).
        if(bb.texOff) for(int t=0;t<8;++t) GXSetArray((GXAttr)(GX_VA_TEX0+t), (void*)(dd+bb.texOff), 4);

        float mv[3][4];
        for(int r=0;r<3;++r){ mv[r][0]=camView[r][0]; mv[r][1]=camView[r][1]; mv[r][2]=camView[r][2];
            mv[r][3]=camView[r][0]*pl.wx+camView[r][1]*pl.wy+camView[r][2]*pl.wz+camView[r][3]; }
        GXLoadPosMtxImm(mv, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        for (int i=0;i<bb.dlCount;++i){ const uint8_t* rec=dd+bb.dlBase+i*0x1C;
            unsigned o=asset_be32(rec); int sz=asset_be16(rec+4); int shIdx=rec[0x13];
            RhiTexture* tex=nullptr; int nTex=1; int alphaMode=RHI_ALPHA_OPAQUE;
            if(shIdx<bb.shCount){ const uint8_t* sh=dd+bb.shOff+shIdx*0x44;
                unsigned shFlags=asset_be32(sh+0x3C); int layerCount=sh[0x41];
                // real mapBlockRender_setVtxDcrs: shader flag 0x80000000 forces a single TEX0,
                // else one TEX coord set per shader layer. A DL that carries 2 tex layers but
                // is decoded with only TEX0 misaligns every vertex -> stretched "spike" tris.
                nTex = (shFlags & 0x80000000u) ? 1 : layerCount;
                if(nTex<0) nTex=0; if(nTex>8) nTex=8;
                // Alpha, per mapBlockRender_setShader: force-blend flags -> src-over blend;
                // alpha-test-opaque (0x400) -> cutout (foliage billboards); else opaque.
                if(shFlags & 0x60000000u)      alphaMode=RHI_ALPHA_BLEND;
                else if(shFlags & 0x400u)      alphaMode=RHI_ALPHA_TEST;
                int ti=(int)asset_be32(sh+0x24);
                if(ti>=0 && ti<(int)blk.texCache.size()) tex=blk.texCache[ti]; }
            for(int t=0;t<8;++t) GXSetVtxDesc((GXAttr)(GX_VA_TEX0+t), (t<nTex)?texIdx:GX_NONE);
            gx_draw_setTexture(tex);
            gx_draw_setAlphaMode(alphaMode);
            if(sz>0 && o+(unsigned)sz<=bb.size) GXCallDisplayList((void*)(dd+o), (unsigned)sz);
        }
    }

    gx_draw_setAlphaMode(RHI_ALPHA_OPAQUE);   // don't leak terrain's last mode into models

    // Real object models loaded by the real ObjModel_Load, drawn via gx_draw.
    for (auto& dm : gDispModels) safeRenderModel(dm, camView, nullptr);

    // Spawned romlist objects: draw each GameObject's model. STAIRFAX_SPAWN_GRID lays them
    // out compactly near the camera (visible together); otherwise at their world positions.
    if (getenv("STAIRFAX_SPAWN")) {
        ensureModelCaches();   // so the full (animated) ObjModel_Load of object models works
        int grid = getenv("STAIRFAX_SPAWN_GRID") ? 1 : 0;
        int cols = 6, cell = 0;
        static int logged = 0;
        int objDrawCount = (gSceneBaseObjCount >= 0 && gSceneBaseObjCount < gObjCount)
                           ? gSceneBaseObjCount : gObjCount;   // skip player-DLL-spawned children
        for (int i = 0; i < objDrawCount; ++i) {
            uint8_t* o = (uint8_t*)gObjList[i];
            if (o == gPlayerObj) continue;   // player rendered separately (dedicated harness)
            void* om = Obj_GetActiveModel(o);
            uint8_t* h = om ? *(uint8_t**)om : nullptr;   // ObjModel.file (offset 0)
            uint8_t* def = *(uint8_t**)(o + 0x50);        // anim.modelInstance = ObjDef
            int32_t* mids = def ? *(int32_t**)(def + 0x08) : nullptr;
            int mcnt = def ? def[0x55] : 0;
            // Most objects are SINGLE_MODEL: loadCharacter defers their model to the object DLL's
            // setup fn (dll+0x18), which the port doesn't run -> no bank -> om/h null. Port-side
            // fix: load the object's own model (modelFileIds[0], resolved via the ROOT table) and
            // render it at the object position, bypassing the DLL-gated bank. Cached by model id.
            if (!h && mcnt > 0 && mids && mids[0] > 0) {
                static int   omId[512]; static uint8_t* omHdr[512]; static int nOm = 0;
                int fid = mids[0]; uint8_t* mh = nullptr; bool found = false;
                for (int k=0;k<nOm;++k) if (omId[k]==fid){ mh=omHdr[k]; found=true; break; }
                if (!found && nOm < 512) {
                    // full load (with animations) where the area's ANIM data allows; else bind pose
                    mh = (uint8_t*)stairfax_objmodel_load_guarded(fid);
                    if (!mh) mh = (uint8_t*)stairfax_model_load_static(-fid);
                    if (mh) stairfax_bswap_model_moves(mh);
                    omId[nOm]=fid; omHdr[nOm]=mh; ++nOm;
                }
                h = mh;
            }
            if (getenv("STAIRFAX_SPAWN_DBG"))
                fprintf(stderr, "[spawn] obj %d mid0=%d h=%p loadedMoveCount=%d jc=%d\n",
                        i, (mids&&mcnt>0)?mids[0]:0, (void*)h,
                        h?*(uint16_t*)(h+0xEC):-1, h?h[0xF3]:-1);
            if (!h) continue;
            if (!logged) fprintf(stderr, "[spawn] obj %d jointCount=%d vtxCount=%d\n",
                                 i, h[0xF3], *(uint16_t*)(h + 0xE4));
            DispModel dm; dm.h = h;
            dm.scale = getenv("STAIRFAX_MODEL_SCALE") ? (float)atof(getenv("STAIRFAX_MODEL_SCALE")) : 1.0f;
            if (grid) {
                float sp = gWorldR * 0.28f;
                dm.wx = gWorldCtr[0] + ((cell % cols) - 2.5f) * sp;
                dm.wy = gWorldCtr[1];
                dm.wz = gWorldCtr[2] + (cell / cols) * sp;
                cell++;
            } else {
                dm.wx = *(float*)(o + 0x0C); dm.wy = *(float*)(o + 0x10); dm.wz = *(float*)(o + 0x14);
            }
            // Spawned-object animation: drive the loaded model header through the real eval chain
            // via a minimal harness (the object's own bank isn't populated - DLL-gated - so we use
            // the header directly, exactly like the model viewer's eval-chain path). The move data
            // is already host-swapped at load. Only animates models that actually loaded moves.
            const char* spawnAnim = getenv("STAIRFAX_SPAWN_ANIM");
            bool animated = false;
            if (spawnAnim) {
                int mc = *(uint16_t*)(h + 0xEC);
                uint8_t** md = *(uint8_t***)(h + 0x64);
                int moveIdx = atoi(spawnAnim); if (moveIdx < 0 || moveIdx >= mc) moveIdx = 0;
                if (mc > 0 && md && md[moveIdx]) {
                    static AnimHarness H; static uint8_t* hHdr = nullptr;
                    if (hHdr != h) { harnessSetup(&H, h); hHdr = h; }
                    float speed = getenv("STAIRFAX_ANIM_SPEED") ? atof(getenv("STAIRFAX_ANIM_SPEED")) : 0.3f;
                    uint8_t* atl = md[moveIdx]; float flen = atl ? (float)atl[7] : 1.0f; if (flen < 1) flen = 1;
                    float progress = fmodf(gFrameCounter * speed, flen) / flen;
                    Object_ObjAnimSetMove(H.objAnim, moveIdx, progress, 0);
                    *(uint16_t*)(H.state + 0x58) = 0;   // eventCountdown -> single slot
                    modelAnimEvalChannels((uint8_t*)H.dst, H.objModel, H.state, progress, 0x7f);
                    safeRenderModel(dm, camView, H.dst);
                    animated = true;
                }
            }
            if (!animated) safeRenderModel(dm, camView, nullptr);
        }
        logged = 1;
    }

    // Player: render through the REAL model path (objRenderModel -> modelDoRenderInstrs), which
    // decodes the character's true vertex/DL format + skinning. Bridge the port camera into the
    // real path via gPortViewMatrix (Camera_GetViewMatrix returns it). STAIRFAX_PLAYER_INTERIM
    // falls back to the old hand-rolled harness for comparison.
    if (gPlayerObj && !getenv("STAIRFAX_PLAYER_INTERIM")) {
        ensureModelCaches();
        for (int r=0;r<3;++r) for (int cc=0;cc<4;++cc) gPortViewMatrix[r][cc]=camView[r][cc];
        int idleMove = getenv("STAIRFAX_PLAYER_IDLE") ? atoi(getenv("STAIRFAX_PLAYER_IDLE")) : 0;
        int walkMove = getenv("STAIRFAX_PLAYER_WALK") ? atoi(getenv("STAIRFAX_PLAYER_WALK")) : idleMove;
        int move = gPlayerMoving ? walkMove : idleMove;
        float speed = getenv("STAIRFAX_ANIM_SPEED") ? (float)atof(getenv("STAIRFAX_ANIM_SPEED")) : 0.3f;
        float progress = fmodf(gFrameCounter * speed * 0.03f, 1.0f);
        if (getenv("STAIRFAX_CAM_TRACE")) { static int c=0; if((c++%60)==0) { uint8_t* o=gPlayerObj;
            fprintf(stderr,"[render] local=(%.1f,%.1f,%.1f) world=(%.1f,%.1f,%.1f) rotX=%d\n",
                *(float*)(o+0x0C),*(float*)(o+0x10),*(float*)(o+0x14),
                *(float*)(o+0x18),*(float*)(o+0x1C),*(float*)(o+0x20), *(short*)(o+0x00)); } }
        stairfax_render_player(gPlayerObj, move, progress);
    }
    else if (gPlayerObj) {
        ensureModelCaches();
        uint8_t* o = gPlayerObj;
        void* om = Obj_GetActiveModel(o);
        uint8_t* h = om ? *(uint8_t**)om : nullptr;
        uint8_t* def = *(uint8_t**)(o + 0x50);
        int32_t* mids = def ? *(int32_t**)(def + 0x08) : nullptr;
        int mcnt = def ? def[0x55] : 0;
        if (!h && mcnt > 0 && mids && mids[0] > 0) {   // DLL-gated bank empty -> port-side direct load
            static int pid = -1; static uint8_t* phdr = nullptr;
            if (pid != mids[0]) { pid = mids[0];
                phdr = (uint8_t*)stairfax_objmodel_load_guarded(pid);
                if (!phdr) phdr = (uint8_t*)stairfax_model_load_static(-pid);
                if (phdr) stairfax_bswap_model_moves(phdr); }
            h = phdr;
        }
        if (h) {
            DispModel dm; dm.h = h;
            dm.scale = getenv("STAIRFAX_PLAYER_SCALE") ? (float)atof(getenv("STAIRFAX_PLAYER_SCALE")) : 1.0f;
            dm.wx=*(float*)(o+0x0C); dm.wy=*(float*)(o+0x10); dm.wz=*(float*)(o+0x14);
            int mc = *(uint16_t*)(h + 0xEC);
            uint8_t** md = *(uint8_t***)(h + 0x64);
            static int loggedP = 0;
            if (!loggedP) { fprintf(stderr,"[player] model=%d moveCount=%d jc=%d\n", mids?mids[0]:-1, mc, h[0xF3]); loggedP=1; }
            int idleMove = getenv("STAIRFAX_PLAYER_IDLE") ? atoi(getenv("STAIRFAX_PLAYER_IDLE")) : 0;
            int walkMove = getenv("STAIRFAX_PLAYER_WALK") ? atoi(getenv("STAIRFAX_PLAYER_WALK")) : idleMove;
            int moveIdx = gPlayerMoving ? walkMove : idleMove;
            if (getenv("STAIRFAX_PLAYER_STATIC")) { renderModel(dm, camView); }  // bind pose (no anim) - diagnostic
            else if (mc > 0 && md) {
                if (moveIdx < 0 || moveIdx >= mc) moveIdx = 0;
                if (md[moveIdx]) {
                    static AnimHarness PH; static uint8_t* phHdr = nullptr;
                    if (phHdr != h) { harnessSetup(&PH, h); phHdr = h; }
                    uint8_t* atl = md[moveIdx]; float flen = atl ? (float)atl[7] : 1.0f; if (flen < 1) flen = 1;
                    float ph = gPlayerMoving ? gPlayerAnimPhase : (gFrameCounter * 0.05f);
                    float progress = fmodf(ph, flen) / flen;
                    Object_ObjAnimSetMove(PH.objAnim, moveIdx, progress, 0);
                    *(uint16_t*)(PH.state + 0x58) = 0;
                    modelAnimEvalChannels((uint8_t*)PH.dst, PH.objModel, PH.state, progress, 0x7f);
                    renderModel(dm, camView, PH.dst);
                } else renderModel(dm, camView);
            } else renderModel(dm, camView);
        }
    }

    VIWaitForRetrace();   // interim present point (see file header)

    // Dev-only: one-shot window capture N frames in (STAIRFAX_CAP=path[,frame]).
    static int sFrame = 0; static int sCapDone = 0;
    ++sFrame;
    const char* cap = getenv("STAIRFAX_CAP");
    if (cap && !sCapDone) {
        const char* fenv = getenv("STAIRFAX_CAP_FRAME");
        int capFrame = fenv ? atoi(fenv) : 60;
        if (sFrame >= capFrame) {
            if (plat_window_capture_bmp(vi_host_window(), cap))
                printf("[scene] captured frame %d -> %s\n", sFrame, cap);
            sCapDone = 1;
        }
    }
}

extern "C" { int gStfxAnimArm = 3, gStfxAnimTrace; }   /* run 55 A/B, see modelAnimBuildJointMatrices */
