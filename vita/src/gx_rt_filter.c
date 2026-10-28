/* gx_rt_filter.c -- THE G-SIDE REDUNDANT-STATE FILTER of the GX render thread (2026-10-21, locked-30 lever B).
 *
 * WHY.  R is the frame's bottleneck (58 ms of a 58 ms frame at the hub).  The counter in gx_thread.c (marker gx_thread_red.txt, "[gxt] red") measured on hardware that ~60% of the ~18,500
 * state-setter records per frame carry a payload IDENTICAL to what the same (op, target) was last set to, and each costs R ~0.6 us to replay.  This module turns that measurement into an
 * action: a recorded setter whose payload equals, word for word, the last RECORDED payload of the same (op, target) is rolled back off the ring on G, before R ever sees it.
 *
 * THE CONTRACT.  What R executes must leave the HAL in exactly the state it would have had without the filter, so the picture is bit-identical.  A dropped record is dropped only if the
 * HAL state it would write is PROVABLY already in place.  Everything below is therefore a WHITELIST:
 *   - FILTER ops (k_filter): modelled setters.  Remembered per target; dropped when equal.
 *   - NEUTRAL ops (k_other, CLS_NEUTRAL): change none of the state the filter remembers.  Remembered values survive them.
 *   - every other op, listed or not (k_other CLS_FORGET, or an op nobody named): FORGETS EVERYTHING.  A new op is safe by default; it only costs yield.
 *   - GXClearVtxDesc zeroes the VtxDesc row only (CLS_CLEARVD).
 * The classification is ONE table, here.  The hook that consults it at allocation time is the inline part of gxt_rec_alloc (gx_rt.h), so it covers EVERY recorder (the generated R rows, the
 * custom X rows of gx_rt_special.c, the ring's own PRESENT / MODE_LEAVE / ABANDON), not only the generated wrappers.  The generated R wrapper of a FILTER op (gxt_flt_cls has GXT_FLT_CLS_F)
 * builds its payload on the STACK and asks gxt_flt_pre BEFORE it allocates: a redundant call returns without touching the ring at all.
 *
 * v2 (2026-10-21, "compare before write").  v1 allocated the record, wrote it, read it back from the ring, compared it with a 72-byte entry of a 225 KB table and rolled the ring back;
 * on hardware that cost G ~9 ms/frame (~1 us per filterable record) to save R ~5 ms.  v2 removes, per call: the ring write and read-back of a dropped record, the rollback and its
 * slow-path / red-counter special cases, the memcmp / memcpy calls; and per frame: 278 GXClearVtxDesc wipes of 128 entries (128 cache lines each) -- an op's values are now
 * dropped by bumping that op's generation, O(1).  Each op's shadow is sized to the op (its key range and its payload width), so a lookup touches one small slot.
 *
 * WHAT THE HAL ACTUALLY DOES (read, not assumed; <engine tree>/src/port/src/hal): every FILTER setter below is a pure store into a static (gx_tev.c: gTev / gx_tev_reg /
 * gx_tev_kreg / gRegS10 / gSwapTbl / gIndMtx / gIndSc; gx_draw.c: gDesc, gProj, gPosMtx, gNrmMtx, gTexMtx / gPostMtx, gChan / gMat / gAmb, gBlend*, gAc*, gZ*, gCullMode, gColorUpdate,
 * gNumChans, gCurMtx) that the draw path reads LATER.  Two side effects were checked and are idempotent: gPxActive = 1 (set by many setters, never cleared anywhere in gx_draw.c), and
 * ++gVtxGen on GXSetVtxDesc (a cache generation: bumping it again for an identical descriptor only costs a re-key).  Statistics counters (RT_COUNT, gx_blend_sets, gJournalLines, the
 * [blend] log's "distinct request" dedupe) see fewer calls: stats only, never picture.
 * Nothing but these setters writes that state (grep of gx_draw.c / gx_tev.c / gx_light.c: the only other writer is dlcSelfTest, which saves and restores gDesc), EXCEPT the aliases below.
 *
 * ALIASES (a write through one op changes a cell another op also writes; found by reading the bodies, each has a mutant in the host test):
 *   - GXSetTevColor / GXSetTevColorS10 write the same registers (gx_tev_reg[id], gRegS10[id]).  Set(0,A); S10(0,B); Set(0,A) must NOT drop the third.
 *   - GXSetTevDirect / GXSetTevIndirect write the same gTev.st[stage].ind.
 *   - GXSetChanCtrl / MatColor / AmbColor with chan 4 (COLOR0A0) write BOTH the colour and the alpha component (chanIdx(): 0 = C0, 1 = C1, 2 = A0, 3 = A1, 4 = C0+A0, 5 = C1+A1), so keys
 *     0, 2 and 4 overlap, and 1, 3 and 5.
 *   - GXSetVtxDesc folds GX_VA_NBT into GX_VA_NRM (GX_NBT_ALIAS: one descriptor field, docs/47 2026-10-14), so the two attributes are ONE target.
 *   - GXLoadPosMtxImm / NrmMtxImm key their slot by id / 3, GXLoadTexMtxImm by (id - 30) / 3 (ids 30..59) and (id - 64) / 3 (ids 64..123): ids 3k, 3k+1, 3k+2 are the SAME slot.
 * Equality is still tested on the WHOLE payload including word 0, so a different spelling of the same target is merely kept (never wrongly dropped); the aliases above only matter
 * because a write under one spelling must invalidate what was remembered under another.
 *
 * TARGETS (A2).  Per-target ops key on payload word 0 (after the normalisations above); global ops have one slot.  A key outside the op's range (nk in its row: the HAL's own id
 * range, e.g. 16 TEV stages) or a payload longer than the op's slot (maxw) is never filtered: it is recorded and the op's remembered values are dropped (a too-wide payload on a
 * valid key drops that key's value and its aliases).  A row whose maxw is too small therefore costs yield ("too wide" in the report), never correctness.
 *
 * FORGET RULES (A3), all stated where they are implemented:
 *   (a) unknown / unmodelled ops forget all (the whitelist), (b) GXClearVtxDesc forgets the GXSetVtxDesc row, (c) GXBeginDisplayList / GXEndDisplayList forget all and nothing is
 *   filtered or remembered while g_gxt.list_open, (d) every DIRECT <-> threaded transition and every arm/disarm forgets all (gxt_flt_mode_changed, gxt_flt_arm), (e) GXCallDisplayList is
 *   NEUTRAL, with the evidence at its table row.
 * Forgetting is a GENERATION bump (O(1) per op): a slot is valid iff its stamp equals its op's current generation.  Forget-all gives every op a new generation (36 stores);
 * GXClearVtxDesc and an out-of-range key give ONE op a new one; a single slot is invalidated by zeroing its stamp (generations start at 1; near 2^32 the table is cleared for real).
 *
 * NO ROLLBACK.  A dropped call never reaches gxt_rec_alloc: no header, no payload, no chunk close, no gxt_red_last update -- the stream is exactly the one the game would have produced
 * without the call.  (An open VBYTES chunk therefore stays open across a dropped setter, as it would if the setter had not been called.)  The redundancy counter measures the RESIDUE.
 *
 * MEMORY.  Tables are calloc'd at the first arming (never static): (2 + maxw) words per (op, key).  The only static state is 1 KB of class bytes (gxt_flt_cls, in gx_thread.c), a 1 KB
 * op index and the per-op descriptors here.
 */
