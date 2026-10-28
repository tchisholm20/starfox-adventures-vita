/* gx_rt.h -- THE SHARED CONTRACT of the GX render thread (docs/53 rev 2, re-planned for the GXM backend in host/engine/RT_THREAD.md).
 *
 * WRITTEN BY THE PLANNER.  Three modules include it and none may edit it (a change is a contract change: ask the orchestrator):
 *     ring     src/gx_thread.c           the ring, the R thread, fences' plumbing, mode switch, exit, stats, markers      (defines g_gxt and every gxt_* function below)
 *     gen      src/gx_rec.c + generated  the --wrap layer: G-side wrappers + the R-side dispatch switch                    (defines gxt_r_exec, gx_rec_set_threaded)
 *     special  src/gx_rt_special.c       everything with GX semantics that is not a plain "record by value": display-list recording, scissor shadow,
 *                                         written set + shadow arrays, dirty-image snapshots, the single-stepped fences, the blocking B8 copy
 *                                         (defines every gxts_* function below and gx_rt_special.h's gxts_G_X / gxts_R_X)
 * This header has NO GX types (so gx_thread.c compiles without the dolphin headers); the GX-typed prototypes live in gx_rt_special.h.
 *
 * VOCABULARY.  G = the game thread (whichever baton thread holds the baton: the ring has ONE producer at any time; the baton semaphores are the barrier).
 * R = the render thread ("stfx_gxr"): the only caller of the HAL's replayed bodies (__real_X) and of the GXM backend while threaded.  "Record" = G appends
 * to the ring.  "Replay" = R pops and calls __real_X.  DIRECT = no thread, no ring, behaviour as today (the default; gx_thread.txt absent or 0).
 *
 * EVERY cross-thread field below is annotated G-writes / R-writes.  Cross-thread reads and writes of anything wider than 32 bits are FORBIDDEN (ARMv7 has no
 * atomic 64-bit access without ldrexd and the vitasdk has no libatomic): times are uint32 microseconds compared modularly.  All sequence counters are 32-bit and
 * compared with (int32_t)(a - b) -- host tests start them at 0xFFFFFF00.
 */
#ifndef GX_RT_H
#define GX_RT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "gx_rt_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RhiInstance;
struct RhiSwapchain;

/* ------------------------------------------------------------------------------------------------------------------------------ constants */
#define GXT_RING_DEFAULT_KB   4096u      /* gx_thread_ring.txt overrides (KB, power of two, 64..16384) */
#define GXT_PUBLISH_BYTES     4096u      /* G publishes at least every this many unpublished bytes (streaming: R trails G by milliseconds, not by a frame) */
#define GXT_VB_CHUNK          2048u      /* maximum payload of one VBYTES record (+8 bytes of header must fit in one publish quantum, so a chunk never needs a special case) */
#define GXT_WAIT_US           2000000u   /* every G wait is bounded: it re-checks and LOGS every 2 s ("[gxt] G waits: <why> ... SLOW|STUCK") */
#define GXT_GIVEUP_US         10000000u  /* after 10 s a wait stops logging and lets the watchdog's deliberate fault happen (no auto-fallback to direct while R is wedged) */
#define GXT_R_TIMED_WAKE_US   1000u      /* R sleeps at most this long on an empty ring: a lost wake-up costs <= 1 ms, never a hang */
#define GXT_R_SPIN_US         300u       /* R spins (yield hint, re-reading pub) this long on an empty ring BEFORE it announces sleep: G publishes every ~200 us, so R rarely sleeps and G rarely pays a SignalSema syscall */
#define GXT_R_STACK_BYTES     0x100000u  /* 1 MB (baton threads get 256 KB); the high-water mark is measured and printed at exit */
#define GXT_RETIRE_MAX        4096u      /* retire-list entries before G drains (waits for R) */
#define GXT_SNAP_CAP_BYTES    (256u * 1024u) /* largest texture / array span copied at record time; above it: counted, loud, read live */
#define GXT_PIPE_SINK_BYTES   16u

/* ------------------------------------------------------------------------------------------------------------------------------ markers (read ONCE, by gxt_boot) */
/* ux0:data/stairfax/gx_thread.txt first char: absent/'0' DIRECT (default) | '1' threaded, AHEAD 1 | '2' threaded lockstep (AHEAD 0) | 'a' arm-driven (perf_ab arms 6/7/96/98) | 'h' device ring-hammer, then DIRECT
 * gx_thread_ring.txt (KB) | gx_thread_core.txt / gx_thread_gcore.txt (R / G: 'x' none, '0'..'2' a user core; absent = auto, RT_THREAD.md 6.3)
 * gx_thread_trace.txt (1: per-frame "[gxt] tr ..." line, printed by whichever thread executes the present, direct AND threaded) | gx_thread_audit.txt (1: G-L3 content audit)
 * gx_thread_rdelay_ms.txt | gx_thread_break.txt (whitespace separated words, one per GXT_BRK_*; HARNESS ONLY, never in a shipped run) */
