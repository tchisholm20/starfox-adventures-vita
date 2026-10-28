/* gx_rt_special.c -- module "special" of the GX render thread (RT_THREAD.md 3.3-3.6, 6.3; docs/53 rev 2 on the GXM backend).
 *
 * Everything with GX SEMANTICS that is not "record the arguments by value":
 *   display-list recording on G (GXBeginDisplayList / GXEndDisplayList / GXBegin's list half), the scissor shadow, the clear-colour split, the blocking B8 copy,
 *   the texture-object split (GXInitTexObjLOD G half + R half, GXLoadTexObj on a scratch object), the WRITTEN SET W + ping-pong array shadows (policy P2'),
 *   the dirty-image set + ping-pong snapshots, the single-stepped draw-sync fence and the break-point delivery gate, GXPeekZ, the content AUDIT (G-L3),
 *   the launch preconditions (gxts_refuse_reason) and the break arms owned here (scribble snapoff bp_early jump2 nostep noflush dropdraw).
 *
 * THREADS.  G = the baton thread (the one producer of the ring).  R = "stfx_gxr".  Every file-static below is G-ONLY unless its comment says [R]: the R-side
 * handlers (gxts_R_*) read their record and call the HAL's real body (__real_X, which --wrap resolves to the original); they touch no G static.  The only
 * cross-thread data are the g_gxt fields of gx_rt.h and the two [R] audit counters (32-bit, R writes, G reads for printing).
 *
 * RECORD LAYOUTS ARE PRIVATE (RT_THREAD.md 4): pointers are PW = sizeof(void*)/4 words (1 on the Vita, 2 on the 64-bit host).
 *
 * LIFETIMES (RT_THREAD.md 3.5): every port-owned buffer a record can point at (array shadows, image snapshots) is released through gxt_free() -- never free()
 * -- because R may not have executed the record that names it yet.  The copies are chosen by `g_gxt.seq & 1`, so a copy made for frame j+1 can only overwrite
 * a buffer last named by frame j-1: safe exactly while G is at most ONE frame ahead of R (AHEAD = 1; the PRESENT wait in gxt_present is what bounds it).
 * LIMITS, stated here and not hidden:
 *   - a span whose bytes change TWICE within one frame (the shadow pass and the main pass of a shadow-type-4 object) is copied again at the second change,
 *     into the SAME parity buffer: records of the first pass that R has not executed yet read the second pass's bytes.  Identical bytes (the same pose) are
 *     harmless; different bytes are not.  Found, if it exists, by the audit (G-L3) under `rdelay`, not by this file.
 *   - a shadow copies 256 bytes past the end of its span (the index limit of a display list is not known to G); that read is of the game's heap.
 *   - an image snapshot is level 0 only.  Since the 2026-10-20 review a MIPMAPPED image (object mipmap bit) is therefore never snapshotted: it is read live, counted snap_mip (the HAL keys the authored chain
 *     and the LOD bias by the original image address, and the chain lives past level 0).  A mipmapped image the CPU rewrites after its load is covered by the audit window only.
 *   - a REGISTERED MOVIE PLANE (gx_tex_yuv_register*) is never snapshotted or pre-seeded either: the HAL identifies planes by pointer equality (counted snap_movie).
 *   - snapshot buffers are released from R's texture cache (a gx_tex_release record per buffer) whenever they are freed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_rt_special.h"
#include "gx_rt_ops.h"
#include "gx_thread_plat.h"   /* gxtp_read_marker (IMM census) */

/* ------------------------------------------------------------------------------------------------------------------------------ configuration */
#ifndef GXTS_DATA_DIR
#define GXTS_DATA_DIR "ux0:data/stairfax/"          /* the markers; the host test points this at a scratch directory */
#endif
#ifndef GXTS_JOURNAL_PATH
#define GXTS_JOURNAL_PATH "ux0:data/STFX00001/JOURNAL"
#endif
#define GXTS_W_CAP            1024u    /* written-set spans */
#define GXTS_W_EXPIRE_FRAMES  8        /* a span with no event for this many frames leaves W */
#define GXTS_W_MARGIN         256u     /* live bytes copied past a span's end (RT_THREAD.md 3.5.3) */
#define GXTS_W_INVAL_MAX      4096u    /* gx_tex_invalidate spans up to this size feed W (the 32-byte splash colour array flushes with DCStoreRange) */
#define GXTS_IMG_CAP          8192u    /* images referenced by a recorded load */
#define GXTS_IMG_IDLE_FRAMES  8        /* a snapshot of an image not loaded for this many frames is released (checked every 16th frame) */
#define GXTS_CD_CAP           64u      /* copy destinations (the HAL's table has the same size) */
#define GXTS_AUDIT_WIN        4096u
#define GXTS_AUDIT_IMG_WHOLE  65536u
#define GXTS_PAGE_BUCKETS     4096u    /* 64 KB pages hashed into this many buckets (the HAL's gTc does the same) */

/* ------------------------------------------------------------------------------------------------------------------------------ the HAL's real bodies */
/* On the device `--wrap=X` makes __real_X the original X.  The host test maps them with `-include` + `#define __real_X X`. */
extern void __real_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
extern void __real_GXBeginDisplayList(void *list, u32 size);
extern void __real_GXCopyTex(void *dest, GXBool clear);
extern void __real_GXGetScissor(u32 *left, u32 *top, u32 *wd, u32 *ht);
extern void __real_GXInitTexObjLOD(GXTexObj *obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                                   GXBool do_edge_lod, GXAnisotropy max_aniso);
extern void __real_GXLoadTexObj(GXTexObj *obj, GXTexMapID id);
extern u16  __real_GXReadDrawSync(void);
int rhi_gxm_peekZ(int x, int y, unsigned *z24, unsigned *raw);   /* rhi_gxm_target.c: reads M's depth memory, never waits */
extern void __real_GXSetArray(GXAttr attr, void *base_ptr, u8 stride);
extern void __real_GXSetCopyClear(GXColor clear_clr, u32 clear_z);
extern void __real_GXSetDrawSync(u16 token);
extern void __real_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht);
extern void __real_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap);
extern u32  __real_GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod);
extern int  __real_gx_breakpoint_deliver(void);
extern void __real_gx_dlk_event(const void *p, size_t n, int isWrite);
extern void __real_gx_tex_invalidate(const void *addr, u32 n);
extern void __real_gx_tex_release(const void *addr, u32 n);
extern void __real_gx_tex_yuv_register(const void *y, const void *u, const void *v, int w, int h);
extern void __real_gx_tex_yuv_register_x(const void *y, const void *u, const void *v, int w, int h, uint8_t *ey, uint8_t *eu, uint8_t *ev);
extern void __real_GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);
extern void __real_os_ext_poll(void) __attribute__((weak));
extern void __real_vi_set_clear_color(float r, float g, float b) __attribute__((weak));

/* game symbols, opaque and weak: this file links into viewers and the host test that do not define them */
extern int  Queue_GetCount(void *queue) __attribute__((weak));
extern char gVideoFlipQueue[] __attribute__((weak));
extern int  mm_guard_mode __attribute__((weak));
extern int  rhi_vita_efbZMode(void) __attribute__((weak));
extern int  rhi_vita_efbMode(void) __attribute__((weak));
extern GXColor gFogColor __attribute__((weak));         /* the GAME's fog colour (track/intersect.c); the HAL's GXSetFog also stores it in DIRECT mode and the game reads it back (intersect_render.c:149) */

/* ------------------------------------------------------------------------------------------------------------------------------ small helpers */
#define PW ((uint32_t)(sizeof(void *) / 4u))
#define BRK(b) (g_gxt.cfg.brk & (b))
#define FRAME() (g_gxt.seq)

