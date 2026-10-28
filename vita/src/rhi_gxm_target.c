/* rhi_gxm_target.c - the TARGET module of the native GXM backend, milestone M2 (host/engine/GXM_M2.md sections 3, 9; contract: src/rhi_gxm_int.h).
 *
 * OWNS: the render target M (colour + depth/stencil), scenes (begin / end / render-target rotation / the fence quad), clear, clearRect, readEfbRect,
 * copyEfb (an EFB copy is one GPU quad into a copy texture), peekZ / efbMode / efbZMode, the three display buffers, present + the display queue, the
 * display blit, the picture SHOT, the smoke scene. It is the ONLY caller of sceGxmBeginScene (R6).
 *
 * ORIENTATION (decided, GXM_M2.md 3.3): M is stored TOP ROW FIRST. Clip +y is memory row 0 (viewport yScale < 0). efb_rect_px() already gives top-origin
 * pixel rects. A copy texture's row 0 is the TOP of the source rectangle, so the copy quad needs no flip (output row 0 <- source v = y0/rh). readEfbRect
 * returns rows BOTTOM first (glReadPixels order).
 *
 * LAYOUT OF THIS FILE
 *   Part A  pure helpers (no SCE, no globals): res parsing, the rect / NDC / uv mapping, the efb_conv table, the readback row order, the shot statistics.
 *           Compiled alone with -DGXM_TARGET_HOST_TEST by host/tests/build_gxm_target_test.sh, which also builds each mutant (-DGXM_TGT_BREAK_*; compiled out here).
 *   Part B  the module (everything that calls sceGxm* / the other modules).
 *
 * NOTHING HERE HAS RUN. Emulator numbers are never timing claims. No identifier starting gl/vgl (R1). */

#ifdef GXM_TARGET_HOST_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efb_rect.h"
#else
#include "rhi_gxm_int.h"
#include <psp2/kernel/processmgr.h>
#include "efb_rect.h"
#include "gxm_copyedge.h"
#include "efb_rect.h"
#include <psp2/io/fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* ================================================================================================================================= Part A: pure helpers */

/* render_res.txt, exactly as rhi_vita_efb.inc:91-111 (efb_res_init). found: the file opened. b: its text (NUL-terminated), len: bytes read.
 * Returns the "why" string of the [res] line; *valid = 1 and *w,*h set only for a usable file. */
static const char *gxm_tgt_res_parse(int found, const char *b, int len, int *w, int *h, int *valid)
{
    int ww = 0, hh = 0, n = 0;
    *valid = 0;
    if (!found) return "default";
    if (len > 0) n = sscanf(b, "%d %d", &ww, &hh);
    if (n != 2) return "render_res.txt invalid: want two integers \"W H\"";
    if ((ww & 7) || (hh & 7)) return "render_res.txt invalid: not multiples of 8";
    if (ww < 320 || ww > 960 || hh < 184 || hh > 544) return "render_res.txt invalid: outside 320..960 x 184..544";
    *w = ww; *h = hh; *valid = 1;
    return "render_res.txt";
}

/* efb528_init (rhi_vita_efb.inc:78-90): vh = the picture height, rh = M's allocated height. off = efb528_off.txt starts with '1'. */
static void gxm_tgt_geom(int h, int off, int *vh, int *rh, int *efb528)
{
    *vh = h;
    *rh = h;
    *efb528 = 0;
    if (!off) { *rh = efb528_total_h(h); *efb528 = *rh > *vh; }
}

typedef struct { int x0, y0, x1, y1; } GxmPx;

/* clearRect's pixel rect (rhi_vita.c:535-558): x*rw/640 .. (x+w)*rw/640, y*vh/480 .. (y+h)*vh/480, or efb_rect_px when the 528-line target is on;
 * then CLAMPED to M (the vitaGL path scissors; a quad past the target is clipped the same way). Returns 1 when something is left to draw. */
static int gxm_tgt_clear_px(int x, int y, int w, int h, int rw, int vh, int rh, int efb528, GxmPx *o)
{
    if (w <= 0 || h <= 0) return 0;
    o->x0 = x * rw / 640; o->x1 = (x + w) * rw / 640;
    o->y0 = y * vh / 480; o->y1 = (y + h) * vh / 480;
    if (efb528) { const EfbRectPx rp = efb_rect_px(x, y, w, h, rw, vh, rh); o->x0 = rp.x0; o->x1 = rp.x1; o->y0 = rp.y0; o->y1 = rp.y1; }
    if (o->x0 < 0) o->x0 = 0;
    if (o->y0 < 0) o->y0 = 0;
    if (o->x1 > rw) o->x1 = rw;
    if (o->y1 > rh) o->y1 = rh;
    return o->x1 > o->x0 && o->y1 > o->y0;
}

/* pixel rect -> NDC {xLeft, xRight, yTop, yBottom} of the clear program's `position` uniform (GXM_M2.md 9) */
static void gxm_tgt_px_ndc(const GxmPx *p, int rw, int rh, float rect[4])
{
    rect[0] = 2.0f * (float) p->x0 / (float) rw - 1.0f;
    rect[1] = 2.0f * (float) p->x1 / (float) rw - 1.0f;
#ifdef GXM_TGT_BREAK_NDC
    rect[2] = 2.0f * (float) p->y0 / (float) rh - 1.0f;     /* mutant: +y down */
    rect[3] = 2.0f * (float) p->y1 / (float) rh - 1.0f;
#else
    rect[2] = 1.0f - 2.0f * (float) p->y0 / (float) rh;
    rect[3] = 1.0f - 2.0f * (float) p->y1 / (float) rh;
#endif
}

/* The stored depth is the clip z of the clear quad mapped through the viewport (window z = 0.5 + 0.5 * clip z): clip z = 2z - 1 stores exactly z. */
static float gxm_tgt_clear_clipz(float z)
{
#ifdef GXM_TGT_BREAK_CLIPZ
    return z;                                                 /* mutant: forgets the window-depth mapping */
#else
    return 2.0f * z - 1.0f;
#endif
}

/* rgba as the hal packs it (memory order R,G,B,A; rhi_vita.c:546-547) -> floats */
static void gxm_tgt_rgba_f(uint32_t rgba, float out[4])
{
    out[0] = (float) (rgba & 0xffu) / 255.0f;
    out[1] = (float) ((rgba >> 8) & 0xffu) / 255.0f;
    out[2] = (float) ((rgba >> 16) & 0xffu) / 255.0f;
    out[3] = (float) (rgba >> 24) / 255.0f;
}

/* The copy quad's uv rectangle. M is top-row-first and the copy quad's aPos.y = 0 edge is the destination's row 0 (the TOP), so the rectangle is
 * {x0/rw, y0/rh, x1/rw, y1/rh} with NO flip (the vitaGL path had 1 - y/rh because GL's v = 0 is the bottom). */
static void gxm_tgt_copy_uvr(int x0, int y0, int x1, int y1, int rw, int rh, float uvr[4])
{
    uvr[0] = (float) x0 / (float) rw;
    uvr[2] = (float) x1 / (float) rw;
#ifdef GXM_TGT_BREAK_FLIP
    uvr[1] = 1.0f - (float) y0 / (float) rh;                  /* mutant: the vitaGL (bottom-origin) form on a top-origin M */
    uvr[3] = 1.0f - (float) y1 / (float) rh;
#else
    uvr[1] = (float) y0 / (float) rh;
    uvr[3] = (float) y1 / (float) rh;
#endif
}

/* GX copy format -> conversion constants (rhi_vita_efb.inc:593-631, verbatim except the Z8 / self-test branches: efbZMode is 0 here, there is no boot
 * self-test). 0 = not handled (every Z format included). sel/cfg feed the copy program; qlv is the quantiser (FIX B), only when cf_on. */
static int gxm_tgt_conv(int f, float sel[4], float cfg[4], float qlv[4], int cf_on)
{
    sel[0] = sel[1] = sel[2] = sel[3] = 0.0f;
    cfg[0] = 0.0f; cfg[1] = 0.0f; cfg[2] = 1.0f; cfg[3] = 0.0f;
    qlv[0] = qlv[1] = qlv[2] = qlv[3] = 0.0f;
    if (f == 0x11) return 0;                                  /* Z8: needs a depth resolve (efbZMode 0): refused */
    if (f & 0x10) return 0;                                   /* every other Z format */
    switch (f) {
    case 0x20: case 0x28: sel[0] = 1.0f; break;               /* R4, R8 -> (r,r,r,r) */
#ifdef GXM_TGT_BREAK_CONV
    case 0x29: sel[2] = 1.0f; break;                          /* mutant: G8 reads blue */
#else
    case 0x29: sel[1] = 1.0f; break;                          /* G8 */
#endif
    case 0x2A: sel[2] = 1.0f; break;                          /* B8 */
    case 0x27: cfg[0] = 1.0f; break;                          /* A8: the EFB is RGB8_Z24, its alpha reads 255 */
    case 0x00: case 0x01:                                     /* I4, I8: BT.601 luma */
        sel[0] = 0.257f; sel[1] = 0.504f; sel[2] = 0.098f; cfg[0] = 16.0f / 255.0f; break;
    case 0x04: case 0x05: case 0x06: cfg[1] = 1.0f; cfg[2] = 0.0f; break;   /* RGB565, RGB5A3, RGBA8 -> (r,g,b,1) */
    default: return 0;
    }
    if (cf_on) {                                              /* FIX B: what the GX format keeps */
        switch (f) {
        case 0x20: case 0x00: qlv[0] = qlv[1] = qlv[2] = qlv[3] = 15.0f; break;
        case 0x04: qlv[0] = 31.0f; qlv[1] = 63.0f; qlv[2] = 31.0f; break;
        case 0x05: qlv[0] = qlv[1] = qlv[2] = 31.0f; break;
        default: break;
        }
    }
    return 1;
}

/* readEfbRect's output: rows of M's rectangle [x0,x1) x [y0,y1) (top-origin pixel rows, M stride in BYTES) copied BOTTOM row first. */
static void gxm_tgt_rows_bottom_first(uint8_t *dst, const uint8_t *src, size_t stride_bytes, int x0, int y0, int x1, int y1)
{
    const size_t wb = (size_t) (x1 - x0) * 4u;
    const int h = y1 - y0;
    for (int r = 0; r < h; ++r) {
#ifdef GXM_TGT_BREAK_ROWS
        const int sr = y0 + r;                                /* mutant: top row first */
#else
        const int sr = y1 - 1 - r;
#endif
        memcpy(dst + (size_t) r * wb, src + (size_t) sr * stride_bytes + (size_t) x0 * 4u, wb);
    }
}

/* One textured fan quad in the blit program's stream layout (vitaGL's: 4 positions then 4 texcoords, 8-byte stride, fan order TL, TR, BR, BL).
 * rect = NDC {xLeft, xRight, yTop, yBottom}; uv = {u0, v0, u1, v1} where v0 is the TOP edge. */
