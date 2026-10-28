/* rhi_gxm_draw.c - the DRAW module of the native GXM backend, milestone M2 (contract: rhi_gxm_int.h "draw module", design: host/engine/GXM_M2.md s3.2, s3.3, s7).
 *
 * WHAT IT OWNS: drawTev / drawTevGx / drawTextured / drawColored / drawStatic, the GX meshes and the static VBOs, every per-draw piece of scene state
 * (cull, depth, viewport, region clip = the scissor, shadow z-bias), the uniform-buffer fill, the vertex staging into the frame module's ring, and THE DRAW
 * ACCOUNTING in g_gx.st (req / issued / skip / refused_pre).
 *
 * THE ACCOUNTING (R5) IS BY CONSTRUCTION. Every entry point has exactly this shape:
 *     malformed input                -> refused_pre[kind]++, return                       (M1's own refusals; not a request)
 *     admit(kind)                    -> req[kind]++                                        (the ONLY place req moves)
 *     ... find the program, run_draw()  -> one reason code, or -1 for "issued"
 *     account(kind, code)            -> issued[kind]++ XOR skip[kind][code]++              (the ONLY place issued/skip move; one call per entry)
 * so req == issued + sum(skip) per kind holds unless the arm SKIPDRAW (which returns between admit() and account() on purpose) is set. -DGXM_SELFCHECK
 * asserts the identity at every report line.
 *
 * WHAT IS PURE (and host-tested, tools in the builder's scratch dir): the region between the PURE BEGIN / PURE END marker lines - the (g_depth, alphaMode)
 * -> (func, write, blend key) resolver, the mesh index/stride validator, the layout validator and the chunk-size rule. The test extracts that region from THIS
 * file (it does not keep a copy) and a mutant of each rule must fail it.
 *
 * Nothing here has run on the console. Emulator numbers are never timing claims. No identifier starting with gl/vgl is used (R1). */

#include "rhi_gxm_int.h"
#include "gx_rt_split_be.h"   /* R5: g_gxsp_cap / g_gxsp_api (the C9 dispatchers below) */
#include "efb_rect.h"
#include "zbias_marker.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
/* step profile (2026-10-20, hw run: immediate 43.8 ms/frame for ~257 draws vs vitaGL 8.4): per source kind, microseconds per step,
   logged every 2048 draws as [gxm-prof]. k 0 = immediate (SRC_IMM), 1 = display list / mesh / static. */
static uint32_t s_pt[2][8], s_pn[2];
static const char *const s_ptn[8] = { "lookup", "vp", "fp", "stage", "scene", "state", "submit", "?" };
/* opt-in (2026-10-20): ~8 clock reads per draw cost 1-2 ms/frame at the hub; on only when ux0:data/stairfax/gxm_prof.txt exists (read once) */
static int s_prof = -1;
static int pt_on(void) { if (s_prof < 0) { SceUID fd = sceIoOpen("ux0:data/stairfax/gxm_prof.txt", SCE_O_RDONLY, 0); s_prof = fd >= 0; if (fd >= 0) sceIoClose(fd);
                                           gxm_log("[gxm-prof] step profile %s (gxm_prof.txt %s)", s_prof ? "ON" : "OFF", s_prof ? "present" : "absent"); } return s_prof; }
