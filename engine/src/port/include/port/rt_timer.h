/* rt_timer.h -- nesting-aware frame-time accounting for the Stairfax port (Task 3, 2026-10-12). Header-only; the device (gx_draw.c,
 * vi_shim.c, rhi_vita*.c, vita_tool_main.c) and the host test (stairfax-vita/host/tests/rt_timer_test.c) compile the SAME code.
 *
 * Question it answers: of a frame's main-thread time, how much is the GAME's own logic, how much is the GX SHIM (gx_draw.c), how
 * much is the BACKEND (rhi_vita*), and how much is WAITING for the retrace (VIWaitForRetrace: pump, callbacks, present)? The
 * [win] line's "REST" lumps the first and last together.
 *
 * Method: a stack of categories. Time is charged to the category on TOP of the stack at every transition, so an inclusive
 * scope (backend called from the shim) takes its time OUT of the shim and nothing is counted twice: sum(acc) == frame wall,
 * exactly, by construction. A sum that disagrees with the wall means an unbalanced enter/leave, and `underflow` / `overflow`
 * say so. Time with an empty stack is RT_GAME.
 *
 * Cost: OFF (rt_stats.txt absent) every hook is one load and one untaken branch. ON, one frame in RT_SAMPLE (8) is measured, a
 * hook costs one clock read (+ thread id) and there are ~2 hooks per GX / backend call: ~600 a frame in ThornTail, well under
 * 0.3 ms averaged over the 8 frames.
 *
 * Threads: only the thread that opened the frame (`owner`) is accounted; the shader writer, audio and movie threads that call the
 * same backend functions are ignored by the owner check, so they cannot corrupt the stack. */
#ifndef RT_TIMER_H
#define RT_TIMER_H
#include <string.h>
#include <stdint.h>

enum { RT_GAME = 0, RT_SHIM, RT_BACKEND, RT_WAIT, RT_NCAT };
/* inclusive sub-timer slots (they overlap the categories and each other: a slot's time includes the backend calls inside it) */
enum { RT_S_ENTRY_DL = 0, RT_S_ENTRY_IMM, RT_S_ENTRY_TEX, RT_S_ENTRY_COPY,   /* the four timed shim entry points */
       RT_S_DLK_ALL, RT_S_FALLBACK,                                          /* GXCallDisplayList: the cache attempt / the per-primitive loop after it refused */
       RT_S_TEVBUILD, RT_S_FOGTEX, RT_S_STATE,                               /* inside a cached call: gx_tev_build (+canon), fog + texture-map checks, alpha/depth/matrix state to the backend */
       RT_S_MTX, RT_S_SETTERS,                                               /* matrix loads / projection; the setters in gx_draw.c that reach state or the backend */
       RT_NSUB };
#define RT_NEAR 0x40000u
#define RT_STACK  24
#define RT_SAMPLE 16      /* one frame in this many is measured */
typedef unsigned long long rt_us;
typedef struct {
    int on;                      /* rt_stats.txt starts with 1 */
    volatile int act;            /* inside a sampled frame */
    int depth, over;
    uintptr_t owner;             /* a stack address of the owning thread: "same thread" = within RT_NEAR bytes (no syscall per hook) */
    unsigned char stack[RT_STACK];
    rt_us last, t0;
    rt_us acc[RT_NCAT];          /* exclusive microseconds per category, summed over the sampled frames of the window */
    rt_us wall;                  /* summed frame wall time of the sampled frames */
    unsigned frames, edges, underflow, overflow, foreign;
    /* inclusive sub-timers (informational: they overlap the categories), sampled frames only */
    rt_us vsUs, meshUs, drawUs, lookupUs;
    unsigned vsN, meshN, drawN, lookupN;
    unsigned syncs;              /* glReadPixels syncs (every frame, not only sampled ones) */
    unsigned scopes, clkNs;      /* category scopes entered in the window; measured cost of one clock read (ns, at enable) */
    rt_us sub[RT_NSUB]; unsigned subN[RT_NSUB];   /* inclusive sub-timers by slot (RT_S_*), sampled frames only */
} RtState;

#ifndef RT_NOW
#  ifdef __vita__
#    include <psp2/kernel/processmgr.h>
#    define RT_NOW() sceKernelGetProcessTimeWide()
#  else
#    define RT_NOW() 0ull
#  endif
#endif
#ifndef RT_ME
#  ifdef __vita__
#    include <psp2/kernel/threadmgr.h>
#    define RT_ME() ((uintptr_t) __builtin_frame_address(0))   /* a stack address: no syscall per hook */
#  else
#    define RT_ME() ((uintptr_t) 0)
#  endif
#endif

