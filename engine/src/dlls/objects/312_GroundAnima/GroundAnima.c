/* Lets Tricky dig a tagged patch of map geometry to uncover a buried collectible. */
#include "dlls/objects/312_GroundAnima.h"
#include "dlls/objects/237.h"
#include "dlls/objects/386_MMP_moonroc.h"
#include "MSL_C/PPCEABI/bare/H/math_float_helpers.h"
#include "dolphin/os/OSCache.h"
#include "main/audio/sfx_play_legacy_api.h"
#include "main/audio/sfx_trigger_ids.h"
#include "main/frame_timing.h"
#include "dlls/objects/196_Tricky.h"
#include "main/gamebits.h"
#include "main/lightmap_api.h"
#include "main/mm.h"
#include "main/object_render.h"
#include "main/objprint_render_api.h"
#include "main/objtype.h"
#include "main/shader_api.h"
#include "main/track_dolphin_api.h"
#include "sys/objects.h"
#include "sys/objects/lifecycle.h"
#include "main/gamebit_ids.h"

#define GROUND_ANIMATOR_OBJECT_GROUP     0x31
#define GROUND_ANIMATOR_SINK_DEPTH_SCALE 100.0f

static inline s16* GroundAnimator_getPackedVertex(MapBlockData* block, u16 vertexId) {
    return (s16*)block->vertices + vertexId * 3;
}

static inline MapTriIndex* GroundAnimator_getPolygon(MapBlockData* block, int polygonIndex) {
    return mapBlockGetPolygon(block, polygonIndex);
}

u16 gGroundAnimatorSfxIds[4] = {SFXTRIG_menuups16k, SFXTRIG_mpick1_b, 0, 0};

u8 GroundAnimator_getDigParticleVariant(GameObject* obj) {
    GroundAnimatorState* state = obj->extra;
    return state->digParticleVariant;
}

u8 GroundAnimator_isFullySunk(GameObject* obj) {
    GroundAnimatorState* state = obj->extra;
    GroundAnimatorPlacement* placement = (GroundAnimatorPlacement*)obj->anim.placementData;
    return state->sinkDepth > GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth;
}

#ifdef STAIRFAX_PORT
/* PORT INSTRUMENT (2026-10-14, run 274): Tricky's "Find" dig softlocks on the Vita (Tyler, SnowHorn: "moves back and then doesn't
 * uncover anything"). One [dig] line on each change of branch and every 60th press: which gate refused, with its numbers. */
extern int printf(const char*, ...);
static void digProbe(GameObject* obj, GroundAnimatorState* state, GroundAnimatorPlacement* placement, GameObject* sidekick, int why,
                     f32 dy, f32 d2, f32 range2) {
    static int sLastWhy = -9; static unsigned sN;
    ++sN;
    if (why == sLastWhy && (sN % 60) != 0) return;
    sLastWhy = why;
    printf("[dig] press #%u %s | obj (%.1f,%.1f,%.1f) tricky (%.1f,%.1f,%.1f) dy %.1f d2 %.0f range2 %.0f | sink %.1f of %d (max %u x100) falloff %.1f "
           "verts %d groups %u collectible %p flags 0x%02x\n",
           sN, why == 0 ? "OUT OF HEIGHT (|dy| > 20): returns 0" : why == 1 ? "OUT OF RANGE: returns -1" : "pressing",
           obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ, sidekick->anim.localPosX, sidekick->anim.localPosY,
           sidekick->anim.localPosZ, dy, d2, range2, state->sinkDepth, 100 * placement->maxSinkDepth, placement->maxSinkDepth,
           state->falloffRadius, state->animatedVertexCount, state->animatedGroupCount, (void*)state->collectible, state->flags);
}
#define DIG_PROBE(why, dy, d2, r2) digProbe(obj, state, placement, sidekick, (why), (dy), (d2), (r2))
#else
#define DIG_PROBE(why, dy, d2, r2) ((void)0)
#endif

