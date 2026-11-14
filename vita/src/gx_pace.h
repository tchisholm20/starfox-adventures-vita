/* gx_pace.h -- the game thread's present pacer (2026-11-10), shared by gx_thread.c and host/tests/gx_pace_test.c.
 *
 * WHY (measured). In the front end (map 63, hw session rc9c_before, 14,770 frames, 30 fps cap on) G's present-to-present time alternated ~51 ms /
 * ~16 ms for eight minutes, with G work 30 / 13 ms and R 21 / 14 ms: mean 33.4, both threads under budget, 40% of frames over 41.7 ms. The flips are
 * capped to 2 vblanks (frame_cap.txt), so a 16 ms frame cannot show an extra picture; it makes the game's time step alternate 3 fields / 1 field.
 * The MECHANISM of the cycle is NOT established (a host model of display-queue slack did not reproduce it; a time-step feedback on G's work is the
 * other candidate). The pacer removes the 16 ms return whichever it is; the device A/B (g_pace.txt "ab 600", frame log, map 63) is the judge.
 *
 * WHAT. With the 30 fps cap on, G does not return from a present sooner than `period` after its previous return. A late frame is never held.
 * The period sits a little UNDER two vblanks (33,367 us) so the pacer can never run G slower than the display; R's gate does the fine timing. */
#ifndef GX_PACE_H
#define GX_PACE_H
#include <stdint.h>

#define GXT_PACE_PERIOD_US 32000u

/* microseconds still to hold at `now`, given the previous return at `last` (both wrapping 32-bit microsecond clocks); period 0 = pacer off */
static inline uint32_t gxt_pace_hold_us(uint32_t now, uint32_t last, uint32_t period)
{
    const uint32_t el = now - last;
    return (period && el < period) ? period - el : 0u;
}

/* "ab N" arm: 1 = pacer on for this present. Windows of N presents alternate ON, OFF, ON, ... starting ON. n 0 = always on. */
static inline int gxt_pace_ab_on(uint32_t seq, uint32_t n)
{
    return n ? ((seq / n) & 1u) == 0u : 1;
}

#endif
