// gx_skin.c -- R8 GPU skinning, T1 (host/engine/R8_GPUSKIN.md): skin table, palette pack, reference of the VS math, S3 dump.
// Proved bit-for-bit against the REAL kernels (hal/model_asm_twins.c) by host/tests/gx_skin_test.c.
#include "port/gx_skin.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__vita__)
#include <sys/stat.h>
#endif

#define BLOCK 32u
static u32 mint_table_id(void);

/* ---------------- reference math (the VS, in float32, in the kernel operation order) ---------------- */
static float skin_w(u8 w) { return (float)w * (1.0f / 128.0f); }

/* __OSf32tos16 / __OSf32tos8: clamp to the integer range, then C truncation toward zero. Returned as a float: the VS has no s16. */
static float skin_q16(float f) {
    f = f > 32767.0f ? 32767.0f : f < -32768.0f ? -32768.0f : f;
    return (float)(s16)f;
}
static float skin_q8(float f) {
    f = f > 127.0f ? 127.0f : f < -128.0f ? -128.0f : f;
    return (float)(s8)f;
}

static float skin_scale(u32 q) { return (float)(1u << (q & 0x3f)); }

/* P = 3 rows of float4 for one bone */
static float skin_row4(const float* P, int r, float x, float y, float z) {
    return P[4 * r + 0] * x + P[4 * r + 1] * y + P[4 * r + 2] * z + P[4 * r + 3];
}
static float skin_row3(const float* P, int r, float x, float y, float z) {
    return P[4 * r + 0] * x + P[4 * r + 1] * y + P[4 * r + 2] * z;
}

void gx_skin_ref_pos(const GxSkinTable* t, const float* palette, u32 idx, const u8* src, float outF[3], s16 outQ[3]) {
    float scale = skin_scale(t->posQ), inv = 1.0f / scale;
    float x = (float)(s16)(src[0] << 8 | src[1]) * inv;
    float y = (float)(s16)(src[2] << 8 | src[3]) * inv;
    float z = (float)(s16)(src[4] << 8 | src[5]) * inv;
    const GxSkinEntry* e = &t->pos[idx];
    float q[3];
    int k;
    if (e->a == GX_SKIN_NONE) {
        q[0] = (float)(s16)(src[0] << 8 | src[1]); q[1] = (float)(s16)(src[2] << 8 | src[3]); q[2] = (float)(s16)(src[4] << 8 | src[5]);
    } else {
        const float* A = palette + 12 * e->a;
        const float* B = palette + 12 * e->b;
        float w0 = skin_w(e->w0), w1 = skin_w(e->w1);
        float ox = skin_row4(A, 0, x, y, z) * w0 + skin_row4(B, 0, x, y, z) * w1;
        float oy = skin_row4(A, 1, x, y, z) * w0 + skin_row4(B, 1, x, y, z) * w1;
        float oz = skin_row4(A, 2, x, y, z) * w0 + skin_row4(B, 2, x, y, z) * w1;
        q[0] = skin_q16(ox * scale); q[1] = skin_q16(oy * scale); q[2] = skin_q16(oz * scale);
    }
    for (k = 0; k < 3; k++) { outQ[k] = (s16)q[k]; outF[k] = q[k] * inv; }
}

void gx_skin_ref_nrm(const GxSkinTable* t, const float* palette, u32 idx, const u8* src, float outF[3], s8 outQ[3]) {
    float scale = skin_scale(t->nrmQ), inv = 1.0f / scale;
    float x = (float)(s8)src[0] * inv, y = (float)(s8)src[1] * inv, z = (float)(s8)src[2] * inv;
    const GxSkinEntry* e = &t->nrm[idx];
    float q[3];
    int k;
    if (e->a == GX_SKIN_NONE) {
        q[0] = (float)(s8)src[0]; q[1] = (float)(s8)src[1]; q[2] = (float)(s8)src[2];
    } else {
        const float* A = palette + 12 * e->a;
        const float* B = palette + 12 * e->b;
        float w0 = skin_w(e->w0), w1 = skin_w(e->w1);
        float ox = skin_row3(A, 0, x, y, z) * w0 + skin_row3(B, 0, x, y, z) * w1;
        float oy = skin_row3(A, 1, x, y, z) * w0 + skin_row3(B, 1, x, y, z) * w1;
        float oz = skin_row3(A, 2, x, y, z) * w0 + skin_row3(B, 2, x, y, z) * w1;
        q[0] = skin_q8(ox * scale); q[1] = skin_q8(oy * scale); q[2] = skin_q8(oz * scale);
    }
    for (k = 0; k < 3; k++) { outQ[k] = (s8)q[k]; outF[k] = q[k] * inv; }
}

/* ---------------- palette ---------------- */
int gx_skin_palette_pack(const GxSkinTable* t, const void* boneMtxBuffer, float* out) {
    int s, r, c;
    for (s = 0; s < t->nb; s++) {
        const float* m = (const float*)((const u8*)boneMtxBuffer + (size_t)t->bones[s] * GX_SKIN_MTX_BYTES);
        float* o = out + 12 * s;
        for (r = 0; r < 3; r++)
            for (c = 0; c < 4; c++)
                o[4 * r + c] = m[3 * c + r];
    }
    return 12 * t->nb;
}