#define GXT_FLT_DEFINE 1      /* strong definitions: see GXT_FLT_WEAK in gx_rt.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gx_rt.h"
#include "gx_rt_filter.h"

#ifdef GXT_HOST
int gxt_host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf gxt_host_printf
#endif

#define FLT_MAXW   16u        /* longest payload compared (A2) */
#define FLT_NKEYS  128u       /* targets 0..127 (A2) */

/* GXAttr values (include/dolphin/gx/GXEnum.h, enum _GXAttr): the host test cross-checks them against the real header. */
#define FLT_GX_VA_NRM 10u
#define FLT_GX_VA_NBT 25u

/* How a record's payload names its target. */
enum { KM_GLOBAL = GXT_KM_GLOBAL,   /* one slot, no target */
       KM_W0 = GXT_KM_W0,           /* word 0 is the target (stage, register, channel, texcoord, table, stage) */
       KM_SLOT3 = GXT_KM_SLOT3,     /* word 0 / 3: position / normal matrix slot */
       KM_TEXMTX = GXT_KM_TEXMTX,   /* word 0 is a texture-matrix id: slot by the HAL's two ranges */
       KM_VTXDESC = GXT_KM_VTXDESC };   /* word 0 is a GXAttr; NBT folds into NRM (gxt_flt_key in gx_rt.h: the ONE key function) */
_Static_assert(FLT_GX_VA_NRM == 10u && FLT_GX_VA_NBT == 25u, "gxt_flt_key (gx_rt.h) folds 25 into 10");

typedef struct {
    uint16_t    op;
    uint8_t     km;
    uint8_t     chan;       /* 1: channel op (keys 0..5 overlap as chanIdx()) */
    uint16_t    partner;    /* op that writes the same cell under the same key (0 = none) */
    uint8_t     nk;         /* keys 0..nk-1 after the KM_ normalisation; 1 for KM_GLOBAL */
    uint8_t     maxw;       /* payload words a slot holds (the generated wrapper's nw); a longer payload is "too wide": recorded */
    const char *why;
} FltDef;

#define FD(name, km, chan, partner, nk, maxw, why) { GXT_OP_##name, (km), (chan), (partner), (nk), (maxw), (why) }

