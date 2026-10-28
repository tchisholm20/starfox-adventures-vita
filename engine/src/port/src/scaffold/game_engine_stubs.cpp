// game_engine_stubs.cpp - Phase 3 bring-up stubs for gameloop_main.c (init/gameLoop).
//
// init()/gameLoop() reference ~169 engine symbols not yet ported. Stand-ins to get the
// real game loop LINKING and RUNNING natively: functions no-op (0, or 1 for init() loop
// gates); Resource_Acquire returns the shared no-op interface (so `gX = Resource_Acquire()`
// leaves a valid vtable); DLL interface pointers point at that no-op vtable; POINTER
// globals point at real zeroed storage; value/array globals get storage. Replaced by real
// game TUs as subsystems come up. STAIRFAX_TRACE=1 logs stub calls.

#include "dolphin/gx/GXStruct.h"
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" GXRenderModeObj GXNtsc480IntDf = {};
static int gTrace = 0;
// Stub interface method: MUST return 0 (not void). A void stub leaves EAX undefined, so a game
// call like `obj = (*gCameraInterface)->getFocusTarget()` reads garbage and then null-checks pass
// on junk -> deref crash. Returning 0 makes every stubbed interface method yield NULL/0, so the
// game's own `if (x != NULL)` guards take the safe path. (cdecl: caller cleans args, we ignore them.)
static int gEngineNoop(void) { return 0; }
extern "C" { void* gEngineStubVtbl[256]; }
static void* gEngineStubVtblPtr = gEngineStubVtbl;  // one "interface" = &this
// Crash diagnostics: print the exception code + faulting address (module-relative) plus a
// module-relative backtrace so a silent access violation OR stack overflow in recompiled game
// code can be located via the .map/objdump. A vectored handler (with a reserved stack, below)
// catches stack overflow too, whose module+offset frames repeat = the recursion cycle.
#if defined(_WIN32)
static void stairfaxPrintCrash(EXCEPTION_POINTERS* ep) {
    void* pc = (void*)ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = GetModuleHandleW(NULL);
    fprintf(stderr, "\n[CRASH] code=0x%08lX at %p (module base %p, +0x%tX)\n",
            ep->ExceptionRecord->ExceptionCode, pc, (void*)mod,
            (char*)pc - (char*)mod);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "[CRASH] access %s addr %p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    void* frames[48];
    USHORT n = CaptureStackBackTrace(0, 48, frames, NULL);
    for (USHORT i = 0; i < n; ++i)
        fprintf(stderr, "[CRASH]   #%02u +0x%tX\n", i, (char*)frames[i] - (char*)mod);
    fflush(stderr);
}
static LONG WINAPI stairfaxCrashFilter(EXCEPTION_POINTERS* ep) {
    stairfaxPrintCrash(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
// Vectored handler: catches stack overflow (which the unhandled-exception filter cannot, having
// no stack) once SetThreadStackGuarantee reserves slack. Only acts on fatal codes, else passes.
static LONG WINAPI stairfaxVectoredHandler(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW) { stairfaxPrintCrash(ep); TerminateProcess(GetCurrentProcess(), 3); }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
struct EngineVtblInit {
    EngineVtblInit(){
        setbuf(stdout, 0); setbuf(stderr, 0);
#if defined(_WIN32)
        SetUnhandledExceptionFilter(stairfaxCrashFilter);
        { ULONG g = 65536; SetThreadStackGuarantee(&g); }   // slack so the SO handler can run
        AddVectoredExceptionHandler(0, stairfaxVectoredHandler);
#endif   /* Vita: crashes go to the OS crash dump (tools/sfa2/crash_report.py) */
        gTrace = getenv("STAIRFAX_TRACE") ? 1 : 0;
        for(int i=0;i<256;++i) gEngineStubVtbl[i]=(void*)gEngineNoop;
        GXNtsc480IntDf.fbWidth=640; GXNtsc480IntDf.efbHeight=480; GXNtsc480IntDf.xfbHeight=480;
        GXNtsc480IntDf.viWidth=640; GXNtsc480IntDf.viHeight=480;
    }
};
static EngineVtblInit gEngineVtblInit;
static void trace(const char* n){ if(gTrace) fprintf(stderr,"[stub] %s\n", n); }
static unsigned char gPtrGlobalStore[3][0x10000];

extern "C" {
// Interface acquisition returns the shared no-op interface (**vtable).
void** gBaddieControlInterface = (void**)&gEngineStubVtblPtr;
void** gBoneParticleEffectInterface = (void**)&gEngineStubVtblPtr;
void** gCameraInterface = (void**)&gEngineStubVtblPtr;
void** gCarryableInterface = (void**)&gEngineStubVtblPtr;
void** gCheckpointInterface = (void**)&gEngineStubVtblPtr;
void** gCloudActionInterface = (void**)&gEngineStubVtblPtr;
void** gDll12Interface = (void**)&gEngineStubVtblPtr;
void** gExpgfxInterface = (void**)&gEngineStubVtblPtr;
void** gGameUIInterface = (void**)&gEngineStubVtblPtr;
void** gMapEventInterface = (void**)&gEngineStubVtblPtr;
void** gMinimapInterface = (void**)&gEngineStubVtblPtr;
void** gModgfxInterface = (void**)&gEngineStubVtblPtr;
void** gNewCloudsInterface = (void**)&gEngineStubVtblPtr;
void** gObjectTriggerInterface = (void**)&gEngineStubVtblPtr;
void** gPartfxInterface = (void**)&gEngineStubVtblPtr;
void** gPathControlInterface = (void**)&gEngineStubVtblPtr;
void** gPlayerInterface = (void**)&gEngineStubVtblPtr;
void** gPlayerShadowInterface = (void**)&gEngineStubVtblPtr;
void** gProjgfxInterface = (void**)&gEngineStubVtblPtr;
void** gRomCurveInterface = (void**)&gEngineStubVtblPtr;
void** gScreenTransitionInterface = (void**)&gEngineStubVtblPtr;
void** gScreensInterface = (void**)&gEngineStubVtblPtr;
void** gSky2Interface = (void**)&gEngineStubVtblPtr;
void** gSkyInterface = (void**)&gEngineStubVtblPtr;
void** gTitleMenuControlInterface = (void**)&gEngineStubVtblPtr;
void** gTitleMenuControlInterfaceCopy = (void**)&gEngineStubVtblPtr;
void** gTitleMenuItemInterface = (void**)&gEngineStubVtblPtr;
void** gTitleMenuLinkInterface = (void**)&gEngineStubVtblPtr;
void** gWaterfxInterface = (void**)&gEngineStubVtblPtr;
void* gAskProgressiveScanFlag = gPtrGlobalStore[0];
void* gGameBitSaveData        = gPtrGlobalStore[1];
void* gGameBitTable           = gPtrGlobalStore[2];
int frameCountdown[256];
int gAudioStreamDvdState[256];
int gAudioStreamPlaying[256];
int gDvdCoverOpenErrorActive[256];
int gDvdLastDriveStatus[256];
int gGameBitCount[256];
int gGameLoopButtonObjectCount[256];
int gGameLoopButtonObjects[256];
int gGameLoopFullMapUnloadPending[256];
int gGameLoopHardReset[256];
int gGameLoopInitComplete[256];
int gGameLoopMapLoadPending[256];
int gGameLoopMapLoaded[256];
int gGameLoopMusicActive[256];
int gGameLoopMusicFadeTimer[256];
int gGameLoopMusicRequestCount[256];
int gGameLoopPendingMapDataFileId[256];
int gGameLoopPendingMapId[256];
int gGameLoopPendingMusicId[256];
int gGameLoopPendingUiDllId[256];
int gGameLoopPlayerTrailIndex[256];
int gGameLoopPlayerTrailTime[256];
int gGameLoopProgressiveMode[256];
int gGameLoopReloadRequested[256];
int gGameLoopResetComboDebounce[256];
int gGameLoopResetFadeOutTimer[256];
int gGameLoopResetHoldTimer[256];
int gVideoRetracePending[256];
int gameState[256];
int hudHiddenFrameCount[256];
int lbl_803DCA38[256];
int lbl_803DCA3F[256];
int sGameLoopResetMessages[256];
int screenBlankFrameCount[256];
int shouldResetNextFrame[256];
int timeStop[256];
int AISetStreamPlayState(){ trace("AISetStreamPlayState"); return 0; }
int AISetStreamVolLeft(){ trace("AISetStreamVolLeft"); return 0; }
int AISetStreamVolRight(){ trace("AISetStreamVolRight"); return 0; }
int LCDisable(){ trace("LCDisable"); return 0; }
int LCEnable(){ trace("LCEnable"); return 0; }
int Music_Trigger(){ trace("Music_Trigger"); return 0; }
int OSInitFastCast(){ trace("OSInitFastCast"); return 0; }
int OSSetSaveRegion(){ trace("OSSetSaveRegion"); return 0; }
int Rcp_InitDistortionEffects(){ trace("Rcp_InitDistortionEffects"); return 0; }
int Sfx_SetObjectSoundsPaused(){ trace("Sfx_SetObjectSoundsPaused"); return 0; }
int Sfx_UpdateLoopedObjectSounds(){ trace("Sfx_UpdateLoopedObjectSounds"); return 0; }
int _initCardAndDsp(){ trace("_initCardAndDsp"); return 0; }
int allocSomething32bytes(){ trace("allocSomething32bytes"); return 0; }
int askProgressiveScanMode(){ trace("askProgressiveScanMode"); return 0; }
int audioInit(){ trace("audioInit"); return 1; }
int audioReset(){ trace("audioReset"); return 0; }
int audioStopAll(){ trace("audioStopAll"); return 0; }
int audioUpdate(){ trace("audioUpdate"); return 0; }
int beginLoadingMap(){ trace("beginLoadingMap"); return 0; }
int clearForceLoadImmediately(){ trace("clearForceLoadImmediately"); return 0; }
// curUiDllDraw now real in bridge/game_model_support.c (uiDll spine) - stub removed.
int debugPrintDraw(){ trace("debugPrintDraw"); return 0; }
int debugPrintInit(){ trace("debugPrintInit"); return 0; }
int debugPrintReset(){ trace("debugPrintReset"); return 0; }
int doNothing_beforeTitleScreen(){ trace("doNothing_beforeTitleScreen"); return 0; }
int doNothing_endOfFrame(){ trace("doNothing_endOfFrame"); return 0; }
int doNothing_startOfFrame(){ trace("doNothing_startOfFrame"); return 0; }
int doPendingMapLoads(){ trace("doPendingMapLoads"); return 0; }
// drawRect now real in intersect_render.c - removed.
int dvdCheckError(){ trace("dvdCheckError"); return 0; }
int errDisplayInstallHandlers(){ trace("errDisplayInstallHandlers"); return 0; }
// gameTextInit/InitRendererState/LoadDir/Run/SetDrawFunc now provided by the real
// text cluster (textrender_run.c) - stubs removed (text subsystem bring-up).
int initGameTimer(){ trace("initGameTimer"); return 0; }
int initLoadFiles(){ trace("initLoadFiles"); return 1; }
int initLoadingScreenTextures(){ trace("initLoadingScreenTextures"); return 0; }
int initMapBlocks(){ trace("initMapBlocks"); return 0; }
int initMaps(){ trace("initMaps"); return 0; }
int initSkyStars(){ trace("initSkyStars"); return 0; }
int initTextures(){ trace("initTextures"); return 0; }
int isSaveGameLoading(){ trace("isSaveGameLoading"); return 0; }
int loadDataFiles(){ trace("loadDataFiles"); return 0; }
int loadMapAndParent(){ trace("loadMapAndParent"); return 0; }
int loadTaskTexts(){ trace("loadTaskTexts"); return 0; }
int loadTextureFiles(){ trace("loadTextureFiles"); return 0; }
// loadUiDll now real in bridge/game_model_support.c (uiDll spine) - stub removed.
// mainLoopDoGameText now provided by the real text cluster (subtitle.c) - stub removed.
int mapLoadDataFiles(){ trace("mapLoadDataFiles"); return 0; }
int mapReloadWithFadeout(){ trace("mapReloadWithFadeout"); return 0; }
int mapSetup(){ trace("mapSetup"); return 0; }
int mapUnload(){ trace("mapUnload"); return 0; }
int mapUpdateCameraPosByTransformSpace(){ trace("mapUpdateCameraPosByTransformSpace"); return 0; }
int newshadows_initProceduralTextures(){ trace("newshadows_initProceduralTextures"); return 0; }
int objRenderModelAndHitVolumes(){ trace("objRenderModelAndHitVolumes"); return 0; }
// playerInitFuncPtrsEntry now provided by the real player DLL (player.c) - stub removed (Phase A).
// resetSomeGxFlags now real in intersect_render.c - removed.
int runLoadingScreens(){ trace("runLoadingScreens"); return 0; }
int setDrawCloudsAndLights(){ trace("setDrawCloudsAndLights"); return 0; }
int setForceLoadImmediately(){ trace("setForceLoadImmediately"); return 0; }
// subtitleInit/subtitleUpdateAndDraw now provided by the real text cluster (subtitle.c) - stubs removed.
int taskHintRecordCompletedTask(){ trace("taskHintRecordCompletedTask"); return 0; }
int trackInitCollisionBuffers(){ trace("trackInitCollisionBuffers"); return 0; }
int trackIntersect(){ trace("trackIntersect"); return 0; }
int tvInit(){ trace("tvInit"); return 0; }
// uiDll_runFrameEnd/StartAndLoadNext now real in bridge/game_model_support.c - stubs removed.
int unloadMap(){ trace("unloadMap"); return 0; }
int updateEnvironment(){ trace("updateEnvironment"); return 0; }
int waterFxInit(){ trace("waterFxInit"); return 0; }
int waterFxUpdate(){ trace("waterFxUpdate"); return 0; }
int mmSetForceHeap3Only(){ trace("mmSetForceHeap3Only"); return 0; }
}