static void gxm_tgt_blit_verts(float v[16], const float rect[4], const float uv[4])
{
    v[0] = rect[0]; v[1] = rect[2];   /* TL */
    v[2] = rect[1]; v[3] = rect[2];   /* TR */
    v[4] = rect[1]; v[5] = rect[3];   /* BR */
    v[6] = rect[0]; v[7] = rect[3];   /* BL */
    v[8]  = uv[0]; v[9]  = uv[1];
    v[10] = uv[2]; v[11] = uv[1];
    v[12] = uv[2]; v[13] = uv[3];
    v[14] = uv[0]; v[15] = uv[3];
}

/* the unit quad of the EFB copy program (aPos), fan order: aPos.y = 0 is the top row */
static void gxm_tgt_unit_verts(float v[8])
{
    v[0] = 0.0f; v[1] = 0.0f;
    v[2] = 1.0f; v[3] = 0.0f;
    v[4] = 1.0f; v[5] = 1.0f;
    v[6] = 0.0f; v[7] = 1.0f;
}

/* The display blit's uv rectangle: the picture is the TOP vh rows of M (the 528-line rows lie below it). */
static void gxm_tgt_disp_uv(int vh, int rh, float uv[4])
{
    uv[0] = 0.0f; uv[1] = 0.0f; uv[2] = 1.0f; uv[3] = (float) vh / (float) rh;
}

static uint32_t gxm_tgt_fnv1a(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *) p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ---- the picture SHOT statistics. rgb[i] = r | g << 8 | b << 16 for each of n pixels. SORTS rgb in place.
 * distinct = min(number of distinct colours, 4096); top_pct = floor(100 * count of the most common colour / n); mean = rounded per-channel mean;
 * nonblack_pct = floor(100 * pixels with any non-zero channel / n). A flat colour is distinct = 1, top_pct = 100: the picture gate's break arms
 * (nodraw, noblit) rely on exactly that. */
typedef struct { unsigned distinct, top_pct, mean[3], nonblack_pct; } GxmShotStats;

static int gxm_tgt_u32cmp(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void gxm_tgt_shot_stats(uint32_t *rgb, unsigned n, GxmShotStats *s)
{
    uint64_t sum[3] = { 0, 0, 0 };
    unsigned nb = 0, distinct = 0, top = 0, run = 0;
    memset(s, 0, sizeof *s);
    if (!n) return;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t c = rgb[i];
        sum[0] += c & 0xffu; sum[1] += (c >> 8) & 0xffu; sum[2] += (c >> 16) & 0xffu;
        if (c & 0xffffffu) ++nb;
    }
    qsort(rgb, n, sizeof rgb[0], gxm_tgt_u32cmp);
    for (unsigned i = 0; i < n; ++i) {
        if (i == 0 || rgb[i] != rgb[i - 1]) { ++distinct; run = 0; }
        ++run;
        if (run > top) top = run;
    }
#ifdef GXM_TGT_BREAK_STATS_DISTINCT
    distinct = n;                                             /* mutant: every pixel counts as a colour */
#endif
#ifdef GXM_TGT_BREAK_STATS_TOP
    top = 1;                                                  /* mutant: the most common colour is never more than one pixel */
#endif
    s->distinct = distinct < 4096u ? distinct : 4096u;
    s->top_pct = (unsigned) ((100ull * top) / n);
    for (int k = 0; k < 3; ++k) s->mean[k] = (unsigned) ((sum[k] + n / 2u) / n);
    s->nonblack_pct = (unsigned) ((100ull * nb) / n);
}

/* ================================================================================================================================= Part B: the module */
#ifndef GXM_TARGET_HOST_TEST

#include "gxm_shaders/precompiled_clear_v.h"
#include "gxm_shaders/precompiled_clear_f.h"
#include "gxm_shaders/precompiled_blit_v.h"
#include "gxm_shaders/precompiled_blit_f.h"

#define TG_DIR          "ux0:data/stairfax/"
#define TG_SHOT_LIST    TG_DIR "gxm_shot.txt"
#define TG_SHOT_PATH    "ux0:data/stairfax_gxm_shot_%u.ppm"
#define TG_SHOT_FILE    "stairfax_gxm_shot_%u.ppm"
#define TG_SHOT_MAX     32
#define TG_SCRATCH_MIN  256

/* The build id a shot is stamped with. build_game_vita.sh stamps -DSTFX_BUILD_ID into vt_game.o only (this file is compiled earlier, by closure.py), so the id reaches
 * this module as a MARKER the harness writes from build_id.txt: ux0:data/stairfax/gxm_build.txt (first line). Absent: "none", which the checker refuses as INVALID
 * (a shot that is not bound to a build). The checker compares it with the id the BINARY prints at boot ("[build] id="), so a stale marker or another eboot is caught. */
static char g_build_id[96];
static const char *tg_build_id(void)
{
    if (!g_build_id[0]) {
        char b[sizeof g_build_id];
        memset(b, 0, sizeof b);
        SceUID f = sceIoOpen("ux0:data/stairfax/gxm_build.txt", SCE_O_RDONLY, 0777);
        if (f >= 0) { sceIoRead(f, b, sizeof b - 1); sceIoClose(f); }
        size_t n = 0;
        while (b[n] && b[n] != '\n' && b[n] != '\r' && b[n] != ' ') ++n;
        b[n] = 0;
        memcpy(g_build_id, n ? b : "none", n ? n + 1 : 5);
    }
    return g_build_id;
}
#define TG_BUILD_ID tg_build_id()

/* the display callback (frame module) reads this: M1's layout, `{ void *addr; }` (rhi_gxm.c DispData). header_requests: the integrator confirms it. */
typedef struct { void *addr; } TgDispData;

/* ---- the Cg of the EFB copy: COPIED VERBATIM from rhi_vita_efb.inc (the vitaGL path's semantic reference). Compiled on the console by
 * gxm_shader_util_cg (shark); on the emulator util_cg answers NULL and the precompiled blit program stands in (counted efb_copy_noconv). ---- */
/* rhi_vita_efb.inc:133-144 */
static const char *TG_EFB_VS =
"void main(float2 aPos : POSITION,\n"
"          uniform float4 rect, uniform float4 uvr, uniform float zz,\n"
"          out float4 vPos : POSITION, out float2 vUv : TEXCOORD0,\n"
"          out float vD : TEXCOORD1)\n"
"{\n"
"    vPos = float4(rect.x + aPos.x * (rect.z - rect.x),\n"
"                  rect.y + aPos.y * (rect.w - rect.y), zz, 1.0f);\n"
"    vUv = float2(uvr.x + aPos.x * (uvr.z - uvr.x),\n"
"                 uvr.y + aPos.y * (uvr.w - uvr.y));\n"
"    vD = zz * 0.5f + 0.5f;\n"
"}\n";

/* rhi_vita_efb.inc:153-162 */
static const char *TG_EFB_FS_COPY =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex, uniform float4 sel, uniform float4 cfg) : COLOR\n"
"{\n"
"    float4 t = tex2D(tex, vUv);\n"
"    float v = t.x * sel.x + t.y * sel.y + t.z * sel.z + t.w * sel.w + cfg.x;\n"
"    float a = t.w * cfg.z + (1.0f - cfg.z);\n"
"    return float4(v + (t.x - v) * cfg.y, v + (t.y - v) * cfg.y,\n"
"                  v + (t.z - v) * cfg.y, v + (a - v) * cfg.y);\n"
"}\n";

/* rhi_vita_efb.inc:171-184 (FIX B: the copy program with the quantiser; used when copyfilter_off.txt is absent) */
static const char *TG_EFB_FS_COPY_Q =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex, uniform float4 sel, uniform float4 cfg, uniform float4 qlv) : COLOR\n"
"{\n"
"    float4 t = tex2D(tex, vUv);\n"
"    float v = t.x * sel.x + t.y * sel.y + t.z * sel.z + t.w * sel.w + cfg.x;\n"
"    float a = t.w * cfg.z + (1.0f - cfg.z);\n"
"    float4 o = float4(v + (t.x - v) * cfg.y, v + (t.y - v) * cfg.y,\n"
"                      v + (t.z - v) * cfg.y, v + (a - v) * cfg.y);\n"
"    float4 one = float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
"    float4 hlf = float4(0.5f, 0.5f, 0.5f, 0.5f);\n"
"    float4 q = floor(floor(o * 255.0f + hlf) * ((qlv + one) * 0.00390625f)) / max(qlv, one);\n"
"    return o + (q - o) * min(qlv, one);\n"
"}\n";

/* ---- state ---- */
/* M's DEPTH RING (2026-10-27, docs/54 "torches through walls"): one depth/stencil surface per frame, TG_DZ_N in rotation.  peekZ used to read the ONE
 * surface "as it stands" -- in threaded mode G reads while R is anywhere in a frame, so it often saw a PARTIAL frame (just after the frame clear, or after a
 * shadow copy cleared its rectangle): walls missing, torches visible through them.  Now each frame's depth stays whole in its own surface; present publishes
 * (frame, last M scene seq) for it, and peekZ reads the newest published frame the GPU has FINISHED (*fnot).  The game clears all of M once per frame
 * (clear == begin_frame in every log), so a surface's 3-frame-old contents are never seen.  Cost: (TG_DZ_N - 1) x 1.2 MB CDRAM. */
#define TG_DZ_N 3
static GxmBlk b_mc, b_md[TG_DZ_N], b_ms[TG_DZ_N];     /* M: colour, depth ring, stencil ring */
static int g_dz_cur;                                  /* [R] the slot this frame renders into */
static uint32_t g_dz_seq[TG_DZ_N], g_dz_frame[TG_DZ_N], g_dz_fcount;   /* published at present (frame 0 = unpublished); read by any thread */
static GxmBlk b_disp[GXM_DISP_BUFS], b_dd, b_ds;      /* display buffers, display depth, display stencil */
static GxmBlk b_sd, b_ss;                             /* the shared scratch depth/stencil of the copy render targets */
static GxmBlk b_idx;                                  /* 0,1,2,3: the fan quad's index buffer */
static uint16_t *g_idx;
static int g_scratch_aw, g_scratch_ah;
static SceGxmRenderTarget *g_rt_m[GXM_M_RT_N], *g_rt_d;
static int g_n_rt_m, g_rt_cap;                        /* M render targets in use, scenes per frame each was created for */
static SceGxmColorSurface g_cs_m, g_cs_d[GXM_DISP_BUFS];
static SceGxmDepthStencilSurface g_ds_m[TG_DZ_N], g_ds_d, g_ds_s;
static SceGxmSyncObject *g_sync_d[GXM_DISP_BUFS];
static SceGxmTexture g_tex_m;                         /* M as a sampler: LINEAR, CLAMP */
static int g_built;                                   /* init got far enough that shutdown has something to free */