/* ============================================================================================================================ THE TABLE: FILTER ops
 * nk = the HAL's id range for the target word (GX enums: 16 TEV stages, 4 TEV / konst registers and swap tables, 4 indirect stages, 8 texcoords, 6 channel ids, 26 attributes;
 * GX_MTX_SLOTS = 64 matrix slots; texture matrices as 10 + 20 slots).  maxw = the generated wrapper's payload words (gx_rt_gen.inc, gxt_G_<op>). */
static const FltDef k_filter[] = {
    /* ---- TEV stage state (gx_tev.c), target = stage ---- */
    FD(GXSetTevColorIn,       KM_W0,     0, 0, 16,  5, "gTev.st[stage].cin: four u8 stores"),
    FD(GXSetTevAlphaIn,       KM_W0,     0, 0, 16,  5, "gTev.st[stage].ain: four u8 stores"),
    FD(GXSetTevColorOp,       KM_W0,     0, 0, 16,  6, "gTev.st[stage].cop: five u8 stores (GXSetTevOp FORGETS: it rewrites In and Op of its stage)"),
    FD(GXSetTevAlphaOp,       KM_W0,     0, 0, 16,  6, "gTev.st[stage].aop: five u8 stores"),
    FD(GXSetTevOrder,         KM_W0,     0, 0, 16,  4, "gTev.st[stage].coord/map/color/dis: u8 stores"),
    FD(GXSetTevSwapMode,      KM_W0,     0, 0, 16,  3, "gTev.st[stage].rasSwap/texSwap"),
    FD(GXSetTevKColorSel,     KM_W0,     0, 0, 16,  2, "gTev.st[stage].kcsel"),
    FD(GXSetTevKAlphaSel,     KM_W0,     0, 0, 16,  2, "gTev.st[stage].kasel"),
    FD(GXSetTevDirect,        KM_W0,     0, GXT_OP_GXSetTevIndirect, 16,  1, "zeroes gTev.st[stage].ind; ALIAS of GXSetTevIndirect (same cell)"),
    FD(GXSetTevIndirect,      KM_W0,     0, GXT_OP_GXSetTevDirect,   16, 10, "writes gTev.st[stage].ind; ALIAS of GXSetTevDirect (same cell)"),
    /* ---- TEV registers (gx_tev.c), target = register id ---- */
    FD(GXSetTevColor,         KM_W0,     0, GXT_OP_GXSetTevColorS10,  4,  2, "gx_tev_reg[id] + gRegS10[id]; ALIAS of GXSetTevColorS10 (same registers)"),
    FD(GXSetTevColorS10,      KM_W0,     0, GXT_OP_GXSetTevColor,     4,  3, "gRegS10[id] + clamped gx_tev_reg[id]; ALIAS of GXSetTevColor (same registers)"),
    FD(GXSetTevKColor,        KM_W0,     0, 0,  4,  2, "gx_tev_kreg[id]"),
    FD(GXSetTevSwapModeTable, KM_W0,     0, 0,  4,  5, "gSwapTbl[table][4]"),
    /* ---- indirect + texgen (gx_tev.c) ---- */
    FD(GXSetIndTexOrder,      KM_W0,     0, 0,  4,  3, "gTev.indOrder[stage]"),
    FD(GXSetIndTexCoordScale, KM_W0,     0, 0,  4,  3, "gIndSc[stage]"),
    FD(GXSetIndTexMtx,        KM_W0,     0, 0, 16,  8, "gIndMtx[id-1] + gIndE[id-1] (ids 1..3; the HAL ignores the rest)"),
    FD(GXSetTexCoordGen2,     KM_W0,     0, 0,  8,  6, "gTev.gen[dst]: five u8 stores"),
    /* ---- rasterizer channels (gx_draw.c), target = channel; keys 0/2/4 and 1/3/5 overlap ---- */
    FD(GXSetChanCtrl,         KM_W0,     1, 0,  6,  7, "gChan[] via chanIdx(): chan 4/5 write the colour AND the alpha component"),
    FD(GXSetChanMatColor,     KM_W0,     1, 0,  6,  2, "gMat[][] via chanIdx(): chan 4/5 write rgb AND a"),
    FD(GXSetChanAmbColor,     KM_W0,     1, 0,  6,  2, "gAmb[][] via chanIdx(): chan 4/5 write rgb AND a"),
    /* ---- vertex descriptor + matrices (gx_draw.c) ---- */
    FD(GXSetVtxDesc,          KM_VTXDESC,0, 0, 26,  2, "gDesc[attr], with GX_VA_NBT folded into GX_VA_NRM (GX_NBT_ALIAS); ++gVtxGen is a cache generation, idempotent"),
    FD(GXLoadPosMtxImm,       KM_SLOT3,  0, 0, 64, 13, "gPosMtx[id/3] (+ the gRealSkin sanity clamp, a pure function of the matrix; gRealSkin changes only in a viewer, outside threaded mode)"),
    FD(GXLoadNrmMtxImm,       KM_SLOT3,  0, 0, 64, 13, "gNrmMtx[id/3]"),
    FD(GXLoadTexMtxImm,       KM_TEXMTX, 0, 0, 30, 14, "gTexMtx[(id-30)/3] / gPostMtx[(id-64)/3]; rows past a 2x4 load are rewritten, and the type is in the payload"),
    /* ---- global pixel / transform state (gx_draw.c), one slot each ---- */
    FD(GXSetCullMode,         KM_GLOBAL, 0, 0,  1,  1, "gCullMode"),
    FD(GXSetBlendMode,        KM_GLOBAL, 0, 0,  1,  4, "gBlendType/Src/Dst + gPxActive; the [blend] log dedupes by request, so a repeat logs nothing anyway"),
    FD(GXSetAlphaCompare,     KM_GLOBAL, 0, 0,  1,  5, "gAc*"),
    FD(GXSetZMode,            KM_GLOBAL, 0, 0,  1,  3, "gZCompare/gZFunc/gZUpdate, read per draw"),
    FD(GXSetColorUpdate,      KM_GLOBAL, 0, 0,  1,  1, "gColorUpdate"),
    FD(GXSetNumTexGens,       KM_GLOBAL, 0, 0,  1,  1, "gTev.numGens"),
    FD(GXSetNumTevStages,     KM_GLOBAL, 0, 0,  1,  1, "gTev.numStages"),
    FD(GXSetNumIndStages,     KM_GLOBAL, 0, 0,  1,  1, "gTev.numInd"),
    FD(GXSetNumChans,         KM_GLOBAL, 0, 0,  1,  1, "gNumChans"),
    FD(GXSetCurrentMtx,       KM_GLOBAL, 0, 0,  1,  1, "gCurMtx"),
    FD(GXSetProjection,       KM_GLOBAL, 0, 0,  1, 17, "gProj + gProjOrtho: a memcpy and one store (+ a journal printf capped at 40 lines).  17 words: v1's 16-word window never compared it (129 calls/frame at the hub)"),
};

