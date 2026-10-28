/* dspmix.h -- software replacement for the GameCube audio DSP running the MusyX
 * ucode, as the Star Fox Adventures decomp drives it.
 *
 * STAGE 1 (host prototype). One translation unit (dspmix.c), C11, libc only.
 * Contract and every claim's evidence: DESIGN.md next to this file.
 *
 * One call to dspmix_run() == one 5 ms DSP frame (160 samples @ 32 kHz) == what the real
 * DSP does between the two mails salCtrlDsp() sends (sal_dsp.c:295-312).
 *
 * NOT hardware-verified: nothing here has been compared against a retail audio dump.
 */
#ifndef DSPMIX_H
#define DSPMIX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSPMIX_FRAME      160   /* samples per DSP frame (0x280 bytes of s16 stereo; sal_ai.c:13) */
#define DSPMIX_SUBFRAME    32   /* the ucode renders 32 samples at a time (ucode IMEM 0x0250: 5 passes) */
#define DSPMIX_SUBFRAMES    5   /* DSP_VOICE_UPDATE_COUNT (dsp_voice.h:37) */
#define DSPMIX_RATE     32000   /* sal_ai.c:16 SAL_AI_OUTPUT_SAMPLE_COUNT = 0x7d00 (reported as the rate) */

/* Bus order, identical to the ucode's DMEM table 0x0e08 and to _SPB's dpop order. */
enum { DM_L, DM_R, DM_S, DM_AL, DM_AR, DM_AS, DM_BL, DM_BR, DM_BS, DM_NBUS };

/* ---- the structures MusyX writes: exact copies of main/unknown/autos/musyx_dsp.h:8-88 ----
 * u16 fields are HOST-ENDIAN (MusyX C stores them natively; the real DSP read them big-endian
 * only because the CPU was). Do not byte-swap. */
typedef struct DspPB {
    uint16_t nextHi, nextLo;          /* 0x00 linked list of voices (host builds it, hw_dspctrl.c:875) */
    uint16_t currHi, currLo;          /* 0x04 this PB's own address (sal_studio.c:65) */
    uint16_t srcSelect, coefSelect;   /* 0x08 0 polyphase, 1 linear, 2 none (ucode IMEM 0x0b31 table) */
    uint16_t mixerCtrl;               /* 0x0c bit0 aux A, bit1 aux B, bit2 surround, bit3 ramp, bit4 DPL2 */
    uint16_t state;                   /* 0x0e 1 = render, else skip */
    uint16_t loopType;                /* 0x10 */
    uint16_t mix[18];                 /* 0x12 vL,dL, vR,dR, vAL,dAL, vAR,dAR, vBL,dBL, vBR,dBR, vBS,dBS, vS,dS, vAS,dAS */
    uint16_t itd[7];                  /* 0x36 flag, bufHi, bufLo, shiftL, shiftR, tgtL, tgtR (NOT implemented) */
    uint16_t updNum[5];               /* 0x44 */
    uint16_t updDataHi, updDataLo;    /* 0x4e patch buffer address */
    uint16_t dpop[9];                 /* 0x52 aL, aAL, aBL, aR, aAR, aBR, aS, aAS, aBS */
    uint16_t veVolume, veDelta;       /* 0x64 */
    uint16_t firNumCoefs, firCoefsHi, firCoefsLo; /* 0x68 unused by the code paths read */
    uint16_t loopFlag, format;        /* 0x6e */
    uint16_t loopAddrHi, loopAddrLo;
    uint16_t endAddrHi, endAddrLo;
    uint16_t curAddrHi, curAddrLo;
    uint16_t coef[8][2];              /* 0x7e ADPCM predictor pairs */
    uint16_t gain, predScale, yn1, yn2;
    uint16_t ratioHi, ratioLo, fracAddr;  /* 0xa6 */
    uint16_t lastSamples[4];
    uint16_t loopPredScale, loopYn1, loopYn2; /* 0xb4 */
    uint16_t streamLoopCnt;           /* 0xba */
} DspPB;

typedef struct DspSPB {               /* _SPB, musyx_dsp.h:78 : per-studio depop ramps */
    uint16_t dpop[9][3];              /* [bus] = { startHi, startLo, delta } */
} DspSPB;

/* mix[] indices */
enum { MX_VL = 0, MX_VR = 2, MX_VAL = 4, MX_VAR = 6, MX_VBL = 8, MX_VBR = 10, MX_VBS = 12, MX_VS = 14, MX_VAS = 16 };
/* dpop[] indices */
enum { DP_L, DP_AL, DP_BL, DP_R, DP_AR, DP_BR, DP_S, DP_AS, DP_BS };

/* PB.format values (hw_dspctrl.c:277,328,342) */
#define DSPMIX_FMT_ADPCM 0x00
#define DSPMIX_FMT_PCM16 0x0A
#define DSPMIX_FMT_PCM8  0x19

/* ---- configuration ---- */
typedef void* (*DspMixXlat)(uint32_t addr, void* user);

