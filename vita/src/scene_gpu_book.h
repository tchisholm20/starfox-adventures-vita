/* scene_gpu_book.h -- the BOOKKEEPING of the per-GXM-scene GPU-time instrument (scene_gpu.c).
 *
 * Pure C, no Vita header: the device build (scene_gpu.c) and the host test
 * (host/tests/scene_gpu_book_test.c) compile the SAME text, so what the test proves about the
 * interval arithmetic, the type buckets, the percentiles, the validity verdicts and the printed
 * lines is true of the binary on the console.
 *
 * THE QUANTITY. The GPU executes scenes in submission order. Scene i is handed over at submit[i]
 * (sceGxmEndScene) and its fragment job finishes at done[i] (the fragment notification). The GPU
 * could not have started it before the scene in front of it finished, nor before it was submitted:
 *
 *     gpu[i] = done[i] - max(done[i-1], submit[i])
 *
 * Summed over a frame's scenes this is the GPU's BUSY time on that frame, never more than the wall
 * time those scenes span.
 *
 * A scene TYPE is (target kind, depth loaded / stored at BeginScene, why the scene ended). */
#ifndef SCENE_GPU_BOOK_H
#define SCENE_GPU_BOOK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SGB_MAX_TYPES 32
#define SGB_SAMPLES   2048    /* per type and window; a reservoir past that (the count and the sum stay exact) */
#define SGB_FRAMES    1024    /* per-window frame samples for the frame-level percentiles */
#define SGB_BOUND     0.15    /* every validity verdict below uses this one bound */

#define SGB_F_LOAD  1u
#define SGB_F_STORE 2u

typedef struct {
    uint8_t  kind, ls, why, used;
    uint32_t n;                 /* scenes of this type in the window (exact) */
    uint64_t gpu_us;            /* their GPU time (exact) */
    uint32_t max_us;
    uint64_t begin_us, end_us;  /* CPU time inside the real sceGxmBeginScene / sceGxmEndScene */
    uint64_t drain_us;          /* drain arm: EndScene entry -> sceGxmFinish return */
    uint32_t nsamp;
    uint32_t samp[SGB_SAMPLES];
} SgbType;

typedef struct {
    SgbType  t[SGB_MAX_TYPES];
    int      nt;
    uint32_t type_overflow;     /* scenes whose type did not fit the table: counted, in no row */
    uint32_t frames;
    uint64_t tot_gpu, tot_wall, tot_cpu, tot_swap, tot_scenes, tot_begin, tot_end, tot_drain;
    uint32_t f_gpu[SGB_FRAMES], f_wall[SGB_FRAMES], f_cpu[SGB_FRAMES], f_swap[SGB_FRAMES], f_scenes[SGB_FRAMES];
    uint64_t cur_gpu; uint32_t cur_scenes;   /* the frame being fed */
    uint32_t rng;
    /* in-boot A/B/A (mode 3): frames by phase & 3 -- phases 1,3 instrument ON, 0,2 OFF */
    uint32_t ab_n[4]; uint64_t ab_wall[4], ab_cpu[4], ab_swap[4];
} SgbWin;

/* ---- the interval arithmetic ------------------------------------------------------------------ */
static uint32_t sgb_scene_us(uint64_t done, uint64_t prev_done, uint64_t submit)
{
#ifdef SGB_BREAK_1          /* the wrong formula: every scene charged from its own submit. Under any */
    uint64_t start = submit; /* pipelining this counts the scenes in front of it again. */
    (void) prev_done;
#else
    uint64_t start = prev_done > submit ? prev_done : submit;
#endif
    return done > start ? (uint32_t) (done - start) : 0u;
}

/* ---- types ------------------------------------------------------------------------------------ */
static int sgb_type(SgbWin *w, uint8_t kind, uint8_t ls, uint8_t why)
{
#ifdef SGB_BREAK_2          /* the wrong bucketing: depth load/store ignored */
    ls = 0;
#endif
    for (int i = 0; i < w->nt; ++i)
        if (w->t[i].kind == kind && w->t[i].ls == ls && w->t[i].why == why) return i;
    if (w->nt >= SGB_MAX_TYPES) return -1;
    SgbType *t = &w->t[w->nt];
    memset(t, 0, sizeof *t);
    t->kind = kind; t->ls = ls; t->why = why; t->used = 1;
    return w->nt++;
}

