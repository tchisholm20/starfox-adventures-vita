/* gx_rec.c -- RECORD-AND-DISCARD WRAPPERS FOR THE GX BOUNDARY (docs/53 milestone T0-a).  An instrument: no thread, no queue,
 * no behavioural change by default.  It BOUNDS what a GX-level render thread would cost the game thread before anyone builds one.
 *
 * WHAT IT IS.  A `__wrap_X` for each of the 133 symbols the game calls across the GX/VI boundary: the 126 defined GX* and VI* of
 * docs/53 Appendix A plus the 7 gxfifo_push_{u8,s8,u16,s16,u32,s32,f32}.  The wrappers are GENERATED (host/engine/gen_gx_rec.py ->
 * gx_rec_wraps.inc, and host/engine/gx_wraps.txt for the link line) so the table, the wrappers, the --wrap flags and the lints share
 * one source.  Each wrapper has EXACTLY the caller-visible prototype of X (the compiler checks it: _Static_assert on
 * __builtin_types_compatible_p against the header's declaration, struct-by-value args like GXColor included), and:
 *
 *     if (!g_rec_mode) return __real_X(args);          // DEFAULT: one load + one branch + a tail call
 *     count it (per symbol; classes, N_tev, DL, prims, pushes, tex, mtx, copytex are derived from the per-symbol counts)
 *     if (g_rec_mode == 2) append the record the render thread would have to send   // then fall through
 *     return __real_X(args);                            // the real body ALWAYS runs: a record-AND-DISCARD, not a diversion
 *
 * g_rec_mode is a plain static, written ONLY by gx_rec_frame_edge(arm):  arm 10 -> 1 (count only), arm 11 -> 2 (count + record),
 * anything else -> 0.  The mode therefore changes only at frame edges.  WHAT THE ARMS READ (corrected 2026-10-18, finding "arm 10 minus
 * arm 0 is not the tax"): the wrappers are linked into EVERY arm, so arm 0 ALREADY pays the mode-0 hop (bl __wrap_X, one load, one
 * branch, a tail call into __real_X).  Therefore
 *     arm 10 - arm 0  = the COUNTING cost only (the extra out-of-line call into gxr_on_X plus the counter work): a LOWER BOUND on the
 *                       cost of having wrappers at all, never the tax itself;
 *     arm 11 - arm 10 = the record cost (instruction-level; see THE RING for why it is also a lower bound).
 * The mode-0 hop itself appears in NO arm: it is a BUILD-vs-BUILD figure (this build's arm 0 against a no-wrap build of the same
 * sources, same A/B/A boot or the cross-boot floor).  Runs before the wrappers existed (e.g. run 279's 84 ms) do not carry the hop.
 *
 * WHAT --wrap DOES NOT REACH (and why that is intended).  The linker redirects a reference to X only when it comes from ANOTHER
 * object.  Calls inside hal/gx_draw.c itself (gxfifo_push_f32 -> gxfifo_push_u32, GXSetTevIndRepeat -> GXSetTevIndirect, the
 * `GXLoadTexObjPreLoaded -> os_ext_poll` edge, ...) are intra-TU and stay unwrapped.  T0-a prices the GAME's calls into the
 * boundary, which are the ones a thread would have to record.  The HAL objects are byte-identical to the build before.
 *
 * WEAK DEFINITIONS (23 of the 126 are `W` in scaffold/game_boot_gx.c).  `--wrap=X` on a weak X works: the reference `X` in other
 * objects binds to `__wrap_X`, and `__real_X` binds to whichever definition of X the link chose (the weak one, if no strong one
 * exists).  The postlink lint (gen_gx_rec.py postlink) checks `X` is still defined and `__real_X` resolved for every row; what nm
 * shows afterwards is recorded in docs/53 section 12 question 5.  A strong definition added later changes nothing here.
 *
 * WHAT A RECORD IS (mode 2).  docs/53 6.1: hdr:u32 = op:10 | nwords:22, then the arguments BY VALUE, each padded to 4 bytes
 * (op = row index + 1, rows sorted by name).  Pointer arguments the callee reads at call time are copied inline (a 3x4 matrix is
 * 48 B, a projection 64 B, a light object 64 B, an indirect matrix 24 B; GXLoadTexObj/PreLoaded and GXInitTexObjLOD carry the
 * 32-byte texture object), not as a pointer.  The seven pipe functions append big-endian bytes to an open VBYTES chunk (8 KB, hdr +
 * length word), exactly as the design's immediate path does.  Only classes A, C and the D fences are recorded; A2 (object fill), B
 * (getters), E (no-ops), the break-point trio and the retrace callbacks run G-locally in the design and are counted only (the one
 * exception is GXInitTexObjLOD's TEXLOD_NOTE).  Pushes made while a display list is being recorded (between GXBeginDisplayList and
 * GXEndDisplayList, tracked here only while mode > 0) write the game's buffer in the design, so they are counted but not recorded.
 *
 * THE RING.  4 MB, malloc'd on the first transition into mode 2 (a default run owns no ring), head reset to 0 at EVERY frame edge.
 * Reuse of the same ~0.4 MB every frame keeps it in L2, so the record cost measured here is the INSTRUCTION cost of building a
 * record, a LOWER BOUND on the real thing, which streams through the whole ring and has a second core pulling it.  A frame that
 * does not fit is counted in `drops` and the rest is discarded; `hw` is the largest bytes-in-one-frame seen.
 *
 * THE INSTRUMENT'S OWN COST.  Per-wrapper bookkeeping is timed with sceKernelGetProcessTimeWide() (1 us resolution, so one call
 * reads 0 or 1; the SUM over the ~15k calls of a frame is unbiased) on 1 frame in 16 only, so the clock reads land on 1/16 of the
 * frames.  The cost of the clock reads themselves is calibrated once (back-to-back reads, 4096 pairs) and subtracted:
 * `book us/frame` raw and net are printed.  It is the bookkeeping inside the wrapper (counting + record), not the call/branch hop of the
 * wrapper itself (see above: that is a build-vs-build figure).  Sampled frames are slower by the clock reads (2 per timed call); the
 * window line prints `tc` timed calls and `ns/read`, and perf_ab.py SUBTRACTS 2 x tc x ns/read per frame from arms 10 and 11's wall before
 * it compares them with arm 0 (it prints both, labelled [E]: the read cost is calibrated at 1 us resolution).  Every number printed is [M] for the build
 * that printed it; none is an estimate except where this comment says "lower bound".
 *
 * THREADS.  The baton serialises the game threads, so the counters and the ring have one writer at a time (plain, no atomics, no
 * locks); torn reads by the printing path are benign.  Wrappers never touch GL, GXM, or any R-owned state beyond calling __real_X.
 *
 * THREADED MODE (g_rec_mode == GXR_MODE_THREAD == 3; RT_THREAD.md 6.2).  The same wrappers also carry the render-thread split: in mode 3 the
 * generated line `if (g_rec_mode == GXR_MODE_THREAD) { ... }` (gx_rec_wraps.inc, from host/engine/gx_thr_table.txt) records an R row's
 * arguments into the SPSC ring (gxt_G_X, gx_rt_gen.inc), runs an L row's real body on G, hands an X row to the custom module
 * (gxts_G_X, gx_rt_special.c) and pushes a P row's bytes with gxt_push_*.  gx_rt_gen.inc also holds the 19 [extra] wrappers and
 * gxt_r_exec, the R-side dispatch the ring thread calls for every record.  Mode 3 is entered/left ONLY at a frame edge by the ring module
 * (gxt_frame_edge -> gx_rec_set_threaded) and is never entered unless gx_thread.txt asks for it: with no marker g_rec_mode is the arm-driven
 * T0-a mode exactly as before, the extra test is one compare on the already-taken instrumented path, and the [extra] wrappers cost one
 * load + one not-taken branch + the real call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/gx/GXCpu2Efb.h>   /* GXPeekZ */
