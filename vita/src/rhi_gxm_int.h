/* rhi_gxm_int.h - the INTERNAL contract of the native GXM backend, milestone M2 (docs: host/engine/GXM_M2.md - read it first).
 *
 * WHAT THIS FILE IS. M1 (rhi_gxm.c) is a skeleton that counts draws. M2 renders every draw the vitaGL path renders, through raw GXM, and is split
 * into FIVE MODULES that five builders write IN PARALLEL, each owning exactly ONE new .c file and editing nothing else:
 *
 *     key      file                   owns
 *     ------   --------------------   -------------------------------------------------------------------------------------------------------
 *     frame    rhi_gxm_frame.c        GXM init/teardown ORDER, context + rings + patcher memory, the GPU memory allocator, the per-frame vertex
 *                                     staging ring, scene fences (notifications) + deferred destruction, the display callback, the periodic
 *                                     counters lines, the abort policy, gx_report. Defines g_gx.
 *     shader   rhi_gxm_shader.c       GxmProg get-or-create (TEV, GX pair, builtin FFP-equivalents, util programs): pack GXP load, shark compile,
 *                                     patcher register, vertex program per attribute layout, fragment program per blend, uniform/sampler name maps
 *     texture  rhi_gxm_texture.c      GxmTex: create/destroy, wrap/filter/lod bias, authored + generated mips, copy-target textures + their
 *                                     render targets, copy-texture sampler state
 *     draw     rhi_gxm_draw.c         drawTev / drawTevGx / drawTextured / drawColored / drawStatic, meshes, static VBOs, per-draw state (depth,
 *                                     blend, alpha, cull, viewport, scissor, shadow z-bias), uniform-buffer fill, vertex staging into the ring
 *     target   rhi_gxm_target.c       M (colour + depth), scenes (begin/end, render-target rotation), EFB copy (GPU quad), clearRect, clear, readEfbRect,
 *                                     peekZ/efbMode/efbZMode, display buffers, present + display queue, the display blit, the picture SHOT
 *
 * rhi_gxm.c stays (the INTEGRATOR trims it to a shell): backend selection, g_stfx_backend, the RhiOps table, rhi_gxm_create/shutdown that call
 * the frame module. No builder edits rhi_gxm.c, this header, closure.py, rhi_vita*.{c,inc} or any file of another module.
 *
 * THE RULES (each is a bug that has already cost a run):
 *  R1 No identifier starting with gl or vgl (glFoo, vglFoo) in any rhi_gxm_*.c (host/engine/gxm_dispatch_check.py R4; the integrator widens it to every rhi_gxm_*.c). vitaGL and raw GXM
 *     cannot share a process. Use sceGxm* only (sceGxmTextureInitLinear, not vglInitLinearTexture).
 *  R2 EVERY SCE return is checked: GXM_CHK(call) for anything that can fail, a plain call only for the void functions of sceGxm.
 *  R3 NO UNBOUNDED WAIT. The only waits are gxm_frame_wait_scene (bounded poll, deadline) and, if the smoke test selected it, sceGxmFinish inside
 *     frame. A timeout is gxm_abort(): the backend goes dead and NOTHING is torn down that the GPU may still read.
 *  R4 Every sceGxmDraw goes through gxm_sce_draw() (below) so the accounting identity sce_draw_ok == issued + internal_quads can be checked.
 *  R5 Every draw ENTRY POINT counts exactly once: refused_pre (malformed, M1's own refusals - not a request) OR req followed by exactly one of
 *     issued / skip[reason]. A silent return after req++ is the bug the accounting gate exists to catch.
 *  R6 A scene is opened ONLY by gxm_target_scene_m() (draw) or by the target module itself; sceGxmEndScene is called ONLY by gxm_frame_end_scene().
 *  R7 GPU memory a draw in flight may read is NEVER freed or overwritten directly: gxm_blk_free_deferred / gxm_frame_defer, ring memory via
 *     gxm_ring_alloc only. Freed when the FRAGMENT notification of the scene that may still read it has been reached (hardware-verified in c2floor;
 *     the vertex notification is not relied on anywhere).
 *  R8 Counted, not hidden: anything not done is a counter in a report line and a row in GXM_M2.md "Deferred". Nothing returns a constant that a
 *     checker could take for a result.
 * Emulator numbers are never timing claims. Nothing here has run on the console.
 */
#ifndef STFX_RHI_GXM_INT_H
#define STFX_RHI_GXM_INT_H

#include "stfx_backend.h"
#include "port/renderer/rhi.h"
#include "port/renderer/rhi_internal.h"
#include "port/gx_tev_key.h"
#include <psp2/gxm.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================================================================== constants */
#define GXM_DISP_W 960
#define GXM_DISP_H 544
#define GXM_DISP_BUFS 3
#define GXM_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define GXM_UB_MAX 56                 /* uniform parameters bound per stage (c2floor UB_MAX) */
#define GXM_PROG_MAX 1536             /* GxmProg alive at once (TEV + GX + builtin + util); a full table refuses with GXM_SK_PROGFULL */
#define GXM_M_RT_SCENES 8             /* "GXM's maximum" scenesPerFrame of an M render target (rhi_vita_efb.inc:366-378) */
#define GXM_M_RT_N 2                  /* M render targets used in rotation: 16 scenes per frame before rt_overflow counts (ch.2 has 11) */
#define GXM_COPY_RT_SCENES 2          /* scenesPerFrame of a copy texture's render target (vitaGL made it 1) */
#define GXM_FENCE_DEADLINE_US 2000000u
#define GXM_MAX_GPU_ATTRS 16