static void sgb_add_scene(SgbWin *w, uint8_t kind, uint8_t ls, uint8_t why, uint32_t gpu_us,
                          uint32_t begin_us, uint32_t end_us, uint32_t drain_us)
{
    int i = sgb_type(w, kind, ls, why);
    w->cur_gpu += gpu_us; w->cur_scenes++;
    w->tot_begin += begin_us; w->tot_end += end_us; w->tot_drain += drain_us;
    if (i < 0) { w->type_overflow++; return; }
    SgbType *t = &w->t[i];
    t->n++; t->gpu_us += gpu_us; t->begin_us += begin_us; t->end_us += end_us; t->drain_us += drain_us;
    if (gpu_us > t->max_us) t->max_us = gpu_us;
    if (t->nsamp < SGB_SAMPLES) t->samp[t->nsamp++] = gpu_us;
    else {
        w->rng = w->rng * 1664525u + 1013904223u;
        uint32_t j = (uint32_t) (((uint64_t) (w->rng >> 8) * t->n) >> 24);
        if (j < SGB_SAMPLES) t->samp[j] = gpu_us;
    }
}

static void sgb_end_frame(SgbWin *w, uint32_t wall_us, uint32_t cpu_us, uint32_t swap_us)
{
    if (w->frames < SGB_FRAMES) {
        uint32_t f = w->frames;
        w->f_gpu[f] = (uint32_t) w->cur_gpu; w->f_wall[f] = wall_us; w->f_cpu[f] = cpu_us; w->f_swap[f] = swap_us;
        w->f_scenes[f] = w->cur_scenes;
    }
    w->frames++;
    w->tot_gpu += w->cur_gpu; w->tot_scenes += w->cur_scenes;
    w->tot_wall += wall_us; w->tot_cpu += cpu_us; w->tot_swap += swap_us;
    w->cur_gpu = 0; w->cur_scenes = 0;
}

static void sgb_ab_frame(SgbWin *w, unsigned phase, uint32_t wall_us, uint32_t cpu_us, uint32_t swap_us)
{
    phase &= 3u;
    w->ab_n[phase]++; w->ab_wall[phase] += wall_us; w->ab_cpu[phase] += cpu_us; w->ab_swap[phase] += swap_us;
}

/* ---- percentiles (nearest rank over a sorted copy) --------------------------------------------- */
static int sgb_cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    return x < y ? -1 : x > y;
}
/* v is SORTED IN PLACE. pct 0..100. */
static void sgb_sort(uint32_t *v, uint32_t n) { if (n > 1) qsort(v, n, sizeof *v, sgb_cmp_u32); }
static uint32_t sgb_pct_sorted(const uint32_t *v, uint32_t n, unsigned pct)
{
    if (!n) return 0;
#ifdef SGB_BREAK_3          /* the wrong percentile: the index is not scaled by pct, so every one is the minimum */
    (void) pct;
    return v[0];
#else
    return v[(uint32_t) (((uint64_t) pct * (n - 1) + 50u) / 100u)];
#endif
}