/* ---------------- table ---------------- */
static u32 vbyte(u32 v, u32 stride) { return v * stride; }

/* owner: 0 untouched, 0xFFFFFFFF raw block copy of the source, i+1 = transformed by chunk i; rel = byte offset inside that chunk's run */
typedef struct { u32 owner; u32 rel; } Own;

static int build_stream(const ModelVtxAnimJob* job, u32 count, u32 stride, GxSkinEntry* ent, u8* slotOf, GxSkinTable* t) {
    u32 total = count * stride, i, v, j, b;
    Own* ow;
    int why = GX_SKIN_OK;
    if (count == 0) return GX_SKIN_REFUSE_ADDR;
    ow = (Own*)calloc(total ? total : 1, sizeof(Own));
    if (!ow) return GX_SKIN_REFUSE_ADDR;
    for (v = 0; v < count; v++) { ent[v].a = ent[v].b = GX_SKIN_NONE; ent[v].w0 = ent[v].w1 = 0; }
    for (i = 0; i < job->chunkCount && why == GX_SKIN_OK; i++) {
        const ModelVtxAnimChunk* c = &job->chunks[i];
        u32 blockBytes = (u32)c->vtxBlocks * BLOCK, run = (u32)c->vtxCount * stride, dst = c->dstByteOffset;
        if (c->srcDataOffset < 0 || dst + run > blockBytes) { why = GX_SKIN_REFUSE_ADDR; break; }
        if (c->vtxCount && (!c->weightStream || 2u * c->vtxCount > (u32)c->weightBlocks * BLOCK)) { why = GX_SKIN_REFUSE_ADDR; break; }
        for (b = 0; b < blockBytes; b++) {
            u32 a = (u32)c->srcDataOffset + b;
            if (a >= total) break;
            if (b >= dst && b < dst + run) { ow[a].owner = i + 1; ow[a].rel = b - dst; }
            else { ow[a].owner = 0xFFFFFFFFu; ow[a].rel = 0; }
        }
    }
    for (v = 0; v < count && why == GX_SKIN_OK; v++) {
        u32 first = vbyte(v, stride);
        u32 o;
        if (first + stride > total) break;
        o = ow[first].owner;
        if (o == 0 || o == 0xFFFFFFFFu) {                       /* untouched or raw copy of the source: unskinned */
            for (j = 0; j < stride; j++)
                if (ow[first + j].owner != 0 && ow[first + j].owner != 0xFFFFFFFFu) { why = GX_SKIN_REFUSE_ADDR; break; }
        } else {
            const ModelVtxAnimChunk* c = &job->chunks[o - 1];
            u32 rel = ow[first].rel;
            u8 idxs[2], sl[2], slotA, slotB;
            GxSkinEntry e;
            int w;
            if (rel % stride) { why = GX_SKIN_REFUSE_ADDR; break; }
            for (j = 0; j < stride; j++)
                if (ow[first + j].owner != o || ow[first + j].rel != rel + j) { why = GX_SKIN_REFUSE_ADDR; break; }
            if (why != GX_SKIN_OK) break;
            idxs[0] = c->mtxIdxA; idxs[1] = c->mtxIdxB;
            for (w = 0; w < 2; w++) {
                if (slotOf[idxs[w]] == GX_SKIN_NONE) {
                    if (t->nb >= GX_SKIN_MAX_BONES) { why = GX_SKIN_REFUSE_BONES; break; }
                    t->bones[t->nb] = idxs[w]; slotOf[idxs[w]] = t->nb++;
                }
                sl[w] = slotOf[idxs[w]];
            }
            if (why != GX_SKIN_OK) break;
            slotA = sl[0]; slotB = sl[1];
            e.a = slotA; e.b = slotB;
            e.w0 = c->weightStream[2 * (rel / stride)]; e.w1 = c->weightStream[2 * (rel / stride) + 1];
            ent[v] = e;
        }
    }
    free(ow);
    return why;
}

void gx_skin_table_free(GxSkinTable* t) {
    if (!t) return;
    free(t->pos); free(t->nrm); free(t->fb); free(t);   /* R8-FB: free(t->fb) added -- the CPU-skinned fallback copy lives with the table */
}

