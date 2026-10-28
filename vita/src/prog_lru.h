/* prog_lru.h -- the bookkeeping of the GL-program budget (2026-10-11, docs/47 "Run 224"). No GL, no OS: the device
 * (rhi_vita_tev.inc) and the host test (host/tests/prog_lru_test.c) compile this same header.
 *
 * WHY. Run 224 died on arrival at Krazoa Palace with 1,008 GL programs alive (810 pair + 196 TEV + 2 EFB) and a burst
 * of new ones in flight. The installed vitaGL holds programs in a fixed `progs[1024]` and shaders in `shaders[2048]`
 * (custom_shaders.c:44-45); it is built NO_DEBUG (SKIP_ERROR_HANDLING), so on a full table glCreateProgram returns
 * 0xFFFFFFFF and glCreateShader returns 0, and every later call indexes `progs[handle - 1]` with no check: a wild
 * write, then a NULL read in glBindAttribLocation / glLinkProgram. Nothing in the port ever deleted a program.
 *
 * WHAT THIS KEEPS. Two things, for several tables at once (the TEV table and the pair table share vitaGL's handles):
 *   1. a BUDGET of live programs (`hard`) and of shader handles (`sec_cap`: a program linked from a binary owns two
 *      private shader objects, one linked from the shared shader objects owns none) that is never exceeded --
 *      plru_reserve makes room by evicting the least recently used program, or REFUSES (counted); and
 *   2. the table SLOTS: a full table hands out the slot of its least recently used entry again (plru_slot).
 * A victim must be at least `urgent_age` presents old, so nothing the open batch or the scene in flight names is
 * ever destroyed; plru_tick trims toward `soft` with the larger `min_age` (an area change frees what the last area
 * used). With `on` = 0 (the break arm) nothing is evicted or recycled: the budget only refuses. */
#ifndef PROG_LRU_H
#define PROG_LRU_H
#include <string.h>

#define PLRU_TABLES 2
typedef struct {
    int last_use;            /* the present of the last lookup that named this entry */
    unsigned char used;      /* the slot holds a key */
    unsigned char live;      /* it owns a GL program handle */
    unsigned char evicted;   /* its program was destroyed by the budget: the next build is a RELOAD */
    unsigned char sec;       /* shader handles it owns (0 or 2) */
} PlruEnt;

typedef struct Plru {
    PlruEnt *e[PLRU_TABLES]; int cap[PLRU_TABLES], n[PLRU_TABLES], tlive[PLRU_TABLES], ntab;
    int hard, soft, sec_cap, sec_soft, min_age, urgent_age, on;
    int live, sec_used, peak, sec_peak, next_scan;
    unsigned evictions, reloads, refused, recycled, table_full, commits;
    unsigned tick_commits, tick_deferred;   /* commits seen by the last tick; ticks that did nothing because a program had just been built */
    void (*destroy)(void *user, int t, int i);   /* delete the GL program of a LIVE entry and forget every index that names it */
    void *user;
} Plru;

static void plru_init(Plru *P, int hard, int soft, int sec_cap, int sec_soft, int min_age, int urgent_age, int on,
                      void (*destroy)(void *, int, int), void *user)
{
    memset(P, 0, sizeof *P);
    P->hard = hard; P->soft = soft; P->sec_cap = sec_cap; P->sec_soft = sec_soft;
    P->min_age = min_age; P->urgent_age = urgent_age < 1 ? 1 : urgent_age; P->on = on; P->destroy = destroy; P->user = user;
}
static int plru_add_table(Plru *P, PlruEnt *ents, int cap)
{
    if (P->ntab == PLRU_TABLES) return -1;
    memset(ents, 0, sizeof *ents * (size_t) cap);
    P->e[P->ntab] = ents; P->cap[P->ntab] = cap; P->n[P->ntab] = 0;
    return P->ntab++;
}
static inline void plru_touch(Plru *P, int t, int i, int now) { P->e[t][i].last_use = now; }
static inline int plru_evicted(const Plru *P, int t, int i) { return P->e[t][i].evicted; }

