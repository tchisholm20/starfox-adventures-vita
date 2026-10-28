/* c3_cuts.h -- GXAOT release track C3: the per-draw cuts, arms 100-107 and 109 (default OFF; arm 0 = today's behaviour -- the always-on additions, in EVERY arm, are: 1-in-16 sampled micro-timers, a few
 * counters, and (rev 2) the immediate batch census on the 1-in-8 primitives gx_draw.c already samples; none changes what is drawn).
 *
 * WHAT THIS IS. Question C3: normal play needs ~7 ms off to lock 30 fps; the GPU-path display-list draw (58.0 us a call, 9.6 ms / ~165) and the immediate primitive
 * (24.1 us, 5.4 ms / ~225; the brief's "~30 us" divided by the wrong count) -- where do they go in OUR code (RANKED_CUTS.md s2, s4b). docs/54 rev 2 s4: arms 100-109 are the RELEASE range (108 is C2's). This header is the
 * game-neutral, GL-free half: arm decoding, the 128-bit key hash, the pair-program memo (arm 100), the frame-pacing lock (arm 103), the
 * counters. The GL-facing half (the uniform shadow, arm 101, the [c3] report line, the exported entry points) is c3_cuts.inc, included by
 * P/src/rhi_vita_tev.inc. Arm 102 lives in U gx_draw.c (patches/C3-perdraw/003) and reports through the gx_c3_* counters.
 *
 * ARMS (perf_ab.txt "arms ..."; gStfxAb, the same switch perf_ab.py already judges):
 *   100  lookup   gxp_program: a 1024-entry memo (128-bit hash of the RAW VsKey+TevKey -> program slot) in front of td_get + the table probe
 *   101  uniform  drawTevGx: per-program shadow of the uniform GROUPS that rarely change (proj, TEV regs, konst, mat/amb, lights, tm/pt, aref);
 *                 an unchanged group is not re-sent. uPos/uNrm/uTMX (per object) are always sent
 *   102  vsbuild  gx_vs_build: the O(distinct vertices) record scans (posOk/normal/NBT counts, matrix-slot range) done once per record
 *   103  pace     present every 2nd vblank (vitaGL's vsync_interval = 2): the even-30 lock
 *   104  = 100+101+102        105 = 104 + 103
 *   106  toff     the ALWAYS-ON perf timers off. Every GX entry point, tev_flush and the draw paths read sceKernelGetProcessTimeWide() (a syscall, 673 ns a read, run 277
 *                "x 673 ns/read") for the [win] columns: 13 reads per GPU-path display-list call, 3.6 per immediate primitive (+4 per flush), 2 per texture load ->
 *                ~3,560 reads a frame in normal play = 2.4 ms [E] (derive_split.py section D). Arm 106 makes every one of them read 0 (U gx_draw.c GXT_NOW; P drawTevGx, tev_flush and
 *                rhi_vita_drawTev through c3_clk -- rev 2 added the last two), so the
 *                [win] dl/imm/tex/efb columns read 0 in that arm (frame CPU and wall are unaffected: they are timed once a frame). Judge it by frame CPU only.
 *   107  = 104 + 106          109 = 105 + 106 (the release candidate: all four cuts, the lock, and no instrument)
 * 100-109 is the RELEASE range (docs/54 s4). ARM 108 IS C2's (gxm_stall, patches/C2-scenestall/060): C3 uses 100-107 and 109 and c3_arm_mask(108) is 0. No new arm reuses gStfxPerfArm.
 *
 * LOCAL MICRO-TIMERS (all arms, 1 call in 16): lookup us/call, uniform-block us/call, gx_vs_build us/call. A cut's own effect is read from
 * these (a 3-10 us/draw change is invisible in a 5 ms wall floor but obvious in a per-call mean of ~600 samples a window).
 *
 * REPAIR ROUND (review of 2026-10-19): the 29.6 us "before the backend" of a GPU-path draw is NOT resolved by any existing counter (RANKED_CUTS.md s2): it
 * contains the flush of whatever immediate batch is open (drawTevGx runs vita_flush() before its d0 stamp). Four always-on instruments (every arm, the control
 * included -- a baseline must run what the arms run) close that, and the immediate path's split:
 *   gf_*   drawTevGx's vita_flush(): calls (exact), calls that flushed a non-empty TEV batch (exact: rhi_vita_tev_batches delta), us/call (1 in 16)
 *   gp_*   gxp_program: calls, and those answered by its one-key memo (s_lpi): the hit rate arm 100 has to beat
 *   ic_*   the immediate path's batch census, on the prims gx_draw.c already samples (rhi_vita_tev_timed, 1 in 8): why a primitive did NOT join the open batch
 *          (program / alpha mode / depth / textures / TevUniforms / mvp), each reason counted alone and "only this one differs": the flush count
 *          (61 a frame, 1.575 ms, run_t0b.txt:22561) is the largest single immediate-path item and nothing printed says what breaks the batches
 *   pclk   the P-side clock reads that really happen (c3_clk): arm 106 now also covers tev_flush and rhi_vita_drawTev, so its proof is U's gx_c3_clk AND this
 */