typedef enum {
    GXT_BRK_SCRIBBLE = 1u << 0,   /* special: after enqueue G scribbles 16 bytes of a recorded, un-shadowed array span (audit must fail)        words: scribble */
    GXT_BRK_JUMP2    = 1u << 1,   /* special: R publishes draw-sync tokens in jumps of two                                                      jump2 */
    GXT_BRK_NOSTEP   = 1u << 2,   /* special: GXReadDrawSync returns the executed token instead of served+1 (with jump2 the flip wedges)        nostep */
    GXT_BRK_RDELAY   = 1u << 3,   /* ring: R sleeps cfg.rdelay_ms per PRESENT replay: LAG INJECTION (timing hazards are only tested with lag)    rdelay */
    GXT_BRK_RSTALL   = 1u << 4,   /* ring: R sleeps 8 s inside one record (G-X1 exit gate)                                                      rstall */
    GXT_BRK_DROPSIG  = 1u << 5,   /* ring: G drops 1 wake signal in 1000 (R's timed wait must recover)                                          dropsig */
    GXT_BRK_BP_EARLY = 1u << 6,   /* special: deliver the breakpoint at every retrace (docs/53 revision 1)                                      bp_early */
    GXT_BRK_SNAPOFF  = 1u << 7,   /* special: P2-prime / dirty-image snapshots OFF                                                              snapoff */
    GXT_BRK_AHEAD2   = 1u << 8,   /* ring: AHEAD forced to 2                                                                                    ahead2 */
    GXT_BRK_DROPDRAW = 1u << 9,   /* special: R drops every 50th GXCallDisplayList record (accounting gate)                                     dropdraw */
    GXT_BRK_NOFLUSH  = 1u << 10,  /* special: GXSetDrawSync does not publish (the draw-sync fence must end in a bounded timeout)                 noflush */
    GXT_BRK_MOVIESNAP = 1u << 11, /* special: a registered movie plane is snapshotted / pre-seeded RW like any image (the pre-fix behaviour: the HAL no longer identifies the plane)   moviesnap */
    GXT_BRK_MIPSNAP  = 1u << 12,  /* special: a MIPMAPPED image is snapshotted too (level 0 only: the authored chain and the LOD bias, keyed by the original address, are lost)       mipsnap */
    GXT_BRK_FOGSHARED = 1u << 13, /* ring: gx_fog_private is NOT set while threaded -- R reads/writes the game's gFogColor like DIRECT does (A/B instrument for finding N1; racy by design)   fogshared */
    GXT_BRK_NOFOGMIRROR = 1u << 14, /* special: G does not mirror a recorded GXSetFog colour into the game's gFogColor (the pre-fix behaviour)                                          nofogmirror */
    GXT_BRK_MEMHOG   = 1u << 15,  /* special: image snapshots are never released and their texture-cache entries never retired (the pre-fix leak; the memory gate G20 must see it)       memhog */
    GXT_BRK_SNAPNRM  = 1u << 16,  /* special, A/B INSTRUMENT: every GX_VA_NRM array outside W is shadowed too (8 KB window, re-copied at every GXSetArray): does the pixel residual come from unannounced normal buffers?   snapnrm */
    GXT_BRK_SNAPALL  = 1u << 17   /* special, A/B INSTRUMENT: every array outside W is shadowed (8 KB window, re-copied at every GXSetArray)                                                                  snapall */
} GxtBrk;

typedef struct GxtCfg {
    int      mode;           /* 0 direct, 1 threaded, 2 lockstep, 'a' arm-driven, 'h' hammer */
    int      ahead;          /* 1 normally, 0 lockstep, 2 only with GXT_BRK_AHEAD2 */
    uint32_t ring_bytes;
    int      r_core, g_core; /* -1 = none, 0..2 = pin; -2 = auto */
    int      trace, audit;
    uint32_t brk;            /* GXT_BRK_* */
    uint32_t rdelay_ms;
    int      refused;        /* non-zero: a precondition failed; the reason was printed as "[gxt] REFUSED: ..." and the process stays DIRECT */
} GxtCfg;