/* ============================================================================================================================ THE TABLE: everything else that is named */
enum { CLS_NEUTRAL = 2, CLS_FORGET = 3, CLS_CLEARVD = 4 };   /* = GXT_FLT_C_* */
typedef struct { uint16_t op; uint8_t cls; uint8_t reason; const char *why; } OtherDef;
#define ND(name, why) { GXT_OP_##name, CLS_NEUTRAL, 0, (why) }
#define FG(name, why) { GXT_OP_##name, CLS_FORGET, GXT_FLT_F_UNKNOWN, (why) }
#define FL(name, why) { GXT_OP_##name, CLS_FORGET, GXT_FLT_F_LISTS, (why) }
static const OtherDef k_other[] = {
    /* ---- display lists (A3 c, e) ---- */
    FL(GXBeginDisplayList,      "FORGET (lists): G starts recording a list; R abandons an open primitive. Nothing is filtered or remembered while list_open"),
    FL(GXEndDisplayList,        "FORGET (lists): G-resident in gx_rt_special.c and NEVER recorded; the END is detected by gxt_flt_pre as g_gxt.list_open dropping to 0 (this class covers a build that records it)"),
    FL(ABANDON,                 "FORGET (lists): the record GXBeginDisplayList emits (R calls __real_GXBeginDisplayList(NULL, 0))"),
    ND(GXCallDisplayList,       "NEUTRAL: the HAL's list decoder (hal/gx_draw.c dlkBuild, ~line 1303) breaks at the first non-primitive opcode (`if (!(prim == GX_QUADS || ...)) break;`), so a display list "
                                "never changes GX state in this port -- it only draws; the pipe bytes of a list are vertex data"),
    /* ---- compound / indirect setters that rewrite filtered cells behind the recorder's back ---- */
    FG(GXSetTevOp,              "FORGET: compound setter, gx_tev_setop() writes TevColorIn/AlphaIn/ColorOp/AlphaOp of its stage (and gx_tev_op[])"),
    FG(GXSetTevIndRepeat,       "FORGET: calls GXSetTevIndirect(stage, ...) with canned arguments, writing gTev.st[stage].ind"),
    ND(GXSetViewportJitter,     "NEUTRAL: forwards to GXSetViewport, which calls the backend (rhi_vita_setViewport) -- not filterable (below), touches no filtered cell"),
    /* ---- state the filter does not model: written by ops outside the FILTER table, never by a FILTER op ---- */
    ND(GXSetViewport,           "NEUTRAL, not filtered: calls rhi_vita_setViewport (a backend side effect that clears / copies also touch), so an identical repeat is not provably a no-op"),
    ND(GXSetScissor,            "NEUTRAL, not filtered (X row, G shadow for GXGetScissor + backend call)"),
    ND(GXSetVtxAttrFmt,         "NEUTRAL: gVat[fmt][attr], separate from gDesc; not modelled"),
    ND(GXSetTexCopySrc,         "NEUTRAL: gCopySrc, read only by the copy ops; not modelled"),
    ND(GXSetTexCopyDst,         "NEUTRAL: X row, gCopyDst*, read only by the copy ops"),
    ND(GXSetCopyClear,          "NEUTRAL: X row, gCopyClr + the clear colour"),
    ND(GXLoadLightObjImm,       "NEUTRAL: writes gx_light_regs[] only (hal/gx_light.c); gChan holds only the light MASK"),
    ND(GXSetFog,                "NEUTRAL: X row (G also mirrors the colour into gFogColor); writes gFog* only; custom semantics, never filtered"),
    ND(GXSetArray,              "NEUTRAL, NEVER filtered: X row; the G side swaps in shadow copies whose CONTENT changes behind an identical pointer. Writes gArray[] only (separate from gDesc)"),
    /* ---- X rows that draw, copy, bind or answer: they read filtered state, none writes it ---- */
    ND(GXBegin,                 "NEUTRAL: X row; starts a primitive, reads state"),
    ND(GXCopyTex,               "NEUTRAL: X row; EFB copy (blocking B8), reads gCopySrc / gCopyDst"),
    ND(GXLoadTexObj,            "NEUTRAL: X row; binds a texture object (texture cache), writes no TEV / channel state"),
    ND(GXLoadTexObjPreLoaded,   "NEUTRAL: same record as GXLoadTexObj"),
    ND(GXInitTexObjLOD,         "NEUTRAL: X row; mutates the game's texture object + R's gTexLod note"),
    ND(GXSetDrawSync,           "NEUTRAL: X row; draw-sync token, publishes"),
    ND(GXPeekZ,                 "NEUTRAL: X row; query, blocking"),
    ND(GXReadDrawSync,          "NEUTRAL: X row; query, single-stepped"),
    ND(GXGetScissor,            "NEUTRAL: X row; served from the G shadow"),
    /* ---- GXClearVtxDesc (A3 b) ---- */
    { GXT_OP_GXClearVtxDesc, CLS_CLEARVD, GXT_FLT_F_CLEARVD, "CLEARVTXDESC: memset(gDesc, 0); forgets the GXSetVtxDesc row only" },
    /* ---- [extra] rows (op >= 200) ---- */
    ND(gx_breakpoint_deliver,   "NEUTRAL: [extra] X; G-resident break-point delivery, gated on R's flip token"),
    ND(gx_dlk_event,            "NEUTRAL: [extra] X; tells R's display-list cache that memory behind a shadow changed -- a cache event, no GX state"),
    ND(gx_tex_invalidate,       "NEUTRAL: [extra] X; texture-cache invalidation of an address"),
    ND(gx_tex_release,          "NEUTRAL: [extra] X; texture-cache release of an address"),
    ND(gx_tex_yuv_register,     "NEUTRAL: [extra] X; movie plane registration (texture cache)"),
    ND(gx_tex_yuv_register_x,   "NEUTRAL: [extra] X; as gx_tex_yuv_register, buffers owned by R"),
    ND(gx_draw_setShadowRecv,   "NEUTRAL: [extra] R; one backend flag (rhi_vita_setShadowZBias), no GX cell"),
    FG(rhi_beginFrame,          "FORGET: [extra] R; a frame boundary in the backend. Not audited for HAL resets, and it is once per frame (~1.5% yield at most)"),
    FG(rhi_clear,               "FORGET: [extra] R; backend clear. Same reason"),
    FG(rhi_endFrame,            "FORGET: [extra] R; backend frame end. Same reason"),
    /* ---- [extra] rows that are never recorded (T = viewer-only trap, E = dropped on R): listed so the table names EVERY extra; if one ever is recorded it forgets ---- */
    FG(gx_draw_setAlphaMode,    "FORGET: [extra] T; viewer / bridge only, trapped and run on G -- never recorded"),
    FG(gx_draw_setForceLayout,  "FORGET: [extra] T; viewer / bridge only -- never recorded"),
    FG(gx_draw_setJointMatrices,"FORGET: [extra] T; viewer / bridge only -- never recorded"),
    FG(gx_draw_setJointOffsets, "FORGET: [extra] T; viewer / bridge only -- never recorded"),
    FG(gx_draw_setRealSkin,     "FORGET: [extra] T; viewer only, and gRealSkin steers GXLoadPosMtxImm's clamp -- never recorded"),
    FG(gx_draw_setSourceBounds, "FORGET: [extra] T; viewer / bridge only -- never recorded"),
    FG(gx_draw_setTexture,      "FORGET: [extra] T; viewer / bridge only -- never recorded"),
    FG(os_ext_poll,             "FORGET: [extra] E; dropped on R (a baton operation) -- never recorded"),
    FG(vi_set_clear_color,      "FORGET: [extra] E; dropped on R, G calls the real one -- never recorded"),
    /* ---- control ops of the ring itself ---- */
    ND(VBYTES,                  "NEUTRAL: vertex bytes of the open chunk"),
    ND(PAD,                     "NEUTRAL: ring filler"),
    ND(SYNC,                    "NEUTRAL: R publishes tail and wakes G"),
    FG(PRESENT,                 "FORGET: the frame boundary (the HAL present path was not audited for resets; ~1.5% yield at most)"),
    FG(QUIT,                    "FORGET: R exits"),
    FG(MODE_LEAVE,              "FORGET: threaded -> direct"),
    FG(HAMMER,                  "FORGET: device ring-hammer payload, never in a shipped run"),
};

