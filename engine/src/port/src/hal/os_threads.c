// os_threads.c - Dolphin OS threads, thread queues, message queues and mutexes on the PS Vita,
// as a BATON scheduler (vita fork, 2026-09-27).
//
// Why: the THP movie player (src/main/thp/*) creates read / video-decode / audio-decode threads
// and blocks the main thread in OSReceiveMessage until the decoder reports ready. The HAL's
// previous thread layer was stubs (OSCreateThread created nothing, OSReceiveMessage returned FALSE
// at once), so preparing the movie could only fail -- and the title menu reads input only once
// the movie is prepared.
//
// Model: the GameCube OS on one core. Every OSThread gets a real Vita thread, but exactly ONE of
// them runs at a time: it holds the baton, and hands it on only where the Dolphin OS would switch:
//   - the running thread blocks (sleep on a thread queue, full/empty message queue, held mutex),
//     suspends itself, yields, or exits -> the highest-priority runnable thread runs next;
//   - a thread made runnable (wakeup, resume) with a HIGHER priority (lower number) than the
//     running one preempts it at once, as the Dolphin scheduler does.
// So game code keeps its single-core guarantees (OSDisableInterrupts sections stay atomic, a
// thread runs until it blocks) without any preemptive-threading races.
//
// A deadlock (the running thread blocks and nothing else is runnable) is REPORTED, never silent:
// one "[os] DEADLOCK" line per site, and the blocking call returns as the old stubs did
// (OSReceiveMessage FALSE with *msg untouched). Counters: os_thread_switches, os_deadlocks.
//
// Layouts: OSMessageQueue and OSMutex are the SDK's exact 32-bit layouts (0x20 / 0x18 bytes) --
// game code embeds queues at retail offsets (thp/dll_3e.c: a queue at base+0x52c in a retail
// buffer). Thread queues are keys only; waiters are tracked in this file's own table.
#if defined(TARGET_PC) && !defined(_WIN32)
#include "dolphin/os.h"
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

typedef struct {                        // the SDK's OSMessageQueue, field for field
    OSThreadQueue queueSend;            // 0x00
    OSThreadQueue queueReceive;         // 0x08
    OSMessage* msgArray;                // 0x10
    s32 msgCount;                       // 0x14
    s32 firstIndex;                     // 0x18
    s32 usedCount;                      // 0x1C
} MQ;
typedef struct {                        // the SDK's OSMutex
    OSThreadQueue queue;                // 0x00
    OSThread* thread;                   // 0x08 owner
    s32 count;                          // 0x0C recursion
    void* link[2];                      // 0x10 (the owner's mutex list; unused here)
} MX;
_Static_assert(sizeof(MQ) == 0x20 || sizeof(void*) != 4, "OSMessageQueue is 0x20 bytes on the GameCube");   /* (a 64-bit HOST test build has wider pointers: host/tests/os_ext_test.c) */
_Static_assert(sizeof(MQ) <= sizeof(OSMessageQueue) || sizeof(void*) != 4, "port OSMessageQueue too small");   /* (that test pads its queues) */
_Static_assert((sizeof(MX) == 0x18 && sizeof(MX) <= sizeof(OSMutex)) || sizeof(void*) != 4, "OSMutex is 0x18 bytes");

enum { T_FREE, T_RUNNABLE, T_BLOCKED, T_DEAD };
#define MAX_THREADS 32
typedef struct {
    OSThread* os;                       // the game's OSThread (key)
    SceUID vita, sema;                  // its Vita thread and baton semaphore
    int state, suspend, prio;
    void* waitKey;                      // the OSThreadQueue it sleeps on (T_BLOCKED)
    int deadlocked;                     // woken by the deadlock breaker, not by a wakeup
    int cancelled;                      // OSCancelThread: its Vita thread must exit when next woken
    int msgWait;                        // T_BLOCKED on a message queue / mutex / join / external wait (1), or a bare OSSleepThread (0)
    void* (*func)(void*);
    void* arg;
    void* ret;
    unsigned order;                     // FIFO among equal priorities
} Tcb;

