#pragma once
// gx_tev_key.h - one draw's TEV COMBINER configuration, as a value (vita fork, docs/47 run 88+).
//
// The key is everything that changes the SHAPE of the fragment program: stage count, per-stage
// inputs / ops / destinations / texture-and-colour use, swap tables, and the alpha-compare
// functions. Values that only change numbers (colour registers, konst colours, alpha-compare
// references) are NOT in the key: they are uniforms (TevUniforms). Two draws with equal keys share
// one compiled shader. Texture COORDINATES are produced on the CPU (texgen), so the key does not
// carry texgen state either -- only which texcoord slot each stage samples.
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEVK_STAGES 16
// The generator's version, mixed into every key's hash on the device: a program the device marked
// BAD (its compile took the process down) under one generator is not blocked under the next.
// v2 (2026-09-28): no `float pass` -- a Cg reserved word (runs 89-90).
// v3 (2026-10-07, docs/47 run 116): a fogged key carries fogType and its program reads clip w + uFog/uFogColor.
#define TEVGEN_VERSION 3

typedef struct {
    uint8_t texSlot;     // 0xff: no texture; else the texcoord/sampler SLOT (0..TEVK_TEX-1) this stage reads
    uint8_t ras;         // 0: COLOR0A0, 1: COLOR1A1, 2: ALPHA_BUMP, 3: ALPHA_BUMPN (an indirect stage with bump alpha), 0xff: none (rasterised input reads as zero)
    uint8_t cin[4];      // GXTevColorArg a, b, c, d
    uint8_t ain[4];      // GXTevAlphaArg a, b, c, d
    uint8_t cop[5];      // op, bias, scale, clamp, out   (GXTevOp, GXTevBias, GXTevScale, bool, GXTevRegID)
    uint8_t aop[5];
    uint8_t rasSwap, texSwap, kcsel, kasel;
} TevKeyStage;

#define TEVK_TEX 8       // distinct (texcoord, map) pairs a key may sample. 6 until 2026-10-08: the title's commonest refusal
                         // (12,570 draws a run, "slots") is ONE state with 7 pairs (run 127). 8 is the ceiling: the programs
                         // pass colour0, colour1 and one texcoord per slot in TEXCOORD0..9, so 8 slots leave NO room for the
                         // fog varying -- a draw with 8 slots is drawn unfogged, counted (gx_fog_nd_slots).
                         // 4 until Vita run 112: the blur filter
                         // (intersect_render.c, 7 stages) samples the Z copy once and the screen copy at FIVE coords;
                         // refused at 4, it fell back to the single-texture path and painted the Z copy white

// INDIRECT TEXTURING (2026-10-12, docs/47 "indirect"). The SFA title uses exactly this subset (every GXSetTevIndirect call in the game was
// tabulated): format ITF_8, bias any of the 8 selections, matrix OFF / ITM_0..2 (the dynamic ITM_S/T matrices are REFUSED), wrap OFF or ITW_0
// (coordinate forced to 0) per axis, add-previous, no utc-lod, bump alpha S/T/U; coord scales ITS_1..ITS_256 per axis.
// Everything that changes the generated source is here; the matrices (floats) and texture sizes (floats) are uniforms.
typedef struct {
    uint8_t on;                  // some stage is indirect
    uint8_t st[TEVK_STAGES][6];  // per TEV stage: [0] active, [1] indirect stage 0..3, [2] bias bits (1 S, 2 T, 4 U), [3] matrix code 0 = OFF, 1..3 = ITM_0..2, 4..6 = ITM_S0..2 (dynamic), 7..9 = ITM_T0..2 (dynamic),
                                 //   [4] bit0 s-wrap to 0, bit1 t-wrap to 0, bit2 the stage does NOT sample its texture (GX_TEXMAP_DISABLE: the map only supplies the coordinate scale), [5] bit0 add previous coordinate, bits1-2 bump alpha select (0 off, 1 S, 2 T, 3 U)
    uint8_t slot[4];             // indirect stage -> texture slot (0xff: not used by the key)
    uint8_t sc[4][2];            // indirect stage -> coordinate scale exponents (GXIndTexScale: coord / 2^n), s then t
    uint8_t stu;                 // channel order of S, T, U in the indirect texel: 0 = A,B,G (assumed, from memory of Dolphin's software TEV), 1 = R,G,B, 2 = A,R,G (ux0:data/stairfax/tev_ind_stu.txt)
} TevIndKey;

