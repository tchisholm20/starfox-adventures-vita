// stfx_debug.c - port-side status printers that need GAME headers (built with the game units' flags
// by compile_all.sh; vt_game.c cannot include them). Called weakly from vt_game.c. 2026-09-27.
#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include "main/camera.h"
#include "main/dll/WC/dll_028A_wcearthwalker.h"
/* The game-text LOAD SLOTS (2026-09-27: the files load -- [load] lines -- but gameTextFinalizeLoad
 * never ran). State 1 = reading, 2 = read (finalise next gameTextRun), 5 = failed, 6 = done. */
#include "main/textrender_internal.h"
extern GameTextLoadSlot curGameTexts[];
void vt_print_gametext_slots(void);
void vt_print_gametext_slots(void)
{
    char b[160];
    int n = snprintf(b, sizeof b, "[textslots] sizeof %u state@%u:", (unsigned) sizeof(GameTextLoadSlot),
                     (unsigned) offsetof(GameTextLoadSlot, state));
    for (int i = 0; i < 8 && n < (int) sizeof b - 12; i++)
        n += snprintf(b + n, sizeof b - n, " %d/%d", curGameTexts[i].state, curGameTexts[i].active);
    printf("%s\n", b);
}

/* The title menu's accept gates (dlls/engine/60/60.c: accept needs gLinkInputEnabled, no hidden-HUD
 * frames, the item's NO_ACCEPT flag clear, and GAMEBIT_MenuRelated044F clear). 2026-09-27: A was
 * ignored on the title menu, runs 73-77. */
extern signed char gLinkInputEnabled, linkSelected, gLinkItemCount;
extern unsigned char gLinkNavigationEnabled;
int getHudHiddenFrameCount(void);
unsigned mainGetBit(int gameBit);
void vt_print_menu_state(void);
void vt_print_menu_state(void)
{
    printf("[menu] inputEnabled %d navEnabled %u selected %d/%d hudHidden %d bit44F %u\n",
           gLinkInputEnabled, gLinkNavigationEnabled, linkSelected, gLinkItemCount,
           getHudHiddenFrameCount(), mainGetBit(0x44F));
}

/* The THP player's counters for the Vita tool's status block (vt_game.c [thp] line; 2026-10-09). */
#include "main/dll/FRONT/attract_movie.h"
void vt_thp_words(int out[8]) {
    out[0] = gAttractMoviePlayer.state; out[1] = gAttractMoviePlayer.internalState;
    out[2] = gAttractMoviePlayer.curVideoFrameNumber; out[3] = gAttractMoviePlayer.curAudioFrameNumber;
    out[4] = gAttractMoviePlayer.videoDecodeCount; out[5] = gAttractMoviePlayer.audioExists;
    out[6] = gAttractMoviePlayer.playFlags; out[7] = (int)gAttractMoviePlayer.retraceCount;
}

/* Ground-probe instrument (docs/47 run 217): a throttled line from trackGetHeight, the one query the player's
 * floor/ground tests share (player.c, curves 21.c, 2.c sequences).
 *   ux0:data/stairfax/floor_log.txt  (read once)  '1' = the player only (logged as def 31; any other object is def 100000 + its romDefNo), '2' = every object, else off.
 * Line:  [floor] def D @(x,y,z) mode M mask 0xQ -> hits N top Y from {block|obj def K} | desc ND tris T (block B)
 *   hits 0 with tris 0   -> nothing was BUILT: no block under the probe and no hit-react object with collision
 *                           in range (block cells not loaded, or the object's model had no collisionBlocks)
 *   hits 0 with tris > 0 -> triangles were built but none lies under (x,z) facing up (a hole, or bad vertices)
 *   top from block       -> the map block's polygon groups answered;  top from obj def K -> that object's model did
 * One line per 30 probes, plus a line at once whenever hit/no-hit flips for that object. Cost when off: one load. */
void stfx_floor_log(int objDef, float x, float y, float z, int mode, int mask, int nHits, float topHeight,
                    int topObjDef, int nDesc, int triTotal, int blockTris);
