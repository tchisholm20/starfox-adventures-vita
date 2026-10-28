// game_model_swap.c - byte-swap a decompressed ModelFileHeader from big-endian (disc)
// to host order, so the recompiled ObjModel_RelocateModelData (which reads the header's
// offset fields natively and adds the base pointer) produces valid pointers instead of
// wild ones. Uses the real struct so fields swap by name (no hand-counted offsets).
// Leaf geometry (vertices/DLs/joints) is NOT swapped here - that's the render follow-up;
// this makes the LOAD + relocation succeed. See port/byteswap.h, [[port-model]].

#include "main/model.h"
#include "port/byteswap.h"

void bswapModelFileHeader(void* p) {
    ModelFileHeader* h = (ModelFileHeader*)p;
    BE16(h->flags); BE16(h->modelId); BE32(h->dataSize);
    // relocated pointer/offset fields (RelocateModelData reads these as u32 + adds base)
    BE32(h->unk18); BE32(h->unk1C); BE32(h->textureIds);
    BE32(h->vertices); BE32(h->normals); BE32(h->colors); BE32(h->texCoords);
    BE32(h->renderOps); BE32(h->jointData); BE32(h->jointFuzzScales);
    BEF32(h->vertexFuzzScale.pivot[0]); BEF32(h->vertexFuzzScale.pivot[1]); BEF32(h->vertexFuzzScale.pivot[2]);
    BEF32(h->vertexFuzzScale.scaleDivisor);
    BE32(h->extraJointDefs); BE32(h->hitVolumes);
    BE32(h->collisionTriangles); BE32(h->collisionBlocks);
    BE32(h->moveData); BE32(h->animationDataSection); BE32(h->animationHeaderBuffer);
    for (int i = 0; i < 8; ++i) BE16(h->animGroupBaseIndices[i]);
    BE32(h->animationDataFileOffset); BE16(h->animationCacheSize);
    BE16(h->vertexAnimJob.chunkCount);
    BE32(h->vertexAnimJob.chunks); BE32(h->vertexAnimEntries); BE32(h->vertexWeightData);
    BE16(h->normalAnimJob.chunkCount);
    BE32(h->normalAnimJob.chunks); BE32(h->normalAnimEntries); BE32(h->normalWeightData);
    BE32(h->displayLists); BE32(h->instrs); BE16(h->instrsBitLenWords);
    BE32(h->morphTargetPtrs);
    BE16(h->cullDistance); BE16(h->shaderFlags);
    BE16(h->vertexCount); BE16(h->normalCount); BE16(h->colorCount); BE16(h->texCoordCount);
    BE16(h->animationCount); BE16(h->collisionBlockCount);
    // 0xF2..0xFB are u8 counts (textureCount/jointCount/...); no swap.
}

