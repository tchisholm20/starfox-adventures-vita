// gx_skin.h -- R8 GPU skinning, T1: the skin table, the palette pack, and the C reference of the vertex-shader math.
// Contract: host/engine/R8_GPUSKIN.md.  No G/HAL wiring lives here.
//
// WHAT THE CPU DOES (model.c ObjModel_BlendVertexStream / ObjModel_BlendNormalStream + hal/model_asm_twins.c):
//   per chunk i: copy ceil blocks (vtxBlocks * 32 bytes) at animData + srcDataOffset to a cache buffer, run the kernel IN PLACE at
//   cache + dstByteOffset on vtxCount vertices (6-byte BE s16 XYZ, or 3-byte s8 normals), then write the WHOLE block range back to
//   dstBase + srcDataOffset (vertexAnimOffsets[i] = srcDataOffset; normalAnimOutputs[i] = normalBuf + srcDataOffset).
//   So the vertex index of the k-th vertex of a chunk is (srcDataOffset + dstByteOffset) / stride + k, the bytes of the block outside
//   the transformed run are copied back unchanged from the source, and a LATER chunk's block overwrites an earlier chunk's bytes
//   where blocks overlap. The table builder follows the kernel's BYTE writes, not the formula.
//   Weights: w0 = u8 * (1/128), w1 likewise, not renormalised. Quantisation: clamp to the integer range, then truncate toward zero
//   (__OSf32tos16 / __OSf32tos8), at scale 2^q with q = the OWNING JOB's quantShift (the normal job has its own).
#pragma once
#include <stdio.h>
#include "main/model.h"

#define GX_SKIN_MAX_BONES 32      /* hub dumps 2026-10-26: 26, 26, 20 bones (R8 T2) */
#define GX_SKIN_NONE      0xFFu   /* entry.a == entry.b == GX_SKIN_NONE: not skinned, pass through */
#define GX_SKIN_MTX_BYTES 0x30    /* sizeof(ROMtx): 3x4 floats, COLUMN-major (m[0..2] = x column, m[9..11] = translation) */

typedef enum GxSkinWhy {
    GX_SKIN_OK = 0,
    GX_SKIN_REFUSE_NBT,       /* NBT-normal model */
    GX_SKIN_REFUSE_BONES,     /* more than GX_SKIN_MAX_BONES distinct bones */
    GX_SKIN_REFUSE_ADDR,      /* addressing the builder cannot map (mixed vertex, overrun, bad scale, ...) */
    GX_SKIN_REFUSE_NOJOBS,    /* no chunks in either job: nothing to skin */
    GX_SKIN_REFUSE_CPUUSE     /* R8-MF C1: the CPU reads / writes this model's vtxBuf (gx_skin_note_cpu_user): the CPU blend must keep it current */
} GxSkinWhy;

typedef struct GxSkinEntry {
    u8 a, b;                  /* palette slots; GX_SKIN_NONE for both = unskinned */
    u8 w0, w1;                /* weights /128 */
} GxSkinEntry;

typedef struct GxSkinTable {
    const void* key;          /* the ModelFileHeader this was built from.  WARNING: NEVER dereference it on R or from a record: the file may be freed (B1) while the table is still alive */
    u32 id;                   /* R8-MF A2: minted per build, never reused, never 0.  Compare THIS across frames, never the table pointer (ABA) */
    const void* vertices;     /* header->vertices / ->normals as built: the arrays the HAL matches a list's POS / NRM base against (no header dereference) */
    const void* normals;
    int status;               /* GxSkinWhy */
    u32 nPos, nNrm;           /* entry counts (vertexCount, normalCount); nNrm == 0 when the normal job has no chunks */
    u8 posQ, nrmQ;            /* quantShift of the vertex job / of the normal job */
    u8 nb;                    /* palette bones */
    u8 bones[GX_SKIN_MAX_BONES]; /* slot -> index into gObjBoneMtxBuffer (matrix = base + index * 0x30) */
    GxSkinEntry* pos;
    GxSkinEntry* nrm;
    /* R8-FB: the CPU-skinned copy of this model's POS (nPos*6) then NRM (nNrm*3) arrays a refused skinned list is drawn from (gx_skin_fallback_fill).  Written by R only, lazily; freed with the table */
    u8* fb; u32 fbId, fbSerial;
} GxSkinTable;

/* Build (uncached). NULL + *why on refusal. gx_skin_table_free for the result. */
GxSkinTable* gx_skin_table_build(const ModelFileHeader* m, int* why);
void gx_skin_table_free(GxSkinTable* t);
/* Cached by file pointer + header identity (refusals cached too). NULL = CPU path.  A table is NEVER freed directly (R8-MF A1): it is retired, and freed by
   gx_skin_frame_edge once the render thread has executed every record G made before the retire.
   gx_skin_cache_clear retires every entry (model files unloaded wholesale). */
const GxSkinTable* gx_skin_table_get(const ModelFileHeader* m, int* why);
void gx_skin_cache_clear(void);
/* B1: ObjModel_Release calls this BEFORE mm_free(header): the entry (table or refusal) is retired, the CPU-user mark dropped. */
void gx_skin_model_released(const ModelFileHeader* m);
/* C1: ObjModel_GetCurrentVertexCoords calls this: the CPU touches this model's vtxBuf, so get refuses it (GX_SKIN_REFUSE_CPUUSE) from now on. */
void gx_skin_note_cpu_user(const ModelFileHeader* m);
/* Is (t, id) still a live table of the cache?  Safe on a dangling t (it is only compared).  objprint's slot kept the pointer across frames. */
int gx_skin_table_alive(const GxSkinTable* t, u32 id);
/* G, once per frame edge (vita_tool_main.c, right before the real present): gseq = g_gxt.seq, rdone = g_gxt.r_presented, ringOn = g_gxt.on.  Frees the retired
   tables R has passed; a no-op when none is retired. */
