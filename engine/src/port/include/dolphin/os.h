#pragma once
// dolphin/os.h - HOST SHADOW of the Dolphin OS header.
//
// This file lives in src/port/include, which the host build searches before
// include/, so a game TU's #include "dolphin/os.h" resolves here instead of the
// real header. The real one pulls in PPC inline asm (OSFastCast, OSException,
// __os) that MSVC/Clang cannot compile; this shadow declares the same OS API in
// portable C. os_shim.c provides the implementations. Only the host build sees
// this - the MWCC matching build uses include/dolphin/os.h unchanged.
//
// Types are declared to match the game's usage; opaque OS objects are sized
// generously so value declarations (OSThread t;) work. Expand as TUs demand.

#include "dolphin/types.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- error / report --------------------------------------------------------
typedef u16 OSError;
#ifndef OS_ERROR_SYSTEM_RESET   /* verbatim from dolphin/os/OSError.h, ALL codes (vita fork) */
#define OS_ERROR_SYSTEM_RESET        0
#define OS_ERROR_MACHINE_CHECK       1
#define OS_ERROR_DSI                 2
#define OS_ERROR_ISI                 3
#define OS_ERROR_EXTERNAL_INTERRUPT  4
#define OS_ERROR_ALIGNMENT           5
#define OS_ERROR_PROGRAM             6
#define OS_ERROR_FLOATING_POINT      7
#define OS_ERROR_DECREMENTER         8
#define OS_ERROR_SYSTEM_CALL         9
#define OS_ERROR_TRACE              10
#define OS_ERROR_PERFORMACE_MONITOR 11
#define OS_ERROR_BREAKPOINT         12
#define OS_ERROR_SYSTEM_INTERRUPT   13
#define OS_ERROR_THERMAL_INTERRUPT  14
#define OS_ERROR_PROTECTION         15
#define OS_ERROR_FPE                16
#define OS_ERROR_MAX                (OS_ERROR_FPE + 1)
#endif
// OSContext, verbatim from include/dolphin/os/OSContext.h (vita fork): game code
// reads its fields (dll_80136a40.c, pi_dolphin.c's srr0), so the host needs the
// real layout, not a forward declaration. 0x2C8 bytes; both f64 arrays are
// 8-aligned, so the ARM layout equals the PPC one -- checked below.
typedef struct OSContext {
    /* 0x000 */ u32 gpr[32];
    /* 0x080 */ u32 cr;
    /* 0x084 */ u32 lr;
    /* 0x088 */ u32 ctr;
    /* 0x08C */ u32 xer;
    /* 0x090 */ f64 fpr[32];
    /* 0x190 */ u32 fpscr_pad;
    /* 0x194 */ u32 fpscr;
    /* 0x198 */ u32 srr0;
    /* 0x19C */ u32 srr1;
    /* 0x1A0 */ u16 mode;
    /* 0x1A2 */ u16 state;
    /* 0x1A4 */ u32 gqr[8];
    /* 0x1C4 */ u32 psf_pad;
    /* 0x1C8 */ f64 psf[32];
} OSContext;
typedef char stfx_oscontext_is_0x2c8[(sizeof(OSContext) == 0x2C8) ? 1 : -1];
typedef void (*OSErrorHandler)(OSError error, OSContext* context, ...);

void OSReport(const char* msg, ...);
void OSVReport(const char* msg, va_list list);
void OSPanic(const char* file, int line, const char* msg, ...);
void OSHalt(const char* msg);
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler);

// --- interrupts ------------------------------------------------------------
BOOL OSDisableInterrupts(void);
BOOL OSEnableInterrupts(void);
BOOL OSRestoreInterrupts(BOOL level);

// --- cache (no-ops on a coherent host) -------------------------------------
void DCInvalidateRange(void* addr, u32 nBytes);
void DCFlushRange(void* addr, u32 nBytes);
void DCStoreRange(void* addr, u32 nBytes);
void DCFlushRangeNoSync(void* addr, u32 nBytes);
void DCStoreRangeNoSync(void* addr, u32 nBytes);
void DCZeroRange(void* addr, u32 nBytes);
void DCTouchRange(void* addr, u32 nBytes);
void ICInvalidateRange(void* addr, u32 nBytes);
void ICFlashInvalidate(void);
void DCEnable(void);

// --- time ------------------------------------------------------------------
typedef s64 OSTime;
typedef u32 OSTick;
OSTime OSGetTime(void);
OSTick OSGetTick(void);
u32    OSGetTickRate(void); // bus clock / 4

// SFA uses a 486MHz-class TB; keep the macros consistent with OSGetTickRate().
#define OS_TIMER_CLOCK (OSGetTickRate())
#define OSTicksToSeconds(t)       ((t) / OS_TIMER_CLOCK)
#define OSTicksToMilliseconds(t)  ((t) / (OS_TIMER_CLOCK / 1000))
#define OSTicksToMicroseconds(t)  (((t) * 8) / (OS_TIMER_CLOCK / 125000))
#define OSMillisecondsToTicks(ms) ((ms) * (OS_TIMER_CLOCK / 1000))
#define OSSecondsToTicks(s)       ((s) * OS_TIMER_CLOCK)
/* vita fork: the console's bus clock (lowmem 0x800000F8 holds 162000000). MusyX sizes its
 * DSP cycle budget from it, so it is the REAL value, not derived from OSGetTickRate().
 * NB: OSGetTickRate() above returns 162 MHz too, but the console's TIMER runs at bus/4 =
 * 40.5 MHz. Self-consistent through these macros; a literal tick constant is not. */
