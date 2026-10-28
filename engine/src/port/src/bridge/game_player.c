// game_player.c - Phase A vertical-slice harness for the real player DLL
// (src/dlls/objects/195_Player/player.c + the engine/15 motion-control interface).
//
// The game dispatches the player specially: Obj_UpdateAllObjects hard-cases romDefNo
// SABRE/KRYSTAL to playerUpdate(obj) (object.c). The port doesn't run Obj_UpdateAllObjects
// live yet, so this harness stands in - it calls the REAL playerUpdate(gPlayerObj) once per
// frame (like game_sky drives skyUpdateTimeOfDay), feeding real pad input, so the recompiled
// player state machine runs and drives the object's motion + animation. Gated on
// STAIRFAX_PLAYER_DLL so the interim hand-driven path stays the default until this is proven.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
extern float sinf(float), cosf(float);   // pull from CRT without <math.h> (project pattern)

extern unsigned char* gPlayerObj;    // spawned Sabre/Krystal GameObject (game_scene.cpp)
extern float timeDelta;              // per-frame time (frames); the port never set it -> 0 -> the
extern unsigned char framesThisStep, framesThisStepUnclamped;  // motion integration divided by 0 -> NaN pos
extern void playerUpdate(void* obj);          // real player DLL entry (player.c)
extern void objLoadPlayerFromSave(void* obj); // real player init (player.c) - Obj_RunInitCallback's
                                              // SABRE/KRYSTAL case; populates PlayerState from the save

// --- Player data-table reconstruction --------------------------------------------------------
// player.c reads its move/param tables as `lbl_80332EC0 + offset` (up to ~+0x1c00): in the retail
// player DLL those are ONE contiguous 0x2028-byte .data blob, which the decomp split into ~17 named
// sub-globals whose retail addresses/offsets we recovered from the DLL object (dtk `dol split` of
// main.dol -> build/GSAE01_rev1/obj/.../player.o, .data section). MSVC scatters those globals, so
// `base + offset` reads garbage. lbl_80332EC0 is enlarged to the full 0x2028 span (player.c); here
// we lay the real sub-tables back into it at their retail offsets before the player runs. Values
// come from player.c's own (already host-endian) definitions - no byte-swap needed. Offsets are
// DLL-internal and rev-independent. Runtime-written regions (gPlayerSpawnedObjects @0x14) stay 0.
extern int lbl_80332EC0[];   // the 0x2028-byte destination table (player.c)
extern unsigned char lbl_80332EF0[], lbl_80332F2C[], lbl_80332F48[], lbl_80332F78[], lbl_80332F88[];
extern unsigned char gPlayerAnimSpeedThresholds[], gPlayerMoveTableA[], lbl_80333110[];
extern unsigned char gPlayerMoveTableB[], gPlayerSurfacePfxModeTable[], gPlayerDefaultMoveParams[];
extern unsigned char gPlayerMotionTuning[], lbl_8033366C[], lbl_8033369C[], gPlayerMoveSlotTable[];
extern unsigned char gPlayerMoveSlotData[], gPlayerSpellGameBits[];

static const struct { unsigned off, size; const unsigned char* src; } gPlayerTableParts[] = {
    { 0x0030, 0x3C,   lbl_80332EF0 },
    { 0x006C, 0x1C,   lbl_80332F2C },
    { 0x0088, 0x30,   lbl_80332F48 },
    { 0x00B8, 0x10,   lbl_80332F78 },
    { 0x00C8, 0x38,   lbl_80332F88 },
    { 0x0100, 0x90,   gPlayerAnimSpeedThresholds },
    { 0x0190, 0xC0,   gPlayerMoveTableA },
    { 0x0250, 0x100,  lbl_80333110 },
    { 0x0350, 0x1C,   gPlayerMoveTableB },
    { 0x036C, 0x24,   gPlayerSurfacePfxModeTable },
    { 0x0390, 0x60,   gPlayerDefaultMoveParams },
    { 0x03F0, 0x3BC,  gPlayerMotionTuning },
    { 0x07AC, 0x30,   lbl_8033366C },
    { 0x07DC, 0x20,   lbl_8033369C },
    { 0x07FC, 0x58,   gPlayerMoveSlotTable },
    { 0x0854, 0x1340, gPlayerMoveSlotData },
    { 0x1B94, 0x68,   gPlayerSpellGameBits },
};

