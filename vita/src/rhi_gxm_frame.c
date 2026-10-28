/* rhi_gxm_frame.c - the FRAME module of the native GXM backend, milestone M2 (design: host/engine/GXM_M2.md sections 3-5, 10; contract: rhi_gxm_int.h).
 *
 * OWNS: g_gx; the init / shutdown ORDER; sceGxmInitialize, the context + its rings, the shader patcher's memory; the GPU memory ARENAS (gxm_blk_*);
 * the per-frame VERTEX STAGING RING (gxm_ring.h); scene fences (the ONLY sceGxmEndScene) and DEFERRED destruction; the display callback;
 * gxm_log / gxm_chk / gxm_abort; the "[gxm] frame N ..." and "[gxm-acct]" / "[gxm-mem]" lines; rhi_gxm_gx_report.
 *
 * WHAT IS NOT HERE: any draw, any scene begin, any texture, any program. Those are the other four modules.
 *
 * DECISIONS THAT ARE NOT IN THE CONTRACT (each is also in the hand-over notes):
 *  - g_gx.up is set as soon as the CORE (context, patcher, fences, ring) is complete, i.e. BEFORE the other modules' init runs: they may call gxm_dead()
 *    or any frame function from their own init (the target's smoke scene does). A failed init clears it again.
 *  - gxm_blk_alloc accepts kind 2 (GXM_MEM_CDRAM_STRICT, defined here): CDRAM, and NEVER falls back to LPDDR (render surfaces). The header has no flag
 *    for it; kind 0 falls back (counted).
 *  - a failed sceGxmEndScene does NOT advance nval and does not tell the ring (no notification was queued, a later wait would otherwise time out).
 *  - with g_gx.fence_finish (the smoke test's fallback) the deferred queue retires on the DISPLAY CALLBACK count instead of the notification: an entry
 *    deferred in frame N is safe once the display scene of frame N (queue entry queued+1) has reached the callback.
 *  - g_gx.queued is advanced HERE: gxm_frame_present_end(presented_ok) with presented_ok != 0 means "the display scene ran AND (unless the nopresent arm)
 *    the display-queue add succeeded". Under the nopresent arm the same event counts as present_skipped instead.
 *  - the M1 periodic line's tex_create / mesh_create / efb_copy / readback / clears / clear_rects / state_calls / peekz come from the OTHER modules'
 *    report tokens (same key names). A key no module printed is printed as 0 in the periodic line (the checker's regex needs a number) and counted in
 *    the tok_missing= token, so a missing token can never pass for a real zero.
 */
#include "rhi_gxm_int.h"
#include <psp2/kernel/processmgr.h>
/* The ring's SHADOW MODEL (independent overlap check) holds one entry per live allocation. 256 (the vita-gxmprobe default) was saturated in the first gameplay run on Vita3K
 * (ring_shadow_full 26,570 of ~100k allocs: a scene stages thousands of small draws); a saturated instrument checks nothing, so it is sized for the ring: 2 MB / 252 B
 * (the smallest draw, 3 TEV vertices) = 8,322 live allocations. The peak is printed (ring_shadow_peak) so a near-full table is visible before it drops. */
#define RING_MAX_SHADOW 9000
#include "gxm_ring.h"
#include "rhi_gxm_cap.h"

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>   /* sceKernelGetCpuId: the poller self-check */
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GXM_MEM_CDRAM_STRICT 2        /* local extension of the kind argument: CDRAM only, no LPDDR fallback (render surfaces) */

#define ARM_FILE        "ux0:data/stairfax/gxm_arm.txt"
#define RINGS_FILE      "ux0:data/stairfax/gxm_rings.txt"
#define COPYFILTER_FILE "ux0:data/stairfax/copyfilter_off.txt"
#define NOTIF_SLOT_V    100           /* notification region slots (rhi_gxm.c M1 used 100 for its single fence) */
#define NOTIF_SLOT_F    101
#define SMOKE_MS        3000u
#define PERIOD          60u           /* presents between periodic lines */
#define MEM_PERIOD      600u

GxmCore g_gx;

/* ====================================================================================================================================== log */
void gxm_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
}

int gxm_chk(int r, const char *what, const char *file, int line)
{
    if (r != 0) {
        ++g_gx.nonzero;
        if (g_gx.nonzero <= 32 || (g_gx.nonzero & 1023) == 0) {
            const char *b = file ? file : "?", *s;
            if ((s = strrchr(b, '/')) != NULL) b = s + 1;
            if ((s = strrchr(b, '\\')) != NULL) b = s + 1;
            gxm_log("[gxm] RC 0x%08X  %s  (%s:%d) [nonzero so far %d]", (unsigned) r, what ? what : "?", b, line, g_gx.nonzero);
        }
    }
    return r;
}

static unsigned c_abort_repeat;      /* gxm_abort calls after the first (reported: abort_repeat=) */
void gxm_abort(const char *fmt, ...)
{
    if (g_gx.dead) { ++c_abort_repeat; return; }
    g_gx.dead = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_gx.dead_why, sizeof g_gx.dead_why, fmt, ap);
    va_end(ap);
    gxm_log("[gxm] ABORT: %s", g_gx.dead_why);
}

