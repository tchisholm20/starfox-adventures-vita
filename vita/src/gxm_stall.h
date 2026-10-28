/* gxm_stall.h -- CPU time inside the sceGxm calls that can block on the GPU (gxm_stall.c). Off unless ux0:data/stairfax/gxm_stall.txt asks. */
#ifndef GXM_STALL_H
#define GXM_STALL_H

void gxm_stall_init(void);             /* once, at boot (before vitaGL): reads the marker; absent = every wrap is one load, one branch, one tail call */
void gxm_stall_arm(int present, int arm); /* every present, with the perf_ab arm that holds (gStfxAb): marker mode 2 turns the timers on only while arm == 108 */
void gxm_stall_present(int present);   /* after every present: every 60th prints the "[stall] presents P..Q ..." line and resets the window */

#endif