/* ============================================================================================================================ state */
#define N_FILTER ((unsigned) (sizeof k_filter / sizeof k_filter[0]))
#define N_OTHER  ((unsigned) (sizeof k_other / sizeof k_other[0]))

/* The hot per-op state is gxt_flt_ops[op] (gx_rt.h, one 32-byte line per op, read by the inline fast path).  What only the slow path needs stays here. */
#define FLT_GEN_LIMIT 0xFFFFFF00u    /* past this, clear the table for real and restart the generations: a stale stamp can never match */

static struct {
    uint32_t   *tab;                 /* calloc'd block: the ops' base pointers point into it */
    size_t      bytes;
    GxtFltOp   *o[N_FILTER];         /* = &gxt_flt_ops[k_filter[i].op] */
    uint8_t     chan[N_FILTER];      /* 1: channel op (keys 0..5 overlap as chanIdx()) */
    uint8_t     pidx[N_FILTER];      /* partner's index + 1 (0 = none) */
    uint8_t     idx[GXT_FLT_OPS];    /* op -> filter def index + 1 */
    uint32_t    clock;               /* the last generation handed out */
    int         inited, dups, in_list;
    int         diag;                /* 0 normal; 1 = compare but never drop (arm 130); 2 = return at entry (arm 131) */
    GxtFltStats st;                  /* the SLOW path's counters; the fast path counts in gxt_flt_ops[op].seen / .dropped */
} f;

