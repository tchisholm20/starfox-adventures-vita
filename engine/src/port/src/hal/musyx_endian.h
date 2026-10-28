/* musyx_endian.h -- byte-order layer for the MusyX sound data of Star Fox Adventures.
 *
 * The files on the disc (audio/starfox{m,s}.{pro,poo,sdi}, audio/midi.wad) are BIG-ENDIAN PowerPC structures that
 * the MusyX runtime (src/musyx/runtime, *.c) walks NATIVELY.  On a little-endian machine every multi-byte field
 * must be swapped once, in place, before the runtime sees the buffer.  Sample data (starfox{m,s}.sam: DSP-ADPCM
 * frames) is bytes and is never touched; there is deliberately no function for it.
 *
 * Contract of every function below:
 *   - ONE loaded file image in, swapped IN PLACE to host order.  Call it exactly once per loaded buffer (see
 *     INTEGRATE.md: the right place is the file loader, right after the read).
 *   - Two passes: pass 1 walks the buffer without writing anything and validates it (bounds, terminators, sizes,
 *     plausible counts); only if that succeeds does pass 2 write.  A failure therefore leaves the buffer
 *     BYTE-IDENTICAL to what the caller passed.  Pass 2 re-checks that it did the same work as pass 1.
 *   - Return value: number of top-level records swapped (>= 0), or a NEGATIVE MUSYX_E_* code.  The text of the
 *     last error, with the byte offset, is musyx_endian_error().  Not thread-safe (loader thread only).
 *   - Double swap: a second call on an already-swapped buffer fails pass 1 in practice (the first offsets read
 *     back as huge numbers) and returns a negative code (MUSYX_E_SWAPPED where the probe recognises it) without
 *     touching the buffer.  This is a tripwire, not a licence; the loader hook is the guarantee.
 *   - libc only, C11, no globals except the stats/error buffer.  Works on a big-endian host too (then every
 *     "swap" is the identity).
 *
 * Record counts and a field counter are kept in musyx_endian_stats (cumulative until reset).
 */
#ifndef MUSYX_ENDIAN_H
#define MUSYX_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MUSYX_E_ARG      = -1,   /* NULL buffer / zero length                                                    */
    MUSYX_E_SWAPPED  = -2,   /* the buffer already looks like host order (probe on the first offsets)         */
    MUSYX_E_BOUNDS   = -3,   /* a read or an offset lands outside the buffer / outside its record              */
    MUSYX_E_ALIGN    = -4,   /* an offset the runtime would dereference as u16/u32 is misaligned               */
    MUSYX_E_FORMAT   = -5,   /* size, count, type or terminator is not what the runtime's walker requires      */
    MUSYX_E_OVERLAP  = -6,   /* two different rules claim the same bytes: a rule is wrong, not the data        */
    MUSYX_E_NOMEM    = -7,
    MUSYX_E_INTERNAL = -8    /* pass 2 disagreed with pass 1 (a bug in this file)                              */
};

typedef struct musyx_endian_stats {
    /* project (.pro) */
    unsigned groups, song_groups, fx_groups, fx_entries, idlists, idlist_ids, idlist_ranges, pages, midi_setups,
             midi_unterminated;
    /* pool (.poo) */
    unsigned macros, macro_steps, curves_dls, curves_linear, curves_table, keymaps, keymap_entries, layers,
             layer_entries;
    /* sample directory (.sdi) */
    unsigned samples, adpcm_infos, adpcm_info_shared;
    /* songs (arrangements, midi.wad) */
    unsigned songs, tracks, track_entries, master_events, patterns, notes, streams, stream_pairs, wad_sizes;
    /* every kind */
    unsigned long fields16, fields32;      /* 16/32-bit fields newly claimed (= swapped) in the last pass 2 */
    unsigned revisits;                     /* identical field met again through a shared offset: NOT re-swapped */
    unsigned long bytes_claimed;           /* bytes covered by some rule (swapped or deliberately untouched)      */
    unsigned long bytes_total;             /* total size of the buffers processed                                  */
} musyx_endian_stats;

int  musyx_swap_project(void* buf, size_t n);   /* starfox{m,s}.pro : returns #groups                          */
int  musyx_swap_pool(void* buf, size_t n);      /* starfox{m,s}.poo : returns #macros+#curves+#keymaps+#layers */
int  musyx_swap_sdir(void* buf, size_t n);      /* starfox{m,s}.sdi : returns #samples                         */
int  musyx_swap_song(void* buf, size_t n);      /* ONE arrangement (a midi.wad entry, n = its byte size): 1     */
int  musyx_swap_wad(void* buf, size_t n);       /* whole midi.wad: header sizes + all 100 arrangements: #songs  */

/* Convenience for a loader that knows only the path: dispatch on the file name's tail
 * (".pro" ".poo" ".sdi" "midi.wad").  Returns 0 and does nothing for any other path (.sam, .adp, ...);
 * otherwise the value of the function above.  Case-insensitive; the disc names are lower case. */
int  musyx_swap_by_path(const char* path, void* buf, size_t n);

const char*               musyx_endian_error(void);   /* text of the last failure ("" if none) */
const musyx_endian_stats* musyx_endian_get_stats(void);
void                      musyx_endian_reset_stats(void);

#ifdef __cplusplus
}
#endif
#endif
