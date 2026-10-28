// dsp_ai_shim.c - the audio DSP and the audio interface (AI), as MusyX sees them (vita fork).
// STAGE 2 REPLACEMENT (2026-10-10) for the "instant and silent" shim. Install as src/port/src/hal/dsp_ai_shim.c
// together with stfx_audio.c/.h, dspmix.c/.h and musyx_endian.c/.h (PATCH.md). NOT RUN ON A CONSOLE.
//
// MusyX (src/musyx, compiled as C) drives two pieces of hardware that the Vita does not have.
//
// DSP. One task (sal_dsp.c:258-283). Per 5 ms frame MusyX builds a command list and sends two mails
//   (sal_dsp.c:303-310): 0xBABE<<16 | first size, then the list's address. Here the second mail RECORDS the
//   address; the pass itself (stfx_audio_pass -> dspmix_run) is run by the tick driver after the AI callback has
//   returned, and the task's res_cb is delivered before the next AI callback. That is the hardware's order: the
//   DSP mixes while the CPU finishes its interrupt handler, then raises "pass finished".
//
//   ⚠ WHY res_cb MATTERS (the old shim's defect, read in the source and reproduced by test_stfx_audio.c against a
//   transcription of sal_ai.c): salCtrlDsp clears salDspCallbackEnabled on EVERY tick (sal_dsp.c:301); salCallback
//   runs the synth only while it is non-zero and otherwise parks the tick (sal_ai.c:54-61); the only code that sets
//   it again is salInitAi (sal_ai.c:104, once), dspInitCallback (:69, once) and dspResumeCallback (:82) -- the
//   task's res_cb. The old shim called res_cb only from DSPAssertInt, which MusyX calls only at hwExit
//   (sal_dsp.c:291 <- hw_init.c:98). So the synth ran ONE frame and every later AI callback was parked, while
//   ai_ticks kept counting AI callbacks. The OFF/DATA/TONE levels keep that behaviour on purpose: they are the
//   "as before" arms.
//
// AI. Records the DMA callback (MusyX's salCallback = the synth tick; or the movie player's). Who calls it:
//   FULL     the AUDIO THREAD calls salCallback once per 160 output frames, under the lock that sndBegin/sndEnd
//            take on the game side (ai_shim_thread_tick) -- ALSO while a foreign callback is registered on top of
//            it (run 199: the attract movie's callback, n_options.c:311, is registered for the whole title phase
//            and beyond, so "foreign callback => ticks on main" meant the thread NEVER owned the tick: choppy).
//            The movie's callback (mode 2, n_attractmode.c:207) calls the callback it replaced (n_options.c:335),
//            then takes AIGetDMAStartAddr() as MusyX's chunk and mixes its own audio over it into its own buffer
//            (:336-349). So when a foreign callback is registered over salCallback at FULL:
//              - AIRegisterDMACallback hands it a NO-OP STAND-IN as "the previous callback" (no second synth tick),
//                and maps the stand-in back to salCallback when the movie restores it (picmenu.c:151);
//              - AIGetDMAStartAddr tells it there is no chunk to mix over (0x80000000: the movie adds 0x80000000
//                and gets NULL, which AttractMovieAudio_Mix treats as "movie audio only", n_options.c:247-308);
//              - it is still delivered on the main thread at retraces (it uses OS message queues: the baton
//                scheduler's), and the buffer it registers goes through the ring to the output thread, which ADDS
//                it to the synth's chunk with saturation.
//   RETRACE, and a foreign callback with NO salCallback under it at FULL: the main thread at each retrace
//            (ai_shim_deliver, from vi_shim.c:163), one call per 5 ms of game-clock time as before; each produced
//            chunk goes to the output thread through a ring, and is all the output there is.
//   OFF/DATA/TONE: the old delivery, unchanged.
//
// ONE INTERRUPT WINDOW PER FOREIGN CALLBACK (2026-10-11, after run 208; NOT RUN ON A CONSOLE). A retrace delivers every
//   AI callback that came due since the last one -- 16 to 20 after a frame that waited for a movie decode. They used
//   to share one os_irq_enter/os_irq_exit pair, and inside it no other baton thread runs (os_threads.c:183-198). The
//   movie's callback takes its samples from THREE decoded buffers of one THP frame each (~33 ms, n_options.c:253-300)
//   and hands each empty one back to the audio decode thread -- which could not run until the window closed. So one
//   retrace could never supply more than ~100 ms of movie audio, and the callbacks after that got silence
//   (n_options.c:256). On the GameCube each DMA interrupt is its own interrupt and the decode thread (priority 12,
//   above main's 16) refills between them. Now each FOREIGN callback has its own window. salCallback's delivery is
//   untouched. ux0:data/stairfax/movie_aiwin_off.txt starting with 1 = one window for the lot (A/B).
//   host/tests/movie_pacing_model.py: with the frame skip alone the movie reaches 87% of real time; with this, 100%.
//
// Every symbol the old file exported is still here (users listed in NOTES.md section 8) EXCEPT sndPushGroup:
// the real one in musyx/runtime/snd_groups.c now links. Refusing a push (OFF, or data that was not byte-swapped)
// is done by the --wrap in vt_game.c, which asks stfx_audio_group_gate().
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef STFX_AUDIO_HOST_TEST            /* host test: no Dolphin headers, the same names */
typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;
typedef void (*DSPCallback)(void* task);
typedef struct DSPTaskInfo { DSPCallback init_cb, res_cb; } DSPTaskInfo;
#else
#include "dolphin/types.h"
#include "dolphin/dsp.h"
#endif
#include "stfx_audio.h"