static inline void put_ptr(uint32_t *w, const void *p) { memcpy(w, &p, sizeof p); }
static inline void *get_ptr(const uint32_t *w) { void *p; memcpy(&p, w, sizeof p); return p; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static uint32_t fnv1a(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* counters.  [G] unless noted */
static struct {
    uint32_t snap_n, snap_bytes, shadow_n, shadow_bytes, snap_over, wset_hits, b8_blocks, ds_steps, bp_gates, bp_held, peekz_g, peekz_far;
    uint32_t alloc_fail, wset_evicted, wset_expired, img_evicted, ds_unserved_end, scribbled, snap_over_logged, snap_idle_freed, rw_pre;
    uint32_t snap_mip, snap_movie, snap_rel, movie_planes, fog_mirrored, reclaim_retry, audit_skip_movie;
} s_c;
static volatile uint32_t s_audit_checked, s_audit_bad;     /* [R] written by R, read by G for printing (32-bit: no tearing) */

/* ------------------------------------------------------------------------------------------------------------------------------ audit (G-L3) */
/* G hashes a bounded window of what R will read, R hashes the same window at replay.  ARRAYS: the first 4096 bytes at a SHADOW base (its extent is known: span + margin);
 * for a LIVE array, whose extent is not known, live_window(): 16 elements (at least 256 bytes), never past the end of the base's 4 KB page.
 * IMAGES: the whole image up to 64 KB, else the first 4096 bytes. */
static void audit_check(const char *kind, const void *p, uint32_t len, uint32_t want, uint32_t rec_seq)
{
    const uint32_t got = fnv1a(p, len);
    ++s_audit_checked;
    if (got != want) {
        const uint32_t n = ++s_audit_bad;
        if (n <= 16) printf("[gxt] AUDIT MISMATCH kind=%s addr=%p seq=%u\n", kind, p, (unsigned)rec_seq);
    }
}
static uint32_t live_window(uintptr_t a, u8 stride)
{
    /* An array outside W has no known extent.  Sixteen elements (at least 256 bytes), never past the end of the base's 4 KB page (a mapped page cannot fault),
     * never less than 16 bytes: far enough to see an in-place rewrite of the array, short enough not to hash the allocation next door. */
    uint32_t n = (uint32_t)stride * 16u;
    if (n < 256u) n = 256u;
    if (n > GXTS_AUDIT_WIN) n = GXTS_AUDIT_WIN;
    const uint32_t to_page = GXTS_AUDIT_WIN - (uint32_t)(a & (GXTS_AUDIT_WIN - 1u));
    if (n > to_page) n = to_page;
    if (n < 16u) n = 16u;
    return n;
}
/* break arm `scribble`: after the record is queued, every 50th un-shadowed reference gets 16 bytes of its SOURCE flipped */
static void scribble_maybe(void *src, uint32_t avail)
{
    static uint32_t n;
    if (!BRK(GXT_BRK_SCRIBBLE) || !src || avail < 16u) return;
    if (++n % 50u) return;
    for (int i = 0; i < 16; ++i) ((uint8_t *)src)[i] ^= 0xA5u;
    ++s_c.scribbled;
}

/* ------------------------------------------------------------------------------------------------------------------------------ page-bucketed index (the HAL's gTc scheme) */
/* An entry lives in up to 4 consecutive 64 KB pages' buckets; a chain is threaded through the entries' per-slot `nx`.  PgLink is the FIRST member of every entry. */
#define PG_NONE 0xFFFFFFFFu
typedef struct { uint32_t pg[4]; int32_t nx[4]; } PgLink;
typedef struct { int32_t *head; uint8_t *pool; size_t stride; } PgIdx;     /* head: GXTS_PAGE_BUCKETS entries, calloc'd by tables_alloc */
static inline PgLink *pl(const PgIdx *ix, int32_t i) { return (PgLink *)(ix->pool + (size_t)i * ix->stride); }
static inline int pl_slot(const PgLink *l, uint32_t b) { for (int k = 0; k < 4; ++k) if (l->pg[k] == b) return k; return -1; }
static inline int32_t pg_next(const PgIdx *ix, int32_t e, uint32_t b) { const int s = pl_slot(pl(ix, e), b); return s < 0 ? 0 : pl(ix, e)->nx[s]; }
static void pg_add(PgIdx *ix, int32_t i, uintptr_t lo, uintptr_t hi)   /* hi > lo */
{
    PgLink *l = pl(ix, i);
    const uintptr_t first = lo >> 16, last = (hi - 1u) >> 16;
    for (int k = 0; k < 4; ++k) l->pg[k] = PG_NONE;
    int k = 0;
    for (uintptr_t p = first; p <= last && k < 4; ++p, ++k) {
        const uint32_t b = (uint32_t)(p & (GXTS_PAGE_BUCKETS - 1u));
        l->pg[k] = b; l->nx[k] = ix->head[b]; ix->head[b] = i;
    }
}
static void pg_del(PgIdx *ix, int32_t i)
{
    PgLink *l = pl(ix, i);
    for (int k = 0; k < 4; ++k) {
        const uint32_t b = l->pg[k];
        if (b == PG_NONE) continue;
        int32_t prev = 0; int pk = 0;
        for (int32_t e = ix->head[b]; e; ) {
            const PgLink *x = pl(ix, e); const int s = pl_slot(x, b);
            if (s < 0) break;
            if (e == i) { if (prev) pl(ix, prev)->nx[pk] = l->nx[k]; else ix->head[b] = l->nx[k]; break; }
            prev = e; pk = s; e = x->nx[s];
        }
        l->pg[k] = PG_NONE;
    }
}
/* the buckets a byte range [lo,hi) touches: calls f(b) for each distinct bucket, at most one lap of the table */
#define PG_FOR_RANGE(lo_, hi_, B_)                                                                                                                         \
    for (uintptr_t pgi_ = (lo_) >> 16, pge_ = ((hi_) > (lo_) ? ((hi_) - 1u) >> 16 : (lo_) >> 16), pgn_ = 0;                                                  \
         pgi_ <= pge_ && pgn_ < GXTS_PAGE_BUCKETS; ++pgi_, ++pgn_)                                                                                         \
        for (uint32_t B_ = (uint32_t)(pgi_ & (GXTS_PAGE_BUCKETS - 1u)), once_ = 1; once_; once_ = 0)

static uint32_t s_visit;                  /* visit stamps: an entry registered in several buckets is handled once per scan */

/* ------------------------------------------------------------------------------------------------------------------------------ the WRITTEN SET W (policy P2') */
typedef struct WSpan {
    PgLink lk;
    uintptr_t lo, hi;
    uint32_t ver;                          /* bumped by every event that lands in / merges into the span */
    uint32_t last;                         /* g_gxt.seq at the last event (expiry) */
    uint32_t visit;
    uint8_t  used;
    uint8_t  valid[2];
    uint8_t *buf[2];                       /* ping-pong shadows, [lo, hi + margin) */
    uint32_t cap[2], bver[2], bn[2];
} WSpan;
/* The tables below are allocated by tables_alloc() the first time threaded mode is considered (gxts_refuse_reason): a DIRECT run (the default) pays no BSS and no heap for them
 * (they were ~0.86 MB of static arrays in every build; measured with arm-vita-eabi-nm on stairfax_game_gxt_rt4.elf: s_img 688 KB, s_w 90 KB, s_ifree 32 KB, s_wix/s_iix 32 KB). */
static WSpan   *s_w;
static PgIdx   s_wix = { .pool = NULL };
static int32_t *s_wfree;
static int     s_wnfree;
static unsigned s_wn;

static void tables_or_die(void);
static void w_init_once(void)
{
    if (s_wix.pool) return;
    tables_or_die();
    s_wix.pool = (uint8_t *)s_w; s_wix.stride = sizeof(WSpan);
    memset(s_wix.head, 0, GXTS_PAGE_BUCKETS * sizeof *s_wix.head);
    s_wnfree = 0;
    for (int32_t i = (int32_t)GXTS_W_CAP; i >= 1; --i) s_wfree[s_wnfree++] = i;
    s_wn = 0;
}
static void span_drop_bufs(WSpan *s)
{
    for (int p = 0; p < 2; ++p) {
        if (s->buf[p]) gxt_free(s->buf[p]);
        s->buf[p] = NULL; s->cap[p] = 0; s->valid[p] = 0;
    }
}
static void w_release(int32_t i)
{
    WSpan *s = &s_w[i];
    pg_del(&s_wix, i); span_drop_bufs(s);
    s->used = 0; s_wfree[s_wnfree++] = i; --s_wn;
}
static void w_reindex(int32_t i) { pg_del(&s_wix, i); pg_add(&s_wix, i, s_w[i].lo, s_w[i].hi); }
static WSpan *w_find(uintptr_t x)
{
    const uint32_t b = (uint32_t)((x >> 16) & (GXTS_PAGE_BUCKETS - 1u));
    for (int32_t e = s_wix.head[b]; e; e = pg_next(&s_wix, e, b))
        if (s_w[e].used && s_w[e].lo <= x && x < s_w[e].hi) return &s_w[e];
    return NULL;
}
static int32_t w_oldest(void)
{
    int32_t o = 0;
    for (int32_t i = 1; i <= (int32_t)GXTS_W_CAP; ++i)
        if (s_w[i].used && (!o || GXT_BEHIND(s_w[i].last, s_w[o].last))) o = i;
    return o;
}
static int32_t w_new(uintptr_t lo, uintptr_t hi, uint32_t ver, uint32_t last, int may_evict)
{
    if (!s_wnfree) {
        if (!may_evict) return 0;
        const int32_t o = w_oldest();
        if (!o) return 0;
        w_release(o); ++s_c.wset_evicted;
    }
    const int32_t i = s_wfree[--s_wnfree];
    WSpan *s = &s_w[i];
    memset(s, 0, sizeof *s);
    s->used = 1; s->lo = lo; s->hi = hi; s->ver = ver; s->last = last;
    pg_add(&s_wix, i, lo, hi); ++s_wn;
    return i;
}
/* a write landed in [lo,hi): the span containing it gets a new version; otherwise touching spans merge into one (their copies are dropped: the offsets moved) */
static void w_write(uintptr_t lo, uintptr_t hi)
{
    if (hi <= lo) return;
    w_init_once();
    const uint32_t frame = FRAME();
    WSpan *c = w_find(lo);
    if (c && hi <= c->hi) { ++c->ver; c->last = frame; return; }                          /* the hot case: inside a span that already exists */
    uintptr_t nlo = lo, nhi = hi; uint32_t ver = 0, found;
    do {
        found = 0;
        ++s_visit;
        PG_FOR_RANGE(nlo ? nlo - 1u : 0u, nhi + 1u, b)
            for (int32_t e = s_wix.head[b]; e; ) {
                const int32_t nx = pg_next(&s_wix, e, b);
                WSpan *s = &s_w[e];
                if (s->used && s->visit != s_visit && s->lo <= nhi && nlo <= s->hi) {
                    s->visit = s_visit;
                    if (s->lo < nlo) nlo = s->lo;
                    if (s->hi > nhi) nhi = s->hi;
                    if (s->ver > ver) ver = s->ver;
                    w_release(e); found = 1;
                }
                e = nx;
            }
    } while (found);
    w_new(nlo, nhi, ver + 1u, frame, 1);
}
/* [flo,fhi) was freed: nothing in it is a write target any more.  Spans are trimmed / split / dropped (a split with no free slot keeps the span whole: a superset) */
static void w_subtract(uintptr_t flo, uintptr_t fhi)
{
    if (fhi <= flo || !s_wix.pool || !s_wn) return;
    ++s_visit; const uint32_t stamp = s_visit;
    for (;;) {
        WSpan *s = NULL; int32_t si = 0;
        PG_FOR_RANGE(flo, fhi, b)
            for (int32_t e = s_wix.head[b]; e && !s; e = pg_next(&s_wix, e, b))
                if (s_w[e].used && s_w[e].visit != stamp && s_w[e].lo < fhi && flo < s_w[e].hi) { s = &s_w[e]; si = e; }
        if (!s) break;
        s->visit = stamp;
        if (flo <= s->lo && s->hi <= fhi) { w_release(si); continue; }
        if (s->lo < flo && fhi < s->hi) {
            const uintptr_t ohi = s->hi;
            const int32_t ni = w_new(fhi, ohi, s->ver, s->last, 0);
            if (!ni) continue;                                                              /* no slot: keep the span whole */
            s = &s_w[si];                                                                   /* (w_new does not move the pool, but be explicit) */
            s_w[ni].visit = stamp;
            s->hi = flo; span_drop_bufs(s); w_reindex(si);
            continue;
        }
        if (s->lo < flo) s->hi = flo; else s->lo = fhi;
        span_drop_bufs(s); w_reindex(si);
    }
}
static void w_expire(void)
{
    if (!s_wix.pool || !s_wn) return;
    const uint32_t frame = FRAME();
    for (int32_t i = 1; i <= (int32_t)GXTS_W_CAP; ++i)
#ifndef GXTS_BREAK_NO_EXPIRE
        if (s_w[i].used && (int32_t)(frame - s_w[i].last) > GXTS_W_EXPIRE_FRAMES) { w_release(i); ++s_c.wset_expired; }
#else   /* negative control (host test): a span never leaves W, so the per-frame copy budget is blown */
        (void)i;
#endif
}
/* the ping-pong copy of span `s` for THIS frame's parity: refreshed only if the span's version moved since the last copy into this parity */
static const uint8_t *span_shadow(WSpan *s, uint32_t *n_out)
{
#ifndef GXTS_BREAK_NO_PARITY
    const unsigned par = FRAME() & 1u;
#else   /* negative control (host test): the parity is never flipped, so a copy for frame j+1 overwrites the buffer frame j has not been executed from */
    const unsigned par = 0u;
#endif
    const uint32_t n = (uint32_t)(s->hi - s->lo) + GXTS_W_MARGIN;
    if (s->cap[par] < n) {
        if (s->buf[par]) gxt_free(s->buf[par]);
        s->buf[par] = NULL; s->cap[par] = 0; s->valid[par] = 0;
        const uint32_t c = (n + 63u) & ~63u;
        uint8_t *b = (uint8_t *)malloc(c);
        if (!b && gxt_reclaim("shadow alloc")) { ++s_c.reclaim_retry; b = (uint8_t *)malloc(c); }
        if (!b) { ++s_c.alloc_fail; return NULL; }
        s->buf[par] = b; s->cap[par] = c;
    }
    if (!s->valid[par] || s->bver[par] != s->ver || s->bn[par] != n) {
        memcpy(s->buf[par], (const void *)s->lo, n);
        s->bver[par] = s->ver; s->bn[par] = n; s->valid[par] = 1;
        uint32_t *e = gxt_rec_alloc(GXT_OP_gx_dlk_event, PW + 2u);          /* R's display-list cache must re-hash what is behind the shadow */
        put_ptr(e, s->buf[par]); e[PW] = n; e[PW + 1u] = 1u;
        ++s_c.shadow_n; s_c.shadow_bytes += n;
    }
    *n_out = n;
    return s->buf[par];
}

/* ------------------------------------------------------------------------------------------------------------------------------ copy destinations (what GXCopyTex wrote / will write) */
static struct { uintptr_t lo, hi; } s_cd[GXTS_CD_CAP];
static unsigned s_cdn;
static int cd_overlaps(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < s_cdn; ++i) if (s_cd[i].lo < hi && lo < s_cd[i].hi) return 1;
    return 0;
}
static void cd_add(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < s_cdn; ++i) if (lo >= s_cd[i].lo && lo < s_cd[i].hi) return;      /* the HAL's copyDstRecord: already inside a recorded range */
    if (s_cdn < GXTS_CD_CAP) { s_cd[s_cdn].lo = lo; s_cd[s_cdn].hi = hi; ++s_cdn; }
}
static void cd_release(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < s_cdn; ) if (s_cd[i].lo < hi && lo < s_cd[i].hi) s_cd[i] = s_cd[--s_cdn]; else ++i;
}

