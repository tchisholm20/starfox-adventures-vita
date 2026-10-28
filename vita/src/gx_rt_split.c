/* gx_rt_split.c -- R5: the render thread R split across two cores (contract: host/engine/R5_SPLIT.md, section 8 amendments).
 *
 *   R-front   the thread that was R.  Runs the GX HAL.  Every HAL -> backend call lands in one of the entry points below and is RECORDED into the RHI ring; the payload bytes
 *             (vertex data, texture texels, the changed words of the key / uniform structs) are written ONCE into the ARENA and the record names them.
 *   R-back    a second thread.  Pops the RHI ring and executes the calls through the backend table (GxspBackend).  The only caller of sceGxm* while the split is on.
 *
 * The decisions (R5_SPLIT.md D1..D11) and where they live:
 *   D1  the call list = GxspApi (every shim that reaches the backend) + the replaced RhiOps entries (capops).  host/engine/gxsp_lint.py fails when the HAL gains a backend call that is
 *       neither here nor classified there.
 *   D2  bulk data goes to the arena; the key and uniform structs travel as DELTAS against what the back already holds (f_delta / b_apply): a draw that changes one position matrix sends
 *       ~50 bytes of the 4.3 KB.  The back keeps a mirror of each struct; the front keeps what it sent.  Both start at zero when the split is switched on.
 *   D3  handles are front-allocated proxies (GxspProxy), kept in BOTH modes while capable: a texture, a copy texture and a mesh are the same pointer for the HAL whether the split is on or off.
 *       The real object is created by whichever thread executes the create (the back when on) and found through proxy->real.
 *   D4  the same-frame verdicts of drawTev / drawTevGx (0 = the HAL draws it another way) are computed on the front: pure checks from the proxy's facts, and "no program" from a per-key
 *       cache that the back answers ONCE per key (a blocking round trip the first time a key is seen).
 *   D5  readEfbRect is a blocking round trip.  peekZ / census / journal stay refused in threaded mode (gx_rt_special.c), and efbMode / efbZMode are constants that the shims answer directly.
 *   D6  gx_prog_hint is read and cleared on the front and travels in the draw record; the cut switches gx_r3a travel as an SP_R3A record whenever the front sees them change.
 *   D7  PRESENT is forwarded; the back runs the real present and calls the present-done hook (gx_thread.c), which publishes r_presented.  The front is bounded by the ring and the arena.
 *   D8  the per-bind sampler calls are shadowed per proxy on the front.
 *   D9  front stalls and back busy / starve are counted; gxsp_print() is the "[gxsp]" window line.
 *   D10 C9 (partial POS/NRM mesh update): the front hands the HAL a scratch image, records either the POS/NRM block or the full image; the back keeps a CPU copy of the mesh's image
 *       and does begin/commit on the real mesh.  (The back's copy is made at the FIRST update, which is always a full one: not at the create record -- most meshes are never updated.)
 *   D11 C6 runs on the back unchanged.
 * Memory order: payload stores, STORE_REL(pub); consumer LOAD_ACQ(pub), execute, STORE_REL(tail).  The arena is retired by the record: record.aend is the arena head after the record, and the back
 * publishes it as the arena tail after executing the record. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gx_rt.h"
#ifdef GXT_HOST
#define gxtp_host_fail_thread gxtp_host_fail_thread_sp   /* the Win32 platform header defines this global: this file is a second translation unit that includes it */
#endif
#include "gx_thread_plat.h"
#include "gx_rt_split_be.h"

#ifdef GXT_HOST
int gxt_host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf gxt_host_printf
void (*gxsp_hook_back_record)(uint32_t op);
uint32_t gxsp_host_start;                 /* host tests: the byte counters start here (the 2^32 wrap) */
#endif

extern unsigned long long gx_prog_hint;   /* gx_draw.c: the key serials of the draw being handed over (R3a C3 / R3c C5); read and cleared by the draw call */
extern unsigned gx_r3a;                   /* gx_draw.c: the cut switches, G-written at a frame edge */

#define SP_RING_DEFAULT   (256u * 1024u)
#define SP_ARENA_DEFAULT  (2u * 1024u * 1024u)
#define SP_PUB_QUANTUM    2048u
#define SP_WAIT_US        2000u
#define SP_STACK          0x100000u
#define SP_VC_N           8192u
#define SP_SECT_WORDS     4096u

enum { SP_PAD = 0, SP_PRESENT, SP_BEGINFRAME, SP_ENDFRAME, SP_CLEAR, SP_XFORM, SP_SETTEX, SP_DRAWCOL, SP_DRAWTEX, SP_TEXNEW, SP_TEXDEL, SP_TEXWRAP, SP_TEXFILT, SP_TEXLOD, SP_TEXMIPS,
       SP_CSAMP, SP_CFILT, SP_DEPTH, SP_VP, SP_SC, SP_ZB, SP_CLRRECT, SP_COPYEFB, SP_COPYFREE, SP_MESHNEW, SP_MESHUPD, SP_MESHC9, SP_MESHDEL, SP_DRAWGX, SP_DRAWTEV, SP_R3A,
       SP_VERDICT, SP_READBACK, SP_SYNC, SP_N };
enum { CLS_RING = 0, CLS_ARENA, CLS_VERDICT, CLS_READBACK, CLS_N };
/* D9: the BACK's own profile (the marker gx_thread_prof.txt = N, power of two 1..256, like rprof for R -- which, with the split on, profiles the FRONT's records): every RHI record counted per op, 1 in N timed,
 * printed every 600 presents as "[gxsp] rprof back ...".  No marker: one predicted branch per record. */
static const char *const k_opn[SP_N] = { "pad", "present", "beginFrame", "endFrame", "clear", "xform", "setTexture", "drawColored", "drawTextured", "texNew", "texDel", "texWrap", "texFilter", "texLod", "texMips",
    "copySampler", "copyFilter", "depth", "viewport", "scissor", "zbias", "clearRect", "copyEfb", "copyFree", "meshNew", "meshUpdate", "meshC9", "meshDel", "drawTevGx", "drawTev", "r3a", "verdict", "readback", "sync" };
static uint32_t pf_on, pf_mask, pf_k, pf_pres, pf_busy0, pf_read;
static uint32_t pf_cnt[SP_N], pf_smp[SP_N], pf_us[SP_N];
enum { DC_TK = 0, DC_TU, DC_VK, DC_VU, DC_ITK, DC_ITU, DC_N };      /* the GX path's four structs, then the immediate path's two: separate mirrors, so each path's "unchanged" means unchanged since ITS last draw */
#define C9_FULL 1u
#define C9_NRM  2u

typedef struct { uint8_t *sent; uint8_t *mirror; uint32_t n, nw; } DCh;
typedef struct { uint64_t key; uint8_t v, used; } VcEnt;
typedef struct { uint32_t off, n; void *heap; } Blk;

static struct Sp {
    const GxspBackend *be;
    RhiInstance *inst;
    RhiOps capops;
    int cap_done, ready, started, audit;
    uint32_t cfg_ring, cfg_arena;
    GxspPresentDone present_done;
    /* ---- the RHI ring: the front produces, the back consumes ---- */
    uint8_t *rraw, *rbuf; uint32_t rsize, rmask;
    uint32_t rhead, rpub_f, rtail_c, rec_aend;                         /* front-private (rec_aend: the arena end the last REAL record named: a PAD record carries it, never the arena head -- the record that follows a PAD has already taken its blocks) */
    volatile uint32_t rpub __attribute__((aligned(64)));               /* front -> back */
    volatile uint32_t rtail __attribute__((aligned(64)));              /* back -> front: bytes executed */
    /* ---- the arena ---- */
    uint8_t *araw, *abuf; uint32_t asize, amask, ahead, atail_c;       /* front-private */
    volatile uint32_t atail __attribute__((aligned(64)));              /* back -> front: arena bytes retired */
    /* ---- sync ---- */
    volatile int b_sleeping, f_waiting, b_alive, b_stop, b_parked, b_in_park;
    int sema_b, sema_f, b_thread, front_thread, front_core, back_core;
    volatile uint32_t sync_done; uint32_t sync_tok;
    volatile int vres; unsigned char *volatile rb_ptr; volatile int rb_ow, rb_oh;
    /* ---- front-private ---- */
    DCh dch[DC_N]; uint8_t *dmem;
    uint32_t r3a_sent; int r3a_valid;
    VcEnt *vc; uint32_t vc_used;
    uint64_t lv_key; int lv_valid, lv_v;                               /* the last GX draw's key and verdict: a draw whose key deltas are empty reuses it */
    uint64_t lt_key; int lt_valid, lt_v;                               /* ... the same for an immediate draw */
    uint8_t *c9_scr; uint32_t c9_cap; GxspProxy *c9_pend; uint32_t c9_vb; int c9_full;
    /* ---- back-private ---- */
    uint32_t btail, btail_pub, b_aend;
    /* ---- counters (cumulative; each written by one side) ---- */
    uint32_t f_recs, f_ring_bytes, f_arena_bytes, f_big, f_publishes, f_wakes, f_delta_full, f_delta_sent, f_verdict_q, f_verdict_hit, f_readbacks, f_pad_bytes;
    uint32_t f_stall_n[CLS_N], f_stall_us[CLS_N];
    uint32_t b_recs, b_busy_us, b_starve_us, b_timedwake, b_wakes, b_audit_bad, b_c9_bad, b_unknown, b_present_us;
    uint32_t ring_hw, arena_hw;
    uint32_t w_recs, w_ring, w_arena, w_stall_n[CLS_N], w_stall_us[CLS_N], w_bbusy, w_bstarve, w_dfull, w_dsent, w_vq, w_vhit, w_rb, w_big, w_pub, w_bpres;   /* window snapshot */
} S;