#ifndef C3_CUTS_H
#define C3_CUTS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifndef C3_NOW
#define C3_NOW() 0ull                 /* the includer maps this to sceKernelGetProcessTimeWide() (us) */
#endif
#ifndef C3_EXT_ATTR
#define C3_EXT_ATTR __attribute__((weak))   /* the device: gStfxAb / vsync_interval / gx_c3_* come from other translation units and a viewer build may lack them; the host test defines this empty (a mingw link does not null weak references) */
#endif
#ifndef C3_GXP_MAX
#define C3_GXP_MAX 1024               /* = rhi_vita_tev.inc GXP_MAX */
#endif

enum { C3_B_LOOKUP = 1u, C3_B_UNI = 2u, C3_B_VSB = 4u, C3_B_PACE = 8u, C3_B_TOFF = 16u };
static inline unsigned c3_arm_mask(int arm)
{
    switch (arm) {
    case 100: return C3_B_LOOKUP;
    case 101: return C3_B_UNI;
    case 102: return C3_B_VSB;
    case 103: return C3_B_PACE;
    case 104: return C3_B_LOOKUP | C3_B_UNI | C3_B_VSB;
    case 105: return C3_B_LOOKUP | C3_B_UNI | C3_B_VSB | C3_B_PACE;
    case 106: return C3_B_TOFF;                                                          /* the always-on perf timers off (ARM 106, below) */
    case 107: return C3_B_LOOKUP | C3_B_UNI | C3_B_VSB | C3_B_TOFF;                       /* 104 + timers off */
    case 109: return C3_B_LOOKUP | C3_B_UNI | C3_B_VSB | C3_B_PACE | C3_B_TOFF;          /* 105 + timers off: the release candidate */
    default:  return 0u;
    }
}
extern int gStfxAb C3_EXT_ATTR;   /* vita_tool_main.c; weak so a viewer build that does not link it reads 0 */
static inline unsigned c3_cuts(void) { return &gStfxAb ? c3_arm_mask(gStfxAb) : 0u; }
#define C3_ON(bit) ((c3_cuts() & (bit)) != 0u)
/* arm 106: the clock read the always-on timers in rhi_vita_tev.inc's tev_flush, rhi_vita_drawTev and drawTevGx use (patch 001); U gx_draw.c's GXT_NOW does the same through
 * gx_c3_toff (patch 003). Real reads are counted in U (gx_c3_clk) and here (c3_st.pclk), so the [c3] line can prove the instrument was off (reads/fr ~ 0) or on. */
extern int gx_c3_toff C3_EXT_ATTR;                    /* U gx_draw.c: 1 = GXT_NOW() returns 0 without the syscall */