typedef struct {
    uint8_t numStages;
    uint8_t numTex;              // sampler/texcoord slots in use
    uint8_t swap[4][4];          // GXSetTevSwapModeTable: [table][r,g,b,a] -> source channel 0..3
    uint8_t acComp0, acOp, acComp1;   // GXSetAlphaCompare functions (references are uniforms)
    uint8_t fogType;             // GX fog SHAPE (GXFogType values): 0 none, 2 linear, 4 exp, 5 exp2, 6 rev exp, 7 rev exp2
                                 // (docs/47 run 116). Perspective fog only; the NUMBERS are uniforms (TevUniforms)
    TevKeyStage st[TEVK_STAGES];
    TevIndKey ik;                // INDIRECT texturing (2026-10-12), after every older member so that a key without it keeps its old hashed bytes (tev_key_len)
    uint8_t projMask;            // PERSPECTIVE texgen (2026-10-12): bit s = slot s arrives as (s, t, q) (float3) and the program divides PER PIXEL. Set by the GPU vertex path
                                 // from the VsKey (tev_vs_proj_mask) -- the vertex program and this key must agree; 0 on the CPU path
} TevKey;
// The bytes of a key that carry meaning: a key with no indirect stage is hashed / saved as the old (pre-indirect) struct, so every program
// binary saved before indirect texturing existed is still found. (ik is the last member.)
static inline size_t tev_key_len(const TevKey* k) { return (k->ik.on || k->projMask) ? sizeof *k : offsetof(TevKey, ik); }

typedef struct {
    int16_t reg[4][4];   // TEVPREV, TEVREG0..2 initial values, r,g,b,a, S10 (-1024..1023)
    uint8_t konst[4][4]; // GX_KCOLOR0..3, r,g,b,a
    uint8_t acRef0, acRef1;
    float fogStart, fogScale;   // fog (docs/47 run 116): scale = 1/(end-start), 0 when end == start; used iff key.fogType
    uint8_t fogColor[3];        // the struct is memcmp'd to batch draws: the whole struct is zeroed where it is filled
    // INDIRECT matrices ITM_0..2 as the hardware applies them: s10 elements (GXSetIndTexMtx: (int)(1024 * f) & 0x7FF, sign-extended) / 1024 * 2^scale_exp.
    // [m][row][col]: offset(row) = sum over col of indM[m][row][col] * (s, t, u) -- in TEXELS of the stage's texture. Zero when the key has no indirect stage.
    float indM[3][2][3];
    // 2^scale_exp of ITM_0..2 (GXSetIndTexMtx's exponent): the DYNAMIC matrices (ITM_S/T) have no elements of their own, they use the exponent of the matrix register with the same index.
    float indE[3];
} TevUniforms;

// One vertex of a TEV draw: view-space position (the projection is the draw's MVP), the two
// rasterised channels (r | g<<8 | b<<16 | a<<24), and the texcoord of each texture slot.
typedef struct {
    float x, y, z;
    uint32_t c0, c1;
    float t[TEVK_TEX][2];
} TevVertex;

// Reference combiner, exact GX integer arithmetic (Dolphin's software TEV): the host oracle the
// generated shaders are tested against. tex[slot] and ras[0..1] are RGBA8 inputs; out = PREV.
void tev_ref_eval(const TevKey* k, const TevUniforms* u, const uint8_t tex[TEVK_TEX][4],
                  const uint8_t ras[2][4], int16_t out[4], int* discarded);