typedef void (*AIDCallback)(void);

static DSPTaskInfo* gTask;
static AIDCallback  gAiCb;                 // what the GAME registered last; written by game threads only (under the lock at FULL)
static volatile int gSynthUnder;           // 1 = gAiCb is a foreign callback registered OVER salCallback at FULL: the audio thread still ticks salCallback
static volatile u32 gAiStart, gAiLen;      // AIInitDMA from a game thread
static volatile u32 gSynthStart, gSynthLen; // AIInitDMA from the audio thread (salCallback's own chunk): neither owner clobbers the other
static u32 gAiRate = 32000;
static volatile int gAiRunning;
unsigned dsp_mails, dsp_tasks, ai_ticks;   // read by the port's report (ai_ticks: vt_game.c:64)

// New-handshake state. Touched only by whoever holds the DSP barrier (stfx_audio_dsp_acquire).
static int gMailExpectList;                // the 0xBABE mail has arrived; the next mail is the list address
static u32 gPassList;                      // a command list waiting for its pass
static int gResumeOwed;                    // a pass has run and res_cb has not been delivered yet
static volatile unsigned gGrants;          // audio_clock.txt: ticks the game clock has allowed the audio thread
unsigned dsp_passes, dsp_resumes, dsp_resume_reentries, dsp_mail_unknown, dsp_pass_overrun, dsp_boot_sync;
unsigned ai_ticks_thread, ai_ticks_retrace, ai_grants_dropped, ai_rate_mismatch;
unsigned ai_foreign_deliveries;            // foreign callbacks run on the main thread while the audio thread owns the synth
unsigned ai_standin_calls;                 // times a foreign callback called "the previous callback" and got the stand-in
unsigned ai_foreign_installs;              // times a foreign callback was registered over salCallback at FULL

// MusyX symbols, weak: this file must link without the runtime (the viewers) and in the host test.
extern void salCallback() __attribute__((weak));          // sal_ai.c:49
extern u32  salAiDmaBuffer __attribute__((weak));         // sal_ai.c:26: 4 chunks of 0x280 bytes

static int is_sal(AIDCallback cb) { return cb && salCallback && (void (*)())cb == (void (*)())salCallback; }
static int thread_owned(void) {            // MusyX's own tick is registered (on top, or under a foreign callback), and the AI is running
    return gAiRunning && (is_sal(gAiCb) || gSynthUnder);
}
int ai_shim_foreign_over_synth(void) { return gSynthUnder; }
// What a foreign callback is given as "the callback I replaced" (see the header): calling it must not tick the synth.
static void ai_synth_standin(void) { ++ai_standin_calls; }

