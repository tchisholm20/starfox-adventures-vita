/* musyx_endian.c -- see musyx_endian.h and ENDIAN.md (the field map, with the MusyX file:line for every rule).
 *
 * House style (mldf_endian.c, envfx_endian.c, objseq_endian.c): one rule per record kind, swap in place, counters.
 * Differences, on purpose:
 *   - TWO PASSES.  Pass 1 (apply = 0) walks and validates and writes nothing; pass 2 (apply = 1) is the same code
 *     with the writes switched on.  A failing buffer is therefore never half-swapped.
 *   - A CLAIM MAP, one byte per buffer byte.  Every field is claimed as "swapped u16", "swapped u32" or "bytes,
 *     left alone".  The same field met again (a list shared by two groups, a pattern shared by two table slots) is
 *     recognised and NOT swapped twice; two different rules over the same bytes are an error (MUSYX_E_OVERLAP).
 *     This is what makes "swap exactly once" a property of the code and not of the caller's discipline.
 *   - get16/get32 read the LOGICAL value whether or not the field was already swapped in pass 2, so a walker can
 *     read a count after it swapped it.
 *
 * MUSYX_FAULT=k compiles a deliberately wrong rule in (the test's fault arms): 1 skip a u16 (FX macro id), 2 swap a byte array,
 * 3 treat a u32 offset as a u16, 4 swap the byte-assembled s32 of a DLS curve, 5 swap the s8 pair of a pattern
 * track entry, 6 claim a byte field as a u16 (the guard, not the diff, must catch this one).
 */
#include "musyx_endian.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUSYX_FAULT
#define MUSYX_FAULT 0
#endif

/* ------------------------------------------------------------------------------------------------ state */
typedef struct W {
    uint8_t* b;                 /* this buffer (a sub-buffer for one arrangement inside midi.wad) */
    size_t n;
    uint8_t* own;               /* claim map, same length                                         */
    int apply;                  /* 0 = validate only, 1 = write                                    */
    int* errp;                  /* first error (a MUSYX_E_* code), shared by sub-windows; 0 = none */
    musyx_endian_stats* st;
} W;

enum { OWN_BYTES = 0x02, OWN_CONT = 0x01, OWN_S16 = 0x12, OWN_S32 = 0x14 };

static char g_err[200];
static musyx_endian_stats g_stats;

const char* musyx_endian_error(void) { return g_err; }
const musyx_endian_stats* musyx_endian_get_stats(void) { return &g_stats; }
void musyx_endian_reset_stats(void) { memset(&g_stats, 0, sizeof g_stats); g_err[0] = 0; }

/* NO longjmp: the first failure is recorded and every later read/claim/write becomes a no-op, every walker returns at its next
   check.  (An earlier version used setjmp/longjmp; on MinGW-w64 x64 the msvcrt longjmp unwinder crashed ~2% of the time inside
   ntdll!RtlUnwindEx, found by test_musyx_endian's guard arm -- see ENDIAN.md.)  die() records, DIE() records and returns. */
static void die(W* w, int code, size_t off, const char* fmt, ...) {
    char msg[150];
    va_list ap;
    if (*w->errp) return;                                           /* keep the FIRST error */
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(g_err, sizeof g_err, "%s (at 0x%zx, buffer %zu bytes)", msg, off, w->n);
    *w->errp = code;
}
#define DIE(w, code, off, ...) do { die((w), (code), (off), __VA_ARGS__); return; } while (0)

static int need(W* w, size_t off, size_t len, const char* what) {
    if (*w->errp) return 0;
    if (off > w->n || len > w->n - off) { die(w, MUSYX_E_BOUNDS, off, "%s: %zu bytes outside the buffer", what, len); return 0; }
    return 1;
}

static uint8_t get8(W* w, size_t off) { return need(w, off, 1, "u8 read") ? w->b[off] : 0; }

static uint16_t get16(W* w, size_t off) {
    uint16_t v;
    if (!need(w, off, 2, "u16 read")) return 0;
    if (w->apply && w->own[off] == OWN_S16) { memcpy(&v, w->b + off, 2); return v; }   /* already host order */
    return (uint16_t)((w->b[off] << 8) | w->b[off + 1]);
}

static uint32_t get32(W* w, size_t off) {
    uint32_t v;
    if (!need(w, off, 4, "u32 read")) return 0;
    if (w->apply && w->own[off] == OWN_S32) { memcpy(&v, w->b + off, 4); return v; }
    return ((uint32_t)w->b[off] << 24) | ((uint32_t)w->b[off + 1] << 16) | ((uint32_t)w->b[off + 2] << 8) | w->b[off + 3];
}

