// mm_shim.c - host implementation of the game's memory manager (main/mm.h).
//
// The real src/main/mm.c can't be compiled on host: it aliases three named .bss
// globals as one MmGlobalLayout via (MmGlobalLayout*)gMmStoreArray, reading regions
// at +0x3F00 past a 0x80-byte array - a GameCube link-order overlay (same hazard as
// pad.c) that isn't reconstructable byte-neutrally. So we reimplement mm's OBSERVABLE
// contract cleanly: mmAlloc onto malloc, 32-byte aligned (the game assumes this for
// DVD reads), with a header so mm_free/getHeapItemSize work. The type/flag/region
// knobs are inert (one flat allocator), but the DEFERRED-FREE timing is faithful:
// mmFreeDeferred holds a pointer for gMmFreeDelay ticks before actually freeing, so
// game code that keeps using a "freed" pointer for a few frames stays valid (matching
// the console) instead of hitting a use-after-free.

#include "main/mm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef struct MmHdr {
    void* base;   // original malloc pointer
    int   size;   // requested size
    int   magic;
} MmHdr;

#define MM_MAGIC 0x6D6D4831 /* "mmH1" */
#define MM_ALIGN 32

// A freed block takes its cached GPU textures with it (gx_draw.c gx_tex_release; weak -- the
// viewers link without gx_draw). 2026-09-27, run 73's texture leak.
extern void gx_tex_release(const void* addr, u32 n) __attribute__((weak));
// ...and it is an EVENT for the display-list cache (gx_draw.c, run 165): records reading this block are re-validated.
extern void gx_dlk_event(const void* p, size_t n, int isWrite) __attribute__((weak));

// ================================================================================================================
// THE HEAP GUARD (2026-10-11, run 226: three 0xFE bytes 64 apart inside SceShaccCg's malloc records and one in a newlib chunk
// size word -- game blocks and the shader compiler's share ONE newlib heap, and nothing said who wrote them).
//
//   ux0:data/stairfax/mm_guard.txt   "<mode> [sweepEveryTicks] [quarantineTicks] [quarantineKB]"       DEFAULT: absent = OFF
//     mode is a bit set:  1 REDZONES    MMG_RZ bytes of pattern before and after every game block, checked when the block is
//                                       really freed, on a sweep of every live block each <sweepEveryTicks> mmFreeTick calls
//                                       (default 1 = every game tick), and memcpyToCache/copyToCache are range-checked against
//                                       the block they touch AT THE CALL (that one names the writer, not just the victim).
//                         2 QUARANTINE  a block the game has really freed (i.e. AFTER its free delay has run out -- touching it
//                                       inside the delay is the retail contract, not a defect) is filled with 0xDD and kept
//                                       for <quarantineTicks> ticks (default 600) or until the quarantine holds
//                                       <quarantineKB> (default 16384); the fill is verified when it leaves, and 256 KB of it
//                                       is re-verified every tick. A stale READ gets 0xDDDDDDDD: a fault address to look for.
//                         4 POISON      fresh blocks are filled with 0xCD (retail mmAlloc does not clear either, so a field the
//                                       game never initialises reads as whatever the last owner left: 0xCDCDCDCD names that).
//     3 = redzones + quarantine is the run to start with.
//
// Every report carries the block, its size, the return address of the game-facing mmAlloc call (and of the mm_free call), the
// tick, and the damaged bytes as +offset=value pairs from the start of the block -- a stride shows in the offsets. Each one is
// flushed to the card before anything else happens (vt_log_flush). Symbolize an address with dump_threads.py's rule: the
// banner prints mmAlloc's own runtime address, so (address & ~1) - (that - nm's mmAlloc) is the nm address.
//
// COST (stated, not measured on hardware -- the [mmguard] status line prints the measured sweep time every 1800 ticks):
//   memory  mode 1: sizeof(MmGuard) + 2*MMG_RZ = 64 + 128 = 192 bytes per live block; mode 2 adds up to quarantineKB.
//   time    mode 1: one pass over the live list per sweep, ~3 cache lines a block; mode 2: one fill per free, one verify per
//           release, 256 KB compared per tick. Off: one predictable branch per call.
// ALIGNMENT: the user pointer is 32-byte aligned in every mode, and the capacity is the size rounded up to 32, as mm.c:393
// does (see mm_alloc_from) -- the rear redzone starts at the ROUNDED size, so what retail allows does not fire.
#define MMG_RZ        64          /* a redzone must be at least the stride of the write it is meant to stop: run 226's was 64 */
#define MMG_FRONT_PAT 0xF5
#define MMG_REAR_PAT  0xB7
#define MMG_FREED_PAT 0xDD
#define MMG_FRESH_PAT 0xCD
#define MM_MAGIC_G    0x6D6D4731u /* "mmG1": a live guarded block */
#define MM_MAGIC_Q    0x6D6D5131u /* "mmQ1": a guarded block in quarantine */
#define MMG_CHECK     0x47554152u
#define MMG_LIVE 1
#define MMG_QUAR 2
#define MMG_NDAM 12
#define MMG_NREP 8

typedef struct MmGuard {            // sits BEFORE the front redzone: an underrun has MMG_RZ bytes to cross before it gets here
    struct MmGuard *prev, *next;    // the live list, or the quarantine (oldest first)
    unsigned char *user, *base;
    unsigned size, cap, total;
    void *alloc_by, *freed_by;
    unsigned tick_alloc, tick_free;
    unsigned short type; unsigned char flag, state;
    unsigned check;
} __attribute__((aligned(32))) MmGuard;

