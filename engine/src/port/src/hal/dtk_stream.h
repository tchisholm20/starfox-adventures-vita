/* dtk_stream.h -- the GameCube's DVD AUDIO STREAM ("DTK": the drive decodes ADPCM off the disc and the AI plays it
 * next to the DSP's output) for the Vita port.  The game's dialogue and streamed music (src/main/audio_stream.c,
 * streams/<dir>/<name>.adp, 958 files on the Rev 1 disc) come through here.
 *
 * Three parties, three threads:
 *   the GAME thread   the drive commands (dvd_shim.c: DVDPrepareStreamAsync / DVDCancelStreamAsync /
 *                     DVDStopStreamAtEndAsync / the status poll) and the AI stream controls (AISetStream*), all
 *                     defined in dtk_stream.c; every callback the game gets is delivered by dvd_shim.c INSIDE the
 *                     call, on the calling game thread, as the stub did;
 *   the READER thread reads the .adp bytes ahead into a ring (never the audio thread, never the game thread);
 *   the AUDIO thread  stfx_audio.c calls dtk_stream_mix() once per grain: decode, 48 kHz -> 32 kHz, volume, add.
 *
 * Break arm: ux0:data/stairfax/dtk_off.txt, first byte
 *   '1'  SILENT: nothing is read, decoded or added, but the stream's state and its length in real time are those of
 *        the working arm (prepare / start / end / cancel happen at the same moments), so sequencing does not change;
 *   '2'  LEGACY: the stub of before 2026-10-11, literally: every command succeeds, the status poll says "in progress"
 *        for ever, no stream ever ends.
 * Stream clock: ux0:data/stairfax/dtk_realtime.txt '1' keeps the stream on REAL time (the behaviour before 2026-10-11:
 *   speech runs ahead of the picture by every long frame); otherwise, with the virtual clock in use, it follows game time.
 * Channel order: ux0:data/stairfax/dtk_swap.txt '1' exchanges left and right (dtk_stream.c, at dtk_decode_block: why).
 * NOT RUN ON A CONSOLE.  Host test: projects/stairfax-vita/host/audio/vita/run_dtk_test.sh. */
#ifndef DTK_STREAM_H
#define DTK_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTK_BLOCK_BYTES   32u       /* one ADPCM block on the disc */
#define DTK_BLOCK_FRAMES  28u       /* ... decodes to 28 stereo frames at 48 kHz */
#define DTK_RATE          48000u

/* ---- the decoder (pure; the hardware's, as Dolphin's StreamADPCM describes it) ------------------------------- */
typedef struct DtkAdpcm { int32_t l1, l2, r1, r2; } DtkAdpcm;     /* history, <<6 fixed point; all zero at a track start */
void dtk_decode_block(DtkAdpcm* st, const uint8_t src[32], int16_t dst[56]);   /* dst: L R L R ... 28 frames */

/* ---- the drive side (game thread; called by dvd_shim.c) ------------------------------------------------------- */
/* DVDLowAudioStream(0, length, offset): `file` is the host file that holds the bytes (the ISO image, or the .adp in
 * directory mode), `base` the byte offset of the stream in it, `length` its size (a multiple of 32), `label` what
 * the log calls it.  No stream active: it becomes the current track and waits for the AI.  A stream active without
 * a pending stop-at-end: it is QUEUED and follows the current track without a gap in the timeline.  A stream active
 * WITH a pending stop-at-end: it REPLACES it (counted; the game never does this -- it cancels first). */
void     dtk_stream_prepare(const char* file, uint32_t base, uint32_t length, const char* label);
void     dtk_stream_stop_at_end(void);      /* DVDLowAudioStream(0, 0, 0): the current track ends the stream */
void     dtk_stream_cancel(void);           /* DVDLowAudioStream(0x10000, 0, 0) */
uint32_t dtk_stream_status(void);           /* DVDLowRequestAudioStatus(0): 1 = a stream is active, 0 = none / ended */
uint32_t dtk_stream_play_offset(void);      /* byte offset of the play position inside `file`, 32 KB granular (status 0x10000) */

/* ---- the game clock (game thread; called by os_shim.c at every retrace while the virtual clock is in use) ----------- */
/* The stream's position follows GAME time, not real time: see the THE GAME CLOCK block in dtk_stream.c.  virt_us is the
 * virtual clock's microseconds after this retrace.  Never called = real time, as before. */
void     dtk_stream_game_tick(long long virt_us);

/* ---- the AI side: AISetStreamPlayState & co. are DEFINED in dtk_stream.c with the SDK's names ------------------ */

/* ---- the audio thread ----------------------------------------------------------------------------------------- */
/* Adds `frames` stereo frames (32 kHz, s16) of the stream to `out`, saturating.  gain_pct = stfx_audio's
 * audio_gain.txt value (100 = unchanged).  Never blocks on I/O; does nothing when no stream has ever been prepared. */
void dtk_stream_mix(int16_t* out, unsigned frames, unsigned gain_pct);

/* ---- housekeeping (game thread) -------------------------------------------------------------------------------- */
void dtk_stream_report(void);               /* the [dtk] line of the periodic audio report */
void dtk_stream_shutdown(void);             /* stops the reader thread (<= 0.5 s), closes the file */

/* counters (plain globals, as stfx_audio.c's) */
extern unsigned dtk_prepared, dtk_started, dtk_ended, dtk_cancelled, dtk_cancel_calls, dtk_queued, dtk_replaced, dtk_loops;
extern unsigned dtk_frames_decoded, dtk_frames_out, dtk_underruns, dtk_blocks_silent, dtk_reads, dtk_read_bytes;
extern unsigned dtk_open_failures, dtk_read_errors, dtk_mix_us_max, dtk_fallback_blocks, dtk_out_clamped;
extern unsigned dtk_holds, dtk_stale_holds, dtk_hold_frames, dtk_hold_max, dtk_ticks;   /* game-clock sync; frames are output (32 kHz) frames */
extern unsigned dtk_mode;                   /* 0 on, 1 silent (dtk_off.txt '1'), 2 legacy stub (dtk_off.txt '2') */

#ifndef __vita__
/* Host test seams (test_dtk_stream.c). */
extern unsigned long long dtk_host_now_us;  /* the clock */
extern int dtk_host_audio_running;          /* stands in for stfx_audio.c's aud_thread_state == 1 */
void dtk_host_reset(int mode);              /* forget everything; mode as dtk_mode */
int  dtk_host_reader_step(void);            /* one iteration of the reader thread; 1 = it read something */
extern uint8_t dtk_host_flog[1u << 21]; extern unsigned dtk_host_flog_n;   /* what the mix did with each output frame it handled: 0 plain, 1 silent hold, 4 silent (stalled game), 2 fade-out, 3 fade-in */
void dtk_host_swap(int on);                 /* what dtk_swap.txt does */
void dtk_host_realtime(int on);             /* what dtk_realtime.txt does */
unsigned dtk_host_ring_bytes(void);
void dtk_host_resample(const int16_t* in48, unsigned in_frames, int16_t* out32, unsigned out_frames);  /* the resampler alone */
#endif

#ifdef __cplusplus
}
#endif
#endif