/* ------------------------------------------------------------------------------------------------------------------------------ counters */
/* writer in brackets.  Read cross-thread only for printing (torn reads of a 32-bit counter are impossible; of the set, benign). */
typedef struct GxtStats {
    uint32_t recs, rec_bytes, chunks, publishes, wakes;                       /* [G] records appended, bytes, VBYTES chunks, publishes, wake signals sent */
    uint32_t present_waits, present_wait_us;                                  /* [G] PRESENT back-pressure: waits that blocked, microseconds */
    uint32_t ring_full_waits, ring_full_us;                                   /* [G] ring-full waits */
    uint32_t block_waits, block_us;                                           /* [G] blocking ops (B8 copy, drains, switches) */
    uint32_t timeouts, stuck;                                                 /* [G] 2 s timeouts logged, waits that went past GXT_GIVEUP_US */
    uint32_t switches, switch_deferred, switch_drain_us;                      /* [G] mode transitions, deferred (a list was open), drain time of the last threaded->direct */
    uint32_t retire_n, retire_max, retire_alloc_waits;                        /* [G] retire list: entries now, high-water, allocation-failure drains */
    uint32_t reclaims, reclaim_freed;                                         /* [G] gxt_reclaim calls (an allocation failed), retire-list entries it freed */
    uint32_t viewer_traps, foreign_hook;                                      /* [G] viewer-only entry points reached in threaded mode (must stay 0), hooks seen from a non-baton thread (must stay 0) */
    uint32_t ahead_hist[3];                                                   /* [G] frames-ahead at each PRESENT record: 0, 1, 2+ */
    uint32_t g_cpu[4];                                                        /* [G] sceKernelGetCpuId() sampled once per frame edge */
    uint32_t r_recs, r_ops_unknown;                                           /* [R] records executed; ops no switch case knew (must stay 0) */
    uint32_t r_busy_us, r_starve_us, r_present_us;                            /* [R] wall time executing records / idle on an empty ring / inside the real present (modular uint32) */
    uint32_t r_timedwake, r_wakes;                                            /* [R] timed re-checks that found data (a lost wake-up recovered) / signals consumed */
    uint32_t r_ext_poll_dropped, r_clear_color_dropped, r_edge_calls;         /* [R] os_ext_poll / vi_set_clear_color / GXSetTevIndirect reached through a wrapper while on R */
    uint32_t r_cpu[4];                                                        /* [R] sceKernelGetCpuId() sampled once per PRESENT replay */
    uint32_t r_stack_hw;                                                      /* [R] bytes of R's stack ever used (measured at exit) */
} GxtStats;

/* ------------------------------------------------------------------------------------------------------------------------------ the shared state */
typedef struct GxtShared {
    /* ===== G-private producer state (hot; ONLY G touches it) ===== */
    uint32_t  head;            /* bytes appended so far, modular; the write offset is head & mask */
    int32_t   room;            /* bytes that may be appended before gxt_alloc_slow must run: min(distance to the ring end, free space, bytes left in this publish quantum) */
    uint8_t  *push_cur;        /* the pipe cursor.  CHUNK mode: inside the open VBYTES chunk.  LIST mode (list_open): inside the game's display-list buffer. */
    uint8_t  *push_end;        /* push_cur == push_end => no room: gxt_push_slow() runs (opens/rolls a chunk, or handles list overflow into `sink`) */
    uint8_t  *chunk_data;      /* CHUNK mode: first payload byte of the open chunk (its 8-byte header sits before it); NULL when no chunk is open */
    uint8_t  *buf;             /* ring storage */
    uint32_t  size, mask;
    uint32_t  pub_g;           /* last published head (G's copy) */
    uint32_t  tail_cache;      /* last tail G saw */
    uint8_t  *list_buf;        /* G-local display-list recording (docs/53 2.3).  OWNED by gx_rt_special.c; the pipe slow path reads these three */
    uint32_t  list_cap;
    int       list_open, list_overflow;
    uint8_t   sink[GXT_PIPE_SINK_BYTES];   /* where bytes go once a list overflowed */
    int       on;              /* [G] 1 while GX calls are recorded (THREADED).  Written only at a frame edge (gxt_frame_edge) */
    int       ahead;           /* [G] the frame-ahead bound in force */
    uint32_t  seq;             /* [G] PRESENT records issued; frame numbers start at 1 */
    uint32_t  ds_recorded;     /* [G] GXSetDrawSync records issued */
    /* ===== published cursors (cache-line separated: written by one side, read by the other) ===== */
    volatile uint32_t pub __attribute__((aligned(64)));     /* [G] head as of the last publish (store-release) */
    volatile uint32_t tail __attribute__((aligned(64)));    /* [R] bytes EXECUTED (store-release): the retire list, blocking ops and fences key on it */
    volatile uint32_t r_presented;                          /* [R] seq of the last PRESENT replayed to completion */
    volatile uint32_t r_ds_seq;                             /* [R] draw-sync records executed (special writes it, ring prints it) */
    volatile uint32_t r_ds_token;                           /* [R] the token value of the last executed GXSetDrawSync */
    volatile int      r_sleeping;                           /* [R] set before R sleeps on an empty ring (followed by a full fence, then a re-check) */
    volatile int      g_waiting;                            /* [G] set before G sleeps on sema_g */
    volatile int      r_alive, r_stop, r_parked;            /* [R] thread running / [G] asks R to stop / [R] parked after MODE_LEAVE */
    uint32_t  r_cur_end;       /* [R] byte counter just past the record being executed (what gxt_r_mark_done publishes) */
    void    (*r_pub_hook)(void);       /* [G sets, R calls] R5 (R5_SPLIT.md): non-NULL while the split is ON -- R publishes its RHI records BEFORE any tail G may wait on is published (gxt_r_mark_done, the ring loop) */
    uintptr_t r_stack_lo, r_stack_hi;   /* [R] R's stack range, set once before r_alive: gxt_on_R() is a range compare, no syscall */
    int       sema_r, sema_g;  /* kernel semaphores (SceUID) */
    int       r_thread;        /* SceUID */
    GxtCfg    cfg;
    GxtStats  st;
} GxtShared;
extern GxtShared g_gxt;        /* defined in gx_thread.c */