/* ------------------------------------------------------------------------------------------------------------------------------ helpers */
static inline void w_ptr(uint32_t **w, const void *p) { const uint64_t v = (uint64_t) (uintptr_t) p; *(*w)++ = (uint32_t) v; *(*w)++ = (uint32_t) (v >> 32); }
static inline void *r_ptr(const uint32_t **r) { const uint64_t v = (uint64_t) (*r)[0] | ((uint64_t) (*r)[1] << 32); *r += 2; return (void *) (uintptr_t) v; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline void w_blk(uint32_t **w, const Blk *b) { *(*w)++ = b->off; *(*w)++ = b->n; w_ptr(w, b->heap); }       /* 4 words */
static inline const void *r_blk(const uint32_t **r, uint32_t *n, void **heap)
{
    const uint32_t off = (*r)[0]; *n = (*r)[1]; *r += 2; *heap = r_ptr(r);
    return *heap ? (const void *) *heap : (const void *) (S.abuf + (off & S.amask));
}

static inline GxspProxy *prox(const void *h)
{
    const GxspProxy *p = (const GxspProxy *) h;
    return (p && p->magic == GXSP_PROXY_MAGIC && p->self == p) ? (GxspProxy *) p : NULL;
}
static inline void *real_of(const void *h)           /* proxy -> the backend object; a pointer that is not a proxy is passed through (never made while capable) */
{
    const GxspProxy *p = prox(h);
    return p ? p->real : (void *) h;
}
static GxspProxy *prox_new(uint32_t kind)
{
    GxspProxy *p = (GxspProxy *) calloc(1, sizeof *p);
    if (!p) return NULL;
    p->magic = GXSP_PROXY_MAGIC; p->kind = kind; p->self = p;
    return p;
}
static void prox_free(GxspProxy *p) { if (!p) return; free(p->img); p->magic = 0; p->self = NULL; free(p); }

static uint64_t hash64(const void *a, size_t n)
{
    uint32_t h1 = 2166136261u, h2 = 0x9E3779B1u;
    const uint8_t *p = (const uint8_t *) a;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) { uint32_t w; memcpy(&w, p + i, 4); h1 = (h1 ^ w) * 16777619u; h2 = (h2 + w) * 0x85EBCA6Bu; h2 ^= h2 >> 13; }
    for (; i < n; ++i) { h1 = (h1 ^ p[i]) * 16777619u; h2 = (h2 + p[i]) * 0x85EBCA6Bu; h2 ^= h2 >> 13; }
    return ((uint64_t) h1 << 32) | h2 | 1u;
}
static uint32_t fnv32(const void *a, size_t n)
{
    const uint8_t *p = (const uint8_t *) a; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------------------------------------------------------------------ the delta streams (D2) */
static uint32_t f_stage[1024] __attribute__((aligned(16)));     /* VsUniforms is 896 words: the largest struct */
static uint32_t f_scr[DC_N * SP_SECT_WORDS] __attribute__((aligned(16)));

/* One struct against what the back holds.  Writes [nruns], then per run [off | len << 16][len words]; updates the front's copy.  Returns the words written. */
static uint32_t f_delta(DCh *c, const void *cur, uint32_t *out)
{
    const uint32_t nw = c->nw;
    uint32_t *sent = (uint32_t *) c->sent;
    uint32_t o = 1, nruns = 0, i = 0;
    f_stage[nw - 1] = 0;
    memcpy(f_stage, cur, c->n);
    while (i < nw) {
        uint32_t j;
        if (f_stage[i] == sent[i]) { ++i; continue; }
        j = i + 1;
        while (j < nw && f_stage[j] != sent[j]) ++j;
        out[o++] = i | ((j - i) << 16);
        memcpy(out + o, f_stage + i, (size_t) (j - i) * 4u);
        memcpy(sent + i, f_stage + i, (size_t) (j - i) * 4u);
        o += j - i; ++nruns; i = j;
    }
    out[0] = nruns;
    S.f_delta_full += nw; S.f_delta_sent += o;
    return o;
}
static const uint32_t *b_apply(DCh *c, const uint32_t *d)
{
    uint32_t n = *d++;
    while (n--) {
        const uint32_t h = *d++, off = h & 0xFFFFu, len = h >> 16;
        if ((size_t) (off + len) * 4u <= (size_t) c->nw * 4u) memcpy(c->mirror + (size_t) off * 4u, d, (size_t) len * 4u);
        d += len;
    }
    return d;
}

/* ------------------------------------------------------------------------------------------------------------------------------ front: waits, publish, ring, arena */
static void f_publish(void)
{
    if (S.rhead == S.rpub_f) return;
    S.rpub_f = S.rhead;
    GXT_STORE_REL(&S.rpub, S.rhead);
    GXT_FENCE_SB();
    ++S.f_publishes;
    { const uint32_t occ = S.rhead - GXT_LOAD_ACQ(&S.rtail); if (occ > S.ring_hw) S.ring_hw = occ; }
    { const uint32_t occ = S.ahead - GXT_LOAD_ACQ(&S.atail); if (occ > S.arena_hw) S.arena_hw = occ; }
    if (S.b_sleeping) { ++S.f_wakes; gxtp_sema_signal(S.sema_b); }
}

/* wait until (int32)(*p - target) >= 0.  Counts the stall by class; logs every 2 s; never gives up (a wedged back is the watchdog's to report). */
static void f_wait_cursor(volatile uint32_t *p, uint32_t target, int cls)
{
    uint32_t t0, next = 2000000u, el;
    if ((int32_t) (GXT_LOAD_ACQ(p) - target) >= 0) return;
    f_publish();
    t0 = gxtp_now_us();
    ++S.f_stall_n[cls];
    for (;;) {
        S.f_waiting = 1;
        GXT_FENCE_SB();
        if ((int32_t) (GXT_LOAD_ACQ(p) - target) >= 0) { S.f_waiting = 0; break; }
        (void) gxtp_sema_wait(S.sema_f, SP_WAIT_US);
        S.f_waiting = 0;
        if ((int32_t) (GXT_LOAD_ACQ(p) - target) >= 0) break;
        el = gxtp_now_us() - t0;
        if (el >= next) {
            printf("[gxsp] front waits: %s since %u ms (back tail %u, pub %u, sync %u)\n", cls == CLS_RING ? "ring" : cls == CLS_ARENA ? "arena" : cls == CLS_VERDICT ? "verdict" : "readback",
                   el / 1000u, GXT_LOAD_ACQ(&S.rtail), GXT_LOAD_ACQ(&S.rpub), GXT_LOAD_ACQ(&S.sync_done));
            next += 2000000u;
        }
    }
    S.f_stall_us[cls] += gxtp_now_us() - t0;
}

static void f_ring_wait(uint32_t bytes)
{
    if (S.rsize - (S.rhead - S.rtail_c) >= bytes) return;
    S.rtail_c = GXT_LOAD_ACQ(&S.rtail);
    if (S.rsize - (S.rhead - S.rtail_c) >= bytes) return;
    f_wait_cursor(&S.rtail, S.rhead + bytes - S.rsize, CLS_RING);
    S.rtail_c = GXT_LOAD_ACQ(&S.rtail);
}

/* a record: header word (op | nwords << 8), the aend word (the arena head after this record), then nwords payload words (rounded up to even: records are 8-byte multiples).
 * Returns the payload.  The record is NOT published here: the next f_rec (or an explicit f_publish) publishes it. */
static uint32_t *f_rec(uint32_t op, uint32_t nw)
{
    uint32_t bytes, off, to_end;
    uint32_t *p;
    if (S.rhead - S.rpub_f >= SP_PUB_QUANTUM) f_publish();
    nw = (nw + 1u) & ~1u;
    bytes = 8u + nw * 4u;
    off = S.rhead & S.rmask; to_end = S.rsize - off;
    if (bytes > to_end) {                                  /* a PAD record fills the gap: a record never straddles the end */
        f_ring_wait(to_end);
        p = (uint32_t *) (S.rbuf + off);
        p[0] = SP_PAD | (((to_end - 8u) / 4u) << 8); p[1] = S.rec_aend;
        S.rhead += to_end; S.f_pad_bytes += to_end;
    }
    f_ring_wait(bytes);
    p = (uint32_t *) (S.rbuf + (S.rhead & S.rmask));
    p[0] = op | (nw << 8); p[1] = S.ahead; S.rec_aend = S.ahead;
    S.rhead += bytes; ++S.f_recs; S.f_ring_bytes += bytes;
    return p + 2;
}

/* an arena block of n bytes (16-aligned): the modular offset counter and the pointer.  Never wraps inside a block. */
static uint32_t f_arena(uint32_t n, uint8_t **pp)
{
    uint32_t off, need, blk;
    n = (n + 15u) & ~15u; if (!n) n = 16u;
    off = S.ahead & S.amask;
    need = (off + n > S.asize) ? (S.asize - off) + n : n;
    if (S.asize - (S.ahead - S.atail_c) < need) {
        S.atail_c = GXT_LOAD_ACQ(&S.atail);
        if (S.asize - (S.ahead - S.atail_c) < need) { f_wait_cursor(&S.atail, S.ahead + need - S.asize, CLS_ARENA); S.atail_c = GXT_LOAD_ACQ(&S.atail); }
    }
    if (off + n > S.asize) S.ahead += S.asize - off;
    blk = S.ahead; S.ahead += n;
    S.f_arena_bytes += n;
    *pp = S.abuf + (blk & S.amask);
    return blk;
}
/* a payload that is copied once: the arena, or (above a quarter of it) the heap, which the back frees */
static Blk f_blk_copy(const void *src, uint32_t n)
{
    Blk b; uint8_t *p;
    b.n = n; b.heap = NULL; b.off = 0;
    if (n > S.asize / 4u) {
        b.heap = malloc(n ? n : 1u);
        if (b.heap && n) memcpy(b.heap, src, n);
        ++S.f_big;
        return b;
    }
    b.off = f_arena(n, &p);
    if (n) memcpy(p, src, n);
    return b;
}
static Blk f_blk_alloc(uint32_t n, uint8_t **pp)       /* the caller fills it */
{
    Blk b;
    b.n = n; b.heap = NULL; b.off = 0;
    if (n > S.asize / 4u) { b.heap = malloc(n ? n : 1u); ++S.f_big; *pp = (uint8_t *) b.heap; return b; }
    b.off = f_arena(n, pp);
    return b;
}

/* the front waits for the back to execute a SYNC-class record (verdict, readback) */
static uint32_t f_sync_token(void) { return ++S.sync_tok; }
static void f_sync_wait(uint32_t tok, int cls) { f_wait_cursor(&S.sync_done, tok, cls); }

/* ------------------------------------------------------------------------------------------------------------------------------ the front: generic ops (the replaced RhiOps entries) */
static void fo_beginFrame(RhiInstance *r)
{
    if (!g_gxsp_on) { S.be->ops->beginFrame(r); return; }
    (void) f_rec(SP_BEGINFRAME, 0);
}
static void fo_endFrame(RhiInstance *r)
{
    if (!g_gxsp_on) { S.be->ops->endFrame(r); return; }
    (void) f_rec(SP_ENDFRAME, 0);
}
static void fo_clear(RhiInstance *r, float cr, float cg, float cb, float ca)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->ops->clear(r, cr, cg, cb, ca); return; }
    w = f_rec(SP_CLEAR, 4); w[0] = f2u(cr); w[1] = f2u(cg); w[2] = f2u(cb); w[3] = f2u(ca);
}
static void fo_setColorTransform(RhiInstance *r, const float m[16])
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->ops->setColorTransform(r, m); return; }
    w = f_rec(SP_XFORM, 16); for (int i = 0; i < 16; ++i) w[i] = f2u(m[i]);
}
static void fo_setTexture(RhiInstance *r, int slot, RhiTexture *h)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->ops->setTexture(r, slot, (RhiTexture *) real_of(h)); return; }
    w = f_rec(SP_SETTEX, 3); *w++ = (uint32_t) slot; w_ptr(&w, h);
}
static void fo_drawColored(RhiInstance *r, const RhiColorVertex *v, uint32_t count)
{
    uint32_t *w; Blk b;
    if (!g_gxsp_on) { S.be->ops->drawColored(r, v, count); return; }
    b = f_blk_copy(v, v ? count * (uint32_t) sizeof *v : 0u);
    w = f_rec(SP_DRAWCOL, 6); *w++ = (uint32_t) r->alphaMode; *w++ = v ? count : 0u; w_blk(&w, &b);
}
static void fo_drawTextured(RhiInstance *r, const RhiTexVertex *v, uint32_t count)
{
    uint32_t *w; Blk b;
    if (!g_gxsp_on) { S.be->ops->drawTextured(r, v, count); return; }
    b = f_blk_copy(v, v ? count * (uint32_t) sizeof *v : 0u);
    w = f_rec(SP_DRAWTEX, 6); *w++ = (uint32_t) r->alphaMode; *w++ = v ? count : 0u; w_blk(&w, &b);
}
static RhiTexture *fo_createTexture(RhiInstance *r, int w_, int h_, int mips, uint32_t fmt, const void *rgba)
{
    GxspProxy *p;
    if (!g_gxsp_on) {
        void *real = S.be->ops->createTexture(r, w_, h_, mips, fmt, rgba);
        if (!real) return NULL;
        p = prox_new(GXSP_K_TEX);
        if (!p) { S.be->ops->destroyTexture(r, (RhiTexture *) real); return NULL; }
        p->real = real; p->w = w_; p->h = h_; p->fmt = fmt;
        return (RhiTexture *) p;
    }
    if (w_ <= 0 || h_ <= 0 || !rgba) return NULL;                       /* the backend's own refusals (gxm_op_createTexture), from the arguments alone */
    if (w_ > S.be->tex_max_dim || h_ > S.be->tex_max_dim) return NULL;
    p = prox_new(GXSP_K_TEX);
    if (!p) return NULL;
    p->w = w_; p->h = h_; p->fmt = fmt;
    {   uint32_t *w; Blk b = f_blk_copy(rgba, (uint32_t) w_ * (uint32_t) h_ * 4u);
        w = f_rec(SP_TEXNEW, 10); w_ptr(&w, p); *w++ = (uint32_t) w_; *w++ = (uint32_t) h_; *w++ = (uint32_t) mips; *w++ = fmt; w_blk(&w, &b); }
    return (RhiTexture *) p;
}
static void fo_destroyTexture(RhiInstance *r, RhiTexture *h)
{
    GxspProxy *p = prox(h);
    if (!h) return;
    if (!g_gxsp_on) { S.be->ops->destroyTexture(r, (RhiTexture *) real_of(h)); prox_free(p); return; }
    { uint32_t *w = f_rec(SP_TEXDEL, 2); w_ptr(&w, h); }
}
static bool fo_present(RhiInstance *r, RhiSwapchain *sc)
{
    if (!g_gxsp_on) return S.be->ops->present(r, sc);
    {   uint32_t *w = f_rec(SP_PRESENT, 6); *w++ = 0u; *w++ = 0u; w_ptr(&w, r); w_ptr(&w, sc); f_publish(); }     /* seq 0: no done hook (gx_thread.c forwards the real ones through gxsp_f_present) */
    return true;
}