/* bounded snprintf appender for the counter lines */
static void bapp(char *b, int cap, int *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void bapp(char *b, int cap, int *n, const char *fmt, ...)
{
    if (*n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(b + *n, (size_t) (cap - *n), fmt, ap);
    va_end(ap);
    if (w < 0) { b[*n] = 0; return; }
    *n += (w >= cap - *n) ? (cap - *n - 1) : w;
}

/* ====================================================================================================================================== free list
 * The arena sub-allocator, pure host code between the FL markers (the host test extracts exactly this region, so it must stay free of Vita headers).
 * Extents are kept sorted by offset; a free coalesces with both neighbours; a free that overlaps a free extent (double free) or leaves the arena is REFUSED
 * and changes nothing. fl_alloc aligns the ABSOLUTE address (base + offset), so an arena base need not be aligned to the request. */
/*FL_BEGIN*/
typedef struct { uint32_t off, len; } FlExt;
typedef struct { FlExt *e; int n, cap; uint32_t total; } FreeList;

static int fl_init(FreeList *f, uint32_t total)
{
    f->e = (FlExt *) malloc(8 * sizeof *f->e);
    if (!f->e) { f->n = f->cap = 0; f->total = 0; return -1; }
    f->cap = 8; f->n = total ? 1 : 0; f->total = total;
    f->e[0].off = 0; f->e[0].len = total;
    return 0;
}
static void fl_destroy(FreeList *f) { free(f->e); f->e = NULL; f->n = f->cap = 0; f->total = 0; }
static int fl_room(FreeList *f)                       /* room for one more extent; 0 ok, -2 host OOM (nothing changed) */
{
    if (f->n < f->cap) return 0;
    FlExt *ne = (FlExt *) realloc(f->e, (size_t) f->cap * 2 * sizeof *ne);
    if (!ne) return -2;
    f->e = ne; f->cap *= 2;
    return 0;
}
static void fl_remove(FreeList *f, int i)
{
    for (int k = i; k + 1 < f->n; ++k) f->e[k] = f->e[k + 1];
    --f->n;
}
static void fl_insert(FreeList *f, int i, uint32_t off, uint32_t len)   /* room already ensured */
{
    for (int k = f->n; k > i; --k) f->e[k] = f->e[k - 1];
    f->e[i].off = off; f->e[i].len = len;
    ++f->n;
}
/* first fit. 0 ok (*out_off set); -1 no fit / bad arguments; -2 host OOM (nothing changed) */
static int fl_alloc(FreeList *f, uintptr_t base, uint32_t size, uint32_t align, uint32_t *out_off)
{
    if (!size || !align || (align & (align - 1u))) return -1;
    for (int i = 0; i < f->n; ++i) {
        const uint32_t s = f->e[i].off, l = f->e[i].len;
        const uintptr_t a = (base + s + (align - 1u)) & ~(uintptr_t) (align - 1u);
        const uint32_t pad = (uint32_t) (a - (base + s));
        if (pad > l || l - pad < size) continue;
        const uint32_t tail = l - pad - size;
        if (pad == 0 && tail == 0) fl_remove(f, i);
        else if (pad == 0) { f->e[i].off = s + size; f->e[i].len = tail; }
        else if (tail == 0) { f->e[i].len = pad; }
        else {
            if (fl_room(f)) return -2;
            f->e[i].len = pad;
            fl_insert(f, i + 1, s + pad + size, tail);
        }
        *out_off = s + pad;
        return 0;
    }
    return -1;
}
/* 0 ok; -1 refused (outside the arena, or overlaps a free extent: a double free); -2 host OOM (nothing changed) */
static int fl_free(FreeList *f, uint32_t off, uint32_t size)
{
    if (!size || off > f->total || size > f->total - off) return -1;
    int i = 0;
    while (i < f->n && f->e[i].off < off) ++i;
    if (i > 0 && f->e[i - 1].off + f->e[i - 1].len > off) return -1;
    if (i < f->n && off + size > f->e[i].off) return -1;
    const int mp = i > 0 && f->e[i - 1].off + f->e[i - 1].len == off;
    const int mn = i < f->n && off + size == f->e[i].off;
    if (mp && mn) { f->e[i - 1].len += size + f->e[i].len; fl_remove(f, i); }
    else if (mp) f->e[i - 1].len += size;
    else if (mn) { f->e[i].off = off; f->e[i].len += size; }
    else {
        if (fl_room(f)) return -2;
        fl_insert(f, i, off, size);
    }
    return 0;
}
static uint32_t fl_free_bytes(const FreeList *f)
{
    uint32_t s = 0;
    for (int i = 0; i < f->n; ++i) s += f->e[i].len;
    return s;
}
/*FL_END*/

/* ====================================================================================================================================== kernel memory blocks (context, patcher) */
typedef struct { void *p; SceUID uid; uint32_t size; int kind; } KMem;
enum { KM_GPU = 0, KM_VUSSE, KM_FUSSE };

static int kmem_alloc(KMem *m, SceKernelMemBlockType type, uint32_t size, int kind, unsigned int *usse_off, const char *what)
{
    memset(m, 0, sizeof *m);
    size = GXM_ALIGN(size, 4096u);
    const SceUID uid = sceKernelAllocMemBlock("stfx_gxm", type, size, NULL);
    if (uid < 0) {
        char w[80];
        snprintf(w, sizeof w, "sceKernelAllocMemBlock(%s, %u KB)", what, (unsigned) (size / 1024u));
        gxm_chk(uid, w, __FILE__, __LINE__);
        return -1;
    }
    m->uid = uid;
    if (GXM_CHK(sceKernelGetMemBlockBase(uid, &m->p))) { m->p = NULL; GXM_CHK(sceKernelFreeMemBlock(uid)); m->uid = 0; return -1; }
    int r;
    if (kind == KM_GPU)        r = sceGxmMapMemory(m->p, size, SCE_GXM_MEMORY_ATTRIB_RW);
    else if (kind == KM_VUSSE) r = sceGxmMapVertexUsseMemory(m->p, size, usse_off);
    else                       r = sceGxmMapFragmentUsseMemory(m->p, size, usse_off);
    if (GXM_CHK(r)) { GXM_CHK(sceKernelFreeMemBlock(uid)); memset(m, 0, sizeof *m); return -1; }
    m->size = size; m->kind = kind;
    return 0;
}
static void kmem_free(KMem *m)
{
    if (!m->p) return;
    if (m->kind == KM_GPU)        GXM_CHK(sceGxmUnmapMemory(m->p));
    else if (m->kind == KM_VUSSE) GXM_CHK(sceGxmUnmapVertexUsseMemory(m->p));
    else                          GXM_CHK(sceGxmUnmapFragmentUsseMemory(m->p));
    GXM_CHK(sceKernelFreeMemBlock(m->uid));
    memset(m, 0, sizeof *m);
}

/* ====================================================================================================================================== arenas */
#define ARENA_MAX     192
#define CDRAM_CAP     (80u << 20)
#define LPDDR_CAP     (16u << 20)
#define ARENA_MIN     (1u << 20)

typedef struct { uint8_t *base; SceUID uid; uint32_t size, freeb; uint8_t kind; FreeList fl; } Arena;
static Arena g_ar[ARENA_MAX];
static int g_nar;
static uint32_t g_ar_total[2], g_ar_used[2], g_ar_peak[2];
static unsigned g_ar_count[2];
static unsigned c_blk_alloc[2], c_blk_free, c_blk_fail, c_blk_bad_free, c_cdram_fallback, c_late_calls;
static unsigned long long c_fallback_bytes;

static const char *kname(int k) { return k == GXM_MEM_LPDDR ? "lpddr" : "cdram"; }

static int arena_new(int k, uint32_t need, uint32_t align)
{
    if (g_nar >= ARENA_MAX) return -1;
    const uint32_t unit = k == GXM_MEM_CDRAM ? 256u * 1024u : ARENA_MIN;
    uint32_t want = need + (align > 4096u ? align : 0u);
    if (want < ARENA_MIN) want = ARENA_MIN;
    want = GXM_ALIGN(want, unit);
    if (want < need) return -1;
    if (g_ar_total[k] + want > (k == GXM_MEM_CDRAM ? CDRAM_CAP : LPDDR_CAP)) return -1;
    Arena *a = &g_ar[g_nar];
    memset(a, 0, sizeof *a);
    const SceKernelMemBlockType type = k == GXM_MEM_CDRAM ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    const SceUID uid = sceKernelAllocMemBlock("stfx_gxm_arena", type, want, NULL);
    if (uid < 0) {
        char w[80];
        snprintf(w, sizeof w, "sceKernelAllocMemBlock(%s arena, %u KB)", kname(k), (unsigned) (want / 1024u));
        gxm_chk(uid, w, __FILE__, __LINE__);
        return -1;
    }
    void *base = NULL;
    if (GXM_CHK(sceKernelGetMemBlockBase(uid, &base)) || GXM_CHK(sceGxmMapMemory(base, want, SCE_GXM_MEMORY_ATTRIB_RW))) {
        GXM_CHK(sceKernelFreeMemBlock(uid));
        return -1;
    }
    if (fl_init(&a->fl, want)) {
        GXM_CHK(sceGxmUnmapMemory(base));
        GXM_CHK(sceKernelFreeMemBlock(uid));
        return -1;
    }
    a->base = (uint8_t *) base; a->uid = uid; a->size = want; a->freeb = want; a->kind = (uint8_t) k;
    g_ar_total[k] += want; ++g_ar_count[k];
    ++g_nar;
    if (g_nar <= 24 || (g_nar & 7) == 0)
        gxm_log("[gxm] arena #%d %s %u KB mapped (%s total %u KB of cap %u KB)", g_nar - 1, kname(k), want / 1024u, kname(k), g_ar_total[k] / 1024u,
                (k == GXM_MEM_CDRAM ? CDRAM_CAP : LPDDR_CAP) / 1024u);
    return g_nar - 1;
}

static int arena_alloc(GxmBlk *b, int k, uint32_t rsz, uint32_t align)
{
    for (int pass = 0; pass < 2; ++pass) {
        int lo = 0, hi = g_nar;
        if (pass == 1) { const int ni = arena_new(k, rsz, align); if (ni < 0) return -1; lo = ni; hi = ni + 1; }
        for (int i = lo; i < hi; ++i) {
            Arena *a = &g_ar[i];
            uint32_t off;
            if (a->kind != k || a->freeb < rsz) continue;
            if (fl_alloc(&a->fl, (uintptr_t) a->base, rsz, align, &off) != 0) continue;
            a->freeb -= rsz;
            g_ar_used[k] += rsz;
            if (g_ar_used[k] > g_ar_peak[k]) g_ar_peak[k] = g_ar_used[k];
            b->p = a->base + off; b->kind = (uint8_t) k; b->arena = (uint16_t) i; b->off = off;
            ++c_blk_alloc[k];
            return 0;
        }
    }
    return -1;
}

int gxm_blk_alloc(GxmBlk *b, int kind, uint32_t size, uint32_t align)
{
    if (!b) return -1;
    memset(b, 0, sizeof *b);
    if (g_gx.dead || g_gx.torn_down || !size || size > (1u << 28)) { ++c_blk_fail; return -1; }
    if (align < 16u) align = 16u;
    if (align & (align - 1u)) { uint32_t p = 16u; while (p < align) p <<= 1; align = p; }
    const int strict = kind == GXM_MEM_CDRAM_STRICT;
    const int k = kind == GXM_MEM_LPDDR ? GXM_MEM_LPDDR : GXM_MEM_CDRAM;
    const uint32_t rsz = GXM_ALIGN(size, 16u);
    int r = arena_alloc(b, k, rsz, align);
    if (r != 0 && k == GXM_MEM_CDRAM && !strict) {
        r = arena_alloc(b, GXM_MEM_LPDDR, rsz, align);
        if (r == 0) {
            ++c_cdram_fallback; c_fallback_bytes += rsz;
            if (c_cdram_fallback <= 8) gxm_log("[gxm] blk: CDRAM request of %u B (align %u) fell back to LPDDR (cdram_fallback %u)", size, align, c_cdram_fallback);
        }
    }
    if (r != 0) {
        ++c_blk_fail;
        if (c_blk_fail <= 16) gxm_log("[gxm] blk_alloc FAILED: %s%s %u B align %u (blk_fail %u; cdram used %u KB of %u KB mapped, lpddr used %u KB of %u KB mapped)",
                                      kname(k), strict ? " (strict)" : "", size, align, c_blk_fail, g_ar_used[0] / 1024u, g_ar_total[0] / 1024u, g_ar_used[1] / 1024u, g_ar_total[1] / 1024u);
        memset(b, 0, sizeof *b);
        return -1;
    }
    b->size = size;
    return 0;
}

void gxm_blk_free_now(GxmBlk *b)
{
    if (!b || !b->p) return;
    if (g_gx.torn_down) { ++c_late_calls; memset(b, 0, sizeof *b); return; }
    Arena *a = b->arena < g_nar ? &g_ar[b->arena] : NULL;
    int r = -1;
    if (a && a->kind == b->kind && (uint8_t *) b->p == a->base + b->off) {
        const uint32_t rsz = GXM_ALIGN(b->size, 16u);
        r = fl_free(&a->fl, b->off, rsz);
        if (r == 0) { a->freeb += rsz; g_ar_used[b->kind] -= rsz; ++c_blk_free; }
    }
    if (r != 0) {
        ++c_blk_bad_free;
        if (c_blk_bad_free <= 8) gxm_log("[gxm] blk_free REFUSED (rc %d): p=%p size %u arena %u off %u (blk_bad_free %u)", r, b->p, b->size, (unsigned) b->arena, b->off, c_blk_bad_free);
    }
    memset(b, 0, sizeof *b);
}

/* ====================================================================================================================================== deferred destruction */
typedef struct DNode {
    struct DNode *next;
    void (*fn)(void *);
    void *arg;
    unsigned seq;            /* retire when *fnot >= seq (fragment notification of the scene that is open now, or the next one) */
    unsigned pres_need;      /* fence_finish mode: retire when the display callback count reaches this */
    GxmBlk blk;              /* fn == NULL: free this block */
} DNode;
static DNode *dq_head, *dq_tail;
static unsigned c_defer_queued, c_defer_ran, c_defer_pending, c_defer_oom;

static int defer_push(DNode *n)
{
    n->next = NULL;
    n->seq = g_gx.nval + 1u;
    n->pres_need = g_gx.queued + 1u;
    if (dq_tail) dq_tail->next = n; else dq_head = n;
    dq_tail = n;
    ++c_defer_queued; ++c_defer_pending;
    return 0;
}

void gxm_blk_free_deferred(GxmBlk *b)
{
    if (!b || !b->p) return;
    if (g_gx.torn_down) { ++c_late_calls; memset(b, 0, sizeof *b); return; }
    DNode *n = (DNode *) calloc(1, sizeof *n);
    if (!n) {      /* cannot defer: LEAK the block (safe) rather than free what the GPU may read */
        ++c_defer_oom;
        if (c_defer_oom <= 8) gxm_log("[gxm] defer: host allocation failed, block %p (%u B) leaked on purpose (defer_oom %u)", b->p, b->size, c_defer_oom);
        memset(b, 0, sizeof *b);
        return;
    }
    n->blk = *b;
    memset(b, 0, sizeof *b);
    defer_push(n);
}

void gxm_frame_defer(void (*fn)(void *), void *arg)
{
    if (!fn) return;
    if (g_gx.torn_down) { ++c_late_calls; return; }
    DNode *n = (DNode *) calloc(1, sizeof *n);
    if (!n) { ++c_defer_oom; if (c_defer_oom <= 8) gxm_log("[gxm] defer: host allocation failed, callback %p dropped (leak) (defer_oom %u)", (void *) fn, c_defer_oom); return; }
    n->fn = fn; n->arg = arg;
    defer_push(n);
}

static void defer_run(DNode *n)
{
    if (n->fn) n->fn(n->arg); else gxm_blk_free_now(&n->blk);
    free(n);
    ++c_defer_ran;
    --c_defer_pending;
}

/* retire what the GPU has finished with. force: the GPU is idle (teardown). Never called when dead (the GPU may still read). */
static void defer_poll(int force)
{
    if (!dq_head) return;
    if (!force && g_gx.fence_finish && (g_gx.arm & GXM_ARM_NOPRESENT)) { sceGxmFinish(g_gx.ctx); force = 1; }   /* nothing else can retire them */
    while (dq_head) {
        DNode *n = dq_head;
        const int ready = force || (g_gx.fence_finish ? (g_gx.presented >= n->pres_need) : ((int) (*g_gx.fnot - n->seq) >= 0));
        if (!ready) break;
        dq_head = n->next;
        if (!dq_head) dq_tail = NULL;
        defer_run(n);
    }
}

/* ====================================================================================================================================== fences, scenes */
static Ring g_ring;
static GxmBlk g_ring_blk;
static int g_ring_up;
static unsigned c_fence_timeouts, c_fence_waits, c_wait_badseq, c_end_fail, c_scenes_ended, c_mod_trunc;

/* bounded poll of the fragment notification; not guarded (shutdown uses it after torn_down) */
static int wait_raw(uint32_t seq, uint32_t deadline_us)
{
    if (!seq) return 0;
    if (g_gx.fence_finish) { sceGxmFinish(g_gx.ctx); return 0; }
    if ((int) (*g_gx.fnot - seq) >= 0) return 0;
    ++c_fence_waits;
    const uint64_t t0 = gxm_now_us();
    for (;;) {
        sceKernelDelayThread(150);
        if ((int) (*g_gx.fnot - seq) >= 0) return 0;
        if (gxm_now_us() - t0 > deadline_us) return (int) (*g_gx.fnot - seq) >= 0 ? 0 : -1;
    }
}

int gxm_frame_wait_scene(uint32_t seq, uint32_t deadline_us)
{
    if (g_gx.dead || g_gx.torn_down || !g_gx.fnot) return -1;
    if (seq > g_gx.nval) {
        if (++c_wait_badseq <= 8) gxm_log("[gxm] wait_scene(%u) REFUSED: that scene has not ended (nval %u) -- a caller bug, nothing to wait for", seq, g_gx.nval);
        return -1;
    }
    if (!deadline_us) deadline_us = GXM_FENCE_DEADLINE_US;
    if (wait_raw(seq, deadline_us) == 0) return 0;
    ++c_fence_timeouts;
    gxm_log("[gxm] fence timeout: scene %u not finished within %u us (fnot %u vnot %u nval %u fence_timeouts %u)", seq, deadline_us, *g_gx.fnot, *g_gx.vnot, g_gx.nval, c_fence_timeouts);
    return -1;
}

/* ---- GPU busy instrument (2026-10-20, opt-in: ux0:data/stairfax/gxm_gpu.txt). A poller thread on core 2 timestamps each change of the
 * fragment notification (*fnot = the last scene the GPU finished); end_scene stamps the submit. GPU time of scene k ~=
 * done[k] - max(done[k-1], submit[k]) (the GPU starts k no earlier than its EndScene and no earlier than k-1 finished). Poll period 50 us,
 * so each scene carries up to ~50 us of error. Reported every 60 presents as [gxm-gpu]. Control: a CPU-only spin (perf arm 97, 12 ms)
 * must move wall and leave GPU busy alone. */
static int read_small(const char *path, char *buf, int cap);
#define GG_N 8192u
static volatile uint32_t gg_sub[GG_N], gg_done[GG_N]; static volatile uint8_t gg_kind[GG_N];
volatile uint32_t g_gg_dur[GG_N];   /* per scene seq: GPU us (the copy log prints one frame of them) */
char g_gxm_end_kind;   /* set by the target module before each end_scene: m main, c copy, d display */
static volatile int gg_on, gg_quit; static SceUID gg_tid = -1;
static int gg_poll(SceSize a, void *b)
{
    (void) a; (void) b; uint32_t last = 0, f0 = 0, done0 = 0; uint64_t busy = 0, bk[4] = { 0, 0, 0, 0 }; unsigned nsc = 0, nk[4] = { 0, 0, 0, 0 }; uint32_t tw = sceKernelGetProcessTimeLow();
    /* SELF-CHECK (2026-10-23, docs/54 P1): a scene is stamped done when the poller next RUNS, so every gap between polls beyond the
     * period is stamp error. hw_P480 read "GPU busy" = 100% of wall with windows of 419 and 1,451 presents: R had landed on the
     * poller's core (rcore auto picks a core G is not on, per boot). late = sum over polls of (gap - 1 ms) for gaps over 1 ms (above sleep jitter;
     * the line prints the mean gap) -- a window with late > 0.5 ms/frame is tagged INVALID on its own line, whatever the cause (R, G, audio, the OS). */
    uint32_t prev = tw, gapmax = 0; uint64_t late = 0; unsigned npoll = 0;
    while (!gg_quit) {
        const uint32_t v = g_gx.fnot ? *g_gx.fnot : 0u, now = sceKernelGetProcessTimeLow();
        { const uint32_t gap = now - prev; prev = now; ++npoll; if (gap > gapmax) gapmax = gap; if (gap > 1000u) late += gap - 1000u; }
        if (v - last < 100000u) {   /* sane forward step */
            while (last != v) {
                ++last; gg_done[last % GG_N] = now;
                if (done0) { const uint32_t st = gg_sub[last % GG_N], prev = gg_done[(last - 1u) % GG_N];
                             const uint32_t start = (int32_t) (st - prev) > 0 ? st : prev; busy += (uint32_t) (now - start); ++nsc;
                             const uint8_t kc = gg_kind[last % GG_N]; const int ki = kc == 'm' ? 0 : kc == 'c' ? 1 : kc == 'd' ? 2 : 3;
                             bk[ki] += (uint32_t) (now - start); ++nk[ki]; g_gg_dur[last % GG_N] = (uint32_t) (now - start); }
                done0 = 1;
            }
        } else last = v;
        if (g_gx.frames - f0 >= 60u) {
            const unsigned nf = g_gx.frames - f0; const uint32_t wall = now - tw;
            gxm_log("[gxm-gpu] presents %u..%u: GPU busy %.1f ms/frame (%.0f%% of wall %.1f), %.1f scenes/frame", f0, g_gx.frames,
                    busy / 1000.0 / nf, wall ? 100.0 * (double) busy / wall : 0.0, wall / 1000.0 / nf, (double) nsc / nf);
            gxm_log("[gxm-gpu]   by scene kind, ms/frame (scenes/frame): main %.1f (%.1f) | copy %.1f (%.1f) | display %.1f (%.1f) | other %.1f (%.1f)",
                    bk[0] / 1000.0 / nf, (double) nk[0] / nf, bk[1] / 1000.0 / nf, (double) nk[1] / nf, bk[2] / 1000.0 / nf, (double) nk[2] / nf, bk[3] / 1000.0 / nf, (double) nk[3] / nf);
            { const double lpf = late / 1000.0 / nf;
              gxm_log("[gxm-gpu]   poller %s: cpu %d, %u polls (%.0f us mean gap), max gap %.2f ms, late %.2f ms/frame (bar 0.50)", lpf > 0.5 ? "INVALID" : "ok",
                      sceKernelGetCpuId(), npoll, npoll ? (double) wall / npoll : 0.0, gapmax / 1000.0, lpf); }
            late = 0; gapmax = 0; npoll = 0;
            for (int i = 0; i < 4; ++i) { bk[i] = 0; nk[i] = 0; }
            f0 = g_gx.frames; busy = 0; nsc = 0; tw = now;
        }
        sceKernelDelayThread(50);
    }
    return 0;
}
static void gg_start(void)
{
    char b[8]; memset(b, 0, sizeof b); if (read_small("ux0:data/stairfax/gxm_gpu.txt", b, sizeof b) < 0) { gxm_log("[gxm-gpu] OFF (gxm_gpu.txt absent)"); return; }
    /* 2026-10-21: the poller sat on core 2 -- the render thread's core (gx_thread rcore auto = 2). Threaded, it was starved while R ran (scene-done
     * stamps late: "GPU busy" == wall in every tour row) and it preempted R every 50 us. Default is now core 0; "c1"/"c2" pick a core (c2 = the old). */
    const int core = (b[0] == 'c' && b[1] >= '0' && b[1] <= '2') ? b[1] - '0' : 0;
    /* "c2e": EQUAL priority (the pre-2026-10-23 poller) -- the self-check's NEGATIVE CONTROL: on R's core it must starve and say INVALID */
    const int eqpri = b[0] == 'c' && b[2] == 'e';
    const int mask = core == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : core == 1 ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2;
    gg_tid = sceKernelCreateThread("stfx_gpu_poll", gg_poll, eqpri ? 0x10000100 : 0x10000100 - 32,   /* ABOVE every game thread (all 0x10000100): hw_G1P1_A480 at equal priority: 86% of windows late (max gaps ~12 ms) */
                                 0x4000, 0, mask, NULL);
    if (gg_tid >= 0 && sceKernelStartThread(gg_tid, 0, NULL) >= 0) { gg_on = 1; gxm_log("[gxm-gpu] ON: poller on core %d, 50 us period, priority %s", core, eqpri ? "EQUAL (negative control)" : "game+32"); }
    else gxm_log("[gxm-gpu] poller thread failed (0x%08x)", (unsigned) gg_tid);
}
static void gg_stop(void) { if (gg_on) { gg_quit = 1; SceUInt to = 200000; sceKernelWaitThreadEnd(gg_tid, NULL, &to); sceKernelDeleteThread(gg_tid); gg_on = 0; } }

/* ---- CORE IDLE METER (2026-10-23, LOCKED-30 lever R5: "core-0 headroom UNMEASURED: measure first", docs/54). Opt-in: ux0:data/stairfax/gxm_idle.txt
 * = the cores to meter, e.g. "0" or "012". One spinner per core, pinned, at the LOWEST user priority (191): it runs only when nothing else on that core
 * wants to. It reads the clock in a tight loop; a step of <= IM_RUN_US means it was running, a longer one means it was preempted. Free time = the sum
 * of the short steps. Self-check (printed, judged by the reader): on G's core the free time must match G's present-wait, on R's core wall - R busy.
 * Every 60 presents: "[idle] presents a..b: core N free X ms/frame (Y% of wall W)". */
#define IM_RUN_US 20u
static volatile int im_quit; static SceUID im_tid[3] = { -1, -1, -1 };
static int im_spin(SceSize a, void *argp)
{
    const int core = argp ? *(const int *) argp : 0; (void) a;
    uint32_t prev = sceKernelGetProcessTimeLow(), tw = prev, f0 = g_gx.frames; uint64_t run = 0;
    while (!im_quit) {
        const uint32_t now = sceKernelGetProcessTimeLow(), d = now - prev; prev = now;
        if (d <= IM_RUN_US) run += d;
        if (g_gx.frames - f0 >= 60u) {
            const unsigned nf = g_gx.frames - f0; const uint32_t wall = now - tw;
            gxm_log("[idle] presents %u..%u: core %d free %.2f ms/frame (%.1f%% of wall %.1f) cpu %d", f0, g_gx.frames, core, run / 1000.0 / nf,
                    wall ? 100.0 * (double) run / wall : 0.0, wall / 1000.0 / nf, sceKernelGetCpuId());
            run = 0; f0 = g_gx.frames; tw = now;
        }
    }
    return 0;
}
static void im_start(void)
{
    static int cores[3] = { 0, 1, 2 };
    char b[8]; memset(b, 0, sizeof b); if (read_small("ux0:data/stairfax/gxm_idle.txt", b, sizeof b) < 0) return;
    for (int i = 0; i < 3; ++i) {
        if (!strchr(b, '0' + i)) continue;
        const int mask = i == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : i == 1 ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2;
        im_tid[i] = sceKernelCreateThread("stfx_idle", im_spin, 191, 0x1000, 0, mask, NULL);
        if (im_tid[i] >= 0 && sceKernelStartThread(im_tid[i], sizeof(int), &cores[i]) >= 0) gxm_log("[idle] ON: core %d, priority 191 (lowest user)", i);
        else { gxm_log("[idle] core %d thread failed (0x%08x)", i, (unsigned) im_tid[i]); im_tid[i] = -1; }
    }
}
static void im_stop(void)
{
    im_quit = 1;
    for (int i = 0; i < 3; ++i) if (im_tid[i] >= 0) { SceUInt to = 200000; sceKernelWaitThreadEnd(im_tid[i], NULL, &to); sceKernelDeleteThread(im_tid[i]); im_tid[i] = -1; }
}

int gxm_frame_end_scene(void)
{
    if (g_gx.dead || g_gx.torn_down || !g_gx.ctx) return -1;
    const unsigned seq = g_gx.nval + 1u;
    SceGxmNotification vn, fn;
    vn.address = g_gx.vnot; vn.value = seq;
    fn.address = g_gx.fnot; fn.value = seq;
    if (gg_on) { gg_kind[seq % GG_N] = (uint8_t) g_gxm_end_kind; g_gxm_end_kind = 0; }
    if (gg_on) gg_sub[seq % GG_N] = sceKernelGetProcessTimeLow();   /* before the kick: the GPU cannot finish it earlier than this */
    const int rc = GXM_CHK(sceGxmEndScene(g_gx.ctx, &vn, &fn));
    if (rc != 0) { ++c_end_fail; return rc; }          /* no notification was queued: nval stays, the ring is not told */
    g_gx.nval = seq;
    ++c_scenes_ended;
    if (g_ring_up) ring_scene_ended(&g_ring, seq);
    return 0;
}

/* the c2floor smoke_fence shape (hardware-verified there): one ended scene, a bounded poll, a loud fallback */
int gxm_frame_smoke(uint32_t seq)
{
    if (g_gx.dead || g_gx.torn_down || !g_gx.ctx || !g_gx.fnot) return -1;
    if (seq > g_gx.nval) { gxm_log("[gxm] smoke fence: REFUSED, scene %u has not ended (nval %u)", seq, g_gx.nval); return -1; }
    if (g_gx.fence_finish) {
        sceGxmFinish(g_gx.ctx);
        gxm_log("[gxm] smoke fence: mode=finish status=OK (notification already known not to signal)");
        return 0;
    }
    const uint64_t t0 = gxm_now_us(), dl = (uint64_t) SMOKE_MS * 1000u;
    while ((int) (*g_gx.fnot - seq) < 0) {
        if (gxm_now_us() - t0 > dl) break;
        sceKernelDelayThread(100);
    }
    const unsigned long long waited = (unsigned long long) (gxm_now_us() - t0);
    if ((int) (*g_gx.fnot - seq) >= 0) {
        gxm_log("[gxm] smoke fence: mode=notification status=OK seq=%u fnot=%u wait_us=%llu (a duration on the emulator is not a claim)", seq, *g_gx.fnot, waited);
        return 0;
    }
    g_gx.fence_finish = 1;
    gxm_log("[gxm] smoke fence: TIMEOUT after %u ms (fnot %u vnot %u want %u) -- the fragment notification does NOT signal on this runtime;"
            " falling back to sceGxmFinish for EVERY fence (UNBOUNDED waits from here on, deferred frees retire on the display callback)",
            SMOKE_MS, *g_gx.fnot, *g_gx.vnot, seq);
    sceGxmFinish(g_gx.ctx);
    return 0;
}

/* ====================================================================================================================================== staging ring */
uint32_t g_gxm_wait_us[2];   /* [0] staging-ring fence waits, [1] display-queue add: CPU time spent waiting on the GPU (2026-10-20 profile) */
static void ring_wait_cb(void *user, uint32_t tag)
{
    (void) user;
    const uint32_t tw0 = sceKernelGetProcessTimeLow();
    const int wr = gxm_frame_wait_scene(tag, 0);
    g_gxm_wait_us[0] += sceKernelGetProcessTimeLow() - tw0;
    if (wr != 0) gxm_abort("staging ring: the fence of scene %u was not reached (nval %u, fence_timeouts %u)", tag, g_gx.nval, c_fence_timeouts);
}

void *gxm_ring_alloc(uint32_t bytes)
{
    if (!g_ring_up || !bytes || gxm_dead()) return NULL;
    void *p = ring_alloc(&g_ring, bytes);
    if (g_gx.dead) return NULL;           /* the wait callback aborted: the memory just handed out may still be read */
    return p;
}
uint32_t gxm_ring_capacity(void) { return g_ring_up ? g_ring.cap : 0u; }
/* 1: gxm_ring_alloc(bytes) cannot fail for want of ring space once every ENDED scene has retired. 0: the OPEN scene alone (g_ring.scene_bytes) already holds so much
 * that the request would be ring "fatal" (ring.h: fq_count == 0 and no room): the caller ends the open scene first (draw module), which moves those bytes into the fence queue. */
int gxm_ring_open_scene_fits(uint32_t bytes)
{
    if (!g_ring_up || !bytes) return 0;
    const uint32_t size = (bytes + 31u) & ~31u;
    if (size > g_ring.cap) return 0;
    const uint32_t waste = (g_ring.head + size > g_ring.cap) ? g_ring.cap - g_ring.head : 0u;
    return g_ring.scene_bytes + size + waste <= g_ring.cap;
}

/* ====================================================================================================================================== display callback (M1 rhi_gxm.c:148-164) */
typedef struct { void *addr; } DispData;
/* FRAME CAP (2026-10-27, Tyler: "feels like 60 fps, then dips to 24-25"): ux0:data/stairfax/frame_cap.txt = 30 holds every flip at least 2 vblanks after
 * the previous one, so a place that renders faster than 33.3 ms shows a steady 30 instead of swinging 60 <-> 25.  The paced clock (fixed_clock.txt -N)
 * advances game time by the retraces that really pass, so the game's speed does not change.  Absent / anything else = uncapped (as before: every tool). */
static int g_min_vb = 1;
static unsigned g_last_vc, c_cap_waits, c_cap_bounded;
/* vcount_bias.txt = N (dev only): the cap reads a 16-bit counter that wraps N s after init, so the wrap is tested in a minute, not 18 */
static int g_vc_fake;
static unsigned g_vc_bias;
static unsigned cap_vcount(void)
{
    const unsigned v = (unsigned) sceDisplayGetVcount();
    return g_vc_fake ? ((v + g_vc_bias) & 0xFFFFu) : v;
}
static void display_cb(const void *cbdata)
{
    const DispData *d = (const DispData *) cbdata;
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = d->addr;
    fb.pitch = GXM_DISP_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = GXM_DISP_W;
    fb.height = GXM_DISP_H;
    /* NEXTFRAME takes effect at the NEXT vblank: to flip min_vb vblanks after the last flip, set it once min_vb - 1 have passed.
     * (First version waited min_vb: every place ran at exactly 50.0 ms = 3 vblanks, tour of eboot 6b9a5993.) */
    if (g_min_vb > 1 && g_gx.presented) {   /* rhi_gxm_cap.h: 16-bit difference + a bounded wait (the 2026-10-28 wrap hang) */
        int k = 0;
        for (; k < GXM_CAP_MAX_WAITS && gxm_cap_need_wait(cap_vcount(), g_last_vc, g_min_vb); ++k) { sceDisplayWaitVblankStart(); ++c_cap_waits; }
        if (k == GXM_CAP_MAX_WAITS) ++c_cap_bounded;
    }
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();
    g_last_vc = cap_vcount();
    ++g_gx.presented;
}

/* ====================================================================================================================================== markers */
static int read_small(const char *path, char *buf, int cap)
{
    memset(buf, 0, (size_t) cap);
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0) return -1;
    const int n = sceIoRead(fd, buf, (SceSize) (cap - 1));
    sceIoClose(fd);
    return n < 0 ? 0 : n;
}

static const struct { const char *word; unsigned bit; } kArms[] = {
    { "nopresent", GXM_ARM_NOPRESENT }, { "skipdraw", GXM_ARM_SKIPDRAW }, { "nodraw", GXM_ARM_NODRAW }, { "noblit", GXM_ARM_NOBLIT },
    { "noprog", GXM_ARM_NOPROG }, { "ringbreak", GXM_ARM_RINGBREAK }, { "rt1", GXM_ARM_RT1 },
};
#define N_ARMS ((int) (sizeof kArms / sizeof kArms[0]))

/* whitespace separated words; returns the bit set, logs unknown words (a typo in a break arm must not silently run the normal path) */
static unsigned arm_parse(const char *s, int *unknown)
{
    unsigned bits = 0;
    *unknown = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
        if (!*s) break;
        char w[24];
        int n = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') { if (n < (int) sizeof w - 1) w[n++] = *s; ++s; }
        w[n] = 0;
        int hit = 0;
        for (int i = 0; i < N_ARMS; ++i) if (!strcmp(w, kArms[i].word)) { bits |= kArms[i].bit; hit = 1; }
        if (!hit) { ++*unknown; gxm_log("[gxm] arm: UNKNOWN WORD '%s' in gxm_arm.txt (ignored)", w); }
    }
    return bits;
}