// Swap the leaf tables ObjModel_ResolveRenderOpTextures reads natively during load: the
// renderOps (Shader, 0x44 each) and the textureIds int array. Call AFTER the header swap
// but BEFORE relocation - the header's renderOps/textureIds fields are still byte OFFSETS.
static void swapShader(Shader* s) {
    BE32(s->reg1Texture); BE32(s->reg2Texture); BE32(s->textureId); BE32(s->unk1C);
    BE32(s->layers[0].textureIndex); BE32(s->layers[1].textureIndex);
    BE32(s->auxTextureIndex); BE32(s->indTextureId); BE32(s->flags);
}
void bswapModelRenderOps(void* m) {
    ModelFileHeader* h = (ModelFileHeader*)m;
    unsigned roOff = *(unsigned*)&h->renderOps;   // still an offset (pre-relocation)
    if (roOff) {
        Shader* ops = (Shader*)((unsigned char*)m + roOff);
        for (int j = 0; j < h->renderOpCount; ++j) swapShader(&ops[j]);
    }
    unsigned tidOff = *(unsigned*)&h->textureIds;
    if (tidOff) {
        int* tids = (int*)((unsigned char*)m + tidOff);
        for (int i = 0; i < h->textureCount; ++i) beFix32(&tids[i]);
    }
    // DL table entries: de+0 is a u32 offset (BE leaf) that RelocateModelData reads
    // natively and adds the base to - swap so it relocates to a valid pointer.
    // DL table entries: ModelDisplayListEntry {void* dlist@0, u16 dlistSize@4, pad}. de+0 is a u32
    // offset (BE leaf) that RelocateModelData reads natively and adds the base to - swap so it
    // relocates. dlistSize@4 is ALSO big-endian and MUST be swapped, or GXCallDisplayList replays
    // far past the intended list (0x14c0 read as 0xc014) into adjacent DLs of a different vertex
    // format - the misaligned decode that flung skinned verts to the camera.
    unsigned dlOff = *(unsigned*)&h->displayLists;
    if (dlOff) {
        unsigned char* dt = (unsigned char*)m + dlOff;
        int total = h->displayListCount + h->shadowDisplayListCount;
        for (int i = 0; i < total; ++i) { beFix32(dt + i * 0x1c); beFix16(dt + i * 0x1c + 4); }
    }
    // jointData contents: ModelBone[jointCount], stride 0x1c = {s8 parent@0, u8 idx[3]@1,
    // f32 head[3]@4, f32 tail[3]@0x10}. The head/tail floats are BE on disc; game code
    // (modelInitBones bind pose, modelCalcVtxGroupMtxs vertex-group blend) reads them natively,
    // so swap them to host order here. parent/idx are bytes (no swap).
    unsigned jdOff = *(unsigned*)&h->jointData;
    if (jdOff) {
        unsigned char* jd = (unsigned char*)m + jdOff;
        for (int i = 0; i < h->jointCount; ++i) {
            unsigned char* b = jd + i * 0x1c;
            for (int k = 0; k < 6; ++k) beFix32(b + 4 + k * 4);   // head[3] then tail[3]
        }
    }
    // unk18 / unk1C: f32[jointCount] each (object.c ~1400: unk18 = per-joint skeleton collision RADIUS, unk1C = per-joint
    // length scale), read natively into ModelJointWork.jointRadii / jointLengths. The header OFFSETS are swapped above
    // but not the tables: raw, a radius of 2.05 read as a denormal and 4.81 as -7e29, so every skeleton hit test ran on
    // garbage -- the wounded EarthWalker (shape 0x30) pushed the player out at the clamp (|v| 14) from ~93 units and the
    // player could never get inside its 52-unit talk range (Vita run 228, docs/47).
    // Host proof: host/tests/model_skeleton_swap_test.py (the head arm must fail).
    {
        unsigned o18 = *(unsigned*)&h->unk18, o1c = *(unsigned*)&h->unk1C;
        for (int k = 0; k < 2; ++k) {
            unsigned o = k ? o1c : o18;
            if (o) for (int i = 0; i < h->jointCount; ++i) beFix32((unsigned char*)m + o + i * 4);
        }
    }
    // Skinning chunks (ModelVtxAnimChunk, 0x74 each; model.h): vertexAnimJob.chunkCount at vertexAnimEntries
    // (positions) and normalAnimJob.chunkCount at normalAnimEntries (normals). ObjModel_RelocateAnimData and
    // ObjModel_Blend{Vertex,Normal}Stream read srcDataOffset s32 @0x60, weightStream u32 @0x64 and
    // vtxCount u16 @0x70 natively; the rest are bytes. Raw, weightStream relocated to a wild
    // pointer (Vita run 43: ObjModel_BlendVertexStream, r1 = 0xc5827ac0 -- the title's FrontFox).
    // unk00[0x60] is read by no consumer found; left raw.
    {
        unsigned off[2] = { *(unsigned*)&h->vertexAnimEntries, *(unsigned*)&h->normalAnimEntries };
        int cnt[2] = { h->vertexAnimJob.chunkCount, h->normalAnimJob.chunkCount };
        for (int k = 0; k < 2; ++k) if (off[k])
            for (int i = 0; i < cnt[k]; ++i) {
                unsigned char* c = (unsigned char*)m + off[k] + i * 0x74;
                beFix32(c + 0x60); beFix32(c + 0x64); beFix16(c + 0x70);
            }
    }
    // hitVolumes: ModelHitSphereDef[hitVolumeCount], 0x18 each (model.h) = {s16 jointIdx@0, f32 radius@4,
    // f32 center[3]@8, u16 linkedSpheres@0x14, s8 sphereIndex, s8 maskBit}. objUpdateHitSpheres
    // (model.c:1725) reads them natively. Raw, a BE float is a denormal (radius ~0, centre ~0) and a
    // jointIdx of 3 reads 768 -> falls back to joint 0: zero-size spheres at the root. 2026-10-11:
    // the ShipBattle figurehead (SB_ShipHead, hit state entirely model-driven) could not be damaged,
    // so the prologue never reached the landing.
    unsigned hvOff = *(unsigned*)&h->hitVolumes;
    if (hvOff) {
        unsigned char* hv = (unsigned char*)m + hvOff;
        for (int i = 0; i < h->hitVolumeCount; ++i) {
            unsigned char* b = hv + i * 0x18;
            beFix16(b); beFix32(b + 4); beFix32(b + 8); beFix32(b + 0xc); beFix32(b + 0x10); beFix16(b + 0x14);
        }
    }
    // Collision tables (header collisionBlocks/collisionTriangles, both still byte OFFSETS here):
    //   collisionBlocks    CollisionPolygonGroup[collisionBlockCount + 1], 0x14 each (collision_polygon.h):
    //                      u16 firstTri@0, s16 minX/maxX/minY/maxY/minZ/maxZ @2..0xC, u32 flags@0x10. The extra
    //                      LAST group is a terminator whose firstTri closes the final range (group[1].firstTri).
    //   collisionTriangles ModelCollisionTriangle[terminator.firstTri], 8 each: u16 vertexIndices[3]@0, bytes.
    // trackBuildModelTriangles (track_dolphin.c) reads all of it natively. Raw, the group count and firstTri
    // are byte-reversed, so no object-model collision triangle was ever built: the galleon deck (ShipBattle
    // model 155: 592 triangles, bbox x+-393) gave no floor and Krystal fell through it (host proof:
    // host/tests/model_collision_test.py). The VERTICES these index stay big-endian for the GX layer; the
    // collision reader byte-swaps each coordinate as it fetches it (STFX_VTX_S16, track_dolphin.c).
    // dataSize is the whole file length on all 14 collision models of the disc, so it bounds both tables.
    {
        unsigned cbOff = *(unsigned*)&h->collisionBlocks, ctOff = *(unsigned*)&h->collisionTriangles;
        unsigned nBlk = h->collisionBlockCount;
        unsigned limit = h->dataSize > 0 ? (unsigned)h->dataSize : 0u;
        if (cbOff && ctOff && nBlk && nBlk < 0x400 && limit && cbOff + (nBlk + 1) * 0x14 <= limit) {
            unsigned char* g = (unsigned char*)m + cbOff;
            for (unsigned i = 0; i <= nBlk; ++i) {
                unsigned char* b = g + i * 0x14;
                beFix16(b);
                for (int k = 0; k < 6; ++k) beFix16(b + 2 + k * 2);
                beFix32(b + 0x10);
            }
            unsigned nTri = *(unsigned short*)(g + nBlk * 0x14);   // terminator.firstTri, host order now
            if (ctOff + nTri * 8 <= limit) {
                unsigned char* t = (unsigned char*)m + ctOff;
                for (unsigned i = 0; i < nTri; ++i)
                    for (int k = 0; k < 3; ++k) beFix16(t + i * 8 + k * 2);
            }
        }
    }
    // morphTargetPtrs entries (u32 offsets, also relocated per-entry).
    unsigned mtOff = *(unsigned*)&h->morphTargetPtrs;
    if (mtOff) {
        unsigned char* mt = (unsigned char*)m + mtOff;
        for (int i = 0; i < h->morphTargetCount; ++i) beFix32(mt + i * 4);
    }
}