void DSPInit(void) { }
// ⚠ RETRACTION KEPT from the old shim, code unchanged (device run 5, 2026-09-22): the task's init_cb must NOT run before
// DSPAddTask returns. salInitDsp does DSPAddTask(); salDspInitIsDone = 0; sndEnd(); while (!done) {} (sal_dsp.c:
// 275-280) -- on hardware the boot interrupt arrives AFTER that clear. Run synchronously, the callback set the
// flag, MusyX cleared it, and the main thread spun forever. The callback comes from a short-lived thread, after
// the caller has moved on. It touches two volatile flags only (sal_ai.c:67-71), so it needs no lock.
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
static int dsp_boot_thread(SceSize args, void* argp) {
    DSPTaskInfo* task = *(DSPTaskInfo**)argp;
    (void)args;
    sceKernelDelayThread(2000);                        // let the caller clear its flag first
    if (task && task->init_cb) task->init_cb(task);
    return sceKernelExitDeleteThread(0);
}
#endif
unsigned dsp_boot_async;
DSPTaskInfo* DSPAddTask(DSPTaskInfo* task) {
    gTask = task; ++dsp_tasks;
    gMailExpectList = 0; gPassList = 0; gResumeOwed = 0;
    if (!task || !task->init_cb) return task;
#if defined(__vita__)
    {
        SceUID t = sceKernelCreateThread("stfx_dsp_boot", dsp_boot_thread, 0x10000100, 0x1000, 0, 0, NULL);
        if (t >= 0 && sceKernelStartThread(t, sizeof task, &task) >= 0) { ++dsp_boot_async; return task; }
    }
#endif
    ++dsp_boot_sync;       // no thread: synchronous. On the Vita salInitDsp then spins forever (see above): counted, so the watchdog dump has a cause
    task->init_cb(task);
    return task;
}
void DSPHalt(void) { }
u32  DSPGetDMAStatus(void) { return 0; }             // no DMA in flight: salStartDsp's wait passes (sal_dsp.c:288)
u32  DSPCheckMailToDSP(void) { return 0; }            // the mailbox is always drained: salCtrlDsp's two waits pass (sal_dsp.c:304, 308)

void DSPSendMailToDSP(u32 mail) {
    ++dsp_mails;
    if (!stfx_audio_new_dsp()) return;                // as before: consumed, nothing mixed
    if (!gMailExpectList) {
        if ((mail >> 16) == 0xBABEu) gMailExpectList = 1;   // low half = the first chunk's size: a DMA length, of no use here
        else ++dsp_mail_unknown;
    } else {
        gMailExpectList = 0;
        if (gPassList) ++dsp_pass_overrun;            // two lists and no pass between them: the driver broke its own rule
        gPassList = mail;
    }
}

// One DSP pass over a recorded list. The caller holds the DSP barrier.
static const int16_t* run_pass(u32 list) {
    const int16_t* out = stfx_audio_pass(list);
    ++dsp_passes;
    gResumeOwed = 1;
    return out;
}

// "Pass finished": the task's res_cb = dspResumeCallback (sal_ai.c:76-89). It sets salDspCallbackEnabled and, if an
// AI callback was parked meanwhile, runs the synth tick right there -- which sends two more mails. That cannot
// happen while this file drives the ticks (res_cb is always delivered before the next AI callback), but it is
// handled: the pass is run at once so the handshake stays whole; its 160 frames are not played (counted).
// The caller holds the DSP barrier and, at FULL, the lock.
static void deliver_resume(void) {
    int guard = 0;
    while (gResumeOwed && guard++ < 4) {
        gResumeOwed = 0;
        ++dsp_resumes;
        if (gTask && gTask->res_cb) gTask->res_cb(gTask);
        if (gPassList) { u32 l = gPassList; gPassList = 0; ++dsp_resume_reentries; run_pass(l); }
    }
}

