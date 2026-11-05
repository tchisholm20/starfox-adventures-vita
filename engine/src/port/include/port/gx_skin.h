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

#define GX_SKIN_MAX_BONES 128     /* 2026-11-03 R8b: was 32 (hub dumps 2026-10-26: 26, 26, 20 bones).  128 = game_scene.cpp MAX_JOINTS; slot 0xFF stays NONE.  The VS draw palette is still VS_SKIN_MAX_BONES (32, gx_tev_key.h):
                                     a table with more slots than that is an EXTENDED table; each display-list record maps the slots it uses onto the VS's 32 (R8b T2: gx_skin_hal_map / _attr_local / gx_skin_vs_select) */
#define GX_SKIN_NONE      0xFFu   /* entry.a == entry.b == GX_SKIN_NONE: not skinned, pass through */
#define GX_SKIN_MTX_BYTES 0x30    /* sizeof(ROMtx): 3x4 floats, COLUMN-major (m[0..2] = x column, m[9..11] = translation) */

typedef enum GxSkinWhy {
    GX_SKIN_OK = 0,
    GX_SKIN_REFUSE_NBT,       /* NBT-normal model.  R8b 2026-11-03: no longer returned (an NBT table builds, ext bit GX_SKIN_EXT_NBT); kept so the numbering and old readers stay */
    GX_SKIN_REFUSE_BONES,     /* more than GX_SKIN_MAX_BONES distinct bones */
    GX_SKIN_REFUSE_ADDR,      /* addressing the builder cannot map (mixed vertex, overrun, bad scale, ...) */
    GX_SKIN_REFUSE_NOJOBS,    /* no chunks in either job: nothing to skin */
    GX_SKIN_REFUSE_CPUUSE     /* R8-MF C1: the CPU reads / writes this model's vtxBuf (gx_skin_note_cpu_user): the CPU blend must keep it current */
} GxSkinWhy;

/* 2026-11-03 R8b: WHY a table is extended (GxSkinTable.ext).  0 = a table exactly as before R8b (<= 32 bones, 3-byte normals).  G treats an extended table as
   refused unless gx_skin_ext_on() (the skin_ext.txt switch); the HAL reads the bits to know what the draw side must do. */
#define GX_SKIN_EXT_NBT   1u      /* the normal stream is NBT: nrmStride 9, one entry per NRM triplet (N, B, T skinned by the same entry) */
#define GX_SKIN_EXT_BONES 2u      /* nb > 32: more slots than the VS palette holds (VS_SKIN_MAX_BONES) */

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
    u8 nb;                    /* palette bones (<= GX_SKIN_MAX_BONES = 128, fits a u8) */
    u8 bones[GX_SKIN_MAX_BONES]; /* slot -> index into gObjBoneMtxBuffer (matrix = base + index * 0x30) */
    GxSkinEntry* pos;
    GxSkinEntry* nrm;
    u8 nrmStride;             /* R8b: bytes of one NRM entry in the file: 3, or 9 for an NBT model (N, B, T = three s8x3).  3 when there is no normal job */
    u8 ext;                   /* R8b: GX_SKIN_EXT_* bits; 0 = not extended */
    /* R8-FB: the CPU-skinned copy of this model's POS (nPos*6) then NRM (nNrm*3) arrays a refused skinned list is drawn from (gx_skin_fallback_fill).  Written by R only, lazily; freed with the table */
    u8* fb; u32 fbId, fbSerial;
    u32 nbtRaw;               /* R8b 2026-11-03: NBT bytes a later chunk's raw block copy restored to rest pose on the CPU path; the GPU skins them (gx_skin.c build_stream) */
} GxSkinTable;
/* R8b: 1 = refuse an NBT triplet that a later raw block copy partly restores (the pre-2026-11-03 rule); host tests / mutants only */
extern int gx_skin_nbt_strict;

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
/* R8b: an NBT table's entry idx = one TRIPLET.  src = the 9 FILE bytes (N, B, T); all three vectors go through the same entry exactly as
   ObjModel_TransformNormalTriplets does (each as gx_skin_ref_nrm does a plain normal).  outF / outQ: N then B then T. */