/* claim `width` bytes at `off`.  swapped: 0 = bytes left alone, 2 / 4 = a field that gets swapped.
 * Returns 1 if newly claimed, 0 if the identical field was already claimed. */
static int claim(W* w, size_t off, int swapped) {
    int width = swapped ? swapped : 1;
    uint8_t first = swapped == 2 ? OWN_S16 : swapped == 4 ? OWN_S32 : OWN_BYTES;
    if (!need(w, off, (size_t)width, "claim")) return 0;
    if (!swapped) {
        if (w->own[off] == OWN_BYTES || w->own[off] == 0) { w->own[off] = OWN_BYTES; return 1; }
        die(w, MUSYX_E_OVERLAP, off, "byte at 0x%zx is claimed as part of a swapped field", off);
        return 0;
    }
    if (w->own[off] == first) {
        for (int i = 1; i < width; i++)
            if (w->own[off + i] != OWN_CONT) { die(w, MUSYX_E_OVERLAP, off, "field overlaps a different field"); return 0; }
        return 0;
    }
    for (int i = 0; i < width; i++)
        if (w->own[off + i]) { die(w, MUSYX_E_OVERLAP, off + i, "a swapped %d-byte field overlaps bytes another rule owns", width); return 0; }
    w->own[off] = first;
    for (int i = 1; i < width; i++) w->own[off + i] = OWN_CONT;
    return 1;
}

static void put16(W* w, size_t off) {
    if (w->apply) { uint16_t v = (uint16_t)((w->b[off] << 8) | w->b[off + 1]); memcpy(w->b + off, &v, 2); }
}
static void put32(W* w, size_t off) {
    if (w->apply) {
        uint32_t v = ((uint32_t)w->b[off] << 24) | ((uint32_t)w->b[off + 1] << 16) | ((uint32_t)w->b[off + 2] << 8) | w->b[off + 3];
        memcpy(w->b + off, &v, 4);
    }
}

/* the two ways a rule marks a field; they return 1 when the field was newly claimed (and, in pass 2, swapped) */
static int sw16(W* w, size_t off) {
    if (*w->errp) return 0;
    if (!claim(w, off, 2)) { if (!*w->errp) w->st->revisits++; return 0; }
    put16(w, off); w->st->fields16++; return 1;
}
static int sw32(W* w, size_t off) {
    if (*w->errp) return 0;
    if (!claim(w, off, 4)) { if (!*w->errp) w->st->revisits++; return 0; }
    put32(w, off); w->st->fields32++; return 1;
}
static void keep(W* w, size_t off, size_t len) {                 /* bytes the runtime reads one at a time */
    if (!need(w, off, len, "byte field")) return;
    for (size_t i = 0; i < len; i++) claim(w, off + i, 0);
}

/* fault arms: a raw swap that bypasses the claim map, like a hand-written loop would */
#if MUSYX_FAULT
static void raw16(W* w, size_t off) { if (need(w, off, 2, "raw")) put16(w, off); }
static void raw32(W* w, size_t off) { if (need(w, off, 4, "raw")) put32(w, off); }
#endif

/* ------------------------------------------------------------------------------------------------ project */
/* snd_groups.c:199-214 ScanIDList.  Returns 1 if the list is new. */
static void idlist(W* w, size_t off, size_t gend) {
    if (off >= gend) DIE(w, MUSYX_E_BOUNDS, off, "id list starts outside its group");
    if (w->own[off] != 0) { w->st->revisits++; return; }          /* shared with an earlier group: already swapped */
    w->st->idlists++;
    for (;;) {
        if (*w->errp) return;
        if (off + 2 > gend) DIE(w, MUSYX_E_FORMAT, off, "id list runs off the end of its group without 0xFFFF");
        uint16_t v = get16(w, off);
        if (v == 0xFFFF) { keep(w, off, 2); return; }              /* palindromic: never swapped */
        sw16(w, off);
        if (v & 0x8000) {                                          /* range: 0x8000|lo, hi (ScanIDList: ref[1]) */
            if (off + 4 > gend) DIE(w, MUSYX_E_FORMAT, off, "id range cut off by the group end");
            uint16_t hi = get16(w, off + 2), lo = v & 0x3FFF;
            sw16(w, off + 2);
            if (hi < lo) DIE(w, MUSYX_E_FORMAT, off, "id range %u..%u descends", lo, hi);
            w->st->idlist_ranges++; w->st->idlist_ids += (unsigned)(hi - lo + 1);
            off += 4;
        } else {
            w->st->idlist_ids++; off += 2;
        }
    }
}