typedef struct MmgRep {
    int kind;                       // 1 UNDERRUN 2 OVERRUN 3 USE AFTER FREE 4 DOUBLE FREE 5 BAD FREE 6 WRITE PAST END 7 READ PAST END
    int quarantined;
    void *user, *alloc_by, *freed_by, *now_by;
    unsigned size, cap, tick_alloc, tick_free;
    int type, flag;
    int ndam, first, last;          // damaged bytes, as offsets from the user pointer
    int off[MMG_NDAM]; unsigned char val[MMG_NDAM];
    long over, at; const char* fn;  // the range checks: bytes past the end, offset of the access, who made it
} MmgRep;

int mm_guard_mode = -1;             // -1 = the marker has not been read yet
static unsigned sGSweepEvery = 1, sGQTicks = 600, sGQBudget = 16384u * 1024u;
static MmGuard *sGLive, *sGQHead, *sGQTail, *sGQScan;
static unsigned sGTick, sGLiveCount, sGLivePeak, sGQCount, sGQBytes, sGQReleased, sGAllocs, sGFrees, sGSweeps, sGSweepUs,
                sGSweepUsMax, sGRangeChecks, sGRangeWalks, sGLockSpins, sGReportsDropped;
unsigned mm_guard_reports, mm_guard_overruns, mm_guard_underruns, mm_guard_uafs, mm_guard_double_frees, mm_guard_bad_frees,
         mm_guard_range_writes, mm_guard_range_reads;
#define MMG_RCACHE 64
static MmGuard* sGRangeCache[MMG_RCACHE];
static volatile char sGLock;
// The game's threads take turns (the baton), so this is uncontended; it is here for the day one does not. Nothing prints or
// calls malloc's own lock-holders while holding it: reports are collected under it and written after it is released.
static void mmg_lock(void)   { while (__atomic_test_and_set(&sGLock, __ATOMIC_ACQUIRE)) ++sGLockSpins; }
static void mmg_unlock(void) { __atomic_clear(&sGLock, __ATOMIC_RELEASE); }

extern void vt_log_flush(void) __attribute__((weak));

void mm_guard_config(int mode, int sweepEvery, int quarantineTicks, int quarantineKB) {
    mm_guard_mode = mode < 0 ? 0 : mode;
    if (sweepEvery > 0) sGSweepEvery = (unsigned)sweepEvery;
    if (quarantineTicks > 0) sGQTicks = (unsigned)quarantineTicks;
    if (quarantineKB > 0) sGQBudget = (unsigned)quarantineKB * 1024u;
    if (mm_guard_mode) {
        printf("[mmguard] ON mode %d (%s%s%s) | redzones %d + %d bytes, header %u: %u bytes a block | sweep every %u tick(s) | "
               "quarantine %u ticks or %u KB | callers are return addresses: mmAlloc itself is at %p\n", mm_guard_mode,
               (mm_guard_mode & 1) ? "REDZONES " : "", (mm_guard_mode & 2) ? "QUARANTINE " : "", (mm_guard_mode & 4) ? "POISON" : "",
               MMG_RZ, MMG_RZ, (unsigned)sizeof(MmGuard), (unsigned)(sizeof(MmGuard) + 2 * MMG_RZ), sGSweepEvery, sGQTicks,
               sGQBudget >> 10, (void*)mmAlloc);
        if (vt_log_flush) vt_log_flush();
    }
}
static int mmg_on(void) {
    if (mm_guard_mode < 0) {
        int m = 0, a = 0, b = 0, c = 0;
#ifdef __vita__
        FILE* f = fopen("ux0:data/stairfax/mm_guard.txt", "r");
        if (f) { if (fscanf(f, "%d %d %d %d", &m, &a, &b, &c) < 1) m = 0; fclose(f); }
#endif
        mm_guard_config(m, a, b, c);
    }
    return mm_guard_mode;
}