void gx_skin_ref_nrm_nbt(const GxSkinTable* t, const float* palette, u32 idx, const u8* src, float outF[9], s8 outQ[9]);

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
enum { GXSK_BINDS, GXSK_BLEND_SKIPPED, GXSK_REF_TABLE, GXSK_REF_MORPH, GXSK_REF_HAL, GXSK_DRAWS, GXSK_PAL_BYTES,
       GXSK_REF_EXT,   /* 2026-11-03 R8b: an EXTENDED table (NBT / > 32 bones) with gx_skin_ext_on() == 0: treated exactly like a refused table (CPU blend, no bind).  Appended: the first seven indexes are unchanged */
       GXSK_REF_BONES, /* 2026-11-03 R8b T2: a skinned list that needs more than VS_SKIN_MAX_BONES (32) distinct palette slots: HAL refuses, the CPU-skinned copy draws it (counted per draw call).  Appended */
       GXSK_N };
extern u32 gGxSkinCtr[GXSK_N];
/* R8b switch (2026-11-03): marker ux0:data/stairfax/skin_ext.txt, read ONCE (one log line).  "1" = on; absent, "0" or anything else = off;
   "ab <N>" = on / off alternating every N presents (N rounded UP to a multiple of 60; ON first), counted from the weak rhi_vita_presents.
   Off is today's behaviour: G treats an extended table as refused.  Host: the marker is absent -> off; gx_skin_ext_set is the test hook. */
int gx_skin_ext_on(void);
/* the marker's text -> 0 off / 1 on / 2 ab (*n = presents per arm, rounded up to a multiple of 60); pure, so the host tests hold the rules.  b is NUL-terminated. */
int gx_skin_ext_parse(const char* b, u32* n);
/* G's gate (objprint stfxSkinDecide, right after gx_skin_table_get): 1 = the table may be used now.  An EXTENDED table (t->ext != 0) with the switch off is a refused
   table: 0, and ++gGxSkinCtr[GXSK_REF_EXT] (the caller then takes the CPU blend and binds nothing).  A non-extended table is always usable. */
int gx_skin_ext_gate(const GxSkinTable* t);
/* test hook: mode -1 = back to the marker (re-read), 0 off, 1 on, 2 ab with n presents per arm (rounded up to a multiple of 60 exactly as the marker's) */
void gx_skin_ext_set(int mode, u32 n);
/* ---- 2026-11-03, R8b T2: the per-record local palette (host/engine/R8B_NBT_BONES.md Design 2) ----
   The VS palette holds VS_SKIN_MAX_BONES (32) slots; a table may have up to 128.  A display-list record maps the table slots ITS distinct vertices use (POS entry and NRM entry) to local slots
   0..31 in first-use order; aSk carries LOCAL slots and the draw's uBone image is gathered from the bound palette by the same list.  A table of <= 32 bones is the identity map (list[i] = i, n = nb):
   its aSk bytes and its uBone image are exactly what they were before R8b.
   gx_skin_hal_map: 1 = list / *n filled; 0 = an index outside the table (dlkSkinPrep's reason 6); 2 = the record needs more than VS_SKIN_MAX_BONES slots (CPU fallback, GXSK_REF_BONES).
   list must hold VS_SKIN_MAX_BONES bytes. */
int gx_skin_hal_map(const GxSkinTable* t, const u32* fidx, u32 ndist, int hasNrm, u8* n, u8* list);
/* aSk bytes of one vertex with the slots mapped through (n, list) (identity, list ignored, for a table of <= 32 bones).  0 = an index outside the table or a slot the list does not hold. */
int gx_skin_hal_attr_local(const GxSkinTable* t, u32 pi, u32 ni, int hasNrm, u8 out[8], u32 n, const u8* list);
/* the draw about to be issued: gx_skin_vs_palette() gathers its uBone image from the bound palette rows list[0..n-1] (NULL / 0 = the bound palette as it is, a table of <= 32 bones only).  Set by
   gx_draw.c right before the backend draw and cleared right after (R and the backend share a thread). */
void gx_skin_vs_select(const u8* list, u32 n);
extern u32 gGxSkinPalGather;   /* tests: how many times gx_skin_vs_palette rebuilt its image (an unchanged bind / list is not regathered) */
u32 gx_skin_hal_nrm_stride(const GxSkinTable* t);   /* bytes of one NRM entry in the FILE: 3, or 9 for an NBT table (the list's NRM array must be bound at this stride) */
void gx_skin_hal_bones(void);                       /* ++GXSK_REF_BONES */
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
