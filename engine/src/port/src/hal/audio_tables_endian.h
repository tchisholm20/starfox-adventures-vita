/* audio_tables_endian.h -- byte-order (and bit-field) layer for the three GAME-side audio tables of Star Fox Adventures:
 *
 *     audio/data/Sfx.bin      1205 x 32-byte SfxTriggerFull   (audio_sfx.c:701-731 binary search, :619-698 reader)
 *     audio/data/Music.bin     159 x 16-byte MusicTrigger     (audio.c:164-175 linear search, :788-1150 readers)
 *     audio/data/Streams.bin   905 x 22-byte StreamEntry      (audio_stream.c:256-380; 830 real + 75 zero records)
 *
 * They are loaded ONCE, by audioLoadTriggerData (audio.c:466-486, called once from audioInit at audio.c:627), kept for
 * the life of the process, and read natively as C structs (include/main/audio_internal.h:74-171).  On the GameCube they
 * are PowerPC big-endian structs; on the Vita (ARM EABI, little-endian) two things differ:
 *   1. every u16 is byte-reversed;
 *   2. every BIT-FIELD is allocated from the other end of its storage byte: PowerPC (MWCC/GCC) puts the first-declared
 *      member in the MOST significant bits, ARM little-endian in the LEAST significant.  A byte swap does nothing for
 *      a one-byte storage unit, yet the members read back wrong.  Each bit-field byte is therefore REWRITTEN so that the
 *      ARM layout yields the same member values the PowerPC layout does (the field order inside the byte is reversed;
 *      each member keeps its width and its value).  TABLES.md has the per-byte maps and the evidence.
 *
 * Same contract as musyx_endian.h:
 *   - one loaded file image in, swapped IN PLACE; call exactly once per loaded buffer (the loader hook is the guarantee);
 *   - two phases: phase 1 validates and writes nothing, phase 2 writes.  A refused buffer is BYTE-IDENTICAL afterwards;
 *   - return = number of records (> 0), or a NEGATIVE STFX_ATE_E_* code; text of the last refusal in
 *     stfx_audio_tables_error();
 *   - libc only, C11, no globals except stats/error text, not thread-safe (loader thread only);
 *   - on a big-endian host every swap and every bit-field remap is the identity (the layouts already agree).
 */
#ifndef STFX_AUDIO_TABLES_ENDIAN_H
#define STFX_AUDIO_TABLES_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STFX_ATE_E_ARG     = -1,   /* NULL buffer / zero length                                                       */
    STFX_ATE_E_SWAPPED = -2,   /* validation failed AND the buffer reads as a valid host-order table (double swap) */
    STFX_ATE_E_SIZE    = -3,   /* length is not a whole number of records (Streams: fewer bytes than one record)    */
    STFX_ATE_E_FORMAT  = -5    /* a record breaks a rule the game's reader relies on (named in the error text)       */
};

enum {
    STFX_SFX_RECORD_SIZE    = 32,
    STFX_MUSIC_RECORD_SIZE  = 16,
    STFX_STREAM_RECORD_SIZE = 22
};

typedef struct stfx_ate_stats {
    unsigned sfx_records, music_records, stream_records, stream_empty_records;
    unsigned long stream_trailing_bytes;      /* bytes after the last whole record (game: info / sizeof(StreamEntry)) */
    unsigned long fields16;                   /* u16 fields reversed                                                   */
    unsigned long bitfield_bytes;             /* bit-field storage bytes rewritten (value changed or not)               */
    unsigned long bitfield_bytes_changed;     /* ... of which the byte actually changed                                 */
    unsigned refused;                         /* buffers refused                                                        */
} stfx_ate_stats;

int stfx_swap_sfx_table(void* buf, size_t n);       /* Sfx.bin:     returns #records (n / 32)                         */
int stfx_swap_music_table(void* buf, size_t n);     /* Music.bin:   returns #records (n / 16)                         */
int stfx_swap_streams_table(void* buf, size_t n);   /* Streams.bin: returns #records (n / 22, as the game computes it) */

/* Loader hook: dispatch on the path's tail "audio/data/sfx.bin" / "music.bin" / "streams.bin" (case-insensitive, a
 * leading '/' or '\' or nothing before "audio").  Returns 0 and does nothing for any other path, otherwise the value of
 * the function above (record count, or a negative code with the buffer left unchanged). */
int stfx_audio_table_swap_by_path(const char* path, void* buf, size_t n);

const char*           stfx_audio_tables_error(void);   /* text of the last refusal ("" if none) */
const stfx_ate_stats* stfx_audio_tables_stats(void);
void                  stfx_audio_tables_reset_stats(void);

#ifdef __cplusplus
}
#endif
#endif