#include <dolphin/gx/GXDraw.h>      /* GXDrawTorus */
#include <main/pi_flush_api.h>      /* GXFlush_ (the game's own wrapper, defined in pi_videoinit.c) */
#include <psp2/kernel/processmgr.h>
#include "gx_rec.h"
#include "gx_rt.h"
#include "gx_rt_special.h"
#if defined(__vita__)
extern void rhi_vita_gxd_edge(int arm) __attribute__((weak));   /* B1-c2floor: rhi_vita_gxdump.inc, perf_ab arm 114 (the one-frame draw-stream dump) */
#endif

/* The pipe prototypes, exactly GXVert.h's. The header declares them only under `STAIRFAX_PORT && !_WIN32`; the Vita build gets them
 * from it (a conflicting redeclaration here would be a compile error), the host test (MinGW defines _WIN32) from this. */
void gxfifo_push_u8(unsigned char v);  void gxfifo_push_s8(signed char v);
void gxfifo_push_u16(unsigned short v); void gxfifo_push_s16(short v);
void gxfifo_push_u32(unsigned int v);  void gxfifo_push_s32(int v);
void gxfifo_push_f32(float v);

#define GXR_RING_BYTES (4u << 20)
#define GXR_VB_CHUNK   8192u
#define GXR_SAMPLE_MASK 15u          /* time the bookkeeping on 1 frame in 16 */
#define GXR_OP_VBYTES  1000u         /* must match gen_gx_rec.py OP_VBYTES */
#define GXR_MODE_THREAD 3            /* the render-thread mode (RT_THREAD.md): set only by gx_rec_set_threaded / the frame edge */

