/* rhi_vita_efb.inc -- included at the END of rhi_vita.c (it uses that file's statics: vita_flush,
 * vita_drawColored, g_vp, g_inst, gs_cur, VitaTexture, rhi_vita_readEfbRect's callers).
 *
 * ==== THE EFB AS AN FBO: GX copies as GPU work (docs/47, 2026-10-06) ================================
 * The CPU readback behind GXCopyTex cost ~18 ms per copy and the game makes 7-15 a frame. The probe
 * (projects/vita-glprobe, arm EFBFBO) measured the alternative on hardware, and this file is built to
 * exactly those facts:
 *   - the whole game renders into ONE off-screen framebuffer M (960x544 RGBA8); a copy draws one quad
 *     from M's colour texture into a small per-destination FBO through a conversion shader; the FBO's
 *     texture can be sampled the moment the FBO is switched away from;
 *   - orientation is GL order everywhere (v=0 is the BOTTOM row), so the copy quad is flipped to give
 *     the port's convention: a copy texture sampled at v=0 is the TOP row of the source rectangle;
 *   - DEPTH DOES NOT SURVIVE an FBO switch unless force-load/force-store is set on M's depth surface:
 *     the probe's hack (mirror of vitaGL's internal framebuffer struct, validated before use) did it;
 *   - vglTexImageDepthBuffer is unusable and an FBO depth-texture attachment is GL_INVALID_ENUM, so a Z
 *     copy has no depth texture to read: the depth buffer is RESOLVED INTO M's ALPHA at copy time
 *     (efb_z_resolve, below) and the copy shader replicates alpha;
 *   - cost: M + the final draw +1.55 ms/frame, a copy +0.66 ms, back-to-back copies nearly free.
 *
 * MODE (ux0:data/stairfax/efb_mode.txt, one integer, read once; absent = 1):
 *   0 direct rendering, no real copies   1 M + GPU copies   2 direct rendering + the CPU readback
 * Z COPIES (ux0:data/stairfax/efb_z.txt, one integer, mode 1 only; absent = 1):
 *   0 none (run 100's behaviour)   1 GX_TF_Z8 copies, the whole-screen one only while something
 *   samples it (gx_draw.c)   2 every Z8 copy, always
 * Anything that goes wrong in mode 1 (M, a program that will not link, a previous boot that died in
 * the compile) turns the feature OFF (mode 0) with a log line instead of crash-looping.
 *
 * ⚠ A Cg compile error takes the whole process down (SceShaccCg). Both programs are built behind a
 * CRASH SENTINEL (rhi_vita_tev.inc's pattern): EFB_COMPILING is written before the build and removed
 * after it; a boot that finds it left behind writes EFB_BAD and stays in mode 0 until the marker is
 * deleted. The shader forms are the ones that linked on the probe. `pass` is a Cg reserved word. */
#include <psp2/gxm.h>
#include <psp2/io/stat.h>

#define EFB_DIR "ux0:data/stairfax/"

/* FIX A / the flicker investigation counters (the periodic [efb] line, vt_game.c) */
unsigned rhi_vita_efb_copy_below480, rhi_vita_efb_copy_clamped;     /* copies whose source ended below GX line 480 / that the clamp cropped */
unsigned rhi_vita_efb_refused_conv, rhi_vita_efb_refused_rect, rhi_vita_efb_refused_new;   /* refusals by kind: no conversion for the format, empty rect, no texture */
unsigned rhi_vita_efb_recreated, rhi_vita_efb_fmt_changed;          /* a destination reused at another size (re-created) / another format (re-converted) */
unsigned rhi_vita_efb_mode, rhi_vita_efb_gpu_copies, rhi_vita_efb_refused_fmt, rhi_vita_efb_switches,
         rhi_vita_efb_alive, rhi_vita_efb_hack, rhi_vita_efb_z_copies, rhi_vita_efb_z_kpx;
static int g_efb_zmode = 1; /* efb_z.txt */
static unsigned g_efb_hack_fails;
static int g_efb_on;        /* mode 1 and M exists: all game rendering goes to M */
static int g_efb_default;   /* the default framebuffer is bound (the frame has been composited) */
static int g_efb_break;     /* self-test only: copy with the flip and the conversion disabled */
static GLuint g_efb_m_tex, g_efb_m_fbo, g_efb_qvbo;

/* ---- the RENDER size (g_rw x g_rh, declared in rhi_vita.c) -----------------------------------------
 * M, S, every M-pixel rectangle and the depth hack use g_rw/g_rh; VITA_SCR_W/H is the PANEL (vglInit, the
 * default framebuffer's viewport, efb_composite's destination, the panel captures). Chosen ONCE, in
 * efb_init before M exists, from ux0:data/stairfax/render_res.txt = "W H" (both multiples of 8 -- vitaGL
 * loses the last rows of an FBO that is not -- 320 <= W <= 960, 184 <= H <= 544). Absent or invalid =
 * the panel size, and then nothing below differs from the build without this feature. */
static int efb_res_differs(void) { return g_rw != VITA_SCR_W || g_vh != VITA_SCR_H; }
/* The cached game viewport and scissor start as the whole PICTURE: all of M, or (528-line target) its top g_vh rows. */
static void efb_res_cache_init(void)
{
    g_vp[0] = 0; g_vp[1] = g_rh - g_vh; g_vp[2] = g_rw; g_vp[3] = g_vh;
    g_sc[0] = 0; g_sc[1] = g_rh - g_vh; g_sc[2] = g_rw; g_sc[3] = g_vh;
    vita_sc_recalc();
}
/* back to the panel size, and the cached game viewport/scissor with it (they start as the full target) */
static void efb_res_panel(void)
{
    g_rw = VITA_SCR_W; g_rh = g_vh = VITA_SCR_H; g_efb528 = 0;
    efb_res_cache_init();
}
/* ---- the 528-LINE EFB (2026-10-12, FIX A) ---------------------------------------------------------------------------
 * The GameCube's EFB is 640 x 528 lines; the game's picture is 480 of them, but the shadow-map passes render and copy down to
 * line 512 (newshadows.c renderShadows: GXSetViewport(0,0,512,512), GXCopyTex (0,0,512,512) -> a 256x256 R4). Until now M was
 * exactly the picture (g_rh = g_vh), a line y mapped to pixel y*g_rh/480 and everything below the picture was clamped away: at
 * 960x544 line 512 = pixel 580 of 544 rows, the copy rect was clamped to 544 and its 544 rows were stretched over the copy's 256
 * texels -- the shadow map was cropped 6% and stretched 7% against the projection the receiver uses. Now M has
 * efb528_total_h(g_vh) rows (544 -> 600, 368 -> 408), the extra rows BELOW the picture, and the picture mapping is untouched.
 * ux0:data/stairfax/efb528_off.txt starting with 1 = M exactly the picture, today's behaviour (read once, here). */
static void efb528_init(void)
{
    FILE *f = fopen(EFB_DIR "efb528_off.txt", "r"); int off = f && fgetc(f) == 49; if (f) fclose(f);
    g_vh = g_rh;
    if (!off) { g_rh = efb528_total_h(g_vh); g_efb528 = g_rh > g_vh; }
    else g_efb528 = 0;
    efb_res_cache_init();
    printf("[efb528] %s: render target %dx%d, picture rows %d, extra rows %d (GX line 528 = pixel row %d) | M colour +%u KB, depth +%u KB | %s\n",
           g_efb528 ? "ON" : "OFF", g_rw, g_rh, g_vh, g_rh - g_vh, 528 * g_vh / 480,
           (unsigned) ((size_t) g_rw * (size_t) (g_rh - g_vh) * 4u / 1024u),
           (unsigned) ((size_t) ((g_rw + 31) & ~31) * (size_t) (((g_rh + 31) & ~31) - ((g_vh + 31) & ~31)) * 4u / 1024u),
           off ? "efb528_off.txt: M is exactly the picture, a line below 480 is clamped away (today's behaviour)" : "no efb528_off.txt: lines up to 528 have pixels");
}
static void efb_res_init(void)
{
    char b[48] = { 0 };
    int w = 0, h = 0, n = 0, found = 0;
    const char *why = "default";
    SceUID f = sceIoOpen(EFB_DIR "render_res.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) {
        int len = sceIoRead(f, b, sizeof b - 1);
        sceIoClose(f);
        found = 1;
        if (len > 0) n = sscanf(b, "%d %d", &w, &h);
    }
    if (!found) why = "default";
    else if (n != 2) why = "render_res.txt invalid: want two integers \"W H\"";
    else if ((w & 7) || (h & 7)) why = "render_res.txt invalid: not multiples of 8";
    else if (w < 320 || w > 960 || h < 184 || h > 544) why = "render_res.txt invalid: outside 320..960 x 184..544";
    else { why = "render_res.txt"; g_rw = w; g_rh = h; }
    g_vh = g_rh;
    printf("[res] render target %dx%d (%s)\n", g_rw, g_rh, why);
    efb528_init();
}
/* M's depth memory is allocated by vitaGL in whole 32x32 GXM tiles: its row stride is the width aligned
 * to 32 (== g_rw at 960). The CPU walks and fills it with these. */
static int efb_dz_stride(void) { return (g_rw + 31) & ~31; }
static size_t efb_dz_bytes(void) { return (size_t) efb_dz_stride() * (size_t) ((g_rh + 31) & ~31) * 4u; }

/* rhi_gxm.c (milestone M1): the same dispatch rule as rhi_vita.c, first statement of each entry point */
int rhi_gxm_efbMode(void); int rhi_gxm_efbZMode(void); int rhi_gxm_peekZ(int x, int y, unsigned *z24, unsigned *raw); void rhi_gxm_peekZ_sync(void);
RhiTexture *rhi_gxm_copyEfb(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt);
void rhi_gxm_copyTexSetFilter(RhiTexture *h, int gxMin, int gxMag); void rhi_gxm_copyTexSetSampler(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT);
void rhi_gxm_copyEfbFree(RhiTexture *t);
int rhi_vita_efbMode(void) { if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_efbMode(); return (int) rhi_vita_efb_mode; }
int rhi_vita_efbZMode(void) { if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_efbZMode(); return rhi_vita_efb_mode == 1 && g_efb_on ? g_efb_zmode : 0; }

/* ---- the programs: forms copied from the probe (EFB_VS, EFB_FS_PASS, EFB_FS_DV) -------------------
 * aPos is the unit-quad corner; rect (NDC x0,y0,x1,y1) places it and uvr maps the same corner to a
 * texture sub-rectangle. The copy program is the DV form (a dot of the texel with `sel`) plus a bias
 * and a colour/gray blend, using only + and * (no lerp):
 *   gray v = dot(t, sel) + cfg.x        out = (v,v,v,v)            when cfg.y = 0
 *   colour (t.x,t.y,t.z, a)             a = t.w*cfg.z + (1-cfg.z)  when cfg.y = 1
 * so R8/G8/B8/A8 are a unit `sel`, I4/I8 are the luma weights + 16/255, and RGB565 is colour with
 * alpha 1. */
static const char *EFB_VS =
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

static const char *EFB_FS_COMPOSITE =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex) : COLOR\n"
"{\n"
"    return tex2D(tex, vUv);\n"
"}\n";

static const char *EFB_FS_COPY =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex, uniform float4 sel, uniform float4 cfg) : COLOR\n"
"{\n"
"    float4 t = tex2D(tex, vUv);\n"
"    float v = t.x * sel.x + t.y * sel.y + t.z * sel.z + t.w * sel.w + cfg.x;\n"
"    float a = t.w * cfg.z + (1.0f - cfg.z);\n"
"    return float4(v + (t.x - v) * cfg.y, v + (t.y - v) * cfg.y,\n"
"                  v + (t.z - v) * cfg.y, v + (a - v) * cfg.y);\n"
"}\n";

/* FIX B: the copy program with the QUANTISER. A GX copy to a sub-8-bit format keeps the top n bits of each channel (efb_rect.h:
 * efb_quant_model is this same arithmetic in float, tested on the host); the plain program above handed the receiver 8 bits where
 * the game was written for 4 (R4 shadow maps) / 5-6 (RGB565). qlv = levels per channel (15, 31, 63; 0 = full precision):
 *     out = o + (floor(floor(o * 255 + 0.5) * ((qlv + 1) / 256)) / max(qlv, 1) - o) * min(qlv, 1)
 * with qlv = 0 the last factor is 0 and out == o exactly. Only operations the TEV programs and EFB_FS_Z already run on the device
 * (floor/min/max on float4, explicit float4 constructors for every vector add -- the TEV generator's rule). Used only when
 * copyfilter_off.txt is absent; otherwise EFB_FS_COPY (above) is compiled instead, byte-for-byte what always ran. */