#define __OSBusClock   162000000u
#define OS_BUS_CLOCK   __OSBusClock
#define OS_TICKS_TO_SEC(x)  ((x) / OS_TIMER_CLOCK)
#define OS_TICKS_TO_MSEC(x) ((x) / (OS_TIMER_CLOCK / 1000))
#define OS_TICKS_TO_USEC(x) (((x) * 8) / (OS_TIMER_CLOCK / 125000))
#define OS_TICKS_TO_NSEC(x) (((x) * 8000) / (OS_TIMER_CLOCK / 125000))
#define OS_USEC_TO_TICKS(x) ((x) * (OS_TIMER_CLOCK / 125000) / 8)

// --- stopwatch (fields match the real struct; game declares it by value) ---
typedef struct OSStopwatch {
    char*  name;
    OSTime total;
    u32    hits;
    OSTime min;
    OSTime max;
    OSTime last;
} OSStopwatch;
void   OSInitStopwatch(OSStopwatch* sw, char* name);
void   OSStartStopwatch(OSStopwatch* sw);
void   OSStopStopwatch(OSStopwatch* sw);
OSTime OSCheckStopwatch(OSStopwatch* sw);
void   OSResetStopwatch(OSStopwatch* sw);
void   OSDumpStopwatch(OSStopwatch* sw);

// --- init / system ---------------------------------------------------------
void OSInit(void);
u32  OSGetConsoleType(void);
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu);
u32  OSGetResetCode(void);
void OSSetSoundMode(u32 mode);
u32  OSGetSoundMode(void);
void OSSetProgressiveMode(u32 on);
u32  OSGetProgressiveMode(void);

// --- arena -----------------------------------------------------------------
void* OSGetArenaHi(void);
void* OSGetArenaLo(void);
void  OSSetArenaHi(void* addr);
void  OSSetArenaLo(void* addr);
void* OSAllocFromArenaLo(u32 size, u32 align);
void* OSAllocFromArenaHi(u32 size, u32 align);

// --- heap ------------------------------------------------------------------
typedef int OSHeapHandle;
void*        OSInitAlloc(void* arenaLo, void* arenaHi, int maxHeaps);
extern volatile OSHeapHandle __OSCurrHeap;
OSHeapHandle OSCreateHeap(void* start, void* end);
OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap);
void*        OSAllocFromHeap(OSHeapHandle heap, u32 size);
void*        OSAllocFixed(void** rstart, void** rend);
void         OSFreeToHeap(OSHeapHandle heap, void* ptr);
#define      OSAlloc(size)     OSAllocFromHeap(-1, (size))
#define      OSFree(ptr)       OSFreeToHeap(-1, (ptr))

// --- thread (fields mirror the real OSThread offsets game code reads) -------
#ifndef OS_THREAD_ATTR_DETACH
#define OS_THREAD_ATTR_DETACH 0x0001u   /* verbatim from dolphin/os/OSThread.h (vita fork) */
#endif
enum { OS_THREAD_STATE_READY = 1, OS_THREAD_STATE_RUNNING = 2,
       OS_THREAD_STATE_WAITING = 4, OS_THREAD_STATE_MORIBUND = 8 };
typedef struct OSThread {
    OSContext context;          /* 0x000, 0x2C8 bytes (vita fork: real type) */
    u16   state;    /* 0x2C8 */
    u16   attr;     /* 0x2CA */
    s32   suspend;  /* 0x2CC */
    s32   priority; /* 0x2D0 */
    s32   base;     /* 0x2D4 */
    void* val;      /* 0x2D8 */
    u8    tail[0x34];   /* 0x2DC..0x310: queue, links, join/mutex queues, stack, error (retail layout) */
} OSThread;
typedef struct OSThreadQueue { OSThread* head; OSThread* tail; } OSThreadQueue;
typedef s32 OSPriority;
void OSInitThreadQueue(OSThreadQueue* queue);
BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param,
                    void* stack, u32 stackSize, OSPriority prio, u16 attr);
void OSExitThread(void* val);
void OSCancelThread(OSThread* thread);
BOOL OSJoinThread(OSThread* thread, void** val);
s32  OSResumeThread(OSThread* thread);
s32  OSSuspendThread(OSThread* thread);
BOOL OSIsThreadTerminated(OSThread* thread);
OSThread* OSGetCurrentThread(void);
void OSSleepThread(OSThreadQueue* queue);
void OSWakeupThread(OSThreadQueue* queue);
void OSYieldThread(void);
OSPriority OSGetThreadPriority(OSThread* thread);
BOOL OSSetThreadPriority(OSThread* thread, OSPriority prio);

