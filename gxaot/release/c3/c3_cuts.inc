/* c3_cuts.inc -- GXAOT release track C3, the GL-facing half (see c3_cuts.h). Included by P/src/rhi_vita_tev.inc right after
 * `static int g_gxp_n;` (patches/C3-perdraw/001), and by the host test with mock gl*. Needs: GxProg / g_gxp, VsKey, VsUniforms, TevKey,
 * TevUniforms, TEVK_TEX, VS_MAX_SLOTS, VS_MAX_LIGHTS, VsLight, GLint, glUniform4f / glUniform4fv / glUniform2f, printf, calloc, memcmp.
 *
 * ARM 101 -- THE UNIFORM SHADOW. drawTevGx sends ~30 glUniform* per draw (docs/47 run 270: the GL state half of a draw is ~13 us, arm 3 - arm 2;
 * run_t0b: drawTevGx outside glDrawElements = 28.4 - 15.7 = 12.7 us/draw, [M] run_t0b.txt:22572). vitaGL keeps each uniform's value in the PROGRAM's own
 * CPU-side buffer (custom_shaders.c glUniform* -> vglSetUniformData -> p->unif_vbuffer / unif_fbuffer) and copies the whole buffer to the GPU at
 * the draw (upload_uniforms), so a glUniform whose value equals what that program's buffer already holds changes nothing: skipping it is exact.
 * The shadow is per GxProg slot; it is wiped when the slot's program is destroyed or the slot is given to another key (C3_SLOT_RESET, the same two
 * hooks as the arm-100 generation). uPos / uNrm / uTMX (30 vec4 each, per object) are NOT shadowed: they change on almost every draw and a compare
 * costs as much as the send. Groups, not single uniforms, so one compare covers 4 glUniform4f. */

#ifndef C3_NOW
#define C3_NOW() sceKernelGetProcessTimeWide()
#endif
#include "c3_cuts.h"

static int c3_gxp_ok(int pi) { return g_gxp[pi].ok; }

typedef struct {
    uint8_t projv, regv, konv, matv, ambv, arefv, ltv[2], ltn[2], tmv[4], ptv[4];
    float proj[16], reg[16], kon[16], mat[4], amb[4], aref[2];
    float lt[2][5][4][4];            /* [channel][q: pos dir a k col][light 0..3][xyzw]: nLights <= 4 only */
    float tm[4][12], pt[4][12];      /* texcoord slots 0..3 */
} C3Sh;
static C3Sh *c3_sh_tab[C3_GXP_MAX];
static void c3_sh_reset(int pi) { if (c3_sh_tab[pi]) memset(c3_sh_tab[pi], 0, sizeof(C3Sh)); }
/* the slot's program was destroyed, or the slot now belongs to another key: every memo entry and every shadowed value for it is dead */
#define C3_SLOT_RESET(pi) do { c3_slot_reset_gen(pi); c3_sh_reset(pi); } while (0)

/* 1 = the group equals what the program already holds: do not send. 0 = send (the caller does), and the shadow now holds it. */
static inline int c3_grp(uint8_t *valid, void *shadow, const void *now, size_t n, int have_shadow, unsigned ncalls)
{
    ++c3_st.un_groups;
    if (!have_shadow) return 0;
#ifdef C3_BREAK_SHORTCMP   /* host negative control: the compare stops 4 bytes early, so a change in the last float is missed */
    if (*valid && !memcmp(shadow, now, n > 4 ? n - 4 : n)) { ++c3_st.un_skipped; c3_st.un_calls_saved += ncalls; return 1; }
#else
    if (*valid && !memcmp(shadow, now, n)) { ++c3_st.un_skipped; c3_st.un_calls_saved += ncalls; return 1; }
#endif
    memcpy(shadow, now, n); *valid = 1; return 0;
}

/* The same uploads as drawTevGx's block between `glUseProgram(g->prog)` and the indirect/fog uploads (the host test compiles that block out of the
 * real source and holds this to it, call for call in effect). */
