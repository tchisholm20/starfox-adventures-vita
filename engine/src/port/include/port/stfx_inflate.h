#pragma once
// stfx_inflate.h - minimal DEFLATE/zlib inflate for SFA's ZLB texture blocks.
//
// SFA stores textures (and other assets) in "ZLB" containers: a 16-byte header
// (magic "ZLB\0", u32 version, u32 uncompressed size, u32 compressed size) followed
// by a zlib stream. This decompresses that stream. Implementation is a compact port
// of Mark Adler's public-domain "puff" reference inflater.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inflate a raw DEFLATE stream into dst (capacity dstCap). Returns 0 on success and
// writes the produced length to *outLen. Non-zero on error.
int stfx_inflate_raw(uint8_t* dst, size_t dstCap, const uint8_t* src, size_t srcLen, size_t* outLen);

// Inflate a zlib stream (2-byte header + DEFLATE + adler32). Skips the header and
// calls the raw inflater. Returns 0 on success.
int stfx_inflate_zlib(uint8_t* dst, size_t dstCap, const uint8_t* src, size_t srcLen, size_t* outLen);

#ifdef __cplusplus
}
#endif
