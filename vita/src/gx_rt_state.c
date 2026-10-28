/* gx_rt_state.c -- THE STATE RECORD of the GX render thread (locked-30 lever R2; contract host/engine/R2_STATE.md).
 *
 * WHAT.  While R2 is active the game thread G does not record GX state setters.  Each runs the real HAL body on G's own instance of the HAL state struct (gx_hal_g, port/gx_hal_state.h)
 * and sets a dirty bit per group it wrote.  Before G appends ANYTHING else to the ring (INV-1: gx_rt.h's gxt_rec_alloc_impl and gx_thread.c's gxt_push_slow call gxs_flush), the dirty groups go
 * out as ONE GXT_OP_STATE record carrying the group bytes by value, and R copies them into the instance the draw path reads (gx_hal).  R never calls a setter while R2 is active.
 *
 * WHY THIS CANNOT RECUR THE FILTER'S ALIAS CLASS.  No setter semantics are modelled here: the HAL body that stores a cell is the code that marks its group (gxh_b_<Name>, gxh_lint.py), and a
 * group travels as the BYTES of the instance, so a field written by an alias spelling travels with its group.  What can still go wrong is (1) a body that stores without marking and (2) R-side
 * code that writes the struct; the whole-struct AUDIT (gx_thread_saudit.txt N) catches both: every Nth STATE record carries a hash of every group of G's instance, R hashes its own after the apply
 * and counts the groups that differ.  Host tests run it at N = 1.
 *
 * WHO WRITES WHAT.  G: gx_hal_g, the dirty header, flushes/groups/bytes.  R: gx_hal (group bytes + hooks), applies/bad/audit counters.  The two instances never share a byte. */
#define GXS_DEFINE 1          /* strong definitions of the weak hooks declared in gx_rt.h (a weak DEFINITION on MinGW becomes an alias to the next symbol) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gx_rt.h"
#include "gx_rt_state.h"
#include "port/gx_hal_state.h"

#ifdef GXT_HOST
int gxt_host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf gxt_host_printf
#endif

/* G's and R's statics are different objects on different cache lines (the lint's data-symbol check, and no false sharing between the producer and the consumer) */
static struct GxsG {
    int      audit_every;                 /* 0 = off */
    uint32_t flush_n;                     /* flushes since gxs_begin (the audit cadence) */
    uint32_t flushes, groups, bytes;      /* STATE records sent, groups in them, payload bytes */
    GxsStats snap;                        /* the window snapshot gxs_print subtracts (merged G + R counters) */
    unsigned calls_snap;
} SG;
static struct GxsR {
    uint32_t applies, bad;                /* records applied / refused */
    uint32_t audit_checked, audit_mismatch, audit_groups_bad;
    uint32_t logged;
} SR __attribute__((aligned(64)));
static void stats_now(GxsStats *o)
{
    o->flushes = SG.flushes; o->groups = SG.groups; o->bytes = SG.bytes;
    o->applies = SR.applies; o->bad = SR.bad; o->audit_checked = SR.audit_checked; o->audit_mismatch = SR.audit_mismatch; o->audit_groups_bad = SR.audit_groups_bad;
}

static inline uint32_t group_words(unsigned gid) { return (uint32_t) ((gxh_groups[gid].bytes + 3u) >> 2); }