static GxmProg *g_pclear, *g_pblit, *g_pcopy;
static SceGxmVertexProgram *g_vp_clear, *g_vp_blit, *g_vp_copy;
static SceGxmFragmentProgram *g_fp_clear, *g_fp_nowrite, *g_fp_blit, *g_fp_copy;
static const SceGxmProgramParameter *g_u_cpos, *g_u_cdepth, *g_u_ccol;                       /* clear: position, u_clear_depth, u_clear_color */
static const SceGxmProgramParameter *g_u_rect, *g_u_uvr, *g_u_zz, *g_u_sel, *g_u_cfg, *g_u_qlv;   /* copy */
static uint32_t g_unit_blit, g_unit_copy;
static int g_copy_q;                                  /* the copy program is the quantising form */

static char g_open;                                   /* the open scene: 0 none, 'm' M, 'c' copy, 'd' display */
static int g_cur_w, g_cur_h;                          /* the open scene's target size (the fence quad needs it) */
static int g_bs_fail_run;                             /* consecutive BeginScene failures */
static unsigned g_m_scene_k;                          /* M scenes so far this frame (render-target rotation) */
static unsigned g_rt_used[GXM_M_RT_N];                /* scenes served per M render target this frame */

static unsigned g_shot_frames[TG_SHOT_MAX];
static int g_shot_n;

/* counters (report tokens) */
static unsigned c_efb_copy, c_efb_copy_refused, c_efb_copy_refused_z, c_efb_copy_refused_rect, c_efb_copy_noconv, c_efb_copy_clamped, c_efb_copy_below480;
static unsigned c_efb_copy_scene_fail, c_efb_copy_ring_fail, c_efb_copy_recreated, c_efb_copy_new_fail, c_efb_copy_fmt_changed;
static unsigned c_readback, c_readback_bytes, c_readback_us, c_readback_null;
static unsigned c_clear, c_clear_rect, c_clear_rect_empty, c_clear_skip;
static unsigned c_peekz, c_peekz_sync, c_peekz_noframe, c_begin_frame, c_end_frame;
static unsigned c_scene_fail, c_end_fail, c_end_c, c_end_x, c_end_p, c_end_r, c_ring_fail, c_present_skipped, c_present_dead, c_disp_fail;
static unsigned c_scratch_grow, c_scratch_fail, c_shots, c_shot_fail;

/* ---- small helpers ---- */
static int tg_fail(const char *what)
{
    gxm_log("[gxm] INIT FAILED at: target: %s [nonzero %d] -- no picture will be produced; this is NOT a pass", what, g_gx.nonzero);
    return -1;
}

static int tg_read_file(const char *path, char *buf, int cap)
{
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (f < 0) return -1;
    const int n = sceIoRead(f, buf, (SceSize) (cap - 1));
    sceIoClose(f);
    return n < 0 ? 0 : n;
}

static void tg_frame_colour(float out[4], unsigned f)     /* M1's: a function of the frame number alone, so a stale picture is a different colour */
{
    for (int k = 0; k < 3; ++k) out[k] = (float) ((53u * f + 61u * (unsigned) k + 20u) & 255u) / 255.0f;
    out[3] = 1.0f;
}

static const SceGxmProgramParameter *tg_param(const SceGxmProgram *p, const char *name)
{
    return p ? sceGxmProgramFindParameterByName(p, name) : NULL;
}

static uint32_t tg_sampler_unit(const SceGxmProgram *fg)
{
    const SceGxmProgramParameter *p = tg_param(fg, "tex");
    return p ? sceGxmProgramParameterGetResourceIndex(p) : 0u;
}

/* a depth/stencil surface over two fresh blocks (CDRAM, render surfaces). 0 ok. */
static int tg_alloc_ds(GxmBlk *depth, GxmBlk *sten, SceGxmDepthStencilSurface *ds, int w, int h)
{
    const uint32_t aw = (uint32_t) GXM_ALIGN(w, 64), ah = (uint32_t) GXM_ALIGN(h, 64);
    if (gxm_blk_alloc(depth, GXM_MEM_CDRAM, 4u * aw * ah, 256)) return -1;
    if (gxm_blk_alloc(sten, GXM_MEM_CDRAM, aw * ah, 256)) { gxm_blk_free_now(depth); return -1; }
    if (GXM_CHK(sceGxmDepthStencilSurfaceInit(ds, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, aw, depth->p, sten->p))) {
        gxm_blk_free_now(depth); gxm_blk_free_now(sten);
        return -1;
    }
    return 0;
}

