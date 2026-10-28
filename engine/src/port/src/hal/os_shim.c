// os_shim.c - host implementation of the Dolphin OS API (see dolphin/os.h shadow).
//
// The console's OS handles interrupts, caches, threads, and a boot-time arena.
// On a coherent multitasking host most of this collapses: cache ops are no-ops,
// interrupt enable/disable is a bookkeeping flag, and time comes from the host
// clock. Threads/messages/mutexes are stubbed for now (the game boots effectively
// single-threaded); they get real host backing when a subsystem needs concurrency.

#include "dolphin/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide (vita fork) */
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// --- error / report --------------------------------------------------------
void OSReport(const char* msg, ...) {
    va_list ap; va_start(ap, msg); vprintf(msg, ap); va_end(ap);
}
void OSVReport(const char* msg, va_list list) { vprintf(msg, list); }

void OSPanic(const char* file, int line, const char* msg, ...) {
    fprintf(stderr, "OSPanic %s:%d: ", file, line);
    va_list ap; va_start(ap, msg); vfprintf(stderr, msg, ap); va_end(ap);
    fputc('\n', stderr);
    abort();
}
void OSHalt(const char* msg) { fprintf(stderr, "OSHalt: %s\n", msg ? msg : ""); abort(); }

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error; (void)handler; return NULL;
}

// --- interrupts (single flag; no real ISRs on the host) --------------------
static BOOL gInterruptsEnabled = TRUE;
BOOL OSDisableInterrupts(void) { BOOL prev = gInterruptsEnabled; gInterruptsEnabled = FALSE; return prev; }
BOOL OSEnableInterrupts(void)  { BOOL prev = gInterruptsEnabled; gInterruptsEnabled = TRUE;  return prev; }
BOOL OSRestoreInterrupts(BOOL level) { BOOL prev = gInterruptsEnabled; gInterruptsEnabled = level; return prev; }

// --- cache (host is coherent) ----------------------------------------------
void DCInvalidateRange(void* addr, u32 n)   { (void)addr; (void)n; }
// A flush/store announces a CPU write the GPU must see: on the port that means a cached texture
// over those bytes is re-decoded on its next load (gx_draw.c gx_tex_invalidate; weak, because the
// viewers link without gx_draw). 2026-09-27.
extern void gx_tex_invalidate(const void* addr, u32 n) __attribute__((weak));
// The audio thread (hal/stfx_audio.c) runs MusyX's tick, which "flushes" its DSP structures: never textures, and
// the texture cache is the main thread's alone. The test is a stack-range compare, no syscall.
extern int stfx_audio_on_audio_thread(void) __attribute__((weak));
static void dc_gpu_sees(const void* addr, u32 n) {
    if (!gx_tex_invalidate) return;
    if (stfx_audio_on_audio_thread && stfx_audio_on_audio_thread()) return;
    gx_tex_invalidate(addr, n);
}
void DCFlushRange(void* addr, u32 n)        { dc_gpu_sees(addr, n); }
void DCStoreRange(void* addr, u32 n)        { dc_gpu_sees(addr, n); }
void DCFlushRangeNoSync(void* addr, u32 n)  { dc_gpu_sees(addr, n); }
void DCStoreRangeNoSync(void* addr, u32 n)  { dc_gpu_sees(addr, n); }
void DCZeroRange(void* addr, u32 n)         { if (addr && n) memset(addr, 0, n); }
void DCTouchRange(void* addr, u32 n)        { (void)addr; (void)n; }
void ICInvalidateRange(void* addr, u32 n)   { (void)addr; (void)n; }
void ICFlashInvalidate(void)                { }
void DCEnable(void)                         { }

// --- time ------------------------------------------------------------------
u32 OSGetTickRate(void) { return 162000000u; } // GC bus 243MHz -> TB = bus/2? use 162MHz TB-like base