static void arm_log(int present)
{
    char name[96];
    int n = 0;
    name[0] = 0;
    for (int i = 0; i < N_ARMS; ++i) {
        if (!(g_gx.arm & kArms[i].bit)) continue;
        if (n) bapp(name, (int) sizeof name, &n, "+");
        for (const char *c = kArms[i].word; *c; ++c) bapp(name, (int) sizeof name, &n, "%c", *c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    }
    if (!g_gx.arm) gxm_log("[gxm] arm: normal (gxm_arm.txt %s)", present ? "present" : "absent");
    else if (g_gx.arm == GXM_ARM_NOPRESENT) gxm_log("[gxm] arm: NOPRESENT (the display-queue add is skipped: a break arm, expect presented 0) (gxm_arm.txt present)");
    else gxm_log("[gxm] arm: %s (a break arm; gxm_arm.txt present)", name);
}

/* ====================================================================================================================================== init */
enum { R_VDM = 0, R_VTX, R_FRAG, R_STAGE, R_PBUF, R_PVU, R_PFU, R_N };
static const unsigned kRingDefKB[R_N] = { 512, 2048, 1024, 2048, 1024, 2048, 1024 };
static const unsigned kRingMinKB[R_N] = { 64, 256, 128, 16, 64, 64, 64 };
static const unsigned kRingMaxKB[R_N] = { 8192, 16384, 8192, 16384, 8192, 8192, 8192 };
static unsigned g_ring_kb[R_N];

static struct {
    int gxm_up, ctx_mem, ctx_up, patch_mem, patcher_up, shader_up, tex_up, target_up, draw_up;
    KMem vdm, vtx, frag, fusse, pbuf, pvu, pfu;
    void *hostmem;
} S;

static void *patcher_host_alloc(void *u, uint32_t size) { (void) u; return malloc(size); }
static void patcher_host_free(void *u, void *p) { (void) u; free(p); }

/* window / periodic state */
static unsigned w_min, w_max, w_zero, w_n, w_sum, t_zero_frames, t_started, last_req, t_present_skipped, c_tok_missing;

static void reset_statics(void)
{
    memset(g_ar, 0, sizeof g_ar);
    g_nar = 0;
    memset(g_ar_total, 0, sizeof g_ar_total); memset(g_ar_used, 0, sizeof g_ar_used); memset(g_ar_peak, 0, sizeof g_ar_peak); memset(g_ar_count, 0, sizeof g_ar_count);
    memset(c_blk_alloc, 0, sizeof c_blk_alloc);
    c_blk_free = c_blk_fail = c_blk_bad_free = c_cdram_fallback = c_late_calls = 0; c_fallback_bytes = 0;
    dq_head = dq_tail = NULL;
    c_defer_queued = c_defer_ran = c_defer_pending = c_defer_oom = 0;
    memset(&g_ring, 0, sizeof g_ring); memset(&g_ring_blk, 0, sizeof g_ring_blk); g_ring_up = 0;
    c_fence_timeouts = c_fence_waits = c_wait_badseq = c_end_fail = c_scenes_ended = c_abort_repeat = c_mod_trunc = 0;
    memset(&S, 0, sizeof S);
    w_min = 0xffffffffu; w_max = w_zero = w_n = w_sum = t_zero_frames = t_started = last_req = t_present_skipped = c_tok_missing = 0;
}

/* everything that is up, in reverse. Precondition: the GPU is idle or nothing was ever submitted. */
static void release_all(void)
{
    if (S.draw_up)   { gxm_draw_shutdown();   S.draw_up = 0; }
    if (S.target_up) { gxm_target_shutdown(); S.target_up = 0; }
    if (S.tex_up)    { gxm_tex_shutdown();    S.tex_up = 0; }
    if (S.shader_up) { gxm_shader_shutdown(); S.shader_up = 0; }
    defer_poll(1);
    if (S.patcher_up) { GXM_CHK(sceGxmShaderPatcherDestroy(g_gx.patcher)); g_gx.patcher = NULL; S.patcher_up = 0; }
    if (S.patch_mem) { kmem_free(&S.pfu); kmem_free(&S.pvu); kmem_free(&S.pbuf); S.patch_mem = 0; }
    if (g_ring_up) { g_ring_up = 0; }                 /* the ring lives in an LPDDR arena block: released with the arenas */
    for (int i = g_nar - 1; i >= 0; --i) {
        Arena *a = &g_ar[i];
        if (!a->base) continue;
        fl_destroy(&a->fl);
        GXM_CHK(sceGxmUnmapMemory(a->base));
        GXM_CHK(sceKernelFreeMemBlock(a->uid));
        a->base = NULL;
    }
    g_nar = 0;
    memset(g_ar_used, 0, sizeof g_ar_used); memset(g_ar_total, 0, sizeof g_ar_total);
    if (S.ctx_up) { GXM_CHK(sceGxmDestroyContext(g_gx.ctx)); g_gx.ctx = NULL; S.ctx_up = 0; }
    free(S.hostmem); S.hostmem = NULL;
    if (S.ctx_mem) { kmem_free(&S.fusse); kmem_free(&S.frag); kmem_free(&S.vtx); kmem_free(&S.vdm); S.ctx_mem = 0; }
    if (S.gxm_up) { GXM_CHK(sceGxmTerminate()); S.gxm_up = 0; }
}

static int init_fail(const char *what)
{
    gxm_log("[gxm] INIT FAILED at: %s [nonzero %d] -- no picture will be produced; this is NOT a pass", what, g_gx.nonzero);
    if (g_gx.nval && !g_gx.dead && wait_raw(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) {
        gxm_abort("init unwind: scene %u never finished -- nothing is freed", g_gx.nval);
    } else if (!g_gx.dead) {
        if (g_gx.queued) GXM_CHK(sceGxmDisplayQueueFinish());
        release_all();
    }
    g_gx.up = 0;
    g_gx.torn_down = 1;
    return -1;
}

static void mem_line(const char *why);

int gxm_frame_init(const RhiCreateInfo *info)
{
    if (g_gx.up && !g_gx.torn_down) { gxm_log("[gxm] init refused: the frame module is already up"); return -1; }
    memset(&g_gx, 0, sizeof g_gx);
    reset_statics();

    /* ---- markers ---- */
    char mb[256];
    {   const int n = read_small(ARM_FILE, mb, sizeof mb);
        int unk = 0;
        g_gx.arm = n > 0 ? arm_parse(mb, &unk) : 0u;
        arm_log(n >= 0);
    }
    for (int i = 0; i < R_N; ++i) g_ring_kb[i] = kRingDefKB[i];
    {   const int n = read_small(RINGS_FILE, mb, sizeof mb);
        if (n > 0) {
            unsigned v[R_N];
            for (int i = 0; i < R_N; ++i) v[i] = g_ring_kb[i];
            const int got = sscanf(mb, "%u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
            for (int i = 0; i < R_N && i < got; ++i) {
                if (v[i] == 0) continue;                                  /* 0 = keep the default */
                g_ring_kb[i] = v[i] < kRingMinKB[i] ? kRingMinKB[i] : v[i] > kRingMaxKB[i] ? kRingMaxKB[i] : v[i];
            }
        }
        if (g_gx.arm & GXM_ARM_RINGBREAK) g_ring_kb[R_STAGE] = 64;
        gxm_log("[gxm] rings KB: vdm %u vtx %u frag %u stage %u patcher_buf %u patcher_vusse %u patcher_fusse %u (gxm_rings.txt %s%s)", g_ring_kb[R_VDM], g_ring_kb[R_VTX], g_ring_kb[R_FRAG],
                g_ring_kb[R_STAGE], g_ring_kb[R_PBUF], g_ring_kb[R_PVU], g_ring_kb[R_PFU], n >= 0 ? "present" : "absent",
                (g_gx.arm & GXM_ARM_RINGBREAK) ? "; ringbreak arm: stage 64 KB, ring skip_wait=1" : "");
    }
    {   const int n = read_small(COPYFILTER_FILE, mb, sizeof mb);
        g_gx.copyfilter_on = !(n > 0 && mb[0] == '1');
        gxm_log("[gxm] copy filter %s (copyfilter_off.txt %s)", g_gx.copyfilter_on ? "ON" : "OFF", n >= 0 ? "present" : "absent");
    }

    /* ---- sceGxmInitialize ---- */
    SceGxmInitializeParams ip;
    memset(&ip, 0, sizeof ip);
    ip.flags = 0;
    ip.displayQueueMaxPendingCount = GXM_DISP_BUFS - 1;
    ip.displayQueueCallback = display_cb;
    ip.displayQueueCallbackDataSize = sizeof(DispData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (GXM_CHK(sceGxmInitialize(&ip))) return init_fail("sceGxmInitialize");
    S.gxm_up = 1;

    /* ---- context rings + context ---- */
    unsigned int fusse_off = 0, pvu_off = 0, pfu_off = 0;
    {   int bad = 0;
        S.ctx_mem = 1;
        bad |= kmem_alloc(&S.vdm, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_VDM] * 1024u, KM_GPU, NULL, "vdm ring");
        bad |= kmem_alloc(&S.vtx, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_VTX] * 1024u, KM_GPU, NULL, "vertex ring");
        bad |= kmem_alloc(&S.frag, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_FRAG] * 1024u, KM_GPU, NULL, "fragment ring");
        bad |= kmem_alloc(&S.fusse, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, KM_FUSSE, &fusse_off, "fragment USSE ring");
        if (bad) return init_fail("context memory");
    }
    SceGxmContextParams cp;
    memset(&cp, 0, sizeof cp);
    S.hostmem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    if (!S.hostmem) { ++g_gx.nonzero; return init_fail("context host memory (malloc)"); }
    cp.hostMem = S.hostmem;                     cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = S.vdm.p;              cp.vdmRingBufferMemSize = S.vdm.size;
    cp.vertexRingBufferMem = S.vtx.p;           cp.vertexRingBufferMemSize = S.vtx.size;
    cp.fragmentRingBufferMem = S.frag.p;        cp.fragmentRingBufferMemSize = S.frag.size;
    cp.fragmentUsseRingBufferMem = S.fusse.p;   cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = fusse_off;
    if (GXM_CHK(sceGxmCreateContext(&cp, &g_gx.ctx))) { g_gx.ctx = NULL; return init_fail("sceGxmCreateContext"); }
    S.ctx_up = 1;

    /* ---- shader patcher memory + patcher ---- */
    {   int bad = 0;
        S.patch_mem = 1;
        bad |= kmem_alloc(&S.pbuf, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_PBUF] * 1024u, KM_GPU, NULL, "patcher buffer");
        bad |= kmem_alloc(&S.pvu, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_PVU] * 1024u, KM_VUSSE, &pvu_off, "patcher vertex USSE");
        bad |= kmem_alloc(&S.pfu, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, g_ring_kb[R_PFU] * 1024u, KM_FUSSE, &pfu_off, "patcher fragment USSE");
        if (bad) return init_fail("patcher memory");
    }
    SceGxmShaderPatcherParams pp;
    memset(&pp, 0, sizeof pp);
    pp.userData = NULL;
    pp.hostAllocCallback = patcher_host_alloc;
    pp.hostFreeCallback = patcher_host_free;
    pp.bufferMem = S.pbuf.p;      pp.bufferMemSize = S.pbuf.size;
    pp.vertexUsseMem = S.pvu.p;   pp.vertexUsseMemSize = S.pvu.size;   pp.vertexUsseOffset = pvu_off;
    pp.fragmentUsseMem = S.pfu.p; pp.fragmentUsseMemSize = S.pfu.size; pp.fragmentUsseOffset = pfu_off;
    if (GXM_CHK(sceGxmShaderPatcherCreate(&pp, &g_gx.patcher))) { g_gx.patcher = NULL; return init_fail("sceGxmShaderPatcherCreate"); }
    S.patcher_up = 1;

    /* ---- notification words (region slots 100/101), zeroed ---- */
    {   volatile unsigned *reg = sceGxmGetNotificationRegion();
        if (!reg) { ++g_gx.nonzero; return init_fail("sceGxmGetNotificationRegion"); }
        g_gx.vnot = reg + NOTIF_SLOT_V;
        g_gx.fnot = reg + NOTIF_SLOT_F;
        *g_gx.vnot = 0; *g_gx.fnot = 0;
        g_gx.nval = 0;
    }

    /* ---- staging ring (LPDDR arena block) ---- */
    if (gxm_blk_alloc(&g_ring_blk, GXM_MEM_LPDDR, g_ring_kb[R_STAGE] * 1024u, 256u)) return init_fail("staging ring memory");
    ring_init(&g_ring, (uint8_t *) g_ring_blk.p, g_ring_kb[R_STAGE] * 1024u, ring_wait_cb, NULL);
    if (g_gx.arm & GXM_ARM_RINGBREAK) g_ring.skip_wait = 1;
    {   char sb[8];   /* the shadow model costs O(live allocations) per alloc: on only when asked (the Vita3K harness writes the file) */
        g_ring.shadow_on = read_small("ux0:data/stairfax/gxm_ring_shadow.txt", sb, sizeof sb) > 0;
        gxm_log("[gxm] ring shadow model: %s (gxm_ring_shadow.txt %s)", g_ring.shadow_on ? "ON" : "OFF", g_ring.shadow_on ? "present" : "absent");
    }
    g_ring_up = 1;
    {   char cb[8]; g_min_vb = 1;
        if (read_small("ux0:data/stairfax/frame_cap.txt", cb, sizeof cb) > 0 && atoi(cb) == 30) g_min_vb = 2;
        gxm_log("[gxm] frame cap: %s (frame_cap.txt) | vcount now %u", g_min_vb == 2 ? "30 fps (a flip at least 2 vblanks after the last)" : "none", (unsigned) sceDisplayGetVcount());
        if (read_small("ux0:data/stairfax/vcount_bias.txt", cb, sizeof cb) > 0 && atoi(cb) > 0) {
            const unsigned raw = (unsigned) sceDisplayGetVcount();
            g_vc_fake = 1; g_vc_bias = (0x10000u - (unsigned) atoi(cb) * 60u - (raw & 0xFFFFu)) & 0xFFFFu;
            gxm_log("[gxm] frame cap TEST: a 16-bit counter that wraps in %d s (vcount_bias.txt; raw %u reads as %u)", atoi(cb), raw, cap_vcount());
        }
    }
    gg_start();
    im_start();

    g_gx.up = 1;                                /* the core is complete: module init may call frame functions (see the header comment) */
    mem_line("init core");

    /* ---- the other modules, fixed order ---- */
    if (gxm_shader_init())          return init_fail("gxm_shader_init");
    S.shader_up = 1;
    if (gxm_tex_init())             return init_fail("gxm_tex_init");
    S.tex_up = 1;
    if (gxm_target_init(info))      return init_fail("gxm_target_init");
    S.target_up = 1;
    if (gxm_draw_init())            return init_fail("gxm_draw_init");
    S.draw_up = 1;

    gxm_log("[gxm] init OK: sceGxmInitialize + context (rings %u/%u/%u KB) + app-owned shader patcher (%u/%u/%u KB) + fragment notification fence (slots %d/%d)"
            " + staging ring %u KB + shader/texture/target/draw modules | M %dx%d (rh %d, efb528 %d) | nonzero_returns %d",
            g_ring_kb[R_VDM], g_ring_kb[R_VTX], g_ring_kb[R_FRAG], g_ring_kb[R_PBUF], g_ring_kb[R_PVU], g_ring_kb[R_PFU], NOTIF_SLOT_V, NOTIF_SLOT_F,
            g_ring_kb[R_STAGE], g_gx.rw, g_gx.vh, g_gx.rh, g_gx.efb528, g_gx.nonzero);
    mem_line("init");
    return 0;
}

