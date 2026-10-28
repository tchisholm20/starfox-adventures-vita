/* gx_thread_plat.h -- the platform layer of the GX render-thread ring (src/gx_thread.c, host/engine/RT_THREAD.md 6.1).
 *
 * ONE interface, two implementations, so the SAME gx_thread.c runs on the console and in the host tests:
 *     Vita   psp2 kernel threads / semaphores / delay / cpu id / process clock / affinity / thread info / sceIo (markers)
 *     host   (-DGXT_HOST)  Win32 (_WIN32) or POSIX threads (-DGXT_HOST_PTHREAD forces pthreads, which MinGW's winpthreads also builds)
 * Nothing here is fatal on failure that the contract says is not fatal: affinity and cpu-id failures are ignored (cpu id answers 0),
 * a failed thread / semaphore creation is REPORTED to the caller (gx_thread.c turns it into "REFUSED, staying DIRECT").
 *
 * Semaphores are BINARY (max count 1): a second signal while one is pending is dropped (SCE_KERNEL_ERROR_SEMA_OVF on the Vita), so a
 * stale signal costs at most one spurious re-check of a predicate, never an unbounded backlog.  Waits take a timeout in microseconds.
 *
 * Handles are plain ints (the header's `sema_r`, `sema_g`, `r_thread` are int).  Host handles are small indexes into a table.
 */
#ifndef GX_THREAD_PLAT_H
#define GX_THREAD_PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define GXTP_MARKER_DIR "ux0:data/stairfax/"

#if defined(__vita__) && !defined(GXT_HOST)
/* =============================================================================================================================== Vita */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/io/fcntl.h>

static void (*gxtp_entry_fn)(void);
static int gxtp_vita_entry(SceSize args, void *argp) { (void) args; (void) argp; gxtp_entry_fn(); return 0; }

static inline uint32_t gxtp_now_us(void)  { return (uint32_t) sceKernelGetProcessTimeWide(); }
static inline void     gxtp_delay_us(uint32_t us) { sceKernelDelayThread(us); }
static inline int      gxtp_cpu_id(void)  { const int c = sceKernelGetCpuId(); return c < 0 ? 0 : c; }
static inline void     gxtp_relax(void)   { __asm__ volatile("yield" ::: "memory"); }
static inline uintptr_t gxtp_thread_pointer(void) { uintptr_t v; __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v)); return v; }   /* TPIDRURO: the TLS pointer, readable in user mode */

static inline int gxtp_sema_create(const char *name)
{
    const int s = (int) sceKernelCreateSema(name, 0, 0, 1, NULL);
    return s < 0 ? -1 : s;
}
static inline void gxtp_sema_delete(int s)  { if (s > 0) sceKernelDeleteSema(s); }
static inline void gxtp_sema_signal(int s)  { sceKernelSignalSema(s, 1); }      /* an overflow (already signalled) is the binary-semaphore behaviour we want */
/* 1 = signalled, 0 = timed out, -1 = error */
static inline int  gxtp_sema_wait(int s, uint32_t timeout_us)
{
    SceUInt t = timeout_us;
    const int r = sceKernelWaitSema(s, 1, &t);
    if (r >= 0) return 1;
    if ((unsigned) r == (unsigned) SCE_KERNEL_ERROR_WAIT_TIMEOUT) return 0;
    return -1;
}