// THE VIRTUAL CLOCK (vita fork, run 114). ux0:data/stairfax/fixed_clock.txt holds microseconds
// per retrace (16683 = one NTSC field); with it the OS clock is a COUNT OF RETRACES, not wall time.
// Why: the game clamps timeDelta at 6 (100 ms, pi_videoinit.c) and the port runs 250-400 ms frames,
// so everything driven by timeDelta (the cutscene camera) runs slow while everything driven by the
// clock (the actors' sequence, the subtitles) does not: run 113's camera matched retail's to one
// unit at hits 5351/5651/5926 with the subtitle and the actors of hit ~6200-6700 on screen. No
// picture of a cutscene can be compared with retail in that state. With the virtual clock every
// frame is exactly one retrace long whatever it cost. Absent file = wall time, as before.
// (+1 TICK per read: a loop that spins on the clock without waiting for a retrace must still end.)
static long long gVirtTicks; static int gVirtUsPerRetrace = -1;
// THE PACED CLOCK (2026-10-08, docs/47 "the first play session"). On WALL time the game came apart on the device: frame times
// GREW every frame (1,031 -> 1,178 -> 1,327 ms by present 30), the AI tick counter showed 7,977 dropped ticks, and the main
// thread stopped presenting at 157 -- every real millisecond the port falls behind is work the game then tries to catch up.
// On the VIRTUAL clock it is stable but in slow motion (one field of game time per frame drawn). fixed_clock.txt = -N is the
// middle: game time advances by the number of fields that REALLY passed since the last retrace, never more than N a frame.
// At N fields or faster the game runs at real speed; slower than that it slows down instead of spiralling.
static int gVirtPacedMax; static unsigned long long gVirtPacedLast; unsigned os_paced_fields, os_paced_capped;
unsigned os_virtual_retraces;
static void os_virtual_init(void) {
    gVirtUsPerRetrace = 0;
#if defined(__vita__)
    FILE* f = fopen("ux0:data/stairfax/fixed_clock.txt", "r");
    if (f) { int us = 0; if (fscanf(f, "%d", &us) == 1) { if (us > 0 && us < 1000000) gVirtUsPerRetrace = us; else if (us < 0 && us >= -6) { gVirtUsPerRetrace = 16683; gVirtPacedMax = -us; } } fclose(f); }
    printf("[clock] %s\n", gVirtPacedMax ? "PACED: the OS clock advances by the retraces that really elapsed, at most fixed_clock.txt's count per frame" : gVirtUsPerRetrace ? "VIRTUAL: the OS clock advances per retrace (fixed_clock.txt)" : "wall time");
    if (gVirtUsPerRetrace) printf("[clock] %d us per retrace\n", gVirtUsPerRetrace);
#endif
}
// microseconds of virtual time, or -1 when the clock is wall time (dsp_ai_shim.c: the AI DMA ticks)
long long os_virtual_now_us(void) {
    if (gVirtUsPerRetrace < 0) os_virtual_init();
    return gVirtUsPerRetrace > 0 ? __atomic_load_n(&gVirtTicks, __ATOMIC_RELAXED) / 162 : -1;
}
void os_virtual_clock_retrace(void) {   // vi_shim.c VIWaitForRetrace
    if (gVirtUsPerRetrace < 0) os_virtual_init();
    if (gVirtUsPerRetrace > 0) {
        int n = 1;
#if defined(__vita__)
        if (gVirtPacedMax) {
            unsigned long long now = sceKernelGetProcessTimeWide();
            if (gVirtPacedLast) { unsigned long long f = (now - gVirtPacedLast + 8341u) / 16683u; n = f < 1 ? 1 : f > (unsigned)gVirtPacedMax ? gVirtPacedMax : (int)f; if (f > (unsigned)gVirtPacedMax) ++os_paced_capped; }
            gVirtPacedLast = now; os_paced_fields += (unsigned)n;
        }
#endif
        __atomic_add_fetch(&gVirtTicks, (long long)gVirtUsPerRetrace * 162 * n, __ATOMIC_RELAXED); ++os_virtual_retraces;   /* the audio thread reads the clock too (OSGetTick in sal_ai.c:53) */
        /* the disc audio stream follows GAME time: tell it what the game clock now says (hal/dtk_stream.c; dtk_realtime.txt '1' = off) */
        { extern void dtk_stream_game_tick(long long virt_us) __attribute__((weak)); if (dtk_stream_game_tick) dtk_stream_game_tick(__atomic_load_n(&gVirtTicks, __ATOMIC_RELAXED) / 162); }
    }
}
OSTime OSGetTime(void) {
    if (gVirtUsPerRetrace < 0) os_virtual_init();
    if (gVirtUsPerRetrace > 0) return (OSTime)__atomic_add_fetch(&gVirtTicks, 1, __ATOMIC_RELAXED);
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (OSTime)((c.QuadPart * (long long)OSGetTickRate()) / f.QuadPart);
#elif defined(__vita__)
    // vita fork: the process clock, in microseconds, at OSGetTickRate() (162 MHz =
    // 162 ticks per us). The #else below returned 0 FOREVER -- frozen game time, and
    // any "wait until time has passed" loop hangs on its first frame.
    return (OSTime)(sceKernelGetProcessTimeWide() * (OSGetTickRate() / 1000000u));
#else
    return 0;
#endif
}
OSTick OSGetTick(void) { return (OSTick)OSGetTime(); }