void stfx_floor_log(int objDef, float x, float y, float z, int mode, int mask, int nHits, float topHeight,
                    int topObjDef, int nDesc, int triTotal, int blockTris)
{
    static int armed = -1, tick, lastHit = -1;
    if (armed < 0) {
        FILE* f = fopen("ux0:data/stairfax/floor_log.txt", "r");
        int c = f ? fgetc(f) : 0;
        if (f) fclose(f);
        armed = c == '1' ? 1 : c == '2' ? 2 : 0;
        if (armed) printf("[floor] instrument on (%s)\n", armed == 2 ? "all objects" : "player only");
    }
    if (!armed || (armed == 1 && objDef != 31)) return;
    int hit = nHits != 0;
    int flip = hit != lastHit;
    if (armed == 1) lastHit = hit;
    if (!flip && (++tick % 30)) return;
    printf("[floor] def %d @(%.1f,%.1f,%.1f) mode %d mask 0x%x -> hits %d", objDef, x, y, z, mode, mask, nHits);
    if (hit) {
        if (topObjDef >= 0) printf(" top %.1f from obj def %d", topHeight, topObjDef);
        else printf(" top %.1f from block", topHeight);
    }
    printf(" | desc %d tris %d (block %d)%s\n", nDesc, triTotal, blockTris, flip ? " FLIP" : "");
}

/* Climb instrument (docs/47 run 218: "got stuck climbing something invisible that locked me").
 *   ux0:data/stairfax/climb_log.txt  (read once)  '1' = on, else off.   Cost when off: one load per call.
 * It answers two questions the player cannot: WHAT started the climb, and WHAT would end it.
 *   [climb] probe  - every collision-line probe hit that playerCheckIfClimbingOntoWall makes is remembered (not
 *                    printed): the last one before a state change is the trigger.
 *   [climb] ENTER  - the player's control mode went from A to B and B is a climb/ledge/hang state: the remembered
 *                    probe (which probe slot, line surfaceType/kind/flags/param, endpoints, the line's top Y, the
 *                    object it belongs to or `map`), the player's position, and the seeded climb numbers.
 *   [climb] LEAVE  - the climb state ended, and into which mode, after how many frames.
 *   [climb] HOLD   - while in a climb state, every 20th frame: the exit condition's inputs --
 *        ladder (mode 15): climbStep / climbStepCount (the top transition needs step >= count-3), A pressed with
 *                          step > 3 jumps off, stick down below step 1 dismounts;
 *        wall (mode 19):   spanTopY / spanBottomY against savedPosY (top and bottom exits fire only within a few
 *                          units of an end, and only when the line's flags permit: dir 0/3 top, 1/3 bottom);
 *        travel (mode 27): traveledDistance vs travelTargetDistance (a rom-curve walk; 0 target = no curve found);
 *        every mode:       current move, move progress, moveDone, stick, buttons, position.
 * Modes: 10 grab-ledge 11 mantle 12 climb-ledge 13 hold 14 onto-ladder 15 on-ladder 16 slide-down-ladder
 *        17 state11 18 onto-wall 19 on-wall 20 up-from-wall 21 down-from-wall 27 curve-travel 28 bump. */
#include "main/dll/player_state.h"
#include "main/track_line_intersect_result.h"
#include "sys/objects.h"
#include <stdarg.h>

static int stfxClimbArmed(void) {
    static int armed = -1;
    if (armed < 0) {
        FILE* f = fopen("ux0:data/stairfax/climb_log.txt", "r");
        int c = f ? fgetc(f) : 0;
        if (f) fclose(f);
        armed = c == '1';
        if (armed) printf("[climb] instrument on\n");
    }
    return armed;
}
static struct { int valid, slot, mask; unsigned frame; float hd, start[3], end[3]; TrackLineIntersectResult hit; } sClimbProbe;
static unsigned sClimbFrame;
static int stfxIsClimbMode(int m) { return (m >= 10 && m <= 21) || m == 27 || m == 28; }