/* ---- the validity verdicts --------------------------------------------------------------------- */
enum { SGB_SUM_NA_CPU, SGB_SUM_NA_VSYNC, SGB_SUM_VALID, SGB_SUM_INVALID_HIGH, SGB_SUM_INVALID_LOW };
/* Per-frame means of one window, any one unit.
 *
 * What can be said without assuming how the CPU and the GPU overlap:
 *   - the GPU cannot be busy for longer than the wall time:       gpu <= wall   (always)
 *   - with vsync off, the time the game thread sits in the swap is time it waits for the GPU to
 *     finish an older frame, so the GPU is busy throughout it:    gpu >= swap
 * `wall - frame CPU` (the requester's reference) is swap plus whatever else runs outside the frame
 * build; it is printed, and gpu / (wall - CPU) is the ratio the verdict quotes. It equals 1 only if
 * the CPU and the GPU never work at the same time; with the GPU saturated it is wall / (wall - CPU).
 * So the verdict is the BRACKET [swap, wall] at the 15% bound, not ratio == 1.
 *
 * CPU-bound (the swap is under 15% of the frame: nothing waits for the GPU) the lower bound is ~0
 * and only the upper one can fire: NOT APPLICABLE. With vsync on the swap also holds the vblank
 * wait, so it bounds nothing: NOT APPLICABLE. A sum above the wall time is INVALID in every regime. */
static int sgb_sum_check(double gpu, double wall, double cpu, double swap, int vsync_off, double *ratio)
{
    *ratio = wall - cpu > 0.0 ? gpu / (wall - cpu) : 0.0;
#ifndef SGB_BREAK_4         /* the wrong check: no upper bound, a sum of twice the wall time passes */
    if (gpu > (1.0 + SGB_BOUND) * wall) return SGB_SUM_INVALID_HIGH;
#endif
    if (swap < SGB_BOUND * wall) return SGB_SUM_NA_CPU;
    if (!vsync_off) return SGB_SUM_NA_VSYNC;
    if (gpu < (1.0 - SGB_BOUND) * swap) return SGB_SUM_INVALID_LOW;
    return SGB_SUM_VALID;
}

/* a measured against a reference: 1 = within the bound */
static int sgb_within(double a, double ref)
{
    if (ref <= 0.0) return 0;
    double r = a / ref;
    return r >= 1.0 - SGB_BOUND && r <= 1.0 + SGB_BOUND;
}

/* ---- names ------------------------------------------------------------------------------------- */
static const char *sgb_kind_name(uint8_t k)
{
    switch (k) {
    case 'D': return "display";      /* the default framebuffer: the composite of M, or the whole frame in EFB mode 0/2 */
    case 'M': return "M";            /* the main target the game renders into */
    case 'm': return "M-probe";      /* the break arm's extra scene on M: one colour-masked quad (prices a break of M) */
    case 'C': return "copy";         /* an EFB colour copy's texture */
    case 'Z': return "zcopy";        /* a Z copy's texture */
    case 'S': return "zsave";        /* S: M's colour saved around a Z resolve */
    case 'B': return "BREAK";        /* the break arm's scratch target */
    default:  return "untagged";
    }
}
static const char *sgb_why_name(uint8_t w)
{
    switch (w) {
    case 'c': return "colour-copy";  /* M was left for an EFB colour copy */
    case 'z': return "z-copy";       /* ... for a Z copy (its save, its resolve or the copy quad) */
    case 'r': return "back-to-M";    /* a pass ended: the FBO switch back to M */
    case 'p': return "present";      /* the composite onto the display, or the swap */
    case 'x': return "readback";     /* glReadPixels / glFinish asked for by the backend */
    case 'b': return "break-arm";
    default:  return "other";        /* a scene vitaGL ended that nothing tagged */
    }
}

/* ---- the report -------------------------------------------------------------------------------- */
typedef struct {
    int present_from, present_to;
    int clk[4];                   /* arm, bus, gpu, xbar MHz */
    int rw, rh;                   /* render size (0 = unknown) */
    int vsync_off, drain, mode;   /* mode 1 on, 2 on + raw dump, 3 in-boot A/B/A */
    int break_k, break_what;      /* break arm slot (0 = off) and what it inserts */
    uint32_t break_drained_us;    /* the boot self-test's figure */
    uint32_t probe_drained_us;
    uint32_t poll_us;             /* requested period */
    uint32_t gap_p50, gap_p90, gap_max, polls;   /* measured poll-to-poll gaps, us */
    uint32_t coalesced, dropped, lost, untimed, mismatch, pending_frames, dropped_frames, skipped_frames;
    uint64_t wrap_us; uint64_t wrap_scenes;      /* the instrument's own time on the game thread, in the wraps */
    uint64_t agg_us;                             /* ... and in the per-present aggregation */
    uint32_t clock_ns;                           /* one sceKernelGetProcessTimeWide, calibrated at init */
} SgbCtx;