typedef struct DspMixCfg {
    uint8_t*   aram;        /* ARAM stand-in; PB addresses are offsets into it (ar_shim.c:13) */
    uint32_t   aram_size;
    /* Command-list / PB-link addresses are 32-bit values split into u16 halves (hw_dspctrl.c:192).
     * NULL xlat => identity cast, correct on the 32-bit Vita. A 64-bit host MUST supply xlat. */
    DspMixXlat xlat;
    void*      xlat_user;
    int        wrap_output;  /* 1 = wrap like the ucode (srri of AC.M in 40-bit mode, IMEM 0x0457); 0 = clamp (default) */
    int        no_surround_matrix; /* 1 = skip op17's "L=-S,R=+S" injection */
    /* How the SPB's 32-bit depop start values are laid out in memory.  The runtime does NOT write them as two u16
     * fields: DoDepopFade stores a native s32 through (s32*)&spb->dpopXHi (hw_dspctrl.c:67-83, 935-943).  On the
     * big-endian GameCube that is hi word, lo word; on a little-endian host the LOW half lands in "Hi".
     *   0 = two u16 words, hi then lo (the DSP's view; what the stage-1 reference model and its test build)
     *   1 = one native s32, as the runtime stores it: REQUIRED when the PBs come from the real runtime
     *       (stfx_audio.c sets it).  With 0 on a little-endian machine a depop value v is read as v << 16:
     *       a full-scale DC rail on the bus from the first voice that is cut off, for as long as the residue lasts. */
    int        spb_native32;
} DspMixCfg;

typedef struct DspMixStats {
    uint32_t frames, voices_mixed, voices_skipped;
    uint32_t aram_oob;          /* sample fetches outside ARAM (returned 0) */
    uint32_t bad_opcode;        /* unknown opcode: list abandoned, like the ucode's 0xbaad halt */
    uint32_t loops;             /* loop/end jumps taken */
    uint32_t coef_missing;      /* polyphase requested but no coefficient table: linear used */
    uint32_t itd_ignored;       /* voices with itd.flag != 0 (ITD not implemented) */
    uint32_t src_none_ratio;    /* srcSelect==2 voices (ratio ignored) */
    uint32_t depop_ramps;       /* studio buses started from a non-zero depop value (op 0) */
    uint32_t depop_peak;        /* largest |depop start value| seen: tens to a few thousand is normal, millions is a layout error */
    uint32_t out_clamped;       /* output samples that hit the 16-bit clamp (op 14) */
} DspMixStats;

typedef struct DspMix {
    DspMixCfg cfg;
    DspMixStats st;
    const int16_t* coefs;      /* 3 sets x 128 phases x 4 taps, or NULL */
    /* scratch: the DSP's DMEM buses */
    int32_t bus[DM_NBUS][DSPMIX_FRAME];
    int32_t* surround;         /* op17's address, host pointer */
} DspMix;

void dspmix_init(DspMix* m, const DspMixCfg* cfg);

/* The 3x512 s16 SRC coefficient ROM (DSP COEF 0x1000/0x1200/0x1400), 4096 bytes big-endian as
 * dumped by dsp_coef.bin. Copied. Without it polyphase degrades to linear (stats.coef_missing). */
void dspmix_set_coefs_be(DspMix* m, const uint8_t* be4096, int16_t* out_table /* 3*512 entries, caller-owned, must outlive m */);

/* Process one frame. `cmdlist` = the address mail #2 carries (salCtrlDsp, sal_dsp.c:306-309). */
void dspmix_run(DspMix* m, uint32_t cmdlist);

/* ---- pieces exposed for the unit test ---- */
/* Standard GC DSP-ADPCM frame: 8 bytes -> 14 samples. coef = 16 s16 (8 predictor pairs). */
void dspmix_adpcm_frame(const uint8_t frame[8], const int16_t coef[16], int16_t* yn1, int16_t* yn2, int16_t out[14]);

/* Stateful sample fetch exactly as the PB describes it (cur/end/loop are nibble addresses for ADPCM,
 * byte addresses for PCM8, 16-bit-word addresses for PCM16). Used by the voice renderer and the test. */
typedef struct DspFetch {
    const uint8_t* aram; uint32_t aram_size;
    uint32_t cur, end, loop;
    uint16_t format, gain, ps, loop_ps;
    int16_t  yn1, yn2, loop_yn1, loop_yn2;
    int16_t  coef[16];
    uint16_t loop_flag, loop_type;  /* PB.loopFlag / PB.loopType: ucode exception handlers @0x0c3b/0x0c50 */
    uint32_t oob, loops;
    uint32_t stopped;       /* end reached with loop_flag != 1: the ucode sets PB.state = 0 (takes effect next sub-frame) */
    uint32_t stream_loops;  /* loop_type == 1 jumps: the ucode increments PB.streamLoopCnt (@0x0c79) */
} DspFetch;
int16_t dspmix_fetch(DspFetch* f);

/* Resampler: one voice, N output samples. Mirrors the SRC state in the PB. */
typedef struct DspSrc {
    uint32_t ratio;        /* 16.16 */
    uint32_t frac;         /* 0..0xffff */
    int16_t  hist[4];      /* oldest .. newest fetched */
    int      mode;         /* 0 polyphase, 1 linear, 2 none */
    int      set;          /* coefficient set 0..2 */
    const int16_t* coefs;  /* 3*512 or NULL */
} DspSrc;
void dspmix_src_run(DspSrc* s, DspFetch* f, int16_t* out, int n);

#ifdef __cplusplus
}
#endif
#endif