static void pages(W* w, size_t off, size_t gend) {               /* SynthPage list, terminated by index == 0xFF */
    for (unsigned k = 0;; k++) {
        if (off + 6 > gend || k > 512) DIE(w, MUSYX_E_FORMAT, off, "page list has no terminator (index 0xFF)");
        sw16(w, off);                                              /* macro */
        keep(w, off + 2, 4);                                       /* priority maxVoices index reserved */
        w->st->pages++;
        if (get8(w, off + 4) == 0xFF) return;
        off += 6;
    }
}

static void walk_project(W* w) {
    size_t o = 0;
    for (;;) {
        if (*w->errp) return;
        uint32_t nx = get32(w, o);
        if (nx == 0xFFFFFFFFu) { keep(w, o, 4); return; }          /* snd_groups.c:226 nextOff == 0xFFFFFFFF */
        if (nx < o + 40 || nx > w->n) DIE(w, MUSYX_E_FORMAT, o, "group nextOff 0x%x not after the 40-byte header / past the file", nx);
        if (nx & 3) DIE(w, MUSYX_E_ALIGN, o, "group nextOff 0x%x not 4-aligned (the runtime reads a u32 there)", nx);
        uint16_t type = get16(w, o + 6);
        uint32_t off[8];
        for (int i = 0; i < 8; i++) off[i] = get32(w, o + 8 + 4 * i);
#if MUSYX_FAULT == 3
        raw16(w, o);                                               /* the offset treated as a u16 (high half only) */
        claim(w, o, 0); keep(w, o + 1, 3);
#else
        sw32(w, o);                                                /* nextOff */
#endif
        sw16(w, o + 4); sw16(w, o + 6);                            /* id, type */
        for (int i = 0; i < 8; i++) sw32(w, o + 8 + 4 * i);        /* macroOff sampleOff curveOff keymapOff layerOff + union */
        for (int i = 0; i < 5; i++) {
            if (off[i] < o + 40) DIE(w, MUSYX_E_BOUNDS, o, "group list %d offset 0x%x before the list area", i, off[i]);
            idlist(w, off[i], nx);
        }
        if (type == 1) {                                           /* FX_DATA at the union's first word (snd_groups.c:241) */
            size_t t = off[5];
            if (t < o + 40 || t + 4 > nx) DIE(w, MUSYX_E_BOUNDS, o, "fx table offset 0x%zx outside the group", t);
            uint16_t num = get16(w, t);
            sw16(w, t); sw16(w, t + 2);
            for (unsigned i = 0; i < num; i++) {                   /* FX_TAB, 10 bytes: id macro (u16) + 6 bytes */
                size_t e = t + 4 + 10 * (size_t)i;
                if (e + 10 > nx) DIE(w, MUSYX_E_FORMAT, e, "fx entry %u crosses the group end", i);
                sw16(w, e);
#if MUSYX_FAULT == 1
                keep(w, e + 2, 2);                                 /* the macro id left big-endian */
#else
                sw16(w, e + 2);                                    /* macro */
#endif
                keep(w, e + 4, 6);
            }
            w->st->fx_groups++; w->st->fx_entries += num;
        } else if (type == 0) {                                    /* song group: seqPlaySong snd_groups.c:274-276 */
            if (off[5] < o + 40 || off[6] < o + 40 || off[7] < o + 40) DIE(w, MUSYX_E_BOUNDS, o, "song group page/setup offset before the list area");
            pages(w, off[5], nx);
            pages(w, off[6], nx);
            size_t e = off[7];
            for (;;) {                                             /* SynthMidiSetup, 84 bytes, songId 0xFFFF ends */
                if (*w->errp) return;
                if (e >= nx) { w->st->midi_unterminated++; break; }
                if (e + 2 > nx) DIE(w, MUSYX_E_FORMAT, e, "midi setup cut off");
                uint16_t sid = get16(w, e);
                if (sid == 0xFFFF) { keep(w, e, 2); break; }
                if (e + 84 > nx) DIE(w, MUSYX_E_FORMAT, e, "midi setup record crosses the group end");
                sw16(w, e); keep(w, e + 2, 82);
                w->st->midi_setups++;
                e += 84;
            }
            w->st->song_groups++;
        } else {
            DIE(w, MUSYX_E_FORMAT, o + 6, "unknown group type %u", type);
        }
        w->st->groups++;
        o = nx;
    }
}

