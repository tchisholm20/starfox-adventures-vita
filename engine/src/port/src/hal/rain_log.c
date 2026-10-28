// rain_log.c - one-shot instrument for the newclouds rain/snow draw (dlls/engine/7 snowPrintSnowCloud).
// 2026-10-11. Armed by the marker file ux0:data/stairfax/rain_log.txt (read once). Prints to the game log
// (printf), the first 3 calls and then one call in 120, per cloud:
//   [rain] cloud id/type/count, active flakes, world pos, camera pos + yaw, wind, drift, scale/height, box
//   [rain] mtxT (cloud-space -> view, 3x4 row-major), view matrix (Camera_GetViewMatrix, 3x4)
//   [rain] size histogram over ALL flakes (the big-endian range table bug showed here: sizes 1..126)
//   [rain] flakes 0..2: x y z angle size fall quad, cloud-space verts, VIEW-space verts, and the streak
//          direction in view space (apex minus base midpoint): a falling streak has view-Y << 0
// Game structs are mirrored here by layout (SnowFlake 0x18, SnowQuad 0x2C) so this file needs no game headers.
#include <stdio.h>
#include <math.h>
#include <stdint.h>

typedef struct { float x, y, z, fall; uint16_t angle, quadIndex; int8_t size, spin; uint8_t texLayer, u17; } RLFlake;
typedef struct { float v[9]; uint16_t vA, vB, aA, aB; } RLQuad;

static void xf(const float* m, const float* p, float* o) {
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

void stfx_rain_log(int id, int type, int count, float active, const float* cf /*15*/, const float* cam /*4*/,
                   const float* view /*12*/, const float* mtxT /*12*/, const void* flakes, const void* quads,
                   const float* wave, float timeDelta, int framesThisStep) {
    static int sOn = -1;
    static unsigned sN;
    const RLFlake* fl = (const RLFlake*)flakes;
    const RLQuad* q = (const RLQuad*)quads;
    int i, k, nOne = 0, nBig = 0, mn = 127, mx = -128;
    if (sOn < 0) { FILE* f = fopen("ux0:data/stairfax/rain_log.txt", "r"); sOn = f ? 1 : 0; if (f) fclose(f); }
    if (!sOn) return;
    if (!(sN < 3 || sN % 120 == 0)) { sN++; return; }
    sN++;
    printf("[rain] #%u cloud %d type %d flakes %d active %.1f timeDelta %.3f framesThisStep %d\n",
           sN, id, type, count, active, timeDelta, framesThisStep);
    printf("[rain]   cloudpos %.1f %.1f %.1f | cam %.1f %.1f %.1f yaw %d | wind %.3f %.3f drift %.2f scale %.2f height %.2f\n",
           cf[0], cf[1], cf[2], cam[0], cam[1], cam[2], (int)cam[3], cf[3], cf[4], cf[5], cf[6], cf[7]);
    printf("[rain]   box x %.1f..%.1f z %.1f..%.1f centerY %.1f | lastpos %.1f %.1f %.1f\n",
           cf[8], cf[9], cf[10], cf[11], cf[12], cf[13], cf[14], cf[15]);
    printf("[rain]   mtxT %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f\n", mtxT[0], mtxT[1], mtxT[2],
           mtxT[3], mtxT[4], mtxT[5], mtxT[6], mtxT[7], mtxT[8], mtxT[9], mtxT[10], mtxT[11]);
    printf("[rain]   view %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f | %.4f %.4f %.4f %.2f\n", view[0], view[1], view[2],
           view[3], view[4], view[5], view[6], view[7], view[8], view[9], view[10], view[11]);
    for (i = 0; i < count; i++) {
        if (fl[i].size <= 1) nOne++;
        if (fl[i].size > 88) nBig++;
        if (fl[i].size < mn) mn = fl[i].size;
        if (fl[i].size > mx) mx = fl[i].size;
    }
    printf("[rain]   flake size min %d max %d | size<=1: %d of %d | size>88: %d  (retail rain: 2..112 by class, never a clump at 1)\n",
           mn, mx, nOne, count, nBig);
    for (k = 0; k < 3 && k < count; k++) {
        const RLFlake* f = &fl[k];
        const RLQuad* qd = &q[f->quadIndex % 20];
        float wv = f->angle < 1024 ? wave[f->angle] : NAN;
        float yb = f->y - wv;
        float lv[3][3], vv[3][3], mid[3], dir[3], len;
        int j;
        for (j = 0; j < 3; j++) {
            lv[j][0] = qd->v[j] * f->fall + f->x;
            lv[j][1] = qd->v[3 + j] * f->fall + yb;
            lv[j][2] = qd->v[6 + j] * f->fall + f->z;
            xf(mtxT, lv[j], vv[j]);
        }
        mid[0] = 0.5f * (vv[0][0] + vv[1][0]); mid[1] = 0.5f * (vv[0][1] + vv[1][1]); mid[2] = 0.5f * (vv[0][2] + vv[1][2]);
        dir[0] = vv[2][0] - mid[0]; dir[1] = vv[2][1] - mid[1]; dir[2] = vv[2][2] - mid[2];
        len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len > 0.0f) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
        printf("[rain]   flake %d xyz %.1f %.1f %.1f angle %u wave %.1f size %d fall %.2f quad %u | local (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f)\n",
               k, f->x, f->y, f->z, f->angle, wv, f->size, f->fall, f->quadIndex, lv[0][0], lv[0][1], lv[0][2],
               lv[1][0], lv[1][1], lv[1][2], lv[2][0], lv[2][1], lv[2][2]);
        printf("[rain]     VIEW (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f) streak len %.1f dir %.3f %.3f %.3f\n",
               vv[0][0], vv[0][1], vv[0][2], vv[1][0], vv[1][1], vv[1][2], vv[2][0], vv[2][1], vv[2][2], len, dir[0], dir[1], dir[2]);
    }
}