// --- MEM1 backing ----------------------------------------------------------
// The console's 24MB MEM1 lives at the fixed physical/cached address 0x80000000,
// and the game reads console globals (bus clock at 0x800000F8, console type, ...)
// AND stores/sub-allocates through absolute 0x8xxxxxxx pointers. Rather than
// rewrite every such access, we map MEM1 at its real address: on 64-bit Windows a
// /LARGEADDRESSAWARE 32-bit process owns the full 4GB user range, so 0x80000000 is
// reservable. Every absolute guest access then simply lands in real memory - the
// most faithful backing, and it keeps stored guest pointers meaningful.
//
// The arena (framebuffers, GX FIFO, game heap) is carved from MEM1 above the OS
// low-memory globals, exactly as videoInit expects.
#define MEM1_BASE  0x80000000u
#define MEM1_SIZE  0x01800000u   // 24MB
#define OS_LOWMEM_END 0x80003100u // OS globals/BI2/dbg live below this on real HW

static u8*  gMem1     = NULL;   // actual mapped base (== MEM1_BASE on success)
static u8*  gArenaLo  = NULL;
static u8*  gArenaHi  = NULL;

static void osArenaInit(void) {
    if (gMem1) return;
#if defined(_WIN32)
    // Reserve+commit exactly at the GameCube MEM1 address.
    gMem1 = (u8*)VirtualAlloc((void*)MEM1_BASE, MEM1_SIZE,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!gMem1) {
        // Fallback: let the OS pick an address. Absolute 0x8xxxxxxx reads will then
        // fault, but relative arena use still works - surfaces the mapping failure.
        fprintf(stderr, "[os] WARNING: could not map MEM1 at 0x%08X (err %lu); "
                        "absolute guest addresses will fault\n",
                        MEM1_BASE, (unsigned long)GetLastError());
        gMem1 = (u8*)VirtualAlloc(NULL, MEM1_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
#else
    gMem1 = (u8*)malloc(MEM1_SIZE);
#endif
    // Populate the OS low-memory globals the game reads.
    if (gMem1 == (u8*)MEM1_BASE) {
        *(u32*)(MEM1_BASE + 0x00F8) = 162000000u; // bus clock (GC: 162MHz)
        *(u32*)(MEM1_BASE + 0x00FC) =  40500000u; // core clock / CPU (162/4-ish base)
        *(u32*)(MEM1_BASE + 0x002C) = 0x10000006u; // console type: retail
    }
    // Arena starts above OS low memory, ends near the top of MEM1.
    gArenaLo = (gMem1 == (u8*)MEM1_BASE) ? (u8*)OS_LOWMEM_END
                                         : (u8*)(((u32)gMem1 + 0x1f) & ~0x1fu);
    gArenaHi = gMem1 + MEM1_SIZE - 0x20000; // leave headroom at the top
}

// --- init / system ---------------------------------------------------------
void OSInit(void) { osArenaInit(); }
u32  OSGetConsoleType(void) { return 0x10000006u; } // retail
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) { (void)reset; (void)resetCode; (void)forceMenu; }
u32  OSGetResetCode(void) { return 0; }
// The sound mode is STORED (vita fork, 2026-10-10, the platform-layer audit): OSRtc.c:162/172 keep it in SRAM, and the game
// reads it back (audio.c:490/600, dlls/engine/55/55.c:401) to decide mono/stereo and to show the option. It was write-only.
static u32 sSoundMode = 1; // stereo, the SRAM default
void OSSetSoundMode(u32 mode) { sSoundMode = mode & 1u; }
u32  OSGetSoundMode(void) { return sSoundMode; }
void OSSetProgressiveMode(u32 on) { (void)on; }
u32  OSGetProgressiveMode(void) { return 0; }

// --- arena / heap (backed by the C heap) -----------------------------------
void* OSGetArenaHi(void) { osArenaInit(); return gArenaHi; }
void* OSGetArenaLo(void) { osArenaInit(); return gArenaLo; }
void  OSSetArenaHi(void* addr) { gArenaHi = (u8*)addr; }
void  OSSetArenaLo(void* addr) { gArenaLo = (u8*)addr; }
void* OSAllocFromArenaLo(u32 size, u32 align) { (void)align; return malloc(size); }
void* OSAllocFromArenaHi(u32 size, u32 align) { (void)align; return malloc(size); }

volatile OSHeapHandle __OSCurrHeap = -1;
void*        OSInitAlloc(void* lo, void* hi, int maxHeaps) { (void)hi; (void)maxHeaps; return lo; }
OSHeapHandle OSCreateHeap(void* start, void* end) { (void)start; (void)end; return 0; }
OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) { (void)heap; return 0; }
void*        OSAllocFromHeap(OSHeapHandle heap, u32 size) { (void)heap; return malloc(size); }
void*        OSAllocFixed(void** rstart, void** rend) { (void)rend; return rstart ? *rstart : NULL; }
void         OSFreeToHeap(OSHeapHandle heap, void* ptr) { (void)heap; free(ptr); }