/* ------------------------------------------------------------------------------------------------ pool */
/* MEM_DATA chain (snd_groups.c:39-54, 77-87): u32 nextOff (relative to the record), u16 id, u16 reserved, data */
typedef void (*chain_fn)(W*, size_t rec, uint32_t size);
static void chain(W* w, size_t start, chain_fn fn) {
    size_t o = start;
    for (;;) {
        if (*w->errp) return;
        uint32_t nx = get32(w, o);
        if (nx == 0xFFFFFFFFu) { keep(w, o, 4); return; }
        if (nx < 8 || (nx & 3) || nx > w->n - o) DIE(w, MUSYX_E_FORMAT, o, "pool record size %u invalid", nx);
        sw32(w, o); sw16(w, o + 4); sw16(w, o + 6);
        fn(w, o, nx);
        o += nx;
    }
}

static void macro_rec(W* w, size_t o, uint32_t nx) {              /* mcmd_exec.c:698-700: two u32 per step */
    if ((nx - 8) % 8) DIE(w, MUSYX_E_FORMAT, o, "macro size %u is not 8 + 8k", nx);
    for (size_t a = o + 8; a < o + nx; a += 8) { sw32(w, a); sw32(w, a + 4); w->st->macro_steps++; }
    w->st->macros++;
}

static void curve_rec(W* w, size_t o, uint32_t nx) {
    size_t a = o + 8, n = nx - 8;
    if (n == 20) {            /* ADSR_INFO.data.dls: mcmd_setup.c:326-339. atime/dtime/ascale/dscale are s32 ASSEMBLED
                                 FROM BYTES, LITTLE-ENDIAN (b[0] | b[1]<<8 ...): the file is LE, leave them. slevel and
                                 rtime are u16 read natively and byte-reversed by the runtime (x>>8 | x<<8): the file is
                                 LE, so the native read must see b0<<8|b1: the ordinary BE->host swap.              */
#if MUSYX_FAULT == 4
        raw32(w, a);
#endif
        keep(w, a, 8); sw16(w, a + 8); sw16(w, a + 10); keep(w, a + 12, 8);
        w->st->curves_dls++;
    } else if (n == 8) {      /* linear: 4 x u16, same convention (mcmd_setup.c:317-321) */
        for (int i = 0; i < 4; i++) sw16(w, a + 2 * i);
        w->st->curves_linear++;
    } else if (n >= 129) {    /* u8 translate table (mcmd_volume.c:25-39 reads ptr[0..128]) */
        keep(w, a, n);
        w->st->curves_table++;
    } else {
        DIE(w, MUSYX_E_FORMAT, o, "curve of %zu data bytes is neither DLS(20), linear(8) nor a u8 table", n);
    }
}

static void keymap_rec(W* w, size_t o, uint32_t nx) {            /* KeymapEntry[128], 8 bytes (synth.c:343-350) */
    if (nx != 8 + 128 * 8) DIE(w, MUSYX_E_FORMAT, o, "keymap record is %u bytes, not 1032", nx);
    for (size_t k = 0; k < 128; k++) {
        size_t a = o + 8 + 8 * k;
        sw16(w, a);                                                /* id */
        keep(w, a + 2, 2);                                         /* transpose (s8) panning (u8) */
        sw16(w, a + 4);                                            /* prioOffset (s16) */
        keep(w, a + 6, 2);
    }
    w->st->keymaps++; w->st->keymap_entries += 128;
}

static void layer_rec(W* w, size_t o, uint32_t nx) {             /* u32 num then LAYER[num], 12 bytes (synth.c:89-99) */
    if (nx < 12) DIE(w, MUSYX_E_FORMAT, o, "layer record too small");
    uint32_t num = get32(w, o + 8);
    sw32(w, o + 8);
    if ((uint64_t)nx != 12u + 12u * (uint64_t)num) DIE(w, MUSYX_E_FORMAT, o, "layer size %u != 12 + 12*%u", nx, num);
    for (uint32_t k = 0; k < num; k++) {
        size_t a = o + 12 + 12 * (size_t)k;
        sw16(w, a);                                                /* id */
#if MUSYX_FAULT == 2
        raw16(w, a + 2);                                           /* keyLow,keyHigh swapped as if one u16 */
        keep(w, a + 2, 4);
#elif MUSYX_FAULT == 6
        sw16(w, a + 2);                                            /* the guard must refuse this: */
        keep(w, a + 2, 4);                                         /* these bytes are already a swapped u16 */
#else
        keep(w, a + 2, 4);                                         /* keyLow keyHigh transpose(s8) volume */
#endif
        sw16(w, a + 6);                                            /* prioOffset (s16) */
        keep(w, a + 8, 4);                                         /* panning + reserved[3] */
    }
    w->st->layers++; w->st->layer_entries += num;
}

