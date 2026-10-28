/* dspmix.c -- software MusyX DSP. See dspmix.h and DESIGN.md (every rule here has a citation there).
 * C11, libc only, one translation unit. Stage-1 host prototype: not hardware-verified. */
#include "dspmix.h"
#include <string.h>

_Static_assert(sizeof(DspPB) == 0xBC, "DspPB must match _PB (musyx_dsp.h:55-76)");
_Static_assert(sizeof(DspSPB) == 0x36, "DspSPB must match _SPB (musyx_dsp.h:78-88)");

#ifndef DSPMIX_FAULT
#define DSPMIX_FAULT 0   /* test-only deliberate faults, each MUST be caught by test_dspmix: 1 ADPCM rounding 0x400->0x3ff; 2 volume ramp off by one sample; 3 loop jump forgets yn1 */
#endif

static int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

/* --------------------------------------------------------------------------------------- */
/* ADPCM                                                                                   */
/* --------------------------------------------------------------------------------------- */

static int16_t adpcm_one(int nib, int scale, int idx, const int16_t* coef, int16_t* yn1, int16_t* yn2)
{
    int32_t s = (nib >= 8 ? nib - 16 : nib);
    int64_t v = ((int64_t)s * (1 << scale)) * 2048 + (DSPMIX_FAULT == 1 ? 0x3ff : 0x400)
              + (int64_t)coef[idx * 2] * *yn1 + (int64_t)coef[idx * 2 + 1] * *yn2;
    v >>= 11;                                  /* arithmetic shift (floor), as the accelerator */
    int16_t o = clamp16((int32_t)(v > 0x7fffffff ? 0x7fffffff : (v < -0x7fffffff ? -0x7fffffff : v)));
    *yn2 = *yn1;
    *yn1 = o;
    return o;
}

void dspmix_adpcm_frame(const uint8_t frame[8], const int16_t coef[16], int16_t* yn1, int16_t* yn2, int16_t out[14])
{
    int scale = frame[0] & 0xf, idx = (frame[0] >> 4) & 7;
    for (int i = 0; i < 14; i++) {
        uint8_t b = frame[1 + (i >> 1)];
        out[i] = adpcm_one((i & 1) ? (b & 0xf) : (b >> 4), scale, idx, coef, yn1, yn2);
    }
}

/* --------------------------------------------------------------------------------------- */
/* Sample fetch: the DSP accelerator (ACCAH/ACEAH/ACSAH + FORMAT/GAIN/PRED_SCALE/YN1/YN2). */
/* --------------------------------------------------------------------------------------- */

static uint8_t aram_byte(DspFetch* f, uint32_t a)
{
    if (a >= f->aram_size) { f->oob++; return 0; }
    return f->aram[a];
}

int16_t dspmix_fetch(DspFetch* f)
{
    int16_t out;
    switch (f->format) {
    case DSPMIX_FMT_PCM16: {
        int16_t raw = (int16_t)((aram_byte(f, f->cur * 2) << 8) | aram_byte(f, f->cur * 2 + 1)); /* ARAM is big-endian */
        out = clamp16(((int32_t)raw * f->gain) >> 11);   /* gain 0x800 = unity (hw_dspctrl.c:343) */
        break;
    }
    case DSPMIX_FMT_PCM8: {
        int8_t raw = (int8_t)aram_byte(f, f->cur);
        out = clamp16((int32_t)raw * f->gain);           /* gain 0x100: raw<<8. UNVERIFIED scaling; no PCM8 sample exists in the banks */
        break;
    }
    default: {                                            /* DSP-ADPCM */
        if ((f->cur & 15) == 0) {                         /* entering a frame: header byte -> pred/scale */
            f->ps = aram_byte(f, f->cur >> 1);
            f->cur += 2;
        }
        uint8_t b = aram_byte(f, f->cur >> 1);
        int nib = (f->cur & 1) ? (b & 0xf) : (b >> 4);
        out = adpcm_one(nib, f->ps & 0xf, (f->ps >> 4) & 7, f->coef, &f->yn1, &f->yn2);
        break;
    }
    }
    if (f->cur == f->end) {                               /* end -> accelerator wraps to the loop address (ACSA) */
        f->cur = f->loop;
        f->loops++;
        if (f->loop_flag != 1) {                          /* ucode @0x0c3b/0x0c5f: not looping -> voice state := 0 */
            f->stopped++;
        } else if (f->loop_type == 1) {                   /* stream loop @0x0c71: only PRED_SCALE restored, yn kept, counter++ */
            f->ps = f->loop_ps;
            f->stream_loops++;
        } else {                                          /* plain loop @0x0c65: PRED_SCALE, YN1, YN2 from the loop fields */
            f->ps = f->loop_ps; if (DSPMIX_FAULT != 3) f->yn1 = f->loop_yn1; f->yn2 = f->loop_yn2;
        }
    } else {
        f->cur++;
    }
    return out;
}