/* ---- counters (cumulative; the report prints window deltas) --------------------------------------------------------------------------- */
typedef struct {
    unsigned pm_find, pm_hit, pm_vfail, pm_put;                  /* arm 100: memo probes, hits, hits refused by the slot check, stores */
    unsigned un_draws, un_groups, un_skipped, un_calls_saved;    /* arm 101: draws through the shadow path, groups examined, groups not re-sent, GL calls not made */
    unsigned long long lk_us, uni_us; unsigned lk_n, uni_n;      /* sampled 1 call in 16, ALL arms */
    unsigned pace_sets;                                           /* times vsync_interval was changed by the lock */
    unsigned ft[5]; unsigned long long ft_dev33; unsigned ft_n33; /* frame-time buckets 16.7 / 33.3 / 50 / 66.7 / 83.3+ ms and the spread inside the 33.3 bucket */
    unsigned long long pclk;                                      /* P-side clock reads that really happened (c3_clk): 0 while arm 106's switch is on */
    unsigned gf_calls, gf_flushed, gf_n, gf_nf; unsigned long long gf_us, gf_usf;   /* drawTevGx's vita_flush(): calls, calls that flushed a TEV batch (exact); sampled 1 in 16: all, flushed only */
    unsigned gp_calls, gp_slhit;                                  /* gxp_program: calls, answered by the one-key memo */
    unsigned ic_n, ic_nobatch, ic_same, ic_diff[6], ic_only[6];   /* immediate batch census (sampled by gx_draw's 1-in-8): prims seen, no open batch, joined the batch, reasons (alone / only) */
} C3Stats;
static C3Stats c3_st;
static unsigned c3_sctr[3];
#ifdef C3_BREAK_NOTOFF   /* host negative control: the clock is read in every arm */
static inline unsigned long long c3_clk(void) { ++c3_st.pclk; return C3_NOW(); }
#else
static inline unsigned long long c3_clk(void) { if (C3_ON(C3_B_TOFF)) return 0ull; ++c3_st.pclk; return C3_NOW(); }
#endif
static inline int c3_sample(int which) { return (++c3_sctr[which] & 15u) == 0u; }
static inline unsigned long long c3_t0(int which) { return c3_sample(which) ? C3_NOW() : 0ull; }
static inline void c3_t1(unsigned long long t0, unsigned long long *us, unsigned *n) { if (t0) { *us += C3_NOW() - t0; ++*n; } }
/* drawTevGx's vita_flush(): `flushed` = how many TEV batches the call really flushed (rhi_vita_tev_batches after - before; 0 = nothing was open) */
static inline void c3_gf(unsigned long long t0, unsigned flushed)
{
#ifdef C3_BREAK_GFALL   /* host negative control: every call is counted as having flushed a batch */
    ++c3_st.gf_calls; ++c3_st.gf_flushed;
#else
    ++c3_st.gf_calls; if (flushed) ++c3_st.gf_flushed;
#endif
    if (t0) { const unsigned long long us = C3_NOW() - t0; c3_st.gf_us += us; ++c3_st.gf_n; if (flushed) { c3_st.gf_usf += us; ++c3_st.gf_nf; } }
}
/* the immediate path's batch census: one sampled primitive. open = a TEV batch is open; d[i] = reason i differs from the open batch:
 * 0 program, 1 alpha mode, 2 depth state, 3 textures, 4 TevUniforms, 5 mvp (rhi_vita_drawTev's `same` is the AND of all six equal). */
static inline void c3_imm_census(int open, int d0, int d1, int d2, int d3, int d4, int d5)
{
    ++c3_st.ic_n;
    if (!open) { ++c3_st.ic_nobatch; return; }
    const int d[6] = { d0 != 0, d1 != 0, d2 != 0, d3 != 0, d4 != 0, d5 != 0 };
    int sum = 0, who = -1;
    for (int i = 0; i < 6; ++i) { if (d[i]) { ++c3_st.ic_diff[i]; ++sum; who = i; } }
    if (!sum) { ++c3_st.ic_same; return; }
#ifndef C3_BREAK_NOONLY   /* host negative control: "only this one differs" is counted for every differing reason */
    if (sum == 1) ++c3_st.ic_only[who];
#else
    for (int i = 0; i < 6; ++i) if (d[i]) ++c3_st.ic_only[i];
#endif
}