GxSkinTable* gx_skin_table_build(const ModelFileHeader* m, int* whyOut) {
    GxSkinTable* t;
    u8 slotOf[256];
    int why = GX_SKIN_OK, hasP = m->vertexAnimJob.chunkCount != 0, hasN = m->normalAnimJob.chunkCount != 0;
    if (whyOut) *whyOut = GX_SKIN_OK;
    if (!hasP && !hasN) why = GX_SKIN_REFUSE_NOJOBS;
    else if (hasN && (m->flags24 & MODEL_FLAGS24_NBT_NORMALS)) why = GX_SKIN_REFUSE_NBT;
    else if ((hasP && (m->vertexAnimJob.quantShift & 0x3f) > 30) || (hasN && (m->normalAnimJob.quantShift & 0x3f) > 30)) why = GX_SKIN_REFUSE_ADDR;
    t = (GxSkinTable*)calloc(1, sizeof(*t));
    if (!t) { if (whyOut) *whyOut = GX_SKIN_REFUSE_ADDR; return NULL; }
    t->key = m; t->id = mint_table_id(); t->vertices = (const void*)m->vertices; t->normals = (const void*)m->normals;
    memset(slotOf, GX_SKIN_NONE, sizeof(slotOf));
    if (why == GX_SKIN_OK && hasP) {
        t->nPos = m->vertexCount;
        t->posQ = (u8)(m->vertexAnimJob.quantShift & 0x3f);
        t->pos = (GxSkinEntry*)calloc(t->nPos ? t->nPos : 1, sizeof(GxSkinEntry));
        why = t->pos ? build_stream(&m->vertexAnimJob, t->nPos, 6, t->pos, slotOf, t) : GX_SKIN_REFUSE_ADDR;
    }
    if (why == GX_SKIN_OK && hasN) {
        t->nNrm = m->normalCount;
        t->nrmQ = (u8)(m->normalAnimJob.quantShift & 0x3f);
        t->nrm = (GxSkinEntry*)calloc(t->nNrm ? t->nNrm : 1, sizeof(GxSkinEntry));
        why = t->nrm ? build_stream(&m->normalAnimJob, t->nNrm, 3, t->nrm, slotOf, t) : GX_SKIN_REFUSE_ADDR;
    }
    if (why != GX_SKIN_OK) {
        gx_skin_table_free(t);
        if (whyOut) *whyOut = why;
        return NULL;
    }
    /* a stream without chunks: one NONE entry so the reference always has an array to index */
    if (!t->pos) { t->pos = (GxSkinEntry*)calloc(1, sizeof(GxSkinEntry)); if (t->pos) t->pos[0].a = t->pos[0].b = GX_SKIN_NONE; }
    if (!t->nrm) { t->nrm = (GxSkinEntry*)calloc(1, sizeof(GxSkinEntry)); if (t->nrm) t->nrm[0].a = t->nrm[0].b = GX_SKIN_NONE; }
    return t;
}

/* ---------------- the table cache and its lifetime (R8-MF, host/engine/R8_MUSTFIX_BRIEF.md) ----------------
   Three defects kept GPU skinning from being the default (R8_GPUSKIN.md "MUST FIX"):
   (a) a table was freed the moment the 128-slot ring evicted it, while an objprint skin slot, a GXStfxSkinBind record still in the G->R ring, gGxSkin.t on R or a
       display-list record could hold it -> use-after-free.  Now NOTHING frees a table directly: retire_table() queues it, and gx_skin_frame_edge() frees it once R has
       EXECUTED every record G made before the retire (g_gxt: G's PRESENT seq and r_presented, passed in by the tick).  No "wait N frames" guess.
   (b) the cache was keyed by the ModelFileHeader address alone: ObjModel_Release frees the file (mm_free) and the next load may land at the same address -> the old table or
       the old REFUSAL came back.  gx_skin_model_released() (model.c) retires the entry before the free; as a backstop each entry also stores the identity of the header
       it was built from, and a get whose pointer matches but whose identity differs retires the entry and rebuilds.
   (c) with GPU skin on the CPU blend is skipped, so vtxBuf is stale; ObjModel_GetCurrentVertexCoords hands it to the bridge objects, which read AND write it.
       gx_skin_note_cpu_user() marks the model; from then on get refuses it (GX_SKIN_REFUSE_CPUUSE).
   Every table also carries a never-reused id (u32, never 0): a pointer is not an identity once memory is reused (ABA), so the HAL's per-record memos compare ids and
   never dereference a pointer they kept across frames. */
static u32 sNextTableId;
static u32 mint_table_id(void) { if (++sNextTableId == 0) ++sNextTableId; return sNextTableId; }

typedef struct { u32 modelId, vertexCount, normalCount, vChunks, nChunks, flags24; const void *vertices, *normals; } SkinIdent;
static void ident_of(const ModelFileHeader* m, SkinIdent* o) {
    memset(o, 0, sizeof *o);
    o->modelId = (u32)m->modelId; o->vertexCount = (u32)m->vertexCount; o->normalCount = (u32)m->normalCount;
    o->vChunks = (u32)m->vertexAnimJob.chunkCount; o->nChunks = (u32)m->normalAnimJob.chunkCount; o->flags24 = (u32)m->flags24;
    o->vertices = (const void*)m->vertices; o->normals = (const void*)m->normals;
}
static int ident_eq(const SkinIdent* a, const SkinIdent* b) { return !memcmp(a, b, sizeof *a); }

typedef struct {
    const ModelFileHeader* key;
    SkinIdent id;
    GxSkinTable* t;           /* NULL = a cached refusal (built == 1) or nothing built yet (built == 0) */
    int why, built;
    u8 cpuUse;                /* C1: survives a rebuild of this header; dropped with the entry (release / identity change) */
    u32 gets;                 /* gets that handed out a table: ~ GPU-skin decisions (the gate asks once per model per frame) */
    u32 used;                 /* LRU stamp */
} SkinEntry;
static SkinEntry* sEnt; static int sEntN, sEntCap;
static u32 sUse;
static const ModelFileHeader* sLastMarked;   /* ObjModel_GetCurrentVertexCoords is per frame per bridge: skip the search when it is the same file */