// salStartDsp (sal_dsp.c:285-293), which MusyX calls from hwExit only (hw_init.c:98) and then FREES the PBs, the
// command list and the AI buffer (sal_studio.c:130-150, sal_ai.c:129). So: wait for a pass in flight to end.
void DSPAssertInt(void) {
    if (!stfx_audio_new_dsp()) {                      // as before: the task "resumes" and finishes at once
        if (gTask && gTask->res_cb) gTask->res_cb(gTask);
        return;
    }
    if (stfx_audio_dsp_acquire()) {
        gResumeOwed = 1;
        deliver_resume();
        stfx_audio_dsp_release();
    } else if (gTask && gTask->res_cb) gTask->res_cb(gTask);
}

void PPCSync(void) { }                                // no write-gather pipe to drain here

void AIInit(u8* stack) { (void)stack; (void)stfx_audio_level(); stfx_audio_start(); }   // audio.c:595, the main thread, before sndInit
void AIReset(void) { stfx_audio_lock(); gAiCb = NULL; gSynthUnder = 0; gAiRunning = 0; stfx_audio_unlock(); }
// The lock: a tick in flight on the audio thread ends before anything changes. Cases, in the order they can happen:
//   foreign over salCallback at FULL   -> the synth stays the audio thread's; the caller gets the stand-in as "previous"
//   the stand-in comes back (restore)  -> salCallback is on top again; returns the foreign callback
//   salCallback / NULL / another one while a foreign callback is over the synth -> plain replacement, as on hardware
//   anything at another level, or with no salCallback registered -> plain replacement (the old behaviour)
AIDCallback AIRegisterDMACallback(AIDCallback cb) {
    AIDCallback old;
    stfx_audio_lock();
    old = gAiCb;
    if (cb == ai_synth_standin) {                     // AttractMovieAudio_Shutdown, picmenu.c:149-152
        gAiCb = gSynthUnder ? (AIDCallback)salCallback : NULL;
        gSynthUnder = 0;
    } else if (cb && !is_sal(cb) && is_sal(old) && stfx_audio_level() == STFX_AUDIO_FULL && stfx_audio_new_dsp()) {
        gAiCb = cb; gSynthUnder = 1; ++ai_foreign_installs;
#if !defined(STFX_SHIM_FAULT) || STFX_SHIM_FAULT != 3  /* test-only fault 3: hand out the real salCallback -> the foreign callback ticks the synth a second time; the test MUST fail */
        old = ai_synth_standin;
#endif
    } else {
        gAiCb = cb; gSynthUnder = 0;
    }
    stfx_audio_unlock();
    return old;
}
// Two owners can register a buffer: salCallback on the audio thread, and a game thread (the movie; or salCallback
// itself when ticks are delivered at retraces). Kept in separate registers.
void AIInitDMA(u32 start_addr, u32 length) {
    if (stfx_audio_on_audio_thread()) { gSynthStart = start_addr; gSynthLen = length; }
    else { gAiStart = start_addr; gAiLen = length; }
}
// n_options.c:333,336 adds 0x80000000 back to get "the chunk MusyX just registered" and mixes the movie over it.
// While the audio thread owns the synth there is no such chunk on this side: 0x80000000 + 0x80000000 = NULL = "no
// source", and the movie's buffer then holds the movie's audio alone (n_options.c:180, 247-308).
u32  AIGetDMAStartAddr(void) {
    if (stfx_audio_on_audio_thread()) return gSynthStart;
    return gSynthUnder ? 0x80000000u : gAiStart;
}
void AIStartDMA(void) { gAiRunning = 1; }
void AIStopDMA(void) { stfx_audio_lock(); gAiRunning = 0; stfx_audio_unlock(); }
void AISetDSPSampleRate(u32 rate) {                   // audio.c:596 passes 0 = AI_SAMPLERATE_32KHZ; 1 = 48 kHz
    gAiRate = rate == 0 ? 32000u : rate == 1 ? 48000u : rate;
    if (gAiRate != STFX_AUDIO_RATE) ++ai_rate_mismatch;   // the output port is opened at 32 kHz
}

