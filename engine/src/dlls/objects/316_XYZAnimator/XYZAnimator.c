/* Applies a game-bit-controlled XYZ offset to map-block polygons and display-list bounds. */
#include "dlls/objects/316_XYZAnimator.h"

#include "dolphin/os/OSCache.h"
#include "main/audio/sfx_keep_alive_api.h"
#include "main/frame_timing.h"
#include "main/gamebits.h"
#include "main/lightmap_api.h"
#include "main/map_block.h"
#include "main/mm.h"
#include "main/object_render.h"
#include "main/objtype.h"
#include "main/pi_dolphin_api.h"
#include "main/track_dolphin_api.h"

#ifdef STAIRFAX_PORT
/* PORT FIX (2026-10-18, build 279 report; docs/47). A map block's vertex array STAYS BIG-ENDIAN on the Vita (mldf_endian.c
 * stfx_mapblock_loaded leaves it in disc order for the GX layer), and this object reads and writes it on the CPU. Natively,
 * captureGeometry stored byte-swapped positions and applyToMapBlock wrote (swapped + 8*offset) back: an identity at offset 0,
 * garbage at any other. Tyler's SwapStone Hollow shortcut (hollow2, idents 302893 / 302913: bit 222 -> layer 1 down 32 units)
 * vanished the moment it began to lower. Both ends are big-endian now, and the write is reported to the display-list cache,
 * which re-reads an array only on an event (the same pair GroundAnima needed, run 276).
 * XYZ_ANIM_BREAK (host test only): the old native access. */
static inline s16 xyzVtxLoad(const s16* p) {
#ifdef XYZ_ANIM_BREAK
    return *p;
#else
    const u8* b = (const u8*)p; return (s16)(u16)((b[0] << 8) | b[1]);
#endif
}
/* v is the float sum; (s32) first so an out-of-range value wraps to its low 16 bits as PowerPC's fctiwz + sth does */
static inline void xyzVtxStore(s16* p, f32 v) {
#ifdef XYZ_ANIM_BREAK
    *p = (s16)v;
#else
    s32 w = (s32)v; u8* b = (u8*)p; b[0] = (u8)((u32)w >> 8); b[1] = (u8)w;
#endif
}
#define XYZ_VTX_LOAD(p)     xyzVtxLoad(&(p))
#define XYZ_VTX_STORE(p, v) xyzVtxStore(&(p), (v))
#else
#define XYZ_VTX_LOAD(p)     (p)
#define XYZ_VTX_STORE(p, v) ((p) = (s16)(v))
#endif

f32 XyzAnimator_getCoordinate(GameObject* obj, u8 coordinate) {
    XyzAnimatorState* state;

    if (obj == NULL || (state = (XyzAnimatorState*)obj->extra, state == NULL)) {
        return 0.0f;
    }
    switch (coordinate) {
    case XYZ_ANIMATOR_COORD_WORLD_X:
        return obj->anim.localPosX + state->offsetX;
    case XYZ_ANIMATOR_COORD_OFFSET_X:
        return state->offsetX;
    case XYZ_ANIMATOR_COORD_WORLD_Y:
        return obj->anim.localPosY + state->offsetY;
    case XYZ_ANIMATOR_COORD_OFFSET_Y:
        return state->offsetY;
    case XYZ_ANIMATOR_COORD_WORLD_Z:
        return obj->anim.localPosZ + state->offsetZ;
    case XYZ_ANIMATOR_COORD_OFFSET_Z:
        return state->offsetZ;
    }
    return 0.0f;
}