/* ------------------------------------------------------------------------------------------------------------------------------ the front: the shim entry points (GxspApi) */
static void fa_setDepthState(int test, int func, int write)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->setDepthState(test, func, write); return; }
    w = f_rec(SP_DEPTH, 3); w[0] = (uint32_t) test; w[1] = (uint32_t) func; w[2] = (uint32_t) write;
}
static void fa_setViewport(float x, float y, float w_, float h_)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->setViewport(x, y, w_, h_); return; }
    w = f_rec(SP_VP, 4); w[0] = f2u(x); w[1] = f2u(y); w[2] = f2u(w_); w[3] = f2u(h_);
}
static void fa_setScissor(int x, int y, int w_, int h_)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->setScissor(x, y, w_, h_); return; }
    w = f_rec(SP_SC, 4); w[0] = (uint32_t) x; w[1] = (uint32_t) y; w[2] = (uint32_t) w_; w[3] = (uint32_t) h_;
}
static void fa_setShadowZBias(int on)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->setShadowZBias(on); return; }
    w = f_rec(SP_ZB, 1); w[0] = (uint32_t) on;
}
static void fa_clearRect(int x, int y, int w_, int h_, uint32_t rgba, float z)
{
    uint32_t *w;
    if (!g_gxsp_on) { S.be->clearRect(x, y, w_, h_, rgba, z); return; }
    w = f_rec(SP_CLRRECT, 6); w[0] = (uint32_t) x; w[1] = (uint32_t) y; w[2] = (uint32_t) w_; w[3] = (uint32_t) h_; w[4] = rgba; w[5] = f2u(z);
}
static void fa_texInfo(RhiTexture *h, int out[4])
{
    if (!g_gxsp_on && S.be->texInfo) { S.be->texInfo((RhiTexture *) real_of(h), out); return; }
    out[0] = out[1] = out[2] = out[3] = 0;                              /* a census read: refused while threaded (RT_THREAD D5) */
}
static inline void sh_clear(GxspProxy *p) { if (p) { p->wrap_v = p->filt_v = p->lod_v = p->csamp_v = p->cfilt_v = 0; } }

static void fa_texSetWrap(RhiTexture *h, int gxS, int gxT)
{
    GxspProxy *p = prox(h);
    uint32_t *w;
    if (!g_gxsp_on) { if (p) p->wrap_v = 0; S.be->texSetWrap((RhiTexture *) real_of(h), gxS, gxT); return; }
    if (!h) return;
    if (p && p->wrap_v && p->wrap_s == gxS && p->wrap_t == gxT) return;   /* D8: the HAL calls this on every bind; the backend dedupes too */
    if (p) { p->wrap_v = 1; p->wrap_s = gxS; p->wrap_t = gxT; }
    w = f_rec(SP_TEXWRAP, 4); w_ptr(&w, h); w[0] = (uint32_t) gxS; w[1] = (uint32_t) gxT;
}
static void fa_texSetFilter(RhiTexture *h, int minGX, int magNearest, int wantMips)
{
    GxspProxy *p = prox(h);
    uint32_t *w;
    if (!g_gxsp_on) { if (p) p->filt_v = 0; S.be->texSetFilter((RhiTexture *) real_of(h), minGX, magNearest, wantMips); return; }
    if (!h) return;
    if (p && p->filt_v && p->filt_min == minGX && p->filt_mag == magNearest && p->filt_mips == wantMips) return;
    if (p) { p->filt_v = 1; p->filt_min = minGX; p->filt_mag = magNearest; p->filt_mips = wantMips; }
    w = f_rec(SP_TEXFILT, 5); w_ptr(&w, h); w[0] = (uint32_t) minGX; w[1] = (uint32_t) magNearest; w[2] = (uint32_t) wantMips;
}
static void fa_texSetLodBias(RhiTexture *h, int bias8)
{
    GxspProxy *p = prox(h);
    uint32_t *w;
    if (!g_gxsp_on) { if (p) p->lod_v = 0; S.be->texSetLodBias((RhiTexture *) real_of(h), bias8); return; }
    if (!h) return;
    if (p && p->lod_v && p->lod == bias8) return;
    if (p) { p->lod_v = 1; p->lod = bias8; }
    w = f_rec(SP_TEXLOD, 3); w_ptr(&w, h); w[0] = (uint32_t) bias8;
}
static int fa_texSetMips(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel)
{
    GxspProxy *p = prox(h);
    uint32_t mask = 0, total = 0, *w; Blk b; uint8_t *dst;
    if (!g_gxsp_on) { sh_clear(p); return S.be->texSetMips((RhiTexture *) real_of(h), nlev, rgba, maxLevel); }
    if (!p || nlev < 0 || (p->w & (p->w - 1)) || (p->h & (p->h - 1))) return 0;          /* the backend's own first refusals, from the arguments and the proxy's facts */
    if (nlev > 0 && !rgba) return 0;
    if (nlev > 24) nlev = 24;
    sh_clear(p);
    for (int L = 1; L <= nlev; ++L) {
        const int lw = p->w >> L, lh = p->h >> L;
        if (rgba[L - 1] && lw >= 1 && lh >= 1) { mask |= 1u << (L - 1); total += (uint32_t) lw * (uint32_t) lh * 4u; }
    }
    b = f_blk_alloc(total, &dst);
    for (int L = 1; L <= nlev; ++L) if (mask >> (L - 1) & 1u) { const uint32_t n = (uint32_t) (p->w >> L) * (uint32_t) (p->h >> L) * 4u; memcpy(dst, rgba[L - 1], n); dst += n; }
    w = f_rec(SP_TEXMIPS, 12); w_ptr(&w, h); *w++ = (uint32_t) nlev; *w++ = (uint32_t) maxLevel; *w++ = mask; *w++ = (uint32_t) p->w; *w++ = (uint32_t) p->h; w_blk(&w, &b);
    return 1;                                                           /* optimistic: the HAL only counts the chain */
}
static RhiTexture *fa_copyEfb(RhiTexture *reuse, int x, int y, int w_, int h_, int dw, int dh, int gxfmt)
{
    GxspProxy *p = prox(reuse);
    uint32_t *w;
    if (!g_gxsp_on) {
        RhiTexture *res = S.be->copyEfb((RhiTexture *) real_of(reuse), x, y, w_, h_, dw, dh, gxfmt);
        if (!res) return NULL;
        sh_clear(p);
        if (p) { p->real = res; p->w = dw; p->h = dh; return reuse; }
        p = prox_new(GXSP_K_COPY);
        if (!p) { S.be->copyEfbFree(res); return NULL; }
        p->real = res; p->w = dw; p->h = dh; p->fmt = (uint32_t) gxfmt;
        return (RhiTexture *) p;
    }
    if (!S.be->copy_ok(x, y, w_, h_, dw, dh, gxfmt)) return NULL;        /* a refusal from the arguments alone (format, empty rectangle): same frame, as today */
    if (!p) {
        p = prox_new(GXSP_K_COPY);
        if (!p) return NULL;
    }
    sh_clear(p); p->w = dw; p->h = dh; p->fmt = (uint32_t) gxfmt;         /* a recreated texture has default sampler state: forget what was sent */
    w = f_rec(SP_COPYEFB, 9); w_ptr(&w, p); w[0] = (uint32_t) x; w[1] = (uint32_t) y; w[2] = (uint32_t) w_; w[3] = (uint32_t) h_; w[4] = (uint32_t) dw; w[5] = (uint32_t) dh; w[6] = (uint32_t) gxfmt;
    return (RhiTexture *) p;
}
static void fa_copyTexSetFilter(RhiTexture *h, int gxMin, int gxMag)
{
    GxspProxy *p = prox(h);
    uint32_t *w;
    if (!g_gxsp_on) { if (p) p->cfilt_v = 0; S.be->copyTexSetFilter((RhiTexture *) real_of(h), gxMin, gxMag); return; }
    if (!h) return;
    if (p && p->cfilt_v && p->cf[0] == gxMin && p->cf[1] == gxMag) return;
    if (p) { p->cfilt_v = 1; p->cf[0] = gxMin; p->cf[1] = gxMag; }
    w = f_rec(SP_CFILT, 4); w_ptr(&w, h); w[0] = (uint32_t) gxMin; w[1] = (uint32_t) gxMag;
}
static void fa_copyTexSetSampler(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT)
{
    GxspProxy *p = prox(h);
    uint32_t *w;
    if (!g_gxsp_on) { if (p) p->csamp_v = 0; S.be->copyTexSetSampler((RhiTexture *) real_of(h), gxMin, gxMag, gxWrapS, gxWrapT); return; }
    if (!h) return;
    if (p && p->csamp_v && p->cs[0] == gxMin && p->cs[1] == gxMag && p->cs[2] == gxWrapS && p->cs[3] == gxWrapT) return;
    if (p) { p->csamp_v = 1; p->cs[0] = gxMin; p->cs[1] = gxMag; p->cs[2] = gxWrapS; p->cs[3] = gxWrapT; }
    w = f_rec(SP_CSAMP, 6); w_ptr(&w, h); w[0] = (uint32_t) gxMin; w[1] = (uint32_t) gxMag; w[2] = (uint32_t) gxWrapS; w[3] = (uint32_t) gxWrapT;
}
static void fa_copyEfbFree(RhiTexture *t)
{
    GxspProxy *p = prox(t);
    uint32_t *w;
    if (!t) return;
    if (!g_gxsp_on) { S.be->copyEfbFree((RhiTexture *) real_of(t)); prox_free(p); return; }
    w = f_rec(SP_COPYFREE, 2); w_ptr(&w, t);
}
static unsigned char *fa_readEfbRect(int x, int y, int w_, int h_, int *ow, int *oh)
{
    uint32_t *w, tok;
    unsigned char *res;
    if (!g_gxsp_on) return S.be->readEfbRect(x, y, w_, h_, ow, oh);
    tok = f_sync_token();
    S.rb_ptr = NULL; S.rb_ow = S.rb_oh = 0;
    w = f_rec(SP_READBACK, 5); w[0] = (uint32_t) x; w[1] = (uint32_t) y; w[2] = (uint32_t) w_; w[3] = (uint32_t) h_; w[4] = tok;
    ++S.f_readbacks;
    f_sync_wait(tok, CLS_READBACK);
    res = S.rb_ptr;
    if (res) { *ow = S.rb_ow; *oh = S.rb_oh; }
    return res;
}