static const char *EFB_FS_COPY_Q =
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

typedef struct { GLuint id; GLint u_rect, u_uvr, u_zz, u_sel, u_cfg, u_q; int ok; } EfbProg;
static int g_efb_selftest_done;   /* (defined again, tentatively, below) */
static float g_efb_qlv[4];   /* the copy program's qlv for the next efb_quad (set by efb_conv) */
static int g_cf_on = 1;      /* FIX B (quantiser + sampler state for copy textures); copyfilter_off.txt starting with 1 turns it off, read once in efb_init */
static EfbProg g_efb_pc, g_efb_px;   /* composite (M -> screen, copy texture -> screen), copy */

static int efb_exists(const char *path)
{
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (f < 0) return 0;
    sceIoClose(f);
    return 1;
}

static void efb_build_program(EfbProg *p, const char *fs_src)
{
    memset(p, 0, sizeof *p);
    GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
    GLuint fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
    GLuint pr = glCreateProgram();
    GLint linked = 0;
    glShaderSource(vs, 1, &EFB_VS, NULL); glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
    glAttachShader(pr, vs); glAttachShader(pr, fs);
    glBindAttribLocation(pr, 0, "aPos");
    glLinkProgram(pr);
    glGetProgramiv(pr, GL_LINK_STATUS, &linked);
    if (!linked) return;
    p->id = pr;
    p->u_rect = glGetUniformLocation(pr, "rect");
    p->u_uvr  = glGetUniformLocation(pr, "uvr");
    p->u_zz   = glGetUniformLocation(pr, "zz");
    p->u_sel  = glGetUniformLocation(pr, "sel");
    p->u_cfg  = glGetUniformLocation(pr, "cfg");
    p->u_q    = glGetUniformLocation(pr, "qlv");
    p->ok = 1;
}