static inline int gxtp_core_mask(int core)
{
    return core == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : core == 1 ? SCE_KERNEL_CPU_MASK_USER_1 : core == 2 ? SCE_KERNEL_CPU_MASK_USER_2 : 0;
}
/* core: 0..2 pin to that user core, anything else = the system default.  Returns the thread id or -1. */
static inline int gxtp_thread_create(const char *name, void (*fn)(void), int prio, uint32_t stack_bytes, int core)
{
    gxtp_entry_fn = fn;
    const int m = gxtp_core_mask(core);
    const SceUID t = sceKernelCreateThread(name, gxtp_vita_entry, prio, stack_bytes, 0, m ? m : SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
    return t < 0 ? -1 : (int) t;
}
static inline int  gxtp_thread_start(int t) { return sceKernelStartThread(t, 0, NULL) < 0 ? -1 : 0; }
/* 1 = the thread has ended, 0 = still running after timeout_us */
static inline int  gxtp_thread_join(int t, uint32_t timeout_us)
{
    int st = 0; SceUInt to = timeout_us;
    return sceKernelWaitThreadEnd(t, &st, &to) >= 0 ? 1 : 0;
}
static inline void gxtp_thread_delete(int t) { if (t > 0) sceKernelDeleteThread(t); }
static inline void gxtp_host_init(void) {}
static inline void gxtp_set_affinity_self(int core)   /* never fatal */
{
    const int m = gxtp_core_mask(core);
    if (m) (void) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), m);
}
/* The calling thread's stack range; the thread-info syscall fails => lo = hi = 0 (gxt_on_R then answers 0 and the stack watermark is skipped). */
static inline void gxtp_self_stack(uintptr_t *lo, uintptr_t *hi, uint32_t hint_bytes)
{
    SceKernelThreadInfo ti; (void) hint_bytes;
    memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) == 0 && ti.stack != NULL && ti.stackSize > 0) {
        *lo = (uintptr_t) ti.stack; *hi = (uintptr_t) ti.stack + (uintptr_t) ti.stackSize;
    } else { *lo = 0; *hi = 0; }
}
/* Reads up to cap-1 bytes of marker `name` into buf (NUL terminated).  Returns the byte count, or -1 if the file is absent. */
static inline int gxtp_read_marker(const char *name, char *buf, int cap)
{
    char path[160];
    snprintf(path, sizeof path, GXTP_MARKER_DIR "%s", name);
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (f < 0) return -1;
    int n = sceIoRead(f, buf, (unsigned) (cap - 1));
    sceIoClose(f);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

#else
/* =============================================================================================================================== host */
#include <stdlib.h>
#if defined(_WIN32) && !defined(GXT_HOST_PTHREAD)
#define GXTP_WIN32 1
#include <windows.h>
#else
#define GXTP_WIN32 0
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#endif

#define GXTP_MAX_OBJ 32
static void (*gxtp_entry_fn)(void);
static uintptr_t gxtp_host_tp_dummy;

#if GXTP_WIN32
/* ---- Win32 ---- */
static HANDLE gxtp_sem[GXTP_MAX_OBJ];
static HANDLE gxtp_thr[GXTP_MAX_OBJ];
static LARGE_INTEGER gxtp_qpf;

static inline uint32_t gxtp_now_us(void)
{
    LARGE_INTEGER c;
    if (gxtp_qpf.QuadPart == 0) QueryPerformanceFrequency(&gxtp_qpf);
    QueryPerformanceCounter(&c);
    return (uint32_t) ((uint64_t) c.QuadPart * 1000000ull / (uint64_t) gxtp_qpf.QuadPart);
}
static inline void gxtp_delay_us(uint32_t us) { Sleep((us + 999u) / 1000u); }
static inline int  gxtp_cpu_id(void) { return (int) (GetCurrentProcessorNumber() & 3u); }
static inline void gxtp_relax(void) { YieldProcessor(); }

static inline int gxtp_sema_create(const char *name)
{
    (void) name;
    for (int i = 1; i < GXTP_MAX_OBJ; ++i)
        if (gxtp_sem[i] == NULL) { gxtp_sem[i] = CreateSemaphoreA(NULL, 0, 1, NULL); return gxtp_sem[i] ? i : -1; }
    return -1;
}
static inline void gxtp_sema_delete(int s) { if (s > 0 && s < GXTP_MAX_OBJ && gxtp_sem[s]) { CloseHandle(gxtp_sem[s]); gxtp_sem[s] = NULL; } }
static inline void gxtp_sema_signal(int s) { if (s > 0 && s < GXTP_MAX_OBJ && gxtp_sem[s]) ReleaseSemaphore(gxtp_sem[s], 1, NULL); }
static inline int  gxtp_sema_wait(int s, uint32_t timeout_us)
{
    if (s <= 0 || s >= GXTP_MAX_OBJ || !gxtp_sem[s]) return -1;
    const DWORD r = WaitForSingleObject(gxtp_sem[s], (timeout_us + 999u) / 1000u);
    return r == WAIT_OBJECT_0 ? 1 : r == WAIT_TIMEOUT ? 0 : -1;
}
static DWORD WINAPI gxtp_win_entry(LPVOID a) { (void) a; gxtp_entry_fn(); return 0; }
int gxtp_host_fail_thread;                                /* host tests: make the next thread create fail */
static inline int gxtp_thread_create(const char *name, void (*fn)(void), int prio, uint32_t stack_bytes, int core)
{
    (void) name; (void) prio; (void) core;
    if (gxtp_host_fail_thread) return -1;
    gxtp_entry_fn = fn;
    for (int i = 1; i < GXTP_MAX_OBJ; ++i)
        if (gxtp_thr[i] == NULL) {
            gxtp_thr[i] = CreateThread(NULL, stack_bytes, gxtp_win_entry, NULL, CREATE_SUSPENDED, NULL);
            return gxtp_thr[i] ? i : -1;
        }
    return -1;
}
static inline int  gxtp_thread_start(int t) { return (t > 0 && t < GXTP_MAX_OBJ && gxtp_thr[t] && ResumeThread(gxtp_thr[t]) != (DWORD) -1) ? 0 : -1; }
static inline int  gxtp_thread_join(int t, uint32_t timeout_us)
{
    if (t <= 0 || t >= GXTP_MAX_OBJ || !gxtp_thr[t]) return 1;
    return WaitForSingleObject(gxtp_thr[t], (timeout_us + 999u) / 1000u) == WAIT_OBJECT_0 ? 1 : 0;
}
static inline void gxtp_thread_delete(int t) { if (t > 0 && t < GXTP_MAX_OBJ && gxtp_thr[t]) { CloseHandle(gxtp_thr[t]); gxtp_thr[t] = NULL; } }
static inline void gxtp_set_affinity_self(int core) { (void) core; }
static inline void gxtp_host_init(void) { static int done; if (!done) { done = 1; timeBeginPeriod(1); } }
#else
/* ---- POSIX ---- */
typedef struct { pthread_mutex_t m; pthread_cond_t c; int count; int used; } GxtpSem;
static GxtpSem gxtp_sem[GXTP_MAX_OBJ];
static pthread_t gxtp_thr[GXTP_MAX_OBJ];
static int gxtp_thr_used[GXTP_MAX_OBJ], gxtp_thr_done[GXTP_MAX_OBJ];
static pthread_mutex_t gxtp_thr_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gxtp_thr_c = PTHREAD_COND_INITIALIZER;

static inline uint32_t gxtp_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull);
}
static inline void gxtp_delay_us(uint32_t us) { struct timespec ts = { (time_t) (us / 1000000u), (long) (us % 1000000u) * 1000L }; nanosleep(&ts, NULL); }
static inline int  gxtp_cpu_id(void)
{
#if defined(__linux__)
    const int c = sched_getcpu(); return c < 0 ? 0 : (c & 3);
#else
    return 0;
#endif
}
static inline void gxtp_relax(void) { __asm__ volatile("" ::: "memory"); }

