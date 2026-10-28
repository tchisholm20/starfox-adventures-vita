/* stfx_audio.h -- Stairfax audio backend for the PS Vita (stage 2): the output thread, the lock between the
 * game's MusyX calls and the synth tick, the software-DSP pass (dspmix), markers and counters.
 *
 * Two files work together and are always linked together:
 *   dsp_ai_shim.c  the Dolphin DSP + AI entry points MusyX calls, the DSP handshake, the tick drivers
 *   stfx_audio.c   everything Vita: SceAudioOut, the thread, the locks, the mixer instance, the ring
 * Design, evidence and what is NOT verified: NOTES.md next to this file.  Nothing here has run on a console.
 */
#ifndef STFX_AUDIO_H
#define STFX_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The level, decided ONCE at first use from marker files in ux0:data/stairfax/ (NOTES.md section 6):
 *   audio_off.txt  first byte '1'            -> OFF      exactly the pre-stage-2 behaviour
 *   audio_mode.txt first byte 'A'            -> DATA     + byte-swapped sound data and the real sndPushGroup
 *                             'B'            -> TONE     + output thread playing a test tone (synth as before)
 *                             'R'            -> RETRACE  + new DSP handshake, mixer, ticks at retraces on main
 *                  absent / anything else    -> FULL     the audio thread owns the synth tick                  */
enum { STFX_AUDIO_OFF = 0, STFX_AUDIO_DATA = 1, STFX_AUDIO_TONE = 2, STFX_AUDIO_RETRACE = 3, STFX_AUDIO_FULL = 4 };

#define STFX_AUDIO_CHUNK   160      /* stereo frames per DSP frame (sal_ai.c:13: 0x280 bytes) */
#define STFX_AUDIO_GRAIN   320      /* SceAudioOut grain: 2 DSP frames (160 is not a multiple of 64; audioout.h:81) */
#define STFX_AUDIO_RATE    32000

int  stfx_audio_level(void);        /* effective level (drops to DATA if the output could not be started) */
int  stfx_audio_new_dsp(void);      /* 1 = new handshake + mixer (RETRACE/FULL with a live output) */
int  stfx_audio_game_clock(void);   /* audio_clock.txt '1': thread ticks are granted by the game clock */
int  stfx_audio_reverb_dry(void);   /* audio_reverb.txt '1': leave the aux-A send in place (the old stub) */
int  stfx_movie_skip_off(void);     /* movie_skip_off.txt '1': the THP video decoder drops no frame (the decompiled loop) */
int  stfx_movie_aiwin_off(void);    /* movie_aiwin_off.txt '1': a retrace's foreign AI callbacks share ONE interrupt window */

void stfx_audio_start(void);        /* main thread, from AIInit: open the port, start the thread */
void stfx_audio_shutdown(void);     /* main thread, before sceKernelExitProcess */

/* THE LOCK between the game's MusyX API calls and the synth tick on the audio thread (FULL only; a no-op at
 * every other level).  Recursive per SIDE: side 1 = the audio thread, side 2 = every game thread (the baton
 * scheduler runs one of them at a time).  sndBegin/sndEnd call these (PATCH.md P3). */
int  stfx_audio_lock(void);                 /* game side; waits up to 2 s, then gives up for good (counted, reported) */
int  stfx_audio_lock_timed(unsigned us);    /* audio side; 0 = not taken */
void stfx_audio_unlock(void);
int  stfx_audio_on_audio_thread(void);      /* cheap (a stack-range test, no syscall): os_shim.c's DC* guard uses it */

/* "The DSP is busy": held from the start of a tick to the end of its pass.  Bounded waits. */
int  stfx_audio_dsp_acquire(void);
void stfx_audio_dsp_release(void);

void*          stfx_audio_addr(uint32_t a);           /* 32-bit address -> pointer (identity on the Vita) */
const int16_t* stfx_audio_pass(uint32_t cmdlist);     /* one DSP frame; returns the 160-frame s16 stereo chunk it wrote, or NULL */
void stfx_audio_push(const int16_t* stereo, unsigned frames);   /* retrace mode: main thread -> ring */
void stfx_audio_main_poll(void);                      /* main thread, once per retrace: deferred messages, test FX */
void stfx_audio_report(void);                         /* main thread: the [audio] status lines */

/* Hooks for the link-time wrappers in vt_game.c (PATCH.md P6). */
void stfx_audio_file_loaded(const char* path, void* buf, int size);          /* swap MusyX data in place, once */
int  stfx_audio_group_gate(const void* prj, const void* sdir, const void* pool);  /* 0 = refuse this sndPushGroup */
void stfx_audio_group_result(int ok);
int  stfx_audio_song_gate(void);                                             /* 0 = refuse sndSeqPlayEx */

/* Implemented by dsp_ai_shim.c, called by the audio thread once per 160 frames:
 *    1  *chunk = 160 fresh stereo frames (the synth ticked and the DSP pass ran)
 *    0  nothing to play for this chunk (AI stopped, lock missed, tick parked): output silence
 *   -1  the tick is not ours (retrace mode, or a foreign AI callback such as the movie's): drain the ring */
int  ai_shim_thread_tick(const int16_t** chunk);
/* 1 = a foreign AI callback (the attract movie's) is registered over MusyX's at FULL: the audio thread still ticks
 * the synth, the foreign callback runs on the main thread, and its audio arrives in the ring to be ADDED. */
int  ai_shim_foreign_over_synth(void);

#ifndef __vita__
/* Host test seams (test_stfx_audio.c).  Not compiled into the Vita build. */
#include <stddef.h>
typedef void* (*StfxHostXlat)(uint32_t addr, void* user);
void stfx_audio_host_reset(int level, uint8_t* aram, uint32_t aram_size, StfxHostXlat xl, void* user,
                           const uint8_t* coef_be4096);
void stfx_audio_host_pump(int16_t* out, int grains);
void stfx_audio_host_pbdump(unsigned voices, unsigned from, unsigned frames);   /* what audio_pbdump.txt does on the console */
int  stfx_audio_host_pbdump_write(const char* path);
void stfx_audio_host_spb_old(int on);   /* what the audio thread does, without the output call */
void ai_shim_host_reset(void);
extern unsigned long long stfx_host_now_us;            /* the clock ai_shim_deliver reads on the host */
#endif

#ifdef __cplusplus
}
#endif
#endif
