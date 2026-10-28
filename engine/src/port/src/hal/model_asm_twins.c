// model_asm_twins.c - C bodies for the model functions upstream keeps as MWCC asm.
//
// Upstream reached 100% matching (2026-09-27) by making the skinning and morph kernels
// function-level `asm` (the retail paired-single code, private register ABIs) in src/main/model.c.
// The build copy removes those bodies (host/engine/wgpipe_rewrite.py strip_mwcc_asm); these are
// the port's definitions. They are the fork's own C bodies from before the merge (vita branch,
// model.c), which ran on hardware in Vita runs 48-67 -- renamed to upstream's names, nothing else:
//
//   upstream name                              fork name before the merge
//   ObjModel_TransformVerticesWithTranslation  (same)
//   ObjModel_TransformVerticesLinear           (same)
//   ObjModel_TransformNormalTriplets           ObjModel_TransformQuadVerticesLinear
//   modelBlendMorphTargetChunk                 modelApplyBoneTransform
//   modelReadMorphDelta                        modelBoneTransforms_next (retail: private ABI, so static)
//
// Upstream's own portable descriptions (docs/foreign/model_skinning_reference.c,
// model_morph_reference.c) compute the same blends; the GQR scale arithmetic agrees (load x 2^-s,
// store x 2^s, s = GQR7[29:24], equal load/store scales as setGQR7Packed(q, T, q, T) sets them).
//
// GQR state: setGQR6/setGQR7 write gStfxGQR[] (their `mtspr` blocks are replaced in the build copy).
#include <dolphin/types.h>
#include "dolphin/os/OSFastCast.h"

unsigned int gStfxGQR[8];

#define MODEL_MORPH_VERTEX_INDEX_MASK 0x1fff
#define MODEL_MORPH_HAS_X 0x2000
#define MODEL_MORPH_HAS_Y 0x4000
#define MODEL_MORPH_HAS_Z 0x8000

/* THE MORPH DATA IS BIG-ENDIAN (2026-10-09, docs/47 "Run 160"). The base vertices stay in disc order for the GX decoder (as
   the skinning below says, run 48), and the morph streams are raw file bytes: game_model_swap.c swaps only the POINTER table
   (morphTargetPtrs). Until run 160 this kernel read the index/flag words, the deltas and the base vertices natively and wrote
   natively -- every morphed vertex was arithmetic on byte-swapped numbers (the CloudRunner's wings in ShipBattle: a star of
   spikes, runs 157-159). MORPH_NATIVE = the old reading, for the host test's break arm only. */
#ifdef MORPH_NATIVE
#define MRD16(p)    (*(const u16*)(p))
#define MWR16(p, v) (*(u16*)(p) = (u16)(v))
#else
#define MRD16(p)    ((u16)(((const u8*)(p))[0] << 8 | ((const u8*)(p))[1]))
#define MWR16(p, v) (((u8*)(p))[0] = (u8)((u16)(v) >> 8), ((u8*)(p))[1] = (u8)(v))
#endif

static u16* modelReadMorphDelta(u16* stream, int* dx, int* dy, int* dz)
{
    u16 flags = MRD16(stream); stream++;

    *dx = 0;
    if (flags & MODEL_MORPH_HAS_X) { *dx = (s16)MRD16(stream); stream++; }
    *dy = 0;
    if (flags & MODEL_MORPH_HAS_Y) { *dy = (s16)MRD16(stream); stream++; }
    *dz = 0;
    if (flags & MODEL_MORPH_HAS_Z) { *dz = (s16)MRD16(stream); stream++; }
    return stream;
}

void modelBlendMorphTargetChunk(u8* baseVertices, u8* outVertices, u16 vertexCount, u16** targetA, u16** targetB,
                                int weightB, u16 firstVertex)
{
    u16* a = *targetA;
    u16* b = *targetB;
    u8* p = baseVertices;
    u8* out = outVertices;
    int i = 0;
    int wHi = 0x10000 - weightB;
    int aIdx, bIdx;
    int ax, ay, az;
    int bx, by, bz;

    while (i < vertexCount)
    {
        aIdx = (MRD16(a) & MODEL_MORPH_VERTEX_INDEX_MASK) - firstVertex;
        bIdx = (MRD16(b) & MODEL_MORPH_VERTEX_INDEX_MASK) - firstVertex;
        if (i >= aIdx)
        {
            if (i == bIdx)
            {
                b = modelReadMorphDelta(b, &bx, &by, &bz);
                a = modelReadMorphDelta(a, &ax, &ay, &az);
                MWR16(out,     ((u32)(ax * wHi + bx * weightB) >> 16) + (s16)MRD16(p));
                MWR16(out + 2, ((u32)(ay * wHi + by * weightB) >> 16) + (s16)MRD16(p + 2));
                MWR16(out + 4, ((u32)(az * wHi + bz * weightB) >> 16) + (s16)MRD16(p + 4));
            }
            else
            {
                a = modelReadMorphDelta(a, &ax, &ay, &az);
                MWR16(out,     ((u32)(ax * wHi) >> 16) + (s16)MRD16(p));
                MWR16(out + 2, ((u32)(ay * wHi) >> 16) + (s16)MRD16(p + 2));
                MWR16(out + 4, ((u32)(az * wHi) >> 16) + (s16)MRD16(p + 4));
            }
        }
        else if (i >= bIdx)
        {
            b = modelReadMorphDelta(b, &bx, &by, &bz);
            MWR16(out,     ((u32)(bx * weightB) >> 16) + (s16)MRD16(p));
            MWR16(out + 2, ((u32)(by * weightB) >> 16) + (s16)MRD16(p + 2));
            MWR16(out + 4, ((u32)(bz * weightB) >> 16) + (s16)MRD16(p + 4));
        }
        else
        {
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; out[4] = p[4]; out[5] = p[5];
        }
        p += 6;
        out += 6;
        i++;
    }
    *targetA = a;
    *targetB = b;
}