static MmGuard* mmg_of(const void* ptr) {      // the guard header of a user pointer, or NULL if it is not (or no longer) one
    MmGuard* g;
    if (!ptr || ((uintptr_t)ptr & (MM_ALIGN - 1))) return NULL;
    g = (MmGuard*)((unsigned char*)ptr - MMG_RZ - sizeof(MmGuard));
    return (g->check == (MMG_CHECK ^ (unsigned)(uintptr_t)ptr) && g->user == (const unsigned char*)ptr) ? g : NULL;
}
static void mmg_rep_init(MmgRep* r, int kind, const MmGuard* g) {
    memset(r, 0, sizeof *r);
    r->kind = kind; r->first = 0x7fffffff; r->last = -0x7fffffff;
    if (g) { r->user = g->user; r->alloc_by = g->alloc_by; r->freed_by = g->freed_by; r->size = g->size; r->cap = g->cap;
             r->tick_alloc = g->tick_alloc; r->tick_free = g->tick_free; r->type = g->type; r->flag = g->flag;
             r->quarantined = g->state == MMG_QUAR; }
}
static void mmg_rep_byte(MmgRep* r, long off, unsigned char v) {
    if (r->ndam < MMG_NDAM) { r->off[r->ndam] = (int)off; r->val[r->ndam] = v; }
    ++r->ndam;
    if (off < r->first) r->first = (int)off;
    if (off > r->last) r->last = (int)off;
}
// n bytes at p must all be `pat`; every one that is not goes into the report as an offset from the user pointer, and is put back
static void mmg_scan(MmGuard* g, unsigned char* p, size_t n, unsigned char pat, MmgRep* r) {
    size_t i = 0;
    const size_t w = (size_t)-1 / 0xFF * pat;
    for (; i + sizeof w <= n; i += sizeof w) {
        size_t v; memcpy(&v, p + i, sizeof v);
        if (v != w) { size_t k; for (k = i; k < i + sizeof w; ++k) if (p[k] != pat) { mmg_rep_byte(r, (long)(p + k - g->user), p[k]); p[k] = pat; } }
    }
    for (; i < n; ++i) if (p[i] != pat) { mmg_rep_byte(r, (long)(p + i - g->user), p[i]); p[i] = pat; }
}
// Both zones of one block, and the MmHdr in front of the user pointer. Returns how many reports it wrote (0, 1 or 2).
static int mmg_check_zones(MmGuard* g, MmgRep* out) {
    MmHdr want; unsigned char* h = g->user - sizeof(MmHdr); size_t k; int n = 0;
    MmgRep* r = &out[0];
    memset(&want, 0, sizeof want);
    want.base = g->base; want.size = (int)g->size; want.magic = (int)(g->state == MMG_QUAR ? MM_MAGIC_Q : MM_MAGIC_G);
    mmg_rep_init(r, 1, g);
    mmg_scan(g, g->user - MMG_RZ, MMG_RZ - sizeof(MmHdr), MMG_FRONT_PAT, r);
    for (k = 0; k < sizeof(MmHdr); ++k) if (h[k] != ((unsigned char*)&want)[k]) mmg_rep_byte(r, (long)k - (long)sizeof(MmHdr), h[k]);
    if (r->ndam) { memcpy(h, &want, sizeof want); ++n; r = &out[n]; }
    mmg_rep_init(r, 2, g);
    mmg_scan(g, g->user + g->cap, MMG_RZ, MMG_REAR_PAT, r);
    if (r->ndam) ++n;
    return n;
}
static void mmg_emit(const MmgRep* r, const char* where) {
    static const char* const kind[] = { "?", "UNDERRUN", "OVERRUN", "USE AFTER FREE", "DOUBLE FREE", "BAD FREE", "WRITE PAST END", "READ PAST END" };
    char dam[MMG_NDAM * 16 + 8]; int i, n = 0;
    ++mm_guard_reports;
    if (r->kind == 1) ++mm_guard_underruns; else if (r->kind == 2) ++mm_guard_overruns; else if (r->kind == 3) ++mm_guard_uafs;
    else if (r->kind == 4) ++mm_guard_double_frees; else if (r->kind == 5) ++mm_guard_bad_frees;
    else if (r->kind == 6) ++mm_guard_range_writes; else if (r->kind == 7) ++mm_guard_range_reads;
    if (mm_guard_reports > 200) { ++sGReportsDropped; return; }      // counted, not printed: the status line says how many
    dam[0] = 0;
    for (i = 0; i < r->ndam && i < MMG_NDAM; ++i) n += snprintf(dam + n, sizeof dam - (size_t)n, " %+d=%02x", r->off[i], r->val[i]);
    if (r->kind == 5)
        printf("[mmguard] BAD FREE of %p by %p -- not a live game block (a second free of one already gone, or a header overwritten "
               "from below) | tick %u\n", r->user, r->now_by, sGTick);
    else if (r->kind == 4)
        printf("[mmguard] DOUBLE FREE block %p size %u allocated by %p freed by %p (tick %u), freed AGAIN by %p | tick %u\n",
               r->user, r->size, r->alloc_by, r->freed_by, r->tick_free, r->now_by, sGTick);
    else if (r->kind >= 6)
        printf("[mmguard] %s block %p size %u (capacity %u) allocated by %p (type 0x%x flag %d, tick %u) -- %s called from %p %s "
               "%ld bytes at +%ld: %ld bytes past the end | tick %u\n", kind[r->kind], r->user, r->size, r->cap, r->alloc_by,
               r->type, r->flag, r->tick_alloc, r->fn, r->now_by, r->kind == 6 ? "writes" : "reads", r->over + (long)r->cap - r->at,
               r->at, r->over, sGTick);
    else if (r->kind == 3)
        printf("[mmguard] USE AFTER FREE block %p size %u allocated by %p (type 0x%x flag %d, tick %u) freed by %p (really freed "
               "tick %u) -- %d bytes changed at %+d..%+d:%s%s | found at %s, tick %u\n", r->user, r->size, r->alloc_by, r->type,
               r->flag, r->tick_alloc, r->freed_by, r->tick_free, r->ndam, r->first, r->last, dam, r->ndam > MMG_NDAM ? " ..." : "",
               where, sGTick);
    else
        printf("[mmguard] %s block %p size %u (capacity %u) allocated by %p (type 0x%x flag %d, tick %u)%s -- %d bytes damaged at "
               "%+d..%+d:%s%s | found at %s, tick %u\n", kind[r->kind], r->user, r->size, r->cap, r->alloc_by, r->type, r->flag,
               r->tick_alloc, r->quarantined ? " [already freed, in quarantine]" : "", r->ndam, r->first, r->last, dam,
               r->ndam > MMG_NDAM ? " ..." : "", where, sGTick);
    fflush(stdout);
    if (vt_log_flush) vt_log_flush();
}
static void mmg_emit_all(const MmgRep* r, int n, const char* where) { int i; for (i = 0; i < n; ++i) mmg_emit(&r[i], where); }

