/* dtk_stream.c -- the GameCube's DVD audio stream for the Vita port.  Contract and threads: dtk_stream.h.
 *
 * NOT RUN ON A CONSOLE.  Compiled for the Vita to an object; exercised on the host by
 * projects/stairfax-vita/host/audio/vita/run_dtk_test.sh (test_dtk_stream.c), every check watched failing.
 *
 * WHAT THE HARDWARE DOES (and this file copies):
 *   - the disc holds 32-byte ADPCM blocks; each decodes to 28 stereo frames at 48 kHz (dtk_decode_block);
 *   - "prepare" (DVDLowAudioStream(0, length, offset)) names a track; with no stream active it becomes the current
 *     one, the decoder history is cleared and the drive is ready; with one active it is queued to follow;
 *   - "stop at end" (DVDLowAudioStream(0, 0, 0)) makes the END of the current track end the stream; without it the
 *     stream goes on with the queued track (the same one again when nothing else was queued: a loop);
 *   - "cancel" (DVDLowAudioStream(0x10000, 0, 0)) ends the stream now;
 *   - the AI pulls samples only while its stream play state is 1 (AISetStreamPlayState): prepared and not started
 *     = the position does not move; stopped in the middle = paused;
 *   - the status request (DVDLowRequestAudioStatus(0)) answers 1 while a stream is active and 0 after it ended or
 *     was cancelled.  THE GAME POLLS THIS EVERY FRAME (fileio.c dvdCheckError) and takes 0 as "the stream is over"
 *     (audio_stream.c AudioStream_PlayAddrCallback): that poll is how a line of dialogue ends.
 *   - the AI's stream volume is 0..255 per side, applied as sample * (v + (v >> 7)) >> 8.
 *
 * THE CLOCK is the audio thread: one grain of 320 output frames consumes 480 stream frames, so a track lasts its
 * length in REAL time whatever the game's frame rate does.  If there is NO audio thread (audio_off.txt, the output
 * did not start: stfx_audio.c's aud_thread_state is not 1) and nothing has pulled for 250 ms, the status poll advances
 * the position by the wall clock instead, so the game sees the same stream lengths with the audio off
 * (dtk_fallback_blocks counts it).  A running audio thread that is merely late is never overtaken.
 *
 * THE GAME CLOCK (2026-10-11, dialogue sync).  The game's clock is the PACED VIRTUAL clock (os_shim.c): a frame longer than
 * 4 fields (66.7 ms) loses time for good, and cutscene animation, camera and subtitles follow that clock while the
 * stream followed real time, so speech ran ahead by the sum of every long frame.  Now the stream's position follows
 * GAME time:
 *   - os_virtual_clock_retrace calls dtk_stream_game_tick(virtual us) once per presented frame.  At that instant the
 *     game thread compares what the audio thread has consumed (gCout, output frames since the anchor) with the game
 *     time that has passed (since the anchor) and, when the stream is AHEAD by >= DTK_HOLD_MIN (10 ms), queues the
 *     difference as a HOLD (gDebt): the audio thread then adds silence instead of consuming the stream, with a 3 ms
 *     fade-out before and a 3 ms fade-in after, both over audio that is still consumed (nothing is lost or repeated).
 *   - a stall the tick has not reported yet is handled without waiting for it: with no tick for DTK_STALE_TICK_US
 *     (100 ms: more than the 66.7 ms a frame can ever advance the clock by) the audio thread holds the stream by
 *     itself until the next tick; the lead a stall can leave is then at most 100 - 66.7 = 33 ms, which the tick holds.
 *   - a stream STARTS (AI play state 1, or a prepare with the AI running) only at the next tick: the picture of the
 *     frame that asked for it is shown at that retrace, so the speech begins with it, not with the stall before it.
 *   The END of a track is reached when the stream has been CONSUMED to its length, i.e. after its length of game time
 *   (plus the holds, which are exactly the game time the stream was ahead): the status poll answers 0 on that clock.
 *   Sequences never read it (they count their own subtitle time in game steps: 2.c ObjSeq), so nothing waits on it.
 *   Without a virtual clock (no tick ever), or with dtk_realtime.txt '1', none of this exists: the stream is real time.
 *
 * THE LOCK: one light-weight mutex around all state.  It is held for the copy of a read chunk into the ring, for a
 * drive/AI command, and for one grain's decode + resample (about 20,000 multiply-adds); never across file I/O. */
#include "dtk_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef DTK_FAULT
#define DTK_FAULT 0      /* test-only faults, each of which run_dtk_test.sh MUST see fail:
                            1 one decoder coefficient wrong (0x73 -> 0x72)
                            2 stop-at-end is ignored (the track loops instead of ending)
                            3 the resampler has no low-pass (one tap)
                            4 a ring underrun advances the position (the stream would skip, not stall)
                            5 the game-clock hold is disabled (the stream stays on real time) */
#endif

#define DTK_DIR      "ux0:data/stairfax/"
#define DTK_RING     32768u                 /* bytes of ADPCM read ahead: 0.6 s */
#define DTK_CHUNK    4096u                  /* bytes per read: 75 ms of stream */
#define DTK_PRIME    4096u                  /* bytes the ring must hold before a (re)start is played */
#define DTK_TAPS     32u                    /* per phase; the prototype low-pass has 64 taps at 96 kHz */
#define DTK_BATCH    320u                   /* output frames per resampler batch */
#define DTK_INMAX    (DTK_BATCH * 3u / 2u + DTK_TAPS + 2u * DTK_BLOCK_FRAMES + 8u)
#define DTK_STALE_US 250000ull              /* no grain for this long = the audio thread is not the clock */
#define DTK_OUT_RATE      32000u            /* the output (mix) rate */
#define DTK_HOLD_MIN      320u              /* output frames (10 ms): a lead smaller than this is never held; it is kept as a debt */
#define DTK_FADE          96u               /* output frames (3 ms) of fade-out before a hold and of fade-in after it */
#define DTK_STALE_TICK_US 100000ull         /* no game tick for this long = the game is stalled: hold the stream until the next */

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#define DTK_NOW() ((unsigned long long)sceKernelGetProcessTimeWide())
static SceKernelLwMutexWork gMtx;
static int gMtxOk;
static SceUID gSema = -1, gThread = -1;
#define LOCK()   do { if (gMtxOk) sceKernelLockLwMutex(&gMtx, 1, NULL); } while (0)
#define UNLOCK() do { if (gMtxOk) sceKernelUnlockLwMutex(&gMtx, 1); } while (0)
#define WAKE()   do { if (gSema >= 0) sceKernelSignalSema(gSema, 1); } while (0)
extern unsigned aud_thread_state __attribute__((weak));     /* stfx_audio.c: 1 = its output thread is running */
#define AUDIO_THREAD_RUNNING() (&aud_thread_state && aud_thread_state == 1u)
#else
unsigned long long dtk_host_now_us;
int dtk_host_audio_running;
#define AUDIO_THREAD_RUNNING() dtk_host_audio_running
#define DTK_NOW() dtk_host_now_us
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#define WAKE()   ((void)0)
#endif