/* Both programs, behind the crash sentinel. 1 = both linked. */
static int efb_build_programs(void)
{
    sceIoMkdir("ux0:data/stairfax", 0777);
    if (efb_exists(EFB_DIR "EFB_BAD")) { printf("[efb] EFB_BAD marker present: a previous boot died compiling the EFB programs -- mode 0 (delete the marker to retry)\n"); return 0; }
    if (efb_exists(EFB_DIR "EFB_COMPILING")) {
        SceUID b = sceIoOpen(EFB_DIR "EFB_BAD", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (b >= 0) { sceIoWrite(b, "1", 1); sceIoClose(b); }
        sceIoRemove(EFB_DIR "EFB_COMPILING");
        printf("[efb] the previous boot died compiling the EFB programs: marked bad, mode 0\n");
        return 0;
    }
    SceUID s = sceIoOpen(EFB_DIR "EFB_COMPILING", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (s < 0) { printf("[efb] cannot write the compile sentinel -- refusing to compile unfenced, mode 0\n"); return 0; }
    sceIoWrite(s, "1", 1); sceIoClose(s);
    /* the exact text about to be compiled, BEFORE the compile (the TEV path's trick) */
    s = sceIoOpen(EFB_DIR "efb_compiling.cg", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (s >= 0) {
        sceIoWrite(s, EFB_VS, (unsigned) strlen(EFB_VS)); sceIoWrite(s, "//----\n", 7);
        sceIoWrite(s, EFB_FS_COMPOSITE, (unsigned) strlen(EFB_FS_COMPOSITE)); sceIoWrite(s, "//----\n", 7);
        sceIoWrite(s, g_cf_on ? EFB_FS_COPY_Q : EFB_FS_COPY, (unsigned) strlen(g_cf_on ? EFB_FS_COPY_Q : EFB_FS_COPY));
        sceIoClose(s);
    }
    efb_build_program(&g_efb_pc, EFB_FS_COMPOSITE);
    efb_build_program(&g_efb_px, g_cf_on ? EFB_FS_COPY_Q : EFB_FS_COPY);
    sceIoRemove(EFB_DIR "EFB_COMPILING");
    printf("[efb] programs: composite linked=%d copy linked=%d (uniforms: copy rect=%d uvr=%d zz=%d sel=%d cfg=%d)\n",
           g_efb_pc.ok, g_efb_px.ok, g_efb_px.u_rect, g_efb_px.u_uvr, g_efb_px.u_zz, g_efb_px.u_sel, g_efb_px.u_cfg);
    return g_efb_pc.ok && g_efb_px.ok;
}

/* One quad through program p over the whole bound target. */
static void efb_quad(const EfbProg *p, const float uvr[4], const float sel[4], const float cfg[4])
{
    glUseProgram(p->id);
    glBindBuffer(GL_ARRAY_BUFFER, g_efb_qvbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (const void *) 0);
    glUniform4f(p->u_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(p->u_uvr, uvr[0], uvr[1], uvr[2], uvr[3]);
    glUniform1f(p->u_zz, 0.0f);
    if (p->u_sel >= 0 && sel) glUniform4f(p->u_sel, sel[0], sel[1], sel[2], sel[3]);
    if (p->u_cfg >= 0 && cfg) glUniform4f(p->u_cfg, cfg[0], cfg[1], cfg[2], cfg[3]);
    if (p->u_q >= 0) glUniform4f(p->u_q, g_efb_qlv[0], g_efb_qlv[1], g_efb_qlv[2], g_efb_qlv[3]);   /* only the quantising copy program has one */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

/* State for a pass that must not touch depth, blend, scissor or the alpha test. Every later flush
 * re-applies its own depth/alpha state (vita_applyAlphaMode), the clears set their own masks. */
static void efb_pass_state(void)
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void efb_bind_m(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_m_fbo);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('M', 'r');
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);   /* the cached viewport follows the game, not the pass */
    g_efb_default = 0;
}

/* texture -> the whole default framebuffer, plain GL uv (the probe: unflipped). */
static void efb_composite(GLuint tex)
{
    /* M is g_rh rows tall and its picture is the TOP g_vh of them (the extra 528-line rows are below it): only the picture is
     * presented. efb_composite_v0 is 0 (the whole texture, as before) unless the extra rows exist. A copy texture is whole. */
    float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (tex == g_efb_m_tex) full[1] = efb_composite_v0(g_vh, g_rh, VITA_SCR_H);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('D', 'p');
    glViewport(0, 0, VITA_SCR_W, VITA_SCR_H);
    efb_pass_state();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (efb_res_differs()) {   /* M is stretched over the panel: filtered, not blocky. At the panel size nothing is added. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    efb_quad(&g_efb_pc, full, NULL, NULL);
    g_efb_default = 1;
}

/* ---- the depth hack (probe: force_load_store, arm E5b: applied=1 survived=yes) ------------------
 * vitaGL's GL FBO name IS a pointer to its `framebuffer` struct. The layout is copied from
 * source/shared.h and VALIDATED before every use (a plausible pointer, the active flag, the depth
 * surface pointing at its own member, READY state, width/height); on any mismatch nothing is touched.
 * It can only validate once M's depth surface exists, so it is retried at every endFrame until it
 * takes. ⚠ UNVERIFIED on this build: whether the surface is READY before M's first scene. */
typedef struct {
    GLboolean active;
    GLboolean is_float;
    uint8_t depthbuffer_state;
    SceGxmRenderTarget *target;
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer;
    SceGxmDepthStencilSurface *depthbuffer_ptr;
    SceGxmTextureFormat format;
    int width, height, stride;
    void *data;
    void *tex;
} efb_fb_mirror;

static uint32_t *g_efb_dz;   /* M's depth memory, CPU-visible; set when the hack takes */
static int g_efb_noboundary; /* BREAK arm (one-shot file): the hack as runs 102-104 had it */
int rhi_vita_efb_rt8;   /* M's render target was replaced by one made for 8 scenes per frame */
static int efb_try_hack(void)
{
    if (rhi_vita_efb_hack) return 1;
    uintptr_t a = (uintptr_t) g_efb_m_fbo;
    efb_fb_mirror *m = (efb_fb_mirror *) a;
    if (a < 0x80000000u || (a & 3u) || !m->active || m->depthbuffer_state != 2 /* DEPTHBUFFER_READY */
        || m->depthbuffer_ptr != &m->depthbuffer || m->width != g_rw || m->height != g_rh) {
        ++g_efb_hack_fails;
        return 0;
    }
    unsigned zls0 = m->depthbuffer.zlsControl;
    g_efb_dz = (uint32_t *) m->depthbuffer.depthData;
    sceGxmDepthStencilSurfaceSetForceLoadMode(&m->depthbuffer, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(&m->depthbuffer, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    /* ⚠ The flags only count for a scene BEGUN after them, and vitaGL begins and ends scenes lazily:
     * runs 102-104 went on drawing in the scene that was open here, which was never stored, and the
     * next one LOADED this allocation as vitaGL left it -- uninitialised (run 105: 18% of its words
     * had bit 31, the mask bit; the Z resolve came out as that lattice, periodic in linear memory
     * order, and test (d) passed on a pixel whose garbage depth was near). Run 105 passed only
     * because a diagnostic called glFinish here. So, deliberately: end the open scene, and give the
     * memory what vitaGL's own STORE_DEPTH_STENCIL build gives it (0x80: "mask update bit to 1").
     * efb_noboundary.txt (one-shot) leaves both out: runs 102-104 again, to watch (d) and (f) fail. */
    if (!g_efb_noboundary) {
        glFinish();
        if (g_efb_dz) memset(g_efb_dz, 0x80, efb_dz_bytes());
    }
    /* SCENES PER FRAME (2026-10-08, docs/47 "Run 137"): vitaGL creates every FBO's render target with scenesPerFrame = 1
     * (gxm.c setup_render_target(..., 1); this build has no shared render targets). The game leaves M and comes back to
     * it for every EFB copy -- about seven M scenes a frame -- and a render target used for more scenes than it was
     * created for makes sceGxmBeginScene WAIT for the GPU to finish the earlier one. Run 137's [win] line: 38-40 ms a
     * frame inside GXCopyTex at the title (6 copies), 75-87 in the prologue; a copy is one quad. So M gets a render
     * target made for 8 scenes (GXM's maximum), swapped in while the GPU is idle. ux0:data/stairfax/efb_rt1.txt
     * starting with 1 = keep vitaGL's own (the A/B arm). */
    {   FILE *rf = fopen("ux0:data/stairfax/efb_rt1.txt", "r"); int keep = rf && fgetc(rf) == 49; if (rf) fclose(rf);
        if (keep) printf("[efb] M render target: vitaGL's own, 1 scene per frame (efb_rt1.txt)\n");
        else if (!m->target) printf("[efb] M render target: none yet, NOT replaced\n");
        else {
            SceGxmRenderTargetParams rp; memset(&rp, 0, sizeof rp);
            rp.width = g_rw; rp.height = g_rh; rp.scenesPerFrame = 8;
            rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rp.driverMemBlock = -1;
            SceGxmRenderTarget *nt = NULL;
            glFinish();   /* no scene open on the old target, nothing queued against it */
            int rr = sceGxmCreateRenderTarget(&rp, &nt);
            if (rr >= 0 && nt) { SceGxmRenderTarget *old = m->target; m->target = nt; sceGxmDestroyRenderTarget(old); rhi_vita_efb_rt8 = 1; }
            printf("[efb] M render target: %s (sceGxmCreateRenderTarget %08x)\n", rhi_vita_efb_rt8 ? "REPLACED, 8 scenes per frame" : "NOT replaced, creation failed", (unsigned) rr);
        }
    }
    rhi_vita_efb_hack = 1;
    printf("[efb] depth hack applied (force-load/store on M's depth surface) after %u failed validation(s) | zlsControl %08x -> %08x depthData %p stencilData %p bgDepth %f bgControl %08x | 0x80 fill %s\n",
           g_efb_hack_fails, zls0, m->depthbuffer.zlsControl, m->depthbuffer.depthData, m->depthbuffer.stencilData,
           (double) m->depthbuffer.backgroundDepth, m->depthbuffer.backgroundControl, !g_efb_noboundary && g_efb_dz ? "DONE, scene boundary forced" : "NO, and no scene boundary (efb_noboundary.txt)");
    return 1;
}

/* What M's depth memory HOLDS, read by the CPU (it is mapped memory). Does a force-store write it at
 * all? zero words, words with bit 31 (the candidate mask bit), words that are 1.0 with or without
 * it, a hash, and the first words. glFinish first: the GPU must be done with the scene -- but
 * glFinish is itself a scene boundary, so the noboundary arm reads without it (possibly stale). */
static void efb_dz_stat(const char *tag)
{
    if (!g_efb_dz) { printf("[efb] depth memory (%s): no pointer\n", tag); return; }
    if (!g_efb_noboundary) glFinish();
    unsigned n = efb_dz_stride() * g_rh, zero = 0, b31 = 0, one = 0, h = 2166136261u;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = g_efb_dz[i];
        zero += w == 0; b31 += w >> 31; one += (w & 0x7fffffffu) == 0x3f800000u;
        h = (h ^ w) * 16777619u;
    }
    const uint32_t *c = g_efb_dz + (unsigned) (g_rh / 2) * (unsigned) efb_dz_stride() + (unsigned) (g_rw / 2);   /* the middle of the screen */
    printf("[efb] depth memory (%s): of %u words zero %u bit31 %u one %u | hash %08x | first %08x %08x %08x %08x | centre %08x %08x %08x %08x\n",
           tag, n, zero, b31, one, h, g_efb_dz[0], g_efb_dz[1], g_efb_dz[2], g_efb_dz[3], c[0], c[1], c[2], c[3]);
}
/* GXPeekZ (2026-10-09, docs/47 "Run 157"): one EFB point (640x480, top-left) read from M's depth memory. The word
 * is a float with the mask in bit 31; the memory's LAST row is the picture's TOP (what the depth-texture Z copy
 * samples: v = 1 - y/h, validated against the resolve in run 144). 0 back = no depth memory (hack not applied). */
int rhi_vita_peekZ(int x, int y, unsigned *z24, unsigned *raw)
{
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_peekZ(x, y, z24, raw);
    if (!g_efb_dz || rhi_vita_efb_mode != 1 || !g_efb_on) return 0;
    int px = x * g_rw / 640, py = y * g_vh / 480;   /* the picture's rows; the memory is g_rh rows (528-line target) */
    if (px < 0) px = 0;
    if (px >= g_rw) px = g_rw - 1;
    if (py < 0) py = 0;
    if (py >= g_rh) py = g_rh - 1;
    uint32_t w = g_efb_dz[(unsigned) (g_rh - 1 - py) * (unsigned) efb_dz_stride() + (unsigned) px] & 0x7fffffffu;
    float d; memcpy(&d, &w, 4);
    if (!(d >= 0.0f)) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    if (raw) *raw = w;
    if (z24) *z24 = (unsigned) (d * 16777215.0f);
    return 1;
}
/* mode 2: everything drawn so far reaches the depth memory before it is read (a scene boundary and a GPU wait) */
void rhi_vita_peekZ_sync(void) { if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_peekZ_sync(); return; } vita_flush(); rhi_vita_scene_why = 'x'; glFinish(); }
static void efb_write(const char *name, const void *p, unsigned bytes)
{
    char path[96];
    snprintf(path, sizeof path, EFB_DIR "%s", name);
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f >= 0) { sceIoWrite(f, p, bytes); sceIoClose(f); }
    printf("[efb] selftest dump %s %s\n", path, f >= 0 ? "written" : "FAILED");
}

/* ---- the frame ---------------------------------------------------------------------------------- */
static void efb_begin_frame(void)
{
    if (g_efb_on && g_efb_default) efb_bind_m();   /* an endFrame with no present before this frame */
}
static void efb_zlate_flush(void);
static int g_scn_copy_idx;   /* scene_gpu_break.txt: EFB copies so far this frame (the break arm's slot counter) */
static void efb_end_frame(void)
{
    if (!g_efb_on) return;
    efb_try_hack();
    if (g_efb_default) return;                    /* already composited: idempotent */
    efb_zlate_flush();                            /* the late Z copy: M's scene ends here anyway */
    efb_composite(g_efb_m_tex);
}
static void efb_after_swap(void)
{
    if (g_efb_on) efb_bind_m();                   /* the next frame renders into M again */
    g_scn_copy_idx = 0;                           /* scene_gpu_break.txt counts the frame's copies */
}
/* mode 1 reads from M. The bounce through the default framebuffer is a scene boundary, so the
 * read sees this frame's draws whichever way vitaGL's glReadPixels treats an FBO. 1 = M is bound. */
static int efb_read_prep(void)
{
    if (!g_efb_on) return 0;
    if (!g_efb_default) { glBindFramebuffer(GL_FRAMEBUFFER, 0); ++rhi_vita_efb_switches; RHI_SCENE_TAG('D', 'x'); }
    efb_bind_m();
    return 1;
}
static int efb_keeps_depth(void) { return g_efb_on && rhi_vita_efb_hack; }

/* ---- depth -> alpha (the Z copies, 2026-10-07) ---------------------------------------------------
 * There is no way to sample M's depth, but the depth TEST can read it: 256 flat quads over the copy's
 * rectangle, quad i at window depth i/256 carrying alpha i, drawn in ascending order with GL_LEQUAL,
 * depth writes off and only the alpha channel unmasked. A pixel keeps the alpha of the LAST quad that
 * passed, the largest i with i/256 <= depth = floor(depth * 256): the top byte of GX's 24-bit Z,
 * which is what GX_TF_Z8 copies. Quad 0 passes everywhere, so every pixel of the rectangle is
 * written and whatever alpha the frame's draws left is irrelevant. Colour is untouched (the mask);
 * it holds for every draw path, the two fixed-function ones included, because no draw takes part.
 * The cost is 256 depth tests per pixel of the rectangle: small for a shadow rectangle; for the whole
 * screen it is MEASURED at boot (selftest (g)) and the copy is made on demand (gx_draw.c).
 * `masked` 0 writes RGB as well: 10x cheaper (hidden-surface removal works again), so production
 * (variant 2, below) uses it with M's colour saved before and drawn back after. Rectangle in M's
 * pixels, top-left origin. */
static int g_efb_zfail;   /* self-test (f) did not pass: no Z copies for the game */
/* The variant rhi_vita_copyEfb uses (the self-test walks them):
 *   1 alpha only: colour untouched, but the mask defeats HSR (whole screen ~299 ms, run 103/106)
 *   0 all channels: ~31 ms, and M's colour is DESTROYED (timing and self-test only)
 *   2 all channels with M's colour saved to S before and drawn back after (production, run 108) */
static int g_efb_zmasked = 2, g_efb_zstep = 1;
static int g_efb_norestore;   /* BREAK arm (one-shot file, self-test only): variant 2 without the restore */
static GLuint g_efb_s_tex, g_efb_s_fbo;
static int g_efb_s_state;     /* 0 not tried, 1 S exists, -1 S could not be made (variant 1 instead) */

/* M's colour over the rectangle -> S (variant 2). Leaves M bound. 0 = no S: the caller falls back. */
static int efb_z_save(int x0, int y0, int x1, int y1)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (!g_efb_s_state) {
        glGenTextures(1, &g_efb_s_tex);
        glGenFramebuffers(1, &g_efb_s_fbo);
        glBindTexture(GL_TEXTURE_2D, g_efb_s_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rw, g_rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_s_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_s_tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        g_efb_s_state = st == GL_FRAMEBUFFER_COMPLETE ? 1 : -1;
        printf("[efb] Z save target S %dx%d %s (0x%04x)\n", g_rw, g_rh, g_efb_s_state > 0 ? "made" : "INCOMPLETE: Z copies use the alpha-only resolve", (unsigned) st);
        if (g_efb_s_state < 0) { efb_bind_m(); return 0; }
    }
    if (g_efb_s_state < 0) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_s_fbo);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('S', 'z');
    glViewport(0, 0, g_rw, g_rh);
    efb_pass_state();
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, g_rh - y1, x1 - x0, y1 - y0);   /* GL rows, like efb_clear_rect_gl */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_m_tex);
    /* NEAREST for this one draw. Run 108, self-test (h): through M's LINEAR filter the 1:1 save came
     * back with 2,847 of 522,240 px changed (3 of them by more than 3), all the count of the test
     * picture's tone edges would give -- the sample point is a hair off the texel centre, and
     * LINEAR turns that into a blend. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    efb_quad(&g_efb_pc, full, NULL, NULL);               /* FBO to FBO, same size, plain uv: identity */
    glBindTexture(GL_TEXTURE_2D, g_efb_m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glDisable(GL_SCISSOR_TEST);
    efb_bind_m();
    return 1;
}
/* S -> M's colour over the rectangle; M is bound, depth is not touched. */
static void efb_z_restore(int x0, int y0, int x1, int y1)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    glViewport(0, 0, g_rw, g_rh);
    efb_pass_state();
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, g_rh - y1, x1 - x0, y1 - y0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_s_tex);
    efb_quad(&g_efb_pc, full, NULL, NULL);
    glDisable(GL_SCISSOR_TEST);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
}
static void efb_z_resolve(int x0, int y0, int x1, int y1, int masked, int step)
{
    static const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    static RhiColorVertex v[256 * 6];
    float l = 2.0f * x0 / g_rw - 1.0f, r = 2.0f * x1 / g_rw - 1.0f;
    float b = 1.0f - 2.0f * y1 / g_rh, t = 1.0f - 2.0f * y0 / g_rh;
    int nq = 0;
    for (int i = 0; i < 256; i += step, ++nq) {
        float z = 2.0f * (i / 256.0f) - 1.0f;                     /* window depth i/256 (exact in a float) */
        uint32_t c = masked ? (uint32_t) i << 24 : (uint32_t) i * 0x01010101u;   /* memory order R,G,B,A */
        RhiColorVertex q[6] = { { l, b, z, c }, { r, b, z, c }, { r, t, z, c }, { l, b, z, c }, { r, t, z, c }, { l, t, z, c } };
        memcpy(v + nq * 6, q, sizeof q);
    }
    vita_loadMvpCols(ident);
    glViewport(0, 0, g_rw, g_rh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    if (masked) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE); else glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(RhiColorVertex), &v[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(RhiColorVertex), &v[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, nq * 6);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
}

/* ---- copies ------------------------------------------------------------------------------------- */
typedef struct { VitaTexture t; GLuint fbo; int aw, ah; uint8_t smin, smag; } EfbCopyTex;   /* t first: the returned RhiTexture* IS &c->t; smin/smag: 1 = GL_NEAREST applied (0 = the creation state, GL_LINEAR) */

/* GX copy format -> conversion constants; 0 = not handled, 2 = a Z copy (resolve depth first). */
static int efb_conv(int f, float sel[4], float cfg[4])
{
    sel[0] = sel[1] = sel[2] = sel[3] = 0.0f;
    cfg[0] = 0.0f; cfg[1] = 0.0f; cfg[2] = 1.0f; cfg[3] = 0.0f;
    g_efb_qlv[0] = g_efb_qlv[1] = g_efb_qlv[2] = g_efb_qlv[3] = 0.0f;
    if (f == 0x11) {                                              /* Z8: M's alpha, after efb_z_resolve */
        if (!g_efb_zmode) return 0;
        sel[3] = 1.0f;
        if (g_efb_break) { sel[3] = 0.0f; cfg[1] = 1.0f; }        /* self-test BREAK: no conversion */
        return 2;
    }
    if (f & 0x10) return 0;                                       /* every other Z format (the game makes none) */
    switch (f) {
    case 0x20: case 0x28: sel[0] = 1.0f; break;                   /* R4, R8 -> (r,r,r,r) */
    case 0x29: sel[1] = 1.0f; break;                              /* G8 */
    case 0x2A: sel[2] = 1.0f; break;                              /* B8 */
    case 0x27: cfg[0] = 1.0f; break;                              /* A8: the EFB is RGB8_Z24, its alpha reads 255 */
    case 0x00: case 0x01:                                         /* I4, I8: BT.601 luma */
        sel[0] = 0.257f; sel[1] = 0.504f; sel[2] = 0.098f; cfg[0] = 16.0f / 255.0f; break;
    /* RGB5A3 and RGBA8 are (r,g,b,1) too. The game sets GX_PF_RGB8_Z24 (pi_videoinit.c): its EFB has
     * NO alpha, so a copy reads alpha as 255 whatever the combiner produced (rcp_dolphin.c's 32x32
     * RGBA8 distortion slots are drawn with TEV alpha 0). M's alpha is therefore free, and is where a
     * Z copy's depth goes. Until 2026-10-07 these two passed M's alpha through, which was the TEV's. */
    case 0x04: case 0x05: case 0x06: cfg[1] = 1.0f; cfg[2] = 0.0f; break;   /* RGB565, RGB5A3, RGBA8 -> (r,g,b,1) */
    default: return 0;
    }
    if (g_efb_break) { sel[0] = sel[1] = sel[2] = sel[3] = 0.0f; cfg[0] = 0.0f; cfg[1] = 1.0f; cfg[2] = 1.0f; }   /* self-test BREAK: no conversion */
    /* FIX B: what the GX format keeps. I4/R4 keep 4 bits of every channel the receiver sees, RGB565 5/6/5, RGB5A3 (the EFB is opaque, so its
     * opaque form) 5/5/5. Not during the boot self-test, whose tones and tolerances were chosen for 8 bits. */
    if (g_cf_on && g_efb_selftest_done && !g_efb_break) {
        switch (f) {
        case 0x20: case 0x00: g_efb_qlv[0] = g_efb_qlv[1] = g_efb_qlv[2] = g_efb_qlv[3] = 15.0f; break;
        case 0x04: g_efb_qlv[0] = 31.0f; g_efb_qlv[1] = 63.0f; g_efb_qlv[2] = 31.0f; break;
        case 0x05: g_efb_qlv[0] = g_efb_qlv[1] = g_efb_qlv[2] = 31.0f; break;
        default: break;
        }
    }
    return 1;
}

static EfbCopyTex *g_zlate_c, *g_zlate_filled;   /* the late Z copy's destination, and the one that holds a copy (run 193) */
static void efb_copy_free(EfbCopyTex *c)
{
    if (g_zlate_c == c) g_zlate_c = NULL;
    if (g_zlate_filled == c) g_zlate_filled = NULL;
    glDeleteFramebuffers(1, &c->fbo);
    glDeleteTextures(1, &c->t.id);
    free(c);
    if (rhi_vita_efb_alive) --rhi_vita_efb_alive;
}

static EfbCopyTex *efb_copy_new(int dw, int dh, int gxfmt)
{
    EfbCopyTex *c = (EfbCopyTex *) calloc(1, sizeof *c);
    if (!c) return NULL;
    c->t.w = dw; c->t.h = dh; c->t.fmt = (uint32_t) gxfmt;
    glGenTextures(1, &c->t.id);
    glGenFramebuffers(1, &c->fbo);
    glBindTexture(GL_TEXTURE_2D, c->t.id);
    /* ⚠ The render target is allocated in multiples of 8 (run 99, boot self-test: a 60x45 copy had
     * its v=0 and middle rows right and its LAST row black, a 64x48 one was whole). The game's 80x60
     * copy is such a size. The picture is drawn over the whole aligned texture and sampled 0..1, so
     * the caller's size (t.w, t.h) stays what GX asked for. */
    c->aw = (dw + 7) & ~7; c->ah = (dh + 7) & ~7;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->aw, c->ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->t.id, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_m_fbo);
    rhi_vita_scene_kind = 'M';   /* scene_gpu.c: the target only; nothing was drawn, so no scene ended */
    ++rhi_vita_efb_alive;
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        printf("[efb] copy FBO %dx%d incomplete (0x%04x)\n", dw, dh, (unsigned) st);
        efb_copy_free(c);
        return NULL;
    }
    return c;
}

