// game_player_stubs.c - Phase A stub surface for the player DLL (195_Player/player.c) and
// the engine/15 motion-control interface. Auto-generated first pass: safe return-0 function
// stubs + real storage for the data globals the player references. Bare-symbol linkage (C
// mismatch across TUs is fine). Replace individual entries with faithful behavior as Phase A
// runtime exposes what actually needs to compute (motion math, camera, ground collision).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
// --- data globals ---
void* gIntersectLinePool;
void* gIntersectPoints;
// Real .sdata2 float constants from the retail player DLL (player.o .sdata2, offsets 0x3C/0x78/0xAC
// off base 0x803E7E68). lbl_803E7EE0 is the default targetAnimSpeed / yawSmoothScale (a divisor) -
// 0 here made the player's position go NaN; the real value is 1.0.
float lbl_803E7EA4 = 0.0f;
float lbl_803E7EE0 = 1.0f;
float lbl_803E7F14 = 0.2f;

// --- function stubs (return 0; refine when a runtime crash proves one needs real behavior) ---
int AudioStream_Play(void) { return 0; }
int AudioStream_StartPrepared(void) { return 0; }
int AudioStream_StopCurrent(void) { return 0; }
int Carryable_putDownAndSavePos(void) { return 0; }
int GXSetColorUpdate(void) { return 0; }
// out = 3x3 rotation part of the 3x4 matrix * vector (no translation). A no-op stub left `out`
// uninitialized -> NaN directions feeding the player's probe rays. (mirrors Matrix_TransformPoint)
void Matrix_TransformVector(const float* m, const float* v, float* out) {
    out[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2];
    out[1] = m[4]*v[0] + m[5]*v[1] + m[6]*v[2];
    out[2] = m[8]*v[0] + m[9]*v[1] + m[10]*v[2];
}
int ObjHits_DisableObject(void) { return 0; }
int ObjHits_EnableObject(void) { return 0; }
int ObjHits_GetPriorityHit(void) { return 0; }
int ObjHits_GetPriorityHitWithPosition(void) { return 0; }
int ObjHits_IsObjectEnabled(void) { return 0; }
int ObjHits_MarkObjectPositionDirty(void) { return 0; }
int ObjHits_RecordObjectHit(void) { return 0; }
int ObjHits_RecordPositionHit(void) { return 0; }
int ObjHits_SyncObjectPositionIfDirty(void) { return 0; }
int ObjLink_AttachChild(void) { return 0; }
int ObjLink_DetachChild(void) { return 0; }
int ObjMsg_AllocQueue(void) { return 0; }
int ObjMsg_Pop(void) { return 0; }
int ObjMsg_SendToObject(void) { return 0; }
int ObjPath_GetPointModelMtx(void) { return 0; }
int ObjPath_GetPointWorldPosition(void) { return 0; }
int ObjPath_GetPointWorldPositionArray(void) { return 0; }
int Obj_GetYawDeltaToObject(void) { return 0; }
int Obj_IsObjectAlive(void) { return 0; }
int Obj_SetParent(void) { return 0; }
int Pause_ResetMenuFrameCounter(void) { return 0; }
int RandomTimer_UpdateRangeTrigger(void) { return 0; }
int Rcp_SetSpiritVisionEnabled(void) { return 0; }
int SB_Galleon_getCameraState(void) { return 0; }
int Sfx_IsPlayingFromObject(void) { return 0; }
int Sfx_KeepAliveLoopedObjectSound(void) { return 0; }
int Sfx_PlayAtPositionFromObject(void) { return 0; }
int Sfx_StopFromObject(void) { return 0; }
int Shield_setMode(void) { return 0; }
int SmallBasket_throw(void) { return 0; }
// Faithful vector math (a no-op stub leaves vectors unnormalized -> downstream divisions NaN the
// player's position). Vec3_Normalize normalizes in place and returns the original length.
float Vec3_Normalize(float* v) {
    float len2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (len2 > 1e-12f) { float len = sqrtf(len2), inv = 1.0f/len;
        v[0]*=inv; v[1]*=inv; v[2]*=inv; return len; }
    return 0.0f;
}
// Faithful: index of `value` in array[count], or -1 if absent. A return-0 stub made callers'
// `arrayIndexOf(...) != -1` guards always true (e.g. the player's vehicle-sync ran on a garbage
// focusObject and crashed).
int arrayIndexOf(int* array, int count, int value) {
    int i; for (i = 0; i < count; ++i) if (array[i] == value) return i;
    return -1;
}
int arwprojectile_createLinkedEffect(void) { return 0; }
int arwprojectile_placeForward(void) { return 0; }
int arwprojectile_setLifetime(void) { return 0; }
int characterDoEyeAnims(void) { return 0; }
// curves_preparePointCollisionFrame / curves_updateLocalPointTransforms are now real (engine/21).
int enemy_getCurveParams(void) { return 0; }
int enemy_getFreezeRecoverSeconds(void) { return 0; }
int getCurSeqNo(void) { return 0; }
int getFocusedNpc(void) { return 0; }
int getSbGalleon(void) { return 0; }
int getYButtonItem(void) { return 0; }
// gxSetOpaqueZWriteMode/gxTevCommitStages/gxTevResetStages/gxTevTextureTimesColor1Stage
// now provided real by intersect_render.c (text/2D render bring-up) - stubs removed.
int hudSetMagicCostPreview(void) { return 0; }
// FLOAT-returning functions stubbed as int return garbage in the FP register (xmm0), not 0 - the
// caller reads that garbage as a float, so e.g. `animSpeedA *= powfBitEstimate(...)` -> NaN position.
// Implemented faithfully (interpolate/powfBitEstimate per vecmath.c; the rest standard math).
// powfBitEstimate is defined below in this file; declare it here so interpolate's call passes
// float args (not default-promoted to double). Without this prototype the call corrupts base/exp
// (double byte-halves read as floats) and every interpolate() smoothing step diverges to NaN.
float powfBitEstimate(float base, float exp);

