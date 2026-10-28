/* stfx_audio.c -- the Vita audio backend of Stairfax (stage 2).  Contract: stfx_audio.h.  Decisions: NOTES.md.
 *
 * NOT RUN ON A CONSOLE.  Compiled for the Vita to an object (symbols listed in NOTES.md) and exercised on the
 * host through test_stfx_audio.c with the output call left out.
 *
 * What lives here:
 *   - the level (markers read once, on the first call, which comes from a game thread);
 *   - the lock between the game's MusyX calls and the synth tick, and the "DSP busy" barrier;
 *   - the software DSP pass: dspmix_run over the command list MusyX built, ARAM = ar_shim's buffer;
 *   - the output thread: 320-frame grains at 32 kHz into a SceAudioOut BGM port; each grain is two DSP frames,
 *     each obtained from ai_shim_thread_tick() (the synth tick, FULL) or from the ring (ticks made on main);
 *   - counters as plain globals, and one report function.  The audio thread never prints.
 *
 * Threads: "main" below means any GAME thread (the baton scheduler of hal/os_threads.c runs one at a time).
 */
#include "stfx_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__has_include) && __has_include("dspmix.h")
#include "dspmix.h"            /* installed next to this file in src/port/src/hal */
#else
#include "../dspmix.h"         /* projects/stairfax-vita/host/audio/vita -> host/audio */
#endif
#if defined(__has_include) && __has_include("musyx_endian.h")
#include "musyx_endian.h"
#else
#include "../musyx_endian.h"
#endif
#if defined(__has_include) && __has_include("audio_tables_endian.h")
#include "audio_tables_endian.h"
#else
#include "tables/audio_tables_endian.h"
#endif

#if defined(__vita__)
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#define AUD_NOW() sceKernelGetProcessTimeWide()
#else
#define AUD_NOW() 0ull
#endif

#define AUD_DIR        "ux0:data/stairfax/"
#define AUD_STACK      0x40000u            /* 256 KB, as the OSThreads get (os_threads.c:269): the MusyX tick runs on it */
#define AUD_RING       32768u              /* frames, power of two: 1 s (was 8192 to run 209: a 150 ms prime + one long frame overflowed/ran dry 1.5 times a second) of ticks made on the main thread */
#define AUD_PRIME_DFLT 1920u               /* frames the ring must hold before it is drained again (60 ms) */
#define AUD_SWAPPED_MAX 16

/* ---- counters: plain globals, read by stfx_audio_report() and by anything that declares them ---------------- */
unsigned aud_level;                  /* effective level (STFX_AUDIO_*) */
unsigned aud_frames;                 /* DSP frames mixed (160 samples each) */
unsigned aud_grains;                 /* grains handed to sceAudioOutOutput */
unsigned aud_underruns;              /* the ring ran dry while it was being played (ticks made on main fell behind) */
unsigned aud_silent_chunks;          /* thread mode: a chunk with no tick behind it (AI stopped, lock missed, parked) */
unsigned aud_lock_misses;            /* audio side: the lock was not free within its wait */
unsigned aud_lock_game_timeouts;     /* game side: 2 s without the lock -> the lock is abandoned (see aud_lock_broken) */
unsigned aud_lock_broken;            /* 1 = the game side gave up on the lock; the audio thread stops ticking */
unsigned aud_unlock_unowned;         /* unlock by a side that does not hold it (sndEnd before the first sndBegin) */
unsigned aud_dsp_wait_timeouts;      /* 1 s waiting for a DSP pass to end */
unsigned aud_late_grains;            /* a grain whose two ticks + passes took longer than the grain (10 ms) */
unsigned aud_work_us_max;            /* longest grain's work, microseconds */
unsigned aud_pass_us_max;            /* longest single DSP pass, microseconds */
unsigned aud_pass_us_total;          /* all DSP passes, microseconds (wraps after 71 minutes of mixing) */
unsigned aud_voices_peak;            /* most voices rendered in one DSP frame */
unsigned aud_voices_last;
unsigned aud_groups_pushed;          /* sndPushGroup returned non-zero */
unsigned aud_groups_failed;          /* the real sndPushGroup ran and returned 0 */
unsigned aud_groups_refused;         /* not handed to MusyX (OFF, or a buffer that was not swapped) */
unsigned aud_swap_ok;                /* files swapped to host order */
unsigned aud_swap_err;               /* files the swapper refused (left big-endian) */
unsigned aud_table_ok;               /* game tables (Sfx/Music/Streams.bin) swapped */
unsigned aud_table_err;              /* game tables refused: left big-endian, the game WILL read garbage from them */
unsigned aud_songs_refused;
unsigned aud_stack_test;             /* 0 thread not started; 1 = the audio thread recognises itself by its stack (lock side, AIInitDMA owner, DC guard); 2 = it does NOT: the stack-window assumption is wrong on this console */
unsigned aud_ring_added;            /* frames of a foreign callback's audio ADDED to the synth's (thread mode) */
unsigned aud_foreign_dry;            /* thread mode: the foreign callback's ring ran dry while it was being added */
unsigned aud_ring_overflow;          /* frames dropped: the ring was full */
unsigned aud_ring_peak;              /* most frames ever waiting in the ring */
unsigned aud_out_errors;             /* sceAudioOutOutput < 0 */
unsigned aud_bad_list;               /* a command list with no output opcode, or an address that did not translate */
unsigned aud_pass_nomix;             /* a pass asked for with no ARAM (ar_shim_base missing or NULL) */
unsigned aud_mix_oob;                /* dspmix: sample fetches outside ARAM */
unsigned aud_mix_badop;              /* dspmix: unknown opcodes */
unsigned aud_mix_itd;                /* dspmix: voices that asked for ITD (not implemented) */
unsigned aud_coef_loaded;            /* 1 = dsp_coef.bin found: polyphase resampler; 0 = linear */
unsigned aud_testfx_fired;
unsigned aud_testfx_last;            /* the last handle sndFXStartEx returned to the test */
unsigned aud_thread_state;           /* 0 none, 1 running, 2 asked to stop, 3 stopped, 4 did not stop in time */
int      aud_port = -1;              /* the SceAudioOut port, or the error it returned */
unsigned aud_gain_pct = 100;         /* audio_gain.txt */
static unsigned gTeeN; static volatile unsigned gTeeIdx; static int16_t* gTeeBuf;   /* audio_tee.txt N (run 208): a RING of the last N grains, the port still plays; written at shutdown (QUIT) */
static unsigned gDumpN, gDumpFrom = 3000; static volatile unsigned gDumpHave; static int16_t* gDumpBuf; static int gDumpWritten;   /* audio_dump.txt (run 204) */
unsigned aud_prime_frames = AUD_PRIME_DFLT;
unsigned aud_depop_ramps;            /* dspmix: studio buses started from a non-zero depop value (a voice was cut off) */
unsigned aud_depop_peak;             /* dspmix: largest |depop start| ever: normal is tens..thousands; millions = the SPB word-order defect (audio_spb_old.txt) */
unsigned aud_out_clamped;            /* dspmix: output samples that hit the 16-bit clamp */
unsigned aud_spb_old;                /* audio_spb_old.txt '1': read the SPB as two u16 words (the pre-2026-10-10 defect), to watch it fail */
unsigned aud_pbdump_frames;          /* audio_pbdump.txt: DSP frames captured so far (PBs before and after the pass, list, ARAM bytes, output) */
unsigned aud_pbdump_truncated;       /* voices in a captured frame beyond the per-frame voice limit (not captured) */


/* ---- THE ATTRACT MOVIE (2026-10-11, after run 208; NOT RUN ON A CONSOLE) ----------------------------------------
 * Counters written by the hooks in the game's THP player (reapply_hooks.py "THE ATTRACT MOVIE IN REAL TIME") and read
 * by the [movie] lines of stfx_audio_report().  Plain unsigned, each written by one baton thread at a time. */