// Fog applied to tev_ref_eval's output (floats; GX's fixed-point fog table is deliberately not reproduced):
// ze = eye-space depth = the interpolated clip w. rgb only -- alpha and the alpha compare are untouched.
// No-op for k->fogType == 0.
void tev_ref_fog(const TevKey* k, const TevUniforms* u, float ze, int16_t rgba[4]);

// The Cg fragment program for a key: writes NUL-terminated source into buf (returns its length,
// or -1 if it did not fit). Uniform and varying names are fixed (see tev_gen.c).
int tev_gen_fragment(const TevKey* k, char* buf, int cap);
// The pieces, for the host test that compiles the body as C++ (tev_cg_compat.h): the #define
// prelude every program starts with, and the shared body (-2: the key needs compare mode).
int tev_gen_prelude(char* buf, int cap);
int tev_gen_body(const TevKey* k, char* buf, int cap);
// The FAST variant (tev_gen.c "FAST VARIANT"): normalised floats, no per-stage floor / wrap / rounding. Same signature, uniforms,
// varyings and samplers as the exact generator. Returns -1 if it did not fit, -2 if the fast variant REFUSES the key (use the exact
// program). tev_fast_safe: 1 = the fast program is within 2 codes of the exact one, 0 = refused; tev_fast_check returns 0 or the
// reason (tev_fast_why_name).
int tev_gen_fragment_fast(const TevKey* k, char* buf, int cap);
int tev_gen_body_fast(const TevKey* k, char* buf, int cap);
int tev_fast_safe(const TevKey* k);
int tev_fast_check(const TevKey* k);
const char* tev_fast_why_name(int why);
// The canonical form of a key (tev_gen.c "KEY CANONICALISATION"): the same generated program for every key that maps to it, so the
// device keeps one GL program per distinct program text. fast = 1 for tev_gen_fragment_fast, 0 for tev_gen_fragment; the result
// is valid for that generator only. Idempotent. The device calls it BEFORE hashing (src/tev_dedupe.h).
void tev_key_canon(const TevKey* in, TevKey* out, int fast);
// COMPARE-MODE stages (op 8..15, tev_gen.c "COMPARE-MODE STAGES"): tev_has_compare = some stage uses one; tev_cmp_unsupported = an op the
// generators refuse (ops 2..7 and >15, bias 3 on an add/sub, alpha compare ops 8..13). The exact generator emits compare; the fast one refuses it.
int tev_has_compare(const TevKey* k);
int tev_cmp_unsupported(const TevKey* k);
// INDIRECT stages: which uniforms the generated program declares (the device looks them up by name): bit m = matrix ITM_m (uIM<m>a, uIM<m>b),
// bit s of the TS mask = uTS<s> (texture size (w, h, 1/w, 1/h) of the slot a stage samples through an indirect offset).
unsigned tev_ind_matrix_mask(const TevKey* k);
unsigned tev_ind_ts_mask(const TevKey* k);
// The oracle (tev_ref.c) for keys with indirect stages: texFn samples slot `slot` at normalised (u, v) (nearest in the tests), vt[slot] is the
// interpolated texcoord of each slot (normalised), texSz[slot] the texture size in texels. tev_ref_eval is this with constant texels.
typedef void (*TevTexFn)(void* ctx, int slot, double u, double v, uint8_t rgba[4]);
void tev_ref_eval_ex(const TevKey* k, const TevUniforms* u, TevTexFn fn, void* ctx, const double vt[TEVK_TEX][2], const double texSz[TEVK_TEX][2],
                     const uint8_t ras[2][4], int16_t out[4], int* discarded);
// The matching vertex program (one for all keys with the same numTex).
int tev_gen_vertex(int numTex, char* buf, int cap);
// With fog the vertex program also passes clip w to the fragment program at TEXCOORD(2 + numTex).
int tev_gen_vertex_ex(int numTex, int fogType, char* buf, int cap);