void stfx_climb_probe(int slot, const TrackLineIntersectResult* hit, float hd, unsigned mask, const float* start, const float* end);
void stfx_climb_probe(int slot, const TrackLineIntersectResult* hit, float hd, unsigned mask, const float* start, const float* end)
{
    if (!stfxClimbArmed()) return;
    sClimbProbe.valid = 1; sClimbProbe.slot = slot; sClimbProbe.mask = (int)mask; sClimbProbe.frame = sClimbFrame;
    sClimbProbe.hd = hd; sClimbProbe.hit = *hit;
    for (int k = 0; k < 3; k++) { sClimbProbe.start[k] = start[k]; sClimbProbe.end[k] = end[k]; }
}

/* Player-state instrument (docs/47, 2026-10-11: "rolled and got stuck" with the climb instrument silent -- the lock was
 * not in a climb mode, and the climb log cannot see any other).
 *   ux0:data/stairfax/player_log.txt  (read once)  '1' = on, else off.   Cost when off: one load per frame.
 * Player object only; driven from playerRunStateMachine through stfx_climb_watch (the same hook, no new game edits).
 *   [pstate] ByteFlags self-check ...   once: what this build's flags3F0.b04 / .b80 really are in the byte (0x04 / 0x80 expected)
 *   [pstate] present/frame N: mode A -> B | pos | vel | move id progress moveDone | stick | held/pressed | flags
 *            every controlMode change (two changes inside ONE frame collapse into the last one).
 *   [pstate] FLAGS 3F0 a->b 3F1 c->d 3F2 ...   a sub-state flag byte changed. Modes 1 (idle) and 2 (moving) hold sub-states in
 *            flagByte3F0 that are NOT controlModes: 80 skid/quick-turn  40 u-turn move  20 swimming  10 staff swing
 *            08 leaping (wall hop)  04 airborne/landing  02 guard  01 landed.   A lock with controlMode 2 and one of these
 *            set for seconds is the suspect; the HOLD line says what its exit waits for.
 *   [pstate] HOLD mode A for N frames | ...  once the mode has been held > 90 frames, every 60 frames. Generic fields for every
 *            mode (move, progress and its delta since the last HOLD line, moveSpeed, moveDone, velocity, groundContact,
 *            surfaceFlags, timers, anim speeds), plus per-mode:
 *              1/2     the flag bytes decoded, gait, input magnitude, yaw-rate, fall frames/severity/threshold Y vs Y, water depth
 *              38 (attack/roll/dodge: moveSlotIndex 0-4 = A+direction, 5-9 = X) the move slot's table index, mapped move id, exit
 *                      progress thresholds (+0x28 chain, +0x2c early exit) and moveChainFlags
 *              others  the generic line only. */
extern float timeDelta;
static int stfxPstateArmed(void) {
    static int armed = -1;
    if (armed < 0) {
        FILE* f = fopen("ux0:data/stairfax/player_log.txt", "r");
        int c = f ? fgetc(f) : 0;
        if (f) fclose(f);
        armed = c == '1';
        if (armed) {
            ByteFlags bf;
            *(unsigned char*)&bf = 0; bf.b04 = 1; unsigned char a = *(unsigned char*)&bf;
            *(unsigned char*)&bf = 0; bf.b80 = 1; unsigned char b = *(unsigned char*)&bf;
            printf("[pstate] instrument on | ByteFlags self-check: .b04 -> byte 0x%02x (expect 0x04), .b80 -> 0x%02x (expect 0x80) %s\n",
                   a, b, (a == 0x04 && b == 0x80) ? "OK" : "WRONG: named flags and byte-mask tests disagree");
        }
    }
    return armed;
}