static Tcb gT[MAX_THREADS];
// TIME PER THREAD (2026-10-08, docs/47 "Run 142"): ~20 ms of every frame is outside the frame build and outside the swap. The
// baton scheduler runs the game's other threads (audio, the THP player) on the main thread's time, so each handoff stamps the
// clock and charges the outgoing holder. os_thread_times() gives vita_tool_main.c the total for every thread but main.
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#define OST_NOW() sceKernelGetProcessTimeWide()
#else
#define OST_NOW() 0ull
#endif
static unsigned long long gThrUs[MAX_THREADS], gThrT0; static unsigned gThrRuns[MAX_THREADS];
static void ostCharge(const Tcb* out, const Tcb* in) {
    unsigned long long now = OST_NOW();
    if (out && gThrT0) gThrUs[out - gT] += now - gThrT0;
    gThrT0 = now;
    if (in) ++gThrRuns[in - gT];
}
unsigned long long os_thread_times(unsigned long long* mainUs) {   /* microseconds held by every thread but main (and main's, if asked) */
    unsigned long long o = 0;
    for (int i = 1; i < MAX_THREADS; ++i) o += gThrUs[i];
    if (mainUs) *mainUs = gThrUs[0];
    return o;
}
void os_thread_report(void) {   /* one line per thread that has ever run */
    for (int i = 0; i < MAX_THREADS; ++i) if (gThrRuns[i] || gThrUs[i])
        printf("[thr] %d%s func %p prio %d state %d: held the baton %u times, %.1f ms cumulative\n", i, i ? "" : " (main)", (void*)gT[i].func, gT[i].prio,
               gT[i].state, gThrRuns[i], gThrUs[i] / 1000.0);
}
static Tcb* gCur;                       // the baton holder
// EXTERNAL WAITS (2026-10-11, docs/47 "the movie decode off the baton"): see os_ext_wait below.
static Tcb* gExtT;                      // the one thread waiting for work done OUTSIDE the baton (NULL: none)
static SceUID gExtSema = -1;            // ... the Vita semaphore that work signals (count 1) when it is done
static void (*gExtCancel)(void);        // ... and what stops that work if the waiter is cancelled
unsigned os_ext_waits, os_ext_polled, os_ext_held;   // waits; ended by a poll at an interrupt window's end; ended with the baton HELD
unsigned long long os_ext_held_us;                   // ... and the time the baton holder spent blocked for those
static void extWake(void) {             // the external work is done (its signal has been consumed): the waiter may run
    Tcb* t = gExtT;
    gExtT = NULL;
    if (t->state == T_BLOCKED) { t->state = T_RUNNABLE; t->waitKey = NULL; }
}
static OSThread gMainOS;                // OSGetCurrentThread() for the Vita main thread
static unsigned gOrder;
unsigned os_thread_switches, os_deadlocks, os_main_irq_waits;

static Tcb* tcbOf(OSThread* t) {
    int i;
    for (i = 0; i < MAX_THREADS; i++)       // a DEAD entry is history, never the live thread
        if (gT[i].state != T_FREE && gT[i].state != T_DEAD && gT[i].os == t) return &gT[i];
    return NULL;
}

static void ensureMain(void) {          // the Vita main thread is thread 0, created on first use
    if (gCur) return;
    memset(gT, 0, sizeof(gT));
    gT[0].os = &gMainOS;
    gT[0].vita = sceKernelGetThreadId();
    gT[0].sema = sceKernelCreateSema("stfx_baton_main", 0, 0, 1, NULL);
    gT[0].state = T_RUNNABLE;
    gT[0].prio = 16;                    // the Dolphin OS main thread's priority
    gMainOS.priority = 16;
    gMainOS.state = 2;                  // OS_THREAD_STATE_RUNNING
    gCur = &gT[0];
}

static Tcb* pickNext(void) {            // highest priority (lowest number), then FIFO
    Tcb* best = NULL;
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        Tcb* t = &gT[i];
        if (t->state != T_RUNNABLE || t->suspend > 0) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->order < best->order)) best = t;
    }
    return best;
}