static void c3_upload_uniforms(const GxProg *g, int pi, const VsKey *vk, const VsUniforms *vu, const TevUniforms *u)
{
    C3Sh *s = c3_sh_tab[pi];
    if (!s) s = c3_sh_tab[pi] = (C3Sh *) calloc(1, sizeof *s);
    C3Sh dummy; const int so = s != NULL; if (!so) s = &dummy;   /* no memory: every group is sent, the dummy is never consulted */
    float rr[16], kk[16], ar[2];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { rr[r * 4 + c] = (float) u->reg[r][c]; kk[r * 4 + c] = (float) u->konst[r][c]; }
    ar[0] = (float) u->acRef0; ar[1] = (float) u->acRef1;
    ++c3_st.un_draws;
    if (!c3_grp(&s->projv, s->proj, vu->proj, sizeof s->proj, so, 4))
        for (int r = 0; r < 4; ++r) glUniform4f(g->uM[r], vu->proj[r][0], vu->proj[r][1], vu->proj[r][2], vu->proj[r][3]);
    if (!c3_grp(&s->regv, s->reg, rr, sizeof rr, so, 4))
        for (int r = 0; r < 4; ++r) glUniform4f(g->uR[r], rr[r * 4], rr[r * 4 + 1], rr[r * 4 + 2], rr[r * 4 + 3]);
    if (!c3_grp(&s->konv, s->kon, kk, sizeof kk, so, 4))
        for (int r = 0; r < 4; ++r) glUniform4f(g->uK[r], kk[r * 4], kk[r * 4 + 1], kk[r * 4 + 2], kk[r * 4 + 3]);
    const int nm = vk->hasMtxIdx ? 3 * VS_MAX_SLOTS : 3;
    if (g->uPos >= 0) glUniform4fv(g->uPos, nm, &vu->pm[0][0][0]);
    if (g->uNrm >= 0) glUniform4fv(g->uNrm, nm, &vu->nm[0][0][0]);
    if (g->uTMX >= 0) glUniform4fv(g->uTMX, 30, &vu->tmx[0][0][0]);
    if (g->uMat >= 0 && !c3_grp(&s->matv, s->mat, vu->mat, sizeof s->mat, so, 1)) glUniform4fv(g->uMat, 1, vu->mat);
    if (g->uAmb >= 0 && !c3_grp(&s->ambv, s->amb, vu->amb, sizeof s->amb, so, 1)) glUniform4fv(g->uAmb, 1, vu->amb);
    for (int c = 0; c < 2; ++c) {
        const int nl = vk->ch[c].nLights; const GLint *loc = c ? g->uAL : g->uCL;
        if (!nl) continue;
        float a[5][VS_MAX_LIGHTS][4];
        for (int i = 0; i < nl && i < VS_MAX_LIGHTS; ++i) {
            const VsLight *l = &vu->lt[c][i];
            memcpy(a[0][i], l->pos, 16); memcpy(a[1][i], l->dir, 16); memcpy(a[2][i], l->a, 16); memcpy(a[3][i], l->k, 16); memcpy(a[4][i], l->col, 16);
        }
        unsigned nc = 0; for (int q = 0; q < 5; ++q) nc += loc[q] >= 0;
        ++c3_st.un_groups;
        if (so && nl <= 4) {
            int same = s->ltv[c] && s->ltn[c] == nl;
            for (int q = 0; same && q < 5; ++q) if (memcmp(s->lt[c][q], a[q], (size_t) nl * 16)) same = 0;
            if (same) { ++c3_st.un_skipped; c3_st.un_calls_saved += nc; continue; }
            for (int q = 0; q < 5; ++q) memcpy(s->lt[c][q], a[q], (size_t) nl * 16);
            s->ltn[c] = (uint8_t) nl; s->ltv[c] = 1;
        }
        for (int q = 0; q < 5; ++q) if (loc[q] >= 0) glUniform4fv(loc[q], nl, &a[q][0][0]);
    }
    for (int sl = 0; sl < vk->numTex && sl < TEVK_TEX; ++sl) {
        if (g->uTM[sl] >= 0) {
            if (sl >= 4 || c3_grp(&s->tmv[sl], s->tm[sl], vu->tm[sl], 48, so, 1) == 0) glUniform4fv(g->uTM[sl], 3, &vu->tm[sl][0][0]);
        }
        if (g->uPT[sl] >= 0) {
            if (sl >= 4 || c3_grp(&s->ptv[sl], s->pt[sl], vu->pt[sl], 48, so, 1) == 0) glUniform4fv(g->uPT[sl], 3, &vu->pt[sl][0][0]);
        }
    }
    if (!c3_grp(&s->arefv, s->aref, ar, sizeof ar, so, 1)) glUniform2f(g->uAref, u->acRef0, u->acRef1);
}