typedef struct { GxSkinTable* t; u32 stamp; } SkinRetired;
static SkinRetired* sRet; static int sRetN, sRetCap;
static u32 sLastG;   /* G's PRESENT seq as of the last gx_skin_frame_edge */

/* A3: the cache GROWS (a skin table is small next to its model, and release retires its entry).  Only this safety cap evicts, and it evicts through the retire path. */
#define SKIN_CACHE_MAX 256

/* R8-MF lifetime census (docs/54 2026-10-26, before the R8 default flip): 0 releases of a cached header, 1 of them with a LIVE table,
 * 2 tables retired, 3 tables freed by the frame-edge tick.  A clean boot that never released a live table proves nothing: [r8-life] says
 * whether the deferred-free path actually ran.  Cumulative; vita_tool_main.c prints it with the [r8] census. */
u32 gGxSkinLife[4];
u32 gGxSkinTick[4];   /* hw_R8tour2: the LAST tick's gseq, rdone, ringOn, and the oldest pending stamp (0 = none): why does a retired table never free on the device? */

static void retire_table(GxSkinTable* t) {
    if (!t) return;
    ++gGxSkinLife[2];
    if (sRetN == sRetCap) {
        SkinRetired* n = (SkinRetired*)realloc(sRet, (size_t)(sRetCap ? sRetCap * 2 : 16) * sizeof *n);
        if (!n) return;   /* out of memory: LEAK the table rather than free one a record may still hold */
        sRet = n; sRetCap = sRetCap ? sRetCap * 2 : 16;
    }
    /* Records G made so far precede the PRESENT of seq g_gxt.seq + 1.  The tick runs right before each present, so g_gxt.seq <= sLastG + 1 at any later point of
       the frame: sLastG + 2 is never too early (at most one frame too late). */
    sRet[sRetN].t = t; sRet[sRetN].stamp = sLastG + 2u; ++sRetN;
}

/* G, once per present (vita_tool_main.c __wrap_rhi_present, before the real present).  gseq = g_gxt.seq, rdone = g_gxt.r_presented (acquire), ringOn = g_gxt.on.
   Free what R has passed: (int32)(rdone - stamp) >= 0.  Ring off (direct: R == G, or parked) = every record has run, so everything retired is free. */
void gx_skin_frame_edge(uint32_t gseq, uint32_t rdone, int ringOn) {
    int i, k = 0;
    sLastG = gseq;
    gGxSkinTick[0] = gseq; gGxSkinTick[1] = rdone; gGxSkinTick[2] = (u32)ringOn; gGxSkinTick[3] = sRetN ? sRet[0].stamp : 0u;
    if (sRetN == 0) return;   /* N1: a no-op when nothing is retired */
    for (i = 0; i < sRetN; i++) {
        if (!ringOn || (int32_t)(rdone - sRet[i].stamp) >= 0) { gx_skin_table_free(sRet[i].t); ++gGxSkinLife[3]; }
        else sRet[k++] = sRet[i];
    }
    sRetN = k;
}
u32 gx_skin_retired_pending(void) { return (u32)sRetN; }
u32 gx_skin_table_id(const GxSkinTable* t) { return t ? t->id : 0u; }

static SkinEntry* ent_find(const ModelFileHeader* m) {
    int i;
    for (i = 0; i < sEntN; i++) if (sEnt[i].key == m) return &sEnt[i];
    return NULL;
}
static void ent_drop_table(SkinEntry* e) { retire_table(e->t); e->t = NULL; e->built = 0; e->why = GX_SKIN_OK; }
static void ent_remove(SkinEntry* e) {
    ent_drop_table(e);
    *e = sEnt[--sEntN];
}
static SkinEntry* ent_new(const ModelFileHeader* m) {
    SkinEntry* e;
    if (sEntN == sEntCap) {
        if (sEntN >= SKIN_CACHE_MAX) {   /* A3 safety cap: evict the least recently used entry WITHOUT a CPU-user mark, through the retire path */
            int i, best = -1;
            for (i = 0; i < sEntN; i++) if (!sEnt[i].cpuUse && (best < 0 || (s32)(sEnt[i].used - sEnt[best].used) < 0)) best = i;
            if (best < 0) best = 0;
            ent_remove(&sEnt[best]);
        } else {
            SkinEntry* n = (SkinEntry*)realloc(sEnt, (size_t)(sEntCap ? sEntCap * 2 : 16) * sizeof *n);
            if (!n) return NULL;
            sEnt = n; sEntCap = sEntCap ? sEntCap * 2 : 16;
        }
    }
    e = &sEnt[sEntN++];
    memset(e, 0, sizeof *e);
    e->key = m; ident_of(m, &e->id);
    return e;
}