/* ====================================================================================================================================== counters */
static unsigned req_sum(void) { unsigned s = 0; for (int k = 0; k < GXM_K_N; ++k) s += g_gx.st.req[k]; return s; }
static unsigned issued_sum(void) { unsigned s = 0; for (int k = 0; k < GXM_K_N; ++k) s += g_gx.st.issued[k]; return s; }
static unsigned skip_kind(int k) { unsigned s = 0; for (int r = 0; r < GXM_SK_N; ++r) s += g_gx.st.skip[k][r]; return s; }
static unsigned skip_reason(int r) { unsigned s = 0; for (int k = 0; k < GXM_K_N; ++k) s += g_gx.st.skip[k][r]; return s; }
static unsigned refused_sum(void) { unsigned s = 0; for (int k = 0; k < GXM_K_N; ++k) s += g_gx.st.refused_pre[k]; return s; }
static const char *const kKind[GXM_K_N] = { "tev", "tevgx", "col", "tex", "static" };

int gxm_frame_report(char *buf, int cap)
{
    int n = 0;
    if (cap <= 0) return 0;
    buf[0] = 0;
    bapp(buf, cap, &n, "blk_alloc=%u blk_alloc_lpddr=%u blk_free=%u blk_fail=%u blk_bad_free=%u cdram_fallback=%u cdram_fallback_kb=%u ",
         c_blk_alloc[0], c_blk_alloc[1], c_blk_free, c_blk_fail, c_blk_bad_free, c_cdram_fallback, (unsigned) (c_fallback_bytes / 1024u));
    bapp(buf, cap, &n, "cdram_arenas=%u cdram_kb=%u cdram_used_kb=%u cdram_peak_kb=%u lpddr_arenas=%u lpddr_kb=%u lpddr_used_kb=%u lpddr_peak_kb=%u ",
         g_ar_count[0], g_ar_total[0] / 1024u, g_ar_used[0] / 1024u, g_ar_peak[0] / 1024u, g_ar_count[1], g_ar_total[1] / 1024u, g_ar_used[1] / 1024u, g_ar_peak[1] / 1024u);
    bapp(buf, cap, &n, "ring_shadow_peak=%d ring_cap_kb=%u ring_skipped_waits=%d ring_max_end_kb=%u fence_waits=%u fence_finish=%d wait_badseq=%u end_scene_fail=%u scenes_ended=%u ",
         g_ring.sh_n, g_ring_up ? g_ring.cap / 1024u : 0u, g_ring.skipped_waits, g_ring.max_end / 1024u, c_fence_waits, g_gx.fence_finish, c_wait_badseq, c_end_fail, c_scenes_ended);
    bapp(buf, cap, &n, "defer_queued=%u defer_oom=%u late_calls=%u present_skipped=%u tok_missing=%u mod_trunc=%u abort_repeat=%u arm=%u ", c_defer_queued, c_defer_oom, c_late_calls, t_present_skipped, c_tok_missing, c_mod_trunc, c_abort_repeat, g_gx.arm);
    return n;
}