/* ---- the key hash: 128 bits, four independent 32-bit lanes (16 bytes a step), as gx_draw.c dlkHash (run 164) and for the same reason:
 * the legacy td_raw_hash / tev_vs_key_hash are ONE dependent byte chain over 517 + 88 bytes. A false hit needs two distinct raw keys with equal
 * 128-bit hashes; the memo's slot check (generation + ok) is on top of that, not instead of it. -DC3_BREAK_WEAKHASH makes the hash constant. */
static inline uint32_t c3_rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static inline void c3_lanes(const void *p, size_t n, uint32_t l[4])
{
    const unsigned char *b = (const unsigned char *) p;
    for (; n >= 16; n -= 16, b += 16) {
        uint32_t w[4]; memcpy(w, b, 16);
        l[0] = (l[0] ^ w[0]) * 0x9E3779B1u; l[0] = c3_rotl(l[0], 13);
        l[1] = (l[1] ^ w[1]) * 0x85EBCA6Bu; l[1] = c3_rotl(l[1], 11);
        l[2] = (l[2] ^ w[2]) * 0xC2B2AE35u; l[2] = c3_rotl(l[2], 15);
        l[3] = (l[3] ^ w[3]) * 0x27D4EB2Fu; l[3] = c3_rotl(l[3], 9);
    }
    if (n) {
        uint32_t w[4] = { 0, 0, 0, 0 }; memcpy(w, b, n);
        for (int i = 0; i < 4; ++i) { l[i] = (l[i] ^ w[i]) * (i & 1 ? 0x85EBCA6Bu : 0x9E3779B1u); l[i] = c3_rotl(l[i], 7 + 2 * i); }
    }
}
static inline uint64_t c3_mix64(uint64_t x) { x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33; return x; }
static inline void c3_hash2(const void *a, size_t na, const void *b, size_t nb, uint64_t out[2])
{
#ifdef C3_BREAK_WEAKHASH
    (void) a; (void) na; (void) b; (void) nb; out[0] = out[1] = 0x1234567u;
#else
#ifdef C3_BREAK_NOTK   /* host negative control: the TevKey is not hashed, so two pairs that differ only in it collide */
    nb = 0;
#endif
    uint32_t l[4] = { 0x243F6A88u ^ (uint32_t) na, 0x85A308D3u ^ (uint32_t) nb, 0x13198A2Eu, 0x03707344u };
    c3_lanes(a, na, l); c3_lanes(b, nb, l);
    out[0] = c3_mix64(((uint64_t) l[0] << 32 | l[1]) ^ c3_mix64((uint64_t) l[2] << 32 | l[3]));
    out[1] = c3_mix64(((uint64_t) l[3] << 32 | l[2]) + 0x9E3779B97F4A7C15ull * out[0]);
#endif
}

/* ---- arm 100: the pair-program memo ---------------------------------------------------------------------------------------------------------
 * key = the RAW (VsKey, TevKey) bytes; value = the program slot the full path (td_get -> canonical key -> table probe) returned for it.
 * The raw -> canonical map is a pure function of the raw bytes (persp_on / tev_fast_on are read once), so the value stays right until the SLOT
 * is given to another key: c3_slot_reset_gen(pi) bumps the slot's generation at exactly the two places that happen (a recycled slot's memset, and
 * the budget destroying the slot's program), and every entry carries the generation it was stored under. An entry is also refused while the
 * slot is not ok (a destroyed or pending program). Only OK programs are stored. */