#if !(defined(TARGET_PC) && !defined(_WIN32))   /* the Vita build: hal/os_threads.c (baton scheduler) */
// --- threads (stubbed; game boots effectively single-threaded) -------------
void OSInitThreadQueue(OSThreadQueue* q) { if (q) { q->head = NULL; q->tail = NULL; } }
BOOL OSCreateThread(OSThread* t, void* (*f)(void*), void* p, void* s, u32 sz, OSPriority pr, u16 a) {
    (void)t; (void)f; (void)p; (void)s; (void)sz; (void)pr; (void)a; return TRUE;
}
void OSExitThread(void* v) { (void)v; }
void OSCancelThread(OSThread* t) { (void)t; }
BOOL OSJoinThread(OSThread* t, void** v) { (void)t; if (v) *v = NULL; return TRUE; }
s32  OSResumeThread(OSThread* t) { (void)t; return 0; }
s32  OSSuspendThread(OSThread* t) { (void)t; return 0; }
BOOL OSIsThreadTerminated(OSThread* t) { (void)t; return TRUE; }
OSThread* OSGetCurrentThread(void) {
    static OSThread gMainThread; // one host "main" thread, always running
    gMainThread.state = OS_THREAD_STATE_RUNNING;
    return &gMainThread;
}
void OSSleepThread(OSThreadQueue* q) { (void)q; }
void OSWakeupThread(OSThreadQueue* q) { (void)q; }
void OSYieldThread(void) { }
OSPriority OSGetThreadPriority(OSThread* t) { (void)t; return 16; }
BOOL OSSetThreadPriority(OSThread* t, OSPriority p) { (void)t; (void)p; return TRUE; }

// --- message queue (stub) --------------------------------------------------
void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* arr, s32 n) { (void)mq; (void)arr; (void)n; }
BOOL OSSendMessage(OSMessageQueue* mq, OSMessage m, s32 f) { (void)mq; (void)m; (void)f; return TRUE; }
BOOL OSJamMessage(OSMessageQueue* mq, OSMessage m, s32 f) { (void)mq; (void)m; (void)f; return TRUE; }
BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* m, s32 f) { (void)mq; (void)f; if (m) *m = NULL; return FALSE; }

// --- mutex (single-thread: trivially satisfied) ----------------------------
void OSInitMutex(OSMutex* m) { (void)m; }
void OSLockMutex(OSMutex* m) { (void)m; }
void OSUnlockMutex(OSMutex* m) { (void)m; }
BOOL OSTryLockMutex(OSMutex* m) { (void)m; return TRUE; }