static void* mmg_alloc(int size, size_t cap, int type, int flag, void* caller) {
    const size_t total = sizeof(MmGuard) + MMG_RZ + cap + MMG_RZ + MM_ALIGN;
    unsigned char* base = (unsigned char*)malloc(total);
    unsigned char* user; MmGuard* g; MmHdr* h;
    if (!base) return NULL;
    user = (unsigned char*)(((uintptr_t)base + sizeof(MmGuard) + MMG_RZ + (MM_ALIGN - 1)) & ~(uintptr_t)(MM_ALIGN - 1));
    g = (MmGuard*)(user - MMG_RZ - sizeof(MmGuard));
    memset(user - MMG_RZ, MMG_FRONT_PAT, MMG_RZ - sizeof(MmHdr));
    memset(user + cap, MMG_REAR_PAT, MMG_RZ);
    if (mm_guard_mode & 4) memset(user, MMG_FRESH_PAT, cap);
    h = (MmHdr*)(user - sizeof(MmHdr));
    memset(h, 0, sizeof *h);
    h->base = base; h->size = size; h->magic = (int)MM_MAGIC_G;
    g->user = user; g->base = base; g->size = (unsigned)size; g->cap = (unsigned)cap; g->total = (unsigned)total;
    g->alloc_by = caller; g->freed_by = NULL; g->tick_alloc = sGTick; g->tick_free = 0;
    g->type = (unsigned short)type; g->flag = (unsigned char)flag; g->state = MMG_LIVE;
    g->check = MMG_CHECK ^ (unsigned)(uintptr_t)user;
    mmg_lock();
    g->prev = NULL; g->next = sGLive; if (sGLive) sGLive->prev = g; sGLive = g;
    ++sGAllocs; if (++sGLiveCount > sGLivePeak) sGLivePeak = sGLiveCount;
    mmg_unlock();
    return user;
}
// the game asked for this block to be freed (now or after the delay): remember who, for the report
static void mmg_note_free(void* ptr, void* freer) {
    MmGuard* g;
    if (mm_guard_mode <= 0 || !ptr) return;
    g = mmg_of(ptr);
    if (g && g->state == MMG_LIVE && !g->freed_by) g->freed_by = freer;
}
static void mmg_unlink(MmGuard** head, MmGuard** tail, MmGuard* g) {
    if (g->prev) g->prev->next = g->next; else *head = g->next;
    if (g->next) g->next->prev = g->prev; else if (tail) *tail = g->prev;
    g->prev = g->next = NULL;
}
// a quarantined block: the fill and both zones. Returns the reports written.
static int mmg_check_quarantined(MmGuard* g, MmgRep* out) {
    int n = 0; MmgRep* r = &out[0];
    mmg_rep_init(r, 3, g);
    mmg_scan(g, g->user, g->cap, MMG_FREED_PAT, r);
    if (r->ndam) ++n;
    return n + mmg_check_zones(g, &out[n]);
}
// oldest blocks leave the quarantine: verified, then really freed. Call with the lock held; returns the reports written.
static int mmg_quarantine_trim(MmgRep* out, int room, int all) {
    int n = 0;
    while (sGQHead && n + 3 <= room && (all || sGQBytes > sGQBudget || sGTick - sGQHead->tick_free >= sGQTicks)) {
        MmGuard* g = sGQHead;
        n += mmg_check_quarantined(g, &out[n]);
        if (sGQScan == g) sGQScan = g->next;
        mmg_unlink(&sGQHead, &sGQTail, g);
        --sGQCount; sGQBytes -= g->total; ++sGQReleased;
        g->check = 0;
        free(g->base);
    }
    return n;
}
// Returns 1 when the pointer was the guard's to deal with (freed, quarantined or reported), 0 for a plain block.
static int mmg_free(void* ptr, void* freer) {
    MmgRep rep[4 + 3]; int n = 0, i; MmGuard* g = mmg_of(ptr);
    if (!g) {
        MmHdr* h = (MmHdr*)((unsigned char*)ptr - sizeof(MmHdr));
        if (h->magic == MM_MAGIC) return 0;                     // allocated before the guard came on
        mmg_rep_init(&rep[0], 5, NULL); rep[0].user = ptr; rep[0].now_by = freer;
        mmg_emit(&rep[0], "free");
        return 1;
    }
    if (g->state != MMG_LIVE) {
        mmg_rep_init(&rep[0], 4, g); rep[0].now_by = freer;
        mmg_emit(&rep[0], "free");
        return 1;
    }
    if (!g->freed_by) g->freed_by = freer;
    if (gx_tex_release) gx_tex_release(ptr, g->size);
    if (gx_dlk_event) gx_dlk_event(ptr, (size_t)g->size, 0);
    mmg_lock();
    n = mmg_check_zones(g, rep);
    for (i = 0; i < MMG_RCACHE; ++i) if (sGRangeCache[i] == g) sGRangeCache[i] = NULL;
    mmg_unlink(&sGLive, NULL, g);
    --sGLiveCount; ++sGFrees;
    g->tick_free = sGTick;
    if (mm_guard_mode & 2) {
        g->state = MMG_QUAR;
        ((MmHdr*)(g->user - sizeof(MmHdr)))->magic = (int)MM_MAGIC_Q;
        memset(g->user, MMG_FREED_PAT, g->cap);
        g->next = NULL; g->prev = sGQTail; if (sGQTail) sGQTail->next = g; else sGQHead = g; sGQTail = g;
        ++sGQCount; sGQBytes += g->total;
        n += mmg_quarantine_trim(&rep[n], (int)(sizeof rep / sizeof rep[0]) - n, 0);
        mmg_unlock();
    } else {
        g->check = 0; g->state = 0;
        ((MmHdr*)(g->user - sizeof(MmHdr)))->magic = 0;
        mmg_unlock();
        free(g->base);
    }
    mmg_emit_all(rep, n, "free");
    return 1;
}
// every live block's zones. `where` goes into the reports.
void mm_guard_sweep(const char* where) {
    MmgRep rep[MMG_NREP + 2]; int n = 0; MmGuard* g; clock_t t0;
    if (mm_guard_mode <= 0 || !(mm_guard_mode & 1)) return;
    t0 = clock();
    mmg_lock();
    for (g = sGLive; g && n < MMG_NREP; g = g->next) n += mmg_check_zones(g, &rep[n]);   // a zone not reached stays damaged for the next sweep
    mmg_unlock();
    sGSweepUs = (unsigned)((unsigned long long)(clock() - t0) * 1000000ull / CLOCKS_PER_SEC);
    if (sGSweepUs > sGSweepUsMax) sGSweepUsMax = sGSweepUs;
    ++sGSweeps;
    mmg_emit_all(rep, n, where);
}
// the quarantine's per-tick work: age the oldest out, and re-verify 256 KB of what stays
static void mmg_quarantine_tick(void) {
    MmgRep rep[MMG_NREP + 3]; int n; unsigned done = 0;
    if (!(mm_guard_mode & 2)) return;
    mmg_lock();
    n = mmg_quarantine_trim(rep, MMG_NREP, 0);
    if (!sGQScan) sGQScan = sGQHead;
    while (sGQScan && done < 256u * 1024u && n + 3 <= MMG_NREP + 3) {
        n += mmg_check_quarantined(sGQScan, &rep[n]);
        done += sGQScan->cap;
        sGQScan = sGQScan->next;
    }
    mmg_unlock();
    mmg_emit_all(rep, n, "the quarantine");
}
void mm_guard_status(void) {
    if (mm_guard_mode <= 0) return;
    printf("[mmguard] status tick %u | live %u blocks (peak %u), %u KB of guard | allocs %u frees %u | sweeps %u: last %u us, "
           "worst %u us | quarantine %u blocks %u KB, %u released | range checks %u (%u list walks) | REPORTS %u (overrun %u "
           "underrun %u use-after-free %u double free %u bad free %u write past end %u read past end %u; %u not printed) | "
           "lock spins %u\n", sGTick, sGLiveCount, sGLivePeak, (unsigned)(sGLiveCount * (sizeof(MmGuard) + 2 * MMG_RZ) >> 10),
           sGAllocs, sGFrees, sGSweeps, sGSweepUs, sGSweepUsMax, sGQCount, sGQBytes >> 10, sGQReleased, sGRangeChecks, sGRangeWalks,
           mm_guard_reports, mm_guard_overruns, mm_guard_underruns, mm_guard_uafs, mm_guard_double_frees, mm_guard_bad_frees,
           mm_guard_range_writes, mm_guard_range_reads, sGReportsDropped, sGLockSpins);
}
// everything the guard can check, now: for the end of a run, and for the host test
void mm_guard_check_all(void) {
    MmgRep rep[MMG_NREP + 3]; int n;
    if (mm_guard_mode <= 0) return;
    mm_guard_sweep("a full check");
    do { mmg_lock(); n = mmg_quarantine_trim(rep, MMG_NREP, 1); mmg_unlock(); mmg_emit_all(rep, n, "the quarantine"); } while (sGQHead);
}
// memcpyToCache / copyToCache: does [p, p+len) stay inside the game block it starts in? This one fires AT the access.
static void mmg_range(const void* p, size_t len, void* caller, int isWrite, const char* fn) {
    MmGuard* g; MmgRep r; const unsigned char* a = (const unsigned char*)p; const unsigned slot = ((uintptr_t)p >> 5) & (MMG_RCACHE - 1);
    if (mm_guard_mode <= 0 || !(mm_guard_mode & 1) || !p || !len) return;
    mmg_lock();
    ++sGRangeChecks;
    g = sGRangeCache[slot];
    if (!(g && g->state == MMG_LIVE && a >= g->user && a < g->user + g->cap)) {
        ++sGRangeWalks;
        for (g = sGLive; g; g = g->next) if (a >= g->user && a < g->user + g->cap) break;
        sGRangeCache[slot] = g;
    }
    if (g && a + len > g->user + g->cap) {
        mmg_rep_init(&r, isWrite ? 6 : 7, g);
        r.now_by = caller; r.fn = fn; r.at = (long)(a - g->user); r.over = (long)(a + len - (g->user + g->cap));
        mmg_unlock();
        if (isWrite || mm_guard_range_reads < 8) mmg_emit(&r, fn); else ++mm_guard_range_reads;
        return;
    }
    mmg_unlock();
}