unsigned stfx_mv_read;               /* THP frames the reader thread read (THPRead.c) */
unsigned stfx_mv_skipped;            /* frames the video decoder freed WITHOUT decoding (THPVideoDecode.c, the skip arm) */
unsigned stfx_mv_aframes;            /* THP audio frames decoded (dll_3b.c) */
unsigned stfx_mv_asamples;           /* ... and the stereo frames they held */
unsigned stfx_mv_mixed;              /* stereo frames of movie audio the AI callback mixed out (n_options.c): 32000 a second = real time */
unsigned stfx_mv_starved;            /* AI callbacks that found NO decoded audio and filled the rest with silence */
unsigned stfx_mv_starved_frames;     /* ... and the stereo frames of silence that made */
unsigned aud_movie_prime_frames = 9600;   /* frames a FOREIGN callback's ring must hold before it is added again: 300 ms (4800 to run 209) (audio_movie_prime.txt; movie_prime_off.txt = aud_prime_frames, the old 60 ms) */
static int gMovieSkipOff, gMovieAiWinOff, gMoviePrimeOff;
#if defined(__vita__)
extern unsigned thp_video_decode_calls __attribute__((weak));   /* thp_decode.c */
#define MV_DECODED() (&thp_video_decode_calls ? thp_video_decode_calls : 0u)
#else
#define MV_DECODED() 0u      /* host test: no THP decoder linked (and no ELF weak references on a PE host) */
#endif
/* THE DISC AUDIO STREAM (hal/dtk_stream.c, 2026-10-11; NOT RUN ON A CONSOLE): dialogue and streamed music, added to each
 * grain on the audio thread.  Weak: a link without dvd_shim.c's stream calls does not need the file. */
#if defined(__vita__)
extern void dtk_stream_mix(int16_t* out, unsigned frames, unsigned gain_pct) __attribute__((weak));
extern void dtk_stream_report(void) __attribute__((weak));
extern void dtk_stream_shutdown(void) __attribute__((weak));
#endif

extern unsigned snd_groups_declined;                 /* dsp_ai_shim.c: kept from the old shim (OFF arm) */
extern unsigned char* ar_shim_base(void) __attribute__((weak));   /* ar_shim.c, PATCH.md P2 */
extern uint32_t       ar_shim_size(void) __attribute__((weak));
extern unsigned sndFXStartEx(unsigned short fxId, unsigned char volume, unsigned char pan, unsigned char studio) __attribute__((weak));

/* ---- state -------------------------------------------------------------------------------------------------- */
static volatile int gLevel = -1;
static int gGameClock, gReverbDry, gTestFx = -1;
static int gStarted, gAlive;
static volatile int gQuit;
static volatile uintptr_t gStackTop;            /* the audio thread's entry frame; 0 until it runs */

static DspMix  gMix;
static int     gMixReady;
static int16_t gCoefTab[3 * 512];
static uint8_t gCoefRaw[4096];
static int     gCoefHave;

static int16_t gRing[AUD_RING * 2];
static volatile unsigned gRingW, gRingR;        /* free-running frame counters: one writer (main), one reader (audio) */
static int gRingPrimed;

static int16_t gSine[256];
static unsigned gTonePhase, gToneFrames;

static const void* gSwapped[AUD_SWAPPED_MAX];   /* buffers the swapper turned to host order */
static unsigned gSwappedN;
static int gWadSwapped;
static unsigned gRetraces, gLastPushRetrace, gTestFxNext;
static int gSaidBroken, gSaidStart;
static void pbd_alloc(unsigned n, unsigned from, unsigned frames);   /* audio_pbdump.txt, defined with the DSP pass */

#ifndef __vita__
static StfxHostXlat gHostXl; static void* gHostXlUser;
static uint8_t* gHostAram; static uint32_t gHostAramSize;
static int gHostLevel = STFX_AUDIO_FULL;
#endif

/* ---- addresses ---------------------------------------------------------------------------------------------- */
void* stfx_audio_addr(uint32_t a)
{
#ifndef __vita__
    if (gHostXl) return gHostXl(a, gHostXlUser);
#endif
    return (void*)(uintptr_t)a;
}
#ifndef __vita__
static void* host_mix_xlat(uint32_t a, void* u) { (void)u; return stfx_audio_addr(a); }
#endif

/* ---- the level ---------------------------------------------------------------------------------------------- */
#if defined(__vita__)
static int read_marker(const char* name, char* out, int n)   /* game thread only */
{
    char path[96]; FILE* f; int got;
    snprintf(path, sizeof path, AUD_DIR "%s", name);
    f = fopen(path, "rb");
    if (!f) return -1;
    got = (int)fread(out, 1, (size_t)(n - 1), f);
    fclose(f);
    if (got < 0) got = 0;
    out[got] = 0;
    return got;
}
static SceUID gLockSema = -1, gDspSema = -1;
#endif