static void walk_pool(W* w) {
    uint32_t h[4];
    need(w, 0, 16, "pool header");
    for (int i = 0; i < 4; i++) h[i] = get32(w, 4 * i);
    for (int i = 0; i < 4; i++) sw32(w, 4 * i);
    for (int i = 0; i < 4; i++) {
        if (i == 1 && h[1] == 0) continue;                         /* starfoxs.poo has no curve section */
        if (h[i] < 16 || (h[i] & 3) || h[i] >= w->n) DIE(w, MUSYX_E_BOUNDS, 4 * (size_t)i, "pool section %d offset 0x%x invalid", i, h[i]);
    }
    chain(w, h[0], macro_rec);
    if (h[1]) chain(w, h[1], curve_rec);
    chain(w, h[2], keymap_rec);
    chain(w, h[3], layer_rec);
}

/* ------------------------------------------------------------------------------------------------ sample dir */
static void walk_sdir(W* w) {
    size_t o = 0, ns = 0, end;
    uint32_t prev = 0;
    for (;;) {
        if (*w->errp) return;
        uint16_t id = get16(w, o);                                 /* data_tables.c:346 loops until id == 0xFFFF */
        if (id == 0xFFFF) { need(w, o, 4, "sdir terminator"); keep(w, o, 4); end = o + 4; break; }
        need(w, o, 32, "SDIR_DATA");
        if (ns && id <= prev) DIE(w, MUSYX_E_FORMAT, o, "sample ids not ascending (bsearch in data_tables.c:613-640 needs it): %u after %u", id, prev);
        prev = id;
        sw16(w, o); sw16(w, o + 2);                                /* id, ref_cnt */
#if MUSYX_FAULT == 3
        raw16(w, o + 4); keep(w, o + 4, 4);                        /* offset swapped as a u16 */
#else
        sw32(w, o + 4);                                            /* offset */
#endif
        for (size_t k = 8; k < 32; k += 4) sw32(w, o + k);         /* addr info length loopOffset loopLength extraData */
        uint32_t ct = get32(w, o + 16) >> 24;
        if (ct != 0 && ct != 4 && ct != 5) DIE(w, MUSYX_E_FORMAT, o, "sample %u compType %u: ADPCM info size unknown here", id, ct);
        o += 32; ns++;
        w->st->samples++;
    }
    for (size_t i = 0; i < ns; i++) {                              /* SNDADPCMinfo, 40 bytes, at sdir + extraData (data_tables.c:638) */
        size_t x = get32(w, 32 * i + 28);
        if (x == 0 || (x & 1) || x < end || x > w->n || w->n - x < 40) DIE(w, MUSYX_E_BOUNDS, 32 * i + 28, "sample %zu extraData 0x%zx not a 40-byte block after the directory", i, x);
        if (!sw16(w, x)) { w->st->adpcm_info_shared++; continue; }   /* unk0 */
        keep(w, x + 2, 2);                                         /* initialPS, loopPS */
        sw16(w, x + 4); sw16(w, x + 6);                            /* loopY0 loopY1 */
        for (int c = 0; c < 16; c++) sw16(w, x + 8 + 2 * (size_t)c);   /* coefTab[8][2] */
        w->st->adpcm_infos++;
    }
}

/* ------------------------------------------------------------------------------------------------ songs */
static void stream(W* w, size_t o) {                              /* seq.c:902-958 GetStreamValue: bytes, 0x80 0x00 ends */
    if (o == 0) return;
    if (o >= w->n) DIE(w, MUSYX_E_BOUNDS, o, "stream offset outside the arrangement");
    if (w->own[o]) return;                                         /* shared with another pattern */
    w->st->streams++;
    for (;;) {
        if (*w->errp) return;
        uint8_t hi = get8(w, o), lo = get8(w, o + 1);
        if (hi == 0x80 && lo == 0x00) { keep(w, o, 2); return; }
        size_t a = (hi & 0x80) ? 2 : 1;                            /* tag */
        keep(w, o, a); o += a;
        a = (get8(w, o) & 0x80) ? 2 : 1;                           /* value */
        keep(w, o, a); o += a;
        w->st->stream_pairs++;
    }
}