// The game-facing allocator. `caller` is the return address of the mmAlloc call (the guard's "allocated by").
// THE CAPACITY IS ROUNDED UP TO 32 BYTES, AS RETAIL'S IS (mm.c:393, mmAllocFromRegion: `if (size & 0x1f) size = (size & ~0x1f)
// + 0x20`). Until 2026-10-11 this shim handed out exactly `size` (plus 4..28 bytes of alignment slack by accident), so game code
// that works in whole cache lines -- DVD reads rounded up to 32, copyToCache/memcpyToCache line counts -- wrote up to 27 bytes
// past the malloc request, into the next chunk's header. getHeapItemSize still answers with the size that was asked for.
static void* mm_alloc_from(int size, int type, int flag, void* caller) {
    size_t cap, total; unsigned char* base; uintptr_t user; MmHdr* h;
    if (size < 0) return NULL;
    cap = ((size_t)size + (MM_ALIGN - 1)) & ~(size_t)(MM_ALIGN - 1);
    if (mmg_on()) return mmg_alloc(size, cap, type, flag, caller);
    total = cap + sizeof(MmHdr) + MM_ALIGN;
    base = (unsigned char*)malloc(total);
    if (!base) {   /* N2: threaded frees are deferred to the frame edge (retire list); DIRECT would have succeeded after them.  Publish, wait for R, free what it has passed, retry once. */
        extern int gxt_reclaim(const char*) __attribute__((weak));
        if (gxt_reclaim && gxt_reclaim("mmAlloc") > 0) base = (unsigned char*)malloc(total);
        if (!base) return NULL;
    }
    user = ((uintptr_t)base + sizeof(MmHdr) + (MM_ALIGN - 1)) & ~(uintptr_t)(MM_ALIGN - 1);
    h = (MmHdr*)(user - sizeof(MmHdr));
    h->base = base;
    h->size = size;
    h->magic = MM_MAGIC;
    return (void*)user;
}
void* mmAlloc(int size, int type, int flag) { return mm_alloc_from(size, type, flag, __builtin_return_address(0)); }