/* ------------------------------------------------------------------------------------------------------------------------------ memory-order helpers */
#define GXT_LOAD_ACQ(p)       __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define GXT_STORE_REL(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define GXT_FENCE_SB()        __atomic_thread_fence(__ATOMIC_SEQ_CST)   /* dmb ish: REQUIRED between "I announce I am about to sleep" and "I re-check the other side" (ARMv7 store->load) */
#define GXT_BEHIND(a, b)      ((int32_t)((a) - (b)) < 0)              /* modular a < b */
#define GXT_LIKELY(x)         __builtin_expect(!!(x), 1)
#define GXT_UNLIKELY(x)       __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------------------------------------------------------------------ PRODUCER FAST PATHS (G; inline: ~10k calls per frame go through them) */
/* The slow paths are in gx_thread.c.  CONTRACT of gxt_alloc_slow(bytes): on return g_gxt.room >= bytes, the write offset (head & mask) + bytes <= size (a PAD record
 * was written and head advanced if the record would have wrapped), and the ring has `bytes` free.  It publishes when GXT_PUBLISH_BYTES are unpublished, waits (bounded, logged) for
 * the ring to drain, and never returns on a dead ring (after GXT_GIVEUP_US it stops logging and spins in bounded 100 ms sleeps until the watchdog faults). */
void gxt_alloc_slow(uint32_t bytes);
uint8_t *gxt_push_slow(uint32_t n);          /* returns where n bytes go; never NULL.  Opens a chunk, rolls a full one, or (list_open) handles overflow into `sink`.  push_cur is NOT advanced here. */
void gxt_chunk_close(void);                  /* writes the open chunk's 8-byte header, advances head/room; no-op when none is open */

/* Append a record: op (GXT_OP_*), nwords payload words.  Returns the payload pointer (4-byte aligned).  The record is NOT visible to R until gxt_publish(). */
/* REDUNDANCY COUNTER (gx_thread.c, marker gx_thread_red.txt): judges the PREVIOUS record (its payload is complete by the next alloc). Off = one branch. */
extern int gxt_red_on;
extern const uint32_t *gxt_red_last;
void gxt_red_note(const uint32_t *payload);
/* G-SIDE REDUNDANT-STATE FILTER (src/gx_rt_filter.c, marker gx_thread_filt.txt; docs in that file's header).  The three symbols below are DEFINED in gx_thread.c (like gxt_red_on): every
 * translation unit that inlines gxt_rec_alloc must link, and the ring test links gx_thread.c alone.  The two functions are the module's; they are WEAK so a build without the module still links
 * (nothing calls them while gxt_flt_on is 0, and gxt_flt_on is only ever set by the module).
 *   gxt_flt_on      1 while the filter is armed (threaded recording AND marker on AND not perf arm 111).  Off = ONE predictable branch per record, here and in the generated wrappers.
 *   gxt_flt_cls[op] the whitelist, filled by the module.  Low 7 bits, consulted at allocation: 0 = nothing to do (a FILTER or NEUTRAL op), 1 = FORGET (forget every remembered value),
 *                   2 = CLEARVTXDESC.  GXT_FLT_CLS_F (bit 7) = a FILTER op: its generated wrapper asks gxt_flt_pre BEFORE allocating.  Ops are masked to 10 bits. */
#define GXT_FLT_OPS 1024u
#define GXT_FLT_CLS_F 0x80u
#define GXT_FLT_PRE_MAXW 20u    /* the generated wrapper's stack payload: an op whose payload is longer is never offered to the filter */
/* GXT_FLT_WEAK: weak for every REFERENCE.  gx_rt_filter.c defines GXT_FLT_DEFINE before including this header so its DEFINITIONS are strong: a definition inherits the weak attribute of an earlier
 * declaration, and on the MinGW host a weak DEFINITION becomes a COFF weak alias to the next symbol in the object (found the hard way: the test jumped to image base). */