static void plru_evict(Plru *P, int t, int i)
{
    PlruEnt *e = &P->e[t][i];
    if (!e->live) return;
    P->destroy(P->user, t, i);
#ifndef PLRU_BREAK_1   /* BREAK 1: the handle is destroyed but still counted -- the budget leaks until everything is refused */
    --P->live; --P->tlive[t]; P->sec_used -= e->sec;
#endif
    e->live = 0; e->sec = 0; e->evicted = 1; ++P->evictions;
}
/* the least recently used LIVE entry at least `age` presents old (sec_only: one that owns shader handles) */
static int plru_victim(const Plru *P, int now, int age, int sec_only, int *pt)
{
    int bt = -1, bi = -1, bu = 0;
    for (int t = 0; t < P->ntab; ++t)
        for (int i = 0; i < P->n[t]; ++i) {
            const PlruEnt *e = &P->e[t][i];
            if (!e->live || (sec_only && !e->sec)) continue;
#ifndef PLRU_BREAK_2   /* BREAK 2: no age floor -- a program used THIS present (the open batch, the scene in flight) can be destroyed */
            if (now - e->last_use < age) continue;
#endif
#ifdef PLRU_BREAK_3    /* BREAK 3: the MOST recently used goes first */
            if (bi < 0 || e->last_use > bu) { bt = t; bi = i; bu = e->last_use; }
#else
            if (bi < 0 || e->last_use < bu) { bt = t; bi = i; bu = e->last_use; }
#endif
        }
    *pt = bt;
    return bi;
}
/* Before glCreateProgram: 1 = one more program (owning up to need_sec shader handles) fits, after evicting if it had to.
 * 0 = it does not: the caller must NOT create anything. sec_extra: shader handles alive outside the tables. need_prog 0
 * asks for shader handles only (a shared shader object about to be compiled). */
static int plru_reserve(Plru *P, int need_prog, int need_sec, int sec_extra, int now)
{
    for (;;) {
#ifdef PLRU_BREAK_4    /* BREAK 4: off by one -- the budget is exceeded by one handle */
        const int over_p = need_prog && P->live + need_prog > P->hard + 1;
#else
        const int over_p = need_prog && P->live + need_prog > P->hard;
#endif
        const int over_s = P->sec_used + need_sec + sec_extra > P->sec_cap;
        if (!over_p && !over_s) return 1;
        if (!P->on) { ++P->refused; return 0; }
        int t, i = plru_victim(P, now, P->urgent_age, !over_p, &t);
        if (i < 0) { ++P->refused; return 0; }
        plru_evict(P, t, i);
    }
}
/* the entry now owns a GL program (and `sec` shader handles) */
static void plru_commit(Plru *P, int t, int i, int sec, int now)
{
    PlruEnt *e = &P->e[t][i];
    if (e->live) return;
    if (e->evicted) ++P->reloads;
    e->live = 1; e->evicted = 0; e->sec = (unsigned char) sec; e->last_use = now;
    ++P->live; ++P->tlive[t]; P->sec_used += sec; ++P->commits;
    if (P->live > P->peak) P->peak = P->live;
    if (P->sec_used > P->sec_peak) P->sec_peak = P->sec_used;
}
/* A slot for a NEW key in table t, or -1 (table full: counted). *recycled = 1: the slot held another key -- the caller
 * rebuilds whatever index it keeps over the table. A dead entry (no program) goes before a live one. */