/* ------------------------------------------------------------------------------------------------------------------------------ registered movie planes */
/* The HAL identifies a THP movie plane by POINTER EQUALITY with the planes the decoder registered (gx_draw.c yuvPlaneGen / yuvIsMovieY / yuvDecodeIfMovie / yuvTakeEx).  A snapshot has another address, so a
 * snapshotted plane is not recognised on R: no YUV->RGB path, the worker's pre-expanded buffers are never taken, gMovieMaps is never set (MEASURED, Vita3K, same build: `[movie] plane loads` 855 DIRECT, 0 threaded;
 * with snapshots off 846).  So G remembers every pointer the decoder registers and NEVER snapshots (nor pre-seeds RW) an image that is one: it is read live and keeps its identity.  The set is bounded (the HAL
 * keeps 4 sets = 12 pointers) and forgets a range when the game frees it.  LIMIT: planes registered while DIRECT (before a direct -> threaded switch) are not known here (the switch is at the first frame edge).
 * THE AUDIT AND THE PLANES' BYTES.  A plane registered with the worker's PRE-EXPANDED buffer (gx_tex_yuv_register_x, ey/eu/ev non-NULL -- the production path) is never read by R: on its first load after the
 * registration the HAL takes the expanded RGBA buffer (gx_draw.c yuvTakeEx; the TEV arm skips yuvDecodeIfMovie), and later loads hit the texture cache.  Its bytes may change after the load is recorded (the decoder
 * reuses the tile set) and nothing on R reads them, so the audit does NOT hash them (counted audit_skip_movie).  MEASURED (Vita3K, thr_audit, build rt5 before this exemption): audit_bad 846 = every U/V plane
 * load, 0 on anything else.  A plane registered WITHOUT an expanded buffer (gx_tex_yuv_register) IS decoded from the live bytes by R: it stays audited (a real R-5 residual would show). */