/* ================================================================================================================================== accounting */
enum { GXM_K_TEV = 0, GXM_K_TEVGX, GXM_K_COL, GXM_K_TEX, GXM_K_STATIC, GXM_K_N };            /* the five draw entry points, in the order of M1's "by kind" */
enum { GXM_SK_NOPROG = 0,     /* no program for this key: not in the pack (emulator: expected, counted) or the compiler is unavailable / refused it */
       GXM_SK_PROGFULL,       /* GxmProg table or patcher memory full */
       GXM_SK_COMPILE,        /* shark compile / patcher register / vertex- or fragment-program creation FAILED (a defect, not a missing input) */
       GXM_SK_NOTEXMEM,       /* a bound texture has no GPU memory (its allocation failed earlier) */
       GXM_SK_SCENE,          /* BeginScene failed / dead */
       GXM_SK_RING,           /* vertex staging does not fit the ring */
       GXM_SK_UB,             /* sceGxmReserve*DefaultUniformBuffer returned NULL or a uniform set failed */
       GXM_SK_SCISSOR_EMPTY,  /* the scissor box is empty: nothing can be drawn (vitaGL: region clip of zero area) */
       GXM_SK_DRAWFAIL,       /* sceGxmDraw returned non-zero */
       GXM_SK_DEAD,           /* the backend is dead (gxm_abort) */
       GXM_SK_N };

typedef struct GxmStats {
    unsigned req[GXM_K_N];                 /* draw module: well-formed requests (R5) */
    unsigned issued[GXM_K_N];              /* draw module: sceGxmDraw returned 0 for at least the whole request */
    unsigned skip[GXM_K_N][GXM_SK_N];      /* draw module: counted skips by reason */
    unsigned refused_pre[GXM_K_N];         /* draw module: refused before it was a request (null/short input, missing texture, mesh lacks an attribute) */
    unsigned sce_draw_ok, sce_draw_fail;   /* gxm_sce_draw(): ALL sceGxmDraw calls of the process */
    unsigned internal_quads;               /* target module: sceGxmDraw calls it made itself that are not game draws (clear, copy, composite, fence quads) */
    unsigned scenes, scenes_m, scenes_copy, scenes_display, fence_quads;   /* target module */
    unsigned rt_overflow;                  /* target module: a render target was used for more scenes in one frame than it was created for */
    unsigned rt_scenes_max;                /* target module: the most scenes one render target served in one frame */
} GxmStats;

/* Arm bits (ux0:data/stairfax/gxm_arm.txt, whitespace separated words; read once by frame; every arm is a BREAK arm the checker must FAIL) */
#define GXM_ARM_NOPRESENT 0x01u   /* target: the display-queue add is skipped (M1's arm) */
#define GXM_ARM_SKIPDRAW  0x02u   /* draw:   every 7th draw is dropped WITHOUT being counted (accounting gate) */
#define GXM_ARM_NODRAW    0x04u   /* draw:   no game draw reaches sceGxmDraw but all are counted issued (picture gate; the accounting identity then FAILS too) */
#define GXM_ARM_NOBLIT    0x08u   /* target: the display scene clears but never draws M (picture gate) */
#define GXM_ARM_NOPROG    0x10u   /* shader: the pack and the compiler are treated as absent (coverage gate) */
#define GXM_ARM_RINGBREAK 0x20u   /* frame:  ring.h skip_wait=1 and a 64 KB ring (ring gate) */
#define GXM_ARM_RT1       0x40u   /* target: M render targets made with scenesPerFrame 1 and one of them (render-target gate) */

typedef struct GxmCore {
    /* ---- frame module writes; everyone reads ---- */
    int up;                    /* init finished */
    int dead;                  /* gxm_abort() was called: every entry point refuses cheaply, nothing is freed that the GPU may read */
    int torn_down;
    char dead_why[96];
    int nonzero;               /* every non-zero SCE return, counted by gxm_chk (M1 name: g_nonzero) */
    SceGxmContext *ctx;
    SceGxmShaderPatcher *patcher;
    volatile unsigned *vnot, *fnot;   /* the two notification words (monotonic scene sequence; fragment one is the fence everything relies on) */
    unsigned nval;             /* sequence number of the last scene ENDED (gxm_frame_end_scene); the open scene will get nval + 1 */
    int fence_finish;          /* smoke test fell back: waits are sceGxmFinish (logged loudly) */
    unsigned arm;              /* GXM_ARM_* */
    int copyfilter_on;         /* !(ux0:data/stairfax/copyfilter_off.txt starts with 1) - read once by frame, used by texture and target */
    unsigned frames;           /* presents so far */
    unsigned queued;           /* display-queue entries added */
    volatile unsigned presented;   /* ++ in the display callback */
    unsigned epoch;            /* STATE EPOCH: bumped by target at every BeginScene and by anyone via gxm_state_dirty(); draw re-applies ALL scene state when it moved */
    unsigned scene_draws;      /* sceGxmDraw calls in the open scene (reset by target at BeginScene; a scene with 0 gets a fence quad before EndScene) */
    /* ---- target module writes once, in gxm_target_init (before draw/texture init); everyone reads ---- */
    int rw;                    /* render width (render_res.txt, else 960) */
    int vh;                    /* visible picture rows: GX lines 0..480 occupy y*vh/480 (544 at the panel size) */
    int rh;                    /* M's ALLOCATED height: == vh, or efb528_total_h(vh) when the 528-line target is on (efb_rect.h) */
    int efb528;
    GxmStats st;
} GxmCore;
extern GxmCore g_gx;         /* defined in rhi_gxm_frame.c */