/* append the four modules' tokens (each separated by a space) to buf; returns the length */
static int collect_modules(char *buf, int cap)
{
    typedef int (*RepFn)(char *, int);
    static const RepFn fns[4] = { gxm_shader_report, gxm_tex_report, gxm_draw_report, gxm_target_report };
    int n = 0;
    buf[0] = 0;
    for (int i = 0; i < 4 && n < cap - 2; ++i) {
        buf[n] = 0;
        (void) fns[i](buf + n, cap - n);
        buf[cap - 1] = 0;
        const int l = (int) strlen(buf + n);
        if (n + l >= cap - 2) ++c_mod_trunc;           /* the module's tokens may have been cut: reported, never silent */
        n += l;
        if (n > 0 && buf[n - 1] != ' ' && n < cap - 1) { buf[n++] = ' '; buf[n] = 0; }
    }
    return n;
}

static int tok_u(const char *buf, const char *key, unsigned *out)
{
    const size_t kl = strlen(key);
    for (const char *p = buf; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == buf || p[-1] == ' ') && p[kl] == '=') { *out = (unsigned) strtoul(p + kl + 1, NULL, 10); return 1; }
    }
    return 0;
}
/* first of up to two key names that exists; 0 when no module printed it */
static int tok_any(const char *buf, const char *k1, const char *k2, unsigned *out) { return tok_u(buf, k1, out) || (k2 && tok_u(buf, k2, out)); }