/* ---- Z COPIES FROM THE DEPTH BUFFER ITSELF (2026-10-08, docs/47 "Run 144") ------------------------------------
 * Run 144 (efb_z.txt 0): without Z copies the bird close-up's GPU wait goes from ~30 ms to 0.2. A Z copy was
 * M's colour saved to S, 64-256 full-screen depth-tested quads writing the depth into M, the copy quad, and M's
 * colour drawn back. vitaGL can alias a texture onto a framebuffer's depth memory (vglTexImageDepthBuffer:
 * a LINEAR DF32M texture over depthData), and M's depth memory is really written since the depth hack
 * (force-store, runs 104-107). So variant 3: ONE quad into the copy texture, sampling the depth; M is not
 * touched, nothing is saved or restored, and the precision is the full 8 bits again.
 * ux0:data/stairfax/efb_zdepth.txt starting with 1 asks for it. It is NOT trusted on its word: the first
 * EFB_ZD_CHECKS Z copies of the game are made BOTH ways and read back; only if every one agrees (texels more
 * than 1 code apart <= 0.5%) does variant 3 take over, otherwise the boot stays on the resolve and says so.
 * (An earlier probe read 0 from vglTexImageDepthBuffer -- before the depth memory was known to be garbage.) */
static int g_efb_selftest_done;   /* the boot self-test tests the resolve; the depth-texture variant engages only after it */
#define EFB_ZD_CHECKS 3
static const char *EFB_FS_Z =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex) : COLOR\n"
"{\n"
"    float d = tex2D(tex, vUv).x;\n"
"    float v = min(floor(d * 256.0f), 255.0f) / 255.0f;\n"
"    return float4(v, v, v, v);\n"
"}\n";
static EfbProg g_efb_pz;
static GLuint g_efb_ztex;
static int g_efb_zd = -1;      /* -1 not read, 0 off, 1 candidate (both ways, compared), 2 active, 3 rejected */
static int g_efb_zd_ok;        /* agreeing comparisons so far */
unsigned rhi_vita_efb_zd_copies;
static void efb_zd_init(void)
{
    FILE *zf = fopen(EFB_DIR "efb_zdepth.txt", "r"); int on = zf && fgetc(zf) == 49; if (zf) fclose(zf);
    g_efb_zd = 0;
    if (!on) { printf("[efbz] depth-texture Z copies: off (efb_zdepth.txt)\n"); return; }
    if (g_rw % 32) { printf("[efbz] depth-texture Z copies: NOT at a render width of %d (the depth rows are 32-aligned)\n", g_rw); return; }
    if (efb_exists(EFB_DIR "EFBZ_COMPILING")) {
        sceIoRemove(EFB_DIR "EFBZ_COMPILING");
        printf("[efbz] depth-texture Z copies: the previous boot died setting them up -- off for this boot\n");
        return;
    }
    SceUID s = sceIoOpen(EFB_DIR "EFBZ_COMPILING", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (s < 0) return;
    sceIoWrite(s, "1", 1); sceIoClose(s);
    efb_build_program(&g_efb_pz, EFB_FS_Z);
    if (g_efb_pz.ok) {
        static const unsigned char zero[8 * 8 * 4];
        glGenTextures(1, &g_efb_ztex);
        glBindTexture(GL_TEXTURE_2D, g_efb_ztex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        vglTexImageDepthBuffer(GL_TEXTURE_2D);   /* M is the bound framebuffer: its depth memory, as a DF32M texture */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_efb_zd = 1;
    }
    sceIoRemove(EFB_DIR "EFBZ_COMPILING");
    printf("[efbz] depth-texture Z copies: %s (program linked=%d); the first %d are checked against the resolve\n",
           g_efb_zd ? "CANDIDATE" : "off, the program did not link", g_efb_pz.ok, EFB_ZD_CHECKS);
}
/* the depth-texture quad into the bound copy target */
static void efb_zd_quad(const float uvr[4])
{
    efb_pass_state();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_ztex);
    efb_quad(&g_efb_pz, uvr, NULL, NULL);
}
/* the late Z copy (run 193): ux0:data/stairfax/efb_zlate.txt starting with 1 turns it on */
static float g_zlate_uvr[4]; static int g_zlate_pending, g_zlate = -1;
unsigned rhi_vita_efb_zlate_deferred, rhi_vita_efb_zlate_done;
static int efb_zlate_on(void)
{
    if (g_zlate < 0) { FILE *zf = fopen(EFB_DIR "efb_zlate.txt", "r"); g_zlate = zf && fgetc(zf) == 49; if (zf) fclose(zf);
                       printf("[efbz] late Z copy (at the end of the frame, one frame old): %s\n", g_zlate ? "ON (efb_zlate.txt)" : "off"); }
    return g_zlate;
}
static void efb_zlate_flush(void)
{
    if (!g_zlate_pending || !g_zlate_c) return;
    g_zlate_pending = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_zlate_c->fbo); ++rhi_vita_efb_switches; RHI_SCENE_TAG('Z', 'z');
    glViewport(0, 0, g_zlate_c->aw, g_zlate_c->ah);
    efb_zd_quad(g_zlate_uvr);
    g_zlate_filled = g_zlate_c; ++rhi_vita_efb_zlate_done; ++rhi_vita_efb_zd_copies;
}
/* ---- THE BREAK ARM of the per-scene GPU-time instrument (scene_gpu.c, 2026-10-11) ------------------------------
 * ux0:data/stairfax/scene_gpu_break.txt = "K [what]": ONE extra scene a frame, inserted just before the frame's
 * K-th EFB copy (K >= 1: M is bound and flushed there, and the copy is about to leave it anyway).
 *   what 1 (default): a full-target quad through a deliberately heavy fragment program (8 reads of M's colour at
 *          computed coordinates) into B, a scratch target of M's size. Nothing is drawn into M between B and the
 *          copy, so vitaGL (lazy scenes) makes exactly ONE more scene: the instrument must show it in its own row
 *          (BREAK) at the cost measured DRAINED here at boot, and no other row may move.
 *   what 2: the same, then ONE more scene on M holding a single colour-masked full-target quad (the M-probe
 *          row): M is loaded and stored once more with nothing visible drawn -- what a BREAK of M costs.
 * ⚠ If the K-th copy is one that does not leave M (the late Z copy handing over last frame's texture), M's own
 * scene is split instead and the rows on M move: read the raw dump (scene_gpu.txt "2 N") to see which slot K is.
 * The picture is not changed by either: B is a scratch target, the probe quad writes no channel and no depth. */
static const char *EFB_FS_BREAK =
"float4 main(float2 vUv : TEXCOORD0, float vD : TEXCOORD1,\n"
"            uniform sampler2D tex) : COLOR\n"
"{\n"
"    float4 a = tex2D(tex, vUv);\n"
"    a = a + tex2D(tex, vUv + float2(0.011f, 0.003f));\n"
"    a = a + tex2D(tex, vUv + float2(-0.007f, 0.013f));\n"
"    a = a + tex2D(tex, vUv + float2(0.005f, -0.017f));\n"
"    a = a + tex2D(tex, vUv + float2(-0.019f, -0.002f));\n"
"    a = a + tex2D(tex, vUv + float2(0.023f, 0.009f));\n"
"    a = a + tex2D(tex, vUv + float2(-0.013f, 0.021f));\n"
"    a = a + tex2D(tex, vUv + float2(0.002f, -0.029f));\n"
"    return a * 0.125f;\n"
"}\n";
static EfbProg g_scn_pb;
static GLuint g_scn_b_tex, g_scn_b_fbo;
static int g_scn_break_ok;

/* the heavy scene: M is bound on entry and on return; `why` = what M is being left for */
static void scn_break_draw(int why)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    glBindFramebuffer(GL_FRAMEBUFFER, g_scn_b_fbo);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('B', why);
    glViewport(0, 0, g_rw, g_rh);
    efb_pass_state();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_m_tex);
    efb_quad(&g_scn_pb, full, NULL, NULL);
    efb_bind_m();
}
/* the probe: one colour-masked quad over all of M (M is bound). After a switch away it begins a scene of its own. */
static void scn_probe_draw(void)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    rhi_vita_scene_kind = 'm';   /* names the scene the draw below begins; a scene already open keeps its name */
    glViewport(0, 0, g_rw, g_rh);
    efb_pass_state();
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_scn_b_tex);
    efb_quad(&g_efb_pc, full, NULL, NULL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    rhi_vita_scene_kind = 'M';
}
/* in the frame: before the K-th copy (rhi_vita_copyEfb, after its flush) */
static void scn_break_hook(int why)
{
    if (!g_scn_break_ok || g_efb_default) return;
    if (++g_scn_copy_idx != rhi_vita_scene_break_k) return;
    scn_break_draw(why);
    if (rhi_vita_scene_break_what == 2) scn_probe_draw();
}
/* DRAINED: glFinish, the work, glFinish. glFinish is itself a scene boundary (it ends the open scene and begins
 * an empty one on the bound target, M), so `what` 0 -- nothing but the two boundaries -- is the cost of one EMPTY
 * scene on M, and the heavy scene's own cost is (what 1) - (what 0). */
