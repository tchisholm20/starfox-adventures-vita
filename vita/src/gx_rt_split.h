/* gx_rt_split.h -- R5: the render thread R split in two (host/engine/R5_SPLIT.md).  This header is the CONTROL surface only: no GX type, no RHI type, so gx_thread.c (which
 * compiles without the dolphin / port headers on the host) can include it.  The typed surface (what the shims call, what a backend provides) is gx_rt_split_be.h.
 *
 *   R-front = the thread that was R ("stfx_gxr"): it consumes the GX ring exactly as before and runs the HAL, but every HAL -> backend call is RECORDED into the RHI ring
 *             (+ the arena that carries the payload bytes) instead of executed.
 *   R-back  = a second thread ("stfx_gxb"): consumes the RHI ring and executes the backend calls.  ONLY R-back calls sceGxm* while the split is on.
 *
 * CAPABLE vs ON.  "Capable" (marker gx_thread_split.txt = 1) means: every texture / mesh / copy-texture handle the HAL holds is a PROXY (gx_rt_split.c), in BOTH modes, so that
 * the split can be switched on and off at a frame edge (arm 146 = control, arm 148 = split, in one boot) without a handle changing kind.  A process that is not capable
 * (the shipping default) never creates a proxy and its shims cost one load and one predicted branch.  "On" means: the HAL's backend calls are being recorded.
 *
 * Memory order: the RHI ring is single producer (front) / single consumer (back), the same discipline as the GX ring (gx_thread.c): payload stores, STORE_REL(pub); the
 * consumer LOAD_ACQ(pub), executes, STORE_REL(tail).  The arena is a second byte ring retired by the back's tail, per record (record.aend). */
#ifndef GX_RT_SPLIT_H
#define GX_RT_SPLIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RhiInstance;
struct RhiSwapchain;

/* defined by the owner of the backend (rhi_gxm.c on the console; the test harness on the host) */
extern volatile int g_gxsp_cap;                 /* 1: handles are proxies (marker gx_thread_split.txt = 1, decided before the first handle exists) */
extern volatile int g_gxsp_on;                  /* 1: the split is ON: backend calls are recorded.  Written by G at a frame edge with the GX ring drained; read by the front */
struct GxspApi;
extern const struct GxspApi *g_gxsp_api;        /* the front's entry points (gx_rt_split_be.h); valid when g_gxsp_cap */

/* ---- G side (gx_thread.c, at a frame edge, GX ring drained, R idle) ---- */
int  gxsp_g_enable(int front_core, int back_core, int front_thread);   /* start the back thread (once), reset cursors and mirrors, flip g_gxsp_on.  front_thread = the R thread's SceUID (affinity), 0 on the host.  1 ok */
void gxsp_g_disable(int rcore);                 /* drain the back, flip g_gxsp_on off, move the R thread back to rcore (-1: leave) */
int  gxsp_g_drain(const char *why);             /* wait until the back has executed everything the front published; 0 ok, -1 gave up (logged) */
void gxsp_g_set_pins(int front_core);           /* (host tests: no-op) */

/* ---- R-front side (gx_thread.c's r_main) ---- */
void gxsp_f_publish(void);                      /* publish the RHI records written so far (cheap when none): called BEFORE the GX ring's tail is published, and when the GX ring runs dry */
void gxsp_f_present(uint32_t seq, int arm, struct RhiInstance *rhi, struct RhiSwapchain *sc);   /* forward the PRESENT: the back runs the real present and the done hook */
int  gxsp_f_quit(void);                         /* QUIT / MODE_LEAVE: drain the back and stop it (joins, bounded).  1 stopped */

/* the back's present-completion hook (gx_thread.c): runs on the BACK after the real present returned; it publishes r_presented */
typedef void (*GxspPresentDone)(uint32_t seq, int arm, uint32_t present_us);
void gxsp_set_present_done(GxspPresentDone f);

/* instruments */
void gxsp_print(uint32_t frames);               /* the "[gxsp]" window line (cumulative counters minus the last snapshot) */
uint32_t gxsp_stall_total(void);                /* front stalls so far (ring + arena + verdict + readback waits): the A/B judge reads it */
void gxsp_stall_report(void);                   /* watchdog: cursors, flags */

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_SPLIT_H */
