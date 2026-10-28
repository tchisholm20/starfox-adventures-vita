#ifndef MAIN_BE_DATA_H_
#define MAIN_BE_DATA_H_

#include "global.h"

/*
 * STFX endian: the decomp emits some static data as raw PowerPC bytes (`u8 tbl[] = {0x3F, 0x80, 0, 0, ...}`) and then
 * reads it through a cast or a struct. On the little-endian Vita those reads return the byte-reversed value.
 *
 *   STFX_BE_STRUCT   put it on a struct that is only ever laid over such big-endian bytes (or initialised statically):
 *                    GCC then loads/stores every scalar member big-endian, so the byte table stays as the original wrote it.
 *                    (Not for a struct that also holds pointers or is filled by native code.)
 *   stfx_be_*(p)     read one big-endian scalar at p (for `*(f32*)(tbl + off)` style reads); no-op off the port.
 */
#if defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define STFX_BE_STRUCT __attribute__((scalar_storage_order("big-endian")))
/* a u32 initialiser word that is really two big-endian s16 (or u16): exchange the halves so the little-endian s16 view reads them in order */
#define STFX_HW16(w) ((u32)((((u32)(w)) << 16) | (((u32)(w)) >> 16)))
/* a u32 initialiser word that is really four bytes (u8[4]) in big-endian order: reverse them */
#define STFX_BS32(w) ((u32)__builtin_bswap32((u32)(w)))
static inline u32 stfx_be_u32(const void* p) {
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}
static inline s32 stfx_be_s32(const void* p) {
    return (s32)stfx_be_u32(p);
}
static inline u16 stfx_be_u16(const void* p) {
    const u8* b = (const u8*)p;
    return (u16)(((u32)b[0] << 8) | (u32)b[1]);
}
static inline s16 stfx_be_s16(const void* p) {
    return (s16)stfx_be_u16(p);
}
static inline f32 stfx_be_f32(const void* p) {
    union {
        u32 u;
        f32 f;
    } v;
    v.u = stfx_be_u32(p);
    return v.f;
}
#else
#define STFX_BE_STRUCT
#define STFX_HW16(w) (w)
#define STFX_BS32(w) (w)
#define stfx_be_u32(p) (*(const u32*)(p))
#define stfx_be_s32(p) (*(const s32*)(p))
#define stfx_be_u16(p) (*(const u16*)(p))
#define stfx_be_s16(p) (*(const s16*)(p))
#define stfx_be_f32(p) (*(const f32*)(p))
#endif

#endif /* MAIN_BE_DATA_H_ */