static inline int gxm_dead(void) { return g_gx.dead || !g_gx.up || g_gx.torn_down; }

/* ================================================================================================================================== R3c C6: the pending immediate draw, and what flushes it
 * rhi_gxm_draw.c may HOLD one run of identical-state immediate draws (rhi_gxm_drawTev) and issue it as ONE sceGxmDraw.  A held run has touched NOTHING of the GXM context yet, so anything else that touches the
 * context or the scene must find it issued first.  The other modules are not edited for this: every sceGxm call that sets draw state, opens / ends a scene, queues the display or changes a texture's control
 * words is wrapped HERE, for every file that includes this header, so no module can reach the GPU ahead of a held draw (the draw module's own flush runs with g_gxm_c6_busy set and passes through).  The texture
 * mutators flush only when the held run samples THAT texture.  A new sceGxm entry that changes context state must be added to this list (host/tests/build_gxm_merge_test.sh greps the sceGxm calls of every module
 * against it). */
extern int g_gxm_c6_pending;               /* rhi_gxm_draw.c: a run is held (or, with C6 off, remembered for the census) */
extern int g_gxm_c6_busy;                  /* rhi_gxm_draw.c: the draw module is issuing a draw itself: the wrappers pass */
void gxm_c6_flush(void);                   /* rhi_gxm_draw.c: issue the held run (if any) and end the run */
void rhi_gxm_c6_census(unsigned o[4]);     /* rhi_gxm_draw.c: {immediate draw calls, draws they became (runs), longest run, calls that joined a run}, cumulative, counted also with C6 off */
void gxm_c6_flush_tex(const SceGxmTexture *t);   /* rhi_gxm_draw.c: the same, only when the held run samples the texture whose control words are *t */
static inline void gxm_c6_hook(void) { if (g_gxm_c6_pending && !g_gxm_c6_busy) gxm_c6_flush(); }
static inline void gxm_c6_hook_tex(const SceGxmTexture *t) { if (g_gxm_c6_pending && !g_gxm_c6_busy) gxm_c6_flush_tex(t); }
#define sceGxmSetVertexProgram(...)                    (gxm_c6_hook(), sceGxmSetVertexProgram(__VA_ARGS__))
#define sceGxmSetFragmentProgram(...)                  (gxm_c6_hook(), sceGxmSetFragmentProgram(__VA_ARGS__))
#define sceGxmSetVertexStream(...)                     (gxm_c6_hook(), sceGxmSetVertexStream(__VA_ARGS__))
#define sceGxmSetFragmentTexture(...)                  (gxm_c6_hook(), sceGxmSetFragmentTexture(__VA_ARGS__))
#define sceGxmReserveVertexDefaultUniformBuffer(...)   (gxm_c6_hook(), sceGxmReserveVertexDefaultUniformBuffer(__VA_ARGS__))
#define sceGxmReserveFragmentDefaultUniformBuffer(...) (gxm_c6_hook(), sceGxmReserveFragmentDefaultUniformBuffer(__VA_ARGS__))
#define sceGxmSetViewport(...)                         (gxm_c6_hook(), sceGxmSetViewport(__VA_ARGS__))
#define sceGxmSetRegionClip(...)                       (gxm_c6_hook(), sceGxmSetRegionClip(__VA_ARGS__))
#define sceGxmSetCullMode(...)                         (gxm_c6_hook(), sceGxmSetCullMode(__VA_ARGS__))
#define sceGxmSetFrontDepthFunc(...)                   (gxm_c6_hook(), sceGxmSetFrontDepthFunc(__VA_ARGS__))
#define sceGxmSetBackDepthFunc(...)                    (gxm_c6_hook(), sceGxmSetBackDepthFunc(__VA_ARGS__))
#define sceGxmSetFrontDepthWriteEnable(...)            (gxm_c6_hook(), sceGxmSetFrontDepthWriteEnable(__VA_ARGS__))
#define sceGxmSetBackDepthWriteEnable(...)             (gxm_c6_hook(), sceGxmSetBackDepthWriteEnable(__VA_ARGS__))
#define sceGxmSetFrontDepthBias(...)                   (gxm_c6_hook(), sceGxmSetFrontDepthBias(__VA_ARGS__))
#define sceGxmSetBackDepthBias(...)                    (gxm_c6_hook(), sceGxmSetBackDepthBias(__VA_ARGS__))
#define sceGxmBeginScene(...)                          (gxm_c6_hook(), sceGxmBeginScene(__VA_ARGS__))
#define sceGxmEndScene(...)                            (gxm_c6_hook(), sceGxmEndScene(__VA_ARGS__))
#define sceGxmFinish(...)                              (gxm_c6_hook(), sceGxmFinish(__VA_ARGS__))
#define sceGxmDisplayQueueAddEntry(...)                (gxm_c6_hook(), sceGxmDisplayQueueAddEntry(__VA_ARGS__))
#define sceGxmDisplayQueueFinish(...)                  (gxm_c6_hook(), sceGxmDisplayQueueFinish(__VA_ARGS__))
#define sceGxmTransferDownscale(...)                   (gxm_c6_hook(), sceGxmTransferDownscale(__VA_ARGS__))
#define sceGxmTextureInitLinear(t, ...)                (gxm_c6_hook_tex(t), sceGxmTextureInitLinear(t, __VA_ARGS__))
#define sceGxmTextureSetMinFilter(t, ...)              (gxm_c6_hook_tex(t), sceGxmTextureSetMinFilter(t, __VA_ARGS__))
#define sceGxmTextureSetMagFilter(t, ...)              (gxm_c6_hook_tex(t), sceGxmTextureSetMagFilter(t, __VA_ARGS__))
#define sceGxmTextureSetUAddrMode(t, ...)              (gxm_c6_hook_tex(t), sceGxmTextureSetUAddrMode(t, __VA_ARGS__))
#define sceGxmTextureSetVAddrMode(t, ...)              (gxm_c6_hook_tex(t), sceGxmTextureSetVAddrMode(t, __VA_ARGS__))
#define sceGxmTextureSetMipmapCount(t, ...)            (gxm_c6_hook_tex(t), sceGxmTextureSetMipmapCount(t, __VA_ARGS__))
#define sceGxmTextureSetMipFilter(t, ...)              (gxm_c6_hook_tex(t), sceGxmTextureSetMipFilter(t, __VA_ARGS__))
#define sceGxmTextureSetLodBias(t, ...)                (gxm_c6_hook_tex(t), sceGxmTextureSetLodBias(t, __VA_ARGS__))