/* Advances the dig and returns the radius Tricky should move to. */
f32 GroundAnimator_applyPress(GameObject* obj, GameObject* sidekick) {
    GroundAnimatorPlacement* placement;
    GroundAnimatorState* state;
    f32 dy;
    f32 dx;
    f32 dz;
    f32 rangeSquared;

    state = obj->extra;
    placement = (GroundAnimatorPlacement*)obj->anim.placementData;
    dy = sidekick->anim.localPosY - obj->anim.localPosY;
    if (dy < -20.0f || dy > 20.0f) {
        DIG_PROBE(0, dy, 0.0f, 0.0f);
        return 0.0f;
    }
    dx = sidekick->anim.localPosX - obj->anim.localPosX;
    dz = sidekick->anim.localPosZ - obj->anim.localPosZ;
    rangeSquared = 10.0f + state->falloffRadius;
    rangeSquared *= rangeSquared;
    if (dx * dx + dz * dz > rangeSquared) {
        DIG_PROBE(1, dy, dx * dx + dz * dz, rangeSquared);
        return -1.0f;
    }
    DIG_PROBE(2, dy, dx * dx + dz * dz, rangeSquared);
    if (state->sinkDepth >= GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth) {
        if (state->collectible != NULL) {
            GameObject* collectible;
            state->sinkDepth = GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth;
            collectible = state->collectible;
            switch (collectible->anim.romDefNo) {
            case MMP_MOON_ROCK_SEQUENCE_ID:
                mmpMoonRock_setFrozen(collectible, 0);
                break;
            default:
                COLLECTIBLE_INTERFACE(collectible)->setDisabled(collectible, 0);
                break;
            }
        }
    }
    state->sinkDepth += 5.0f * timeDelta;
    state->flags |= GROUND_ANIMATOR_STATE_PRESSED;
    return state->falloffRadius * (state->sinkDepth / (GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth));
}

/* Caches the influence and original height of each vertex in matching polygon groups. */
static void GroundAnimator_gatherVertices(GameObject* obj, GroundAnimatorState* state,
                                          GroundAnimatorPlacement* placement) {
    MapTriIndex* polygon;
    int animatedVertexIndex;
    int mapCellX;
    int mapCellZ;
    int polygonGroupIndex;
    int polygonIndex;
    int vertexIndex;
    MapBlockData* block =
        mapGetBlock(objPosToMapBlockIdx(obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ));
    f32 blockLocalZ;
    f32 radiusSquared;
    f32 blockLocalX;

    if (block == NULL || (block->flags4 & MAP_BLOCK_FLAG_LOADED) == 0) {
        return;
    }
    mapCellX = fastFloorf((obj->anim.localPosX - playerMapOffsetX) / 640.0f);
    mapCellZ = fastFloorf((obj->anim.localPosZ - playerMapOffsetZ) / 640.0f);
    blockLocalX = obj->anim.localPosX - (640.0f * mapCellX + playerMapOffsetX);
    blockLocalZ = obj->anim.localPosZ - (640.0f * mapCellZ + playerMapOffsetZ);
    animatedVertexIndex = 0;
    state->animatedGroupCount = 0;
    radiusSquared = state->falloffRadius * state->falloffRadius;
    for (polygonGroupIndex = 0; polygonGroupIndex < block->polyGroupCount; polygonGroupIndex++) {
        CollisionPolygonGroup* polygonGroup = mapBlockGetPolygonGroup(block, polygonGroupIndex);

        if (placement->animatorId == mapBlockGetPolygonGroupType(polygonGroup)) {
            for (polygonIndex = polygonGroup->firstTri; polygonIndex < polygonGroup[1].firstTri; polygonIndex++) {
                polygon = GroundAnimator_getPolygon(block, polygonIndex);
                for (vertexIndex = 0; vertexIndex < ARRAY_COUNT(polygon->vert); vertexIndex++) {
                    f32 vertexPosition[3];
                    f32 dx;
                    f32 dz;
                    f32 normalizedDistance;
                    s16* packedVertex = GroundAnimator_getPackedVertex(block, polygon->vert[vertexIndex]);

                    trackUnpackVector(packedVertex, vertexPosition);
                    dx = vertexPosition[0] - blockLocalX;
                    dz = vertexPosition[2] - blockLocalZ;
                    normalizedDistance = (dx * dx + dz * dz) / radiusSquared;
                    if (normalizedDistance > 1.0f) {
                        normalizedDistance = 1.0f;
                    }
                    normalizedDistance *= normalizedDistance;
                    state->vertexWeights[animatedVertexIndex] = 1.0f - normalizedDistance;
                    state->baseVertexHeights[animatedVertexIndex] = vertexPosition[1];
                    animatedVertexIndex++;
                }
            }
            state->animatedGroupIndices[state->animatedGroupCount++] = polygonGroupIndex;
        }
    }
}

