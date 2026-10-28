// game_camera_stubs.c - support layer so the real src/main/camera.c + voxmaps.c link and run.
//
// Stage 1 of the real-camera bring-up (engine/1_camcontrol). camera.c/voxmaps.c compile cleanly but
// reference symbols scattered across src/main TUs (vecmath/lightmap/shader/pi_dolphin/gametext) that
// aren't compiled into the port yet - pulling each in cascades further deps. So the handful they need
// are provided here: REAL matrix/perspective math (the camera needs correct view/projection), SAFE
// stubs for the voxel-collision / map-query / viewport bits (the camera works without wall-avoidance
// initially - refined in a later stage), and the output-matrix / vox-data globals as zeroed storage.
// C linkage matches by name, so float* stands in for the Mtx/Mtx44 array typedefs.

#include <string.h>
extern float floorf(float);
extern float tanf(float);

// --- camera output matrices (camera.c writes these) + map-origin globals -----
float gCameraModelViewMatrix[3][4];
float gCameraLightPerspectiveFlipYMatrix[3][4];
float gCameraLightPerspectiveScaledMatrix[3][4];
unsigned int gViewportJitterField;
int gMapBlockOriginWorldX;
int gMapBlockOriginWorldZ;

// --- voxel-collision data (loadVoxMaps fills it; zeroed = no line-of-sight hits) ---
unsigned char gVoxMaps[0x74];           // sizeof(VoxMaps) == 0x74
unsigned char gVoxMapsRouteState[0x14]; // sizeof(VoxState) == 0x14
int gVoxMapsSlotTimers[6];              // VOXMAP_SLOT_COUNT == 6

// --- matrix / perspective math (real; convention verified when wired to the render in stage 2) -----
#define DEG2RAD 0.017453292f

// GC C_MTXPerspective: symmetric perspective, +Z into the screen (LH), row-major 4x4.
void C_MTXPerspective(float* m, float fovY, float aspect, float n, float f) {
    float cot = 1.0f / tanf(fovY * 0.5f * DEG2RAD);
    memset(m, 0, 16 * sizeof(float));
    m[0*4+0] = cot / aspect;
    m[1*4+1] = cot;
    m[2*4+2] = f / (f - n);
    m[2*4+3] = -(n * f) / (f - n);
    m[3*4+2] = 1.0f;
}

// Light-perspective (texture projection matrix): perspective scaled/biased into [0,1] tex space.
void C_MTXLightPerspective(float* m, float fovY, float aspect, float scaleS, float scaleT,
                           float transS, float transT) {
    float cot = 1.0f / tanf(fovY * 0.5f * DEG2RAD);
    memset(m, 0, 12 * sizeof(float));   // Mtx is 3x4
    m[0*4+0] = (cot / aspect) * scaleS;  m[0*4+2] = -transS;
    m[1*4+1] = cot * scaleT;             m[1*4+2] = -transT;
    m[2*4+2] = -1.0f;
}

// Port perspective (LH/+Z, matching game_scene.cpp's own projection); perspectiveNorm unused here.
void mtx44Perspective(float* matrix, unsigned short* perspectiveNorm, float fovY, float aspect,
                      float nearPlane, float farPlane, float scale) {
    (void)perspectiveNorm; (void)scale;
    float ys = 1.0f / tanf(fovY * 0.5f * DEG2RAD);
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0*4+0] = ys / aspect;
    matrix[1*4+1] = ys;
    matrix[2*4+2] = farPlane / (farPlane - nearPlane);
    matrix[2*4+3] = -nearPlane * farPlane / (farPlane - nearPlane);
    matrix[3*4+2] = 1.0f;
}

void mtx44ScaleRow1(float* matrix, float scale) { for (int i = 0; i < 4; ++i) matrix[4 + i] *= scale; }

void copyMatrix44(float* src, float* dst) { memcpy(dst, src, 16 * sizeof(float)); }

void mtx44_multSafe(float* lhs, float* rhs, float* out) {
    float tmp[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            tmp[r*4+c] = lhs[r*4+0]*rhs[0*4+c] + lhs[r*4+1]*rhs[1*4+c]
                       + lhs[r*4+2]*rhs[2*4+c] + lhs[r*4+3]*rhs[3*4+c];
    memcpy(out, tmp, 16 * sizeof(float));
}

float fastFloorf(float value) { return floorf(value); }

// --- safe stubs: camera runs without voxel-collision / per-cell map queries initially -----
void* mapGetBlockAtPos(int x, int y, int layer) { (void)x;(void)y;(void)layer; return 0; }
void* mapGetCellEntry(int x, int z)             { (void)x;(void)z; return 0; }
void  loadVoxMaps(int handle, int* outCount, int* outSize) {
    (void)handle; if (outCount) *outCount = 0; if (outSize) *outSize = 0;
}
void  GXSetViewportJitter(float l, float t, float w, float h, float nz, float fz, unsigned f) {
    (void)l;(void)t;(void)w;(void)h;(void)nz;(void)fz;(void)f;
}
void  gxSetScissorRect(int p1, int p2, int x, int y, int x2, int y2) {
    (void)p1;(void)p2;(void)x;(void)y;(void)x2;(void)y2;
}
unsigned char pauseMenuGetState(void) { return 0; }   // 0 = not paused