// ============================================================================================================
// GPU VERTEX PATH (2026-10-08, docs/47 fast path stage 2): a Cg VERTEX program generated per VsKey.
// Today dlkCall (gx_draw.c) skins, lights and texgens every distinct vertex on the CPU. The vertex program does the
// same per vertex on the GPU; the CPU path is the SPECIFICATION and host/tests/vs_shader_test.cpp holds the program to
// it (positions to 1e-5 relative, colour code for code, texcoords to 1e-5 relative).
//
// The shim keeps on the CPU path (gx_vs_build returns 0 and a VSW_* reason): a vertex with !posOk, a list where only SOME
// distinct vertices carry a normal, a matrix slot above 9, more than 8 lights' worth of uniforms, more than 8 input
// attributes, a non-real-skin state, gPxActive == 0, gStfxLightArm == 0 (and chan_oldguess.txt: that A/B branch is not
// reproduced, so the shim must not take the GPU path while it is active).
//
// ATTRIBUTES (name, location the shim binds it to, Cg input semantic, type). Present only when the key reads them:
//   aPos     0  POSITION   float3  object position
//   aNrm     1  NORMAL     float3  raw (object-space) normal
//   aColor0  2  COLOR0     float4  vertex colour, byte/255 per channel (r,g,b,a)
//   aMtx     3  TEXCOORD0  float   matrix SLOT = PNMTXIDX/3 (0..9) as a float
//   aT<n>    4+ TEXCOORD<1+rank> float2  raw TEXn (s,t), rank = position of n among the TEXn the key reads, ascending n
//                 (location 4+rank). Which n: VsKey.texMask.
//   then, in this order and each only when the key reads it, at the next locations (4 + number of aT, ...):
//   aBin           float3  raw binormal  (the 2nd vector of a GX_VA_NBT normal; GX_TG_BINRM)              VsKey.hasBin
//   aTan           float3  raw tangent   (the 3rd vector of a GX_VA_NBT normal; GX_TG_TANGENT)            VsKey.hasTan
//   aTmA           float4  TEX0MTXIDX..TEX3MTXIDX of the vertex: the XF matrix-memory ROW (30..57), as floats   VsKey.tmiSlots/tmiCoord
//   aTmB           float4  TEX4MTXIDX..TEX7MTXIDX, same
// UNIFORMS (all float4; "rows" = 4 floats, row-major, clip = M * (x,y,z,1)):
//   uM0..uM3   projection rows (VsUniforms.proj, with gStfxGxZToGL already folded in)
//   uPos[30]   10 position matrices x 3 rows (slot s row r = uPos[3s+r]); uPos[3] (slot 0 only) when the key has no aMtx:
//              the shim uploads the CURRENT matrix into slot 0
//   uNrm[30]   the same for the normal matrices (3x3 used; w ignored); only when the key lights with a normal
//   uMat, uAmb material / ambient REGISTER colour, codes 0..255 (r,g,b,a)
//   uCLpos[n] uCLdir[n] uCLa[n] uCLk[n] uCLcol[n]   the n packed lights of the COLOUR channel: position xyz, direction xyz,
//              attenuation a0 a1 a2, k0 k1 k2 (w unused), colour codes r,g,b,a. In ascending light index; BLACK lights
//              (colour word 0) are DROPPED by the packer -- exact, since the CPU skips them, and keeping them with zero
//              colour is not identical (a non-finite attenuation times 0 is NaN)
//   uALpos[n] ... uALcol[n]   the same for the ALPHA channel, only when its lighting is not shared with the colour channel
//   uTM<s>[3]  texture matrix rows of texcoord slot s (hasTM); uPT<s>[3] post matrix rows (hasPost)
//   uTMX[30]   the 10 texture matrices x 3 rows (GX_TEXMTX0..9 = rows 30..57, VsUniforms.tmx), only when a slot takes its matrix from the vertex
//              (tmiSlots): that slot reads rows uTMX[r - 30 .. r - 28] for its vertex's index r
// VARYINGS (what tev_gen_fragment consumes): vPos POSITION; vColor0 TEXCOORD0; vColor1 TEXCOORD1 (zero, as the CPU's c1);
//   vT<s> TEXCOORD<2+s>; vW TEXCOORD<2+numTex> (clip w) when fog.
#define VS_MAX_SLOTS 10
#define VS_MAX_LIGHTS 8      // packed lights over both channels
#define VS_MAX_ATTRS 8

