/* gx_hal_state.h -- THE HAL STATE STRUCT (locked-30 lever R2; contract: projects/stairfax-vita/host/engine/R2_STATE.md).
 *
 * Every static a GX *state setter* writes and the draw path reads lives in ONE struct type, GxHal, with TWO instances:
 *     gx_hal    the one the draw path reads: R's while the render thread runs, the only one in DIRECT mode
 *     gx_hal_g  G's, written only by the G variants (gxh_g_<GXName>) while R2 is active
 * Both are DEFINED in hal/gx_tev.c (the TEV tests link that object alone).
 *
 * Each setter body is written ONCE, as an always_inline function  gxh_b_<Name>(GxHal *h, const int mark, <the setter's parameters>):
 *     void GXSetX(args)         { gxh_b_GXSetX(&gx_hal,   0, args); }   // DIRECT / R: the same code as before (constant pointer, constant 0)
 *     void gxh_g_GXSetX(args)   { gxh_b_GXSetX(&gx_hal_g, 1, args); }   // G: the same body, plus the dirty bit of every group it wrote
 * A body uses ONLY its `h` parameter for struct state (gxh_lint.py enforces it), so the code that stores a cell is the code that marks its group.
 *
 * GROUPS.  A group is a contiguous byte range of GxHal (gxh_groups[]).  GXH_MARK sets the group's bit in h->dirty[] and h->dirty_any (both at the START of
 * the struct: one cache line, in no group).  Before G appends ANYTHING to the ring the dirty groups travel as one STATE record (gx_rt_state.c) and R copies
 * them into gx_hal.  The dirty words are meaningless in gx_hal.
 *
 * NOT IN THE STRUCT (R2_STATE.md D4): gVtxGen (a cache generation that GXSetArray / setSourceBounds / init also bump on R), the side statistics
 * (gx_blend_sets, gBlendSeen, gx_light_loads, gJournalLines, RT_COUNT) -- while R2 is active only G runs setters, so they keep one writer.
 */
#ifndef PORT_GX_HAL_STATE_H
#define PORT_GX_HAL_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "dolphin/gx/GXEnum.h"
#include "dolphin/gx/GXStruct.h"   /* NOT dolphin/gx.h: gx_draw.c declares several GX entry points with its own (int) signatures and must not see the SDK's */