static inline int gxtp_sema_create(const char *name)
{
    (void) name;
    for (int i = 1; i < GXTP_MAX_OBJ; ++i)
        if (!gxtp_sem[i].used) { pthread_mutex_init(&gxtp_sem[i].m, NULL); pthread_cond_init(&gxtp_sem[i].c, NULL); gxtp_sem[i].count = 0; gxtp_sem[i].used = 1; return i; }
    return -1;
}
static inline void gxtp_sema_delete(int s) { if (s > 0 && s < GXTP_MAX_OBJ && gxtp_sem[s].used) { gxtp_sem[s].used = 0; pthread_mutex_destroy(&gxtp_sem[s].m); pthread_cond_destroy(&gxtp_sem[s].c); } }
static inline void gxtp_sema_signal(int s)
{
    if (s <= 0 || s >= GXTP_MAX_OBJ || !gxtp_sem[s].used) return;
    pthread_mutex_lock(&gxtp_sem[s].m);
    if (gxtp_sem[s].count < 1) { gxtp_sem[s].count = 1; pthread_cond_signal(&gxtp_sem[s].c); }
    pthread_mutex_unlock(&gxtp_sem[s].m);
}
static inline int gxtp_sema_wait(int s, uint32_t timeout_us)
{
    if (s <= 0 || s >= GXTP_MAX_OBJ || !gxtp_sem[s].used) return -1;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t ns = (uint64_t) ts.tv_nsec + (uint64_t) timeout_us * 1000ull;
    ts.tv_sec += (time_t) (ns / 1000000000ull); ts.tv_nsec = (long) (ns % 1000000000ull);
    int r = 0;
    pthread_mutex_lock(&gxtp_sem[s].m);
    while (gxtp_sem[s].count == 0 && r != ETIMEDOUT) r = pthread_cond_timedwait(&gxtp_sem[s].c, &gxtp_sem[s].m, &ts);
    const int got = gxtp_sem[s].count > 0;
    if (got) gxtp_sem[s].count = 0;
    pthread_mutex_unlock(&gxtp_sem[s].m);
    return got ? 1 : 0;
}
static void *gxtp_posix_entry(void *a)
{
    const int i = (int) (intptr_t) a;
    gxtp_entry_fn();
    pthread_mutex_lock(&gxtp_thr_m); gxtp_thr_done[i] = 1; pthread_cond_broadcast(&gxtp_thr_c); pthread_mutex_unlock(&gxtp_thr_m);
    return NULL;
}
static uint32_t gxtp_thr_stack[GXTP_MAX_OBJ];
int gxtp_host_fail_thread;                                /* host tests: make the next thread create fail */
static inline int gxtp_thread_create(const char *name, void (*fn)(void), int prio, uint32_t stack_bytes, int core)
{
    (void) name; (void) prio; (void) core;
    if (gxtp_host_fail_thread) return -1;
    gxtp_entry_fn = fn;
    for (int i = 1; i < GXTP_MAX_OBJ; ++i)
        if (!gxtp_thr_used[i]) {
            /* POSIX has no create-suspended: gxtp_thread_start() is where the thread is born; the slot and the stack size are reserved here. */
            gxtp_thr_used[i] = 1; gxtp_thr_done[i] = 0; gxtp_thr_stack[i] = stack_bytes; gxtp_thr[i] = 0;
            return i;
        }
    return -1;
}
static inline int gxtp_thread_start(int t)
{
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, gxtp_thr_stack[t] ? gxtp_thr_stack[t] : (1u << 20));
    const int r = pthread_create(&gxtp_thr[t], &at, gxtp_posix_entry, (void *) (intptr_t) t);
    pthread_attr_destroy(&at);
    return r == 0 ? 0 : -1;
}
static inline int gxtp_thread_join(int t, uint32_t timeout_us)
{
    if (t <= 0 || t >= GXTP_MAX_OBJ || !gxtp_thr_used[t]) return 1;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t ns = (uint64_t) ts.tv_nsec + (uint64_t) timeout_us * 1000ull;
    ts.tv_sec += (time_t) (ns / 1000000000ull); ts.tv_nsec = (long) (ns % 1000000000ull);
    int r = 0;
    pthread_mutex_lock(&gxtp_thr_m);
    while (!gxtp_thr_done[t] && r != ETIMEDOUT) r = pthread_cond_timedwait(&gxtp_thr_c, &gxtp_thr_m, &ts);
    const int done = gxtp_thr_done[t];
    pthread_mutex_unlock(&gxtp_thr_m);
    if (done) pthread_join(gxtp_thr[t], NULL);
    return done;
}
static inline void gxtp_thread_delete(int t) { if (t > 0 && t < GXTP_MAX_OBJ) gxtp_thr_used[t] = 0; }
static inline void gxtp_set_affinity_self(int core) { (void) core; }
static inline void gxtp_host_init(void) {}
#endif