static uint16_t flt_op_mask(uint32_t op) { return (uint16_t) (op & (GXT_FLT_OPS - 1u)); }

/* The inline path may decide only when nothing needs the module's attention. */
static void set_fast(void) { gxt_flt_fast = (uint8_t) (f.diag == 0 && !f.in_list); }

static void restart_generations(void)
{
    unsigned i;
    memset(f.tab, 0, f.bytes);
    f.clock = 0u;
    for (i = 0; i < N_FILTER; ++i) f.o[i]->gen = ++f.clock;
}

/* Every remembered value of ONE op is stale (GXClearVtxDesc, an out-of-range key). */
static void drop_op(unsigned i)
{
    if (f.clock >= FLT_GEN_LIMIT) { restart_generations(); return; }
    f.o[i]->gen = ++f.clock;
}

static void forget_all(int why)
{
    unsigned i;
    if (!f.tab) return;
    if (f.clock >= FLT_GEN_LIMIT - N_FILTER) restart_generations();
    else for (i = 0; i < N_FILTER; ++i) f.o[i]->gen = ++f.clock;
    ++f.st.forgets[why];
}

static int flt_init(void)
{
    size_t words = 0;
    unsigned i;
    if (f.inited) return f.tab != NULL;
    f.inited = 1;
    memset(gxt_flt_cls, 1, GXT_FLT_OPS);                                        /* the WHITELIST: every op forgets until a row below says otherwise */
    memset(f.idx, 0, sizeof f.idx);
    for (i = 0; i < N_FILTER; ++i) {
        const uint16_t o = flt_op_mask(k_filter[i].op);
        if (f.idx[o]) ++f.dups;
        f.idx[o] = (uint8_t) (i + 1u);
        gxt_flt_cls[o] = GXT_FLT_CLS_F;                                         /* nothing to do at allocation; the generated wrapper checks first */
        words += (size_t) (k_filter[i].km == KM_GLOBAL ? 1u : k_filter[i].nk) * (2u + k_filter[i].maxw);
    }
    for (i = 0; i < N_OTHER; ++i) {
        const uint16_t o = flt_op_mask(k_other[i].op);
        unsigned j;
        if (f.idx[o]) ++f.dups;
        for (j = 0; j < i; ++j) if (flt_op_mask(k_other[j].op) == o) ++f.dups;
        gxt_flt_cls[o] = (uint8_t) (k_other[i].cls == CLS_NEUTRAL ? 0 : k_other[i].cls == CLS_FORGET ? 1 : 2);
    }
    f.bytes = words * sizeof(uint32_t);
    f.tab = (uint32_t *) calloc(words, sizeof(uint32_t));
    if (!f.tab) { printf("[gxt] filt: table allocation failed (%u bytes), filter OFF\n", (unsigned) f.bytes); f.bytes = 0; return 0; }
    {   uint32_t *p = f.tab;
        for (i = 0; i < N_FILTER; ++i) {
            GxtFltOp *o = &gxt_flt_ops[flt_op_mask(k_filter[i].op)];
            unsigned j;
            f.o[i] = o;
            f.chan[i] = k_filter[i].chan;
            f.pidx[i] = 0;
            for (j = 0; k_filter[i].partner && j < N_FILTER; ++j) if (k_filter[j].op == k_filter[i].partner) f.pidx[i] = (uint8_t) (j + 1u);
            o->nk = (uint8_t) (k_filter[i].km == KM_GLOBAL ? 1u : k_filter[i].nk);
            o->maxw = k_filter[i].maxw;
            o->stride = (uint16_t) (2u + o->maxw);
            o->km = k_filter[i].km;
            o->slow = (uint8_t) (f.chan[i] || f.pidx[i]);
            o->seen = o->dropped = 0u;
            o->base = p;                                                        /* last: a non-NULL base is what the generated wrapper tests */
            p += (size_t) o->nk * o->stride;
        }
    }
    restart_generations();
    printf("[gxt] filt: armed lazily (v3, inline compare before write): %u filterable ops, %u named others, %u bytes of tables, %d duplicate rows\n",
           N_FILTER, N_OTHER, (unsigned) f.bytes, f.dups);
    return 1;
}