#ifdef __cplusplus
extern "C" {
#endif

#define GXH_NKINDS_ 20                 /* = GXH_NKINDS (the group KINDS, below); the header needs it before the enum */
#define GXH_TEV_STAGES 16
#define GXH_TEX_GENS   8
#define GXH_IND_STAGES 4
#define GXH_MTX_SLOTS  64
#define GXH_NTEXMTX    10
#define GXH_NPOSTMTX   20
#define GXH_NLIGHTS    8

/* ---- types that moved here from gx_tev.c / gx_draw.c / gx_light.c, layout unchanged ---- */
typedef struct {
    u8 coord, map, color;              // GXSetTevOrder
    u8 cin[4], ain[4];                 // GXSetTevColorIn / AlphaIn
    u8 cop[5], aop[5];                 // op, bias, scale, clamp, out
    u8 rasSwap, texSwap, kcsel, kasel; // GXSetTevSwapMode, KColorSel, KAlphaSel
    u8 ind[10];                        // GXSetTevIndirect (all zero = direct)
    u8 dis;                            // GXSetTevOrder was given GX_TEXMAP_DISABLE (0x100): the stage does not sample, its map only supplies the coordinate scale
} TevStage;
typedef struct { u8 func, src, mtx, norm, post; } TexGen;
typedef struct {
    u8 numStages, numGens, numInd;
    TevStage st[GXH_TEV_STAGES];
    TexGen   gen[GXH_TEX_GENS];
    u8       indOrder[GXH_IND_STAGES][2];  // coord, map
} TevState;
typedef struct { u8 cnt; u8 type; u8 frac; } VatAttr;
typedef struct { u8 enable, ambSrc, matSrc, lightMask, diffFn, attnFn; } PxChan;
typedef struct { u32 reserved[3]; u32 color; f32 a[3]; f32 k[3]; f32 lpos[3]; f32 ldir[3]; } GxHalLight;   /* color = r<<24|g<<16|b<<8|a (GXInitLightColor) */
/* pixel / transform scalars (one group, "PIX"): everything gx_draw.c's pixel-state setters write */
typedef struct {
    int pxActive;                      /* set by many setters, never cleared (the upstream viewers never call them) */
    int curMtx;
    u8  numChans, colorUpdate, blendType, blendSrc, blendDst;
    u8  acComp0, acComp1, acOp, acRef0, acRef1, cullMode;
    u8  zCompare, zFunc, zUpdate;
} PxScalars;

/* ---- the struct.  ORDER IS A COST DECISION (A6, "DIRECT pays nothing"): the game's Thumb-2 code reaches a field with strb/ldr [reg, #imm12], so a field past offset 4095 pays an extra movw per access.  The
 * state the per-call setters touch with byte stores (TEV stages, channels, registers, the pixel scalars) therefore comes FIRST, the big float arrays (matrices) LAST.  Group ids do not follow the
 * order (gxh_groups[] is built with offsetof).  The one pad byte is before regS10 (s16 alignment) and belongs to no group. ---- */
typedef struct GxHal {
    uint32_t dirty_any;                /* offset 0 (GXT_HAL_DIRTY_ANY_OFF in gx_rt.h) */
    uint32_t dirty[8];                 /* bit g = group g was written since the last flush (G instance only) */
    uint32_t hdr_pad_[7];              /* the header is exactly one 64-byte line */
    /* ---- groups ---- */
    TevState     tev;                  /* TEVCNT (the three counts), STAGE(i), GEN(i); indOrder + indSc are the IND group */
    u8           indSc[GXH_IND_STAGES][2];
    u8           kd_stage, kd_gen, kd_tevcnt, kd_ind;   /* R3a C3: the kind flags (see KIND FLAGS below).  Each kd_* sits beside the data of its kind -- here after the TEV block, elsewhere in the last byte of
                                                         * the 4-byte slot before the data (kdp_*) -- so a setter's flag store shares the base register of its data stores */
    u8           kdp_chan[3] __attribute__((aligned(4))); u8 kd_chan;
    PxChan       chan[4];              /* CHAN: chan + mat + amb */
    u8           mat[2][4];
    u8           amb[2][4];
    u8           kdp_tevop[3] __attribute__((aligned(4))); u8 kd_tevop;
    u8           tevOp[16];            /* TEVOP: GXSetTevOp's recorded mode per stage */
    u8           kdp_swap[3] __attribute__((aligned(4))); u8 kd_swap;
    u8           swapTbl[4][4];        /* SWAP */
    u8           kdp_kreg[3] __attribute__((aligned(4))); u8 kd_kreg;
    GXColor      kreg[4];              /* KREG */
    u8           kdp_tevreg[3] __attribute__((aligned(4))); u8 kd_tevreg;
    s16          regS10[4][4];         /* TEVREG: regS10 + tevReg (the same registers at full S10 range) */
    GXColor      tevReg[4];
    u8           kdp_copy[3] __attribute__((aligned(4))); u8 kd_copy;
    u16          copySrc[4];           /* COPY */
    u8           kdp_pix[3] __attribute__((aligned(4))); u8 kd_pix;
    PxScalars    px;                   /* PIX */
    u8           kdp_desc[3] __attribute__((aligned(4))); u8 kd_desc;
    GXAttrType   desc[GX_VA_MAX_ATTR]; /* DESC */
    u8           kdp_proj[3] __attribute__((aligned(4))); u8 kd_proj;
    float        proj[4][4];           /* PROJ: proj + projOrtho */
    int          projOrtho;            /* the current projection's type: perspective fog needs GX_PERSPECTIVE */
    u8           kdp_indm[3] __attribute__((aligned(4))); u8 kd_indm;
    float        indMtx[3][2][3];      /* INDM: indMtx + indE */
    float        indE[3];              /* 2^scale_exp of ITM_0..2 */
    u8           kdp_vat[3] __attribute__((aligned(4))); u8 kd_vat;
    VatAttr      vat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];   /* VAT(fmt) */
    u8           kdp_light[3] __attribute__((aligned(4))); u8 kd_light;
    GxHalLight   lightRegs[GXH_NLIGHTS];          /* LIGHT(i) */
    u8           kdp_texm[3] __attribute__((aligned(4))); u8 kd_texm;   /* ONE flag for both matrix kinds (TEXM and POSTM): GXLoadTexMtxImm picks the group at run time, and a flag per kind would cost it a branch */
    float        texMtx[GXH_NTEXMTX][3][4];       /* TEXM(i) */
    float        postMtx[GXH_NPOSTMTX][3][4];     /* POSTM(i) */
    u8           kdp_pos[2] __attribute__((aligned(4))); u8 kd_nrm, kd_pos;   /* both before posMtx: nrmMtx lies past offset 4095, so its own flag could not be reached from the struct base */
    float        posMtx[GXH_MTX_SLOTS][3][4];     /* POS(slot) */
    float        nrmMtx[GXH_MTX_SLOTS][3][4];     /* NRM(slot) */
} GxHal;