static inline uintptr_t gxtp_thread_pointer(void) { return gxtp_host_tp_dummy; }

/* Host stack range: the Vita answers from the thread-info syscall; here the entry frame stands in.  `hint_bytes` is the size the thread was created with. */
static inline void gxtp_self_stack(uintptr_t *lo, uintptr_t *hi, uint32_t hint_bytes)
{
    const uintptr_t fa = (uintptr_t) __builtin_frame_address(0);
    const uintptr_t margin = 128u * 1024u;       /* the pages the loader / guard handling may own at the far end of the reservation */
    *hi = fa + 4096u;
    *lo = hint_bytes > margin * 2 ? fa - (hint_bytes - margin) : fa - hint_bytes / 2;
}
/* The host marker directory is $GXT_MARKER_DIR (a trailing slash is added); unset => every marker is absent. */
static inline int gxtp_read_marker(const char *name, char *buf, int cap)
{
    const char *d = getenv("GXT_MARKER_DIR");
    char path[512];
    if (!d || !*d) return -1;
    snprintf(path, sizeof path, "%s/%s", d, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    const size_t n = fread(buf, 1, (size_t) (cap - 1), f);
    fclose(f);
    buf[n] = 0;
    return (int) n;
}
#endif

#endif /* GX_THREAD_PLAT_H */
