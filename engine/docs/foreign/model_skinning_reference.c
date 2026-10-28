/* Portable description of the two-matrix paired-single skinning loops.
 * The live implementations in src/main/model.c are the retail assembly; these
 * ordinary-C bodies compute the same blend with GQR6 weights and GQR7 vertices. */
#include "types.h"

typedef struct ModelSkinWeightPair {
    u8 matrixA;
    u8 matrixB;
} ModelSkinWeightPair;

typedef struct ModelPackedNormal {
    s8 x;
    s8 y;
    s8 z;
} ModelPackedNormal;

/* Skinning reads the same live quantization state as retail psq instructions. */
static inline u32 modelGetGQR7(void) {
    register u32 config;
    asm {
        mfspr config, GQR7
    }
    return config;
}

/* GQR scale fields encode signed six-bit powers of two. */
static inline f32 modelQuantizationFactor(u32 encodedScale) {
    union {
        u32 bits;
        f32 value;
    } factor;
    int shift = (int)(encodedScale & 0x3f);
    shift = (shift ^ 0x20) - 0x20;
    factor.bits = (u32)(127 + shift) << 23;
    return factor.value;
}

static inline __vec2x32float__ modelLoadFloatPair(const f32* pair) {
    return *(const __vec2x32float__*)pair;
}

void ObjModel_TransformVerticesWithTranslation(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source, u8* destination,
                                               int count) {
    f32* a = (f32*)matrixA;
    f32* b = (f32*)matrixB;
    ModelSkinWeightPair* weights = (ModelSkinWeightPair*)weightPairs;
    S16Vec* input = (S16Vec*)source;
    S16Vec* output = (S16Vec*)destination;
    u32 quantization = modelGetGQR7();
    f32 storeFactor = modelQuantizationFactor(quantization >> 8);
    f32 loadFactor = 1.0f / modelQuantizationFactor(quantization >> 24);
    f32 x, y, z, weightA, weightB, outputZ;
    union {
        __vec2x32float__ pair;
        f32 values[2];
    } outputXY;
    int vertex;

    for (vertex = 0; vertex < count; vertex++) {
        weightA = __OSu8tof32(&weights->matrixA) * (1.0f / 128.0f);
        weightB = __OSu8tof32(&weights->matrixB) * (1.0f / 128.0f);
        weights++;
        x = __OSs16tof32(&input->x) * loadFactor;
        y = __OSs16tof32(&input->y) * loadFactor;
        z = __OSs16tof32(&input->z) * loadFactor;
        input++;
        outputXY.pair = (modelLoadFloatPair(b) * x + modelLoadFloatPair(b + 9) + modelLoadFloatPair(b + 3) * y +
                         modelLoadFloatPair(b + 6) * z) *
                            weightB +
                        (modelLoadFloatPair(a) * x + modelLoadFloatPair(a + 9) + modelLoadFloatPair(a + 3) * y +
                         modelLoadFloatPair(a + 6) * z) *
                            weightA;
        outputZ =
            (b[2] * x + b[11] + b[5] * y + b[8] * z) * weightB + (a[2] * x + a[11] + a[5] * y + a[8] * z) * weightA;
        output->x = __OSf32tos16(outputXY.values[0] * storeFactor);
        output->y = __OSf32tos16(outputXY.values[1] * storeFactor);
        output->z = __OSf32tos16(outputZ * storeFactor);
        output++;
    }
}

void ObjModel_TransformVerticesLinear(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source, u8* destination,
                                      int count) {
    f32* a = (f32*)matrixA;
    f32* b = (f32*)matrixB;
    ModelSkinWeightPair* weights = (ModelSkinWeightPair*)weightPairs;
    ModelPackedNormal* input = (ModelPackedNormal*)source;
    ModelPackedNormal* output = (ModelPackedNormal*)destination;
    u32 quantization = modelGetGQR7();
    f32 storeFactor = modelQuantizationFactor(quantization >> 8);
    f32 loadFactor = 1.0f / modelQuantizationFactor(quantization >> 24);
    f32 x, y, z, weightA, weightB, outputX, outputY, outputZ;
    int vertex;

    for (vertex = 0; vertex < count; vertex++) {
        weightA = __OSu8tof32(&weights->matrixA) * (1.0f / 128.0f);
        weightB = __OSu8tof32(&weights->matrixB) * (1.0f / 128.0f);
        weights++;
        x = __OSs8tof32(&input->x) * loadFactor;
        y = __OSs8tof32(&input->y) * loadFactor;
        z = __OSs8tof32(&input->z) * loadFactor;
        input++;
        outputX = (b[0] * x + b[3] * y + b[6] * z) * weightB + (a[0] * x + a[3] * y + a[6] * z) * weightA;
        outputY = (b[1] * x + b[4] * y + b[7] * z) * weightB + (a[1] * x + a[4] * y + a[7] * z) * weightA;
        outputZ = (b[2] * x + b[5] * y + b[8] * z) * weightB + (a[2] * x + a[5] * y + a[8] * z) * weightA;
        output->x = __OSf32tos8(outputX * storeFactor);
        output->y = __OSf32tos8(outputY * storeFactor);
        output->z = __OSf32tos8(outputZ * storeFactor);
        output++;
    }
}

void ObjModel_TransformNormalTriplets(u8* matrixA, u8* matrixB, u8* weightPairs, u8* source, u8* destination,
                                      int count) {
    f32* a = (f32*)matrixA;
    f32* b = (f32*)matrixB;
    ModelSkinWeightPair* weights = (ModelSkinWeightPair*)weightPairs;
    ModelPackedNormal* input = (ModelPackedNormal*)source;
    ModelPackedNormal* output = (ModelPackedNormal*)destination;
    u32 quantization = modelGetGQR7();
    f32 storeFactor = modelQuantizationFactor(quantization >> 8);
    f32 loadFactor = 1.0f / modelQuantizationFactor(quantization >> 24);
    f32 x, y, z, weightA, weightB, outputX, outputY, outputZ;
    int vertex;
    int vector;

    for (vertex = 0; vertex < count; vertex++) {
        weightA = __OSu8tof32(&weights->matrixA) * (1.0f / 128.0f);
        weightB = __OSu8tof32(&weights->matrixB) * (1.0f / 128.0f);
        weights++;
        for (vector = 0; vector < 3; vector++) {
            x = __OSs8tof32(&input->x) * loadFactor;
            y = __OSs8tof32(&input->y) * loadFactor;
            z = __OSs8tof32(&input->z) * loadFactor;
            input++;
            outputX = (b[0] * x + b[3] * y + b[6] * z) * weightB + (a[0] * x + a[3] * y + a[6] * z) * weightA;
            outputY = (b[1] * x + b[4] * y + b[7] * z) * weightB + (a[1] * x + a[4] * y + a[7] * z) * weightA;
            outputZ = (b[2] * x + b[5] * y + b[8] * z) * weightB + (a[2] * x + a[5] * y + a[8] * z) * weightA;
            output->x = __OSf32tos8(outputX * storeFactor);
            output->y = __OSf32tos8(outputY * storeFactor);
            output->z = __OSf32tos8(outputZ * storeFactor);
            output++;
        }
    }
}
