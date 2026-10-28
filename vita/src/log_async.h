/* log_async.h -- the buffered log's hand-off to a writer thread (Task 2, 2026-10-12). Header-only; vita_tool_main.c and the host test
 * (host/tests/log_async_test.c) compile the SAME code.
 *
 * Why: from present LOG_BUF_FROM lines are collected in RAM and written out every log_flush.txt presents. The write was a
 * sceIoOpen/Write/Close of up to 256 KB ON THE MAIN THREAD inside __wrap_rhi_present: the [long] frames of 58-91 ms at
 * presents 1800k+1 (run 237). Now the main thread only SWAPS two buffers (two pointer writes under a spin flag, a few
 * hundred ns) and wakes a low-priority writer that does the file I/O.
 *
 *   active buffer  (act): the main thread, the watchdog thread and anyone else who prints append here, under `lock`.
 *   writer buffer  (wr) : owned by the writer from la_swap until it clears `busy`.
 *
 * Guarantees:
 *   ORDER     lines reach the sink in append order: the writer's block is always older than the active buffer, and
 *             la_flush_sync waits for the writer before it writes the active buffer.
 *   NO LOSS   in normal operation a swap that finds the writer busy is simply retried (la.want stays set) and the active buffer
 *             keeps filling; only when the active buffer is FULL and the writer is STILL busy are lines dropped. Drops are
 *             counted and reported by the next line that fits ("[log] dropped N lines (M bytes)").
 *   CRASH     every exit / abort / assert / watchdog / stop path calls la_flush_sync (the existing log_flush_all), which waits
 *             for the in-flight block (bounded: LA_WAIT_SPINS pauses) and then writes the rest on the CALLER's thread. A hard
 *             fault with no hook loses what the old design lost (up to one interval) plus nothing more: an in-flight block is on
 *             the card within one write time.
 *   NO BLOCK  the append path never waits for I/O. */
#ifndef LOG_ASYNC_H
#define LOG_ASYNC_H
#include <string.h>
#include <stdio.h>

#ifndef LA_WAIT_SPINS
#define LA_WAIT_SPINS 3000   /* x pause() (1 ms on the Vita) = 3 s at most, in la_flush_sync */
#endif

typedef struct LogAsync {
    char *act, *wr;                    /* two buffers of `cap` bytes each (caller-provided) */
    unsigned cap, n, wlen;
    volatile int lock, busy, want, enabled;
    unsigned long long dropLines, dropBytes, dropReported, swaps, syncFlushes, waitTimeouts, appended;
    void (*sink)(void* ud, const char* s, int n);   /* the file write */
    void (*wake)(void* ud);                         /* signal the writer thread */
    void (*pause)(void* ud);                        /* sleep ~1 ms (spin-wait helper) */
    void* ud;
} LogAsync;

static inline void la_init(LogAsync* la, char* a, char* b, unsigned cap, void (*sink)(void*, const char*, int), void (*wake)(void*), void (*pause)(void*), void* ud) {
    memset(la, 0, sizeof *la); la->act = a; la->wr = b; la->cap = cap; la->sink = sink; la->wake = wake; la->pause = pause; la->ud = ud;
}
static inline void la_lock(LogAsync* la)   { while (__atomic_test_and_set(&la->lock, __ATOMIC_ACQUIRE)) la->pause(la->ud); }
static inline void la_unlock(LogAsync* la) { __atomic_clear(&la->lock, __ATOMIC_RELEASE); }

/* hand the active buffer to the writer. 1 = handed off (or nothing to hand), 0 = writer still busy (la.want stays set by the caller) */
static inline int la_swap(LogAsync* la) {
    int ok = 1;
    la_lock(la);
    if (la->n) {
        if (__atomic_load_n(&la->busy, __ATOMIC_ACQUIRE)) ok = 0;
        else {
            char* t = la->act; la->act = la->wr; la->wr = t; la->wlen = la->n; la->n = 0; ++la->swaps;
            __atomic_store_n(&la->busy, 1, __ATOMIC_RELEASE);
        }
    }
    la_unlock(la);
    if (ok && la->n == 0 && __atomic_load_n(&la->busy, __ATOMIC_ACQUIRE) && la->wlen) la->wake(la->ud);   /* idempotent: the writer re-checks busy */
    return ok;
}
/* the writer thread's body for ONE wake-up */
static inline void la_writer_once(LogAsync* la) {
    if (!__atomic_load_n(&la->busy, __ATOMIC_ACQUIRE)) return;
    if (la->wlen) la->sink(la->ud, la->wr, (int)la->wlen);
    la->wlen = 0;
    __atomic_store_n(&la->busy, 0, __ATOMIC_RELEASE);
}
/* append one line (any thread). Never waits for I/O. */
static inline void la_append(LogAsync* la, const char* s, int n) {
    if (n <= 0) return;
    la_lock(la);
    if (la->dropLines > la->dropReported && la->n + (unsigned)n + 96 <= la->cap) {   /* report what was lost, as soon as there is room */
        int m = snprintf(la->act + la->n, 96, "[log] dropped %llu lines (%llu bytes) while the writer was busy and the buffer full\n",
                         la->dropLines - la->dropReported, la->dropBytes);
        if (m > 0 && m < 96) la->n += (unsigned)m;
        la->dropReported = la->dropLines; la->dropBytes = 0;
    }
    if (la->n + (unsigned)n > la->cap) {   /* full: hand it over if the writer is free, otherwise drop */
        if (!__atomic_load_n(&la->busy, __ATOMIC_ACQUIRE) && la->n) {
            char* t = la->act; la->act = la->wr; la->wr = t; la->wlen = la->n; la->n = 0; ++la->swaps;
            __atomic_store_n(&la->busy, 1, __ATOMIC_RELEASE);
            la->wake(la->ud);
        }
        if (la->n + (unsigned)n > la->cap) {
#ifndef LA_BREAK_DROPCOUNT   /* negative control: a dropped line is not counted */
            ++la->dropLines; la->dropBytes += (unsigned)n;
#endif
            la_unlock(la); return; }
    }
    memcpy(la->act + la->n, s, (size_t)n); la->n += (unsigned)n; ++la->appended;
    la_unlock(la);
}
/* everything buffered reaches the sink, in order, on THE CALLER's thread (exit, abort, assert, watchdog, vt_log_flush) */
static inline void la_flush_sync(LogAsync* la) {
    int spins = 0;
    la_lock(la);
#ifndef LA_BREAK_ORDER   /* negative control: do not wait for the in-flight block, so the newer lines overtake the older ones */
    while (__atomic_load_n(&la->busy, __ATOMIC_ACQUIRE) && spins < LA_WAIT_SPINS) { la->pause(la->ud); ++spins; }
#endif
    if (__atomic_load_n(&la->busy, __ATOMIC_ACQUIRE)) ++la->waitTimeouts;   /* the writer is stuck: write anyway (order may break; nothing is lost) */
#ifndef LA_BREAK_NOFLUSH   /* negative control: the exit flush forgets the active buffer */
    if (la->n) { la->sink(la->ud, la->act, (int)la->n); la->n = 0; ++la->syncFlushes; }
#endif
    la_unlock(la);
}
#endif