/* ---- counters ------------------------------------------------------------------------------------------------- */
unsigned dtk_prepared;          /* prepare commands */
unsigned dtk_started;           /* a stream began to be pulled (AI play state 1 with a stream active) */
unsigned dtk_ended;             /* streams that reached the end of their track with stop-at-end set */
unsigned dtk_cancelled;         /* cancel commands that found a stream active */
unsigned dtk_cancel_calls;      /* all cancel commands */
unsigned dtk_queued;            /* prepares that found a stream active: queued behind it */
unsigned dtk_replaced;          /* prepares that found a stream active WITH stop-at-end pending: it was replaced */
unsigned dtk_loops;             /* track ends without stop-at-end: went on with the queued track */
unsigned dtk_frames_decoded;    /* 48 kHz stereo frames decoded */
unsigned dtk_frames_out;        /* 32 kHz stereo frames added to the output while a stream was being pulled */
unsigned dtk_underruns;         /* the ring ran dry while it was being played (the reader fell behind) */
unsigned dtk_blocks_silent;     /* blocks passed without data: SILENT arm, or after an open/read failure */
unsigned dtk_reads, dtk_read_bytes;
unsigned dtk_open_failures, dtk_read_errors;
unsigned dtk_mix_us_max;        /* longest dtk_stream_mix, microseconds */
unsigned dtk_fallback_blocks;   /* blocks passed by the wall clock because no audio grain was pulling */
unsigned dtk_out_clamped;       /* output samples that hit the 16-bit clamp when the stream was added */
unsigned dtk_mode;              /* 0 on, 1 silent, 2 legacy stub */
unsigned dtk_holds;             /* holds begun (game-clock sync) */
unsigned dtk_stale_holds;       /* ... of which begun by the audio thread because the game stopped ticking */
unsigned dtk_hold_frames;       /* output frames of silence inserted by holds (the fades are not counted) */
unsigned dtk_hold_max;          /* the longest hold's silence, output frames */
unsigned dtk_ticks;             /* game ticks received */

/* ---- state (all under the lock unless noted) ------------------------------------------------------------------ */
typedef struct DtkTrack { char file[200]; uint32_t start, length; char label[64]; } DtkTrack;

static volatile int gInit;      /* set last by dtk_init: the audio thread does nothing before */
static volatile int gQuit;
static DtkTrack gCur, gNext;
static int      gStream, gStopAtEnd, gAiPlay;
static uint32_t gPos;           /* byte offset in gCur.file of the next block */
static DtkAdpcm gAd;
static uint8_t  gVolL, gVolR;
static uint32_t gAiRate = 1;    /* AI_SAMPLERATE_48KHZ */
static uint32_t gAiSamples;     /* AIGetStreamSampleCount: 48 kHz frames pulled since the last reset */
static unsigned long long gClockUs;     /* the moment up to which the stream's timeline has been advanced */
static int      gFallback;      /* the wall clock advanced the position: the reader job is stale */
static int      gEndPending;    /* an end not yet printed by the game thread */
static char     gEndLabel[64];
static uint32_t gEndBytes;

static unsigned gGen;           /* reader job generation */
static char     gJobFile[200];
static uint32_t gJobOff, gJobLeft;
static int      gJobErr;
static char     gFailPath[200];
static uint32_t gFailOff;
static uint8_t  gRing[DTK_RING];
static unsigned gRingW, gRingR; /* free-running byte counters */
static int      gPrimed;

#if defined(__vita__)
#define FLOG(m, n) ((void)0)
#else
uint8_t dtk_host_flog[1u << 21]; unsigned dtk_host_flog_n;      /* per output frame the mix produced: 0 plain, 1 silent (a hold), 4 silent (game stalled), 2 fade-out, 3 fade-in, 5 the filter tail (not live) */
#define FLOG(m, n) do { unsigned q_; for (q_ = 0; q_ < (n) && dtk_host_flog_n < sizeof dtk_host_flog; q_++) dtk_host_flog[dtk_host_flog_n++] = (uint8_t)(m); } while (0)
#endif
static int16_t  gCoef[2][DTK_TAPS];             /* Q15, each phase sums to 32768 */
static int16_t  gIn[DTK_INMAX * 2];             /* decoded 48 kHz frames waiting for the resampler */
static unsigned gInHave, gInPos2;               /* frames valid; read position in HALF frames */
static int      gTail;                          /* gIn[0 .. gTail) may hold real audio; beyond it, zeros */
static unsigned gSilentAcc;                     /* SILENT arm: half frames owed */
static int      gSwapLR;                        /* dtk_swap.txt '1': low nibble to the RIGHT (see dtk_decode_block) */
static int      gInReset;                       /* a new stream or a cancel: whatever is still inside the filter is dropped */

/* the game clock (all under the lock) */
static int      gRealtime;                      /* dtk_realtime.txt '1': the stream stays on real time */
static int      gClockSeen;                     /* a game tick has ever arrived: the game clock is the virtual one */
static int      gGate;                          /* started, waiting for the next tick to anchor: nothing is pulled */
static int      gAnchored;                      /* gVirt0 / gCout are valid */
static long long gVirtNow, gVirt0;              /* virtual us: at the last tick; at the anchor */
static unsigned long long gTickReal;            /* DTK_NOW() at the last tick */
static unsigned long long gCout;                /* output frames consumed (not held) since the anchor */
static unsigned gDebt;                          /* output frames the stream is ahead of the game and not yet scheduled */
static unsigned gHoldLeft;                      /* output frames of silence still to be inserted */
static unsigned gHoldCur;                       /* silence output so far in the hold in progress */
static int      gPhase;                         /* 0 normal, 1 fade-out, 2 silent, 3 fade-in */
static unsigned gFadeC;                         /* fade counter: gain = gFadeC / DTK_FADE */
static long long gLastExcess, gLastResid;       /* at the last tick: how far ahead the stream was, and what no hold covers */
static long long gExcessMax;                    /* the largest lead seen at a tick */

