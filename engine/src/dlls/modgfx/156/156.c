/*
 * DLL 156 / 0x9C - a variant-driven layered modgfx effect spawner.
 */
#include "main/dll/dll_009C_modgfx.h"
#include "main/be_data.h"
#include "main/dll/modgfx_interface.h"
#include "main/dll/modgfx_types.h"

typedef struct Dll9CEffectResourceView {
    ModgfxEffectVertex vertices[21];
    u8 opaqueD2[2];
    s16 triangles[24][3];
    u8 opaque164[0x4C];
    s16 allVertexIndices[21];
    s16 opaque1DA;
    s16 lastFourteenVertexIndices[14];
    s16 sequenceParams[2][7];
} Dll9CEffectResourceView;

STATIC_ASSERT(offsetof(Dll9CEffectResourceView, vertices) == 0x000);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, opaqueD2) == 0x0D2);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, triangles) == 0x0D4);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, opaque164) == 0x164);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, allVertexIndices) == 0x1B0);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, opaque1DA) == 0x1DA);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, lastFourteenVertexIndices) == 0x1DC);
STATIC_ASSERT(offsetof(Dll9CEffectResourceView, sequenceParams) == 0x1F8);
STATIC_ASSERT(sizeof(Dll9CEffectResourceView) == 0x214);

extern u32 gDll9CEffectResourceData[sizeof(Dll9CEffectResourceView) / sizeof(u32)];