#ifdef GXT_FLT_DEFINE
#define GXT_FLT_WEAK
#else
#define GXT_FLT_WEAK __attribute__((weak))
#endif
extern int gxt_flt_on;
extern uint8_t gxt_flt_cls[GXT_FLT_OPS];
void gxt_flt_alloc_note(uint32_t op) GXT_FLT_WEAK;                  /* G, at the allocation of a FORGET / CLEARVTXDESC op */
int  gxt_flt_pre(uint32_t op, const uint32_t *w, uint32_t nwords) GXT_FLT_WEAK;   /* G, BEFORE a FILTER op is allocated, w = its payload: 1 = redundant, record nothing */
/* The module's entry points gx_thread.c calls (declared HERE, not in gx_rt_filter.h: the ring test copies only gx_rt.h next to gx_thread.c when it builds its mutants).  gxt_flt_arm(on) arms /
 * disarms the filter (first arming allocates the tables) and forgets on every change; gxt_flt_mode_changed() forgets (a DIRECT <-> threaded transition); gxt_flt_print(frames) prints the
 * "[gxt] filt:" lines and resets the window counters. */
void gxt_flt_arm(int on) GXT_FLT_WEAK;
void gxt_flt_mode_changed(void) GXT_FLT_WEAK;
void gxt_flt_print(uint32_t frames) GXT_FLT_WEAK;
/* v3 INLINE FAST PATH (2026-10-21, docs/54 "Filter v2" -> hw_Filt4): on G every extra cache line or code location a GX call touches costs ~35 cycles, so the
 * common case is decided HERE, inline in the generated wrapper, from ONE 32-byte line: the op's descriptor, indexed by its constant op number, which also holds
 * its counters.  No call, no class-byte read, no libc copy.  The module (gx_rt_filter.c) fills the descriptors at arming, owns the generations, and takes every
 * rare path through gxt_flt_pre: an alias op's miss, a key out of range, a too-wide payload, an open or just-closed display list, the diagnostic arms. */
