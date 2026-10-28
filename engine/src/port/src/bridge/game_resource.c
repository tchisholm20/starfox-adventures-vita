// game_resource.c - decomp-native resource/DLL registry for the port.
//
// On GameCube the ~705 game "DLLs" are relocatable PPC modules loaded from disc,
// relocated, and registered in gResourceDescriptors[]; Resource_Acquire(id) then
// hands back the module's interface (a vtable). We can't run PPC modules on x86,
// so decomp-native loading means compiling each DLL's SOURCE (src/dlls/**) to x86
// and registering its descriptor statically. This file is the faithful engine for
// that: it reimplements resource.h exactly (Resource_Acquire/Release/ResetRefCounts
// over gResourceDescriptors/gResourceLoadedHandles/gResourceRefCounts) and defaults
// every id to a generated no-op stub descriptor, so the game boots today. Real DLLs
// slot in one at a time via the REGISTER list below - each replaces its stub with
// the compiled `*_funcs` descriptor, and the running engine calls its real code.
//
// Descriptor layout (resource.h / object_descriptor.h are identical):
//   0x00 u32 metadata[4]      (reserved0..2, slotCountAndFlags)
//   0x10 acquire/initialise   (NULL for most engine DLLs - no disc load)
//   0x14 release
//   0x18 data[] = the interface methods (what the game calls)
// Resource_Acquire returns &gResourceLoadedHandles[id], a void** whose target is
// &descriptor->data; both `(*gXInterface)->m()` and `gXInterface->vtable->m()`
// access patterns resolve through it.

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define RESOURCE_DESCRIPTOR_COUNT 0x2c1
#define DESC_DATA_OFFSET 0x18

typedef struct RD {
    unsigned int metadata[4];
    void (*acquire)(struct RD* d);
    void (*release)(void);
    /* data (interface methods) follow at offset 0x18 */
} RD;

RD*   gResourceDescriptors[RESOURCE_DESCRIPTOR_COUNT];
void* gResourceLoadedHandles[RESOURCE_DESCRIPTOR_COUNT];
unsigned short gResourceRefCounts[RESOURCE_DESCRIPTOR_COUNT];

// One shared stub descriptor: NULL acquire/release + a wide table of no-op methods
// (wider than the largest real interface). Serves every id not yet brought up.
// Stub resource-interface method: MUST return 0, not void. gXInterface globals are reassigned at
// runtime to Resource_Acquire() results, so an un-brought-up DLL's methods dispatch HERE (not the
// game_engine_stubs vtable). A void stub leaves EAX garbage, so a game call like
// `obj = (*gCameraInterface)->getFocusTarget()` reads junk and its null-check passes on garbage ->
// deref crash. Returning 0 makes every stubbed method yield NULL/0 so the game's guards take the
// safe path. (cdecl: caller cleans args; we ignore them.)
static int resource_noop(void) { return 0; }
static struct StubDesc {
    unsigned int metadata[4];
    void (*acquire)(RD*);
    void (*release)(void);
    void* methods[64];
} gStubDesc;

// ---- real DLL registration -------------------------------------------------
// Each brought-up DLL declares its descriptor here and registers it by id. The
// symbol names match src/main/modelEngine.c's gResourceDescriptors[] table.
// (none yet - add entries as DLL sources are compiled into game_engine)
typedef struct { int id; RD* desc; } DllReg;

// Real engine DLLs compiled into game_engine and registered by id (folder number).
extern char sky_funcs[];      // src/dlls/engine/5/5.c  - SkyDllInterface (id 5)
extern char player_funcs[];   // src/dlls/engine/15/15.c - PlayerDllInterface / gPlayerInterface (id 0xf)
extern char dll_15_funcs[];   // src/dlls/engine/21/21.c - CurvesDllInterface / gPathControlInterface (id 0x15)
// Real-camera bring-up: engine/1 camcontrol (id 1) + engine/66 standard follow cam (0x42). Registered
// UNCONDITIONALLY: once camcontrol.c is compiled in, the player calls its real functions (e.g.
// camcontrol_setAButtonIconForTarget) directly, and those deref camcontrol state (gCamcontrolCamera)
// that only Camera_initialise (the id-1 acquire) sets up - so leaving id 1 unregistered null-derefs.
// Registering is safe with no follow mode set: Obj_UpdateAllObjects drives Camera_update (object.c:2571)
// which takes the focus==NULL early-out (camcontrol.c:1398) until stairfax_real_camera_setup runs
// setMode(0x42). The actual follow-cam ACTIVATION (setMode + the render switch) is gated on the player
// spawning, in game_camera_wire.c / game_scene.cpp, not here.
extern char gCamcontrolResourceDescriptor[];   // camcontrol.c - CamcontrolResourceDescriptor (id 1)
extern char gCameraModeNormalDescriptor[];      // engine/66 - locomotion follow (0x42)
extern char gCameraModeStaffAnimDescriptor[];   // engine/67 - staff-aim/action (0x43)
extern char gCameraModeViewfinderDescriptor[];  // engine/68 - first-person viewfinder (0x44)
extern char gCameraModeTalkDescriptor[];        // engine/69 - talk/look-at-NPC (0x45)
extern char gCameraModeStaticDescriptor[];      // engine/72 - static fixed-point (0x48)
extern char gCameraModeCombatDescriptor[];      // engine/73 - combat/lock-on (0x49)
extern char gCameraModeClimbDescriptor[];       // engine/75 - climb (0x4B)
extern char gCameraModeCrawlDescriptor[];       // engine/80 - crawl (0x50)
extern char gCameraModeForceBehindDescriptor[]; // engine/82 - force-behind snap (0x52)