/* ---- scene state shared by every quad this module draws ---- */
static void tg_quad_state(int w, int h, int depth_write)
{
    sceGxmSetCullMode(g_gx.ctx, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(g_gx.ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(g_gx.ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_gx.ctx, depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(g_gx.ctx, depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontDepthBias(g_gx.ctx, 0, 0);     /* the draw module's shadow z-bias may still be set in this scene: it must not move a clear's depth */
    sceGxmSetBackDepthBias(g_gx.ctx, 0, 0);
    /* top-row-first: yScale < 0 (misc.c:329-332); window z = 0.5 + 0.5 * clip z */
    sceGxmSetViewport(g_gx.ctx, (float) w * 0.5f, (float) w * 0.5f, (float) h * 0.5f, -(float) h * 0.5f, 0.5f, 0.5f);
    sceGxmSetRegionClip(g_gx.ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, (unsigned) w - 1u, (unsigned) h - 1u);
}

static int tg_draw_fan(void)
{
    const int rc = gxm_sce_draw(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, g_idx, 4);
    if (rc == 0) ++g_gx.st.internal_quads;
    return rc;
}

/* a flat quad through the clear program: rect = NDC {xL, xR, yT, yB}, clip z, colour. 0 ok (the draw was issued). */
static int tg_quad_clear(const float rect[4], float clipz, const float col[4], SceGxmFragmentProgram *fp, int depth_write, int w, int h)
{
    void *vub = NULL, *fub = NULL;
    tg_quad_state(w, h, depth_write);
    sceGxmSetVertexProgram(g_gx.ctx, g_vp_clear);
    sceGxmSetFragmentProgram(g_gx.ctx, fp);
    if (GXM_CHK(sceGxmReserveVertexDefaultUniformBuffer(g_gx.ctx, &vub)) || !vub) return -1;
    GXM_CHK(sceGxmSetUniformDataF(vub, g_u_cpos, 0, 4, rect));
    GXM_CHK(sceGxmSetUniformDataF(vub, g_u_cdepth, 0, 1, &clipz));
    if (GXM_CHK(sceGxmReserveFragmentDefaultUniformBuffer(g_gx.ctx, &fub)) || !fub) return -1;
    GXM_CHK(sceGxmSetUniformDataF(fub, g_u_ccol, 0, 4, col));
    return tg_draw_fan();
}

/* a textured fan through the precompiled blit program (position + texcoord stream); samples M. 0 ok. */
static int tg_quad_blit_m(const float rect[4], const float uv[4], int w, int h)
{
    float *v = (float *) gxm_ring_alloc(16u * sizeof(float));
    if (!v) { ++c_ring_fail; return -1; }
    gxm_tgt_blit_verts(v, rect, uv);
    tg_quad_state(w, h, 0);
    sceGxmSetVertexProgram(g_gx.ctx, g_vp_blit);
    sceGxmSetFragmentProgram(g_gx.ctx, g_fp_blit);
    if (GXM_CHK(sceGxmSetFragmentTexture(g_gx.ctx, g_unit_blit, &g_tex_m))) return -1;      /* AFTER the fragment program (gxmprobe review round 3) */
    if (GXM_CHK(sceGxmSetVertexStream(g_gx.ctx, 0, v))) return -1;
    return tg_draw_fan();
}

/* ---- scenes ---- */
static int tg_begin(char kind, SceGxmRenderTarget *rt, SceGxmSyncObject *fsync, const SceGxmColorSurface *cs, const SceGxmDepthStencilSurface *ds, int w, int h)
{
    if (g_gx.dead) return -1;
    if (GXM_CHK(sceGxmBeginScene(g_gx.ctx, 0, rt, NULL, NULL, fsync, cs, ds))) {
        ++c_scene_fail;
        if (++g_bs_fail_run >= 3) gxm_abort("sceGxmBeginScene failed %d times in a row (last: '%c' scene)", g_bs_fail_run, kind);
        return -1;
    }
    g_bs_fail_run = 0;
    g_open = kind;
    g_cur_w = w; g_cur_h = h;
    ++g_gx.epoch;
    g_gx.scene_draws = 0;
    ++g_gx.st.scenes;
    if (kind == 'm') ++g_gx.st.scenes_m; else if (kind == 'c') ++g_gx.st.scenes_copy; else ++g_gx.st.scenes_display;
    return 0;
}

/* the fence quad: a scene that holds no draw is not known to signal its notification (c2floor :701-704). Colour writes OFF, no blend, depth ALWAYS /
 * no write, 1x1 pixel at (0,0): it cannot change a pixel. */
static void tg_fence_quad(void)
{
    const float rect[4] = { -1.0f, -1.0f + 2.0f / (float) g_cur_w, 1.0f, 1.0f - 2.0f / (float) g_cur_h };
    const float col[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (tg_quad_clear(rect, 0.0f, col, g_fp_nowrite, 0, g_cur_w, g_cur_h) == 0) ++g_gx.st.fence_quads;
}

/* DEBUG (gxm_mshot.txt = 2): after every scene of the first shot frame, wait for it and count the non-black pixels of M's picture rows. Not part of any gate. */
static int g_trace = -1;
int gxm_target_trace_frame(void)
{
    if (g_trace < 0) {
        char mc = 0;
        SceUID mf = sceIoOpen("ux0:data/stairfax/gxm_mshot.txt", SCE_O_RDONLY, 0777);
        if (mf >= 0) { sceIoRead(mf, &mc, 1); sceIoClose(mf); }
        g_trace = (mc == '2');
    }
    return g_trace > 0 && g_shot_n && g_gx.frames == g_shot_frames[0];
}
static void tg_trace_scene(char kind, char why, unsigned draws)
{
    if (!gxm_target_trace_frame() || !b_mc.p) return;
    if (gxm_frame_wait_scene(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) return;
    unsigned nb = 0;
    for (int y = 0; y < g_gx.vh; ++y) {
        const uint32_t *s = (const uint32_t *) ((const uint8_t *) b_mc.p + (size_t) y * (size_t) g_gx.rw * 4u);
        for (int x = 0; x < g_gx.rw; ++x) nb += (s[x] & 0x00ffffffu) != 0;
    }
    gxm_log("[gxm-trace] frame=%u scene seq=%u kind=%c ended_by=%c draws=%u M_nonblack_px=%u", g_gx.frames, g_gx.nval, kind, why, draws, nb);
}

typedef struct { const void *t; int x0, y0, x1, y1, dw, dh, fmt; unsigned mdraws, draw_at, first_use, clr_w, clr_h, clr_full; } TgCl;
static TgCl s_cl[48]; static int s_cl_n, s_cl_on = -1; static unsigned s_cl_frame, s_cl_draws, s_cl_done, s_cl_lastop;
static unsigned s_cl_seq0, s_cl_seq1;   /* the scene seqs of the sampled frame: their GPU times are printed one frame later */
static unsigned s_cl_printed_gpu;
static uint16_t s_cl_enddraw[512];   /* per scene of the sampled frame: the frame draw ordinal when it ended */
static int tg_cl_active(void);
void gxm_target_scene_end(char why)
{
    if (!g_open) return;
    if (g_gx.dead) { g_open = 0; return; }       /* dead: nothing more is submitted */
    const char kind0 = g_open;
    const unsigned draws0 = g_gx.scene_draws;
    if (g_gx.scene_draws == 0) tg_fence_quad();
    { extern char g_gxm_end_kind; g_gxm_end_kind = kind0; }   /* the [gxm-gpu] split by scene kind */
    if (gxm_frame_end_scene() != 0) ++c_end_fail;
    if (s_cl_seq0 && !s_cl_done && g_gx.nval >= s_cl_seq0) s_cl_enddraw[(g_gx.nval - s_cl_seq0) & 511u] = (uint16_t) s_cl_draws;
    g_open = 0;
    tg_trace_scene(kind0, why, draws0);
    switch (why) { case 'c': ++c_end_c; break; case 'x': ++c_end_x; break; case 'p': ++c_end_p; break; default: ++c_end_r; break; }
}

/* the M scene, init-safe (does not look at g_gx.up) */
static int tg_scene_m(void)
{
    if (g_gx.dead) return -1;
    if (g_open == 'm') return 0;
    gxm_target_scene_end('r');                   /* a copy/display scene is never left open across calls; defensive */
    /* scene k of the frame -> render target (k/8)%N; one served beyond its scenesPerFrame is rt_overflow (it may stall BeginScene) */
    const unsigned idx = (g_m_scene_k / (unsigned) g_rt_cap) % (unsigned) g_n_rt_m;
    if (tg_begin('m', g_rt_m[idx], NULL, &g_cs_m, &g_ds_m[g_dz_cur], g_gx.rw, g_gx.rh)) return -1;
    ++g_m_scene_k;
    if (++g_rt_used[idx] > (unsigned) g_rt_cap) ++g_gx.st.rt_overflow;
    if (g_rt_used[idx] > g_gx.st.rt_scenes_max) g_gx.st.rt_scenes_max = g_rt_used[idx];
    return 0;
}

int gxm_target_scene_m(void)
{
    if (gxm_dead()) return -1;
    return tg_scene_m();
}

/* ---- init ---- */
static int tg_make_programs(void)
{
    g_pclear = gxm_shader_util_precompiled("clear", clear_v, clear_f);
    g_pblit = gxm_shader_util_precompiled("blit", blit_v, blit_f);
    (void) size_clear_v; (void) size_clear_f; (void) size_blit_v; (void) size_blit_f;
    if (!g_pclear || !g_pblit) return tg_fail("util programs (clear, blit)");
    g_u_cpos = tg_param(g_pclear->vg, "position");
    g_u_cdepth = tg_param(g_pclear->vg, "u_clear_depth");
    g_u_ccol = tg_param(g_pclear->fg, "u_clear_color");
    if (!g_u_cpos || !g_u_cdepth || !g_u_ccol) return tg_fail("clear program parameter lookup");
    g_unit_blit = tg_sampler_unit(g_pblit->fg);

    static const GxmAttr a_blit[2] = {
        { "position", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },
        { "texcoord", 8 * sizeof(float), SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },     /* vitaGL's blit layout: 4 positions, then 4 texcoords, stride 8 */
    };
    g_vp_clear = gxm_shader_vp(g_pclear, NULL, 0, 0);                           /* INDEX-driven: no attributes (header_requests: n == 0 must pass NULL/0) */
    g_vp_blit = gxm_shader_vp(g_pblit, a_blit, 2, 2 * (int) sizeof(float));
    g_fp_clear = gxm_shader_fp(g_pclear, RHI_ALPHA_OPAQUE);
    g_fp_nowrite = gxm_shader_fp_nowrite(g_pclear);
    g_fp_blit = gxm_shader_fp(g_pblit, RHI_ALPHA_OPAQUE);
    if (!g_vp_clear || !g_vp_blit || !g_fp_clear || !g_fp_nowrite || !g_fp_blit) return tg_fail("clear/blit vertex or fragment programs");

    /* the EFB copy program: Cg on the console only. NULL on the emulator -> the blit stands in (counted efb_copy_noconv) */
    g_copy_q = g_gx.copyfilter_on;
    g_pcopy = gxm_shader_util_cg("efbcopy", TG_EFB_VS, g_copy_q ? TG_EFB_FS_COPY_Q : TG_EFB_FS_COPY);
    if (g_pcopy) {
        static const GxmAttr a_copy[1] = { { "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 } };
        g_u_rect = tg_param(g_pcopy->vg, "rect");
        g_u_uvr = tg_param(g_pcopy->vg, "uvr");
        g_u_zz = tg_param(g_pcopy->vg, "zz");                  /* dead-uniform elimination: any of these may be NULL and is then skipped, not failed */
        g_u_sel = tg_param(g_pcopy->fg, "sel");
        g_u_cfg = tg_param(g_pcopy->fg, "cfg");
        g_u_qlv = g_copy_q ? tg_param(g_pcopy->fg, "qlv") : NULL;
        g_unit_copy = tg_sampler_unit(g_pcopy->fg);
        g_vp_copy = gxm_shader_vp(g_pcopy, a_copy, 1, 2 * (int) sizeof(float));
        g_fp_copy = gxm_shader_fp(g_pcopy, RHI_ALPHA_OPAQUE);
        if (!g_vp_copy || !g_fp_copy) { gxm_log("[gxm] target: the EFB copy program compiled but its vertex/fragment program failed: copies fall back to the blit (noconv)"); g_pcopy = NULL; }
    }
    gxm_log("[gxm] target: util programs clear + blit registered; EFB copy program %s (%s)", g_pcopy ? "compiled" : "ABSENT",
            g_pcopy ? (g_copy_q ? "quantising form" : "plain form: copyfilter_off") : "copies use the blit: format conversion NOT exercised, counted efb_copy_noconv");
    return 0;
}

static void tg_free_all(int gpu_idle);

int gxm_target_init(const RhiCreateInfo *info)
{
    (void) info;
    /* ---- geometry: render_res.txt + efb528_off.txt exactly as rhi_vita_efb.inc:78-111 ---- */
    {   char b[48];
        int w = GXM_DISP_W, h = GXM_DISP_H, valid = 0;
        memset(b, 0, sizeof b);
        const int len = tg_read_file(TG_DIR "render_res.txt", b, (int) sizeof b);
        const char *why = gxm_tgt_res_parse(len >= 0, b, len, &w, &h, &valid);
        if (!valid) { w = GXM_DISP_W; h = GXM_DISP_H; }
        gxm_log("[res] render target %dx%d (%s)", w, h, why);
        char o[8];
        memset(o, 0, sizeof o);
        const int ol = tg_read_file(TG_DIR "efb528_off.txt", o, (int) sizeof o);
        const int off = ol > 0 && o[0] == '1';
        int vh, rh, e528;
        gxm_tgt_geom(h, off, &vh, &rh, &e528);
        g_gx.rw = w; g_gx.vh = vh; g_gx.rh = rh; g_gx.efb528 = e528;
        gxm_log("[efb528] %s: render target %dx%d, picture rows %d, extra rows %d (GX line 528 = pixel row %d) | M colour +%u KB, depth +%u KB | %s",
                e528 ? "ON" : "OFF", w, rh, vh, rh - vh, 528 * vh / 480,
                (unsigned) ((size_t) w * (size_t) (rh - vh) * 4u / 1024u),
                (unsigned) ((size_t) GXM_ALIGN(w, 64) * (size_t) (GXM_ALIGN(rh, 64) - GXM_ALIGN(vh, 64)) * 4u / 1024u),
                off ? "efb528_off.txt: M is exactly the picture, a line below 480 is clamped away" : "no efb528_off.txt: lines up to 528 have pixels");
    }
    g_built = 1;
    const int rw = g_gx.rw, rh = g_gx.rh;

    /* ---- M: CDRAM colour A8B8G8R8 LINEAR stride rw; DF32M_S8 LINEAR depth/stencil, ForceLoad/Store BEFORE the first BeginScene, depth prefilled 0x80 ---- */
    if (gxm_blk_alloc(&b_mc, GXM_MEM_CDRAM, 4u * (uint32_t) rw * (uint32_t) rh, 256)) { tg_free_all(1); return tg_fail("M colour memory"); }
    memset(b_mc.p, 0, 4u * (size_t) rw * (size_t) rh);
    if (GXM_CHK(sceGxmColorSurfaceInit(&g_cs_m, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                       SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, (uint32_t) rw, (uint32_t) rh, (uint32_t) rw, b_mc.p))) { tg_free_all(1); return tg_fail("M colour surface"); }
    for (int z = 0; z < TG_DZ_N; ++z) {
        if (tg_alloc_ds(&b_md[z], &b_ms[z], &g_ds_m[z], rw, rh)) { tg_free_all(1); return tg_fail("M depth/stencil"); }
        sceGxmDepthStencilSurfaceSetForceLoadMode(&g_ds_m[z], SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&g_ds_m[z], SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        memset(b_md[z].p, 0x80, 4u * (size_t) GXM_ALIGN(rw, 64) * (size_t) GXM_ALIGN(rh, 64));     /* the DF32M mask-update bit: without it the first scene loads garbage */
        memset(b_ms[z].p, 0, (size_t) GXM_ALIGN(rw, 64) * (size_t) GXM_ALIGN(rh, 64));
        g_dz_seq[z] = 0; g_dz_frame[z] = 0;
    }
    g_dz_cur = 0; g_dz_fcount = 0;
    gxm_log("[gxm] target: M %dx%d colour %u B + depth %u B x %d (ring; DF32M_S8 LINEAR, force load %d store %d, prefill 0x80)", rw, rh, 4u * (unsigned) rw * (unsigned) rh,
            4u * (unsigned) GXM_ALIGN(rw, 64) * (unsigned) GXM_ALIGN(rh, 64), TG_DZ_N,
            (int) sceGxmDepthStencilSurfaceGetForceLoadMode(&g_ds_m[0]), (int) sceGxmDepthStencilSurfaceGetForceStoreMode(&g_ds_m[0]));

    /* M as a sampler: LINEAR / CLAMP */
    if (GXM_CHK(sceGxmTextureInitLinear(&g_tex_m, b_mc.p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (uint32_t) rw, (uint32_t) rh, 0))) { tg_free_all(1); return tg_fail("M texture"); }
    GXM_CHK(sceGxmTextureSetMinFilter(&g_tex_m, SCE_GXM_TEXTURE_FILTER_LINEAR));
    GXM_CHK(sceGxmTextureSetMagFilter(&g_tex_m, SCE_GXM_TEXTURE_FILTER_LINEAR));
    GXM_CHK(sceGxmTextureSetUAddrMode(&g_tex_m, SCE_GXM_TEXTURE_ADDR_CLAMP));
    GXM_CHK(sceGxmTextureSetVAddrMode(&g_tex_m, SCE_GXM_TEXTURE_ADDR_CLAMP));

    /* M's render targets: GXM_M_RT_N of GXM_M_RT_SCENES scenes per frame (arm RT1: one target, scenesPerFrame 1) */
    {   SceGxmRenderTargetParams rp;
        memset(&rp, 0, sizeof rp);
        g_n_rt_m = (g_gx.arm & GXM_ARM_RT1) ? 1 : GXM_M_RT_N;
        g_rt_cap = (g_gx.arm & GXM_ARM_RT1) ? 1 : GXM_M_RT_SCENES;
        rp.flags = 0; rp.width = (uint16_t) rw; rp.height = (uint16_t) rh; rp.scenesPerFrame = (uint16_t) g_rt_cap;
        rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rp.multisampleLocations = 0; rp.driverMemBlock = -1;
        for (int i = 0; i < g_n_rt_m; ++i)
            if (GXM_CHK(sceGxmCreateRenderTarget(&rp, &g_rt_m[i]))) { g_rt_m[i] = NULL; g_n_rt_m = i; tg_free_all(1); return tg_fail("M render target"); }
        gxm_log("[gxm] target: %d M render target(s), scenesPerFrame %d each%s", g_n_rt_m, g_rt_cap, (g_gx.arm & GXM_ARM_RT1) ? " (arm RT1)" : "");
    }

    /* ---- display: render target 960x544 (1 scene), 3 CDRAM buffers + colour surfaces + sync objects, depth surface (M1 :239-261) ---- */
    {   SceGxmRenderTargetParams rp;
        memset(&rp, 0, sizeof rp);
        rp.flags = 0; rp.width = GXM_DISP_W; rp.height = GXM_DISP_H; rp.scenesPerFrame = 1;
        rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rp.multisampleLocations = 0; rp.driverMemBlock = -1;
        if (GXM_CHK(sceGxmCreateRenderTarget(&rp, &g_rt_d))) { g_rt_d = NULL; tg_free_all(1); return tg_fail("display render target"); }
    }
    for (int i = 0; i < GXM_DISP_BUFS; ++i) {
        if (gxm_blk_alloc(&b_disp[i], GXM_MEM_CDRAM, 4u * GXM_DISP_W * GXM_DISP_H, 256)) { tg_free_all(1); return tg_fail("display buffer"); }
        memset(b_disp[i].p, 0, 4u * GXM_DISP_W * GXM_DISP_H);
        if (GXM_CHK(sceGxmColorSurfaceInit(&g_cs_d[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, GXM_DISP_W, GXM_DISP_H, GXM_DISP_W, b_disp[i].p))) { tg_free_all(1); return tg_fail("display colour surface"); }
        if (GXM_CHK(sceGxmSyncObjectCreate(&g_sync_d[i]))) { g_sync_d[i] = NULL; tg_free_all(1); return tg_fail("sceGxmSyncObjectCreate"); }
    }
    if (tg_alloc_ds(&b_dd, &b_ds, &g_ds_d, GXM_DISP_W, GXM_DISP_H)) { tg_free_all(1); return tg_fail("display depth/stencil"); }

    /* the fan quad's index buffer */
    if (gxm_blk_alloc(&b_idx, GXM_MEM_LPDDR, 64, 32)) { tg_free_all(1); return tg_fail("index buffer"); }
    g_idx = (uint16_t *) b_idx.p;
    g_idx[0] = 0; g_idx[1] = 1; g_idx[2] = 2; g_idx[3] = 3;

    /* the picture SHOT list: ux0:data/stairfax/gxm_shot.txt = frame numbers */
    {   char sb[160];
        memset(sb, 0, sizeof sb);
        const int sl = tg_read_file(TG_SHOT_LIST, sb, (int) sizeof sb);
        g_shot_n = 0;
        if (sl > 0) {
            const char *s = sb;
            while (*s && g_shot_n < TG_SHOT_MAX) {
                while (*s && (*s < '0' || *s > '9')) ++s;
                if (!*s) break;
                char *e = NULL;
                const unsigned long v = strtoul(s, &e, 10);
                if (e == s) break;
                g_shot_frames[g_shot_n++] = (unsigned) v;
                s = e;
            }
        }
        gxm_log("[gxm] target: picture shots at %d frame(s)%s", g_shot_n, sl < 0 ? " (gxm_shot.txt absent)" : "");
    }

    if (tg_make_programs()) { tg_free_all(1); return -1; }

    /* ---- the SMOKE scene: one fence-quad scene on M, then the frame module's bounded fence poll (c2floor smoke_fence) ---- */
    if (tg_scene_m()) { tg_free_all(1); return tg_fail("smoke scene: BeginScene on M"); }
    gxm_target_scene_end('r');                                         /* scene_draws == 0 -> the fence quad, then gxm_frame_end_scene() */
    if (gxm_frame_smoke(g_gx.nval) != 0) { tg_free_all(0); return tg_fail("smoke fence (neither the notification nor sceGxmFinish usable)"); }
    g_m_scene_k = 0;                                                   /* the smoke scene is not part of any frame */
    memset(g_rt_used, 0, sizeof g_rt_used);
    gxm_log("[gxm] target: init OK: M %dx%d (picture %d rows) | display %d buffers %dx%d | smoke scene fenced (seq %u, fence_finish %d)",
            rw, rh, g_gx.vh, GXM_DISP_BUFS, GXM_DISP_W, GXM_DISP_H, g_gx.nval, g_gx.fence_finish);
    return 0;
}

/* gpu_idle: the GPU has finished with every block (the frame module waited before calling shutdown). 0 = free NOTHING the GPU may read (leak on purpose). */
static void tg_free_all(int gpu_idle)
{
    if (!gpu_idle) return;
    for (int i = GXM_DISP_BUFS - 1; i >= 0; --i)
        if (g_sync_d[i]) { GXM_CHK(sceGxmSyncObjectDestroy(g_sync_d[i])); g_sync_d[i] = NULL; }
    if (g_rt_d) { GXM_CHK(sceGxmDestroyRenderTarget(g_rt_d)); g_rt_d = NULL; }
    for (int i = 0; i < GXM_M_RT_N; ++i)
        if (g_rt_m[i]) { GXM_CHK(sceGxmDestroyRenderTarget(g_rt_m[i])); g_rt_m[i] = NULL; }
    g_n_rt_m = 0;
    gxm_blk_free_now(&b_idx); g_idx = NULL;
    gxm_blk_free_now(&b_ss); gxm_blk_free_now(&b_sd);
    gxm_blk_free_now(&b_ds); gxm_blk_free_now(&b_dd);
    for (int i = GXM_DISP_BUFS - 1; i >= 0; --i) gxm_blk_free_now(&b_disp[i]);
    for (int z = TG_DZ_N - 1; z >= 0; --z) { gxm_blk_free_now(&b_ms[z]); gxm_blk_free_now(&b_md[z]); g_dz_frame[z] = 0; }
    gxm_blk_free_now(&b_mc);
    g_scratch_aw = g_scratch_ah = 0;
    g_built = 0;
}

void gxm_target_shutdown(void)
{
    if (!g_built) return;
    if (g_gx.dead) {                                  /* the GPU may still be reading: free nothing (GXM_M2.md 5, abort policy) */
        gxm_log("[gxm] target: shutdown after ABORT: nothing freed (the GPU may still read it)");
        return;
    }
    if (g_open) {                                     /* a scene left open would never be submitted: end it and wait for it (bounded) before anything is freed */
        gxm_target_scene_end('r');
        if (gxm_frame_wait_scene(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) { gxm_abort("target shutdown: the last scene never finished"); return; }
    }
    tg_free_all(1);
}

/* ---- the RhiOps ---- */
void gxm_op_beginFrame(RhiInstance *r) { (void) r; ++c_begin_frame; }
void gxm_op_endFrame(RhiInstance *r) { (void) r; ++c_end_frame; }

/* clear (rhi_vita.c:410-425): the whole of M, colour + depth 1.0 (far), depth func ALWAYS + write ENABLED, colour mask ALL */
void gxm_op_clear(RhiInstance *r, float cr, float cg, float cb, float ca)
{
    (void) r;
    if (gxm_dead()) return;
    ++c_clear;
    if (tg_scene_m()) { ++c_clear_skip; return; }
    static const float full[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float col[4] = { cr, cg, cb, ca };
    if (tg_quad_clear(full, 1.0f, col, g_fp_clear, 1, g_gx.rw, g_gx.rh)) ++c_clear_skip;
    gxm_state_dirty();
}

/* clearRect (rhi_vita.c:535-558) */
void rhi_gxm_clearRect(int x, int y, int w, int h, uint32_t rgba, float z)
{
    if (gxm_dead()) return;
    GxmPx px;
    if (!gxm_tgt_clear_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh, g_gx.efb528, &px)) { ++c_clear_rect_empty; return; }
    ++c_clear_rect;
    if (s_cl_lastop == 2 && s_cl_n && tg_cl_active()) { TgCl *e = &s_cl[s_cl_n - 1]; e->clr_w = (unsigned) (px.x1 - px.x0); e->clr_h = (unsigned) (px.y1 - px.y0);
                                                     e->clr_full = px.x0 <= 0 && px.y0 <= 0 && px.x1 >= g_gx.rw && px.y1 >= g_gx.vh; s_cl_lastop = 3; }
    if (tg_scene_m()) { ++c_clear_skip; return; }
    float rect[4], col[4];
    gxm_tgt_px_ndc(&px, g_gx.rw, g_gx.rh, rect);
    gxm_tgt_rgba_f(rgba, col);
    if (tg_quad_clear(rect, gxm_tgt_clear_clipz(z), col, g_fp_clear, 1, g_gx.rw, g_gx.rh)) ++c_clear_skip;
    gxm_state_dirty();
}

/* readEfbRect (rhi_vita.c:503-533): M's rectangle as malloc'd RGBA8, BOTTOM row first. No depth clear: ForceLoad/Store keeps the depth. */
unsigned char *rhi_gxm_readEfbRect(int x, int y, int w, int h, int *ow, int *oh)
{
    if (gxm_dead()) return NULL;
    if (w <= 0 || h <= 0) { ++c_readback_null; return NULL; }
    const EfbRectPx rp = efb_rect_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh);
    if (rp.x1 <= rp.x0 || rp.y1 <= rp.y0) { ++c_readback_null; return NULL; }
    const uint64_t t0 = gxm_now_us();
    gxm_target_scene_end('x');
    if (gxm_frame_wait_scene(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) { gxm_abort("readEfbRect: the scene fence never reached %u", g_gx.nval); return NULL; }
    const size_t nb = (size_t) (rp.x1 - rp.x0) * (size_t) (rp.y1 - rp.y0) * 4u;
    unsigned char *buf = (unsigned char *) malloc(nb);
    if (!buf) { ++c_readback_null; return NULL; }
    gxm_tgt_rows_bottom_first(buf, (const uint8_t *) b_mc.p, (size_t) g_gx.rw * 4u, rp.x0, rp.y0, rp.x1, rp.y1);
    *ow = rp.x1 - rp.x0; *oh = rp.y1 - rp.y0;
    ++c_readback; c_readback_bytes += (unsigned) nb; c_readback_us += (unsigned) (gxm_now_us() - t0);
    return buf;
}

/* the shared scratch depth/stencil of the copy render targets (contents never read): grows to the largest aw/ah, the old one is freed deferred */
static int tg_scratch(int aw, int ah)
{
    int nw = GXM_ALIGN(aw < TG_SCRATCH_MIN ? TG_SCRATCH_MIN : aw, 64), nh = GXM_ALIGN(ah < TG_SCRATCH_MIN ? TG_SCRATCH_MIN : ah, 64);
    if (b_sd.p && g_scratch_aw >= nw && g_scratch_ah >= nh) return 0;
    if (nw < g_scratch_aw) nw = g_scratch_aw;
    if (nh < g_scratch_ah) nh = g_scratch_ah;
    GxmBlk nd, ns;
    SceGxmDepthStencilSurface nds;
    memset(&nd, 0, sizeof nd); memset(&ns, 0, sizeof ns);
    if (tg_alloc_ds(&nd, &ns, &nds, nw, nh)) { ++c_scratch_fail; return -1; }
    GxmBlk od = b_sd, os = b_ss;
    b_sd = nd; b_ss = ns; g_ds_s = nds;
    g_scratch_aw = nw; g_scratch_ah = nh;
    if (od.p) gxm_blk_free_deferred(&od);
    if (os.p) gxm_blk_free_deferred(&os);
    ++c_scratch_grow;
    return 0;
}

/* the copy quad into `c`'s render target. 0 ok. */
static int tg_copy_draw(GxmTex *c, const float uvr[4], const float sel[4], const float cfg[4], const float qlv[4])
{
    if (tg_begin('c', c->rt, NULL, &c->cs, &g_ds_s, c->aw, c->ah)) return -1;
    if (g_pcopy) {
        float *v = (float *) gxm_ring_alloc(8u * sizeof(float));
        if (!v) { ++c_efb_copy_ring_fail; return -1; }                      /* the scene still ends with a fence quad; the copy counts as failed */
        gxm_tgt_unit_verts(v);
        static const float rect[4] = { -1.0f, 1.0f, 1.0f, -1.0f };           /* aPos.y = 0 -> clip y = +1 -> the destination's TOP row */
        const float zz = 0.0f;
        void *vub = NULL, *fub = NULL;
        tg_quad_state(c->aw, c->ah, 0);
        sceGxmSetVertexProgram(g_gx.ctx, g_vp_copy);
        sceGxmSetFragmentProgram(g_gx.ctx, g_fp_copy);
        if (GXM_CHK(sceGxmReserveVertexDefaultUniformBuffer(g_gx.ctx, &vub)) || !vub) return -1;
        if (g_u_rect) GXM_CHK(sceGxmSetUniformDataF(vub, g_u_rect, 0, 4, rect));
        if (g_u_uvr) GXM_CHK(sceGxmSetUniformDataF(vub, g_u_uvr, 0, 4, uvr));
        if (g_u_zz) GXM_CHK(sceGxmSetUniformDataF(vub, g_u_zz, 0, 1, &zz));
        if (GXM_CHK(sceGxmReserveFragmentDefaultUniformBuffer(g_gx.ctx, &fub)) || !fub) return -1;
        if (g_u_sel) GXM_CHK(sceGxmSetUniformDataF(fub, g_u_sel, 0, 4, sel));
        if (g_u_cfg) GXM_CHK(sceGxmSetUniformDataF(fub, g_u_cfg, 0, 4, cfg));
        if (g_u_qlv) GXM_CHK(sceGxmSetUniformDataF(fub, g_u_qlv, 0, 4, qlv));
        if (GXM_CHK(sceGxmSetFragmentTexture(g_gx.ctx, g_unit_copy, &g_tex_m))) return -1;     /* AFTER the fragment program */
        if (GXM_CHK(sceGxmSetVertexStream(g_gx.ctx, 0, v))) return -1;
        return tg_draw_fan();
    } else {
        /* no compiler (emulator): the precompiled blit, identity conversion */
        static const float rect[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        ++c_efb_copy_noconv;
        return tg_quad_blit_m(rect, uvr, c->aw, c->ah);
    }
}

/* copyEfb (rhi_vita_efb.inc:891-1038) */
/* ---- copy log (2026-10-20, opt-in: ux0:data/stairfax/gxm_copylog.txt = "<frame>"): for ONE frame, every EFB copy with its rect, the
 * M draws before it, the clear that follows it, and the first draw of the SAME frame that samples its destination. Question it answers:
 * which of the ~10 copies/frame really need M split mid-frame (sampled later in the frame) and which reopen M only to clear it. */
static int tg_cl_active(void)
{
    if (s_cl_on < 0) { char b[16] = { 0 }; SceUID f = sceIoOpen("ux0:data/stairfax/gxm_copylog.txt", SCE_O_RDONLY, 0); s_cl_on = 0;
                       if (f >= 0) { sceIoRead(f, b, sizeof b - 1); sceIoClose(f); s_cl_frame = (unsigned) strtoul(b, NULL, 10); s_cl_on = s_cl_frame > 0; } }
    if (s_cl_on > 0 && s_cl_done && !s_cl_printed_gpu && g_gx.frames > s_cl_frame + 3u) {   /* 3 frames later every scene of it has finished */
        extern volatile uint32_t g_gg_dur[8192]; char b[1400]; int n = 0; uint32_t tot = 0;
        n += snprintf(b + n, sizeof b - n, "[gxm-copylog] frame %u scene GPU us (seq %u..%u, kind):", s_cl_frame, s_cl_seq0, s_cl_seq1);
        gxm_log("%s", b); n = 0;
        for (unsigned q = s_cl_seq0; q <= s_cl_seq1; ++q) {
            tot += g_gg_dur[q % 8192u];
            n += snprintf(b + n, sizeof b - n, " %u@%u", (unsigned) g_gg_dur[q % 8192u], (unsigned) s_cl_enddraw[(q - s_cl_seq0) & 511u]);
            if (n > (int) sizeof b - 40) { gxm_log("[gxm-copylog]  us@enddraw:%s", b); n = 0; }
        }
        if (n) gxm_log("[gxm-copylog]  us@enddraw:%s", b);
        gxm_log("[gxm-copylog]  total %u us", (unsigned) tot); s_cl_printed_gpu = 1;
    }
    if (!s_cl_on || s_cl_done) return 0;
    if (g_gx.frames == s_cl_frame && !s_cl_seq0) s_cl_seq0 = g_gx.nval + 1u;
    if (g_gx.frames > s_cl_frame) {
        s_cl_seq1 = g_gx.nval;
        gxm_log("[gxm-copylog] frame %u: %d copies, %u draws", s_cl_frame, s_cl_n, s_cl_draws);
        for (int i = 0; i < s_cl_n; ++i) { const TgCl *c = &s_cl[i];
            gxm_log("[gxm-copylog]  #%d rect %d,%d..%d,%d (%dx%d px of %dx%d) -> %dx%d fmt %02x | M draws before %u, at frame draw %u | clear after: %ux%u%s | first use in frame: %s%u",
                    i, c->x0, c->y0, c->x1, c->y1, c->x1 - c->x0, c->y1 - c->y0, g_gx.rw, g_gx.vh, c->dw, c->dh, c->fmt, c->mdraws, c->draw_at, c->clr_w, c->clr_h,
                    c->clr_full ? " FULL" : "", c->first_use ? "draw " : "NONE", c->first_use ? c->first_use : 0u); }
        s_cl_done = 1; return 0;
    }
    return g_gx.frames == s_cl_frame;
}
unsigned gxm_target_cl_draw(void) { return (s_cl_on > 0 && !s_cl_done && g_gx.frames == s_cl_frame) ? s_cl_draws : 0u; }
void gxm_target_note_draw(void *const *tex, int n)
{
    if (s_cl_on == 0 || s_cl_printed_gpu) return;
    if (!tg_cl_active()) return;
    if (s_cl_draws && g_open == 'm' && !(s_cl_draws % 16u)) gxm_target_scene_end('r');   /* sampled frame: 16-draw chunks (per-draw splits overran the queue, run 2026-10-20) */   /* sampled frame only: 16-draw chunks, each timed by [gxm-gpu] */
    ++s_cl_draws; s_cl_lastop = 1;
    for (int k = 0; k < n; ++k) if (tex[k]) for (int i = 0; i < s_cl_n; ++i) if (s_cl[i].t == tex[k] && !s_cl[i].first_use) s_cl[i].first_use = s_cl_draws;
}

/* ---- copy-edge diagnostic (2026-10-27, opt-in: ux0:data/stairfax/gxm_copyedge.txt, first char '1'): shadow "dangling lines" = a non-zero EDGE texel of a copy
 * texture smeared by CLAMP over a whole ground triangle.  Each copy is queued with the scene seq that wrote it; once the GPU has passed that seq (*fnot, as
 * peekZ; in fence_finish mode: a later present) the valid dw x dh region's outer two rings are counted (gxm_copyedge.h) and one line per 60 presents is logged.
 * Marker absent/0: the only cost is the s_ce_on branch.  A texture that is re-copied or freed while pending is scanned first if finished, else dropped (counted). */
typedef struct { GxmTex *t; uint32_t seq, frame; int x0, y0, x1, y1, dw, dh, fmt, below; } TgCe;
static TgCe s_ce[48]; static int s_ce_n, s_ce_on = -1;
static unsigned s_ce_presents, s_ce_dropped, s_ce_logged;
static struct { unsigned copies, shadow, r0_nz, r0_tot, r0_max, r1_nz, r1_tot, worst_k0, worst_w, worst_h, worst_fmt; } s_ce_acc;
static int tg_ce_active(void)
{
    if (s_ce_on < 0) { char b[4] = { 0 }; SceUID f = sceIoOpen("ux0:data/stairfax/gxm_copyedge.txt", SCE_O_RDONLY, 0); s_ce_on = 0;
                       if (f >= 0) { sceIoRead(f, b, 1); sceIoClose(f); s_ce_on = b[0] == '1'; } }
    return s_ce_on > 0;
}
static int tg_ce_ready(const TgCe *e)
{
    if (e->frame == g_gx.frames) return 0;                                          /* never in the frame that issued it */
    return g_gx.fence_finish || (g_gx.fnot && (int) (*g_gx.fnot - e->seq) >= 0);
}
static void tg_ce_scan(const TgCe *e)
{
    if (!e->t->mem.p) return;
    const GxmCopyEdge r = gxm_copyedge_scan((const uint32_t *) e->t->mem.p, e->t->aw, e->dw, e->dh);
    ++s_ce_acc.copies; s_ce_acc.shadow += e->below != 0;
    s_ce_acc.r0_nz += r.r0_nz; s_ce_acc.r0_tot += r.r0_total; s_ce_acc.r1_nz += r.r1_nz; s_ce_acc.r1_tot += r.r1_total;
    if (r.r0_max > s_ce_acc.r0_max) s_ce_acc.r0_max = r.r0_max;
    if (r.r0_nz > s_ce_acc.worst_k0) { s_ce_acc.worst_k0 = r.r0_nz; s_ce_acc.worst_w = (unsigned) e->dw; s_ce_acc.worst_h = (unsigned) e->dh; s_ce_acc.worst_fmt = (unsigned) e->fmt; }
    if (r.r0_nz && s_ce_logged < 8) { ++s_ce_logged;
        gxm_log("[copyedge] hot edge #%u: rect %d,%d..%d,%d%s -> %dx%d fmt %02x | ring0 nonzero %u of %u (max %u), ring1 nonzero %u of %u",
                s_ce_logged, e->x0, e->y0, e->x1, e->y1, e->below ? " (shadow pass)" : "", e->dw, e->dh, e->fmt, r.r0_nz, r.r0_total, r.r0_max, r.r1_nz, r.r1_total); }
}
static void tg_ce_drop(int i) { s_ce[i] = s_ce[--s_ce_n]; }
/* every pending entry of `t` (NULL = all) whose GPU work is finished is scanned; the others are dropped when `force` (the texture is about to change or go) */
static void tg_ce_flush(const GxmTex *t, int force)
{
    for (int i = s_ce_n - 1; i >= 0; --i) {
        if (t && s_ce[i].t != t) continue;
        if (tg_ce_ready(&s_ce[i])) { tg_ce_scan(&s_ce[i]); tg_ce_drop(i); }
        else if (force) { ++s_ce_dropped; tg_ce_drop(i); }
    }
}
void gxm_target_copyedge_forget(GxmTex *t) { if (s_ce_on != 0 && tg_ce_active()) tg_ce_flush(t, 1); }
static void tg_ce_present(void)
{
    if (!tg_ce_active()) return;
    tg_ce_flush(NULL, 0);
    if (++s_ce_presents % 60u) return;
    gxm_log("[copyedge] copies %u (shadow %u) | ring0 nonzero %u of %u, max %u | ring1 nonzero %u of %u | worst: %ux%u fmt %02x k0 %u%s", s_ce_acc.copies, s_ce_acc.shadow,
            s_ce_acc.r0_nz, s_ce_acc.r0_tot, s_ce_acc.r0_max, s_ce_acc.r1_nz, s_ce_acc.r1_tot, s_ce_acc.worst_w, s_ce_acc.worst_h, s_ce_acc.worst_fmt, s_ce_acc.worst_k0,
            s_ce_dropped ? " (dropped pending)" : "");
    memset(&s_ce_acc, 0, sizeof s_ce_acc);
}

/* R5 (R5_SPLIT.md D3/D4): the refusals of rhi_gxm_copyEfb that depend on its ARGUMENTS alone (format, empty rectangles), without the counters and without touching a scene -- the render-thread split's front answers a copy
 * with them in the same frame, as today.  Keep the three tests below equal to the first three of rhi_gxm_copyEfb (host/engine/gxsp_lint.py greps both).  Constants only after init: safe on any thread. */
int rhi_gxm_copy_precheck(int x, int y, int w, int h, int dw, int dh, int gxfmt)
{
    float sel[4], cfg[4], qlv[4];
    if (!gxm_tgt_conv(gxfmt, sel, cfg, qlv, g_gx.copyfilter_on)) return 0;
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return 0;
    { const EfbRectPx rp = efb_rect_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh); if (rp.x1 <= rp.x0 || rp.y1 <= rp.y0) return 0; }
    return 1;
}

RhiTexture *rhi_gxm_copyEfb(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt)
{
    if (gxm_dead()) return NULL;
    float sel[4], cfg[4], qlv[4];
    const int conv = gxm_tgt_conv(gxfmt, sel, cfg, qlv, g_gx.copyfilter_on);
    if (!conv) { ++c_efb_copy_refused; if (gxfmt & 0x10) ++c_efb_copy_refused_z; return NULL; }
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) { ++c_efb_copy_refused; ++c_efb_copy_refused_rect; return NULL; }
    const EfbRectPx rp = efb_rect_px(x, y, w, h, g_gx.rw, g_gx.vh, g_gx.rh);
    if (rp.below480) ++c_efb_copy_below480;
    if (rp.clamped) ++c_efb_copy_clamped;
    if (rp.x1 <= rp.x0 || rp.y1 <= rp.y0) { ++c_efb_copy_refused; ++c_efb_copy_refused_rect; return NULL; }

    /* everything drawn so far must be in M before it is read: end M's scene */
    const unsigned m_draws_before = g_open == 'm' ? g_gx.scene_draws : 0u;
    gxm_target_scene_end('c');

    GxmTex *c = (GxmTex *) reuse;
    int fresh = 0;
    const int ce_on = s_ce_on != 0 && tg_ce_active();
    if (ce_on && c) tg_ce_flush(c, 1);                      /* the previous picture in this texture is read before it is overwritten (or dropped, counted) */
    if (c && (c->w != dw || c->h != dh)) {                 /* size changed: the NEW texture first; on NULL the caller still owns `reuse` */
        GxmTex *n = gxm_tex_new_copy(dw, dh, gxfmt);
        if (!n) { ++c_efb_copy_refused; ++c_efb_copy_new_fail; return NULL; }
        ++c_efb_copy_recreated;
        if (g_gxm_inst.cur_tex == (RhiTexture *) c) g_gxm_inst.cur_tex = NULL;
        gxm_tex_free(c);
        c = n;
    } else if (c && c->fmt != (uint32_t) gxfmt) {
        ++c_efb_copy_fmt_changed;                          /* same size, another copy format: reused, re-converted */
    }
    if (!c) {
        c = gxm_tex_new_copy(dw, dh, gxfmt);
        if (!c) { ++c_efb_copy_refused; ++c_efb_copy_new_fail; return NULL; }
        fresh = 1;
    }
    c->fmt = (uint32_t) gxfmt;

    float uvr[4];
    gxm_tgt_copy_uvr(rp.x0, rp.y0, rp.x1, rp.y1, g_gx.rw, g_gx.rh, uvr);

    int ok = tg_scratch(c->aw, c->ah) == 0 && tg_copy_draw(c, uvr, sel, cfg, qlv) == 0;
    if (g_open == 'c') gxm_target_scene_end('c');          /* ends with a fence quad if the copy drew nothing */
    gxm_state_dirty();
    if (!ok) {
        ++c_efb_copy_scene_fail;
        if (fresh) { gxm_tex_free(c); return NULL; }       /* nothing was ever written into it: do not hand out garbage */
        return (RhiTexture *) c;                           /* a reused texture keeps its previous picture (counted) */
    }
    ++c_efb_copy;
    if (ce_on) {
        if (s_ce_n < 48) { TgCe *e = &s_ce[s_ce_n++]; e->t = c; e->seq = g_gx.nval; e->frame = g_gx.frames; e->x0 = rp.x0; e->y0 = rp.y0; e->x1 = rp.x1; e->y1 = rp.y1;
                           e->dw = dw; e->dh = dh; e->fmt = gxfmt; e->below = rp.below480; }
        else ++s_ce_dropped;
    }
    if (tg_cl_active() && s_cl_n < 48) { TgCl *e = &s_cl[s_cl_n++]; memset(e, 0, sizeof *e); e->t = c; e->x0 = rp.x0; e->y0 = rp.y0; e->x1 = rp.x1; e->y1 = rp.y1;
                                         e->dw = dw; e->dh = dh; e->fmt = gxfmt; e->mdraws = m_draws_before; e->draw_at = s_cl_draws; s_cl_lastop = 2; }
    return (RhiTexture *) c;
}

/* Z copies: not in M2 (efbZMode 0).
 * peekZ (2026-10-27, docs/54 "first hand-play" item 2): until today this answered 0, so the hal kept "far" in EVERY mode and the glows' occlusion
 * probes (pi_dolphin.c gDepthReadResults) saw nothing in front of them: lights through walls.  Now: one EFB point read from M's depth memory as it
 * stands -- the vitaGL path's answer (rhi_vita_efb.inc rhi_vita_peekZ, peekz.txt mode 1): DF32M, a float with the mask in bit 31, never waits,
 * may be a scene behind.  Differences from vitaGL: M is TOP ROW FIRST here (header, ORIENTATION), so no row flip; the stride is the depth
 * surface's (rw aligned to 64 samples, tg_alloc_ds).  Reads memory fixed at init only, so G calls it directly in threaded mode (gx_rt_special.c). */
int rhi_gxm_efbMode(void) { return 1; }
int rhi_gxm_efbZMode(void) { return 0; }
int rhi_gxm_peekZ(int x, int y, unsigned *z24, unsigned *raw)
{
    ++c_peekz;
    if (gxm_dead() || g_gx.rw <= 0 || g_gx.rh <= 0) return 0;
    int slot = -1; uint32_t best = 0;                            /* the newest published frame whose last M scene the GPU has finished */
    const uint32_t done = g_gx.fnot ? *g_gx.fnot : 0u;
    for (int z = 0; z < TG_DZ_N; ++z) {
        const uint32_t f = __atomic_load_n(&g_dz_frame[z], __ATOMIC_ACQUIRE);
        if (!f || f <= best) continue;
        if (!g_gx.fence_finish && (int) (done - __atomic_load_n(&g_dz_seq[z], __ATOMIC_RELAXED)) < 0) continue;
        best = f; slot = z;
    }
    const uint32_t *dz = slot >= 0 ? (const uint32_t *) b_md[slot].p : NULL;
    if (!dz) { ++c_peekz_noframe; return 0; }                   /* no finished frame yet (boot): the hal keeps "far" */
    int px = x * g_gx.rw / 640, py = y * g_gx.vh / 480;          /* efb_rect_px's floor mapping, as every EFB user */
    if (px < 0) px = 0;
    if (px >= g_gx.rw) px = g_gx.rw - 1;
    if (py < 0) py = 0;
    if (py >= g_gx.rh) py = g_gx.rh - 1;
    const uint32_t w = dz[(unsigned) py * (unsigned) GXM_ALIGN(g_gx.rw, 64) + (unsigned) px] & 0x7fffffffu;
    float d; memcpy(&d, &w, 4);
    /* SCALE: the memory holds window depth 0.5 + 0.5 * z'/w, and in play the HAL hands GX clip z (gx_draw.c gxClipZ: z'/w = GX depth - 1, in [-1, 0];
     * gStfxGxZToGL is 0 outside captures) -- so the memory spans [0, 0.5] and GX's 24-bit depth is 2 * d.  The game compares it with
     * sz = 0xFFFFFF * (1 + clipZ) (camera.c Camera_ClipToScreen) and draws a glow when sz <= d (shader.c:1340): at d * 0xFFFFFF every probe read
     * half-depth and EVERY glow faded out.  Device evidence (2026-10-27, eboot d7118ebb, DIRECT hub): 24 of 24 probes <= 0.5 (far = 0.4997).
     * ⚠ rhi_vita_peekZ (vitaGL, rhi_vita_efb.inc) has the same half-scale conversion; vitaGL is not the shipped backend and is left as it is. */
    d *= 2.0f;
    if (!(d >= 0.0f)) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    if (raw) *raw = w;
    if (z24) *z24 = (unsigned) (d * 16777215.0f);
    return 1;
}
void rhi_gxm_peekZ_sync(void) { ++c_peekz_sync; }

/* DEBUG (marker ux0:data/stairfax/gxm_mshot.txt starting with 1): the colour target M itself (the picture BEFORE the display blit) as ux0:data/stairfax_gxm_m_<F>.ppm,
 * so "nothing was drawn" and "the blit lost it" can be told apart. Not part of any gate. */
static void tg_mshot(unsigned f)
{
    char c = 0;
    SceUID mf = sceIoOpen("ux0:data/stairfax/gxm_mshot.txt", SCE_O_RDONLY, 0777);
    if (mf < 0) return;
    sceIoRead(mf, &c, 1); sceIoClose(mf);
    if ((c != '1' && c != '2') || !b_mc.p) return;
    char path[96], hdr[64];
    snprintf(path, sizeof path, "ux0:data/stairfax_gxm_m_%u.ppm", f);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    const int hn = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", g_gx.rw, g_gx.vh);
    sceIoWrite(fd, hdr, (SceSize) hn);
    uint8_t *row = (uint8_t *) malloc((size_t) g_gx.rw * 3u);
    unsigned nonblack = 0, total = 0;
    if (row) {
        for (int y = 0; y < g_gx.vh; ++y) {
            const uint8_t *s = (const uint8_t *) b_mc.p + (size_t) y * (size_t) g_gx.rw * 4u;
            for (int x = 0; x < g_gx.rw; ++x) {
                row[x * 3 + 0] = s[x * 4 + 0]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2];
                nonblack += (s[x * 4 + 0] | s[x * 4 + 1] | s[x * 4 + 2]) != 0; ++total;
            }
            sceIoWrite(fd, row, (SceSize) g_gx.rw * 3u);
        }
        free(row);
    }
    sceIoClose(fd);
    gxm_log("[gxm-mshot] frame=%u M %dx%d (top %d rows) nonblack_px=%u of %u (debug dump of M before the blit)", f, g_gx.rw, g_gx.rh, g_gx.vh, nonblack, total);
}

/* ---- the SHOT: the display buffer as a PPM + the log line that binds it ---- */
static void tg_shot(unsigned f, unsigned back)
{
    if (gxm_frame_wait_scene(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) { gxm_abort("shot: the display scene fence never reached %u", g_gx.nval); return; }
    char name[64], path[96], hdr[96];
    snprintf(name, sizeof name, TG_SHOT_FILE, f);
    snprintf(path, sizeof path, TG_SHOT_PATH, f);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { gxm_chk((int) fd, "sceIoOpen(shot)", __FILE__, __LINE__); ++c_shot_fail; return; }
    const unsigned npx = GXM_DISP_W * GXM_DISP_H;
    uint32_t *keys = (uint32_t *) malloc((size_t) npx * sizeof(uint32_t));
    uint32_t fnv = 2166136261u;
    unsigned bytes = 0;
    const int hn = snprintf(hdr, sizeof hdr, "P6\n# frame=%u build=%s\n%d %d\n255\n", f, TG_BUILD_ID, GXM_DISP_W, GXM_DISP_H);
    sceIoWrite(fd, hdr, (SceSize) hn);
    fnv = gxm_tgt_fnv1a(fnv, hdr, (size_t) hn);
    bytes += (unsigned) hn;
    static uint8_t in[GXM_DISP_W * 4], out[GXM_DISP_W * 3];
    const uint8_t *fb = (const uint8_t *) b_disp[back].p;
    for (int yy = 0; yy < GXM_DISP_H; ++yy) {
        memcpy(in, fb + (size_t) yy * GXM_DISP_W * 4u, sizeof in);        /* memory order R,G,B,A */
        for (int xx = 0; xx < GXM_DISP_W; ++xx) {
            out[xx * 3 + 0] = in[xx * 4 + 0]; out[xx * 3 + 1] = in[xx * 4 + 1]; out[xx * 3 + 2] = in[xx * 4 + 2];
            if (keys) keys[(unsigned) yy * GXM_DISP_W + (unsigned) xx] = (uint32_t) in[xx * 4 + 0] | ((uint32_t) in[xx * 4 + 1] << 8) | ((uint32_t) in[xx * 4 + 2] << 16);
        }
        sceIoWrite(fd, out, sizeof out);
        fnv = gxm_tgt_fnv1a(fnv, out, sizeof out);
        bytes += (unsigned) sizeof out;
    }
    sceIoClose(fd);
    tg_mshot(f);
    GxmShotStats st;
    if (keys) gxm_tgt_shot_stats(keys, npx, &st); else memset(&st, 0, sizeof st);
    free(keys);
    ++c_shots;
    gxm_log("[gxm-shot] frame=%u fnv=%08x bytes=%u w=%d h=%d distinct=%u top_pct=%u mean=%u,%u,%u nonblack_pct=%u file=%s%s",
            f, (unsigned) fnv, bytes, GXM_DISP_W, GXM_DISP_H, st.distinct, st.top_pct, st.mean[0], st.mean[1], st.mean[2], st.nonblack_pct, name,
            keys ? "" : " stats=unavailable(malloc)");
}

/* ---- present (M1 display_scene / gxm_present :304-339, :362-388) ---- */
bool gxm_op_present(RhiInstance *r, RhiSwapchain *sc)
{
    (void) r; (void) sc;
    rhi_gxm_r9_frame();                                         /* R9 replay census: a frame boundary (instrument; one load when off) */
    if (gxm_dead()) { ++c_present_dead; return false; }
    gxm_target_scene_end('p');                                  /* M's scene */
    {   /* the depth ring: this frame's surface is whole once the GPU passes scene nval (its last M scene, or later); publish it, then UNPUBLISH the next slot
         * before any scene of the next frame can write it (a reader never picks the slot R renders into) */
        const int c = g_dz_cur, nx = (c + 1) % TG_DZ_N;
        __atomic_store_n(&g_dz_seq[c], g_gx.nval, __ATOMIC_RELAXED);
        __atomic_store_n(&g_dz_frame[c], ++g_dz_fcount, __ATOMIC_RELEASE);
        __atomic_store_n(&g_dz_frame[nx], 0u, __ATOMIC_RELEASE);
        g_dz_cur = nx;
    }
    if (s_ce_on != 0) tg_ce_present();
    if (gxm_dead()) { ++c_present_dead; return false; }

    const unsigned f = g_gx.frames;
    const unsigned back = f % GXM_DISP_BUFS, front = f ? (f - 1) % GXM_DISP_BUFS : 0;
    const int with_sync = !(g_gx.arm & GXM_ARM_NOPRESENT);      /* the nopresent arm never queues the buffer: a sync object nobody releases would wedge the third scene */
    int ok = 0;
    if (tg_begin('d', g_rt_d, with_sync ? g_sync_d[back] : NULL, &g_cs_d[back], &g_ds_d, GXM_DISP_W, GXM_DISP_H) == 0) {
        if (g_gx.arm & GXM_ARM_NOBLIT) {                         /* the break arm: the scene clears but never draws M */
            static const float full[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
            float col[4];
            tg_frame_colour(col, f);
            tg_quad_clear(full, 0.0f, col, g_fp_clear, 0, GXM_DISP_W, GXM_DISP_H);
        } else {
            /* TL, TR, BR, BL; uv (0,0) (1,0) (1,vh/rh) (0,vh/rh): the picture is the top vh rows of M. LINEAR / CLAMP. */
            static const float full[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
            float uv[4];
            gxm_tgt_disp_uv(g_gx.vh, g_gx.rh, uv);
            tg_quad_blit_m(full, uv, GXM_DISP_W, GXM_DISP_H);
        }
        gxm_target_scene_end('p');
        ok = !gxm_dead();
    } else {
        ++c_disp_fail;
    }
    if (ok) {
        if (!with_sync) ++c_present_skipped;
        else {
            TgDispData dd;
            dd.addr = b_disp[back].p;
            { extern uint32_t g_gxm_wait_us[2]; const uint32_t tq0 = sceKernelGetProcessTimeLow();
              if (GXM_CHK(sceGxmDisplayQueueAddEntry(g_sync_d[front], g_sync_d[back], &dd))) ok = 0;
              g_gxm_wait_us[1] += sceKernelGetProcessTimeLow() - tq0; }   /* frame owns g_gx.queued: it counts it from presented_ok */
        }
        for (int i = 0; i < g_shot_n; ++i)
            if (g_shot_frames[i] == f) { tg_shot(f, back); break; }
    }
    /* a new frame: render-target rotation restarts (the counters rt_overflow / rt_scenes_max are cumulative) */
    g_m_scene_k = 0;
    memset(g_rt_used, 0, sizeof g_rt_used);
    gxm_frame_present_end(ok);
    return ok && !gxm_dead();
}

/* ---- the report tokens ---- */
int gxm_target_report(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    const int n = snprintf(buf, (size_t) cap,
        "efb_copy=%u efb_copy_refused=%u efb_copy_refused_z=%u efb_copy_refused_rect=%u efb_copy_noconv=%u efb_copy_clamped=%u efb_copy_below480=%u"
        " efb_copy_scene_fail=%u efb_copy_ring_fail=%u efb_copy_recreated=%u efb_copy_new_fail=%u efb_copy_fmt_changed=%u"
        " readback=%u readback_bytes=%u readback_us=%u readback_null=%u clear=%u clear_rect=%u clear_rect_empty=%u clear_skip=%u peekz=%u peekz_noframe=%u peekz_sync=%u"
        " begin_frame=%u end_frame=%u tg_scene_fail=%u tg_end_fail=%u tg_end_c=%u tg_end_x=%u tg_end_p=%u tg_end_r=%u tg_ring_fail=%u"
        " present_skipped=%u present_dead=%u disp_fail=%u scratch_grow=%u scratch_fail=%u shots=%u shot_fail=%u res=%dx%d rh=%d ",
        c_efb_copy, c_efb_copy_refused, c_efb_copy_refused_z, c_efb_copy_refused_rect, c_efb_copy_noconv, c_efb_copy_clamped, c_efb_copy_below480,
        c_efb_copy_scene_fail, c_efb_copy_ring_fail, c_efb_copy_recreated, c_efb_copy_new_fail, c_efb_copy_fmt_changed,
        c_readback, c_readback_bytes, c_readback_us, c_readback_null, c_clear, c_clear_rect, c_clear_rect_empty, c_clear_skip, c_peekz, c_peekz_noframe, c_peekz_sync,
        c_begin_frame, c_end_frame, c_scene_fail, c_end_fail, c_end_c, c_end_x, c_end_p, c_end_r, c_ring_fail,
        c_present_skipped, c_present_dead, c_disp_fail, c_scratch_grow, c_scratch_fail, c_shots, c_shot_fail, g_gx.rw, g_gx.vh, g_gx.rh);
    if (n < 0) { buf[0] = 0; return 0; }
    return n >= cap ? cap - 1 : n;
}

#endif /* !GXM_TARGET_HOST_TEST */