/* The ring module (src/gx_thread.c) is OPTIONAL at link time: weak, so a viewer that links no ring keeps the T0-a behaviour. */
extern void gxt_boot(void) __attribute__((weak));
extern int gxt_frame_edge(int arm, int present_no) __attribute__((weak));

/* ---- the switch and the state the wrappers share ---------------------------------------------------------------------------- */
static int g_rec_mode;               /* 0 off, 1 count, 2 count + record, 3 THREADED (record for the render thread).  Written ONLY by gx_rec_frame_edge / gx_rec_set_threaded */
static int g_thr_on;                 /* the ring module's last gx_rec_set_threaded() */
static int g_rec_timed;              /* this frame times its bookkeeping (1 in 16) */
static unsigned g_cnt[256];          /* per row; GXR_N <= 256 (static-asserted below) */
static unsigned g_dl_bytes, g_copytex_b8, g_push_in_list, g_dst_fmt, g_in_list;
static unsigned g_book_us, g_timed_calls;
static unsigned *g_ring;             /* malloc'd on the first transition to mode 2 */
static unsigned g_head;              /* bytes written this frame */
static unsigned g_nrec, g_drops;
static unsigned char *g_vb; static unsigned g_vb_len; static int g_vb_dead;

#define GXR_W(x) ((unsigned) ((sizeof(__typeof__(x)) + 3u) >> 2))
#define GXR_PUT(p, x) do { __typeof__(x) v_ = (x);                                                           \
        if (sizeof v_ < 4) { unsigned w_ = 0; memcpy(&w_, &v_, sizeof v_); *(p)++ = w_; }                    \
        else { memcpy((p), &v_, sizeof v_); (p) += GXR_W(x); } } while (0)
#define GXR_T0 unsigned t0_ = 0; if (__builtin_expect(g_rec_timed, 0)) t0_ = (unsigned) sceKernelGetProcessTimeWide()
#define GXR_T1 if (__builtin_expect(g_rec_timed, 0)) { g_book_us += (unsigned) sceKernelGetProcessTimeWide() - t0_; ++g_timed_calls; }