static int level_init(void)
{
    int level = STFX_AUDIO_FULL, i;
#if defined(__vita__)
    char b[32];
    if (read_marker("audio_off.txt", b, sizeof b) > 0 && b[0] == '1') level = STFX_AUDIO_OFF;
    else if (read_marker("audio_mode.txt", b, sizeof b) > 0) {
        if (b[0] == 'A' || b[0] == 'a') level = STFX_AUDIO_DATA;
        else if (b[0] == 'B' || b[0] == 'b') level = STFX_AUDIO_TONE;
        else if (b[0] == 'R' || b[0] == 'r') level = STFX_AUDIO_RETRACE;
    }
    if (read_marker("audio_clock.txt", b, sizeof b) > 0 && b[0] == '1') gGameClock = 1;
    if (read_marker("audio_reverb.txt", b, sizeof b) > 0 && b[0] == '1') gReverbDry = 1;
    if (read_marker("audio_dump.txt", b, sizeof b) > 0) { int n = atoi(b); if (n > 0 && n <= 6000) { gDumpN = (unsigned)n; gDumpBuf = (int16_t*)malloc((size_t)n * STFX_AUDIO_GRAIN * 2 * sizeof(int16_t)); if (!gDumpBuf) gDumpN = 0; } }
    if (read_marker("audio_tee.txt", b, sizeof b) > 0) { int n = atoi(b); if (n > 0 && n <= 6000) { gTeeBuf = (int16_t*)malloc((size_t)n * STFX_AUDIO_GRAIN * 2 * sizeof(int16_t)); if (gTeeBuf) gTeeN = (unsigned)n; } }
    if (read_marker("audio_dump_from.txt", b, sizeof b) > 0) { int n = atoi(b); if (n >= 0) gDumpFrom = (unsigned)n; }
    if (read_marker("audio_spb_old.txt", b, sizeof b) > 0 && b[0] == '1') aud_spb_old = 1;
    if (read_marker("audio_pbdump.txt", b, sizeof b) > 0) { unsigned n = 0, from = 0, frames = 8; sscanf(b, "%u %u %u", &n, &from, &frames); pbd_alloc(n, from, frames); }
    if (read_marker("audio_gain.txt", b, sizeof b) > 0) { int g = atoi(b); if (g >= 0 && g <= 400) aud_gain_pct = (unsigned)g; }
    if (read_marker("audio_testfx.txt", b, sizeof b) > 0) gTestFx = (int)strtol(b, NULL, 0);
    if (read_marker("audio_prime.txt", b, sizeof b) > 0) { int p = atoi(b); if (p >= 160 && p <= (int)AUD_RING - 320) aud_prime_frames = (unsigned)p; }
    /* the attract movie in real time (2026-10-11): three break arms, each '1' = the behaviour of run 208 */
    if (read_marker("movie_skip_off.txt", b, sizeof b) > 0 && b[0] == '1') gMovieSkipOff = 1;     /* video decoder: the decompiled loop, no frame is dropped */
    if (read_marker("movie_aiwin_off.txt", b, sizeof b) > 0 && b[0] == '1') gMovieAiWinOff = 1;   /* a retrace's AI callbacks in ONE interrupt window */
    if (read_marker("audio_movie_prime.txt", b, sizeof b) > 0) { int p = atoi(b); if (p >= 160 && p <= (int)AUD_RING - 320) aud_movie_prime_frames = (unsigned)p; }
    if (read_marker("movie_prime_off.txt", b, sizeof b) > 0 && b[0] == '1') { gMoviePrimeOff = 1; aud_movie_prime_frames = aud_prime_frames; }   /* the movie's ring primed at 60 ms */
    {   FILE* f = fopen(AUD_DIR "dsp_coef.bin", "rb");
        if (f) { gCoefHave = fread(gCoefRaw, 1, sizeof gCoefRaw, f) == sizeof gCoefRaw; fclose(f); }
    }
    if (level >= STFX_AUDIO_RETRACE) {
        gDspSema = sceKernelCreateSema("stfx_aud_dsp", 0, 1, 1, NULL);
        if (level == STFX_AUDIO_FULL) gLockSema = sceKernelCreateSema("stfx_aud_lock", 0, 1, 1, NULL);
        if (gDspSema < 0 || (level == STFX_AUDIO_FULL && gLockSema < 0)) {
            printf("[audio] semaphores NOT created (%08x %08x): level DATA (silent)\n", (unsigned)gDspSema, (unsigned)gLockSema);
            level = STFX_AUDIO_DATA;
        }
    }
#else
    level = gHostLevel;
#endif
    for (i = 0; i < 256; i++) gSine[i] = (int16_t)(8000.0f * sinf((float)i * (6.2831853f / 256.0f)));
    aud_coef_loaded = (unsigned)gCoefHave;
    aud_level = (unsigned)level;
    gLevel = level;
#if defined(__vita__)
    {   static const char* const nm[] = { "OFF (audio_off.txt): as before stage 2", "DATA: sound data swapped and pushed, synth as before",
                                          "TONE: DATA + output thread with a test tone", "RETRACE: mixer on, ticks at retraces on the main thread",
                                          "FULL: the audio thread owns the synth tick" };
        printf("[movie] video frame skip %s | AI callbacks %s | the movie's ring is primed at %u frames (%u ms)%s\n",
               gMovieSkipOff ? "OFF (movie_skip_off.txt): the decompiled decoder loop" : "on: the newest frame is decoded, older ones are freed undecoded",
               gMovieAiWinOff ? "all of a retrace's in ONE interrupt window (movie_aiwin_off.txt)" : "one interrupt window each",
               aud_movie_prime_frames, aud_movie_prime_frames * 1000u / STFX_AUDIO_RATE, gMoviePrimeOff ? " (movie_prime_off.txt)" : "");
        printf("[audio] level %d = %s | clock %s | reverb %s | gain %u%% | resampler %s | test fx %d\n", level, nm[level],
               gGameClock ? "GAME (audio_clock.txt)" : "real time", gReverbDry ? "dry send left in (audio_reverb.txt)" : "aux return zeroed",
               aud_gain_pct, gCoefHave ? "polyphase (dsp_coef.bin)" : "linear (no dsp_coef.bin)", gTestFx);
    }
#endif
    return level;
}

int stfx_audio_level(void)      { int l = gLevel; return l >= 0 ? l : level_init(); }
int stfx_audio_new_dsp(void)    { return stfx_audio_level() >= STFX_AUDIO_RETRACE && gAlive; }   /* no output thread = nobody to play a mix */
int stfx_audio_game_clock(void) { return gGameClock; }
int stfx_audio_reverb_dry(void) { return gReverbDry; }
int stfx_movie_skip_off(void)   { (void)stfx_audio_level(); return gMovieSkipOff; }    /* THPVideoDecode.c's hook (a game thread) */
int stfx_movie_aiwin_off(void)  { (void)stfx_audio_level(); return gMovieAiWinOff; }   /* dsp_ai_shim.c ai_shim_deliver */

/* ---- the lock ----------------------------------------------------------------------------------------------- */
/* Which side is calling: the audio thread is recognised by its stack (no syscall, usable from os_shim.c's cache
 * functions on every call).  The window is [top - (AUD_STACK - 4096), top]: strictly inside the thread's own stack
 * block as long as its entry frame sits within 4 KB of the block's end (NOTES.md "unverified" 9). */
#ifndef __vita__
static int gHostInPump;             /* host test: the pump stands in for the audio thread */
#endif
int stfx_audio_on_audio_thread(void)
{
    volatile char probe;
    uintptr_t top = gStackTop;
#ifndef __vita__
    if (gHostInPump) return 1;
#endif
    /* Run 200 (hardware): the test read 2 at thread start and the synth never ticked (34,889 lock misses). The entry
       function had this one inlined and `probe` sat a few bytes ABOVE `top`, so the unsigned difference wrapped.
       The distance is taken in either direction: anything within the window of the entry frame is this thread. */
    {   const uintptr_t p = (uintptr_t)&probe, d = top > p ? top - p : p - top;
        return top && d < (uintptr_t)(AUD_STACK - 4096u);
    }
}

#if defined(__vita__)
static volatile int gLockOwner;     /* 0 free, 1 audio thread, 2 game side */
static int gLockDepth;              /* touched only by the owner */
unsigned aud_lk_game_take, aud_lk_game_rel, aud_lk_aud_take, aud_lk_aud_rel, aud_lk_noop_take, aud_lk_noop_rel;   /* run 202: where is the imbalance? */
int aud_lk_max_depth;

static int lock_take(unsigned us)
{
    int me;
    SceUInt t = us;
    if (gLevel != STFX_AUDIO_FULL || gLockSema < 0 || aud_lock_broken) { ++aud_lk_noop_take; return 1; }
    me = stfx_audio_on_audio_thread() ? 1 : 2;
    if (gLockOwner == me) { ++gLockDepth; if (gLockDepth > aud_lk_max_depth) aud_lk_max_depth = gLockDepth; if (me == 2) ++aud_lk_game_take; else ++aud_lk_aud_take; return 1; }
    if (sceKernelWaitSema(gLockSema, 1, &t) < 0) return 0;
    gLockOwner = me; gLockDepth = 1;
    if (me == 2) ++aud_lk_game_take; else ++aud_lk_aud_take;
    return 1;
}
int stfx_audio_lock(void)
{
    if (lock_take(2000000u)) return 1;
    /* Two seconds: the audio thread is stuck inside a tick.  A frozen game with no log line is the worse outcome,
       so the game goes on WITHOUT the lock and the audio thread is told to stop ticking; main reports it. */
    ++aud_lock_game_timeouts; aud_lock_broken = 1;
    return 0;
}
int stfx_audio_lock_timed(unsigned us)
{
    if (aud_lock_broken) return 0;
    if (lock_take(us)) return 1;
    ++aud_lock_misses;
    return 0;
}
void stfx_audio_unlock(void)
{
    int me;
    if (gLevel != STFX_AUDIO_FULL || gLockSema < 0 || aud_lock_broken) { ++aud_lk_noop_rel; return; }
    me = stfx_audio_on_audio_thread() ? 1 : 2;
    if (gLockOwner != me) { ++aud_unlock_unowned; return; }
    if (me == 2) ++aud_lk_game_rel; else ++aud_lk_aud_rel;
    if (--gLockDepth == 0) { gLockOwner = 0; sceKernelSignalSema(gLockSema, 1); }
}
/* THE LEAKED GAME-SIDE LOCK (run 202, hardware): owner 2, depth 1 for the whole run; game takes 2021, releases 2020; audio
   takes 0 -- one early take is never matched (not found by reading), so the audio thread never ticked the synth. The main
   thread at a retrace is between frames and no game thread can be inside a MusyX section there (they are not blocking
   sections and the baton only changes hands at blocking calls), so a lock the game side still holds there is leaked: it is
   released and counted. A count that keeps rising means the leak recurs. */