typedef struct GxtFltOp {
    uint32_t *base;             /* slot 0 of this op's shadow; NULL = not a FILTER op (the generated wrapper's test) */
    uint32_t  gen;              /* a slot is valid iff its stamp == gen (never 0) */
    uint16_t  stride;           /* words per slot = 2 + maxw: [0] stamp, [1] nw, [2..] payload */
    uint8_t   nk, maxw;         /* keys 0..nk-1; payload words a slot holds */
    uint8_t   km;               /* GXT_KM_*: how word 0 names the target */
    uint8_t   slow;             /* 1 = a miss goes to the module (the op has aliases: the module invalidates the other spellings) */
    uint8_t   pad_[2];
    uint32_t  seen, dropped;    /* fast-path counters (the module adds its own slow-path ones when it reports) */
    uint32_t  pad2_;
} __attribute__((aligned(32))) GxtFltOp;
enum { GXT_KM_GLOBAL = 0, GXT_KM_W0, GXT_KM_SLOT3, GXT_KM_TEXMTX, GXT_KM_VTXDESC };
extern GxtFltOp gxt_flt_ops[GXT_FLT_OPS];   /* defined in gx_thread.c (32 KB of BSS, zero = no op filtered) */
extern uint8_t  gxt_flt_fast;               /* defined in gx_thread.c; the module clears it to route EVERY call through gxt_flt_pre (a list closed, diagnostic arms) */
/* The target of a payload as a slot index 0..nk-1, or -1.  ONE definition: the module uses it too.  GX_VA_NBT (25) folds into GX_VA_NRM (10). */
static inline __attribute__((always_inline)) int gxt_flt_key(const GxtFltOp *o, const uint32_t *pl, uint32_t nw)
{
    uint32_t w;
    if (o->km == GXT_KM_GLOBAL) return 0;
    if (nw == 0u) return -1;
    w = pl[0];
    switch (o->km) {
    case GXT_KM_W0:      break;
    case GXT_KM_SLOT3:   w /= 3u; break;
    case GXT_KM_TEXMTX:  if (w >= 30u && w < 60u) w = (w - 30u) / 3u; else if (w >= 64u && w < 124u) w = 10u + (w - 64u) / 3u; else return -1; break;   /* 10 + 20 slots */
    case GXT_KM_VTXDESC: if (w == 25u) w = 10u; break;
    default:             return -1;
    }
    return w < o->nk ? (int) w : -1;
}
/* The generated R-row wrapper of every op tests this first: one load + one branch when the filter is off; the descriptor line it loads is the one the check uses. */
#define GXT_FLT_WANTS(op) (GXT_UNLIKELY(gxt_flt_on) && gxt_flt_ops[(op) & (GXT_FLT_OPS - 1u)].base != NULL)
/* 1 = the HAL already holds exactly this state: record nothing. */
static inline __attribute__((always_inline)) int gxt_flt_check(uint32_t op, const uint32_t *w, uint32_t nw)
{
    GxtFltOp *o = &gxt_flt_ops[op & (GXT_FLT_OPS - 1u)];
    uint32_t *s, k;
    int key;
    if (GXT_UNLIKELY(!gxt_flt_fast) || GXT_UNLIKELY(g_gxt.list_open)) return gxt_flt_pre(op, w, nw);
    key = gxt_flt_key(o, w, nw);
    if (GXT_UNLIKELY(key < 0) || GXT_UNLIKELY(nw > o->maxw)) return gxt_flt_pre(op, w, nw);
    s = o->base + (uint32_t) key * o->stride;
    if (s[0] == o->gen && s[1] == nw) {                 /* EXACT, word for word */
        for (k = 0; k < nw && s[2u + k] == w[k]; ++k) {}
        if (k == nw) { ++o->seen; ++o->dropped; return 1; }
    }
    if (o->slow) return gxt_flt_pre(op, w, nw);         /* a different value under an op with aliases: the module invalidates the other spellings and stores */
    ++o->seen;
    s[0] = o->gen; s[1] = nw;
    for (k = 0; k < nw; ++k) s[2u + k] = w[k];
    return 0;
}
/* R2 STATE RECORDS (2026-10-23, host/engine/R2_STATE.md; module src/gx_rt_state.c, the HAL struct in port/gx_hal_state.h).  While R2 is active a GX state setter runs the HAL body on G's own
 * instance of the state struct and marks dirty groups INSTEAD of recording; the dirty groups travel as ONE GXT_OP_STATE record before G appends anything else (INV-1: a record header or a
 * VBYTES chunk is only opened with the dirty set empty; INV-2: a setter closes an open chunk first).  The symbols below are DEFINED in gx_thread.c (the ring test links it alone) except the
 * module's own, which are WEAK so a build without the module still links; gx_rt_state.c defines GXS_DEFINE before including this header (a weak DEFINITION on MinGW becomes an alias).
 *   gxt_state_on     1 while R2 is active (G-written, plain: only G's wrappers read it).  Written only at a frame edge / mode switch, with R drained or parked.
 *   gxt_state_dirty  points at the dirty-any word at the START of G's HAL instance while R2 is active, else at an always-zero word: the producer fast path below is one load, one load, one branch.
 *   gxt_state_calls  S setter calls made through the G variants (the "[gxt] state:" line's s_calls/f). */
#ifdef GXS_DEFINE
#define GXS_WEAK
#else
#define GXS_WEAK __attribute__((weak))
#endif
extern int gxt_state_on;
extern unsigned gxt_state_calls;
extern const uint32_t gxt_state_zero;
extern const uint32_t *gxt_state_dirty;
void gxs_flush(void) GXS_WEAK;                                  /* G: emit one STATE record of every dirty group and clear the dirty set (no re-entry into itself) */
void gxs_begin(void) GXS_WEAK;                                  /* G, R parked or drained: gx_hal_g := gx_hal, dirty cleared, gxt_state_dirty bound, gxt_state_on := 1 */
void gxs_end(void) GXS_WEAK;                                    /* G: flush, unbind, gxt_state_on := 0 */
int  gxs_r_apply(const uint32_t *p, uint32_t nw) GXS_WEAK;      /* R: validate and apply a STATE record; 0 ok, -1 refused (counted state_bad) */
void gxs_config(int audit_every) GXS_WEAK;                      /* G, at threaded entry: the whole-struct audit period (gx_thread_saudit.txt; 0 = off) */
void gxs_print(uint32_t frames) GXS_WEAK;                       /* G: the "[gxt] state:" line for a report window of `frames` frames, then reset the window counters */
static inline __attribute__((always_inline)) uint32_t *gxt_rec_alloc_impl(uint32_t op, uint32_t nwords, int hook)
{
    GxtShared *g = &g_gxt;
    if (GXT_UNLIKELY(*gxt_state_dirty != 0u)) gxs_flush();       /* INV-1: the dirty groups go out BEFORE this record (the flush's own allocation sees a clean set) */
    if (GXT_UNLIKELY(gxt_red_on) && gxt_red_last) gxt_red_note(gxt_red_last);
    if (hook && GXT_UNLIKELY(gxt_flt_on) && GXT_UNLIKELY((gxt_flt_cls[op & (GXT_FLT_OPS - 1u)] & 0x7Fu) != 0)) gxt_flt_alloc_note(op);
    if (GXT_UNLIKELY(g->chunk_data != NULL)) gxt_chunk_close();
    const uint32_t bytes = (nwords + 1u) * 4u;
    if (GXT_UNLIKELY((int32_t)bytes > g->room)) gxt_alloc_slow(bytes);
    uint32_t *p = (uint32_t *)(g->buf + (g->head & g->mask));
    p[0] = op | (nwords << 10);
    g->head += bytes; g->room -= (int32_t)bytes;
    if (GXT_UNLIKELY(gxt_red_on)) gxt_red_last = p + 1;
    return p + 1;
}
static inline uint32_t *gxt_rec_alloc(uint32_t op, uint32_t nwords) { return gxt_rec_alloc_impl(op, nwords, 1); }
/* A FILTER op's allocation after gxt_flt_check said "record it": a FILTER op has no allocation-time class action, so the class byte is not read (one line fewer). */
static inline uint32_t *gxt_rec_alloc_flt(uint32_t op, uint32_t nwords) { return gxt_rec_alloc_impl(op, nwords, 0); }