/* ---- meshes ---- */
static void mesh_facts(GxspProxy *p, const GxMeshLayout *lay, uint32_t vbytes, const uint16_t *idx, uint32_t nidx)
{
    uint32_t need = 0;
    p->lay = *lay; p->nidx = nidx; p->vbytes = vbytes; p->bad = 0;
    (void) S.be->mesh_check((uint32_t) lay->stride, vbytes, idx, nidx, &need);
    p->need = need;
}
static void *fa_meshCreate(const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, const GxMeshLayout *lay)
{
    GxspProxy *p;
    uint32_t need = 0;
    if (!verts || !lay || !vbytes || (nidx && !idx)) return NULL;
    if (!S.be->layout_ok(lay) || S.be->mesh_check((uint32_t) lay->stride, vbytes, idx, nidx, &need)) return NULL;   /* pure: the same refusals as gxm */
    if (!g_gxsp_on) {
        void *real = S.be->meshCreate(verts, vbytes, idx, nidx, lay);
        if (!real) return NULL;
        p = prox_new(GXSP_K_MESH);
        if (!p) { S.be->meshDestroy(real); return NULL; }
        p->real = real; mesh_facts(p, lay, vbytes, idx, nidx);
        return p;
    }
    p = prox_new(GXSP_K_MESH);
    if (!p) return NULL;
    mesh_facts(p, lay, vbytes, idx, nidx);
    {   uint32_t *w; Blk bv = f_blk_copy(verts, vbytes), bi = f_blk_copy(idx ? (const void *) idx : (const void *) "", nidx * 2u);
        w = f_rec(SP_MESHNEW, 29); w_ptr(&w, p); *w++ = vbytes; *w++ = nidx;
        memcpy(w, lay, sizeof *lay); w += sizeof *lay / 4u;
        w_blk(&w, &bv); w_blk(&w, &bi); }
    return p;
}
/* the backend's own re-validation of an update's size (rhi_gxm_gx_mesh_update_begin) */
static int mesh_vb_ok(GxspProxy *p, uint32_t vbytes)
{
    if (!vbytes || vbytes % (uint32_t) p->lay.stride || vbytes / (uint32_t) p->lay.stride < p->need) { p->bad = 1; return 0; }
    return 1;
}
/* a size the backend would refuse (and mark the mesh bad for): the back must hear of it too, or a later switch to OFF would draw a mesh the front called bad.  The data is never read. */
static void f_notify_bad(GxspProxy *p, uint32_t vbytes)
{
    static const uint8_t zero[16] = { 0 };
    uint32_t *w; Blk b = f_blk_copy(zero, 16u);
    w = f_rec(SP_MESHUPD, 7); w_ptr(&w, p); *w++ = vbytes; w_blk(&w, &b);
}
static void fa_meshUpdate(void *mesh, const void *verts, uint32_t vbytes)
{
    GxspProxy *p = prox(mesh);
    if (!verts || !p) return;
    p->c9_primed = 0;                                                   /* a plain full update: the back's copy of the image is not maintained */
    if (!g_gxsp_on) { if (!p->bad && mesh_vb_ok(p, vbytes)) p->vbytes = vbytes; S.be->meshUpdate(p->real, verts, vbytes); return; }
    if (p->bad) { f_notify_bad(p, vbytes); return; }                    /* (the backend refuses a bad mesh's update before it looks at the size) */
    if (!mesh_vb_ok(p, vbytes)) { f_notify_bad(p, vbytes); return; }
    p->vbytes = vbytes;
    {   uint32_t *w; Blk b = f_blk_copy(verts, vbytes);
        w = f_rec(SP_MESHUPD, 7); w_ptr(&w, p); *w++ = vbytes; w_blk(&w, &b); }
}
static void fa_meshDestroy(void *mesh)
{
    GxspProxy *p = prox(mesh);
    uint32_t *w;
    if (!p) return;
    if (!g_gxsp_on) { S.be->meshDestroy(p->real); prox_free(p); return; }
    w = f_rec(SP_MESHDEL, 2); w_ptr(&w, p);
}
/* R6 C9 (D10).  OFF: the real two-half update.  ON: the HAL gets a scratch image; commit records the POS/NRM block (or, for a first / invalidated / resized update, the full image). */
static void *fa_meshBegin(void *mesh, uint32_t vbytes, int *fresh)
{
    GxspProxy *p = prox(mesh);
    if (!g_gxsp_on) {
        if (p) { p->c9_primed = 0; if (!p->bad && mesh_vb_ok(p, vbytes)) p->vbytes = vbytes; }
        return S.be->meshBegin(p ? p->real : NULL, vbytes, fresh);
    }
    {   const int inval = fresh && *fresh;
        if (fresh) *fresh = 0;
        S.c9_pend = NULL;
        if (!p || p->bad) return NULL;
        if (!mesh_vb_ok(p, vbytes)) { f_notify_bad(p, vbytes); return NULL; }
        if (S.c9_cap < vbytes) {
            void *q = realloc(S.c9_scr, vbytes + 64u);
            if (!q) return NULL;
            S.c9_scr = (uint8_t *) q; S.c9_cap = vbytes;
        }
        S.c9_full = inval || !p->c9_primed || p->vbytes != vbytes;
        S.c9_pend = p; S.c9_vb = vbytes;
        if (fresh) *fresh = S.c9_full;
        return S.c9_scr;
    }
}
static void fa_meshCommit(void *mesh)
{
    GxspProxy *p = prox(mesh);
    if (!g_gxsp_on) { S.be->meshCommit(p ? p->real : NULL); return; }
    if (!p || p != S.c9_pend) return;
    {   const uint32_t vb = S.c9_vb, stride = (uint32_t) p->lay.stride, nd = vb / stride;
        const int nrm = p->lay.offNrm >= 0;
        uint32_t *w; Blk b; uint8_t *dst;
        if (S.c9_full) {
            b = f_blk_copy(S.c9_scr, vb);
            w = f_rec(SP_MESHC9, 8); w_ptr(&w, p); *w++ = vb; *w++ = C9_FULL | (nrm ? C9_NRM : 0u); w_blk(&w, &b);
            p->c9_primed = 1;
        } else {
            b = f_blk_alloc(nd * (nrm ? 24u : 12u), &dst);
            for (uint32_t d = 0; d < nd; ++d) {
                const uint8_t *v = S.c9_scr + (size_t) d * stride;
                memcpy(dst, v + p->lay.offPos, 12); dst += 12;
                if (nrm) { memcpy(dst, v + p->lay.offNrm, 12); dst += 12; }
            }
            w = f_rec(SP_MESHC9, 8); w_ptr(&w, p); *w++ = vb; *w++ = nrm ? C9_NRM : 0u; w_blk(&w, &b);
        }
        p->vbytes = vb;
        S.c9_pend = NULL;
    }
}