// --- message queue ---------------------------------------------------------
typedef void* OSMessage;
/* The SDK's 0x20-byte layout (hal/os_threads.c implements it field for field). Game code embeds
   queues at retail offsets (thp/dll_3e.c: base+0x52c), so the size must be retail's. (vita fork) */
typedef struct OSMessageQueue { u8 opaque[0x20]; } OSMessageQueue;
#define OS_MESSAGE_NOBLOCK 0
#define OS_MESSAGE_BLOCK   1
void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount);
BOOL OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags);
BOOL OSJamMessage(OSMessageQueue* mq, OSMessage msg, s32 flags);
BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags);

// --- mutex -----------------------------------------------------------------
typedef struct OSMutex { u8 opaque[0x18]; } OSMutex;
#if !defined(__cplusplus) && defined(__vita__)   /* retail sizes: 32-bit targets only */
_Static_assert(sizeof(OSThread) == 0x310, "OSThread is 0x310 bytes on the GameCube");
_Static_assert(sizeof(OSMessageQueue) == 0x20, "OSMessageQueue is 0x20 bytes on the GameCube");
#endif
void OSInitMutex(OSMutex* mutex);
void OSLockMutex(OSMutex* mutex);
void OSUnlockMutex(OSMutex* mutex);
BOOL OSTryLockMutex(OSMutex* mutex);

// --- alarm -----------------------------------------------------------------
typedef struct OSAlarm OSAlarm;
typedef void (*OSAlarmHandler)(OSAlarm* alarm, OSContext* context);
struct OSAlarm { u8 opaque[0x28]; };
void OSCreateAlarm(OSAlarm* alarm);
void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler);
void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler);
void OSCancelAlarm(OSAlarm* alarm);

// --- font (IPL ROM system font) --------------------------------------------
// Struct layout matches the canonical include/dolphin/os/OSFont.h exactly so a
// deref stays correct; the functions are a HAL boundary (the game's system font
// comes from the GameCube IPL ROM, which the port supplies via os_shim). SFA uses
// this only for the fallback GAMETEXT_SLOT_ERROR charset; real menu/game text is
// rendered from the game's own font data loaded off the disc.
#define OS_FONT_ENCODE_ANSI 0u
#define OS_FONT_ENCODE_SJIS 1u
// ROM font sizes and the IPL language setting: upstream's OSFont.h / OSRtc.h values, which the
// umbrella never carried (needed since upstream's 2026-09 gametext.c / intersect_memcard.c). (vita fork)
#define OS_FONT_ROM_SIZE_ANSI 0x03000
#define OS_FONT_ROM_SIZE_SJIS 0x4D000
#define OS_LANGUAGE_ENGLISH 0
#define OS_LANGUAGE_GERMAN  1
#define OS_LANGUAGE_FRENCH  2
#define OS_LANGUAGE_SPANISH 3
#define OS_LANGUAGE_ITALIAN 4
#define OS_LANGUAGE_DUTCH   5
u8 OSGetLanguage(void);
typedef struct OSFontHeader {
    u16 fontType;
    u16 firstChar;
    u16 lastChar;
    u16 invalChar;
    u16 ascent;
    u16 descent;
    u16 width;
    u16 leading;
    u16 cellWidth;
    u16 cellHeight;
    u32 sheetSize;
    u16 sheetFormat;
    u16 sheetColumn;
    u16 sheetRow;
    u16 sheetWidth;
    u16 sheetHeight;
    u16 widthTable;
    u32 sheetImage;
    u32 sheetFullSize;
    u8 c0;
    u8 c1;
    u8 c2;
    u8 c3;
} OSFontHeader;
u16 OSGetFontEncode(void);
u16 OSSetFontEncode(u16 encode);
BOOL OSInitFont(OSFontHeader* fontData);
u32 OSLoadFont(OSFontHeader* fontData, void* tmp);
char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width);
char* OSGetFontWidth(const char* string, s32* width);
char* OSGetFontTexel(const char* string, void* image, s32 pos, s32 stride, s32* width);
int OSSetFontWidth(int fixed);

// --- locked cache / reboot save region (vita fork) ------------------------
// Real prototypes from dolphin/os/OSCache.h and OSReboot.h, which this shadow
// replaces. The GameCube locked cache and the reboot save region have no host
// meaning; os_shim.c implements them as no-ops. gameloop_main.c and model.c
// call them, and gcc (unlike MSVC) refuses an implicit declaration.
void LCEnable(void);
BOOL OSGetResetButtonState(void);   /* dolphin/os/OSResetSW.h; host: never pressed */
void LCDisable(void);
void LCLoadBlocks(void* destTag, void* srcAddr, u32 numBlocks);
void LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks);
void LCQueueWait(u32 len);
void OSSetSaveRegion(void* start, void* end);

// Fast casts live in dolphin/os/OSFastCast.h (the port shadow).


#ifdef __cplusplus
}
#endif

// The real dolphin/os.h pulls in OSFastCast.h; mirror that so a TU that includes
// only the umbrella still sees OSInitFastCast and the casts. (vita fork)
#include "dolphin/os/OSFastCast.h"