static void pattern(W* w, size_t po) {
    if (!sw32(w, po)) return;                                      /* shared body already done */
    sw32(w, po + 4); sw32(w, po + 8);                              /* SynthSeqPattern {unk00, pitchBendOffset, modulationOffset} */
    uint32_t pb = get32(w, po + 4), md = get32(w, po + 8);
    w->st->patterns++;
    for (size_t o = po + 12;;) {                                   /* seq.c:1000-1035 */
        if (*w->errp) return;
        need(w, o, 4, "note");
        uint8_t k = get8(w, o + 2), v = get8(w, o + 3);
        sw16(w, o);                                                /* time */
        keep(w, o + 2, 2);                                         /* key, velocity */
        w->st->notes++;
        if (k == 0xFF && v == 0xFF) break;                         /* end marker (checked first, seq.c:1006) */
        if (k & 0x80) { o += 4; }                                  /* meta / program change */
        else if ((k | v) == 0) { o += 4; }
        else { need(w, o, 6, "note with length"); sw16(w, o + 4); o += 6; }   /* + u16 length */
    }
    stream(w, pb);
    stream(w, md);
}

static void walk_song(W* w) {
    need(w, 0, 24, "arrangement header");
    uint32_t tt = get32(w, 0), pt = get32(w, 4), mt = get32(w, 8), mst = get32(w, 12), info = get32(w, 16);
    /* Two header sizes.  info & 0x80000000 ("key groups": 16 sections, seq.c:374,492-497) selects the full 22-word
       form {5 words, loopPoint[16], trackSectionTableOffset}; without it the header is 6 words {5 words, loopPoint[0]}
       and the track table follows at 0x18 (seq.c:1420 reads loopPoint[voice] with voice == 0 only). */
    unsigned hw = (info & 0x80000000u) ? 22 : 6;
    uint32_t sect = 0;
    if (hw == 22) { need(w, 0, 88, "arrangement header (full form)"); sect = get32(w, 84); }
    for (unsigned i = 0; i < hw; i++) sw32(w, 4 * (size_t)i);      /* offsets/flags, loopPoint[], trackSectionTableOffset */
    if (tt < 4 * hw || (tt & 3) || tt > w->n || w->n - tt < 256) DIE(w, MUSYX_E_BOUNDS, 0, "trackTableOffset 0x%x invalid", tt);
    if (pt < 4 * hw || (pt & 3) || pt > w->n || w->n - pt < 8) DIE(w, MUSYX_E_BOUNDS, 4, "patternTableOffset 0x%x invalid", pt);
    if (mt > w->n || w->n - mt < 64) DIE(w, MUSYX_E_BOUNDS, 8, "trackMidiTableOffset 0x%x invalid", mt);
    keep(w, mt, 64);                                               /* seq.c:1206 u8 per track */
    if (hw == 22) {                                                /* seq.c:374 keyGroupMap = u8[64] */
        if (sect > w->n || w->n - sect < 64) DIE(w, MUSYX_E_BOUNDS, 84, "trackSectionTableOffset 0x%x invalid", sect);
        keep(w, sect, 64);
    }
    int maxpat = -1;
    for (int i = 0; i < 64; i++) sw32(w, tt + 4 * (size_t)i);      /* seq.c:398 trackOffsets[64] */
    for (int i = 0; i < 64; i++) {
        uint32_t t = get32(w, tt + 4 * (size_t)i);
        if (!t) continue;
        if ((t & 3) || t < 4 * hw) DIE(w, MUSYX_E_ALIGN, tt + 4 * (size_t)i, "track %d offset 0x%x invalid", i, t);
        w->st->tracks++;
        for (size_t o = t;; o += 12) {                             /* SynthTrackEntry, 12 bytes (synth_internal.h:72-88) */
            if (*w->errp) return;
            need(w, o, 12, "track entry");
            if (sw32(w, o)) w->st->track_entries++;                /* time */
            keep(w, o + 4, 4);                                     /* programChange volume reserved[2] */
            uint16_t kind = get16(w, o + 8);
            sw16(w, o + 8);
            if (kind == 0xFFFF) { keep(w, o + 10, 2); break; }     /* SYNTH_TRACK_COMMAND_END  (seq.c:974) */
            if (kind == 0xFFFE) { sw16(w, o + 10); break; }        /* JUMP: argument.jumpIndex is a u16 */
#if MUSYX_FAULT == 5
            raw16(w, o + 10); keep(w, o + 10, 2);                  /* pattern entry: transpose/velocityAdd are s8s */
#else
            keep(w, o + 10, 2);
#endif
            if ((int)kind > maxpat) maxpat = (int)kind;
        }
    }
    if (mst) {                                                     /* seq.c:477-497 SynthMasterTrackEvent {time, bpm} */
        if ((mst & 3) || mst >= w->n) DIE(w, MUSYX_E_BOUNDS, 12, "masterTrackOffset 0x%x invalid", mst);
        for (size_t o = mst;; o += 8) {
            if (*w->errp) return;
            uint32_t tm = get32(w, o);
            if (tm == 0xFFFFFFFFu) { keep(w, o, 4); break; }
            need(w, o, 8, "master track event");
            sw32(w, o); sw32(w, o + 4);
            w->st->master_events++;
        }
    }
    uint32_t first = get32(w, pt);                                 /* the table ends where body 0 begins */
    if (first <= pt || ((first - pt) & 3) || first > w->n - 12) DIE(w, MUSYX_E_FORMAT, pt, "pattern table entry 0 = 0x%x is not a boundary after the table", first);
    uint32_t npat = (first - pt) / 4;
    if (npat > 4096 || maxpat >= (int)npat) DIE(w, MUSYX_E_FORMAT, pt, "tracks use pattern %d but the table holds %u", maxpat, npat);
    for (uint32_t i = 0; i < npat; i++) {
        uint32_t po = get32(w, pt + 4 * (size_t)i);
        sw32(w, pt + 4 * (size_t)i);
        if (po < first || (po & 3) || po > w->n - 12) DIE(w, MUSYX_E_BOUNDS, pt + 4 * (size_t)i, "pattern %u offset 0x%x invalid", i, po);
        pattern(w, po);
    }
    w->st->songs++;
}