/* ---- draws, D4 verdicts ---- */
static void f_r3a_sync(void)                                            /* D6: the cut switches travel in the stream, at the point the front saw them change */
{
    const unsigned m = gx_r3a;
    if (S.r3a_valid && S.r3a_sent == m) return;
    S.r3a_valid = 1; S.r3a_sent = m;
    { uint32_t *w = f_rec(SP_R3A, 1); w[0] = m; }
}
static int vc_find(uint64_t key, int *v)
{
    uint32_t i = (uint32_t) (key >> 7) & (SP_VC_N - 1u);
    for (uint32_t c = 0; c < 64; ++c, i = (i + 1u) & (SP_VC_N - 1u)) {
        if (!S.vc[i].used) return 0;
        if (S.vc[i].key == key) { *v = S.vc[i].v; return 1; }
    }
    return 0;
}
static void vc_add(uint64_t key, int v)
{
    uint32_t i;
    if (S.vc_used >= SP_VC_N / 2u) { memset(S.vc, 0, SP_VC_N * sizeof *S.vc); S.vc_used = 0; }       /* verdicts never change: forgetting costs only a round trip */
    i = (uint32_t) (key >> 7) & (SP_VC_N - 1u);
    for (uint32_t c = 0; c < SP_VC_N; ++c, i = (i + 1u) & (SP_VC_N - 1u)) if (!S.vc[i].used) { S.vc[i].used = 1; S.vc[i].key = key; S.vc[i].v = (uint8_t) v; ++S.vc_used; return; }
}
/* the back's answer for a key it has not been asked about: a blocking round trip */
static int f_verdict_ask(int gx, const VsKey *vk, const TevKey *tk)
{
    uint8_t buf[sizeof(VsKey) + sizeof(TevKey)];
    uint32_t vl = gx ? (uint32_t) sizeof *vk : 0u, tl = (uint32_t) sizeof *tk, tok, *w;
    Blk b;
    if (gx) memcpy(buf, vk, vl);
    memcpy(buf + vl, tk, tl);
    b = f_blk_copy(buf, vl + tl);
    tok = f_sync_token();
    w = f_rec(SP_VERDICT, 8); *w++ = (uint32_t) gx; *w++ = vl; *w++ = tl; w_blk(&w, &b); *w++ = tok;
    ++S.f_verdict_q;
    f_sync_wait(tok, CLS_VERDICT);
    return S.vres;
}
static int f_verdict(int gx, const VsKey *vk, const TevKey *tk, int same_as_last)
{
    uint64_t key;
    int v;
    if (gx ? (same_as_last && S.lv_valid) : (same_as_last && S.lt_valid)) return gx ? S.lv_v : S.lt_v;
    key = hash64(tk, tev_key_len(tk));
    if (gx) { key ^= hash64(vk, tev_vs_key_len(vk)) * 0x9E3779B97F4A7C15ull; key |= 1u; }
    else key = ~key;
    if (vc_find(key, &v)) { ++S.f_verdict_hit; }
    else { v = f_verdict_ask(gx, vk, tk) != 0; vc_add(key, v); }
    if (gx) { S.lv_valid = 1; S.lv_v = v; } else { S.lt_valid = 1; S.lt_v = v; }
    return v;
}

static int fa_drawTev(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16], int alphaMode)
{
    uint32_t *w, *sect = f_scr, nk, nu, nsec;
    Blk bv, bd; uint8_t *dst; uint32_t ntex, vcount;
    int rv = 1;
    if (!g_gxsp_on) {
        RhiTexture *tr[TEVK_TEX];
        if (!k || !u || !tex || !mvp) return S.be->drawTev(k, u, tex, verts, count, mvp, alphaMode);
        for (int t = 0; t < TEVK_TEX; ++t) tr[t] = (RhiTexture *) real_of(tex[t]);
        return S.be->drawTev(k, u, tr, verts, count, mvp, alphaMode);
    }
    if (!k || !u || !tex || !mvp) return 1;                             /* rhi_gxm_drawTev: malformed, consumed */
    vcount = count - count % 3u;
    if (!vcount || !verts) return 1;                                    /* nothing to draw: consumed */
    ntex = k->numTex;
    if (ntex > TEVK_TEX) return 0;
    for (uint32_t t = 0; t < ntex; ++t) if (!tex[t]) rv = 0;
    f_r3a_sync();
    nk = f_delta(&S.dch[DC_ITK], k, sect);                 sect += nk;
    nu = f_delta(&S.dch[DC_ITU], u, sect);                 sect += nu;
    nsec = (uint32_t) (sect - f_scr);
    if (rv) rv = f_verdict(0, NULL, k, nk == 1u);          /* nk == 1: no run, the key is what the last immediate draw had */
    else S.lt_valid = 0;
    bd = f_blk_copy(f_scr, nsec * 4u);
    bv = f_blk_alloc(vcount * (uint32_t) sizeof *verts, &dst); memcpy(dst, verts, (size_t) vcount * sizeof *verts);
    w = f_rec(SP_DRAWTEV, 29 + 2u * ntex);
    *w++ = (uint32_t) alphaMode; *w++ = vcount; w_blk(&w, &bv); *w++ = ntex; w_blk(&w, &bd);
    *w++ = S.audit ? fnv32(k, sizeof *k) : 0u; *w++ = S.audit ? fnv32(u, sizeof *u) : 0u;
    for (int i = 0; i < 16; ++i) *w++ = f2u(mvp[i]);
    for (uint32_t t = 0; t < ntex; ++t) w_ptr(&w, tex[t]);
    return rv;
}

static int fa_drawTevGx(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode)
{
    const unsigned long long hint = gx_prog_hint;
    GxspProxy *mp;
    uint32_t *w, *sect = f_scr, nk, nu, nv, nvu, nsec, ntex;
    Blk bd;
    int rv = 1;
    gx_prog_hint = 0ull;                                    /* D6: read and cleared here, exactly where the backend did; it travels in the record */
    if (!g_gxsp_on) {
        RhiTexture *tr[TEVK_TEX];
        for (int t = 0; t < TEVK_TEX; ++t) tr[t] = (RhiTexture *) real_of(tex ? tex[t] : NULL);
        mp = prox(mesh);
        return S.be->drawTevGx(tk, u, tex ? tr : NULL, vk, vu, mp ? mp->real : mesh, alphaMode, hint);
    }
    mp = prox(mesh);
    /* the pure refusals of rhi_gxm_drawTevGx (rhi_gxm.c:622 and the layout checks), from the proxy's facts: the HAL's CPU fallback must see 0 in the SAME frame */
    if (!mp || !mp->nidx || !tk || !u || !tex || !vk || !vu || mp->bad) rv = 0;
    else {
        const GxMeshLayout *L = &mp->lay;
        if ((vk->hasNrm && L->offNrm < 0) || (vk->hasClr0 && L->offClr < 0) || (vk->hasMtxIdx && L->offMtx < 0)) rv = 0;
        for (int n = 0; rv && n < 8; ++n) if ((vk->texMask >> n & 1) && L->offTex[n] < 0) rv = 0;
        if (rv && ((vk->hasBin && L->offBin < 0) || (vk->hasTan && L->offTan < 0))) rv = 0;
        if (rv) { const unsigned tg = tev_vs_tmi_groups(vk); if (((tg & 1) && L->offTmi[0] < 0) || ((tg & 2) && L->offTmi[1] < 0)) rv = 0; }
        if (rv && tk->numTex > TEVK_TEX) rv = 0;
        for (int t = 0; rv && t < tk->numTex; ++t) if (!tex[t]) rv = 0;
    }
    if (!tk || !u || !vk || !vu) return 0;                  /* nothing to record: the backend counts a malformed call as refused_pre and so does the HAL's 0 */
    ntex = tk->numTex > TEVK_TEX ? 0u : tk->numTex;
    f_r3a_sync();
    nk = f_delta(&S.dch[DC_TK], tk, sect);                  sect += nk;
    nu = f_delta(&S.dch[DC_TU], u, sect);                   sect += nu;
    nv = f_delta(&S.dch[DC_VK], vk, sect);                  sect += nv;
    nvu = f_delta(&S.dch[DC_VU], vu, sect);                 sect += nvu;
    nsec = (uint32_t) (sect - f_scr);
    if (rv) rv = f_verdict(1, vk, tk, nk == 1u && nv == 1u);   /* both keys unchanged since the last GX draw: its verdict stands */
    else S.lv_valid = 0;
    bd = f_blk_copy(f_scr, nsec * 4u);
    w = f_rec(SP_DRAWGX, 14 + 2u * ntex);
    *w++ = (uint32_t) alphaMode; *w++ = (uint32_t) hint; *w++ = (uint32_t) (hint >> 32); w_ptr(&w, mesh);
    *w++ = ntex; w_blk(&w, &bd);
    *w++ = S.audit ? fnv32(tk, sizeof *tk) : 0u; *w++ = S.audit ? fnv32(u, sizeof *u) : 0u; *w++ = S.audit ? fnv32(vk, sizeof *vk) : 0u; *w++ = S.audit ? fnv32(vu, sizeof *vu) : 0u;
    for (uint32_t t = 0; t < ntex; ++t) w_ptr(&w, tex[t]);
    return rv;
}

static const GxspApi k_api = {
    fa_setDepthState, fa_setViewport, fa_setScissor, fa_setShadowZBias, fa_clearRect, fa_texInfo, fa_texSetWrap, fa_texSetFilter, fa_texSetLodBias, fa_texSetMips, fa_copyEfb,
    fa_copyTexSetFilter, fa_copyTexSetSampler, fa_copyEfbFree, fa_readEfbRect, fa_drawTev, fa_meshCreate, fa_meshUpdate, fa_meshDestroy, fa_meshBegin, fa_meshCommit, fa_drawTevGx,
};

/* ------------------------------------------------------------------------------------------------------------------------------ the back: the executor */
static inline const RhiOps *b_ops(void) { return S.be->ops; }
static inline RhiInstance *b_ex(void) { return S.be->exec_inst ? S.be->exec_inst : S.be->inst; }

