/* rhi_vita_gxdump.inc - the GAME-SIDE half of the one-frame draw-stream dump (docs/54 s5.1 H1-pre (b); B1-c2floor).  Included by rhi_vita_tev.inc, which
 * is itself included by rhi_vita.c, so everything here sees GxMesh, GxProg, TevProg, VitaTexture, g_depth and rhi_vita_scene_kind.
 *
 * WHAT IT DOES.  Between two frame edges with perf_ab arm 114 current, every draw that reaches rhi_vita_drawTevGx (the GPU vertex path: ~524 a frame
 * in chapter 2) or rhi_vita_drawTev (the immediate path) is recorded, with the program binary of its (VsKey,TevKey) pair, the mesh's vertex/index
 * BYTES (read back from vitaGL's buffers with glMapBuffer), the texture identities, the uniforms, the depth/blend state and the target kind; every
 * EFB copy is an EVENT.  At the next edge the frame is written to ux0:data/stairfax/gxdump_NN.gxd (format: gxd_format.h; reader/replayer:
 * projects/gxaot/probe/c2floor).  The formatting is gx_dump.h (host-tested); this file only does the GL reads and the file write.
 *
 * HOW IT IS SWITCHED ON.  ux0:data/stairfax/perf_ab.txt, the harness's existing marker:   from 600 period 60 arms 0 114 0
 * Arm 114 holds for 60 presents; the FIRST edge inside it begins the dump and the NEXT edge writes it, so exactly one frame is recorded per arm-114
 * window.  Reaching the window in chapter 2 is the operator's job (the scripted pad run that reaches the bench).  Absent marker = nothing here runs.
 *
 * COST WHEN OFF.  Each hook is `if (__builtin_expect(g_gxd_on, 0)) ...`: one load and one predicted-not-taken branch per draw (NOT measured; the claim is the
 * instruction count, and  -DSTFX_NO_GXDUMP  removes even that -- tests/check_zero_cost.sh shows the object code with the hooks compiled out equals the
 * unpatched file's).  While ON, a frame costs milliseconds (program binaries, mesh readback, ~8 MB of file write); that frame is not a timing sample.
 */
#ifndef STFX_NO_GXDUMP
#include <psp2/io/fcntl.h>

#define GXD_ARM 114

static unsigned g_gxd_files, g_gxd_seen_gx, g_gxd_seen_tev, g_gxd_skipped;
static int g_gxd_phase;                     /* 0 idle, 1 recording this frame, 2 wrote it: wait for the arm to leave */
static SceUInt64 g_gxd_t0;

/* a program: vitaGL's glGetProgramBinary image (attribute table + vertex GXP + fragment GXP), the same bytes the shader pack stores */
static int gxd_prog(uint32_t key, uint32_t hash, uint32_t th, uint32_t kind, GLuint prog)
{
    const int have = gxd_prog_find(key);
    if (have >= 0) return have;
    GLint want = 0; glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &want);
    if (want <= 0 || want > (1 << 20)) return -1;
    uint8_t *buf = (uint8_t *) malloc((size_t) want);
    if (!buf) return -1;
    GLsizei len = 0; GLenum fmt = 0;
    glGetProgramBinary(prog, want, &len, &fmt, buf);      /* vitaGL does not check bufSize: the length was checked above, as shc_save does */
    const int r = (len > 0 && len <= want) ? gxd_prog_add(key, hash, th, kind, buf, (uint32_t) len) : -1;
    free(buf);
    return r;
}

static int gxd_texture(RhiTexture *h)
{
    int t = gxd_tex_find(h);
    if (t >= 0) return t;
    const VitaTexture *v = (const VitaTexture *) h;
    GxdTex g; memset(&g, 0, sizeof g);
    g.w = (uint32_t) v->w; g.h = (uint32_t) v->h; g.fmt = v->fmt; g.filtMin = v->filtMin; g.filtMagNear = v->filtMagNear; g.hasMips = v->hasMips;
    g.wrapS = v->wrapS; g.wrapT = v->wrapT; g.lodBias8 = v->lodBias8;
    return gxd_tex_add(h, &g);
}