#define PT_NOW() (s_prof > 0 ? sceKernelGetProcessTimeLow() : 0u)
unsigned long long g_gxm_ptc[2][8]; unsigned g_gxm_pnc[2];   /* hw_Rsplit (docs/54 2026-10-24): the same steps, cumulative (read by vita_tool_main.c's [rsplit] line) */
#define PT_STEP(k, i) do { if (s_prof > 0) { const uint32_t t1_ = sceKernelGetProcessTimeLow(); s_pt[k][i] += t1_ - pt0_; g_gxm_ptc[k][i] += t1_ - pt0_; pt0_ = t1_; } } while (0)
static void pt_done(int k)
{
    if (s_prof <= 0) return;
    ++g_gxm_pnc[k];
    if (++s_pn[k] < 2048u) return;
    char b[400]; int n = snprintf(b, sizeof b, "[gxm-prof] %s: %u draws, us/draw", k ? "list" : "imm", s_pn[k]);
    for (int i = 0; i < 7 && n < (int) sizeof b; ++i) n += snprintf(b + n, sizeof b - n, " %s %.1f", s_ptn[i], s_pt[k][i] / (double) s_pn[k]);
    if (!k) {   /* once per imm print: GPU waits per frame since the last print */
        extern uint32_t g_gxm_wait_us[2]; static unsigned f0; static uint32_t w0[2];
        const unsigned nf = g_gx.frames - f0;
        if (nf) n += snprintf(b + n, sizeof b - n, " | per frame over %u: ring wait %.2f ms, queue add %.2f ms", nf,
                              (g_gxm_wait_us[0] - w0[0]) / 1000.0 / nf, (g_gxm_wait_us[1] - w0[1]) / 1000.0 / nf);
        f0 = g_gx.frames; w0[0] = g_gxm_wait_us[0]; w0[1] = g_gxm_wait_us[1];
    }
    gxm_log("%s", b);
    memset(s_pt[k], 0, sizeof s_pt[k]); s_pn[k] = 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GXM_SELFCHECK
#include <assert.h>
#endif

/* ================================================================================================================================== pure rules */
/* GXM_PURE_BEGIN */
/* depth function indices, in the order of vitaGL's kFunc[] (rhi_vita.c:671): GL_NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS */
enum { GXD_NEVER = 0, GXD_LESS, GXD_EQUAL, GXD_LEQUAL, GXD_GREATER, GXD_NOTEQUAL, GXD_GEQUAL, GXD_ALWAYS };
typedef struct GxmDepthRes {
    uint8_t func;      /* GXD_*: the function to set (ALWAYS when the test is off) */
    uint8_t test;      /* the test is on */
    uint8_t write;     /* depth write enabled (never with the test off: vitaGL tests.c:70-77) */
    uint8_t blended;   /* the alpha mode blends (BLEND = 2, ADD = 3 in the low byte) */
    uint32_t bkey;     /* the blend identity: 0 for OPAQUE / TEST, else the bits that change the blend (== gxm_blend_key) */
} GxmDepthRes;
/* vita_applyAlphaMode (rhi_vita.c:638-677) + vitaGL's change_depth_func (tests.c:70-77).
 *   depth    = the game's packed z mode (rhi_gxm_setDepthState): -1 = the reference (test + write, LEQUAL); else bit0 test, bits1-3 GX compare func, bit4 write
 *   alpha    = the alpha-mode word the draw is issued with (TEV paths pass TEST as OPAQUE: the program does the compare)
 *   nodepth  = rhi_vita_force_nodepth (an A/B diagnostic: the depth test is off) */
static inline GxmDepthRes gxm_depth_resolve(int depth, uint32_t alpha, int nodepth)
{
    static const uint8_t kMap[8] = { GXD_NEVER, GXD_LESS, GXD_EQUAL, GXD_LEQUAL, GXD_GREATER, GXD_NOTEQUAL, GXD_GEQUAL, GXD_ALWAYS };
    GxmDepthRes r;
    const uint32_t mode = alpha & 0xffu;
    r.blended = (uint8_t) (mode == 2u || mode == 3u);
    r.bkey = r.blended ? (alpha & 0x773ffu) : 0u;
    const int zmode = depth >= 0 && !nodepth;
    int test, want_write, func;
    if (!zmode) {                        /* g_depth == -1: test on, LEQUAL, write unless blended (the diagnostic switches the test off) */
        test = !nodepth;
        func = GXD_LEQUAL;
        want_write = !r.blended;
    } else {                             /* the game's GX z mode */
        test = depth & 1;
        func = kMap[(depth >> 1) & 7];
        want_write = ((depth >> 4) & 1) && !r.blended;
    }
    r.test = (uint8_t) (test != 0);
    r.func = (uint8_t) (test ? func : GXD_ALWAYS);
    r.write = (uint8_t) (want_write && test);
    return r;
}
/* A bad index is a GPU hang (gxd_load.c:119-124). 0 = ok, else a reason: 1 stride, 2 vbytes % stride, 3 nidx % 3, 4 index out of range, 5 no vertices for indices.
 * *need = max index + 1 (0 for no indices), so an update can re-validate a new vbytes without a second scan. */
static inline int gxm_mesh_check(uint32_t stride, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, uint32_t *need)
{
    uint32_t mx = 0;
    *need = 0;
    if (stride == 0u) return 1;
    if (vbytes % stride) return 2;
    if (nidx % 3u) return 3;
    if (nidx && !idx) return 5;
    for (uint32_t i = 0; i < nidx; ++i) if (idx[i] > mx) mx = idx[i];
    if (nidx) *need = mx + 1u;
    if (nidx && *need > vbytes / stride) return 4;
    return 0;
}
/* The layout's offsets must lie inside the stride (an attribute read past the stride of the last vertex is a read past the block). 1 = ok. */
static inline int gxm_layout_ok(const GxMeshLayout *L)
{
    int ok = L->stride > 0 && L->stride <= 4096;
    if (L->offPos < 0 || L->offPos + 12 > L->stride) ok = 0;
    if (L->offNrm >= 0 && L->offNrm + 12 > L->stride) ok = 0;
    if (L->offClr >= 0 && L->offClr + 4 > L->stride) ok = 0;
    if (L->offMtx >= 0 && L->offMtx + 4 > L->stride) ok = 0;
    for (int n = 0; n < 8; ++n) if (L->offTex[n] >= 0 && L->offTex[n] + 8 > L->stride) ok = 0;
    if (L->offBin >= 0 && L->offBin + 12 > L->stride) ok = 0;
    if (L->offTan >= 0 && L->offTan + 12 > L->stride) ok = 0;
    for (int q = 0; q < 2; ++q) if (L->offTmi[q] >= 0 && L->offTmi[q] + 16 > L->stride) ok = 0;
    if (L->offSk >= 0 && L->offSk + 32 > L->stride) ok = 0;   /* R8: aSkP + aSkN, F32x4 each */
    return ok;
}
/* Vertices per staged chunk: at most 65535 (u16 identity index), at most a quarter of the ring, a multiple of 3. 0 = the ring can never hold one triangle. */
static inline uint32_t gxm_chunk_verts(uint32_t ring_cap, uint32_t stride)
{
    if (stride == 0u) return 0u;
    uint32_t m = ring_cap / (4u * stride);
    if (m > 65535u) m = 65535u;
    return m - m % 3u;
}
/* GXM_PURE_END */

/* ================================================================================================================================== R3a switches and the state shadow (C2) */
/* The R3a cut switches: gx_draw.c's gx_r3a (set at the frame edge by gx_thread.c from the perf arm; arm 6 / fixed mode = all ON).  Bit values = port/gx_hal_state.h GX_R3A_* (gxh_lint.py checks). */
extern unsigned gx_r3a;
/* R8 GPU skinning: hal/gx_skin.c defines this; the weak default keeps a link without it (host tests) building -- no palette, so no skinned draw is ever issued */
__attribute__((weak)) const float *gx_skin_vs_palette(void) { return NULL; }
extern unsigned long long gx_prog_hint;   /* gx_draw.c: the TevKey / VsKey serials of the draw being handed over (R3a C3); 0 = none */
int g_gxm_r3a_ovr_on; unsigned g_gxm_r3a_ovr;   /* R5 (R5_SPLIT.md D6): while the render thread is split, R-BACK uses the cut switches the FRONT saw at each draw (they travel in the stream as a record), not G's live gx_r3a */
static inline unsigned r3a_now(void) { return g_gxm_r3a_ovr_on ? g_gxm_r3a_ovr : gx_r3a; }
void rhi_gxm_set_r3a_ovr(int on, unsigned mask) { g_gxm_r3a_ovr = mask; g_gxm_r3a_ovr_on = on; }
#define R3A_C3 4u
#define R3A_C1 1u   /* uniforms written by offset into the default uniform buffer */
#define R3A_C2 2u   /* depth / fragment-texture state shadowing */
#define R3A_C5 16u  /* R3c: the HAL's program-clean draw (gx_draw.c); here it only means the draw carries a key-id hint */
#define R3A_C6 32u  /* R3c: consecutive identical-state immediate draws are held and issued as ONE sceGxmDraw */
#define R3A_C7 64u  /* R3c: the HAL's display-list check trim (gx_draw.c); here it only means the census runs */
/* every entry point of this module that is not drawTev (or setDepthState / setColorTransform, whose effect a held run carries by value) first issues the run held by C6 (see the C6 block below) */
static inline void c6_entry(void) { if (g_gxm_c6_pending) gxm_c6_flush(); }
/* GXM_SHADOW_BEGIN */
/* R3a C2 (R3_DRAW.md s2): what this module last told the GXM context, so an identical call can be skipped.  THE INVALIDATION LIST IS THE CONTRACT -- a shadow that survives a change of the real state
 * is a wrong picture.  A shadow entry is valid until one of:
 *   (1) the STATE EPOCH moves (g_gx.epoch: every sceGxmBeginScene (target's tg_begin), and gxm_state_dirty() after every quad the target module draws -- clear, clearRect, EFB copy -- which set
 *       their own depth state and textures): apply_scene_state() calls gxm_sh_reset() in its epoch branch, before this draw reads anything;
 *   (2) the fragment program changes (sceGxmSetFragmentProgram): the TEXTURE entries die (gxm_sh_reset_tex); the code comment at the texture loop says Vita3K keeps texture state across a
 *       program change and says not to rely on it, so neither does this;
 *   (3) the C2 switch is off for a draw: the whole shadow dies (gxm_sh_reset), so flipping the switch back on never trusts what was written before;
 *   (4) a sceGxmSetFragmentTexture that fails records nothing (the texture entry is set only after the call returned 0);
 *   (5) init / shutdown (memset of DrawState).
 * Nothing else in the backend sets the depth function / write enable or a fragment texture (grep rhi_gxm_*.c: only tg_quad_state and the target's quads, covered by (1)). */
typedef struct GxmShadow {
    uint8_t dep_ok, dep_func, dep_write;        /* the last depth function (kGxmFunc index) and write-enable the four depth calls carried */
    uint8_t tex_ok[16];                         /* per fragment texture unit: tex[] holds what sceGxmSetFragmentTexture last received */
    uint32_t tex[16][4];                        /* SceGxmTexture = 4 control words */
} GxmShadow;
static inline void gxm_sh_reset(GxmShadow *s) { memset(s, 0, sizeof *s); }
static inline void gxm_sh_reset_tex(GxmShadow *s) { memset(s->tex_ok, 0, sizeof s->tex_ok); }
/* 1 = the four depth calls are needed (and the shadow now holds them), 0 = identical to the last ones, skip */
static inline int gxm_sh_depth_need(GxmShadow *s, unsigned func, unsigned write)
{
    if (s->dep_ok && s->dep_func == (uint8_t) func && s->dep_write == (uint8_t) write) return 0;
    s->dep_ok = 1; s->dep_func = (uint8_t) func; s->dep_write = (uint8_t) write;
    return 1;
}
/* 1 = sceGxmSetFragmentTexture is needed for this unit, 0 = the unit already holds exactly these control words */
static inline int gxm_sh_tex_need(const GxmShadow *s, unsigned unit, const uint32_t w[4])
{
    return !(unit < 16u && s->tex_ok[unit] && s->tex[unit][0] == w[0] && s->tex[unit][1] == w[1] && s->tex[unit][2] == w[2] && s->tex[unit][3] == w[3]);
}
/* call after a sceGxmSetFragmentTexture that returned 0 */
static inline void gxm_sh_tex_set(GxmShadow *s, unsigned unit, const uint32_t w[4])
{
    if (unit >= 16u) return;
    s->tex[unit][0] = w[0]; s->tex[unit][1] = w[1]; s->tex[unit][2] = w[2]; s->tex[unit][3] = w[3]; s->tex_ok[unit] = 1;
}
/* GXM_SHADOW_END */

_Static_assert(sizeof(SceGxmTexture) == 16, "gxm_sh_tex_* compare a SceGxmTexture as four control words");
_Static_assert(RHI_ALPHA_BLEND == 2 && RHI_ALPHA_ADD == 3 && RHI_ALPHA_TEST == 1 && RHI_ALPHA_OPAQUE == 0, "gxm_depth_resolve hardcodes the alpha classes");
_Static_assert(sizeof(TevVertex) == 84 && offsetof(TevVertex, c0) == 12 && offsetof(TevVertex, c1) == 16 && offsetof(TevVertex, t) == 20, "TevVertex layout (rhi_vita_tev.inc:1092)");
_Static_assert(sizeof(RhiColorVertex) == 16 && offsetof(RhiColorVertex, rgba) == 12, "RhiColorVertex layout");
_Static_assert(sizeof(RhiTexVertex) == 24 && offsetof(RhiTexVertex, rgba) == 12 && offsetof(RhiTexVertex, u) == 16, "RhiTexVertex layout");

/* ================================================================================================================================== state */
static inline void blk_defer(GxmBlk *b) { if (b->p) gxm_blk_free_deferred(b); }
static inline void blk_now(GxmBlk *b) { if (b->p) gxm_blk_free_now(b); }
#define MESH_MAX_SLOTS 6             /* 3 from the first update; a mesh updated more than 3 times inside one scene grows (counted) up to this */
#define MESH_FIRST_SLOTS 3
#define VPM_N 16
#define FPM_N 8
#define STATIC_STRIDE ((uint32_t) sizeof(RhiTexVertex))

typedef struct GxmMesh {
    struct GxmMesh *prev, *next;
    GxMeshLayout lay;
    uint32_t nidx, vbytes, need;                         /* need: max index + 1 */
    GxmBlk vtx;                                          /* the first (CDRAM) copy of the vertices; deferred-freed at the first update */
    GxmBlk slot[MESH_MAX_SLOTS]; uint32_t slot_cap[MESH_MAX_SLOTS], slot_seq[MESH_MAX_SLOTS];   /* dynamic vertex slots (LPDDR); seq: the scene a draw last read it in */
    int nslots, cur;
    uint8_t slot_img[MESH_MAX_SLOTS]; uint32_t slot_vb[MESH_MAX_SLOTS]; int pend_c; uint32_t pend_vb;   /* R6 C9: slot_img: the slot holds a full vertex image of slot_vb bytes; pend_*: the slot update_begin chose, for update_commit */
    GxmBlk idx;
    uint8_t bad;
    uint32_t upd;                                        /* R9 census: count of vertex rewrites (update_commit); a replay must not reuse a rewritten mesh's record */
} GxmMesh;

typedef struct StaticVbo { GxmBlk blk; uint32_t n; uint8_t used; } StaticVbo;

typedef struct DrawState {
    int inited;
    int depth;                                           /* the game's packed z mode (-1 = the reference) */
    int vp[4];                                           /* GL rect {x, y, w, h}, y from the BOTTOM of M */
    int sc[4];                                           /* GL rect, same */
    int clip[4]; int sc_empty;                           /* region clip, top-origin inclusive, already intersected with M */
    int zb_on, zb_f, zb_u;
    int zb_faces, sh_cull;   /* shadow receivers (2026-10-27 dotted-shadow A/B): shadow_zbias_faces.txt 2 = bias BACK faces too; shadow_cull.txt 1 = CW, 2 = CCW */
    int vp_dirty, sc_dirty, zb_dirty;
    unsigned applied_epoch;
    SceGxmVertexProgram *cur_vp; SceGxmFragmentProgram *cur_fp;
    GxmBlk ident;                                        /* u16 0..65535 (LPDDR) */
    GxmMesh *meshes; unsigned mesh_alive;
    StaticVbo *st; unsigned st_cap, st_alive;
    unsigned skipctr;
    /* counters (report tokens) */
    unsigned ring_breaks;
    unsigned draw_calls, chunk_splits, mesh_bad, mesh_slot_waits, mesh_slot_grow, mesh_update_refused, mesh_alloc_fail, mesh_created, mesh_updated, mesh_destroyed, state_calls;
    unsigned static_fail, static_bad_id, ring_skips, state_reapplies, ub_fail, arm_dropped, arm_nodraw, vpm_miss, fpm_miss, vp_changes, sc_changes, sc_empty_n, acct_viol;
    unsigned mesh_logged;
    GxmShadow sh; int sh_active;                         /* R3a C2: the shadow, and whether it has been written since the switch was last off */
    unsigned dep_skipped, tex_skipped, sh_resets;       /* R3a C2 counters: depth call groups / texture binds not issued, shadow resets by epoch or program change */
    unsigned uni_fast, uni_ref, uni_verify_ok, uni_verify_bad;   /* R3a C1: draws whose stage buffers were written by offset / by the API; first-use comparisons that matched / did not */
} DrawState;
static DrawState s_d;

typedef struct { const GxmProg *p; const void *key; SceGxmVertexProgram *vp; } VpMemo;
typedef struct { const GxmProg *p; uint32_t bk; SceGxmFragmentProgram *fp; } FpMemo;
static VpMemo s_vpm[VPM_N]; static unsigned s_vpm_next;
static FpMemo s_fpm[FPM_N]; static unsigned s_fpm_next;
static const char s_key_tev = 0, s_key_col = 0, s_key_tex = 0;      /* addresses are the memo keys of the immediate layouts */

extern int rhi_vita_colored_honors_alpha;                /* rhi_vita.c:261 - the game build sets it */
extern int rhi_vita_force_nodepth;                       /* rhi_vita.c:591 - an A/B diagnostic */

static const SceGxmDepthFunc kGxmFunc[8] = {
    SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_LESS, SCE_GXM_DEPTH_FUNC_EQUAL, SCE_GXM_DEPTH_FUNC_LESS_EQUAL,
    SCE_GXM_DEPTH_FUNC_GREATER, SCE_GXM_DEPTH_FUNC_NOT_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER_EQUAL, SCE_GXM_DEPTH_FUNC_ALWAYS };

/* ================================================================================================================================== accounting */
static inline void account(int kind, int why)
{
    if (why < 0) ++g_gx.st.issued[kind];
    else ++g_gx.st.skip[kind][why < GXM_SK_N ? why : GXM_SK_COMPILE];
}
static inline void refuse_pre(int kind) { ++g_gx.st.refused_pre[kind]; }
/* req++ and the dead / SKIPDRAW gates. Returns 1 to go on; 0 = the request is finished here (accounted, or SILENTLY dropped by the break arm). */
static int admit(int kind)
{
    ++g_gx.st.req[kind];
    if ((g_gx.arm & GXM_ARM_SKIPDRAW) && ++s_d.skipctr % 7u == 0u) { ++s_d.arm_dropped; return 0; }   /* the break arm: req moved, nothing else does */
    if (!s_d.inited || gxm_dead()) { account(kind, GXM_SK_DEAD); return 0; }
    return 1;
}
static unsigned skip_total(int kind)
{
    unsigned t = 0;
    for (int w = 0; w < GXM_SK_N; ++w) t += g_gx.st.skip[kind][w];
    return t;
}

/* ================================================================================================================================== scene state */
static void compute_clip(void)
{
    const int rw = g_gx.rw, rh = g_gx.rh;
    const int x0 = s_d.sc[0], x1 = s_d.sc[0] + s_d.sc[2] - 1;
    const int y0 = rh - (s_d.sc[1] + s_d.sc[3]), y1 = rh - s_d.sc[1] - 1;     /* GL row (bottom origin) -> top-origin rows, inclusive */
    int c[4] = { x0, y0, x1, y1 };
    s_d.sc_empty = s_d.sc[2] <= 0 || s_d.sc[3] <= 0;
    if (c[0] < 0) c[0] = 0;
    if (c[1] < 0) c[1] = 0;
    if (c[2] > rw - 1) c[2] = rw - 1;
    if (c[3] > rh - 1) c[3] = rh - 1;
    if (c[2] < c[0] || c[3] < c[1]) s_d.sc_empty = 1;
    memcpy(s_d.clip, c, sizeof c);
}
static void apply_viewport(void)
{
    const float xs = (float) (s_d.vp[2] >> 1), ys = -(float) (s_d.vp[3] >> 1);       /* vitaGL misc.c:329-332 viewport maths, on a top-first target */
    sceGxmSetViewport(g_gx.ctx, (float) s_d.vp[0] + xs, xs, (float) (g_gx.rh - s_d.vp[1]) + ys, ys, 0.5f, 0.5f);
}
static void apply_clip(void)
{
    if (s_d.sc_empty) return;                                                        /* a draw never reaches here with an empty box */
    sceGxmSetRegionClip(g_gx.ctx, SCE_GXM_REGION_CLIP_OUTSIDE, (unsigned) s_d.clip[0], (unsigned) s_d.clip[1], (unsigned) s_d.clip[2], (unsigned) s_d.clip[3]);
}
static void apply_zbias(void)
{
    const int f = s_d.zb_on ? s_d.zb_f : 0, u = s_d.zb_on ? s_d.zb_u : 0;
    sceGxmSetFrontDepthBias(g_gx.ctx, f, u);                                          /* front only by default, like vitaGL misc.c:157 (cull is off) */
    /* Dotted-shadow A/B (2026-10-27): objDrawShadowCasterMesh draws its receivers with GX_CULL_FRONT, i.e. retail keeps their BACK faces, which a
       front-only bias never reaches; and the port never culls, so the faces retail drops are drawn too. Both behind markers until a shot pair decides. */
    sceGxmSetBackDepthBias(g_gx.ctx, s_d.zb_faces == 2 ? f : 0, s_d.zb_faces == 2 ? u : 0);
    sceGxmSetCullMode(g_gx.ctx, s_d.zb_on && s_d.sh_cull == 1 ? SCE_GXM_CULL_CW : s_d.zb_on && s_d.sh_cull == 2 ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_NONE);
}
/* inside an open scene: re-apply everything when the epoch moved, else only what a setter changed */
/* GXM_SCENE_STATE_BEGIN */
static void apply_scene_state(void)
{
    if (s_d.applied_epoch != g_gx.epoch) {
        sceGxmSetCullMode(g_gx.ctx, SCE_GXM_CULL_NONE);                              /* vitaGL never culls (rhi_vita.c:1082) */
        apply_viewport(); apply_clip(); apply_zbias();
        s_d.cur_vp = NULL; s_d.cur_fp = NULL;
        gxm_sh_reset(&s_d.sh); ++s_d.sh_resets;                                       /* R3a C2 invalidation (1): a new scene / a target quad changed the context behind our back */
        s_d.applied_epoch = g_gx.epoch;
        s_d.vp_dirty = s_d.sc_dirty = s_d.zb_dirty = 0;
        ++s_d.state_reapplies;
        return;
    }
    if (s_d.vp_dirty) { apply_viewport(); s_d.vp_dirty = 0; }
    if (s_d.sc_dirty) { apply_clip(); s_d.sc_dirty = 0; }
    if (s_d.zb_dirty) { apply_zbias(); s_d.zb_dirty = 0; }
}
static void apply_depth(const GxmDepthRes *r)
{
    const SceGxmDepthFunc f = kGxmFunc[r->func & 7];
    const SceGxmDepthWriteMode w = r->write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthFunc(g_gx.ctx, f); sceGxmSetBackDepthFunc(g_gx.ctx, f);       /* all four, as vitaGL re-sends them */
    sceGxmSetFrontDepthWriteEnable(g_gx.ctx, w); sceGxmSetBackDepthWriteEnable(g_gx.ctx, w);
}
/* GXM_SCENE_STATE_END */

static void zbias_load(void)
{
    char b[64]; int len = 0;
    memset(b, 0, sizeof b);
    const SceUID f = sceIoOpen("ux0:data/stairfax/shadow_zbias.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) { len = sceIoRead(f, b, (SceSize) (sizeof b - 1)); sceIoClose(f); if (len < 0) len = 0; }
    const int ok = zb_parse(b, len, &s_d.zb_f, &s_d.zb_u);
    {   char c = 0; SceUID g;
        /* DEFAULT CCW (2026-10-28, eboot 6f49a3dc, hub shots vs retail_ch2_frame.png): GX_CULL_FRONT on the receivers = CCW here. It removes the dotted
           lines AND the phantom Arwing ground shadow that retail never draws; CW and no-cull keep both. shadow_cull.txt 0 = none (the old port), 1 = CW. */
        s_d.zb_faces = 1; s_d.sh_cull = 2;
        if ((g = sceIoOpen("ux0:data/stairfax/shadow_zbias_faces.txt", SCE_O_RDONLY, 0777)) >= 0) { if (sceIoRead(g, &c, 1) == 1 && c == '2') s_d.zb_faces = 2; sceIoClose(g); }
        c = 0;
        if ((g = sceIoOpen("ux0:data/stairfax/shadow_cull.txt", SCE_O_RDONLY, 0777)) >= 0) { if (sceIoRead(g, &c, 1) == 1 && (c == '0' || c == '1' || c == '2')) s_d.sh_cull = c - '0'; sceIoClose(g); }
        gxm_log("[gxm-draw] shadow receivers: bias on %s face(s) (shadow_zbias_faces.txt) | cull %s (shadow_cull.txt)",
                s_d.zb_faces == 2 ? "BOTH" : "the front", s_d.sh_cull == 1 ? "CW" : s_d.sh_cull == 2 ? "CCW" : "none");
    }
    gxm_log("[gxm-draw] shadow-zbias factor %d units %d%s%s", s_d.zb_f, s_d.zb_u, (s_d.zb_f || s_d.zb_u) ? "" : " (OFF)",
            f < 0 ? " (no shadow_zbias.txt: default)" : ok ? " (shadow_zbias.txt)" : " (shadow_zbias.txt unparsable: default)");
}

/* the setters: state only, applied by the next draw inside a scene */
void rhi_gxm_setDepthState(int test, int func, int write)
{
    ++s_d.state_calls;
    s_d.depth = (test & 1) | ((func & 7) << 1) | ((write & 1) << 4);                  /* rhi_vita.c:228 */
}
void rhi_gxm_setViewport(float x, float y, float w, float h)
{
    int vp[4];
    ++s_d.state_calls;
    if (!s_d.inited || g_gx.rw <= 0) return;
    if (!efb_vp_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh, vp)) return;                 /* invalid / empty: the call is ignored (rhi_vita.c:443) */
    if (!memcmp(vp, s_d.vp, sizeof vp)) return;
    c6_entry();                                                                         /* R3c C6: a held run applies s_d.vp when it is issued */
    memcpy(s_d.vp, vp, sizeof vp);
    s_d.vp_dirty = 1; ++s_d.vp_changes;
}
void rhi_gxm_setScissor(int x, int y, int w, int h)
{
    int sc[4];
    ++s_d.state_calls;
    if (!s_d.inited || g_gx.rw <= 0) return;
    efb_sc_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh, sc);
    if (!memcmp(sc, s_d.sc, sizeof sc)) return;
    c6_entry();                                                                         /* R3c C6: ... and the scissor */
    memcpy(s_d.sc, sc, sizeof sc);
    compute_clip();
    s_d.sc_dirty = 1; ++s_d.sc_changes;
    if (s_d.sc_empty) ++s_d.sc_empty_n;
}
void rhi_gxm_setShadowZBias(int on)
{
    ++s_d.state_calls;
    if (!s_d.inited) return;
    on = on && (s_d.zb_f || s_d.zb_u);
    if (on == s_d.zb_on) return;
    c6_entry();                                                                         /* R3c C6: ... and the shadow bias */
    s_d.zb_on = on; s_d.zb_dirty = 1;
}