void ObjModel_TransformVerticesWithTranslation(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source,
                                               u8* destination, int count)
{
    f32* ma = (f32*)matrixA;
    f32* mb = (f32*)matrixB;
    u8* w = weightPairs;
    u8* in = source;
    u8* out = destination;
    f32 scale = (f32)(1 << ((gStfxGQR[7] >> 24) & 0x3f));
    f32 invScale = 1.0f / scale;
    f32 x, y, z, w0, w1, ox, oy, oz;
    s16 v[3];
    int i, k;

    for (i = 0; i < count; i++)
    {
        w0 = __OSu8tof32(w) * (1.0f / 128.0f);
        w1 = __OSu8tof32(w + 1) * (1.0f / 128.0f);
        w += 2;
        /* vertex position arrays stay in DISC order (big-endian) -- the GX decoder (hal/gx_draw.c
           rdS) reads them that way -- so the CPU skinning reads and writes big-endian (run 48). */
        x = (f32)(s16)(in[0] << 8 | in[1]) * invScale;
        y = (f32)(s16)(in[2] << 8 | in[3]) * invScale;
        z = (f32)(s16)(in[4] << 8 | in[5]) * invScale;
        in += 6;
        ox = (ma[0] * x + ma[3] * y + ma[6] * z + ma[9]) * w0 + (mb[0] * x + mb[3] * y + mb[6] * z + mb[9]) * w1;
        oy = (ma[1] * x + ma[4] * y + ma[7] * z + ma[10]) * w0 + (mb[1] * x + mb[4] * y + mb[7] * z + mb[10]) * w1;
        oz = (ma[2] * x + ma[5] * y + ma[8] * z + ma[11]) * w0 + (mb[2] * x + mb[5] * y + mb[8] * z + mb[11]) * w1;
        v[0] = __OSf32tos16(ox * scale);
        v[1] = __OSf32tos16(oy * scale);
        v[2] = __OSf32tos16(oz * scale);
        for (k = 0; k < 3; k++)
        {
            out[k * 2] = (u8)((u16)v[k] >> 8);
            out[k * 2 + 1] = (u8)v[k];
        }
        out += 6;
    }
}

void ObjModel_TransformVerticesLinear(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source, u8* destination,
                                      int count)
{
    f32* ma = (f32*)matrixA;
    f32* mb = (f32*)matrixB;
    u8* w = weightPairs;
    s8* in = (s8*)source;
    s8* out = (s8*)destination;
    f32 scale = (f32)(1 << ((gStfxGQR[7] >> 24) & 0x3f));
    f32 invScale = 1.0f / scale;
    f32 x, y, z, w0, w1, ox, oy, oz;
    int i;

    for (i = 0; i < count; i++)
    {
        w0 = __OSu8tof32(w) * (1.0f / 128.0f);
        w1 = __OSu8tof32(w + 1) * (1.0f / 128.0f);
        w += 2;
        x = __OSs8tof32(&in[0]) * invScale;
        y = __OSs8tof32(&in[1]) * invScale;
        z = __OSs8tof32(&in[2]) * invScale;
        in += 3;
        ox = (ma[0] * x + ma[3] * y + ma[6] * z) * w0 + (mb[0] * x + mb[3] * y + mb[6] * z) * w1;
        oy = (ma[1] * x + ma[4] * y + ma[7] * z) * w0 + (mb[1] * x + mb[4] * y + mb[7] * z) * w1;
        oz = (ma[2] * x + ma[5] * y + ma[8] * z) * w0 + (mb[2] * x + mb[5] * y + mb[8] * z) * w1;
        out[0] = __OSf32tos8(ox * scale);
        out[1] = __OSf32tos8(oy * scale);
        out[2] = __OSf32tos8(oz * scale);
        out += 3;
    }
}

void ObjModel_TransformNormalTriplets(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source, u8* destination,
                                      int count)
{
    f32* ma = (f32*)matrixA;
    f32* mb = (f32*)matrixB;
    u8* w = weightPairs;
    s8* in = (s8*)source;
    s8* out = (s8*)destination;
    f32 scale = (f32)(1 << ((gStfxGQR[7] >> 24) & 0x3f));
    f32 invScale = 1.0f / scale;
    f32 x, y, z, w0, w1, ox, oy, oz;
    int i, k;

    for (i = 0; i < count; i++)
    {
        w0 = __OSu8tof32(w) * (1.0f / 128.0f);
        w1 = __OSu8tof32(w + 1) * (1.0f / 128.0f);
        w += 2;
        for (k = 0; k < 3; k++)
        {
            x = __OSs8tof32(&in[0]) * invScale;
            y = __OSs8tof32(&in[1]) * invScale;
            z = __OSs8tof32(&in[2]) * invScale;
            in += 3;
            ox = (ma[0] * x + ma[3] * y + ma[6] * z) * w0 + (mb[0] * x + mb[3] * y + mb[6] * z) * w1;
            oy = (ma[1] * x + ma[4] * y + ma[7] * z) * w0 + (mb[1] * x + mb[4] * y + mb[7] * z) * w1;
            oz = (ma[2] * x + ma[5] * y + ma[8] * z) * w0 + (mb[2] * x + mb[5] * y + mb[8] * z) * w1;
            out[0] = __OSf32tos8(ox * scale);
            out[1] = __OSf32tos8(oy * scale);
            out[2] = __OSf32tos8(oz * scale);
            out += 3;
        }
    }
}