#define GXTS_MOV_CAP 64u
static uintptr_t s_mov[GXTS_MOV_CAP];
static uint8_t   s_mov_ex[GXTS_MOV_CAP];                                                       /* 1: registered with the worker's pre-expanded buffer */
static unsigned  s_mov_n, s_mov_i;
static int mov_find(uintptr_t a)
{
    for (unsigned i = 0; i < s_mov_n; ++i) if (s_mov[i] == a) return (int)i;
    return -1;
}
static int mov_has(uintptr_t a) { return mov_find(a) >= 0; }
static int mov_has_ex(uintptr_t a) { const int i = mov_find(a); return i >= 0 && s_mov_ex[i]; }
static void mov_note(const void *p, int ex)
{
    const uintptr_t a = (uintptr_t)p;
    if (!a) return;
    const int i = mov_find(a);
    if (i >= 0) { s_mov_ex[i] = (uint8_t)(ex != 0); return; }                                  /* a re-registration: the latest frame's kind wins */
    if (s_mov_n < GXTS_MOV_CAP) { s_mov[s_mov_n] = a; s_mov_ex[s_mov_n] = (uint8_t)(ex != 0); ++s_mov_n; }
    else { const unsigned k = s_mov_i++ % GXTS_MOV_CAP; s_mov[k] = a; s_mov_ex[k] = (uint8_t)(ex != 0); }   /* full: replace the oldest */
    ++s_c.movie_planes;
}
static void mov_forget(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < s_mov_n; ) {
        if (s_mov[i] >= lo && s_mov[i] < hi) { --s_mov_n; s_mov[i] = s_mov[s_mov_n]; s_mov_ex[i] = s_mov_ex[s_mov_n]; } else ++i;
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------ the image set + snapshots */
typedef struct ImgEnt {
    PgLink lk;
    uintptr_t lo;
    uint32_t size;                         /* level-0 byte size (GXGetTexBufferSize) */
    uint32_t wver;                         /* bumped by every invalidate that overlaps the image */
    uint32_t last;                         /* g_gxt.seq at the last reference */
    uint32_t visit;
    uint8_t  used, rw;                     /* rw: rewritten since it was first referenced */
    uint8_t  fresh;                        /* created by the load being recorded now (the pre-seed rule below looks at it once) */
    uint8_t  valid[2];
    uint8_t *buf[2];
    uint32_t cap[2], bver[2];
} ImgEnt;
static ImgEnt  *s_img;
static PgIdx   s_iix = { .pool = NULL };
static int32_t *s_ifree;
static int     s_inf;
static unsigned s_in;
static int tables_alloc(void)                                            /* 1 = every table exists (idempotent); 0 = out of memory (the caller refuses threaded mode) */
{
    if (!s_w)           s_w = (WSpan *)calloc(GXTS_W_CAP + 1u, sizeof *s_w);
    if (!s_wfree)       s_wfree = (int32_t *)calloc(GXTS_W_CAP, sizeof *s_wfree);
    if (!s_wix.head)    s_wix.head = (int32_t *)calloc(GXTS_PAGE_BUCKETS, sizeof *s_wix.head);
    if (!s_img)         s_img = (ImgEnt *)calloc(GXTS_IMG_CAP + 1u, sizeof *s_img);
    if (!s_ifree)       s_ifree = (int32_t *)calloc(GXTS_IMG_CAP, sizeof *s_ifree);
    if (!s_iix.head)    s_iix.head = (int32_t *)calloc(GXTS_PAGE_BUCKETS, sizeof *s_iix.head);
    return s_w && s_wfree && s_wix.head && s_img && s_ifree && s_iix.head;
}
static void tables_or_die(void)                                          /* unreachable after a clean gxts_refuse_reason; a table-less threaded start would corrupt memory, so say so and stop */
{
    if (tables_alloc()) return;
    printf("[gxt] FATAL: the special tables could not be allocated\n");
    fflush(stdout);
    abort();
}

static void img_init_once(void)
{
    if (s_iix.pool) return;
    tables_or_die();
    s_iix.pool = (uint8_t *)s_img; s_iix.stride = sizeof(ImgEnt);
    memset(s_iix.head, 0, GXTS_PAGE_BUCKETS * sizeof *s_iix.head);
    s_inf = 0;
    for (int32_t i = (int32_t)GXTS_IMG_CAP; i >= 1; --i) s_ifree[s_inf++] = i;
    s_in = 0;
}
static void rec_event(uint32_t op, const void *p, uint32_t n, int has_flag, uint32_t flag);
static int s_thr;                          /* [G] 1 between gxts_threaded_begin and gxts_threaded_end: records may be appended; 0: the HAL is G's again and is called directly */
/* Drop both parities' snapshot buffers of image entry `e`.  The buffers are malloc'd by this module, so the HAL never sees them freed (mm_free_now is what emits gx_tex_release for game memory): without a
 * release the texture cache on R keeps a decoded texture (CDRAM) for every snapshot address ever used, and a recycled address can hit a stale entry.  So the release is RECORDED (in stream order, after the
 * last load that named the buffer) while threaded, and called directly once the ring is off (gxts_threaded_end/begin: R is parked, the HAL is G's again).  Break arm `memhog` = the pre-fix behaviour. */
static void snap_drop(ImgEnt *e)
{
    for (int p = 0; p < 2; ++p) {
        if (!e->buf[p]) continue;
        if (!BRK(GXT_BRK_MEMHOG)) {
            if (s_thr) rec_event(GXT_OP_gx_tex_release, e->buf[p], e->cap[p], 0, 0);
            else __real_gx_tex_release(e->buf[p], e->cap[p]);
            ++s_c.snap_rel;
        }
        gxt_free(e->buf[p]);
        e->buf[p] = NULL; e->cap[p] = 0; e->valid[p] = 0;
    }
}
static void img_release(int32_t i)
{
    ImgEnt *e = &s_img[i];
    pg_del(&s_iix, i);
    snap_drop(e);
    e->used = 0; s_ifree[s_inf++] = i; --s_in;
}
static void img_evict_old(void)
{
    const uint32_t frame = FRAME();
    for (uint32_t win = 64; ; win >>= 1) {
        for (int32_t i = 1; i <= (int32_t)GXTS_IMG_CAP; ++i)
            if (s_img[i].used && (int32_t)(frame - s_img[i].last) > (int32_t)win) { img_release(i); ++s_c.img_evicted; }
        if (s_inf >= (int)(GXTS_IMG_CAP / 8u) || win == 0) break;
    }
    if (!s_inf) { img_release(1); ++s_c.img_evicted; }                                      /* a table that is full of this frame's images: lose one, loudly counted */
}
static int32_t img_ref(uintptr_t image, uint32_t size)
{
    img_init_once();
    const uint32_t b = (uint32_t)((image >> 16) & (GXTS_PAGE_BUCKETS - 1u));
    for (int32_t e = s_iix.head[b]; e; e = pg_next(&s_iix, e, b))
        if (s_img[e].used && s_img[e].lo == image && s_img[e].size == size) { s_img[e].last = FRAME(); return e; }
    if (!s_inf) img_evict_old();
    const int32_t i = s_ifree[--s_inf];
    ImgEnt *e = &s_img[i];
    memset(e, 0, sizeof *e);
    e->used = 1; e->fresh = 1; e->lo = image; e->size = size; e->last = FRAME();
    pg_add(&s_iix, i, image, image + size); ++s_in;
    return i;
}
/* RECENT IMAGE-SIZED INVALIDATES (integration finding, Vita3K thr_audit, 2026-10-20).  An invalidate marks a REFERENCED image RW; one that arrives BEFORE the image's first load finds no member and is
 * forgotten, so a texture the CPU rewrites every frame (the movie planes: decode -> gx_tex_invalidate -> register -> load) is first loaded LIVE, and the decoder's next rewrite then reaches that
 * not-yet-replayed load (docs/53 R-5, "one frame, one image" -- here 13 loads of each chroma plane, audit_bad 26).  So an image-sized invalidate is remembered for GXTS_INV_FRAMES presents, and an image
 * whose FIRST load finds an overlapping one starts out RW (snapshotted from its first load).  Counted as rw_pre. */
#define GXTS_INV_CAP    128u
#ifndef GXTS_INV_MIN
#define GXTS_INV_MIN    4096u    /* texture-sized: 4096 included (a 32x32 RGBA8 texture is exactly that).  Smaller spans are arrays' and colour tables' DCStoreRange; with 512 as the floor they evicted the movie planes' entries
                                  * before the first load (Vita3K thr_audit, audit_bad 10, build rt3).  A texture under 4 KB that the CPU rewrites every frame is NOT covered: the residual R-5 for it stays counted by the audit. */
#endif
#define GXTS_INV_FRAMES 8
static struct { uintptr_t lo, hi; uint32_t frame; } s_inv[GXTS_INV_CAP];
static unsigned s_inv_i;
static void inv_note(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < GXTS_INV_CAP; ++i) if (s_inv[i].hi && s_inv[i].lo == lo && s_inv[i].hi == hi) { s_inv[i].frame = FRAME(); return; }
    s_inv[s_inv_i % GXTS_INV_CAP].lo = lo; s_inv[s_inv_i % GXTS_INV_CAP].hi = hi; s_inv[s_inv_i % GXTS_INV_CAP].frame = FRAME(); ++s_inv_i;
}
static int inv_recent(uintptr_t lo, uintptr_t hi)
{
    for (unsigned i = 0; i < GXTS_INV_CAP; ++i)
        if (s_inv[i].hi && (int32_t)(FRAME() - s_inv[i].frame) <= GXTS_INV_FRAMES && s_inv[i].lo < hi && lo < s_inv[i].hi) return 1;
    return 0;
}
/* the CPU rewrote [lo,hi): every referenced image it overlaps becomes RW */
static void img_invalidate(uintptr_t lo, uintptr_t hi)
{
    if (!s_iix.pool || !s_in) return;
    ++s_visit;
    PG_FOR_RANGE(lo, hi, b)
        for (int32_t e = s_iix.head[b]; e; e = pg_next(&s_iix, e, b)) {
            ImgEnt *x = &s_img[e];
            if (x->used && x->visit != s_visit && x->lo < hi && lo < x->lo + x->size) { x->visit = s_visit; x->rw = 1; ++x->wver; }
        }
}
/* [lo,hi) was freed: images that START inside it are retired (gx_draw.c gx_tex_release), their snapshots with them */
static void img_free_range(uintptr_t lo, uintptr_t hi)
{
    if (!s_iix.pool || !s_in) return;
    ++s_visit;
    PG_FOR_RANGE(lo, hi, b)
        for (int32_t e = s_iix.head[b]; e; ) {
            const int32_t nx = pg_next(&s_iix, e, b);
            ImgEnt *x = &s_img[e];
            if (x->used && x->visit != s_visit && x->lo >= lo && x->lo < hi) { x->visit = s_visit; img_release(e); }
            e = nx;
        }
}
/* the ping-pong snapshot of RW image `x` for THIS frame's parity (refreshed only if the image's version moved since the last copy into this parity) */
static const uint8_t *img_snapshot(ImgEnt *x)
{
    const unsigned par = FRAME() & 1u;
    const uint32_t n = x->size;
    if (x->cap[par] < n) {
        if (x->buf[par]) gxt_free(x->buf[par]);
        x->buf[par] = NULL; x->cap[par] = 0; x->valid[par] = 0;
        const uint32_t c = (n + 63u) & ~63u;
        uint8_t *b = (uint8_t *)malloc(c);
        if (!b && gxt_reclaim("snapshot alloc")) { ++s_c.reclaim_retry; b = (uint8_t *)malloc(c); }   /* frees are deferred to the frame edge: the block we need may be on the retire list */
        if (!b) { ++s_c.alloc_fail; return NULL; }
        x->buf[par] = b; x->cap[par] = c;
    }
    if (!x->valid[par] || x->bver[par] != x->wver) {
        memcpy(x->buf[par], (const void *)x->lo, n);
        x->bver[par] = x->wver; x->valid[par] = 1;
        uint32_t *e = gxt_rec_alloc(GXT_OP_gx_tex_invalidate, PW + 1u);     /* R's texture cache re-decodes the snapshot's address */
        put_ptr(e, x->buf[par]); e[PW] = n;
        ++s_c.snap_n; s_c.snap_bytes += n;
    }
    return x->buf[par];
}

