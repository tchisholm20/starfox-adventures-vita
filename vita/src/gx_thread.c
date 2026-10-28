/* gx_thread.c -- the RING MODULE of the GX render thread (docs/53 rev 2, re-planned for the GXM backend: host/engine/RT_THREAD.md 6.1).
 *
 * WHAT THIS FILE IS.  G (the game thread) appends records to a single-producer / single-consumer ring (gx_rt.h: the inline fast paths are the
 * contract, this file is their slow paths); R ("stfx_gxr", pinned to its own core) pops them and replays them through gxt_r_exec (the
 * generated dispatch) and the HAL / backend.  Everything with GX semantics lives in gx_rt_special.c; the generated wrappers in gx_rec.c.  This file knows
 * NO GX type: it owns the ring, the cursors and their memory order, the R thread, the fences' plumbing (PRESENT, SYNC, mode switch,
 * exit), the retire list, the markers, the instruments, and the device ring-hammer.
 *
 * THE RING.  Modular uint32 byte counters (head [G], pub [G -> R], tail [R -> G]); the write offset is counter & mask; a record is a header word
 * (op | nwords << 10) and nwords payload words and NEVER straddles the ring end (a PAD record fills the gap).  Visibility:
 *     G:  payload stores ... STORE_REL(pub, head); fence; if (r_sleeping) signal            R:  LOAD_ACQ(pub) ... payload loads
 *     R:  ... STORE_REL(tail, r_tail);                                                      G:  LOAD_ACQ(tail) before it reuses the bytes
 * and the Dekker pair for sleep (each side: announce, FULL fence, re-check the other side): r_sleeping/pub for R, g_waiting/<predicate> for G.
 * A lost wake-up is not a hang on either side: R re-checks every GXT_R_TIMED_WAKE_US, G every 2 ms slice.
 *
 * ROOM.  g->room is the one number the inline fast path tests: min(distance to the ring end, free space, bytes left in this 4 KB publish quantum).
 * It is recomputed (recompute_room) after every publish and every slow path, so the fast path never needs to know about wrap, back-pressure or the quantum.
 *
 * RECORDS COUNTED.  The inline gxt_rec_alloc cannot count (it is on the 31k-calls-per-frame path), so G counts records at PUBLISH by walking the headers of the
 * bytes it publishes (a dependent-load chain over cache-hot memory; PAD records are not records).  R counts what it executes.  `recs == r_recs` at exit is
 * therefore a REAL closure test (a record lost, duplicated or mis-framed shows up), not a number counted twice from one source.  The walk's cost is a
 * measured host figure in the test log and an UNMEASURED device figure (RT_THREAD.md 9): see gxt_count_recs().
 *
 * DIRECT is the default and costs nothing: no marker => gxt_boot prints nothing, creates nothing, and gxt_frame_edge is one load and a return.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gx_rt.h"
#include "gx_thread_plat.h"
#include "stfx_backend.h"
#include "gx_rt_split.h"

#ifdef GXT_HOST
/* Host tests read what the module printed: every printf below goes through this (thread-safe, captured AND forwarded to stdout). */
int gxt_host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf gxt_host_printf
#endif

/* ------------------------------------------------------------------------------------------------------------------------------ device build breaks */
#if defined(GXT_BREAK_NOFENCE)       /* no release/acquire pairing: plain stores and loads (the device ring-hammer must FAIL with this; on x86 it cannot) */
#undef  GXT_STORE_REL
#undef  GXT_LOAD_ACQ
#define GXT_STORE_REL(p, v)   (*(p) = (v))
#define GXT_LOAD_ACQ(p)       (*(p))
#endif
#if defined(GXT_BREAK_NOSBFENCE)     /* no dmb between "announce I sleep" and "re-check the other side": the Dekker pair degrades to timed re-checks */
#undef  GXT_FENCE_SB
#define GXT_FENCE_SB()        __asm__ volatile("" ::: "memory")
#endif

/* ------------------------------------------------------------------------------------------------------------------------------ weak environment */
extern int  g_stfx_backend __attribute__((weak));
extern int  gx_tev_ind_enabled(void) __attribute__((weak));                  /* lazy marker read, also called by the GAME: pre-warmed on G before R exists */
extern int  gx_fog_private __attribute__((weak));                            /* N1 (RT_THREAD.md 6.2): HAL fog colour private to R while threaded */
extern unsigned char gx_fogColorR[4] __attribute__((weak));
extern unsigned char gFogColor[4] __attribute__((weak));
extern volatile int gGpuStallRetraceCount __attribute__((weak));
extern unsigned gStfxGxDlCalls __attribute__((weak));                        /* trace line counters (HAL, cumulative) */
extern unsigned gx_imm_prims __attribute__((weak));
extern unsigned gx_tex_loads __attribute__((weak));
extern unsigned gxm_issued_draws(void) __attribute__((weak));
extern _Bool __real_rhi_present(struct RhiInstance *rhi, struct RhiSwapchain *sc);
/* R5 (host/engine/R5_SPLIT.md): the render-thread split.  Weak FUNCTIONS (a link without gx_rt_split.o -- the ring test -- simply has no split; on the device an undefined weak function is NULL, on the host see the weak definitions below). */
extern int  gxsp_g_enable(int front_core, int back_core, int front_thread) __attribute__((weak));
extern void gxsp_g_disable(int rcore) __attribute__((weak));
extern void gxsp_f_publish(void) __attribute__((weak));
extern void gxsp_f_present(uint32_t seq, int arm, struct RhiInstance *rhi, struct RhiSwapchain *sc) __attribute__((weak));
extern int  gxsp_f_quit(void) __attribute__((weak));
extern void gxsp_set_present_done(GxspPresentDone f) __attribute__((weak));
extern void gxsp_print(uint32_t frames) __attribute__((weak));
extern void gxsp_stall_report(void) __attribute__((weak));
static int sp_have(void *f) { return f != NULL; }        /* (a helper: -Waddress rejects `&weak_fn` in a TU that also defines it, the host build below) */
#ifdef GXT_HOST
/* a MinGW link does NOT null an undefined weak function (it fails with an undefined .refptr reference): on the host the "no split here" answer is a WEAK DEFINITION that gx_rt_split.o's strong one replaces.  gxsp_g_enable refuses. */
int  gxsp_g_enable(int front_core, int back_core, int front_thread) { (void) front_core; (void) back_core; (void) front_thread; return 0; }
void gxsp_g_disable(int rcore) { (void) rcore; }
void gxsp_f_publish(void) { }
void gxsp_f_present(uint32_t seq, int arm, struct RhiInstance *rhi, struct RhiSwapchain *sc) { (void) seq; (void) arm; (void) rhi; (void) sc; }
int  gxsp_f_quit(void) { return 0; }
void gxsp_set_present_done(GxspPresentDone f) { (void) f; }
void gxsp_print(uint32_t frames) { (void) frames; }
void gxsp_stall_report(void) { }
#endif

/* ------------------------------------------------------------------------------------------------------------------------------ timing (constants on the device, test hooks on the host) */
#ifdef GXT_HOST
uint32_t gxt_t_wait_us = GXT_WAIT_US, gxt_t_giveup_us = GXT_GIVEUP_US, gxt_t_slice_us = 2000u, gxt_t_join_us = 2000000u, gxt_t_spin_us = GXT_R_SPIN_US;
uint32_t gxt_t_rstall_ms = 8000u, gxt_t_dropsig_n = 1000u, gxt_t_create_us = 2000000u;
void (*gxt_host_free_hook)(void *);
uint32_t gxt_host_ham_corrupt = 0xFFFFFFFFu;
#define T_WAIT      gxt_t_wait_us
#define T_GIVEUP    gxt_t_giveup_us
#define T_SLICE     gxt_t_slice_us
#define T_JOIN      gxt_t_join_us
#define T_SPIN      gxt_t_spin_us
#define T_RSTALL_MS gxt_t_rstall_ms
#define T_DROPSIG_N gxt_t_dropsig_n
#define T_CREATE    gxt_t_create_us
#else
#define T_WAIT      GXT_WAIT_US
#define T_GIVEUP    GXT_GIVEUP_US
#define T_SLICE     2000u
#define T_JOIN      2000000u
#define T_SPIN      GXT_R_SPIN_US
#define T_RSTALL_MS 8000u
static uint32_t gxt_t_dropsig_n = 1000u;   /* device: 1 wake signal in 1000 is dropped by the dropsig break arm; the marker word `dropsig=N` makes it 1 in N (the Vita3K run is 180 frames = ~60 sleeps) */
#define T_DROPSIG_N gxt_t_dropsig_n
#define T_CREATE    2000000u
#endif

/* ------------------------------------------------------------------------------------------------------------------------------ definitions */
GxtShared g_gxt;
volatile int gxt_r_unsafe;
/* R2 (gx_rt.h "R2 STATE RECORDS"): defined HERE so every translation unit that inlines gxt_rec_alloc links without the state module (the ring test links this file alone) */
int gxt_state_on;
unsigned gxt_state_calls;
const uint32_t gxt_state_zero = 0u;
const uint32_t *gxt_state_dirty = &gxt_state_zero;

#define PW  ((uint32_t) (sizeof(void *) / 4u))
#define STACK_PATTERN 0xA5u
#define RING_FILL     0xA5u
#define WAIT_REACHED(cur, target) ((int32_t) ((cur) - (target)) >= 0)

typedef struct { void *p; uint32_t stamp; } RetEnt;

enum { W_PRESENT, W_RING, W_BLOCK };

/* G-private and R-private file state, in one place so the host test can reset it. */
static struct GxtPriv {
    int       booted, idle, silent, quit_done, hammer_ran, sema_ok, r_created, noisy_quit;
    uint8_t  *ring_raw;
    RetEnt   *ret;
    uint32_t  ret_head, ret_n;
    int       arm;
    uint32_t  edges, win_from, mode_mask, occ_hw;
    int       flipstall;
    GxtStats  snap;
    uint32_t  sig_n, pads;
    int       rcore_eff, gcore_eff;
    int       fog_on;
    char      refuse[200];
    int       count_recs;
    int       filt_off;           /* gx_thread_filt.txt = 0 (the zero default of this struct = ON) */
    int       state_off;          /* gx_thread_state.txt = 0 (the zero default of this struct = R2 ON when the module is linked) */
    int       saudit;             /* gx_thread_saudit.txt N: the R2 whole-struct audit every Nth flush (0 = off) */
    volatile int split_on;        /* R5: the render thread is split (G writes at a frame edge with the GX ring drained; R reads).  1 = R is the FRONT, the RHI calls are recorded for R-back */
    int       split_m1;           /* R5: gx_thread_split.txt is 1 (the process is split-capable: handles are proxies).  Absent or 0 = never split (the default until the hardware PASS) */
    int       skin_m;             /* R8: gx_skin.txt = 1 (GPU skinning ON in the fixed modes; in arm mode the arm decides: 149) */
    int       vb_off_m;           /* R-VB DEFAULT FLIP (docs/54 hw_VB, -1.73 ms): gx_vb_bulk.txt = 0 -> the per-byte replay in the fixed modes; zero = the default ON */
    int       vb_seen, vb_last;   /* gxt_vb_bulk_wanted printed once / its last answer */
    int       skin_off_m;         /* R8 DEFAULT FLIP (docs/54 hw_R8tour3 PASS + Krazoa look check, 2026-10-26): gx_skin.txt = 0 -> OFF in the fixed modes; zero-initialised = the default ON */
    int       skin_seen, skin_last;   /* R8: gxt_skin_wanted has printed once, and the state it last printed */
    int       split_refused, split_warned, fcore_cfg;   /* R5: enable refused (stay OFF), the arm-148-without-marker warning printed, the front core (gx_thread_fcore.txt; default 0) */
    /* R-private */
    uint32_t  r_tail;
    volatile uint32_t r_last_op;
    uint32_t  r_presents;
    int       r_stalled;
    volatile int r_cpu_last;
    volatile uintptr_t r_tp;
    uint32_t  r_corrupt, r_corrupt_logged, r_unknown_logged;
    int       r_prefilled;
    volatile int r_in_park;       /* R: 1 while R sits in the parked wait (tail published, batch loop left): the only time G may touch s.r_tail / reset the ring */
    uint32_t  ham_expect, ham_bad, ham_recs, ham_logged;
    uint32_t  r_pres_acc;                  /* R: microseconds of the current batch spent inside the real present (not busy time) */
} s;

/* the effective filter state for the boot line: the marker is not 0 AND the module is linked */
static int filt_effective(void) { return !s.filt_off && (&gxt_flt_arm != NULL); }
/* R2 (host/engine/R2_STATE.md): the state-record path replaces the G-side filter.  Effective = the marker is not 0 AND the module is linked.  Wanted for an arm: threaded and effective and the perf arm
 * is not a LEGACY arm (111 = filter OFF, 130 / 131 = filter diagnostics, 132 = the R2 A/B control: threaded, R2 off, filter v3 armed = the pre-R2 arm 6).  While R2 is active the filter is disarmed. */
static int state_effective(void) { return !s.state_off && (&gxs_begin != NULL) && (&gxs_end != NULL) && (&gxs_flush != NULL) && (&gxs_r_apply != NULL); }
#ifdef GXT_HOST
int gxt_host_state_off;                /* host tests: 1 = force the legacy path (R2 off) whatever the module says */
int gxt_host_saudit;                   /* host tests: the whole-struct audit period handed to the state module at every threaded entry (the device reads gx_thread_saudit.txt) */
#endif
static int state_wanted(int arm)
{
    if (!state_effective()) return 0;
#ifdef GXT_HOST
    if (gxt_host_state_off) return 0;
#endif
    return !(g_gxt.cfg.mode == 'a' && (arm == 111 || arm == 130 || arm == 131 || arm == 132));
}
static int filt_arm_wanted(int arm) { return g_gxt.on && filt_effective() && !(g_gxt.cfg.mode == 'a' && arm == 111) && !state_wanted(arm); }
/* R2 transitions.  legacy -> R2 at a frame edge: R may still hold legacy setter records, so DRAIN first (gx_hal must be G's program-order state when it is copied into gx_hal_g); from DIRECT
 * (enter_threaded) R is parked with an empty ring.  R2 -> legacy: gxs_end flushes the dirty groups (R applies them before any later record), then setters record again. */