static char g_line[6144], g_mod[3072];

static void acct_line(const char *tag)
{
    int n = 0;
    char *b = g_line;
    const int cap = (int) sizeof g_line;
    const unsigned req = req_sum(), iss = issued_sum();
    unsigned skp = 0;
    for (int k = 0; k < GXM_K_N; ++k) skp += skip_kind(k);
    bapp(b, cap, &n, "[gxm-acct] %sframe=%u presented=%u req=%u issued=%u skipped=%u ", tag, g_gx.frames, (unsigned) g_gx.presented, req, iss, skp);
    for (int k = 0; k < GXM_K_N; ++k)
        bapp(b, cap, &n, "req_%s=%u issued_%s=%u skip_%s=%u ", kKind[k], g_gx.st.req[k], kKind[k], g_gx.st.issued[k], kKind[k], skip_kind(k));
    bapp(b, cap, &n, "sk_noprog=%u sk_progfull=%u sk_compile=%u sk_notexmem=%u sk_scene=%u sk_ring=%u sk_ub=%u sk_scissor=%u sk_drawfail=%u sk_dead=%u refused_pre=%u ",
         skip_reason(GXM_SK_NOPROG), skip_reason(GXM_SK_PROGFULL), skip_reason(GXM_SK_COMPILE), skip_reason(GXM_SK_NOTEXMEM), skip_reason(GXM_SK_SCENE), skip_reason(GXM_SK_RING),
         skip_reason(GXM_SK_UB), skip_reason(GXM_SK_SCISSOR_EMPTY), skip_reason(GXM_SK_DRAWFAIL), skip_reason(GXM_SK_DEAD), refused_sum());
    bapp(b, cap, &n, "sce_draw_ok=%u sce_draw_fail=%u internal_quads=%u scenes=%u scenes_m=%u scenes_copy=%u scenes_display=%u fence_quads=%u rt_overflow=%u rt_scenes_max=%u ",
         g_gx.st.sce_draw_ok, g_gx.st.sce_draw_fail, g_gx.st.internal_quads, g_gx.st.scenes, g_gx.st.scenes_m, g_gx.st.scenes_copy, g_gx.st.scenes_display, g_gx.st.fence_quads,
         g_gx.st.rt_overflow, g_gx.st.rt_scenes_max);
    bapp(b, cap, &n, "ring_allocs=%d ring_wraps=%d ring_waits=%d ring_overwrote=%d ring_fatal=%d ring_oob=%d ring_shadow_full=%d fence_timeouts=%u defer_pending=%u defer_ran=%u nonzero=%d dead=%d frame_cap_vb=%d cap_waits=%u cap_bounded=%u vcount=%u ",
         g_ring.allocs, g_ring.wraps, g_ring.waits, g_ring.overwrote, g_ring.fatal, g_ring.oob, g_ring.shadow_full, c_fence_timeouts, c_defer_pending, c_defer_ran, g_gx.nonzero, g_gx.dead ? 1 : 0, g_min_vb, c_cap_waits, c_cap_bounded, (unsigned) sceDisplayGetVcount());
    {   char fr[1024];
        (void) gxm_frame_report(fr, (int) sizeof fr);
        bapp(b, cap, &n, "%s", fr);
    }
    if (!g_gx.torn_down) { collect_modules(g_mod, (int) sizeof g_mod); bapp(b, cap, &n, "%s", g_mod); }
    else bapp(b, cap, &n, "modules=down ");
    while (n > 0 && b[n - 1] == ' ') b[--n] = 0;
    printf("%s\n", b);
}

