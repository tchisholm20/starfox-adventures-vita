#pragma once
// byteswap.h - endianness strategy for the decomp-native port.
//
// THE PROBLEM. The GameCube is big-endian; every structure on the disc is stored BE.
// The recompiled game reads struct fields NATIVELY (e.g. `gGameBitTable[id].firstBit`),
// which is correct on a BE console but reads byte-swapped garbage on a little-endian
// host. So any BE buffer that will be handed to REAL game code must be converted to host
// byte order ONCE, at load time, according to its struct layout, before the game reads it.
//
// TWO PATHS, DON'T CONFUSE THEM:
//   * Port-side readers (stairfax_assets: asset_be16/asset_be32/...) decode BE bytes into
//     freshly-built host values. Data consumed ONLY by port code stays untouched on disc
//     and is read through those value-readers. Do NOT also in-place-swap those buffers.
//   * Real game TUs read loaded structs directly. Buffers on THIS path must be converted
//     in place with the helpers below, right after loading and before the game sees them.
//
// THE DISCIPLINE. For each BE-on-disc structure the game reads natively, write one
// `bswap<Type>(void*)` next to its loader that swaps exactly its multi-byte fields (u8 /
// char / packed-byte-bitfield members need nothing), then apply it at load:
//     beFixRecords(buf, count, sizeof(T), bswap<T>);   // array of records
//     bswap<T>(buf);                                    // single struct
// Nested structs: call the child's bswap from the parent's. Embedded file offsets are u32
// -> BE32 them. This is deliberately manual (one swapper per type) so the layout knowledge
// lives with the type; if the manual burden grows, a codegen pass over the struct defs is
// the escalation, not blind whole-buffer swapping (which would corrupt byte arrays).
//
// Header-only, C and C++. Assumes a little-endian host (x86, ARM LE); on a big-endian host
// every helper is a no-op because the loaded data is already native.

#include <stdint.h>
#include <stddef.h>

#ifndef STAIRFAX_HOST_BIG_ENDIAN
#  if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define STAIRFAX_HOST_BIG_ENDIAN 1
#  else
#    define STAIRFAX_HOST_BIG_ENDIAN 0
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t stfx_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t stfx_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000ff00u) | ((v << 8) & 0x00ff0000u) | (v << 24);
}
static inline uint64_t stfx_bswap64(uint64_t v) {
    return ((uint64_t)stfx_bswap32((uint32_t)v) << 32) | stfx_bswap32((uint32_t)(v >> 32));
}

// --- value readers: BE bytes -> host value (non-mutating) ------------------
static inline uint16_t beRead16(const void* p) {
    const uint8_t* b = (const uint8_t*)p; return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static inline uint32_t beRead32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static inline float beReadF32(const void* p) {
    union { uint32_t u; float f; } x; x.u = beRead32(p); return x.f;
}

// --- in-place converters: BE in memory -> host, mutating -------------------
// Byte-wise so they work on unaligned pointers inside loaded buffers, and compile to
// nothing on a big-endian host.
static inline void beFix16(void* p) {
#if !STAIRFAX_HOST_BIG_ENDIAN
    uint8_t* b = (uint8_t*)p; uint8_t t = b[0]; b[0] = b[1]; b[1] = t;
#else
    (void)p;
#endif
}
static inline void beFix32(void* p) {
#if !STAIRFAX_HOST_BIG_ENDIAN
    uint8_t* b = (uint8_t*)p, t;
    t = b[0]; b[0] = b[3]; b[3] = t;
    t = b[1]; b[1] = b[2]; b[2] = t;
#else
    (void)p;
#endif
}
static inline void beFix64(void* p) {
#if !STAIRFAX_HOST_BIG_ENDIAN
    uint8_t* b = (uint8_t*)p, t;
    for (int i = 0; i < 4; ++i) { t = b[i]; b[i] = b[7 - i]; b[7 - i] = t; }
#else
    (void)p;
#endif
}

// Field macros: swap a struct field in place by name. BEF32 is a synonym for BE32
// (a float and its u32 bit pattern swap identically).
#define BE16(field)  beFix16(&(field))
#define BE32(field)  beFix32(&(field))
#define BE64(field)  beFix64(&(field))
#define BEF32(field) beFix32(&(field))

// --- bulk helpers ----------------------------------------------------------
static inline void beFixArray16(void* base, size_t n) {
    uint8_t* b = (uint8_t*)base; for (size_t i = 0; i < n; ++i) beFix16(b + i * 2);
}
static inline void beFixArray32(void* base, size_t n) {
    uint8_t* b = (uint8_t*)base; for (size_t i = 0; i < n; ++i) beFix32(b + i * 4);
}
// Apply a per-record swapper across an array of `count` records of `stride` bytes.
static inline void beFixRecords(void* base, size_t count, size_t stride, void (*swap)(void*)) {
    uint8_t* b = (uint8_t*)base;
    for (size_t i = 0; i < count; ++i) swap(b + i * stride);
}

#ifdef __cplusplus
}
#endif
