// stfx_phase.c - the [ph] line: the game loop's phases, own CPU vs GX inside (port/stfx_phase.h). 2026-10-14, docs/47 perf phase.
#if defined(__vita__)
#include <stdio.h>
#include <psp2/kernel/processmgr.h>
#include "port/stfx_phase.h"

int gStfxPhOn;
unsigned long long gStfxPhUs[PH_N], gStfxPhGx[PH_N];
unsigned gStfxPhN[PH_N];

static const char* const kPhName[PH_N] = {
    "stateTick", "waitNextFrame", "padUpdate", "voxmapTimers", "gameText", "camTargetFb", "uiFrameStart", "buttons",
    "Obj_UpdateAll", "updateEnvironment", "mapEventTimes", "waterFx", "uiFrameEnd", "trackIntersect", "mapCamPos",
    "pendingMapLoads", "parentLinks", "checkpoint", "gxFlags", "sceneRender", "screens", "uiDraw", "minimap", "dvdCheck",
    "gameTextRun", "music", "viewport", "camShakeFar", "loadDataFiles", "audioUpdate", "sfxLooped", "debugPrint",
    "screenTrans", "subtitles", "GXFlush", "deferredFree", "mmFreeTick", "queuedLoads",
    "sr_visGeom", "sr_lights", "sr_reflTex", "sr_visObjs", "sr_mapGeom", "sr_objects", "sr_deferred", "sr_particles", "sr_transp",
    "sr_effects", "sr_glows", "sr_rest",
};

unsigned long long stfx_ph_now(void) { return sceKernelGetProcessTimeWide(); }
extern void gx_win_times(unsigned long long out[8]);
/* THREADED (2026-10-23, docs/54 G1): the HAL runs on the render thread R, so gx_win_times' counters advance on R, concurrently with G's
 * phases. Subtracting them would make a G phase's "own" = its inclusive time minus R's work in the same window: meaningless. Threaded,
 * GX-inside is 0, so own = INCLUSIVE G time (the recording wrappers included: G's cost of issuing GX). gxt_is_active is weak: a build
 * without the render thread reads it as DIRECT. The [ph] line ends "| mode DIRECT|THREADED|MIXED" (MIXED: crossed a mode change). */
extern int gxt_is_active(void) __attribute__((weak));
static unsigned gPhThreadedSamples, gPhSamples;
unsigned long long stfx_ph_gx(void) {
    unsigned long long g[8];
    ++gPhSamples;
    if (gxt_is_active && gxt_is_active()) { ++gPhThreadedSamples; return 0; }
    gx_win_times(g);
    return g[0] + g[3] + g[4] + g[5];
}

void stfx_ph_enable(int on) { gStfxPhOn = on; }

/* [phobj] (2026-10-25 hub census): per romDefNo, the outermost objRender's inclusive CPU and the GX inside it. Open addressing, 512 slots;
 * a full table counts drops (printed) rather than merging types. */