static void gxr_vb_close(void)
{
    if (!g_vb) return;
    unsigned *h = (unsigned *) (g_vb - 8);
    h[0] = GXR_OP_VBYTES | ((1u + ((g_vb_len + 3u) >> 2)) << 10);
    h[1] = g_vb_len;
    g_head += 8u + ((g_vb_len + 3u) & ~3u);
    g_vb = 0; g_vb_len = 0;
}
/* a record of `nwords` payload words; NULL (and a counted drop) when the frame's ring is full */
static unsigned *gxr_alloc(unsigned op, unsigned nwords)
{
    gxr_vb_close(); g_vb_dead = 0;
    const unsigned bytes = (1u + nwords) * 4u;
    if (g_head + bytes > GXR_RING_BYTES) { ++g_drops; return 0; }
    unsigned *p = (unsigned *) ((char *) g_ring + g_head);
    p[0] = op | (nwords << 10);
    g_head += bytes; ++g_nrec;
    return p + 1;
}
static int gxr_vb_open(void)
{
    if (g_head + 8u + GXR_VB_CHUNK > GXR_RING_BYTES) { ++g_drops; g_vb_dead = 1; return 0; }
    g_vb = (unsigned char *) g_ring + g_head + 8u; g_vb_len = 0; ++g_nrec;
    return 1;
}
/* one big-endian store of n (1, 2 or 4) bytes into the open VBYTES chunk */
static void gxr_vb_put(unsigned v, unsigned n)
{
    if (g_vb_dead) return;
    if (!g_vb) { if (!gxr_vb_open()) return; }
    else if (g_vb_len + n > GXR_VB_CHUNK) { gxr_vb_close(); if (!gxr_vb_open()) return; }
    unsigned char *d = g_vb + g_vb_len;
    if (n == 1) d[0] = (unsigned char) v;
    else if (n == 2) { d[0] = (unsigned char) (v >> 8); d[1] = (unsigned char) v; }
    else { d[0] = (unsigned char) (v >> 24); d[1] = (unsigned char) (v >> 16); d[2] = (unsigned char) (v >> 8); d[3] = (unsigned char) v; }
    g_vb_len += n;
}

/* ---- the generated render-thread layer (gxt_G_X, the extras, gxt_r_exec), then the wrappers, __real_ declarations and report tables ---- */
#include "gx_rt_gen.inc"
#include "gx_rec_wraps.inc"
_Static_assert(GXR_N <= 256, "g_cnt is sized for 256 rows");
_Static_assert(GXT_OP_TABLE_N == GXR_N, "gx_rt_ops.h and gx_rec_wraps.inc disagree on the number of table rows");
_Static_assert(sizeof(GXTexObj) >= 32 && sizeof(GXLightObj) >= 64, "inline payload sizes in the table exceed the game's objects");

/* ---- frame edges and the per-window report ------------------------------------------------------------------------------------ */
static int g_booted, g_ever_on, g_thr_booted, g_edges;
static unsigned g_frames_total;                 /* frame edges seen with mode > 0 (selects the timed frames) */
static unsigned g_clock_pair_ns;
static unsigned g_snap[256];                    /* g_cnt at the last window print */
static unsigned g_snap_dl_bytes, g_snap_b8, g_snap_inlist, g_snap_nrec, g_snap_book, g_snap_tcalls;
static unsigned g_w_frames, g_w_rec_bytes, g_w_hw, g_w_timed_frames, g_w_mode, g_w_drops0;
static unsigned g_hw_all;                       /* largest bytes-in-one-frame, whole run */

static void gxr_boot_line(void)
{
    g_booted = 1;
    printf("[gxr] wraps %d symbols, table hash %08x (arm %d = count, arm %d = count + record; any other arm = wrappers off)\n",
           (int) GXR_N, (unsigned) GXR_TABLE_HASH, GXR_ARM_COUNT, GXR_ARM_RECORD);
}

static void gxr_calibrate(void)
{
    unsigned long long s = 0;
    for (int i = 0; i < 4096; ++i) { const unsigned long long a = sceKernelGetProcessTimeWide(), b = sceKernelGetProcessTimeWide(); s += b - a; }
    g_clock_pair_ns = (unsigned) (s * 1000ull / 4096ull);
    printf("[gxr] clock read cost, calibrated (4096 back-to-back pairs): %u ns per read\n", g_clock_pair_ns);
}

