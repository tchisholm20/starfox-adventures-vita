// game_sky.c - vertical slice harness for the real sky DLL (src/dlls/engine/5/5.c).
//
// This is the first REAL functional game DLL brought up end-to-end in the port: 5.c is
// compiled to x86, registered at resource id 5 (game_resource.c), and its real time-of-day
// logic runs. gameloop_main only ACQUIRES gSkyInterface; the callers (updateEnvironment/
// sceneDraw in lightmap.c) are stubbed, so this harness stands in for them - it drives the
// sky's real clock and reads the result, proving the DLL executes and produces correct
// output. Rendering paths (sun/moon, lighting) stay stubbed (game_sky_stubs.c) until the
// object/texture/curve subsystems come up.
//
// Observable proof: skyUpdateTimeOfDay() advances the REAL gSkyState->timeOfDay every
// frame; stairfax_sky_tick() converts it to a day/night brightness that game_scene uses
// to tint the sky - a real day/night cycle driven by the recompiled sky code.

#include <math.h>
#include <stdlib.h>
#include <stdio.h>

// sky DLL (5.c) exports we drive
extern unsigned char* gSkyState;         // u8* SkyState, allocated by skyResetState
extern int  gSkyObjectsInitialized;      // gate in skyUpdateTimeOfDay (set by loadSunAndMoon)
extern void skyResetState(void);
extern void skyUpdateTimeOfDay(void);

// SkyState field offsets (main/sky_state.h): timeOfDay @0x20C, timeOfDayRate @0x214.
#define SKY_TIMEOFDAY_OFF  0x20C
#define SKY_TIMEOFDAYRATE_OFF 0x214

static int gSkyReady;

void stairfax_sky_init(void) {
    skyResetState();                     // allocates gSkyState, sets noon + defaults
    gSkyObjectsInitialized = 1;          // enable the time update (skip sun/moon objects)
    gSkyReady = (gSkyState != 0);
    if (gSkyReady) {
        // Optional fast-forward so the day/night cycle is visible in seconds.
        const char* r = getenv("STAIRFAX_SKY_RATE");
        if (r) *(float*)(gSkyState + SKY_TIMEOFDAYRATE_OFF) = (float)atof(r);
        // Optional fixed time-of-day (seconds), e.g. 43200=noon, 0=midnight.
        const char* t = getenv("STAIRFAX_SKY_TIME");
        if (t) *(float*)(gSkyState + SKY_TIMEOFDAY_OFF) = (float)atof(t);
    }
    if (getenv("STAIRFAX_SKY_TEST"))
        fprintf(stderr, "[sky] init: gSkyState=%p ready=%d\n", (void*)gSkyState, gSkyReady);
}

// Advance the real sky clock one frame; return day brightness in [0,1] (1 = noon).
float stairfax_sky_tick(void) {
    if (!gSkyReady) return 0.5f;
    skyUpdateTimeOfDay();                 // REAL sky DLL code
    float tod = *(float*)(gSkyState + SKY_TIMEOFDAY_OFF);   // seconds-of-day [0,86400)
    float hour = tod / 3600.0f;
    float b = cosf((hour - 12.0f) / 12.0f * 3.14159265f) * 0.5f + 0.5f;

    if (getenv("STAIRFAX_SKY_TEST")) {
        static int f;
        if ((f++ % 30) == 0)
            fprintf(stderr, "[sky] t=%.0fs (%02d:%02d) brightness=%.2f\n",
                    tod, (int)hour % 24, (int)(tod / 60) % 60, b);
    }
    return b;
}
