// game_object_stubs.c - link stand-ins for object.c's siblings (objhits/objanim/objlib/
// objprint/player/map/model/math), so the real object.c links. The object vertical slice
// exercises only Obj_InitObjectSystem + Obj_UpdateAllObjects over an EMPTY object list, so
// none of the per-object helpers below are reached at runtime; they only need to LINK. As
// real objects get spawned, replace these with the real TUs one at a time. Headerless
// (bare-symbol linkage) to avoid signature conflicts, like game_boot_stubs.c.


#include <stdlib.h>   // calloc (ObjHits_AllocObjectState state buffer)

// data globals object.c reads (player/map offsets)
float gMapSavedPlayerOffsetX;
float gMapSavedPlayerOffsetZ;
float playerMapOffsetX;
float playerMapOffsetZ;

// function stubs
int AudioStream_StopAll(void) { return 0; }
int ObjContact_RemoveObjectCallbacks(void) { return 0; }
// These run inside loadCharacter's allocation-layout section as `cursor = fn(..., cursor)`,
// advancing a bump cursor within the object allocation. The hit/hitbox subsystems aren't ported,
// but the stubs MUST return the cursor UNCHANGED (not 0) - returning 0 zeroed the cursor and made
// every later sub-buffer (jointPoseData, textureSlots, hitVolumes...) a null/garbage pointer.
int ObjHitReact_InitState(int romDefNo, void* bank, void* state, int cursor, void* anim) {
    (void)romDefNo; (void)bank; (void)state; (void)anim; return cursor;   // cursor = 4th arg
}
int ObjHitReact_ResetActiveObjects(void) { return 0; }
int ObjHitReact_UpdateResetObjects(void) { return 0; }
int ObjHitbox_AllocRotatedBounds(void* obj, int cursor) { (void)obj; return cursor; }
// The real ObjHits_AllocObjectState carves an ObjHitsPriorityState (~0xB8 bytes) from the
// bump arena and stores it at obj->anim.hitReactState (GameObject+0x54). The real player's
// playerRefreshCollisionState writes localPos/worldPos into it (offsets 0x10/0x1C), so a null
// hitReactState faults. Give each object its own zeroed state buffer (the ObjHits collision
// SYSTEM stays stubbed, so only the player's own writes land here) and keep the cursor
// unchanged (existing contract for downstream sub-buffers).
int ObjHits_AllocObjectState(void* obj, int cursor) {
    void* st = calloc(1, 0xC0);
    *(void**)((char*)obj + 0x54) = st;   // obj->anim.hitReactState
    return cursor;
}
int ObjHits_InitWorkBuffers(void) { return 0; }
int ObjHits_ResetWorkBuffers(void) { return 0; }
int ObjHits_TickPriorityHitCooldowns(void) { return 0; }
int ObjHits_Update(void) { return 0; }
int PSMTXMultVec(void) { return 0; }
int PSMTXRotAxisRad(void) { return 0; }
int PSVECCrossProduct(void) { return 0; }
int Sfx_PlayFromObject(void) { return 0; }
int Sfx_RemoveLoopedObjectSoundForObject(void) { return 0; }
int Sfx_StopObjectChannel(void) { return 0; }
int basisVectorsToEulerAngles(void) { return 0; }
int debugPrintf(void) { return 0; }
int getCurMapType(void) { return 0; }
// getCurUiDll now real in bridge/game_model_support.c (uiDll spine) - stub removed.
int getTabEntry(void) { return 0; }
int intersectModLineBuild(void) { return 0; }
int mapLoadForObject(void) { return 0; }
int mapUnloadRomListPage(void) { return 0; }
// mathSinf(radians): the game's sine (arg already in radians, e.g. pi*angle/32768). It was an
// int return-0 stub - a float-returning function stubbed as int leaves garbage in xmm0, so the
// player's heading math (velocityX/Z = speed*mathSinf(yaw)) went NaN the first locomotion frame.
extern float sinf(float);
float mathSinf(float radians) { return sinf(radians); }
int mtx44Transpose(void) { return 0; }
int mtxRotateByVec3s(void) { return 0; }
int newshadows_getSmallDiskTexture(void) { return 0; }
int objAddObjectType(void) { return 0; }
// objCausticReflectionRenderCb now real in intersect_render.c - removed.
int objFreeObjectType(void) { return 0; }
// out-param: must zero *count (Obj_GetPlayerObject reads it before indexing).
void* objGetAllOfType(int type, int* count) { (void)type; if (count) *count = 0; return 0; }
int objGetObjectType(void) { return 0; }
int objListAdd(void) { return 0; }
int objListInit(void) { return 0; }
int objList_remove(void) { return 0; }
// objLoadPlayerFromSave now provided by the real player DLL (player.c) - stub removed (Phase A).
// objModelNormalDiskRenderCb/objModelProjectedIndirectRenderCb now real in intersect_render.c - removed.
int objTypeInit(void) { return 0; }
// playerDoHitDetection/playerFree/playerUpdate/playerUpdateWhileTimeStopped are now provided
// by the real player DLL (src/dlls/objects/195_Player/player.c); stubs removed (Phase A).
// Also a loadCharacter allocation-cursor advancer (cursor = shadowInit(obj, cursor, 0)); shadows
// aren't ported but the stub MUST return the cursor unchanged so later sub-buffers stay valid.
int shadowInit(void* obj, int cursor, int flag) { (void)obj; (void)flag; return cursor; }
int shadowVolumesSetDirty(void) { return 0; }
int staffUpdateWhileTimeStopped(void) { return 0; }
int trackTickDynamicSlotCooldowns(void) { return 0; }
