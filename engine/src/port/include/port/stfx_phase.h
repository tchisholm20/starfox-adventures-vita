/* stfx_phase.h -- REST, split: the game loop's own phases, timed (2026-10-14, docs/47 perf phase, step 3).
 *
 * The [win] line's REST is frame CPU minus the four GX shim entries: the game's own work, unsplit (14.7 ms/frame in Tyler's
 * run-267 play). STFX_PH(id, stmt) times one phase of gameLoop()/gameUpdate() (main/gameloop.c) and snapshots the GX shim's
 * cumulative time (gx_win_times: display lists + immediate + texture load + EFB copy) around it, so every phase reports
 * its OWN CPU = inclusive - GX inside it. The phases are disjoint (no phase inside another), so their sum plus the GX time
 * outside them is the loop's whole time; the [ph] line prints the remainder as "outside the phases".
 *
 * ux0:data/stairfax/phase_stats.txt starting with 1 turns it on (read at present 1 by vita_tool_main.c). OFF: one load and
 * one untaken branch per phase. ON: two clock reads (~0.7 us each on the Vita) + two gx_win_times per phase, ~33 phases. */
#ifndef STFX_PHASE_H
#define STFX_PHASE_H

enum {
    PH_STATE, PH_WAIT, PH_PAD, PH_VOXT, PH_TEXT, PH_CAMFB, PH_UIF, PH_BTN, PH_OBJ, PH_ENV, PH_MAPEVT, PH_WATER, PH_UIE, PH_TRACK,
    PH_MAPCAM, PH_MAPLOAD, PH_LINKS, PH_CHKPT, PH_GXFLAGS, PH_SCENE, PH_SCREENS, PH_UIDRAW, PH_MINIMAP, PH_DVDERR, PH_GAMETEXT,
    PH_MUSIC, PH_VIEWPORT, PH_CAMSHAKE, PH_DATAFILES, PH_AUDIO, PH_SFX, PH_DEBUG, PH_TRANS, PH_SUBT, PH_GXFLUSH, PH_FREE,
    PH_MMFREE, PH_QLOADS,
    /* inside sceneRender (main/shader.c), nested: kept OUT of the loop's sum; sr_rest = sceneRender own - these (2026-10-14, run 271) */
    PH_SUB0, PH_S_VISGEOM = PH_SUB0, PH_S_LIGHTS, PH_S_REFL, PH_S_VISOBJ, PH_S_GEOM, PH_S_OBJS, PH_S_DEFER, PH_S_PART, PH_S_TRANSP,
    PH_S_FX, PH_S_GLOWS, PH_S_REST, PH_N
};

#if defined(__vita__)
extern int gStfxPhOn;
extern unsigned long long gStfxPhUs[PH_N], gStfxPhGx[PH_N];
extern unsigned gStfxPhN[PH_N];
unsigned long long stfx_ph_now(void);
unsigned long long stfx_ph_gx(void);
extern int gStfxObjDepth;                                                  /* [phobj]: objRender nesting (objprint.c) */
void stfx_obj_add(unsigned romDefNo, const char* name, unsigned long long us, unsigned long long gx);   /* [phobj]: one outermost objRender */
#define STFX_PH(id, stmt) do {                                                                                   \
        if (gStfxPhOn) {                                                                                         \
            const unsigned long long _pt = stfx_ph_now(), _pg = stfx_ph_gx();                                    \
            stmt;                                                                                                \
            gStfxPhGx[id] += stfx_ph_gx() - _pg; gStfxPhUs[id] += stfx_ph_now() - _pt; ++gStfxPhN[id];           \
        } else { stmt; }                                                                                         \
    } while (0)
#else
#define STFX_PH(id, stmt) do { stmt; } while (0)
#endif

#endif