const GxSkinTable* gx_skin_table_get(const ModelFileHeader* m, int* why) {
    int w = GX_SKIN_OK;
    SkinIdent id;
    SkinEntry* e = ent_find(m);
    GxSkinTable* t;
    ident_of(m, &id);
    if (e && !ident_eq(&e->id, &id)) {   /* B2: same address, another model (a free that never went through ObjModel_Release) */
        e->cpuUse = 0; e->gets = 0; ent_drop_table(e); e->id = id;
    }
    if (e && e->cpuUse) { if (why) *why = GX_SKIN_REFUSE_CPUUSE; return NULL; }   /* C1 */
    if (e && e->built) {
        e->used = ++sUse;
        if (e->t) ++e->gets;
        if (why) *why = e->why;
        return e->t;
    }
    t = gx_skin_table_build(m, &w);
    if (t) {   /* R8 diagnostic (2026-10-26): what an exact-in-fp16 single-attribute encoding would need -- weight sums and slots of the USED entries */
        u32 k, ps = 0, pb = 0, ns = 0, nbad = 0, mx = 0;
        for (k = 0; k < t->nPos; k++) if (t->pos[k].a != GX_SKIN_NONE) { ++ps; if (t->pos[k].w0 + t->pos[k].w1 != 128) ++pb; if (t->pos[k].a > mx) mx = t->pos[k].a; if (t->pos[k].b > mx) mx = t->pos[k].b; }
        for (k = 0; k < t->nNrm; k++) if (t->nrm[k].a != GX_SKIN_NONE) { ++ns; if (t->nrm[k].w0 + t->nrm[k].w1 != 128) ++nbad; if (t->nrm[k].a > mx) mx = t->nrm[k].a; if (t->nrm[k].b > mx) mx = t->nrm[k].b; }
        printf("[r8-tab] model %04x nb %u: pos skinned %u (w-sum != 128: %u), nrm skinned %u (w-sum != 128: %u), max slot %u\n", (unsigned) m->modelId, (unsigned) t->nb, (unsigned) ps, (unsigned) pb, (unsigned) ns, (unsigned) nbad, (unsigned) mx);
    }
    if (!e) e = ent_new(m);
    if (!e) { if (t) retire_table(t); if (why) *why = GX_SKIN_REFUSE_ADDR; return NULL; }   /* out of memory: never hand out a table nothing can retire later */
    e->t = t; e->why = w; e->built = 1; e->used = ++sUse; e->gets = t ? 1u : 0u;
    if (why) *why = w;
    return t;
}

/* B1: ObjModel_Release (model.c), before mm_free(header). */
void gx_skin_model_released(const ModelFileHeader* m) {
    SkinEntry* e = ent_find(m);
    if (sLastMarked == m) sLastMarked = NULL;
    if (e) { ++gGxSkinLife[0]; if (e->t) ++gGxSkinLife[1]; ent_remove(e); }
}

/* C1: ObjModel_GetCurrentVertexCoords (model.c) hands vtxBuf to the CPU.  The first mark of a model that has already been GPU-skinned prints the incident line:
   the device boot then shows whether any frame drew a stale vtxBuf. */
void gx_skin_note_cpu_user(const ModelFileHeader* m) {
    SkinEntry* e;
    SkinIdent id;
    if (m == sLastMarked) return;
    ident_of(m, &id);
    e = ent_find(m);
    if (e && !ident_eq(&e->id, &id)) { e->cpuUse = 0; e->gets = 0; ent_drop_table(e); e->id = id; }
    if (!e) e = ent_new(m);
    if (!e) return;
    sLastMarked = m;
    if (e->cpuUse) return;
    if (e->gets) printf("[r8-cpu] model %04x marked CPU vtxBuf user after %u GPU-skinned frames\n", (unsigned) m->modelId, (unsigned) e->gets);
    e->cpuUse = 1;
    ent_drop_table(e);   /* G stops handing it out; objprint's slot sees it dead through gx_skin_table_alive, and the free is deferred */
}

/* objprint's slot kept a table pointer across frames: is (t, id) still a live table of the cache?  Never dereferences t before it is found in the cache (it may be gone). */
int gx_skin_table_alive(const GxSkinTable* t, u32 id) {
    int i;
    if (!t) return 0;
    for (i = 0; i < sEntN; i++) if (sEnt[i].t == t && sEnt[i].built) return t->id == id;
    return 0;
}

void gx_skin_cache_clear(void) {
    int i;
    for (i = 0; i < sEntN; i++) retire_table(sEnt[i].t);
    sEntN = 0; sLastMarked = NULL;
}

/* ---------------- S3 dump ----------------
   little-endian, as the Vita stores it:
     "SKN1"; u32 flags24, vertexCount, normalCount;
     twice (vertex job, then normal job): u32 quantShift, chunkCount; per chunk: s32 srcDataOffset; u8 mtxIdxA, mtxIdxB, weightBlocks,
        vtxBlocks, dstByteOffset, 0, 0, 0; u32 vtxCount; u32 weightBytes (= weightBlocks*32); u8 weights[weightBytes];
     u32 posLen; u8 srcVertices[posLen]; u32 nrmLen; u8 srcNormals[nrmLen];   (posLen = max(vertexCount*6, max chunk end), nrmLen likewise
                                                                                with 3 (or 9 for NBT) bytes per normal)
     u32 nBones; per bone: u32 index; f32 m[12];                                (every distinct mtxIdxA/B of either job, ascending)
     u32 posLen; u8 cpuOutVertices[posLen]; u32 nrmLen; u8 cpuOutNormals[nrmLen]. */
