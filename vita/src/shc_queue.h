/* shc_queue.h -- the bookkeeping of the BACKGROUND program-binary SAVE (2026-10-11, docs/47 "Run 224" follow-up).
 * No GL, no OS: rhi_vita_tev.inc (device) and host/tests/shc_queue_test.c compile this same header.
 *
 * WHY. Run 224: every first-time program cost ~550 ms on the main thread and 390-485 ms of it was shc_save writing the
 * program's binary to the card (211 saves = 86.6 s of 92.7 s). The binary is now copied into a malloc'd image the QUEUE
 * OWNS, and a writer thread puts it on the card; the main thread never touches the card for a save.
 *
 * OWNERSHIP (the whole point; every rule is a host test):
 *   - main thread only: sq_reap, sq_room, sq_drop, sq_enqueue, sq_find*, sq_abandon, the byte accounting. It is the ONLY
 *     thread that frees an image, and only a DONE/FAILED slot's (the writer is finished with it by then).
 *   - writer thread only: sq_take, sq_finish, and READING img/len of a slot it holds (WRITING).
 *   - a slot's `state` is the hand-off: FREE -> QUEUED (main) -> WRITING (writer, or FAILED if main abandons it first:
 *     both sides use a compare-and-swap) -> DONE | FAILED (writer) -> FREE (main, after freeing the image).
 *   - the main thread may READ the image of a QUEUED / WRITING / DONE slot at the same time as the writer does (both read).
 * BOUNDS: SQ_SLOTS images and `max_bytes` in all. A save that does not fit is DROPPED and counted, never waited for; the
 * program works this boot and is compiled again next boot (and again, correctly, if the LRU evicts it and asks for it back).
 * THE EVICTED-WITH-A-PENDING-SAVE CASE: shc_load asks sq_find_valid first, so a reload of a program whose file has not
 * landed yet links from the queued bytes. A slot whose write FAILED or was dropped is simply not found: the load misses
 * and the caller compiles from source -- correct, just slow.
 * TORN WRITES: sq_write_image writes to a temp name and renames onto the final one, so a crash or power loss mid-write
 * leaves the temp file (overwritten by the next write) and the final name untouched; shc_image_valid rejects a short or
 * re-keyed image on load. The on-disk image layout is the one shc_load has always read:
 *   u32 magic, u32 version, u32 nkey, u32 binary-length, key bytes padded to 4, binary. */
#ifndef SHC_QUEUE_H
#define SHC_QUEUE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SQ_SLOTS 64
enum { SQ_FREE = 0, SQ_QUEUED, SQ_WRITING, SQ_DONE, SQ_FAILED };
#define SQ_BARRIER() __sync_synchronize()

typedef struct {
    volatile int state;
    uint32_t h, kind, seq;
    uint8_t *img; unsigned len;
    unsigned us;          /* the writer's time for this image, set before DONE */
    int abandoned;        /* main set it, with the CAS QUEUED -> FAILED, at shutdown */
} SqEnt;

typedef struct {
    SqEnt e[SQ_SLOTS];
    unsigned max_bytes, bytes, seq;                       /* main thread only; bytes = images held in non-FREE slots */
    unsigned queued, written, failed, dropped_full, dropped_dup, abandoned, depth_max, served;
    unsigned long long written_us; unsigned write_us_max, bytes_written;
} Sq;

static void sq_init(Sq *q, unsigned max_bytes) { memset(q, 0, sizeof *q); q->max_bytes = max_bytes; }