/* the instance the RhiOps carry (defined by the shell, rhi_gxm.c). cur_tex: written by texture's gxm_op_setTexture, read by draw. mvp: row-major, written by draw's gxm_op_setColorTransform. */
typedef struct RhiGxm { RhiInstance base; int w, h; RhiTexture *cur_tex; float mvp[16]; } RhiGxm;
extern RhiGxm g_gxm_inst;

/* ================================================================================================================================== frame module */
void gxm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));      /* printf + newline; "[gxm] ..." lines are parsed by tools/stfx_gxm_check.py */
int  gxm_chk(int r, const char *what, const char *file, int line);              /* returns r; r != 0 -> "[gxm] RC 0x%08X  <what>  (<file>:<line>) [nonzero so far N]" (first 32, then every 1024th) and g_gx.nonzero++ */
#define GXM_CHK(call) gxm_chk((call), #call, __FILE__, __LINE__)
void gxm_abort(const char *why_fmt, ...) __attribute__((format(printf, 1, 2))); /* sets g_gx.dead (once), logs "[gxm] ABORT: ..."; idempotent */
#include <psp2/kernel/processmgr.h>
static inline uint64_t gxm_now_us(void) { return (uint64_t) sceKernelGetProcessTimeWide(); }

/* GPU memory. Arenas are mapped once (sceGxmMapMemory) and sub-allocated: no kernel call per texture. align: power of two, >= 16 (textures 256, vertex data 32). */
enum { GXM_MEM_CDRAM = 0,    /* USER_CDRAM_RW, 256 KB arenas: render targets, textures, static meshes, copy textures */
       GXM_MEM_LPDDR = 1 };  /* USER_RW_UNCACHE, 1 MB arenas: CPU-streamed (staging ring, dynamic mesh slots, identity index). A CDRAM request that fails falls back to LPDDR (counted) except for render surfaces. */
typedef struct GxmBlk { void *p; uint32_t size; uint8_t kind; uint16_t arena; uint32_t off; } GxmBlk;   /* p == NULL: empty. Only p/size are read by other modules. */
int  gxm_blk_alloc(GxmBlk *b, int kind, uint32_t size, uint32_t align);   /* 0 ok; -1 (counted) with b->p == NULL */
void gxm_blk_free_deferred(GxmBlk *b);   /* queue the free for when the next scene that may read it has finished (seq = nval + 1); zeroes *b */
void gxm_blk_free_now(GxmBlk *b);        /* ONLY for memory never submitted to the GPU, and at teardown after the GPU is idle; zeroes *b */
void gxm_frame_defer(void (*fn)(void *), void *arg);   /* run fn(arg) once the fragment fence of the scene with seq nval + 1 has been reached (the next EndScene at the latest = the next present) */
void *gxm_ring_alloc(uint32_t bytes);    /* per-frame vertex staging: 32-byte aligned GPU-visible memory no unretired scene reads; NULL if it can never fit (the caller skips with GXM_SK_RING) */
uint32_t gxm_ring_capacity(void);
int gxm_ring_open_scene_fits(uint32_t bytes);   /* 1: gxm_ring_alloc(bytes) cannot fail for want of space after ended scenes retire; 0: the OPEN scene holds too much - end it first (gxm_target_scene_end) */

