/* audio_tables_endian.c -- see audio_tables_endian.h and TABLES.md (field maps with the game file:line for each rule).
 *
 * House style (musyx_endian.c, mldf_endian.c): swap in place, once, with counters; a refused buffer is untouched.
 * Two phases: *_validate() reads and writes nothing; *_apply() is the only code that writes.
 *
 * ATE_FAULT=k compiles a deliberately wrong rule in (the test's fault arms; each MUST be caught):
 *   1 Sfx.bin: selectRange (u16 at +28) not swapped            2 Sfx.bin: E byte (tableIdx/bit3/pad/bit0) not remapped
 *   3 Sfx.bin: weights[6] (a byte array) swapped as u16 pairs  4 Music.bin: byte 15 (priorityGroup) not remapped
 *   5 Streams.bin: volume byte remapped with the wrong rule     6 Streams.bin: name[] swapped as u16 pairs
 *   7 Sfx.bin: the ascending-id validation removed (a guard fault: the descending-id buffer must be REFUSED)
 *   8 Streams.bin: fade byte remapped as a nibble swap instead of a 2-bit-group reversal
 */
#include "audio_tables_endian.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef ATE_FAULT
#define ATE_FAULT 0
#endif

/* Big-endian host: nothing to do (the PowerPC layouts ARE the host layouts). */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ATE_HOST_BE 1
#else
#define ATE_HOST_BE 0
#endif

static char g_err[200];
static stfx_ate_stats g_stats;

const char* stfx_audio_tables_error(void) { return g_err; }
const stfx_ate_stats* stfx_audio_tables_stats(void) { return &g_stats; }
void stfx_audio_tables_reset_stats(void) { memset(&g_stats, 0, sizeof g_stats); g_err[0] = 0; }

