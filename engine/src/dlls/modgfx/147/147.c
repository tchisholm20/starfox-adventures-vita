/*
 * DLL 147 / 0x93 - a randomized six-command layered modgfx effect spawner.
 */
#include "main/dll/dll_0093_modgfx.h"
#include "main/be_data.h"
#include "main/dll/modgfx_interface.h"
#include "main/dll/modgfx_types.h"
#include "main/vecmath.h"

typedef struct Dll93EffectResourceView {
    ModgfxEffectVertex vertices[21];
    u8 padD2[2];
    s16 triangles[24][3];
    u8 opaque164[0x4C];
    s16 allVertexIndices[21];
    s16 opaque1DA;
    u8 opaque1DC[0x1C];
    s16 sequenceParams[7];
    s16 opaqueTail;
} Dll93EffectResourceView;

STATIC_ASSERT(offsetof(Dll93EffectResourceView, vertices) == 0x000);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, padD2) == 0x0D2);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, triangles) == 0x0D4);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, opaque164) == 0x164);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, allVertexIndices) == 0x1B0);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, opaque1DA) == 0x1DA);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, opaque1DC) == 0x1DC);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, sequenceParams) == 0x1F8);
STATIC_ASSERT(offsetof(Dll93EffectResourceView, opaqueTail) == 0x206);
STATIC_ASSERT(sizeof(Dll93EffectResourceView) == 0x208);

extern u32 gDll93EffectResourceData[sizeof(Dll93EffectResourceView) / sizeof(u32)];

void dll_93_spawnEffect(GameObject* sourceObj, int variant, PartFxSpawnParams* spawnParams, u32 spawnFlags) {
    ModgfxSpawnPacket packet;
    u8* resourceData = (u8*)(int)gDll93EffectResourceData;
    GfxCmd* commands = packet.entries;
    f32 randomScale;

    commands[0].layer = 0;
    commands[0].flags = 0x15;
    commands[0].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[0].mode = 0x4;
    commands[0].x = 0.0f;
    commands[0].y = 0.0f;
    commands[0].z = 0.0f;
    commands[1].layer = 0;
    commands[1].flags = 0x15;
    commands[1].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[1].mode = 0x2;
    randomScale = 0.1f * randomGetRange(0, 10) + 1.0f;
    commands[1].x = randomScale;
    commands[1].y = 10.5f;
    commands[1].z = randomScale;
    commands[2].layer = 1;
    commands[2].flags = 0x15;
    commands[2].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[2].mode = 0x4;
    commands[2].x = 255.0f;
    commands[2].y = 0.0f;
    commands[2].z = 0.0f;
    commands[3].layer = 1;
    commands[3].flags = 0x15;
    commands[3].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[3].mode = 0x4000;
    commands[3].x = 1.1f;
    commands[3].y = 0.0f;
    commands[3].z = 0.0f;
    commands[4].layer = 2;
    commands[4].flags = 0x15;
    commands[4].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[4].mode = 0x4;
    commands[4].x = 0.0f;
    commands[4].y = 0.0f;
    commands[4].z = 0.0f;
    commands[5].layer = 2;
    commands[5].flags = 0x15;
    commands[5].tex = &resourceData[offsetof(Dll93EffectResourceView, allVertexIndices)];
    commands[5].mode = 0x4000;
    commands[5].x = 1.1f;
    commands[5].y = 0.0f;
    commands[5].z = 0.0f;
    packet.modeByte = 0;
    packet.sourceObj = sourceObj;
    packet.sourceMode = variant;
    packet.position[0] = 0.0f;
    packet.position[1] = 0.0f;
    packet.position[2] = 0.0f;
    packet.velocity[0] = 0.0f;
    packet.velocity[1] = 0.0f;
    packet.velocity[2] = 0.0f;
    packet.scale = 1.2f;
    packet.drawGroupCount = 2;
    packet.drawGroupStride = 7;
    packet.initialStateByte = 0xE;
    packet.byte5A = 0;
    packet.textureFrameTimer = 0x1E;
    packet.commandCount = (GfxCmd*)((u8*)commands + sizeof(GfxCmd) * 6) - commands;
    packet.sequenceParams[0] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[0])];
    packet.sequenceParams[1] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[1])];
    packet.sequenceParams[2] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[2])];
    packet.sequenceParams[3] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[3])];
    packet.sequenceParams[4] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[4])];
    packet.sequenceParams[5] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[5])];
    packet.sequenceParams[6] = *(s16*)&resourceData[offsetof(Dll93EffectResourceView, sequenceParams[6])];
    packet.commands = (GfxCmd*)((u8*)&packet + offsetof(ModgfxSpawnPacket, entries));
    packet.flags = 0xc0104c0;
    packet.flags |= spawnFlags;
    if ((packet.flags & 1) != 0) {
        if ((u32)sourceObj != 0) {
            GameObject* anchorObj = sourceObj;
            packet.position[0] += anchorObj->anim.localPosX;
            packet.position[1] += anchorObj->anim.localPosY;
            packet.position[2] += anchorObj->anim.localPosZ;
        } else {
            PartFxSpawnParams* anchorParams = spawnParams;
            packet.position[0] += anchorParams->posX;
            packet.position[1] += anchorParams->posY;
            packet.position[2] += anchorParams->posZ;
        }
    }
    (*gModgfxInterface)
        ->spawnEffect(&packet, 0, 0x15, (u8*)(int)gDll93EffectResourceData, 0x18,
                      &resourceData[offsetof(Dll93EffectResourceView, triangles)], 0x89, 0);
}