static const DllReg* dllRegList(int* count) {
    static const DllReg regs[] = {
        { 5, (RD*)sky_funcs },
        { 0xf, (RD*)player_funcs },
        { 0x15, (RD*)dll_15_funcs },
        // Camera mode-dispatch system: camcontrol + the mode-handler DLLs the game switches to by
        // context (player.c / engine-66 call setMode(id) LIVE in the port's playerUpdate path). Each
        // registered here activates for real; any un-brought-up id still falls to the stub handler.
        { 1, (RD*)gCamcontrolResourceDescriptor },      // camera control DLL
        { 0x42, (RD*)gCameraModeNormalDescriptor },     // locomotion follow (standing + moving)
        { 0x43, (RD*)gCameraModeStaffAnimDescriptor },  // staff aim/action (L / staff)
        { 0x44, (RD*)gCameraModeViewfinderDescriptor }, // first-person viewfinder (Z)
        { 0x45, (RD*)gCameraModeTalkDescriptor },       // talk / look-at-NPC
        { 0x48, (RD*)gCameraModeStaticDescriptor },     // static fixed-point (scripts)
        { 0x49, (RD*)gCameraModeCombatDescriptor },     // combat / lock-on (near foes)
        { 0x4B, (RD*)gCameraModeClimbDescriptor },      // climb (wall / ladder)
        { 0x50, (RD*)gCameraModeCrawlDescriptor },      // crawl
        { 0x52, (RD*)gCameraModeForceBehindDescriptor },// force-behind snap (transitions)
    };
    *count = (int)(sizeof(regs) / sizeof(regs[0]));
    return regs;
}

static int gInited;
static void resource_init(void) {
    int i, n;
    const DllReg* regs;
    if (gInited) return;
    gInited = 1;

    for (i = 0; i < 4; ++i) gStubDesc.metadata[i] = 0;
    gStubDesc.acquire = NULL;
    gStubDesc.release = NULL;
    for (i = 0; i < 64; ++i) gStubDesc.methods[i] = (void*)resource_noop;

    for (i = 0; i < RESOURCE_DESCRIPTOR_COUNT; ++i) {
        gResourceDescriptors[i] = (RD*)&gStubDesc;
        gResourceLoadedHandles[i] = NULL;
        gResourceRefCounts[i] = 0;
    }

    regs = dllRegList(&n);
    for (i = 0; i < n; ++i) {
        if (regs[i].id >= 0 && regs[i].id < RESOURCE_DESCRIPTOR_COUNT)
            gResourceDescriptors[regs[i].id] = regs[i].desc;
    }

    // Minimal SaveGame/MapEvent descriptor (id 0x17): supplies getLast() -> the
    // save-state buffer that gGameBitSaveData points at. See game_savestate.c.
    {
        extern void* stairfax_mapevent_descriptor(void);
        void* me = stairfax_mapevent_descriptor();
        if (me) { gResourceDescriptors[0x17] = (RD*)me; n++; }
    }
    if (getenv("STAIRFAX_TRACE"))
        fprintf(stderr, "[resource] registry init: %d ids, %d real DLL(s)\n",
                RESOURCE_DESCRIPTOR_COUNT, n);
}

void* Resource_Acquire(unsigned short id, int unused) {
    RD* d;
    (void)unused;
    resource_init();
    if (id >= RESOURCE_DESCRIPTOR_COUNT) return &gResourceLoadedHandles[0];
    d = gResourceDescriptors[id];
    if (gResourceRefCounts[id] == 0 && d->acquire != NULL) d->acquire(d);
    gResourceRefCounts[id]++;
    gResourceLoadedHandles[id] = (char*)d + DESC_DATA_OFFSET;
    return &gResourceLoadedHandles[id];
}

int Resource_Release(void* handleSlot) {
    int i = 0;
    RD* descriptor = (RD*)handleSlot;
    resource_init();
    while (i < RESOURCE_DESCRIPTOR_COUNT) {
        if ((void*)&gResourceLoadedHandles[i] == handleSlot) {
            descriptor = gResourceDescriptors[i];
            break;
        }
        i++;
    }
    if (i >= RESOURCE_DESCRIPTOR_COUNT) return 0;
    gResourceRefCounts[i]--;
    if (gResourceRefCounts[i] == 0) {
        if (descriptor->release != NULL) descriptor->release();
        return 1;
    }
    return 0;
}

void Resource_ResetRefCounts(void) {
    int i;
    resource_init();
    for (i = 0; i < RESOURCE_DESCRIPTOR_COUNT; ++i) gResourceRefCounts[i] = 0;
}