static void w32(FILE* f, u32 v) { fwrite(&v, 4, 1, f); }
static u32 stream_len(const ModelVtxAnimJob* j, u32 base) {
    u32 i, n = base;
    for (i = 0; i < j->chunkCount; i++) {
        u32 e = (u32)j->chunks[i].srcDataOffset + (u32)j->chunks[i].vtxBlocks * BLOCK;
        if (e > n) n = e;
    }
    return n;
}

int gx_skin_dump_write(FILE* f, const ModelFileHeader* m, const u8* outVtx, const u8* outNrm, const u8* mtxs) {
    const ModelVtxAnimJob* jobs[2];
    u8 used[256];
    u32 posLen = stream_len(&m->vertexAnimJob, (u32)m->vertexCount * 6u);
    u32 nrmLen = stream_len(&m->normalAnimJob, (u32)m->normalCount * ((m->flags24 & MODEL_FLAGS24_NBT_NORMALS) ? 9u : 3u));
    u32 i, n, jn;
    jobs[0] = &m->vertexAnimJob; jobs[1] = &m->normalAnimJob;
    memset(used, 0, sizeof(used));
    fwrite("SKN1", 1, 4, f);
    w32(f, m->flags24); w32(f, m->vertexCount); w32(f, m->normalCount);
    for (jn = 0; jn < 2; jn++) {
        w32(f, jobs[jn]->quantShift); w32(f, jobs[jn]->chunkCount);
        for (i = 0; i < jobs[jn]->chunkCount; i++) {
            const ModelVtxAnimChunk* c = &jobs[jn]->chunks[i];
            u8 h[8];
            w32(f, (u32)c->srcDataOffset);
            h[0] = c->mtxIdxA; h[1] = c->mtxIdxB; h[2] = c->weightBlocks; h[3] = c->vtxBlocks; h[4] = c->dstByteOffset; h[5] = h[6] = h[7] = 0;
            fwrite(h, 1, 8, f);
            w32(f, c->vtxCount); w32(f, (u32)c->weightBlocks * BLOCK);
            fwrite(c->weightStream, 1, (u32)c->weightBlocks * BLOCK, f);
            used[c->mtxIdxA] = used[c->mtxIdxB] = 1;
        }
    }
    w32(f, posLen); fwrite(m->vertices, 1, posLen, f);
    w32(f, nrmLen); fwrite(m->normals, 1, nrmLen, f);
    for (n = 0, i = 0; i < 256; i++) n += used[i];
    w32(f, n);
    for (i = 0; i < 256; i++) if (used[i]) { w32(f, i); fwrite(mtxs + (size_t)i * GX_SKIN_MTX_BYTES, 4, 12, f); }
    w32(f, posLen); fwrite(outVtx, 1, posLen, f);
    w32(f, nrmLen); fwrite(outNrm, 1, nrmLen, f);
    return ferror(f) ? -1 : 0;
}

void gx_skin_dump_once(const ModelFileHeader* m, const u8* outVtx, const u8* outNrm, int vtxBufDirty, const u8* mtxs) {
#if defined(__vita__)
    static int armed = -1;          /* marker ux0:data/stairfax/skin_dump.txt, read once */
    static const void* done[256];
    static int nDone;
    int i;
    char path[96];
    FILE* f;
    if (armed == 0) return;
    if (armed < 0) {
        f = fopen("ux0:data/stairfax/skin_dump.txt", "rb");
        armed = f != NULL;
        if (f) fclose(f);
        if (!armed) return;
        mkdir("ux0:data/stairfax/skin", 0777);
    }
    if (vtxBufDirty || m->vertexAnimJob.chunkCount == 0) return;
    for (i = 0; i < nDone; i++) if (done[i] == m) return;
    if (nDone >= 256) return;
    done[nDone++] = m;
    snprintf(path, sizeof(path), "ux0:data/stairfax/skin/model_%04x.bin", (unsigned)m->modelId);
    f = fopen(path, "wb");
    if (!f) return;
    gx_skin_dump_write(f, m, outVtx, outNrm, mtxs);
    fclose(f);
#else
    (void)m; (void)outVtx; (void)outNrm; (void)vtxBufDirty; (void)mtxs;
#endif
}

/* R8 T2: the bound skin (gx_skin.h).  T2a extends the HAL side (mesh attribute, VsKey, uniforms) around this state. */
GxSkinBound gGxSkin;
int gGxSkinOn;
u32 gGxSkinCtr[GXSK_N];
void GXStfxSkinBind(const GxSkinTable* t, const f32* palette, u32 nb)
{
    if (!t || !palette || nb == 0 || nb > GX_SKIN_MAX_BONES || nb != t->nb) { gGxSkin.t = NULL; gGxSkin.nb = 0; }
    else { ++gGxSkinCtr[GXSK_BINDS]; gGxSkinCtr[GXSK_PAL_BYTES] += nb * 48u; gGxSkin.t = t; gGxSkin.nb = nb; memcpy(gGxSkin.pal, palette, (size_t) nb * 12u * sizeof(float)); }
    ++gGxSkin.serial;
}