#define GXT_HAL_DIRTY_ANY_OFF 0u
_Static_assert(offsetof(GxHal, dirty_any) == GXT_HAL_DIRTY_ANY_OFF, "gx_rt.h reaches the dirty-any flag at this offset");
_Static_assert(offsetof(GxHal, tev) == 64, "the dirty header is one 64-byte line");
_Static_assert(GXH_NKINDS_ == 20, "one kind flag per group kind");

extern GxHal gx_hal, gx_hal_g;         /* defined in hal/gx_tev.c */

/* ---- R3a (host/engine/R3_DRAW.md): the cut switches and the draw path's generation counters ---- */
#define GX_R3A_C1 1u                   /* P: uniforms written by offset into the default uniform buffer (rhi_gxm_draw.c) */
#define GX_R3A_C2 2u                   /* P: GXM depth / fragment-texture state shadowing (rhi_gxm_draw.c) */
#define GX_R3A_C3 4u                   /* HAL + P: generation-keyed memos of the TEV key / VS key + uniforms / program choice */
#define GX_R3A_C4 8u                   /* HAL: the small per-call waste */
#define GX_R3A_ALL 15u
#define GX_R3A_C5 16u                  /* HAL + P: program-clean display-list draw (R3c: reuse the TevKey / VsKey of the previous GPU-path draw while no program input moved) */
#define GX_R3A_C6 32u                  /* P: consecutive identical-state immediate draws merged into one sceGxmDraw (rhi_gxm_draw.c) */
#define GX_R3A_C7 64u                  /* HAL: display-list validation trim (last-hit record cache, VAT compare by generation) */
#define GX_R3C_CUTS 112u               /* C5 | C6 | C7 (R3c, host/engine/R3C_CACHE.md); GX_R3A_ALL keeps its R3a meaning */
#define GX_R3A_C8 128u                 /* HAL: R6 (host/engine/R6_SKIN.md) a dynamic display-list record learns WHICH arrays changed from the announced writes (gx_dlk_event), not from a hash */
#define GX_R3A_C9 256u                 /* HAL + P: R6 a mesh update rewrites only POS / NRM into a rotating GPU slot that already holds the rest (rhi_gxm_gx_mesh_update_begin / _commit) */
#define GX_R3D_ALL 384u                /* C8 | C9 */
#define GX_R3C_ALL (GX_R3C_CUTS | GX_R3D_ALL)   /* every bit gx_r3a_set (hal/gx_tev.c) lets through beyond R3a's: it masks with GX_R3A_ALL | GX_R3C_ALL, and that file is not ours to edit, so C8 / C9 ride in this mask; gx_draw.c's r3c tracker switch reads GX_R3C_CUTS */
extern unsigned gx_r3a;                /* gx_draw.c: the cuts that are ON (gx_thread.c sets it at the frame edge from the perf arm); read once per draw */
void gx_r3a_set(unsigned mask);
/* KIND FLAGS AND GENERATIONS.  A setter that writes a group of kind K stores ONE constant byte, the kind's flag, in the instance it wrote (GXH_MARK, mark == 0 arm: gx_hal -- DIRECT, R, and R's
 * replay).  The 20 flags are the GxHal fields kd_*, each beside the data of its kind: a setter's flag store then shares the base register of its data stores (the A6 rule, "DIRECT pays nothing", is
 * +2 static instructions per public setter: one `movs`, one `strb`).  gxh_collect() (the draw path, on whichever thread owns gx_hal, at the top of every memoised build) turns the flags into the
 * generation counters gx_hal_gen[K] and clears them.  A memo records the generations of exactly the kinds it reads and is valid while they are unchanged.  EVERY other writer of gx_hal sets the
 * flags itself: gxs_r_apply (per applied group), gx_draw_init and dlcSelfTest (gxh_kdirty_all), test harnesses that overwrite the struct. */