float interpolate(float a, float t, float exp) {
    return (t <= 1.0f) ? a * (1.0f - powfBitEstimate(1.0f - t, exp)) : 0.0f;
}
// isInBounds(localPosX, localPosZ): is the player within the level's playable bounds. Returning
// 0 makes playerUpdate take its out-of-bounds RESET branch every frame (zeroing velocity, skipping
// the whole locomotion update), so the character never responds. Report in-bounds (1) so the real
// state machine runs. Refine with the real bounds test when the map bounds data is wired.
int isInBounds(void) { return 1; }
int lightmapDrawTriangleList(void) { return 0; }
int logPrintf(void) { return 0; }
float mathCosfHighPrecision(float angle) { return cosf(angle); }
int objAudioDispatchAnimEvents(void) { return 0; }
int objAudioDispatchEventMask(void) { return 0; }
int objDoHitParticleFx(void) { return 0; }
int objDoParticleFx(void) { return 0; }
int objFindJointPoseVector(void) { return 0; }
int objFindTexture(void) { return 0; }
int objGetJointWorldPosition(void) { return 0; }
int objGetNearestTypeTo(void) { return 0; }
int objPosToMapBlockIdx(void) { return 0; }
int objSetAnimField48to0(void) { return 0; }
int objfx_shakeCameraByDistance(void) { return 0; }
// playerHasKrazoaSpirit is now real (engine/21). The save globals engine/21's link-only save helpers
// reference (declared in dll_0017_savegame_api.h; save DLL not compiled): dummy backing storage so
// the link resolves - the save-settings path is never exercised for the player.
unsigned char saveData[0xE4];          /* SAVE_DATA_SIZE */
unsigned char gSaveGameData[0x400];
int playerShadowClearPositionOverride(void) { return 0; }
int playerUpdateBlinkAnimation(void) { return 0; }
float powfBitEstimate(float base, float exp) { return powf(base, exp); }
int setAButtonIcon(void) { return 0; }
int setBButtonIcon(void) { return 0; }
int setHudForceShowMask(void) { return 0; }
int setPendingMapLoad(void) { return 0; }
int setTextColor(void) { return 0; }
int showDeathMenu(void) { return 0; }
int staffDoGrowShrinkAnim(void) { return 0; }
int staffStartQuakeSpell(void) { return 0; }
int staffactivated_calcInteractionTargetXZ(void) { return 0; }
int staffactivated_getLiftHeight(void) { return 0; }
int staffactivated_getMode(void) { return 0; }
int staffactivated_getPullRateMode(void) { return 0; }
int staffactivated_isGameBitMirrorSet(void) { return 0; }
int staffactivated_setGameBitMirror(void) { return 0; }
int staffactivated_setLiftHeight(void) { return 0; }
int staffactivated_spawnMapEventDebris(void) { return 0; }
int surfaceSfxSelectTrigger(void) { return 0; }
// --- Ground collision (Phase C, first pass: a flat walkable floor) --------------------------
// The player probes the terrain via trackGetHeight/trackGetNearestGroundOffset; the real return-0
// stubs reported "no ground", so gravity dropped her through the world forever. Until the real map
// collision is wired, report a flat, fully-walkable floor at gStairfaxGroundY (set by the harness
// to the spawn Y). TrackGroundHit = {f32 height@0, normalX@4, normalY@8, normalZ@0xC} size 0x18.
float gStairfaxGroundY = -1.0e9f;
static unsigned char gGroundHit[0x18];
static void* gGroundHitPtr = gGroundHit;
int trackGetHeight(void* obj, float x, float y, float z, void*** hitsOut, int mode, int mask) {
    (void)obj; (void)x; (void)y; (void)z; (void)mode; (void)mask;
    *(float*)(gGroundHit + 0x0) = gStairfaxGroundY;  // height
    *(float*)(gGroundHit + 0x4) = 0.0f;              // normalX
    *(float*)(gGroundHit + 0x8) = 1.0f;              // normalY (flat -> walkable)
    *(float*)(gGroundHit + 0xC) = 0.0f;              // normalZ
    if (hitsOut) *hitsOut = (void**)&gGroundHitPtr;
    return 1;
}
int trackGetLineIntersect(float* from, float* to, float radius, int mode, void* hit, void* self, int flags, int mask, int slot, int arg10) {
    (void)from;(void)to;(void)radius;(void)mode;(void)hit;(void)self;(void)flags;(void)mask;(void)slot;(void)arg10;
    return 0;   // no wall/probe hits yet (real swept-line collision is future work)
}
int trackGetNearestGroundOffset(void* obj, float x, float y, float z, float* outGroundOffset, int mask) {
    (void)obj; (void)x; (void)z; (void)mask;
    if (outGroundOffset) *outGroundOffset = y - gStairfaxGroundY;  // height above the floor
    return 1;
}
// trackGetIntersect: the segment sweep whose return becomes CurvesCollisionState.surfaceFlags
// (engine/21 curves_advanceCollision:985) - THE signal that makes the player state machine consider
// herself grounded. `results` = &segmentHits (TrackHitResults, planes[4][4]@0, surfaceTypes@0x50,
// hitCount@0x6C). Report a flat walkable floor: up-normal (0,1,0) for the first hit, one hit, and the
// surfaceFlags mask 0x2 (on ground) | 0x10 (HAS_NEARBY_FLOOR) - which satisfies engine/15's velocity
// feedback gate (15.c:1021) and the idle->moving transition, without 0x1 (water/floor-resolve).
int trackGetIntersect(void* contactSource, float* startPoints, float* endPoints, int pointCount,
                      void* results, int flags) {
    (void)contactSource; (void)startPoints; (void)endPoints; (void)pointCount; (void)flags;
    if (results) {
        float* planes = (float*)results;                 // planes[0] = {nx,ny,nz,d}
        planes[0] = 0.0f; planes[1] = 1.0f; planes[2] = 0.0f; planes[3] = 0.0f;
        *(signed char*)((unsigned char*)results + 0x50) = 0;   // surfaceTypes[0]: walkable (not water 0x21)
        *(short*)((unsigned char*)results + 0x6C) = 1;         // hitCount = 1
        *(unsigned char*)((unsigned char*)results + 0x6E) = 1; // hitMask
    }
    return 0x12;
}
// No-op collision helpers engine/21 calls over the flat floor (no dynamic track triangles yet).
void trackIntersectBroadphase(void* obj, void* bounds, unsigned mask, int flags) { (void)obj;(void)bounds;(void)mask;(void)flags; }
void trackInvalidateDynamicSlotsForObject(void* target) { (void)target; }
void ObjHits_AddContactObject(void* obj, void* contactObj) { (void)obj; (void)contactObj; }
// Link-only settings sinks reached from engine/21's loadSaveSettings (not on the player path).
int setWidescreen(void) { return 0; }
// setSubtitlesEnabled now provided by the real text cluster (subtitle.c) - stub removed.
int audioSetSoundMode(void) { return 0; }
int audioSetVolumes(void) { return 0; }
int trickyImpress(void) { return 0; }
float vec3f_distanceSquared(float* a, float* b) {
    float dx=a[0]-b[0], dy=a[1]-b[1], dz=a[2]-b[2]; return dx*dx+dy*dy+dz*dz;
}
int viewFinderSetZoom(void) { return 0; }
int viewFinderSetZoomTo50(void) { return 0; }