/* ---------------- R8 T2a: the HAL side (declared in port/gx_tev_key.h) ----------------
   Pure helpers, so host/tests/gx_skin_test.c can hold them to the real data and mutate them: what a skinned mesh vertex carries (aSk), when a mesh
   may be reused, which arrays a list must read, and the uTMX image the backend uploads.  gx_draw.c calls these; it does not repeat their rules. */
#include "port/gx_tev_key.h"

const struct GxSkinTable* gx_skin_hal_table(const void* posBase)
{
    const GxSkinTable* t = gGxSkin.t;
    if (!t || !posBase) return NULL;
    return posBase == t->vertices ? t : NULL;   /* the table's own copy: the header may already be freed (B1) while a record in the ring still names this table */
}

int gx_skin_hal_check(const struct GxSkinTable* tt, const void* posBase, const void* nrmBase, int hasNrm, int posFrac, int posS16, int nrmFrac, int nrmS8)
{
    const GxSkinTable* t = tt;
    if (posBase != t->vertices) return 1;   /* t->vertices / normals, not t->key's header: that file may be freed while a record still reads this table */
    if (hasNrm && nrmBase != t->normals) return 2;
    if (!posS16 || posFrac != (int)t->posQ) return 3;
    if (hasNrm && (!nrmS8 || nrmFrac != (int)t->nrmQ)) return 4;
    return 0;
}

int gx_skin_hal_attr(const struct GxSkinTable* tt, uint32_t pi, uint32_t ni, int hasNrm, uint8_t out[8])
{
    const GxSkinTable* t = tt;
    const GxSkinEntry* p;
    const GxSkinEntry* n;
    if (pi >= t->nPos) return 0;
    p = &t->pos[pi];
    out[0] = p->a; out[1] = p->b; out[2] = p->w0; out[3] = p->w1;
    if (hasNrm) {
        if (ni >= t->nNrm) return 0;
        n = &t->nrm[ni];
        out[4] = n->a; out[5] = n->b; out[6] = n->w0; out[7] = n->w1;
    } else { out[4] = GX_SKIN_NONE; out[5] = GX_SKIN_NONE; out[6] = 0; out[7] = 0; }
    return 1;
}

int gx_skin_mesh_reusable_id(uint32_t meshId, uint32_t wantId)
{
    return meshId == wantId;   /* ids are never reused; a table POINTER can be (freed, then a new table lands at the same address) and would reuse a mesh built for the old one */
}

const float* gx_skin_vs_palette(void)
{
    static float img[4 * VS_SKIN_PAL_VEC4];
    static u32 serial = ~0u;
    static const GxSkinTable* tab;
    const GxSkinTable* t = gGxSkin.t;
    if (!t) return NULL;
    if (serial != gGxSkin.serial || tab != t) {
        const u32 nf = gGxSkin.nb * 12u;
        memset(img, 0, sizeof img);
        memcpy(img, gGxSkin.pal, (size_t)nf * sizeof(float));
        img[4 * (VS_SKIN_PAL_VEC4 - 1) + 0] = skin_scale(t->posQ);
        img[4 * (VS_SKIN_PAL_VEC4 - 1) + 1] = 1.0f / skin_scale(t->posQ);
        img[4 * (VS_SKIN_PAL_VEC4 - 1) + 2] = skin_scale(t->nrmQ);
        img[4 * (VS_SKIN_PAL_VEC4 - 1) + 3] = 1.0f / skin_scale(t->nrmQ);
        serial = gGxSkin.serial; tab = t;
    }
    return img;
}

void gx_skin_hal_ref(void) { ++gGxSkinCtr[GXSK_REF_HAL]; }

/* R8-FB: see gx_skin.h.  The mutable scratch lives in the (otherwise immutable) table; only R touches it, and the table's deferred free (gx_skin_table_free) releases it. */
u32 gGxSkinFb[2];
void gx_skin_hal_fallback(void) { ++gGxSkinFb[0]; }
int gx_skin_fallback_fill(const GxSkinTable* tc, const void* nrmBase, const u8** pos, const u8** nrm)
{
    GxSkinTable* t = (GxSkinTable*)tc;
    const u8 *fp, *fn;
    u8 *dp, *dn;
    u32 i;
    *pos = NULL; *nrm = NULL;
    if (!t) return 0;
    fp = (const u8*)t->vertices; fn = (const u8*)t->normals;
    if (gGxSkin.t != tc || !t->nPos || !fp) return 0;
    if (!t->fb) { t->fb = (u8*)malloc((size_t)t->nPos * 6u + (size_t)t->nNrm * 3u); if (!t->fb) return 0; t->fbId = 0; }
    dp = t->fb; dn = t->fb + (size_t)t->nPos * 6u;
    if (t->fbId != t->id || t->fbSerial != gGxSkin.serial) {
        for (i = 0; i < t->nPos; i++) {
            float f[3]; s16 q[3];
            gx_skin_ref_pos(t, gGxSkin.pal, i, fp + (size_t)i * 6u, f, q);
            dp[6 * i] = (u8)((u16)q[0] >> 8); dp[6 * i + 1] = (u8)q[0];
            dp[6 * i + 2] = (u8)((u16)q[1] >> 8); dp[6 * i + 3] = (u8)q[1];
            dp[6 * i + 4] = (u8)((u16)q[2] >> 8); dp[6 * i + 5] = (u8)q[2];
        }
        if (t->nNrm && fn) for (i = 0; i < t->nNrm; i++) {
            float f[3]; s8 q[3];
            gx_skin_ref_nrm(t, gGxSkin.pal, i, fn + (size_t)i * 3u, f, q);
            dn[3 * i] = (u8)q[0]; dn[3 * i + 1] = (u8)q[1]; dn[3 * i + 2] = (u8)q[2];
        }
        t->fbId = t->id; t->fbSerial = gGxSkin.serial; ++gGxSkinFb[1];
    }
    *pos = dp;
    if (nrmBase && t->nNrm && fn && nrmBase == (const void*)fn) *nrm = dn;
    return 1;
}
u32 gGxSkinWhy[32];   /* R8 instrument: refusal reasons (gx_draw.c dlkSkinPrep comment), cumulative; printed as [r8-why] */
void gx_skin_hal_why(int w) { ++gGxSkinWhy[(unsigned) w & 31u]; }
void gx_skin_hal_draw(void) { ++gGxSkinCtr[GXSK_DRAWS]; }