static void b_exec(uint32_t op, const uint32_t *r)
{
    const GxspBackend *be = S.be;
    void *heap = NULL, *heap2 = NULL;
    switch (op) {
    case SP_PAD: break;
    case SP_PRESENT: {
        const uint32_t seq = r[0]; const int arm = (int) r[1]; uint32_t t0, dt;
        struct RhiInstance *rhi; struct RhiSwapchain *sc;
        r += 2; rhi = (struct RhiInstance *) r_ptr(&r); sc = (struct RhiSwapchain *) r_ptr(&r);
        t0 = gxtp_now_us();
        (void) b_ops()->present(rhi, sc);
        dt = gxtp_now_us() - t0; S.b_present_us += dt;
        if (S.present_done && seq) S.present_done(seq, arm, dt);
        break;
    }
    case SP_BEGINFRAME: b_ops()->beginFrame(b_ex()); break;
    case SP_ENDFRAME:   b_ops()->endFrame(b_ex()); break;
    case SP_CLEAR:      b_ops()->clear(b_ex(), u2f(r[0]), u2f(r[1]), u2f(r[2]), u2f(r[3])); break;
    case SP_XFORM: { float m[16]; for (int i = 0; i < 16; ++i) m[i] = u2f(r[i]); b_ops()->setColorTransform(b_ex(), m); break; }
    case SP_SETTEX: { const int slot = (int) r[0]; r += 1; b_ops()->setTexture(b_ex(), slot, (RhiTexture *) real_of(r_ptr(&r))); break; }
    case SP_DRAWCOL: case SP_DRAWTEX: {
        const int am = (int) r[0]; const uint32_t count = r[1]; uint32_t n; const void *v;
        r += 2; v = r_blk(&r, &n, &heap);
        b_ex()->alphaMode = am;
        if (op == SP_DRAWCOL) b_ops()->drawColored(b_ex(), count ? (const RhiColorVertex *) v : NULL, count);
        else b_ops()->drawTextured(b_ex(), count ? (const RhiTexVertex *) v : NULL, count);
        break;
    }
    case SP_TEXNEW: {
        GxspProxy *p = (GxspProxy *) r_ptr(&r); const int w = (int) r[0], h = (int) r[1], mips = (int) r[2]; const uint32_t fmt = r[3]; uint32_t n; const void *px;
        r += 4; px = r_blk(&r, &n, &heap);
        p->real = b_ops()->createTexture(b_ex(), w, h, mips, fmt, px);
        break;
    }
    case SP_TEXDEL: { GxspProxy *p = prox(r_ptr(&r)); if (p) { b_ops()->destroyTexture(b_ex(), (RhiTexture *) p->real); prox_free(p); } break; }
    case SP_TEXWRAP:  { void *h = r_ptr(&r); be->texSetWrap((RhiTexture *) real_of(h), (int) r[0], (int) r[1]); break; }
    case SP_TEXFILT:  { void *h = r_ptr(&r); be->texSetFilter((RhiTexture *) real_of(h), (int) r[0], (int) r[1], (int) r[2]); break; }
    case SP_TEXLOD:   { void *h = r_ptr(&r); be->texSetLodBias((RhiTexture *) real_of(h), (int) r[0]); break; }
    case SP_TEXMIPS: {
        void *h = r_ptr(&r); const int nlev = (int) r[0], maxl = (int) r[1]; const uint32_t mask = r[2]; const int tw = (int) r[3], th = (int) r[4]; uint32_t n; const unsigned char *src;
        unsigned char *lv[24]; r += 5; src = (const unsigned char *) r_blk(&r, &n, &heap);
        for (int L = 1; L <= 24; ++L) lv[L - 1] = NULL;
        for (int L = 1; L <= nlev; ++L) if (mask >> (L - 1) & 1u) { lv[L - 1] = (unsigned char *) src; src += (size_t) (tw >> L) * (size_t) (th >> L) * 4u; }
        (void) be->texSetMips((RhiTexture *) real_of(h), nlev, lv, maxl);
        break;
    }
    case SP_CSAMP: { void *h = r_ptr(&r); be->copyTexSetSampler((RhiTexture *) real_of(h), (int) r[0], (int) r[1], (int) r[2], (int) r[3]); break; }
    case SP_CFILT: { void *h = r_ptr(&r); be->copyTexSetFilter((RhiTexture *) real_of(h), (int) r[0], (int) r[1]); break; }
    case SP_DEPTH: be->setDepthState((int) r[0], (int) r[1], (int) r[2]); break;
    case SP_VP:    be->setViewport(u2f(r[0]), u2f(r[1]), u2f(r[2]), u2f(r[3])); break;
    case SP_SC:    be->setScissor((int) r[0], (int) r[1], (int) r[2], (int) r[3]); break;
    case SP_ZB:    be->setShadowZBias((int) r[0]); break;
    case SP_CLRRECT: be->clearRect((int) r[0], (int) r[1], (int) r[2], (int) r[3], r[4], u2f(r[5])); break;
    case SP_COPYEFB: {
        GxspProxy *p = (GxspProxy *) r_ptr(&r); RhiTexture *res;
        res = be->copyEfb((RhiTexture *) p->real, (int) r[0], (int) r[1], (int) r[2], (int) r[3], (int) r[4], (int) r[5], (int) r[6]);
        if (res) p->real = res;
        break;
    }
    case SP_COPYFREE: { GxspProxy *p = prox(r_ptr(&r)); if (p) { be->copyEfbFree((RhiTexture *) p->real); prox_free(p); } break; }
    case SP_MESHNEW: {
        GxspProxy *p = (GxspProxy *) r_ptr(&r); const uint32_t vb = r[0], nidx = r[1]; GxMeshLayout lay; uint32_t n; const void *verts, *idx;
        r += 2; memcpy(&lay, r, sizeof lay); r += sizeof lay / 4u;
        verts = r_blk(&r, &n, &heap); idx = r_blk(&r, &n, &heap2);
        p->real = be->meshCreate(verts, vb, nidx ? (const uint16_t *) idx : NULL, nidx, &lay);
        break;
    }
    case SP_MESHUPD: { GxspProxy *p = (GxspProxy *) r_ptr(&r); const uint32_t vb = r[0]; uint32_t n; const void *v; r += 1; v = r_blk(&r, &n, &heap); free(p->img); p->img = NULL; p->imgn = 0; be->meshUpdate(p->real, v, vb); break; }
    case SP_MESHC9: {
        GxspProxy *p = (GxspProxy *) r_ptr(&r); const uint32_t vb = r[0], fl = r[1]; uint32_t n; const uint8_t *blk; int f = 0; uint8_t *dst;
        const uint32_t stride = (uint32_t) p->lay.stride, nd = vb / stride; const int nrm = (fl & C9_NRM) != 0;
        r += 2; blk = (const uint8_t *) r_blk(&r, &n, &heap);
        if (fl & C9_FULL) {
            if (p->imgn != vb) { free(p->img); p->img = (uint8_t *) malloc(vb); p->imgn = p->img ? vb : 0; }
            if (p->img) memcpy(p->img, blk, vb);
            f = 1;                                                      /* every slot's image is stale: the whole write */
            dst = (uint8_t *) be->meshBegin(p->real, vb, &f);
            if (dst) { memcpy(dst, blk, vb); be->meshCommit(p->real); }
        } else {
            const uint32_t per = nrm ? 24u : 12u;
            if (!p->img || p->imgn != vb) { ++S.b_c9_bad; break; }     /* a partial update without the image it patches: skipped, counted (the front sends a full image first) */
            for (uint32_t d = 0; d < nd; ++d) {
                memcpy(p->img + (size_t) d * stride + p->lay.offPos, blk + (size_t) d * per, 12);
                if (nrm) memcpy(p->img + (size_t) d * stride + p->lay.offNrm, blk + (size_t) d * per + 12, 12);
            }
            dst = (uint8_t *) be->meshBegin(p->real, vb, &f);          /* f = 0 in: no slot is declared stale */
            if (dst) {
                if (f) memcpy(dst, p->img, vb);                          /* a slot that does not hold the image: the whole image from the back's copy */
                else for (uint32_t d = 0; d < nd; ++d) {
                    memcpy(dst + (size_t) d * stride + p->lay.offPos, blk + (size_t) d * per, 12);
                    if (nrm) memcpy(dst + (size_t) d * stride + p->lay.offNrm, blk + (size_t) d * per + 12, 12);
                }
                be->meshCommit(p->real);
            }
        }
        break;
    }
    case SP_MESHDEL: { GxspProxy *p = prox(r_ptr(&r)); if (p) { be->meshDestroy(p->real); prox_free(p); } break; }
    case SP_R3A: be->setR3a(1, r[0]); break;
    case SP_DRAWGX: {
        const int am = (int) r[0]; const unsigned long long hint = (unsigned long long) r[1] | ((unsigned long long) r[2] << 32); void *mesh; uint32_t ntex, n; const uint32_t *d; RhiTexture *tex[TEVK_TEX];
        GxspProxy *mp;
        r += 3; mesh = r_ptr(&r); ntex = r[0]; r += 1; d = (const uint32_t *) r_blk(&r, &n, &heap);
        { const uint32_t a0 = r[0], a1 = r[1], a2 = r[2], a3 = r[3]; r += 4;
          d = b_apply(&S.dch[DC_TK], d); d = b_apply(&S.dch[DC_TU], d); d = b_apply(&S.dch[DC_VK], d); d = b_apply(&S.dch[DC_VU], d);
          if (a0 | a1 | a2 | a3) {
              if (fnv32(S.dch[DC_TK].mirror, S.dch[DC_TK].n) != a0 || fnv32(S.dch[DC_TU].mirror, S.dch[DC_TU].n) != a1 || fnv32(S.dch[DC_VK].mirror, S.dch[DC_VK].n) != a2 || fnv32(S.dch[DC_VU].mirror, S.dch[DC_VU].n) != a3) ++S.b_audit_bad;
          } }
        for (int t = 0; t < TEVK_TEX; ++t) tex[t] = NULL;
        for (uint32_t t = 0; t < ntex; ++t) tex[t] = (RhiTexture *) real_of(r_ptr(&r));
        mp = prox(mesh);
        (void) be->drawTevGx((const TevKey *) S.dch[DC_TK].mirror, (const TevUniforms *) S.dch[DC_TU].mirror, tex, (const VsKey *) S.dch[DC_VK].mirror, (const VsUniforms *) S.dch[DC_VU].mirror,
                             mp ? mp->real : mesh, am, hint);
        break;
    }
    case SP_DRAWTEV: {
        const int am = (int) r[0]; const uint32_t count = r[1]; uint32_t n, ntex; const TevVertex *verts; const uint32_t *d; float mvp[16]; RhiTexture *tex[TEVK_TEX]; void *heap3 = NULL; uint32_t n2;
        r += 2; verts = (const TevVertex *) r_blk(&r, &n, &heap); ntex = r[0]; r += 1; d = (const uint32_t *) r_blk(&r, &n2, &heap3);
        { const uint32_t a0 = r[0], a1 = r[1]; r += 2;
          d = b_apply(&S.dch[DC_ITK], d); d = b_apply(&S.dch[DC_ITU], d);
          if ((a0 | a1) && (fnv32(S.dch[DC_ITK].mirror, S.dch[DC_ITK].n) != a0 || fnv32(S.dch[DC_ITU].mirror, S.dch[DC_ITU].n) != a1)) ++S.b_audit_bad; }
        for (int i = 0; i < 16; ++i) mvp[i] = u2f(*r++);
        for (int t = 0; t < TEVK_TEX; ++t) tex[t] = NULL;
        for (uint32_t t = 0; t < ntex; ++t) tex[t] = (RhiTexture *) real_of(r_ptr(&r));
        (void) be->drawTev((const TevKey *) S.dch[DC_ITK].mirror, (const TevUniforms *) S.dch[DC_ITU].mirror, tex, verts, count, mvp, am);
        free(heap3);
        break;
    }
    case SP_VERDICT: {
        const uint32_t gx = r[0], vl = r[1]; uint32_t n; const uint8_t *keys; int v;
        r += 3; keys = (const uint8_t *) r_blk(&r, &n, &heap);
        {   const uint32_t tok = r[0];
            v = gx ? be->verdictGx((const VsKey *) keys, (const TevKey *) (keys + vl)) : be->verdictTev((const TevKey *) keys);
            S.vres = v;
            GXT_STORE_REL(&S.sync_done, tok); }
        break;
    }
    case SP_READBACK: {
        const uint32_t tok = r[4]; int ow = 0, oh = 0;
        unsigned char *res = be->readEfbRect((int) r[0], (int) r[1], (int) r[2], (int) r[3], &ow, &oh);
        S.rb_ow = ow; S.rb_oh = oh; S.rb_ptr = res;
        GXT_STORE_REL(&S.sync_done, tok);
        break;
    }
    case SP_SYNC: GXT_STORE_REL(&S.sync_done, r[0]); break;
    default: ++S.b_unknown; break;
    }
    free(heap); free(heap2);
}