// The old tick: one AI callback, nothing else. Used by the OFF/DATA/TONE levels.
void ai_shim_tick(void) {
    if (gAiRunning && gAiCb) { ++ai_ticks; gAiCb(); }
}

// The new tick, first half: deliver the previous pass's res_cb, run the AI callback, hand back the command list
// it mailed. Returns 0 if no callback ran. On return with *list != 0 the caller OWNS the DSP barrier and must
// run_pass() then stfx_audio_dsp_release(); with *list == 0 the barrier has been released.
// The caller holds the lock if the callback is MusyX's and another thread could be inside MusyX (FULL).
static int tick_begin(u32* list, AIDCallback cb) {
    *list = 0;
    if (!stfx_audio_dsp_acquire()) return 0;          // the previous pass has not ended within 1 s: counted there
#if !defined(STFX_SHIM_FAULT) || STFX_SHIM_FAULT != 1  /* test-only fault 1: never deliver res_cb (the old shim's defect); test_stfx_audio MUST fail */
    deliver_resume();
#endif
    if (!gAiRunning || !cb) { stfx_audio_dsp_release(); return 0; }
    ++ai_ticks;
    cb();
    *list = gPassList; gPassList = 0;
    if (!*list) stfx_audio_dsp_release();
    return 1;
}

// FULL: called by the audio thread once per 160 output frames (stfx_audio.c fill_chunk). See stfx_audio.h.
int ai_shim_thread_tick(const int16_t** chunk) {
    u32 list = 0;
    int ran;
    *chunk = NULL;
    if (stfx_audio_level() != STFX_AUDIO_FULL) return -1;
    if (!thread_owned()) return (gAiRunning && gAiCb) ? -1 : 0;   // unlocked look: a foreign callback is ticked on main
    if (!stfx_audio_lock_timed(8000)) return 0;       // a game thread is inside MusyX for more than 8 ms: silence, counted
    if (!thread_owned()) { stfx_audio_unlock(); return 0; }
    if (stfx_audio_game_clock()) {                    // audio_clock.txt: only as many ticks as game time has passed
        unsigned g = __atomic_load_n(&gGrants, __ATOMIC_RELAXED);
        if (!g) { stfx_audio_unlock(); return 0; }
        __atomic_sub_fetch(&gGrants, 1, __ATOMIC_RELAXED);
    }
    ran = tick_begin(&list, (AIDCallback)salCallback);   // MusyX's tick, whatever the game registered on top of it
    if (ran) ++ai_ticks_thread;
    stfx_audio_unlock();                              // the pass needs no lock (stfx_audio.c stfx_audio_pass)
    if (ran && list) { *chunk = run_pass(list); stfx_audio_dsp_release(); }
    return *chunk ? 1 : 0;
}

// The buffer the AI would play: what the callback registered with AIInitDMA. MusyX registers its chunk with the
// GameCube cached-RAM base added (sal_ai.c:52,108: buffer + 0x80000000, which wraps on a 32-bit machine); the
// movie registers a plain pointer (n_options.c:316,340). Vita heap addresses already have bit 31 set, so the bit
// cannot tell them apart: the MusyX buffer's own range does.
static const int16_t* ai_buffer(void) {
    u32 start = gAiStart, base = &salAiDmaBuffer ? salAiDmaBuffer : 0, alt = start - 0x80000000u;
    if (base && alt - base < 0xA00u && !(start - base < 0xA00u)) start = alt;
    return (const int16_t*)stfx_audio_addr(start);
}