void dll_9C_spawnEffect(GameObject* sourceObj, int variant, PartFxSpawnParams* spawnParams, u32 spawnFlags) {
    ModgfxSpawnPacket packet;
    GfxCmd* commandCursor;
    u8* resourceData = (u8*)(int)gDll9CEffectResourceData;
    GfxCmd* commands = packet.entries;
    int idx;
    u8* q;

    commandCursor = commands;
    commandCursor[0].layer = 0;
    commandCursor[0].flags = 0x15;
    commandCursor[0].tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
    commandCursor[0].mode = 4;
    commandCursor[0].x = 0.0f;
    commandCursor[0].y = 0.0f;
    commandCursor[0].z = 0.0f;
    commandCursor[1].layer = 0;
    commandCursor[1].flags = 0x15;
    commandCursor[1].tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
    commandCursor[1].mode = 2;
    commandCursor[1].x = 0.01f;
    commandCursor[1].y = 2.0f;
    commandCursor[1].z = 0.01f;
    commandCursor += 2;
    if (variant != 1) {
        commandCursor->layer = 0;
        commandCursor->flags = 0;
        commandCursor->tex = NULL;
        commandCursor->mode = 0x400000;
        commandCursor->x = 0.0f;
        commandCursor->y = 0.0f;
        commandCursor->z = 0.0f;
        commandCursor++;
    }
    if (variant == 1) {
        commandCursor->layer = 0;
        commandCursor->flags = 0;
        commandCursor->tex = NULL;
        commandCursor->mode = 0x80;
        commandCursor->x = (f32)spawnParams->arg2;
        commandCursor->y = (f32)spawnParams->arg1;
        commandCursor->z = (f32)spawnParams->arg0;
        commandCursor++;
    }
    if (variant == 1) {
        commandCursor->layer = 1;
        commandCursor->flags = 0x15;
        commandCursor->tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
        commandCursor->mode = 2;
        commandCursor->x = 30.0f;
        commandCursor->y = spawnParams->posY / 30.0f;
        commandCursor->z = 30.0f;
    } else {
        commandCursor->layer = 1;
        commandCursor->flags = 0x15;
        commandCursor->tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
        commandCursor->mode = 2;
        commandCursor->x = 30.0f;
        commandCursor->y = 1.2f;
        commandCursor->z = 30.0f;
    }
    commandCursor[1].layer = 1;
    commandCursor[1].flags = 0xe;
    commandCursor[1].tex = &resourceData[offsetof(Dll9CEffectResourceView, lastFourteenVertexIndices)];
    commandCursor[1].mode = 4;
    commandCursor[1].x = 155.0f;
    commandCursor[1].y = 0.0f;
    commandCursor[1].z = 0.0f;
    commandCursor[2].layer = 1;
    commandCursor[2].flags = 0x15;
    commandCursor[2].tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
    commandCursor[2].mode = 0x4000;
    commandCursor[2].x = 2.0f;
    commandCursor[2].y = -4.0f;
    commandCursor[2].z = 0.0f;
    commandCursor += 3;
    if (variant != 1) {
        commandCursor->layer = 1;
        commandCursor->flags = 0;
        commandCursor->tex = NULL;
        commandCursor->mode = 0x100;
        commandCursor->x = 0.0f;
        commandCursor->y = 0.0f;
        commandCursor->z = -150.0f;
        commandCursor++;
    }
    commandCursor[0].layer = 2;
    commandCursor[0].flags = 0x15;
    commandCursor[0].tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
    commandCursor[0].mode = 0x4000;
    commandCursor[0].x = 2.0f;
    commandCursor[0].y = -4.0f;
    commandCursor[0].z = 0.0f;
    commandCursor[1].layer = 3;
    commandCursor[1].flags = 0x15;
    commandCursor[1].tex = &resourceData[offsetof(Dll9CEffectResourceView, allVertexIndices)];
    commandCursor[1].mode = 0x4000;
    commandCursor[1].x = 2.0f;
    commandCursor[1].y = -4.0f;
    commandCursor[1].z = 0.0f;
    commandCursor[2].layer = 3;
    commandCursor[2].flags = 0xe;
    commandCursor[2].tex = &resourceData[offsetof(Dll9CEffectResourceView, lastFourteenVertexIndices)];
    commandCursor[2].mode = 4;
    commandCursor[2].x = 0.0f;
    commandCursor[2].y = 0.0f;
    commandCursor[2].z = 0.0f;
    commandCursor[3].layer = 1;

    packet.modeByte = 0;
    packet.sourceObj = sourceObj;
    packet.sourceMode = variant;
    packet.position[0] = 0.0f;
    packet.position[1] = 0.0f;
    packet.position[2] = 0.0f;
    packet.velocity[0] = 0.0f;
    packet.velocity[1] = 0.0f;
    packet.velocity[2] = 0.0f;
    packet.scale = 4.0f;
    packet.drawGroupCount = 2;
    packet.drawGroupStride = 7;
    packet.initialStateByte = 0xe;
    packet.byte5A = 0;
    packet.textureFrameTimer = 0x1e;
    packet.commandCount = (s8)(((u8*)(commandCursor + 3) - (u8*)commands) / (int)sizeof(GfxCmd));
    idx = variant * 7;
    q = resourceData + idx * 2;
    packet.sequenceParams[0] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 1) * 2;
    packet.sequenceParams[1] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 2) * 2;
    packet.sequenceParams[2] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 3) * 2;
    packet.sequenceParams[3] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 4) * 2;
    packet.sequenceParams[4] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 5) * 2;
    packet.sequenceParams[5] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    q = resourceData + (idx + 6) * 2;
    packet.sequenceParams[6] = *(s16*)&q[offsetof(Dll9CEffectResourceView, sequenceParams)];
    packet.commands = (GfxCmd*)((u8*)&packet + offsetof(ModgfxSpawnPacket, entries));
    packet.flags = 0xc010480;
    packet.flags |= spawnFlags;
    if ((packet.flags & 1) != 0) {
        if ((void*)sourceObj != NULL) {
            packet.position[0] += sourceObj->anim.worldPosX;
            packet.position[1] += sourceObj->anim.worldPosY;
            packet.position[2] += sourceObj->anim.worldPosZ;
        } else {
            packet.position[0] += spawnParams->posX;
            packet.position[1] += spawnParams->posY;
            packet.position[2] += spawnParams->posZ;
        }
    }
    (*gModgfxInterface)
        ->spawnEffect(&packet, 0, 0x15, (u8*)(int)gDll9CEffectResourceData, 0x18,
                      &resourceData[offsetof(Dll9CEffectResourceView, triangles)], 0x154, 0);
}

void dll_9C_release(void) {
}

void dll_9C_initialise(void) {
}