static unsigned scn_break_time(int what)
{
    glFinish();
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    if (what == 1) scn_break_draw('b');
    if (what == 2) scn_probe_draw();
    rhi_vita_scene_why = 'b';
    glFinish();
    return (unsigned) (sceKernelGetProcessTimeWide() - t0);
}
static unsigned scn_med5(unsigned v[5])
{
    for (int i = 1; i < 5; ++i) for (int j = i; j > 0 && v[j - 1] > v[j]; --j) { unsigned t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return v[2];
}
static void scn_break_init(void)   /* from efb_init, after the self-test: M exists and is bound */
{
    char b[32] = { 0 }; int k = 0, what = 1;
    SceUID f = sceIoOpen(EFB_DIR "scene_gpu_break.txt", SCE_O_RDONLY, 0777);
    if (f < 0) return;
    sceIoRead(f, b, sizeof b - 1); sceIoClose(f);
    if (sscanf(b, "%d %d", &k, &what) < 1 || k < 1) { printf("[scene] break arm: scene_gpu_break.txt holds no K >= 1 -- off\n"); return; }
    if (what != 2) what = 1;
    if (efb_exists(EFB_DIR "SCENE_BREAK_COMPILING")) {
        sceIoRemove(EFB_DIR "SCENE_BREAK_COMPILING");
        printf("[scene] break arm: the previous boot died setting it up -- off for this boot\n");
        return;
    }
    SceUID s = sceIoOpen(EFB_DIR "SCENE_BREAK_COMPILING", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (s < 0) return;
    sceIoWrite(s, "1", 1); sceIoClose(s);
    efb_build_program(&g_scn_pb, EFB_FS_BREAK);
    sceIoRemove(EFB_DIR "SCENE_BREAK_COMPILING");
    if (!g_scn_pb.ok) { printf("[scene] break arm: the heavy program did not link -- off\n"); return; }
    glGenTextures(1, &g_scn_b_tex);
    glGenFramebuffers(1, &g_scn_b_fbo);
    glBindTexture(GL_TEXTURE_2D, g_scn_b_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rw, g_rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scn_b_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scn_b_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    efb_bind_m();
    if (st != GL_FRAMEBUFFER_COMPLETE) { printf("[scene] break arm: scratch target B %dx%d incomplete (0x%04x) -- off\n", g_rw, g_rh, (unsigned) st); return; }
    unsigned t0[5], t1[5], t2[5], r0[5], r1[5], r2[5];
    scn_break_time(1); scn_break_time(2);   /* warm-up: B's render target and both programs' first use are not the cost */
    for (int i = 0; i < 5; ++i) { r0[i] = t0[i] = scn_break_time(0); r1[i] = t1[i] = scn_break_time(1); r2[i] = t2[i] = scn_break_time(2); }
    const unsigned m0 = scn_med5(t0), m1 = scn_med5(t1), m2 = scn_med5(t2);
    rhi_vita_scene_break_us = m1 > m0 ? m1 - m0 : 0;
    rhi_vita_scene_probe_us = m2;
    rhi_vita_scene_break_k = k; rhi_vita_scene_break_what = what;
    g_scn_break_ok = 1;
    printf("[scene] break scene drained cost %.3f ms (heavy scene %dx%d, median of 5 with the scene boundaries: %.3f | %.3f %.3f %.3f %.3f %.3f | minus the boundaries alone = one EMPTY scene on M: %.3f | %.3f %.3f %.3f %.3f %.3f) | M-probe scene drained %.3f ms (%.3f %.3f %.3f %.3f %.3f) | depth hack %s at this point | slot: before EFB copy %d of each frame, what %d\n",
           rhi_vita_scene_break_us / 1000.0, g_rw, g_rh, m1 / 1000.0, r1[0] / 1000.0, r1[1] / 1000.0, r1[2] / 1000.0, r1[3] / 1000.0, r1[4] / 1000.0,
           m0 / 1000.0, r0[0] / 1000.0, r0[1] / 1000.0, r0[2] / 1000.0, r0[3] / 1000.0, r0[4] / 1000.0,
           m2 / 1000.0, r2[0] / 1000.0, r2[1] / 1000.0, r2[2] / 1000.0, r2[3] / 1000.0, r2[4] / 1000.0,
           rhi_vita_efb_hack ? "APPLIED (M loads and stores its depth)" : "NOT applied", k, what);
}

RhiTexture *rhi_vita_copyEfb(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt)
{
    if (GXSP_CAP()) return g_gxsp_api->copyEfb(reuse, x, y, w, h, dw, dh, gxfmt);
    if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_copyEfb(reuse, x, y, w, h, dw, dh, gxfmt);
    float sel[4], cfg[4];
    if (rhi_vita_efb_mode != 1 || !g_efb_on) return NULL;
    if (__builtin_expect(g_gxd_on, 0)) gxd_event(GXD_EV_EFB_COPY, (uint32_t) x, (uint32_t) y, (uint32_t) w, (uint32_t) h, (uint32_t) dw, (uint32_t) dh);   /* B1-c2floor: a copy ends the open scene */
    int conv = efb_conv(gxfmt, sel, cfg);   /* 2: a Z copy */
    if (!conv || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) { ++rhi_vita_efb_refused_fmt; ++rhi_vita_efb_refused_conv; return NULL; }
    /* the source rectangle in M's pixels: EFB 640x480 top-left -> 960x544, the same mapping as
     * readEfbRect/clearRect (integer maths included), clamped to the screen */
    const EfbRectPx rp = efb_rect_px(x, y, w, h, g_rw, g_vh, g_rh);
    const int x0 = rp.x0, x1 = rp.x1, y0 = rp.y0, y1 = rp.y1;
    if (rp.below480) ++rhi_vita_efb_copy_below480;
    if (rp.clamped) {   /* the copy will not cover the rectangle the game rendered: it is stretched (FIX A's counter; 0 with the 528-line target) */
        ++rhi_vita_efb_copy_clamped;
        if (rhi_vita_efb_copy_clamped <= 8) printf("[efb528] copy CLAMPED: EFB %d,%d %dx%d -> M pixels %d,%d..%d,%d of %dx%d (picture rows %d)\n", x, y, w, h, x0, y0, x1, y1, g_rw, g_rh, g_vh);
    }
    if (x1 <= x0 || y1 <= y0) { ++rhi_vita_efb_refused_fmt; ++rhi_vita_efb_refused_rect; return NULL; }
    vita_flush();   /* everything drawn so far must be in M before it is read */
    if (g_scn_break_ok) scn_break_hook(conv == 2 ? 'z' : 'c');   /* scene_gpu_break.txt: the break arm's extra scene */

    EfbCopyTex *c = (EfbCopyTex *) reuse;
    if (c && (c->t.w != dw || c->t.h != dh)) {   /* size changed: recreate (the caller remaps) */
        /* the new one FIRST: on a NULL return the caller frees `reuse`, so it must still be alive */
        EfbCopyTex *n = efb_copy_new(dw, dh, gxfmt);
        if (!n) { ++rhi_vita_efb_refused_fmt; ++rhi_vita_efb_refused_new; return NULL; }
        ++rhi_vita_efb_recreated;   /* the destination was reused at another size: a new texture (and a new GL object) */
        efb_copy_free(c); c = n;
    }
    else if (c && c->t.fmt != (uint32_t) gxfmt) ++rhi_vita_efb_fmt_changed;   /* same size, another copy format: reused, re-converted */
    if (!c) c = efb_copy_new(dw, dh, gxfmt);
    if (!c) { ++rhi_vita_efb_refused_fmt; ++rhi_vita_efb_refused_new; return NULL; }
    c->t.fmt = (uint32_t) gxfmt;

    /* M's v=0 is the BOTTOM of the picture. The copy texture must give the source's TOP row at v=0
     * (the port's convention), and a draw into an FBO puts the quad's aPos.y=0 edge on the texture's
     * v=0 row: so that edge samples the rectangle's top. (BREAK for the self-test: unflipped.) */
    float uL = (float) x0 / g_rw, uR = (float) x1 / g_rw;
    float vTop = 1.0f - (float) y0 / g_rh, vBot = 1.0f - (float) y1 / g_rh;
    float uvr[4] = { uL, g_efb_break ? vBot : vTop, uR, g_efb_break ? vTop : vBot };

    int zsaved = 0;
    if (conv == 2 && g_efb_zd < 0 && rhi_vita_efb_hack && !g_efb_break && g_efb_selftest_done) efb_zd_init();
    const int zd = conv == 2 && !g_efb_break && g_efb_selftest_done ? (g_efb_zd == 1 || g_efb_zd == 2 ? g_efb_zd : 0) : 0;
    if (zd == 2 && efb_zlate_on()) {
        /* THE LATE Z COPY (run 193, docs/47): the copy is not made HERE, in the middle of the frame, where leaving M ends its
           scene and coming back reloads the whole target (run 192: one quad per copy and the frame still waits ~10 ms on the
           GPU; run 191 with no Z copy: 0.2). The request is remembered and the quad is drawn in efb_end_frame, where M's
           scene ends anyway; the filter that samples it this frame gets LAST frame's depth. The first request has nothing
           to hand over, so it copies now. */
        g_zlate_c = c; memcpy(g_zlate_uvr, uvr, sizeof g_zlate_uvr); g_zlate_pending = 1; ++rhi_vita_efb_zlate_deferred;
        if (g_zlate_filled == c) { ++rhi_vita_efb_gpu_copies; return (RhiTexture *) &c->t; }
    }
    if (zd == 2) {   /* variant 3: the depth buffer itself, one quad, M untouched */
        glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
        ++rhi_vita_efb_switches;
        RHI_SCENE_TAG('Z', 'z');
        glViewport(0, 0, c->aw, c->ah);
        efb_zd_quad(uvr);
        efb_bind_m();
        ++rhi_vita_efb_z_copies; rhi_vita_efb_z_kpx += (unsigned) ((x1 - x0) * (y1 - y0)) / 1000u; ++rhi_vita_efb_zd_copies;
        ++rhi_vita_efb_gpu_copies;
        return (RhiTexture *) &c->t;
    }
    if (conv == 2) {   /* M is bound and its depth is this scene's: put it in alpha before M is left */
        if (!g_efb_break) {
            zsaved = g_efb_zmasked == 2 && efb_z_save(x0, y0, x1, y1);
            efb_z_resolve(x0, y0, x1, y1, g_efb_zmasked == 1 || (g_efb_zmasked == 2 && !zsaved), g_efb_zstep);
        }
        ++rhi_vita_efb_z_copies; rhi_vita_efb_z_kpx += (unsigned) ((x1 - x0) * (y1 - y0)) / 1000u;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG(conv == 2 ? 'Z' : 'C', conv == 2 ? 'z' : 'c');
    glViewport(0, 0, c->aw, c->ah);
    efb_pass_state();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_m_tex);
    efb_quad(&g_efb_px, uvr, sel, cfg);
    efb_bind_m();   /* back to M with the game's cached viewport; scissor/depth/alpha state re-applied per flush */
    if (zsaved && !g_efb_norestore) efb_z_restore(x0, y0, x1, y1);
    if (zd == 1) {   /* the candidate: the same copy from the depth texture into a scratch target, both read back */
        EfbCopyTex *sc = efb_copy_new(dw, dh, gxfmt);
        const size_t nb = (size_t) c->aw * (size_t) c->ah * 4u;
        unsigned char *pa = (unsigned char *) malloc(nb), *pb = (unsigned char *) malloc(nb);
        if (sc && pa && pb && sc->aw == c->aw && sc->ah == c->ah) {
            glBindFramebuffer(GL_FRAMEBUFFER, sc->fbo); ++rhi_vita_efb_switches; RHI_SCENE_TAG('Z', 'z');
            glViewport(0, 0, sc->aw, sc->ah);
            efb_zd_quad(uvr);
            glReadPixels(0, 0, sc->aw, sc->ah, GL_RGBA, GL_UNSIGNED_BYTE, pb);
            glBindFramebuffer(GL_FRAMEBUFFER, c->fbo); ++rhi_vita_efb_switches; RHI_SCENE_TAG('Z', 'x');
            glReadPixels(0, 0, c->aw, c->ah, GL_RGBA, GL_UNSIGNED_BYTE, pa);
            efb_bind_m();
            unsigned far1 = 0, mx = 0, lo = 255, hi = 0; const unsigned px = (unsigned) (c->aw * c->ah);
            for (unsigned i = 0; i < px; ++i) {
                int d = (int) pa[i * 4] - (int) pb[i * 4]; if (d < 0) d = -d;
                if (d > 1) ++far1;
                if ((unsigned) d > mx) mx = (unsigned) d;
                if (pb[i * 4] < lo) lo = pb[i * 4];
                if (pb[i * 4] > hi) hi = pb[i * 4];
            }
            /* the resolve runs at efb_zstep precision: compare at that precision (a step of 4 may sit 3 below) */
            unsigned far2 = 0, far2strict = 0; const int tol = g_efb_zstep;
            for (unsigned i = 0; i < px; ++i) { int d = (int) pb[i * 4] - (int) pa[i * 4]; if (d < -1 || d > tol) ++far2strict; }
            /* Run 190 (docs/47): REJECTED in ShipBattle on check 3 with 647 of 76,800 texels "outside" (0.84% against 0.5%),
               max |diff| 100 -- and the game then paid the 64-slice resolve + colour save/restore every frame: ~13 ms of GPU
               (run 191, efb_z 0: wall 53.5 -> 40.7). Those texels are SILHOUETTES: both methods minify 640 -> 320 and may
               pick different source pixels where the depth steps, so a texel-for-texel compare counts every edge. A texel
               now agrees if the depth texture's value lies within the range of the resolve's 3x3 neighbourhood (same
               tolerance at both ends). A wrong texture (constant, flipped, wrong scale) still fails on every interior texel;
               the strict count is still printed. */
            {   const int W = (int) c->aw, H = (int) c->ah;
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    int mn = 255, mxn = 0;
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                        int v = pa[(yy * W + xx) * 4]; if (v < mn) mn = v; if (v > mxn) mxn = v;
                    }
                    int b = pb[(y * W + x) * 4];
                    if (b < mn - 1 || b > mxn + tol) ++far2;
                }
            }
            /* Run 145: the game's FIRST Z copy was of an all-far depth -- the depth texture read 255 everywhere, the resolve 252
               (255 at a step of 4): consistent, and it proves nothing (a texture that always reads 1.0 looks the same). A FLAT
               frame is therefore neither an agreement nor a disagreement: it is skipped. */
            const int flat = hi - lo < 8;
            static int s_flat;
            if (flat && far2 * 200u <= px) {
                if (++s_flat <= 3 || !(s_flat & 63)) printf("[efbz] depth-texture check: flat depth (%u..%u), consistent, not counted (%d skipped)\n", lo, hi, s_flat);
                /* Run 146: the prologue's 240 Z copies were ALL flat far (text over clouds: nothing writes depth), on both methods,
                   so the candidate was dropped before the first scene with geometry. No cap: it stays a candidate until one comes. */
                free(pa); free(pb); efb_copy_free(sc);
                ++rhi_vita_efb_gpu_copies;
                return (RhiTexture *) &c->t;
            }
            int agree = far2 * 200u <= px;   /* <= 0.5% of texels outside the tolerance, on a frame whose depth varies */
            printf("[efbz] depth-texture check %d: %ux%u, texels more than 1 apart %u, outside the resolve's own step (%d): %u texel-for-texel, %u against the 3x3 neighbourhood (the test), max |diff| %u, depth texture range %u..%u -> %s\n",
                   g_efb_zd_ok + 1, (unsigned) c->aw, (unsigned) c->ah, far1, tol, far2strict, far2, mx, lo, hi, agree ? "agrees" : "DISAGREES");
            if (!agree) { g_efb_zd = 3; printf("[efbz] depth-texture Z copies REJECTED for this boot: the resolve stays\n"); }
            else if (++g_efb_zd_ok >= EFB_ZD_CHECKS) { g_efb_zd = 2; printf("[efbz] depth-texture Z copies ACTIVE: one quad per Z copy, no resolve, no save/restore\n"); }
        }
        free(pa); free(pb);
        if (sc) efb_copy_free(sc);
    }
    ++rhi_vita_efb_gpu_copies;
    return (RhiTexture *) &c->t;
}

/* FIX B: the sampler state of the GXTexObj that samples a copy texture. Until now every copy was GL_LINEAR whatever the game asked
 * for (and gx_draw.c's GXInitTexObjLOD recorded nothing for ANY texture). The game's shadow maps ask for NEAR on the packed bit-plane
 * texture (newshadows.c textureAlloc(0x100,0x100,1,...,0,0)): bilinear blends neighbouring bit planes at a texel edge before the
 * receiver's x64 gain and threshold. GX min filters 0/2/4 (NEAR, NEAR_MIP_NEAR, NEAR_MIP_LIN) are nearest within a level; a copy has
 * one level, so the base-level choice is all there is. The parameters change only when the request does. */
unsigned rhi_vita_copyfilter_calls, rhi_vita_copyfilter_nearest, rhi_vita_copyfilter_changes;
void rhi_vita_copyTexSetFilter(RhiTexture *h, int gxMin, int gxMag)
{
    if (GXSP_CAP()) { g_gxsp_api->copyTexSetFilter(h, gxMin, gxMag); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_copyTexSetFilter(h, gxMin, gxMag); return; }
    EfbCopyTex *c = (EfbCopyTex *) h;
    if (!c || !g_cf_on) return;
    const uint8_t nmin = gxMin == 0 || gxMin == 2 || gxMin == 4, nmag = gxMag == 0;
    ++rhi_vita_copyfilter_calls;
    if (nmin || nmag) ++rhi_vita_copyfilter_nearest;
    if (nmin == c->smin && nmag == c->smag) return;
    vita_flush();
    glBindTexture(GL_TEXTURE_2D, c->t.id);
    if (nmin != c->smin) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nmin ? GL_NEAREST : GL_LINEAR);
    if (nmag != c->smag) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nmag ? GL_NEAREST : GL_LINEAR);
    c->smin = nmin; c->smag = nmag;
    ++rhi_vita_copyfilter_changes;
}