// Hand the baton to `next` and wait until it comes back to the caller.
static void switchTo(Tcb* next) {
    Tcb* me = gCur;
    if (next == me) return;
    ++os_thread_switches;
    next->order = ++gOrder;
    ostCharge(me, next);
    gCur = next;
    sceKernelSignalSema(next->sema, 1);
    sceKernelWaitSema(me->sema, 1, NULL);
    if (me->cancelled) sceKernelExitDeleteThread(0);   // woken only to die (OSCancelThread)
    // the baton is back: gCur == me (set by whoever handed it over)
}

// The running thread can no longer run (blocked / suspended / dead): run someone else.
// Returns 0 normally, -1 if nothing else is runnable (DEADLOCK: the caller resumes as if woken).
//
// Who is released on a deadlock: the MAIN thread if it is blocked, else the caller. On the GameCube
// the main thread's sleeps end by INTERRUPT (the VI retrace wakes waitNextFrame's flip queue); in
// this port the "interrupts" run on the main thread itself (VIWaitForRetrace), so time can only pass
// if main runs. Releasing a worker instead would hand it a NULL message it never expected.
static int reschedule(const char* why, void* key) {
    Tcb* next = pickNext();
#ifndef OS_EXT_BREAK_2   /* host/tests/os_ext_test.c: without this the test must FAIL */
    if (!next && gExtT && !(gT[0].state == T_BLOCKED && !gT[0].msgWait)) {
        // Nothing can run, and one thread waits for work a NON-baton thread is doing (os_ext_wait). That
        // is not a deadlock: the work ends by itself. The baton holder really blocks until it is
        // signalled (no baton thread could have used the time), then the waiter runs. NOT taken when
        // main sleeps on a bare thread queue (waitNextFrame's flip queue): that sleep ends by an
        // interrupt, interrupts are delivered on main, so main is released below, as always.
        unsigned long long t0 = OST_NOW();
        Tcb* t;
        ++os_ext_held;
        for (;;) {
            SceUInt timeout = 2000000;
            if (sceKernelWaitSema(gExtSema, 1, &timeout) >= 0) break;
            printf("[os] external wait: no signal in 2 s (thread %d waits, thread %d holds the baton) -- still waiting\n",
                   (int)(gExtT - gT), (int)(gCur - gT));
        }
        os_ext_held_us += OST_NOW() - t0;
        t = gExtT;
        extWake();
        if (t == gCur) return 0;
        switchTo(t);                    // the caller stays BLOCKED until a real wakeup
        return 0;
    }
#endif
    if (!next) {
        static const void* lastKey;
        Tcb* victim = gT[0].state == T_BLOCKED ? &gT[0] : gCur;
        if (victim == &gT[0] && gCur == &gT[0]) {
            // MAIN waits with nothing else runnable: on the GameCube an INTERRUPT would end this
            // (waitNextFrame's flip queue is woken by the retrace); here interrupts are delivered
            // on main (VIWaitForRetrace), so main is released. Counted, not a deadlock.
            ++os_main_irq_waits;
        } else {
            ++os_deadlocks;
            if (key != lastKey) {
                printf("[os] DEADLOCK: thread %d blocks (%s %p) with nothing else runnable -- thread %d's "
                       "blocking call returns as unblocked (the pre-2026-09-27 stub behaviour)\n",
                       (int)(gCur - gT), why, key, (int)(victim - gT));
                lastKey = key;
            }
        }
        victim->state = T_RUNNABLE;
        victim->waitKey = NULL;
        victim->deadlocked = 1;
        if (victim == gCur) return -1;
        switchTo(victim);               // the caller stays BLOCKED until a real wakeup
        return 0;
    }
    switchTo(next);
    return 0;
}