/* Scene fences. The ONLY caller of sceGxmEndScene. Passes both a vertex and a fragment notification (value = ++nval), tells the ring the scene ended,
 * returns the sceGxmEndScene rc (0 ok). The open scene's sequence number will be g_gx.nval + 1. */
int  gxm_frame_end_scene(void);
/* Bounded wait for the fragment fence of scene `seq` (seq <= nval). 0 reached; -1 deadline passed (counted in the fence_timeouts counter and logged; the CALLER
 * decides: target aborts on its own waits). With g_gx.fence_finish it is sceGxmFinish and returns 0. */
int  gxm_frame_wait_scene(uint32_t seq, uint32_t deadline_us);
/* The fence smoke test (c2floor smoke_fence, hardware-verified shape): target calls it ONCE from gxm_target_init after ending a one-quad scene with
 * gxm_frame_end_scene(). Bounded poll (3 s) of that scene's fragment notification; on timeout it switches g_gx.fence_finish to 1 (waits become sceGxmFinish),
 * logs "[gxm] smoke fence: TIMEOUT ... falling back to sceGxmFinish" and still returns 0; returns -1 only if the fallback itself cannot be used. */
int  gxm_frame_smoke(uint32_t seq);
/* target calls this LAST in present: advances the display frame, polls the deferred queue, rolls the stats window, prints the periodic lines (every 60th). */
void gxm_frame_present_end(int presented_ok);
static inline void gxm_state_dirty(void) { ++g_gx.epoch; }   /* any module that changed GXM scene state behind draw's back (target quads) calls this */

/* init / shutdown (the shell's rhi_gxm_create / rhi_gxm_shutdown call these). Init order is FIXED:
 *   frame (sceGxmInitialize, memory, context, patcher, fences) -> shader -> texture -> target (surfaces, util programs, smoke scene) -> draw
 * and shutdown is the exact reverse (draw, target, texture, shader, frame). Any init failure unwinds the modules already up and returns -1. */
int  gxm_frame_init(const RhiCreateInfo *info);
void gxm_frame_shutdown(void);

/* Counter text for the report lines: each module appends "key=value " tokens (space separated, no newline) to buf (cap bytes) and returns how many it wrote.
 * frame prints them after its own tokens in "[gxm-acct] frame=N ..." lines, so a module's tokens are part of the checked log. */
int  gxm_frame_report(char *buf, int cap);
int  gxm_shader_report(char *buf, int cap);
int  gxm_tex_report(char *buf, int cap);
int  gxm_draw_report(char *buf, int cap);
int  gxm_target_report(char *buf, int cap);

/* the one sceGxmDraw wrapper (R4) */
static inline int gxm_sce_draw(SceGxmPrimitiveType prim, SceGxmIndexFormat ifmt, const void *idx, uint32_t n)
{
    gxm_c6_hook();   /* R3c C6: unreachable by design (every state call before a draw flushes), kept so a missed wrapper cannot reorder a draw */
    const int r = sceGxmDraw(g_gx.ctx, prim, ifmt, idx, n);
    if (r == 0) { ++g_gx.st.sce_draw_ok; ++g_gx.scene_draws; }
    else { ++g_gx.st.sce_draw_fail; gxm_chk(r, "sceGxmDraw", __FILE__, __LINE__); }
    return r;
}

/* ================================================================================================================================== shader module */
enum { GXM_PK_TEV = 0,        /* TEV program of drawTev (tev_gen_vertex_ex + tev_gen_fragment[_fast]); pack kinds 't' / 'f' */
       GXM_PK_GX,             /* GX pair (tev_gen_vertex_gx + fragment); pack kinds 'g' / 'h' */
       GXM_PK_UTIL };         /* clear, blit, EFB copy: registered from a precompiled GXP or compiled from Cg */
enum { GXM_FROM_PACK = 0, GXM_FROM_SHARK, GXM_FROM_GXPCACHE, GXM_FROM_PRECOMPILED };
/* Builtin programs for the fixed-function entry points: synthesised TevKeys through the TEV path (no new Cg). */
enum { GXM_BI_COLORED = 0,    /* drawColored: colour = vertex colour (numTex 0) */
       GXM_BI_TEXTURED,       /* drawTextured / drawStatic, opaque or blended: tex * vertex colour (numTex 1) */
       GXM_BI_TEXTURED_ATEST, /* the same with a discarding alpha compare a >= 128 (RHI_ALPHA_TEST) */
       GXM_BI_N };