static void state_apply(int want)
{
    if (want == gxt_state_on) return;
    if (want) {
        if (g_gxt.on) (void) gxt_drain("state: legacy -> R2");
        gxs_begin();
    } else gxs_end();
}
/* Filter-cost DIAGNOSTIC arms (2026-10-21, docs/54 "Filter v2"): 130 = the filter compares and remembers but never drops; 131 = the wrapper builds the payload and calls in,
 * the filter returns at entry.  Both run threaded with the filter armed.  Weak: defined in gx_rt_filter.c. */
void gxt_flt_diag(int mode) __attribute__((weak));
static int filt_diag_wanted(int arm) { return g_gxt.cfg.mode == 'a' && arm == 130 ? 1 : g_gxt.cfg.mode == 'a' && arm == 131 ? 2 : 0; }

/* ------------------------------------------------------------------------------------------------------------------------------ small helpers */
static void fatal(const char *what, uint32_t a, uint32_t b)
{
    printf("[gxt] FATAL: %s (%u, %u)\n", what, a, b);
    for (;;) gxtp_delay_us(100000u);        /* the game watchdog faults; a silent corrupt ring is worse than a loud dead one */
}

static void do_free(void *p)
{
#ifdef GXT_HOST
    if (gxt_host_free_hook) { gxt_host_free_hook(p); return; }
#endif
    free(p);
}

uint32_t gxt_now_us(void) { return gxtp_now_us(); }

static inline int has_free(const GxtShared *g, uint32_t n)
{
    return (uint32_t) (g->size - (g->head - g->tail_cache)) >= n;
}

static inline void recompute_room(GxtShared *g)
{
    const uint32_t to_end = g->size - (g->head & g->mask);
    const uint32_t fr     = g->size - (g->head - g->tail_cache);
    const uint32_t un     = g->head - g->pub_g;
    const uint32_t q      = un >= GXT_PUBLISH_BYTES ? 0u : GXT_PUBLISH_BYTES - un;
    uint32_t r = to_end < fr ? to_end : fr;
    if (q < r) r = q;
    g->room = (int32_t) r;
}

/* G walks the headers it is about to publish.  Cache-hot (<= one quantum), a dependent chain: ~2 loads and 4 ALU ops per record. */
static void gxt_count_recs(GxtShared *g, uint32_t from, uint32_t to)
{
    uint32_t n = 0, off = from;
    const uint32_t span = to - from;
    if (!s.count_recs) return;
    while (off != to) {
        const uint32_t h = *(const uint32_t *) (g->buf + (off & g->mask));
        n += (h & 1023u) != GXT_OP_PAD;
        off += ((h >> 10) + 1u) << 2;
        if ((uint32_t) (to - off) > span) break;           /* walked past the end: a ring bug, not data; R will say so */
    }
    g->st.recs += n;
}

/* ------------------------------------------------------------------------------------------------------------------------------ publish */
static void publish_now(void)
{
    GxtShared *g = &g_gxt;
    if (g->head == g->pub_g) return;
    gxt_count_recs(g, g->pub_g, g->head);
    g->st.rec_bytes += g->head - g->pub_g;
    g->pub_g = g->head;
    GXT_STORE_REL(&g->pub, g->head);
    GXT_FENCE_SB();
    ++g->st.publishes;
    if (g->r_sleeping) {
        if ((g->cfg.brk & GXT_BRK_DROPSIG) && ++s.sig_n % T_DROPSIG_N == 0) { /* break arm: this wake signal is lost; R's timed wait must recover */ }
        else { ++g->st.wakes; gxtp_sema_signal(g->sema_r); }
    }
    recompute_room(g);
}

void gxt_publish(void)
{
    GxtShared *g = &g_gxt;
    if (g->chunk_data) gxt_chunk_close();
    publish_now();
}

uint32_t gxt_mark(void) { gxt_publish(); return g_gxt.head; }

/* ------------------------------------------------------------------------------------------------------------------------------ G waits */
static int wait_ge(volatile uint32_t *p, uint32_t target, const char *why, int cls, int forever)
{
    GxtShared *g = &g_gxt;
    uint32_t t0, tail0, next_log, el;
    int stuck = 0, rc = 0;
    if (WAIT_REACHED(GXT_LOAD_ACQ(p), target)) return 0;
    publish_now();                                   /* R must see everything recorded; callers that may have a chunk open close it first (gxt_publish) */
    t0 = gxtp_now_us(); tail0 = GXT_LOAD_ACQ(&g->tail); next_log = T_WAIT;
    if (cls == W_PRESENT) ++g->st.present_waits; else if (cls == W_RING) ++g->st.ring_full_waits; else ++g->st.block_waits;
    for (;;) {
        g->g_waiting = 1;
        GXT_FENCE_SB();
        if (WAIT_REACHED(GXT_LOAD_ACQ(p), target)) { g->g_waiting = 0; break; }
        (void) gxtp_sema_wait(g->sema_g, T_SLICE);
        g->g_waiting = 0;
        if (WAIT_REACHED(GXT_LOAD_ACQ(p), target)) break;
        el = gxtp_now_us() - t0;
        if (el >= T_GIVEUP) {
            if (!stuck) {
                stuck = 1; ++g->st.stuck;
                printf("[gxt] G waits: %s since %u ms, R tail +%u bytes, R last op %u (STUCK)\n", why, el / 1000u, GXT_LOAD_ACQ(&g->tail) - tail0, s.r_last_op);
            }
            if (!forever) { rc = -1; break; }
        } else if (el >= next_log) {
            ++g->st.timeouts;
            printf("[gxt] G waits: %s since %u ms, R tail +%u bytes, R last op %u (SLOW)\n", why, el / 1000u, GXT_LOAD_ACQ(&g->tail) - tail0, s.r_last_op);
            next_log += T_WAIT;
        }
    }
    el = gxtp_now_us() - t0;
    if (cls == W_PRESENT) g->st.present_wait_us += el; else if (cls == W_RING) g->st.ring_full_us += el; else g->st.block_us += el;
    return rc;
}

int gxt_wait_u32(volatile uint32_t *p, uint32_t target, const char *why) { gxt_publish(); return wait_ge(p, target, why, W_BLOCK, 0); }
int gxt_wait_tail(uint32_t mark, const char *why)                        { return gxt_wait_u32(&g_gxt.tail, mark, why); }
int gxt_drain(const char *why)                                           { return gxt_wait_tail(gxt_mark(), why); }

void gxt_r_wake_g(void)
{
    GXT_FENCE_SB();
    if (g_gxt.g_waiting) gxtp_sema_signal(g_gxt.sema_g);
}

/* ------------------------------------------------------------------------------------------------------------------------------ producer slow paths */
static void ensure_free(GxtShared *g, uint32_t n)
{
    for (;;) {
        if (has_free(g, n)) return;
        g->tail_cache = GXT_LOAD_ACQ(&g->tail);
        if (has_free(g, n)) return;
        wait_ge(&g->tail, g->head + n - g->size, "ring full", W_RING, 1);
    }
}

void gxt_alloc_slow(uint32_t bytes)
{
    GxtShared *g = &g_gxt;
    uint32_t off, to_end;
    if (g->chunk_data) gxt_chunk_close();                /* defensive: gxt_rec_alloc already closed it */
    if (!g->buf || bytes > g->size) fatal("record larger than the ring, or no ring", bytes, g->size);
    off = g->head & g->mask; to_end = g->size - off;
    if (bytes > to_end) {                                /* would wrap: fill to the end with a PAD record the consumer skips */
        ensure_free(g, to_end);
        *(uint32_t *) (g->buf + off) = GXT_OP_PAD | ((to_end / 4u - 1u) << 10);
        g->head += to_end; ++s.pads;
    }
    ensure_free(g, bytes);
    if (g->head - g->pub_g >= GXT_PUBLISH_BYTES) publish_now();        /* the quantum is used */
    recompute_room(g);
    if (g->room < (int32_t) bytes) {                     /* the record does not fit in what is left of the quantum: publish, a fresh quantum */
        publish_now(); recompute_room(g);
    }
    if (g->room < (int32_t) bytes) {                     /* bigger than a quantum (or a tiny ring): the quantum yields, space and wrap do not */
        const uint32_t te = g->size - (g->head & g->mask), fr = g->size - (g->head - g->tail_cache);
        g->room = (int32_t) (te < fr ? te : fr);
    }
}

/* Write the open chunk's header (length word first) and advance head/room.  No publish: callers decide. */
static void chunk_finish(void)
{
    GxtShared *g = &g_gxt;
    uint8_t *data = g->chunk_data;
    uint32_t len, w, bytes, i;
    uint32_t *h;
    if (!data) return;
    len = (uint32_t) (g->push_cur - data);
    g->chunk_data = NULL; g->push_cur = g->push_end = NULL;
    if (len == 0) return;                                 /* an empty chunk writes nothing */
    w = (len + 3u) >> 2; h = (uint32_t *) (data - 8);
    for (i = len; i < (w << 2); ++i) data[i] = 0;
    h[1] = len;
    h[0] = GXT_OP_VBYTES | ((1u + w) << 10);
    bytes = 8u + (w << 2);
    g->head += bytes; g->room -= (int32_t) bytes;
    ++g->st.chunks;
}

void gxt_chunk_close(void)
{
    GxtShared *g = &g_gxt;
    if (!g->chunk_data) return;
    chunk_finish();
    if (g->head - g->pub_g >= GXT_PUBLISH_BYTES) publish_now();     /* streaming: R must not wait for the end of the frame to see a quantum of vertices */
}

uint8_t *gxt_push_slow(uint32_t n)
{
    GxtShared *g = &g_gxt;
    if (g->list_open) {                                   /* LIST mode: the game's buffer is exhausted -> the sink, flagged */
        uint8_t *cur = g->push_cur;
        if (!(cur >= g->sink && cur <= g->sink + GXT_PIPE_SINK_BYTES)) { cur = g->sink; g->push_end = g->sink + GXT_PIPE_SINK_BYTES; }
        g->list_overflow = 1;
        if ((uint32_t) (g->push_end - cur) < n) cur = g->sink;
        g->push_cur = cur;
        return cur;
    }
    if (g->chunk_data) {
        if ((uint32_t) (g->push_end - g->push_cur) >= n) return g->push_cur;   /* defensive */
        gxt_chunk_close();                                /* roll: the full chunk goes out, a new one opens below */
    }
    if (GXT_UNLIKELY(*gxt_state_dirty != 0u)) gxs_flush();   /* INV-1: the dirty groups go out BEFORE the bytes that open a chunk (a setter closed the previous one: INV-2) */
    if (g->room < (int32_t) (8u + GXT_VB_CHUNK)) gxt_alloc_slow(8u + GXT_VB_CHUNK);
    g->chunk_data = g->buf + (g->head & g->mask) + 8u;
    g->push_cur = g->chunk_data;
    g->push_end = g->chunk_data + GXT_VB_CHUNK;
    return g->chunk_data;
}

/* ------------------------------------------------------------------------------------------------------------------------------ small public helpers */
int gxt_is_active(void) { return g_gxt.on; }

int gxt_on_R(void)
{
    const uintptr_t lo = g_gxt.r_stack_lo, hi = g_gxt.r_stack_hi;
    return g_gxt.r_alive && (uintptr_t) __builtin_frame_address(0) - lo < hi - lo;
}

void gxt_viewer_trap(const char *name)
{
    if (g_gxt.st.viewer_traps++ < 8) printf("[gxt] VIEWER TRAP: %s reached in threaded mode (#%u)\n", name ? name : "?", g_gxt.st.viewer_traps);
}

/* ------------------------------------------------------------------------------------------------------------------------------ the retire list */
static void retire_free_ready(void)
{
    GxtShared *g = &g_gxt;
    while (s.ret_n && WAIT_REACHED(GXT_LOAD_ACQ(&g->tail), s.ret[s.ret_head].stamp)) {
        do_free(s.ret[s.ret_head].p);
        s.ret_head = (s.ret_head + 1u) % GXT_RETIRE_MAX; --s.ret_n;
    }
    g->st.retire_n = s.ret_n;
}

void gxt_free(void *p)
{
    GxtShared *g = &g_gxt;
    RetEnt *e;
    if (!g->on || !s.ret) { do_free(p); return; }
    if (s.ret_n >= GXT_RETIRE_MAX) {                      /* allocation pressure: publish, wait for R, free what it has passed */
        ++g->st.retire_alloc_waits;
        retire_free_ready();
        while (s.ret_n >= GXT_RETIRE_MAX) {
            gxt_publish();
            wait_ge(&g->tail, s.ret[s.ret_head].stamp, "retire list full", W_BLOCK, 1);
            retire_free_ready();
        }
    }
    e = &s.ret[(s.ret_head + s.ret_n) % GXT_RETIRE_MAX];
    e->p = p; e->stamp = g->head; ++s.ret_n;
    g->st.retire_n = s.ret_n;
    if (s.ret_n > g->st.retire_max) g->st.retire_max = s.ret_n;
}