/* ============================================================================================================================ arming, forgetting */
void gxt_flt_arm(int on)
{
    on = on != 0;
    if (on == (gxt_flt_on != 0)) return;
    if (on && !flt_init()) return;                         /* allocation failed: stay OFF */
    f.in_list = 0;
    forget_all(GXT_FLT_F_MODE);                            /* every change of the armed state: what was remembered may be stale (the HAL changes while nothing is recorded) */
    set_fast();
    gxt_flt_on = on;
}

void gxt_flt_mode_changed(void)
{
    forget_all(GXT_FLT_F_MODE);                            /* A3(d) */
}

/* Called from the inline gxt_rec_alloc for ops whose class (low 7 bits) is non-zero.  Runs at ALLOCATION: the record being allocated is itself not a FILTER op. */
void gxt_flt_alloc_note(uint32_t op)
{
    const uint8_t c = (uint8_t) (gxt_flt_cls[op & (GXT_FLT_OPS - 1u)] & 0x7Fu);
    if (c == 2u) {                                         /* GXClearVtxDesc: the GXSetVtxDesc row only (A3 b) */
        const unsigned i = f.idx[GXT_OP_GXSetVtxDesc & (GXT_FLT_OPS - 1u)];
        if (f.tab && i) { drop_op(i - 1u); ++f.st.forgets[GXT_FLT_F_CLEARVD]; }
    } else if (c == 1u) {                                  /* unknown / unmodelled / list ops: forget everything (A3 a, c) */
        const uint32_t o = op & (GXT_FLT_OPS - 1u);
        forget_all(o == GXT_OP_GXBeginDisplayList || o == GXT_OP_GXEndDisplayList || o == GXT_OP_ABANDON ? GXT_FLT_F_LISTS : GXT_FLT_F_UNKNOWN);
    }
}

/* ============================================================================================================================ the slow path */
static const uint8_t k_chmask[6] = { 1u, 2u, 4u, 8u, 5u, 10u };   /* chanIdx(): C0, C1, A0, A1, C0+A0, C1+A1 as bit pairs {C0=1, C1=2, A0=4, A1=8} */

/* A write to (def i, key) changed the cell: invalidate every remembered value that names it under another spelling. */
static void invalidate_aliases(unsigned i, int key)
{
    if (f.pidx[i]) {
        const GxtFltOp *p = f.o[f.pidx[i] - 1u];
        if ((unsigned) key < p->nk) p->base[(unsigned) key * p->stride] = 0u;
    }
    if (f.chan[i] && key >= 0 && key < 6) {
        const GxtFltOp *o = f.o[i];
        for (int j = 0; j < 6; ++j) if (j != key && (k_chmask[j] & k_chmask[key])) o->base[(unsigned) j * o->stride] = 0u;
    }
}

/* G, through gxt_flt_check (gx_rt.h) whenever the inline path cannot decide alone: lists, an alias op's miss, an out-of-range key, a too-wide payload,
 * the diagnostic arms.  Same contract: 1 = the HAL already holds this state: do not record. */
