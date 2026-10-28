/* rhi_gxm_cap.h -- the frame cap's wait decision, shared by display_cb (rhi_gxm_frame.c) and host/tests/cap_wrap_test.c.
 *
 * 2026-10-28 HANG (Tyler, Krazoa Palace, eboot 76c12fa3): the first cap compared (int) (vcount - last) < min_vb - 1 in 32 bits. The
 * display counter wrapped (~65,536 vblanks = 18.2 min at 59.94 Hz into the boot), the difference went to ~-65535 and the display-queue
 * thread waited for vblanks forever: picture frozen, audio alive, the watchdog dump put SceGxmDisplayQueue in sceDisplayWaitVblankStart
 * under display_cb. Two guards, either of which alone would have stopped it:
 *   - the difference is taken in 16 bits (a real gap between flips is a handful of vblanks, never 65,536);
 *   - the caller waits at most GXM_CAP_MAX_WAITS vblanks, whatever the counter says. */
#ifndef RHI_GXM_CAP_H
#define RHI_GXM_CAP_H

#define GXM_CAP_MAX_WAITS 4

/* 1 = wait one more vblank before setting the next frame buffer */
static inline int gxm_cap_need_wait(unsigned now, unsigned last, int min_vb)
{
    return (int) ((now - last) & 0xFFFFu) < min_vb - 1;
}

#endif