uint32_t gxs_group_hash(const void *hal, unsigned gid)
{
    const uint8_t *b = (const uint8_t *) hal + gxh_groups[gid].off;
    uint32_t h = 2166136261u, n = gxh_groups[gid].bytes, i;
    for (i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}

int gxs_first_diff(const void *a, const void *b)
{
    unsigned g;
    for (g = 0; g < GXH_NGROUPS; ++g) if (memcmp((const uint8_t *) a + gxh_groups[g].off, (const uint8_t *) b + gxh_groups[g].off, gxh_groups[g].bytes) != 0) return (int) g;
    return -1;
}

void gxs_get_stats(GxsStats *out) { stats_now(out); }
void gxs_reset_stats(void) { SG.flushes = SG.groups = SG.bytes = 0; memset(&SG.snap, 0, sizeof SG.snap); gxt_state_calls = 0; SG.calls_snap = 0; memset(&SR, 0, sizeof SR); }
void gxs_config(int audit_every) { SG.audit_every = audit_every < 0 ? 0 : audit_every; }

/* ------------------------------------------------------------------------------------------------------------------------------ G: begin / flush / end */
void gxs_begin(void)
{
    /* R is parked (DIRECT -> threaded) or drained (legacy -> R2): gx_hal is G's program-order state */
    memcpy(&gx_hal_g, &gx_hal, sizeof gx_hal_g);
    memset(gx_hal_g.dirty, 0, sizeof gx_hal_g.dirty);
    gx_hal_g.dirty_any = 0;
    SG.flush_n = 0;
    gxt_state_dirty = &gx_hal_g.dirty_any;
    gxt_state_on = 1;
}

void gxs_flush(void)
{
    GxHal *g = &gx_hal_g;
    uint32_t dirty[8], w, bits, words = 1, n = 0, *p, *q;
    int audit;
    if (!g->dirty_any) return;
    memcpy(dirty, g->dirty, sizeof dirty);
    memset(g->dirty, 0, sizeof g->dirty);
    g->dirty_any = 0;                                         /* clear BEFORE allocating: the allocation's own INV-1 check must see a clean set (no re-entry) */
    for (w = 0; w < 8u; ++w)
        for (bits = dirty[w]; bits; bits &= bits - 1u) {
            const unsigned gid = w * 32u + (unsigned) __builtin_ctz(bits);
            if (gid >= GXH_NGROUPS) continue;                 /* cannot happen: the marks are bounded; the high bits of the last word are never set */
            words += 1u + group_words(gid); ++n;
        }
    audit = SG.audit_every > 0 && (++SG.flush_n % (uint32_t) SG.audit_every) == 0u;
    if (audit) words += GXH_NGROUPS;
    p = q = gxt_rec_alloc_impl(GXT_OP_STATE, words, 0);
    *q++ = GXS_MAGIC | (n << 8) | (audit ? GXS_F_AUDIT : 0u);
    for (w = 0; w < 8u; ++w)
        for (bits = dirty[w]; bits; bits &= bits - 1u) {
            const unsigned gid = w * 32u + (unsigned) __builtin_ctz(bits);
            const uint32_t gw = group_words(gid);
            if (gid >= GXH_NGROUPS) continue;
            *q++ = (gid << 16) | gw;
            q[gw - 1u] = 0u;                                  /* zero the pad of the last word */
            memcpy(q, (const uint8_t *) g + gxh_groups[gid].off, gxh_groups[gid].bytes);
            q += gw;
        }
    if (audit) { unsigned gid; for (gid = 0; gid < GXH_NGROUPS; ++gid) *q++ = gxs_group_hash(g, gid); }
    (void) p;
    ++SG.flushes; SG.groups += n; SG.bytes += words * 4u;
}

void gxs_end(void)
{
    if (*gxt_state_dirty != 0u) {
        if (g_gxt.on) gxs_flush();
        else { memset(gx_hal_g.dirty, 0, sizeof gx_hal_g.dirty); gx_hal_g.dirty_any = 0; }
    }
    gxt_state_dirty = &gxt_state_zero;
    gxt_state_on = 0;
}

/* ------------------------------------------------------------------------------------------------------------------------------ R: validate + apply */
static void bad_record(const char *why, uint32_t a, uint32_t b)
{
    ++SR.bad;
    if (SR.logged++ < 8u) printf("[gxt] state: BAD STATE RECORD (%s %u %u): dropped whole\n", why, a, b);
}

int gxs_r_apply(const uint32_t *p, uint32_t nw)
{
    GxHal *r = &gx_hal;
    uint32_t n, flags, pos = 1u, k, prev = 0xFFFFFFFFu;
    if (nw < 1u) { bad_record("empty", nw, 0u); return -1; }
    if ((p[0] & 0xFF000000u) != GXS_MAGIC) { bad_record("magic", p[0], 0u); return -1; }
    n = (p[0] >> 8) & 0xFFFFu; flags = p[0] & 0xFFu;
    /* pass 1: validate the whole record before anything is copied */
    for (k = 0; k < n; ++k) {
        uint32_t e, gid, gw;
        if (pos >= nw) { bad_record("truncated", k, n); return -1; }
        e = p[pos]; gid = e >> 16; gw = e & 0xFFFFu;
        if (gid >= GXH_NGROUPS) { bad_record("unknown group", gid, 0u); return -1; }
        if (gw != group_words(gid)) { bad_record("size mismatch", gid, gw); return -1; }
        if (prev != 0xFFFFFFFFu && gid <= prev) { bad_record("order", gid, prev); return -1; }
        pos += 1u + gw; prev = gid;
        if (pos > nw) { bad_record("overrun", gid, pos); return -1; }
    }
    if (pos + ((flags & GXS_F_AUDIT) ? GXH_NGROUPS : 0u) != nw) { bad_record("length", pos, nw); return -1; }
    /* pass 2: copy each group into R's instance, then the post-apply hooks (the derived state R keeps outside the struct) */
    pos = 1u;
    for (k = 0; k < n; ++k) {
        const uint32_t e = p[pos], gid = e >> 16, gw = e & 0xFFFFu;
        memcpy((uint8_t *) r + gxh_groups[gid].off, p + pos + 1u, gxh_groups[gid].bytes);
        gxh_kdirty_set(r, gxh_groups[gid].kind);              /* R3a C3: the draw path's memos see this group's kind change (gxh_collect) -- a writer of gx_hal that is not a setter body */
        if (gxh_groups[gid].hook) gxh_post_apply(gxh_groups[gid].hook);
        pos += 1u + gw;
    }
    ++SR.applies;
    if (flags & GXS_F_AUDIT) {
        unsigned gid, nbad = 0;
        for (gid = 0; gid < GXH_NGROUPS; ++gid) {
            const uint32_t h = gxs_group_hash(r, gid);
            if (h != p[pos + gid]) {
                if (SR.logged++ < 8u) printf("[gxt] state: AUDIT MISMATCH group %u (%s): R's %08x, G's %08x\n", gid, gxh_kind_names[gxh_groups[gid].kind], h, p[pos + gid]);
                ++nbad;
            }
        }
        ++SR.audit_checked;
        if (nbad) { ++SR.audit_mismatch; SR.audit_groups_bad += nbad; }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------------------ the report line */
void gxs_print(uint32_t frames)
{
    const double F = frames ? (double) frames : 1.0;
    GxsStats nowst; const GxsStats *a = &nowst, *b = &SG.snap;
    stats_now(&nowst);
    if (g_gxt.on) {
        printf("[gxt] state: on %d flushes/f %.1f groups/f %.1f bytes/f %.0f s_calls/f %.1f bad %u audit %u/%u\n", gxt_state_on,
               (a->flushes - b->flushes) / F, (a->groups - b->groups) / F, (a->bytes - b->bytes) / F, (gxt_state_calls - SG.calls_snap) / F,
               a->bad, a->audit_checked, a->audit_mismatch);
    }
    SG.snap = *a; SG.calls_snap = gxt_state_calls;
}