void dll_93_release(void) {
}

void dll_93_initialise(void) {
}

/* STFX endian: gDll93EffectResourceData is the decomp's PowerPC image of s16 data (vertices, triangle and vertex-INDEX lists, sequence params) written as u32 words, so each word holds two big-endian s16; on ARM the halves read in the wrong order (index lists like {0,1,2,3} became {1,0,3,2}). STFX_HW16 exchanges the halves. */
u32 gDll93EffectResourceData[sizeof(Dll93EffectResourceView) / sizeof(u32)] = {
    STFX_HW16(0x00000000), STFX_HW16(0x03e80000), STFX_HW16(0x00ff0362), STFX_HW16(0x000001f4), STFX_HW16(0x000b00ff), STFX_HW16(0x03620000), STFX_HW16(0xfe0c0016), STFX_HW16(0x00ff0000), STFX_HW16(0x0000fc18),
    STFX_HW16(0x002000ff), STFX_HW16(0xfc9e0000), STFX_HW16(0xfe0c0016), STFX_HW16(0x00fffc9e), STFX_HW16(0x000001f4), STFX_HW16(0x000b00ff), STFX_HW16(0x00000000), STFX_HW16(0x03e80000), STFX_HW16(0x00ff0000),
    STFX_HW16(0x0bb803e8), STFX_HW16(0x0000007f), STFX_HW16(0x03620bb8), STFX_HW16(0x01f4000b), STFX_HW16(0x007f0362), STFX_HW16(0x0bb8fe0c), STFX_HW16(0x0016007f), STFX_HW16(0x00000bb8), STFX_HW16(0xfc180020),
    STFX_HW16(0x007ffc9e), STFX_HW16(0x0bb8fe0c), STFX_HW16(0x0016007f), STFX_HW16(0xfc9e0bb8), STFX_HW16(0x01f4000b), STFX_HW16(0x007f0000), STFX_HW16(0x0bb803e8), STFX_HW16(0x0000007f), STFX_HW16(0x00001770),
    STFX_HW16(0x03e80000), STFX_HW16(0x00000362), STFX_HW16(0x177001f4), STFX_HW16(0x000b0000), STFX_HW16(0x03621770), STFX_HW16(0xfe0c0016), STFX_HW16(0x00000000), STFX_HW16(0x1770fc18), STFX_HW16(0x00200000),
    STFX_HW16(0xfc9e1770), STFX_HW16(0xfe0c0016), STFX_HW16(0x0000fc9e), STFX_HW16(0x177001f4), STFX_HW16(0x000b0000), STFX_HW16(0x00001770), STFX_HW16(0x03e80000), STFX_HW16(0x00000000), STFX_HW16(0x00000001),
    STFX_HW16(0x00080000), STFX_HW16(0x00080007), STFX_HW16(0x00010002), STFX_HW16(0x00090001), STFX_HW16(0x00090008), STFX_HW16(0x00020003), STFX_HW16(0x000a0002), STFX_HW16(0x000a0009), STFX_HW16(0x00030004),
    STFX_HW16(0x000b0003), STFX_HW16(0x000b000a), STFX_HW16(0x00040005), STFX_HW16(0x000c0004), STFX_HW16(0x000c000b), STFX_HW16(0x00050006), STFX_HW16(0x000d0005), STFX_HW16(0x000d000c), STFX_HW16(0x00070008),
    STFX_HW16(0x000f0007), STFX_HW16(0x000f000e), STFX_HW16(0x00080009), STFX_HW16(0x00100008), STFX_HW16(0x0010000f), STFX_HW16(0x0009000a), STFX_HW16(0x00110009), STFX_HW16(0x00110010), STFX_HW16(0x000a000b),
    STFX_HW16(0x0012000a), STFX_HW16(0x00120011), STFX_HW16(0x000b000c), STFX_HW16(0x0013000b), STFX_HW16(0x00130012), STFX_HW16(0x000c000d), STFX_HW16(0x0014000c), STFX_HW16(0x00140013), STFX_HW16(0x00000001),
    STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x00060000), STFX_HW16(0x00070008), STFX_HW16(0x0009000a), STFX_HW16(0x000b000c), STFX_HW16(0x000d0000), STFX_HW16(0x000e000f), STFX_HW16(0x00100011),
    STFX_HW16(0x00120013), STFX_HW16(0x00140000), STFX_HW16(0x00000001), STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x0006000e), STFX_HW16(0x000f0010), STFX_HW16(0x00110012), STFX_HW16(0x00130014),
    STFX_HW16(0x00000001), STFX_HW16(0x00020003), STFX_HW16(0x00040005), STFX_HW16(0x00060007), STFX_HW16(0x00080009), STFX_HW16(0x000a000b), STFX_HW16(0x000c000d), STFX_HW16(0x000e000f), STFX_HW16(0x00100011),
    STFX_HW16(0x00120013), STFX_HW16(0x00140000), STFX_HW16(0x00070008), STFX_HW16(0x0009000a), STFX_HW16(0x000b000c), STFX_HW16(0x000d000e), STFX_HW16(0x000f0010), STFX_HW16(0x00110012), STFX_HW16(0x00130014),
    STFX_HW16(0x00000032), STFX_HW16(0x00320000), STFX_HW16(0x00000000), STFX_HW16(0x00000000)};
Dll93ResourceDescriptor gDll93ResourceDescriptor = {
    {0x00000000, 0x00000000, 0x00000000, 0x00030000}, dll_93_initialise, dll_93_release, NULL, dll_93_spawnEffect,
};
