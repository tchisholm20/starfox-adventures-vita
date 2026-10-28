/* gxm_ring.h -- VERBATIM COPY of projects/vita-gxmprobe/src/ring.h (the reviewed notification-guarded ring: host-tested by
 * vita-gxmprobe/test/run_ring_test.sh incl. the RING_INJECT_OLD_BUG mutant). Copied 2026-10-20 so the native backend (src/rhi_gxm_frame.c)
 * can include it without a path into another project. ONLY the include guard, the line endings (LF), this comment and the #ifndef around RING_MAX_SHADOW differ. If ring.h changes upstream, re-copy.
 * It is the frame module's: no other module includes it. */
/* ring.h -- the notification-guarded vertex ring, in a header with NO Vita includes
 * so test/ring_test.c can compile it on the host and attack it.
 *
 * WHY THIS IS A HEADER: the first version lived inside main.c, where the only check
 * on it was "the picture is right" -- and Vita3K runs scenes in order, so a ring that
 * hands out memory the GPU has not finished reading still gives the right picture.
 * That version was in fact wrong (wrap decided by `waste != 0`, so a head that had
 * reached the end never wrapped: the 5th 64 B alloc of a 256 B ring went out at
 * offset 256, and `waste = cap - head` underflowed on the next call). The picture
 * never showed it. Three independent checks now exist:
 *   1. test/ring_test.c: a fake GPU reads each scene's bytes late and verifies them
 *      (watched failing on the old allocator: -DRING_INJECT_OLD_BUG).
 *   2. a SHADOW MODEL in here: every live (allocated, not-truly-retired) range is
 *      recorded and each new alloc is checked for overlap -> `overwrote`. It does not
 *      share the `used/head` arithmetic it is checking.
 *   3. `oob` / `max_end`: every returned range must lie inside [0, cap).
 * The app logs all of them and tools/gxmprobe_check.py FAILs on any non-zero.
 *
 * Contract: ring_alloc() returns memory no scene whose fence has not been WAITED ON
 * still reads. A fence is retired only by ring_retire_oldest() calling the wait
 * callback (sceGxmNotificationWait in the app). `skip_wait` is the BREAK knob: it
 * retires the ACCOUNTING without waiting, which is exactly the hazard being guarded.
 */
#ifndef STFX_GXM_RING_H
#define STFX_GXM_RING_H

#include <stdint.h>
#include <stddef.h>

#define RING_MAX_FENCES 64
#ifndef RING_MAX_SHADOW     /* integrator, 2026-10-20: guarded so rhi_gxm_frame.c can size the shadow model (see there); the default is the reviewed 256 */
#define RING_MAX_SHADOW 256
#endif

typedef void (*ring_wait_fn)(void *user, uint32_t tag);

typedef struct { uint32_t off, size, seq; int live; } RingShadow;
typedef struct { uint32_t bytes, seq, tag; } RingFence;

typedef struct {
    uint8_t *base;
    uint32_t cap;
    uint32_t head, used, scene_bytes;
    uint32_t seq;                       /* sequence number of the scene being recorded */
    RingFence fq[RING_MAX_FENCES];
    int fq_head, fq_count;
    ring_wait_fn wait;
    void *user;
    int skip_wait;                      /* BREAK: free the accounting without waiting */
    /* counters (all logged; the checker fails on fatal/oob/overwrote/shadow_full) */
    int allocs, wraps, waits, skipped_waits, fatal, oob, overwrote, shadow_full;
    uint32_t max_end;
    RingShadow sh[RING_MAX_SHADOW];
    int sh_n;
    int shadow_on;                      /* the shadow model: O(sh_n) per alloc AND per retire (131 us per immediate draw on hardware at
                                           sh_n ~4100, 2026-10-20) -- a verification aid, on by default (host tests), off on the console */
} Ring;

static inline void ring_init(Ring *r, uint8_t *base, uint32_t cap, ring_wait_fn wait, void *user)
{
    uint8_t *b = (uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++) { b[i] = 0; }
    r->base = base; r->cap = cap; r->wait = wait; r->user = user;
    r->shadow_on = 1;
}

static inline void ring_retire_oldest(Ring *r)
{
    RingFence *f = &r->fq[r->fq_head];
    if (r->skip_wait) {
        r->skipped_waits++;             /* the GPU may still be reading: shadow stays live */
    } else {
        r->wait(r->user, f->tag);
        r->waits++;
        if (r->shadow_on) for (int i = 0; i < r->sh_n; i++) {
            if (r->sh[i].live && r->sh[i].seq == f->seq) { r->sh[i].live = 0; }
        }
    }
    r->used -= f->bytes;
    r->fq_head = (r->fq_head + 1) % RING_MAX_FENCES;
    r->fq_count--;
}

/* Returns GPU-visible memory for `size` bytes (rounded up to 32) that no scene with an
 * unretired fence still reads; NULL (and fatal++) if it cannot ever fit. */
static inline void *ring_alloc(Ring *r, uint32_t size)
{
    size = (size + 31u) & ~31u;
    if (size > r->cap) { r->fatal++; return NULL; }
    uint32_t waste, wrap;
    for (;;) {
#ifdef RING_INJECT_OLD_BUG
        /* the original: wrap only if `waste != 0`, so head == cap never wraps */
        waste = (r->head + size > r->cap) ? (r->cap - r->head) : 0;
        wrap = waste != 0;
#else
        wrap = (r->head + size > r->cap);
        waste = wrap ? (r->cap - r->head) : 0;
#endif
        if (r->used + size + waste <= r->cap) { break; }
        if (r->fq_count == 0) { r->fatal++; return NULL; }
        ring_retire_oldest(r);
    }
    if (wrap) { r->head = 0; r->used += waste; r->scene_bytes += waste; r->wraps++; }
    uint32_t off = r->head;

    /* ---- shadow model + bounds, independent of head/used ---- */
    if (off + size > r->cap) { r->oob++; }
    if (off + size > r->max_end) { r->max_end = off + size; }
    if (r->shadow_on) {
    for (int i = 0; i < r->sh_n; i++) {
        if (r->sh[i].live && off < r->sh[i].off + r->sh[i].size && r->sh[i].off < off + size) {
            r->overwrote++;
            break;
        }
    }
    int slot = -1;
    for (int i = 0; i < r->sh_n; i++) { if (!r->sh[i].live) { slot = i; break; } }
    if (slot < 0 && r->sh_n < RING_MAX_SHADOW) { slot = r->sh_n++; }
    if (slot < 0) { r->shadow_full++; }
    else { r->sh[slot].off = off; r->sh[slot].size = size; r->sh[slot].seq = r->seq; r->sh[slot].live = 1; }
    }

    r->head += size;
    r->used += size;
    r->scene_bytes += size;
    r->allocs++;
    return r->base + off;
}

/* The scene being recorded has ended; `tag` identifies its notification. */
static inline void ring_scene_ended(Ring *r, uint32_t tag)
{
    if (r->fq_count == RING_MAX_FENCES) { ring_retire_oldest(r); }   /* queue full: must wait */
    RingFence *f = &r->fq[(r->fq_head + r->fq_count) % RING_MAX_FENCES];
    f->bytes = r->scene_bytes; f->seq = r->seq; f->tag = tag;
    r->fq_count++;
    r->scene_bytes = 0;
    r->seq++;
}

#endif