/* gx_draw.c's one call per bind of a copy texture: the GXTexObj's filter and wrap. The wrap is only requested on a texture whose GL
 * size is its GX size (a 60x45 copy is allocated 64x48: REPEAT would wrap through the padding). */
void rhi_vita_copyTexSetSampler(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT)
{
    if (GXSP_CAP()) { g_gxsp_api->copyTexSetSampler(h, gxMin, gxMag, gxWrapS, gxWrapT); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_copyTexSetSampler(h, gxMin, gxMag, gxWrapS, gxWrapT); return; }
    EfbCopyTex *c = (EfbCopyTex *) h;
    if (!c || !g_cf_on) return;
    rhi_vita_copyTexSetFilter(h, gxMin, gxMag);
    if (c->t.w == c->aw && c->t.h == c->ah) rhi_vita_texSetWrap(h, gxWrapS, gxWrapT);
}

void rhi_vita_copyEfbFree(RhiTexture *t)
{
    if (GXSP_CAP()) { g_gxsp_api->copyEfbFree(t); return; }
    if (g_stfx_backend == STFX_BK_GXM) { rhi_gxm_copyEfbFree(t); return; }
    if (!t) return;
    vita_flush();   /* the open batch may draw with it */
    efb_copy_free((EfbCopyTex *) t);
}

/* ---- boot self-test (mode 1, once, before the first game frame) ----------------------------------
 * Draws a two-tone rectangle into M plus a near and a far marker, copies the rectangle (R8 and
 * RGB565), and reads everything back THROUGH THE DEFAULT FRAMEBUFFER (the only readback the probe
 * proved): each copy texture is drawn over the screen and its first and last row sampled, M is
 * composited and the markers sampled. Raw values are logged next to the verdicts.
 *   (a) orientation: the RGB565 copy's v=0 row is the source's TOP tone, its last row the bottom
 *   (b) R8 conversion: (r,r,r) of each tone
 *   (c) M outside the rectangle survived the copy (the background and a marker drawn before it)
 *   (d) depth survived the round trip (a near marker, a copy, a far draw over it: near must remain)
 *   (f) a Z8 copy of the whole screen reads floor(depth*256): 64 near, 128 mid, 192 far, 255 cleared
 *   (g) INFO, no verdict: what the depth resolve costs (whole screen alpha-only / all channels,
 *       96x96), wall time around a drained scene, three samples of each next to an empty arm
 * efb_selftest_break.txt: the same test with the flip, the conversion and the depth resolve disabled
 * -- (a), (b) and (f) must then read FAIL, which is how the test is shown able to fail. */
static void efb_px_read(int x, int y, unsigned char o[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o); }
static int efb_near(const unsigned char *p, int r, int g, int b)
{
    return abs((int) p[0] - r) <= 3 && abs((int) p[1] - g) <= 3 && abs((int) p[2] - b) <= 3;
}
static void efb_test_quad(float x0, float y0, float x1, float y1, float z, uint32_t rgba)
{
    RhiColorVertex v[6] = { { x0, y0, z, rgba }, { x1, y0, z, rgba }, { x1, y1, z, rgba },
                            { x0, y0, z, rgba }, { x1, y1, z, rgba }, { x0, y1, z, rgba } };
    vita_drawColored(&g_inst.base, v, 6);
}
static void efb_clear_rect_gl(int x, int y, int w, int h, int r, int g, int b)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/* (g): one M scene (a clear, one quad, then `arm`), ended and drained; microseconds of wall time. */
static unsigned efb_time_arm(int arm)
{
    glFinish();
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    efb_bind_m();
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    efb_test_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, 0xff808080u);
    if (arm == 1) efb_z_resolve(0, 0, g_rw, g_rh, 1, 1);
    if (arm == 2) efb_z_resolve(0, 0, g_rw, g_rh, 0, 1);
    /* the 96x96 probe rectangle: its position scales with the render size (400,200 at the panel size), its size stays 96 */
    int sx0 = 400 * g_rw / VITA_SCR_W, sy0 = 200 * g_vh / VITA_SCR_H;
    if (arm == 3) efb_z_resolve(sx0, sy0, sx0 + 96, sy0 + 96, 1, 1);
    if (arm == 4) efb_z_resolve(sx0, sy0, sx0 + 96, sy0 + 96, 0, 1);
    if (arm == 5) efb_z_resolve(0, 0, g_rw, g_rh, 0, 16);
    if (arm == 6 || arm == 7) {   /* the whole COPY as the game makes it: 6 = variant 2 (save, resolve, copy, restore), 7 = variant 0 (resolve, copy) */
        static RhiTexture *tt;
        int keep = g_efb_zmasked;
        vita_flush();
        g_efb_zmasked = arm == 6 ? 2 : 0;
        tt = rhi_vita_copyEfb(tt, 0, 0, 640, 480, 320, 240, 0x11);
        g_efb_zmasked = keep;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('D', 'o');
    g_efb_default = 1;
    glFinish();
    return (unsigned) (sceKernelGetProcessTimeWide() - t0);
}

static void efb_dump(const char *name)
{
    char path[96];
    unsigned char *px = (unsigned char *) malloc(VITA_SCR_W * VITA_SCR_H * 4);
    if (!px) return;
    glReadPixels(0, 0, VITA_SCR_W, VITA_SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(path, sizeof path, EFB_DIR "%s", name);
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f >= 0) { sceIoWrite(f, px, VITA_SCR_W * VITA_SCR_H * 4); sceIoClose(f); }
    printf("[efb] selftest dump %s %s\n", path, f >= 0 ? "written" : "FAILED");
    free(px);
}