static void stfxPstateLine(const char* tag, GameObject* obj, BaddieState* st, unsigned frame, int mode, int prevMode, unsigned held)
{
    PlayerState* ps = (PlayerState*)obj->extra;
    static float lastProgress = 0.0f;
    float prog = obj->anim.currentMoveProgress;
    if (held) printf("[pstate] HOLD mode %d for %u frames (present/frame %u)", mode, held, frame);
    else printf("[pstate] present/frame %u: mode %d -> %d", frame, prevMode, mode);
    (void)tag;
    printf(" | pos (%.1f,%.1f,%.1f) vel (%.3f,%.3f,%.3f) | move %d progress %.3f (d %+.3f) speed %.4f moveDone %d | stick (%.2f,%.2f) mag %.2f | "
           "held 0x%x pressed 0x%x | ground %d surf 0x%x | ctrlT %d stateT %d dt %.2f | animSpd A %.3f B %.3f C %.3f\n",
           obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ, obj->anim.velocityX, obj->anim.velocityY, obj->anim.velocityZ,
           obj->anim.currentMove, prog, prog - lastProgress, st->moveSpeed, st->moveDone, st->moveInputX, st->moveInputZ, st->inputMagnitude,
           (unsigned)st->heldButtons, (unsigned)st->pressedButtons, (int)st->groundContact, (unsigned)(unsigned char)st->surfaceFlags,
           (int)st->controlTimer, (int)st->stateTimer, timeDelta, st->animSpeedA, st->animSpeedB, st->animSpeedC);
    lastProgress = prog;
    if (mode == 1 || mode == 2) {
        printf("[pstate]   flags 3F0 0x%02x 3F1 0x%02x 3F2 0x%02x 3F3 0x%02x 3F4 0x%02x | gait %d curSpeed %.3f maxSpeed %.3f yawRate %d | fall frames %d sev %d "
               "thresholdY %.1f worldY %.1f | water depth %.1f | heldObj %s moveAnimIds %s\n",
               ps->flagByte3F0, ps->flagByte3F1, *(unsigned char*)&ps->flags3F2, *(unsigned char*)&ps->flags3F3, *(unsigned char*)&ps->flags3F4,
               (int)(signed char)ps->gaitLevel, ps->currentSpeed, ps->maxSpeed, (int)ps->yawRateSigned, (int)ps->fallFrames, (int)ps->fallSeverity,
               ps->fallThresholdY, obj->anim.worldPosY, ps->waterDepth, ps->heldObj ? "yes" : "no", ps->moveAnimIds ? "set" : "NULL");
        printf("[pstate]   yaw: rotX %d yaw %d target %d inputH %d lastInputH %d baddie.cameraYaw %d cam.yaw %d flags360 0x%x\n", (int)obj->anim.rotX, (int)ps->yaw,
               (int)ps->targetYaw, ps->inputHeading, ps->lastInputHeading, (int)st->cameraYaw, Camera_GetCurrent() ? (int)Camera_GetCurrent()->yaw : 0, (unsigned)ps->flags360);
    }
    if (mode == 38 || mode == 39) {
        const unsigned char* slot = (const unsigned char*)(unsigned long)ps->moveSlots + (unsigned)ps->moveSlotIndex * 0xb0;
        if (ps->moveSlots)
            printf("[pstate]   attack slot %d | tableIdx %d | chain %.2f earlyExit %.2f slotSpeed %.4f | chainFlags 0x%x target %s\n",
                   (int)ps->moveSlotIndex, (int)*(const short*)(slot + 2), *(const float*)(slot + 0x28), *(const float*)(slot + 0x2c),
                   *(const float*)(slot + 0x1c), (unsigned)st->moveChainFlags, st->targetObj ? "yes" : "no");
    }
}

/* Discontinuity instrument (docs/47, run 226: "the wounded EarthWalker teleports me away" / "Krystal keeps facing the camera").
 * Same switch (player_log.txt).  Per player frame the watch compares position and yaw with the previous frame:
 *   [pstate] JUMP frame N mode M: pos (a)->(b) d D | rotX a -> b (+-deg) | yaw/target/inputH/lastInputH/cameraYaw | stick | seq | flags360 | notes
 *   [pstate] YAW  ...same line, printed when only the yaw moved by more than 45 degrees in one frame.
 * `notes` are the position/yaw SETTERS that ran since the previous frame, each as tag@0xADDRESS (the caller's return
 * address -- symbolize it with the build's map/nm):  playerTeleport, objSetXRot, and PUSH (ObjHits_ApplyPairResponse
 * moving the player out of another object's hit volume: the other object's def number and the penetration vector).
 *   [pstate] SET  frame N: tag by 0xADDR  ...  immediate line for playerTeleport / objSetXRot.
 *   [pstate] PUSH frame N: other def D cls C vec (x,y,z) |v| by 0xADDR   rate-limited to one per 10 frames unless |v| > 20.
 *   [pstate] EW   frame N: def D enc E istate S flags F lastTrig T dist R (xz) | anim move M interact 0xI | player pos
 *                 every 30 frames while the player is within 450 units of a DLL-650 EarthWalker, and at once whenever
 *                 its interactionState / flags / interact-flag byte changes.  interact bit 0x1 in-range (the camera
 *                 targeted it: the talk prompt belongs here), 0x2 activated by A.  No EW line at all = never nearby. */