// The AI DMA INTERRUPT as the main thread delivers it (vi_shim.c:163, once per retrace, 2026-09-27): one callback
// per DMA period elapsed since the last delivery, the period from the game's own AIInitDMA length (0x280 bytes
// at 32 kHz = 5 ms), counted on the game's clock when it is virtual or paced (run 114), capped at 64 a delivery.
// The THP movie's clock is this interrupt. Off with the marker ux0:data/STFX00001/NOAITICK.
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#else
unsigned long long stfx_host_now_us;
#endif
unsigned ai_ticks_dropped;
#if defined(__vita__)
void os_irq_enter(void);                              // hal/os_threads.c:195 (strong, as in the old shim)
void os_irq_exit(void);
#define IRQ_ENTER() os_irq_enter()
#define IRQ_EXIT()  os_irq_exit()
#else
#define IRQ_ENTER() ((void)0)                         /* host test: no scheduler */
#define IRQ_EXIT()  ((void)0)
#endif
static unsigned long long gDeliverLast;
// 1 = the callback about to be delivered is FOREIGN (not MusyX's) and gets its own interrupt window (see the header).
static int foreign_window(AIDCallback cb) { return cb && !is_sal(cb) && !stfx_movie_aiwin_off(); }
unsigned ai_foreign_windows;               // interrupt windows opened for single foreign callbacks
void ai_shim_deliver(void) {
    { extern void stfx_audio_lock_leak_check(void); stfx_audio_lock_leak_check(); }   /* run 203: a game-side hold still there at a retrace is leaked */
    static int off = -1;
    unsigned long long now, period;
#define last gDeliverLast
    unsigned n;
    stfx_audio_main_poll();                           // deferred [audio] messages, the test FX
#if defined(__vita__)
    now = sceKernelGetProcessTimeWide();
    {   extern long long os_virtual_now_us(void) __attribute__((weak));
        long long v = os_virtual_now_us ? os_virtual_now_us() : -1;
        /* Run 211: a FOREIGN callback (the attract movie's) is clocked by REAL time.  The paced OS clock advances at most
           fixed_clock.txt's 4 retraces a frame, and the title's frames run 6+: run 210 delivered 72% of real time's
           callbacks, so the movie's sound had a 340 ms hole 0.8 times a second.  The AI DMA is a hardware clock.
           ux0:data/stairfax/movie_realclock_off.txt 1 = the paced clock again. */
        static int rcOff = -1, wasForeign = 0; int fw;
        if (rcOff < 0) { char c = 0; SceUID f = sceIoOpen("ux0:data/stairfax/movie_realclock_off.txt", SCE_O_RDONLY, 0); if (f >= 0) { sceIoRead(f, &c, 1); sceIoClose(f); } rcOff = c == '1'; }
        fw = !rcOff && gAiCb && foreign_window(gAiCb);
        if (fw != wasForeign) { wasForeign = fw; gDeliverLast = 0; }   /* the two clocks have different origins */
        if (v >= 0 && !fw) now = (unsigned long long)v;
    }
    if (off < 0) {
        SceUID f = sceIoOpen("ux0:data/STFX00001/NOAITICK", SCE_O_RDONLY, 0);
        off = f >= 0;
        if (f >= 0) sceIoClose(f);
    }
#else
    now = stfx_host_now_us; off = 0;
#endif
    if (off || !gAiRunning || !gAiCb) { last = now; return; }
    period = (unsigned long long)((gAiLen ? gAiLen : 0x280) / 4) * 1000000u / (gAiRate ? gAiRate : 32000);
    if (!period) period = 5000;
    if (!last) last = now;
    n = (unsigned)((now - last) / period);
    last += (unsigned long long)n * period;
    if (n > 64) { ai_ticks_dropped += n - 64; n = 64; last = now; }

    if (!stfx_audio_new_dsp()) {                      // OFF / DATA / TONE: exactly the old delivery, unless the callback is foreign
        if (foreign_window(gAiCb)) {                  // the movie's: it is the movie's clock at every level, heard or not
            while (n--) { ++ai_foreign_windows; IRQ_ENTER(); ai_shim_tick(); IRQ_EXIT(); }
            return;
        }
        IRQ_ENTER();
        while (n--) ai_shim_tick();
        IRQ_EXIT();
        return;
    }
    if (stfx_audio_level() == STFX_AUDIO_FULL && thread_owned()) {   // the audio thread ticks the synth
        if (gSynthUnder) {                            // a foreign callback over it: delivered HERE (it uses the baton's message queues),
            unsigned k = n;                           // at the game clock's rate as before; it finds the stand-in, not the synth
            int win = foreign_window(gAiCb);          // its own interrupt window per callback (movie_aiwin_off.txt: one for all)
            if (!win) IRQ_ENTER();
            while (k--) {
                AIDCallback cb = gAiCb;
                if (!cb) break;
                ++ai_foreign_deliveries;
                if (win) { ++ai_foreign_windows; IRQ_ENTER(); }
                cb();
                if (win) IRQ_EXIT();                  // the movie's audio decode thread refills HERE, before the next callback
                stfx_audio_push(ai_buffer(), (gAiLen ? gAiLen : 0x280) / 4);   // ADDED to the synth's chunk by the output thread
            }
            if (!win) IRQ_EXIT();
        }
        if (stfx_audio_game_clock() && n) {
            unsigned g = __atomic_load_n(&gGrants, __ATOMIC_RELAXED);
            if (g + n > 64) { ai_grants_dropped += g + n - 64; n = 64 - (g > 64 ? 64 : g); }
            __atomic_add_fetch(&gGrants, n, __ATOMIC_RELAXED);
        }
        return;
    }
    // RETRACE, or a foreign callback (the movie's) at FULL: tick here, mix here, queue the chunk for the output thread.
    // No lock: the audio thread ticks only while the callback is salCallback, and callbacks are changed under the lock.
    IRQ_ENTER();
    while (n--) {
        u32 list = 0;
        if (!tick_begin(&list, gAiCb)) continue;
        ++ai_ticks_retrace;
        if (list) { run_pass(list); stfx_audio_dsp_release(); }
        stfx_audio_push(ai_buffer(), (gAiLen ? gAiLen : 0x280) / 4);
    }
    IRQ_EXIT();
#undef last
}