#endif

// --- stopwatch -------------------------------------------------------------
void   OSInitStopwatch(OSStopwatch* sw, char* name) { if (sw) { memset(sw, 0, sizeof(*sw)); sw->name = name; } }
void   OSStartStopwatch(OSStopwatch* sw) { if (sw) sw->last = OSGetTime(); }
void   OSStopStopwatch(OSStopwatch* sw) { if (sw) { sw->total += OSGetTime() - sw->last; sw->hits++; } }
OSTime OSCheckStopwatch(OSStopwatch* sw) { return sw ? sw->total : 0; }
void   OSResetStopwatch(OSStopwatch* sw) { if (sw) { sw->total = 0; sw->hits = 0; } }
void   OSDumpStopwatch(OSStopwatch* sw) { (void)sw; }

// --- alarm (stub) ----------------------------------------------------------
void OSCreateAlarm(OSAlarm* a) { (void)a; }
void OSSetAlarm(OSAlarm* a, OSTime t, OSAlarmHandler h) { (void)a; (void)t; (void)h; }
void OSSetPeriodicAlarm(OSAlarm* a, OSTime s, OSTime p, OSAlarmHandler h) { (void)a; (void)s; (void)p; (void)h; }
void OSCancelAlarm(OSAlarm* a) { (void)a; }

// --- font (IPL ROM system font) --------------------------------------------
// The GameCube supplies this font from IPL ROM; the port has no ROM, and SFA uses it
// only for the fallback GAMETEXT_SLOT_ERROR charset (real text = disc font). ANSI
// encoding, empty load. Give real impls if the error charset is ever needed on screen.
u16 OSGetFontEncode(void) { return OS_FONT_ENCODE_ANSI; }
u32 OSLoadFont(OSFontHeader* fontData, void* tmp) {
    (void)tmp;
    if (fontData) memset(fontData, 0, sizeof(OSFontHeader));
    return 0;
}
char* OSGetFontTexel(const char* string, void* image, s32 pos, s32 stride, s32* width) {
    (void)string; (void)image; (void)pos; (void)stride;
    if (width) *width = 0;
    return 0;
}
char* OSGetFontWidth(const char* string, s32* width) {
    (void)string;
    if (width) *width = 0;
    return 0;
}

// --- math helpers used by intersect_render.c (real impls; float returns must be exact
// or callers NaN). __frsqrte is the PPC reciprocal-sqrt estimate; atanf_fast/mathSinCosf
// are game math helpers whose real TUs aren't compiled yet. ---
double __frsqrte(double x) { return x > 0.0 ? 1.0 / sqrt(x) : 0.0; }
float atanf_fast(float x) { return atanf(x); }
void mathSinCosf(float angle, float* sinOut, float* cosOut) {
    if (sinOut) *sinOut = sinf(angle);
    if (cosOut) *cosOut = cosf(angle);
}

// Locked cache / reboot save region: no host meaning (see dolphin/os.h). vita fork.
void LCEnable(void) {}
void LCDisable(void) {}
void OSSetSaveRegion(void* start, void* end) { (void)start; (void)end; }
BOOL OSGetResetButtonState(void) { return FALSE; }   /* no reset button on host (vita fork) */
// Locked-cache DMA: on GameCube these move 32-byte blocks between main memory and
// the locked cache (0 blocks = 128). mm.c's other branch does the same with a
// memcpy, so the host does a synchronous copy. (vita fork)
// WARNING: a destination/source that is the locked-cache address itself
// (0xE0000000) is unmapped on the host -- see docs/47.
#include <string.h>
void LCLoadBlocks(void* destTag, void* srcAddr, u32 numBlocks)  { memcpy(destTag, srcAddr, (numBlocks ? numBlocks : 128u) * 32u); }
void LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks) { memcpy(destAddr, srcTag, (numBlocks ? numBlocks : 128u) * 32u); }
void LCQueueWait(u32 len) { (void)len; }   /* host copies are synchronous */

// The IPL (SRAM) language setting. The Vita build ships the English disc (GSAE01): English.
// (vita fork, 2026-09-27: upstream's intersect_memcard.c now reads it.)
u8 OSGetLanguage(void) { return OS_LANGUAGE_ENGLISH; }