/* ================================================================================================================================== programs and vertex layouts */
enum { LAY_TEV = 0, LAY_COL, LAY_TEX, LAY_MESH };
enum { SRC_IMM = 0, SRC_STATIC, SRC_MESH };
typedef struct Draw {
    int kind;                                            /* GXM_K_* */
    GxmProg *prog;
    uint32_t alpha;                                      /* the alpha-mode word the fragment program and the depth rule see */
    int depth;
    GxmTex *tex[TEVK_TEX]; int ntex;
    const float *mvp;                                    /* row-major 4x4: TEV / fixed-function; NULL on the GX path */
    const VsUniforms *vu; const VsKey *vk;               /* GX path */
    const TevUniforms *u;
    int lay; GxmMesh *mesh;
    int src;
    const void *cpu;                                     /* SRC_IMM: the caller's vertices (copied into the ring) */
    const uint8_t *gpu_vtx;                              /* SRC_STATIC: first vertex; SRC_MESH: the current slot */
    uint32_t count;                                      /* vertices (IMM / STATIC) */
    uint32_t stride;
    uint32_t nidx;                                       /* SRC_MESH */
    const void *gpu_idx;
    GxmTex *const *tex_id;                               /* R3c C6: the textures' real identities when tex[] holds copies (a held run outlives a destroyed texture); NULL = tex[] itself */
    const float *skin_pal;                               /* R8: a hasSkin GX draw: the uTMX image (palette rows + the scale row, VS_SKIN_PAL_VEC4 float4); NULL otherwise */
} Draw;