/* R8 isolation instrument (2026-10-26, hw_R8 look check): marker ux0:data/stairfax/gx_skin_only.txt = a hex modelId -> only that model is
   GPU-skinned (gx_skin_model_allowed); absent = every model.  Read once. */
int gx_skin_model_allowed(const ModelFileHeader* m) {
    static int only = -2;
    if (only == -2) {
        only = -1;
#if defined(__vita__)
        { FILE* f = fopen("ux0:data/stairfax/gx_skin_only.txt", "rb");
          if (f) { char b[16] = {0}; if (fread(b, 1, sizeof(b) - 1, f) > 0) only = (int)strtoul(b, NULL, 16); fclose(f); printf("[r8] gx_skin_only: model %04x\n", (unsigned)only); } }
#endif
    }
    return only < 0 || (int)m->modelId == only;
}
const u8* gGxSkinChkCpu;
static float sk_q(float v, float sc, float isc, float lo, float hi) { float q = v * sc; q = q < lo ? lo : q > hi ? hi : q; q = q < 0.0f ? -floorf(-q) : floorf(q); return q * isc; }
void gx_skin_hal_oracle(const GxSkinTable* t, const float* pos, const u32* fidx, u32 ndist, int hasNrm, const float* img)
{
    static u32 nCalls, nBadTot, nPrint;
    u32 d, bad = 0, chk = 0;
    if (!gGxSkinChkCpu || !t || !pos || !fidx || !img) return;
    const float* s = img + 4 * (VS_SKIN_PAL_VEC4 - 1);
    for (d = 0; d < ndist; d++) {
        const u32 pi = fidx[2 * d]; u8 sk[8]; int k;
        if (pi == ~0u || pi >= t->nPos) continue;
        if (!gx_skin_hal_attr(t, pi, fidx[2 * d + 1], hasNrm, sk)) continue;
        { const float a = (float)sk[0], b = (float)sk[1], w0 = (float)sk[2], w1 = (float)sk[3];   /* the U8x4 aSkP the VS reads */
          const float* p = pos + 3 * d; float o[3];
          if (a > 254.5f) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
          else {
              const float* A = img + 12 * (int)a; const float* B = img + 12 * (int)b; const float k0 = w0 * (1.0f / 128.0f), k1 = w1 * (1.0f / 128.0f);
              for (k = 0; k < 3; k++) {
                  const float va = A[4 * k] * p[0] + A[4 * k + 1] * p[1] + A[4 * k + 2] * p[2] + A[4 * k + 3];
                  const float vb = B[4 * k] * p[0] + B[4 * k + 1] * p[1] + B[4 * k + 2] * p[2] + B[4 * k + 3];
                  o[k] = sk_q(va * k0 + vb * k1, s[0], s[1], -32768.0f, 32767.0f);
              }
          }
          for (k = 0; k < 3; k++) {
              const s16 c = (s16)((gGxSkinChkCpu[6 * pi + 2 * k] << 8) | gGxSkinChkCpu[6 * pi + 2 * k + 1]);
              const float want = (float)c * s[1];
              ++chk;
              if (o[k] != want) { ++bad; if (nPrint < 40) { ++nPrint; printf("[r8-hal] MISMATCH dist %u pos %u.%d: vs %f cpu %f (s16 %d) | a %d b %d w %d %d | in %f %f %f\n",
                  (unsigned)d, (unsigned)pi, k, (double)o[k], (double)want, (int)c, (int)a, (int)b, (int)w0, (int)w1, (double)p[0], (double)p[1], (double)p[2]); } }
          }
        }
    }
    ++nCalls; nBadTot += bad;
    if ((nCalls & 127) == 1 || bad) { static u32 nl; if (nl++ < 300) printf("[r8-hal] draw %u: %u of %u values mismatch (total %u), ndist %u\n", (unsigned)nCalls, (unsigned)bad, (unsigned)chk, (unsigned)nBadTot, (unsigned)ndist); }
}