/* ------------------------------------------------------------------------------------------------------------------------------ the HAL's texture object (gx_draw.c PortTexObj; the host test compares these offsets with the real GXInitTexObj bytes) */
typedef struct PortTexObjS {
    void *image;
    void *userData;
    void *rhi;
    u16 width, height;
    u8 fmt, wrap, mipmap;
    u8 tag;
} PortTexObjS;
#define PORT_TEXOBJ_TAG 0xA5
_Static_assert(sizeof(PortTexObjS) <= 32, "PortTexObj must fit the game's 32-byte GXTexObj");
typedef union { PortTexObjS t; uint32_t w[8]; } TexObjBytes;

/* ------------------------------------------------------------------------------------------------------------------------------ G shadows (initialised from the HAL by gxts_threaded_begin) */
static u32  s_scis[4] = { 0, 0, 640, 480 };
static u16  s_served;                      /* the draw-sync token GXReadDrawSync last handed to the game */
static int  s_dst_fmt = -1;
static u16  s_dst_w, s_dst_h;

/* ============================================================================================================================== lifecycle */
static int marker_first_char(const char *name)
{
    char path[160];
    snprintf(path, sizeof path, "%s%s", GXTS_DATA_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    const int c = fgetc(f);
    fclose(f);
    return c == EOF ? 0 : c;
}
static int path_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
int gxts_refuse_reason(char *buf, int cap)
{
    char path[160];
    const char *why = NULL;
    if (rhi_vita_efbZMode && rhi_vita_efbZMode() != 0) why = "efbZMode != 0 (EFB Z copies are not supported on the GXM threaded path)";
    else if (!rhi_vita_efbMode || rhi_vita_efbMode() != 1) why = "efbMode != 1 (threaded mode is built for the FBO copies)";
    else if (marker_first_char("gx_bp.txt") == '0') why = "gx_bp.txt 0 (the break-point delivery gate needs the break point delivered)";
    else if (&mm_guard_mode && mm_guard_mode > 0) why = "mm_guard_mode > 0 (the guard allocator frees outside the retire list)";
    else if (marker_first_char("rt_stats.txt") == '1') why = "rt_stats.txt 1 (rt_timer cannot see R)";
    else if (path_exists(GXTS_JOURNAL_PATH)) why = "STFX00001/JOURNAL present (the draw journal reads backend state from G)";
    else if ((snprintf(path, sizeof path, "%scaptures.txt", GXTS_DATA_DIR), path_exists(path))) why = "captures.txt present (captures read the composite from G)";
    /* phase_stats.txt 1 was refused here until 2026-10-23: the [ph] split subtracted gx_win_times (R's HAL counters) from G's phases.
     * hal/stfx_phase.c no longer reads them while gxt_is_active() (GX-inside = 0: own = inclusive G time), so the split is G-only. */
    else if ((snprintf(path, sizeof path, "%sshot_now.txt", GXTS_DATA_DIR), path_exists(path))) why = "shot_now.txt present at start (a capture request reads backend state from G)";
    else if (!tables_alloc()) why = "the special module's tables (~0.8 MB, allocated lazily) could not be allocated";
    if (!why) return 0;
    if (buf && cap > 0) snprintf(buf, (size_t)cap, "%s", why);
    return 1;
}
void gxts_threaded_begin(void)
{
    w_init_once(); img_init_once();
    for (int32_t i = 1; i <= (int32_t)GXTS_W_CAP; ++i) if (s_w[i].used) w_release(i);
    for (int32_t i = 1; i <= (int32_t)GXTS_IMG_CAP; ++i) if (s_img[i].used) img_release(i);
    s_cdn = 0; s_dst_fmt = -1; s_dst_w = s_dst_h = 0;
    memset(s_inv, 0, sizeof s_inv); s_inv_i = 0;
    s_mov_n = s_mov_i = 0;
    s_thr = 1;
    __real_GXGetScissor(&s_scis[0], &s_scis[1], &s_scis[2], &s_scis[3]);
    s_served = __real_GXReadDrawSync();
    GXT_STORE_REL(&g_gxt.r_ds_token, (uint32_t)s_served);
    GXT_STORE_REL(&g_gxt.r_ds_seq, g_gxt.ds_recorded);
}
void gxts_threaded_end(void)
{
    s_thr = 0;                                            /* from here the HAL is G's again: snapshot releases call it directly */
    const uint32_t tok = GXT_LOAD_ACQ(&g_gxt.r_ds_token);
    /* After this the HAL's own token is what GXReadDrawSync returns.  The game flips on sync == last + 1: one un-served step is consumed by that very read (safe);
     * two or more are skipped for good -- the flip then wedges.  This module cannot make the game read, so the ring must not leave threaded mode in that state. */
    if ((uint16_t)(tok - s_served) >= 2u && (uint16_t)(tok - s_served) < 0x8000u) {
        ++s_c.ds_unserved_end;
        printf("[gxt] WARNING: leaving threaded mode with %u un-served draw-sync step(s) (served %u executed %u)\n", (unsigned)(uint16_t)(tok - s_served), (unsigned)s_served, (unsigned)(uint16_t)tok);
    }
    if (s_wix.pool) for (int32_t i = 1; i <= (int32_t)GXTS_W_CAP; ++i) if (s_w[i].used) w_release(i);
    if (s_iix.pool) for (int32_t i = 1; i <= (int32_t)GXTS_IMG_CAP; ++i) if (s_img[i].used) img_release(i);
    s_cdn = 0;
}
void gxts_frame_edge_g(uint32_t seq)
{
    (void)seq;                                            /* the parity and the clock are g_gxt.seq, read where used */
    w_expire();
    /* image snapshots (2 x up to 256 KB each) are released once the image has not been loaded for a while; the entry itself stays (it is what knows the image is RW) */
    if (s_iix.pool && s_in && !(FRAME() & 15u)) {
        const uint32_t frame = FRAME();
        for (int32_t i = 1; i <= (int32_t)GXTS_IMG_CAP; ++i) {
            ImgEnt *x = &s_img[i];
            if (!x->used || (!x->buf[0] && !x->buf[1]) || (int32_t)(frame - x->last) <= GXTS_IMG_IDLE_FRAMES) continue;
            if (BRK(GXT_BRK_MEMHOG)) continue;                 /* break arm `memhog`: the snapshots of idle images are never released (the memory gate must see it) */
            snap_drop(x);
            ++s_c.snap_idle_freed;
        }
    }
}
int gxts_report(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    const int n = snprintf(buf, (size_t)cap,
        "snap_n=%u snap_bytes=%u shadow_n=%u shadow_bytes=%u snap_over=%u wset=%u wset_hits=%u b8_blocks=%u ds_steps=%u bp_gates=%u bp_held=%u peekz_g=%u peekz_far=%u audit_checked=%u audit_bad=%u"
        " wset_evicted=%u shadow_alloc_fail=%u rw_pre=%u snap_mip=%u snap_movie=%u snap_rel=%u movie_planes=%u fog_mirrored=%u reclaim_retry=%u audit_skip_movie=%u",
        (unsigned)s_c.snap_n, (unsigned)s_c.snap_bytes, (unsigned)s_c.shadow_n, (unsigned)s_c.shadow_bytes, (unsigned)s_c.snap_over, s_wn, (unsigned)s_c.wset_hits,
        (unsigned)s_c.b8_blocks, (unsigned)s_c.ds_steps, (unsigned)s_c.bp_gates, (unsigned)s_c.bp_held, (unsigned)s_c.peekz_g, (unsigned)s_c.peekz_far,
        (unsigned)s_audit_checked, (unsigned)s_audit_bad, (unsigned)s_c.wset_evicted, (unsigned)s_c.alloc_fail, (unsigned)s_c.rw_pre,
        (unsigned)s_c.snap_mip, (unsigned)s_c.snap_movie, (unsigned)s_c.snap_rel, (unsigned)s_c.movie_planes, (unsigned)s_c.fog_mirrored, (unsigned)s_c.reclaim_retry, (unsigned)s_c.audit_skip_movie);
    if (n < 0) { buf[0] = 0; return 0; }
    return n >= cap ? cap - 1 : n;
}
int gxts_audit_bad(void) { return (int)s_audit_bad; }

/* R, before every dispatch while brk != 0.  `dropdraw`: every 50th GXCallDisplayList record is skipped (the accounting gate must see it). */
int gxts_r_filter(uint32_t op)
{
    static uint32_t n;                                                                    /* [R] */
    if ((g_gxt.cfg.brk & GXT_BRK_DROPDRAW) && op == GXT_OP_GXCallDisplayList) return (++n % 50u) == 0;
    return 0;
}
void gxts_r_present_executed(uint32_t seq)
{
    static uint32_t last_bad;                                                             /* [R] */
    const uint32_t bad = s_audit_bad;
    if (bad != last_bad) { printf("[gxt] audit: %u mismatch(es) of %u checks through frame %u\n", (unsigned)bad, (unsigned)s_audit_checked, (unsigned)seq); last_bad = bad; }
}

/* ============================================================================================================================== GXBegin and the display lists */
/* IMM CENSUS (docs/54 2026-10-26, lever (a): what are the ~71 backend immediate draws/frame that cost 8.4 ms of R in the hub?).
 * Marker ux0:data/stairfax/gx_imm_census.txt.  Keyed by the GAME's call site: __wrap_GXBegin TAIL-CALLS this function
 * (`b.w gxts_G_GXBegin`, read in the ELF 2026-10-26), so LR on entry is the game's return address.  The host reader resolves the
 * sites against the build's ELF; a site that resolves to __wrap_GXBegin means the tail call is gone and the census is blind (INVALID).
 * Threaded mode only (direct mode never reaches this function: total 0 in the flush line).  Adds work on G: not a timing boot. */
#define IMMC_N 64
static struct { uintptr_t ra; uint32_t calls, verts, maxv, prims, fmts; } s_immc[IMMC_N];
static uint32_t s_immc_drops, s_immc_list, s_immc_total;
static int s_immc_on = -1;
static int immc_on(void)
{
    if (s_immc_on < 0) { char b[8]; s_immc_on = gxtp_read_marker("gx_imm_census.txt", b, sizeof b) >= 0; }
    return s_immc_on;
}
static __attribute__((noinline)) void immc_note(uintptr_t ra, uint32_t type, uint32_t fmt, uint32_t n)
{
    int i;
    ra &= ~(uintptr_t)1;
    for (i = 0; i < IMMC_N && s_immc[i].ra != 0 && s_immc[i].ra != ra; ++i) {}
    if (i == IMMC_N) { ++s_immc_drops; return; }
    s_immc[i].ra = ra; ++s_immc[i].calls; s_immc[i].verts += n; if (n > s_immc[i].maxv) s_immc[i].maxv = n;
    s_immc[i].prims |= 1u << ((type >> 3) & 15u); s_immc[i].fmts |= 1u << (fmt & 7u);
    ++s_immc_total;
}
void gxts_imm_census_flush(int arm, unsigned frames);
void gxts_imm_census_flush(int arm, unsigned frames)
{
    int i, used = 0;
    if (!immc_on()) return;
    for (i = 0; i < IMMC_N && s_immc[i].ra != 0; ++i) {
        ++used;
        if (!s_immc[i].calls) continue;
        printf("[immc] arm %d site %08lx /%u fr: calls %u verts %u maxv %u prims %04x fmts %02x\n", arm, (unsigned long)s_immc[i].ra, frames,
               (unsigned)s_immc[i].calls, (unsigned)s_immc[i].verts, (unsigned)s_immc[i].maxv, (unsigned)s_immc[i].prims, (unsigned)s_immc[i].fmts);
        s_immc[i].calls = s_immc[i].verts = s_immc[i].maxv = s_immc[i].prims = s_immc[i].fmts = 0;
    }
    printf("[immc] arm %d /%u fr: total calls %u | in display lists %u | slots %d/%d drops %u\n", arm, frames, (unsigned)s_immc_total,
           (unsigned)s_immc_list, used, IMMC_N, (unsigned)s_immc_drops);   /* total 0 with the marker on = not threaded (only mode 3 reaches here) */
    s_immc_total = s_immc_list = 0;
}
void gxts_G_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    if (__builtin_expect(s_immc_on != 0, 0) && immc_on()) {
        if (g_gxt.list_open) ++s_immc_list;
        else immc_note((uintptr_t)__builtin_return_address(0), (uint32_t)type, (uint32_t)vtxfmt, nverts);
    }
    if (g_gxt.list_open) { gxt_push_u8((uint32_t)type | (uint32_t)vtxfmt); gxt_push_u16(nverts); return; }
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXBegin, 1);
    p[0] = ((uint32_t)type & 0xFFu) | (((uint32_t)vtxfmt & 0xFFu) << 8) | ((uint32_t)nverts << 16);
}
void gxts_R_GXBegin(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    __real_GXBegin((GXPrimitive)(p[0] & 0xFFu), (GXVtxFmt)((p[0] >> 8) & 0xFFu), (u16)(p[0] >> 16));
}
void gxts_G_GXBeginDisplayList(void *list, u32 size)
{
    gxt_chunk_close();
    (void)gxt_rec_alloc(GXT_OP_ABANDON, 0);                /* R: an unfinished primitive is abandoned, as on hardware (gx_draw.c:2676) */
    g_gxt.list_buf = (uint8_t *)list; g_gxt.list_cap = size;
    g_gxt.list_open = 1; g_gxt.list_overflow = 0;
    g_gxt.push_cur = (uint8_t *)list; g_gxt.push_end = (uint8_t *)list + size;
}
void gxts_R_GXBeginDisplayList(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; __real_GXBeginDisplayList(NULL, 0); }   /* never recorded under this op (GXT_OP_ABANDON is); equivalent if it ever arrives */
u32 gxts_G_GXEndDisplayList(void)
{
    if (!g_gxt.list_open) return 0;
    const u32 cap = g_gxt.list_cap;
    u32 len = g_gxt.list_overflow ? cap : (u32)(g_gxt.push_cur - g_gxt.list_buf);
#ifndef GXTS_BREAK_NO_PAD
    while ((len & 31u) && len < cap) g_gxt.list_buf[len++] = 0;                          /* SDK contract: rounded up to 32 bytes with NOP padding (gx_draw.c:2682) */
#endif
    const u32 n = (g_gxt.list_overflow || (len & 31u)) ? 0u : len;
    g_gxt.list_open = 0; g_gxt.list_overflow = 0;
    g_gxt.push_cur = g_gxt.push_end = NULL;
    return n;
}
void gxts_R_GXEndDisplayList(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; }      /* G-resident: never recorded */