static void mm_free_now(void* ptr, void* freer) {
    if (!ptr) return;
    if (mm_guard_mode > 0 && mmg_free(ptr, freer)) return;
    MmHdr* h = (MmHdr*)((unsigned char*)ptr - sizeof(MmHdr));
    if (h->magic != MM_MAGIC) return; // not ours / double free guard
    h->magic = 0;
    if (gx_tex_release) gx_tex_release(ptr, (u32)h->size);
    if (gx_dlk_event) gx_dlk_event(ptr, (size_t)h->size, 0);
    { extern void gxt_free(void*) __attribute__((weak)); if (gxt_free) gxt_free(h->base); else free(h->base); }   /* N2: retire list when threaded */
}

static void mm_deferred_forget(void* p);
// mm.c: mmFree is the immediate one. A block that is ALSO waiting in the deferred list must leave it: its address may be
// handed out again before the tick reaches it, and the tick would then free the new owner's block.
void  mmFree(void* p)          { void* by = __builtin_return_address(0); mm_deferred_forget(p); mm_free_now(p, by); }

// --- deferred free: hold pointers gMmFreeDelay ticks before actually freeing ----
// The console defers so callers can keep touching a "freed" buffer for a few frames.
#define MM_DEFERRED_CAP 4096
static struct { void* ptr; int delay; void* by; } sDeferred[MM_DEFERRED_CAP];
static int sDeferredCount;
static int sFreeDelay = 2; // console default (mmInit sets gMmFreeDelay = 2)
unsigned mm_deferred_frees, mm_deferred_dups, mm_deferred_overflows;
static void mm_deferred_forget(void* p) { for (int i = 0; i < sDeferredCount; ++i) if (sDeferred[i].ptr == p) { sDeferred[i] = sDeferred[--sDeferredCount]; return; } }

static void mm_free_deferred_by(void* p, void* by) {
    if (!p) return;
    for (int i = 0; i < sDeferredCount; ++i) if (sDeferred[i].ptr == p) { ++mm_deferred_dups; return; }   // already waiting: one free
    if (sDeferredCount >= MM_DEFERRED_CAP) {   // mm.c:621 drains the whole stack when it is full
        ++mm_deferred_overflows;
        while (sDeferredCount > 0) { --sDeferredCount; mm_free_now(sDeferred[sDeferredCount].ptr, sDeferred[sDeferredCount].by); }
    }
    mmg_note_free(p, by);
    sDeferred[sDeferredCount].ptr = p;
    sDeferred[sDeferredCount].delay = sFreeDelay > 0 ? sFreeDelay : 1;
    sDeferred[sDeferredCount].by = by;
    sDeferredCount++;
    ++mm_deferred_frees;
}
void mmFreeDeferred(void* p) { mm_free_deferred_by(p, __builtin_return_address(0)); }

// mm_free IS DEFERRED BY DEFAULT (vita fork, 2026-10-10, docs/47 "the platform-layer audit"). mm.c:496:
//     void mm_free(void* p) { if (gMmFreeDelay == 0) mmFree(p); else mmFreeDeferred(p); }      with gMmFreeDelay = 2 (mm.c:266)
// Until run 181 this shim freed at once, for all 255 call sites: the game goes on reading a block for up to two ticks after
// mm_free (it brackets the frees that must be immediate with mmSetFreeDelay(0)), so every such read was a use-after-free of
// memory malloc may already have handed out again. ux0:data/stairfax/mmfree_now.txt starting with 1 = the old immediate free.
void mm_free(void* ptr) {
    static int sNow = -1;
    if (sNow < 0) { sNow = 0;
#ifdef __vita__
        { FILE* f = fopen("ux0:data/stairfax/mmfree_now.txt", "r"); sNow = f && fgetc(f) == 49; if (f) fclose(f); }
        printf("[mm] mm_free: %s\n", sNow ? "IMMEDIATE (mmfree_now.txt: the pre-run-181 defect)" : "deferred by the game's free delay (retail)");
#endif
    }
    if (!ptr) return;
    if (sFreeDelay == 0 || sNow) mm_free_now(ptr, __builtin_return_address(0)); else mm_free_deferred_by(ptr, __builtin_return_address(0));
}