// A thread became runnable: preempt the caller if it outranks it (Dolphin: immediate reschedule).
// Inside an interrupt (os_irq_enter .. os_irq_exit) the switch is DEFERRED to the interrupt's end,
// as on the GameCube: a handler that wakes a thread is not itself paused half-way.
static int gIrqDepth, gIrqResched;
static void maybePreempt(void) {
    Tcb* next;
    if (gIrqDepth > 0) { gIrqResched = 1; return; }
    next = pickNext();
    if (next && next != gCur && next->prio < gCur->prio) {
        gCur->order = ++gOrder;          // the preempted thread goes to the back of its level
        switchTo(next);
    }
}

// Interrupt delivery (vi_shim.c VIWaitForRetrace: the VI retrace callbacks and the AI DMA ticks).
void os_irq_enter(void) { ensureMain(); ++gIrqDepth; }
void os_irq_exit(void) {
    if (--gIrqDepth != 0) return;
    // The end of an interrupt window is where an external wait is ended (os_ext_wait): the caller holds
    // the baton, so the scheduler's state is its to change, and a poll costs one system call.
#ifndef OS_EXT_BREAK_1
    if (gExtT && sceKernelPollSema(gExtSema, 1) >= 0) { ++os_ext_polled; gExtT->order = ++gOrder; extWake(); gIrqResched = 1; }
#endif
    if (gIrqResched) { gIrqResched = 0; maybePreempt(); }
}

// MID-FRAME POLL (2026-10-11, docs/47 "movie upload"): os_irq_exit is the only place an external wait ended, and
// the windows come at the END of main's frame (VIWaitForRetrace), 33-66 ms apart, so a 16 ms decode that finished
// at +16 ms woke its thread at +50: one decode per game frame at best. os_ext_poll is called by the baton holder
// at a point where the scheduler's state is its to change (GXLoadTexObj entry: before it reads any GX state),
// never inside an interrupt window. It does what os_irq_exit does: take the one signal, make the waiter runnable,
// preempt if it outranks the caller. os_ext_midframe = 0 (movie_wake_off.txt) restores the window-only wake.
int os_ext_midframe = 1;
unsigned os_ext_midpolled;
void os_ext_poll(void) {
    if (!gExtT || gIrqDepth > 0 || !os_ext_midframe) return;
#ifdef OS_EXT_BREAK_4   /* host/tests/os_ext_test.c: with the poll disabled B2 must FAIL */
    return;
#endif
    if (sceKernelPollSema(gExtSema, 1) >= 0) { ++os_ext_midpolled; gExtT->order = ++gOrder; extWake(); maybePreempt(); }
}

// ---- external waits -----------------------------------------------------------------------------
// A baton thread hands work to a Vita thread that is NOT an OSThread (the THP video decode,
// hal/thp_decode.c) and must sleep until it is done WITHOUT holding the baton. The outside thread
// may touch nothing in this file -- every variable here belongs to whoever holds the baton -- so
// its only message is a Vita semaphore (created with max count 1), signalled ONCE per piece of
// work. The baton side consumes that one signal in exactly one of three places, each on the baton:
//   1. here, at once, if the work is already done (poll);
//   2. os_irq_exit, at the end of an interrupt window (VIWaitForRetrace opens two or three a frame):
//      a poll; the waiter becomes runnable and preempts if it outranks the caller, like any wakeup;
//   3. reschedule(), when the baton holder blocks and NOTHING else is runnable: it waits on the
//      semaphore for real (the baton was idle anyway), then runs the waiter.
// There is no wakeup to lose: the signal is a kernel count that stays until one of the three takes
// it, and gExtT is cleared by the same code that takes it. One waiter at a time; a second one, or a
// wait inside an interrupt window (no thread switch is allowed there), blocks holding the baton.
// If the waiter is CANCELLED (OSCancelThread) its work is still running: onCancel is called first,
// on the cancelling thread, and must not return until the outside thread is idle (it takes the
// signal itself). A caller loops on its own "done" flag: this returns when the semaphore was taken.
void os_ext_wait(int sema, void (*onCancel)(void)) {
    Tcb* me;
    ensureMain();
    ++os_ext_waits;
    if (sceKernelPollSema(sema, 1) >= 0) return;
    if (gIrqDepth > 0 || gExtT) {
        unsigned long long t0 = OST_NOW();
        ++os_ext_held;
        sceKernelWaitSema(sema, 1, NULL);
        os_ext_held_us += OST_NOW() - t0;
        return;
    }
    me = gCur;
    gExtT = me;
    gExtSema = sema;
    gExtCancel = onCancel;
    me->state = T_BLOCKED;
    me->waitKey = &gExtT;
    me->deadlocked = 0;
    me->msgWait = 1;
    reschedule("external wait", &gExtT);
}