typedef struct {             // one colour or alpha channel (all zero when it is not enabled, bar matSrc)
    uint8_t enable;
    uint8_t matSrc;          // 1: the vertex colour (the record has CLR0 and the channel asks for it), else the register
    uint8_t ambSrc;          // same, ambient (only meaningful when enabled)
    uint8_t nLights;         // NON-BLACK lights packed (0 for the alpha channel when it shares the colour channel's lighting)
    uint8_t diffFn;          // 0 none, 1 sign, 2 clamp; forced 0 when attnFn == 0 (SPEC), as pxLitRGBA
    uint8_t attnFn;          // 0 spec, 1 spot, 2 none
} VsChanKey;
typedef struct {
    uint8_t kind;            // 0: unsupported source/function -> (0,0); 1: supported
    uint8_t func;            // 0 GX_TG_MTX3x4 (q = third row, projective), 1 GX_TG_MTX2x4 (q = 1)
    uint8_t src;             // 0 object position, 1 raw normal, 2 TEXn (texN), 3 TEXn the list does not carry (reads 0,0) -- and a BINRM / TANGENT source the list does not carry (reads 0,0,1: Dolphin's coord default), 4 raw binormal (aBin), 5 raw tangent (aTan)
    uint8_t texN;
    uint8_t hasTM, norm, hasPost;
} VsTexKey;
typedef struct {             // fully zero-initialised by its builder (no padding): memcmp-able and hashable
    uint8_t hasMtxIdx;       // the vertex carries PNMTXIDX (aMtx); else slot 0 = the current matrix
    uint8_t hasNrm;          // aNrm is read (and the list carries it)
    uint8_t hasClr0;         // aColor0 is read
    uint8_t numChans;        // 0: colour is white; else 1 (GXSetNumChans > 1 still uses channel 0)
    uint8_t shareAlpha;      // alpha channel lighting == colour channel lighting: alpha takes the colour channel's accumulated alpha
    uint8_t numTex;
    uint8_t fog;             // pass clip w on (vW)
    uint8_t texMask;         // bit n: aT<n> is read
    VsChanKey ch[2];         // [0] colour (rgb), [1] alpha
    VsTexKey tex[TEVK_TEX];
    uint8_t projFrag;        // 2026-10-12: the projective texgens (MTX3x4, or any post matrix) leave the vertex program as (s, t, q) and the fragment program divides (TevKey.projMask)
    // 2026-10-13: per-vertex texture matrix index (GX_VA_TEXnMTXIDX) and the BINRM / TANGENT texgen sources
    uint8_t hasBin, hasTan;  // aBin / aTan are read (VsTexKey.src 4 / 5)
    uint8_t tmiSlots;        // bit s: texcoord slot s takes its texture matrix from its vertex's TEX<tmiCoord[s]>MTXIDX (the slot's hasTM is 0 then)
    uint8_t tmiCoord[TEVK_TEX];   // slot s's texgen (texcoord) index 0..7 -- which TEXnMTXIDX byte
    uint8_t hasSkin;         // 2026-10-26 (R8 GPU skinning): aSk (float4) is read and the vertex program skins aPos / aNrm by the palette in uBone[VS_SKIN_PAL_VEC4]; zero in every other key
} VsKey;
// A VsKey without projFrag is hashed / saved as the old struct (every program binary saved before keeps its hash); one without the 2026-10-13
// fields hashes as it did before them.
static inline size_t tev_vs_key_len(const VsKey* k) { return (k->hasBin || k->hasTan || k->tmiSlots || k->hasSkin) ? sizeof *k : k->projFrag ? offsetof(VsKey, hasBin) : offsetof(VsKey, projFrag); }
// Which of the two aTm attributes the key reads: bit 0 aTmA (texcoords 0..3), bit 1 aTmB (4..7).
unsigned tev_vs_tmi_groups(const VsKey* k);
// Slots whose texgen is projective: supported (kind) and MTX3x4 or with a post matrix. The vertex program emits float3 for exactly these when projFrag is set.
unsigned tev_vs_proj_mask(const VsKey* k);