void gx_skin_frame_edge(uint32_t gseq, uint32_t rdone, int ringOn);
u32 gx_skin_retired_pending(void);   /* tests */
/* HAL (gx_draw.c): per-record memos key on the table's id, never on its pointer (a freed table's address can be reused by the next one: ABA). */
u32 gx_skin_table_id(const GxSkinTable* t);
int gx_skin_mesh_reusable_id(uint32_t meshId, uint32_t wantId);   /* a mesh built for table id meshId (0 = unskinned) may serve a draw that wants wantId only if they are equal */

/* Palette: 12 floats per slot = 3 rows of float4; row r = (m[r], m[3+r], m[6+r], m[9+r]) of the slot's bone matrix.
   Returns the float count written (12 * nb). boneMtxBuffer = gObjBoneMtxBuffer. */
int gx_skin_palette_pack(const GxSkinTable* t, const void* boneMtxBuffer, float* out);

/* C model of the vertex shader. src = the FILE bytes of entry idx (6 BE bytes of s16 / 3 bytes of s8). Same float32 operation order
   as the CPU kernel. outF = dequantised result (q * 2^-Q, what the rest of the pipeline sees), outQ = the integer the CPU stores. */
void gx_skin_ref_pos(const GxSkinTable* t, const float* palette, u32 idx, const u8* src, float outF[3], s16 outQ[3]);
void gx_skin_ref_nrm(const GxSkinTable* t, const float* palette, u32 idx, const u8* src, float outF[3], s8 outQ[3]);

/* S3 dump (R8 T1): layout in gx_skin.c. gx_skin_dump_write is plain stdio; gx_skin_dump_once is the marker-gated hook
   (Vita only; no-op elsewhere): ux0:data/stairfax/skin_dump.txt present -> ux0:data/stairfax/skin/model_XXXX.bin, once per model. */
int gx_skin_dump_write(FILE* f, const ModelFileHeader* m, const u8* outVtx, const u8* outNrm, const u8* boneMtxBuffer);
void gx_skin_dump_once(const ModelFileHeader* m, const u8* outVtx, const u8* outNrm, int vtxBufDirty, const u8* boneMtxBuffer);

/* R8 T2 (host/engine/R8_GPUSKIN.md "T2"): the bound skin -- GX state, HAL side.  GXStfxSkinBind(t, palette, nb) binds a table and its
   palette (12*nb floats from gx_skin_palette_pack; COPIED, so the caller may reuse its buffer at once); (NULL, NULL, 0) unbinds.
   In threaded mode it is a recorded GX call (gen_gx_rec.py row, palette payload nb*48 bytes); the table pointer crosses as-is
   (tables are immutable once built).  serial increments on every bind/unbind. */
typedef struct GxSkinBound { const GxSkinTable* t; u32 nb; u32 serial; float pal[12 * GX_SKIN_MAX_BONES]; } GxSkinBound;
extern GxSkinBound gGxSkin;
void GXStfxSkinBind(const GxSkinTable* t, const f32* palette, u32 nb);

/* R8 T2: the switch and the census (shared by T2a = HAL, T2b = G side / arm / print).  gGxSkinOn is written by G at a frame edge
   (gx_thread.c: arm 149, or marker gx_skin.txt = 1 in fixed modes) and read by G (objprint) -- one thread, no fence needed.
   gGxSkinCtr: cumulative; the [r8] census prints window deltas. */
extern int gGxSkinOn;
enum { GXSK_BINDS, GXSK_BLEND_SKIPPED, GXSK_REF_TABLE, GXSK_REF_MORPH, GXSK_REF_HAL, GXSK_DRAWS, GXSK_PAL_BYTES, GXSK_N };
extern u32 gGxSkinCtr[GXSK_N];
int gx_skin_model_allowed(const ModelFileHeader* m);   /* R8 isolation instrument: marker gx_skin_only.txt */
/* R8 HAL-side oracle (direct mode + marker gx_skin_check.txt): objprint sets gGxSkinChkCpu to the CPU-blended vtxBuf of the model it binds;
   gx_draw.c's skinned draw calls gx_skin_hal_oracle with the mesh inputs (decoded positions, the POS/NRM indices, the uniform image) and it
   recomputes the vertex program's position per distinct vertex and compares with the CPU bytes. */
extern const u8* gGxSkinChkCpu;
void gx_skin_hal_oracle(const GxSkinTable* t, const float* pos, const u32* fidx, u32 ndist, int hasNrm, const float* img);

/* R8-FB (host/engine/R8_FALLBACK.md): a skinned list the HAL cannot take (dlkCall refused it) used to be DROPPED -- docs/54 hw_R8tour: 420 draws per 60 frames,
   geometry missing.  gx_skin_fallback_fill returns CPU-skinned copies of the bound model's arrays: every entry through gx_skin_ref_pos / _nrm with the BOUND
   palette (gGxSkin.pal), stored in the FILE's format (6-byte BE s16 XYZ, 3-byte s8), NONE entries keeping the file bytes = what the CPU blend writes into vtxBuf.
   Allocated on first use, refilled only when (table id, gGxSkin.serial) changed.  *nrm is NULL unless nrmBase is the model's normals and the table has a normal job.
   Returns 0 (nothing usable) when t is not the bound table or memory is short.  gGxSkinFb[0] = fallback draws, [1] = fills (cumulative). */
int gx_skin_fallback_fill(const GxSkinTable* t, const void* nrmBase, const u8** pos, const u8** nrm);
void gx_skin_hal_fallback(void);
extern u32 gGxSkinFb[2];