static inline void rt_charge(RtState *s, rt_us now) {
    const int cat = s->depth > 0 ? s->stack[s->depth - 1] : RT_GAME;
    s->acc[cat] += now - s->last; s->last = now;
}
static inline int rt_mine(const RtState *s, uintptr_t me) { return (me > s->owner ? me - s->owner : s->owner - me) < RT_NEAR; }
static inline void rt_enter(RtState *s, int cat, rt_us now, uintptr_t me) {
    if (!s->act) return;
    if (!rt_mine(s, me)) { ++s->foreign; return; }
    ++s->scopes;
#ifndef RT_BREAK_DOUBLECOUNT   /* negative control: the outer scope is not charged at the transition, so nested time lands in BOTH */
    rt_charge(s, now);
#endif
    if (s->depth < RT_STACK) s->stack[s->depth++] = (unsigned char) cat; else { ++s->over; ++s->overflow; }
}
static inline void rt_leave(RtState *s, rt_us now, uintptr_t me) {
    if (!s->act || !rt_mine(s, me)) return;
    if (s->over) { --s->over; return; }
    if (s->depth <= 0) { ++s->underflow; return; }
    rt_charge(s, now); --s->depth;
}
/* A frame boundary (once per VIWaitForRetrace, before its WAIT scope opens). Closes the open sampled frame, then opens the next
 * one if it is a sampled frame. */
static inline void rt_frame_edge(RtState *s, rt_us now, uintptr_t me) {
    ++s->edges;
    if (s->act) { rt_charge(s, now); s->wall += now - s->t0; ++s->frames; s->act = 0; }
    if (((s->edges) % RT_SAMPLE) == 0) { s->depth = s->over = 0; s->last = s->t0 = now; s->owner = me; s->act = 1; }
}
static inline rt_us rt_sum(const RtState *s) { rt_us t = 0; for (int i = 0; i < RT_NCAT; ++i) t += s->acc[i]; return t; }
static inline void rt_window_reset(RtState *s) {   /* after a report: keep on / act / the stack, drop the sums */
    memset(s->acc, 0, sizeof s->acc); s->wall = 0; s->frames = 0; s->underflow = s->overflow = s->foreign = 0;
    s->vsUs = s->meshUs = s->drawUs = s->lookupUs = 0; s->vsN = s->meshN = s->drawN = s->lookupN = 0; s->syncs = 0; s->scopes = 0;
    memset(s->sub, 0, sizeof s->sub); memset(s->subN, 0, sizeof s->subN);
}

extern RtState gRt;   /* defined in hal/gx_draw.c */
#define RT_ENTER(cat) do { if (gRt.act) rt_enter(&gRt, (cat), RT_NOW(), RT_ME()); } while (0)
#define RT_LEAVE()    do { if (gRt.act) rt_leave(&gRt, RT_NOW(), RT_ME()); } while (0)
#define RT_EDGE()     do { if (gRt.on) rt_frame_edge(&gRt, RT_NOW(), RT_ME()); } while (0)
/* a scope that closes on every return path (gcc cleanup): one line at the top of a backend entry point */
typedef struct { char armed; } RtScope;
static inline void rt_scope_end(RtScope *sc) { if (sc->armed && gRt.act) rt_leave(&gRt, RT_NOW(), RT_ME()); }
#define RT_SCOPE(cat) RtScope _rt_sc __attribute__((cleanup(rt_scope_end), unused)) = { (char) (gRt.act ? (rt_enter(&gRt, (cat), RT_NOW(), RT_ME()), 1) : 0) }
/* inclusive sub-timers */
#define RT_COUNT(slot)  do { if (gRt.act) ++gRt.subN[slot]; } while (0)   /* count only: no clock read (a read costs 700 ns on the Vita, more than the setters it would time) */
#define RT_T0()          (gRt.act ? RT_NOW() : 0ull)
/* slot sub-timers: RT_SUBSL(slot, t0) closes one started with RT_T0(); RT_SUBS(slot) is a scope that closes on every return path */
#define RT_SUBSL(slot, t0) ((t0) ? (void) (gRt.sub[slot] += RT_NOW() - (t0), ++gRt.subN[slot]) : (void) 0)
typedef struct { rt_us t0; int slot; } RtSub;
static inline void rt_sub_end(RtSub *x) { if (x->t0) { gRt.sub[x->slot] += RT_NOW() - x->t0; ++gRt.subN[x->slot]; } }
#define RT_SUBS(slot) RtSub _rt_sb __attribute__((cleanup(rt_sub_end), unused)) = { RT_T0(), (slot) }
#define RT_SUB(field, n, t0) ((t0) ? (void) (gRt.field += RT_NOW() - (t0), ++gRt.n) : (void) 0)   /* an expression: usable inside a comma chain */
#endif