int GroundAnimator_getExtraSize(void) {
    return sizeof(GroundAnimatorState);
}

void GroundAnimator_free(GameObject* obj, int onlySelf) {
    int animatedVertexIndex;
    int polygonGroupIndex;
    int polygonIndex;
    int vertexIndex;
    MapBlockData* block;
    GroundAnimatorState* state = obj->extra;
    GroundAnimatorPlacement* placement = (GroundAnimatorPlacement*)obj->anim.placementData;

    if (onlySelf == 0) {
        block = mapGetBlock(objPosToMapBlockIdx(obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ));
        if (block != NULL) {
            animatedVertexIndex = 0;
            for (polygonGroupIndex = 0; polygonGroupIndex < block->polyGroupCount; polygonGroupIndex++) {
                CollisionPolygonGroup* polygonGroup = mapBlockGetPolygonGroup(block, polygonGroupIndex);

                if (placement->animatorId == mapBlockGetPolygonGroupType(polygonGroup)) {
                    for (polygonIndex = polygonGroup->firstTri; polygonIndex < polygonGroup[1].firstTri;
                         polygonIndex++) {
                        MapTriIndex* polygon = GroundAnimator_getPolygon(block, polygonIndex);

                        for (vertexIndex = 0; vertexIndex < ARRAY_COUNT(polygon->vert); vertexIndex++) {
                            f32 vertexPosition[3];
                            s16* packedVertex = GroundAnimator_getPackedVertex(block, polygon->vert[vertexIndex]);

                            trackUnpackVector(packedVertex, vertexPosition);
                            if (state->baseVertexHeights != NULL) {
                                vertexPosition[1] = state->baseVertexHeights[animatedVertexIndex];
                                trackPackVector(packedVertex, vertexPosition);
                            }
                            animatedVertexIndex++;
                        }
                    }
                }
            }
        }
    }
    if (state->vertexWeights != NULL) {
        mm_free(state->vertexWeights);
    }
    objFreeObjectType(obj, GROUND_ANIMATOR_OBJECT_GROUP);
}

void GroundAnimator_render(GameObject* obj, int gdl, int mtxs, int vtxs, int pols, s8 visibility) {
    if (visibility == 0) {
        return;
    }
    objRenderModelAndHitVolumes(obj, gdl, mtxs, vtxs, pols, 1.0f);
}

