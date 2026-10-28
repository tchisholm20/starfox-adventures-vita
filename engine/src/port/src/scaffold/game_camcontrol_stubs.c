// game_camcontrol_stubs.c - link stand-ins for engine/1 camcontrol.c + engine/66 (0x42 follow cam).
//
// Real-camera bring-up Stage 2. camcontrol.c + 66.c compile and link against the port's existing
// object/track/player/curve subtrees for the *core* follow-cam path (Camera_update -> activate
// handler -> CameraModeNormal_update -> follow/slide/wall-avoidance). What remains here is the
// small tail of TARGET-RETICLE / ENEMY-FEEDBACK / MAP-STREAMING cosmetics camcontrol calls when
// the player locks onto a target, plus one vertical-bounds collision helper in engine/66 - none on
// the plain "walk around and the camera follows" path. Return types match the real prototypes
// EXACTLY (a float-returning function stubbed as int puts garbage in the FP return -> NaN), and the
// one out-param writer (hitDetect_calcSweptSphereBounds) fills its output for real so downstream
// bounds math never sees garbage.
//
// C linkage decorates by name only, so plain void*/short stand in for the GameObject*/s16 typedefs.

extern float floorf(float);
extern float ceilf(float);

// --- camcontrol target-reticle / feedback cosmetics (only hit while locked onto a target) -------
int   isFrontEndUiActive(void)                      { return 0; }   // front-end UI not up in-game
int   isTalkingToNpc(void)                          { return 0; }   // s32; not in a conversation
int   dll_19_isBaddieControlObject(void* obj)       { (void)obj; return 0; }
float enemy_getHealthFraction(void* obj)            { (void)obj; return 1.0f; }  // f32: full health
float LargeCrate_getReticleDistance(void* obj)      { (void)obj; return 1000.0f; } // f32: far (no div-by-0)
void  objShowButtonGlow(void* obj, float intensity, unsigned char mode) { (void)obj; (void)intensity; (void)mode; }
void  SaveGame_setCamActionNo(short actionNo)       { (void)actionNo; }
void  modelLightStruct_setTransformMode(void* light, int mode)          { (void)light; (void)mode; }
void  modelLightStruct_setObjectLightMaskIndex(void* light, int idx)    { (void)light; (void)idx; }

// Map streaming keyed off camera position: the port already loads the map elsewhere, so the camera
// does not drive streaming yet. No-op = the loaded map stays put as the camera moves.
void  loadMapForCameraPos(float x, float y, float z) { (void)x; (void)y; (void)z; }

// engine/68 viewfinder (0x44): toggles the first-person viewfinder HUD overlay (cosmetic; no HUD yet).
void  Rcp_SetViewFinderHudEnabled(unsigned char x) { (void)x; }

// engine/75 climb cam (0x4B) blur + engine-DoF blur (cosmetic; RHI has no blur pass yet).
void  Rcp_DisableBlurFilter(void) {}
void  turnOnBlurFilter(float x, float y, float z, unsigned char useArea, unsigned char bigger)
      { (void)x;(void)y;(void)z;(void)useArea;(void)bigger; }

// engine/75 climb-camera tuning constants (f32). These are the DLL's own .data - DOL-recoverable like
// the player tables (TODO: extract exact retail values from engine/75's .data). The trig/angle ones
// are exact from usage (mathSinf(Pi*rotX/HalfCircle) is the standard s16->radian); the radii/rates/
// heights are sane placeholders so the climb cam activates without NaN until the real bytes are pulled.
// Climb is situational (wall/ladder), not on the normal-locomotion path.
float gCamClimbPi                    = 3.14159265f;
float gCamClimbHalfCircleBinaryAngle = 32768.0f;      // 180deg in s16 binary angle
float gCamClimbDegreesToBinaryAngle  = 182.04445f;    // 65536/360
float gCamClimbTraceRadius           = 20.0f;         // placeholder
float gCamClimbTraceOrbitRadius      = 30.0f;         // placeholder
float gCamClimbDistanceSmoothRate    = 0.1f;          // placeholder
float gCamClimbDefaultHeightAdjustRate = 0.09f;       // placeholder
float gCamClimbDefaultDistanceScale  = 1.0f;          // placeholder
float gCamClimbDefaultEndMinHeight   = 0.0f;          // placeholder
float gCamClimbDefaultEndMaxHeight   = 60.0f;         // placeholder
float lbl_803E19A0                   = 0.0f;          // in-bounds height delta = no adjust (75.c:85)

// --- engine/66 vertical-bounds collision --------------------------------------
// Writes boundsOut = { minX,minY,minZ, maxX,maxY,maxZ } (s32 world coords). Real behaviour: the
// integer AABB enclosing each swept sphere (segment start..end expanded by its radius). Computing
// it for real (cheap) keeps CameraModeNormal_updateVerticalBounds' downstream track query valid;
// a zeroed/garbage box could feed NaN into the vertical clamp.
void hitDetect_calcSweptSphereBounds(int* boundsOut, float* startPoints, float* endPoints,
                                     float* radii, int pointCount) {
    float mn[3] = { 1e30f, 1e30f, 1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };
    int i, a;
    if (pointCount <= 0) { for (a = 0; a < 6; ++a) boundsOut[a] = 0; return; }
    for (i = 0; i < pointCount; ++i) {
        float r = radii ? radii[i] : 0.0f;
        for (a = 0; a < 3; ++a) {
            float s = startPoints[3 * i + a], e = endPoints[3 * i + a];
            float lo = (s < e ? s : e) - r, hi = (s > e ? s : e) + r;
            if (lo < mn[a]) mn[a] = lo;
            if (hi > mx[a]) mx[a] = hi;
        }
    }
    boundsOut[0] = (int)floorf(mn[0]); boundsOut[1] = (int)floorf(mn[1]); boundsOut[2] = (int)floorf(mn[2]);
    boundsOut[3] = (int)ceilf(mx[0]);  boundsOut[4] = (int)ceilf(mx[1]);  boundsOut[5] = (int)ceilf(mx[2]);
}