typedef struct { float pos[4], dir[4], a[4], k[4], col[4]; } VsLight;
typedef struct {
    float proj[4][4];
    float pm[VS_MAX_SLOTS][3][4], nm[VS_MAX_SLOTS][3][4];
    float mat[4], amb[4];
    VsLight lt[2][VS_MAX_LIGHTS];            // [0] colour channel, [1] alpha channel
    float tm[TEVK_TEX][3][4], pt[TEVK_TEX][3][4];
    float tmx[10][3][4];                     // the texture-matrix palette (GX_TEXMTX0..9), when a slot reads its matrix from the vertex
} VsUniforms;

// Why a list/key stays on the CPU path (also the generator's -2 reasons).
enum { VSW_OK = 0, VSW_NOT_REAL_SKIN, VSW_NO_PX, VSW_LIGHT_ARM_OFF, VSW_UNCACHEABLE, VSW_POS_NOT_OK, VSW_MIXED_NRM, VSW_SLOT_RANGE,
       VSW_TOO_MANY_LIGHTS, VSW_TOO_MANY_ATTRS, VSW_TOO_MANY_TEX, VSW_BAD_KEY, VSW_MIXED_NBT, VSW_TMI_RANGE, VSW_N };
const char* tev_vs_why_name(int why);
// Validate a key against what the generator reproduces exactly: VSW_OK or the reason.
int tev_vs_check(const VsKey* k);
uint32_t tev_vs_key_hash(const VsKey* k);
// How many vec4 uniforms the program declares.
int tev_vs_uniform_vec4(const VsKey* k);
// The full Cg program (returns length, -1 if it does not fit, -2 if tev_vs_check refuses the key).
int tev_gen_vertex_gx(const VsKey* k, char* buf, int cap);
// The pieces, for the host test (same split as tev_gen_prelude / tev_gen_body): the prelude (empty today), the BODY
// (statements only; valid Cg and valid C++ with host/tests/tev_cg_compat.h) and the parameter list in either Cg form
// (cpp = 0, with semantics) or C++ form (cpp = 1: out parameters are references; same names, same order).
int tev_gen_vertex_gx_prelude(char* buf, int cap);
int tev_gen_vertex_gx_body(const VsKey* k, char* buf, int cap);
int tev_gen_vertex_gx_params(const VsKey* k, int cpp, char* buf, int cap);