/* uniform kinds (c2floor classify()): found by NAME in the GXP; a name that is not here is counted as unmapped, never a failure */
enum { GU_NONE = 0, GU_M, GU_POS, GU_NRM, GU_TMX, GU_MAT, GU_AMB, GU_LT, GU_TM, GU_PT, GU_R, GU_K, GU_AREF, GU_FOG, GU_FOGC, GU_IMA, GU_IMB, GU_TS, GU_BONE };
typedef struct GxmUBind { const SceGxmProgramParameter *p; uint8_t kind, idx; uint16_t maxc; uint16_t off; } GxmUBind;   /* maxc: array size * component count - never write more floats. off (R3a C1): the parameter's resource index = its offset in 32-bit words in the default uniform buffer, resolved ONCE at program creation */
typedef struct GxmAttr { const char *name; uint16_t off; uint8_t fmt; uint8_t comps; } GxmAttr;              /* fmt: SCE_GXM_ATTRIBUTE_FORMAT_*; the GXP's regIndex is found by name */

typedef struct GxmProg {
    int ok;                              /* registered and usable */
    uint8_t family, fast, from;          /* GXM_PK_*, the fast fragment variant, GXM_FROM_* */
    uint32_t hash;                       /* the device's program hash (the pack key), 0 for util programs */
    const char *name;                    /* util programs; "tev"/"gx"/"bi" otherwise */
    SceGxmProgram *vg, *fg;              /* 16-byte-aligned private copies, alive until shutdown (the patcher keeps the pointers) */
    SceGxmShaderPatcherId vid, fid;
    GxmUBind vb[GXM_UB_MAX], fb[GXM_UB_MAX]; int nvb, nfb;
    int texunit[TEVK_TEX];               /* sampler "tex<s>" -> fragment texture unit (resource index); -1: the GXP has no such sampler (dead / GX_TEXMAP_DISABLE) */
    uint32_t vub_bytes, fub_bytes;       /* default uniform buffer sizes; 0: do NOT reserve (reserving a zero-size buffer returns NULL) */
    uint8_t ufast, ucheck;               /* R3a C1: ufast = every uniform binding of both stages resolved to a word offset inside its buffer (writes by offset allowed); ucheck = draws whose offset writes were compared with the API's */
    VsKey vk;                            /* GX family: the VsKey the program was made from (attribute set); zero otherwise */
    TevKey tk;                           /* the CANONICAL TevKey (after perspective + canonicalisation) */
    void *priv;                          /* shader module only (variant caches) */
} GxmProg;

int  gxm_shader_init(void);              /* needs g_gx.patcher. Reads ux0:data/stairfax/tev_fast.txt + ux0:data/STFX00001/{texgen_persp_off,tev_dedupe_off}.txt, indexes shc/pack.bin, shark_init (console) */
void gxm_shader_shutdown(void);          /* releases every vertex/fragment program, unregisters every program, frees every GXP copy; NOT shark_end (it crashes the process) */
/* get-or-create. NULL = no program; *why = GXM_SK_NOPROG / PROGFULL / COMPILE. Cheap on a hit (one-key memo). The returned pointer is stable until shutdown. */
GxmProg *gxm_shader_tev(const TevKey *raw, int *why);
GxmProg *gxm_shader_gx(const VsKey *raw_vk, const TevKey *raw_tk, int *why);
/* R3a C3: the same with a HINT from the HAL (gx_draw.c gx_prog_hint: bit 63 | TevKey serial | VsKey serial | fog type).  A hint equal to the hint of the call that stored the one-key memo means the
 * HAL rebuilt neither key since: the memo answers without comparing or hashing the ~700 raw key bytes.  hint 0 = none, exactly gxm_shader_gx. */
GxmProg *gxm_shader_gx_hint(const VsKey *raw_vk, const TevKey *raw_tk, unsigned long long hint, int *why);
GxmProg *gxm_shader_builtin(int which, int *why);
GxmProg *gxm_shader_util_precompiled(const char *name, const void *vgxp, const void *fgxp);    /* NULL on failure (logged) */
GxmProg *gxm_shader_util_cg(const char *name, const char *vs_src, const char *fs_src);         /* shark; NULL when the compiler is unavailable (emulator) or the compile failed */
/* variants, cached inside the program. NULL on failure (counted). n attributes whose name the GXP lacks are skipped (counted as unmapped_attr), not failed. */
SceGxmVertexProgram *gxm_shader_vp(GxmProg *p, const GxmAttr *a, int n, int stride);          /* u16 index source */
SceGxmFragmentProgram *gxm_shader_fp(GxmProg *p, uint32_t alphaMode);                          /* alphaMode = the word the hal passes (low byte RhiAlphaMode, 0x100 exact, 0x200 subtract, src<<12, dst<<16) */
/* The same program with colour writes OFF and no blend: the scene-closing FENCE QUAD (a scene with no draw is not known to signal its notification; this draw
 * cannot change a pixel). Used by the target module with the util clear program. */
SceGxmFragmentProgram *gxm_shader_fp_nowrite(GxmProg *p);
/* blend identity: 0 for OPAQUE/TEST, else the bits that change the blend (c2floor blend_key) */
static inline uint32_t gxm_blend_key(uint32_t am) { const uint32_t m = am & 0xffu; return (m == RHI_ALPHA_BLEND || m == RHI_ALPHA_ADD) ? (am & 0x773ffu) : 0u; }
/* housekeeping the game's wrapper calls (M1 entry points, now owned by shader) */
void rhi_gxm_asc_tick(void);
void rhi_gxm_shc_flush(unsigned ms);
void rhi_gxm_shc_shutdown(void);