/* STFX endian: gDll9CEffectResourceData is the decomp's PowerPC image of s16 data (vertices, triangle and vertex-INDEX lists, sequence params) written as u32 words, so each word holds two big-endian s16; on ARM the halves read in the wrong order (index lists like {0,1,2,3} became {1,0,3,2}). STFX_HW16 exchanges the halves. */
u32 gDll9CEffectResourceData[sizeof(Dll9CEffectResourceView) / sizeof(u32)] = {
    STFX_HW16(0x00000000), STFX_HW16(0x03e80000), STFX_HW16(0x00000362), STFX_HW16(0x000001f4), STFX_HW16(0x000b0000), STFX_HW16(0x03620000), STFX_HW16(0xfe0c0016), STFX_HW16(0x00000000), STFX_HW16(0x0000fc18),
    STFX_HW16(0x00200000), STFX_HW16(0xfc9e0000), STFX_HW16(0xfe0c0016), STFX_HW16(0x0000fc9e), STFX_HW16(0x000001f4), STFX_HW16(0x000b0000), STFX_HW16(0x00000000), STFX_HW16(0x03e80000), STFX_HW16(0x00000000),
    STFX_HW16(0x01f403e8), STFX_HW16(0x0000000f), STFX_HW16(0x036201f4), STFX_HW16(0x01f4000b), STFX_HW16(0x000f0362), STFX_HW16(0x01f4fe0c), STFX_HW16(0x0016000f), STFX_HW16(0x000001f4), STFX_HW16(0xfc180020),
    STFX_HW16(0x000ffc9e), STFX_HW16(0x01f4fe0c), STFX_HW16(0x0016000f), STFX_HW16(0xfc9e01f4), STFX_HW16(0x01f4000b), STFX_HW16(0x000f0000), STFX_HW16(0x01f403e8), STFX_HW16(0x0000000f), STFX_HW16(0x00001770),
    STFX_HW16(0x03e80000), STFX_HW16(0x007f0362), STFX_HW16(0x177001f4), STFX_HW16(0x000b007f), STFX_HW16(0x03621770), STFX_HW16(0xfe0c0016), STFX_HW16(0x007f0000), STFX_HW16(0x1770fc18), STFX_HW16(0x0020007f),
    STFX_HW16(0xfc9e1770), STFX_HW16(0xfe0c0016), STFX_HW16(0x007ffc9e), STFX_HW16(0x177001f4), STFX_HW16(0x000b007f), STFX_HW16(0x00001770), STFX_HW16(0x03e80000), STFX_HW16(0x007f0000), STFX_HW16(0x00000001),
    STFX_HW16(0x00080000), STFX_HW16(0x00080007), STFX_HW16(0x00010002), STFX_HW16(0x00090001), STFX_HW16(0x00090008), STFX_HW16(0x00020003), STFX_HW16(0x000a0002), STFX_HW16(0x000a0009), STFX_HW16(0x00030004),
    STFX_HW16(0x000b0003), STFX_HW16(0x000b000a), STFX_HW16(0x00040005), STFX_HW16(0x000c0004), STFX_HW16(0x000c000b), STFX_HW16(0x00050006), STFX_HW16(0x000d0005), STFX_HW16(0x000d000c), STFX_HW16(0x00070008),
    STFX_HW16(0x000f0007), STFX_HW16(0x000f000e), STFX_HW16(0x00080009), STFX_HW16(0x00100008), STFX_HW16(0x0010000f), STFX_HW16(0x0009000a), STFX_HW16(0x00110009), STFX_HW16(0x00110010), STFX_HW16(0x000a000b),
    STFX_HW16(0x0012000a), STFX_HW16(0x00120011), STFX_HW16(0x000b000c), STFX_HW16(0x0013000b), STFX_HW16(0x00130012), STFX_HW16(0x000c000d), STFX_HW16(0x0014000c), STFX_HW16(0x00140013), STFX_HW16(0x00000001),
    STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x00060000), STFX_HW16(0x00070008), STFX_HW16(0x0009000a), STFX_HW16(0x000b000c), STFX_HW16(0x000d0000), STFX_HW16(0x000e000f), STFX_HW16(0x00100011),
    STFX_HW16(0x00120013), STFX_HW16(0x00140000), STFX_HW16(0x00000001), STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x0006000e), STFX_HW16(0x000f0010), STFX_HW16(0x00110012), STFX_HW16(0x00130014),
    STFX_HW16(0x00000001), STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x00060007), STFX_HW16(0x00080009), STFX_HW16(0x000a000b), STFX_HW16(0x000c000d), STFX_HW16(0x000e000f), STFX_HW16(0x00100011),
    STFX_HW16(0x00120013), STFX_HW16(0x00140000), STFX_HW16(0x00070008), STFX_HW16(0x0009000a), STFX_HW16(0x000b000c), STFX_HW16(0x000d000e), STFX_HW16(0x000f0010), STFX_HW16(0x00110012), STFX_HW16(0x00130014),
    STFX_HW16(0x00000096), STFX_HW16(0x044c0032), STFX_HW16(0x00000000), STFX_HW16(0x00000000), STFX_HW16(0x0028000a), STFX_HW16(0x00140001), STFX_HW16(0x00000000)};
Dll9CResourceDescriptor gDll9CResourceDescriptor = {
    {0x00000000, 0x00000000, 0x00000000, 0x00030000},
    dll_9C_initialise,
    dll_9C_release,
    NULL,
    dll_9C_spawnEffect,
    0x00000000,
};
