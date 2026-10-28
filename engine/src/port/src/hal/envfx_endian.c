// envfx_endian.c - ENVFXACT.bin records (main/render.c getEnvfxAct / getEnvfxActImmediately) to
// host order. 2026-09-28, Vita run 86: the ShipBattle sky asked dlls/engine/5 to blend two 8x8 IA8
// textures into its 4x256 RGBA8 gradient -- the sky texture ids (u16, +0xc38) were read big-endian
// from this file, blendTextures refused the mismatched formats, and the gradient stayed as its
// uninitialised allocation: the rainbow sky of runs 82-86.
//
// A record is 0x60 bytes; its layout is chosen by `kind` (u8 @0x5C) and read by one consumer each.
// The swaps are exactly the multi-byte fields each consumer reads (surveyed 2026-09-28):
//   0,1,2,4  dlls/engine/7 newclouds   CloudSpawnParams:  f32 @0,4,8; u16 @0x24,26,28,2A,2C
//   3        dlls/engine/6 sky2        Sky2Config:        u16 @0x24,2A,2C, 0x2E..0x4D (fog keys /
//   5        dlls/engine/5 sky         Sky2Config          sky texture ids: u16 in BOTH union arms,
//                                                          and the raw bytes 0x36..0x3D are never
//                                                          read), u16 @0x54,56
//   6        dlls/engine/9 cloudaction CloudActionConfig: f32 @8; u16 @0x24
// Every other byte is a u8 field or unread. The copy is getEnvfxAct's own stack buffer, so there is
// nothing to double-swap. Unknown kinds are left alone (and counted).
#include <stdint.h>

static inline void sw16(uint8_t* p) { uint8_t t = p[0]; p[0] = p[1]; p[1] = t; }
static inline void sw32(uint8_t* p) { uint8_t t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; }

unsigned gStfxEnvfxSwapped, gStfxEnvfxUnknownKind;

void stfx_swap_envfx_act(void* rec) {
    uint8_t* b = (uint8_t*)rec;
    if (!b) return;
    switch (b[0x5C]) {
    case 0: case 1: case 2: case 4:
        sw32(b + 0x00); sw32(b + 0x04); sw32(b + 0x08);
        for (int o = 0x24; o <= 0x2C; o += 2) sw16(b + o);
        break;
    case 3: case 5:
        sw16(b + 0x24); sw16(b + 0x2A); sw16(b + 0x2C);
        for (int o = 0x2E; o < 0x4E; o += 2) sw16(b + o);
        sw16(b + 0x54); sw16(b + 0x56);
        break;
    case 6:
        sw32(b + 0x08); sw16(b + 0x24);
        break;
    default:
        ++gStfxEnvfxUnknownKind;
        return;
    }
    ++gStfxEnvfxSwapped;
}