/* ================================================================================================================================== texture module */
typedef struct GxmTex {
    SceGxmTexture t;                     /* control words: the sampler state (filter, wrap, lod bias, mip count) lives HERE and is snapshotted by sceGxmSetFragmentTexture */
    int w, h;                            /* the GX-declared size (a copy texture: dw x dh, not the allocation) */
    uint32_t fmt;                        /* GX format (census only) */
    uint8_t filtMin, filtMagNear, hasMips, copy, wrapS, wrapT;   /* as VitaTexture (rhi_vita.c:116-124): filtMin 0..5, 1 = GX_LIN; wrap = tex_wrap.h TW_* */
    int8_t lodBias8; uint8_t nlev;       /* nlev: levels in the chain (1 = base only) */
    GxmBlk mem;                          /* texel storage; mem.p == NULL: allocation failed - a draw naming this texture skips with GXM_SK_NOTEXMEM */
    /* copy targets only (copy == 1) */
    int aw, ah;                          /* allocation size: dw, dh rounded UP to multiples of 8 (rhi_vita_efb.inc:652-657 - the picture is drawn over the whole aligned texture) */
    SceGxmRenderTarget *rt;              /* GXM_COPY_RT_SCENES scenes per frame, aw x ah */
    SceGxmColorSurface cs;               /* LINEAR A8B8G8R8 over mem, stride aw */
    uint8_t smin, smag;                  /* copyTexSetFilter state: 1 = POINT applied */
} GxmTex;

int  gxm_tex_init(void);
void gxm_tex_shutdown(void);             /* frees every texture still alive (the game may not destroy them all), counted */
/* RhiOps (texture): installed in the shell's table */
RhiTexture *gxm_op_createTexture(RhiInstance *r, int w, int h, int mips, uint32_t fmt, const void *rgba8);
void        gxm_op_destroyTexture(RhiInstance *r, RhiTexture *h);
void        gxm_op_setTexture(RhiInstance *r, int slot, RhiTexture *h);
/* for the target module's EFB copies */
GxmTex *gxm_tex_new_copy(int dw, int dh, int gxfmt);   /* NULL on failure (counted, reason logged once). Never returns a texture without rt/cs/mem. */
void    gxm_tex_free(GxmTex *t);                       /* deferred GPU free; the struct is freed with it. Used for ANY GxmTex (ordinary or copy). */
void    gxm_target_copyedge_forget(GxmTex *t);         /* rhi_gxm_target.c: gxm_copyedge diagnostic hook, called by gxm_tex_free for copy textures (one branch when the marker is absent) */

/* ================================================================================================================================== draw module */
int  gxm_draw_init(void);
void gxm_draw_shutdown(void);            /* frees leftover meshes and static VBOs (counted) */
void gxm_op_drawColored(RhiInstance *r, const RhiColorVertex *v, uint32_t count);
void gxm_op_setColorTransform(RhiInstance *r, const float m[16]);
void gxm_op_drawTextured(RhiInstance *r, const RhiTexVertex *v, uint32_t count);

/* ================================================================================================================================== target module */
int  gxm_target_trace_frame(void);   /* DEBUG: 1 while the frame of the first gxm_shot.txt entry is being drawn and gxm_mshot.txt starts with 2 (per-scene / per-draw trace lines) */
int  gxm_target_init(const RhiCreateInfo *info);   /* after frame/shader/texture init: geometry (render_res.txt, efb528_off.txt) -> g_gx.rw/vh/rh/efb528, surfaces, util programs, display buffers, then the smoke scene */
void gxm_target_shutdown(void);
/* Make sure a scene is open on M (lazily BeginScene on the M render target chosen by rotation, depth surface with ForceLoad/Store). Ends a COPY/other scene first.
 * 0 ok; -1 failed (counted by the caller as GXM_SK_SCENE; after repeated failures the backend aborts). Call it LAST, right before the first sceGxm state call of a
 * draw: a scene opened and then abandoned still ends with a fence quad, but it is a wasted scene. g_gx.epoch moves at every BeginScene. */
int  gxm_target_scene_m(void);
/* End whatever scene is open (adds the fence quad first when scene_draws == 0). why: 'c' copy, 'x' readback, 'p' present, 'r' reset. Safe when no scene is open. */
void gxm_target_scene_end(char why);
bool gxm_op_present(RhiInstance *r, RhiSwapchain *sc);   /* ends M's scene, display scene (blit of M's picture), display queue, then gxm_frame_present_end(); false only if dead / the display scene failed */
void gxm_op_beginFrame(RhiInstance *r);
void gxm_op_endFrame(RhiInstance *r);
void rhi_gxm_r9_frame(void);                                  /* R9 replay census (rhi_gxm_draw.c): called once per present */
void gxm_op_clear(RhiInstance *r, float cr, float cg, float cb, float ca);