static void b_publish_tails(void)
{
    GXT_STORE_REL(&S.atail, S.b_aend);
    GXT_STORE_REL(&S.rtail, S.btail);
    S.btail_pub = S.btail;
    GXT_FENCE_SB();
    if (S.f_waiting) gxtp_sema_signal(S.sema_f);
}

static void pf_print(uint32_t frames)
{
    double tot = 0.0; int printed = 0;
    printf("[gxsp] rprof back: %u presents, sample 1/%u | back busy %.2f ms/frame\n", frames, pf_mask + 1u, (S.b_busy_us - pf_busy0) / 1000.0 / frames);
    for (;;) {
        double best = -1.0; int bi = -1;
        for (int i = 0; i < SP_N; ++i) if (pf_cnt[i]) { const double est = pf_smp[i] ? (double) pf_us[i] / pf_smp[i] * pf_cnt[i] : 0.0; if (est > best) { best = est; bi = i; } }
        if (bi < 0) break;
        tot += best;
        if (printed < 24 && best / 1000.0 / frames >= 0.05) {
            printf("[gxsp] rprof back op %s: %.1f/frame %.2f us each (%u samples) = %.2f ms/frame\n", k_opn[bi], (double) pf_cnt[bi] / frames, pf_smp[bi] ? (double) pf_us[bi] / pf_smp[bi] : 0.0, pf_smp[bi], best / 1000.0 / frames);
            ++printed;
        }
        pf_cnt[bi] = 0;
    }
    printf("[gxsp] rprof back: attributed %.2f ms/frame\n", tot / 1000.0 / frames);
    memset(pf_cnt, 0, sizeof pf_cnt); memset(pf_smp, 0, sizeof pf_smp); memset(pf_us, 0, sizeof pf_us); pf_busy0 = S.b_busy_us;
}

static void b_main(void)
{
    const uint32_t T_SPIN = GXT_R_SPIN_US;
    GXT_STORE_REL(&S.b_alive, 1);
    for (;;) {
        uint32_t pub;
        if (S.b_stop) break;
        if (S.b_parked) {
            GXT_STORE_REL(&S.b_in_park, 1);
            while (S.b_parked && !S.b_stop) (void) gxtp_sema_wait(S.sema_b, 100000u);
            GXT_STORE_REL(&S.b_in_park, 0);
            continue;
        }
        pub = GXT_LOAD_ACQ(&S.rpub);
        if (S.btail != pub) {
            const uint32_t tb = gxtp_now_us(), pres0 = S.b_present_us;
            while (S.btail != pub) {
                const uint32_t hdr = *(const uint32_t *) (S.rbuf + (S.btail & S.rmask));
                const uint32_t op = hdr & 255u, nw = hdr >> 8, bytes = 8u + nw * 4u;
                const uint32_t *pl = (const uint32_t *) (S.rbuf + (S.btail & S.rmask)) + 2;
                S.b_aend = ((const uint32_t *) (S.rbuf + (S.btail & S.rmask)))[1];
#ifdef GXT_HOST
                if (gxsp_hook_back_record) gxsp_hook_back_record(op);
#endif
                if (pf_on && op < SP_N) {
                    const int smp = ((++pf_k & pf_mask) == 0);
                    const uint32_t t0 = smp ? gxtp_now_us() : 0u;
                    b_exec(op, pl);
                    if (smp) { pf_us[op] += gxtp_now_us() - t0; ++pf_smp[op]; }
                    ++pf_cnt[op];
                    if (op == SP_PRESENT && ++pf_pres % 600u == 0u) pf_print(600u);
                } else b_exec(op, pl);
                if (op != SP_PAD) ++S.b_recs;
                S.btail += bytes;
                if ((uint32_t) (S.btail - S.btail_pub) >= 8192u) b_publish_tails();
            }
            S.b_busy_us += (gxtp_now_us() - tb) - (S.b_present_us - pres0);
            b_publish_tails();
            continue;
        }
        {   const uint32_t ti = gxtp_now_us();
            int found = 0; uint32_t n = 0;
            for (;;) {
                gxtp_relax();
                if (GXT_LOAD_ACQ(&S.rpub) != S.btail) { found = 1; break; }
                if ((++n & 15u) == 0 && (uint32_t) (gxtp_now_us() - ti) >= T_SPIN) break;
            }
            if (!found && !S.b_stop && !S.b_parked) {
                S.b_sleeping = 1;
                GXT_FENCE_SB();
                if (GXT_LOAD_ACQ(&S.rpub) != S.btail) S.b_sleeping = 0;
                else {
                    const int wk = gxtp_sema_wait(S.sema_b, GXT_R_TIMED_WAKE_US);
                    const int data = GXT_LOAD_ACQ(&S.rpub) != S.btail;
                    S.b_sleeping = 0;
                    if (wk > 0) ++S.b_wakes; else if (data) ++S.b_timedwake;
                }
            }
            S.b_starve_us += gxtp_now_us() - ti;
        }
    }
    b_publish_tails();
    GXT_STORE_REL(&S.b_alive, 0);
}

/* ------------------------------------------------------------------------------------------------------------------------------ control */
static void set_affinity_thread(int tid, int core)
{
#if defined(__vita__) && !defined(GXT_HOST)
    const int m = gxtp_core_mask(core);
    if (tid > 0 && m) (void) sceKernelChangeThreadCpuAffinityMask(tid, m);
#else
    (void) tid; (void) core;
#endif
}

/* sizes (0 = the default) and the audit switch.  Called before an enable: a different geometry than the one allocated frees the buffers (the back is stopped and the split is off) so the next enable allocates the new one. */
void gxsp_host_config(uint32_t ring_bytes, uint32_t arena_bytes, int audit)
{
    if (S.ready && !g_gxsp_on && !GXT_LOAD_ACQ(&S.b_alive) && (ring_bytes != S.cfg_ring || arena_bytes != S.cfg_arena)) {
        free(S.rraw); free(S.araw); free(S.dmem); free(S.vc);
        S.rraw = S.araw = NULL; S.dmem = NULL; S.vc = NULL; S.vc_used = 0; S.ready = 0;
    }
    S.cfg_ring = ring_bytes; S.cfg_arena = arena_bytes; S.audit = audit;
}
void gxsp_set_present_done(GxspPresentDone f) { S.present_done = f; }

int gxsp_cap_init(const GxspBackend *be)
{
    if (!be || !be->inst || !be->ops) return 0;
    S.be = be; S.inst = be->inst;
    S.capops = *be->ops;
    S.capops.present = fo_present; S.capops.beginFrame = fo_beginFrame; S.capops.endFrame = fo_endFrame; S.capops.clear = fo_clear;
    S.capops.drawColored = fo_drawColored; S.capops.setColorTransform = fo_setColorTransform; S.capops.createTexture = fo_createTexture; S.capops.destroyTexture = fo_destroyTexture;
    S.capops.setTexture = fo_setTexture; S.capops.drawTextured = fo_drawTextured;
    be->inst->ops = &S.capops;
    g_gxsp_api = &k_api; g_gxsp_cap = 1;
    S.cap_done = 1;
    return 1;
}
void gxsp_cap_attach(RhiInstance *inst) { if (S.cap_done && inst && inst->ops != &S.capops) inst->ops = &S.capops; }   /* host: a create that re-sets the ops table */

static int sp_alloc(void)
{
    uint32_t total = 0;
    S.rsize = S.cfg_ring ? S.cfg_ring : SP_RING_DEFAULT; S.asize = S.cfg_arena ? S.cfg_arena : SP_ARENA_DEFAULT;
    while (S.rsize & (S.rsize - 1u)) S.rsize += S.rsize & -S.rsize;              /* the offsets are masked: both sizes are powers of two (round up) */
    while (S.asize & (S.asize - 1u)) S.asize += S.asize & -S.asize;
    S.rmask = S.rsize - 1u; S.amask = S.asize - 1u;
    S.rraw = (uint8_t *) malloc((size_t) S.rsize + 64u); S.araw = (uint8_t *) malloc((size_t) S.asize + 64u);
    if (!S.rraw || !S.araw) { free(S.rraw); free(S.araw); S.rraw = S.araw = NULL; return 0; }
    S.rbuf = (uint8_t *) (((uintptr_t) S.rraw + 63u) & ~(uintptr_t) 63u); S.abuf = (uint8_t *) (((uintptr_t) S.araw + 63u) & ~(uintptr_t) 63u);
    memset(S.rbuf, 0xA5, S.rsize);
    {   static const uint32_t sz[DC_N] = { sizeof(TevKey), sizeof(TevUniforms), sizeof(VsKey), sizeof(VsUniforms), sizeof(TevKey), sizeof(TevUniforms) };
        for (int i = 0; i < DC_N; ++i) { S.dch[i].n = sz[i]; S.dch[i].nw = (sz[i] + 3u) / 4u; total += 2u * ((sz[i] + 31u) & ~15u); }
        S.dmem = (uint8_t *) calloc(1, total + 64u);
        if (!S.dmem) return 0;
        {   uint8_t *p = (uint8_t *) (((uintptr_t) S.dmem + 15u) & ~(uintptr_t) 15u);
            for (int i = 0; i < DC_N; ++i) { const uint32_t one = (sz[i] + 31u) & ~15u; S.dch[i].sent = p; S.dch[i].mirror = p + one; p += 2u * one; } } }
    S.vc = (VcEnt *) calloc(SP_VC_N, sizeof *S.vc);
    if (!S.vc) return 0;
    S.ready = 1;
    return 1;
}
static int sp_start(int back_core)
{
    uint32_t t0;
    S.sema_b = gxtp_sema_create("stfx_gxb_b"); S.sema_f = gxtp_sema_create("stfx_gxb_f");
    if (S.sema_b <= 0 || S.sema_f <= 0) return 0;
    S.b_parked = 1; S.b_stop = 0; S.b_alive = 0; S.b_in_park = 0;
    S.b_thread = gxtp_thread_create("stfx_gxb", b_main, 0x10000100, SP_STACK, back_core);
    if (S.b_thread <= 0) return 0;
    if (gxtp_thread_start(S.b_thread) < 0) { gxtp_thread_delete(S.b_thread); S.b_thread = 0; return 0; }
    t0 = gxtp_now_us();
    while (!GXT_LOAD_ACQ(&S.b_alive)) { if ((uint32_t) (gxtp_now_us() - t0) > 2000000u) return 0; gxtp_delay_us(500u); }
    S.started = 1;
    printf("[gxsp] back thread started: stfx_gxb, core %d, ring %u KB, arena %u KB\n", back_core, S.rsize / 1024u, S.asize / 1024u);
    return 1;
}

