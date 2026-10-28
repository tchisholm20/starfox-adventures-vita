// game_savestate.c - brings up gamebits / save-state for the port.
//
// The gamebit read/write logic (mainGetBit/mainSetBits in gameloop_main.c) is already
// real and compiled; it was inert only because its DATA wasn't provided:
//   - gGameBitTable  : loaded by init() via loadAssetFileById(&gGameBitTable, BITTABLE)
//                      -> stubbed, so the table was empty and gGameBitCount stayed 0.
//   - gGameBitSaveData: set by init() from (*gMapEventInterface)->getLast() -> the stub
//                      vtable returned 0, so any real bit access would deref NULL.
// This file supplies both, decomp-native where it can:
//   1) loadAssetFileById/getDataFileSize load the real BITTABLE.bin from the ISO
//      (the only loadAssetFileById call in the compiled engine), so gGameBitTable and
//      gGameBitCount become real.
//   2) A minimal MapEventInterface descriptor (registered at resource id 0x17, the
//      SaveGame DLL) whose getLast() returns a zeroed SaveGameData-sized buffer - a
//      blank new-game save. mainGetBit/mainSetBits then read/write real story bits.
// The full SaveGame DLL (engine/23) isn't brought up; this is the save-state buffer +
// the one accessor the boot path needs. See [[port-resource-dll]], [[port-engine-boot]].

#include "main/mapEventTypes.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SAVEGAMEDATA_SIZE 0xF70   // sizeof(SaveGameData), engine/23/23.c

// The MLDF data-file loader (loadAssetFileById/getDataFileSize incl. BITTABLE.bin) now
// lives in game_assetfile.c; gamebits just consumes it.
extern int getDataFileSize(int idx);

// --- save-state buffer (blank new game = zeros) -----------------------------
static uint8_t gSaveData[0x1000];          // >= SAVEGAMEDATA_SIZE, zero-initialised
static uint8_t gCharStateBuf[0x40];
static uint8_t gTrickyStatsBuf[0x20];

// --- minimal MapEventInterface (SaveGame DLL, id 0x17) ----------------------
// Descriptor layout matches resource.h: [0x10 metadata][acquire][release][data@0x18].
// The game reads (*gMapEventInterface) = &data, which is a MapEventInterface, so we
// place a real MapEventInterface at offset 0x18 and fill every slot with a safe stub,
// overriding the ones the boot/loop path actually needs.
typedef struct MeDesc {
    uint32_t         metadata[4];
    void           (*acquire)(void* d);
    void           (*release)(void);
    MapEventInterface iface;
} MeDesc;

static MeDesc gMeDesc;
static int    gMeBuilt;

static void   me_noop(void)              { }
static uint8_t* me_getLast(void)         { return gSaveData; }
static float  me_getTime(int id)         { (void)id; return 0.0f; }
static void*  me_getCurCharState(void)   { return gCharStateBuf; }
static uint8_t* me_getCurCharPos(void)   { return gSaveData; }
static void*  me_getTrickyStats(void)    { return gTrickyStatsBuf; }
// getCurChar returns which of the player's shared model banks is active - objLoadPlayerFromSave
// feeds it straight into Obj_SetActiveModelIndex. playerSetDisguised confirms the layout: the
// undisguised character is index 1, the SharpClaw disguise is index 2. Left as the me_noop stub it
// returned a garbage register value (2 -> the SharpClaw model). Default to 1 (undisguised); allow an
// env override so the real bank for a given character can be dialed in by observation.
static uint8_t me_getCurChar(void) {
    const char* e = getenv("STAIRFAX_PLAYER_CHARID");
    return (uint8_t)(e ? atoi(e) : 1);
}

void* stairfax_mapevent_descriptor(void) {
    if (!gMeBuilt) {
        gMeBuilt = 1;
        memset(&gMeDesc, 0, sizeof gMeDesc);
        // acquire/release NULL -> Resource_Acquire hands back the methods directly.
        // Fill every interface slot with a no-op, then override the real ones.
        void** slots = (void**)&gMeDesc.iface;
        int n = (int)(sizeof(MapEventInterface) / sizeof(void*));
        for (int i = 0; i < n; ++i) slots[i] = (void*)me_noop;
        gMeDesc.iface.getLast              = me_getLast;
        gMeDesc.iface.getTime              = me_getTime;
        gMeDesc.iface.getCurCharacterState = me_getCurCharState;
        gMeDesc.iface.getCurCharPos        = me_getCurCharPos;
        gMeDesc.iface.getCurChar           = me_getCurChar;
        gMeDesc.iface.getTrickyStats       = (TrickyStats*(*)(void))me_getTrickyStats;
    }
    return &gMeDesc;
}

// --- one-shot runtime self-test (STAIRFAX_GAMEBIT_TEST) ---------------------
extern short gGameBitCount;              // s16, gameloop_main.c
extern unsigned char* gGameBitSaveData;  // set by init() from getLast()
extern unsigned int mainGetBit(int gameBit);
extern void mainSetBits(int gameBit, int value);

extern void* gGameBitTable;

void stairfax_gamebit_selftest(void) {
    static int done;
    if (done || !getenv("STAIRFAX_GAMEBIT_TEST")) return;
    done = 1;
    int wired = (gGameBitSaveData == gSaveData);
    fprintf(stderr, "[savestate] BITTABLE %d bytes, gGameBitCount=%d, saveData wired=%d\n",
            getDataFileSize(0x33), (int)gGameBitCount, wired);
    if (gGameBitCount <= 0 || !gGameBitTable) { fprintf(stderr, "[savestate] SKIP (no table)\n"); return; }
    int id = 100;
    unsigned before = mainGetBit(id);
    mainSetBits(id, 1); unsigned v1 = mainGetBit(id);
    mainSetBits(id, 0); unsigned v0 = mainGetBit(id);
    mainSetBits(id, before);
    fprintf(stderr, "[savestate] gamebit round-trip id=%d: set1->%u set0->%u : %s\n",
            id, v1, v0, (v1 == 1 && v0 == 0) ? "PASS" : "FAIL");
}