static int refuse(int code, size_t off, const char* fmt, ...)
{
    char msg[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(g_err, sizeof g_err, "%s (at 0x%zx)", msg, off);
    ++g_stats.refused;
    return code;
}

/* ---------------------------------------------------------------------------------------------- primitives */
static unsigned rd16be(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd16le(const uint8_t* p) { return (unsigned)p[1] << 8 | p[0]; }
#if !ATE_HOST_BE || ATE_FAULT
static void sw16(uint8_t* p) { uint8_t t = p[0]; p[0] = p[1]; p[1] = t; }
#endif

/* Bit-field storage byte, PowerPC -> ARM.  w[] = member widths in DECLARATION order (they sum to 8).  PowerPC allocates
 * the first member from bit 7 down; ARM EABI little-endian from bit 0 up.  Member i keeps its width and value; only its
 * position moves.  (Evidence: TABLES.md section 0, and test_audio_tables.py's ARM-compiler layout arm.) */
static uint8_t bf_ppc_to_arm(uint8_t p, const uint8_t* w, int nf)
{
    int from = 8, to = 0, i;
    unsigned a = 0;
    for (i = 0; i < nf; i++) {
        unsigned v;
        from -= w[i];
        v = ((unsigned)p >> from) & ((1u << w[i]) - 1u);
        a |= v << to;
        to += w[i];
    }
    return (uint8_t)a;
}
static void bf_fix(uint8_t* p, const uint8_t* w, int nf)
{
    uint8_t before = *p;
#if !ATE_HOST_BE
    *p = bf_ppc_to_arm(before, w, nf);
#else
    (void)w; (void)nf;
#endif
    ++g_stats.bitfield_bytes;
    if (*p != before) ++g_stats.bitfield_bytes_changed;
}
static void fix16(uint8_t* p)
{
#if !ATE_HOST_BE
    sw16(p);
#else
    (void)p;
#endif
    ++g_stats.fields16;
}

/* Declaration order of each bit-field byte (include/main/audio_internal.h):
 *   SfxTriggerFull.+30: e_tableIdx:4 e_bit3:1 e_pad:2 e_bit0:1        (:161-164)
 *   SfxTriggerFull.+31: f_count:4 f_curIdx:4                          (:165-166)
 *   MusicTrigger.+15:   pad0FHigh:2 priorityGroup:1 pad0FLow:5        (:96-98)
 *   StreamEntry.+2:     fadeModeA:2 fadeModeB:2 stopObjectSounds:2 unusedFade:2   (:76-79)
 *   StreamEntry.+3:     fullVolume:1 volume:7                          (:80-81)                                       */
static const uint8_t W_SFX_E[]   = { 4, 1, 2, 1 };
static const uint8_t W_SFX_F[]   = { 4, 4 };
static const uint8_t W_MUS_F[]   = { 2, 1, 5 };
static const uint8_t W_STR_A[]   = { 2, 2, 2, 2 };
static const uint8_t W_STR_B[]   = { 1, 7 };

/* ---------------------------------------------------------------------------------------------- Sfx.bin */
/* record (32): +0 u16 id | +2 volBase +3 volRand +4 pitchBase +5 pitchRand | +6 u16 nearDistanceRaw +8 u16 farDistanceRaw
 *              | +10 u16 sfxIds[6] | +22 u8 weights[6] | +28 u16 selectRange | +30 E bit-field byte | +31 F bit-field byte */
static int sfx_validate(const uint8_t* b, size_t n, int little)
{
    size_t i, cnt = n / STFX_SFX_RECORD_SIZE;
    unsigned prev = 0;
    for (i = 0; i < cnt; i++) {
        const uint8_t* r = b + i * STFX_SFX_RECORD_SIZE;
        unsigned id = little ? rd16le(r) : rd16be(r);
        /* bit-field bytes are PowerPC layout in a fresh file, ARM layout in an already-swapped one */
        unsigned fcount = little ? (r[31] & 15u) : (r[31] >> 4);
        unsigned fcur   = little ? (r[31] >> 4) : (r[31] & 15u);
        unsigned tidx   = little ? (r[30] & 15u) : (r[30] >> 4);
        /* audio_sfx.c:701-731 is a binary search: ids must be strictly ascending */
#if ATE_FAULT != 7
        if (i && id <= prev) return refuse(STFX_ATE_E_FORMAT, i * 32, "sfx record %zu: id %u not above previous %u", i, id, prev);
#endif
        prev = id;
        /* sfxIds[6] / weights[6]: idx < f_count must stay inside the arrays (audio_sfx.c:646-662) */
        if (fcount > 6) return refuse(STFX_ATE_E_FORMAT, i * 32 + 31, "sfx record %zu: f_count %u > 6", i, fcount);
        /* f_curIdx is the runtime's "last pick"; a fresh file has it < max(f_count,1) (audio_sfx.c:633-660) */
        if (fcur >= (fcount ? fcount : 1u)) return refuse(STFX_ATE_E_FORMAT, i * 32 + 31, "sfx record %zu: f_curIdx %u >= f_count %u", i, fcur, fcount);
        /* gSfxTriggerExtraTable[8] (audio_sfx.c:29, :695) */
        if (tidx > 7) return refuse(STFX_ATE_E_FORMAT, i * 32 + 30, "sfx record %zu: e_tableIdx %u > 7", i, tidx);
    }
    return 0;
}
static void sfx_apply(uint8_t* b, size_t n)
{
    size_t i, cnt = n / STFX_SFX_RECORD_SIZE;
    int k;
    for (i = 0; i < cnt; i++) {
        uint8_t* r = b + i * STFX_SFX_RECORD_SIZE;
        fix16(r + 0);
        fix16(r + 6);
        fix16(r + 8);
        for (k = 0; k < 6; k++) fix16(r + 10 + 2 * k);
#if ATE_FAULT != 1
        fix16(r + 28);
#endif
#if ATE_FAULT == 3
        for (k = 0; k < 3; k++) fix16(r + 22 + 2 * k);
#endif
#if ATE_FAULT != 2
        bf_fix(r + 30, W_SFX_E, 4);
#endif
        bf_fix(r + 31, W_SFX_F, 2);
    }
    g_stats.sfx_records += (unsigned)cnt;
}

/* ---------------------------------------------------------------------------------------------- Music.bin */
/* record (16): +0 u16 id | +2 u16 track | +4 u16 fadeTime | +6 u16 speed | +8 u8 pad08[4] | +12 u8 volume | +13 u8 priority
 *              | +14 u8 pad0E | +15 bit-field byte (pad0FHigh:2 priorityGroup:1 pad0FLow:5) */
static int music_validate(const uint8_t* b, size_t n, int little)
{
    size_t i, cnt = n / STFX_MUSIC_RECORD_SIZE;
    unsigned prev = 0;
    for (i = 0; i < cnt; i++) {
        const uint8_t* r = b + i * STFX_MUSIC_RECORD_SIZE;
        unsigned id = little ? rd16le(r) : rd16be(r);
        unsigned track = little ? rd16le(r + 2) : rd16be(r + 2);
        /* Music_FindTrackSlot walks sMusicTrackTable's 100 slots (audio.c:189-198); musicInitMidiWad tracks 0..0x63 (:1044) */
        if (track > 99) return refuse(STFX_ATE_E_FORMAT, i * 16 + 2, "music record %zu: track %u > 99", i, track);
        /* not required by the linear search, but true of the whole disc table: it makes a double swap detectable */
        if (i && id <= prev) return refuse(STFX_ATE_E_FORMAT, i * 16, "music record %zu: id %u not above previous %u", i, id, prev);
        prev = id;
    }
    return 0;
}
static void music_apply(uint8_t* b, size_t n)
{
    size_t i, cnt = n / STFX_MUSIC_RECORD_SIZE;
    for (i = 0; i < cnt; i++) {
        uint8_t* r = b + i * STFX_MUSIC_RECORD_SIZE;
        fix16(r + 0); fix16(r + 2); fix16(r + 4); fix16(r + 6);
#if ATE_FAULT != 4
        bf_fix(r + 15, W_MUS_F, 3);
#endif
    }
    g_stats.music_records += (unsigned)cnt;
}

/* ---------------------------------------------------------------------------------------------- Streams.bin */
/* record (22): +0 u16 id | +2 bit-field byte (fadeModeA:2 fadeModeB:2 stopObjectSounds:2 unusedFade:2)
 *              | +3 bit-field byte (fullVolume:1 volume:7) | +4 u16 lengthRaw | +6 char name[15] | +21 u8 flag */
static int streams_validate(const uint8_t* b, size_t n, int little)
{
    size_t i, cnt = n / STFX_STREAM_RECORD_SIZE;
    unsigned prev = 0;
    int in_tail = 0;
    if (cnt == 0) return refuse(STFX_ATE_E_SIZE, 0, "streams: %zu bytes is less than one 22-byte record", n);
    for (i = 0; i < cnt; i++) {
        const uint8_t* r = b + i * STFX_STREAM_RECORD_SIZE;
        unsigned id = little ? rd16le(r) : rd16be(r);
        size_t k;
        int nul = 0;
        /* the 75 trailing all-zero records are the file's padding (the game's count includes them, the search never matches id 0) */
        if (id == 0) {
            in_tail = 1;
        } else {
            if (in_tail) return refuse(STFX_ATE_E_FORMAT, i * 22, "streams record %zu: id %u after an id-0 record", i, id);
            /* NOT strictly ascending: id 426 appears twice (records 188, 189; the linear search at audio_stream.c:270-293 takes the first) */
            if (id < prev) return refuse(STFX_ATE_E_FORMAT, i * 22, "streams record %zu: id %u below previous %u", i, id, prev);
            prev = id;
        }
        /* name[15] + flag is concatenated as a C string (audio_stream.c:308): a NUL must exist in name[0..15] or flag */
        for (k = 0; k < 16; k++) if (r[6 + k] == 0) { nul = 1; break; }
        if (!nul) return refuse(STFX_ATE_E_FORMAT, i * 22 + 6, "streams record %zu: name has no NUL", i);
    }
    return 0;
}
static void streams_apply(uint8_t* b, size_t n)
{
    size_t i, cnt = n / STFX_STREAM_RECORD_SIZE;
    for (i = 0; i < cnt; i++) {
        uint8_t* r = b + i * STFX_STREAM_RECORD_SIZE;
        fix16(r + 0);
        fix16(r + 4);
#if ATE_FAULT == 8
        r[2] = (uint8_t)((r[2] >> 4) | (r[2] << 4)); ++g_stats.bitfield_bytes;
#else
        bf_fix(r + 2, W_STR_A, 4);
#endif
#if ATE_FAULT == 5
        r[3] = (uint8_t)((r[3] >> 1) | (r[3] << 7)); ++g_stats.bitfield_bytes;   /* rotates the wrong way: full<->volume land off by one bit */
#else
        bf_fix(r + 3, W_STR_B, 2);
#endif
#if ATE_FAULT == 6
        { int k; for (k = 0; k < 7; k++) sw16(r + 6 + 2 * k); }
#endif
        if (r[0] == 0 && r[1] == 0) ++g_stats.stream_empty_records;
    }
    g_stats.stream_records += (unsigned)cnt;
    g_stats.stream_trailing_bytes += n - cnt * STFX_STREAM_RECORD_SIZE;
}

/* ---------------------------------------------------------------------------------------------- entry points */
typedef int  (*validate_fn)(const uint8_t*, size_t, int);
typedef void (*apply_fn)(uint8_t*, size_t);

static int run(const char* what, size_t rec, int strict_size, validate_fn v, apply_fn a, void* buf, size_t n)
{
    int rc;
    if (!buf || n == 0) return refuse(STFX_ATE_E_ARG, 0, "%s: NULL buffer or zero length", what);
    g_err[0] = 0;
    if (strict_size && n % rec) return refuse(STFX_ATE_E_SIZE, n, "%s: %zu bytes is not a multiple of %zu", what, n, rec);
    rc = v((const uint8_t*)buf, n, 0);
    if (rc) {
        char first[200];
        memcpy(first, g_err, sizeof first);
        if (v((const uint8_t*)buf, n, 1) == 0) {       /* it is a clean HOST-order table: the loader hook ran twice */
            snprintf(g_err, sizeof g_err, "%s: buffer already looks swapped (first big-endian refusal: %.120s)", what, first);
            return STFX_ATE_E_SWAPPED;
        }
        memcpy(g_err, first, sizeof g_err);
        return rc;
    }
    a((uint8_t*)buf, n);
    return (int)(n / rec);
}

int stfx_swap_sfx_table(void* buf, size_t n)     { return run("Sfx.bin", STFX_SFX_RECORD_SIZE, 1, sfx_validate, sfx_apply, buf, n); }
int stfx_swap_music_table(void* buf, size_t n)   { return run("Music.bin", STFX_MUSIC_RECORD_SIZE, 1, music_validate, music_apply, buf, n); }
int stfx_swap_streams_table(void* buf, size_t n) { return run("Streams.bin", STFX_STREAM_RECORD_SIZE, 0, streams_validate, streams_apply, buf, n); }

/* path tail "audio/data/<name>", case-insensitive, preceded by nothing, '/' or '\\' */
static int tail_is(const char* path, const char* tail)
{
    size_t lp = strlen(path), lt = strlen(tail), i;
    if (lp < lt) return 0;
    for (i = 0; i < lt; i++) {
        char c = path[lp - lt + i];
        if (c == '\\') c = '/';
        if (tolower((unsigned char)c) != tail[i]) return 0;
    }
    if (lp > lt) { char c = path[lp - lt - 1]; if (c != '/' && c != '\\') return 0; }
    return 1;
}
int stfx_audio_table_swap_by_path(const char* path, void* buf, size_t n)
{
    if (!path) return 0;
    if (tail_is(path, "audio/data/sfx.bin"))     return stfx_swap_sfx_table(buf, n);
    if (tail_is(path, "audio/data/music.bin"))   return stfx_swap_music_table(buf, n);
    if (tail_is(path, "audio/data/streams.bin")) return stfx_swap_streams_table(buf, n);
    return 0;
}