/* the arena books against the free lists: a->freeb must equal the free list's own sum (cheap, run with the memory line) */
static unsigned arena_audit(void)
{
    unsigned bad = 0;
    for (int i = 0; i < g_nar; ++i) if (g_ar[i].base && fl_free_bytes(&g_ar[i].fl) != g_ar[i].freeb) ++bad;
    return bad;
}

static void mem_line(const char *why)
{
    const unsigned audit_bad = arena_audit();
    SceKernelFreeMemorySizeInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.size = sizeof fi;
    unsigned ku = 0, kc = 0, kp = 0;
    /* (the kernel's figures are SIGNED ints that can go a little below zero: clamped, not wrapped) */
    if (sceKernelGetFreeMemorySize(&fi) >= 0) { ku = fi.size_user > 0 ? (unsigned) fi.size_user / 1024u : 0u; kc = fi.size_cdram > 0 ? (unsigned) fi.size_cdram / 1024u : 0u; kp = fi.size_phycont > 0 ? (unsigned) fi.size_phycont / 1024u : 0u; }
    struct mallinfo mi = mallinfo();
    unsigned sh = 0, sb = 0, sv = 0, sf = 0;
    if (g_gx.patcher && !g_gx.torn_down) {
        sh = sceGxmShaderPatcherGetHostMemAllocated(g_gx.patcher) / 1024u;
        sb = sceGxmShaderPatcherGetBufferMemAllocated(g_gx.patcher) / 1024u;
        sv = sceGxmShaderPatcherGetVertexUsseMemAllocated(g_gx.patcher) / 1024u;
        sf = sceGxmShaderPatcherGetFragmentUsseMemAllocated(g_gx.patcher) / 1024u;
    }
    gxm_log("[gxm-mem] %s (present %u) | arenas KB: cdram used %u of %u mapped (peak %u, %u arenas) lpddr used %u of %u mapped (peak %u, %u arenas) | blk_fail %u cdram_fallback %u blk_bad_free %u audit_bad %u"
            " | kernel free KB: user %u cdram %u phycont %u | newlib heap KB: in use %u, from the system %u"
            " | shader patcher KB: host %u, buffer %u/%u, vertex USSE %u/%u, fragment USSE %u/%u | context rings KB: vdm %u vtx %u frag %u | staging ring KB %u",
            why, g_gx.frames, g_ar_used[0] / 1024u, g_ar_total[0] / 1024u, g_ar_peak[0] / 1024u, g_ar_count[0], g_ar_used[1] / 1024u, g_ar_total[1] / 1024u, g_ar_peak[1] / 1024u, g_ar_count[1],
            c_blk_fail, c_cdram_fallback, c_blk_bad_free, audit_bad, ku, kc, kp, (unsigned) mi.uordblks / 1024u, (unsigned) mi.arena / 1024u,
            sh, sb, g_ring_kb[R_PBUF], sv, g_ring_kb[R_PVU], sf, g_ring_kb[R_PFU], g_ring_kb[R_VDM], g_ring_kb[R_VTX], g_ring_kb[R_FRAG], g_ring_kb[R_STAGE]);
}