static const char *const kTn[8] = { "aT0", "aT1", "aT2", "aT3", "aT4", "aT5", "aT6", "aT7" };
static int add_attr(GxmAttr *a, int n, const char *name, int off, int fmt, int comps)
{
    a[n].name = name; a[n].off = (uint16_t) off; a[n].fmt = (uint8_t) fmt; a[n].comps = (uint8_t) comps;
    return n + 1;
}
/* attribute NAMES are the generated programs' own (rhi_vita_tev.inc:1092-1093, 1520-1531); a name the GXP lacks is skipped by the shader module */
static int build_attrs(const Draw *d, GxmAttr *a)
{
    int n = 0;
    switch (d->lay) {
    case LAY_TEV: {
        int nt = d->prog->tk.numTex; if (nt > TEVK_TEX) nt = TEVK_TEX;
        n = add_attr(a, n, "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        n = add_attr(a, n, "aColor0", (int) offsetof(TevVertex, c0), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        n = add_attr(a, n, "aColor1", (int) offsetof(TevVertex, c1), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        for (int t = 0; t < nt; ++t) n = add_attr(a, n, kTn[t], (int) (offsetof(TevVertex, t) + 8u * (unsigned) t), SCE_GXM_ATTRIBUTE_FORMAT_F32, 2);
        break; }
    case LAY_COL:     /* aColor1 is aliased to the colour: an unbound attribute would read undefined bytes */
        n = add_attr(a, n, "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        n = add_attr(a, n, "aColor0", (int) offsetof(RhiColorVertex, rgba), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        n = add_attr(a, n, "aColor1", (int) offsetof(RhiColorVertex, rgba), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        break;
    case LAY_TEX:
        n = add_attr(a, n, "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        n = add_attr(a, n, "aColor0", (int) offsetof(RhiTexVertex, rgba), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        n = add_attr(a, n, "aColor1", (int) offsetof(RhiTexVertex, rgba), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        n = add_attr(a, n, "aT0", (int) offsetof(RhiTexVertex, u), SCE_GXM_ATTRIBUTE_FORMAT_F32, 2);
        break;
    default: {        /* LAY_MESH: the attribute set comes from the key the PROGRAM was made from (c2floor :593-607), only where the mesh carries it */
        const VsKey *vk = &d->prog->vk; const GxMeshLayout *L = &d->mesh->lay;
        n = add_attr(a, n, "aPos", L->offPos, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        if (vk->hasNrm && L->offNrm >= 0) n = add_attr(a, n, "aNrm", L->offNrm, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        if (vk->hasClr0 && L->offClr >= 0) n = add_attr(a, n, "aColor0", L->offClr, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4);
        if (vk->hasMtxIdx && L->offMtx >= 0) n = add_attr(a, n, "aMtx", L->offMtx, SCE_GXM_ATTRIBUTE_FORMAT_F32, 1);
        for (int t = 0; t < 8; ++t) if ((vk->texMask >> t & 1) && L->offTex[t] >= 0) n = add_attr(a, n, kTn[t], L->offTex[t], SCE_GXM_ATTRIBUTE_FORMAT_F32, 2);
        if (vk->hasBin && L->offBin >= 0) n = add_attr(a, n, "aBin", L->offBin, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        if (vk->hasTan && L->offTan >= 0) n = add_attr(a, n, "aTan", L->offTan, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3);
        { const unsigned tg = tev_vs_tmi_groups(vk);
          if ((tg & 1) && L->offTmi[0] >= 0) n = add_attr(a, n, "aTmA", L->offTmi[0], SCE_GXM_ATTRIBUTE_FORMAT_F32, 4);
          if ((tg & 2) && L->offTmi[1] >= 0) n = add_attr(a, n, "aTmB", L->offTmi[1], SCE_GXM_ATTRIBUTE_FORMAT_F32, 4); }
        if (vk->hasSkin && L->offSk >= 0) {   /* R8: F32x4 holding the integers 0..255 (a packed float lost low bits, a non-normalised U8 did not arrive as integers) */
            n = add_attr(a, n, "aSkP", L->offSk, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4);
            if (vk->hasNrm) n = add_attr(a, n, "aSkN", L->offSk + 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4);
        }   /* R8: last, as the generator declares it */
        break; }
    }
    return n;
}
static const void *vp_key(const Draw *d)
{
    switch (d->lay) { case LAY_TEV: return &s_key_tev; case LAY_COL: return &s_key_col; case LAY_TEX: return &s_key_tex; default: return d->mesh; }
}
static SceGxmVertexProgram *vp_get(const Draw *d)
{
    const void *key = vp_key(d);
    for (int i = 0; i < VPM_N; ++i) if (s_vpm[i].p == d->prog && s_vpm[i].key == key) return s_vpm[i].vp;
    GxmAttr a[GXM_MAX_GPU_ATTRS];
    const int n = build_attrs(d, a);
    SceGxmVertexProgram *vp = gxm_shader_vp(d->prog, a, n, (int) d->stride);
    ++s_d.vpm_miss;
    if (vp) { s_vpm[s_vpm_next % VPM_N].p = d->prog; s_vpm[s_vpm_next % VPM_N].key = key; s_vpm[s_vpm_next % VPM_N].vp = vp; ++s_vpm_next; }
    return vp;
}
static void vp_forget(const void *key)
{
    for (int i = 0; i < VPM_N; ++i) if (s_vpm[i].key == key) { s_vpm[i].p = NULL; s_vpm[i].key = NULL; s_vpm[i].vp = NULL; }
}
static SceGxmFragmentProgram *fp_get(const GxmProg *p, uint32_t alpha)
{
    const uint32_t bk = gxm_blend_key(alpha);
    for (int i = 0; i < FPM_N; ++i) if (s_fpm[i].p == p && s_fpm[i].bk == bk && s_fpm[i].fp) return s_fpm[i].fp;
    SceGxmFragmentProgram *fp = gxm_shader_fp((GxmProg *) p, alpha);
    ++s_d.fpm_miss;
    if (fp) { s_fpm[s_fpm_next % FPM_N].p = p; s_fpm[s_fpm_next % FPM_N].bk = bk; s_fpm[s_fpm_next % FPM_N].fp = fp; ++s_fpm_next; }
    return fp;
}

/* ================================================================================================================================== uniforms (c2floor :719-770) */
/* GXM_UNI_BEGIN */
static inline void fcopy(float *dst, const void *src, unsigned n) { memcpy(dst, src, n * 4u); }
static int fill_vub(const GxmProg *p, const Draw *d, void *vub)
{
    int bad = 0; float tmp[4 * VS_MAX_LIGHTS];
    const VsUniforms *vu = d->vu; const VsKey *vk = d->vk;
    for (int i = 0; i < p->nvb; ++i) {
        const GxmUBind *b = &p->vb[i]; const float *src = NULL; unsigned n = 0;
        switch (b->kind) {
        case GU_M:
            if (vu) src = vu->proj[b->idx & 3]; else if (d->mvp) src = d->mvp + 4 * (b->idx & 3); else continue;   /* rhi_vita_tev.inc:1152 / :1639: row r of the matrix */
            n = 4; break;
        case GU_POS: if (!vu || !vk) continue; src = &vu->pm[0][0][0]; n = (vk->hasMtxIdx ? 30u : 3u) * 4u; break;
        case GU_NRM: if (!vu || !vk) continue; src = &vu->nm[0][0][0]; n = (vk->hasMtxIdx ? 30u : 3u) * 4u; break;
        case GU_TMX: if (!vu) continue; src = &vu->tmx[0][0][0]; n = 120; break;
        case GU_BONE: if (!d->skin_pal) continue; src = d->skin_pal; n = 4u * VS_SKIN_PAL_VEC4; break;   /* R8: the bone palette + scale row (gx_tev_key.h), only a hasSkin program declares it */
        case GU_MAT: if (!vu) continue; src = vu->mat; n = 4; break;
        case GU_AMB: if (!vu) continue; src = vu->amb; n = 4; break;
        case GU_TM: if (!vu || b->idx >= TEVK_TEX) continue; src = &vu->tm[b->idx][0][0]; n = 12; break;
        case GU_PT: if (!vu || b->idx >= TEVK_TEX) continue; src = &vu->pt[b->idx][0][0]; n = 12; break;
        case GU_LT: {
            if (!vu || !vk) continue;
            const int c = (b->idx >> 4) & 1, q = b->idx & 15;
            unsigned nl = vk->ch[c].nLights; if (nl > VS_MAX_LIGHTS) nl = VS_MAX_LIGHTS;
            if (!nl || q > 4) continue;
            for (unsigned l = 0; l < nl; ++l) {
                const VsLight *L = &vu->lt[c][l];
                fcopy(tmp + 4 * l, q == 0 ? L->pos : q == 1 ? L->dir : q == 2 ? L->a : q == 3 ? L->k : L->col, 4);
            }
            src = tmp; n = nl * 4u; break; }
        default: continue;
        }
        if (n > b->maxc) n = b->maxc;
        if (!n) continue;
        bad |= GXM_CHK(sceGxmSetUniformDataF(vub, b->p, 0, n, src));
    }
    return bad;
}
static int fill_fub(const GxmProg *p, const Draw *d, void *fub)
{
    int bad = 0; float t[4]; const TevUniforms *u = d->u; const TevKey *tk = &p->tk;
    for (int i = 0; i < p->nfb; ++i) {
        const GxmUBind *b = &p->fb[i]; unsigned n = 4;
        switch (b->kind) {
        case GU_R: for (int c = 0; c < 4; ++c) t[c] = (float) u->reg[b->idx & 3][c]; break;
        case GU_K: for (int c = 0; c < 4; ++c) t[c] = (float) u->konst[b->idx & 3][c]; break;
        case GU_AREF: t[0] = (float) u->acRef0; t[1] = (float) u->acRef1; n = 2; break;
        case GU_FOG: if (!tk->fogType) continue; t[0] = u->fogStart; t[1] = u->fogScale; t[2] = t[3] = 0.f; break;
        case GU_FOGC: if (!tk->fogType) continue; for (int c = 0; c < 3; ++c) t[c] = u->fogColor[c] / 255.0f; t[3] = 0.f; break;
        case GU_IMA: if (!tk->ik.on || b->idx > 2) continue; t[0] = u->indM[b->idx][0][0]; t[1] = u->indM[b->idx][0][1]; t[2] = u->indM[b->idx][0][2]; t[3] = u->indE[b->idx]; break;
        case GU_IMB: if (!tk->ik.on || b->idx > 2) continue; t[0] = u->indM[b->idx][1][0]; t[1] = u->indM[b->idx][1][1]; t[2] = u->indM[b->idx][1][2]; t[3] = 0.f; break;
        case GU_TS: {
            if (!tk->ik.on || b->idx >= d->ntex || !d->tex[b->idx]) continue;
            t[0] = (float) d->tex[b->idx]->w; t[1] = (float) d->tex[b->idx]->h; t[2] = 1.0f / t[0]; t[3] = 1.0f / t[1]; break; }
        default: continue;
        }
        if (n > b->maxc) n = b->maxc;
        if (!n) continue;
        bad |= GXM_CHK(sceGxmSetUniformDataF(fub, b->p, 0, n, t));
    }
    return bad;
}
/* R3a C1 (R3_DRAW.md s2): the same two fills, writing by OFFSET.  sceGxmSetUniformDataF(buf, param, 0, n, src) stores n floats at buf + 4 * (resource index + 0); the resource index is resolved once per
 * program (GxmUBind.off, rhi_gxm_shader.c gxs_bind_params) and only for programs whose every binding is a plain F32 uniform inside its buffer (GxmProg.ufast).  fill_vub / fill_fub above stay as
 * the reference (the A/B arm, and host/tests/gxm_uni_test.c compares the two byte for byte: the per-kind skips, the clamp to maxc, the light gather).  On the device the first UNI_VERIFY_N draws
 * of every program ALSO run the API fill and compare the whole buffer (uni_verify): a program whose offsets disagree falls back to the API for good. */
static inline void ufast_put(void *ub, const GxmUBind *b, const float *src, unsigned n) { memcpy((float *) ub + b->off, src, (size_t) n * 4u); }
static void fill_vub_off(const GxmProg *p, const Draw *d, void *vub)
{
    float tmp[4 * VS_MAX_LIGHTS];
    const VsUniforms *vu = d->vu; const VsKey *vk = d->vk;
    for (int i = 0; i < p->nvb; ++i) {
        const GxmUBind *b = &p->vb[i]; const float *src = NULL; unsigned n = 0;
        switch (b->kind) {
        case GU_M:
            if (vu) src = vu->proj[b->idx & 3]; else if (d->mvp) src = d->mvp + 4 * (b->idx & 3); else continue;
            n = 4; break;
        case GU_POS: if (!vu || !vk) continue; src = &vu->pm[0][0][0]; n = (vk->hasMtxIdx ? 30u : 3u) * 4u; break;
        case GU_NRM: if (!vu || !vk) continue; src = &vu->nm[0][0][0]; n = (vk->hasMtxIdx ? 30u : 3u) * 4u; break;
        case GU_TMX: if (!vu) continue; src = &vu->tmx[0][0][0]; n = 120; break;
        case GU_BONE: if (!d->skin_pal) continue; src = d->skin_pal; n = 4u * VS_SKIN_PAL_VEC4; break;   /* R8: the bone palette + scale row (gx_tev_key.h), only a hasSkin program declares it */
        case GU_MAT: if (!vu) continue; src = vu->mat; n = 4; break;
        case GU_AMB: if (!vu) continue; src = vu->amb; n = 4; break;
        case GU_TM: if (!vu || b->idx >= TEVK_TEX) continue; src = &vu->tm[b->idx][0][0]; n = 12; break;
        case GU_PT: if (!vu || b->idx >= TEVK_TEX) continue; src = &vu->pt[b->idx][0][0]; n = 12; break;
        case GU_LT: {
            if (!vu || !vk) continue;
            const int c = (b->idx >> 4) & 1, q = b->idx & 15;
            unsigned nl = vk->ch[c].nLights; if (nl > VS_MAX_LIGHTS) nl = VS_MAX_LIGHTS;
            if (!nl || q > 4) continue;
            for (unsigned l = 0; l < nl; ++l) {
                const VsLight *L = &vu->lt[c][l];
                fcopy(tmp + 4 * l, q == 0 ? L->pos : q == 1 ? L->dir : q == 2 ? L->a : q == 3 ? L->k : L->col, 4);
            }
            src = tmp; n = nl * 4u; break; }
        default: continue;
        }
        if (n > b->maxc) n = b->maxc;
        if (!n) continue;
        ufast_put(vub, b, src, n);
    }
}
static void fill_fub_off(const GxmProg *p, const Draw *d, void *fub)
{
    float t[4]; const TevUniforms *u = d->u; const TevKey *tk = &p->tk;
    for (int i = 0; i < p->nfb; ++i) {
        const GxmUBind *b = &p->fb[i]; unsigned n = 4;
        switch (b->kind) {
        case GU_R: for (int c = 0; c < 4; ++c) t[c] = (float) u->reg[b->idx & 3][c]; break;
        case GU_K: for (int c = 0; c < 4; ++c) t[c] = (float) u->konst[b->idx & 3][c]; break;
        case GU_AREF: t[0] = (float) u->acRef0; t[1] = (float) u->acRef1; n = 2; break;
        case GU_FOG: if (!tk->fogType) continue; t[0] = u->fogStart; t[1] = u->fogScale; t[2] = t[3] = 0.f; break;
        case GU_FOGC: if (!tk->fogType) continue; for (int c = 0; c < 3; ++c) t[c] = u->fogColor[c] / 255.0f; t[3] = 0.f; break;
        case GU_IMA: if (!tk->ik.on || b->idx > 2) continue; t[0] = u->indM[b->idx][0][0]; t[1] = u->indM[b->idx][0][1]; t[2] = u->indM[b->idx][0][2]; t[3] = u->indE[b->idx]; break;
        case GU_IMB: if (!tk->ik.on || b->idx > 2) continue; t[0] = u->indM[b->idx][1][0]; t[1] = u->indM[b->idx][1][1]; t[2] = u->indM[b->idx][1][2]; t[3] = 0.f; break;
        case GU_TS: {
            if (!tk->ik.on || b->idx >= d->ntex || !d->tex[b->idx]) continue;
            t[0] = (float) d->tex[b->idx]->w; t[1] = (float) d->tex[b->idx]->h; t[2] = 1.0f / t[0]; t[3] = 1.0f / t[1]; break; }
        default: continue;
        }
        if (n > b->maxc) n = b->maxc;
        if (!n) continue;
        ufast_put(fub, b, t, n);
    }
}
/* GXM_UNI_END */
#define UNI_VERIFY_N 2u
/* The API fills have just written `vub` / `fub`.  Replay the offset writes over a COPY of each buffer: a correct offset rewrites identical bytes, a wrong one changes some.  1 = a difference, -1 = a
 * buffer too big to check (the program then stays on the API), 0 = identical. */
static int uni_verify(const GxmProg *p, const Draw *d, const void *vub, const void *fub)
{
    static float scr[4096];
    int bad = 0;
    if (p->vub_bytes > sizeof scr || p->fub_bytes > sizeof scr) return -1;
    if (p->vub_bytes && vub) { memcpy(scr, vub, p->vub_bytes); fill_vub_off(p, d, scr); bad |= memcmp(scr, vub, p->vub_bytes) != 0; }
    if (p->fub_bytes && fub) { memcpy(scr, fub, p->fub_bytes); fill_fub_off(p, d, scr); bad |= memcmp(scr, fub, p->fub_bytes) != 0; }
    return bad;
}

/* ================================================================================================================================== the one draw */
/* Returns -1 when the whole request reached the GPU command stream (or, under the NODRAW arm, would have), else a GXM_SK_* reason. */
/* ================================================================================================================================== R7 census (2026-10-25, docs/54 hub census)
 * Could consecutive drawTevGx calls be ONE instanced draw?  The game sorts opaque RUNTIME_BATCHABLE objects by romDefNo (shader.c sceneDraw sort key), so
 * same-type objects (long grass x37, ferns, flowers) reach R back to back.  Per issued drawTevGx, against the previous issued draw (a run is broken by ANY draw
 * in between): same mesh + same TEV/VS keys + same TevUniforms + same textures / alpha / depth / viewport / scissor, and the VsUniforms equal
 *   A: except the position / normal matrix palette (pm, nm)                 -> per-instance matrices only
 *   B: except pm, nm and the texture matrices (tm, pt, tmx)                 -> + per-instance texture matrices
 *   C: except those and the colours / lights (mat, amb, lt)                 -> + per-instance lighting
 *   D: like C but a DIFFERENT mesh                                          -> concatenation, not instancing
 * Instrument only: on when ux0:data/stairfax/gxm_r7.txt exists (read once); off = one load and an untaken branch. */
static unsigned s_r7_seq;
static int s_r7_on = -1;
static unsigned s_r7_c[8];   /* tevgx issued, A, B, C, D, runs (A), max run (A), not used */
static struct { unsigned seq, run; const void *mesh; uint32_t alpha; int depth, vp[4], sc[4]; TevKey tk; VsKey vk; TevUniforms u; VsUniforms vu; RhiTexture *tex[TEVK_TEX]; } s_r7p;
void rhi_gxm_r7_census(unsigned o[8]) { memcpy(o, s_r7_c, sizeof s_r7_c); }
static int r7_on(void) { if (s_r7_on < 0) { SceUID fd = sceIoOpen("ux0:data/stairfax/gxm_r7.txt", SCE_O_RDONLY, 0); s_r7_on = fd >= 0; if (fd >= 0) sceIoClose(fd);
                                            gxm_log("[gxm] R7 merge census %s (gxm_r7.txt %s)", s_r7_on ? "ON" : "OFF", s_r7_on ? "present" : "absent"); } return s_r7_on; }
static int r7_eq_except(const VsUniforms *a, const VsUniforms *b, size_t lo, size_t hi)   /* equal outside [lo, hi) */
{
    return !memcmp(a, b, lo) && !memcmp((const char *) a + hi, (const char *) b + hi, sizeof *a - hi);
}
static void r7_note(const void *mesh, const TevKey *tk, const VsKey *vk, const TevUniforms *u, const VsUniforms *vu, RhiTexture *const tex[TEVK_TEX], uint32_t alpha, int depth)
{
    ++s_r7_c[0];
    const int adj = s_r7p.seq && s_r7p.seq + 1u == s_r7_seq;
    int a = 0, b = 0, c = 0, dd = 0;
    if (adj && s_r7p.alpha == alpha && s_r7p.depth == depth && !memcmp(s_r7p.vp, s_d.vp, sizeof s_r7p.vp) && !memcmp(s_r7p.sc, s_d.sc, sizeof s_r7p.sc) &&
        !memcmp(&s_r7p.tk, tk, sizeof *tk) && !memcmp(&s_r7p.vk, vk, sizeof *vk) && !memcmp(&s_r7p.u, u, sizeof *u) && !memcmp(s_r7p.tex, tex, sizeof s_r7p.tex)) {
        const size_t pm0 = offsetof(VsUniforms, pm), pm1 = offsetof(VsUniforms, mat);
        const size_t cl0 = offsetof(VsUniforms, pm), cl1 = offsetof(VsUniforms, tm);
        const size_t tx0 = offsetof(VsUniforms, pm), tx1 = sizeof(VsUniforms);
        if (s_r7p.mesh == mesh) {
            a = r7_eq_except(&s_r7p.vu, vu, pm0, pm1);
            /* B: pm/nm and tm/pt/tmx may differ; mat/amb/lt must match */
            b = a || (!memcmp(&s_r7p.vu, vu, pm0) && !memcmp(&s_r7p.vu.mat, &vu->mat, offsetof(VsUniforms, tm) - offsetof(VsUniforms, mat)));
            c = b || r7_eq_except(&s_r7p.vu, vu, tx0, tx1) || r7_eq_except(&s_r7p.vu, vu, cl0, cl1);
            (void) cl0;
        } else dd = 1;
    }
    s_r7_c[1] += a; s_r7_c[2] += b; s_r7_c[3] += c; s_r7_c[4] += dd;
    if (a) { if (++s_r7p.run > s_r7_c[6]) s_r7_c[6] = s_r7p.run; } else { s_r7p.run = 1; ++s_r7_c[5]; }
    s_r7p.seq = s_r7_seq; s_r7p.mesh = mesh; s_r7p.alpha = alpha; s_r7p.depth = depth; memcpy(s_r7p.vp, s_d.vp, sizeof s_r7p.vp); memcpy(s_r7p.sc, s_d.sc, sizeof s_r7p.sc);
    s_r7p.tk = *tk; s_r7p.vk = *vk; s_r7p.u = *u; s_r7p.vu = *vu; memcpy(s_r7p.tex, tex, sizeof s_r7p.tex);
}

/* ================================================================================================================================== R9 replay census (2026-10-27, docs/54)
 * How many display-list draws REPEAT FRAME TO FRAME?  That count caps a record-once / replay draw path (R3's 58 -> 25 us/call, never reached by
 * per-call memos).  A draw's match is the same mesh at the same occurrence (its n-th draw this frame) one present earlier.  Each issued drawTevGx
 * gets ONE class, the first that applies:
 *   NEW     no match in the previous frame (a new mesh, or more draws of it than last frame)
 *   MESH    the mesh's vertices were rewritten since its match (update_commit: skinned / morphed / CPU-animated)
 *   STATE   TEV / VS keys, textures, alpha, depth, viewport or scissor differ            -> a new record, not a replay
 *   COLOUR  same state; TevUniforms or VsUniforms mat / amb / lt differ                   \
 *   TEXMTX  same state + colours; tm / pt / tmx differ                                     > same program, streams, textures: replay + uniform refill
 *   MTX     only proj / pm / nm (and the skin palette) differ                              /
 *   SAME    bit-identical inputs                                                          -> verbatim replay
 * 64-bit FNV-1a hashes, not copies (a collision can only move a draw toward SAME, ~2^-64 per compare).  Coverage is published: draws past 3/4 of
 * the table in one frame are counted as DROPPED, never classified.  Instrument only: on when ux0:data/stairfax/gxm_r9.txt exists (read once);
 * off = one load and an untaken branch.  Content "2" = the BREAK arm: on frames whose (frame / 600) is odd, the state hash takes the frame number,
 * so every matched draw must read STATE there (watched failing: the census can report a change). */
#define R9_CAP 4096u
typedef struct { const void *mesh; uint32_t occ, upd, fr; uint64_t hs, hc, ht, hm; } R9E;
typedef struct { const void *mesh; uint32_t n, fr; } R9O;
static R9E *s_r9t;                   /* [2][R9_CAP]: this frame's table and the previous frame's, by frame parity; an entry is live iff its fr is that frame's tag */
static R9O *s_r9o;                   /* [R9_CAP]: draws of each mesh so far this frame */
static uint32_t s_r9_fr = 1, s_r9_used;
static int s_r9_on = -1, s_r9_brk;
static unsigned s_r9_c[12];          /* 0 issued, 1..7 NEW MESH STATE COLOUR TEXMTX MTX SAME, 8 dropped, 9 frames, 10 max classified in a frame, 11 frames in the break phase */
void rhi_gxm_r9_census(unsigned o[12]) { memcpy(o, s_r9_c, sizeof s_r9_c); }
static int r9_on(void)
{
    if (s_r9_on < 0) {
        SceUID fd = sceIoOpen("ux0:data/stairfax/gxm_r9.txt", SCE_O_RDONLY, 0);
        s_r9_on = fd >= 0;
        if (fd >= 0) { char c = 0; if (sceIoRead(fd, &c, 1) == 1 && c == '2') s_r9_brk = 1; sceIoClose(fd); }
        if (s_r9_on) { s_r9t = (R9E *) calloc(2u * R9_CAP, sizeof *s_r9t); s_r9o = (R9O *) calloc(R9_CAP, sizeof *s_r9o); if (!s_r9t || !s_r9o) s_r9_on = 0; }
        gxm_log("[gxm] R9 replay census %s (gxm_r9.txt %s%s)", s_r9_on ? "ON" : "OFF", fd >= 0 ? "present" : "absent", s_r9_brk ? ", BREAK arm" : "");
    }
    return s_r9_on;
}
void rhi_gxm_r9_frame(void)
{
    if (s_r9_on <= 0) return;
    if (s_r9_used > s_r9_c[10]) s_r9_c[10] = s_r9_used;
    s_r9_used = 0; ++s_r9_c[9];
    if (++s_r9_fr == 0) s_r9_fr = 1;
    if (s_r9_brk && (s_r9_fr / 600u) & 1u) ++s_r9_c[11];
}
static uint64_t r9_h(uint64_t h, const void *p, size_t n) { const uint8_t *b = (const uint8_t *) p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static uint32_t r9_slot(const void *mesh, uint32_t occ) { return (((uint32_t) (uintptr_t) mesh >> 4) * 2654435761u ^ occ * 40503u) & (R9_CAP - 1u); }
static void r9_note(const GxmMesh *m, const TevKey *tk, const VsKey *vk, const TevUniforms *u, const VsUniforms *vu, RhiTexture *const tex[TEVK_TEX],
                    uint32_t alpha, int depth, const float *skin_pal)
{
    ++s_r9_c[0];
    if (s_r9_used >= R9_CAP / 4u * 3u) { ++s_r9_c[8]; return; }
    const uint32_t fr = s_r9_fr, pf = fr - 1u;
    uint32_t occ = 0;
    for (uint32_t k = 0, i = r9_slot(m, 0); k < R9_CAP; ++k, i = (i + 1u) & (R9_CAP - 1u)) {
        R9O *o = &s_r9o[i];
        if (o->fr != fr) { o->fr = fr; o->mesh = m; o->n = 1; break; }
        if (o->mesh == m) { occ = o->n++; break; }
    }
    const size_t mat0 = offsetof(VsUniforms, mat), tm0 = offsetof(VsUniforms, tm);
    const int nt = tk->numTex < TEVK_TEX ? tk->numTex : TEVK_TEX;
    uint64_t hs = r9_h(0xcbf29ce484222325ull, tk, sizeof *tk);
    hs = r9_h(hs, vk, sizeof *vk); hs = r9_h(hs, tex, (size_t) nt * sizeof tex[0]);
    hs = r9_h(hs, &alpha, sizeof alpha); hs = r9_h(hs, &depth, sizeof depth); hs = r9_h(hs, s_d.vp, sizeof s_d.vp); hs = r9_h(hs, s_d.sc, sizeof s_d.sc);
    if (s_r9_brk && (fr / 600u) & 1u) hs = r9_h(hs, &fr, sizeof fr);
    uint64_t hc = r9_h(0xcbf29ce484222325ull, u, sizeof *u); hc = r9_h(hc, (const char *) vu + mat0, tm0 - mat0);
    const uint64_t ht = r9_h(0xcbf29ce484222325ull, (const char *) vu + tm0, sizeof *vu - tm0);
    uint64_t hm = r9_h(0xcbf29ce484222325ull, vu, mat0);
    if (skin_pal) hm = r9_h(hm, skin_pal, VS_SKIN_PAL_VEC4 * 4u * sizeof(float));
    const R9E *t = s_r9t + (pf & 1u) * R9_CAP, *p = NULL;
    for (uint32_t k = 0, i = r9_slot(m, occ); k < R9_CAP; ++k, i = (i + 1u) & (R9_CAP - 1u)) {
        if (t[i].fr != pf) break;
        if (t[i].mesh == m && t[i].occ == occ) { p = &t[i]; break; }
    }
    const int cls = !p ? 0 : p->upd != m->upd ? 1 : p->hs != hs ? 2 : p->hc != hc ? 3 : p->ht != ht ? 4 : p->hm != hm ? 5 : 6;
    ++s_r9_c[1 + cls];
    R9E *c = s_r9t + (fr & 1u) * R9_CAP;
    for (uint32_t k = 0, i = r9_slot(m, occ); k < R9_CAP; ++k, i = (i + 1u) & (R9_CAP - 1u))
        if (c[i].fr != fr) { c[i] = (R9E) { m, occ, m->upd, fr, hs, hc, ht, hm }; break; }
    ++s_r9_used;
}

static int run_draw(const Draw *d)
{
    ++s_r7_seq;   /* R7 census: any draw breaks a run of drawTevGx calls */
    if (gxm_target_trace_frame()) {
        const TevVertex *tv = d->lay == LAY_TEV ? (const TevVertex *) d->cpu : NULL;
        gxm_log("[gxm-trace] draw kind=%d hash=%08x fam=%d n=%u alpha=%08x depth=%d ntex=%d vp=%d,%d,%d,%d sc=%d,%d,%d,%d v0=%.1f,%.1f,%.1f c0=%08x mvp0=%.4f,%.4f,%.4f,%.4f mvp3=%.4f,%.4f,%.4f,%.4f",
                d->kind, (unsigned) d->prog->hash, d->prog->family, d->src == SRC_MESH ? d->nidx : d->count, (unsigned) d->alpha, d->depth, d->ntex,
                s_d.vp[0], s_d.vp[1], s_d.vp[2], s_d.vp[3], s_d.sc[0], s_d.sc[1], s_d.sc[2], s_d.sc[3],
                tv ? tv[0].x : 0.f, tv ? tv[0].y : 0.f, tv ? tv[0].z : 0.f, tv ? tv[0].c0 : 0u,
                d->mvp ? d->mvp[0] : 0.f, d->mvp ? d->mvp[1] : 0.f, d->mvp ? d->mvp[2] : 0.f, d->mvp ? d->mvp[3] : 0.f,
                d->mvp ? d->mvp[12] : 0.f, d->mvp ? d->mvp[13] : 0.f, d->mvp ? d->mvp[14] : 0.f, d->mvp ? d->mvp[15] : 0.f);
    }
    const int pk_ = d->src == SRC_IMM ? 0 : 1; (void) pt_on(); uint32_t pt0_ = PT_NOW();
    { extern void gxm_target_note_draw(void *const *tex, int n); extern unsigned gxm_target_cl_draw(void);
      gxm_target_note_draw((void *const *) (d->tex_id ? d->tex_id : d->tex), d->ntex);
      const unsigned dn = gxm_target_cl_draw();
      if (dn) gxm_log("[gxm-drawlog] %u kind %d src %d prog %08x fam %d alpha %08x depth %d n %u ntex %d t0 %dx%d%s t1 %dx%d%s vp %d,%d,%d,%d",
                      dn, d->kind, (int) d->src, (unsigned) d->prog->hash, d->prog->family, (unsigned) d->alpha, d->depth, d->src == SRC_MESH ? d->nidx : d->count, d->ntex,
                      d->ntex > 0 && d->tex[0] ? d->tex[0]->w : 0, d->ntex > 0 && d->tex[0] ? d->tex[0]->h : 0, d->ntex > 0 && d->tex[0] && d->tex[0]->copy ? "C" : "",
                      d->ntex > 1 && d->tex[1] ? d->tex[1]->w : 0, d->ntex > 1 && d->tex[1] ? d->tex[1]->h : 0, d->ntex > 1 && d->tex[1] && d->tex[1]->copy ? "C" : "",
                      s_d.vp[0], s_d.vp[1], s_d.vp[2], s_d.vp[3]);
      if (dn) { const TevKey *k = &d->prog->tk; int nind = 0, nds = 0; for (int s = 0; s < k->numStages && s < TEVK_STAGES; ++s) { if (k->ik.on && k->ik.st[s][0]) ++nind; if (k->st[s].texSlot != 0xff) ++nds; }
                gxm_log("[gxm-drawlog] %u   key: stages %d texstages %d numTex %d indirect %d (on %d) fog %d proj %02x acomp %d/%d/%d t2 %dx%d t3 %dx%d fmts %x %x %x %x", dn, k->numStages, nds, k->numTex, nind, k->ik.on,
                        k->fogType, k->projMask, k->acComp0, k->acOp, k->acComp1,
                        d->ntex > 2 && d->tex[2] ? d->tex[2]->w : 0, d->ntex > 2 && d->tex[2] ? d->tex[2]->h : 0, d->ntex > 3 && d->tex[3] ? d->tex[3]->w : 0, d->ntex > 3 && d->tex[3] ? d->tex[3]->h : 0,
                        d->ntex > 0 && d->tex[0] ? (unsigned) d->tex[0]->fmt : 0u, d->ntex > 1 && d->tex[1] ? (unsigned) d->tex[1]->fmt : 0u,
                        d->ntex > 2 && d->tex[2] ? (unsigned) d->tex[2]->fmt : 0u, d->ntex > 3 && d->tex[3] ? (unsigned) d->tex[3]->fmt : 0u); } }
    GxmProg *p = d->prog;
    SceGxmVertexProgram *vp = vp_get(d);
    PT_STEP(pk_, 1);
    if (!vp) return GXM_SK_COMPILE;
    SceGxmFragmentProgram *fp = fp_get(p, d->alpha);
    if (!fp) return GXM_SK_COMPILE;
    PT_STEP(pk_, 2);
    for (int i = 0; i < d->ntex; ++i) if (d->tex[i] && !d->tex[i]->mem.p) return GXM_SK_NOTEXMEM;
    if (s_d.sc_empty) return GXM_SK_SCISSOR_EMPTY;

    /* vertex source: how many vertices per chunk, and chunk 0 staged BEFORE the scene is opened (a ring failure must not leave an abandoned scene) */
    const uint32_t total = d->src == SRC_MESH ? d->nidx : d->count;
    uint32_t maxv = total;
    const uint8_t *vptr0 = NULL;
    if (d->src == SRC_IMM) {
        maxv = gxm_chunk_verts(gxm_ring_capacity(), d->stride);
        if (maxv < 3u) { ++s_d.ring_skips; return GXM_SK_RING; }
    } else if (d->src == SRC_STATIC) {
        maxv = 65535u;                                   /* 3 * 21845 */
    }
    if (d->src == SRC_IMM) {
        const uint32_t n0 = total < maxv ? total : maxv;
        /* the open scene may already hold the whole staging ring (ring.h cannot retire a scene that has not ended): end it, the draw then opens a new one */
        if (!gxm_ring_open_scene_fits(n0 * d->stride)) { gxm_target_scene_end('r'); ++s_d.ring_breaks; }
        void *r = gxm_ring_alloc(n0 * d->stride);
        if (!r) { ++s_d.ring_skips; return GXM_SK_RING; }
        memcpy(r, d->cpu, (size_t) n0 * d->stride);
        vptr0 = (const uint8_t *) r;
    }

    PT_STEP(pk_, 3);
    if (gxm_target_scene_m()) return GXM_SK_SCENE;       /* LAST before any sceGxm state call */
    PT_STEP(pk_, 4);

    /* R3A_GLUE_BEGIN */
    const unsigned r3a = gx_r3a;                         /* R3a: the cut switches, read ONCE per draw */
    const int c2 = (r3a & R3A_C2) != 0;
    if (c2) s_d.sh_active = 1; else if (s_d.sh_active) { gxm_sh_reset(&s_d.sh); s_d.sh_active = 0; }   /* invalidation (3): never trust a shadow written before the switch was off */
    apply_scene_state();
    {
        const GxmDepthRes dr = gxm_depth_resolve(d->depth, d->alpha, rhi_vita_force_nodepth);
#ifdef GXM_SELFCHECK
        assert(dr.bkey == gxm_blend_key(d->alpha));
#endif
        if (!c2 || gxm_sh_depth_need(&s_d.sh, (unsigned) (dr.func & 7), (unsigned) (dr.write != 0))) apply_depth(&dr);   /* R3a C2: the four depth calls only when the pair changed */
        else ++s_d.dep_skipped;
    }
    if (vp != s_d.cur_vp) { sceGxmSetVertexProgram(g_gx.ctx, vp); s_d.cur_vp = vp; }
    if (fp != s_d.cur_fp) { sceGxmSetFragmentProgram(g_gx.ctx, fp); s_d.cur_fp = fp; gxm_sh_reset_tex(&s_d.sh); }   /* invalidation (2): a new fragment program re-issues every texture */
    for (int s = 0; s < d->ntex && s < TEVK_TEX; ++s) {  /* AFTER the fragment program: Vita3K keeps texture state across a program change (gxmprobe round 3) */
        const int unit = p->texunit[s];
        if (unit < 0 || !d->tex[s]) continue;
        if (c2 && !gxm_sh_tex_need(&s_d.sh, (unsigned) unit, (const uint32_t *) &d->tex[s]->t)) { ++s_d.tex_skipped; continue; }   /* R3a C2: this unit already holds these control words */
        if (GXM_CHK(sceGxmSetFragmentTexture(g_gx.ctx, (unsigned) unit, &d->tex[s]->t))) return GXM_SK_NOTEXMEM;
        if (c2) gxm_sh_tex_set(&s_d.sh, (unsigned) unit, (const uint32_t *) &d->tex[s]->t);                               /* invalidation (4): only after the call succeeded */
    }
    /* R3A_GLUE_END */
    if (d->src == SRC_MESH && d->mesh->nslots) d->mesh->slot_seq[d->mesh->cur] = g_gx.nval + 1u;   /* the open scene reads this slot */
    PT_STEP(pk_, 5);

    for (uint32_t done = 0; done < total;) {
        uint32_t n = total - done; if (n > maxv) n = maxv;
        const void *vptr, *iptr;
        if (d->src == SRC_IMM) {
            if (done == 0) vptr = vptr0;
            else {
                void *r = gxm_ring_alloc(n * d->stride);
                if (!r) { ++s_d.ring_skips; return GXM_SK_RING; }
                memcpy(r, (const uint8_t *) d->cpu + (size_t) done * d->stride, (size_t) n * d->stride);
                vptr = r;
            }
            iptr = s_d.ident.p;
        } else if (d->src == SRC_STATIC) {
            vptr = d->gpu_vtx + (size_t) done * d->stride; iptr = s_d.ident.p;
        } else {
            vptr = d->gpu_vtx; iptr = d->gpu_idx;
        }
        void *vub = NULL, *fub = NULL;
        const int c1 = (r3a & R3A_C1) != 0 && p->ufast;                       /* R3a C1: write by offset... */
        const int c1_fast = c1 && p->ucheck >= UNI_VERIFY_N;                  /* ...once the program's first draws have been compared with the API */
        if (p->vub_bytes) {                              /* a zero-size default buffer: reserving it returns NULL (c2floor) */
            if (GXM_CHK(sceGxmReserveVertexDefaultUniformBuffer(g_gx.ctx, &vub)) || !vub) { ++s_d.ub_fail; return GXM_SK_UB; }
            if (c1_fast) fill_vub_off(p, d, vub);
            else if (fill_vub(p, d, vub)) { ++s_d.ub_fail; return GXM_SK_UB; }
        }
        if (p->fub_bytes) {
            if (GXM_CHK(sceGxmReserveFragmentDefaultUniformBuffer(g_gx.ctx, &fub)) || !fub) { ++s_d.ub_fail; return GXM_SK_UB; }
            if (c1_fast) fill_fub_off(p, d, fub);
            else if (fill_fub(p, d, fub)) { ++s_d.ub_fail; return GXM_SK_UB; }
        }
        if (c1_fast) ++s_d.uni_fast; else ++s_d.uni_ref;
        if (c1 && !c1_fast) {                           /* a draw that used the API: compare the offset writes with it (the first UNI_VERIFY_N per program) */
            const int v = uni_verify(p, d, vub, fub);
            GxmProg *pm = (GxmProg *) p;
            if (v == 0) { ++pm->ucheck; ++s_d.uni_verify_ok; }
            else {
                pm->ufast = 0; ++s_d.uni_verify_bad;
                if (s_d.uni_verify_bad <= 8u) gxm_log("[gxm-draw] uniform offsets %s for program %08x (%s): reverting this program to sceGxmSetUniformDataF", v < 0 ? "UNCHECKABLE (buffer > 16 KB)" : "DISAGREE with the API", (unsigned) p->hash, p->family == GXM_PK_GX ? "gx" : "tev");
            }
        }
        if (GXM_CHK(sceGxmSetVertexStream(g_gx.ctx, 0, vptr))) return GXM_SK_DRAWFAIL;
        if (g_gx.arm & GXM_ARM_NODRAW) ++s_d.arm_nodraw;     /* counted issued, never submitted: the picture gate must see a flat frame */
        else {
            if (gxm_sce_draw(SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, iptr, n)) return GXM_SK_DRAWFAIL;
            ++s_d.draw_calls;
        }
        done += n;
        if (done < total) ++s_d.chunk_splits;
    }
    PT_STEP(pk_, 6);
    pt_done(pk_);
    return -1;
}

/* ================================================================================================================================== R3c C6: immediate-draw merge (host/engine/R3C_CACHE.md s1)
 * The HAL draws a primitive that is not in a display list as one rhi_gxm_drawTev: ~267 a frame, most of them one small triangle batch, each paying a program lookup, a vertex / fragment program lookup, two uniform
 * reservations and fills, the vertex stream and a sceGxmDraw.  Consecutive calls that would issue IDENTICAL GPU state (same program, alpha word and depth state, same textures and control words, same uniform numbers, same
 * mvp, same vertex layout, triangle lists) are appended into ONE held run: one ring allocation, one run_draw, one sceGxmDraw with the summed vertex count.  The held run has touched nothing of the GXM context, and it is
 * issued -- by gxm_c6_flush() -- before ANYTHING else can:
 *   - every entry point of THIS module that is not drawTev or one of the two whose effect the run carries by value (setDepthState: d.depth; setColorTransform: the run copies the caller's mvp) calls c6_entry();
 *     the viewport / scissor / shadow-bias setters flush only when they change something (a run applies s_d.vp / sc / zb at ISSUE time);
 *   - every sceGxm call of the other modules that sets draw state, opens or ends a scene, queues the display or changes a texture's control words is wrapped in rhi_gxm_int.h (gxm_c6_hook / gxm_c6_hook_tex),
 *     so scene end, present, target switch, EFB copy, clear, texture upload, mesh create and frame end all find the run issued;
 *   - a run is held only while a scene is open on M (gxm_target_scene_m() at its creation), so the target module can never begin a scene with a run held;
 *   - a run also ends when the next draw differs, when it would exceed the ring's chunk limit, and when C6 is switched off.
 * Everything a run's draws would have read at issue time is captured by value at creation (alpha word, depth, textures' control words / memory / size, TevUniforms, mvp) or protected by the flush rules above.
 * ACCOUNTING: each call is accounted `issued` when it joins a run; if the run's issue fails with reason R, all of its calls are moved from issued to skip[R], so req == issued + sum(skip) holds at every instant.
 * The census counts runs also when C6 is off (the signature of the last immediate draw is remembered, the draw is issued as always): g_gxm_c6 below, printed as the [r3c] line by vita_tool_main.c. */
int g_gxm_c6_pending, g_gxm_c6_busy;
static unsigned s_c6_calls, s_c6_draws, s_c6_maxrun, s_c6_merged;   /* census, cumulative: immediate draw calls seen, draws they became (runs), the longest run, calls that joined a run */
/* GXM_MERGE_BEGIN */
typedef struct MergeRun {
    int valid, held;                                     /* valid: a run exists; held: its draws have NOT been issued (C6 on); 0 = only the signature of the last issued draw (the census) */
    const GxmProg *prog; uint32_t alpha; int depth, ntex;
    GxmTex *tex[TEVK_TEX]; uint32_t tw[TEVK_TEX][4]; void *tmem[TEVK_TEX]; int tdim[TEVK_TEX][2];   /* tex: the identities; tw / tmem / tdim: what they held when the run began */
    GxmTex texc[TEVK_TEX];                               /* ...and a COPY of each: the texture module frees a destroyed texture's struct at once, the run is issued later and reads the copy */
    TevUniforms u; float mvp[16];
    uint8_t *buf; uint32_t cap, count, stride, ncalls;   /* buf: the staged vertices (cap bytes), count vertices, calls in the run */
} MergeRun;
static MergeRun s_mr;
/* 1 = a draw with these parameters would issue exactly the state of the run (so its vertices may follow the run's) */
static int c6_same(const MergeRun *m, const GxmProg *p, uint32_t alpha, int depth, int ntex, GxmTex *const *tex, const TevUniforms *u, const float *mvp, uint32_t stride)
{
    if (!m->valid || m->prog != p || m->alpha != alpha || m->depth != depth || m->ntex != ntex || m->stride != stride) return 0;
    for (int t = 0; t < ntex; ++t) {
        if (m->tex[t] != tex[t]) return 0;
        if (tex[t] && (memcmp(m->tw[t], &tex[t]->t, sizeof m->tw[t]) || m->tmem[t] != tex[t]->mem.p || m->tdim[t][0] != tex[t]->w || m->tdim[t][1] != tex[t]->h)) return 0;
    }
    return !memcmp(&m->u, u, sizeof *u) && !memcmp(m->mvp, mvp, sizeof m->mvp);
}
static void c6_open(MergeRun *m, const GxmProg *p, uint32_t alpha, int depth, int ntex, GxmTex *const *tex, const TevUniforms *u, const float *mvp, uint32_t stride, int held)
{
    m->valid = 1; m->held = held; m->prog = p; m->alpha = alpha; m->depth = depth; m->ntex = ntex; m->stride = stride; m->count = 0; m->ncalls = 1;
    for (int t = 0; t < TEVK_TEX; ++t) {
        m->tex[t] = t < ntex ? tex[t] : NULL; m->tmem[t] = NULL; m->tdim[t][0] = m->tdim[t][1] = 0; memset(m->tw[t], 0, sizeof m->tw[t]);
        if (t < ntex && tex[t]) { memcpy(m->tw[t], &tex[t]->t, sizeof m->tw[t]); m->tmem[t] = tex[t]->mem.p; m->tdim[t][0] = tex[t]->w; m->tdim[t][1] = tex[t]->h; m->texc[t] = *tex[t]; }
    }
    m->u = *u; memcpy(m->mvp, mvp, sizeof m->mvp);
}
/* the vertices of one more call, after the run's; 0 = out of memory (the caller issues the call by itself) */
static int c6_stage(MergeRun *m, const void *verts, uint32_t n)
{
    const uint32_t need = (m->count + n) * m->stride;
    if (m->cap < need) {
        uint32_t nc = m->cap ? m->cap : 16384u; while (nc < need) nc *= 2u;
        uint8_t *nb = (uint8_t *) realloc(m->buf, nc);
        if (!nb) return 0;
        m->buf = nb; m->cap = nc;
    }
    memcpy(m->buf + (size_t) m->count * m->stride, verts, (size_t) n * m->stride);
    m->count += n;
    return 1;
}
/* GXM_MERGE_END */
static void c6_run_ended_n(uint32_t ncalls) { ++s_c6_draws; if (ncalls > s_c6_maxrun) s_c6_maxrun = ncalls; }
/* Issue the held run (if any) and end the run.  Safe to call from anywhere, any number of times. */
void gxm_c6_flush(void)
{
    MergeRun *m = &s_mr;
    if (!m->valid || g_gxm_c6_busy) return;
    c6_run_ended_n(m->ncalls);
    m->valid = 0; g_gxm_c6_pending = 0;
    if (!m->held) return;                                /* the census remembered a signature; its draws were issued one by one */
    m->held = 0;
    g_gxm_c6_busy = 1;                                   /* run_draw's own sceGxm calls pass the wrappers */
    Draw d; memset(&d, 0, sizeof d);
    d.kind = GXM_K_TEV; d.prog = (GxmProg *) m->prog; d.alpha = m->alpha; d.depth = m->depth; d.mvp = m->mvp; d.u = &m->u; d.ntex = m->ntex;
    for (int t = 0; t < m->ntex; ++t) d.tex[t] = m->tex[t] ? &m->texc[t] : NULL;
    d.tex_id = m->tex;
    d.lay = LAY_TEV; d.src = SRC_IMM; d.cpu = m->buf; d.count = m->count; d.stride = m->stride;
    const int why = gxm_dead() ? GXM_SK_DEAD : run_draw(&d);
    if (why >= 0) {                                      /* the run was not issued: its calls were optimistically accounted issued */
        const int w = why < GXM_SK_N ? why : GXM_SK_COMPILE;
        g_gx.st.issued[GXM_K_TEV] -= m->ncalls; g_gx.st.skip[GXM_K_TEV][w] += m->ncalls;
    }
    g_gxm_c6_busy = 0;
}
void gxm_c6_flush_tex(const SceGxmTexture *t)
{
    const MergeRun *m = &s_mr;
    if (!m->valid) return;
    for (int i = 0; i < m->ntex; ++i) if (m->tex[i] && &m->tex[i]->t == t) { gxm_c6_flush(); return; }
}
void rhi_gxm_c6_census(unsigned o[4]) { o[0] = s_c6_calls; o[1] = s_c6_draws; o[2] = s_c6_maxrun; o[3] = s_c6_merged; }
/* The R3c front end of rhi_gxm_drawTev: returns what run_draw would (-1 = issued, or held to be issued with its run). */
static int c6_tev(Draw *d, const TevUniforms *u, const TevVertex *verts, uint32_t count, const float *mvp)
{
    MergeRun *m = &s_mr;
    const int on = (r3a_now() & R3A_C6) != 0;
    const uint32_t stride = d->stride, maxv = gxm_chunk_verts(gxm_ring_capacity(), stride);
    ++s_c6_calls;
    const int census = !on || gxm_target_trace_frame();  /* C6 off (or a traced frame, which logs every draw): the census only */
    if (census && m->valid && m->held) gxm_c6_flush();
    const int same = c6_same(m, d->prog, d->alpha, d->depth, d->ntex, d->tex, u, mvp, stride) && m->count + count <= maxv;
    if (census) {                                        /* remember the signature, issue the draw as ever */
        if (same) { m->count += count; ++m->ncalls; ++s_c6_merged; }
        else { if (m->valid) gxm_c6_flush(); c6_open(m, d->prog, d->alpha, d->depth, d->ntex, d->tex, u, mvp, stride, 0); m->count = count; g_gxm_c6_pending = 1; }
        g_gxm_c6_busy = 1;                               /* the draw's own sceGxm calls must not end the run it belongs to */
        const int why = run_draw(d);
        g_gxm_c6_busy = 0;
        return why;
    }
    if (same && m->held) {                               /* one more call of the held run */
        if (c6_stage(m, verts, count)) { ++m->ncalls; ++s_c6_merged; return -1; }
        gxm_c6_flush();                                  /* out of memory: issue what is held, this call goes alone below */
    } else if (m->valid) gxm_c6_flush();
    /* a run starts here -- only if run_draw would get as far as the ring and the scene (else the eager path gives the very same answer) */
    if (maxv >= 3u && count <= maxv && vp_get(d) && fp_get(d->prog, d->alpha) && !s_d.sc_empty) {
        int texok = 1;
        for (int t = 0; t < d->ntex; ++t) if (d->tex[t] && !d->tex[t]->mem.p) texok = 0;
        if (texok) {
            if (gxm_target_scene_m()) return GXM_SK_SCENE;   /* a held run lives inside an open scene: nobody can begin another with it held */
            c6_open(m, d->prog, d->alpha, d->depth, d->ntex, d->tex, u, mvp, stride, 1);
            m->count = 0;
            if (c6_stage(m, verts, count)) { g_gxm_c6_pending = 1; return -1; }
            m->valid = 0;                                /* out of memory: not held */
        }
    }
    c6_run_ended_n(1);                                   /* an eager draw is a run of one */
    return run_draw(d);
}

/* ================================================================================================================================== immediate entry points */
static const TevUniforms s_bi_u_atest = { .acRef0 = 128 };   /* the synthesised builtin keys: a >= 128 (GXM_M2.md s6) */

static int run_ffp(int kind, RhiInstance *r, int bi, int lay, uint32_t alpha, const void *verts, uint32_t count, uint32_t stride, GxmTex *tex)
{
    int why = GXM_SK_NOPROG;
    GxmProg *p = gxm_shader_builtin(bi, &why);
    if (!p || !p->ok) return why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG;
    Draw d; memset(&d, 0, sizeof d);
    d.kind = kind; d.prog = p; d.alpha = alpha; d.depth = s_d.depth;
    d.mvp = g_gxm_inst.mvp; d.u = &s_bi_u_atest;
    d.lay = lay; d.src = SRC_IMM; d.cpu = verts; d.count = count; d.stride = stride;
    if (tex) { d.tex[0] = tex; d.ntex = 1; }
    (void) r;
    return run_draw(&d);
}
void gxm_op_setColorTransform(RhiInstance *r, const float m[16])
{
    (void) r;
    memcpy(g_gxm_inst.mvp, m, sizeof g_gxm_inst.mvp);                                  /* row-major, as the hal gives it (rhi.h:88) */
}
void gxm_op_drawColored(RhiInstance *r, const RhiColorVertex *v, uint32_t count)
{
    c6_entry();
    if (!v || count < 3) { refuse_pre(GXM_K_COL); return; }                            /* M1's own refusal (rhi_gxm.c:393) */
    count -= count % 3u;
    if (!admit(GXM_K_COL)) return;
    const uint32_t alpha = rhi_vita_colored_honors_alpha ? (uint32_t) r->alphaMode : (uint32_t) RHI_ALPHA_OPAQUE;   /* rhi_vita.c:709 */
    account(GXM_K_COL, run_ffp(GXM_K_COL, r, GXM_BI_COLORED, LAY_COL, alpha, v, count, (uint32_t) sizeof *v, NULL));
}
void gxm_op_drawTextured(RhiInstance *r, const RhiTexVertex *v, uint32_t count)
{
    RhiGxm *g = (RhiGxm *) r;
    c6_entry();
    if (!v || count < 3 || !g->cur_tex) { refuse_pre(GXM_K_TEX); return; }             /* M1's own refusal (rhi_gxm.c:420) */
    count -= count % 3u;
    if (!admit(GXM_K_TEX)) return;
    const uint32_t alpha = (uint32_t) r->alphaMode;
    const int bi = (alpha & 0xffu) == RHI_ALPHA_TEST ? GXM_BI_TEXTURED_ATEST : GXM_BI_TEXTURED;
    account(GXM_K_TEX, run_ffp(GXM_K_TEX, r, bi, LAY_TEX, alpha, v, count, (uint32_t) sizeof *v, (GxmTex *) g->cur_tex));
}

int rhi_gxm_drawTev(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16], int alphaMode)
{
    if (!k || !u || !tex || !mvp) { refuse_pre(GXM_K_TEV); return 1; }
    count -= count % 3u;
    if (!count) { refuse_pre(GXM_K_TEV); return 1; }                                   /* rhi_gxm.c:572-577: consumed */
    if (!verts) { refuse_pre(GXM_K_TEV); return 1; }
    if (k->numTex > TEVK_TEX) { refuse_pre(GXM_K_TEV); return 0; }
    for (int t = 0; t < k->numTex; ++t) if (!tex[t]) { refuse_pre(GXM_K_TEV); return 0; }   /* the missing texture: the hal falls back */
    if (!admit(GXM_K_TEV)) return 1;
    int why = GXM_SK_NOPROG;
    (void) pt_on(); uint32_t pt0_ = PT_NOW();
    GxmProg *p = gxm_shader_tev(k, &why);
    PT_STEP(0, 0);
    if (!p || !p->ok) {
        why = why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG;
        account(GXM_K_TEV, why);
        return why == GXM_SK_NOPROG ? 0 : 1;                                           /* NOPROG: the hal draws it another way */
    }
    Draw d; memset(&d, 0, sizeof d);
    d.kind = GXM_K_TEV; d.prog = p;
    d.alpha = (alphaMode == RHI_ALPHA_TEST) ? (uint32_t) RHI_ALPHA_OPAQUE : (uint32_t) alphaMode;   /* the alpha test is the program's own compare (tev_flush :1149) */
    d.depth = s_d.depth; d.mvp = mvp; d.u = u;
    d.ntex = k->numTex;
    for (int t = 0; t < k->numTex; ++t) d.tex[t] = (GxmTex *) tex[t];
    d.lay = LAY_TEV; d.src = SRC_IMM; d.cpu = verts; d.count = count; d.stride = (uint32_t) sizeof(TevVertex);
    if (r3a_now() & (R3A_C5 | R3A_C6 | R3A_C7)) account(GXM_K_TEV, c6_tev(&d, u, verts, count, mvp));   /* R3c: held with its run (C6), or issued as ever with the census */
    else { c6_entry(); account(GXM_K_TEV, run_draw(&d)); }               /* a run left over from before the switch went off is issued first */
    return 1;
}

/* ================================================================================================================================== static VBOs */
unsigned rhi_gxm_uploadStatic(const RhiTexVertex *v, uint32_t n)
{
    c6_entry();
    if (!s_d.inited || gxm_dead() || !v || !n || n > (1u << 26) / STATIC_STRIDE) return 0;
    unsigned id = 0;
    for (unsigned i = 0; i < s_d.st_cap; ++i) if (!s_d.st[i].used) { id = i + 1u; break; }
    if (!id) {
        const unsigned cap = s_d.st_cap ? s_d.st_cap * 2u : 16u;
        StaticVbo *ns = (StaticVbo *) realloc(s_d.st, cap * sizeof *ns);
        if (!ns) { ++s_d.static_fail; return 0; }
        memset(ns + s_d.st_cap, 0, (cap - s_d.st_cap) * sizeof *ns);
        id = s_d.st_cap + 1u; s_d.st = ns; s_d.st_cap = cap;
    }
    StaticVbo *s = &s_d.st[id - 1u];
    memset(s, 0, sizeof *s);
    if (gxm_blk_alloc(&s->blk, GXM_MEM_CDRAM, n * STATIC_STRIDE, 32) || !s->blk.p) { memset(s, 0, sizeof *s); ++s_d.static_fail; return 0; }
    memcpy(s->blk.p, v, (size_t) n * STATIC_STRIDE);
    s->n = n; s->used = 1; ++s_d.st_alive;
    return id;
}
void rhi_gxm_deleteStatic(unsigned id)
{
    c6_entry();
    if (!id) return;
    if (id > s_d.st_cap || !s_d.st[id - 1u].used) { ++s_d.static_bad_id; return; }
    StaticVbo *s = &s_d.st[id - 1u];
    blk_defer(&s->blk);                                                   /* a draw in flight may still read it (R7) */
    memset(s, 0, sizeof *s);
    --s_d.st_alive;
}
void rhi_gxm_drawStatic(RhiInstance *r, unsigned vbo, uint32_t first, uint32_t count)
{
    RhiGxm *g = (RhiGxm *) r;
    c6_entry();
    count -= count % 3u;
    if (!vbo || !count || !g->cur_tex) { refuse_pre(GXM_K_STATIC); return; }           /* rhi_gxm.c:603-607 */
    if (vbo > s_d.st_cap || !s_d.st[vbo - 1u].used || (uint64_t) first + count > s_d.st[vbo - 1u].n) { refuse_pre(GXM_K_STATIC); return; }   /* a stale id / range: never read past the block */
    if (!admit(GXM_K_STATIC)) return;
    const uint32_t alpha = (uint32_t) r->alphaMode;
    const int bi = (alpha & 0xffu) == RHI_ALPHA_TEST ? GXM_BI_TEXTURED_ATEST : GXM_BI_TEXTURED;
    int why = GXM_SK_NOPROG;
    GxmProg *p = gxm_shader_builtin(bi, &why);
    if (!p || !p->ok) { account(GXM_K_STATIC, why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG); return; }
    Draw d; memset(&d, 0, sizeof d);
    d.kind = GXM_K_STATIC; d.prog = p; d.alpha = alpha; d.depth = s_d.depth;
    d.mvp = g_gxm_inst.mvp; d.u = &s_bi_u_atest;
    d.tex[0] = (GxmTex *) g->cur_tex; d.ntex = 1;
    d.lay = LAY_TEX; d.src = SRC_STATIC; d.stride = STATIC_STRIDE; d.count = count;
    d.gpu_vtx = (const uint8_t *) s_d.st[vbo - 1u].blk.p + (size_t) first * STATIC_STRIDE;
    account(GXM_K_STATIC, run_draw(&d));
}

/* ================================================================================================================================== GX meshes */
static void mesh_log_bad(const char *where, int why, uint32_t stride, uint32_t vbytes, uint32_t nidx)
{
    ++s_d.mesh_bad;
    if (s_d.mesh_logged++ < 8) gxm_log("[gxm-draw] mesh_bad (%s): reason %d stride %u vbytes %u nidx %u", where, why, stride, vbytes, nidx);
}
void *rhi_gxm_gx_mesh_create(const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, const GxMeshLayout *lay)
{
    c6_entry();
    if (!s_d.inited || gxm_dead() || !verts || !lay || !vbytes || (nidx && !idx)) return NULL;
    uint32_t need = 0;
    int why = gxm_layout_ok(lay) ? 0 : 6;
    if (!why) why = gxm_mesh_check((uint32_t) lay->stride, vbytes, idx, nidx, &need);
    if (why) { mesh_log_bad("create", why, (uint32_t) lay->stride, vbytes, nidx); return NULL; }
    GxmMesh *m = (GxmMesh *) calloc(1, sizeof *m);
    if (!m) { ++s_d.mesh_alloc_fail; return NULL; }
    m->lay = *lay; m->nidx = nidx; m->vbytes = vbytes; m->need = need;
    if (gxm_blk_alloc(&m->vtx, GXM_MEM_CDRAM, vbytes, 32) || !m->vtx.p) { free(m); ++s_d.mesh_alloc_fail; return NULL; }
    if (gxm_blk_alloc(&m->idx, GXM_MEM_CDRAM, nidx ? nidx * 2u : 16u, 16) || !m->idx.p) {
        blk_now(&m->vtx); free(m); ++s_d.mesh_alloc_fail; return NULL;       /* never submitted: freed now */
    }
    memcpy(m->vtx.p, verts, vbytes);
    if (nidx) memcpy(m->idx.p, idx, (size_t) nidx * 2u);
    m->next = s_d.meshes; if (m->next) m->next->prev = m; s_d.meshes = m;
    ++s_d.mesh_alive; ++s_d.mesh_created;
    return m;
}
/* a slot big enough for `vbytes`: (re)allocates when the capacity is short. 0 ok. The old block is deferred-freed (R7). */
static int slot_reserve(GxmMesh *m, int i, uint32_t vbytes)
{
    if (m->slot[i].p && m->slot_cap[i] >= vbytes) return 0;
    GxmBlk nb; memset(&nb, 0, sizeof nb);
    if (gxm_blk_alloc(&nb, GXM_MEM_LPDDR, vbytes, 32) || !nb.p) return -1;
    if (m->slot[i].p) blk_defer(&m->slot[i]);
    m->slot[i] = nb; m->slot_cap[i] = vbytes; m->slot_seq[i] = 0; m->slot_img[i] = 0; m->slot_vb[i] = 0;   /* R6 C9: a new block holds no image */
    return 0;
}
/* R6 C9 (host/engine/R6_SKIN.md s2): the update in two halves.  _begin does everything the update did up to the copy (the same slot choice, waits, growth, refusals and counters) and returns the slot's CPU pointer, or NULL
 * (*fresh 0 = refused, counted; *fresh -1 = this is not the GXM backend: the caller takes the vitaGL path).  *fresh on return = 1 when the slot's bytes are not a full image of this mesh at `vbytes` (new, reallocated, never
 * fully written, or written at another size): the caller must write the whole image.  *fresh on ENTRY nonzero = the caller changed bytes outside what it will write (a non-POS/NRM attribute): every slot's image is stale. */
void *rhi_gxm_gx_mesh_update_begin_real(void *mesh, uint32_t vbytes, int *fresh)
{
    c6_entry();
    GxmMesh *m = (GxmMesh *) mesh;
    const int inval = fresh && *fresh;
    if (fresh) *fresh = 0;
    if (!s_d.inited) { if (fresh) *fresh = -1; return NULL; }
    if (!m || m->bad || gxm_dead()) return NULL;
    if (inval) for (int i = 0; i < MESH_MAX_SLOTS; ++i) m->slot_img[i] = 0;
    if (!vbytes || vbytes % (uint32_t) m->lay.stride || vbytes / (uint32_t) m->lay.stride < m->need) {   /* vbytes may change (rhi_vita_tev.inc:1402): re-validate */
        m->bad = 1; mesh_log_bad("update", 4, (uint32_t) m->lay.stride, vbytes, m->nidx); return NULL;
    }
    int c;
    if (!m->nslots) {                                    /* the first update: three slots, the CDRAM copy retires */
        int i = 0;
        for (; i < MESH_FIRST_SLOTS; ++i) if (slot_reserve(m, i, vbytes)) break;
        if (i < MESH_FIRST_SLOTS) {                      /* out of memory: keep what the mesh had */
            for (int j = 0; j < i; ++j) { blk_now(&m->slot[j]); m->slot_cap[j] = 0; }   /* never submitted */
            ++s_d.mesh_alloc_fail; ++s_d.mesh_update_refused; return NULL;
        }
        m->nslots = MESH_FIRST_SLOTS;
        blk_defer(&m->vtx);
        c = 0;
    } else {
        c = (m->cur + 1) % m->nslots;
        const uint32_t sq = m->slot_seq[c];
        if (sq > g_gx.nval) {                            /* last read by the OPEN scene: no fence to wait for - grow, else refuse (counted) */
            if (m->nslots >= MESH_MAX_SLOTS) { ++s_d.mesh_update_refused; return NULL; }
            c = m->nslots;
            if (slot_reserve(m, c, vbytes)) { ++s_d.mesh_alloc_fail; ++s_d.mesh_update_refused; return NULL; }
            ++m->nslots; ++s_d.mesh_slot_grow;
        } else if (sq) {
            if (g_gx.fence_finish || !g_gx.fnot || (int) (*g_gx.fnot - sq) < 0) {   /* not yet reached: a bounded wait (R3), counted */
                ++s_d.mesh_slot_waits;
                if (gxm_frame_wait_scene(sq, GXM_FENCE_DEADLINE_US)) { ++s_d.mesh_update_refused; return NULL; }
            }
        }
        if (slot_reserve(m, c, vbytes)) { ++s_d.mesh_alloc_fail; ++s_d.mesh_update_refused; return NULL; }
    }
    m->pend_c = c; m->pend_vb = vbytes;
    if (fresh) *fresh = (!m->slot_img[c] || m->slot_vb[c] != vbytes) ? 1 : 0;
    return m->slot[c].p;
}
/* R6 C9: the slot update_begin returned now holds the mesh's current vertices (a full image, or the previous full image with POS / NRM rewritten): exactly the old update's tail. */
void rhi_gxm_gx_mesh_update_commit_real(void *mesh)
{
    GxmMesh *m = (GxmMesh *) mesh;
    if (!m || m->bad || !s_d.inited || gxm_dead()) return;
    const int c = m->pend_c;
    m->slot_seq[c] = 0;
    m->slot_img[c] = 1; m->slot_vb[c] = m->pend_vb;
    m->cur = c; m->vbytes = m->pend_vb;
    ++m->upd;
    ++s_d.mesh_updated;
}
#ifdef GXM_HOST_TEST   /* host/tests/gxm_merge_test.c only: what the mesh slots hold, and a way to say an open scene reads them (compiled out of every device build) */
const uint8_t *rhi_gxm_test_mesh_cur(void *mesh, uint32_t *vb) { GxmMesh *m = (GxmMesh *) mesh; if (vb) *vb = m->vbytes; return m->nslots ? (const uint8_t *) m->slot[m->cur].p : (const uint8_t *) m->vtx.p; }
void rhi_gxm_test_mesh_open(void *mesh) { GxmMesh *m = (GxmMesh *) mesh; for (int i = 0; i < m->nslots; ++i) m->slot_seq[i] = g_gx.nval + 1u; }
int rhi_gxm_test_mesh_nslots(void *mesh) { return ((GxmMesh *) mesh)->nslots; }
#endif
void rhi_gxm_gx_mesh_update(void *mesh, const void *verts, uint32_t vbytes)
{
    if (!verts) return;                                  /* (update_begin issues a held run first) */
    int fresh = 0;
    void *dst = rhi_gxm_gx_mesh_update_begin_real(mesh, vbytes, &fresh);
    if (!dst) return;
    memcpy(dst, verts, vbytes);
    rhi_gxm_gx_mesh_update_commit_real(mesh);
}
/* R5 (R5_SPLIT.md D10): the HAL reaches the two-half update through these two names (weak symbols in gx_draw.c).  A split-capable process routes them to the front (gx_rt_split.c), which hands out a scratch image and
 * records the POS/NRM block; R-back runs the _real halves above.  Otherwise: exactly the real ones. */
void *rhi_gxm_gx_mesh_update_begin(void *mesh, uint32_t vbytes, int *fresh) { if (g_gxsp_cap) return g_gxsp_api->meshBegin(mesh, vbytes, fresh); return rhi_gxm_gx_mesh_update_begin_real(mesh, vbytes, fresh); }
void rhi_gxm_gx_mesh_update_commit(void *mesh) { if (g_gxsp_cap) { g_gxsp_api->meshCommit(mesh); return; } rhi_gxm_gx_mesh_update_commit_real(mesh); }
void rhi_gxm_gx_mesh_destroy(void *mesh)
{
    c6_entry();
    GxmMesh *m = (GxmMesh *) mesh;
    if (!m) return;
    vp_forget(m);
    if (m->prev) m->prev->next = m->next; else s_d.meshes = m->next;
    if (m->next) m->next->prev = m->prev;
    blk_defer(&m->vtx);                      /* all of it deferred: a draw in flight may read any block */
    blk_defer(&m->idx);
    for (int i = 0; i < MESH_MAX_SLOTS; ++i) blk_defer(&m->slot[i]);
    free(m);
    if (s_d.mesh_alive) --s_d.mesh_alive;
    ++s_d.mesh_destroyed;
}
/* R5 (D6): the body takes the hint as an argument.  The split's back passes the one the FRONT read and cleared when the draw was recorded; nothing else reads gx_prog_hint on that thread. */
int rhi_gxm_drawTevGx_h(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode, unsigned long long hint)
{
    GxmMesh *m = (GxmMesh *) mesh;
    c6_entry();
    if (!m || !m->nidx || !tk || !u || !tex || !vk || !vu) { refuse_pre(GXM_K_TEVGX); return 0; }   /* rhi_gxm.c:622 */
    if (m->bad) { refuse_pre(GXM_K_TEVGX); return 0; }                                 /* a mesh marked bad is never drawn */
    const GxMeshLayout *L_ = &m->lay;
    if ((vk->hasNrm && L_->offNrm < 0) || (vk->hasClr0 && L_->offClr < 0) || (vk->hasMtxIdx && L_->offMtx < 0)) { refuse_pre(GXM_K_TEVGX); return 0; }
    for (int n = 0; n < 8; ++n) if ((vk->texMask >> n & 1) && L_->offTex[n] < 0) { refuse_pre(GXM_K_TEVGX); return 0; }
    if ((vk->hasBin && L_->offBin < 0) || (vk->hasTan && L_->offTan < 0)) { refuse_pre(GXM_K_TEVGX); return 0; }
    { const unsigned tg = tev_vs_tmi_groups(vk); if (((tg & 1) && L_->offTmi[0] < 0) || ((tg & 2) && L_->offTmi[1] < 0)) { refuse_pre(GXM_K_TEVGX); return 0; } }
    const float *skin_pal = NULL;   /* R8: a skinned key needs the mesh's aSk and a bound palette (HAL: gx_skin_vs_palette) -- else the HAL skips the draw, it never draws the model unskinned */
    if (vk->hasSkin) { skin_pal = gx_skin_vs_palette(); if (!skin_pal || L_->offSk < 0) { refuse_pre(GXM_K_TEVGX); return 0; } }
    if (tk->numTex > TEVK_TEX) { refuse_pre(GXM_K_TEVGX); return 0; }
    for (int t = 0; t < tk->numTex; ++t) if (!tex[t]) { refuse_pre(GXM_K_TEVGX); return 0; }
    if (!admit(GXM_K_TEVGX)) return 1;
    int why = GXM_SK_NOPROG;
    (void) pt_on(); uint32_t pt0_ = PT_NOW();
    GxmProg *p = gxm_shader_gx_hint(vk, tk, (r3a_now() & (R3A_C3 | R3A_C5)) ? hint : 0ull, &why);   /* a C3 hint (bit 63) or a C5 key id (bit 62): either names the raw key pair */
    PT_STEP(1, 0);
    if (!p || !p->ok) {
        why = why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG;
        account(GXM_K_TEVGX, why);
        return why == GXM_SK_NOPROG ? 0 : 1;
    }
    Draw d; memset(&d, 0, sizeof d);
    d.kind = GXM_K_TEVGX; d.prog = p;
    d.alpha = (alphaMode == RHI_ALPHA_TEST) ? (uint32_t) RHI_ALPHA_OPAQUE : (uint32_t) alphaMode;   /* rhi_vita_tev.inc:1638 */
    d.depth = s_d.depth; d.vu = vu; d.vk = vk; d.u = u; d.skin_pal = skin_pal;
    d.ntex = tk->numTex;
    for (int t = 0; t < tk->numTex; ++t) d.tex[t] = (GxmTex *) tex[t];
    d.lay = LAY_MESH; d.mesh = m; d.src = SRC_MESH; d.stride = (uint32_t) L_->stride; d.nidx = m->nidx;
    d.gpu_vtx = (const uint8_t *) (m->nslots ? m->slot[m->cur].p : m->vtx.p);
    d.gpu_idx = m->idx.p;
    { const int why_ = run_draw(&d); account(GXM_K_TEVGX, why_);
      if (why_ < 0 && r7_on()) r7_note(m, tk, vk, u, vu, tex, d.alpha, d.depth);
      if (why_ < 0 && r9_on()) r9_note(m, tk, vk, u, vu, tex, d.alpha, d.depth, skin_pal); }
    return 1;
}
int rhi_gxm_drawTevGx(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode)
{
    const unsigned long long hint = gx_prog_hint; gx_prog_hint = 0ull;   /* R3a C3: the HAL's key serials for THIS call (one use; cleared at once, so a refused draw leaves none behind) */
    return rhi_gxm_drawTevGx_h(tk, u, tex, vk, vu, mesh, alphaMode, hint);
}

/* R5 (R5_SPLIT.md D4): would the draw come back 0 because no program exists for the key?  Asked by the split's front ONCE per key (the back answers; the front remembers).  The same lookup the draw makes, so the
 * answer is the draw's own; it creates the program, which the draw would have done a moment later. */
int rhi_gxm_verdict_gx(const VsKey *vk, const TevKey *tk)
{
    int why = GXM_SK_NOPROG;
    GxmProg *p = gxm_shader_gx_hint(vk, tk, 0ull, &why);
    if (!p || !p->ok) { why = why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG; return why == GXM_SK_NOPROG ? 0 : 1; }
    return 1;
}
int rhi_gxm_verdict_tev(const TevKey *tk)
{
    int why = GXM_SK_NOPROG;
    GxmProg *p = gxm_shader_tev(tk, &why);
    if (!p || !p->ok) { why = why > 0 && why < GXM_SK_N ? why : GXM_SK_NOPROG; return why == GXM_SK_NOPROG ? 0 : 1; }
    return 1;
}
/* R5: the pure rules (GXM_PURE_BEGIN..END above) for the split's front, which answers a mesh create / update without the backend */
int rhi_gxm_pure_layout_ok(const GxMeshLayout *L) { return gxm_layout_ok(L); }
int rhi_gxm_pure_mesh_check(uint32_t stride, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, uint32_t *need) { return gxm_mesh_check(stride, vbytes, idx, nidx, need); }

/* ================================================================================================================================== init / shutdown / report */
int gxm_draw_init(void)
{
    memset(&s_d, 0, sizeof s_d);
    free(s_mr.buf); memset(&s_mr, 0, sizeof s_mr); g_gxm_c6_pending = g_gxm_c6_busy = 0;   /* R3c C6 */
    memset(s_vpm, 0, sizeof s_vpm); memset(s_fpm, 0, sizeof s_fpm); s_vpm_next = s_fpm_next = 0;
    s_d.depth = -1;
    s_d.zb_f = ZB_DEFAULT_FACTOR; s_d.zb_u = ZB_DEFAULT_UNITS;
    zbias_load();
    if (g_gx.rw <= 0 || g_gx.rh <= 0 || g_gx.vh <= 0) { gxm_log("[gxm-draw] init: the target geometry is not set (rw %d vh %d rh %d)", g_gx.rw, g_gx.vh, g_gx.rh); return -1; }
    s_d.vp[0] = 0; s_d.vp[1] = g_gx.rh - g_gx.vh; s_d.vp[2] = g_gx.rw; s_d.vp[3] = g_gx.vh;      /* the whole picture (rhi_vita_efb.inc:58-63) */
    s_d.sc[0] = 0; s_d.sc[1] = 0; s_d.sc[2] = g_gx.rw; s_d.sc[3] = g_gx.rh;                      /* all of M */
    compute_clip();
    s_d.applied_epoch = g_gx.epoch - 1u;                                                          /* force the first draw to apply everything */
    if (gxm_blk_alloc(&s_d.ident, GXM_MEM_LPDDR, 65536u * 2u, 16) || !s_d.ident.p) { gxm_log("[gxm-draw] init: the identity index buffer did not fit"); return -1; }
    { uint16_t *ix = (uint16_t *) s_d.ident.p; for (uint32_t i = 0; i < 65536u; ++i) ix[i] = (uint16_t) i; }
    s_d.inited = 1;
    gxm_log("[gxm-draw] init OK: viewport %d,%d %dx%d clip %d,%d..%d,%d chunk_verts(ring %u, stride 84) %u", s_d.vp[0], s_d.vp[1], s_d.vp[2], s_d.vp[3],
            s_d.clip[0], s_d.clip[1], s_d.clip[2], s_d.clip[3], gxm_ring_capacity(), gxm_chunk_verts(gxm_ring_capacity(), 84u));
    return 0;
}
void gxm_draw_shutdown(void)
{
    if (!s_d.inited) return;
    c6_entry();                                          /* R3c C6: a held run is issued (or, with the backend dead, counted skipped) before anything is freed */
    free(s_mr.buf); s_mr.buf = NULL; s_mr.cap = 0;
    const int gpu_may_read = gxm_dead();                 /* an aborted backend frees nothing the GPU may still read (R3) */
    unsigned meshes_left = 0, statics_left = 0;
    while (s_d.meshes) {
        GxmMesh *m = s_d.meshes; s_d.meshes = m->next;
        if (!gpu_may_read) {
            blk_now(&m->vtx); blk_now(&m->idx);
            for (int i = 0; i < MESH_MAX_SLOTS; ++i) blk_now(&m->slot[i]);
        }
        free(m); ++meshes_left;
    }
    for (unsigned i = 0; i < s_d.st_cap; ++i) if (s_d.st[i].used) { if (!gpu_may_read) blk_now(&s_d.st[i].blk); ++statics_left; }
    free(s_d.st); s_d.st = NULL; s_d.st_cap = 0;
    if (!gpu_may_read) blk_now(&s_d.ident);
    gxm_log("[gxm-draw] shutdown: meshes freed at exit %u, static VBOs freed at exit %u%s", meshes_left, statics_left, gpu_may_read ? " (backend dead: GPU blocks left alone)" : "");
    memset(s_vpm, 0, sizeof s_vpm); memset(s_fpm, 0, sizeof s_fpm);
    s_d.mesh_alive = 0; s_d.st_alive = 0; s_d.inited = 0;
}
int gxm_draw_report(char *buf, int cap)
{
    unsigned viol = 0;
    for (int k = 0; k < GXM_K_N; ++k) if (g_gx.st.req[k] != g_gx.st.issued[k] + skip_total(k)) ++viol;
    s_d.acct_viol = viol;
#ifdef GXM_SELFCHECK
    if (!(g_gx.arm & GXM_ARM_SKIPDRAW)) assert(viol == 0);   /* the by-construction identity; the SKIPDRAW break arm violates it on purpose */
#endif
    if (cap <= 0) return 0;
    const int n = snprintf(buf, (size_t) cap,
        "ring_breaks=%u draw_calls=%u chunk_splits=%u mesh_bad=%u mesh_slot_waits=%u static_alive=%u ring_skips=%u state_reapplies=%u "
        "mesh_alive=%u mesh_slot_grow=%u mesh_update_refused=%u mesh_alloc_fail=%u static_fail=%u static_bad_id=%u ub_fail=%u "
        "vp_changes=%u sc_changes=%u sc_empty_n=%u vpm_miss=%u fpm_miss=%u arm_dropped=%u arm_nodraw=%u draw_acct_viol=%u mesh_create=%u mesh_update=%u mesh_destroy=%u state_calls=%u dep_skipped=%u tex_skipped=%u sh_resets=%u uni_fast=%u uni_ref=%u uni_verify_ok=%u uni_verify_bad=%u ",
        s_d.ring_breaks, s_d.draw_calls, s_d.chunk_splits, s_d.mesh_bad, s_d.mesh_slot_waits, s_d.st_alive, s_d.ring_skips, s_d.state_reapplies,
        s_d.mesh_alive, s_d.mesh_slot_grow, s_d.mesh_update_refused, s_d.mesh_alloc_fail, s_d.static_fail, s_d.static_bad_id, s_d.ub_fail,
        s_d.vp_changes, s_d.sc_changes, s_d.sc_empty_n, s_d.vpm_miss, s_d.fpm_miss, s_d.arm_dropped, s_d.arm_nodraw, s_d.acct_viol, s_d.mesh_created, s_d.mesh_updated, s_d.mesh_destroyed, s_d.state_calls,
        s_d.dep_skipped, s_d.tex_skipped, s_d.sh_resets, s_d.uni_fast, s_d.uni_ref, s_d.uni_verify_ok, s_d.uni_verify_bad);
    if (n < 0) return 0;
    return n < cap ? n : cap - 1;
}