/* ============================================================================================================================== scissor, copy clear, copy destination */
void gxts_G_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    s_scis[0] = left; s_scis[1] = top; s_scis[2] = wd; s_scis[3] = ht;
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetScissor, 4);
    p[0] = left; p[1] = top; p[2] = wd; p[3] = ht;
}
void gxts_R_GXSetScissor(const uint32_t *p, uint32_t nw) { (void)nw; __real_GXSetScissor(p[0], p[1], p[2], p[3]); }
void gxts_G_GXGetScissor(u32 *left, u32 *top, u32 *wd, u32 *ht)
{
    if (left) *left = s_scis[0];
    if (top) *top = s_scis[1];
    if (wd) *wd = s_scis[2];
    if (ht) *ht = s_scis[3];
}
void gxts_R_GXGetScissor(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; }          /* G-resident */
void gxts_G_GXSetCopyClear(GXColor clear_clr, u32 clear_z)
{
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetCopyClear, 2);
    memcpy(&p[0], &clear_clr, 4); p[1] = clear_z;
    if (__real_vi_set_clear_color) __real_vi_set_clear_color(clear_clr.r / 255.0f, clear_clr.g / 255.0f, clear_clr.b / 255.0f);   /* gx_draw.c:2772, on G: viOpenFrame reads gClearRGB here */
}
void gxts_R_GXSetCopyClear(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    GXColor c; memcpy(&c, &p[0], 4);
    __real_GXSetCopyClear(c, p[1]);                        /* its vi_set_clear_color call is dropped on R by the E wrapper */
}
void gxts_G_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap)
{
    s_dst_w = wd; s_dst_h = ht; s_dst_fmt = (int)fmt;
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetTexCopyDst, 3);
    p[0] = (uint32_t)wd | ((uint32_t)ht << 16); p[1] = (uint32_t)fmt; p[2] = (uint32_t)mipmap;
}
void gxts_R_GXSetTexCopyDst(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    __real_GXSetTexCopyDst((u16)(p[0] & 0xFFFFu), (u16)(p[0] >> 16), (GXTexFmt)p[1], (GXBool)p[2]);
}
void gxts_G_GXCopyTex(void *dest, GXBool clear)
{
    const int b8 = dest && s_dst_fmt == 0x2A;                                              /* GX_CTF_B8: the game blurs this texture on the CPU (newshadows.c boxBlurTexture) */
    if (dest) {
        const u32 n = __real_GXGetTexBufferSize(s_dst_w, s_dst_h, (u32)s_dst_fmt, 0, 0);
        cd_add((uintptr_t)dest, (uintptr_t)dest + (n ? n : 32u));
    }
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXCopyTex, PW + 1u);
    put_ptr(p, dest); p[PW] = ((uint32_t)clear & 0xFFu) | (b8 ? 0x100u : 0u);
    if (b8) { ++s_c.b8_blocks; gxt_wait_tail(gxt_mark(), "b8-copy"); }                    /* R-1: R writes `dest` below while G is parked here */
}
void gxts_R_GXCopyTex(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    __real_GXCopyTex(get_ptr(p), (GXBool)(p[PW] & 0xFFu));
    if (p[PW] & 0x100u) gxt_r_mark_done();                                                 /* the B8 copy: G is waiting for the tail to pass this record */
}

/* ============================================================================================================================== texture objects */
void gxts_G_GXInitTexObjLOD(GXTexObj *obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso)
{
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXInitTexObjLOD, 16);
    if (obj) memcpy(p, obj, 32); else memset(p, 0, 32);                                    /* the ENTRY object: R's body reads image / mipmap / tag from it */
    p[8] = (uint32_t)min_filt; p[9] = (uint32_t)mag_filt; p[10] = f2u(min_lod); p[11] = f2u(max_lod); p[12] = f2u(lod_bias);
    p[13] = (uint32_t)bias_clamp; p[14] = (uint32_t)do_edge_lod; p[15] = (uint32_t)max_aniso;
#ifndef GXTS_BREAK_NO_LOD_G
    if (obj) {                                                                             /* THEN the G half: the game reads the filter bits back through its own object */
        TexObjBytes u; memcpy(u.w, obj, 32);
        if (u.t.tag == PORT_TEXOBJ_TAG) {
            u.t.mipmap = (u8)((u.t.mipmap & 1u) | ((((unsigned)min_filt + 1u) & 7u) << 1) | ((((unsigned)mag_filt + 1u) & 7u) << 4));
            memcpy(obj, u.w, 32);
        }
    }