/* main: take back every slot the writer has finished with */
static void sq_reap(Sq *q)
{
    for (int i = 0; i < SQ_SLOTS; ++i) {
        SqEnt *e = &q->e[i]; const int s = e->state;
        if (s != SQ_DONE && s != SQ_FAILED) continue;
        SQ_BARRIER();
        if (s == SQ_DONE) { ++q->written; q->written_us += e->us; if (e->us > q->write_us_max) q->write_us_max = e->us; q->bytes_written += e->len; }
        else if (!e->abandoned) ++q->failed;
        free(e->img); e->img = NULL;
#ifndef SQ_BREAK_5   /* BREAK 5: the bytes of a finished image are never given back -- the queue fills up and drops everything */
        q->bytes -= e->len;
#endif
        e->len = 0; e->abandoned = 0;
        SQ_BARRIER();
        e->state = SQ_FREE;
    }
}
static unsigned sq_pending(const Sq *q)   /* QUEUED + WRITING: what a flush waits for (safe from any thread) */
{
    unsigned n = 0;
    for (int i = 0; i < SQ_SLOTS; ++i) n += q->e[i].state == SQ_QUEUED || q->e[i].state == SQ_WRITING;
    return n;
}
static unsigned sq_slots_used(const Sq *q) { unsigned n = 0; for (int i = 0; i < SQ_SLOTS; ++i) n += q->e[i].state != SQ_FREE; return n; }
/* main: 0 = an image of `len` bytes fits (a free slot, under the byte cap); -1 = it does not. Counts nothing. */
static int sq_room(const Sq *q, unsigned len)
{
    int free_slot = 0;
    for (int i = 0; i < SQ_SLOTS; ++i) if (q->e[i].state == SQ_FREE) { free_slot = 1; break; }
#ifdef SQ_BREAK_1   /* BREAK 1: no byte cap -- the queue is unbounded in memory */
    return free_slot ? 0 : -1;
#else
    return free_slot && q->bytes + len <= q->max_bytes ? 0 : -1;
#endif
}
static void sq_drop(Sq *q) { ++q->dropped_full; }
/* main: hand `img` (malloc'd, `len` bytes) to the queue. 0 = the queue owns it now. <0 = refused: the CALLER still owns it
 * and frees it (-1 full or over the byte cap, -2 an identical save is already waiting). */
static int sq_enqueue(Sq *q, uint32_t h, uint32_t kind, uint8_t *img, unsigned len)
{
    if (!img || !len) return -1;
    for (int i = 0; i < SQ_SLOTS; ++i)
        if (q->e[i].state == SQ_QUEUED && q->e[i].h == h && q->e[i].kind == kind) { ++q->dropped_dup; return -2; }
    if (sq_room(q, len)) { ++q->dropped_full; return -1; }
    for (int i = 0; i < SQ_SLOTS; ++i) {
        SqEnt *e = &q->e[i];
        if (e->state != SQ_FREE) continue;
        e->h = h; e->kind = kind; e->seq = ++q->seq; e->img = img; e->len = len; e->us = 0; e->abandoned = 0;
        q->bytes += len; ++q->queued;
        SQ_BARRIER();
        e->state = SQ_QUEUED;
        { unsigned d = sq_slots_used(q); if (d > q->depth_max) q->depth_max = d; }
        return 0;
    }
    ++q->dropped_full;
    return -1;
}
/* writer: the oldest QUEUED slot, now WRITING (its image is the writer's to read), or NULL */
static SqEnt *sq_take(Sq *q)
{
    for (;;) {
        SqEnt *b = NULL;
        for (int i = 0; i < SQ_SLOTS; ++i) if (q->e[i].state == SQ_QUEUED && (!b || q->e[i].seq < b->seq)) b = &q->e[i];
        if (!b) return NULL;
        if (__sync_bool_compare_and_swap(&b->state, SQ_QUEUED, SQ_WRITING)) return b;   /* lost to sq_abandon: look again */
    }
}
static void sq_finish(SqEnt *e, int ok, unsigned us) { e->us = us; SQ_BARRIER(); e->state = ok ? SQ_DONE : SQ_FAILED; }
/* main, at exit when the wait ran out: every slot the writer has not started is given up (counted), never written */
static unsigned sq_abandon(Sq *q)
{
    unsigned n = 0;
    for (int i = 0; i < SQ_SLOTS; ++i) {
        SqEnt *e = &q->e[i];
        if (e->state != SQ_QUEUED) continue;
#ifdef SQ_BREAK_6   /* BREAK 6: an abandoned slot is left QUEUED -- the writer would start it after the process was told to stop, and it is not counted */
        continue;
#endif
        e->abandoned = 1;
        if (__sync_bool_compare_and_swap(&e->state, SQ_QUEUED, SQ_FAILED)) { ++q->abandoned; ++n; } else e->abandoned = 0;
    }
    return n;
}