void GroundAnimator_update(GameObject* obj) {
    GameObject* player = Obj_GetPlayerObject();
    int animatedVertexIndex;
    MapBlockData* block;
    u8 wasOnMap;
    int animatedGroupIndex;
    u8 findCommandEnabled;
    int polygonIndex;
    int vertexIndex;
    GroundAnimatorState* state = obj->extra;
    GroundAnimatorPlacement* placement = (GroundAnimatorPlacement*)obj->anim.placementData;
    s8 blockIndex;

    if (placement->animatorId == 0) {
        return;
    }

    /* Track when the containing map block becomes available. */
    blockIndex = objPosToMapBlockIdx(obj->anim.localPosX, obj->anim.localPosY, obj->anim.localPosZ);
    wasOnMap = state->flags & GROUND_ANIMATOR_STATE_ON_MAP;
    if (blockIndex > -1) {
        state->flags |= GROUND_ANIMATOR_STATE_ON_MAP;
    } else {
        state->flags &= ~GROUND_ANIMATOR_STATE_ON_MAP;
    }
    if ((state->flags & GROUND_ANIMATOR_STATE_ON_MAP) != wasOnMap) {
        state->vertexUpdateFrames = 2;
    }
    if ((state->flags & GROUND_ANIMATOR_STATE_ON_MAP) == 0) {
        return;
    }

    /* Capture the affected vertices the first time the block is available. */
    if ((state->flags & GROUND_ANIMATOR_STATE_ON_MAP) != 0 && state->vertexWeights == NULL) {
        state->animatedVertexCount = mapBlockCountTrianglesByType(mapGetBlock(blockIndex), placement->animatorId) * 3;
        if (state->animatedVertexCount > 0) {
            f32* buffer = mmAlloc(
                state->animatedVertexCount * (sizeof(*state->vertexWeights) + sizeof(*state->baseVertexHeights)), 5, 0);

            state->vertexWeights = buffer;
            state->baseVertexHeights = (s16*)(buffer + state->animatedVertexCount);
            GroundAnimator_gatherVertices(obj, state, placement);
        }
    }
    if (state->animatedVertexCount == 0) {
        return;
    }

    /* Find and position the collectible buried beneath the dig spot. */
    if (placement->disableAutoLink == 0) {
        if (state->collectible == NULL) {
            GameObject* collectible;
            f32 searchDistance = 100.0f;

            state->collectible = objGetNearestTypeTo(COLLECTIBLE_OBJECT_GROUP, obj, &searchDistance);
            collectible = state->collectible;
            if (collectible != NULL) {
                switch (state->collectible->anim.romDefNo) {
                case MMP_MOON_ROCK_SEQUENCE_ID:
                    if ((state->flags & GROUND_ANIMATOR_STATE_COMPLETE) == 0) {
                        mmpMoonRock_setFrozen(collectible, 1);
                    }
                    mmpMoonRock_setPosition(collectible, obj->anim.localPosX,
                                            obj->anim.localPosY - state->collectibleDepth, obj->anim.localPosZ);
                    break;
                default:
                    if ((state->flags & GROUND_ANIMATOR_STATE_COMPLETE) == 0) {
                        COLLECTIBLE_INTERFACE(collectible)->setDisabled(collectible, 1);
                    }
                    COLLECTIBLE_INTERFACE(collectible)
                        ->setPosition(collectible, obj->anim.localPosX, obj->anim.localPosY - state->collectibleDepth,
                                      obj->anim.localPosZ);
                    break;
                }
            }
        } else if ((state->collectible->objectFlags & OBJECT_OBJFLAG_FREED) != 0) {
            state->collectible = NULL;
        }
#ifdef STAIRFAX_PORT
        /* PORT INSTRUMENT (2026-10-14, run 275): run 274 showed the dig completing with collectible NULL -- nothing in group 4 within
         * 100 units. Once at first sight and every 1200 frames while it stays NULL: the group-4 census and every object within 300. */
        if (state->collectible == NULL) {
            extern int printf(const char*, ...);
            extern GameObject** ObjList_GetObjects(int* startIndex, int* objectCount);
            extern float sqrtf(float);
            static unsigned sTick;
            if ((sTick++ % 1200) == 0) {
                int n4 = 0, start = 0, count = 0, i;
                GameObject** g4 = objGetAllOfType(COLLECTIBLE_OBJECT_GROUP, &n4);
                GameObject** all = ObjList_GetObjects(&start, &count);
                f32 best = 3.4e38f; GameObject* bestObj = NULL;
                for (i = 0; g4 && i < n4; ++i) {
                    f32 d = vec3f_distanceSquared(&obj->anim.worldPosX, &g4[i]->anim.worldPosX);
                    if (d < best) { best = d; bestObj = g4[i]; }
                }
                printf("[dig] spot %p (%.1f,%.1f,%.1f) animatorId %u sunkBit %d enableBit %d autoLink %u: group-4 collectibles %d, nearest %p romDef 0x%x at %.1f\n",
                       (void*)obj, obj->anim.worldPosX, obj->anim.worldPosY, obj->anim.worldPosZ, placement->animatorId,
                       placement->sunkGameBit, placement->enableGameBit, placement->disableAutoLink, n4, (void*)bestObj,
                       bestObj ? bestObj->anim.romDefNo : -1, bestObj ? sqrtf(best) : -1.0f);
                for (i = start; all && i < count; ++i) {
                    GameObject* o = all[i];
                    f32 d;
                    if (o == NULL || o == obj) continue;
                    d = vec3f_distanceSquared(&obj->anim.worldPosX, &o->anim.worldPosX);
                    if (d < 300.0f * 300.0f)
                        printf("[dig]   near: %p romDef 0x%x at %.1f (%.1f,%.1f,%.1f) objFlags 0x%x\n", (void*)o, o->anim.romDefNo,
                               sqrtf(d), o->anim.worldPosX, o->anim.worldPosY, o->anim.worldPosZ, (unsigned)o->objectFlags);
                }
            }
        }
#endif
    }
    block = mapGetBlock(blockIndex);
    if (block == NULL || (block->flags4 & MAP_BLOCK_FLAG_LOADED) == 0) {
        return;
    }

    /* Update the ground deformation whenever the dig depth changes. */
    if (state->sinkDepth > 0.0f) {
        if ((state->flags & GROUND_ANIMATOR_STATE_PRESSED) != 0) {
            state->flags &= ~GROUND_ANIMATOR_STATE_PRESSED;
        } else if (state->sinkDepth < GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth) {
            state->sinkDepth -= timeDelta;
            if (state->sinkDepth < 0.0f) {
                state->sinkDepth = 0.0f;
            }
        }
        if (state->sinkDepth != state->previousSinkDepth) {
            state->vertexUpdateFrames = 2;
            state->previousSinkDepth = state->sinkDepth;
        }
        if (state->vertexUpdateFrames != 0) {
            f32 maxSinkDepth;
            state->vertexUpdateFrames--;
            if (state->previousSinkDepth >
                (maxSinkDepth = GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth)) {
                state->previousSinkDepth = maxSinkDepth;
                state->sinkDepth = maxSinkDepth;
                if (state->collectible != NULL && state->collectible->extra != NULL) {
                    switch (state->collectible->anim.romDefNo) {
                    case MMP_MOON_ROCK_SEQUENCE_ID:
                        mmpMoonRock_setFrozen(state->collectible, 0);
                        break;
                    default:
                        COLLECTIBLE_INTERFACE(state->collectible)->setDisabled(state->collectible, 0);
                        break;
                    }
                }
                mainSetBits(placement->sunkGameBit, 1);
                state->flags |= GROUND_ANIMATOR_STATE_COMPLETE;
                Sfx_PlayFromObject(obj, gGroundAnimatorSfxIds[placement->sfxIndex]);
            }
            for (animatedGroupIndex = 0, animatedVertexIndex = 0; animatedGroupIndex < state->animatedGroupCount;
                 animatedGroupIndex++) {
                CollisionPolygonGroup* polygonGroup =
                    mapBlockGetPolygonGroup(block, state->animatedGroupIndices[animatedGroupIndex]);

                for (polygonIndex = polygonGroup->firstTri; polygonIndex < polygonGroup[1].firstTri; polygonIndex++) {
                    MapTriIndex* polygon = GroundAnimator_getPolygon(block, polygonIndex);

                    for (vertexIndex = 0; vertexIndex < ARRAY_COUNT(polygon->vert); vertexIndex++) {
                        if (state->vertexWeights[animatedVertexIndex] > 0.0f) {
                            f32 vertexPosition[3];
                            f32 sinkOffset;
                            s16* packedVertex = GroundAnimator_getPackedVertex(block, polygon->vert[vertexIndex]);

                            trackUnpackVector(packedVertex, vertexPosition);
                            sinkOffset = state->previousSinkDepth / GROUND_ANIMATOR_SINK_DEPTH_SCALE *
                                         state->vertexWeights[animatedVertexIndex];
                            vertexPosition[1] = state->baseVertexHeights[animatedVertexIndex] - sinkOffset;
                            trackPackVector(packedVertex, vertexPosition);
                        }
                        animatedVertexIndex++;
                    }
                }
            }
            DCStoreRangeNoSync(block->vertices, block->vertexCount * sizeof(s16[3]));
#ifdef STAIRFAX_PORT
            {   /* PORT FIX (run 276): the display-list cache trusts arrays that matched 8 calls in a row and re-reads them only on an
                 * EVENT (gx_draw.c "VALIDATION BY EVENTS"); a plain CPU store into a map block is not one, so the sunk ground could stay
                 * drawn flat. A write event makes this block's vertex buffer dynamic: every record reading it hashes its arrays. */
                extern void gx_dlk_event(const void* p, size_t n, int isWrite) __attribute__((weak));
                if (gx_dlk_event) gx_dlk_event(block->vertices, (size_t)block->vertexCount * sizeof(s16[3]), 1);
            }
#endif
        }
    }

    /* Offer Tricky's Find command while the dig spot is active. */
    if (placement->enableGameBit == -1 || mainGetBit(placement->enableGameBit) != 0) {
        findCommandEnabled = 1;
    } else {
        findCommandEnabled = 0;
    }
    if ((state->flags & GROUND_ANIMATOR_STATE_COMPLETE) == 0 && findCommandEnabled != 0) {
        GameObject* tricky = getTrickyObject();

        if (tricky != NULL && mainGetBit(GAMEBIT_Tricky_Unlocked_Sidekick_Commands) != 0) {
            obj->anim.resetHitboxFlags &= ~INTERACT_FLAG_PROMPT_SUPPRESSED;
        } else {
            obj->anim.resetHitboxFlags |= INTERACT_FLAG_PROMPT_SUPPRESSED;
        }
        obj->anim.resetHitboxFlags &= ~INTERACT_FLAG_DISABLED;
        if (tricky != NULL && (obj->anim.resetHitboxFlags & INTERACT_FLAG_IN_RANGE) != 0) {
            TRICKY_INTERFACE(tricky)->sideCommandEnable(tricky, obj, TRICKY_COMMAND_KIND_PRIORITY,
                                                        TRICKY_COMMAND_TYPE_FIND_SECRET);
        }
    } else {
        obj->anim.resetHitboxFlags |= INTERACT_FLAG_DISABLED;
    }
    objUpdateHitVolumeTransforms(obj);
}