#endif
}
void gxts_R_GXInitTexObjLOD(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    TexObjBytes u; memcpy(u.w, p, 32);                                                     /* a scratch copy: R never writes the game's object (R-1) */
    __real_GXInitTexObjLOD((GXTexObj *)&u, (GXTexFilter)p[8], (GXTexFilter)p[9], u2f(p[10]), u2f(p[11]), u2f(p[12]), (GXBool)p[13], (GXBool)p[14], (GXAnisotropy)p[15]);
}
static void load_tex_g(GXTexObj *obj, GXTexMapID id)
{
    if (__real_os_ext_poll) __real_os_ext_poll();                                         /* the real body's first statement; a baton operation, so it runs here and is dropped on R */
    TexObjBytes u;
    if (obj) memcpy(u.w, obj, 32); else memset(u.w, 0, 32);
    uint32_t alen = 0, ahash = 0;
    void *scribble_src = NULL; uint32_t scribble_avail = 0;
    if (u.t.tag == PORT_TEXOBJ_TAG && u.t.image) {
        const uint32_t size0 = __real_GXGetTexBufferSize(u.t.width, u.t.height, u.t.fmt, 0, 0);
        void *image = u.t.image;
        if (size0) {
            const uintptr_t lo = (uintptr_t)image;
            const int cdst = cd_overlaps(lo, lo + size0);
            ImgEnt *x = &s_img[img_ref(lo, size0)];
            const int movie = !BRK(GXT_BRK_MOVIESNAP) && mov_has(lo);                          /* a registered movie plane keeps its identity: never snapshotted, never pre-seeded */
            if (x->fresh) {                                                                     /* first sight of this image: was the CPU already rewriting it? */
                x->fresh = 0;
#ifndef GXTS_BREAK_NO_PRESEED
                if (!movie && !x->rw && inv_recent(lo, lo + size0)) { x->rw = 1; ++x->wver; ++s_c.rw_pre; }
#endif
            }
#ifndef GXTS_BREAK_NO_SNAPSHOT
            if (x->rw && !BRK(GXT_BRK_SNAPOFF) && !cdst) {
                if (movie) {
                    ++s_c.snap_movie;                                                           /* read live (see the movie-plane note above) */
                } else if ((u.t.mipmap & 1u) && !BRK(GXT_BRK_MIPSNAP)) {
                    ++s_c.snap_mip;                                                             /* read live: a snapshot is level 0 only, and the HAL's authored chain + LOD bias are keyed by the original address */
                } else if (size0 > GXT_SNAP_CAP_BYTES) {
                    ++s_c.snap_over;
                    if (s_c.snap_over_logged++ < 8) printf("[gxt] image %p (%u bytes) was rewritten after a load but exceeds the %u-byte snapshot cap: read live (counted snap_over)\n", image, (unsigned)size0, (unsigned)GXT_SNAP_CAP_BYTES);
                } else {
                    const uint8_t *snap = img_snapshot(x);
                    if (snap) u.t.image = (void *)snap;
                }
            }
#endif
#ifndef GXTS_BREAK_AUDIT_MOVIE
            const int audit_skip = movie && mov_has_ex(lo);                                     /* R never reads a pre-expanded plane's bytes (see the movie-plane note) */
#else   /* negative control (host test): the movie planes are audited like any image */
            const int audit_skip = 0;
#endif
            if (!cdst && g_gxt.cfg.audit && !audit_skip) {
                alen = size0 <= GXTS_AUDIT_IMG_WHOLE ? size0 : GXTS_AUDIT_WIN;
                ahash = fnv1a(u.t.image, alen);
            } else if (audit_skip && g_gxt.cfg.audit) ++s_c.audit_skip_movie;
            if (u.t.image == image) { scribble_src = image; scribble_avail = size0; }
        }
    }
    const uint32_t nw = alen ? 12u : 9u;
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXLoadTexObj, nw);
    memcpy(p, u.w, 32); p[8] = (uint32_t)id;
    if (alen) { p[9] = ahash; p[10] = alen; p[11] = g_gxt.seq; }
    scribble_maybe(scribble_src, scribble_avail);
}
void gxts_G_GXLoadTexObj(GXTexObj *obj, GXTexMapID id) { load_tex_g(obj, id); }
void gxts_G_GXLoadTexObjPreLoaded(GXTexObj *obj, GXTexRegion *region, GXTexMapID id) { (void)region; load_tex_g(obj, id); }   /* a preload is nothing; the load is an ordinary load (gx_draw.c:2613) */
void gxts_R_GXLoadTexObj(const uint32_t *p, uint32_t nw)
{
    TexObjBytes u; memcpy(u.w, p, 32);                                                     /* the HAL writes t->rhi into the object: it gets a scratch (R-1) */
    if (nw >= 12u) audit_check("image", u.t.image, p[10], p[9], p[11]);
    __real_GXLoadTexObj((GXTexObj *)&u, (GXTexMapID)p[8]);
}
void gxts_R_GXLoadTexObjPreLoaded(const uint32_t *p, uint32_t nw) { gxts_R_GXLoadTexObj(p, nw); }

/* ============================================================================================================================== vertex arrays (P2') */
void gxts_G_GXSetArray(GXAttr attr, void *base_ptr, u8 stride)
{
    const uint8_t *use = (const uint8_t *)base_ptr;
    uint32_t avail = 0; int shadowed = 0;
    /* A/B INSTRUMENT (harness only): `snapnrm` / `snapall` shadow arrays that nothing announced, over a fixed 8 KB window, re-copied at every call.  The 2026-10-21 hub run is pixel-identical to DIRECT in
     * lockstep and 358 px off at AHEAD 1: some CPU-written data the game never announces is read live by R.  These words say which kind (normals / any array). */
    const int dbg_all = base_ptr && (BRK(GXT_BRK_SNAPALL) || (BRK(GXT_BRK_SNAPNRM) && attr == GX_VA_NRM));
#ifndef GXTS_BREAK_NO_P2
    if (base_ptr && (s_wn || dbg_all) && !BRK(GXT_BRK_SNAPOFF)) {
#else   /* negative control (host test): policy P2' compiled out -- every array is read live */
    if (0) {
#endif
        WSpan *s = w_find((uintptr_t)base_ptr);
        if (dbg_all) {
            if (!s) { w_write((uintptr_t)base_ptr, (uintptr_t)base_ptr + 8192u); s = w_find((uintptr_t)base_ptr); }
            else { ++s->ver; s->last = FRAME(); }
        }
        if (s) {
            ++s_c.wset_hits;
            if (s->hi - s->lo > GXT_SNAP_CAP_BYTES) {
                ++s_c.snap_over;
                if (s_c.snap_over_logged++ < 8) printf("[gxt] array span %p (%u bytes) exceeds the %u-byte shadow cap: read live (counted snap_over)\n", (void *)s->lo, (unsigned)(s->hi - s->lo), (unsigned)GXT_SNAP_CAP_BYTES);
                avail = (uint32_t)(s->hi + GXTS_W_MARGIN - (uintptr_t)base_ptr);
            } else {
                uint32_t n = 0;
                const uint8_t *sh = span_shadow(s, &n);
                if (sh) { const uint32_t off = (uint32_t)((uintptr_t)base_ptr - s->lo); use = sh + off; avail = n - off; shadowed = 1; }
            }
        }
    }
    uint32_t alen = 0, ahash = 0;
    if (g_gxt.cfg.audit && use) {
        if (!shadowed && !avail) avail = live_window((uintptr_t)use, stride);
        alen = avail < GXTS_AUDIT_WIN ? avail : GXTS_AUDIT_WIN;
        ahash = fnv1a(use, alen);
    }
    const uint32_t nw = 2u + PW + (alen ? 3u : 0u);
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetArray, nw);
    p[0] = (uint32_t)attr; put_ptr(&p[1], use); p[1u + PW] = (uint32_t)stride;
    if (alen) { p[2u + PW] = ahash; p[3u + PW] = alen; p[4u + PW] = g_gxt.seq; }
    if (!shadowed) scribble_maybe((void *)use, avail ? avail : 16u);
}
void gxts_R_GXSetArray(const uint32_t *p, uint32_t nw)
{
    const void *base = get_ptr(&p[1]);
    if (nw >= 5u + PW) audit_check("array", base, p[3u + PW], p[2u + PW], p[4u + PW]);
    __real_GXSetArray((GXAttr)p[0], (void *)base, (u8)p[1u + PW]);
}

/* ============================================================================================================================== ordered events (feed W and the image set, then record in order) */
static void rec_event(uint32_t op, const void *p, uint32_t n, int has_flag, uint32_t flag)
{
    uint32_t *r = gxt_rec_alloc(op, PW + 1u + (has_flag ? 1u : 0u));
    put_ptr(r, p); r[PW] = n;
    if (has_flag) r[PW + 1u] = flag;
}
void gxts_G_gx_dlk_event(const void *p, size_t n, int isWrite)
{
    if (!p || !n) return;                                                                   /* the HAL ignores these too (gx_draw.c:1507) */
    if (isWrite) w_write((uintptr_t)p, (uintptr_t)p + n);
    else w_subtract((uintptr_t)p, (uintptr_t)p + n);
    rec_event(GXT_OP_gx_dlk_event, p, (uint32_t)n, 1, isWrite ? 1u : 0u);
}
void gxts_R_gx_dlk_event(const uint32_t *p, uint32_t nw) { (void)nw; __real_gx_dlk_event(get_ptr(p), (size_t)p[PW], (int)p[PW + 1u]); }
void gxts_G_gx_tex_invalidate(const void *addr, u32 n)
{
    if (addr) {
        const uintptr_t lo = (uintptr_t)addr, hi = lo + (n ? n : 1u);
        if (n && n <= GXTS_W_INVAL_MAX) w_write(lo, hi);                                   /* DCStoreRange / DCFlushRange on a small span: a CPU write the GPU must see */
        if (n >= GXTS_INV_MIN) inv_note(lo, hi);
        img_invalidate(lo, hi);
    }
    rec_event(GXT_OP_gx_tex_invalidate, addr, n, 0, 0);
}
void gxts_R_gx_tex_invalidate(const uint32_t *p, uint32_t nw) { (void)nw; __real_gx_tex_invalidate(get_ptr(p), p[PW]); }
void gxts_G_gx_tex_release(const void *addr, u32 n)
{
    if (addr) {
        const uintptr_t lo = (uintptr_t)addr, hi = lo + (n ? n : 1u);
        img_free_range(lo, hi);
        cd_release(lo, hi);
        mov_forget(lo, hi);
    }
    rec_event(GXT_OP_gx_tex_release, addr, n, 0, 0);
}
void gxts_R_gx_tex_release(const uint32_t *p, uint32_t nw) { (void)nw; __real_gx_tex_release(get_ptr(p), p[PW]); }