unsigned aud_lk_forced;
void stfx_audio_lock_leak_check(void)
{
    if (gLevel != STFX_AUDIO_FULL || gLockSema < 0 || aud_lock_broken) return;
    if (gLockOwner == 2 && !stfx_audio_on_audio_thread()) { gLockDepth = 0; gLockOwner = 0; sceKernelSignalSema(gLockSema, 1); ++aud_lk_forced; }
}
int stfx_audio_dsp_acquire(void)
{
    SceUInt t = 1000000u;
    if (gDspSema < 0) return 1;
    if (sceKernelWaitSema(gDspSema, 1, &t) < 0) { ++aud_dsp_wait_timeouts; return 0; }
    return 1;
}
void stfx_audio_dsp_release(void) { if (gDspSema >= 0) sceKernelSignalSema(gDspSema, 1); }
#else
int  stfx_audio_lock(void) { return 1; }
int  stfx_audio_lock_timed(unsigned us) { (void)us; return 1; }
void stfx_audio_unlock(void) { }
unsigned aud_lk_forced;
void stfx_audio_lock_leak_check(void) { }
int  stfx_audio_dsp_acquire(void) { return 1; }
void stfx_audio_dsp_release(void) { }
#endif

/* ---- the DSP pass ------------------------------------------------------------------------------------------- */
static int mix_init(void)
{
    DspMixCfg c;
    memset(&c, 0, sizeof c);
#if defined(__vita__)
    if (!ar_shim_base || !ar_shim_size) return 0;
    c.aram = ar_shim_base(); c.aram_size = ar_shim_size();
#else
    c.aram = gHostAram; c.aram_size = gHostAramSize;
    if (gHostXl) c.xlat = host_mix_xlat;
#endif
    if (!c.aram || !c.aram_size) return 0;
    /* The runtime stores the SPB's depop start values as native s32 (hw_dspctrl.c:81 through the casts at :935-943),
       not as hi/lo u16 pairs: see dspmix.h.  Until 2026-10-10 the mixer read hi:lo, i.e. v << 16 on this machine. */
    c.spb_native32 = aud_spb_old ? 0 : 1;
    dspmix_init(&gMix, &c);
    if (gCoefHave) dspmix_set_coefs_be(&gMix, gCoefRaw, gCoefTab);
    gMixReady = 1;
    return 1;
}

#ifndef STFX_AUDIO_FAULT
#define STFX_AUDIO_FAULT 0   /* test-only fault 2: take opcode 14's SURROUND address for the output chunk; test_stfx_audio MUST fail */
#endif
/* Where the list's output opcode (14) writes its 160 stereo frames.  Opcode sizes: DESIGN.md section 4. */
static const int16_t* list_dest(uint32_t list)
{
    const uint16_t* p = (const uint16_t*)stfx_audio_addr(list);
    int guard;
    for (guard = 0; guard < 4096 && p; guard++) {
        switch (p[0]) {
        case 0: case 2: case 6: case 9: case 17: p += 3; break;
        case 1: p += 6; break;
        case 3: p += 1; break;
        case 4: case 5: case 16: p += 5; break;
        case 13: p = (const uint16_t*)stfx_audio_addr(((uint32_t)p[1] << 16) | p[2]); break;
        case 14: return (const int16_t*)stfx_audio_addr(STFX_AUDIO_FAULT == 2 ? (((uint32_t)p[1] << 16) | p[2]) : (((uint32_t)p[3] << 16) | p[4]));
        default: return NULL;
        }
    }
    return NULL;
}

/* ---- audio_pbdump.txt: what the runtime REALLY hands the mixer (2026-10-10, after run 205) ------------------------
 * "N [FROM [FRAMES]]": from DSP frame FROM on (aud_frames), starting at the first frame whose voice chain is not
 * empty, FRAMES consecutive DSP frames (default 8, max 64) are captured with up to N voices each (max 32):
 * the command list words, the studio's SPB, every PB BEFORE the pass and AFTER it, its patch buffer, 384 bytes of
 * ARAM at its current address and 64 at its loop address, and the 160 stereo frames the pass wrote.  Captured on
 * the thread that runs the pass, into memory allocated at start-up: no file I/O, no printf there.  The main thread
 * writes ux0:data/stairfax/audio_pbdump.bin from stfx_audio_report().  host/audio/diag/pbdump_run.c reads it,
 * prints every field, re-runs dspmix on exactly these bytes and compares with what the console's pass produced.
 * Layout (little-endian, fixed-size records): PbdHead, then FRAMES x (PbdFrame + N x PbdVoice). */
#define PBD_MAXV   32u
#define PBD_WORDS  256u
#define PBD_CUR    384u
#define PBD_LOOP   64u
typedef struct { char magic[8]; uint32_t maxv, frames, frame_size, voice_size, aram_size, from, coef_loaded, level, spb_native32; } PbdHead;
typedef struct { uint32_t dsp_frame, grain, list_addr, nwords, spb_addr, first_pb, dest_addr, nvoices, nvoices_total, flags;
                 uint16_t words[PBD_WORDS]; uint8_t spb[0x38]; int16_t out[STFX_AUDIO_CHUNK * 2]; } PbdFrame;
typedef struct { uint32_t pb_addr, patch_addr, cur_base, loop_base; uint8_t pre[0xBC], post[0xBC], patch[0x80], aram_cur[PBD_CUR], aram_loop[PBD_LOOP]; } PbdVoice;
static unsigned gPbdN, gPbdFrom, gPbdFrames = 8, gPbdStarted;
static volatile unsigned gPbdHave;
static uint8_t* gPbdBuf;
static int gPbdWritten;