int gxsp_g_enable(int front_core, int back_core, int front_thread)
{
    uint32_t t0, base = 0;
    if (!S.cap_done) return 0;
    if (g_gxsp_on) return 1;
    if (!pf_read) {                                       /* D9: the back's profile marker, read once */
        char b[16]; const int n = gxtp_read_marker("gx_thread_prof.txt", b, (int) sizeof b);
        pf_read = 1;
        if (n > 0) { const unsigned long v = strtoul(b, NULL, 10); if (v >= 1ul && v <= 256ul && !(v & (v - 1ul))) { pf_on = 1; pf_mask = (uint32_t) v - 1u; } }
    }
    if (!S.ready && !sp_alloc()) { printf("[gxsp] REFUSED: allocation failed\n"); return 0; }
    if (!S.started && !sp_start(back_core)) { printf("[gxsp] REFUSED: back thread did not start\n"); return 0; }
    t0 = gxtp_now_us();
    while (!GXT_LOAD_ACQ(&S.b_in_park)) { if ((uint32_t) (gxtp_now_us() - t0) > 2000000u) { printf("[gxsp] REFUSED: back did not park\n"); return 0; } gxtp_delay_us(200u); }
#ifdef GXT_HOST
    base = gxsp_host_start & ~7u;
#endif
    S.rhead = S.rpub_f = S.rtail_c = S.btail = S.btail_pub = base; S.rpub = S.rtail = base;
    S.ahead = S.atail_c = S.b_aend = S.rec_aend = base; S.atail = base;
    S.sync_tok = 0; S.sync_done = 0; S.vres = 1;
    for (int i = 0; i < DC_N; ++i) { memset(S.dch[i].sent, 0, S.dch[i].nw * 4u); memset(S.dch[i].mirror, 0, S.dch[i].nw * 4u); }
    S.r3a_valid = 0; S.lv_valid = S.lt_valid = 0; S.c9_pend = NULL;
    S.front_thread = front_thread; S.front_core = front_core; S.back_core = back_core;
    if (front_core >= 0) set_affinity_thread(front_thread, front_core);
    S.b_parked = 0;
    GXT_FENCE_SB();
    g_gxsp_on = 1;
    GXT_FENCE_SB();
    gxtp_sema_signal(S.sema_b);
    return 1;
}

int gxsp_g_drain(const char *why)
{
    uint32_t target, t0, next = 2000000u;
    if (!S.started || !g_gxsp_on) return 0;
    target = S.rhead;                                   /* the front is idle (the GX ring is drained) and has published before its tail passed the last record */
    t0 = gxtp_now_us();
    while ((int32_t) (GXT_LOAD_ACQ(&S.rtail) - target) < 0) {
        const uint32_t el = gxtp_now_us() - t0;
        if (el >= next) { printf("[gxsp] G waits for the back: %s since %u ms (back tail %u, target %u)\n", why ? why : "?", el / 1000u, GXT_LOAD_ACQ(&S.rtail), target); next += 2000000u; }
        if (el >= 10000000u) return -1;
        gxtp_delay_us(50u);
    }
    return 0;
}

void gxsp_g_disable(int rcore)
{
    if (!g_gxsp_on) return;
    (void) gxsp_g_drain("split off");
    g_gxsp_on = 0;
    GXT_FENCE_SB();
    if (S.be && S.be->setR3a) S.be->setR3a(0, 0);              /* the back is drained and idle: its private copy of the cut switches goes back to G's live ones */
    S.b_parked = 1;
    gxtp_sema_signal(S.sema_b);
    if (rcore >= 0) set_affinity_thread(S.front_thread, rcore);
}
void gxsp_g_set_pins(int front_core) { (void) front_core; }

void gxsp_f_publish(void) { if (g_gxsp_on) f_publish(); }

void gxsp_f_present(uint32_t seq, int arm, struct RhiInstance *rhi, struct RhiSwapchain *sc)
{
    uint32_t *w = f_rec(SP_PRESENT, 6);
    *w++ = seq; *w++ = (uint32_t) arm; w_ptr(&w, rhi); w_ptr(&w, sc);
    f_publish();                                        /* the back starts the present now, not at the next quantum */
}

int gxsp_f_quit(void)
{
    int joined = 1;
    if (!S.started) return 1;
    if (g_gxsp_on) {
        uint32_t tok = f_sync_token(), *w = f_rec(SP_SYNC, 1); w[0] = tok;
        f_publish();
        f_sync_wait(tok, CLS_VERDICT);
    }
    S.b_stop = 1;
    GXT_FENCE_SB();
    gxtp_sema_signal(S.sema_b);
    for (int i = 0; i < 3 && !(joined = gxtp_thread_join(S.b_thread, 2000000u)); ++i) { }
    if (joined) { gxtp_thread_delete(S.b_thread); S.b_thread = 0; S.started = 0; printf("[gxsp] back stopped\n"); }
    else printf("[gxsp] back did not stop\n");
    g_gxsp_on = 0;
    GXT_FENCE_SB();
    return joined;
}

uint32_t gxsp_stall_total(void)
{
    uint32_t n = 0;
    for (int i = 0; i < CLS_N; ++i) n += S.f_stall_n[i];
    return n;
}

void gxsp_print(uint32_t frames)
{
    const double F = frames ? frames : 1u;
    uint32_t sn[CLS_N], su[CLS_N];
    if (!S.cap_done || !S.ready) return;
    for (int i = 0; i < CLS_N; ++i) { sn[i] = S.f_stall_n[i] - S.w_stall_n[i]; su[i] = S.f_stall_us[i] - S.w_stall_us[i]; }
    printf("[gxsp] %s | front: recs/frame %.1f ring %.1f KB/frame arena %.1f KB/frame (struct words sent %.1f%%) big %u | stalls ring %u/%.2fms arena %u/%.2fms verdict %u/%.2fms (asked %u, cached %u) readback %u/%.2fms"
           " | back: busy %.2f starve %.2f present %.2f ms/frame, bad %u c9bad %u unknown %u | hw ring %u KB arena %u KB\n",
           g_gxsp_on ? "ON" : "off",
           (S.f_recs - S.w_recs) / F, (S.f_ring_bytes - S.w_ring) / 1024.0 / F, (S.f_arena_bytes - S.w_arena) / 1024.0 / F,
           (S.f_delta_full - S.w_dfull) ? 100.0 * (S.f_delta_sent - S.w_dsent) / (S.f_delta_full - S.w_dfull) : 0.0, S.f_big - S.w_big,
           sn[CLS_RING], su[CLS_RING] / 1000.0 / F, sn[CLS_ARENA], su[CLS_ARENA] / 1000.0 / F, sn[CLS_VERDICT], su[CLS_VERDICT] / 1000.0 / F, S.f_verdict_q - S.w_vq, S.f_verdict_hit - S.w_vhit,
           sn[CLS_READBACK], su[CLS_READBACK] / 1000.0 / F,
           (S.b_busy_us - S.w_bbusy) / 1000.0 / F, (S.b_starve_us - S.w_bstarve) / 1000.0 / F, (S.b_present_us - S.w_bpres) / 1000.0 / F, S.b_audit_bad, S.b_c9_bad, S.b_unknown,
           (S.ring_hw + 1023u) / 1024u, (S.arena_hw + 1023u) / 1024u);
    S.w_recs = S.f_recs; S.w_ring = S.f_ring_bytes; S.w_arena = S.f_arena_bytes; S.w_dfull = S.f_delta_full; S.w_dsent = S.f_delta_sent; S.w_big = S.f_big; S.w_vq = S.f_verdict_q; S.w_vhit = S.f_verdict_hit;
    for (int i = 0; i < CLS_N; ++i) { S.w_stall_n[i] = S.f_stall_n[i]; S.w_stall_us[i] = S.f_stall_us[i]; }
    S.w_bbusy = S.b_busy_us; S.w_bstarve = S.b_starve_us; S.w_bpres = S.b_present_us;
    S.ring_hw = S.arena_hw = 0;
}

void gxsp_stall_report(void)
{
    if (!S.cap_done) return;
    printf("[gxsp] STALL on %d rhead %u rpub %u rtail %u ahead %u atail %u b_sleeping %d f_waiting %d b_alive %d b_parked %d sync %u/%u recs f %u b %u\n", g_gxsp_on, S.rhead, GXT_LOAD_ACQ(&S.rpub),
           GXT_LOAD_ACQ(&S.rtail), S.ahead, GXT_LOAD_ACQ(&S.atail), S.b_sleeping, S.f_waiting, S.b_alive, S.b_parked, S.sync_done, S.sync_tok, S.f_recs, S.b_recs);
}

#ifdef GXT_HOST
/* host tests: the counters the equivalence run checks (0 audit mismatches, 1 C9 partial updates without an image, 2 unknown records, 3 records, 4 PAD bytes, 5 heap blocks, 6 keys asked, 7 keys answered from the cache,
 * 8 ring stalls, 9 arena stalls, 10 arena bytes, 11 ring bytes, 12 sent struct words, 13 struct words, 14 verdict stalls, 15 readbacks) */
uint32_t gxsp_host_stat(int w)
{
    switch (w) {
    case 0: return S.b_audit_bad;  case 1: return S.b_c9_bad;  case 2: return S.b_unknown;  case 3: return S.f_recs;  case 4: return S.f_pad_bytes;  case 5: return S.f_big;
    case 6: return S.f_verdict_q;  case 7: return S.f_verdict_hit;  case 8: return S.f_stall_n[CLS_RING];  case 9: return S.f_stall_n[CLS_ARENA];  case 10: return S.f_arena_bytes;
    case 11: return S.f_ring_bytes;  case 12: return S.f_delta_sent;  case 13: return S.f_delta_full;  case 14: return S.f_stall_n[CLS_VERDICT];  case 15: return S.f_readbacks;
    default: return 0;
    }
}
#endif