/* ====================================================================================================================================== present_end + periodic lines */
static void periodic_line(void)
{
    collect_modules(g_mod, (int) sizeof g_mod);
    unsigned tc = 0, mc = 0, ec = 0, rb = 0, cl = 0, cr = 0, sc = 0, pz = 0;
    c_tok_missing += !tok_u(g_mod, "tex_create", &tc) + !tok_u(g_mod, "mesh_create", &mc) + !tok_u(g_mod, "efb_copy", &ec) + !tok_u(g_mod, "readback", &rb)
                   + !tok_any(g_mod, "clears", "clear", &cl) + !tok_any(g_mod, "clear_rects", "clear_rect", &cr) + !tok_u(g_mod, "state_calls", &sc) + !tok_u(g_mod, "peekz", &pz);
    gxm_log("[gxm] frame %u presented %u nonzero %d | draws %u tex_create %u mesh_create %u efb_copy %u readback %u"
            " | M2: rendered through raw GXM (draws = requests; see [gxm-acct] for issued/skipped)"
            " | window of %u frames: draws/frame min %u max %u avg %u, zero-draw frames %u | zero-draw frames since the first draw %u of %u"
            " | by kind: tev %u tevgx %u col %u tex %u static %u | clears %u clear_rects %u state_calls %u peekz %u | queued %u present_skipped %u",
            g_gx.frames, (unsigned) g_gx.presented, g_gx.nonzero, req_sum(), tc, mc, ec, rb,
            w_n, w_n ? w_min : 0u, w_max, w_n ? w_sum / w_n : 0u, w_zero, t_zero_frames, t_started ? g_gx.frames - t_started : 0u,
            g_gx.st.req[GXM_K_TEV], g_gx.st.req[GXM_K_TEVGX], g_gx.st.req[GXM_K_COL], g_gx.st.req[GXM_K_TEX], g_gx.st.req[GXM_K_STATIC], cl, cr, sc, pz, g_gx.queued, t_present_skipped);
    w_min = 0xffffffffu; w_max = 0; w_zero = 0; w_n = 0; w_sum = 0;
}

void gxm_frame_present_end(int presented_ok)
{
    if (!g_gx.up || g_gx.torn_down) return;
    /* this frame's request count, settled (M1: f_draws) */
    const unsigned rs = req_sum(), fd = rs - last_req;
    last_req = rs;
    if (fd) { if (!t_started) t_started = g_gx.frames + 1u; }
    else if (t_started) { ++t_zero_frames; ++w_zero; }
    if (fd < w_min) w_min = fd;
    if (fd > w_max) w_max = fd;
    w_sum += fd; ++w_n;

    if (presented_ok) { if (g_gx.arm & GXM_ARM_NOPRESENT) ++t_present_skipped; else ++g_gx.queued; }
    ++g_gx.frames;
    if (!g_gx.dead) defer_poll(0);          /* dead: the GPU may still read what is queued */

    if (g_gx.frames % PERIOD == 0) { periodic_line(); acct_line(""); }
    if (g_gx.frames % MEM_PERIOD == 0) mem_line("periodic");
}

/* ====================================================================================================================================== shutdown */
static int poll_presented(unsigned want, uint32_t deadline_us)
{
    const uint64_t t0 = gxm_now_us();
    while (g_gx.presented < want) {
        if (gxm_now_us() - t0 > deadline_us) return -1;
        sceKernelDelayThread(200);
    }
    return 0;
}

static void shutdown_aborted(const char *why)
{
    gxm_log("[gxm] GXM-TIMEOUT: %s; NO teardown of a GPU that may still be reading", why);
    acct_line("FINAL ");
    gxm_log("[gxm] nonzero_returns=%d", g_gx.nonzero);
    gxm_log("[gxm] TEARDOWN ABORTED -- not a clean exit");
    g_gx.torn_down = 1;
}

void gxm_frame_shutdown(void)
{
    gg_stop();
    im_stop();
    if (!g_gx.up || g_gx.torn_down) return;
    gxm_log("[gxm] teardown begin: frames %u queued %u presented %u nonzero %d", g_gx.frames, g_gx.queued, (unsigned) g_gx.presented, g_gx.nonzero);
    if (g_gx.dead) {
        char w[160];
        snprintf(w, sizeof w, "the backend is dead (%s)", g_gx.dead_why);
        shutdown_aborted(w);
        return;
    }
    /* bounded waits only: the last scene's fence, then the display callback for every queued entry */
    if (g_gx.nval && wait_raw(g_gx.nval, GXM_FENCE_DEADLINE_US) != 0) {
        char w[160];
        snprintf(w, sizeof w, "the last scene's fragment notification never reached %u within the deadline (*fnot=%u)", g_gx.nval, *g_gx.fnot);
        ++c_fence_timeouts;
        shutdown_aborted(w);
        return;
    }
    if (poll_presented(g_gx.queued, GXM_FENCE_DEADLINE_US) != 0) {
        char w[160];
        snprintf(w, sizeof w, "only %u of %u queued frames reached the display callback within the deadline", (unsigned) g_gx.presented, g_gx.queued);
        shutdown_aborted(w);
        return;
    }
    /* GPU idle and the display queue empty: these two return at once */
    sceGxmFinish(g_gx.ctx);
    GXM_CHK(sceGxmDisplayQueueFinish());
    defer_poll(1);
    acct_line("FINAL ");
    mem_line("final");
    /* reverse order of init */
    release_all();                       /* modules (draw, target, texture, shader), the rest of the deferred queue, patcher, arenas, context, rings, terminate */
    g_gx.torn_down = 1;
    g_gx.up = 0;
    gxm_log("[gxm] nonzero_returns=%d", g_gx.nonzero);
    gxm_log("[gxm] TEARDOWN DONE (reverse order of init)");
}

/* ====================================================================================================================================== status (vt_game.c's status block) */
static void tok_or_na(char *b, int cap, int *n, const char *label, const char *buf, const char *k1, const char *k2)
{
    unsigned v;
    if (tok_any(buf, k1, k2, &v)) bapp(b, cap, n, "%s %u ", label, v);
    else bapp(b, cap, n, "%s n/a ", label);
}

void rhi_gxm_gx_report(void)
{
    if (!g_gx.up || g_gx.torn_down) { gxm_log("[gxm] status: backend not up (up %d torn_down %d dead %d nonzero %d)", g_gx.up, g_gx.torn_down, g_gx.dead, g_gx.nonzero); return; }
    collect_modules(g_mod, (int) sizeof g_mod);
    char s[1024];
    int n = 0;
    unsigned skp = 0;
    for (int k = 0; k < GXM_K_N; ++k) skp += skip_kind(k);
    bapp(s, (int) sizeof s, &n, "[gxm] status: frames %u presented %u queued %u nonzero %d | draws %u (tev %u tevgx %u col %u tex %u static %u) issued %u skipped %u | ",
         g_gx.frames, (unsigned) g_gx.presented, g_gx.queued, g_gx.nonzero, req_sum(), g_gx.st.req[GXM_K_TEV], g_gx.st.req[GXM_K_TEVGX], g_gx.st.req[GXM_K_COL], g_gx.st.req[GXM_K_TEX],
         g_gx.st.req[GXM_K_STATIC], issued_sum(), skp);
    tok_or_na(s, (int) sizeof s, &n, "textures created", g_mod, "tex_create", NULL);
    tok_or_na(s, (int) sizeof s, &n, "destroyed", g_mod, "tex_destroy", NULL);
    bapp(s, (int) sizeof s, &n, "| ");
    tok_or_na(s, (int) sizeof s, &n, "meshes created", g_mod, "mesh_create", NULL);
    tok_or_na(s, (int) sizeof s, &n, "updated", g_mod, "mesh_update", NULL);
    tok_or_na(s, (int) sizeof s, &n, "destroyed", g_mod, "mesh_destroy", NULL);
    bapp(s, (int) sizeof s, &n, "| ");
    tok_or_na(s, (int) sizeof s, &n, "efb copies", g_mod, "efb_copy", NULL);
    tok_or_na(s, (int) sizeof s, &n, "readbacks", g_mod, "readback", NULL);
    tok_or_na(s, (int) sizeof s, &n, "clear_rects", g_mod, "clear_rects", "clear_rect");
    tok_or_na(s, (int) sizeof s, &n, "peekz", g_mod, "peekz", NULL);
    bapp(s, (int) sizeof s, &n, "| M2 raw GXM%s", g_gx.dead ? " | DEAD" : "");
    printf("%s\n", s);
    acct_line("STATUS ");
}