/* Close the frame that just ended (idempotent). Called by the frame edge AND by gx_rec_window, because vt_perf_ab prints the window of
 * presents P-60..P BEFORE it calls the edge for present P: without the close here the 60th frame's close landed in the NEXT window
 * (59 closed frames against 60 frames of call counts: every per-frame figure +1.7%, a stale fr=1 line after an arm switch, and the timed
 * frames' time and count in different windows). g_frame_open is set by the edge for mode > 0 and cleared here. */
static int g_frame_open;
static void gxr_close_frame(void)
{
    if (!g_frame_open) return;
    g_frame_open = 0;
    gxr_vb_close();
    ++g_w_frames; g_w_rec_bytes += g_head;
    if (g_head > g_w_hw) g_w_hw = g_head;
    if (g_head > g_hw_all) g_hw_all = g_head;
    if (g_rec_timed) ++g_w_timed_frames;
}

/* Called ONLY by the ring module (gxt_frame_edge), at a frame edge: the wrappers record for the render thread (on) or go back to the
 * arm-driven mode (off; gx_rec_frame_edge re-derives it from the arm right after). */
void gx_rec_set_threaded(int on)
{
    g_thr_on = on;
    g_rec_mode = on ? GXR_MODE_THREAD : 0;
}

void gx_rec_frame_edge(int arm)
{
#if defined(__vita__)
    if (rhi_vita_gxd_edge) rhi_vita_gxd_edge(arm);   /* B1-c2floor: records / writes the frame while arm 114 is current; a no-op otherwise */
#endif
    if (!g_booted) gxr_boot_line();
    if (!g_thr_booted) { g_thr_booted = 1; if (gxt_boot) gxt_boot(); }        /* the render-thread markers, read once; prints nothing when there is no marker */
    const int thr = gxt_frame_edge ? gxt_frame_edge(arm, ++g_edges) : g_thr_on;   /* may switch the mode (gx_rec_set_threaded) */
    gxr_close_frame();                              /* a no-op when gx_rec_window already closed it */
    g_head = 0; g_vb = 0; g_vb_len = 0; g_vb_dead = 0;   /* the ring is reset at EVERY frame edge */
    int m = thr ? GXR_MODE_THREAD : arm == GXR_ARM_COUNT ? 1 : arm == GXR_ARM_RECORD ? 2 : 0;   /* arms 10/11 are ignored while threaded */
    if (m == 2 && !g_ring) {
        g_ring = (unsigned *) malloc(GXR_RING_BYTES);
        if (!g_ring) { printf("[gxr] ring malloc(%u) FAILED: arm %d degrades to count-only\n", GXR_RING_BYTES, arm); m = 1; }
    }
    if ((m == 1 || m == 2) && !g_clock_pair_ns) gxr_calibrate();
    if (m != g_rec_mode) g_in_list = 0;             /* the list-recording flag is only tracked while mode > 0: do not carry a stale one over a switch */
    g_rec_mode = m;
    const int t0a = m == 1 || m == 2;               /* the T0-a instrument runs only in its own modes, never in the threaded one */
    g_frame_open = t0a;
    if (t0a) { g_ever_on = 1; ++g_frames_total; }
    g_rec_timed = t0a && ((g_frames_total & GXR_SAMPLE_MASK) == GXR_SAMPLE_MASK);
    g_w_mode = (unsigned) (t0a ? m : 0);
}