extern uint32_t gx_hal_gen[GXH_NKINDS_];
extern const uint16_t gxh_kflag_off[GXH_NKINDS_];   /* offsetof(GxHal, kd_*) per kind (hal/gx_tev.c) */
void gxh_collect(void);
void gxh_kdirty_all(GxHal *h);         /* every kind's flag */
void gxh_kdirty_set(GxHal *h, unsigned kind);   /* variable kind (gxs_r_apply) */

/* ---- group ids ---- */
enum {
    GXH_G_STAGE0 = 0,                  /* 16 */
    GXH_G_GEN0   = 16,                 /* 8 */
    GXH_G_TEVCNT = 24, GXH_G_IND, GXH_G_TEVREG, GXH_G_KREG, GXH_G_SWAP, GXH_G_INDM, GXH_G_DESC,   /* 24..30 */
    GXH_G_VAT0   = 31,                 /* 8 */
    GXH_G_PROJ   = 39,
    GXH_G_POS0   = 40,                 /* 64 */
    GXH_G_NRM0   = 104,                /* 64 */
    GXH_G_TEXM0  = 168,                /* 10 */
    GXH_G_POSTM0 = 178,                /* 20 */
    GXH_G_LIGHT0 = 198,                /* 8 */
    GXH_G_CHAN   = 206, GXH_G_PIX, GXH_G_COPY, GXH_G_TEVOP,
    GXH_NGROUPS  = 210
};
/* group KINDS (the mutation script and the sweep name them): the first group id of each kind and how many there are */
enum { GXH_K_STAGE, GXH_K_GEN, GXH_K_TEVCNT, GXH_K_IND, GXH_K_TEVREG, GXH_K_KREG, GXH_K_SWAP, GXH_K_INDM, GXH_K_DESC, GXH_K_VAT, GXH_K_PROJ, GXH_K_POS, GXH_K_NRM,
       GXH_K_TEXM, GXH_K_POSTM, GXH_K_LIGHT, GXH_K_CHAN, GXH_K_PIX, GXH_K_COPY, GXH_K_TEVOP, GXH_NKINDS };
_Static_assert(GXH_NKINDS == GXH_NKINDS_, "GXH_NKINDS_ (the struct header) and GXH_NKINDS (the enum) agree");
enum { GXH_HOOK_NONE = 0, GXH_HOOK_VTXGEN = 1 };      /* post-apply hooks (R): the vertex-descriptor group bumps gVtxGen */

typedef struct { uint32_t off; uint16_t bytes; uint8_t hook; uint8_t kind; } GxhGroup;
extern const GxhGroup gxh_groups[GXH_NGROUPS];        /* defined in hal/gx_tev.c, built with offsetof */
extern const char *const gxh_kind_names[GXH_NKINDS];
/* R side, hal/gx_draw.c: a group's post-apply hook (D4: the derived state R keeps outside the struct) */
void gxh_post_apply(unsigned hook);

/* ---- marking ---- */
#define GXH_BIT_SET(h, g) ((h)->dirty[(g) >> 5] |= 1u << ((g) & 31u), (h)->dirty_any = 1u)
#define GXH_NOMARK(h, mark, g) ((void) 0)                 /* host mutation scripts replace GXH_MARK with this inside one body (gxh_b_<Name>) */
/* R3a C3 (R3_DRAW.md s2): the KIND of a group id.  Constant-folded wherever the id is a constant or a bounded index added to a base (every setter checks its index before it marks); gx_rt_state_test
 * compares it with gxh_groups[g].kind for ALL ids.  The groups of one kind are contiguous. */
