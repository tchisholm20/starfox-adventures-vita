/* paced_clock.h -- one step of the PACED virtual clock (os_shim.c, fixed_clock.txt = -N).  Pure C, no Vita headers: os_shim.c and the
 * host test (projects/stairfax-vita/host/audio/vita/test_dtk_stream.c, run by run_dtk_test.sh) compile the SAME function.
 *
 * At every retrace the game's clock advances by the fields (16,683 us) that really elapsed, at least 1 and at most `max`.
 *
 * THE CARRY (2026-11-06, audio choppy in ThornTail).  The first version rounded each frame to whole fields and then set
 * `last = now`, so the part of a field it did not count was thrown away every frame.  ThornTail's frames are 38-42 ms
 * (RC3 tour hub.frames.bin: 3,059 of 3,080 frames); every one rounded to 2 fields (33.4 ms), so the game clock lost ~7 ms a
 * frame: 13.4 s in 126 s, the game ran at ~89% speed, and the disc stream (dtk_stream.c follows game time) was held ~7.6 times
 * a second, 10-15 ms of silence each (RC9 log, ThornTail window: 106 holds, 1.57 s, in 600 presents).  Now `last` moves by the
 * fields counted, so what was not counted is counted at a later frame: the clock follows real time to within one field.
 * A frame longer than `max` fields is still a stall whose excess is lost for good (that is what the cap is for).
 *
 * DTK_FAULT 6 (test only) = the old rounding, which run_dtk_test.sh must see fail. */
#ifndef PACED_CLOCK_H
#define PACED_CLOCK_H

#define PACED_FIELD_US 16683ll

/* now_us: a monotonic microsecond clock.  *last_us: 0 before the first call.  Returns the fields to add (>= 1). */
static inline int paced_clock_step(long long now_us, long long* last_us, int max, unsigned* capped)
{
    long long el, f;
    int n;
    if (*last_us == 0) { *last_us = now_us; return 1; }
    el = now_us - *last_us;
#if defined(DTK_FAULT) && DTK_FAULT == 6
    f = (el + PACED_FIELD_US / 2) / PACED_FIELD_US;
    n = f < 1 ? 1 : f > max ? max : (int)f;
    if (f > max && capped) ++*capped;
    *last_us = now_us;
    return n;
#else
    f = el > 0 ? (el + PACED_FIELD_US / 2) / PACED_FIELD_US : 0;     /* rounded: the clock is within half a field of real time */
    n = f < 1 ? 1 : f > max ? max : (int)f;
    if (f > max) {                                                  /* a stall: its excess is lost, the clock restarts from now */
        if (capped) ++*capped;
        *last_us = now_us;
    } else {
        *last_us += (long long)n * PACED_FIELD_US;                  /* the carry: the uncounted part of a field stays in `el` */
        if (*last_us > now_us + PACED_FIELD_US) *last_us = now_us;  /* frames faster than a field (n forced to 1): never lead by more than one */
    }
    return n;
#endif
}
#endif
