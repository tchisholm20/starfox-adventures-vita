/* gx_rt_state.h -- the STATE-RECORD module of the GX render thread (locked-30 lever R2; contract host/engine/R2_STATE.md; implementation gx_rt_state.c).
 *
 * The ring-facing entry points (gxs_flush / gxs_begin / gxs_end / gxs_r_apply / gxs_config / gxs_print) are declared in gx_rt.h (the ring calls them weakly).  This header holds
 * what tests and tools need: the record layout and the counters.
 *
 * THE STATE RECORD (GXT_OP_STATE, payload words):
 *   [0]            GXS_MAGIC (top byte 'S') | n << 8 | flags       n = groups in this record (0..GXH_NGROUPS), flags bit 0 = an audit trailer follows
 *   then n entries, ascending group id:   [gid << 16 | words]  followed by `words` data words = the group's bytes (zero-padded to 4) copied from G's instance
 *   if flags & 1:  GXH_NGROUPS words = the FNV-1a hash of EVERY group of G's instance (whole-struct audit, R2_STATE.md D6)
 * R validates everything before it copies anything (unknown group id, a size that is not the table's, ids not ascending, trailing words): a bad record is counted (state_bad),
 * logged and DROPPED WHOLE -- it must stay 0. */
#ifndef GX_RT_STATE_H
#define GX_RT_STATE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GXS_MAGIC 0x53000000u
#define GXS_F_AUDIT 1u

typedef struct GxsStats {
    uint32_t flushes, groups, bytes;          /* [G] STATE records sent, groups in them, payload bytes */
    uint32_t applies;                         /* [R] STATE records applied */
    uint32_t bad;                             /* [R] records refused (must stay 0) */
    uint32_t audit_checked, audit_mismatch;   /* [R] audited records / records with at least one group whose hash differs from G's */
    uint32_t audit_groups_bad;                /* [R] groups that differed, summed */
} GxsStats;
void gxs_get_stats(GxsStats *out);
void gxs_reset_stats(void);
/* FNV-1a over the bytes of group `gid` of an instance (a GxHal; typed void * so gx_rt.h users need not see the struct) */
uint32_t gxs_group_hash(const void *hal, unsigned gid);
/* the first group whose hash differs between R's applied state and a given instance; -1 = none (host tests: compare gx_hal with a reference) */
int gxs_first_diff(const void *a, const void *b);

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_STATE_H */
