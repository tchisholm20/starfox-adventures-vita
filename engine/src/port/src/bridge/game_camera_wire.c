// game_camera_wire.c - activates the REAL engine/1 camcontrol + engine/66 (0x42) follow camera.
//
// Real-camera bring-up Stage 2, runtime wiring. The port spawns Fox through its own harness rather
// than the object system's loadCharacter path, so the engine's own camera setup (object.c:838-848 -
// init / setMode(0x42) / update) never runs. This mirrors that default-gameplay setup from the port
// side, once, after the player is loaded: point the real camcontrol interface at the player and put
// it in the standard follow mode. Everything here is gated by STAIRFAX_REAL_CAM (registration of the
// camera DLLs is gated by the same flag in game_resource.c), so the default build keeps running on
// the interim port follow-cam while this is brought up and verified.

#include "main/camera_interface.h"   // CameraInterface, gCameraInterface (set by gameloop Resource_Acquire(1))
#include <stdio.h>
#include <stdlib.h>

extern unsigned char gObjCameraSetupBlock[32];   // object.c:142 - the default-mode setup params
extern void* Camera_GetCurrent(void);            // camera.c:862 - &gCameras[gCameraCurrentViewIndex]

// PPC->x86 ABI fix. staffanim (67.c:264) invokes the SHARED normal-mode updatePitch through the handler
// vtable as (CameraObject*, double, double), but the impl is CameraModeNormal_updatePitch(f32, f32,
// CameraObject*). On PPC floats and pointers use separate register files, so both the arg reorder and
// the double->f32 are free; on x86 cdecl every arg is stack-passed in order at its own size, so the
// callee reads a double's bits where it expects the camera pointer -> deref garbage -> crash. Patch the
// normal descriptor's updatePitch slot (offset 0x30) with an adapter that has the x86 call shape and
// forwards to the real impl. Only cross-mode VTABLE calls hit the slot; 66.c calls updatePitch directly
// by name and is unaffected. gCameraModeNormalDescriptor is engine/66's descriptor global.
extern char gCameraModeNormalDescriptor[];
extern void CameraModeNormal_updatePitch(float targetY, float dist, void* camera);
static void updatePitch_x86adapter(void* camera, double targetY, double dist) {
    CameraModeNormal_updatePitch((float)targetY, (float)dist, camera);
}
static void stairfax_patch_camera_abi(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    *(void**)(gCameraModeNormalDescriptor + 0x30) = (void*)updatePitch_x86adapter;
}
extern float* Camera_GetViewMatrix(void);        // camera.c:745 - gCameraViewMatrix (3x4 world->view)

// Set to 1 once the real follow cam is activated (setMode 0x42 done). game_scene.cpp reads this to
// switch the terrain render onto the real view matrix - only when the real cam is actually driving,
// so non-player dev runs (map/object viewers, no follow mode) keep their own interim view.
int gStairfaxRealCamActive = 0;

// Object world position: worldPosX/Y/Z @ 0x18/0x1C/0x20 (the harness syncs these from localPos).
static float objX(void* o) { return *(float*)((unsigned char*)o + 0x18); }
static float objY(void* o) { return *(float*)((unsigned char*)o + 0x1C); }
static float objZ(void* o) { return *(float*)((unsigned char*)o + 0x20); }