static void pbd_alloc(unsigned n, unsigned from, unsigned frames)       /* game thread, at start-up */
{
    if (n > PBD_MAXV) n = PBD_MAXV;
    if (frames < 1) frames = 1;
    if (frames > 64) frames = 64;
    free(gPbdBuf); gPbdBuf = NULL; gPbdN = 0; gPbdHave = 0; gPbdStarted = 0; gPbdWritten = 0; aud_pbdump_frames = aud_pbdump_truncated = 0;
    if (!n) return;
    gPbdBuf = (uint8_t*)calloc(frames, sizeof(PbdFrame) + (size_t)n * sizeof(PbdVoice));
    if (!gPbdBuf) return;
    gPbdN = n; gPbdFrom = from; gPbdFrames = frames;
}
static uint32_t pbd_u32(const uint16_t* w) { return ((uint32_t)w[0] << 16) | w[1]; }
static uint32_t pbd_byte_base(uint16_t format, uint32_t addr)   /* PB address -> ARAM byte offset (ADPCM: the frame's header byte) */
{
    if (format == 0x0A) return addr * 2u;        /* PCM16: words */
    if (format == 0x19) return addr;             /* PCM8 */
    return (addr >> 1) & ~7u;                    /* ADPCM: nibbles, 8-byte frames */
}
static void pbd_aram(uint8_t* dst, uint32_t base, unsigned n)
{
    const uint8_t* a = gMix.cfg.aram; uint32_t size = gMix.cfg.aram_size;
    if (a && base < size) memcpy(dst, a + base, size - base < n ? size - base : n);
}
/* Before the pass.  Returns the frame record to finish after the pass, or NULL. */
static PbdFrame* pbd_before(uint32_t list)
{
    PbdFrame* f; PbdVoice* v; const uint16_t* p; uint32_t a; unsigned nv = 0, guard;
    if (!gPbdN || gPbdHave >= gPbdFrames || aud_frames < gPbdFrom) return NULL;
    f = (PbdFrame*)(gPbdBuf + (size_t)gPbdHave * (sizeof(PbdFrame) + (size_t)gPbdN * sizeof(PbdVoice)));
    v = (PbdVoice*)(f + 1);
    memset(f, 0, sizeof *f + (size_t)gPbdN * sizeof *v);
    f->dsp_frame = aud_frames; f->grain = aud_grains; f->list_addr = list;
    p = (const uint16_t*)stfx_audio_addr(list);
    for (guard = 0; guard < 4096 && p && f->nwords + 6 <= PBD_WORDS; guard++) {     /* the list, flattened over its continuation jumps */
        unsigned op = p[0], len = (op == 1) ? 6 : (op == 3 || op == 15) ? 1 : (op == 4 || op == 5 || op == 16 || op == 14) ? 5 : (op == 13) ? 4 : 3, i;
        if (op > 17 || op == 7 || op == 8 || op == 10 || op == 11 || op == 12) { f->flags |= 1; break; }
        for (i = 0; i < len; i++) f->words[f->nwords++] = p[i];
        if (op == 0 && !f->spb_addr) { const void* spb = stfx_audio_addr(f->spb_addr = pbd_u32(p + 1)); if (spb) memcpy(f->spb, spb, 0x36); }
        if (op == 2 && !f->first_pb) f->first_pb = pbd_u32(p + 1);
        if (op == 14) f->dest_addr = pbd_u32(p + 3);
        if (op == 15) break;
        p = (op == 13) ? (const uint16_t*)stfx_audio_addr(pbd_u32(p + 1)) : p + len;
    }
    for (a = f->first_pb, guard = 0; a && guard < 256; guard++) {
        const DspPB* pb = (const DspPB*)stfx_audio_addr(a);
        if (!pb) break;
        if (nv < gPbdN) {
            const void* patch;
            v[nv].pb_addr = a; memcpy(v[nv].pre, pb, 0xBC);
            v[nv].patch_addr = ((uint32_t)pb->updDataHi << 16) | pb->updDataLo;
            patch = stfx_audio_addr(v[nv].patch_addr); if (patch) memcpy(v[nv].patch, patch, 0x80);
            v[nv].cur_base = pbd_byte_base(pb->format, ((uint32_t)pb->curAddrHi << 16) | pb->curAddrLo);
            v[nv].loop_base = pbd_byte_base(pb->format, ((uint32_t)pb->loopAddrHi << 16) | pb->loopAddrLo);
            pbd_aram(v[nv].aram_cur, v[nv].cur_base, PBD_CUR); pbd_aram(v[nv].aram_loop, v[nv].loop_base, PBD_LOOP);
            ++nv;
        } else ++aud_pbdump_truncated;
        ++f->nvoices_total;
        a = ((uint32_t)pb->nextHi << 16) | pb->nextLo;
    }
    f->nvoices = nv;
    if (!gPbdStarted && !f->nvoices_total) return NULL;        /* wait for the first frame that has a voice; then take consecutive frames */
    gPbdStarted = 1;
    return f;
}
static void pbd_after(PbdFrame* f, const int16_t* out)
{
    PbdVoice* v = (PbdVoice*)(f + 1); unsigned i;
    for (i = 0; i < f->nvoices; i++) { const void* pb = stfx_audio_addr(v[i].pb_addr); if (pb) memcpy(v[i].post, pb, 0xBC); }
    if (out) memcpy(f->out, out, sizeof f->out);
    aud_pbdump_frames = ++gPbdHave;
}
static int pbd_write(const char* path)          /* game thread */
{
    PbdHead h; FILE* fp = fopen(path, "wb");
    if (!fp) return 0;
    memset(&h, 0, sizeof h); memcpy(h.magic, "STFXPBD1", 8);
    h.maxv = gPbdN; h.frames = gPbdHave; h.frame_size = (uint32_t)sizeof(PbdFrame); h.voice_size = (uint32_t)sizeof(PbdVoice);
    h.aram_size = gMix.cfg.aram_size; h.from = gPbdFrom; h.coef_loaded = gMix.coefs != NULL; h.level = (uint32_t)gLevel; h.spb_native32 = (uint32_t)gMix.cfg.spb_native32;
    fwrite(&h, sizeof h, 1, fp);
    fwrite(gPbdBuf, sizeof(PbdFrame) + (size_t)gPbdN * sizeof(PbdVoice), gPbdHave, fp);
    fclose(fp);
    return 1;
}

/* One DSP frame.  The caller holds the DSP barrier, so gMix has one user.  No lock on MusyX is needed here: the
 * pass touches only what the command list names (PBs, patch buffers, studio buffers, the AI chunk, ARAM), and
 * nothing but salBuildCommandList writes those (NOTES.md section 1, evidence E5). */
const int16_t* stfx_audio_pass(uint32_t list)
{
    const int16_t* dst;
    unsigned long long t0 = AUD_NOW();
    unsigned v0, us;
    if (!gMixReady && !mix_init()) { ++aud_pass_nomix; return NULL; }
    dst = list_dest(list);
    if (!dst) { ++aud_bad_list; return NULL; }
    v0 = gMix.st.voices_mixed;
    {   PbdFrame* pf = gPbdN ? pbd_before(list) : NULL;          /* audio_pbdump.txt: memory only on this thread */
        dspmix_run(&gMix, list);
        if (pf) pbd_after(pf, dst);
    }
    aud_voices_last = gMix.st.voices_mixed - v0;
    if (aud_voices_last > aud_voices_peak) aud_voices_peak = aud_voices_last;
    aud_mix_oob = gMix.st.aram_oob; aud_mix_badop = gMix.st.bad_opcode; aud_mix_itd = gMix.st.itd_ignored;
    aud_depop_ramps = gMix.st.depop_ramps; aud_depop_peak = gMix.st.depop_peak; aud_out_clamped = gMix.st.out_clamped;
    ++aud_frames;
    us = (unsigned)(AUD_NOW() - t0);
    aud_pass_us_total += us;
    if (us > aud_pass_us_max) aud_pass_us_max = us;
    return dst;
}

/* ---- the ring: ticks made on the main thread (retrace mode, and the movie's AI callback) --------------------- */
void stfx_audio_push(const int16_t* s, unsigned frames)
{
    unsigned w = gRingW, r = __atomic_load_n(&gRingR, __ATOMIC_ACQUIRE), used = w - r, i, at;
    if (!s || !frames) return;
    if (frames > AUD_RING - used) { aud_ring_overflow += frames; return; }   /* never block the game thread: drop the newest */
    at = w & (AUD_RING - 1);
    i = AUD_RING - at; if (i > frames) i = frames;
    memcpy(gRing + at * 2, s, (size_t)i * 4);
    if (i < frames) memcpy(gRing, s + i * 2, (size_t)(frames - i) * 4);
    __atomic_store_n(&gRingW, w + frames, __ATOMIC_RELEASE);
    if (used + frames > aud_ring_peak) aud_ring_peak = used + frames;
}
static unsigned ring_level(void) { return __atomic_load_n(&gRingW, __ATOMIC_ACQUIRE) - gRingR; }
static void ring_pop(int16_t* d, unsigned frames)    /* the caller checked the level */
{
    unsigned r = gRingR, at = r & (AUD_RING - 1), i = AUD_RING - at;
    if (i > frames) i = frames;
    memcpy(d, gRing + at * 2, (size_t)i * 4);
    if (i < frames) memcpy(d + i * 2, gRing, (size_t)(frames - i) * 4);
    __atomic_store_n(&gRingR, r + frames, __ATOMIC_RELEASE);
}