static void efb_selftest(void)
{
    enum { A_R = 220, A_G = 40, A_B = 90, B_R = 30, B_G = 150, B_B = 200, K_R = 16, K_G = 64, K_B = 32 };
    GeomStats keep = gs_cur;   /* the test's own draws must not show up in the first frame's counts */
    g_efb_break = efb_exists(EFB_DIR "efb_selftest_break.txt");
    if (g_efb_break) sceIoRemove(EFB_DIR "efb_selftest_break.txt");   /* ONE-SHOT, like the QUIT sentinel: no tool here deletes a file on the console */
    g_efb_noboundary = efb_exists(EFB_DIR "efb_noboundary.txt");
    if (g_efb_noboundary) sceIoRemove(EFB_DIR "efb_noboundary.txt");  /* ONE-SHOT too */
    g_efb_norestore = efb_exists(EFB_DIR "efb_norestore.txt");
    if (g_efb_norestore) sceIoRemove(EFB_DIR "efb_norestore.txt");    /* ONE-SHOT: (c), (d) and (h) must then FAIL */
    int h_ok = 0;
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);   /* the test quads are NDC: they span the viewport, which is the PICTURE (all of M unless the 528-line rows exist) */
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* warm-up scene: M's depth surface only validates for the hack once a scene has used it, and a
     * scene ends when the framebuffer is switched */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    efb_test_quad(-0.1f, -0.1f, 0.1f, 0.1f, 0.0f, 0xff0000ffu);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++rhi_vita_efb_switches;
    RHI_SCENE_TAG('D', 'o');
    int hack_first = efb_try_hack();
    efb_dz_stat("at the hack, before any store");
    efb_bind_m();

    glClearColor(K_R / 255.0f, K_G / 255.0f, K_B / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* EFB rect (60,60,120,90) in M pixels, by the same integer maths as rhi_vita_copyEfb: at 960x544 it is
     * 180 x 102 pixels at x 90, GL rows 374..476, top third = 34 rows (the numbers this test always used) */
    int tx0 = 60 * g_rw / 640, tx1 = 180 * g_rw / 640, ty0 = 60 * g_vh / 480, ty1 = 150 * g_vh / 480;   /* the picture's rows (g_vh); the GL row below is M's (g_rh) */
    int tw = tx1 - tx0, trh = ty1 - ty0, ttop = trh / 3;
    efb_clear_rect_gl(tx0, g_rh - ty1 + (trh - ttop), tw, ttop, A_R, A_G, A_B);
    efb_clear_rect_gl(tx0, g_rh - ty1, tw, trh - ttop, B_R, B_G, B_B);
    efb_test_quad(0.25f, -0.2647f, 0.4583f, 0.1029f, -0.5f, 0xff00ff00u);   /* near green (window depth .25), px 600..700 x 200..300 */
    efb_test_quad(0.625f, -0.2647f, 0.8333f, 0.1029f, 0.0f, 0xffff00ffu);   /* magenta marker, px 780..880 */

    /* 64x48: a size like the game's (its copies are 32, 64, 80x60, 128, 256, 320x240). Run 98 used
     * 60x45 here and the last row read BLACK with the v=0 row right: asked again below as INFO (o_*),
     * because whether an odd-sized render target works is a different question from whether copies do. */
    RhiTexture *tr = rhi_vita_copyEfb(NULL, 60, 60, 120, 90, 64, 48, 0x28);   /* R8 */
    RhiTexture *tc = rhi_vita_copyEfb(NULL, 60, 60, 120, 90, 64, 48, 0x04);   /* RGB565 */
    RhiTexture *to = rhi_vita_copyEfb(NULL, 60, 60, 120, 90, 60, 45, 0x04);   /* RGB565, odd size (INFO) */
    /* M is bound again, but vitaGL begins its scene at the next draw: the GPU is idle on M's depth,
     * which now holds whatever the store of the scene with the green and the magenta left */
    efb_dz_stat("after the first stored scene (near .25 100x100, mid .5 100x100, rest 1.0)");
    if (g_efb_dz) efb_write("zself_depth.raw", g_efb_dz, (unsigned) (efb_dz_stride() * g_rh * 4));
    efb_test_quad(0.2f, -0.3f, 0.5f, 0.15f, 0.5f, 0xff00ffffu);              /* far yellow over the green (window depth .75) */
    vita_flush();
    /* (f) the game's own whole-screen Z copy. (c) and (d) are read after it, so they also say that
     * the resolve left M's colour and depth alone. */
    RhiTexture *tz = rhi_vita_copyEfb(NULL, 0, 0, 640, 480, 320, 240, 0x11);  /* Z8 */

    unsigned char r_top[4] = {0}, r_bot[4] = {0}, c_top[4] = {0}, c_bot[4] = {0}, bg[4] = {0}, gr[4] = {0}, mg[4] = {0};
    unsigned char o_top[4] = {0}, o_mid[4] = {0}, o_bot[4] = {0};
    if (to) { efb_composite(((EfbCopyTex *) to)->t.id); efb_px_read(480, 3, o_top); efb_px_read(480, 300, o_mid); efb_px_read(480, 540, o_bot); }
    if (tr) { efb_composite(((EfbCopyTex *) tr)->t.id); efb_px_read(480, 3, r_top); efb_px_read(480, 540, r_bot); }
    if (tc) { efb_composite(((EfbCopyTex *) tc)->t.id); efb_px_read(480, 3, c_top); efb_px_read(480, 540, c_bot); }
    /* a copy texture drawn over the screen is upside down (its v=0 is the source's top): GL row y of
     * the source is screen row 543-y. Near green 600..700 x 200..300, magenta 780..880, the yellow
     * (far) alone at x 576..600, background elsewhere. */
    unsigned char z_near[4] = {0}, z_mid[4] = {0}, z_far[4] = {0}, z_clr[4] = {0};
    if (tz) { efb_composite(((EfbCopyTex *) tz)->t.id); efb_px_read(650, 293, z_near); efb_px_read(830, 293, z_mid);
              efb_px_read(588, 293, z_far); efb_px_read(500, 93, z_clr); }
    efb_composite(g_efb_m_tex);
    efb_px_read(500, 450, bg); efb_px_read(650, 250, gr); efb_px_read(830, 250, mg);
    /* (d) by COUNT. Until run 104 it was the one pixel above, and a pixel whose reloaded depth is
     * garbage that happens to be NEAR keeps the green too: the test agreed with the bug. The whole
     * near quad, 5 px inside its edges. Since run 108 (f)'s resolve writes every channel and M's
     * colour is drawn back from S: (c) and (d) are also the first check of that restore. */
    unsigned d_green = 0, d_yellow = 0, d_other = 0;
    /* the margin inside the quad's edges, in PANEL pixels: 5 at the panel size (the quad is then counted as 90x90 = 8100).
     * M is stretched over the panel, so a quad edge is blurred by about one M pixel = 960/g_rw panel pixels: the margin
     * grows with that factor (rounded up), and the wanted count with it. */
    const int d_mx = 5 * ((VITA_SCR_W + g_rw - 1) / g_rw), d_my = 5 * ((VITA_SCR_H + g_vh - 1) / g_vh);
    const unsigned d_want = (unsigned) ((100 - 2 * d_mx) * (100 - 2 * d_my));
    {
        unsigned char *fr = (unsigned char *) malloc(VITA_SCR_W * VITA_SCR_H * 4);
        if (fr) {
            glReadPixels(0, 0, VITA_SCR_W, VITA_SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, fr);
            for (int y = 200 + d_my; y < 300 - d_my; ++y) for (int x = 600 + d_mx; x < 700 - d_mx; ++x) {
                const unsigned char *p = fr + (y * VITA_SCR_W + x) * 4;
                if (efb_near(p, 0, 255, 0)) ++d_green; else if (efb_near(p, 255, 255, 0)) ++d_yellow; else ++d_other;
            }
            efb_write("zself_pre.raw", fr, VITA_SCR_W * VITA_SCR_H * 4);
            free(fr);
        }
    }
    printf("[efb] selftest (d) by count, near quad %dx%d: green %u yellow %u other %u (want %u / 0 / 0)\n", 100 - 2 * d_mx, 100 - 2 * d_my, d_green, d_yellow, d_other, d_want);

    int pa = tc && efb_near(c_top, A_R, A_G, A_B) && efb_near(c_bot, B_R, B_G, B_B);
    int pb = tr && efb_near(r_top, A_R, A_R, A_R) && efb_near(r_bot, B_R, B_R, B_R);
    int pc = efb_near(bg, K_R, K_G, K_B) && efb_near(mg, 255, 0, 255);
    int pd = efb_near(gr, 0, 255, 0) && d_green == d_want;
    printf("[efb] selftest break=%d hack=%d(first try %d) | (a) orientation %s: 565 v0row=%02x%02x%02x lastrow=%02x%02x%02x want %02x%02x%02x/%02x%02x%02x"
           " | (b) R8 %s: v0row=%02x%02x%02x lastrow=%02x%02x%02x want %02x/%02x"
           " | (c) survive %s: bg=%02x%02x%02x marker=%02x%02x%02x | (d) depth %s: near px=%02x%02x%02x (green 00ff00 = kept, yellow ffff00 = lost)\n",
           g_efb_break, (int) rhi_vita_efb_hack, hack_first,
           pa ? "PASS" : "FAIL", c_top[0], c_top[1], c_top[2], c_bot[0], c_bot[1], c_bot[2], A_R, A_G, A_B, B_R, B_G, B_B,
           pb ? "PASS" : "FAIL", r_top[0], r_top[1], r_top[2], r_bot[0], r_bot[1], r_bot[2], A_R, B_R,
           pc ? "PASS" : "FAIL", bg[0], bg[1], bg[2], mg[0], mg[1], mg[2],
           pd ? "PASS" : "FAIL", gr[0], gr[1], gr[2]);
    printf("[efb] selftest (e) odd size 60x45, allocated 64x48 (RGB565) %s: v0row=%02x%02x%02x midrow=%02x%02x%02x lastrow=%02x%02x%02x (want %02x%02x%02x / %02x%02x%02x / %02x%02x%02x)\n",
           to && efb_near(o_top, A_R, A_G, A_B) && efb_near(o_mid, B_R, B_G, B_B) && efb_near(o_bot, B_R, B_G, B_B) ? "PASS" : "FAIL",
           o_top[0], o_top[1], o_top[2], o_mid[0], o_mid[1], o_mid[2], o_bot[0], o_bot[1], o_bot[2], A_R, A_G, A_B, B_R, B_G, B_B, B_R, B_G, B_B);
    {
        int pf = tz && abs((int) z_near[0] - 64) <= 2 && abs((int) z_mid[0] - 128) <= 2 && abs((int) z_far[0] - 192) <= 2 && z_clr[0] >= 253;
        g_efb_zfail = !pf;
        printf("[efb] selftest (f) Z8 whole screen -> 320x240 %s (zmode %d): near=%u mid=%u far=%u cleared=%u (want 64 / 128 / 192 / 255, +-2; near texel rgba %02x%02x%02x%02x)\n",
               pf ? "PASS" : "FAIL", g_efb_zmode, z_near[0], z_mid[0], z_far[0], z_clr[0], z_near[0], z_near[1], z_near[2], z_near[3]);
    }
    if (tz) rhi_vita_copyEfbFree(tz);
    /* (h) variant 2 leaves M's colour as it found it. A known picture is put in M (colour only: (f2)
     * still needs the depth), read, a Z copy is made, and M is read again: pixels that differ, over
     * the whole screen, for a whole-screen copy and for a sub-rectangle (whose scissor could be
     * upside down without the first one noticing). The reference is read BEFORE the copy, so with
     * efb_norestore.txt nearly every pixel of the rectangle differs. */
    if (g_efb_zmode && !g_efb_break) {
        static const int hr[2][4] = { { 0, 0, 640, 480 }, { 60, 60, 120, 90 } };
        unsigned char *ref = (unsigned char *) malloc(VITA_SCR_W * VITA_SCR_H * 4), *cur = (unsigned char *) malloc(VITA_SCR_W * VITA_SCR_H * 4);
        h_ok = ref && cur;
        if (!h_ok) printf("[efb] selftest (h) SKIPPED: no memory for two pictures\n");
        for (int k = 0; ref && cur && k < 2; ++k) {   /* both rectangles, whatever the first one said */
            efb_bind_m();
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_SCISSOR_TEST);
            /* the two tone rectangles are given in panel pixels; scaled to M's size (identity at 960x544) */
            glScissor(0, 0, g_rw, g_rh); glClearColor(K_R / 255.0f, K_G / 255.0f, K_B / 255.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
            glScissor(40 * g_rw / VITA_SCR_W, (g_rh - g_vh) + 300 * g_vh / VITA_SCR_H, 500 * g_rw / VITA_SCR_W, 200 * g_vh / VITA_SCR_H);   /* GL rows: the picture sits at the TOP of M */
            glClearColor(A_R / 255.0f, A_G / 255.0f, A_B / 255.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
            glScissor(400 * g_rw / VITA_SCR_W, (g_rh - g_vh) + 40 * g_vh / VITA_SCR_H, 500 * g_rw / VITA_SCR_W, 330 * g_vh / VITA_SCR_H);
            glClearColor(B_R / 255.0f, B_G / 255.0f, B_B / 255.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            efb_composite(g_efb_m_tex);
            glReadPixels(0, 0, VITA_SCR_W, VITA_SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, ref);
            efb_bind_m();
            RhiTexture *th = rhi_vita_copyEfb(NULL, hr[k][0], hr[k][1], hr[k][2], hr[k][3], 64, 48, 0x11);
            efb_composite(g_efb_m_tex);
            glReadPixels(0, 0, VITA_SCR_W, VITA_SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, cur);
            unsigned any = 0, far3 = 0, tones = 0;
            for (unsigned i = 0; i < (unsigned) (VITA_SCR_W * VITA_SCR_H); ++i) {
                const unsigned char *a = ref + i * 4, *b = cur + i * 4;
                int d0 = abs((int) a[0] - b[0]), d1 = abs((int) a[1] - b[1]), d2 = abs((int) a[2] - b[2]);
                any += (d0 | d1 | d2) != 0; far3 += d0 > 3 || d1 > 3 || d2 > 3;
                tones += efb_near(a, A_R, A_G, A_B) || efb_near(a, B_R, B_G, B_B);
            }
            /* the reference must BE the picture, or two equal garbage reads would pass */
            int ok = th && far3 == 0 && tones > 100000;
            printf("[efb] selftest (h) colour kept across a Z copy, EFB rect %d,%d %dx%d %s%s: of %u px, %u differ at all, %u by more than 3 (want 0); reference holds %u px of the two tones (want > 100000)\n",
                   hr[k][0], hr[k][1], hr[k][2], hr[k][3], ok ? "PASS" : "FAIL", g_efb_norestore ? " [efb_norestore.txt: FAIL is the expected reading]" : "",
                   (unsigned) (VITA_SCR_W * VITA_SCR_H), any, far3, tones);
            h_ok = h_ok && ok;
            if (th) rhi_vita_copyEfbFree(th);
        }
        free(ref); free(cur);
        efb_bind_m();
    }
    /* (f2) run 102: (f) read 0 / 0 / 62 / 52. Which part is wrong -- the mask, or the number of
     * slices? The four variants at the same four points; M's depth is intact (nothing here writes
     * it), its colour is not needed any more (the unmasked ones overwrite it). INFO, raw values. */
    if (g_efb_zmode && !g_efb_break) {
        static const int var[4][2] = { { 1, 1 }, { 0, 1 }, { 1, 16 }, { 0, 16 } };
        for (int k = 0; k < 4; ++k) {
            unsigned char pa[4] = {0}, pb2[4] = {0}, pc2[4] = {0}, pd2[4] = {0};
            efb_bind_m();
            g_efb_zmasked = var[k][0]; g_efb_zstep = var[k][1];
            RhiTexture *t2 = rhi_vita_copyEfb(NULL, 0, 0, 640, 480, 320, 240, 0x11);
            /* run 103: all four variants read the same wrong values, and 16 slices gave one that is
             * not a multiple of 16 -- so LOOK at it: M right after the all-channels resolve (its RGB is
             * then the slice index), and the alpha-only copy as the screen shows it. Raw RGBA, 960x544,
             * bottom row first. */
            if (k == 1) { efb_composite(g_efb_m_tex); efb_dump("zself_m.raw"); }
            if (t2) { efb_composite(((EfbCopyTex *) t2)->t.id); if (k == 0) efb_dump("zself_copy.raw"); efb_px_read(650, 293, pa); efb_px_read(830, 293, pb2); efb_px_read(588, 293, pc2); efb_px_read(500, 93, pd2);
                      rhi_vita_copyEfbFree(t2); }
            printf("[efb] selftest (f2) %s, %d slices: near=%u mid=%u far=%u cleared=%u (want 64 / 128 / 192 / %d)\n",
                   var[k][0] ? "alpha only" : "all channels", 256 / var[k][1], pa[0], pb2[0], pc2[0], pd2[0], var[k][1] == 1 ? 255 : 240);
        }
        g_efb_zmasked = 2; g_efb_zstep = 1;
    }
    if (g_efb_zmode) {   /* (g) interleaved, so drift shows as disagreement between samples of one arm */
        unsigned us[3][8];
        for (int s = 0; s < 3; ++s) for (int a = 0; a < 8; ++a) us[s][a] = efb_time_arm(a);
        printf("[efb] selftest (g) depth resolve cost, us per drained scene (3 samples): empty %u %u %u | whole screen, alpha only %u %u %u | whole screen, all channels %u %u %u | 96x96, alpha only %u %u %u | 96x96, all channels %u %u %u | whole screen, all channels, 16 slices %u %u %u\n",
               us[0][0], us[1][0], us[2][0], us[0][1], us[1][1], us[2][1], us[0][2], us[1][2], us[2][2], us[0][3], us[1][3], us[2][3],
               us[0][4], us[1][4], us[2][4], us[0][5], us[1][5], us[2][5]);
        printf("[efb] selftest (g2) the whole Z COPY, whole screen -> 320x240, us per drained scene (3 samples): save + resolve + copy + restore (production) %u %u %u | resolve + copy, colour destroyed %u %u %u\n",
               us[0][6], us[1][6], us[2][6], us[0][7], us[1][7], us[2][7]);
    }
    /* The restore that did not pass (h) is not given to the game: the slow variant keeps the colour. */
    g_efb_norestore = 0;
    if (g_efb_zmode && !h_ok) { g_efb_zmasked = 1; printf("[efb] Z copies use the ALPHA-ONLY resolve for this boot (~300 ms per whole-screen copy): self-test (h) did not pass\n"); }
    printf("[efb] Z copy variant for the game: %d (2 = all channels + colour save/restore, 1 = alpha only)\n", g_efb_zmasked);
    /* SLICES (2026-10-08, docs/47 "Run 138"): with the render-target wait gone the Z resolve shows up as GPU time -- the
     * prologue swap waits 37-39 ms a frame with one whole-screen Z copy in it, 256 depth-tested full-screen quads. The step
     * between slices is the precision: 1 = 8 bits (256 quads), 4 = 6 bits (64). ux0:data/stairfax/efb_zstep.txt, one of
     * 1 2 4 8 16; absent = 1. The self-test above always runs at 1. */
    {   FILE *zf = fopen("ux0:data/stairfax/efb_zstep.txt", "r"); int zs = 1;
        if (zf) { if (fscanf(zf, "%d", &zs) != 1) zs = 1; fclose(zf); }
        if (zs != 1 && zs != 2 && zs != 4 && zs != 8 && zs != 16) zs = 1;
        g_efb_zstep = zs;
        printf("[efb] Z resolve: %d slices (efb_zstep.txt %d)\n", 256 / zs, zs);
    }
    efb_dz_stat("end of the self-test");
    if (to) rhi_vita_copyEfbFree(to);
    if (!rhi_vita_efb_hack)
        printf("[efb] depth hack NOT applied (validation failed %u time(s)): depth will not survive a copy; readEfbRect keeps its depth clear\n", g_efb_hack_fails);

    if (tr) rhi_vita_copyEfbFree(tr);
    if (tc) rhi_vita_copyEfbFree(tc);
    g_efb_break = 0;
    rhi_vita_efb_gpu_copies = 0; rhi_vita_efb_refused_fmt = 0;   /* the game's counters start at zero */
    rhi_vita_efb_z_copies = 0; rhi_vita_efb_z_kpx = 0;
    /* A Z copy that fails its own test is worse than none: a wrong texture, at up to 296 ms a frame
     * (run 103). The game gets Z copies only from a resolve that passed (f) on this boot. */
    if (g_efb_zmode && g_efb_zfail) { g_efb_zmode = 0; printf("[efb] Z copies OFF for this boot: self-test (f) FAILED (zmode forced to 0)\n"); }
    efb_bind_m();
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);   /* M clean for the game */
    gs_cur = keep;
}

/* ---- init (from rhi_gl_create, after vitaGL is up) --------------------------------------------- */
static void efb_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    int want = 1;
    SceUID f = sceIoOpen(EFB_DIR "efb_mode.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) {
        char b[16] = { 0 };
        int n = sceIoRead(f, b, sizeof b - 1);
        sceIoClose(f);
        if (n > 0) want = atoi(b);
        if (want < 0 || want > 2) { printf("[efb] efb_mode.txt holds %d: not 0/1/2, using 1\n", want); want = 1; }
    }
    printf("[efb] requested mode %d (%s)\n", want, f >= 0 ? "efb_mode.txt" : "no efb_mode.txt: default");
    rhi_vita_efb_mode = (unsigned) want;
    if (want == 1) efb_res_init();   /* before M exists */
    else printf("[res] render target %dx%d (efb mode %d: the game draws to the default framebuffer, render_res.txt not used)\n", g_rw, g_rh, want);
    if (want != 1) { printf("[efb] mode %d: direct rendering%s\n", want, want == 2 ? " + the CPU readback" : ", no real copies"); return; }
    f = sceIoOpen(EFB_DIR "efb_z.txt", SCE_O_RDONLY, 0777);
    if (f >= 0) {
        char b[16] = { 0 };
        int n = sceIoRead(f, b, sizeof b - 1);
        sceIoClose(f);
        if (n > 0) g_efb_zmode = atoi(b);
        if (g_efb_zmode < 0 || g_efb_zmode > 2) { printf("[efb] efb_z.txt holds %d: not 0/1/2, using 1\n", g_efb_zmode); g_efb_zmode = 1; }
    }
    printf("[efb] Z copies: zmode %d (%s)\n", g_efb_zmode, f >= 0 ? "efb_z.txt" : "no efb_z.txt: default");

    {   FILE *cf = fopen(EFB_DIR "copyfilter_off.txt", "r"); int off = cf && fgetc(cf) == 49; if (cf) fclose(cf);
        g_cf_on = !off;
        printf("[copyfilter] %s: copy textures %s; R4/I4 copies %s, RGB565/5A3 %s\n", g_cf_on ? "ON" : "OFF",
               g_cf_on ? "take the min/mag filter and wrap of the GXTexObj that samples them" : "stay GL_LINEAR / CLAMP, whatever the game asks (copyfilter_off.txt, today's behaviour)",
               g_cf_on ? "keep 4 bits" : "keep 8 bits", g_cf_on ? "keep 5/6/5 and 5/5/5" : "keep 8 bits");
    }
    const char *why = NULL;
    if (!efb_build_programs()) why = "programs unavailable";
    else {
        static const float qv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        glGenBuffers(1, &g_efb_qvbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_efb_qvbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizei) sizeof qv, qv, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glGenTextures(1, &g_efb_m_tex);
        glGenFramebuffers(1, &g_efb_m_fbo);
        glBindTexture(GL_TEXTURE_2D, g_efb_m_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rw, g_rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* the probe used NEAREST; LINEAR is 1:1 on the screen draw and smooths a downscaled copy */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_m_fbo);
        RHI_SCENE_TAG('M', 'o');
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_m_tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        printf("[efb] fbo M status=0x%04x (0x8cd5 = complete)\n", (unsigned) st);
        if (st != GL_FRAMEBUFFER_COMPLETE) { glBindFramebuffer(GL_FRAMEBUFFER, 0); RHI_SCENE_TAG('D', 'o'); why = "M incomplete"; }
    }
    if (why) {
        rhi_vita_efb_mode = 0;
        printf("[efb] FALLING BACK to mode 0: %s\n", why);
        if (efb_res_differs() || g_efb528) { efb_res_panel(); printf("[res] render target %dx%d (efb mode 0 fallback: forced to the panel)\n", g_rw, g_rh); }
        return;
    }
    g_efb_on = 1;
    ++rhi_vita_efb_switches;
    efb_selftest();
    g_efb_selftest_done = 1;
    scn_break_init();   /* scene_gpu_break.txt: the break arm's program, target and drained cost (nothing without the file) */
    printf("[efb] mode 1 live: the game renders into M, copies are GPU quads\n");
}