/* ---- the entry points vita_tool_main.c (vt_perf_ab) calls, weak ---------------------------------------------------------------------------- */
extern unsigned gx_c3_vsb_calls C3_EXT_ATTR, gx_c3_vsb_hits C3_EXT_ATTR, gx_c3_vsb_inval C3_EXT_ATTR, gx_c3_vs_n C3_EXT_ATTR, gx_c3_clk C3_EXT_ATTR;
extern unsigned long long gx_c3_vs_us C3_EXT_ATTR;   /* U gx_draw.c, patch 003 */

void rhi_vita_c3_frame_edge(int arm, unsigned long long now_us) { c3_frame_edge_h(c3_arm_mask(arm), now_us); }

/* ONE line per 60-present window. The judge (c3_judge.py C3 regex) and this printf are tied by the host test (it formats a window and parses it). */
void rhi_vita_c3_window(int from, int to, int arm)
{
    static C3Stats p; static unsigned long long p_vus; static unsigned p_vn, p_vc, p_vh, p_vi, p_clk, p_td, p_tm, p_tb;
    const double nf = to > from ? (double) (to - from) : 1.0;
    const unsigned mask = c3_arm_mask(arm);
    const unsigned vc = &gx_c3_vsb_calls ? gx_c3_vsb_calls : 0, vh = &gx_c3_vsb_hits ? gx_c3_vsb_hits : 0, vi = &gx_c3_vsb_inval ? gx_c3_vsb_inval : 0,
                   vn = &gx_c3_vs_n ? gx_c3_vs_n : 0, clk = &gx_c3_clk ? gx_c3_clk : 0;
    const unsigned long long vus = &gx_c3_vs_us ? gx_c3_vs_us : 0;
    const uint32_t now_v = &vsync_interval ? vsync_interval : 0;
    const uint32_t want = (c3_pace_orig == 0) ? 0u : (((mask & C3_B_PACE) || c3_pace_default) ? 2u : (uint32_t) (c3_pace_orig < 0 ? 1 : c3_pace_orig));
    const unsigned lkn = c3_st.lk_n - p.lk_n, un = c3_st.uni_n - p.uni_n;
    const unsigned gfn = c3_st.gf_n - p.gf_n, gfnf = c3_st.gf_nf - p.gf_nf;
    printf("[c3] presents %d..%d arm %d cuts %u | vsync_interval %u want %u sets %u | frames 16.7ms %u 33.3ms %u 50.0ms %u 66.7ms %u 83.3ms+ %u | 33.3ms bucket mean|t-33.3| %.2f ms"
           " | lookup find/fr %.1f hit/fr %.1f vfail/fr %.2f put/fr %.2f | lookup us/call %.2f (n=%u)"
           " | uniform draws/fr %.1f groups/fr %.1f skipped/fr %.1f calls_saved/fr %.1f | uniform us/call %.2f (n=%u)"
           " | vsbuild calls/fr %.1f hits/fr %.1f inval/fr %.2f%s | vsbuild us/call %.2f (n=%u) | gxclk reads/fr %.0f toff %d | immtev draws/fr %.1f memo-hits/fr %.1f batches/fr %.1f"
           " | gpuflush calls/fr %.1f flushed/fr %.1f us/call %.2f (n=%u) us/flushed %.2f (n=%u) | gxp calls/fr %.1f slhit/fr %.1f | pclk reads/fr %.0f"
           " | immcensus x8/fr n %.1f nobatch %.1f same %.1f | differs prog %.1f mode %.1f depth %.1f tex %.1f u %.1f mvp %.1f | only prog %.1f mode %.1f depth %.1f tex %.1f u %.1f mvp %.1f\n",
           from, to, arm, mask, now_v, want, c3_st.pace_sets,
           c3_st.ft[0], c3_st.ft[1], c3_st.ft[2], c3_st.ft[3], c3_st.ft[4],
           c3_st.ft_n33 ? (double) c3_st.ft_dev33 / c3_st.ft_n33 / 1000.0 : 0.0,
           (c3_st.pm_find - p.pm_find) / nf, (c3_st.pm_hit - p.pm_hit) / nf, (c3_st.pm_vfail - p.pm_vfail) / nf, (c3_st.pm_put - p.pm_put) / nf,
           lkn ? (double) (c3_st.lk_us - p.lk_us) / lkn : 0.0, lkn,
           (c3_st.un_draws - p.un_draws) / nf, (c3_st.un_groups - p.un_groups) / nf, (c3_st.un_skipped - p.un_skipped) / nf, (c3_st.un_calls_saved - p.un_calls_saved) / nf,
           un ? (double) (c3_st.uni_us - p.uni_us) / un : 0.0, un,
           (vc - p_vc) / nf, (vh - p_vh) / nf, (vi - p_vi) / nf, &gx_c3_vsb_calls ? "" : " (U patch 003 not linked)",
           (vn - p_vn) ? (double) (vus - p_vus) / (vn - p_vn) : 0.0, vn - p_vn, (clk - p_clk) / nf, (mask & C3_B_TOFF) != 0,
           (rhi_vita_tev_draws - p_td) / nf, (rhi_vita_tev_memo_hits - p_tm) / nf, (rhi_vita_tev_batches - p_tb) / nf,
           (c3_st.gf_calls - p.gf_calls) / nf, (c3_st.gf_flushed - p.gf_flushed) / nf, gfn ? (double) (c3_st.gf_us - p.gf_us) / gfn : 0.0, gfn,
           gfnf ? (double) (c3_st.gf_usf - p.gf_usf) / gfnf : 0.0, gfnf,
           (c3_st.gp_calls - p.gp_calls) / nf, (c3_st.gp_slhit - p.gp_slhit) / nf, (double) (c3_st.pclk - p.pclk) / nf,
           8.0 * (c3_st.ic_n - p.ic_n) / nf, 8.0 * (c3_st.ic_nobatch - p.ic_nobatch) / nf, 8.0 * (c3_st.ic_same - p.ic_same) / nf,
           8.0 * (c3_st.ic_diff[0] - p.ic_diff[0]) / nf, 8.0 * (c3_st.ic_diff[1] - p.ic_diff[1]) / nf, 8.0 * (c3_st.ic_diff[2] - p.ic_diff[2]) / nf,
           8.0 * (c3_st.ic_diff[3] - p.ic_diff[3]) / nf, 8.0 * (c3_st.ic_diff[4] - p.ic_diff[4]) / nf, 8.0 * (c3_st.ic_diff[5] - p.ic_diff[5]) / nf,
           8.0 * (c3_st.ic_only[0] - p.ic_only[0]) / nf, 8.0 * (c3_st.ic_only[1] - p.ic_only[1]) / nf, 8.0 * (c3_st.ic_only[2] - p.ic_only[2]) / nf,
           8.0 * (c3_st.ic_only[3] - p.ic_only[3]) / nf, 8.0 * (c3_st.ic_only[4] - p.ic_only[4]) / nf, 8.0 * (c3_st.ic_only[5] - p.ic_only[5]) / nf);
    p = c3_st; p_vus = vus; p_vn = vn; p_vc = vc; p_vh = vh; p_vi = vi; p_clk = clk; p_td = rhi_vita_tev_draws; p_tm = rhi_vita_tev_memo_hits; p_tb = rhi_vita_tev_batches;
    memset(c3_st.ft, 0, sizeof c3_st.ft); c3_st.ft_dev33 = 0; c3_st.ft_n33 = 0; p.ft_dev33 = 0; p.ft_n33 = 0; memset(p.ft, 0, sizeof p.ft);
}