extern int getCurSeqNo(void);
typedef struct { const char* tag; void* ra; float v[3]; } PNote;
static PNote sPNotes[8];
static int sPNoteN;
static unsigned sPFrame;

static void stfxPnoteAdd(const char* tag, void* ra, float x, float y, float z)
{
    if (sPNoteN < 8) { PNote* n = &sPNotes[sPNoteN++]; n->tag = tag; n->ra = ra; n->v[0] = x; n->v[1] = y; n->v[2] = z; }
}

void stfx_pstate_set(GameObject* o, const char* tag, void* ra, float x, float y, float z);
void stfx_pstate_set(GameObject* o, const char* tag, void* ra, float x, float y, float z)
{
    if (!stfxPstateArmed() || o != Obj_GetPlayerObject()) return;
    stfxPnoteAdd(tag, ra, x, y, z);
    printf("[pstate] SET frame %u: %s by %p -> (%.1f,%.1f,%.1f) | from pos (%.1f,%.1f,%.1f) rotX %d\n", sPFrame, tag, ra, x, y, z,
           o->anim.localPosX, o->anim.localPosY, o->anim.localPosZ, (int)o->anim.rotX);
}

void stfx_pstate_push(GameObject* a, GameObject* b, float x, float y, float z, void* ra);
void stfx_pstate_push(GameObject* a, GameObject* b, float x, float y, float z, void* ra)
{
    static unsigned lastPrint;
    GameObject* pl;
    GameObject* other;
    float mag;
    if (!stfxPstateArmed()) return;
    pl = Obj_GetPlayerObject();
    if (pl == NULL || (a != pl && b != pl)) return;
    other = a == pl ? b : a;
    mag = sqrtf(x * x + y * y + z * z);
    stfxPnoteAdd(other->anim.romDefNo == 0 ? "PUSH(def0)" : "PUSH", ra, x, y, z);
    if (mag > 20.0f || sPFrame - lastPrint >= 10) {
        lastPrint = sPFrame;
        printf("[pstate] PUSH frame %u: other def %d cls %d vec (%.2f,%.2f,%.2f) |v| %.2f by %p | player pos (%.1f,%.1f,%.1f) other pos (%.1f,%.1f,%.1f)\n",
               sPFrame, (int)other->anim.romDefNo, (int)other->anim.classId, x, y, z, mag, ra, pl->anim.localPosX, pl->anim.localPosY,
               pl->anim.localPosZ, other->anim.worldPosX, other->anim.worldPosY, other->anim.worldPosZ);
    }
}