/* --------------------------------------------------------------------------------------- */
/* Sample-rate converter                                                                   */
/* --------------------------------------------------------------------------------------- */

void dspmix_src_run(DspSrc* s, DspFetch* f, int16_t* out, int n)
{
    for (int i = 0; i < n; i++) {
        if (s->mode == 2) { out[i] = dspmix_fetch(f); continue; }
        if (s->mode == 0 && s->coefs) {
            const int16_t* c = s->coefs + s->set * 512 + (s->frac >> 9) * 4;
            int64_t acc = (int64_t)c[0] * s->hist[0] + (int64_t)c[1] * s->hist[1]
                        + (int64_t)c[2] * s->hist[2] + (int64_t)c[3] * s->hist[3];
            out[i] = clamp16((int32_t)((acc * 2) >> 16));
        } else {
            int32_t a = s->hist[1], b = s->hist[2];
            out[i] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)s->frac) >> 16));
        }
        s->frac += s->ratio;
        while (s->frac >= 0x10000) {
            s->frac -= 0x10000;
            s->hist[0] = s->hist[1]; s->hist[1] = s->hist[2]; s->hist[2] = s->hist[3];
            s->hist[3] = dspmix_fetch(f);
        }
    }
}

/* --------------------------------------------------------------------------------------- */
/* Helpers                                                                                 */
/* --------------------------------------------------------------------------------------- */

static void* X(DspMix* m, uint32_t a)
{
    return m->cfg.xlat ? m->cfg.xlat(a, m->cfg.xlat_user) : (void*)(uintptr_t)a;
}
static uint32_t U32(const uint16_t* p) { return ((uint32_t)p[0] << 16) | p[1]; }
static uint32_t pbadr(uint16_t hi, uint16_t lo) { return ((uint32_t)hi << 16) | lo; }

void dspmix_init(DspMix* m, const DspMixCfg* cfg)
{
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
}

void dspmix_set_coefs_be(DspMix* m, const uint8_t* be, int16_t* table)
{
    for (int i = 0; i < 3 * 512; i++) table[i] = (int16_t)((be[2 * i] << 8) | be[2 * i + 1]);
    m->coefs = table;
}

/* 32-bit bus value stored in host s32 triples: ch 0 at +0, ch 1 at +160, ch 2 at +320 (sal_studio.c:94-100, hw_dspctrl.c:1085-1087) */

/* --------------------------------------------------------------------------------------- */
/* One voice                                                                               */
/* --------------------------------------------------------------------------------------- */

typedef struct { int bus; int volidx; } MixRoute;   /* volidx indexes pb->mix[] */