static void stairfax_player_build_table(void) {
    unsigned char* base = (unsigned char*)lbl_80332EC0;
    unsigned i;
    for (i = 0; i < sizeof(gPlayerTableParts)/sizeof(gPlayerTableParts[0]); ++i)
        memcpy(base + gPlayerTableParts[i].off, gPlayerTableParts[i].src, gPlayerTableParts[i].size);
}
// Vita (2026-10-07, docs/47 run 118): the table was built ONLY from stairfax_player_dll_tick below, a
// desktop-viewer path behind getenv("STAIRFAX_PLAYER_DLL"). The Vita runs the game's own loop, so
// every `lbl_80332EC0 + offset` read past the first 0x14 bytes was ZERO there: in the prologue
// player_SeqFn's vehicle list (base + 0x13c: 114, ..., 140 = SB_Cloudrun, ...) matched nothing and
// Krystal never mounted the CloudRunner (retail: she takes its rotation and rider position; Vita:
// she stood at the sequence's placement). Built before main() now; the sources are static data.
int stairfax_player_table_built;
#if defined(__GNUC__)
__attribute__((constructor)) static void stairfax_player_build_table_ctor(void) {
    stairfax_player_build_table();
    stairfax_player_table_built = 1;
}
#endif

static int gEnabled = -1;

int stairfax_player_dll_enabled(void) {
    if (gEnabled < 0) gEnabled = getenv("STAIRFAX_PLAYER_DLL") ? 1 : 0;
    return gEnabled;
}

