/* geom_stats.h - per-frame geometry fingerprint, shared VERBATIM by the Vita
 * backend (rhi_vita.c) and the host oracle (host/oracle_backend.c).
 *
 * Why: the first map comparison disagreed (IoU 0.514) and three explanations
 * in a row were wrong (oracle modulate, oracle near-clip, GL near-plane). A
 * picture cannot say whether the two sides were GIVEN different vertices or
 * DREW the same vertices differently. This can: identical counters over what
 * each backend receives, printed for the captured frame. Same numbers -> the
 * difference is rasterisation. Different -> it is upstream code behaving
 * differently on the two machines.
 *
 * Header-only and static so each backend owns its own copy of the state.
 */
#ifndef STAIRFAX_GEOM_STATS_H
#define STAIRFAX_GEOM_STATS_H

#include <math.h>
#include <stdint.h>

typedef struct GeomStats {
    unsigned tex_calls, col_calls;      /* draw calls this frame        */
    unsigned tex_tris, col_tris;        /* triangles this frame         */
    double   sum_xyz;                   /* sum of x+y+z over vertices   */
    double   sum_mvp;                   /* sum of MVP entries at draws  */
    float    max_abs;                   /* largest |coordinate|         */
    unsigned backend_draws;             /* glDrawArrays actually issued (Vita
                                         * only; the oracle leaves it 0)   */
    unsigned batches;                   /* runs of textured draws with the same
                                         * texture + MVP + alpha mode: the
                                         * draw-call count a state-batching
                                         * backend would need             */
} GeomStats;

static GeomStats gs_cur, gs_last;

/* Batch key of the previous textured draw this frame. */
static struct { const void *tex; float m[16]; int mode; int valid; } gs_key;

static inline void gs_batch(const void *tex, const float *m16, int mode)
{
    int same = gs_key.valid && gs_key.tex == tex && gs_key.mode == mode;
    for (int i = 0; same && i < 16; ++i) same = (gs_key.m[i] == m16[i]);
    if (!same) {
        gs_cur.batches++;
        gs_key.tex = tex; gs_key.mode = mode; gs_key.valid = 1;
        for (int i = 0; i < 16; ++i) gs_key.m[i] = m16[i];
    }
}

static inline void gs_vert(float x, float y, float z)
{
    gs_cur.sum_xyz += (double) x + (double) y + (double) z;
    float a = fabsf(x); if (fabsf(y) > a) a = fabsf(y); if (fabsf(z) > a) a = fabsf(z);
    if (a > gs_cur.max_abs) gs_cur.max_abs = a;
}
static inline void gs_mvp(const float *m16)
{
    for (int i = 0; i < 16; ++i) gs_cur.sum_mvp += m16[i];
}
/* Call once per present: the frame just drawn becomes gs_last. */
static inline void gs_frame_end(void) { gs_last = gs_cur; GeomStats z = {0}; gs_cur = z; gs_key.valid = 0; }

#endif