/* a file image is usable for THIS key: right size, magic, generator version, key bytes. */
static int shc_image_valid(const uint8_t *img, unsigned n, uint32_t magic, uint32_t version, const void *k1, unsigned n1, const void *k2, unsigned n2)
{
    const unsigned klen = (n1 + n2 + 3u) & ~3u, hdr = 16u + klen;
    uint32_t w[4];
    if (n < hdr + 16) return 0;
    memcpy(w, img, sizeof w);
    if (w[0] != magic || w[1] != version || w[2] != n1 + n2) return 0;
#ifndef SQ_BREAK_4   /* BREAK 4: the recorded binary length is not compared with what is there -- a torn (short) image loads */
    if (w[3] != n - hdr) return 0;
#endif
    if (memcmp(img + 16, k1, n1) || (n2 && memcmp(img + 16 + n1, k2, n2))) return 0;
    return 1;
}
/* main: the newest queued / being-written / just-written image for (h, kind) that is valid for this key, or NULL */
static const SqEnt *sq_find_valid(Sq *q, uint32_t h, uint32_t kind, uint32_t magic, uint32_t version, const void *k1, unsigned n1, const void *k2, unsigned n2)
{
    const SqEnt *b = NULL;
    for (int i = 0; i < SQ_SLOTS; ++i) {
        const SqEnt *e = &q->e[i]; const int s = e->state;
        if (e->h != h || e->kind != kind) continue;
#ifdef SQ_BREAK_2   /* BREAK 2: a save still QUEUED is invisible to a reload -- the evicted program is recompiled for nothing */
        if (s != SQ_WRITING && s != SQ_DONE) continue;
#else
        if (s != SQ_QUEUED && s != SQ_WRITING && s != SQ_DONE) continue;
#endif
        if (!e->img || !shc_image_valid(e->img, e->len, magic, version, k1, n1, k2, n2)) continue;
        if (!b || e->seq > b->seq) b = e;
    }
    if (b) ++q->served;
    return b;
}

/* ---- the file side: the same sequence on the device (sceIo) and in the host test (an in-memory card) ---- */
typedef struct {
    void *user;
    int (*open_w)(void *user, const char *path);                       /* create/truncate; fd >= 0 */
    int (*write)(void *user, int fd, const void *p, unsigned n);       /* bytes written, or < 0 */
    int (*close)(void *user, int fd);
    int (*rename)(void *user, const char *from, const char *to);       /* < 0: refused (e.g. the target exists) */
    int (*remove)(void *user, const char *path);
} ShcIo;
/* 1 = `final` now holds exactly `img`. 0 = it does not (and `final` still holds what it held before: a torn write never reaches it). */
static int sq_write_image(const ShcIo *io, const char *tmp, const char *final, const void *img, unsigned len)
{
#ifdef SQ_BREAK_3   /* BREAK 3: written straight onto the final name -- a crash mid-write leaves a torn file there */
    (void) tmp;
    int fd = io->open_w(io->user, final);
    if (fd < 0) return 0;
    int w = io->write(io->user, fd, img, len); io->close(io->user, fd);
    if (w != (int) len) { io->remove(io->user, final); return 0; }
    return 1;
#else
    int fd = io->open_w(io->user, tmp);
    if (fd < 0) return 0;
    int w = io->write(io->user, fd, img, len); io->close(io->user, fd);
    if (w != (int) len) { io->remove(io->user, tmp); return 0; }
    if (io->rename(io->user, tmp, final) < 0) {          /* sceIoRename does not replace: take the old one away, once */
        io->remove(io->user, final);
        if (io->rename(io->user, tmp, final) < 0) { io->remove(io->user, tmp); return 0; }
    }
    return 1;
#endif
}
#endif