// mm.c:538 also runs the game's per-tick timer for TRANSIENT MAP BITS from here (its only caller): without it they never expire.
extern void SaveGame_updateTransientMapBits(void) __attribute__((weak));
void mmFreeTick(int arg) {
    (void)arg;
    int i = 0;
    if (mm_guard_mode > 0) {   // the guard's tick: every live block's zones BEFORE this tick's frees, so a report names the tick the damage was done in
        ++sGTick;
        if (sGTick % sGSweepEvery == 0) mm_guard_sweep("the tick sweep");
        if (sGTick % 1800 == 1) mm_guard_status();
    }
    while (i < sDeferredCount) {
        if (--sDeferred[i].delay <= 0) {
            mm_free_now(sDeferred[i].ptr, sDeferred[i].by);
            sDeferred[i] = sDeferred[--sDeferredCount]; // swap-remove (matches mm.c)
        } else {
            i++;
        }
    }
    if (mm_guard_mode > 0) mmg_quarantine_tick();
    { extern void mm_stores_rewind(void); mm_stores_rewind(); }   // mm.c:530: every store back to its start, each tick
    if (SaveGame_updateTransientMapBits) SaveGame_updateTransientMapBits();
}

// vita fork: the real mmInit (src/main/mm.c) has two effects OUTSIDE the heap, and the
// game depends on both: it allocates the save-game work buffer (0x6ED bytes) and points
// gAskProgressiveScanFlag at its LAST byte. Upstream's shim did neither, so init() read
// *gAskProgressiveScanFlag through NULL (device run 7: data abort at init+0x4c2).
extern u8* gSaveGameWorkBuffer;        // dlls/engine/23/23.c
extern u8* gAskProgressiveScanFlag;    // main/gameloop.c
void  mmInit(void) {
    sDeferredCount = 0; sFreeDelay = 2;
    gSaveGameWorkBuffer = (u8*)calloc(1, 0x6ed);
    gAskProgressiveScanFlag = gSaveGameWorkBuffer ? gSaveGameWorkBuffer + 0x6ec : NULL;
}

int getHeapItemSize(void* ptr) {
    if (!ptr) return 0;
    MmHdr* h = (MmHdr*)((unsigned char*)ptr - sizeof(MmHdr));
    return (h->magic == MM_MAGIC || (unsigned)h->magic == MM_MAGIC_G) ? h->size : 0;
}

// --- alignment helpers -----------------------------------------------------
int alignUp2(int v)    { return (v + 1) & ~1; }
int roundUpTo4(int v)  { return (v + 3) & ~3; }
int roundUpTo8(int v)  { return (v + 7) & ~7; }
int roundUpTo16(int v) { return (v + 15) & ~15; }
int roundUpTo32(int v) { return (v + 31) & ~31; }

// --- heap mode knobs (inert on host) ---------------------------------------
int mmSetFreeDelay(int v)               { int old = sFreeDelay; sFreeDelay = v; return old; }
int testAndSet_onlyUseHeaps1and2(int v) { (void)v; return 0; }
int testAndSet_onlyUseHeap3(int v)      { (void)v; return 0; }
// vita fork: the rest of mm.c's public surface. mm_shim replaces mm.c WHOLE; with these
// three missing, the link pulled the real mm.c in to supply them -- and with it the
// console heap machinery (region tables in a GameCube arena), which this shim never
// builds. First device run of the game crashed inside mm.c's heap-slot walker.
// The heap-selection flags are kept (callers read the old value back) but steer
// nothing: every allocation is malloc.
static int sForce12, sForce3;
int mmSetForceHeaps1and2Only(int v) { int old = sForce12; sForce12 = v; return old; }
int mmSetForceHeap3Only(int v)      { int old = sForce3;  sForce3  = v; return old; }
void* mmInitRegion(u8* buf, int size, int numSlots) { (void)size; (void)numSlots; return buf; }
int mmGetRegionForPtr(u8* ptr)          { (void)ptr; return 0; }
void mmSetTextureAllocationState(int s) { (void)s; }
int printHeapStats(int mode)            { (void)mode; return 0; }