/* ---- the decoder ---------------------------------------------------------------------------------------------- */
static inline int32_t dtk_sample(int32_t bits, int32_t q, int32_t* h1, int32_t* h2)
{
    int32_t hist, cur;
    switch (q >> 4) {
    case 1:  hist = *h1 * 0x3c; break;
    case 2:  hist = *h1 * (DTK_FAULT == 1 ? 0x72 : 0x73) - *h2 * 0x34; break;
    case 3:  hist = *h1 * 0x62 - *h2 * 0x37; break;
    default: hist = 0; break;
    }
    hist = (hist + 0x20) >> 6;
    if (hist < -0x200000) hist = -0x200000; else if (hist > 0x1fffff) hist = 0x1fffff;
    cur = ((int32_t)(int16_t)(bits << 12) >> (q & 0xf)) * 64 + hist;
    *h2 = *h1;
    *h1 = cur;
    cur >>= 6;
    return cur < -0x8000 ? -0x8000 : cur > 0x7fff ? 0x7fff : cur;
}
/* Bytes 0 and 1: the left and right block headers (2 and 3 repeat them).  Bytes 4..31: one frame each, the left
 * sample in the low nibble, the right one in the high nibble.
 * WHICH SIDE IS "LEFT" IS NOT SETTLED (2026-10-11).  The naming here is Dolphin's StreamADPCM (histl = byte 0 + low
 * nibble) and the format's CD-XA ancestry (sound unit 0 = left = low nibble).  But Dolphin's own OUTPUT has them the
 * other way round: its dtkdump of streams/GB/Intro.adp is this decoder's output value for value over all 3,612,672
 * frames with the two channels EXCHANGED (run_dtk_test.sh DTKDUMP=...) -- its mixer takes every buffer as right-first,
 * which is true of the DSP's (the ucode stores the R bus first) and unverified for the drive's.  No hardware recording
 * was available.  Default: the naming.  ux0:data/stairfax/dtk_swap.txt '1': Dolphin's output order. */
void dtk_decode_block(DtkAdpcm* st, const uint8_t src[32], int16_t dst[56])
{
    unsigned i;
    for (i = 0; i < DTK_BLOCK_FRAMES; i++) {
        dst[2 * i]     = (int16_t)dtk_sample(src[4 + i] & 0xf, src[0], &st->l1, &st->l2);
        dst[2 * i + 1] = (int16_t)dtk_sample(src[4 + i] >> 4,  src[1], &st->r1, &st->r2);
    }
}

/* ---- the resampler: 48 kHz -> 32 kHz, a 2-phase polyphase FIR ---------------------------------------------------
 * 2/3 = up by 2, low-pass, down by 3.  The low-pass is a 64-tap Kaiser-windowed sinc at the virtual 96 kHz (beta
 * 5.65, cut-off 15 kHz: flat to ~12 kHz, more than 55 dB down from ~18 kHz, which is where the fold to 14 kHz and
 * below starts).  An output frame is 32 input frames x 32 taps of ONE phase: phase = parity of the position in half
 * frames.  Linear interpolation would do no low-pass at all: everything between 16 and 24 kHz folds into 8..16. */