void stfx_ew_watch(GameObject* obj, void* st);
void stfx_ew_watch(GameObject* obj, void* st)
{
    static unsigned lastPrint;
    static unsigned lastSig = 0xffffffffu;
    EarthWalkerState* s = (EarthWalkerState*)st;
    GameObject* pl;
    float dx, dz, dist;
    unsigned sig;
    if (!stfxPstateArmed()) return;
    pl = Obj_GetPlayerObject();
    if (pl == NULL) return;
    dx = pl->anim.worldPosX - obj->anim.worldPosX; dz = pl->anim.worldPosZ - obj->anim.worldPosZ;
    dist = sqrtf(dx * dx + dz * dz);
    if (dist > 450.0f) return;
    sig = (unsigned)s->interactionState | ((unsigned)s->flags << 8) | ((unsigned)obj->anim.resetHitboxFlags << 16);
    if (sig == lastSig && sPFrame - lastPrint < 30) return;
    lastSig = sig; lastPrint = sPFrame;
    printf("[pstate] EW frame %u: def %d enc %d istate %d flags 0x%x lastTrig %d dist %.1f (xz) | anim move %d interact 0x%x | ew pos (%.1f,%.1f,%.1f) player (%.1f,%.1f,%.1f)\n",
           sPFrame, (int)obj->anim.romDefNo, (int)s->encounterType, (int)s->interactionState, (unsigned)s->flags, (int)s->lastTriggeredState,
           dist, (int)obj->anim.currentMove, (unsigned)obj->anim.resetHitboxFlags, obj->anim.worldPosX, obj->anim.worldPosY,
           obj->anim.worldPosZ, pl->anim.worldPosX, pl->anim.worldPosY, pl->anim.worldPosZ);
    /* camcontrol_findBestTarget's tests, one by one, for this object (camcontrol.c:428-510):  alpha==0xff, no 0x?? DISABLED/0x20
     * in interact, RENDERED|HAS_MODELS, not HIDDEN, class mask has bit kind, dy in (-100,20), xz distance to the hit volume's
     * CENTRE < bounds[2]*4 -- and then what the camera actually picked. */
    {
        extern unsigned short gCamcontrolTargetClassMask;
        extern int playerCanUseCombatTargeting(GameObject*);
        extern GameObject* Camera_getTarget(void);
        int hv = obj->hitVolumeIndex;
        if (obj->anim.hitVolumeBounds && obj->anim.hitVolumeTransforms) {
            float cx = obj->anim.hitVolumeTransforms[hv].centerX, cy = obj->anim.hitVolumeTransforms[hv].centerY,
                  cz = obj->anim.hitVolumeTransforms[hv].centerZ;
            float ddx = pl->anim.worldPosX - cx, ddz = pl->anim.worldPosZ - cz;
            int kind = obj->anim.hitVolumeBounds[hv].flags & 0x0f;
            printf("[pstate] EWT frame %u: alpha %d interact 0x%x objFlags 0x%x animFlags 0x%x defFlags 0x%x | hv %d kind %d bounds %d,%d,%d,%d range %d | "
                   "centre (%.1f,%.1f,%.1f) dy %.1f dxz %.1f | classMask 0x%x(bit %s) canTarget %d | camera target %s\n",
                   sPFrame, (int)obj->anim.alpha, (unsigned)obj->anim.resetHitboxFlags, (unsigned)obj->objectFlags, (unsigned)obj->anim.flags,
                   (unsigned)obj->anim.modelInstance->flags, hv, kind, (int)obj->anim.hitVolumeBounds[hv].bounds[0],
                   (int)obj->anim.hitVolumeBounds[hv].bounds[1], (int)obj->anim.hitVolumeBounds[hv].bounds[2],
                   (int)obj->anim.hitVolumeBounds[hv].bounds[3], (int)obj->anim.hitVolumeBounds[hv].bounds[2] << 2, cx, cy, cz,
                   pl->anim.worldPosY - cy, sqrtf(ddx * ddx + ddz * ddz), (unsigned)gCamcontrolTargetClassMask,
                   (gCamcontrolTargetClassMask >> kind) & 1 ? "set" : "CLEAR", playerCanUseCombatTargeting(pl),
                   Camera_getTarget() == obj ? "THIS DINO" : (Camera_getTarget() ? "another object" : "none"));
        } else printf("[pstate] EWT frame %u: no hit-volume bounds/transforms on this object\n", sPFrame);
    }
}