static void walk_wad(W* w) {
    uint32_t sz[100];
    size_t pos = 0x1A0;
    need(w, 0, 0x1A0, "midi.wad header");
    for (int i = 0; i < 100; i++) {
        sz[i] = get32(w, 4 * (size_t)i);
        size_t padded = ((size_t)sz[i] + 31) & ~(size_t)31;         /* audio.c:1049-1063 */
        if (sz[i] < 24 || padded > w->n - pos) DIE(w, MUSYX_E_BOUNDS, 4 * (size_t)i, "wad entry %d size %u invalid", i, sz[i]);
        pos += padded;
    }
    if (pos != w->n) DIE(w, MUSYX_E_FORMAT, 0, "wad sizes add up to 0x%zx, the file is 0x%zx", pos, w->n);
    for (size_t i = 400; i < 0x1A0; i++) if (get8(w, i)) DIE(w, MUSYX_E_FORMAT, i, "wad header padding not zero");
    keep(w, 400, 0x1A0 - 400);
    pos = 0x1A0;
    for (int i = 0; i < 100; i++) {
        if (*w->errp) return;
        W s = *w;                                                   /* same error slot, same stats, own window */
        s.b = w->b + pos; s.n = sz[i]; s.own = w->own + pos;
        walk_song(&s);
        pos += ((size_t)sz[i] + 31) & ~(size_t)31;
    }
    for (int i = 0; i < 100; i++) sw32(w, 4 * (size_t)i);
    w->st->wad_sizes += 100;
}

/* ------------------------------------------------------------------------------------------------ drivers */
static int probe_le(const uint8_t* b, size_t n, size_t off, uint32_t lo, uint32_t hi) {
    if (n < off + 4) return 0;
    uint32_t be = ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) | ((uint32_t)b[off + 2] << 8) | b[off + 3];
    uint32_t le = ((uint32_t)b[off + 3] << 24) | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 1] << 8) | b[off];
    return !(be >= lo && be <= hi) && le >= lo && le <= hi;
}
static int probe_project(const uint8_t* b, size_t n) { return probe_le(b, n, 0, 40, (uint32_t)n); }
static int probe_pool(const uint8_t* b, size_t n) { return probe_le(b, n, 0, 16, (uint32_t)n); }
static int probe_song(const uint8_t* b, size_t n) { return probe_le(b, n, 0, 24, (uint32_t)n); }
static int probe_wad(const uint8_t* b, size_t n) { return probe_le(b, n, 0, 88, (uint32_t)n); }

