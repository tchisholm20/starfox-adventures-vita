/* dlk_dyn.h -- the display-list cache's DYNAMIC-BUFFER set (docs/47, Task 1 of the 2026-10-12 perf pass).
 *
 * What it is for: gx_draw.c's validation-by-events trusts a cached record's arrays when they matched DLK_EV_TRUST calls in a
 * row, unless an event touched them. Frees go through a small ring; the morph / skinning WRITE-BACKS (memcpyToCache, ~100 a
 * frame) do not -- they hit the same buffers every frame, so a buffer that has been written is DYNAMIC and every record that
 * reads it re-hashes its arrays on every call. This is the set of those buffers.
 *
 * Run 237 (ThornTail Hollow): the old set was a 512-slot hash keyed by the exact write address. It never forgot anything, so
 * every model that streamed in and out left its write destinations behind (226 entries in Krazoa, 512 -- FULL -- after a
 * few thousand frees in ThornTail), and a full table made EVERY record dynamic. This one is
 *   - a sorted array of DISJOINT byte intervals (touching writes merge: a 0x1000-byte chunked write-back is one interval),
 *   - RETIRED ON FREE: gx_dlk_event(free) subtracts the freed block, so a dead model leaves nothing behind,
 *   - never "full": at capacity the two closest intervals are merged over their gap (a SUPERSET: still safe, a little
 *     imprecise, counted in `coarse`) instead of declaring everything dynamic.
 * Safety argument: a record's `dyn` flag is only ever stale-CONSERVATIVE except when coverage GROWS. Every growth (new
 * interval, extension, coarse merge) bumps `gen`, which makes every record re-evaluate itself at its next call. Shrinking
 * (free) does not bump: a record that is still dyn simply stays dyn until the next growth. The next write into a retired
 * range re-adds it (and bumps), and a write is declared by memcpyToCache AFTER the data lands and before any later call.
 *
 * Header-only and pointer-agnostic so the host test (host/tests/dlk_dyn_test.c) compiles the SAME code the device does. */
#ifndef DLK_DYN_H
#define DLK_DYN_H
#include <stddef.h>
#include <string.h>

#ifndef DLK_DYN_CAP
#define DLK_DYN_CAP 1024
#endif

typedef struct { const unsigned char *lo, *hi; } DlkIv;
typedef struct {
    DlkIv iv[DLK_DYN_CAP];
    unsigned n, gen, peak;
    unsigned coarse, retired, trimmed, splitSkipped, grows;
} DlkDyn;

static inline void dlkDynInit(DlkDyn* d) { memset(d, 0, sizeof *d); d->gen = 1; }

/* index of the first interval whose hi >= x (the first that touches or follows x) */
static inline unsigned dlkDynFirstHi(const DlkDyn* d, const unsigned char* x, int strict) {
    unsigned a = 0, b = d->n;
    while (a < b) { unsigned m = (a + b) >> 1; if (strict ? d->iv[m].hi <= x : d->iv[m].hi < x) a = m + 1; else b = m; }
    return a;
}

/* the two neighbours with the smallest gap between them become one interval: a superset, so every later answer stays safe */
static inline void dlkDynCoarsen(DlkDyn* d) {
    if (d->n < 2) return;
    unsigned best = 0; size_t bg = (size_t)(d->iv[1].lo - d->iv[0].hi);
    for (unsigned i = 1; i + 1 < d->n; ++i) { size_t g = (size_t)(d->iv[i + 1].lo - d->iv[i].hi); if (g < bg) { bg = g; best = i; } }
    d->iv[best].hi = d->iv[best + 1].hi;
    memmove(&d->iv[best + 1], &d->iv[best + 2], (size_t)(d->n - best - 2) * sizeof(DlkIv));
    --d->n; ++d->coarse;
}

/* a write landed in [lo,hi). Returns 1 when the covered set GREW (gen bumped: every record must re-evaluate its dyn flag). */
static inline int dlkDynWrite(DlkDyn* d, const unsigned char* lo, const unsigned char* hi) {
    if (hi <= lo) return 0;
    unsigned a = dlkDynFirstHi(d, lo, 0);
#ifndef DLK_DYN_BREAK_CONTAINED
    if (a < d->n && d->iv[a].lo <= lo && d->iv[a].hi >= hi) return 0;
#else   /* negative control: "contained" by the start address alone (an extension past the old end is lost) */
    if (a < d->n && d->iv[a].lo <= lo) return 0;
#endif
    unsigned j = a; const unsigned char *nlo = lo, *nhi = hi;
    while (j < d->n && d->iv[j].lo <= hi) { if (d->iv[j].lo < nlo) nlo = d->iv[j].lo; if (d->iv[j].hi > nhi) nhi = d->iv[j].hi; ++j; }
    if (j == a) {   /* a new, separate interval */
        if (d->n == DLK_DYN_CAP) { dlkDynCoarsen(d); return dlkDynWrite(d, lo, hi); }
        memmove(&d->iv[a + 1], &d->iv[a], (size_t)(d->n - a) * sizeof(DlkIv));
        d->iv[a].lo = nlo; d->iv[a].hi = nhi; ++d->n;
    } else {
        d->iv[a].lo = nlo; d->iv[a].hi = nhi;
        memmove(&d->iv[a + 1], &d->iv[j], (size_t)(d->n - j) * sizeof(DlkIv));
        d->n -= (j - a - 1);
    }
    if (d->n > d->peak) d->peak = d->n;
#ifndef DLK_DYN_BREAK_NOGEN
    ++d->gen;
#else   /* negative control: growth is silent (gen not bumped AND reported as no growth) so records keep a stale "not dynamic" */
    ++d->grows; return 0;
#endif
    ++d->grows;
    return 1;
}

/* a block [lo,hi) was freed: nothing in it is a write-back target any more. Subtracts it from the set. */
static inline void dlkDynFree(DlkDyn* d, const unsigned char* lo, const unsigned char* hi) {
    if (hi <= lo || !d->n) return;
    unsigned i = dlkDynFirstHi(d, lo, 1);   /* first interval with hi > lo */
    while (i < d->n && d->iv[i].lo < hi) {
        DlkIv* v = &d->iv[i];
        if (v->lo >= lo && v->hi <= hi) {   /* wholly inside the freed block */
            memmove(v, v + 1, (size_t)(d->n - i - 1) * sizeof(DlkIv)); --d->n; ++d->retired;
        } else if (v->lo < lo && v->hi > hi) {   /* the freed block is inside the interval: split */
            if (d->n < DLK_DYN_CAP) {
                memmove(v + 2, v + 1, (size_t)(d->n - i - 1) * sizeof(DlkIv));
                v[1].lo = hi; v[1].hi = v->hi; v->hi = lo; ++d->n; ++d->trimmed; i += 2;
            } else { ++d->splitSkipped; ++i; }   /* keep it whole: a superset, safe */
        } else if (v->lo < lo) {
#ifndef DLK_DYN_BREAK_TRIM
            v->hi = lo;
#else       /* negative control: a partial overlap drops the WHOLE interval, forgetting live bytes outside the freed block */
            v->hi = v->lo;
#endif
            ++d->trimmed; ++i;
        } else { v->lo = hi; ++d->trimmed; ++i; }
    }
}

/* does any dynamic interval overlap [b,t)? */
static inline int dlkDynOverlaps(const DlkDyn* d, const unsigned char* b, const unsigned char* t) {
    if (t <= b) return 0;
    unsigned i = dlkDynFirstHi(d, b, 1);
    return i < d->n && d->iv[i].lo < t;
}
#endif