static void stfxPstateDiscontinuity(GameObject* obj, BaddieState* st, int mode)
{
    static float lp[3];
    static short lrot;
    static int have;
    PlayerState* ps = (PlayerState*)obj->extra;
    float x = obj->anim.localPosX, y = obj->anim.localPosY, z = obj->anim.localPosZ;
    short rot = obj->anim.rotX;
    if (have) {
        float d = sqrtf((x - lp[0]) * (x - lp[0]) + (y - lp[1]) * (y - lp[1]) + (z - lp[2]) * (z - lp[2]));
        int dyaw = (short)(rot - lrot);
        int big = d > 30.0f;
        if (big || dyaw > 8192 || dyaw < -8192) {
            Camera* cam = Camera_GetCurrent();
            int i;
            printf("[pstate] %s frame %u mode %d: pos (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) d %.1f | rotX %d -> %d (%+.1f deg) | yaw %d target %d inputH %d lastInputH %d "
                   "baddie.cameraYaw %d cam.yaw %d | stick (%.0f,%.0f) mag %.2f | seq %d | flags360 0x%x | cameraTargetObject %s targetObj %s | notes %d:",
                   big ? "JUMP" : "YAW ", sPFrame, mode, lp[0], lp[1], lp[2], x, y, z, d, (int)lrot, (int)rot, dyaw * 360.0f / 65536.0f, (int)ps->yaw,
                   (int)ps->targetYaw, ps->inputHeading, ps->lastInputHeading, (int)st->cameraYaw, cam ? (int)cam->yaw : 0, st->moveInputX,
                   st->moveInputZ, st->inputMagnitude, getCurSeqNo(), (unsigned)ps->flags360, ps->cameraTargetObject ? "yes" : "no",
                   st->targetObj ? "yes" : "no", sPNoteN);
            for (i = 0; i < sPNoteN; i++) printf(" %s@%p(%.1f,%.1f,%.1f)", sPNotes[i].tag, sPNotes[i].ra, sPNotes[i].v[0], sPNotes[i].v[1], sPNotes[i].v[2]);
            printf("\n");
        }
    }
    sPNoteN = 0;
    have = 1; lp[0] = x; lp[1] = y; lp[2] = z; lrot = rot;
}

void stfx_pstate_watch(GameObject* obj, BaddieState* st, int prevMode);
void stfx_pstate_watch(GameObject* obj, BaddieState* st, int prevMode)
{
    static unsigned modeSince;
    static int curMode = -999;
    static unsigned lastHold;
    static unsigned char last3F0 = 0xff, last3F1 = 0xff, last3F2 = 0xff;
    if (!stfxPstateArmed() || obj != Obj_GetPlayerObject()) return;
    PlayerState* ps = (PlayerState*)obj->extra;
    int mode = st->controlMode;
    unsigned frame = ++sPFrame;
    stfxPstateDiscontinuity(obj, st, mode);
    if (mode != curMode) {
        stfxPstateLine("CHANGE", obj, st, frame, mode, curMode == -999 ? prevMode : curMode, 0);
        curMode = mode; modeSince = frame; lastHold = 0;
    }
    {
        unsigned char a = ps->flagByte3F0, b = ps->flagByte3F1, c = *(unsigned char*)&ps->flags3F2;
        if (a != last3F0 || b != last3F1 || c != last3F2) {
            printf("[pstate] FLAGS frame %u mode %d: 3F0 %02x->%02x 3F1 %02x->%02x 3F2 %02x->%02x\n", frame, mode,
                   last3F0 == 0xff ? 0 : last3F0, a, last3F1 == 0xff ? 0 : last3F1, b, last3F2 == 0xff ? 0 : last3F2, c);
            last3F0 = a; last3F1 = b; last3F2 = c;
        }
    }
    {
        unsigned held = frame - modeSince;
        if (held > 90 && held - lastHold >= 60) {
            lastHold = held;
            stfxPstateLine("HOLD", obj, st, frame, mode, mode, held);
        }
    }
}