#define C3_PM_N 1024
typedef struct { uint64_t h0, h1; int32_t pi; uint32_t gen; uint8_t used; } C3PmE;
static C3PmE c3_pm[C3_PM_N];
static uint32_t c3_gen[C3_GXP_MAX];
static inline void c3_pm_clear(void) { memset(c3_pm, 0, sizeof c3_pm); memset(c3_gen, 0, sizeof c3_gen); }
static inline void c3_slot_reset_gen(int pi)
{
#ifndef C3_BREAK_NOGEN   /* host negative control: a recycled slot keeps its generation, so a stale entry still names it */
    ++c3_gen[pi];
#else
    (void) pi;
#endif
}
/* 1 + *pi = a usable hit. slot_ok = the includer's "g_gxp[pi].ok". */
static inline int c3_pm_find(const void *vk, size_t nv, const void *tk, size_t nt, int (*slot_ok)(int), int *pi)
{
    uint64_t h[2]; c3_hash2(vk, nv, tk, nt, h);
    const C3PmE *e = &c3_pm[h[0] & (C3_PM_N - 1)];
    ++c3_st.pm_find;
    if (!e->used || e->h0 != h[0] || e->h1 != h[1]) return 0;
#ifndef C3_BREAK_NOGEN
    if (e->gen != c3_gen[e->pi]) { ++c3_st.pm_vfail; return 0; }
#endif
#ifndef C3_BREAK_NOOK    /* host negative control: a destroyed / pending slot is still answered */
    if (!slot_ok(e->pi)) { ++c3_st.pm_vfail; return 0; }
#endif
    ++c3_st.pm_hit; *pi = e->pi; return 1;
}
static inline void c3_pm_put(const void *vk, size_t nv, const void *tk, size_t nt, int pi)
{
    uint64_t h[2]; c3_hash2(vk, nv, tk, nt, h);
    C3PmE *e = &c3_pm[h[0] & (C3_PM_N - 1)];
    e->h0 = h[0]; e->h1 = h[1]; e->pi = pi; e->gen = c3_gen[pi]; e->used = 1; ++c3_st.pm_put;
}

/* ---- arm 103: the even-30 lock + frame-time statistics -------------------------------------------------------------------------------------
 * vitaGL (vgl.c) waits sceDisplayWaitVblankStartMulti(vsync_interval) after every flip; the default is 1. With 2 each finished frame is held for
 * two vblanks: a frame that costs <= 33.3 ms is shown for exactly 33.3 (no 3-2-3-2 judder, no running ahead of the game clock), a frame that costs more
 * is shown for 50. It does NOT make the frame cheaper: the lock is only meaningful once CPU+GPU are under 33.3 ms (docs/47: wall 35.0 today). It also
 * deepens the flip queue's latency by up to one frame at the same wall time (the triple buffer fills), which the judge cannot see: say so to Tyler.
 * Never fights vsync_off.txt (vsync_interval 0 at the first call = left alone). The original value is restored when the arm ends. */
extern uint32_t vsync_interval C3_EXT_ATTR;   /* vitaGL vgl.c, a plain global (nm: D) */
static int c3_pace_orig = -1;
static int c3_pace_default;                              /* the release build sets 1 here (or T10 wires a marker): the lock without an arm */
static inline uint32_t c3_pace_apply(unsigned mask)
{
    if (!&vsync_interval) return 0;
    if (c3_pace_orig < 0) c3_pace_orig = (int) vsync_interval;
    if (c3_pace_orig == 0) return vsync_interval;        /* vsync is off: not ours to change */
#ifdef C3_BREAK_NORESTORE   /* host negative control: when the arm ends the interval stays where the lock left it */
    const uint32_t want = ((mask & C3_B_PACE) || c3_pace_default) ? 2u : vsync_interval;
#else
    const uint32_t want = ((mask & C3_B_PACE) || c3_pace_default) ? 2u : (uint32_t) c3_pace_orig;
#endif
    if (vsync_interval != want) { vsync_interval = want; ++c3_st.pace_sets; }
    return want;
}
static unsigned long long c3_ft_last;
static inline void c3_frame_edge_h(unsigned mask, unsigned long long now_us)
{
    if (c3_ft_last) {
        const unsigned dt = (unsigned) (now_us - c3_ft_last);
        int b = (int) ((dt + 8333u) / 16667u) - 1; if (b < 0) b = 0; if (b > 4) b = 4;
        ++c3_st.ft[b];
        if (b == 1) { const int d = (int) dt - 33333; c3_st.ft_dev33 += (unsigned) (d < 0 ? -d : d); ++c3_st.ft_n33; }
    }
    c3_ft_last = now_us;
    (void) c3_pace_apply(mask);
    if (&gx_c3_toff) gx_c3_toff = (mask & C3_B_TOFF) != 0u;   /* arm 106/107/109: U's GXT_NOW() stops reading the clock */
}
#endif