// AXFX standard reverb: ReverbSTDCreate is the game's C (dolphin/axfx/reverb_std_create.c, linked), but the wet
// path -- HandleReverb, under ReverbSTDCallback -- is Gekko paired-single ASM in both this decomp and dolsdk2004.
// The callback is handed the aux-A SEND of the tick before (hw_dspctrl.c:1084-1088) and whatever it leaves there
// the DSP adds to the main mix at unity one tick later (command 4, hw_dspctrl.c:899-904; dspmix.c:319-327). Left
// untouched, every reverb send would come back as a dry copy 5-10 ms late: a comb filter, not a reverb. So until
// HandleReverb is ported the aux return is SILENCE. (audio_reverb.txt '1' keeps the dry copy, to hear the difference.)
unsigned reverb_dry_calls, reverb_zeroed;
void ReverbSTDCallback(s32* left, s32* right, s32* surround, void* rv) {
    (void)rv; ++reverb_dry_calls;
    if (!stfx_audio_new_dsp() || stfx_audio_reverb_dry()) return;
    if (left)     memset(left, 0, STFX_AUDIO_CHUNK * sizeof(s32));
    if (right)    memset(right, 0, STFX_AUDIO_CHUNK * sizeof(s32));
    if (surround) memset(surround, 0, STFX_AUDIO_CHUNK * sizeof(s32));
    ++reverb_zeroed;
}

// Kept from the old shim: the count of group pushes REFUSED because the level is OFF (stfx_audio_group_gate).
// The stub sndPushGroup itself is gone: musyx/runtime/snd_groups.c:216 links.
unsigned snd_groups_declined;

#ifndef __vita__
void ai_shim_host_reset(void) {
    gTask = NULL; gAiCb = NULL; gAiStart = gAiLen = 0; gAiRate = 32000; gAiRunning = 0;
    gSynthUnder = 0; gSynthStart = gSynthLen = 0; ai_foreign_deliveries = ai_standin_calls = ai_foreign_installs = 0;
    gMailExpectList = 0; gPassList = 0; gResumeOwed = 0; gGrants = 0;
    dsp_mails = dsp_tasks = ai_ticks = dsp_passes = dsp_resumes = dsp_resume_reentries = dsp_mail_unknown = dsp_pass_overrun = 0;
    ai_ticks_thread = ai_ticks_retrace = ai_ticks_dropped = 0; ai_foreign_windows = 0;
    stfx_host_now_us = 0; gDeliverLast = 0;
}
#endif