void stfx_climb_watch(GameObject* obj, BaddieState* st, int prevMode);
void stfx_climb_watch(GameObject* obj, BaddieState* st, int prevMode)
{
    static int enterFrame = -1, inMode = -1;
    stfx_pstate_watch(obj, st, prevMode);
    if (!stfxClimbArmed() || obj != Obj_GetPlayerObject()) return;
    sClimbFrame++;
    PlayerState* ps = (PlayerState*)obj->extra;
    int mode = st->controlMode;
    float x = obj->anim.localPosX, y = obj->anim.localPosY, z = obj->anim.localPosZ;
    if (mode != prevMode) {
        if (stfxIsClimbMode(prevMode) && !stfxIsClimbMode(mode))
            printf("[climb] LEAVE mode %d -> %d after %u frames @(%.1f,%.1f,%.1f) move %d\n", prevMode, mode,
                   sClimbFrame - (unsigned)enterFrame, x, y, z, obj->anim.currentMove);
        if (stfxIsClimbMode(mode) && !stfxIsClimbMode(prevMode)) {
            enterFrame = (int)sClimbFrame; inMode = mode;
            printf("[climb] ENTER mode %d (from %d) @(%.1f,%.1f,%.1f) parent %s\n", mode, prevMode, x, y, z,
                   obj->anim.parent ? "yes" : "no");
            if (sClimbProbe.valid && sClimbFrame - sClimbProbe.frame < 3) {
                const TrackLineIntersectResult* h = &sClimbProbe.hit;
                printf("[climb]   trigger: probe slot %d (mask 0x%x) hit dist %.1f | line surfaceType %d kind(param) %d flags 0x%02x "
                       "adj %d,%d | (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) top y %.1f / %.1f (height %.1f/%.1f) | from %s%d\n",
                       sClimbProbe.slot, (unsigned)sClimbProbe.mask, sClimbProbe.hd, h->surfaceType, h->kind, h->flags,
                       h->adjacentLine0, h->adjacentLine1, h->lineStartX, h->lineStartY, h->lineStartZ,
                       h->lineEndX, h->lineEndY, h->lineEndZ, h->upperY0, h->upperY1,
                       h->upperY0 - h->lineStartY, h->upperY1 - h->lineEndY,
                       h->object ? "object def " : "map", h->object ? (int)h->object->anim.romDefNo : 0);
            } else printf("[climb]   trigger: no probe hit within 3 frames (entered by a non-probe path)\n");
            printf("[climb]   seeded: climbStep %d count %d stepH %.2f baseY %.1f endLocalY %.1f targetY %.1f | span %.1f..%.1f savedY %.1f\n",
                   ps->climbStep, ps->climbStepCount, ps->climbStepHeight, ps->climbBaseY, ps->climbEndLocalY,
                   ps->climbTargetY, ps->spanBottomY, ps->spanTopY, ps->savedPosY);
        } else if (stfxIsClimbMode(mode)) {
            printf("[climb] mode %d -> %d @(%.1f,%.1f,%.1f)\n", prevMode, mode, x, y, z);
            inMode = mode;
        }
    }
    if (stfxIsClimbMode(mode) && enterFrame >= 0 && (sClimbFrame % 20) == 0) {
        printf("[climb] HOLD mode %d f%u @(%.1f,%.1f,%.1f) move %d progress %.2f moveDone %d stick(%.0f,%.0f) buttons 0x%x",
               mode, sClimbFrame - (unsigned)enterFrame, x, y, z, obj->anim.currentMove, obj->anim.currentMoveProgress,
               st->moveDone, st->moveInputX, st->moveInputZ, (unsigned)st->pressedButtons);
        if (mode == 15 || mode == 14 || mode == 16)
            printf(" | step %d/%d (top at >= %d) stepH %.2f targetY %.1f", ps->climbStep, ps->climbStepCount, ps->climbStepCount - 3,
                   ps->climbStepHeight, ps->climbTargetY);
        if (mode >= 18 && mode <= 21)
            printf(" | span %.1f..%.1f savedY %.1f (to top %.1f, to bottom %.1f)", ps->spanBottomY, ps->spanTopY, ps->savedPosY,
                   ps->spanTopY - ps->savedPosY, ps->savedPosY - ps->spanBottomY);
        if (mode == 27)
            printf(" | traveled %.1f / target %.1f curve (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)", ps->traveledDistance,
                   ps->travelTargetDistance, ps->curveStartX, ps->curveStartY, ps->curveStartZ, ps->curveEndX,
                   ps->curveEndY, ps->curveEndZ);
        if (mode == 11 || mode == 12)
            printf(" | leapTargetY %.1f heightOff %.1f", ps->leapTargetY, ps->characterHeightOffset);
        printf("\n");
    }
    (void)inMode;
}