typedef unsigned (*count_fn)(const musyx_endian_stats*);
static unsigned c_project(const musyx_endian_stats* s) { return s->groups; }
static unsigned c_pool(const musyx_endian_stats* s) { return s->macros + s->curves_dls + s->curves_linear + s->curves_table + s->keymaps + s->layers; }
static unsigned c_sdir(const musyx_endian_stats* s) { return s->samples; }
static unsigned c_song(const musyx_endian_stats* s) { return s->songs; }

static unsigned long claimed(const uint8_t* own, size_t n) { unsigned long c = 0; for (size_t i = 0; i < n; i++) c += own[i] != 0; return c; }

static int run(void (*fn)(W*), void* buf, size_t n, int (*probe)(const uint8_t*, size_t), count_fn count) {
    g_err[0] = 0;
    if (!buf || !n) { snprintf(g_err, sizeof g_err, "NULL buffer or zero length"); return MUSYX_E_ARG; }
    musyx_endian_stats s1, s2;
    W w;
    int err;
    for (int pass = 0; pass < 2; pass++) {
        memset(&w, 0, sizeof w);
        err = 0;
        w.b = buf; w.n = n; w.apply = pass; w.errp = &err;
        w.st = pass ? &s2 : &s1;
        memset(w.st, 0, sizeof *w.st);
        w.own = calloc(n, 1);
        if (!w.own) { snprintf(g_err, sizeof g_err, "out of memory for a %zu-byte claim map", n); return MUSYX_E_NOMEM; }
        fn(&w);
        if (err != 0) {
            int rc = err;
            free(w.own);
            if (pass == 0) {
                if (probe && probe(buf, n)) {
                    snprintf(g_err, sizeof g_err, "buffer already looks host-order (first offset reads sane as little-endian); not touched");
                    return MUSYX_E_SWAPPED;
                }
                return rc;                                         /* nothing written */
            }
            snprintf(g_err + strlen(g_err), sizeof g_err - strlen(g_err), " [pass 2 failed after pass 1 passed: buffer may be partly swapped]");
            return MUSYX_E_INTERNAL;
        }
        w.st->bytes_total = (unsigned long)n;
        w.st->bytes_claimed = claimed(w.own, n);
        free(w.own);
    }
    if (memcmp(&s1, &s2, sizeof s1) != 0) {
        snprintf(g_err, sizeof g_err, "pass 2 did different work from pass 1 (counters differ)");
        return MUSYX_E_INTERNAL;
    }
    musyx_endian_stats* g = &g_stats;
#define ADD(f) g->f += s2.f
    ADD(groups); ADD(song_groups); ADD(fx_groups); ADD(fx_entries); ADD(idlists); ADD(idlist_ids); ADD(idlist_ranges);
    ADD(pages); ADD(midi_setups); ADD(midi_unterminated); ADD(macros); ADD(macro_steps); ADD(curves_dls);
    ADD(curves_linear); ADD(curves_table); ADD(keymaps); ADD(keymap_entries); ADD(layers); ADD(layer_entries);
    ADD(samples); ADD(adpcm_infos); ADD(adpcm_info_shared); ADD(songs); ADD(tracks); ADD(track_entries);
    ADD(master_events); ADD(patterns); ADD(notes); ADD(streams); ADD(stream_pairs); ADD(wad_sizes); ADD(fields16);
    ADD(fields32); ADD(revisits); ADD(bytes_claimed); ADD(bytes_total);
#undef ADD
    return (int)count(&s2);
}

int musyx_swap_project(void* b, size_t n) { return run(walk_project, b, n, probe_project, c_project); }
int musyx_swap_pool(void* b, size_t n)    { return run(walk_pool, b, n, probe_pool, c_pool); }
int musyx_swap_sdir(void* b, size_t n)    { return run(walk_sdir, b, n, NULL, c_sdir); }
int musyx_swap_song(void* b, size_t n)    { return run(walk_song, b, n, probe_song, c_song); }
int musyx_swap_wad(void* b, size_t n)     { return run(walk_wad, b, n, probe_wad, c_song); }

static int ends_with(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != suf[i]) return 0;
    return 1;
}

int musyx_swap_by_path(const char* path, void* buf, size_t n) {
    if (!path) return 0;
    if (ends_with(path, ".pro")) return musyx_swap_project(buf, n);
    if (ends_with(path, ".poo")) return musyx_swap_pool(buf, n);
    if (ends_with(path, ".sdi")) return musyx_swap_sdir(buf, n);
    if (ends_with(path, "midi.wad")) return musyx_swap_wad(buf, n);
    return 0;
}