/* An allocation failed on G (mm_alloc_from, the snapshot / shadow buffers): the retire list may be holding the very blocks that would have satisfied it (frees are deferred to the frame edge).
 * Publish, wait for R to execute everything recorded so far, free what it has passed.  Bounded like every G wait.  Not callable from R (the list is G's): returns 0 there. */
int gxt_reclaim(const char *why)
{
    GxtShared *g = &g_gxt;
    uint32_t n0;
    if (!g->on || !s.ret || !s.ret_n || gxt_on_R()) return 0;
    n0 = s.ret_n;
    ++g->st.reclaims;
    (void) gxt_drain(why ? why : "reclaim");
    retire_free_ready();
    g->st.reclaim_freed += n0 - s.ret_n;
    return (int) (n0 - s.ret_n);
}

static void retire_free_all(void)
{
    retire_free_ready();
    while (s.ret_n) {                                     /* only reachable if tail has not passed a stamp: wait for it (a transition drains first, so this is a belt) */
        wait_ge(&g_gxt.tail, s.ret[s.ret_head].stamp, "retire drain", W_BLOCK, 1);
        retire_free_ready();
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------ present */
static void trace_line(uint32_t seq, int thr)
{
    const long draws = gxm_issued_draws ? (long) gxm_issued_draws() : -1L;
    printf("[gxt] tr seq=%u dl=%u imm=%u tex=%u draws=%ld thr=%d\n", seq,
           &gStfxGxDlCalls ? gStfxGxDlCalls : 0u, &gx_imm_prims ? gx_imm_prims : 0u, &gx_tex_loads ? gx_tex_loads : 0u, draws, thr);
}

int gxt_present(struct RhiInstance *rhi, struct RhiSwapchain *sc)
{
    GxtShared *g = &g_gxt;
    uint32_t seq, *p;
    int32_t behind;
    if (GXT_LIKELY(!g->on)) {                             /* DIRECT: the real present, counted so the frame numbers of a direct run match a threaded one */
        int r;
        seq = ++g->seq;
        r = __real_rhi_present(rhi, sc);
        g->r_presented = seq;                             /* no R is running a present: R is parked or absent */
        if (GXT_UNLIKELY(g->cfg.trace)) trace_line(seq, 0);
        return r;
    }
    seq = ++g->seq;
    p = gxt_rec_alloc(GXT_OP_PRESENT, 2u + 2u * PW);
    p[0] = seq; p[1] = (uint32_t) s.arm;
    memcpy(p + 2, &rhi, sizeof rhi);
    memcpy(p + 2 + PW, &sc, sizeof sc);
    behind = (int32_t) (seq - 1u - GXT_LOAD_ACQ(&g->r_presented));
    ++g->st.ahead_hist[behind <= 0 ? 0 : behind == 1 ? 1 : 2];
    {   const uint32_t occ = g->head - GXT_LOAD_ACQ(&g->tail); if (occ > s.occ_hw) s.occ_hw = occ; }
    gxt_publish();
    wait_ge(&g->r_presented, seq - (uint32_t) g->ahead, "present", W_PRESENT, 1);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------------------ R thread */
static void ham_check(const uint32_t *p, uint32_t nw)
{
    uint32_t c = 2166136261u ^ p[0], i;
    ++s.ham_recs;
    for (i = 2; i < nw; ++i) c = (c ^ p[i]) * 16777619u;
    if (nw < 2 || p[0] != s.ham_expect || c != p[1]) {
        ++s.ham_bad;
        if (s.ham_logged++ < 8) printf("[gxt] hammer BAD record #%u: seq %u (expected %u) nwords %u sum %08x (got %08x)\n", s.ham_recs, nw ? p[0] : 0u, s.ham_expect, nw, nw > 1 ? p[1] : 0u, c);
    }
    s.ham_expect = p[0] + 1u;                             /* resynchronise on what the record claims: one bad record is one bad record, not a cascade */
    if ((s.ham_recs & 1023u) == 0) s.r_cpu_last = gxtp_cpu_id();
}

/* What follows the real present: on R when the render thread is whole, on R-back when it is split (R5).  Same statements, same order.  `on_back`: R-back must not publish the tail (r_cur_end is the
 * FRONT's cursor; the front published it when it forwarded the record), so it only wakes G; the time inside the present is the back's, not R's batch. */
static void r_present_post(uint32_t seq, int arm, uint32_t present_us, int on_back)
{
    GxtShared *g = &g_gxt;
    g->st.r_present_us += present_us;
    if (!on_back) s.r_pres_acc += present_us;
    if (g->cfg.trace) trace_line(seq, 1);
    gxts_r_present_executed(seq);
    if (arm == 98) { const uint32_t t = gxtp_now_us(); while ((uint32_t) (gxtp_now_us() - t) < 12000u) gxtp_relax(); }     /* arm 98: the 12 ms positive control on R */
    if ((g->cfg.brk & GXT_BRK_RDELAY) && g->cfg.rdelay_ms) gxtp_delay_us(g->cfg.rdelay_ms * 1000u);                         /* lag injection */
    ++s.r_presents;
    if ((g->cfg.brk & GXT_BRK_RSTALL) && !s.r_stalled && s.r_presents > 300u) {                                            /* G-X1 exit gate: R wedges inside one record */
        s.r_stalled = 1;
        printf("[gxt] R: rstall -- sleeping %u ms inside the PRESENT record\n", T_RSTALL_MS);
        gxtp_delay_us(T_RSTALL_MS * 1000u);
    }
    { const int c = gxtp_cpu_id(); ++g->st.r_cpu[c & 3]; s.r_cpu_last = c; }
    GXT_STORE_REL(&g->r_presented, seq);
    if (on_back) gxt_r_wake_g(); else gxt_r_mark_done();
}
static void r_present_done_back(uint32_t seq, int arm, uint32_t present_us) { r_present_post(seq, arm, present_us, 1); }   /* R5: GxspPresentDone, runs on R-back */

static void r_present(const uint32_t *p, uint32_t nw)
{
    GxtShared *g = &g_gxt;
    struct RhiInstance *rhi; struct RhiSwapchain *sc;
    uint32_t seq, t0, t1;
    int arm;
    if (nw < 2u + 2u * PW) { ++g->st.r_ops_unknown; return; }
    seq = p[0]; arm = (int) p[1];
    memcpy(&rhi, p + 2, sizeof rhi); memcpy(&sc, p + 2 + PW, sizeof sc);
    if (s.split_on) {                                     /* R5: forward it; R-back runs the real present and r_presented is published THERE (G's view of "presented" is unchanged) */
        gxsp_f_present(seq, arm, rhi, sc);
        gxt_r_mark_done();                                /* the tail passes the record when the FRONT has consumed it (nothing G drains on needs the picture) */
        return;
    }
    t0 = gxtp_now_us();
    (void) __real_rhi_present(rhi, sc);
    t1 = gxtp_now_us();
    r_present_post(seq, arm, t1 - t0, 0);
}

static void r_stack_prefill(void)
{
    GxtShared *g = &g_gxt;
    uintptr_t lo, hi, sp = (uintptr_t) __builtin_frame_address(0);
    gxtp_self_stack(&lo, &hi, GXT_R_STACK_BYTES);
    if (lo && sp > lo + 1024u && sp < hi) {                       /* the thread-info range contains our own sp: trust it, pre-fill below sp for the high-water mark */
        memset((void *) lo, STACK_PATTERN, (size_t) (sp - 1024u - lo));
        s.r_prefilled = 1;
    } else {                                                      /* thread info failed or disagrees with sp: gxt_on_R must still work (E-wrappers key on it), so use an sp-based range and measure nothing */
        hi = sp + 4096u; lo = sp + 4096u - (GXT_R_STACK_BYTES - 16384u);
        s.r_prefilled = 0;
        printf("[gxt] R: the thread-info stack range does not contain sp; using an sp-based range for gxt_on_R, stack high-water not measured\n");
    }
    g->r_stack_lo = lo; g->r_stack_hi = hi;
}

static uint32_t r_stack_highwater(void)
{
    const uintptr_t lo = g_gxt.r_stack_lo, hi = g_gxt.r_stack_hi;
    const volatile uint8_t *b = (const volatile uint8_t *) lo;
    uintptr_t i, n = hi - lo;
    if (!s.r_prefilled || !lo || hi <= lo) return 0;
    for (i = 0; i < n && b[i] == STACK_PATTERN; ++i) { }
    return (uint32_t) (n - i);
}

/* R PROFILE (2026-10-21, the locked-30 work: ~17 ms of R's 71 at the hub had no timer).  ux0:data/stairfax/gx_thread_prof.txt = N (a power of
 * two, 1..256): EVERY record is counted per op, and 1 record in N is timed (two clock reads around its handler; the clock's 1 us grain averages
 * out over thousands of samples).  Every 600 presents R prints the window: R busy, then per op count/frame, us/record and estimated ms/frame
 * (= count x mean), largest first, and "unattributed" = busy - the sum (the loop's own framing, counting and tail publication).  The op NUMBERS
 * are printed; host/engine/rprof.py maps them to names from gx_rt_ops.h.  No file = off = one predicted branch per record. */
static uint32_t rp_on, rp_mask, rp_k, rp_pres;
static uint32_t rp_cnt[GXT_OP_MAX + 1], rp_smp[GXT_OP_MAX + 1], rp_us[GXT_OP_MAX + 1];
static uint32_t rp_busy_us;
static void rp_print(uint32_t frames)
{
    double tot = 0.0; int printed = 0;
    printf("[gxt] rprof: %u presents, sample 1/%u | R busy %.2f ms/frame\n", frames, rp_mask + 1u, rp_busy_us / 1000.0 / frames);
    for (;;) {                                                   /* largest estimated cost first; at most 30 lines, stop under 0.05 ms/frame */
        double best = -1.0; int bi = -1;
        for (int i = 0; i <= GXT_OP_MAX; ++i) if (rp_cnt[i]) {
            const double est = rp_smp[i] ? (double) rp_us[i] / rp_smp[i] * rp_cnt[i] : 0.0;
            if (est > best) { best = est; bi = i; }
        }
        if (bi < 0) break;
        tot += best;
        if (printed < 30 && best / 1000.0 / frames >= 0.05) {
            printf("[gxt] rprof op %u: %.1f/frame %.2f us each (%u samples) = %.2f ms/frame\n", (unsigned) bi, (double) rp_cnt[bi] / frames,
                   rp_smp[bi] ? (double) rp_us[bi] / rp_smp[bi] : 0.0, rp_smp[bi], best / 1000.0 / frames);
            ++printed;
        }
        rp_cnt[bi] = 0;
    }
    printf("[gxt] rprof: attributed %.2f ms/frame, unattributed (loop framing/publish) %.2f ms/frame\n", tot / 1000.0 / frames, (rp_busy_us - tot) / 1000.0 / frames);
    memset(rp_cnt, 0, sizeof rp_cnt); memset(rp_smp, 0, sizeof rp_smp); memset(rp_us, 0, sizeof rp_us); rp_busy_us = 0;
}

/* REDUNDANT-STATE COUNTER (2026-10-21, locked-30 lever B: does the game set state it already set?).  G side, marker gx_thread_red.txt = 1.
 * Each recorded setter is compared with the last value recorded for the same (op, target): target = payload word 0 for the per-stage /
 * per-slot setters (TEV stage, matrix id, vertex attribute, channel, texcoord), none for the global ones.  Two bounds are counted:
 *   cons  -- the known state is FORGOTTEN at every display-list call and at every op this table does not model (a list or a compound
 *            setter such as GXSetTevOp can rewrite the state behind the recorder's back): a LOWER bound on what G could drop;
 *   upper -- payload equal to the last same-target call, ignoring those resets: an UPPER bound.
 * Calls between GXBeginDisplayList and GXEndDisplayList are not judged.  Printed every 600 presents by gxt_frame_edge ("[gxt] red"). */
int gxt_red_on;
const uint32_t *gxt_red_last;
/* The filter's hook state lives HERE, not in gx_rt_filter.c: gx_rt.h's inline gxt_rec_alloc names it, so every TU that inlines it must link it, and the ring test links gx_thread.c alone. */
int gxt_flt_on;
GxtFltOp gxt_flt_ops[GXT_FLT_OPS];          /* the filter's per-op descriptors (gx_rt.h v3 fast path); zero = nothing filtered */
uint8_t gxt_flt_fast;
uint8_t gxt_flt_cls[GXT_FLT_OPS];
enum { RK_NEUTRAL = 0, RK_TARGET, RK_GLOBAL, RK_BUMP, RK_CLEARVD };
typedef struct { uint32_t h_c, ep_c, h_u; uint8_t v_c, v_u; } RedEnt;
static uint8_t  red_kind[GXT_OP_MAX + 1];
static RedEnt  *red_tab;                      /* (GXT_OP_MAX + 1) x 256, calloc'd on enable */
static uint32_t red_epoch = 1, red_in_list;
static uint32_t red_n[GXT_OP_MAX + 1], red_c[GXT_OP_MAX + 1], red_u[GXT_OP_MAX + 1];
static void red_init(void)
{
    static const uint16_t tgt[] = { GXT_OP_GXSetTevAlphaIn, GXT_OP_GXSetTevAlphaOp, GXT_OP_GXSetTevColorIn, GXT_OP_GXSetTevColorOp, GXT_OP_GXSetTevOrder,
        GXT_OP_GXSetTevSwapMode, GXT_OP_GXSetTevDirect, GXT_OP_GXSetTevKColorSel, GXT_OP_GXSetTevKAlphaSel, GXT_OP_GXSetTevColor, GXT_OP_GXSetTevKColor,
        GXT_OP_GXSetTevColorS10, GXT_OP_GXSetTevIndirect, GXT_OP_GXSetTevSwapModeTable, GXT_OP_GXSetIndTexOrder, GXT_OP_GXSetIndTexCoordScale,
        GXT_OP_GXSetIndTexMtx, GXT_OP_GXSetTexCoordGen2, GXT_OP_GXSetChanCtrl, GXT_OP_GXSetChanAmbColor, GXT_OP_GXSetChanMatColor, GXT_OP_GXSetVtxDesc,
        GXT_OP_GXSetArray, GXT_OP_GXLoadPosMtxImm, GXT_OP_GXLoadNrmMtxImm, GXT_OP_GXLoadTexMtxImm };
    static const uint16_t glob[] = { GXT_OP_GXSetCullMode, GXT_OP_GXSetBlendMode, GXT_OP_GXSetFog, GXT_OP_GXSetAlphaCompare, GXT_OP_GXSetNumTexGens,
        GXT_OP_GXSetNumTevStages, GXT_OP_GXSetNumIndStages, GXT_OP_GXSetNumChans, GXT_OP_GXSetZMode, GXT_OP_GXSetColorUpdate, GXT_OP_GXSetAlphaUpdate,
        GXT_OP_GXSetProjection, GXT_OP_GXSetViewport, GXT_OP_GXSetScissor, GXT_OP_GXSetCurrentMtx, GXT_OP_GXSetZCompLoc, GXT_OP_GXSetDither,
        GXT_OP_GXSetLineWidth, GXT_OP_GXSetPointSize };
    static const uint16_t neutral[] = { GXT_OP_GXBegin, GXT_OP_VBYTES, GXT_OP_PAD, GXT_OP_PRESENT, GXT_OP_SYNC, GXT_OP_GXLoadTexObj,
        GXT_OP_GXLoadTexObjPreLoaded, GXT_OP_GXCopyTex, GXT_OP_GXSetTexCopySrc, GXT_OP_GXSetTexCopyDst, GXT_OP_GXCopyDisp, GXT_OP_GXFlush,
        GXT_OP_GXPixModeSync, GXT_OP_GXTexModeSync, GXT_OP_GXInvalidateTexAll, GXT_OP_GXInvalidateVtxCache, GXT_OP_GXSetDrawSync, GXT_OP_GXReadDrawSync,
        GXT_OP_GXInitTexObj, GXT_OP_GXInitTexObjLOD, GXT_OP_GXInitTexObjUserData };
    for (uint32_t i = 0; i <= GXT_OP_MAX; ++i) red_kind[i] = i >= GXT_OP_EXTRA_BASE && i < 1000u ? RK_NEUTRAL : RK_BUMP;   /* the [extra] hooks (gx_dlk_event, gx_tex_*) change no GX state */
    for (size_t i = 0; i < sizeof tgt / sizeof tgt[0]; ++i) red_kind[tgt[i]] = RK_TARGET;
    for (size_t i = 0; i < sizeof glob / sizeof glob[0]; ++i) red_kind[glob[i]] = RK_GLOBAL;
    for (size_t i = 0; i < sizeof neutral / sizeof neutral[0]; ++i) red_kind[neutral[i]] = RK_NEUTRAL;
    red_kind[GXT_OP_GXClearVtxDesc] = RK_CLEARVD;
    red_tab = (RedEnt *) calloc((size_t) (GXT_OP_MAX + 1) * 256u, sizeof *red_tab);
    if (!red_tab) { gxt_red_on = 0; printf("[gxt] red: table allocation failed, counter OFF\n"); }
}
void gxt_red_note(const uint32_t *pl)
{
    const uint32_t op = pl[-1] & 1023u, nw = pl[-1] >> 10;
    if (op == GXT_OP_GXBeginDisplayList) { red_in_list = 1; ++red_epoch; return; }
    if (op == GXT_OP_GXEndDisplayList) { red_in_list = 0; ++red_epoch; return; }
    if (red_in_list || !red_tab) return;
    const uint8_t k = red_kind[op];
    if (k == RK_NEUTRAL) return;
    if (k == RK_BUMP) { ++red_epoch; ++red_n[op]; return; }
    if (k == RK_CLEARVD) { memset(&red_tab[(size_t) GXT_OP_GXSetVtxDesc * 256u], 0, 256u * sizeof *red_tab); ++red_n[op]; return; }
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < nw; ++i) h = (h ^ pl[i]) * 16777619u;
    RedEnt *e = &red_tab[(size_t) op * 256u + (k == RK_TARGET && nw ? (pl[0] & 255u) : 0u)];
    ++red_n[op];
    if (e->v_c && e->ep_c == red_epoch && e->h_c == h) ++red_c[op];
    if (e->v_u && e->h_u == h) ++red_u[op];
    e->h_c = h; e->ep_c = red_epoch; e->v_c = 1; e->h_u = h; e->v_u = 1;
}
static void red_print(uint32_t frames)
{
    uint64_t n = 0, c = 0, u = 0;
    printf("[gxt] red: %u presents | per judged op: calls/frame, redundant/frame conservative (forgotten at lists + unmodelled ops) .. upper\n", frames);
    for (uint32_t i = 0; i <= GXT_OP_MAX; ++i) if (red_n[i] && (red_kind[i] == RK_TARGET || red_kind[i] == RK_GLOBAL)) {
        n += red_n[i]; c += red_c[i]; u += red_u[i];
        if (red_n[i] / frames >= 20u) printf("[gxt] red op %u: %.1f/frame redundant %.1f .. %.1f\n", (unsigned) i, (double) red_n[i] / frames, (double) red_c[i] / frames, (double) red_u[i] / frames);
    }
    uint64_t b = 0; for (uint32_t i = 0; i <= GXT_OP_MAX; ++i) if (red_kind[i] == RK_BUMP) b += red_n[i];
    printf("[gxt] red: judged %.1f/frame, redundant %.1f (%.1f%%) .. %.1f (%.1f%%); state forgotten %.1f times/frame (lists + unmodelled ops)\n",
           (double) n / frames, (double) c / frames, n ? 100.0 * c / n : 0.0, (double) u / frames, n ? 100.0 * u / n : 0.0, (double) b / frames);
    memset(red_n, 0, sizeof red_n); memset(red_c, 0, sizeof red_c); memset(red_u, 0, sizeof red_u);
}

static void r_main(void)
{
    GxtShared *g = &g_gxt;
    uint32_t tail_pub;
    r_stack_prefill();
    s.r_tp = gxtp_thread_pointer();
    s.r_cpu_last = gxtp_cpu_id();
    GXT_STORE_REL(&g->r_alive, 1);
    tail_pub = s.r_tail;
    for (;;) {
        uint32_t pub;
        if (g->r_stop) break;
        if (g->r_parked) {                                /* DIRECT: nothing to do; sleep in bounded slices so a lost signal costs 100 ms, never a hang */
            GXT_STORE_REL(&s.r_in_park, 1);                   /* G may now rewrite R's private cursor (ring_reset): R is out of the batch loop and will re-read tail below */
            while (g->r_parked && !g->r_stop) (void) gxtp_sema_wait(g->sema_r, 100000u);
            GXT_STORE_REL(&s.r_in_park, 0);
            s.r_tail = GXT_LOAD_ACQ(&g->tail); tail_pub = s.r_tail;      /* the ring is empty and head == pub == tail when G unparks R */
            continue;
        }
        pub = GXT_LOAD_ACQ(&g->pub);
        if (s.r_tail != pub) {
            const uint32_t tb = gxtp_now_us();
            int quit = 0;
            s.r_pres_acc = 0;
            while (s.r_tail != pub) {
                const uint32_t hdr = *(const uint32_t *) (g->buf + (s.r_tail & g->mask));
                const uint32_t op = hdr & 1023u, nw = hdr >> 10;
                const uint32_t end = s.r_tail + ((nw + 1u) << 2);
                const uint32_t *pl = (const uint32_t *) (g->buf + (s.r_tail & g->mask)) + 1;
                s.r_last_op = op;
                if ((int32_t) (end - pub) > 0 || (s.r_tail & g->mask) + ((nw + 1u) << 2) > g->size) {          /* corrupt framing: never run off the published bytes */
                    ++s.r_corrupt; ++g->st.r_ops_unknown;
                    if (s.r_corrupt_logged++ < 8) printf("[gxt] R: corrupt record at +%u: op %u nwords %u (published to +%u)\n", s.r_tail - tail_pub, op, nw, pub - s.r_tail);
                    g->r_cur_end = pub; s.r_tail = pub;
                    break;
                }
                g->r_cur_end = end;
                if (op != GXT_OP_PAD) ++g->st.r_recs;     /* counted BEFORE the handler: a handler that publishes tail itself (PRESENT, SYNC, MODE_LEAVE) must not let G see tail past a record that r_recs has not counted */
                switch (op) {
                case GXT_OP_PAD:       break;
                case GXT_OP_PRESENT:   r_present(pl, nw); break;
                case GXT_OP_SYNC:      gxt_r_mark_done(); break;
                case GXT_OP_HAMMER:    ham_check(pl, nw); break;
                case GXT_OP_STATE: {   /* R2: copy the dirty groups into gx_hal; a refused record is also an unknown op (must stay 0).  Timed by rprof like a default op (2026-10-23: hw_R2a's R did not move; where does R2's apply go?) */
                    const int smp = rp_on && ((++rp_k & rp_mask) == 0);
                    const uint32_t t0 = smp ? gxtp_now_us() : 0u;
                    if (&gxs_r_apply == NULL || gxs_r_apply(pl, nw) != 0) ++g->st.r_ops_unknown;
                    if (smp) { rp_us[op] += gxtp_now_us() - t0; ++rp_smp[op]; }
                    break;
                }
                case GXT_OP_MODE_LEAVE:
                    g->r_parked = 1;                      /* park BEFORE tail covers this record: G sees "tail passed" => R is already (about to be) parked */
                    gxt_r_mark_done();
                    break;
                case GXT_OP_QUIT:
                    if ((g->cfg.brk & GXT_BRK_RSTALL) && !s.r_stalled) {   /* the exit gate on a run QUIT reaches before the PRESENT stall point (300 presents): R ignores QUIT for T_RSTALL_MS, so gxt_quit's bounded join must end in `R did not stop` */
                        s.r_stalled = 1;
                        printf("[gxt] R: rstall -- sleeping %u ms inside the QUIT record\n", T_RSTALL_MS);
                        gxtp_delay_us(T_RSTALL_MS * 1000u);
                    }
                    if (s.split_on) { (void) gxsp_f_quit(); s.split_on = 0; g->r_pub_hook = NULL; }        /* R5: the back drains and stops before the front does */
                    quit = 1; break;
                default: {
                    const int smp = rp_on && ((++rp_k & rp_mask) == 0);
                    const uint32_t t0 = smp ? gxtp_now_us() : 0u;
                    if (gxt_r_exec(op, pl, nw) < 0) {
                        ++g->st.r_ops_unknown;
                        if (s.r_unknown_logged++ < 8) printf("[gxt] R: unknown op %u (nwords %u)\n", op, nw);
                    }
                    if (smp) { rp_us[op] += gxtp_now_us() - t0; ++rp_smp[op]; }
                    break;
                }
                }
                if (rp_on) {
                    ++rp_cnt[op];
                    if (op == GXT_OP_PRESENT && ++rp_pres % 600u == 0u) { rp_busy_us += (gxtp_now_us() - tb) - s.r_pres_acc; rp_print(600u); rp_busy_us = -((gxtp_now_us() - tb) - s.r_pres_acc); }
                }
                s.r_tail = end;
                if ((uint32_t) (s.r_tail - tail_pub) >= 4096u) { if (g->r_pub_hook) g->r_pub_hook(); GXT_STORE_REL(&g->tail, s.r_tail); tail_pub = s.r_tail; gxt_r_wake_g(); }
                if (quit) break;
            }
            g->st.r_busy_us += (gxtp_now_us() - tb) - s.r_pres_acc;
            if (rp_on) rp_busy_us += (gxtp_now_us() - tb) - s.r_pres_acc;
            if (g->r_pub_hook) g->r_pub_hook();               /* R5: the RHI records written by this batch reach R-back BEFORE G can see the tail pass them */
            GXT_STORE_REL(&g->tail, s.r_tail); tail_pub = s.r_tail;
            gxt_r_wake_g();
            if (quit) break;
            continue;
        }
        /* the ring is empty: the tail is published; spin (G publishes every ~200 us), then announce sleep */
        {
            const uint32_t ti = gxtp_now_us();
            int found = 0;
            uint32_t n = 0;
            for (;;) {
                gxtp_relax();
                if (GXT_LOAD_ACQ(&g->pub) != s.r_tail) { found = 1; break; }
                if ((++n & 15u) == 0 && (uint32_t) (gxtp_now_us() - ti) >= T_SPIN) break;
            }
            if (!found && !g->r_stop && !g->r_parked) {
                g->r_sleeping = 1;
                GXT_FENCE_SB();
                if (GXT_LOAD_ACQ(&g->pub) != s.r_tail) g->r_sleeping = 0;
                else {
                    const uint32_t sent0 = g->st.wakes;           /* G counts a wake signal BEFORE it sends it */
                    const int w = gxtp_sema_wait(g->sema_r, GXT_R_TIMED_WAKE_US);
                    const int data = GXT_LOAD_ACQ(&g->pub) != s.r_tail;      /* checked while still announced asleep: whatever was published since, G saw r_sleeping == 1 */
                    const int sent = g->st.wakes != sent0;
                    g->r_sleeping = 0;
                    if (w > 0) ++g->st.r_wakes;
                    else if (data) {
                        if (sent && gxtp_sema_wait(g->sema_r, 0) > 0) ++g->st.r_wakes;   /* the signal was sent and arrived just after the timeout: not lost; it is consumed here */
                        else ++g->st.r_timedwake;     /* data appeared while R was asleep and no signal arrived: a lost wake-up (or the dropsig break arm), recovered by the timer */
                    }
                }
            }
            g->st.r_starve_us += gxtp_now_us() - ti;
        }
    }
    GXT_STORE_REL(&g->tail, s.r_tail);
    gxt_r_wake_g();
    g->st.r_stack_hw = r_stack_highwater();
    GXT_STORE_REL(&g->r_alive, 0);
}

/* ------------------------------------------------------------------------------------------------------------------------------ resources */
static int ring_alloc(GxtShared *g, uint32_t bytes)
{
    uint8_t *raw;
#ifdef GXT_HOST
    extern int gxt_host_fail_malloc;
    if (gxt_host_fail_malloc) return -1;
#endif
    raw = (uint8_t *) malloc((size_t) bytes + 64u);
    if (!raw) return -1;
    memset(raw, RING_FILL, (size_t) bytes + 64u);
    if (s.ring_raw) free(s.ring_raw);
    s.ring_raw = raw;
    g->buf = (uint8_t *) (((uintptr_t) raw + 63u) & ~(uintptr_t) 63u);
    g->size = bytes; g->mask = bytes - 1u;
    return 0;
}

/* Put every cursor at `start` (an empty ring).  R must be parked or absent. */
static void ring_reset(GxtShared *g, uint32_t start)
{
    g->head = g->pub_g = g->tail_cache = start;
    g->pub = start; g->tail = start; g->r_cur_end = start;
    s.r_tail = start;
    g->push_cur = g->push_end = g->chunk_data = NULL;
    g->room = 0;
}

static int r_ensure(void)
{
    GxtShared *g = &g_gxt;
    uint32_t t0;
    if (s.r_created) return 1;
    g->r_parked = 1; g->r_stop = 0; g->r_alive = 0;
    {
        int core = g->cfg.r_core;
        if (core == -2) {                                 /* auto: a user core G is not on (G's own core is read now; T0-d found G on core 1 in 84% of arm-0 samples) */
            const int gc = gxtp_cpu_id(); static const int pref[3] = { 2, 0, 1 };
            core = -1;
            for (int i = 0; i < 3; ++i) if (pref[i] != gc) { core = pref[i]; break; }
        }
        s.rcore_eff = core;
        g->r_thread = gxtp_thread_create("stfx_gxr", r_main, 0x10000100, GXT_R_STACK_BYTES, core);
    }
    if (g->r_thread <= 0) { snprintf(s.refuse, sizeof s.refuse, "thread create failed"); return 0; }
    if (gxtp_thread_start(g->r_thread) < 0) { gxtp_thread_delete(g->r_thread); g->r_thread = 0; snprintf(s.refuse, sizeof s.refuse, "thread start failed"); return 0; }
    t0 = gxtp_now_us();
    while (!GXT_LOAD_ACQ(&g->r_alive)) {
        if ((uint32_t) (gxtp_now_us() - t0) > T_CREATE) { snprintf(s.refuse, sizeof s.refuse, "R did not start within %u ms", T_CREATE / 1000u); return 0; }
        gxtp_delay_us(500u);
    }
    s.r_created = 1;
    printf("[gxt] R started: thread stfx_gxr, rcore=%d (G on cpu %d), stack %u KB\n", s.rcore_eff, gxtp_cpu_id(), GXT_R_STACK_BYTES / 1024u);
    return 1;
}

static void refuse_now(const char *why)
{
    printf("[gxt] REFUSED: %s; staying DIRECT\n", why);
    g_gxt.cfg.refused = 1;
    s.idle = 1;
}

/* ------------------------------------------------------------------------------------------------------------------------------ boot */
static const struct { const char *w; uint32_t bit; } k_brk[] = {
    { "scribble", GXT_BRK_SCRIBBLE }, { "jump2", GXT_BRK_JUMP2 }, { "nostep", GXT_BRK_NOSTEP }, { "rdelay", GXT_BRK_RDELAY }, { "rstall", GXT_BRK_RSTALL },
    { "dropsig", GXT_BRK_DROPSIG }, { "bp_early", GXT_BRK_BP_EARLY }, { "snapoff", GXT_BRK_SNAPOFF }, { "ahead2", GXT_BRK_AHEAD2 },
    { "dropdraw", GXT_BRK_DROPDRAW }, { "noflush", GXT_BRK_NOFLUSH }, { "moviesnap", GXT_BRK_MOVIESNAP }, { "mipsnap", GXT_BRK_MIPSNAP },
    { "fogshared", GXT_BRK_FOGSHARED }, { "nofogmirror", GXT_BRK_NOFOGMIRROR }, { "memhog", GXT_BRK_MEMHOG }, { "snapnrm", GXT_BRK_SNAPNRM }, { "snapall", GXT_BRK_SNAPALL },
};

static int parse_core(const char *b, int n)
{
    if (n < 1) return -2;
    if (b[0] == 'x' || b[0] == 'X') return -1;
    if (b[0] >= '0' && b[0] <= '2') return b[0] - '0';
    return -3;                                            /* garbage */
}

static const char *mode_name(int m)
{
    return m == '1' ? "threaded" : m == '2' ? "lockstep" : m == 'a' ? "arm" : m == 'h' ? "hammer" : "direct";
}
static const char *core_str(int c, char *buf)
{
    if (c == -2) return "auto";
    if (c == -1) return "none";
    snprintf(buf, 8, "%d", c);
    return buf;
}

void gxt_boot(void)
{
    GxtShared *g = &g_gxt;
    char b[256], bad[200] = { 0 };
    int n, mode = 0, mode_default = 0;
#ifndef GXT_HOST
    mode = '1'; mode_default = 1;   /* 2026-10-25 (Tyler, docs/54): THREADED is the shipping default; gx_thread.txt "0" = DIRECT. The host tests keep DIRECT as their default. */
#endif
    uint32_t kb = GXT_RING_DEFAULT_KB;
    if (s.booted) return;
    s.booted = 1; s.count_recs = 1;
    memset(&g->cfg, 0, sizeof g->cfg);
    g->cfg.ahead = 1; g->cfg.r_core = g->cfg.g_core = -2; g->cfg.rdelay_ms = 33;
    g->sema_r = g->sema_g = 0;

    n = gxtp_read_marker("gx_thread.txt", b, sizeof b);
    if (n > 0) {
        const char c = b[0];
        mode_default = 0;
        if (c == '1' || c == '2' || c == 'a' || c == 'h') mode = c;
        else if (c == '0') mode = 0;
        else if (c != '0' && c != '\n' && c != '\r' && c != ' ') snprintf(bad, sizeof bad, "gx_thread.txt: unknown mode '%c' (want 0|1|2|a|h)", c);
    }
    if ((n = gxtp_read_marker("gx_thread_ring.txt", b, sizeof b)) > 0) {
        const unsigned long v = strtoul(b, NULL, 10);
        if (v < 64ul || v > 16384ul || (v & (v - 1ul))) snprintf(bad, sizeof bad, "gx_thread_ring.txt: %lu KB is not a power of two in 64..16384", v);
        else kb = (uint32_t) v;
    }
    if ((n = gxtp_read_marker("gx_thread_core.txt", b, sizeof b)) >= 0) {
        g->cfg.r_core = parse_core(b, n);
        if (g->cfg.r_core == -3) { g->cfg.r_core = -2; snprintf(bad, sizeof bad, "gx_thread_core.txt: want x|0|1|2"); }
    }
    if ((n = gxtp_read_marker("gx_thread_gcore.txt", b, sizeof b)) >= 0) {
        g->cfg.g_core = parse_core(b, n);
        if (g->cfg.g_core == -3) { g->cfg.g_core = -2; snprintf(bad, sizeof bad, "gx_thread_gcore.txt: want x|0|1|2"); }
    }
    if ((n = gxtp_read_marker("gx_thread_trace.txt", b, sizeof b)) > 0) g->cfg.trace = b[0] == '1';
    if ((n = gxtp_read_marker("gx_thread_prof.txt", b, sizeof b)) > 0) {      /* the R profile (r_main): 1 record in N timed */
        const unsigned long v = strtoul(b, NULL, 10);
        if (v >= 1ul && v <= 256ul && !(v & (v - 1ul))) { rp_on = 1; rp_mask = (uint32_t) v - 1u; }
        else if (v) snprintf(bad, sizeof bad, "gx_thread_prof.txt: %lu is not a power of two in 1..256", v);
    }
    if ((n = gxtp_read_marker("gx_vb_bulk.txt", b, sizeof b)) > 0 && b[0] == '0') s.vb_off_m = 1;   /* R-VB default flip: '0' = the per-byte control */
    if ((n = gxtp_read_marker("gx_skin.txt", b, sizeof b)) > 0 && b[0] == '0') s.skin_off_m = 1;   /* R8 default flip: '0' = the CPU-blend control */
    if ((n = gxtp_read_marker("gx_skin.txt", b, sizeof b)) > 0 && b[0] == '1') s.skin_m = 1;   /* R8: GPU skinning ON in the fixed modes (0 / 1 / 2); arm mode: arm 149 decides.  Absent / 0: OFF (the default until the hardware PASS) */
    if ((n = gxtp_read_marker("gx_thread_split.txt", b, sizeof b)) > 0 && b[0] == '1') s.split_m1 = 1;   /* R5: capable + (threaded modes 1 / 2) ON; in arm mode the arm decides (148).  Absent / 0: never split */
    if ((n = gxtp_read_marker("gx_thread_fcore.txt", b, sizeof b)) >= 0) { s.fcore_cfg = parse_core(b, n); if (s.fcore_cfg == -3 || s.fcore_cfg == -2) { s.fcore_cfg = 0; snprintf(bad, sizeof bad, "gx_thread_fcore.txt: want x|0|1|2"); } }   /* R5: the front's core (default 0) */
    if ((n = gxtp_read_marker("gx_thread_red.txt", b, sizeof b)) > 0 && b[0] == '1') { gxt_red_on = 1; red_init(); }   /* the redundant-state counter (gxt_red_note) */
    if ((n = gxtp_read_marker("gx_thread_filt.txt", b, sizeof b)) > 0 && b[0] == '0') s.filt_off = 1;   /* the G-side redundant-state filter: absent or 1 = ON, 0 = OFF (gx_rt_filter.c) */
    if ((n = gxtp_read_marker("gx_thread_state.txt", b, sizeof b)) > 0 && b[0] == '0') s.state_off = 1;   /* R2 state records: absent or 1 = ON when threaded, 0 = the legacy setter records (+ filter v3) */
    if ((n = gxtp_read_marker("gx_thread_saudit.txt", b, sizeof b)) > 0) { const unsigned long v = strtoul(b, NULL, 10); s.saudit = v > 100000ul ? 100000 : (int) v; }   /* R2 whole-struct audit: every Nth flush */
    if ((n = gxtp_read_marker("gx_thread_audit.txt", b, sizeof b)) > 0) g->cfg.audit = b[0] == '1';
    if ((n = gxtp_read_marker("gx_thread_rdelay_ms.txt", b, sizeof b)) > 0) g->cfg.rdelay_ms = (uint32_t) strtoul(b, NULL, 10);
    if ((n = gxtp_read_marker("gx_thread_break.txt", b, sizeof b)) > 0) {
        char *w = b;
        while (*w) {
            char *e; size_t i; int known = 0;
            while (*w == ' ' || *w == '\t' || *w == '\r' || *w == '\n' || *w == ',') ++w;
            if (!*w) break;
            e = w; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n' && *e != ',') ++e;
            { const char sv = *e; *e = 0;
              for (i = 0; i < sizeof k_brk / sizeof k_brk[0]; ++i) if (!strcmp(w, k_brk[i].w)) { g->cfg.brk |= k_brk[i].bit; known = 1; }
              if (!strncmp(w, "dropsig=", 8)) { const unsigned long v = strtoul(w + 8, NULL, 10); if (v >= 2ul && v <= 100000ul) { gxt_t_dropsig_n = (uint32_t) v; g->cfg.brk |= GXT_BRK_DROPSIG; known = 1; } }
              if (!known) snprintf(bad, sizeof bad, "gx_thread_break.txt: unknown word '%.40s'", w);
              *e = sv; }
            w = e;
        }
    }
    if (mode_default) {   /* the shipping default runs the measured layout (every hw A/B, docs/54): R on core 2, G on core 1, unless a core marker says otherwise */
        if (g->cfg.r_core == -2) g->cfg.r_core = 2;
        if (g->cfg.g_core == -2) g->cfg.g_core = 1;
    }
    g->cfg.mode = mode; g->cfg.ring_bytes = kb * 1024u;
    g->cfg.ahead = mode == '2' ? 0 : (g->cfg.brk & GXT_BRK_AHEAD2) ? 2 : 1;
    if (mode == 'a' || mode == '1') g->cfg.ahead = (g->cfg.brk & GXT_BRK_AHEAD2) ? 2 : 1;

    s.silent = (mode == 0 && !g->cfg.trace && !g->cfg.audit && !g->cfg.brk && !bad[0] && g->cfg.r_core == -2 && g->cfg.g_core == -2);
    s.idle = mode == 0 || bad[0];
    if (s.silent) return;                                 /* plain DIRECT: print nothing, create nothing, and gxt_frame_edge returns at once */

    gxtp_host_init();
    if (mode == '1' || mode == '2' || mode == 'a') {
        if (gx_tev_ind_enabled) (void) gx_tev_ind_enabled();   /* pre-warm the lazy marker read on G before R exists */
    }
    {   char rc[8], gc[8];
        printf("[gxt] boot: mode=%s ahead=%d ring=%uKB rcore=%s gcore=%s trace=%d audit=%d brk=0x%x refused=%d filt=%d state=%d saudit=%d\n", mode_name(mode), g->cfg.ahead, kb,
               core_str(g->cfg.r_core, rc), core_str(g->cfg.g_core, gc), g->cfg.trace, g->cfg.audit, g->cfg.brk, bad[0] ? 1 : 0, filt_effective(), state_effective(), s.saudit);
    }
    if (bad[0]) { refuse_now(bad); return; }
    if (mode == 0) return;                                /* direct with trace/audit/break markers: the instruments only */

    if (mode != 'h') {
        char why[200];
        if (!(&g_stfx_backend) || g_stfx_backend != STFX_BK_GXM) { refuse_now("backend is not GXM (threaded mode exists only on the GXM backend)"); return; }
        why[0] = 0;
        if (gxts_refuse_reason(why, (int) sizeof why)) { char m[260]; snprintf(m, sizeof m, "%s", why[0] ? why : "special module refused"); refuse_now(m); return; }
    }
    if (g->cfg.g_core >= 0) { gxtp_set_affinity_self(g->cfg.g_core); s.gcore_eff = g->cfg.g_core; printf("[gxt] G pinned to core %d\n", g->cfg.g_core); }
    g->sema_r = gxtp_sema_create("stfx_gxr_r"); g->sema_g = gxtp_sema_create("stfx_gxr_g");
    if (g->sema_r <= 0 || g->sema_g <= 0) { refuse_now("semaphore create failed"); return; }
    if (ring_alloc(g, g->cfg.ring_bytes) < 0) { refuse_now("ring malloc failed"); return; }
    s.ret = (RetEnt *) calloc(GXT_RETIRE_MAX, sizeof(RetEnt));
    if (!s.ret) { refuse_now("retire list malloc failed"); return; }
    ring_reset(g, 0);
    g->ahead = g->cfg.ahead;
    s.sema_ok = 1;
    s.win_from = 0;
}

/* ------------------------------------------------------------------------------------------------------------------------------ transitions */
static void fog_private(int on)
{
    if (on) {
        if ((g_gxt.cfg.brk & GXT_BRK_FOGSHARED) != 0u) return;       /* break arm `fogshared`: R reads and writes the game's gFogColor like DIRECT (the A/B of finding N1; racy by design) */
        if (&gx_fog_private && gx_fogColorR && gFogColor) { memcpy(gx_fogColorR, gFogColor, 4); gx_fog_private = 1; s.fog_on = 1; }
    } else if (&gx_fog_private) { gx_fog_private = 0; s.fog_on = 0; }
}

/* THE G-SIDE FILTER'S ARMING (gx_rt_filter.c).  Armed = threaded AND the marker is not 0 AND the module is linked AND the perf arm is not 111 (arm 111 = threaded AHEAD 1 with the filter OFF: the
 * A/B control of arm 6).  Every change of the armed state, and every DIRECT <-> threaded transition, forgets everything the filter remembers: the HAL's state may change while nothing is
 * recorded (direct mode runs the game's calls straight into it), so a remembered value could be stale. */
static void filt_transition(void) { if (&gxt_flt_mode_changed) gxt_flt_mode_changed(); }

static int enter_threaded(int ahead)
{
    GxtShared *g = &g_gxt;
    if (!r_ensure()) { char m[200]; snprintf(m, sizeof m, "%s", s.refuse); refuse_now(m); return 0; }
    gxts_threaded_begin();                                /* R is parked and nothing is in flight: every G shadow is rebuilt from the HAL's real getters */
    g->r_presented = g->seq;
    fog_private(1);
    g->ahead = ahead;
    g->push_cur = g->push_end = g->chunk_data = NULL;
    g->room = 0;                                          /* the first record takes the slow path and computes it */
    filt_transition();                                    /* A3(d): the filter forgets everything before the first threaded record */
    if (&gxs_config) gxs_config(s.saudit);
    state_apply(state_wanted(s.arm));                     /* R2: R is parked and the ring empty: gx_hal_g := gx_hal before the first threaded setter */
    g->on = 1;
    gx_rec_set_threaded(1);
    g->r_parked = 0;
    gxtp_sema_signal(g->sema_r);                          /* unpark R (it also re-checks every 100 ms) */
    ++g->st.switches;
    return 1;
}

/* R5 (host/engine/R5_SPLIT.md, section 8): the render thread split in two.  G switches it at a frame edge with the GX ring DRAINED (R is idle: nothing it recorded for the backend is in flight)
 * and, when it goes off, with the back drained too, so a switch is a quiescent point for both threads.  The marker gx_thread_split.txt: absent / 0 = OFF (the default until the hardware PASS), 1 =
 * capable (handles are proxies in both modes) and ON in the threaded modes 1 / 2; in arm mode ('a') the ARM decides: 148 = the split ON, every other threaded arm (146 = the control) OFF. */
static int desired_split(int arm)
{
    if (!sp_have((void *) &gxsp_g_enable) || !sp_have((void *) &gxsp_f_present) || s.split_refused) return 0;
    if (!s.split_m1) {
        if (g_gxt.cfg.mode == 'a' && arm == 148 && !s.split_warned) { s.split_warned = 1; printf("[gxt] split: arm 148 asks for the split but gx_thread_split.txt is not 1 (handles are real, not proxies): staying OFF\n"); }
        return 0;
    }
    if (g_gxt.cfg.mode == 'a') return arm == 148;
    return g_gxt.cfg.mode == '1' || g_gxt.cfg.mode == '2';
}
static void split_set(int want)
{
    GxtShared *g = &g_gxt;
    if (want == (int) s.split_on) return;
    (void) gxt_drain(want ? "split on" : "split off");        /* R has executed (= recorded for the back) everything the game recorded so far */
    if (want) {
        /* hw_R5 (docs/54 2026-10-26): moving R from G with a cross-thread affinity call did not take on the console (R sampled 60/60 on
         * core 2, the back there too). So R (= the front) stays on its create-time core and the BACK is created on gx_thread_fcore.txt's
         * core (default 0) -- a create-time mask, the path R itself uses. */
        if (!gxsp_g_enable(-1, s.fcore_cfg, g->r_thread)) { s.split_refused = 1; printf("[gxt] split: REFUSED, staying whole\n"); return; }
        gxsp_set_present_done(r_present_done_back);
        g->r_pub_hook = gxsp_f_publish;
        GXT_FENCE_SB();
        s.split_on = 1;
        printf("[gxt] split ON: R-front on core %d, R-back on core %d (arm %d)\n", s.rcore_eff, s.fcore_cfg, s.arm);
    } else {
        s.split_on = 0;
        g->r_pub_hook = NULL;
        GXT_FENCE_SB();
        gxsp_g_disable(-1);   /* R never moved (split_set's enable) */
        printf("[gxt] split OFF: R whole again on core %d (arm %d)\n", s.rcore_eff, s.arm);
    }
}

static void leave_threaded(void)
{
    GxtShared *g = &g_gxt;
    uint32_t t0, m;
    if (s.split_on) split_set(0);                         /* R5: the back is drained and parked before R parks */
    state_apply(0);                                    /* R2: the last dirty groups reach R BEFORE it parks (the HAL must hold G's program-order state when DIRECT resumes) */
    (void) gxt_rec_alloc(GXT_OP_MODE_LEAVE, 0);
    m = gxt_mark();
    t0 = gxtp_now_us();
    wait_ge(&g->tail, m, "mode-leave", W_BLOCK, 1);
    g->st.switch_drain_us = gxtp_now_us() - t0;
    g->on = 0;
    gx_rec_set_threaded(0);
    if (&gxt_flt_arm) gxt_flt_arm(0);                     /* disarm; gxt_flt_arm forgets on every change */
    filt_transition();                                    /* A3(d) */
    fog_private(0);
    gxts_threaded_end();
    retire_free_all();
    ++g->st.switches;
}

static int desired_threaded(int arm)
{
    if (g_gxt.cfg.mode == 'a' && arm == 155) return 1;   /* R-VB: 155 = arm 149 + the bulk VBYTES replay (gxt_vb_bulk_wanted) */
    if (g_gxt.cfg.mode == 'a' && arm == 156) return 1;   /* R-SC: 156 = arm 149 + the shadow caster mesh drawn from a prebuilt display list (shadow_dolphin.c, gStfxAb == 156) */
    switch (g_gxt.cfg.mode) {
    case '1': case '2': return 1;
    case 'a': return arm == 6 || arm == 7 || arm == 96 || arm == 98 || arm == 111 || arm == 130 || arm == 131 || arm == 132 || arm == 151 || arm == 152 || arm == 153 || arm == 154 || (arm >= 133 && arm <= 137) || arm == 139 || arm == 140 || (arm >= 141 && arm <= 150);   /* 141..144 = the release cuts + the R3c cuts, 145..147 = those + the R6 cuts (r3a_mask_for), 148 = arm 146's cuts + the R5 split (desired_split), 149 = arm 146's cuts + R8 GPU skinning (gxt_skin_wanted); 111 = arm 6 with the G-side filter OFF (filt_arm_wanted); 132 = arm 6 with R2 OFF and the filter ON (state_wanted); 133..137 = arm 6 with the R3a cuts switched (r3a_mask_for) */
    default:  return 0;
    }
}
/* R3a C0 (host/engine/R3_DRAW.md s2): the arm the C3 machinery (c3_cuts.h cuts, the always-on timers' switch gx_c3_toff, gx_draw.c's record-scan memo) must see.  The release default (fixed mode)
 * runs gStfxAb = 107 = every C3 cut ON and the per-call timers OFF; an arm-mode boot left gStfxAb = the perf arm, so every threaded arm ran with the timers ON (~3,560 clock reads a frame) and
 * the A/B measured them.  EVERY threaded perf arm now maps to 107; every other arm (0, 97, 99, the 100..109 C3 arms, 5, 114, ...) is itself.  vita_tool_main.c keeps the result in gStfxC3Arm. */
/* Arm 139 (hw_Rsplit, docs/54 2026-10-24) is the one exception: the release cuts with the always-on timers ON (C3 arm 104 = 107 without C3_B_TOFF) -- R's split is read from those timers. */
int gxt_c3_arm(int arm) { return (g_gxt.cfg.mode == 'a' && desired_threaded(arm)) ? ((arm == 139 || arm == 140 || arm == 150 || arm == 153) ? 104 : 107) : arm; }
/* Arm 150 (hw_R8p, docs/54 2026-10-26) = arm 149 (cuts + GPU skinning) with the timers ON, as 139 was for 107: today's release config, R's split read from the timers. */

/* R3a (host/engine/R3_DRAW.md s3): the four cuts as bits, one global (gx_draw.c's gx_r3a, read per draw) set at the frame edge from the perf arm.
 *   C1 uniforms by offset (rhi_gxm_draw.c)   C2 GXM state shadowing (rhi_gxm_draw.c)   C3 key/uniform memos (gx_draw.c, gx_tev.c, rhi_gxm_shader.c)   C4 small per-call waste (gx_draw.c)
 * The bit values are port/gx_hal_state.h's GX_R3A_* (gxh_lint.py checks the two agree; gx_thread.c cannot include that header on the host ring test).
 *   arm 6 = all ON; 133 = all OFF (today's path with C0 applied); 134..137 = all ON except C1..C4; arm 107 (the fixed-mode release default) = all ON unless ux0:data/stairfax/r3_off.txt exists;
 *   every other arm = all OFF, so no historical control changes meaning. */
#define GXT_R3A_C1 1u
#define GXT_R3A_C2 2u
#define GXT_R3A_C3 4u
#define GXT_R3A_C4 8u
#define GXT_R3A_ALL 15u
#define GXT_R3A_C5 16u   /* R3c (host/engine/R3C_CACHE.md): HAL program-clean display-list draw */
#define GXT_R3A_C6 32u   /* R3c: backend immediate-draw merge */
#define GXT_R3A_C7 64u   /* R3c: HAL display-list check trim */
#define GXT_R3C_ALL 112u
#define GXT_R3A_C8 128u  /* R6 (host/engine/R6_SKIN.md): HAL dynamic display-list records learn which arrays changed from the announced writes */
#define GXT_R3A_C9 256u  /* R6: partial POS / NRM mesh updates into rotating GPU slots (rhi_gxm_draw.c, gx_draw.c) */
#define GXT_R6_ALL 384u
extern void gx_r3a_set(unsigned mask) __attribute__((weak));   /* gx_draw.c: publishes the cut switches (a weak FUNCTION: a MinGW link nulls those, not a weak data reference) */
unsigned gxt_r3a_mask_for(int arm, int off_marker)
{
    switch (arm) {
    case 6: return GXT_R3A_ALL;
    case 107: return off_marker ? 0u : ((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL | GXT_R3A_C9);   /* release default = arm 146 (hw_R6, docs/54 2026-10-25: R 51.43 vs 57.98, the pre-registered
                                                                           default rule). C3 OFF -- hw_R3a measured it HARMFUL (+3.07 ms R); C8 OFF -- hw_R6 measured it HARMFUL (+15.6 ms R) */
    case 134: return GXT_R3A_ALL & ~GXT_R3A_C1;
    case 135: return GXT_R3A_ALL & ~GXT_R3A_C2;
    case 136: return GXT_R3A_ALL & ~GXT_R3A_C3;
    case 137: return GXT_R3A_ALL & ~GXT_R3A_C4;
    case 141: return (GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL;                           /* R3c: the release default's cuts + C5 C6 C7 */
    case 142: return ((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) & ~GXT_R3A_C5;           /* 141 without C5 */
    case 143: return ((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) & ~GXT_R3A_C6;           /* 141 without C6 */
    case 144: return ((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) & ~GXT_R3A_C7;           /* 141 without C7 */
    case 145: return ((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) | GXT_R6_ALL;            /* R6: 141 + C8 + C9 */
    case 146: case 148: case 149: case 150: return (((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) | GXT_R6_ALL) & ~GXT_R3A_C8;   /* 145 without C8; 148 = the same cuts with the R5 split ON (R5_SPLIT.md s8: 146 is its control); 149 = the same cuts with R8 GPU skinning ON (R8_GPUSKIN.md; 146 is its control) */
    case 155: return gxt_r3a_mask_for(149, off_marker);   /* R-VB: arm 149 + gx_vb_bulk = 1 */
    case 156: return gxt_r3a_mask_for(149, off_marker);   /* R-SC: arm 149's cuts; the change is game-side (shadow_dolphin.c) */
    case 151: case 152: return gxt_r3a_mask_for(149, off_marker);
    case 153: return gxt_r3a_mask_for(149, off_marker);
    case 154: return gxt_r3a_mask_for(149, off_marker);   /* hw_GB: arm 149 + the BATCHED drawGlow (expgfx.c) */   /* hw_KO2: arm 150 (149 + timers) + drawGlow skipped = WHERE the glow cost lives in the [rsplit] split */   /* hw_KO (docs/54 2026-10-26): the immediate-draw KNOCKOUT arms = arm 149 + the game skipping drawGlow (151) / objDrawShadowCasterMesh (152) */
    case 147: return (((GXT_R3A_ALL & ~GXT_R3A_C3) | GXT_R3C_ALL) | GXT_R6_ALL) & ~GXT_R3A_C9;   /* 145 without C9 */
    case 139: case 140: return GXT_R3A_ALL & ~GXT_R3A_C3;   /* 140 = 139 + the 4-way program memo (hw_Mm2) */   /* hw_Rsplit: the release default's cuts (= 107's), timers ON via gxt_c3_arm */
    default:  return 0u;
    }
}
/* R8 (host/engine/R8_GPUSKIN.md): is GPU skinning wanted?  Arm mode ('a'): arm 149 (arm 146 is its control).  Every other mode: the marker gx_skin.txt = 1.  Default OFF.  vita_tool_main.c writes the answer into gGxSkinOn at the frame
 * edge (one writer, G; objprint reads it on G).  Prints one line per change. */
int gxt_skin_wanted(int arm)
{
    int on = g_gxt.cfg.mode == 'a' ? (arm == 149 || arm == 150) : s.skin_m != 0;   /* 150 = 149 + timers (profile arm) */
    if (g_gxt.cfg.mode == 'a' && (arm == 151 || arm == 152)) on = 1;
    if (g_gxt.cfg.mode == 'a' && arm == 153) on = 1;
    if (g_gxt.cfg.mode == 'a' && arm == 155) on = 1;   /* R-VB: 155 = 149 + the bulk VBYTES replay */
    if (g_gxt.cfg.mode == 'a' && arm == 156) on = 1;   /* R-SC: 156 = 149 + the prebuilt shadow-caster display list */
    if (g_gxt.cfg.mode == 'a' && arm == 154) on = 1;   /* hw_GB: 154 = 149 + batched drawGlow */   /* hw_KO2: 153 = 150 + drawGlow skipped */   /* hw_KO knockout arms = 149 + a skipped immediate-draw family */
    if (g_gxt.cfg.mode != 'a' && !s.skin_off_m) on = 1;   /* R8 DEFAULT FLIP (hw_R8tour3 PASS): the fixed modes skin on the GPU unless gx_skin.txt = 0 */
    if (!s.skin_seen || on != s.skin_last) {
        s.skin_seen = 1; s.skin_last = on;
        if (on || g_gxt.cfg.mode == 'a' || s.skin_m || s.skin_off_m) printf("[r8-skin] arm %d: GPU skinning %s (%s)\n", arm, on ? "ON" : "off", g_gxt.cfg.mode == 'a' ? "arm mode: arm 149 turns it on" : s.skin_off_m ? "gx_skin.txt = 0" : s.skin_m ? "gx_skin.txt" : "DEFAULT since hw_R8tour3");
    }
    return on;
}
/* R-VB (host/engine/R_VB.md): does R replay an immediate primitive's vertex bytes in bulk (gxfifo_push_bytes) instead of one gxfifo_push_u8 per byte?  Arm mode: arm 155 only (= 149 + this).  Every other arm and
 * mode: no.  vita_tool_main.c writes the answer into gx_vb_bulk at the frame edge (vt_c3_arm_sync, G only).  R reads it per VBYTES chunk with no handshake, which is safe BECAUSE the two replays are byte-exact
 * (gx_vb_bulk_test.c / the V1 replay runs): a chunk that sees the old or the new value produces the same gFifo, list bytes and draws. */
int gxt_vb_bulk_wanted(int arm)
{
    int on;
    if (g_gxt.cfg.mode != 'a') on = !s.vb_off_m;   /* R-VB DEFAULT (hw_VB VALID, -1.73 ms): fixed modes replay in bulk unless gx_vb_bulk.txt = 0 */
    else on = arm == 155;   /* arm mode: 155 = 149 + bulk; 149 stays the per-byte control */
    if (!s.vb_seen || on != s.vb_last) { s.vb_seen = 1; s.vb_last = on; printf("[vb] arm %d: bulk VBYTES replay %s (%s)\n", arm, on ? "ON" : "off", g_gxt.cfg.mode == 'a' ? "arm mode: arm 155 turns it on" : s.vb_off_m ? "gx_vb_bulk.txt = 0" : "DEFAULT since hw_VB"); }
    return on;
}
void gxt_r3a_edge(int arm)   /* called every present by vita_tool_main.c (vt_c3_arm_sync), NOT from gxt_frame_edge: a plain DIRECT boot keeps its silent, one-load frame edge (gx_ring_test section 7) */
{
    static int s_marker = -1, s_arm = -1; static unsigned s_mask = 0xFFFFFFFFu;
    unsigned m;
    if (s_marker < 0) {
#ifdef GXT_HOST
        s_marker = 0;
#else
        FILE *f = fopen("ux0:data/stairfax/r3_off.txt", "r");
        s_marker = f != NULL; if (f) fclose(f);
#endif
    }
    m = gxt_r3a_mask_for(arm, s_marker);
    if (arm != s_arm || m != s_mask) {
        s_arm = arm; s_mask = m;
        printf("[r3a] arm %d: cuts C1(uniforms by offset) %s C2(state shadow) %s C3(memos) %s C4(per-call waste) %s%s\n", arm, m & GXT_R3A_C1 ? "ON" : "off", m & GXT_R3A_C2 ? "ON" : "off",
               m & GXT_R3A_C3 ? "ON" : "off", m & GXT_R3A_C4 ? "ON" : "off", arm == 107 && s_marker ? " (r3_off.txt present)" : "");
        printf("[r3c-cuts] arm %d: C5(program-clean draw) %s C6(immediate merge) %s C7(dl check trim) %s\n", arm, m & GXT_R3A_C5 ? "ON" : "off", m & GXT_R3A_C6 ? "ON" : "off", m & GXT_R3A_C7 ? "ON" : "off");
        printf("[r6-cuts] arm %d: C8(dyn-record write prediction) %s C9(partial POS/NRM mesh update) %s\n", arm, m & GXT_R3A_C8 ? "ON" : "off", m & GXT_R3A_C9 ? "ON" : "off");
    }
    if (gx_r3a_set) gx_r3a_set(m);
}

static int desired_ahead(int arm)
{
    if (g_gxt.cfg.brk & GXT_BRK_AHEAD2) return 2;
    if (g_gxt.cfg.mode == '2') return 0;
    if (g_gxt.cfg.mode == 'a' && arm == 7) return 0;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------------------ the window line */
static void print_window(int present_no)
{
    GxtShared *g = &g_gxt;
    const GxtStats *a = &g->st, *b = &s.snap;
    const uint32_t from = s.win_from, to = (uint32_t) present_no;
    const uint32_t nf = to > from ? to - from : 1u;
    const double F = nf;
    char rep[640];
    int mode = s.mode_mask == 1u ? 0 : s.mode_mask == 2u ? 1 : -1, rl;
    rep[0] = 0;
    rl = gxts_report(rep, (int) sizeof rep - 1);
    if (rl < 0) rl = 0;
    if (rl >= (int) sizeof rep) rl = (int) sizeof rep - 1;
    rep[rl] = 0;
    while (rl > 0 && (rep[rl - 1] == ' ' || rep[rl - 1] == '\n')) rep[--rl] = 0;
    if (rl < (int) sizeof rep - 40) rl += snprintf(rep + rl, sizeof rep - (size_t) rl, "%sreclaims=%u reclaim_freed=%u", rl ? " " : "", a->reclaims, a->reclaim_freed);   /* appended to the special report: the window line's own format is the perf_ab contract */
    printf("[gxt] presents %u..%u arm %d mode %d ahead %d | recs/frame %.1f bytes/frame %.0f hw %u KB | G ms/frame: present-wait %.2f ring-full %.2f block %.2f"
           " | R ms/frame: busy %.2f starve %.2f present %.2f | behind 0:%u 1:%u 2+:%u | cpu G %u,%u,%u,%u R %u,%u,%u,%u"
           " | timedwake %u wakes %u retire %u/%u | timeouts %u stuck %u traps %u foreign %u unknown %u flipstall %d | %s\n",
           from, to, s.arm, mode, g->ahead,
           (a->recs - b->recs) / F, (a->rec_bytes - b->rec_bytes) / F, (s.occ_hw + 1023u) / 1024u,
           (a->present_wait_us - b->present_wait_us) / 1000.0 / F, (a->ring_full_us - b->ring_full_us) / 1000.0 / F, (a->block_us - b->block_us) / 1000.0 / F,
           (a->r_busy_us - b->r_busy_us) / 1000.0 / F, (a->r_starve_us - b->r_starve_us) / 1000.0 / F, (a->r_present_us - b->r_present_us) / 1000.0 / F,
           a->ahead_hist[0] - b->ahead_hist[0], a->ahead_hist[1] - b->ahead_hist[1], a->ahead_hist[2] - b->ahead_hist[2],
           a->g_cpu[0] - b->g_cpu[0], a->g_cpu[1] - b->g_cpu[1], a->g_cpu[2] - b->g_cpu[2], a->g_cpu[3] - b->g_cpu[3],
           a->r_cpu[0] - b->r_cpu[0], a->r_cpu[1] - b->r_cpu[1], a->r_cpu[2] - b->r_cpu[2], a->r_cpu[3] - b->r_cpu[3],
           a->r_timedwake, a->r_wakes, a->retire_n, a->retire_max,
           a->timeouts, a->stuck, a->viewer_traps, a->foreign_hook, a->r_ops_unknown, s.flipstall, rep[0] ? rep : "-");
    if (&gxs_print) gxs_print(nf);                        /* the "[gxt] state:" line of the same window (R2) */
    if (sp_have((void *) &gxsp_print) && s.split_m1) gxsp_print(nf);        /* the "[gxsp]" line of the same window (R5) */
    s.snap = *a;
    s.win_from = to; s.mode_mask = 0; s.occ_hw = 0; s.flipstall = 0;
}

/* ------------------------------------------------------------------------------------------------------------------------------ the device ring-hammer */
static uint32_t lcg(uint32_t *x) { *x = *x * 1664525u + 1013904223u; return *x; }

/* One variant: `nrecs` HAMMER records of random size through a ring of `ring_bytes` whose counters start at `start`.  `bursts` > 0: that many (burst, pause) pairs first, so R
 * goes to sleep between bursts and dropsig has signals to drop.  Returns the number of bad records (framing included), -1 if the variant could not run. */
static int hammer_variant(const char *name, uint32_t ring_bytes, uint32_t start, int dropsig, uint32_t nrecs, uint32_t bursts, uint32_t pause_us)
{
    GxtShared *g = &g_gxt;
    uint32_t i, x = 12345u, tw0, unk0, big_nw, bad, sw0;
    const uint32_t big_every = nrecs >= 1000000u ? 100000u : 5000u;     /* the device run (2,000,000 records) has 19 ring/2 records per variant; test scale has more per record */
    const uint32_t brk0 = g->cfg.brk;
    if (!r_ensure()) { printf("[gxt] hammer: %s not run (%s)\n", name, s.refuse); return -1; }
    { const uint32_t t0 = gxtp_now_us();                 /* R may still be finishing the MODE_LEAVE batch of the previous variant: swapping the ring or resetting its cursor now would race it */
      while (!GXT_LOAD_ACQ(&s.r_in_park)) {
          if ((uint32_t) (gxtp_now_us() - t0) > T_CREATE) { printf("[gxt] hammer: %s not run (R did not park)\n", name); return -1; }
          gxtp_delay_us(200u);
      } }
    if (g->size != ring_bytes && ring_alloc(g, ring_bytes) < 0) { printf("[gxt] hammer: %s not run (ring malloc %u failed)\n", name, ring_bytes); return -1; }
    ring_reset(g, start);
    s.pads = 0; s.ham_expect = 0; s.ham_bad = 0; s.ham_recs = 0; s.ham_logged = 0;
    if (dropsig) g->cfg.brk |= GXT_BRK_DROPSIG;
    tw0 = g->st.r_timedwake; unk0 = g->st.r_ops_unknown; sw0 = g->st.wakes;
    big_nw = ring_bytes / 8u - 1u; if (big_nw > (1u << 20)) big_nw = 1u << 20;
    g->r_parked = 0; gxtp_sema_signal(g->sema_r);
    for (i = 0; i < nrecs; ++i) {
        uint32_t nw = 2u + (lcg(&x) >> 16) % 62u, k, *p, c;
        if (i && (i % big_every) == 0u) nw = big_nw;     /* a few at ring/2 */
        p = gxt_rec_alloc(GXT_OP_HAMMER, nw);
        p[0] = i;
        for (k = 2; k < nw; ++k) p[k] = lcg(&x);
        c = 2166136261u ^ i;
        for (k = 2; k < nw; ++k) c = (c ^ p[k]) * 16777619u;
        p[1] = c;
#ifdef GXT_HOST
        if (i == gxt_host_ham_corrupt) p[1] ^= 1u;           /* host test hook: one deliberately wrong checksum -- proves the hammer's checker CAN fail */
#endif
        if (bursts) {
            if (i % 16u == 15u && i / 16u < bursts) { gxt_publish(); gxtp_delay_us(pause_us); }
        } else if ((lcg(&x) >> 20) == 0u) gxt_publish();       /* ~1 in 4096: vary the publish points */
    }
    gxt_drain("hammer");
    (void) gxt_rec_alloc(GXT_OP_MODE_LEAVE, 0);           /* park R again: the next variant (or the game) starts from an empty ring and a parked consumer */
    wait_ge(&g->tail, gxt_mark(), "hammer-park", W_BLOCK, 1);
    bad = s.ham_bad + (g->st.r_ops_unknown - unk0) + (s.ham_recs != nrecs ? 1u : 0u);
    g->cfg.brk = brk0;
    printf("[gxt] hammer: %s records %u bad %u timedwake %u\n", name, s.ham_recs, bad, g->st.r_timedwake - tw0);
    printf("[gxt] hammer cpu: G %d R %d tp G %p R %p pads %u wakes %u ring %uKB\n", gxtp_cpu_id(), s.r_cpu_last, (void *) gxtp_thread_pointer(), (void *) s.r_tp,
           s.pads, g->st.wakes - sw0, ring_bytes / 1024u);
    return (int) bad;
}

static void run_hammer(void)
{
    const uint32_t n = 2000000u;
    printf("[gxt] hammer: start, %u records per variant\n", n);
    hammer_variant("plain",    g_gxt.cfg.ring_bytes, 0xFFFFFF00u, 0, n, 0u,    0u);
    hammer_variant("dropsig",  g_gxt.cfg.ring_bytes, 0xFFFFF000u, 1, n, 4000u, 1500u);
    hammer_variant("wrap256k", 256u * 1024u,         0xFFFF0000u, 0, n, 0u,    0u);
    { const uint32_t t0 = gxtp_now_us(); while (!GXT_LOAD_ACQ(&s.r_in_park) && (uint32_t) (gxtp_now_us() - t0) < T_CREATE) gxtp_delay_us(200u); }   /* R parked before its ring is swapped */
    if (g_gxt.size != g_gxt.cfg.ring_bytes) (void) ring_alloc(&g_gxt, g_gxt.cfg.ring_bytes);     /* the last variant used a smaller ring: restore the configured one */
    ring_reset(&g_gxt, 0);
    printf("[gxt] hammer: done, returning to DIRECT\n");
}

/* ------------------------------------------------------------------------------------------------------------------------------ the frame edge */
int gxt_frame_edge(int arm, int present_no)
{
    GxtShared *g = &g_gxt;
    int want, ahead;
    if (GXT_LIKELY(s.idle)) return 0;
    ++s.edges;
    ++g->st.g_cpu[gxtp_cpu_id() & 3];
    s.mode_mask |= g->on ? 2u : 1u;
    if (&gGpuStallRetraceCount) { const int v = gGpuStallRetraceCount; if (v > s.flipstall) s.flipstall = v; }
    if (s.edges == 1) { s.arm = arm; s.win_from = present_no > 0 ? (uint32_t) present_no - 1u : 0u; }
    if (present_no > 0 && present_no % 60 == 0) print_window(present_no);
    if (gxt_red_on && present_no > 0 && present_no % 600 == 0) red_print(600u);
    if (&gxt_flt_print && !s.filt_off && present_no > 0 && present_no % 600 == 0) gxt_flt_print(600u);
    s.arm = arm;
    if (g->on) retire_free_ready();

    if (g->cfg.mode == 'h') {
        if (!s.hammer_ran) { s.hammer_ran = 1; run_hammer(); }
        s.idle = 1;
        return 0;
    }
    want = desired_threaded(arm); ahead = desired_ahead(arm);
    if (want != g->on) {
        if (g->list_open || g->chunk_data) ++g->st.switch_deferred;          /* never with a list or a chunk open */
        else if (want) enter_threaded(ahead);
        else leave_threaded();
    } else if (g->on && g->ahead != ahead) g->ahead = ahead;                  /* arm 6 <-> 7: the bound changes, nothing else does */
    if (g->on) state_apply(state_wanted(arm));                                 /* R2 <-> legacy at a frame edge (before the filter is armed or disarmed for the same arm) */
    if (g->on && !g->list_open && !g->chunk_data) { const int ws = desired_split(arm); if (ws != (int) s.split_on) split_set(ws); }   /* R5: the split follows the arm / marker, at a frame edge with the ring drained by split_set */
    if (&gxt_flt_diag) gxt_flt_diag(filt_diag_wanted(arm));                    /* before arming: a mode change forgets */
    if (&gxt_flt_arm) gxt_flt_arm(filt_arm_wanted(arm));                      /* arm 6 <-> 111 flips the filter at a frame edge; gxt_flt_arm forgets on every change */
    if (g->on) gxts_frame_edge_g(g->seq + 1u);
    return g->on;
}

/* ------------------------------------------------------------------------------------------------------------------------------ exit */
static void print_fin(void)
{
    GxtShared *g = &g_gxt;
    printf("[gxt] fin: seq %u r_presented %u recs %u r_recs %u pub %u tail %u\n", g->seq, GXT_LOAD_ACQ(&g->r_presented), g->st.recs, g->st.r_recs, GXT_LOAD_ACQ(&g->pub), GXT_LOAD_ACQ(&g->tail));
}

void gxt_quit(const char *why)
{
    GxtShared *g = &g_gxt;
    int joined = 0, i;
    if (s.quit_done) return;
    s.quit_done = 1;
    if (!s.booted || s.silent) return;                    /* plain DIRECT: silent */
    printf("[gxt] quit: %s\n", why ? why : "?");
    if (s.r_created && g->r_alive) {
        if (g->on) { (void) gxt_rec_alloc(GXT_OP_QUIT, 0); gxt_publish(); }
        else { g->r_stop = 1; gxtp_sema_signal(g->sema_r); }
        for (i = 0; i < 3 && !joined; ++i) joined = gxtp_thread_join(g->r_thread, T_JOIN);
        if (joined) {
            gxtp_thread_delete(g->r_thread); g->r_thread = 0;
            printf("[gxt] R stopped (stack hw %u KB)\n", (g->st.r_stack_hw + 1023u) / 1024u);
            if (g->on) {                                  /* R is gone and drained to the QUIT record: the shutdown that follows runs DIRECT */
                g->on = 0; gx_rec_set_threaded(0); if (&gxt_flt_arm) gxt_flt_arm(0); if (gxt_state_on) gxs_end(); filt_transition(); fog_private(0); gxts_threaded_end(); retire_free_all();
            }
        } else {
            printf("[gxt] R did not stop (last op %u)\n", s.r_last_op);
            gxt_r_unsafe = 1;
        }
    }
    if (g->cfg.audit) printf("[gxt] audit: mismatches %d\n", gxts_audit_bad());
    print_fin();
}

void gxt_stall_report(void)
{
    const GxtShared *g = &g_gxt;
    printf("[gxt] STALL head %u pub %u tail %u (unpublished %u, in flight %u) r_sleeping %d g_waiting %d r_parked %d r_alive %d r_last_op %u ahead %d seq %u r_presented %u"
           " ds_recorded %u r_ds_seq %u on %d room %d chunk %d list %d\n",
           g->head, g->pub, g->tail, g->head - g->pub, g->pub - g->tail, g->r_sleeping, g->g_waiting, g->r_parked, g->r_alive, s.r_last_op, g->ahead, g->seq, g->r_presented,
           g->ds_recorded, g->r_ds_seq, g->on, g->room, g->chunk_data != NULL, g->list_open);
    if (sp_have((void *) &gxsp_stall_report) && s.split_m1) gxsp_stall_report();
}

/* ------------------------------------------------------------------------------------------------------------------------------ host test hooks */
#ifdef GXT_HOST
#undef printf
#include <stdarg.h>
static char gxt_log_buf[1 << 18];
static size_t gxt_log_len;
static volatile int gxt_log_lock;
int gxt_host_printf(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap; int n;
    va_start(ap, fmt); n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return n;
    if ((size_t) n >= sizeof tmp) n = (int) sizeof tmp - 1;
    while (__atomic_test_and_set(&gxt_log_lock, __ATOMIC_ACQUIRE)) { }
    if (gxt_log_len + (size_t) n < sizeof gxt_log_buf - 1) { memcpy(gxt_log_buf + gxt_log_len, tmp, (size_t) n); gxt_log_len += (size_t) n; gxt_log_buf[gxt_log_len] = 0; }
    fwrite(tmp, 1, (size_t) n, stdout);
    __atomic_clear(&gxt_log_lock, __ATOMIC_RELEASE);
    return n;
}
void gxt_host_log_clear(void) { while (__atomic_test_and_set(&gxt_log_lock, __ATOMIC_ACQUIRE)) { } gxt_log_len = 0; gxt_log_buf[0] = 0; __atomic_clear(&gxt_log_lock, __ATOMIC_RELEASE); }
const char *gxt_host_log(void) { return gxt_log_buf; }
int gxt_host_fail_malloc;
void gxt_host_reset_all(void) { memset(&s, 0, sizeof s); memset(&g_gxt, 0, sizeof g_gxt); gxt_r_unsafe = 0; }
/* Bring the module to "threaded, ring of ring_bytes, counters at `start`" WITHOUT markers or preconditions, through the same enter_threaded() as the game uses. */
int gxt_host_setup(uint32_t ring_bytes, uint32_t start, int ahead, uint32_t brk, int go_threaded)
{
    GxtShared *g = &g_gxt;
    memset(&s, 0, sizeof s);
    memset(g, 0, sizeof *g);
    gxt_r_unsafe = 0;
    gxtp_host_init();
    s.booted = 1; s.count_recs = 1; s.saudit = gxt_host_saudit;
    g->cfg.mode = '1'; g->cfg.ahead = ahead; g->cfg.r_core = g->cfg.g_core = -1; g->cfg.rdelay_ms = 33; g->cfg.brk = brk; g->cfg.ring_bytes = ring_bytes;
    g->sema_r = gxtp_sema_create("r"); g->sema_g = gxtp_sema_create("g");
    if (g->sema_r <= 0 || g->sema_g <= 0) return -1;
    if (ring_alloc(g, ring_bytes) < 0) return -1;
    s.ret = (RetEnt *) calloc(GXT_RETIRE_MAX, sizeof(RetEnt));
    if (!s.ret) return -1;
    ring_reset(g, start & ~3u);                           /* byte counters are 4-aligned; the frame counters may start anywhere */
    g->ahead = ahead; g->seq = start; g->r_presented = start;
    if (go_threaded) { if (!enter_threaded(ahead)) return -1; }
    return 0;
}
void gxt_host_leave(void) { if (g_gxt.on) leave_threaded(); }
int  gxt_host_split(int on) { s.split_m1 = 1; s.fcore_cfg = -1; s.rcore_eff = -1; split_set(on); return (int) s.split_on; }   /* R5 host tests (gx_rt_replay_test.c threaded-split): the same split_set the frame edge uses */
int  gxt_host_enter(int ahead) { return g_gxt.on ? 1 : enter_threaded(ahead); }
void gxt_host_teardown(void)
{
    GxtShared *g = &g_gxt;
    gxt_quit("host teardown");
    if (gxt_r_unsafe) {                                   /* R was stalled by a break arm: it ends by itself once the stall is over; wait for it, then clean up as usual */
        uint32_t t0 = gxtp_now_us();
        while (GXT_LOAD_ACQ(&g->r_alive) && (uint32_t) (gxtp_now_us() - t0) < 15000000u) gxtp_delay_us(10000u);
        if (!g->r_alive) gxt_r_unsafe = 0;
    }
    if (!gxt_r_unsafe) {
        gxtp_sema_delete(g->sema_r); gxtp_sema_delete(g->sema_g);
        if (g->r_thread) gxtp_thread_delete(g->r_thread);
        free(s.ring_raw); free(s.ret);
        memset(&s, 0, sizeof s); memset(g, 0, sizeof *g);
    }
}
/* One hammer variant at test scale (the device runs the same function with 2,000,000 records). */
int gxt_host_hammer(const char *name, uint32_t ring_bytes, uint32_t start, int dropsig, uint32_t nrecs, uint32_t bursts, uint32_t pause_us)
{
    return hammer_variant(name, ring_bytes, start, dropsig, nrecs, bursts, pause_us);
}
uint32_t gxt_host_pads(void) { return s.pads; }
uint32_t gxt_host_r_last_op(void) { return s.r_last_op; }
void gxt_host_window(int present_no) { print_window(present_no); }
void gxt_host_set_arm(int arm) { s.arm = arm; }
void gxt_host_set_mode(int m) { g_gxt.cfg.mode = m; }
void gxt_host_set_skin_off(int v) { s.skin_off_m = v; }
void gxt_host_set_vb_off(int v) { s.vb_off_m = v; }   /* the gx_vb_bulk.txt = 0 control (c3_arm_map_test) */   /* the gx_skin.txt = 0 control (c3_arm_map_test) */
void gxt_host_set_idle(int v) { s.idle = v; s.silent = 0; }
void gxt_host_set_count_recs(int v) { s.count_recs = v; }
uint32_t gxt_host_occ_hw(void) { return s.occ_hw; }
#endif