typedef void (*SgbEmit)(const char *line);

static int sgb_row_order(const void *a, const void *b)
{
    const SgbType *x = *(const SgbType *const *) a, *y = *(const SgbType *const *) b;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    if (x->ls != y->ls) return x->ls > y->ls ? -1 : 1;
    return x->why < y->why ? -1 : x->why > y->why;
}

/* Prints the window and CLEARS it (the A/B/A groups included). */
static void sgb_report(SgbWin *w, const SgbCtx *c, SgbEmit emit)
{
    char b[640];
    const double F = w->frames ? (double) w->frames : 1.0;
    const uint32_t nf = w->frames < SGB_FRAMES ? w->frames : SGB_FRAMES;

    snprintf(b, sizeof b, "[scene] window presents %d..%d: %u frames aggregated, %llu scenes | mode %d%s%s | clocks arm %d bus %d gpu %d xbar %d | render %dx%d | vsync %s",
             c->present_from, c->present_to, (unsigned) w->frames, (unsigned long long) w->tot_scenes, c->mode,
             c->drain ? " DRAIN ARM (sceGxmFinish after every scene: frame times are NOT the game's)" : "",
             c->break_k ? " BREAK ARM" : "", c->clk[0], c->clk[1], c->clk[2], c->clk[3], c->rw, c->rh,
             c->vsync_off ? "OFF (vsync_off.txt)" : "on");
    emit(b);
    snprintf(b, sizeof b, "[scene] coverage: poll period %u us, measured gap p50 %u p90 %u max %u us over %u polls (a completion is stamped at the middle of its gap: +-gap/2) | %u completions shared a poll | scenes dropped (ring full) %u, lost (no notification in 2 s) %u, untimed (EndScene without a fragment notification) %u, tag/target mismatches %u, types past the table %u | frames not yet complete %u, dropped %u, skipped at an on/off edge %u",
             (unsigned) c->poll_us, (unsigned) c->gap_p50, (unsigned) c->gap_p90, (unsigned) c->gap_max, (unsigned) c->polls,
             (unsigned) c->coalesced, (unsigned) c->dropped, (unsigned) c->lost, (unsigned) c->untimed, (unsigned) c->mismatch,
             (unsigned) w->type_overflow, (unsigned) c->pending_frames, (unsigned) c->dropped_frames, (unsigned) c->skipped_frames);
    emit(b);

    if (w->frames) {
        const SgbType *rows[SGB_MAX_TYPES];
        for (int i = 0; i < w->nt; ++i) rows[i] = &w->t[i];
        qsort(rows, (size_t) w->nt, sizeof rows[0], sgb_row_order);
        emit("[scene] row  target   depth why          n/frame |  median     p10     p90     max ms/scene | ms/frame  share | CPU in BeginScene  EndScene (ms/frame)");
        uint64_t non_m = 0, m_us = 0; uint32_t m_n = 0, probe_med = 0, break_med = 0; int have_probe = 0, have_break = 0;
        for (int i = 0; i < w->nt; ++i) {
            SgbType *t = (SgbType *) rows[i];
            sgb_sort(t->samp, t->nsamp);
            uint32_t med = sgb_pct_sorted(t->samp, t->nsamp, 50);
            snprintf(b, sizeof b, "[scene] row  %-8s %c%c    %-12s %7.2f | %7.3f %7.3f %7.3f %7.3f          | %8.3f %5.1f%% | %8.3f %8.3f",
                     sgb_kind_name(t->kind), t->ls & SGB_F_LOAD ? 'L' : '-', t->ls & SGB_F_STORE ? 'S' : '-', sgb_why_name(t->why),
                     t->n / F, med / 1000.0, sgb_pct_sorted(t->samp, t->nsamp, 10) / 1000.0, sgb_pct_sorted(t->samp, t->nsamp, 90) / 1000.0,
                     t->max_us / 1000.0, t->gpu_us / F / 1000.0, w->tot_gpu ? 100.0 * (double) t->gpu_us / (double) w->tot_gpu : 0.0,
                     t->begin_us / F / 1000.0, t->end_us / F / 1000.0);
            emit(b);
            if (t->kind == 'M') { m_us += t->gpu_us; m_n += t->n; }
            else if (t->kind != 'B' && t->kind != 'm') non_m += t->gpu_us;   /* the break arm's own rows are not the game's */
            if (t->kind == 'm') { probe_med = med; have_probe = 1; }
            if (t->kind == 'B') { break_med = med; have_break = 1; }
        }
        sgb_sort(w->f_gpu, nf); sgb_sort(w->f_wall, nf); sgb_sort(w->f_cpu, nf); sgb_sort(w->f_swap, nf); sgb_sort(w->f_scenes, nf);
        const double gpu = w->tot_gpu / F / 1000.0, wall = w->tot_wall / F / 1000.0, cpu = w->tot_cpu / F / 1000.0, swap = w->tot_swap / F / 1000.0;
        snprintf(b, sizeof b, "[scene] frame: GPU sum median %.3f p10 %.3f p90 %.3f mean %.3f ms | scenes/frame median %u min %u max %u | wall median %.3f mean %.3f | frame CPU median %.3f mean %.3f | swap median %.3f mean %.3f | CPU inside BeginScene %.3f, EndScene %.3f ms/frame",
                 sgb_pct_sorted(w->f_gpu, nf, 50) / 1000.0, sgb_pct_sorted(w->f_gpu, nf, 10) / 1000.0, sgb_pct_sorted(w->f_gpu, nf, 90) / 1000.0, gpu,
                 (unsigned) sgb_pct_sorted(w->f_scenes, nf, 50), (unsigned) sgb_pct_sorted(w->f_scenes, nf, 0), (unsigned) sgb_pct_sorted(w->f_scenes, nf, 100),
                 sgb_pct_sorted(w->f_wall, nf, 50) / 1000.0, wall, sgb_pct_sorted(w->f_cpu, nf, 50) / 1000.0, cpu,
                 sgb_pct_sorted(w->f_swap, nf, 50) / 1000.0, swap, w->tot_begin / F / 1000.0, w->tot_end / F / 1000.0);
        emit(b);

        /* what the rows say about STRUCTURE (an estimate, labelled as one) */
        const double gpu_all = gpu;
        if (have_probe) {
            const double per = probe_med / 1000.0, breaks = m_n / F > 1.0 ? m_n / F - 1.0 : 0.0;
            const double arm = gpu_all - (double) (non_m + m_us) / F / 1000.0;   /* the BREAK and M-probe rows */
            const double game = gpu_all - arm, est = non_m / F / 1000.0 + breaks * per;
            (void) have_break;
            snprintf(b, sizeof b, "[scene] structure (ESTIMATE): rows not on M %.3f ms/frame | M is %.2f scenes/frame = %.2f breaks x the M-probe scene's median %.3f ms = %.3f | together %.3f ms = %.1f%% of the game's GPU sum %.3f (the frame's %.3f less the break arm's own rows %.3f)",
                     non_m / F / 1000.0, m_n / F, breaks, per, breaks * per, est, game > 0.0 ? 100.0 * est / game : 0.0, game, gpu_all, arm);
        } else {
            snprintf(b, sizeof b, "[scene] structure: rows not on M %.3f ms/frame = %.1f%% of the frame's GPU sum %.3f | M %.3f ms in %.2f scenes/frame -- what a BREAK of M costs (its load + store) is inside those rows and is priced only by the break arm (scene_gpu_break.txt \"K 2\")",
                     non_m / F / 1000.0, gpu > 0.0 ? 100.0 * (non_m / F / 1000.0) / gpu : 0.0, gpu, m_us / F / 1000.0, m_n / F);
        }
        emit(b);

        double r = 0.0;
        int v = sgb_sum_check(gpu, wall, cpu, swap, c->vsync_off, &r);
        const char *vs = v == SGB_SUM_VALID ? "SUM CHECK VALID"
                       : v == SGB_SUM_NA_CPU ? "SUM CHECK NOT APPLICABLE: CPU-bound (the swap is under 15% of the frame: nothing waits for the GPU)"
                       : v == SGB_SUM_NA_VSYNC ? "SUM CHECK NOT APPLICABLE: vsync on (the swap holds the vblank wait; use vsync_off.txt)"
                       : v == SGB_SUM_INVALID_HIGH ? "SUM CHECK INVALID: the GPU sum exceeds the wall time" : "SUM CHECK INVALID: the GPU sum is below the time the swap waited for the GPU";
        snprintf(b, sizeof b, "[scene] sum check (ms/frame, this window): GPU sum %.3f | wall %.3f | frame CPU %.3f | swap %.3f | wall - CPU %.3f || GPU / (wall - CPU) = %.3f, GPU / swap = %.3f, GPU busy = %.1f%% of wall | bracket [0.85 x swap, 1.15 x wall] = [%.3f, %.3f] -> %s (ratio %.3f)",
                 gpu, wall, cpu, swap, wall - cpu, r, swap > 0.0 ? gpu / swap : 0.0, wall > 0.0 ? 100.0 * gpu / wall : 0.0,
                 (1.0 - SGB_BOUND) * swap, (1.0 + SGB_BOUND) * wall, vs, r);
        emit(b);

        if (c->drain) {
            const double d = w->tot_drain / F / 1000.0;
            snprintf(b, sizeof b, "[scene] drain check (ms/frame): GPU sum from the notifications %.3f vs EndScene -> sceGxmFinish return %.3f, ratio %.3f -> %s",
                     gpu, d, d > 0.0 ? gpu / d : 0.0, sgb_within(gpu, d) ? "DRAIN CHECK VALID" : "DRAIN CHECK INVALID");
            emit(b);
            for (int i = 0; i < w->nt; ++i) {
                const SgbType *t = rows[i];
                if (!t->n) continue;
                snprintf(b, sizeof b, "[scene] drain row %-8s %c%c %-12s notification %.3f vs drained %.3f ms/scene (mean), ratio %.3f",
                         sgb_kind_name(t->kind), t->ls & SGB_F_LOAD ? 'L' : '-', t->ls & SGB_F_STORE ? 'S' : '-', sgb_why_name(t->why),
                         t->gpu_us / (double) t->n / 1000.0, t->drain_us / (double) t->n / 1000.0, t->drain_us ? (double) t->gpu_us / (double) t->drain_us : 0.0);
                emit(b);
            }
        }
        if (c->break_k) {
            if (have_break && c->break_drained_us) {
                snprintf(b, sizeof b, "[scene] break arm (slot %d, what %d): BREAK row median %.3f ms vs %.3f ms drained at boot, ratio %.3f -> %s | every other row must match the run without the arm",
                         c->break_k, c->break_what, break_med / 1000.0, c->break_drained_us / 1000.0, (double) break_med / (double) c->break_drained_us,
                         sgb_within((double) break_med, (double) c->break_drained_us) ? "BREAK CHECK VALID" : "BREAK CHECK INVALID");
            } else {
                snprintf(b, sizeof b, "[scene] break arm (slot %d, what %d): %s -> BREAK CHECK INVALID", c->break_k, c->break_what,
                         have_break ? "no drained cost from the boot self-test" : "NO BREAK row in this window (the slot was never reached: fewer copies a frame than K?)");
            }
            emit(b);
            if (have_probe) {
                snprintf(b, sizeof b, "[scene] break arm: M-probe scene (M loaded + stored, one colour-masked full-target quad) median %.3f ms in the frame vs %.3f ms drained at boot",
                         probe_med / 1000.0, c->probe_drained_us / 1000.0);
                emit(b);
            }
        }
    } else {
        emit("[scene] no complete frame in this window (nothing to print: see the coverage line)");
    }

    snprintf(b, sizeof b, "[scene] overhead on the game thread: %.2f us/scene in the wraps (%.3f ms/frame over %llu scenes) + %.3f ms/frame aggregating at the present | not in those figures: 6 clock reads a scene at %u ns each",
             c->wrap_scenes ? (double) c->wrap_us / (double) c->wrap_scenes : 0.0, c->wrap_us / F / 1000.0,
             (unsigned long long) c->wrap_scenes, c->agg_us / F / 1000.0, (unsigned) c->clock_ns);
    emit(b);

    if (c->mode == 3) {
        const uint32_t n_on = w->ab_n[1] + w->ab_n[3], n_off = w->ab_n[0] + w->ab_n[2];
        if (n_on && n_off && w->ab_n[0] && w->ab_n[1] && w->ab_n[2] && w->ab_n[3]) {
            double m[4][3];
            for (int p = 0; p < 4; ++p) { m[p][0] = w->ab_wall[p] / (double) w->ab_n[p] / 1000.0; m[p][1] = w->ab_cpu[p] / (double) w->ab_n[p] / 1000.0; m[p][2] = w->ab_swap[p] / (double) w->ab_n[p] / 1000.0; }
            const double on_w = (w->ab_wall[1] + w->ab_wall[3]) / (double) n_on / 1000.0, off_w = (w->ab_wall[0] + w->ab_wall[2]) / (double) n_off / 1000.0;
            const double on_c = (w->ab_cpu[1] + w->ab_cpu[3]) / (double) n_on / 1000.0, off_c = (w->ab_cpu[0] + w->ab_cpu[2]) / (double) n_off / 1000.0;
            double fl_off = m[0][0] - m[2][0], fl_on = m[1][0] - m[3][0];
            if (fl_off < 0) fl_off = -fl_off;
            if (fl_on < 0) fl_on = -fl_on;
            const double floor_ = fl_off > fl_on ? fl_off : fl_on;
            double d = on_w - off_w; const double ad = d < 0 ? -d : d;
            snprintf(b, sizeof b, "[scene] A/B/A in one boot (alternating 60-present phases): instrument ON %u frames wall %.3f CPU %.3f | OFF %u frames wall %.3f CPU %.3f | ON - OFF wall %+.3f CPU %+.3f ms | floor (the two OFF groups %.3f vs %.3f, the two ON groups %.3f vs %.3f) %.3f ms -> %s",
                     (unsigned) n_on, on_w, on_c, (unsigned) n_off, off_w, off_c, d, on_c - off_c, m[0][0], m[2][0], m[1][0], m[3][0], floor_,
                     ad <= floor_ ? "WITHIN THE FLOOR: the instrument does not move the frame" : "ABOVE THE FLOOR: the instrument moves the frame");
        } else {
            snprintf(b, sizeof b, "[scene] A/B/A in one boot: not all four phase groups have frames yet (%u %u %u %u)",
                     (unsigned) w->ab_n[0], (unsigned) w->ab_n[1], (unsigned) w->ab_n[2], (unsigned) w->ab_n[3]);
        }
        emit(b);
    }

    {   uint32_t rng = w->rng; uint64_t cg = w->cur_gpu; uint32_t cs = w->cur_scenes;
        w->nt = 0; w->type_overflow = 0; w->frames = 0;
        w->tot_gpu = w->tot_wall = w->tot_cpu = w->tot_swap = w->tot_scenes = w->tot_begin = w->tot_end = w->tot_drain = 0;
        memset(w->ab_n, 0, sizeof w->ab_n); memset(w->ab_wall, 0, sizeof w->ab_wall);
        memset(w->ab_cpu, 0, sizeof w->ab_cpu); memset(w->ab_swap, 0, sizeof w->ab_swap);
        w->rng = rng; w->cur_gpu = cg; w->cur_scenes = cs;
    }
}

#endif /* SCENE_GPU_BOOK_H */