// --- cache staging (host has no separate locked cache; use a real scratch buffer) ----
// The GC locked-cache staging area. The model render path stages matrices here:
// modelInitMtxs copies the joint bank to getCache()+0x2700, and renderOpMatrix/modelBuildPosNrmMtxs
// read/write pos matrices at +0, normal/tex at +0x12C0. Must be a persistent, aligned buffer big
// enough for +0x2700 + (jointCount+extra)*0x40 (~0x3000 for 36 joints); 0x8000 is ample.
void* getCache(void) {
    static _Alignas(32) unsigned char gCacheBuf[0x8000];
    return gCacheBuf;
}
void  cacheQueueWait(int sync)                    { (void)sync; }
// Faithful to mm.c's copyToCache non-locked-cache branch: the count argument is a cache-line
// count (0x20 bytes each), NOT a byte count - len = count ? count<<5 : 0x1000. modelInitMtxs
// relies on this: its full-chunk copies pass count=0 (=> 0x1000 bytes) and the final passes
// (jointCount+extra)*2 lines. The old memcpy(dst,src,n) copied n BYTES, so a 36-joint model
// staged only 72 of its 2304 joint-matrix bytes and every skinned draw read a garbage bank.
void  copyToCache(void* dst, void* src, u32 count) {
    if (dst && src) {
        if (mm_guard_mode > 0) mmg_range(src, count ? ((size_t)count << 5) : 0x1000, __builtin_return_address(0), 0, "copyToCache");
        memcpy(dst, src, count ? ((size_t)count << 5) : 0x1000);
    }
}
// memcpyToCache counts CACHE LINES too (mm.c:936: len = count ? count << 5 : 0x1000), exactly like copyToCache above.
// Until run 160 (2026-10-09, docs/47) this copied `count` BYTES: the morph kernel's write-back (model.c:1345) stored 1/32 of
// each chunk -- 21 of 672 vertices -- and left the rest of the model's vertex buffer stale (the CloudRunner's wings in
// ShipBattle, runs 157-159); the CPU-skinning write-backs (model.c:2460/2540) and the heavy-fog shadow texture
// (newshadows.c:1771, count 0 = NOTHING copied) took the same cut. ux0:data/stairfax/memcpy_old.txt starting with 1 = the
// old byte count (A/B).
void  memcpyToCache(void* dst, void* src, u32 count) {
    static int sOld = -1;
    if (sOld < 0) { sOld = 0;
#ifdef __vita__
        { FILE* f = fopen("ux0:data/stairfax/memcpy_old.txt", "r"); sOld = f && fgetc(f) == 49; if (f) fclose(f); }
        printf("[cache] memcpyToCache counts %s\n", sOld ? "BYTES (memcpy_old.txt: the pre-run-160 defect)" : "cache lines (retail)");
#endif
    }
    if (dst && src) {
        const size_t len = sOld ? (size_t)count : (count ? ((size_t)count << 5) : 0x1000);
        if (mm_guard_mode > 0) mmg_range(dst, len, __builtin_return_address(0), 1, "memcpyToCache");
        memcpy(dst, src, len);
        if (gx_dlk_event) gx_dlk_event(dst, len, 1);   /* the morph / skinning write-back: the vertex buffers the cache reads */
    }
}

// --- atomic singly-linked list (node's first word is the next pointer) ------
void AtomicSList_Push(void** list, void* node) {
    if (!list || !node) return;
    *(void**)node = *list;
    *list = node;
}
void* AtomicSList_Pop(void** list) {
    if (!list || !*list) return NULL;
    void* n = *list;
    *list = *(void**)n;
    return n;
}

// --- pools / stores --------------------------------------------------------
// vita fork: the REAL stackCreate's contract (src/main/mm.c), not a bare malloc. The game
// pops nodes with AtomicSList_Pop(pool), which reads the pool's FIRST word as the free-list
// head -- upstream's malloc left it uninitialized, so the first pop returned garbage (device
// run 6: a data abort in AtomicSList_Pop under piRomLoadSection). Same 0x20 header, same
// linking, including the original's quirk: nodes 0..count-2 are linked, the last is not.
typedef struct StfxStackPool {
    void* freeList; void* end; u32 unk8; s16 itemSize; s16 itemCount; u16 usedCount; u8 pad12[0xe];
} StfxStackPool;
_Static_assert(sizeof(void*) != 4 || sizeof(StfxStackPool) == 0x20, "StackPool header is 0x20 bytes, as in mm.c");   /* the 64-bit host test (mm_guard_test) has 8-byte pointers */
void* stackCreate(int count, int size) {
    StfxStackPool* stack = (StfxStackPool*)calloc(1, (size_t)size * (size_t)count + sizeof(StfxStackPool));
    void** cur; u8* next; int n;
    if (!stack) return NULL;
    stack->itemSize = (s16)size; stack->itemCount = (s16)count; stack->usedCount = 0;
    stack->end = (u8*)stack + (size_t)count * (size_t)size + sizeof(StfxStackPool);
    cur = (void**)(stack + 1);
    next = (u8*)cur + size;
    for (n = count - 2; n > 0; n--) { *cur = next; cur = (void**)*cur; next += size; }
    *cur = 0;
    stack->freeList = stack + 1;
    return stack;
}
// MEMORY STORES ARE REAL (vita fork, 2026-10-10, the platform-layer audit). mm.c:820-905: a store is a fixed arena, bumped by
// mmAllocateFromFBMemoryStore and REWOUND every tick by mmFreeTick (mm.c:530). The stub returned handle 0 and malloc'd every
// request with nothing ever freeing it: gametext.c:2978 leaked one block per laid-out text with a max line height.
#define MM_STORES 32
static struct { int handle, size; char *base, *cur; } sStore[MM_STORES];
static int sNextStoreHandle = 1;
unsigned mm_store_allocs, mm_store_full;
int   mmCreateMemoryStore(int size) {
    if (size <= 0) return 0;
    for (int i = 0; i < MM_STORES; ++i) if (!sStore[i].base) {
        char* b = (char*)mm_alloc_from(size, 0, 0, __builtin_return_address(0));
        if (!b) return 0;
        sStore[i].base = sStore[i].cur = b; sStore[i].size = size; sStore[i].handle = sNextStoreHandle++;
        return sStore[i].handle;
    }
    return 0;
}
void* mmAllocateFromFBMemoryStore(int handle, int size) {
    for (int i = 0; i < MM_STORES; ++i) if (sStore[i].base && sStore[i].handle == handle) {
        if (size < 0 || sStore[i].size - (int)(sStore[i].cur - sStore[i].base) < size) { ++mm_store_full; return 0; }   // mm.c:899: no space this tick
        sStore[i].cur += size; ++mm_store_allocs;
        return sStore[i].cur - size;
    }
    return 0;
}
void mm_stores_rewind(void) { for (int i = 0; i < MM_STORES; ++i) if (sStore[i].base) sStore[i].cur = sStore[i].base; }
