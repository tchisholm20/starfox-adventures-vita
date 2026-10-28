#pragma once
// Host shadow: forwards to the umbrella dolphin/os.h so a direct
// #include "dolphin/os/OSFastCast.h" gets one consistent host definition.
#include "dolphin/os.h"

// OSFastCast integer<->float conversions. On PPC these are paired-single
// quantized loads/stores through GQRs set by OSInitFastCast.
// Host: a quantized LOAD is a plain widening conversion. A quantized STORE CLAMPS
// to the integer type's range and then truncates -- how Dolphin emulates psq_st;
// a bare C cast would differ out of range. (vita fork: the __OS* forms and the
// stores were missing; model.c and gameloop_main.c use them.)
#ifdef __cplusplus
extern "C" {
#endif
static inline void OSInitFastCast(void) {}
static inline float __OSs8tof32 (const s8*  in) { return (float)*in; }
static inline float __OSu8tof32 (const u8*  in) { return (float)*in; }
static inline float __OSs16tof32(const s16* in) { return (float)*in; }
static inline float __OSu16tof32(const u16* in) { return (float)*in; }
static inline s8  __OSf32tos8 (f32 f) { return (s8) (f > 127.0f   ? 127.0f   : f < -128.0f   ? -128.0f   : f); }
static inline u8  __OSf32tou8 (f32 f) { return (u8) (f > 255.0f   ? 255.0f   : f < 0.0f      ? 0.0f      : f); }
static inline s16 __OSf32tos16(f32 f) { return (s16)(f > 32767.0f ? 32767.0f : f < -32768.0f ? -32768.0f : f); }
static inline u16 __OSf32tou16(f32 f) { return (u16)(f > 65535.0f ? 65535.0f : f < 0.0f      ? 0.0f      : f); }
static inline void OSs8tof32 (const s8*  in, float* out) { *out = __OSs8tof32(in); }
static inline void OSu8tof32 (const u8*  in, float* out) { *out = __OSu8tof32(in); }
static inline void OSs16tof32(const s16* in, float* out) { *out = __OSs16tof32(in); }
static inline void OSu16tof32(const u16* in, float* out) { *out = __OSu16tof32(in); }
static inline void OSf32tos8 (f32* f, s8*  out) { *out = __OSf32tos8(*f); }
static inline void OSf32tou8 (f32* f, u8*  out) { *out = __OSf32tou8(*f); }
static inline void OSf32tos16(f32* f, s16* out) { *out = __OSf32tos16(*f); }
static inline void OSf32tou16(f32* f, u16* out) { *out = __OSf32tou16(*f); }
#ifdef __cplusplus
}
#endif