int gxt_flt_pre(uint32_t op, const uint32_t *w, uint32_t nw)
{
    const unsigned x = f.idx[op & (GXT_FLT_OPS - 1u)];
    GxtFltOp *o;
    uint32_t *s;
    unsigned i;
    int key;
    if (GXT_UNLIKELY(f.diag == 2)) return 0;              /* arm 131: the wrapper's own cost only */
    if (!x || !f.tab) return 0;
    ++f.st.seen;
    if (g_gxt.list_open) {                                 /* A3(c): nothing is filtered or remembered while a list is open on G (the Begin record forgot everything when it was allocated) */
        f.in_list = 1; set_fast();
        ++f.st.kept_list; return 0;
    }
    if (f.in_list) { f.in_list = 0; forget_all(GXT_FLT_F_LISTS); set_fast(); }   /* A3(c), the END: gxts_G_GXEndDisplayList is G-resident and records NOTHING (gx_rt_special.c), so the END is seen as the list closing */
    i = x - 1u;
    o = f.o[i];
    key = gxt_flt_key(o, w, nw);
    if (key < 0) {                                         /* A2: target outside the op's range (or missing): recorded; this op's remembered values (and its partner's) are unknown now */
        drop_op(i);
        if (f.pidx[i]) drop_op(f.pidx[i] - 1u);
        ++f.st.kept_unknown_key;
        return 0;
    }
    s = o->base + (unsigned) key * o->stride;
    if (nw > o->maxw) {                                    /* A2: too wide for the slot: recorded, this target is unknown now */
        s[0] = 0u;
        invalidate_aliases(i, key);
        ++f.st.kept_wide;
        return 0;
    }
    if (s[0] == o->gen && s[1] == nw) {                    /* EXACT, word for word (not a hash) */
        uint32_t k = 0;
        while (k < nw && s[2u + k] == w[k]) ++k;
        if (k == nw) { ++f.st.dropped; return f.diag != 1; }   /* arm 130: everything but the drop */
    }
    invalidate_aliases(i, key);                            /* a different value is going in: whatever was remembered under an overlapping spelling is stale */
    s[0] = o->gen; s[1] = nw;
    for (uint32_t k = 0; k < nw; ++k) s[2u + k] = w[k];
    return 0;
}

/* ============================================================================================================================ report */
void gxt_flt_get_stats(GxtFltStats *out)
{
    unsigned i;
    *out = f.st;
    for (i = 0; f.tab && i < N_FILTER; ++i) { out->seen += f.o[i]->seen; out->dropped += f.o[i]->dropped; }
}
void gxt_flt_reset_stats(void)
{
    unsigned i;
    memset(&f.st, 0, sizeof f.st);
    for (i = 0; f.tab && i < N_FILTER; ++i) f.o[i]->seen = f.o[i]->dropped = 0u;
}

int gxt_flt_format(char *buf, size_t cap, uint32_t frames)
{
    GxtFltStats s;
    const double F = frames ? (double) frames : 1.0;
    double fg;
    gxt_flt_get_stats(&s);
    fg = (double) (s.forgets[0] + s.forgets[1] + s.forgets[2] + s.forgets[3]);
    return snprintf(buf, cap, "[gxt] filt: %u presents | filtered %.1f/frame of %.1f filterable/frame (%.1f%%) | forgets %.1f/frame (unknown ops %.1f, lists %.1f, clearvtxdesc %.1f, mode %.1f)",
                    frames, s.dropped / F, s.seen / F, s.seen ? 100.0 * s.dropped / s.seen : 0.0, fg / F,
                    s.forgets[GXT_FLT_F_UNKNOWN] / F, s.forgets[GXT_FLT_F_LISTS] / F, s.forgets[GXT_FLT_F_CLEARVD] / F, s.forgets[GXT_FLT_F_MODE] / F);
}

void gxt_flt_print(uint32_t frames)
{
    char b[400];
    if (!f.tab) return;
    (void) gxt_flt_format(b, sizeof b, frames);
    printf("%s\n", b);
    printf("[gxt] filt: kept anyway: %u while a list was open, %u too wide, %u with an unknown target | slow path %.1f calls/frame | armed %d fast %d (v3: inline compare before write)\n",
           f.st.kept_list, f.st.kept_wide, f.st.kept_unknown_key, frames ? (double) f.st.seen / frames : 0.0, gxt_flt_on, gxt_flt_fast);
    gxt_flt_reset_stats();
}

int gxt_flt_class(uint32_t op)
{
    unsigned i;
    for (i = 0; i < N_FILTER; ++i) if (k_filter[i].op == op) return GXT_FLT_C_FILTER;
    for (i = 0; i < N_OTHER; ++i) if (k_other[i].op == op) return k_other[i].cls;
    return GXT_FLT_C_UNLISTED;
}

const char *gxt_flt_why(uint32_t op)
{
    unsigned i;
    for (i = 0; i < N_FILTER; ++i) if (k_filter[i].op == op) return k_filter[i].why;
    for (i = 0; i < N_OTHER; ++i) if (k_other[i].op == op) return k_other[i].why;
    return "";
}

size_t gxt_flt_table_bytes(void) { return f.bytes; }

/* Diagnostic arms 130 / 131 (gx_thread.c filt_diag_wanted): every call goes through gxt_flt_pre (fast off).  Every change forgets: arm 131 records without updating the shadow. */
void gxt_flt_diag(int mode)
{
    if (mode == f.diag) return;
    f.diag = mode;
    forget_all(GXT_FLT_F_MODE);
    set_fast();
}

#ifdef GXT_HOST
void gxt_flt_host_set_clock(uint32_t c) { f.clock = c; }     /* the host test watches the generation restart without 2^32 forgets */
#endif

int gxt_flt_table_dups(void) { return f.inited ? f.dups : -1; }