/* movie planes: G remembers the pointers (so load_tex_g leaves them alone), then records them by value; R registers them with the HAL exactly as the decoder passed them */
void gxts_G_gx_tex_yuv_register(const void *y, const void *u, const void *v, int w, int h)
{
    mov_note(y, 0); mov_note(u, 0); mov_note(v, 0);
    uint32_t *p = gxt_rec_alloc(GXT_OP_gx_tex_yuv_register, 3u * PW + 2u);
    put_ptr(&p[0], y); put_ptr(&p[PW], u); put_ptr(&p[2u * PW], v); p[3u * PW] = (uint32_t)w; p[3u * PW + 1u] = (uint32_t)h;
}
void gxts_R_gx_tex_yuv_register(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    __real_gx_tex_yuv_register(get_ptr(&p[0]), get_ptr(&p[PW]), get_ptr(&p[2u * PW]), (int)p[3u * PW], (int)p[3u * PW + 1u]);
}
void gxts_G_gx_tex_yuv_register_x(const void *y, const void *u, const void *v, int w, int h, uint8_t *ey, uint8_t *eu, uint8_t *ev)
{
    mov_note(y, ey != NULL); mov_note(u, eu != NULL); mov_note(v, ev != NULL);
    uint32_t *p = gxt_rec_alloc(GXT_OP_gx_tex_yuv_register_x, 6u * PW + 2u);
    put_ptr(&p[0], y); put_ptr(&p[PW], u); put_ptr(&p[2u * PW], v); put_ptr(&p[3u * PW], ey); put_ptr(&p[4u * PW], eu); put_ptr(&p[5u * PW], ev);
    p[6u * PW] = (uint32_t)w; p[6u * PW + 1u] = (uint32_t)h;
}
void gxts_R_gx_tex_yuv_register_x(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    __real_gx_tex_yuv_register_x(get_ptr(&p[0]), get_ptr(&p[PW]), get_ptr(&p[2u * PW]), (int)p[6u * PW], (int)p[6u * PW + 1u],
                                 (uint8_t *)get_ptr(&p[3u * PW]), (uint8_t *)get_ptr(&p[4u * PW]), (uint8_t *)get_ptr(&p[5u * PW]));
}

/* GXSetFog: R replays it into the HAL's private colour (N1, gx_draw.c); G mirrors the colour into the GAME's gFogColor at the point of the call, which is exactly when DIRECT's GXSetFog wrote it.  The game reads
 * gFogColor back (intersect_render.c:149 _gxSetFogParams feeds it into the next GXSetFog; 168-175, 1209, 4100, 4259 ... use it as a tint): without the mirror a threaded run reads a colour no GXSetFog has
 * updated.  NOT mirrored: a write the GAME itself makes to gFogColor between a GXSetFog and a draw still reaches DIRECT's draw (it reads the same variable) and not R's -- retail has no such path (retail's
 * GXSetFog never touched the game's variable); said here, measured by the pixel gate, not hidden.  Break arm `nofogmirror` = the pre-fix behaviour. */
void gxts_G_GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color)
{
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetFog, 6u);
    p[0] = (uint32_t)type; p[1] = f2u(startz); p[2] = f2u(endz); p[3] = f2u(nearz); p[4] = f2u(farz);
    memcpy(&p[5], &color, sizeof color);
#ifndef GXTS_BREAK_NO_FOGMIRROR
    if (&gFogColor && !BRK(GXT_BRK_NOFOGMIRROR)) { gFogColor = color; ++s_c.fog_mirrored; }
#endif
}
void gxts_R_GXSetFog(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    GXColor c; memcpy(&c, &p[5], sizeof c);
    __real_GXSetFog((GXFogType)p[0], u2f(p[1]), u2f(p[2]), u2f(p[3]), u2f(p[4]), c);
}

/* ============================================================================================================================== GXPeekZ */
static int s_pz_mode = -1;   /* G: peekz.txt, read once */
void gxts_G_GXPeekZ(u16 x, u16 y, u32 *z)
{
    /* 2026-10-27 (docs/54 "first hand-play" item 2): answered ON G from M's depth memory.  It was a constant far plane here, so threaded mode drew every
     * glow unoccluded (lights through walls).  NOT the HAL's body: that reaches rhi_vita_peekZ_sync (peekz.txt 2: vitaGL flush + glFinish), R's state --
     * gxt_lint (ii) refused it (g_tb, g_tevp).  rhi_gxm_peekZ is the read alone: b_md.p and g_gx.rw / vh / rh / up / dead / torn_down, all fixed after init,
     * plus its own counter -- checked BY HAND (the lint's G closure does not include gxts_G_* handlers).  peekz.txt as the HAL reads it: 0 / absent = far
     * plane; 1 or 2 = the memory as it stands (2's sync is not served on G).  Up to a frame older than DIRECT's answer (G runs AHEAD = 1 of R), which the
     * game's probes tolerate (they fade, docs/47:4628).  Not recorded: R has nothing to do (gxts_R_GXPeekZ is a no-op). */
    if (s_pz_mode < 0) { const int c = marker_first_char("peekz.txt"); s_pz_mode = c == '1' || c == '2'; }
    unsigned v = 0xFFFFFFu;
    if (s_pz_mode && rhi_gxm_peekZ((int)x, (int)y, &v, NULL)) ++s_c.peekz_g; else { v = 0xFFFFFFu; ++s_c.peekz_far; }
    if (z) *z = v;
}
void gxts_R_GXPeekZ(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; }                  /* G-resident */

/* ============================================================================================================================== draw sync and the break point */
void gxts_G_GXSetDrawSync(u16 token)
{
    const uint32_t seq = ++g_gxt.ds_recorded;
    uint32_t *p = gxt_rec_alloc(GXT_OP_GXSetDrawSync, 2);
    p[0] = (uint32_t)token; p[1] = seq;
    if (!BRK(GXT_BRK_NOFLUSH)) gxt_publish();                                               /* videoSwapFrameBuffers polls the token on G: R must see this record without waiting for 4 KB */
}
void gxts_R_GXSetDrawSync(const uint32_t *p, uint32_t nw)
{
    (void)nw;
    const uint32_t token = p[0], seq = p[1];
    __real_GXSetDrawSync((u16)token);
    /* break arm jump2: the token is published only at even sequence numbers, so it advances in jumps of two (never past what R executed) */
    if (!BRK(GXT_BRK_JUMP2) || (seq & 1u) == 0u) GXT_STORE_REL(&g_gxt.r_ds_token, token);
    GXT_STORE_REL(&g_gxt.r_ds_seq, seq);
    gxt_r_mark_done();
}
u16 gxts_G_GXReadDrawSync(void)
{
    const uint16_t tok = (uint16_t)GXT_LOAD_ACQ(&g_gxt.r_ds_token);
    if (BRK(GXT_BRK_NOSTEP)) return tok;                                                    /* docs/53 revision 1: hand out the executed token itself */
    const uint16_t d = (uint16_t)(tok - s_served);
    if (d >= 1u && d < 0x8000u) { s_served = (u16)(s_served + 1u); ++s_c.ds_steps; }        /* ONE step per read: videoSwapFrameBuffers flips only on sync == last + 1 (pi_dolphin.c:4411) */
    return s_served;
}
void gxts_R_GXReadDrawSync(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; }          /* G-resident */
int gxts_G_gx_breakpoint_deliver(void)
{
    int go = 1;
    if (!BRK(GXT_BRK_BP_EARLY)) {
        int32_t count = -1;
        if (Queue_GetCount && gVideoFlipQueue) count = (int32_t)Queue_GetCount((void *)gVideoFlipQueue);
        /* the head flip token belongs to draw sync number ds_recorded - count + 1; without the queue, wait for R to be fully caught up */
        const uint32_t head_seq = count >= 0 ? g_gxt.ds_recorded - (uint32_t)count + 1u : g_gxt.ds_recorded;
        go = (int32_t)(GXT_LOAD_ACQ(&g_gxt.r_ds_seq) - head_seq) >= 0;
    }
    if (!go) { ++s_c.bp_held; return 1; }                                                   /* held: the next retrace re-fires, the game's callback unchanged */
    ++s_c.bp_gates;
    return __real_gx_breakpoint_deliver();
}
void gxts_R_gx_breakpoint_deliver(const uint32_t *p, uint32_t nw) { (void)p; (void)nw; }   /* G-resident */