void GroundAnimator_init(GameObject* obj, GroundAnimatorPlacement* placement) {
    GroundAnimatorState* state = obj->extra;
    state->digParticleVariant = placement->digParticleVariant;
    state->collectibleDepth = placement->collectibleDepth;
    state->previousSinkDepth = -1.0f;
    state->falloffRadius = placement->falloffRadius;
    if (placement->animatorId == 0) {
        return;
    }
    if (mainGetBit(placement->sunkGameBit) != 0) {
        state->sinkDepth = GROUND_ANIMATOR_SINK_DEPTH_SCALE * placement->maxSinkDepth;
        state->flags |= GROUND_ANIMATOR_STATE_COMPLETE;
    }
    objAddObjectType(obj, GROUND_ANIMATOR_OBJECT_GROUP);
    if (placement->sfxIndex > 1) {
        placement->sfxIndex = 0;
    }
}

ObjectDescriptor14 gGroundAnimatorObjDescriptor = {
    0,
    0,
    0,
    OBJECT_DESCRIPTOR_FLAGS_13_SLOTS,
    0,
    0,
    0,
    (ObjectDescriptorCallback)GroundAnimator_init,
    (ObjectDescriptorCallback)GroundAnimator_update,
    0,
    (ObjectDescriptorCallback)GroundAnimator_render,
    (ObjectDescriptorCallback)GroundAnimator_free,
    0,
    GroundAnimator_getExtraSize,
    (ObjectDescriptorCallback)GroundAnimator_applyPress,
    (ObjectDescriptorCallback)GroundAnimator_isFullySunk,
    (ObjectDescriptorCallback)GroundAnimator_getDigParticleVariant,
    0,
};
