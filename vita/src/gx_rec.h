/* gx_rec.h -- T0-a record-and-discard instrument for the GX boundary (docs/53 sections 2.3, 10).  See gx_rec.c. */
#ifndef GX_REC_H
#define GX_REC_H

/* perf_ab arms.  Anything else (including 0 = control) leaves the wrappers on their one-branch fast path. */
#define GXR_ARM_COUNT  10   /* wrappers count calls (per symbol, per class, N_tev, pushes ...); no record */
#define GXR_ARM_RECORD 11   /* ... and also append a record to a 4 MB port-owned ring, reset at every frame edge */

/* Call once per present with the CURRENT gStfxAb, BEFORE the frame's GX work (vt_perf_ab, vita_tool_main.c).  The only place
 * the wrapper mode changes.  The first call prints "[gxr] wraps N symbols, table hash H". */
void gx_rec_frame_edge(int arm);

/* Call once per 60-present window, right where the "[ab] presents P..Q arm A" line is printed (A = the arm that held for the
 * window just ended).  Prints "[gxr] presents P..Q arm A mode M | ..." -- but only once an arm 10/11 has ever been seen, so a
 * default run prints nothing. */
void gx_rec_window(int from, int to, int arm);

#endif