/* Where do n (1, 2 or 4) pipe bytes go?  Hot path: one subtract, one compare. */
static inline uint8_t *gxt_push_ptr(uint32_t n)
{
    GxtShared *g = &g_gxt;
    uint8_t *p = g->push_cur;
    if (GXT_UNLIKELY((uint32_t)(g->push_end - p) < n)) p = gxt_push_slow(n);
    g->push_cur = p + n;
    return p;
}
static inline void gxt_push_u8(uint32_t v)  { *gxt_push_ptr(1) = (uint8_t)v; }
static inline void gxt_push_u16(uint32_t v) { uint8_t *p = gxt_push_ptr(2); p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void gxt_push_u32(uint32_t v) { uint8_t *p = gxt_push_ptr(4); p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void gxt_push_f32(float f)    { uint32_t u; memcpy(&u, &f, 4); gxt_push_u32(u); }

/* ------------------------------------------------------------------------------------------------------------------------------ API: ring module (src/gx_thread.c) */
/* ---- lifecycle, called from gx_rec.c / vita_tool_main.c ---- */
void gxt_boot(void);                                   /* once; idempotent.  Reads the markers, runs the preconditions (gxts_refuse_reason + backend + markers), prints "[gxt] boot: ..." (the EFFECTIVE config) */
int  gxt_frame_edge(int arm, int present_no);          /* G, once per present, from gx_rec_frame_edge, BEFORE the frame's GX work.  Performs mode transitions (never with a list open or a chunk open), retires
                                                          frees, samples cpu ids, prints the 60-present "[gxt] presents P..Q" line.  Returns 1 iff THREADED recording is on afterwards. */
int  gxt_present(struct RhiInstance *rhi, struct RhiSwapchain *sc);   /* G: the body of __wrap_rhi_present's real present.  DIRECT: __real_rhi_present.  THREADED: PRESENT record + publish + the
                                                          frame-ahead wait ((int32_t)(r_presented - (seq - ahead)) >= 0, bounded).  Returns 1 (queued/ok). */
void gxt_quit(const char *why);                        /* vt_stop, the post-VT_TOOL epilogue and __wrap_exit call it FIRST (before every shutdown).  Idempotent.  QUIT record -> bounded join (2 s x 3).  If R does not stop:
                                                          logs "[gxt] R did not stop (last op N)" and sets gxt_r_unsafe so the caller skips the shutdowns that assume an idle backend owner. */
extern volatile int gxt_r_unsafe;                      /* 1 after gxt_quit failed to stop R: callers must NOT run rhi_gxm_shutdown / any backend teardown */
void gxt_stall_report(void);                           /* watchdog: ring head/pub/tail, R's last op, flags, in-backend flag, per-thread counters */
static inline int gxt_active(void) { return g_gxt.on; }                   /* G side: are GX calls being recorded? */
int  gxt_is_active(void);                              /* non-inline gxt_active() for code that links without gx_thread.o only through a weak reference (vita_tool_main.c) */
int  gxt_on_R(void);                                   /* any thread: 1 iff the caller is R (stack-range compare against r_stack_lo/hi; 0 when R does not exist) */

/* ---- G-side blocking helpers (bounded, logged; `why` names the reason in the "[gxt] G waits: <why>" line) ---- */
void     gxt_publish(void);                            /* close any open chunk, store-release pub = head, full fence, wake R if r_sleeping.  Cheap when nothing is unpublished. */
uint32_t gxt_mark(void);                               /* publish, then return head: "everything recorded so far".  Pair with gxt_wait_tail. */
int      gxt_wait_tail(uint32_t mark, const char *why);/* wait until (int32_t)(tail - mark) >= 0.  0 ok, -1 the wait went past GXT_GIVEUP_US (logged; the caller proceeds degraded) */
int      gxt_wait_u32(volatile uint32_t *p, uint32_t target, const char *why);   /* wait until (int32_t)(*p - target) >= 0 (r_presented, r_ds_seq ...) */
int      gxt_drain(const char *why);                   /* gxt_wait_tail(gxt_mark(), why): R has executed everything recorded */
/* ---- frees: the retire list (mm_shim.c's mm_free_now calls this instead of free) ---- */
void     gxt_free(void *p);                            /* DIRECT: free(p).  THREADED: stamped with head; freed at a frame edge / on allocation pressure once tail has passed the stamp.  Order preserved. */
int      gxt_reclaim(const char *why);                         /* G, on an allocation failure: publish, wait for R to execute everything recorded, free every retired block it has passed.  Returns the number freed (0 = nothing to free: do not retry).  DIRECT / on R: 0. */
/* ---- R side ---- */
void     gxt_r_wake_g(void);                           /* signal sema_g iff g_waiting (full fence first) */
static inline void gxt_r_mark_done(void)               /* a record that G may be waiting on (draw sync, present, sync, copy) calls this FROM INSIDE its R handler: tail := end of the current record */
{
    if (GXT_UNLIKELY(g_gxt.r_pub_hook != NULL)) g_gxt.r_pub_hook();
    GXT_STORE_REL(&g_gxt.tail, g_gxt.r_cur_end);
    gxt_r_wake_g();
}
/* ---- stats helpers ---- */
void     gxt_viewer_trap(const char *name);            /* a viewer-only entry point was reached in threaded mode: counts, logs the first 8 */
uint32_t gxt_now_us(void);                             /* sceKernelGetProcessTimeWide() truncated to 32 bits (host: a monotonic clock) */

/* ------------------------------------------------------------------------------------------------------------------------------ API: gen module (src/gx_rec.c + generated gx_rt_gen.inc) */
void gx_rec_set_threaded(int on);                      /* sets the wrappers' mode to 3 (threaded: G wrappers record) / back to the arm-driven T0-a mode; called ONLY by gxt_frame_edge at a frame edge */
/* The R dispatch.  op is a GXT_OP_* of an entry point (table row or [extra] row) or GXT_OP_VBYTES / GXT_OP_ABANDON; p points at the payload (4-byte aligned, valid until the call returns);
 * nw = payload words.  Returns 0, or -1 when no case knows op (the ring counts r_ops_unknown and logs).  Custom rows call gxts_R_<name>(p, nw).  PAD / PRESENT / SYNC / QUIT / MODE_LEAVE /
 * HAMMER never reach it: the ring loop handles them. */
int  gxt_r_exec(uint32_t op, const uint32_t *p, uint32_t nw);

/* ------------------------------------------------------------------------------------------------------------------------------ API: special module (src/gx_rt_special.c) */
int  gxts_refuse_reason(char *buf, int cap);           /* G, at boot: 0 = threaded mode may start; else non-zero and buf holds the reason.  Checks: efbZMode == 0 (peekZ is far-plane on GXM), gx_bp.txt != 0, mm guard off,
                                                          rt_stats off, no census / perf arm that reads R state from G, ... (RT_THREAD.md 5.4) */
void gxts_threaded_begin(void);                        /* G, at direct -> threaded, R parked, nothing in flight: initialise every G shadow FROM THE HAL by its real getter (scissor, token served/executed, ...) */
void gxts_threaded_end(void);                          /* G, at threaded -> direct, AFTER the drain: drop shadows, free snapshot/shadow memory (through gxt_free), clear the written set */
void gxts_frame_edge_g(uint32_t seq);                  /* G, every threaded frame edge: expire written-set entries, flip the shadow parity, drain-check lifetimes */
int  gxts_report(char *buf, int cap);                  /* G, per window: append "key=value " tokens (snap_bytes, shadow_bytes, wset, bp_gates, ds_steps, b8_blocks, audit_checked, audit_bad, ...) and return how many bytes were written */
int  gxts_audit_bad(void);                             /* G-L3 mismatches so far (record-time hash != replay-time hash); the ring prints it and the checker gates on it */

/* the hot special paths the ring calls on R / G */
int  gxts_r_filter(uint32_t op);                      /* R, before EVERY dispatch while g_gxt.cfg.brk != 0 (gxt_r_exec calls it only then): 1 = skip this record.  Break arm `dropdraw` (drops every 50th GXT_OP_GXCallDisplayList) only; otherwise 0 */
void gxts_r_present_executed(uint32_t seq);            /* R, after the real present of frame `seq` returned and BEFORE r_presented is published: special-owned per-frame R work (audit window close) */

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_H */