static int plru_slot(Plru *P, int t, int now, int *recycled)
{
    *recycled = 0;
    PlruEnt *E = P->e[t];
    int i = -1;
    if (P->n[t] < P->cap[t]) i = P->n[t]++;
    else if (P->on) {
        int bd = -1, bl = -1;
        for (int k = 0; k < P->n[t]; ++k) {
            if (now - E[k].last_use < P->urgent_age) continue;
            if (!E[k].live) { if (bd < 0 || E[k].last_use < E[bd].last_use) bd = k; }
            else if (bl < 0 || E[k].last_use < E[bl].last_use) bl = k;
        }
        i = bd >= 0 ? bd : bl;
        if (i >= 0) { plru_evict(P, t, i); *recycled = 1; ++P->recycled; }
    }
    if (i < 0) { ++P->table_full; return -1; }
#ifdef PLRU_BREAK_5    /* BREAK 5: a recycled slot keeps the old key's `evicted` mark -- a NEW key is counted as a reload */
    { const unsigned char ev = E[i].evicted; memset(&E[i], 0, sizeof E[i]); E[i].evicted = ev; }
#else
    memset(&E[i], 0, sizeof E[i]);
#endif
    E[i].used = 1; E[i].last_use = now;
    return i;
}
/* Once a present: trim toward the soft marks, at most `max` (<= 8) programs, each idle for min_age presents. One scan;
 * a scan that finds too little is not repeated for 30 presents. Returns the number evicted.
 *
 * Run 226 (trim from 672 of 896, 4 a present): 422 evictions at 5.5 ms each (glDeleteProgram waits for the GPU) and 26
 * reloads in one ordinary session -- the trim was itself a stutter, and it ran in the same presents that had just paid
 * for a program build. So: (1) a tick that follows a present in which a program was BUILT does nothing (that present
 * was long already; the trim can wait one more); (2) `pressure` is the caller's own reason to trim although the counts
 * are under their marks (the device: a shader-patcher area filling up) -- then one program that OWNS shader objects
 * goes (only those give patcher memory back), per tick. The device passes max = 1 and soft marks near the budget. */
static int plru_tick(Plru *P, int sec_extra, int now, int max, int pressure)
{
    if (!P->on) return 0;
    {   const unsigned built = P->commits - P->tick_commits;
        P->tick_commits = P->commits;
#ifndef PLRU_BREAK_6   /* BREAK 6: the trim also runs right after a present that built a program (run 226's behaviour) */
        if (built) { ++P->tick_deferred; return 0; }
#endif
        (void) built;
    }
#ifdef PLRU_BREAK_7    /* BREAK 7: the caller's pressure is ignored (a patcher area can fill while the counts look fine) */
    pressure = 0;
#endif
    const int want_p = P->live - P->soft, want_s = P->sec_used + sec_extra - P->sec_soft;
    if (want_p <= 0 && want_s <= 0 && !pressure) return 0;
    if (now < P->next_scan) return 0;
    if (max > 8) max = 8;
    int vt[8], vi[8], vu[8], nv = 0;   /* the `max` oldest, ascending by last use */
    for (int t = 0; t < P->ntab; ++t)
        for (int i = 0; i < P->n[t]; ++i) {
            const PlruEnt *e = &P->e[t][i];
            if (!e->live || now - e->last_use < P->min_age) continue;
            if (want_p <= 0 && !e->sec) continue;   /* only shader handles are short: a program that owns none frees none */
            int k = nv;
            if (nv == max) { if (e->last_use >= vu[nv - 1]) continue; k = nv - 1; } else ++nv;
            while (k > 0 && vu[k - 1] > e->last_use) { vt[k] = vt[k - 1]; vi[k] = vi[k - 1]; vu[k] = vu[k - 1]; --k; }
            vt[k] = t; vi[k] = i; vu[k] = e->last_use;
        }
    int done = 0;
    for (int k = 0; k < nv; ++k) {
        if (P->live <= P->soft && P->sec_used + sec_extra <= P->sec_soft && !(pressure && !done)) break;   /* under pressure alone: one */
        plru_evict(P, vt[k], vi[k]); ++done;
    }
    if (done < max && (P->live > P->soft || P->sec_used + sec_extra > P->sec_soft || (pressure && !done))) P->next_scan = now + 30;
    return done;
}
/* the counts, recounted from the entries: 0 = the running totals agree and no entry is live without being used */
static int plru_check(const Plru *P)
{
    int live = 0, sec = 0;
    for (int t = 0; t < P->ntab; ++t) {
        int tl = 0;
        if (P->n[t] < 0 || P->n[t] > P->cap[t]) return 1;
        for (int i = 0; i < P->n[t]; ++i) {
            const PlruEnt *e = &P->e[t][i];
            if (e->live && !e->used) return 2;
            if (e->live && e->evicted) return 3;
            if (!e->live && e->sec) return 4;
            if (e->live) { ++tl; sec += e->sec; }
        }
        if (tl != P->tlive[t]) return 5;
        live += tl;
    }
    if (live != P->live) return 6;
    if (sec != P->sec_used) return 7;
    return 0;
}
#endif