// Run one frame of the real player logic over the spawned character.
void stairfax_player_dll_tick(void) {
    if (!stairfax_player_dll_enabled() || !gPlayerObj) return;

    // One-time init: the game runs Obj_RunInitCallback on spawn, which for the player calls
    // objLoadPlayerFromSave to populate PlayerState (move tables, param curves, yaw, timers,
    // sub-interface init). The port's spawn path doesn't run that callback, so do it once here
    // before the first playerUpdate - without it the state is raw and the update null-derefs.
    static int inited = 0;
    int tr = getenv("STAIRFAX_PLAYER_DLL_TRACE") ? 1 : 0;
    if (!inited) {
        inited = 1;
        // Flat-ground first pass: the ground level for the collision stubs is the player's spawn Y
        // (she's spawned standing on the terrain). Captured before the update so she doesn't fall.
        extern float gStairfaxGroundY;
        gStairfaxGroundY = *(float*)(gPlayerObj + 0x10);   // spawn localPosY
        // Zero the PlayerState (obj->extra, dllStateSize 0x8e0). loadCharacter's alloc doesn't
        // guarantee zeroed dll-state, so fields the init doesn't explicitly set (e.g. focusObject)
        // are garbage -> a non-null focusObject makes the vehicle-sync path deref junk. A freshly
        // spawned player has zeroed state.
        { unsigned char* st = *(unsigned char**)(gPlayerObj + 0xB8); if (st) memset(st, 0, 0x8e0); }
        stairfax_player_build_table();   // lay the real move/param tables into lbl_80332EC0 first
        if (tr) { fprintf(stderr, "[player-dll] init: calling objLoadPlayerFromSave obj=%p\n", (void*)gPlayerObj); fflush(stderr); }
        objLoadPlayerFromSave(gPlayerObj);
        if (tr) {
            unsigned char* o = gPlayerObj;
            fprintf(stderr, "[player-dll] init done; pos=(%.1f,%.1f,%.1f)\n",
                    *(float*)(o+0x0C), *(float*)(o+0x10), *(float*)(o+0x14)); fflush(stderr);
        }
        // Real camera: the port harness skips the object system's own camera setup (object.c:838-848),
        // so point the real engine/1 camcontrol at the player and enter the standard follow mode here.
        // Default for a spawned player (the interim port follow-cam no longer reaches the player once
        // camera.c owns Camera_GetCurrent); STAIRFAX_NO_REAL_CAM is a kill-switch. worldPos is synced
        // below on the first frame, but init/setMode only need a valid focus object + start position.
        {
            extern void stairfax_real_camera_setup(void* player);
            if (!getenv("STAIRFAX_NO_REAL_CAM")) stairfax_real_camera_setup(gPlayerObj);
        }
    }

    // Frame-rate independence: SFA is a variable-timestep engine. waitNextFrame() (runs each frame in
    // gameLoop, BEFORE this harness) already computes the real per-frame timeDelta from wall-clock
    // elapsed (60Hz quantum: timeDelta=1.0 == one 60Hz frame, clamped to 6.0), so we consume it here
    // instead of the old fixed 1.0 override - the player now runs at correct real-world speed at any
    // render rate (verified dt-invariant: steady-state speed matches across timeDelta 0.5/1.0/2.0).
    // Fallbacks: timeDelta<=0 (paused / pre-timing first frame) floors to a single 60Hz frame.
    // Diagnostics: STAIRFAX_DT_TRACE logs the incoming value; STAIRFAX_FORCE_DT pins timeDelta (the
    // dt-invariance test harness); STAIRFAX_LOCKDT restores the old fixed 1.0 (A/B).
    if (getenv("STAIRFAX_DT_TRACE")) { static int n=0; if(n++<40)
        fprintf(stderr, "[dt] incoming timeDelta=%.4f fts=%d ftsU=%d\n", timeDelta, framesThisStep, framesThisStepUnclamped); }
    { const char* fd = getenv("STAIRFAX_FORCE_DT");
      if (fd) {
        float d = (float)atof(fd); if (d <= 0.0f) d = 1.0f;
        static float rem = 0.0f; rem += d; int fts = (int)rem; rem -= (float)fts;
        timeDelta = d; framesThisStep = (unsigned char)(fts < 1 ? 1 : fts); framesThisStepUnclamped = (unsigned char)fts;
      } else if (getenv("STAIRFAX_LOCKDT") || timeDelta <= 0.0f) {
        timeDelta = 1.0f; framesThisStep = 1; framesThisStepUnclamped = 1;
      }
    }

    // No water in this scene: the real per-frame water-volume query that fills baddie.waterSurfaceY
    // (BaddieState @ PlayerState+0x1C0) isn't ported, so the zeroed field reads as "water surface at
    // Y=0". The ground-pin holds worldPosY far below 0, so waterDepth = 0 - worldPosY is large and
    // playerIsInWater() -> true -> the swim animation. Write the engine's no-water sentinel (-1e5) so
    // playerUpdateSurfaceResponse resolves waterDepth to not-submerged. Removed once real water
    // collision is wired.
    { unsigned char* st = *(unsigned char**)(gPlayerObj + 0xB8); if (st) *(float*)(st + 0x1C0) = -100000.0f; }
    // (Ground contact - baddie.groundContact/surfaceFlags - is now produced by the real engine/21
    // path-control each frame via trackGetIntersect; the harness no longer force-writes it.)

    if (tr) { static int q=0; if(q++<3){ fprintf(stderr,"[player-dll] calling playerUpdate\n"); fflush(stderr);} }
    playerUpdate(gPlayerObj);

    // Ground-pin (Phase C interim): the real vertical motion is driven by engine/15's gravity via the
    // gPlayerInterface motion call, which expects the real per-triangle ground contract that the
    // port's flat-floor collision stub (trackGetHeight/trackGetNearestGroundOffset) doesn't satisfy -
    // so the grounding loop never closes and verticalVel holds nonzero, floating her off the world.
    // Until real terrain collision is wired, close the loop from the port side: hold her at the flat
    // ground plane (spawn Y) and clear verticalVel, so she settles into a standing idle instead of
    // drifting. localPosY@0x10, worldPosY@0x1C, velocityY@0x28.
    {
        extern float gStairfaxGroundY;
        extern void  playerSetVerticalVel(void* obj, float v);
        unsigned char* o = gPlayerObj;
        *(float*)(o + 0x10) = gStairfaxGroundY;   // localPosY: pin to the ground plane
        *(float*)(o + 0x28) = 0.0f;               // velocityY: not falling/rising
        playerSetVerticalVel(gPlayerObj, 0.0f);   // clear inner->verticalVel -> exit the landing state
        // Sync worldPos from localPos: the render path draws at worldPos (0x18), but the port's
        // spawn/update never syncs it (X/Z stay 0), so she renders at the world origin ~2000u away
        // (tiny, off to the side) while the camera targets localPos. Parentless -> world = local.
        *(float*)(o + 0x18) = *(float*)(o + 0x0C);   // worldPosX = localPosX
        *(float*)(o + 0x1C) = *(float*)(o + 0x10);   // worldPosY = localPosY
        *(float*)(o + 0x20) = *(float*)(o + 0x14);   // worldPosZ = localPosZ
    }

    // Real-camera diagnostic (STAIRFAX_CAM_TRACE): the real follow cam updates in Obj_UpdateAllObjects
    // (object.c:2571) BEFORE this harness runs inside sceneRender, so this reads the current frame's
    // camera pose to confirm it tracks the player before the render is switched onto it.
    { extern void stairfax_real_camera_trace(void* player); stairfax_real_camera_trace(gPlayerObj); }

    // Direction/steering diagnostic (STAIRFAX_DIR_TRACE): does a CONSTANT stick converge to a fixed
    // heading (absolute camera-relative, correct) or spiral (camera<->player feedback = "steering
    // wheel")? Trace moveInput, inputHeading, the camera yaw the player reads, and the player's yaw.
    if (getenv("STAIRFAX_DIR_TRACE")) {
        static int d=0; if ((d++ % 15)==0) {
            unsigned char* st = *(unsigned char**)(gPlayerObj + 0xB8);
            unsigned char* vc = (unsigned char*)0; { extern void* Camera_GetCurrent(void); vc=(unsigned char*)Camera_GetCurrent(); }
            if (st) fprintf(stderr, "[dir] mIn=(%.2f,%.2f) inHead=%d camYaw@330=%d yaw@484=%d viewYaw=%d\n",
                    *(float*)(st+0x290), *(float*)(st+0x28c), *(int*)(st+0x474),
                    *(short*)(st+0x330), *(short*)(st+0x484), vc?*(short*)vc:0);
            fflush(stderr);
        }
    }

    // Interim locomotion shim (Milestone 2), now OFF by default (STAIRFAX_SHIM_MOVE to re-enable):
    // it moved localPos directly to stand in for the unported move-system speed. With engine/21
    // path-control live, the REAL velocity->position path owns movement; running this shim too makes
    // it a second uncoordinated mover that feeds the velocity feedback a runaway (animSpeedC -> inf).
    if (getenv("STAIRFAX_SHIM_MOVE")) {
        unsigned char* st = *(unsigned char**)(gPlayerObj + 0xB8);   // PlayerState (obj->extra)
        float imag = st ? *(float*)(st + 0x298) : 0.0f;
        if (st && imag > 0.05f) {
            short heading = *(short*)(st + 0x474);                   // camera-relative s16 heading
            float rad = (float)heading * (2.0f * 3.14159265f / 65536.0f);
            float speed = getenv("STAIRFAX_WALK_SPEED") ? (float)atof(getenv("STAIRFAX_WALK_SPEED")) : 6.0f;
            float step = speed * (imag > 1.0f ? 1.0f : imag);
            unsigned char* o = gPlayerObj;
            *(float*)(o + 0x0C) += sinf(rad) * step;    // localPosX
            *(float*)(o + 0x14) += -cosf(rad) * step;   // localPosZ (heading->world = (sin,-cos): flips fwd/back)
            *(short*)(o + 0x00) = (short)(-heading);   // face movement dir (mesh yaw is opposite handedness)
            *(float*)(o + 0x18) = *(float*)(o + 0x0C); // re-sync worldPos (render draws worldPos)
            *(float*)(o + 0x20) = *(float*)(o + 0x14);
        }
    }

    if (getenv("STAIRFAX_PLAYER_DLL_TRACE")) {
        static int f = 0; f++;
        unsigned char* o = gPlayerObj;
        unsigned char* inner = *(unsigned char**)(o + 0xB8);   // PlayerState (obj->extra)
        float imag = inner ? *(float*)(inner + 0x298) : 0.0f;
        // Per-frame locomotion trace while moving: controlMode@0x274 (1 idle / 2 moving),
        // animSpeedA/C@0x280/0x294 (velocity + gait drivers), currentMove@0xA0, position@0x0C/0x14.
        // Capped so it doesn't spam a long run.
        static int nMove = 0;
        if (inner && imag > 0.05f && nMove < 120) { nMove++;
            short ctrlMode = *(short*)(inner + 0x274);
            float spA = *(float*)(inner + 0x280);
            float spC = *(float*)(inner + 0x294);
            int   curMove = *(int*)(o + 0xA0);
            float locX = *(float*)(o + 0x0C), locZ = *(float*)(o + 0x14);
            float velX = *(float*)(o + 0x24), velZ = *(float*)(o + 0x2C);
            fprintf(stderr, "[mv] f=%d ctrl=%d spA=%.3f spC=%.3f imag=%.2f vel=(%.3f,%.3f) loc=(%.1f,%.1f) move=%d\n",
                    f, ctrlMode, spA, spC, imag, velX, velZ, locX, locZ, curMove);
        }
    }
}