// ---- thread queues -----------------------------------------------------------------------------
void OSInitThreadQueue(OSThreadQueue* q) { if (q) { q->head = NULL; q->tail = NULL; } }

static void sleepOn(OSThreadQueue* q, int msgWait) {
    ensureMain();
    gCur->state = T_BLOCKED;
    gCur->waitKey = q;
    gCur->deadlocked = 0;
    gCur->msgWait = msgWait;
    reschedule("sleep", q);
}
// A bare sleep is the game waiting for an INTERRUPT (pi_videoinit.c waitNextFrame: the flip queue);
// a sleep inside a message queue or a mutex (sleepOn(q, 1)) waits for another THREAD. reschedule()
// tells them apart when nothing is runnable and an external wait is pending.
void OSSleepThread(OSThreadQueue* q) { sleepOn(q, 0); }

static int wakeKey(void* key) {
    int i, n = 0;
    for (i = 0; i < MAX_THREADS; i++)
        if (gT[i].state == T_BLOCKED && gT[i].waitKey == key) {
            gT[i].state = T_RUNNABLE;
            gT[i].waitKey = NULL;
            gT[i].order = ++gOrder;
            n++;
        }
    return n;
}

void OSWakeupThread(OSThreadQueue* q) {
    ensureMain();
    if (wakeKey(q)) maybePreempt();
}

void OSYieldThread(void) {
    Tcb* next;
    ensureMain();
    gCur->order = ++gOrder;
    next = pickNext();                  // only an EQUAL or higher priority thread gets the core
    if (next && next != gCur && next->prio <= gCur->prio) switchTo(next);
}

// ---- threads -----------------------------------------------------------------------------------
static int threadEntry(SceSize args, void* argp) {
    Tcb* t = *(Tcb**)argp;
    (void)args;
    sceKernelWaitSema(t->sema, 1, NULL);        // first run: wait for the baton
    if (t->cancelled) sceKernelExitDeleteThread(0);
    t->ret = t->func(t->arg);
    OSExitThread(t->ret);                       // does not return
    return 0;
}

BOOL OSCreateThread(OSThread* os, void* (*f)(void*), void* p, void* stack, u32 size, OSPriority prio, u16 attr) {
    int i;
    Tcb* t = NULL;
    char name[32];
    (void)stack; (void)size; (void)attr;
    ensureMain();
    for (i = 1; i < MAX_THREADS && !t; i++)     // the same OSThread's dead slot first (the movie
        if (gT[i].state == T_DEAD && gT[i].os == os) t = &gT[i];   // player re-creates its threads)
    for (i = 1; i < MAX_THREADS && !t; i++)
        if (gT[i].state == T_FREE || gT[i].state == T_DEAD) t = &gT[i];
    if (!t || !os) { printf("[os] OSCreateThread: no thread slot\n"); return FALSE; }
    // (a dead slot's semaphore stays: a cancelled Vita thread may still be leaving through it)
    memset(t, 0, sizeof(*t));
    t->os = os;
    t->func = f;
    t->arg = p;
    t->prio = prio;
    t->suspend = 1;                             // Dolphin threads are created SUSPENDED
    t->state = T_RUNNABLE;
    snprintf(name, sizeof(name), "stfx_os_%d", i);
    t->sema = sceKernelCreateSema(name, 0, 0, 1, NULL);
    // Game thread stacks are small GameCube buffers; the Vita thread gets its own 256 KB.
    t->vita = sceKernelCreateThread(name, threadEntry, 0x10000100, 0x40000, 0, 0, NULL);
    if (t->vita < 0 || t->sema < 0) { printf("[os] OSCreateThread: Vita thread/sema failed\n"); t->state = T_FREE; return FALSE; }
    memset(os, 0, sizeof(*os));
    os->priority = prio;
    os->base = prio;
    os->suspend = 1;
    os->state = 1;                              // OS_THREAD_STATE_READY
    { Tcb* tp = t; sceKernelStartThread(t->vita, sizeof(tp), &tp); }
    return TRUE;
}