static int mix_routes(uint16_t ctrl, MixRoute* r)
{
    int c = ctrl & 0x1f, lo = c & 7, n = 0;
    r[n++] = (MixRoute){ DM_L, MX_VL };
    r[n++] = (MixRoute){ DM_R, MX_VR };
    if (c & 16) {                                   /* DPL2 aux-B pair (ucode IMEM 0x0a39 / 0x0a5b); entries with lo>1 fall to plain */
        if (lo <= 1) {
            r[n++] = (MixRoute){ DM_BL, MX_VBL };
            r[n++] = (MixRoute){ DM_BR, MX_VBR };
            if (lo == 1) { r[n++] = (MixRoute){ DM_AL, MX_VAL }; r[n++] = (MixRoute){ DM_AR, MX_VAR }; r[n++] = (MixRoute){ DM_AS, MX_VAS }; }
        }
        return n;
    }
    if (lo & 1) { r[n++] = (MixRoute){ DM_AL, MX_VAL }; r[n++] = (MixRoute){ DM_AR, MX_VAR }; }
    if (lo & 2) { r[n++] = (MixRoute){ DM_BL, MX_VBL }; r[n++] = (MixRoute){ DM_BR, MX_VBR }; }
    if (lo & 4) {
        r[n++] = (MixRoute){ DM_S, MX_VS };
        if (lo == 5 || lo == 7) r[n++] = (MixRoute){ DM_AS, MX_VAS };
        if (lo == 6 || lo == 7) r[n++] = (MixRoute){ DM_BS, MX_VBS };
    }
    return n;
}

static void apply_patches(DspPB* pb, const uint16_t** pp, int count)
{
    uint16_t* w = (uint16_t*)pb;
    for (int i = 0; i < count; i++, *pp += 2) {
        unsigned idx = (*pp)[0];
        if (idx < sizeof(DspPB) / 2) w[idx] = (*pp)[1];
    }
}

static void render_voice(DspMix* m, DspPB* pb)
{
    uint16_t* w = (uint16_t*)pb;
    const uint16_t* patch = (const uint16_t*)X(m, pbadr(pb->updDataHi, pb->updDataLo));
    uint16_t upd[5];
    memcpy(upd, pb->updNum, sizeof upd);

    DspFetch f;
    memset(&f, 0, sizeof f);
    f.aram = m->cfg.aram; f.aram_size = m->cfg.aram_size;
    f.cur = pbadr(pb->curAddrHi, pb->curAddrLo);
    f.end = pbadr(pb->endAddrHi, pb->endAddrLo);
    f.loop = pbadr(pb->loopAddrHi, pb->loopAddrLo);
    f.format = pb->format; f.gain = pb->gain;
    f.ps = pb->predScale; f.yn1 = (int16_t)pb->yn1; f.yn2 = (int16_t)pb->yn2;
    f.loop_flag = pb->loopFlag; f.loop_type = pb->loopType;
    f.loop_ps = pb->loopPredScale; f.loop_yn1 = (int16_t)pb->loopYn1; f.loop_yn2 = (int16_t)pb->loopYn2;
    for (int i = 0; i < 8; i++) { f.coef[2 * i] = (int16_t)pb->coef[i][0]; f.coef[2 * i + 1] = (int16_t)pb->coef[i][1]; }

    DspSrc s;
    memset(&s, 0, sizeof s);
    s.frac = pb->fracAddr;
    for (int i = 0; i < 4; i++) s.hist[i] = (int16_t)pb->lastSamples[i];
    s.set = pb->coefSelect > 2 ? 0 : pb->coefSelect;
    s.coefs = m->coefs;

    int itd = pb->itd[0] != 0;
    int any = 0;

    for (int sf = 0; sf < DSPMIX_SUBFRAMES; sf++) {
        if (patch) apply_patches(pb, &patch, upd[sf]);
        if (pb->state != 1) continue;
        any = 1;

        /* SRC mode / ratio are re-read every sub-frame: the host patches ratio at 0x53/0x54 (hw_dspctrl.c:772-775) */
        s.ratio = pbadr(pb->ratioHi, pb->ratioLo);
        s.mode = pb->srcSelect;
        if (s.mode == 0 && !s.coefs) { m->st.coef_missing++; s.mode = 1; }
        if (s.mode > 2) s.mode = 1;
        if (s.mode == 2) m->st.src_none_ratio++;

        int16_t src[DSPMIX_SUBFRAME];
        dspmix_src_run(&s, &f, src, DSPMIX_SUBFRAME);

        /* volume envelope (adsrHandle output, synth_adsr.c:166-199): e_i = vol + i*delta, applied (x*e*2)>>16 */
        uint16_t evol = pb->veVolume; int16_t edel = (int16_t)pb->veDelta;
        for (int i = 0; i < DSPMIX_SUBFRAME; i++) {
            uint16_t e = (uint16_t)(evol + i * edel);
            src[i] = clamp16((int32_t)(((int64_t)src[i] * e * 2) >> 16));
        }
        pb->veVolume = (uint16_t)(evol + DSPMIX_SUBFRAME * edel);

        MixRoute r[9];
        int nr = mix_routes(pb->mixerCtrl, r);
        int ramp = (pb->mixerCtrl & 8) != 0;
        for (int k = 0; k < nr; k++) {
            uint16_t vol = pb->mix[r[k].volidx];
            int16_t del = (int16_t)pb->mix[r[k].volidx + 1];
            int32_t* dst = m->bus[r[k].bus] + (sf * DSPMIX_SUBFRAME);
            int32_t last = 0;
            for (int i = 0; i < DSPMIX_SUBFRAME; i++) {
                uint16_t v = ramp ? (uint16_t)(vol + (i + (DSPMIX_FAULT == 2)) * del) : vol;
                last = (int32_t)(((int64_t)src[i] * v * 2) >> 16);   /* vol is unsigned 16-bit: 0x8000 = 1.0 */
                dst[i] += last;
            }
            if (ramp) pb->mix[r[k].volidx] = (uint16_t)(vol + DSPMIX_SUBFRAME * del);
            /* depop feedback: the last sample's contribution to this bus (ucode IMEM 0x0775 etc.) */
            static const int dp[DM_NBUS] = { DP_L, DP_R, DP_S, DP_AL, DP_AR, DP_AS, DP_BL, DP_BR, DP_BS };
            pb->dpop[dp[r[k].bus]] = (uint16_t)(int16_t)last;
        }
        if (f.stopped) { pb->state = 0; f.stopped = 0; }
        if (itd) m->st.itd_ignored++;
    }
    (void)w;
    if (any) m->st.voices_mixed++; else m->st.voices_skipped++;

    /* feedback MusyX reads back (see DESIGN.md section 5) */
    pb->curAddrHi = (uint16_t)(f.cur >> 16); pb->curAddrLo = (uint16_t)f.cur;
    pb->fracAddr = (uint16_t)s.frac;
    for (int i = 0; i < 4; i++) pb->lastSamples[i] = (uint16_t)s.hist[i];
    pb->predScale = f.ps; pb->yn1 = (uint16_t)f.yn1; pb->yn2 = (uint16_t)f.yn2;
    pb->streamLoopCnt = (uint16_t)(pb->streamLoopCnt + f.stream_loops);
    m->st.aram_oob += f.oob;
    m->st.loops += f.loops;
}