static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0; int k;
    for (k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
static void coef_build(void)
{
    const double pi = 3.14159265358979323846, beta = 5.65, fc = 15000.0 / 96000.0, c = (2.0 * DTK_TAPS - 1.0) / 2.0;
    unsigned p, k;
    for (p = 0; p < 2; p++) {
        double v[DTK_TAPS], sum = 0.0;
        int isum = 0, best = 0;
        for (k = 0; k < DTK_TAPS; k++) {
            double m = (double)(2u * (DTK_TAPS - 1u - k) + p), x = m - c, r = x / c;
            v[k] = sin(2.0 * pi * fc * x) / (pi * x) * bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
            sum += v[k];
        }
        for (k = 0; k < DTK_TAPS; k++) {
            int q = (int)floor(v[k] / sum * 32768.0 + 0.5);
            if (DTK_FAULT == 3) q = (k == DTK_TAPS / 2) ? 32767 : 0;
            gCoef[p][k] = (int16_t)q; isum += q;
            if (v[k] > v[best]) best = (int)k;
        }
        if (DTK_FAULT != 3) gCoef[p][best] = (int16_t)(gCoef[p][best] + (32768 - isum));   /* unity gain at DC, both phases alike */
    }
}
static inline void fir_frame(const int16_t* in, unsigned pos2, int* l, int* r)
{
    const int16_t* s = in + (pos2 >> 1) * 2;
    const int16_t* c = gCoef[pos2 & 1];
    int32_t al = 0, ar = 0; unsigned k;
    for (k = 0; k < DTK_TAPS; k++) { al += s[2 * k] * c[k]; ar += s[2 * k + 1] * c[k]; }
    al = (al + 16384) >> 15; ar = (ar + 16384) >> 15;
    *l = al < -32768 ? -32768 : al > 32767 ? 32767 : al;
    *r = ar < -32768 ? -32768 : ar > 32767 ? 32767 : ar;
}

/* ---- the reader job and the ring (lock held) -------------------------------------------------------------------- */
static void job_set(const char* file, uint32_t off, uint32_t len)
{
    snprintf(gJobFile, sizeof gJobFile, "%s", file);
    gJobOff = off; gJobLeft = len; gJobErr = 0;
    ++gGen;                                     /* a read in flight for the old job is thrown away */
    gRingR = gRingW; gPrimed = 0;
}
/* The game clock is in use: ticks have arrived, and the break arm is off. */
static inline int clock_on(void) { return gClockSeen && !gRealtime && DTK_FAULT != 5 && dtk_mode != 2; }
/* A stream starts, resumes, ends or is cancelled: forget the anchor and every hold.  If a stream is about to be
 * pulled, it waits for the next game tick (gGate) and is anchored there. */
static void anchor_reset(void)
{
    gAnchored = 0; gCout = 0; gDebt = gHoldLeft = gHoldCur = 0; gPhase = 0; gFadeC = 0;
    gGate = gStream && gAiPlay && clock_on();
}
static void track_start(const DtkTrack* t)      /* t becomes the current track, from its first block */
{
    if (t != &gCur) gCur = *t;
    gPos = gCur.start;
    memset(&gAd, 0, sizeof gAd);
    job_set(gCur.file, gCur.start, gCur.length);
}
static void stream_end(void)
{
    gStream = 0; gStopAtEnd = 0;
    job_set("", 0, 0);
    anchor_reset();
}

/* One block of the stream's timeline.  Returns 1 with 28 decoded frames in dst, or 0 with dst zeroed (nothing to
 * pull, no data yet, silent).  virt = the wall clock is pulling: never touch the ring. */
static int next_block(int16_t* dst, int virt)
{
    if (dst) memset(dst, 0, DTK_BLOCK_FRAMES * 2 * sizeof(int16_t));
    if (!gStream || !gAiPlay || gGate) return 0;
    if (gPos >= gCur.start + gCur.length) {     /* the end of the track */
        if (gStopAtEnd && DTK_FAULT != 2) {
            snprintf(gEndLabel, sizeof gEndLabel, "%s", gCur.label); gEndBytes = gCur.length; gEndPending = 1;
            ++dtk_ended;
            stream_end();
            return 0;
        }
        ++dtk_loops;
        track_start(&gNext);
        if (virt) job_set("", 0, 0);
    }
    gAiSamples += DTK_BLOCK_FRAMES;
    if (virt || dtk_mode == 1 || gJobErr) { gPos += DTK_BLOCK_BYTES; ++dtk_blocks_silent; return 0; }
    {   unsigned have = gRingW - gRingR;
        if (!gPrimed) {
            if (have >= DTK_PRIME || (gJobLeft == 0 && have >= DTK_BLOCK_BYTES)) gPrimed = 1;
            else return 0;                      /* waiting for the first read: the position does not move */
        }
        if (have < DTK_BLOCK_BYTES) {           /* ran dry: silence, and the stream STALLS until the ring has refilled */
            ++dtk_underruns; gPrimed = 0;
            if (DTK_FAULT == 4) gPos += DTK_BLOCK_BYTES;
            return 0;
        }
    }
    {   uint8_t blk[DTK_BLOCK_BYTES]; unsigned at = gRingR & (DTK_RING - 1u);     /* the ring holds whole blocks */
        memcpy(blk, gRing + at, DTK_BLOCK_BYTES);
        gRingR += DTK_BLOCK_BYTES;
        if (dst) dtk_decode_block(&gAd, blk, dst);
        gPos += DTK_BLOCK_BYTES;
        dtk_frames_decoded += DTK_BLOCK_FRAMES;
    }
    return 1;
}

/* ---- the reader thread -------------------------------------------------------------------------------------- */
static char gOpenPath[200];                     /* touched by the reader only */
#if defined(__vita__)
static SceUID gFd = -1;
static int  file_open(const char* p) { gFd = sceIoOpen(p, SCE_O_RDONLY, 0); return gFd >= 0; }
static void file_close(void) { if (gFd >= 0) sceIoClose(gFd); gFd = -1; }
static int  file_read(uint32_t off, void* buf, unsigned n)
{
    if (sceIoLseek(gFd, (SceOff)off, SCE_SEEK_SET) < 0) return -1;
    return sceIoRead(gFd, buf, n);
}
#else
static FILE* gFp;
static int  file_open(const char* p) { gFp = fopen(p, "rb"); return gFp != NULL; }
static void file_close(void) { if (gFp) fclose(gFp); gFp = NULL; }
static int  file_read(uint32_t off, void* buf, unsigned n)
{
    if (fseek(gFp, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, n, gFp);
}
#endif

/* One read of at most DTK_CHUNK bytes into the ring.  Returns 1 if it did anything. */
static int reader_step(void)
{
    static uint8_t buf[DTK_CHUNK];
    char file[200];
    unsigned gen = 0, n = 0;
    uint32_t off = 0;
    int got;
    LOCK();
    if (gJobLeft && !gJobErr && dtk_mode == 0 && DTK_RING - (gRingW - gRingR) >= DTK_CHUNK) {
        n = gJobLeft < DTK_CHUNK ? gJobLeft : DTK_CHUNK; off = gJobOff; gen = gGen;
        memcpy(file, gJobFile, sizeof file);
    }
    UNLOCK();
    if (!n) return 0;
    if (!gOpenPath[0] || strcmp(file, gOpenPath) != 0) {
        file_close(); gOpenPath[0] = 0;
        if (!file[0] || !file_open(file)) {
            LOCK();
            if (gen == gGen) { gJobErr = 1; ++dtk_open_failures; memcpy(gFailPath, file, sizeof gFailPath); gFailOff = off; }
            UNLOCK();
            return 1;
        }
        memcpy(gOpenPath, file, sizeof gOpenPath);
    }
    got = file_read(off, buf, n);
    LOCK();
    if (gen == gGen) {
        if (got == (int)n) {
            unsigned at = gRingW & (DTK_RING - 1u), first = DTK_RING - at;
            if (first > n) first = n;
            memcpy(gRing + at, buf, first);
            if (first < n) memcpy(gRing, buf + first, n - first);
            gRingW += n; gJobOff += n; gJobLeft -= n;
            ++dtk_reads; dtk_read_bytes += n;
        } else {
            gJobErr = 1; ++dtk_read_errors; memcpy(gFailPath, file, sizeof gFailPath); gFailOff = off;
        }
    }
    UNLOCK();
    return 1;
}

#if defined(__vita__)
static volatile int gReaderState;               /* 0 none, 1 running, 3 stopped */
static int reader_thread(SceSize args, void* argp)
{
    (void)args; (void)argp;
    gReaderState = 1;
    while (!gQuit) {
        if (!reader_step()) { SceUInt t = 20000; sceKernelWaitSema(gSema, 1, &t); }   /* woken by a new job; 20 ms otherwise */
    }
    file_close();
    gReaderState = 3;
    return 0;
}
#endif

/* ---- start-up (game thread, at the first drive or AI stream call) ------------------------------------------- */
static void dtk_init(void)
{
    if (gInit) return;
    coef_build();
#if defined(__vita__)
    {   FILE* f = fopen(DTK_DIR "dtk_off.txt", "rb");
        int c = f ? fgetc(f) : -1;
        if (f) fclose(f);
        dtk_mode = c == '1' ? 1u : c == '2' ? 2u : 0u;
        f = fopen(DTK_DIR "dtk_swap.txt", "rb");
        if (f) { gSwapLR = fgetc(f) == '1'; fclose(f); }
        f = fopen(DTK_DIR "dtk_realtime.txt", "rb");
        if (f) { gRealtime = fgetc(f) == '1'; fclose(f); }
        printf("[dtk] stream clock: %s\n", gRealtime ? "REAL TIME (dtk_realtime.txt 1: the stream ignores the game clock)"
                                                     : "GAME TIME when the game clock is the virtual one (holds of >= 10 ms, 3 ms fades; dtk_realtime.txt 1 = real time)");
    }
    gMtxOk = sceKernelCreateLwMutex(&gMtx, "stfx_dtk", 0, 0, NULL) >= 0;
    if (dtk_mode == 0) {
        gSema = sceKernelCreateSema("stfx_dtk_wake", 0, 0, 1, NULL);
        gThread = sceKernelCreateThread("stfx_dtk", reader_thread, 0x10000100, 0x8000, 0, 0, NULL);
        if (!gMtxOk || gSema < 0 || gThread < 0 || sceKernelStartThread(gThread, 0, NULL) < 0) {
            printf("[dtk] reader NOT started (mutex %d sema %08x thread %08x): SILENT, stream lengths kept\n", gMtxOk, (unsigned)gSema, (unsigned)gThread);
            dtk_mode = 1;
        }
    }
    printf("[dtk] disc audio stream: %s | read-ahead %u bytes (%u ms), chunk %u, 48 kHz -> 32 kHz by a 2 x %u-tap polyphase low-pass | low nibble = %s\n",
           dtk_mode == 0 ? "ON" : dtk_mode == 1 ? "SILENT (dtk_off.txt 1): same stream states and lengths, no read, no decode, no sound"
                                               : "LEGACY STUB (dtk_off.txt 2): every command succeeds, no stream ever ends",
           DTK_RING, DTK_RING * 1000u / (DTK_RATE * DTK_BLOCK_BYTES / DTK_BLOCK_FRAMES), DTK_CHUNK, DTK_TAPS,
           gSwapLR ? "RIGHT (dtk_swap.txt 1: Dolphin's output order)" : "left (dtk_swap.txt 1 = Dolphin's output order)");
#endif
    gInit = 1;
}

/* ---- the drive side (game thread) ------------------------------------------------------------------------------ */
void dtk_stream_prepare(const char* file, uint32_t base, uint32_t length, const char* label)
{
    DtkTrack t;
    int how;                    /* 0 started, 1 queued, 2 replaced */
    unsigned n;
    dtk_init();
    memset(&t, 0, sizeof t);
    snprintf(t.file, sizeof t.file, "%s", file ? file : "");
    snprintf(t.label, sizeof t.label, "%s", label ? label : "?");
    t.start = base; t.length = length & ~(DTK_BLOCK_BYTES - 1u);
    LOCK();
    n = ++dtk_prepared;
    if (dtk_mode == 2) { UNLOCK(); return; }
    if (gStream && !gStopAtEnd) { gNext = t; ++dtk_queued; how = 1; }
    else {
        how = gStream ? 2 : 0;
        if (gStream) ++dtk_replaced;
        gNext = t; gStream = 1; gStopAtEnd = 0; gFallback = 0; gInReset = 1;
        track_start(&gNext);
        gClockUs = DTK_NOW();
        if (gAiPlay) ++dtk_started;             /* the AI was left running: it pulls from the first grain on (or the next game tick) */
        anchor_reset();
    }
    UNLOCK();
    WAKE();
    printf("[dtk] prepare #%u %s: %u bytes at 0x%08x = %.2f s%s\n", n, t.label, (unsigned)t.length, (unsigned)t.start,
           (double)(t.length / DTK_BLOCK_BYTES) * DTK_BLOCK_FRAMES / DTK_RATE,
           how == 1 ? " (QUEUED behind the active stream)" : how == 2 ? " (REPLACED an active stream that had stop-at-end pending)" : "");
}

void dtk_stream_stop_at_end(void)
{
    dtk_init();
    LOCK();
    if (gStream) gStopAtEnd = 1;
    UNLOCK();
}

void dtk_stream_cancel(void)
{
    dtk_init();
    LOCK();
    ++dtk_cancel_calls;
    if (dtk_mode != 2 && gStream) { ++dtk_cancelled; stream_end(); gInReset = 1; }     /* silence at once: no tail */
    UNLOCK();
}

uint32_t dtk_stream_status(void)
{
    uint32_t st;
    char label[64];
    uint32_t bytes = 0;
    int ended;
    dtk_init();
    if (dtk_mode == 2) return 1;
    LOCK();
    /* The wall clock stands in for the audio thread when no grain has pulled for a while: the position is advanced
       by the time that passed, without data, so a stream still lasts its length. */
    if (gStream && gAiPlay) {
        unsigned long long now = DTK_NOW();
        if (!AUDIO_THREAD_RUNNING() && now > gClockUs && (gFallback || now - gClockUs > DTK_STALE_US)) {      /* once it stands in, it advances at every poll */
            unsigned long long blocks = (now - gClockUs) * DTK_RATE / (1000000ull * DTK_BLOCK_FRAMES), i;
            if (!gFallback) { gFallback = 1; job_set("", 0, 0); }
            for (i = 0; i < blocks && gStream; i++) { next_block(NULL, 1); ++dtk_fallback_blocks; }
            gClockUs += blocks * (1000000ull * DTK_BLOCK_FRAMES) / DTK_RATE;
        }
    }
    st = gStream ? 1u : 0u;
    ended = gEndPending; gEndPending = 0;
    if (ended) { memcpy(label, gEndLabel, sizeof label); bytes = gEndBytes; }
    UNLOCK();
    if (ended) printf("[dtk] ended %s after its %.2f s (stop-at-end): the status poll now answers 0\n", label,
                      (double)(bytes / DTK_BLOCK_BYTES) * DTK_BLOCK_FRAMES / DTK_RATE);
    return st;
}

uint32_t dtk_stream_play_offset(void)
{
    uint32_t p;
    LOCK();
    p = gStream ? (gPos & ~0x7fffu) : 0u;
    UNLOCK();
    return p;
}

/* ---- the AI side: the SDK's names (dolphin/ai.h), game thread ------------------------------------------------- */
void AISetStreamPlayState(uint32_t state)
{
    dtk_init();
    LOCK();
    state = state ? 1u : 0u;
    if ((int)state != gAiPlay) {
        gAiPlay = (int)state;
        if (state) { gClockUs = DTK_NOW(); if (gStream && dtk_mode != 2) ++dtk_started; }
        anchor_reset();                         /* a (re)start is anchored at the next game tick; a pause drops any hold */
    }
    UNLOCK();
}
uint32_t AIGetStreamPlayState(void) { return (uint32_t)gAiPlay; }
void     AISetStreamVolLeft(uint8_t vol)  { dtk_init(); gVolL = vol; }
void     AISetStreamVolRight(uint8_t vol) { dtk_init(); gVolR = vol; }
uint8_t  AIGetStreamVolLeft(void)  { return gVolL; }
uint8_t  AIGetStreamVolRight(void) { return gVolR; }
void     AISetStreamSampleRate(uint32_t rate) { gAiRate = rate; }      /* recorded only: disc streams are 48 kHz */
uint32_t AIGetStreamSampleRate(void) { return gAiRate; }
uint32_t AIGetStreamSampleCount(void) { return gAiSamples; }
void     AIResetStreamSampleCount(void) { LOCK(); gAiSamples = 0; UNLOCK(); }

/* ---- the game clock (game thread) ------------------------------------------------------------------------------ */
/* Called by os_shim.c at every retrace with the virtual clock's microseconds (only when that clock is in use). */
void dtk_stream_game_tick(long long virt_us)
{
    unsigned long long now;
    if (gRealtime || DTK_FAULT == 5 || virt_us < 0) return;
    now = DTK_NOW();
    LOCK();
    ++dtk_ticks;
    gClockSeen = 1; gVirtNow = virt_us; gTickReal = now;
    if (gStream && gAiPlay && dtk_mode != 2) {
        if (!gAnchored) {                       /* the start waited for this tick (or the clock appeared mid-stream) */
            gAnchored = 1; gGate = 0; gVirt0 = virt_us; gCout = 0;
            gLastExcess = gLastResid = 0;
        } else {
            long long v = (virt_us - gVirt0) * (long long)DTK_OUT_RATE / 1000000ll;     /* game time since the anchor, output frames */
            long long ex = (long long)gCout - v, pend = (long long)gDebt + (long long)gHoldLeft, need = ex - pend;
            if (need >= (long long)DTK_HOLD_MIN) gDebt += (unsigned)need;       /* smaller leads wait: they are added to by the next tick's */
            gLastExcess = ex; gLastResid = ex - ((long long)gDebt + (long long)gHoldLeft);
            if (ex > gExcessMax) gExcessMax = ex;
        }
    }
    UNLOCK();
}

/* The next chunk of the audio thread's output (lock held).  `want` frames are left in the batch; returns how many of
 * them are handled the same way, and *mode: 0 consumed as they are, 1 SILENT (the stream is not consumed; a hold's debt is paid), 4 silent because the game has
 * stopped ticking (no debt paid), 2 consumed
 * with a fade-out ramp, 3 consumed with a fade-in ramp.  The ramp is gFadeC / DTK_FADE per frame. */
static unsigned hold_chunk(unsigned want, unsigned long long now, int* mode)
{
    const int stale = (now > gTickReal) && (now - gTickReal > DTK_STALE_TICK_US);
    *mode = 0;
    if (!clock_on() || !gAnchored) { gPhase = 0; gDebt = gHoldLeft = 0; return want; }
    for (;;) {
        switch (gPhase) {
        case 0:
            if (!stale && gDebt < DTK_HOLD_MIN) return want;
            ++dtk_holds; if (stale) ++dtk_stale_holds;
            gPhase = 1; gFadeC = DTK_FADE; gHoldLeft += gDebt; gDebt = 0; gHoldCur = 0;
            break;
        case 1: {
            unsigned n = want < gFadeC ? want : gFadeC;
            *mode = 2; return n;                /* the caller commits with hold_done */
        }
        case 2: {
            unsigned n;
            gHoldLeft += gDebt; gDebt = 0;
            if (stale) { *mode = 4; return want; }
            if (gHoldLeft == 0) {
                if (gHoldCur > dtk_hold_max) dtk_hold_max = gHoldCur;
                gPhase = 3; gFadeC = 1; break;
            }
            n = want < gHoldLeft ? want : gHoldLeft;
            *mode = 1; return n;
        }
        default: {
            unsigned n = DTK_FADE - gFadeC + 1u;
            if (n > want) n = want;
            *mode = 3; return n;
        }
        }
    }
}
static void hold_done(int mode, unsigned n)
{
    if (mode == 1 || mode == 4) {
        if (mode == 1) { if (gHoldLeft >= n) gHoldLeft -= n; else gHoldLeft = 0; }     /* 4: waiting for the game's next tick: no debt is paid */
        dtk_hold_frames += n; gHoldCur += n;
    } else if (mode == 2 && gPhase == 1) {
        gFadeC -= n; if (gFadeC == 0) gPhase = 2;
    } else if (mode == 3 && gPhase == 3) {
        gFadeC += n; if (gFadeC > DTK_FADE) { gPhase = 0; gFadeC = 0; }
    }
}

/* ---- the audio thread ------------------------------------------------------------------------------------------ */
static inline int16_t add_sat(int a, int b)
{
    int v = a + b;
    if (v > 32767) { ++dtk_out_clamped; return 32767; }
    if (v < -32768) { ++dtk_out_clamped; return -32768; }
    return (int16_t)v;
}
void dtk_stream_mix(int16_t* out, unsigned frames, unsigned gain_pct)
{
    unsigned long long t0;
    unsigned us;
    if (!gInit || gQuit || dtk_mode == 2 || !out || !frames) return;
    t0 = DTK_NOW();
    LOCK();
    gClockUs = t0;
    if (gFallback) {                            /* the wall clock was pulling; the audio thread is back: read on from here */
        gFallback = 0;
        if (gStream && gPos < gCur.start + gCur.length) job_set(gCur.file, gPos, gCur.start + gCur.length - gPos);
    }
    if (gInReset) { gInReset = 0; gInHave = 0; gInPos2 = 0; gTail = 0; }
    if (dtk_mode == 1 || gJobErr) {             /* no data: only the timeline moves, 3 half frames per output frame */
        gInHave = 0; gInPos2 = 0; gTail = 0;
        if (gStream && gAiPlay && !gGate) {
            unsigned left = frames;
            while (left) {                      /* the same holds as the audible path: the stream lasts its length in game time */
                int hm; unsigned n = hold_chunk(left, t0, &hm);
                if (hm == 0 || hm == 2 || hm == 3) {
                    gSilentAcc += n * 3u; gCout += n;
                    while (gSilentAcc >= 2u * DTK_BLOCK_FRAMES) { gSilentAcc -= 2u * DTK_BLOCK_FRAMES; next_block(NULL, 0); }
                }
                hold_done(hm, n); left -= n;
                if (!(gStream && gAiPlay && !gGate)) break;      /* the track ended in there */
            }
        } else gSilentAcc = 0;
        UNLOCK();
        return;
    }
    while (frames) {
        unsigned n = frames < DTK_BATCH ? frames : DTK_BATCH, need, i, used;
        const int live = gStream && gAiPlay && !gGate;
        int hm = 0;
        const int vl = gVolL + (gVolL >> 7), vr = gVolR + (gVolR >> 7);
        if (!live && gTail <= 0) { gInHave = 0; gInPos2 = 0; break; }     /* nothing to play and nothing left inside the filter */
        if (live) {
            n = hold_chunk(n, t0, &hm);
            if (hm == 1 || hm == 4) {           /* a hold: the stream is not consumed, nothing is added; the filter stands still */
                FLOG(hm, n); hold_done(hm, n); out += n * 2u; frames -= n;
                continue;
            }
        } else if (gPhase) { gPhase = 0; gDebt = gHoldLeft = 0; }
        need = ((gInPos2 + 3u * (n - 1u)) >> 1) + DTK_TAPS;
        while (gInHave < need) {
            if (next_block(gIn + gInHave * 2u, 0)) gTail = (int)(gInHave + DTK_BLOCK_FRAMES);   /* real audio reaches up to here */
            gInHave += DTK_BLOCK_FRAMES;
        }
        for (i = 0; i < n; i++) {
            int l, r;
            fir_frame(gIn, gInPos2, &l, &r);
            gInPos2 += 3u;
            if (gSwapLR) { int t = l; l = r; r = t; }
            l = (l * vl) >> 8; r = (r * vr) >> 8;
            if (hm == 2) { int k = (int)(gFadeC - i); l = l * k / (int)DTK_FADE; r = r * k / (int)DTK_FADE; }       /* ramp down to the hold */
            else if (hm == 3) { int k = (int)(gFadeC + i); l = l * k / (int)DTK_FADE; r = r * k / (int)DTK_FADE; }  /* ramp up out of it */
            if (gain_pct != 100u) { l = l * (int)gain_pct / 100; r = r * (int)gain_pct / 100; }
            out[2 * i]     = add_sat(out[2 * i], l);
            out[2 * i + 1] = add_sat(out[2 * i + 1], r);
        }
        used = gInPos2 >> 1;
        memmove(gIn, gIn + used * 2u, (size_t)(gInHave - used) * 2u * sizeof(int16_t));
        gInHave -= used; gInPos2 &= 1u; gTail -= (int)used;
        FLOG(live ? hm : 5, n);                 /* 5: the filter tail (not live) */
        if (live) { dtk_frames_out += n; gCout += n; hold_done(hm, n); }
        out += n * 2u; frames -= n;
    }
    UNLOCK();
    WAKE();                                     /* the ring has room again */
    us = (unsigned)(DTK_NOW() - t0);
    if (us > dtk_mix_us_max) dtk_mix_us_max = us;
}

/* ---- housekeeping (game thread) -------------------------------------------------------------------------------- */
/* WORKING, while a line of dialogue plays: "now <name> ACTIVE ai 1", pos climbing at 54,857 bytes a second
 * (1.00 s of stream per second), frames decoded +48000 a second, out +32000 a second, ring near 32768, underruns
 * and failures 0, vol non-zero; and when it is over "[dtk] ended <name> after its N s" with ended +1. */
void dtk_stream_report(void)
{
    DtkTrack cur;
    char fail[200];
    unsigned ring, pos = 0, stream, stop, ai, fb, jerr;
    uint32_t failoff;
    long long gpos_ms = 0, spos_ms = 0, ex_ms, resid_ms, exmax_ms, tick_age_ms = 0;
    int anch, clk;
    if (!gInit) { printf("[dtk] no disc audio stream call yet\n"); return; }
    LOCK();
    cur = gCur; stream = (unsigned)gStream; stop = (unsigned)gStopAtEnd; ai = (unsigned)gAiPlay; fb = (unsigned)gFallback; jerr = (unsigned)gJobErr;
    ring = gRingW - gRingR;
    if (gStream) pos = gPos - gCur.start;
    memcpy(fail, gFailPath, sizeof fail); failoff = gFailOff;
    anch = gAnchored; clk = clock_on();
    spos_ms = (long long)(gCout * 1000ull / DTK_OUT_RATE);
    gpos_ms = anch ? (gVirtNow - gVirt0) / 1000 : 0;
    ex_ms = gLastExcess * 1000 / (long long)DTK_OUT_RATE; resid_ms = gLastResid * 1000 / (long long)DTK_OUT_RATE; exmax_ms = gExcessMax * 1000 / (long long)DTK_OUT_RATE;
    if (clk && gTickReal) tick_age_ms = (long long)((DTK_NOW() - gTickReal) / 1000ull);
    UNLOCK();
    printf("[dtk] %s | prepared %u started %u ended %u cancelled %u (cancel calls %u, queued %u, replaced %u, loops %u) | now %s %s ai %u stop-at-end %u pos %u/%u bytes (%.2f/%.2f s)"
           " | frames decoded %u (48 kHz) out %u (32 kHz) | ring %u bytes, underruns %u | reads %u (%u KB) | open failures %u read errors %u%s%s%s"
           " | vol %u/%u | silent blocks %u, by the wall clock %u%s | clamped %u | mix us max %u"
           " | sync %s: held %u times, %.1f ms total (largest %.1f ms; %u by a stalled game), ticks %u | game-time position %lld ms vs stream position %lld ms,"
           " drift %+lld ms at the last tick (%+lld ms left after the holds queued; largest lead %+lld ms)%s\n",
           dtk_mode == 0 ? "ON" : dtk_mode == 1 ? "SILENT (dtk_off.txt 1)" : "LEGACY STUB (dtk_off.txt 2)",
           dtk_prepared, dtk_started, dtk_ended, dtk_cancelled, dtk_cancel_calls, dtk_queued, dtk_replaced, dtk_loops,
           cur.label[0] ? cur.label : "-", stream ? "ACTIVE" : "idle", ai, stop, pos, stream ? (unsigned)cur.length : 0u,
           (double)(pos / DTK_BLOCK_BYTES) * DTK_BLOCK_FRAMES / DTK_RATE, stream ? (double)(cur.length / DTK_BLOCK_BYTES) * DTK_BLOCK_FRAMES / DTK_RATE : 0.0,
           dtk_frames_decoded, dtk_frames_out, ring, dtk_underruns, dtk_reads, dtk_read_bytes >> 10,
           dtk_open_failures, dtk_read_errors, fail[0] ? " [FAILED: " : "", fail, fail[0] ? "]" : "",
           (unsigned)gVolL, (unsigned)gVolR, dtk_blocks_silent, dtk_fallback_blocks, fb ? " (NOW: no audio grain is pulling)" : "",
           dtk_out_clamped, dtk_mix_us_max,
           gRealtime ? "REAL TIME (dtk_realtime.txt 1)" : clk ? (anch ? "GAME TIME" : "GAME TIME (waiting for the next tick)") : "real time (no virtual clock yet)",
           dtk_holds, (double)dtk_hold_frames * 1000.0 / DTK_OUT_RATE, (double)dtk_hold_max * 1000.0 / DTK_OUT_RATE, dtk_stale_holds, dtk_ticks,
           gpos_ms, spos_ms, ex_ms, resid_ms, exmax_ms,
           tick_age_ms > 200 ? " [NO GAME TICK FOR >200 ms: the game is stalled; the stream is held]" : "");
    if (fail[0]) printf("[dtk] the failed read was at offset 0x%08x of %s%s\n", (unsigned)failoff, fail, jerr ? ": this stream is silent, its length is kept" : "");
}

void dtk_stream_shutdown(void)
{
    if (!gInit) return;
    gQuit = 1;
#if defined(__vita__)
    if (gThread >= 0) {
        int i;
        WAKE();
        for (i = 0; i < 500 && gReaderState == 1; i++) sceKernelDelayThread(1000);
        if (gReaderState != 1) { sceKernelDeleteThread(gThread); gThread = -1; printf("[dtk] reader stopped after %u reads (%u KB)\n", dtk_reads, dtk_read_bytes >> 10); }
        else printf("[dtk] reader did NOT stop in 0.5 s: left for process exit\n");
    }
#else
    file_close(); gOpenPath[0] = 0;
#endif
}

/* ---- host test seams -------------------------------------------------------------------------------------------- */
#ifndef __vita__
void dtk_host_reset(int mode)
{
    file_close(); gOpenPath[0] = 0;
    gQuit = 0;
    memset(&gCur, 0, sizeof gCur); memset(&gNext, 0, sizeof gNext); memset(&gAd, 0, sizeof gAd);
    gStream = gStopAtEnd = gAiPlay = 0; gPos = 0; gVolL = gVolR = 0; gAiRate = 1; gAiSamples = 0; gClockUs = 0; gFallback = 0; gEndPending = 0;
    gGen = 0; gJobFile[0] = 0; gJobOff = gJobLeft = 0; gJobErr = 0; gFailPath[0] = 0; gFailOff = 0; gRingW = gRingR = 0; gPrimed = 0;
    gInHave = gInPos2 = 0; gTail = 0; gSilentAcc = 0; gInReset = 0; gSwapLR = 0;
    dtk_prepared = dtk_started = dtk_ended = dtk_cancelled = dtk_cancel_calls = dtk_queued = dtk_replaced = dtk_loops = 0;
    dtk_frames_decoded = dtk_frames_out = dtk_underruns = dtk_blocks_silent = dtk_reads = dtk_read_bytes = 0;
    dtk_open_failures = dtk_read_errors = dtk_mix_us_max = dtk_fallback_blocks = dtk_out_clamped = 0;
    dtk_host_now_us = 0; dtk_host_audio_running = 0;
    gRealtime = 0; gClockSeen = 0; gGate = 0; gAnchored = 0; gVirtNow = gVirt0 = 0; gTickReal = 0; gCout = 0; gDebt = gHoldLeft = gHoldCur = 0;
    gPhase = 0; gFadeC = 0; gLastExcess = gLastResid = gExcessMax = 0;
    dtk_holds = dtk_stale_holds = dtk_hold_frames = dtk_hold_max = dtk_ticks = 0; dtk_host_flog_n = 0;
    dtk_mode = (unsigned)mode;
    coef_build();
    gInit = 1;
}
int dtk_host_reader_step(void) { return reader_step(); }
void dtk_host_swap(int on) { gSwapLR = on; }
void dtk_host_realtime(int on) { gRealtime = on; }
unsigned dtk_host_ring_bytes(void) { return gRingW - gRingR; }
/* The resampler alone, on PCM: in48 must hold (out_frames * 3 + 1) / 2 + DTK_TAPS frames (the caller zero-pads). */
void dtk_host_resample(const int16_t* in48, unsigned in_frames, int16_t* out32, unsigned out_frames)
{
    unsigned i;
    for (i = 0; i < out_frames; i++) {
        int l = 0, r = 0;
        if (((3u * i) >> 1) + DTK_TAPS <= in_frames) fir_frame(in48, 3u * i, &l, &r);
        out32[2 * i] = (int16_t)l; out32[2 * i + 1] = (int16_t)r;
    }
}
#endif