// One-time: put the real camera into follow mode targeting the player. Mirrors object.c:846-848
// (default gameplay branch): init establishes the focus, setMode(0x42) selects engine/66, update
// primes it. Safe no-op if the camcontrol interface isn't real (id 1 not registered).
// gObjCameraSetupBlock is raw GC (big-endian) CameraModeNormalInitSettings data (object.c:142). Its
// multi-byte fields must be swapped to host order or the camera reads garbage. Only the two u16
// distance fields carry nonzero multi-byte data: maxDistanceWide@0x1A, minDistanceWide@0x1C (bytes
// 0x00..0x18 are all zero; fovWide@0x19 / heightOffsetWide@0x1F are single bytes). Read BE they are
// 0x005A=90 / 0x0055=85 (sane); read LE they were 23040 / 21760 -> camera ~256x too far.
static unsigned char sCamSetup[32];
static void* camSetupHostOrder(void) {
    int i; unsigned char t;
    for (i = 0; i < 32; ++i) sCamSetup[i] = gObjCameraSetupBlock[i];
    t = sCamSetup[0x1A]; sCamSetup[0x1A] = sCamSetup[0x1B]; sCamSetup[0x1B] = t;  // maxDistanceWide
    t = sCamSetup[0x1C]; sCamSetup[0x1C] = sCamSetup[0x1D]; sCamSetup[0x1D] = t;  // minDistanceWide
    return sCamSetup;
}

void stairfax_real_camera_setup(void* player) {
    if (gCameraInterface == 0 || *gCameraInterface == 0 || player == 0) return;
    stairfax_patch_camera_abi();   // fix the cross-mode updatePitch vtable ABI before any mode switch
    (*gCameraInterface)->init(player, objX(player), objY(player) + 40.0f, objZ(player));
    (*gCameraInterface)->setMode(0x42, 0, 0, 0x20, camSetupHostOrder(), 0, 0xff);
    (*gCameraInterface)->update(1);
    gStairfaxRealCamActive = 1;
    if (getenv("STAIRFAX_CAM_TRACE"))
        fprintf(stderr, "[realcam] setup: mode=%d iface=%p\n",
                (*gCameraInterface)->getMode(), (void*)*gCameraInterface);
}

// Per-frame diagnostic: is the real camera following the player? Reads the live Camera the player
// itself reads (Camera_GetCurrent -> gCameras[view]); yaw@0x00 (s16), pos@0x0C/0x10/0x14 (f32).
void stairfax_real_camera_trace(void* player) {
    if (!getenv("STAIRFAX_CAM_TRACE")) return;
    static int n = 0;
    unsigned char* c = (unsigned char*)Camera_GetCurrent();
    if (c == 0) return;
    if ((n++ % 30) != 0) return;
    int mode = (gCameraInterface && *gCameraInterface) ? (*gCameraInterface)->getMode() : -1;
    fprintf(stderr, "[realcam] mode=0x%X camYaw=%d camPos=(%.1f,%.1f,%.1f) playerPos=(%.1f,%.1f,%.1f)\n",
            mode, *(short*)c, *(float*)(c + 0x0C), *(float*)(c + 0x10), *(float*)(c + 0x14),
            objX(player), objY(player), objZ(player));
    // CameraObject (getCamera): anim.localPos@0x0C, worldPos@0x18, parent@0x30, rotY@0x02. If
    // worldPos = localPos + huge with parent!=0, the local->world transform is the culprit.
    if (gCameraInterface && *gCameraInterface) {
        unsigned char* co = (unsigned char*)(*gCameraInterface)->getCamera();
        if (co) {
            fprintf(stderr, "[realcam]   camObj local=(%.1f,%.1f,%.1f) world=(%.1f,%.1f,%.1f) parent=%p rotY=%d fov=%.1f\n",
                    *(float*)(co + 0x0C), *(float*)(co + 0x10), *(float*)(co + 0x14),
                    *(float*)(co + 0x18), *(float*)(co + 0x1C), *(float*)(co + 0x20),
                    *(void**)(co + 0x30), *(short*)(co + 0x02), *(float*)(co + 0xB4));
        }
    }
    { const float* vm = Camera_GetViewMatrix();
      if (vm) fprintf(stderr, "[realcam]   viewMtx [%.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f]\n",
              vm[0],vm[1],vm[2],vm[3], vm[4],vm[5],vm[6],vm[7], vm[8],vm[9],vm[10],vm[11]); }
    fflush(stderr);
}