static inline __attribute__((always_inline)) uint8_t *gxh_kflag(GxHal *h, unsigned k)   /* constant k folds to &h->kd_*: one base + immediate */
{
    switch (k) {
    case GXH_K_STAGE: return &h->kd_stage;   case GXH_K_GEN: return &h->kd_gen;     case GXH_K_TEVCNT: return &h->kd_tevcnt; case GXH_K_IND: return &h->kd_ind;
    case GXH_K_TEVREG: return &h->kd_tevreg; case GXH_K_KREG: return &h->kd_kreg;   case GXH_K_SWAP: return &h->kd_swap;     case GXH_K_INDM: return &h->kd_indm;
    case GXH_K_DESC: return &h->kd_desc;     case GXH_K_VAT: return &h->kd_vat;     case GXH_K_PROJ: return &h->kd_proj;     case GXH_K_POS: return &h->kd_pos;
    case GXH_K_NRM: return &h->kd_nrm;       case GXH_K_TEXM: return &h->kd_texm;   case GXH_K_POSTM: return &h->kd_texm;   case GXH_K_LIGHT: return &h->kd_light;
    case GXH_K_CHAN: return &h->kd_chan;     case GXH_K_PIX: return &h->kd_pix;     case GXH_K_COPY: return &h->kd_copy;     default: return &h->kd_tevop;
    }
}
static inline __attribute__((always_inline)) unsigned gxh_kind_of_group(unsigned g)
{
    return g < 16u ? (unsigned) GXH_K_STAGE : g < 24u ? (unsigned) GXH_K_GEN : g < 31u ? g - 24u + (unsigned) GXH_K_TEVCNT : g < 39u ? (unsigned) GXH_K_VAT : g < 40u ? (unsigned) GXH_K_PROJ
         : g < 104u ? (unsigned) GXH_K_POS : g < 168u ? (unsigned) GXH_K_NRM : g < 178u ? (unsigned) GXH_K_TEXM : g < 198u ? (unsigned) GXH_K_POSTM : g < 206u ? (unsigned) GXH_K_LIGHT
         : g - 206u + (unsigned) GXH_K_CHAN;
}
/* ONLY the mark parameter decides: with mark == 0 (DIRECT / R) the dirty-bit arm is dead code and the statement is ONE constant byte store into the header (the kind's flag); with mark == 1 (G's
 * instance) it is the dirty bit, exactly as before.  The flag is what keeps the draw path's memos honest: gxh_collect() turns flags into generations (C3). */
#define GXH_MARK(h, mark, g) do { if (mark) GXH_BIT_SET(h, g); else *gxh_kflag(h, gxh_kind_of_group((unsigned) (g))) = 1u; } while (0)

#define GXH_INLINE static inline __attribute__((always_inline))
/* The public setter and its G variant around one body.  PARAMS is the parenthesised prototype list, the rest are the argument names. */
#define GXH_PAIR(NAME, PARAMS, ...) \
    void NAME PARAMS { gxh_b_##NAME(&gx_hal, 0, ##__VA_ARGS__); } \
    void gxh_g_##NAME PARAMS { gxh_b_##NAME(&gx_hal_g, 1, ##__VA_ARGS__); }

/* ---- the G variants of the S ops ----
 * Each HAL object that defines an S op defines  gxh_g_<Name>  with the op's PUBLIC parameter list (GXH_PAIR); the generated gx_rt_gen.inc declares them with the table's prototypes
 * (gx_rec.c does not include this header: the SDK's enum types and gx_draw.c's int parameters would conflict).  gxh_lint.py checks that every S row has a definition.
 * The two composed helpers other HAL objects call: */
void gxh_g_GXSetTevIndirect(GXTevStageID s, GXIndTexStageID ind, GXIndTexFormat fmt, GXIndTexBiasSel bias, GXIndTexMtxID mtx, GXIndTexWrap ws, GXIndTexWrap wt, GXBool addPrev, GXBool utcLod, GXIndTexAlphaSel alpha);
void gxh_g_tev_setop(int stage, int mode);   /* gx_tev.c's compound helper (GXSetTevOp's body): the G variant of gx_tev_setop */

/* ---- initial values: exactly today's static initialisers ---- */
#define GXH_INIT { \
    .tev = { 1, 1, 0 }, \
    .indE = { 1.0f, 1.0f, 1.0f }, \
    .swapTbl = { {0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3} }, \
    .chan = { {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1} }, \
    .mat = { {255,255,255,255}, {255,255,255,255} }, \
    .px = { .numChans = 1, .colorUpdate = 1, .blendSrc = 1, .acComp0 = 7, .acComp1 = 7, .zCompare = 1, .zFunc = 3 /*GX_LEQUAL*/, .zUpdate = 1 }, \
}