void OSExitThread(void* val) {
    Tcb* me;
    ensureMain();
    me = gCur;
    me->ret = val;
    me->state = T_DEAD;
    if (me->os) { me->os->state = 8; me->os->val = val; }  // OS_THREAD_STATE_MORIBUND
    wakeKey(me);                                // OSJoinThread waiters sleep on the tcb
    if (me == &gT[0]) { printf("[os] OSExitThread on the MAIN thread -- ignored\n"); me->state = T_RUNNABLE; return; }
    {   Tcb* next = pickNext();
        if (!next && gExtT && gExtT != me) {    // nothing runnable but an external wait: that work ends by itself (see reschedule)
            sceKernelWaitSema(gExtSema, 1, NULL);
            next = gExtT;
            extWake();
        }
        if (next) { ostCharge(me, next); gCur = next; ++os_thread_switches; sceKernelSignalSema(next->sema, 1); }
        else printf("[os] DEADLOCK: the last runnable thread exited\n");
    }
    sceKernelExitDeleteThread(0);
}

void OSCancelThread(OSThread* os) {
    Tcb* t;
    ensureMain();
    t = tcbOf(os);
    if (!t) return;
    if (t == gCur) OSExitThread(NULL);
    if (t == gExtT) {                           // its work outside the baton is still running (os_ext_wait):
        gExtT = NULL;                           // stopped and waited for HERE, before the caller can free
#ifndef OS_EXT_BREAK_3
        if (gExtCancel) gExtCancel();           // what that work reads and writes
#endif
    }
    t->state = T_DEAD;                          // never scheduled again ...
    t->cancelled = 1;
    if (t->os) t->os->state = 8;
    wakeKey(t);                                 // (OSJoinThread waiters)
    sceKernelSignalSema(t->sema, 1);            // ... and its parked Vita thread wakes only to exit.
    {   // WAIT for it to be gone: the slot may be re-created for the same OSThread next (the movie
        // player does), and a late-waking old thread must never read a reused, cleared slot.
        SceUInt timeout = 1000000;
        if (sceKernelWaitThreadEnd(t->vita, NULL, &timeout) < 0)
            printf("[os] OSCancelThread: Vita thread %d did not exit in 1 s\n", (int)(t - gT));
        sceKernelDeleteSema(t->sema);
        t->sema = -1;
    }
}

BOOL OSJoinThread(OSThread* os, void** val) {
    Tcb* t;
    ensureMain();
    t = tcbOf(os);
    if (!t) { if (val) *val = NULL; return TRUE; }
    while (t->state != T_DEAD) {
        gCur->state = T_BLOCKED;
        gCur->waitKey = t;
        gCur->msgWait = 1;
        if (reschedule("join", t) < 0) return FALSE;
    }
    if (val) *val = t->ret;
    return TRUE;
}

s32 OSResumeThread(OSThread* os) {
    Tcb* t;
    s32 prev;
    ensureMain();
    t = tcbOf(os);
    if (!t) return 0;
    prev = t->suspend;
    if (t->suspend > 0 && --t->suspend == 0) {
        os->suspend = 0;
        t->order = ++gOrder;
        if (t->state == T_RUNNABLE) maybePreempt();
    }
    return prev;
}

s32 OSSuspendThread(OSThread* os) {
    Tcb* t;
    s32 prev;
    ensureMain();
    t = tcbOf(os);
    if (!t) return 0;
    prev = t->suspend++;
    if (os) os->suspend = t->suspend;
    if (t == gCur && prev == 0) reschedule("suspend", os);
    return prev;
}