void XyzAnimator_captureGeometry(XyzAnimatorPlacement* placement, XyzAnimatorState* state, MapBlockData* blockAddress) {
    Vec3s* vertex;
    int vertexDataOffset[1];
    int groupDataOffset[1];
    int triangleDataOffset[1];
    int blockIndex;
    int triangle;
    int triangleEnd;
    u16* mapEntry;
    int t;
    int dataOffset;
    int index;
    int boundsBufferOffset[1];
    int displayListIndex[1];
    MapBlockData* blockData = blockAddress;

    vertexDataOffset[0] = 0;
    boundsBufferOffset[0] = 0;
    blockIndex = 0;
    groupDataOffset[0] = 0;
    triangleDataOffset[0] = groupDataOffset[0];
    for (; blockIndex < (int)(u32)blockData->polyGroupCount; blockIndex++) {
        mapEntry = (u16*)mapBlockGetPolygonGroup(blockAddress, blockIndex);
        t = mapBlockGetPolygonGroupType(mapEntry);
        if ((int)placement->blockLayer == t) {
            *(s16*)(state->posABuffer + groupDataOffset[0]) = ((CollisionPolygonGroup*)mapEntry)->minY;
            *(s16*)(state->posBBuffer + groupDataOffset[0]) = ((CollisionPolygonGroup*)mapEntry)->maxY;
            groupDataOffset[0] += 2;
            triangleEnd = mapEntry[10];
            triangle = *mapEntry;
            vertexDataOffset[0] = triangleDataOffset[0];
            for (; triangle < triangleEnd; triangle++) {
                mapEntry = mapBlockGetPolygon(blockAddress, triangle)->vert;
                dataOffset = vertexDataOffset[0];
                for (index = 0; index != 3; index++) {
                    vertex = &((Vec3s*)blockData->vertices)[mapEntry[index]];
                    ((Vec3s*)(state->geometryBuffer + dataOffset))->x = XYZ_VTX_LOAD(vertex->x);
                    ((Vec3s*)(state->geometryBuffer + dataOffset))->y = XYZ_VTX_LOAD(vertex->y);
                    ((Vec3s*)(state->geometryBuffer + dataOffset))->z = XYZ_VTX_LOAD(vertex->z);
                    dataOffset += 6;
                }
                vertexDataOffset[0] += 0x12;
                triangleDataOffset[0] += 0x12;
            }
        }
    }
    displayListIndex[0] = 0;
    boundsBufferOffset[0] = displayListIndex[0];
    for (; displayListIndex[0] < (int)(u32)blockData->displayListCount; displayListIndex[0]++) {
        blockIndex = (int)mapBlockGetDisplayListBounds(blockAddress, displayListIndex[0]);
        *(s16*)(state->minXBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->minX;
        *(s16*)(state->maxXBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->maxX;
        *(s16*)(state->minYBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->minY;
        *(s16*)(state->maxYBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->maxY;
        *(s16*)(state->minZBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->minZ;
        *(s16*)(state->maxZBuffer + boundsBufferOffset[0]) = ((MapBlockBoundsRec*)blockIndex)->maxZ;
        boundsBufferOffset[0] += 2;
    }
}

int XyzAnimator_getExtraSize(void) {
    return sizeof(XyzAnimatorState);
}

void XyzAnimator_free(GameObject* obj, int flags) {
    int blockAddress;
    XyzAnimatorState* state;
    XyzAnimatorPlacement* placement;
    f32 zeroOffset;

    state = (XyzAnimatorState*)obj->extra;
    placement = (XyzAnimatorPlacement*)obj->anim.placementData;
    zeroOffset = 0.0f;
    state->offsetX = zeroOffset;
    state->offsetY = zeroOffset;
    state->offsetZ = zeroOffset;
    if (flags == 0) {
        blockAddress =
            objPosToMapBlockIdx((double)obj->anim.localPosX, (double)obj->anim.localPosY, (double)obj->anim.localPosZ);
        blockAddress = (int)mapGetBlock(blockAddress);
        if (((void*)blockAddress != NULL) && (state->vertexCount != 0)) {
            XyzAnimator_applyToMapBlock(placement, state, (MapBlockData*)blockAddress);
        }
    }
    if ((void*)state->geometryBuffer != NULL) {
        mm_free((void*)state->geometryBuffer);
    }
    objFreeObjectType(obj, XYZ_ANIMATOR_OBJECT_GROUP);
}

void XyzAnimator_render(GameObject* obj, int renderArg2, int renderArg3, int renderArg4, int renderArg5, s8 visible) {
    s32 isVisible = visible;

    if (isVisible != 0) {
        objRenderModelAndHitVolumes(obj, renderArg2, renderArg3, renderArg4, renderArg5, 1.0f);
    }
}

void XyzAnimator_applyToMapBlock(XyzAnimatorPlacement* placement, XyzAnimatorState* state, MapBlockData* blockAddress) {
    Vec3s* vertex;
    MapBlockData* blockData = blockAddress;
    int vertexOffset[1];
    int vertexIndex;
    int polygonGroupIndex;
    int polygonGroupType;
    int dataOffset;
    u16* mapEntry;
    f32 scale;
    int triangle;
    int triangleEnd;
    int index;
    int groupDataOffset[1];
    void* shaderLayer;
#ifdef STAIRFAX_PORT
    Vec3s* xyzFirstV = 0; s16 xyzFirstCap = 0;   /* [xyz] instrument (2026-10-19): the first vertex this call writes */
#endif

    polygonGroupIndex = 0;
    groupDataOffset[0] = 0;
    vertexOffset[0] = groupDataOffset[0];
    for (; polygonGroupIndex < (int)(u32)blockData->polyGroupCount; polygonGroupIndex++) {
        mapEntry = (u16*)mapBlockGetPolygonGroup(blockAddress, polygonGroupIndex);
        polygonGroupType = mapBlockGetPolygonGroupType(mapEntry);
        if ((int)placement->blockLayer == polygonGroupType) {
            ((CollisionPolygonGroup*)mapEntry)->minY =
                (s16)(state->offsetY + (f32) * (s16*)(state->posABuffer + groupDataOffset[0]));
            ((CollisionPolygonGroup*)mapEntry)->maxY =
                (s16)(state->offsetY + (f32) * (s16*)(state->posBBuffer + groupDataOffset[0]));
            groupDataOffset[0] += 2;
            triangleEnd = mapEntry[10];
            triangle = *mapEntry;
            vertexIndex = vertexOffset[0];
            scale = 8.0f;
            for (; triangle < triangleEnd; triangle++) {
                mapEntry = mapBlockGetPolygon(blockAddress, triangle)->vert;
                dataOffset = vertexIndex;
                for (index = 0; index != 3; index++) {
                    vertex = &((Vec3s*)blockData->vertices)[mapEntry[index]];
                    XYZ_VTX_STORE(vertex->x, scale * state->offsetX + (f32)((Vec3s*)(state->geometryBuffer + dataOffset))->x);
                    XYZ_VTX_STORE(vertex->y, scale * state->offsetY + (f32)((Vec3s*)(state->geometryBuffer + dataOffset))->y);
#ifdef STAIRFAX_PORT
                    if (!xyzFirstV) { xyzFirstV = vertex; xyzFirstCap = ((Vec3s*)(state->geometryBuffer + dataOffset))->y; }
#endif
                    XYZ_VTX_STORE(vertex->z, scale * state->offsetZ + (f32)((Vec3s*)(state->geometryBuffer + dataOffset))->z);
                    dataOffset += 6;
                    vertexIndex += 6;
                    vertexOffset[0] += 6;
                }
            }
        }
    }
    DCStoreRange((void*)blockData->vertices, (u32)blockData->vertexCount * 6);
#ifdef STAIRFAX_PORT
    {   /* the port's DCStoreRange only invalidates textures; the display-list cache needs a write event to re-read the block */
        extern void gx_dlk_event(const void* p, size_t n, int isWrite) __attribute__((weak));
        if (gx_dlk_event) gx_dlk_event((void*)blockData->vertices, (size_t)blockData->vertexCount * 6, 1);
    }
    /* [xyz] instrument (2026-10-19, build 285): what the move WROTE, read back the way the GX decoder reads it (big-endian).
       One line per object at its first apply, then each time offsetY moves >= 4 units, and at the target. <= 4 objects tracked. */
    {   extern int printf(const char*, ...);
        static struct { void* st; int last; } xyzSlots[4];
        int k, oy = (int)state->offsetY;
        for (k = 0; k < 4 && xyzSlots[k].st && xyzSlots[k].st != (void*)state; ++k) { }
        if (k < 4 && xyzFirstV) {
            const int first = !xyzSlots[k].st, atTarget = state->offsetY == (f32)placement->targetY;
            if (first || oy - xyzSlots[k].last >= 4 || xyzSlots[k].last - oy >= 4 || (atTarget && oy != xyzSlots[k].last)) {
                const u8* gb = (const u8*)&xyzFirstV->y; const s16 got = (s16)(u16)((gb[0] << 8) | gb[1]);   /* explicit BE: NOT XYZ_VTX_LOAD, so a broken accessor cannot agree with itself */
                const s16 want = (s16)(s32)(8.0f * state->offsetY + (f32)xyzFirstCap);
                printf("[xyz] obj state %p layer %d offset (%.2f, %.2f, %.2f) target Y %d | vertex 0 y: captured %d -> written %d = %.1f units (want %.1f)%s\n",
                       (void*)state, (int)placement->blockLayer, state->offsetX, state->offsetY, state->offsetZ, (int)placement->targetY,
                       (int)xyzFirstCap, (int)got, got / 8.0, want / 8.0, got == want ? "" : "  <-- MISMATCH");
                xyzSlots[k].st = (void*)state; xyzSlots[k].last = oy;
            }
        }
    }
#endif
    index = 0;
    dataOffset = index;
    for (; index < (int)(u32)blockData->displayListCount; index++) {
        vertexOffset[0] = (int)mapBlockGetDisplayListBounds(blockAddress, index);
        shaderLayer = mapBlockGetShader(blockAddress, *(u8*)(vertexOffset[0] + 0x13));
        shaderLayer = Shader_getLayer(shaderLayer, 0);
        if ((int)*(u8*)((int)shaderLayer + 5) == placement->blockLayer) {
            scale = 8.0f;
            ((MapBlockBoundsRec*)vertexOffset[0])->minX =
                (s16)(scale * state->offsetX + (f32) * (s16*)(state->minXBuffer + dataOffset));
            ((MapBlockBoundsRec*)vertexOffset[0])->maxX =
                (s16)(scale * state->offsetX + (f32) * (s16*)(state->maxXBuffer + dataOffset));
            ((MapBlockBoundsRec*)vertexOffset[0])->minY =
                (s16)(scale * state->offsetY + (f32) * (s16*)(state->minYBuffer + dataOffset));
            ((MapBlockBoundsRec*)vertexOffset[0])->maxY =
                (s16)(scale * state->offsetY + (f32) * (s16*)(state->maxYBuffer + dataOffset));
            ((MapBlockBoundsRec*)vertexOffset[0])->minZ =
                (s16)(scale * state->offsetZ + (f32) * (s16*)(state->minZBuffer + dataOffset));
            ((MapBlockBoundsRec*)vertexOffset[0])->maxZ =
                (s16)(scale * state->offsetZ + (f32) * (s16*)(state->maxZBuffer + dataOffset));
        }
        dataOffset += 2;
    }
    blockAddress->unused00 = mapBlockGetUnused00Value(blockAddress);
}

void XyzAnimator_update(GameObject* obj) {
    XyzAnimatorPlacement* placement = (XyzAnimatorPlacement*)obj->anim.placementData;
    XyzAnimatorState* state = (XyzAnimatorState*)obj->extra;
    MapBlockData* blockAddress;
    CollisionPolygonGroup* polygonGroup;
    int polygonGroupIndex;
    int completedAxes;
    u8* bufferAddress;
    int streamSize;
    int value;

    blockAddress =
        (MapBlockData*)mapGetBlock(objPosToMapBlockIdx(obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ));
    if (blockAddress == NULL) {
        state->passCount = 0;
        return;
    }
    if ((blockAddress->flags4 & MAP_BLOCK_FLAG_LOADED) == 0) {
        return;
    }
    if (state->vertexCount == 0) {
        for (polygonGroupIndex = 0; polygonGroupIndex < blockAddress->polyGroupCount; polygonGroupIndex++) {
            polygonGroup = mapBlockGetPolygonGroup((void*)blockAddress, polygonGroupIndex);
            value = mapBlockGetPolygonGroupType(polygonGroup);
            if (placement->blockLayer == value) {
                state->polygonGroupCount++;
                state->vertexCount += (polygonGroup)[1].firstTri - polygonGroup->firstTri;
            }
        }
        if (state->vertexCount == 0) {
            return;
        }
        state->vertexCount *= 3;
        if (placement->triggerGameBit == -1) {
            state->triggerBitValue = 1;
        } else {
            state->triggerBitValue = mainGetBit(placement->triggerGameBit);
        }
        state->displayListCount = blockAddress->displayListCount;
        state->offsetX = (f32)placement->startX;
        state->offsetY = (f32)placement->startY;
        state->offsetZ = (f32)placement->startZ;
        if (placement->completionGameBit != -1 && mainGetBit(placement->completionGameBit) != 0) {
            state->offsetX = (f32)placement->targetX;
            state->offsetY = (f32)placement->targetY;
            state->offsetZ = (f32)placement->targetZ;
            state->triggerBitValue = 1;
        }
        value = state->vertexCount * 6 + state->polygonGroupCount * 0xc;
        value = value + state->displayListCount * 0xc;
        bufferAddress = mmAlloc(value, 5, 0);
        state->geometryBuffer = bufferAddress;
        streamSize = state->polygonGroupCount * 2;
        bufferAddress = bufferAddress + state->vertexCount * 6;
        state->polygonBuffer0 = bufferAddress;
        bufferAddress += streamSize;
        state->polygonBuffer1 = bufferAddress;
        bufferAddress += streamSize;
        state->posABuffer = bufferAddress;
        bufferAddress += streamSize;
        state->posBBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->polygonBuffer4 = bufferAddress;
        bufferAddress += streamSize;
        state->polygonBuffer5 = bufferAddress;
        bufferAddress += streamSize;
        streamSize = state->displayListCount * 2;
        state->minXBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->maxXBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->minYBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->maxYBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->minZBuffer = bufferAddress;
        bufferAddress += streamSize;
        state->maxZBuffer = bufferAddress;
        XyzAnimator_captureGeometry(placement, state, blockAddress);
        if (placement->mode != XYZ_ANIMATOR_MODE_DEFERRED_ONESHOT) {
            XyzAnimator_applyToMapBlock(placement, state, blockAddress);
            blockAddress->flags4 ^= 1;
            XyzAnimator_applyToMapBlock(placement, state, blockAddress);
            blockAddress->flags4 ^= 1;
        }
    }
    if (placement->mode == XYZ_ANIMATOR_MODE_GATED) {
        value = mainGetBit(placement->triggerGameBit);
        if (state->triggerBitValue != value) {
            state->triggerBitValue = value;
            if (value == 0) {
                if (placement->completionGameBit > -1) {
                    mainSetBits(placement->completionGameBit, 0);
                }
            }
            if (state->passCount > 2) {
                state->passCount = 0;
            }
        }
        if (state->passCount > 2) {
            return;
        }
        if (state->loopSfxId != 0) {
            Sfx_KeepAliveLoopedObjectSound(obj, state->loopSfxId);
        }
    } else {
        if (state->passCount > 2) {
            return;
        }
        if (state->triggerBitValue == 0) {
            state->triggerBitValue = mainGetBit(placement->triggerGameBit);
            if (state->triggerBitValue == 0) {
                return;
            }
        }
    }
    switch (placement->mode) {
    case XYZ_ANIMATOR_MODE_ONESHOT:
    case XYZ_ANIMATOR_MODE_DEFERRED_ONESHOT:
        completedAxes = 0;
        if (placement->startX > placement->targetX) {
            state->offsetX = -(0.1f * ((f32)(int)placement->speedX * timeDelta) - state->offsetX);
            if (state->offsetX <= (f32)placement->targetX) {
                state->offsetX = (f32)placement->targetX;
                completedAxes = 1;
            }
        } else {
            state->offsetX = 0.1f * ((f32)(int)placement->speedX * timeDelta) + state->offsetX;
            if (state->offsetX >= (f32)placement->targetX) {
                state->offsetX = (f32)placement->targetX;
                completedAxes = 1;
            }
        }
        if (placement->startY > placement->targetY) {
            state->offsetY = -(0.1f * ((f32)(int)placement->speedY * timeDelta) - state->offsetY);
            if (state->offsetY <= (f32)placement->targetY) {
                state->offsetY = (f32)placement->targetY;
                completedAxes += 1;
            }
        } else {
            state->offsetY = 0.1f * ((f32)(int)placement->speedY * timeDelta) + state->offsetY;
            if (state->offsetY >= (f32)placement->targetY) {
                state->offsetY = (f32)placement->targetY;
                completedAxes += 1;
            }
        }
        if (placement->startZ > placement->targetZ) {
            state->offsetZ = -(0.1f * ((f32)(int)placement->speedZ * timeDelta) - state->offsetZ);
            if (state->offsetZ <= (f32)placement->targetZ) {
                state->offsetZ = (f32)placement->targetZ;
                completedAxes += 1;
            }
        } else {
            state->offsetZ = 0.1f * ((f32)(int)placement->speedZ * timeDelta) + state->offsetZ;
            if (state->offsetZ >= (f32)placement->targetZ) {
                state->offsetZ = (f32)placement->targetZ;
                completedAxes += 1;
            }
        }
        if (completedAxes == 3) {
            if (placement->completionGameBit != -1) {
                mainSetBits(placement->completionGameBit, 1);
            }
            state->passCount += 1;
        }
        break;
    case XYZ_ANIMATOR_MODE_LOOP:
        if (placement->startX > placement->targetX) {
            state->offsetX = -(0.1f * ((f32)(int)placement->speedX * timeDelta) - state->offsetX);
            if (state->offsetX < (f32)placement->targetX) {
                state->offsetX = (f32)(placement->startX - (int)((f32)placement->targetX - state->offsetX));
            }
        } else {
            state->offsetX = 0.1f * ((f32)(int)placement->speedX * timeDelta) + state->offsetX;
            if (state->offsetX > (f32)placement->startX) {
                state->offsetX = (f32)(placement->targetX + (int)(state->offsetX - (f32)placement->targetX));
            }
        }
        if (placement->startY > placement->targetY) {
            state->offsetY = -(0.1f * ((f32)(int)placement->speedY * timeDelta) - state->offsetY);
            if (state->offsetY < (f32)placement->targetY) {
                state->offsetY =
                    -(0.1f * (f32)(int)((f32)placement->targetY - state->offsetY) - (f32)placement->startY);
            }
        } else {
            state->offsetY = 0.1f * ((f32)(int)placement->speedY * timeDelta) + state->offsetY;
            if (state->offsetY > (f32)placement->startY) {
                state->offsetY = (f32)(placement->targetY + (int)(state->offsetY - (f32)placement->targetY));
            }
        }
        if (placement->startZ > placement->targetZ) {
            state->offsetZ = -(0.1f * ((f32)(int)placement->speedZ * timeDelta) - state->offsetZ);
            if (state->offsetZ < (f32)placement->targetZ) {
                state->offsetZ = (f32)(placement->startZ - (int)((f32)placement->targetZ - state->offsetZ));
            }
        } else {
            state->offsetZ = 0.1f * ((f32)(int)placement->speedZ * timeDelta) + state->offsetZ;
            if (state->offsetZ > (f32)placement->startZ) {
                state->offsetZ = (f32)(placement->targetZ + (int)(state->offsetZ - (f32)placement->targetZ));
            }
        }
        break;
    case XYZ_ANIMATOR_MODE_GATED:
        completedAxes = 0;
        if (state->triggerBitValue != 0) {
            if (placement->startX > placement->targetX) {
                state->offsetX = -(0.1f * ((f32)(int)placement->speedX * timeDelta) - state->offsetX);
                if (state->offsetX <= (f32)placement->targetX) {
                    state->offsetX = (f32)placement->targetX;
                    completedAxes = 1;
                }
            } else {
                state->offsetX = 0.1f * ((f32)(int)placement->speedX * timeDelta) + state->offsetX;
                if (state->offsetX >= (f32)placement->targetX) {
                    state->offsetX = (f32)placement->targetX;
                    completedAxes = 1;
                }
            }
            if (placement->startY > placement->targetY) {
                state->offsetY = -(0.1f * ((f32)(int)placement->speedY * timeDelta) - state->offsetY);
                if (state->offsetY <= (f32)placement->targetY) {
                    state->offsetY = (f32)placement->targetY;
                    completedAxes += 1;
                }
            } else {
                state->offsetY = 0.1f * ((f32)(int)placement->speedY * timeDelta) + state->offsetY;
                if (state->offsetY >= (f32)placement->targetY) {
                    state->offsetY = (f32)placement->targetY;
                    completedAxes += 1;
                }
            }
            if (placement->startZ > placement->targetZ) {
                state->offsetZ = -(0.1f * ((f32)(int)placement->speedZ * timeDelta) - state->offsetZ);
                if (state->offsetZ <= (f32)placement->targetZ) {
                    state->offsetZ = (f32)placement->targetZ;
                    completedAxes += 1;
                }
            } else {
                state->offsetZ = 0.1f * ((f32)(int)placement->speedZ * timeDelta) + state->offsetZ;
                if (state->offsetZ >= (f32)placement->targetZ) {
                    state->offsetZ = (f32)placement->targetZ;
                    completedAxes += 1;
                }
            }
            if (completedAxes == 3) {
                if (placement->completionGameBit != -1) {
                    mainSetBits(placement->completionGameBit, 1);
                }
                state->passCount += 1;
            }
        } else {
            if (placement->startX > placement->targetX) {
                state->offsetX = 0.1f * ((f32)(int)placement->speedX * timeDelta) + state->offsetX;
                if (state->offsetX >= (f32)placement->startX) {
                    state->offsetX = (f32)placement->startX;
                    completedAxes = 1;
                }
            } else {
                state->offsetX = -(0.1f * ((f32)(int)placement->speedX * timeDelta) - state->offsetX);
                if (state->offsetX <= (f32)placement->startX) {
                    state->offsetX = (f32)placement->startX;
                    completedAxes = 1;
                }
            }
            if (placement->startY > placement->targetY) {
                state->offsetY = 0.1f * ((f32)(int)placement->speedY * timeDelta) + state->offsetY;
                if (state->offsetY >= (f32)placement->startY) {
                    state->offsetY = (f32)placement->startY;
                    completedAxes += 1;
                }
            } else {
                state->offsetY = -(0.1f * ((f32)(int)placement->speedY * timeDelta) - state->offsetY);
                if (state->offsetY <= (f32)placement->startY) {
                    state->offsetY = (f32)placement->startY;
                    completedAxes += 1;
                }
            }
            if (placement->startZ > placement->targetZ) {
                state->offsetZ = 0.1f * ((f32)(int)placement->speedZ * timeDelta) + state->offsetZ;
                if (state->offsetZ >= (f32)placement->startZ) {
                    state->offsetZ = (f32)placement->startZ;
                    completedAxes += 1;
                }
            } else {
                state->offsetZ = -(0.1f * ((f32)(int)placement->speedZ * timeDelta) - state->offsetZ);
                if (state->offsetZ <= (f32)placement->startZ) {
                    state->offsetZ = (f32)placement->startZ;
                    completedAxes += 1;
                }
            }
            if (completedAxes == 3) {
                state->passCount += 1;
            }
        }
        break;
    }
    XyzAnimator_applyToMapBlock(placement, state, blockAddress);
    return;
}

void XyzAnimator_init(GameObject* obj) {
    XyzAnimatorState* state = (XyzAnimatorState*)obj->extra;
    int mapId;

    objAddObjectType(obj, XYZ_ANIMATOR_OBJECT_GROUP);
    mapId = obj->anim.placement->ident;
    switch (mapId) {
    case XYZ_ANIMATOR_DRAGON_ROCK_FIREPIPE_IDENT:
    case 0x4BAB1:
        state->loopSfxId = 0x7D;
        break;
    case 0x49275:
    case 0x49CB7:
    case 0x4C797:
        state->loopSfxId = 0x4B7;
        break;
    }
}

ObjectDescriptor gXYZAnimatorObjDescriptor = {
    0,
    0,
    0,
    OBJECT_DESCRIPTOR_FLAGS_10_SLOTS,
    0,
    0,
    0,
    (ObjectDescriptorCallback)XyzAnimator_init,
    (ObjectDescriptorCallback)XyzAnimator_update,
    0,
    (ObjectDescriptorCallback)XyzAnimator_render,
    (ObjectDescriptorCallback)XyzAnimator_free,
    0,
    XyzAnimator_getExtraSize,
};