/* the GPU-path draw: rhi_vita_drawTevGx calls this after the program lookup, before any GL state is touched */
static void gxd_cap_gx(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu,
                       const void *mesh, int alphaMode, int pi)
{
    ++g_gxd_seen_gx;
    const GxMesh *m = (const GxMesh *) mesh; const GxProg *g = &g_gxp[pi];
    const int po = gxd_prog((1u << 16) | (uint32_t) pi, g->hash, g->tevHash, 'g', g->prog);
    int mo = gxd_mesh_find(m);
    if (mo < 0) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
        const void *v = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY); const void *ix = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_READ_ONLY);
        mo = (v && ix) ? gxd_mesh_add(m, &m->lay, v, m->vbytes, (const uint16_t *) ix, m->nidx) : -1;
        if (ix) glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        if (v) glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    int to[GXD_MAX_TEX]; int ok = po >= 0 && mo >= 0 && tk->numTex <= GXD_MAX_TEX;
    for (int t = 0; ok && t < tk->numTex; ++t) { to[t] = tex[t] ? gxd_texture(tex[t]) : -1; if (to[t] < 0) ok = 0; }
    if (!ok || gxd_draw_gx(po, mo, to, (uint32_t) tk->numTex, (uint32_t) alphaMode, (uint32_t) g_depth, (uint32_t) rhi_vita_scene_kind, vk, tk, vu, u) < 0) ++g_gxd_skipped;
}

/* the immediate-path call: rhi_vita_drawTev, after its program lookup (pi indexes g_tevp) */
static void gxd_cap_tev(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16],
                 int alphaMode, int pi)
{
    ++g_gxd_seen_tev;
    const TevProg *p = &g_tevp[pi];
    const int po = gxd_prog((2u << 16) | (uint32_t) pi, p->hash, p->hash, 't', p->prog);
    int to[GXD_MAX_TEX]; int ok = po >= 0 && k->numTex <= GXD_MAX_TEX;
    for (int t = 0; ok && t < k->numTex; ++t) { to[t] = tex[t] ? gxd_texture(tex[t]) : -1; if (to[t] < 0) ok = 0; }   /* drawTev checks tex[] AFTER this hook: a NULL texture is a draw the game will refuse, so it is not recorded */
    if (!ok || gxd_draw_tev(po, to, (uint32_t) k->numTex, (uint32_t) alphaMode, (uint32_t) g_depth, (uint32_t) rhi_vita_scene_kind, k, u, mvp, verts, count) < 0) ++g_gxd_skipped;
}

static int gxd_sink(void *ctx, const void *p, unsigned n) { return sceIoWrite(*(SceUID *) ctx, p, n); }
static int gxd_sink_fail(void *ctx, const void *p, unsigned n) { (void) ctx; (void) p; (void) n; return -1; }

static void gxd_write_frame(void)
{
    char path[96]; snprintf(path, sizeof path, "ux0:data/stairfax/gxdump_%02u.gxd", g_gxd_files % 100u);
    const uint32_t nd = gxd_ndraws(), nb = gxd_nbytes();
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int trunc = 0;
    long n = f >= 0 ? gxd_end(gxd_sink, &f, &trunc) : gxd_end(gxd_sink_fail, NULL, &trunc);
    if (f >= 0) sceIoClose(f);
    printf("[gxd] frame written: %s | %ld bytes (%u buffered) | %u draws recorded (gx path seen %u, immediate seen %u, skipped %u)%s | in %.1f ms | %s\n",
           path, n, nb, nd, g_gxd_seen_gx, g_gxd_seen_tev, g_gxd_skipped, trunc ? " | TRUNCATED: the replay app will REFUSE this file" : "",
           (double) (sceKernelGetProcessTimeWide() - g_gxd_t0) / 1000.0, n < 0 ? "WRITE FAILED" : "ok");
    ++g_gxd_files;
}

/* once per present, BEFORE that frame's GX work (gx_rec.c's gx_rec_frame_edge calls this with the current perf_ab arm) */
void rhi_vita_gxd_edge(int arm)
{
    if (g_gxd_phase == 1) { gxd_write_frame(); g_gxd_phase = 2; }     /* the frame that just ended is complete */
    if (arm != GXD_ARM) { if (g_gxd_phase == 2) g_gxd_phase = 0; return; }   /* leaving the arm re-arms it for the next window */
    if (g_gxd_phase != 0) return;
    g_gxd_seen_gx = g_gxd_seen_tev = g_gxd_skipped = 0;
    g_gxd_t0 = sceKernelGetProcessTimeWide();
    int rw = 0, rh = 0; rhi_vita_render_size(&rw, &rh);
    if (gxd_begin((uint32_t) rw, (uint32_t) rh, (uint32_t) rhi_vita_presents, NULL, 0)) { g_gxd_phase = 1; printf("[gxd] recording the frame of present %d (M %dx%d)\n", rhi_vita_presents, rw, rh); }
    else printf("[gxd] cannot begin (out of memory)\n");
}
#endif /* STFX_NO_GXDUMP */