void gx_rec_window(int from, int to, int arm)
{
    gxr_close_frame();                              /* the frame that ended at this present belongs to THIS window */
    if (!g_ever_on) return;                         /* a default run prints nothing */
    const unsigned fr = g_w_frames;
    if (!fr) {
        printf("[gxr] presents %d..%d arm %d mode 0 | wrappers off (fast path: one load, one branch, a tail call)\n", from, to, arm);
        memcpy(g_snap, g_cnt, sizeof g_snap);       /* counts move only in mode > 0, so this is a no-op unless a mode flipped mid-window */
        return;
    }
    double cls[7] = {0}, ntev = 0, dl = 0, prims = 0, pushc = 0, pushb = 0, tex = 0, mtx = 0, copytex = 0, total = 0;
    for (unsigned i = 0; i < GXR_N; ++i) {
        const double d = (double) (unsigned) (g_cnt[i] - g_snap[i]);
        total += d; cls[gxr_cls[i]] += d;
        const unsigned k = gxr_kind[i];
        if (k & 1) ntev += d;
        if (k & 2) dl += d;
        if (k & 4) prims += d;
        if (k & 8) tex += d;
        if (k & 16) mtx += d;
        if (k & 32) copytex += d;
        if (k & 64) { pushc += d; pushb += d * gxr_push_bytes[i]; }
    }
    const double F = fr;
    printf("[gxr] presents %d..%d arm %d mode %u | calls/frame %.1f (A %.1f A2 %.1f B %.1f C %.1f D %.1f E %.1f pipe %.1f) | ntev/frame %.1f"
           " | dl %.2f (%.0f B) | prims %.1f | push calls %.1f bytes %.1f (in-dl calls %.1f) | tex %.2f | mtx %.1f | copytex %.2f b8 %.2f"
           " | rec bytes/frame %.0f hw %u (run hw %u, records/frame %.1f, drops %u)",
           from, to, arm, g_w_mode, total / F, cls[0] / F, cls[1] / F, cls[2] / F, cls[3] / F, cls[4] / F, cls[5] / F, cls[6] / F, ntev / F,
           dl / F, (double) (g_dl_bytes - g_snap_dl_bytes) / F, prims / F, pushc / F, pushb / F, (double) (g_push_in_list - g_snap_inlist) / F,
           tex / F, mtx / F, copytex / F, (double) (g_copytex_b8 - g_snap_b8) / F,
           (double) g_w_rec_bytes / F, g_w_hw, g_hw_all, (double) (g_nrec - g_snap_nrec) / F, g_drops - g_w_drops0);
    const unsigned tf = g_w_timed_frames, tc = g_timed_calls - g_snap_tcalls, raw = g_book_us - g_snap_book;
    if (tf) {
        const double clk = (double) tc * g_clock_pair_ns / 1000.0;
        printf(" | book us/frame %.2f raw, %.2f net of clock reads (%u of %u frames timed, %u calls, %u ns/read)\n",
               (double) raw / tf, ((double) raw - clk) / tf, tf, fr, tc, g_clock_pair_ns);
    } else printf(" | book us/frame - (no timed frame in this window)\n");
    /* the census: the 8 most-called boundary functions of the window */
    unsigned best[8]; int nb = 0;
    for (unsigned i = 0; i < GXR_N; ++i) {
        const unsigned d = g_cnt[i] - g_snap[i];
        if (!d || (nb == 8 && d <= g_cnt[best[7]] - g_snap[best[7]])) continue;
        int j = nb < 8 ? nb++ : 7;
        best[j] = i;
        while (j > 0 && g_cnt[best[j]] - g_snap[best[j]] > g_cnt[best[j - 1]] - g_snap[best[j - 1]]) { const unsigned t = best[j]; best[j] = best[j - 1]; best[j - 1] = t; --j; }
    }
    printf("[gxr] top8/frame:");
    for (int j = 0; j < nb; ++j) printf(" %s=%.1f", gxr_name[best[j]], (double) (g_cnt[best[j]] - g_snap[best[j]]) / F);
    printf("\n");
    memcpy(g_snap, g_cnt, sizeof g_snap);
    g_snap_dl_bytes = g_dl_bytes; g_snap_b8 = g_copytex_b8; g_snap_inlist = g_push_in_list; g_snap_nrec = g_nrec;
    g_snap_book = g_book_us; g_snap_tcalls = g_timed_calls; g_w_drops0 = g_drops;
    g_w_frames = 0; g_w_rec_bytes = 0; g_w_hw = 0; g_w_timed_frames = 0;
}