#define OBJ_SLOTS 512
int gStfxObjDepth;
static unsigned short sObjKey[OBJ_SLOTS]; static unsigned char sObjUsed[OBJ_SLOTS];
static char sObjName[OBJ_SLOTS][12];
static unsigned long long sObjUs[OBJ_SLOTS], sObjGx[OBJ_SLOTS]; static unsigned sObjN[OBJ_SLOTS], sObjDrop;
void stfx_obj_add(unsigned key, const char* name, unsigned long long us, unsigned long long gx) {
    unsigned h = (key * 2654435761u) >> 23;
    for (int k = 0; k < OBJ_SLOTS; ++k, h = (h + 1) & (OBJ_SLOTS - 1)) {
        if (!sObjUsed[h]) {
            sObjUsed[h] = 1; sObjKey[h] = (unsigned short) key; sObjName[h][0] = 0;
            if (name) { int c = 0; for (; c < 11 && name[c] > 32 && name[c] < 127; ++c) sObjName[h][c] = name[c]; sObjName[h][c] = 0; }
        }
        if (sObjKey[h] == key) { sObjUs[h] += us; sObjGx[h] += gx; ++sObjN[h]; return; }
    }
    ++sObjDrop;
}
static void stfx_obj_report(int frames) {
    int idx[OBJ_SLOTS], n = 0; double tg = 0, tu = 0; unsigned tn = 0;
    for (int i = 0; i < OBJ_SLOTS; ++i) if (sObjUsed[i] && sObjN[i]) { idx[n++] = i; tg += (double) sObjGx[i]; tu += (double) sObjUs[i]; tn += sObjN[i]; }
    for (int i = 1; i < n; ++i) { int k = idx[i], j = i; while (j > 0 && sObjGx[idx[j - 1]] < sObjGx[k]) { idx[j] = idx[j - 1]; --j; } idx[j] = k; }
    printf("[phobj] objRender per romDefNo, GX ms/frame (own ms; renders/fr): total GX %.2f own %.2f, %.1f renders/fr, %d types, dropped %u |",
           tg / 1000.0 / frames, (tu - tg) / 1000.0 / frames, (double) tn / frames, n, sObjDrop);
    for (int q = 0; q < n && q < 24; ++q) {
        const int i = idx[q];
        printf(" 0x%03x:%s %.2f (%.2f; %.1f)", sObjKey[i], sObjName[i][0] ? sObjName[i] : "?", (double) sObjGx[i] / 1000.0 / frames, (double) (sObjUs[i] - sObjGx[i]) / 1000.0 / frames, (double) sObjN[i] / frames);
    }
    printf("\n");
    for (int i = 0; i < OBJ_SLOTS; ++i) { sObjUsed[i] = 0; sObjUs[i] = sObjGx[i] = 0; sObjN[i] = 0; }
    sObjDrop = 0;
}

/* Every 60 presents (vita_tool_main.c, after the [win] line): the phases over 0.3 ms/frame own or GX, largest own first, then
 * the total. frames = presents in the window. Reset after printing. */
void stfx_ph_report(int frames) {
    if (!gStfxPhOn || frames <= 0) return;
    int idx[PH_N]; double own[PH_N];
    double sumOwn = 0, sumGx = 0;
    for (int i = 0; i < PH_N; ++i) {
        idx[i] = i;
        own[i] = (double) (gStfxPhUs[i] - gStfxPhGx[i]) / 1000.0 / frames;
        if (i < PH_SUB0) { sumOwn += own[i]; sumGx += (double) gStfxPhGx[i] / 1000.0 / frames; }
    }
    { /* sr_rest: sceneRender's own CPU outside its timed sub-phases */
        unsigned long long su = 0, sg = 0;
        for (int i = PH_SUB0; i < PH_S_REST; ++i) { su += gStfxPhUs[i]; sg += gStfxPhGx[i]; }
        gStfxPhUs[PH_S_REST] = gStfxPhUs[PH_SCENE] - su; gStfxPhGx[PH_S_REST] = gStfxPhGx[PH_SCENE] - sg; gStfxPhN[PH_S_REST] = gStfxPhN[PH_SCENE];
        own[PH_S_REST] = (double) ((long long) gStfxPhUs[PH_S_REST] - (long long) gStfxPhGx[PH_S_REST]) / 1000.0 / frames;
    }
    for (int i = 1; i < PH_N; ++i) { int k = idx[i], j = i; while (j > 0 && own[idx[j - 1]] < own[k]) { idx[j] = idx[j - 1]; --j; } idx[j] = k; }
    printf("[ph] game loop, own CPU (GX inside) ms/frame: phases own %.2f + GX %.2f |", sumOwn, sumGx);
    for (int q = 0; q < PH_N; ++q) {
        const int i = idx[q]; const double gx = (double) gStfxPhGx[i] / 1000.0 / frames;
        if (own[i] < 0.3 && gx < 0.3) continue;
        printf(" %s %.2f (%.2f; %.1f/fr)", kPhName[i], own[i], gx, (double) gStfxPhN[i] / frames);
    }
    printf(" | mode %s\n", gPhThreadedSamples == 0 ? "DIRECT" : gPhThreadedSamples == gPhSamples ? "THREADED" : "MIXED");
    stfx_obj_report(frames);
    gPhThreadedSamples = gPhSamples = 0;
    for (int i = 0; i < PH_N; ++i) { gStfxPhUs[i] = gStfxPhGx[i] = 0; gStfxPhN[i] = 0; }
}
#endif