BOOL OSIsThreadTerminated(OSThread* os) { Tcb* t = tcbOf(os); return !t || t->state == T_DEAD; }

OSThread* OSGetCurrentThread(void) { ensureMain(); return gCur->os; }

OSPriority OSGetThreadPriority(OSThread* os) { Tcb* t = tcbOf(os); return t ? t->prio : 16; }

BOOL OSSetThreadPriority(OSThread* os, OSPriority p) {
    Tcb* t;
    ensureMain();
    t = tcbOf(os);
    if (!t) return FALSE;
    t->prio = p;
    os->priority = p;
    if (t != gCur) maybePreempt();
    else { Tcb* n = pickNext(); if (n && n->prio < p) { gCur->order = ++gOrder; switchTo(n); } }
    return TRUE;
}

// ---- message queues (the SDK's algorithm) ------------------------------------------------------
void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* arr, s32 n) {
    MQ* q = (MQ*)mq;
    OSInitThreadQueue(&q->queueSend);
    OSInitThreadQueue(&q->queueReceive);
    q->msgArray = arr;
    q->msgCount = n;
    q->firstIndex = 0;
    q->usedCount = 0;
}

BOOL OSSendMessage(OSMessageQueue* mq, OSMessage m, s32 flags) {
    MQ* q = (MQ*)mq;
    ensureMain();
    while (q->msgCount <= q->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        sleepOn(&q->queueSend, 1);
        if (gCur->deadlocked) return FALSE;
    }
    q->msgArray[(q->firstIndex + q->usedCount) % q->msgCount] = m;
    q->usedCount++;
    OSWakeupThread(&q->queueReceive);
    return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* mq, OSMessage m, s32 flags) {
    MQ* q = (MQ*)mq;
    ensureMain();
    while (q->msgCount <= q->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        sleepOn(&q->queueSend, 1);
        if (gCur->deadlocked) return FALSE;
    }
    q->firstIndex = (q->firstIndex + q->msgCount - 1) % q->msgCount;
    q->msgArray[q->firstIndex] = m;
    q->usedCount++;
    OSWakeupThread(&q->queueReceive);
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* m, s32 flags) {
    MQ* q = (MQ*)mq;
    ensureMain();
    while (q->usedCount == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        sleepOn(&q->queueReceive, 1);
        if (gCur->deadlocked) return FALSE;
    }
    if (m) *m = q->msgArray[q->firstIndex];
    q->firstIndex = (q->firstIndex + 1) % q->msgCount;
    q->usedCount--;
    OSWakeupThread(&q->queueSend);
    return TRUE;
}

// ---- mutexes ------------------------------------------------------------------------------------
void OSInitMutex(OSMutex* mx) {
    MX* x = (MX*)mx;
    OSInitThreadQueue(&x->queue);
    x->thread = NULL;
    x->count = 0;
}

void OSLockMutex(OSMutex* mx) {
    MX* x = (MX*)mx;
    OSThread* me;
    ensureMain();
    me = gCur->os;
    for (;;) {
        if (!x->thread) { x->thread = me; x->count = 1; return; }
        if (x->thread == me) { x->count++; return; }
        sleepOn(&x->queue, 1);
        if (gCur->deadlocked) { x->thread = me; x->count = 1; return; }   // reported; take it
    }
}

void OSUnlockMutex(OSMutex* mx) {
    MX* x = (MX*)mx;
    ensureMain();
    if (x->thread != gCur->os) return;
    if (--x->count == 0) {
        x->thread = NULL;
        OSWakeupThread(&x->queue);
    }
}

BOOL OSTryLockMutex(OSMutex* mx) {
    MX* x = (MX*)mx;
    ensureMain();
    if (!x->thread) { x->thread = gCur->os; x->count = 1; return TRUE; }
    if (x->thread == gCur->os) { x->count++; return TRUE; }
    return FALSE;
}
#endif