/* --------------------------------------------------------------------------------------- */
/* Command list                                                                            */
/* --------------------------------------------------------------------------------------- */

static void bus_to_buf(const int32_t b[DM_NBUS][DSPMIX_FRAME], int first, int32_t* buf)
{
    for (int c = 0; c < 3; c++) memcpy(buf + c * DSPMIX_FRAME, b[first + c], DSPMIX_FRAME * sizeof(int32_t));
}
static void buf_add_bus(int32_t b[DM_NBUS][DSPMIX_FRAME], int first, const int32_t* buf, int nch)
{
    for (int c = 0; c < nch; c++)
        for (int i = 0; i < DSPMIX_FRAME; i++) b[first + c][i] += buf[c * DSPMIX_FRAME + i];
}

void dspmix_run(DspMix* m, uint32_t cmdlist)
{
    const uint16_t* p = (const uint16_t*)X(m, cmdlist);
    uint32_t first_pb = 0;
    int master_seen = 0; (void)master_seen;
    m->st.frames++;

    for (int guard = 0; guard < 4096 && p; guard++) {
        switch (p[0]) {
        case 0: {                                           /* studio start: buses := depop ramps */
            const DspSPB* spb = (const DspSPB*)X(m, U32(p + 1));
            for (int b = 0; b < DM_NBUS; b++) {
                int32_t start;
                int16_t delta = (int16_t)spb->dpop[b][2];
                if (m->cfg.spb_native32) memcpy(&start, &spb->dpop[b][0], 4);   /* as DoDepopFade stored it (hw_dspctrl.c:81): one native s32 */
                else start = (int32_t)U32(&spb->dpop[b][0]);                    /* two u16 words, hi then lo */
                if (start) { uint32_t mag = (uint32_t)(start < 0 ? -(int64_t)start : start); m->st.depop_ramps++; if (mag > m->st.depop_peak) m->st.depop_peak = mag; }
                for (int i = 0; i < DSPMIX_FRAME; i++)
                    m->bus[b][i] = start ? start + i * (int32_t)delta : 0;   /* ucode: start==0 -> zero-fill (IMEM 0x009e) */
            }
            p += 3; break;
        }
        case 1: {                                           /* studio input: another studio's main, scaled */
            const int32_t* src = (const int32_t*)X(m, U32(p + 1));
            int32_t v[3] = { p[3], p[4], p[5] };             /* vol -> main, volA -> aux A, volB -> aux B */
            for (int g = 0; g < 3; g++)
                for (int c = 0; c < 3; c++)
                    for (int i = 0; i < DSPMIX_FRAME; i++)
                        m->bus[g * 3 + c][i] += (int32_t)(((int64_t)src[c * DSPMIX_FRAME + i] * v[g] * 2) >> 16);
            p += 6; break;
        }
        case 2: first_pb = U32(p + 1); p += 3; break;       /* first PB of the voice chain */
        case 3: {                                           /* run the chain */
            uint32_t a = first_pb;
            for (int n = 0; a && n < 256; n++) {
                DspPB* pb = (DspPB*)X(m, a);
                render_voice(m, pb);
                a = pbadr(pb->nextHi, pb->nextLo);
            }
            first_pb = 0; p += 1; break;
        }
        case 4: case 5: case 16: {                          /* aux A / aux B / DPL2 aux B */
            int32_t* store = (int32_t*)X(m, U32(p + 1));
            const int32_t* get = (const int32_t*)X(m, U32(p + 3));
            int first = (p[0] == 4) ? DM_AL : DM_BL;
            bus_to_buf(m->bus, first, store);
            if (p[0] != 16) buf_add_bus(m->bus, DM_L, get, 3);  /* host-processed (reverb) result back into main, unity */
            /* op16 (DPL2 matrix, ucode IMEM 0x0bb1) NOT implemented: stored only */
            p += 5; break;
        }
        case 6: bus_to_buf(m->bus, DM_L, (int32_t*)X(m, U32(p + 1))); p += 3; break;  /* studio main -> its main[salFrame] */
        case 9: buf_add_bus(m->bus, DM_L, (const int32_t*)X(m, U32(p + 1)), 3); p += 3; break; /* master main accumulates */
        case 13: p = (const uint16_t*)X(m, U32(p + 1)); break;  /* continue list at the next chunk */
        case 17: {                                           /* output buses := surround of previous frame, anti-phase */
            m->surround = (int32_t*)X(m, U32(p + 1));
            for (int i = 0; i < DSPMIX_FRAME; i++) {
                int32_t sv = m->cfg.no_surround_matrix ? 0 : m->surround[i];
                m->bus[DM_L][i] = -sv; m->bus[DM_R][i] = sv; m->bus[DM_S][i] = 0;
            }
            p += 3; break;
        }
        case 14: {                                           /* export S, emit stereo s16 */
            int32_t* sur = (int32_t*)X(m, U32(p + 1));
            int16_t* dst = (int16_t*)X(m, U32(p + 3));
            memcpy(sur, m->bus[DM_S], DSPMIX_FRAME * sizeof(int32_t));
            for (int i = 0; i < DSPMIX_FRAME; i++) {
                int32_t l = m->bus[DM_L][i], r = m->bus[DM_R][i];
                if (m->cfg.wrap_output) { dst[2 * i] = (int16_t)l; dst[2 * i + 1] = (int16_t)r; }
                else { dst[2 * i] = clamp16(l); dst[2 * i + 1] = clamp16(r); if (l > 32767 || l < -32768) m->st.out_clamped++; if (r > 32767 || r < -32768) m->st.out_clamped++; }
            }
            p += 5; break;
        }
        case 15: return;
        default: m->st.bad_opcode++; return;
        }
    }
}
