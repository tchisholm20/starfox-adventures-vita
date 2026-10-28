/* gx_rt_filter.h -- the G-side redundant-state filter of the GX render thread (src/gx_rt_filter.c).  Public surface = what gx_thread.c and the host tests call.
 * The hot-path symbols (gxt_flt_on, gxt_flt_cls, gxt_flt_pre, gxt_flt_alloc_note, GXT_FLT_WANTS) are in gx_rt.h because gxt_rec_alloc and the generated wrappers inline them.
 * The entry points gx_thread.c calls are WEAK (GXT_FLT_WEAK, gx_rt.h): gx_thread.c is linked alone by the ring test, and a build without this module must still link (nothing is called while gxt_flt_on is 0). */
#ifndef GX_RT_FILTER_H
#define GX_RT_FILTER_H

#include <stdint.h>
#include <stddef.h>
#include "gx_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why the remembered state was forgotten (the four counters of the "[gxt] filt:" line). */
enum { GXT_FLT_F_UNKNOWN = 0, GXT_FLT_F_LISTS, GXT_FLT_F_CLEARVD, GXT_FLT_F_MODE, GXT_FLT_F_N };

/* Classification of an op, as the table in gx_rt_filter.c states it (gxt_flt_class). */
enum { GXT_FLT_C_UNLISTED = 0, GXT_FLT_C_FILTER, GXT_FLT_C_NEUTRAL, GXT_FLT_C_FORGET, GXT_FLT_C_CLEARVD };

/* gxt_flt_arm / gxt_flt_mode_changed / gxt_flt_print are declared in gx_rt.h (see there).  gxt_flt_arm: threaded && marker != 0 && perf arm != 111; the first arming allocates the tables
 * (calloc; on failure it logs and stays OFF); EVERY change of the armed state forgets everything remembered; idempotent.  gxt_flt_mode_changed: a DIRECT <-> threaded transition: forget
 * everything (a no-op before the first arming). */

/* Introspection for the host tests and the boot log. */
int  gxt_flt_class(uint32_t op);                      /* GXT_FLT_C_*: how the table classifies op (UNLISTED = not named: the whitelist makes it FORGET at run time) */
const char *gxt_flt_why(uint32_t op);                 /* the one-line reason the table gives for op ("" when unlisted) */
int  gxt_flt_table_dups(void);                       /* op numbers named twice in the table (must be 0; -1 before the first arming) */
size_t gxt_flt_table_bytes(void);                     /* bytes calloc'd by the first arming (0 before it) */
int  gxt_flt_format(char *buf, size_t cap, uint32_t frames);   /* the A6 line, without printing or resetting */
typedef struct GxtFltStats {
    uint32_t seen, dropped;                           /* calls offered to gxt_flt_pre / answered "redundant" (never recorded) */
    uint32_t kept_list, kept_wide, kept_unknown_key;  /* calls recorded for a reason other than "different payload" */
    uint32_t forgets[GXT_FLT_F_N];
} GxtFltStats;
void gxt_flt_get_stats(GxtFltStats *out);
void gxt_flt_reset_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_FILTER_H */