/* ---- the group table (expanded ONLY in hal/gx_tev.c) ---- */
#define GXH__SZ(f) sizeof(((GxHal *) 0)->f)
#define GXH__G(first_field, bytes, hook, kind) { (uint32_t) offsetof(GxHal, first_field), (uint16_t) (bytes), (hook), (kind) }
#define GXH__R4(m, i)  m(i) m((i) + 1) m((i) + 2) m((i) + 3)
#define GXH__R16(m, i) GXH__R4(m, i) GXH__R4(m, (i) + 4) GXH__R4(m, (i) + 8) GXH__R4(m, (i) + 12)
#define GXH__R64(m, i) GXH__R16(m, i) GXH__R16(m, (i) + 16) GXH__R16(m, (i) + 32) GXH__R16(m, (i) + 48)
#define GXH__M_STAGE(i) GXH__G(tev.st[i], sizeof(TevStage), GXH_HOOK_NONE, GXH_K_STAGE),
#define GXH__M_GEN(i)   GXH__G(tev.gen[i], sizeof(TexGen), GXH_HOOK_NONE, GXH_K_GEN),
#define GXH__M_VAT(i)   GXH__G(vat[i], GXH__SZ(vat[0]), GXH_HOOK_NONE, GXH_K_VAT),
#define GXH__M_POS(i)   GXH__G(posMtx[i], GXH__SZ(posMtx[0]), GXH_HOOK_NONE, GXH_K_POS),
#define GXH__M_NRM(i)   GXH__G(nrmMtx[i], GXH__SZ(nrmMtx[0]), GXH_HOOK_NONE, GXH_K_NRM),
#define GXH__M_TEXM(i)  GXH__G(texMtx[i], GXH__SZ(texMtx[0]), GXH_HOOK_NONE, GXH_K_TEXM),
#define GXH__M_POSTM(i) GXH__G(postMtx[i], GXH__SZ(postMtx[0]), GXH_HOOK_NONE, GXH_K_POSTM),
#define GXH__M_LIGHT(i) GXH__G(lightRegs[i], GXH__SZ(lightRegs[0]), GXH_HOOK_NONE, GXH_K_LIGHT),
#define GXH_GROUP_TABLE \
    GXH__R16(GXH__M_STAGE, 0) \
    GXH__R4(GXH__M_GEN, 0) GXH__R4(GXH__M_GEN, 4) \
    GXH__G(tev.numStages, 3, GXH_HOOK_NONE, GXH_K_TEVCNT), \
    GXH__G(tev.indOrder, GXH__SZ(tev.indOrder) + GXH__SZ(indSc), GXH_HOOK_NONE, GXH_K_IND), \
    GXH__G(regS10, GXH__SZ(regS10) + GXH__SZ(tevReg), GXH_HOOK_NONE, GXH_K_TEVREG), \
    GXH__G(kreg, GXH__SZ(kreg), GXH_HOOK_NONE, GXH_K_KREG), \
    GXH__G(swapTbl, GXH__SZ(swapTbl), GXH_HOOK_NONE, GXH_K_SWAP), \
    GXH__G(indMtx, GXH__SZ(indMtx) + GXH__SZ(indE), GXH_HOOK_NONE, GXH_K_INDM), \
    GXH__G(desc, GXH__SZ(desc), GXH_HOOK_VTXGEN, GXH_K_DESC), \
    GXH__R4(GXH__M_VAT, 0) GXH__R4(GXH__M_VAT, 4) \
    GXH__G(proj, GXH__SZ(proj) + GXH__SZ(projOrtho), GXH_HOOK_NONE, GXH_K_PROJ), \
    GXH__R64(GXH__M_POS, 0) GXH__R64(GXH__M_NRM, 0) \
    GXH__R4(GXH__M_TEXM, 0) GXH__R4(GXH__M_TEXM, 4) GXH__M_TEXM(8) GXH__M_TEXM(9) \
    GXH__R16(GXH__M_POSTM, 0) GXH__R4(GXH__M_POSTM, 16) \
    GXH__R4(GXH__M_LIGHT, 0) GXH__R4(GXH__M_LIGHT, 4) \
    GXH__G(chan, GXH__SZ(chan) + GXH__SZ(mat) + GXH__SZ(amb), GXH_HOOK_NONE, GXH_K_CHAN), \
    GXH__G(px, sizeof(PxScalars), GXH_HOOK_NONE, GXH_K_PIX), \
    GXH__G(copySrc, GXH__SZ(copySrc), GXH_HOOK_NONE, GXH_K_COPY), \
    GXH__G(tevOp, GXH__SZ(tevOp), GXH_HOOK_NONE, GXH_K_TEVOP),

#ifdef __cplusplus
}
#endif
#endif /* PORT_GX_HAL_STATE_H */