/* ---- one 160-frame chunk for the output ------------------------------------------------------------------- */
static void put_gain(int16_t* d, const int16_t* s, unsigned frames)
{
    unsigned i, g = aud_gain_pct;
    if (g == 100) { if (d != s) memcpy(d, s, (size_t)frames * 4); return; }
    for (i = 0; i < frames * 2; i++) {
        int v = (int)s[i] * (int)g / 100;
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
static void tone(int16_t* d)     /* 440 Hz, half a second on, half a second off: a gap or a click is an output fault */
{
    unsigned i;
    for (i = 0; i < STFX_AUDIO_CHUNK; i++) {
        int16_t v = (gToneFrames % 32000u) < 16000u ? gSine[(gTonePhase >> 8) & 255] : 0;
        d[2 * i] = v; d[2 * i + 1] = v;
        gTonePhase += 901u;           /* 440 / 32000 * 65536 */
        ++gToneFrames;
    }
}
/* Thread mode with a foreign callback over the synth: its audio (made on the main thread, in the ring) is ADDED to
 * the chunk, saturating.  dry = the foreign audio drops out, the synth does not.
 * PRIMING (2026-10-11): the ring is filled once per PRESENT and drained in real time, so it has to hold more than the
 * longest frame.  With the movie on, every second frame waits for a video decode: 83-100 ms (run 208's [ft] line), and
 * the old threshold was 60 ms (aud_prime_frames) -- 263,891 dry events in the 11-hour idle run, 6.7 a second.  Now
 * aud_movie_prime_frames, 150 ms.  It is LATENCY, on the movie's audio only; the synth's own chunk is not delayed. */
static void ring_add(int16_t* d)
{
    int16_t t[STFX_AUDIO_CHUNK * 2];
    unsigned have = ring_level(), i;
    if (!gRingPrimed && have >= aud_movie_prime_frames) gRingPrimed = 1;
    if (!gRingPrimed) return;
    if (have < STFX_AUDIO_CHUNK) { ++aud_foreign_dry; gRingPrimed = 0; return; }
    ring_pop(t, STFX_AUDIO_CHUNK); put_gain(t, t, STFX_AUDIO_CHUNK);
    for (i = 0; i < STFX_AUDIO_CHUNK * 2; i++) {
        int v = (int)d[i] + (int)t[i];
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    aud_ring_added += STFX_AUDIO_CHUNK;
}
static void fill_chunk(int16_t* d)
{
    const int16_t* c = NULL;
    int r;
    if (gLevel == STFX_AUDIO_TONE) { tone(d); return; }
    r = aud_lock_broken ? 0 : ai_shim_thread_tick(&c);
    if (r >= 0) {                                       /* the synth is this thread's (or nothing is running) */
        if (r > 0 && c) put_gain(d, c, STFX_AUDIO_CHUNK);
        else { memset(d, 0, STFX_AUDIO_CHUNK * 4); ++aud_silent_chunks; }
        if (ai_shim_foreign_over_synth()) ring_add(d);
        else { gRingPrimed = 0; gRingR = __atomic_load_n(&gRingW, __ATOMIC_ACQUIRE); }   /* anything left from a retrace phase is stale */
        return;
    }
    {   unsigned have = ring_level();                   /* ticks are being made on the main thread */
        if (!gRingPrimed && have >= aud_prime_frames) gRingPrimed = 1;
        if (gRingPrimed && have >= STFX_AUDIO_CHUNK) { ring_pop(d, STFX_AUDIO_CHUNK); put_gain(d, d, STFX_AUDIO_CHUNK); return; }
        if (gRingPrimed) { ++aud_underruns; gRingPrimed = 0; }   /* ran dry: silence until it has refilled */
        memset(d, 0, STFX_AUDIO_CHUNK * 4);
    }
}

/* ---- the output thread -------------------------------------------------------------------------------------- */
#if defined(__vita__)
static SceUID gThread = -1;
static int16_t gGrain[STFX_AUDIO_GRAIN * 2];

static int audio_thread(SceSize args, void* argp)
{
    volatile char top;
    (void)args; (void)argp;
    gStackTop = (uintptr_t)&top;
    aud_stack_test = stfx_audio_on_audio_thread() ? 1 : 2;
    aud_thread_state = 1;
    while (!gQuit) {
        unsigned long long t0 = AUD_NOW();
        unsigned us;
        fill_chunk(gGrain);
        fill_chunk(gGrain + STFX_AUDIO_CHUNK * 2);
        us = (unsigned)(AUD_NOW() - t0);
        if (us > aud_work_us_max) aud_work_us_max = us;
        if (us > 10000u) ++aud_late_grains;
        if (dtk_stream_mix) dtk_stream_mix(gGrain, STFX_AUDIO_GRAIN, aud_gain_pct);   /* the disc audio stream: outside the synth's timing above (its own is dtk_mix_us_max), before the dump and the tee */
        /* Blocks until the port can take the grain: this call is the thread's clock (audioout.h:101). */
        /* THE DUMP (run 204): run 203 was smooth by every counter and, by ear, "sounded like it was going to break my speakers"
           (Tyler). ux0:data/stairfax/audio_dump.txt N: the first N grains from grain gDumpFrom on are kept in RAM and the
           port is fed SILENCE for the whole run; the main thread writes ux0:data/stairfax/audio_dump.raw (s16 LE stereo,
           32 kHz) from stfx_audio_report. Nobody has to listen to a broken stream to find out what is in it. */
        if (gDumpN) {
            if (gDumpBuf && aud_grains >= gDumpFrom && gDumpHave < gDumpN) { memcpy(gDumpBuf + (size_t)gDumpHave * STFX_AUDIO_GRAIN * 2, gGrain, sizeof gGrain); ++gDumpHave; }
            memset(gGrain, 0, sizeof gGrain);
        }
        if (gTeeN) { memcpy(gTeeBuf + (size_t)(gTeeIdx % gTeeN) * STFX_AUDIO_GRAIN * 2, gGrain, sizeof gGrain); ++gTeeIdx; }
        if (sceAudioOutOutput(aud_port, gGrain) < 0) { ++aud_out_errors; sceKernelDelayThread(10000); }
        ++aud_grains;
    }
    gStackTop = 0;
    aud_thread_state = 3;
    return 0;
}

void stfx_audio_start(void)
{
    int level = stfx_audio_level(), vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    if (gStarted) return;
    gStarted = 1;
    if (level < STFX_AUDIO_TONE) return;
    /* BGM port: MAIN only takes 48000 Hz (audioout.h:38); 320 = 5 x 64, the smallest legal grain that is whole DSP frames. */
    aud_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, STFX_AUDIO_GRAIN, STFX_AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (aud_port >= 0) {
        sceAudioOutSetVolume(aud_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
        gThread = sceKernelCreateThread("stfx_audio", audio_thread, 0x10000100, AUD_STACK, 0, 0, NULL);
        if (gThread >= 0 && sceKernelStartThread(gThread, 0, NULL) >= 0) gAlive = 1;
    }
    if (!gAlive) {
        printf("[audio] output NOT started (port %08x thread %08x): level DATA (silent, synth as before)\n", (unsigned)aud_port, (unsigned)gThread);
        if (aud_port >= 0) { sceAudioOutReleasePort(aud_port); aud_port = -2; }
        gLevel = STFX_AUDIO_DATA; aud_level = STFX_AUDIO_DATA;
        return;
    }
    printf("[audio] output: BGM port %d, %d Hz stereo, grain %d frames (10 ms), thread %08x, stack %u KB\n",
           aud_port, STFX_AUDIO_RATE, STFX_AUDIO_GRAIN, (unsigned)gThread, AUD_STACK >> 10);
}

/* The thread is never killed and the port is never released under it: it is asked to stop, and it sees the request
 * within one grain (its longest waits are 8 ms on the lock, 1 s on the DSP barrier and ~10 ms in the output call). */
void stfx_audio_shutdown(void)
{
    int i;
    if (dtk_stream_shutdown) dtk_stream_shutdown();   /* the stream's reader thread; dtk_stream_mix does nothing from here on */
    if (!gAlive) return;
    gAlive = 0;
    aud_thread_state = 2; gQuit = 1;
    for (i = 0; i < 1500 && aud_thread_state != 3; i++) sceKernelDelayThread(1000);
    if (gTeeN && gTeeIdx) {     /* oldest grain first */
        FILE* tf = fopen("ux0:data/stairfax/audio_tee.raw", "wb");
        unsigned have = gTeeIdx < gTeeN ? gTeeIdx : gTeeN, first = gTeeIdx < gTeeN ? 0 : gTeeIdx % gTeeN, k;
        if (tf) { for (k = 0; k < have; k++) fwrite(gTeeBuf + (size_t)((first + k) % gTeeN) * STFX_AUDIO_GRAIN * 2, sizeof(int16_t) * 2 * STFX_AUDIO_GRAIN, 1, tf); fclose(tf); }
        printf("[audio] tee: the last %u grains (ending at grain %u) written to ux0:data/stairfax/audio_tee.raw\n", have, (unsigned)gTeeIdx);
    }
    if (aud_thread_state == 3) { sceKernelDeleteThread(gThread); sceAudioOutReleasePort(aud_port); printf("[audio] output stopped after %u grains\n", aud_grains); }
    else { aud_thread_state = 4; printf("[audio] output thread did NOT stop in 1.5 s: port left open for process exit\n"); }
}
#else
void stfx_audio_start(void) { (void)stfx_audio_level(); gStarted = 1; gAlive = gLevel >= STFX_AUDIO_TONE; }
void stfx_audio_shutdown(void) { gAlive = 0; }
#endif

/* ---- main-thread housekeeping --------------------------------------------------------------------------------- */
void stfx_audio_main_poll(void)
{
    ++gRetraces;
    if (aud_lock_broken && !gSaidBroken) {
        gSaidBroken = 1;
        printf("[audio] LOCK TIMEOUT: a game thread waited 2 s for the audio thread's tick -- lock abandoned, audio thread told to stop ticking (silence from here)\n");
    }
    if (!gSaidStart && aud_frames) { gSaidStart = 1; printf("[audio] first DSP frame mixed (retrace %u)\n", gRetraces); }
    /* audio_testfx.txt: start one known FX through MusyX's own API, 300 retraces after the last group push and every
       300 after.  It proves MusyX -> mixer -> output without the game's trigger tables (which are still big-endian). */
    if (gTestFx >= 0 && sndFXStartEx && gLevel >= STFX_AUDIO_RETRACE && aud_groups_pushed
        && gRetraces - gLastPushRetrace >= 300 && gRetraces >= gTestFxNext) {
        gTestFxNext = gRetraces + 300;
        aud_testfx_last = sndFXStartEx((unsigned short)gTestFx, 127, 64, 0);
        ++aud_testfx_fired;
        printf("[audio] test fx %d -> handle %08x (retrace %u)\n", gTestFx, aud_testfx_last, gRetraces);
    }
}

void stfx_audio_report(void)
{
#if defined(__vita__)
    if (gDumpN && !gDumpWritten && gDumpHave >= gDumpN) {
        FILE* df = fopen("ux0:data/stairfax/audio_dump.raw", "wb");
        if (df) { fwrite(gDumpBuf, sizeof(int16_t) * 2 * STFX_AUDIO_GRAIN, gDumpN, df); fclose(df); }
        gDumpWritten = 1;
        printf("[audio] dump: %u grains from grain %u written to ux0:data/stairfax/audio_dump.raw (the port got silence)\n", gDumpN, gDumpFrom);
    } else if (gDumpN && !gDumpWritten) printf("[audio] dump: %u of %u grains so far (the port gets silence)\n", (unsigned)gDumpHave, gDumpN);
    if (gPbdN && !gPbdWritten && gPbdHave >= gPbdFrames) {
        gPbdWritten = 1;
        printf("[audio] pbdump: %u DSP frames from frame %u (up to %u voices each, %u voices not captured) %s ux0:data/stairfax/audio_pbdump.bin\n",
               (unsigned)gPbdHave, gPbdFrom, gPbdN, aud_pbdump_truncated, pbd_write("ux0:data/stairfax/audio_pbdump.bin") ? "written to" : "NOT written to");
    } else if (gPbdN && !gPbdWritten) printf("[audio] pbdump: %u of %u DSP frames so far (waiting for DSP frame %u with a voice; now at %u)\n", (unsigned)gPbdHave, gPbdFrames, gPbdFrom, aud_frames);
#endif
    extern unsigned ai_ticks, ai_ticks_dropped, dsp_mails, dsp_passes, dsp_resumes, dsp_resume_reentries, ai_ticks_thread,
                    ai_ticks_retrace, reverb_dry_calls, ai_foreign_deliveries, ai_standin_calls, ai_foreign_installs;
#if defined(__vita__)
    printf("[audio] lock: owner %d (0 free, 1 audio, 2 game) depth %d max %d | game takes %u releases %u | audio takes %u releases %u | before FULL: takes %u releases %u | leaked game-side holds released at a retrace %u\n",
           (int) gLockOwner, gLockDepth, aud_lk_max_depth, aud_lk_game_take, aud_lk_game_rel, aud_lk_aud_take, aud_lk_aud_rel, aud_lk_noop_take, aud_lk_noop_rel, aud_lk_forced);
#endif
    printf("[audio] depop: %u bus ramps started, largest start value %u (normal: up to a few thousand; millions = the SPB word order is wrong)%s | output samples clamped %u\n",
           aud_depop_ramps, aud_depop_peak, aud_spb_old ? " | audio_spb_old.txt: reading the SPB the OLD, WRONG way on purpose" : "", aud_out_clamped);
    printf("[audio] foreign AI callback over the synth: %s (installed %u times) | delivered on main %u, asked for the previous callback %u (stand-in) | its frames added to the mix %u, ran dry %u | audio thread stack test %u (1 = ok)\n",
           ai_shim_foreign_over_synth() ? "YES" : "no", ai_foreign_installs, ai_foreign_deliveries, ai_standin_calls, aud_ring_added, aud_foreign_dry, aud_stack_test);
    {   /* THE ATTRACT MOVIE, per real second since the last report.  Working (movie playing): audio to the AI 32000/s, starved 0,
           frames read 29.97/s = decoded + skipped, the ring dry 0.  Run 208's behaviour: read ~7.5/s, skipped 0, audio ~8000/s. */
        static unsigned p_read, p_dec, p_skip, p_af, p_mix, p_starv, p_starvf, p_dry, p_del;
        static unsigned long long p_t;
        unsigned long long now = AUD_NOW();
        unsigned dec = MV_DECODED();
        double s = p_t && now > p_t ? (double)(now - p_t) / 1000000.0 : 0.0;
        if (stfx_mv_read != p_read || ai_foreign_deliveries != p_del) {
            if (s > 0.0)
                printf("[movie] last %.1f s, per second: frames read %.2f | video decoded %.2f skipped %.2f | audio frames decoded %.2f | movie audio to the AI %.0f samples (32000 = real time) | AI callbacks %.1f, starved %.2f (%.0f samples of silence) | ring dry %.2f\n",
                       s, (stfx_mv_read - p_read) / s, (dec - p_dec) / s, (stfx_mv_skipped - p_skip) / s, (stfx_mv_aframes - p_af) / s, (stfx_mv_mixed - p_mix) / s,
                       (ai_foreign_deliveries - p_del) / s, (stfx_mv_starved - p_starv) / s, (stfx_mv_starved_frames - p_starvf) / s, (aud_foreign_dry - p_dry) / s);
            printf("[movie] totals: frames read %u | video decoded %u skipped %u | audio frames decoded %u (%u samples) mixed out %u | starved callbacks %u (%u samples) | ring added %u dry %u peak %u overflow %u | skip %s, AI window %s, prime %u\n",
                   stfx_mv_read, dec, stfx_mv_skipped, stfx_mv_aframes, stfx_mv_asamples, stfx_mv_mixed, stfx_mv_starved, stfx_mv_starved_frames,
                   aud_ring_added, aud_foreign_dry, aud_ring_peak, aud_ring_overflow, gMovieSkipOff ? "OFF" : "on", gMovieAiWinOff ? "OFF" : "on", aud_movie_prime_frames);
        }
        p_read = stfx_mv_read; p_dec = dec; p_skip = stfx_mv_skipped; p_af = stfx_mv_aframes; p_mix = stfx_mv_mixed; p_starv = stfx_mv_starved;
        p_starvf = stfx_mv_starved_frames; p_dry = aud_foreign_dry; p_del = ai_foreign_deliveries; p_t = now;
    }
    printf("[audio] level %u | ticks %u (thread %u retrace %u dropped %u) mails %u passes %u resumes %u (re-entered %u) | frames mixed %u voices peak %u last %u"
           " | grains %u underruns %u silent chunks %u late grains %u | ring peak %u overflow %u\n",
           aud_level, ai_ticks, ai_ticks_thread, ai_ticks_retrace, ai_ticks_dropped, dsp_mails, dsp_passes, dsp_resumes, dsp_resume_reentries,
           aud_frames, aud_voices_peak, aud_voices_last, aud_grains, aud_underruns, aud_silent_chunks, aud_late_grains, aud_ring_peak, aud_ring_overflow);
    printf("[audio] groups pushed %u failed %u refused %u | swapped %u swap errors %u songs refused %u | pass us max %u avg %u, grain work us max %u"
           " | lock misses %u game timeouts %u unowned unlocks %u dsp waits %u | mixer oob %u bad op %u itd %u no-aram %u bad list %u | out errors %u port %d thread %u | reverb calls %u test fx %u\n",
           aud_groups_pushed, aud_groups_failed, aud_groups_refused, aud_swap_ok, aud_swap_err, aud_songs_refused, aud_pass_us_max,
           aud_frames ? aud_pass_us_total / aud_frames : 0u, aud_work_us_max, aud_lock_misses, aud_lock_game_timeouts, aud_unlock_unowned,
           aud_dsp_wait_timeouts, aud_mix_oob, aud_mix_badop, aud_mix_itd, aud_pass_nomix, aud_bad_list, aud_out_errors, aud_port, aud_thread_state,
           reverb_dry_calls, aud_testfx_fired);
#if defined(__vita__)
    if (dtk_stream_report) dtk_stream_report();        /* the [dtk] line: hal/dtk_stream.c */
#endif
}

/* ---- the data hooks (vt_game.c wrappers; game thread) -------------------------------------------------------- */
/* Called once per loadFileByPathAsync, after the (synchronous) read: dvd_shim.c:346-351 reads, then runs the
 * game's completion callback, before the loader returns -- and the callbacks for these files only set flags
 * (audio.c:259-400, 727-735), so nothing has parsed the buffer yet. */
void stfx_audio_file_loaded(const char* path, void* buf, int size)
{
    int rc;
    if (!path || !buf || size <= 0) return;
    rc = stfx_audio_table_swap_by_path(path, buf, (size_t)size);   /* audio/data/{Sfx,Music,Streams}.bin: game data, swapped at EVERY level */
    if (rc != 0) {
        if (rc < 0) { ++aud_table_err; printf("[audio] TABLE SWAP FAILED %s (%d bytes): %s -- buffer left big-endian\n", path, size, stfx_audio_tables_error()); }
        else        { ++aud_table_ok;  printf("[audio] swapped table %s (%d bytes, %d records)\n", path, size, rc); }
        return;                                           /* never a MusyX file: skip gSwapped / gWadSwapped / aud_swap_* */
    }
    if (stfx_audio_level() == STFX_AUDIO_OFF) return;
    rc = musyx_swap_by_path(path, buf, (size_t)size);
    if (rc == 0) return;                                  /* not a MusyX data file (.sam included: never swapped) */
    if (rc < 0) {
        ++aud_swap_err;
        printf("[audio] SWAP FAILED %s (%d bytes): %s -- buffer left as it was; every group push and song is refused from here\n", path, size, musyx_endian_error());
        return;
    }
    ++aud_swap_ok;
    if (gSwappedN < AUD_SWAPPED_MAX) gSwapped[gSwappedN++] = buf;
    {   size_t n = strlen(path);                          /* the only MusyX data file whose name ends in 'd' is midi.wad */
        if (n && (path[n - 1] == 'd' || path[n - 1] == 'D')) gWadSwapped = 1;
    }
    printf("[audio] swapped %s (%d bytes, %d records)\n", path, size, rc);
}
static int was_swapped(const void* p)
{
    unsigned i;
    for (i = 0; i < gSwappedN; i++) if (gSwapped[i] == p) return 1;
    return 0;
}
/* MusyX walks these three buffers natively: one that is still big-endian aborts inside ScanIDList (the reason the
 * old shim declined every group).  So a push is let through only for buffers this file swapped. */
int stfx_audio_group_gate(const void* prj, const void* sdir, const void* pool)
{
    if (stfx_audio_level() == STFX_AUDIO_OFF) { ++snd_groups_declined; ++aud_groups_refused; return 0; }
    if (aud_swap_err || !was_swapped(prj) || !was_swapped(sdir) || !was_swapped(pool)) { ++aud_groups_refused; return 0; }
    return 1;
}
void stfx_audio_group_result(int ok)
{
    if (ok) ++aud_groups_pushed; else ++aud_groups_failed;
    gLastPushRetrace = gRetraces;
}
int stfx_audio_song_gate(void)
{
    if (stfx_audio_level() != STFX_AUDIO_OFF && !aud_swap_err && gWadSwapped) return 1;
    ++aud_songs_refused;
    return 0;
}

/* ---- host test seams ------------------------------------------------------------------------------------------ */
#ifndef __vita__
void stfx_audio_host_reset(int level, uint8_t* aram, uint32_t aram_size, StfxHostXlat xl, void* user, const uint8_t* coef)
{
    gHostLevel = level; gLevel = -1;
    gHostAram = aram; gHostAramSize = aram_size; gHostXl = xl; gHostXlUser = user;
    gCoefHave = coef != NULL; if (coef) memcpy(gCoefRaw, coef, sizeof gCoefRaw);
    gMixReady = 0; gStarted = 0; gAlive = 0; gRingW = gRingR = 0; gRingPrimed = 0; gTonePhase = gToneFrames = 0;
    aud_frames = aud_underruns = aud_silent_chunks = aud_ring_overflow = aud_ring_peak = aud_bad_list = aud_pass_nomix = 0;
    aud_ring_added = aud_foreign_dry = 0;
    aud_voices_peak = aud_voices_last = 0;
    gSwappedN = 0; gWadSwapped = 0; aud_swap_ok = aud_swap_err = aud_groups_pushed = aud_groups_failed = aud_groups_refused = 0;
    (void)stfx_audio_level();
    stfx_audio_start();
}
void stfx_audio_host_pbdump(unsigned voices, unsigned from, unsigned frames) { pbd_alloc(voices, from, frames); }
void stfx_audio_host_spb_old(int on) { aud_spb_old = (unsigned)on; gMixReady = 0; }   /* what audio_spb_old.txt does; takes effect at the next pass */
int  stfx_audio_host_pbdump_write(const char* path) { return gPbdHave ? pbd_write(path) : 0; }
void stfx_audio_host_pump(int16_t* out, int grains)
{
    int g;
    gHostInPump = 1;
    for (g = 0; g < grains; g++) {
        fill_chunk(out + (size_t)g * STFX_AUDIO_GRAIN * 2);
        fill_chunk(out + (size_t)g * STFX_AUDIO_GRAIN * 2 + STFX_AUDIO_CHUNK * 2);
        ++aud_grains;
    }
    gHostInPump = 0;
}
#endif