/* ================================================================================================================================== public entry points
 * (declared in rhi_vita.c / rhi_vita_tev.inc / rhi_vita_efb.inc; each is defined in EXACTLY ONE module file - the owner is the file that defines it)
 *   draw    : rhi_gxm_setDepthState setViewport setScissor setShadowZBias uploadStatic deleteStatic drawStatic drawTev gx_mesh_create gx_mesh_update gx_mesh_destroy drawTevGx
 *   texture : rhi_gxm_texInfo texSetWrap texSetMips texSetLodBias texSetFilter copyTexSetFilter copyTexSetSampler copyEfbFree
 *   target  : rhi_gxm_readEfbRect clearRect efbMode efbZMode peekZ peekZ_sync copyEfb
 *   shader  : rhi_gxm_asc_tick shc_flush shc_shutdown
 *   frame   : rhi_gxm_gx_report
 *   shell   : rhi_gxm_create rhi_gxm_shutdown stfx_backend_select g_stfx_backend, the RhiOps table, destroy/swapchain* ops
 * Prototypes: exactly as in rhi_vita.c:70-85, rhi_vita_tev.inc:19-24, rhi_vita_efb.inc:117-121 (they are repeated here so each module sees them). */
RhiInstance *rhi_gxm_create(const RhiCreateInfo *info);
void rhi_gxm_shutdown(void);
void rhi_gxm_setDepthState(int test, int func, int write);
void rhi_gxm_setViewport(float x, float y, float w, float h);
void rhi_gxm_setScissor(int x, int y, int w, int h);
void rhi_gxm_setShadowZBias(int on);
unsigned char *rhi_gxm_readEfbRect(int x, int y, int w, int h, int *ow, int *oh);
void rhi_gxm_clearRect(int x, int y, int w, int h, uint32_t rgba, float z);
void rhi_gxm_texInfo(RhiTexture *h, int out[4]);
void rhi_gxm_texSetWrap(RhiTexture *h, int gxS, int gxT);
int  rhi_gxm_texSetMips(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel);
void rhi_gxm_texSetLodBias(RhiTexture *h, int bias8);
void rhi_gxm_texSetFilter(RhiTexture *h, int minGX, int magNearest, int wantMips);
unsigned rhi_gxm_uploadStatic(const RhiTexVertex *v, uint32_t n);
void rhi_gxm_deleteStatic(unsigned id);
void rhi_gxm_drawStatic(RhiInstance *r, unsigned vbo, uint32_t first, uint32_t count);
int  rhi_gxm_efbMode(void);
int  rhi_gxm_efbZMode(void);
int  rhi_gxm_peekZ(int x, int y, unsigned *z24, unsigned *raw);
void rhi_gxm_peekZ_sync(void);
RhiTexture *rhi_gxm_copyEfb(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt);
void rhi_gxm_copyTexSetFilter(RhiTexture *h, int gxMin, int gxMag);
void rhi_gxm_copyTexSetSampler(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT);
void rhi_gxm_copyEfbFree(RhiTexture *t);
int  rhi_gxm_drawTev(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16], int alphaMode);
void *rhi_gxm_gx_mesh_create(const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, const GxMeshLayout *lay);
void rhi_gxm_gx_mesh_update(void *mesh, const void *verts, uint32_t vbytes);
void *rhi_gxm_gx_mesh_update_begin(void *mesh, uint32_t vbytes, int *fresh);   /* R6 C9 (rhi_gxm_draw.c): the slot's CPU pointer or NULL; *fresh: see the definition */
void rhi_gxm_gx_mesh_update_commit(void *mesh);
void *rhi_gxm_gx_mesh_update_begin_real(void *mesh, uint32_t vbytes, int *fresh);   /* R5: the real halves (what R-back runs); the two names above are dispatchers while the process is split-capable */
void rhi_gxm_gx_mesh_update_commit_real(void *mesh);
void rhi_gxm_gx_mesh_destroy(void *mesh);
int  rhi_gxm_drawTevGx(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode);
/* R5_SPLIT.md (render-thread split): */
int  rhi_gxm_drawTevGx_h(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode, unsigned long long hint);   /* D6: the hint is an argument */
int  rhi_gxm_verdict_gx(const VsKey *vk, const TevKey *tk);     /* D4: 0 = the draw would come back 0 (no program for the key) */
int  rhi_gxm_verdict_tev(const TevKey *tk);
void rhi_gxm_set_r3a_ovr(int on, unsigned mask);                /* D6: R-back's private copy of the cut switches */
int  rhi_gxm_pure_layout_ok(const GxMeshLayout *L);              /* the pure rules of rhi_gxm_draw.c, for the front */
int  rhi_gxm_pure_mesh_check(uint32_t stride, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, uint32_t *need);
int  rhi_gxm_copy_precheck(int x, int y, int w, int h, int dw, int dh, int gxfmt);   /* 1: rhi_gxm_copyEfb would not refuse on its arguments alone (rhi_gxm_target.c) */
int  rhi_gxm_tex_max_dim(void);                                 /* rhi_gxm_texture.c TEX_MAX_DIM */
void rhi_gxm_asc_tick(void);
void rhi_gxm_shc_flush(unsigned ms);
void rhi_gxm_shc_shutdown(void);
void rhi_gxm_gx_report(void);

#ifdef __cplusplus
}
#endif
#endif /* STFX_RHI_GXM_INT_H */