// ---------------------------------------------------------------------------------------------------------------------------------
// R8 GPU SKINNING, the HAL side (host/engine/R8_GPUSKIN.md; the table / palette / reference live in hal/gx_skin.c, gx_skin.h).
//   ATTRIBUTES aSkP / aSkN (VsKey.hasSkin; aSkN only when the key reads a normal; GxMeshLayout.offSk): two F32x4 attributes, 32 bytes per distinct vertex,
//     static for the mesh: aSkP at offSk = (posA, posB, posW0, posW1), aSkN at offSk + 16 = (nrmA, nrmB, nrmW0, nrmW1), each component a FLOAT holding an exact integer 0..255
//     (a / b = palette slots, GX_SKIN_NONE 255 = unskinned entry; w0 / w1 = the u8 weights, / 128 in the program; exact in fp32 and fp16).  Device history: a float
//     packing a * 256 + b lost its low bits, and a non-normalised U8 attribute did not reach the float program input as 0..255; plain F32 does.
//   UNIFORM: the palette AND the quantisation scales travel as ONE array, VS_SKIN_PAL_VEC4 float4, under the name uBone (its own uniform kind in the
//     GXM binder, GU_BONE; uTMX stays the texture-matrix palette, so a skinned key may also carry per-vertex texture matrices):
//        uBone[3 * s + r]  row r (0..2) of palette slot s (s < VS_SKIN_MAX_BONES);   uBone[VS_SKIN_PAL_VEC4 - 1] = (2^posQ, 2^-posQ, 2^nrmQ, 2^-nrmQ)
//     A backend fills a hasSkin draw's uBone from gx_skin_vs_palette(); VsUniforms is NOT grown.
#define VS_SKIN_MAX_BONES 32
#define VS_SKIN_PAL_VEC4  (3 * VS_SKIN_MAX_BONES + 1)
struct GxSkinTable;
// The bound skin whose model's FILE vertex array is posBase (NULL when none is bound or the bound model is another).
const struct GxSkinTable* gx_skin_hal_table(const void* posBase);
// 0 = the list may be skinned by t; else a reason (1 pos base, 2 normal base, 3 pos frac / type, 4 nrm frac / type). posFrac / nrmFrac: the VAT frac,
// posS16 / nrmS8: the component type is GX_S16 / GX_S8.
int  gx_skin_hal_check(const struct GxSkinTable* t, const void* posBase, const void* nrmBase, int hasNrm, int posFrac, int posS16, int nrmFrac, int nrmS8);
// One distinct vertex's aSkP / aSkN (8 bytes); 0 = an index outside the table (pi >= nPos, or ni >= nNrm when hasNrm) or not indexed (~0u).
int  gx_skin_hal_attr(const struct GxSkinTable* t, uint32_t pi, uint32_t ni, int hasNrm, uint8_t out[8]);
// A mesh built with skin table meshT may serve a draw that wants wantT (NULL = the unskinned image) only if they are the same table.
// The uBone image of the bound skin: VS_SKIN_PAL_VEC4 * 4 floats (palette rows, zero padding, the scale row); NULL when nothing is bound.
const float* gx_skin_vs_palette(void);
// Census: ++GXSK_REF_HAL (a skinned list that could not be honoured and was SKIPPED, never drawn unskinned) / ++GXSK_DRAWS (a skinned draw issued).
void gx_skin_hal_ref(void);
void gx_skin_hal_why(int why);   /* R8 instrument: the refusal reason (gx_draw.c dlkSkinPrep comment) */
void gx_skin_hal_draw(void);

// HOST-TEST HOOK (gx_draw.c): after GXCallDisplayList on the cached arm, what the GPU path would be fed for that list.
typedef struct { float pos[3], nrm[3], clr[4], mtx, tex[8][2], bin[3], tan[3], tmi[8]; } VsRawVertex;   // one distinct vertex, every attribute (the key picks); tmi[n] = TEXnMTXIDX (a row, 0 = not carried)
typedef struct {
    int why;                  // VSW_OK or the refusal
    int nv, ni;               // distinct vertices; triangle-corner indices (expandIdx order, !posOk triangles dropped)
    const VsRawVertex* v; const int* idx;
    VsKey key; VsUniforms u; TevKey tk;
} GxVsProbe;
int gx_draw_vsProbe(const unsigned char* dl, unsigned nbytes, GxVsProbe* out);   // 1 built, 0 refused (out->why), -1 no usable record
extern unsigned gx_vs_refused[VSW_N];

// THE DEVICE SIDE of the GPU vertex path (2026-10-08, docs/47 fast path stage 2). gx_draw.c hands the backend one MESH per
// cached display list -- the record's distinct vertices, interleaved, every attribute the LIST carries (the key of a given
// call picks which are bound) plus a u16 triangle list -- and draws it with a (VsKey, TevKey) program. Offsets are bytes
// into one vertex, -1 = the list does not carry it. pos/nrm: 3 floats; clr: 4 unsigned bytes r,g,b,a (normalised);
// mtx: 1 float (the matrix SLOT); tex[n]: 2 floats (raw TEXn).
typedef struct { int stride, offPos, offNrm, offClr, offMtx, offTex[8], offBin, offTan, offTmi[2], offSk; } GxMeshLayout;   // offSk: 32 bytes = aSkP + aSkN (F32x4 each, R8 GPU skinning), -1 = none; bin/tan: 3 floats; tmi[0] / tmi[1]: 4 floats (TEX0..3 / TEX4..7 MTXIDX rows)

#ifdef __cplusplus
}
#endif
