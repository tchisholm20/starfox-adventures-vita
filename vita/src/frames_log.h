/* frames_log.h -- the per-frame log's file format (2026-11-03). Pure C99, no Vita headers: the device (frames_log.inc, included by
 * vita_tool_main.c) and the host test (host/tests/frames_log_test.c) compile the SAME pack/unpack, and the host judge
 * (host/engine/frames_judge.py) parses the same layout by hand -- the C <-> Python agreement test is build_frames_log_test.sh.
 *
 * Why: the [win] / [gxt] / [gxm-gpu] lines are 60-present MEANS; a session with 26% of frames >= 50 ms and one 1.88 s frame read as
 * a normal window. This file holds EVERY frame: ux0:data/stairfax/frames.bin.
 *
 * FILE   = 64-byte header, then fixed 32-byte records, little-endian, no padding.
 * HEADER   0 char magic[8] "STFXFRM1" | 8 u32 version (1) | 12 u32 record size (32) | 16 char build_id[40] (NUL padded, the
 *          "[build] id=" string) | 56 u32 flags (bit0 threaded at the first record, bit1 GPU poller on) | 60 u32 reserved
 * RECORD   0 u32 present        g_presents at the hook
 *          4 u32 wall_us        present-to-present: the SAME `now - s_last` the [ft] histogram uses
 *          8 u32 g_cpu_us       frame CPU on the game thread (delta of rhi_vita_timing()[0], what [win] calls "frame CPU"); THREADED: wall_us minus
 *                               G's waits on R (present back-pressure + ring-full + blocking ops, g_gxt.st), because rhi_vita_timing is not fed there
 *         12 u32 r_busy_us      render-thread busy delta (g_gxt.st.r_busy_us); FRM_NONE when not threaded
 *         16 u32 gpu_us         GPU busy delta (the gxm_gpu.txt poller); FRM_NONE when the poller is off
 *         20 u32 compile_us     delta of rhi_vita_gx_compile_us + rhi_vita_tev_compile_us (shader builds on the draw path)
 *         24 u16 async_installed delta of gxm_shader_async_counts()[1], saturating at 0xFFFF
 *         26 u16 map            the game's current map id (gShaderCurMapEventId); 0xFFFF = none (-1)
 *         28 u8  game_state     `gameState`
 *         29 u8  flags          bit0 positive-control stall injected this frame, bit1 GPU value from a poller window marked INVALID
 *                               (NOT populated on the device yet: the poller only judges per 60-present window), bits 2-7 zero
 *         30 u16 reserved       0
 * A missing source writes FRM_NONE (u32 fields that can be absent) or 0 (map/state/async), never garbage. */
#ifndef FRAMES_LOG_H
#define FRAMES_LOG_H
#include <stdint.h>
#include <string.h>

#define FRM_MAGIC        "STFXFRM1"
#define FRM_VERSION      1u
#define FRM_HDR_BYTES    64u
#define FRM_REC_BYTES    32u
#define FRM_BUILD_ID_MAX 40u
#define FRM_NONE         0xFFFFFFFFu
#define FRM_MAP_NONE     0xFFFFu
#define FRM_HF_THREADED  1u   /* header flags */
#define FRM_HF_GPU_POLL  2u
#define FRM_RF_STALL     1u   /* record flags */
#define FRM_RF_GPU_INVALID 2u
#define FRM_MAX_BYTES    (32u * 1024u * 1024u)   /* the device stops writing here */

typedef struct FrmHdr { char magic[9]; uint32_t version, rec_bytes; char build_id[FRM_BUILD_ID_MAX + 1]; uint32_t flags, reserved; } FrmHdr;
typedef struct FrmRec {
    uint32_t present, wall_us, g_cpu_us, r_busy_us, gpu_us, compile_us;
    uint16_t async_installed, map;
    uint8_t  game_state, flags;
    uint16_t reserved;
} FrmRec;

static inline void frm_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }
static inline void frm_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static inline uint32_t frm_get32(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static inline uint16_t frm_get16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static inline uint16_t frm_sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t) v; }

static inline void frm_pack_header(uint8_t out[FRM_HDR_BYTES], const char *build_id, uint32_t flags)
{
    size_t n = build_id ? strlen(build_id) : 0;
    memset(out, 0, FRM_HDR_BYTES);
    memcpy(out, FRM_MAGIC, 8);
    frm_put32(out + 8, FRM_VERSION); frm_put32(out + 12, FRM_REC_BYTES);
    if (n > FRM_BUILD_ID_MAX) n = FRM_BUILD_ID_MAX;
    if (n) memcpy(out + 16, build_id, n);
    frm_put32(out + 56, flags); frm_put32(out + 60, 0);
}
/* 0 = ok; 1 = bad magic; 2 = version != 1; 3 = record size != 32 */
static inline int frm_unpack_header(const uint8_t in[FRM_HDR_BYTES], FrmHdr *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, in, 8);
    if (memcmp(in, FRM_MAGIC, 8) != 0) return 1;
    h->version = frm_get32(in + 8); h->rec_bytes = frm_get32(in + 12);
    memcpy(h->build_id, in + 16, FRM_BUILD_ID_MAX);
    h->flags = frm_get32(in + 56); h->reserved = frm_get32(in + 60);
    if (h->version != FRM_VERSION) return 2;
    if (h->rec_bytes != FRM_REC_BYTES) return 3;
    return 0;
}
static inline void frm_pack_rec(uint8_t out[FRM_REC_BYTES], const FrmRec *r)
{
    frm_put32(out + 0, r->present);  frm_put32(out + 4, r->wall_us);   frm_put32(out + 8, r->g_cpu_us);
    frm_put32(out + 12, r->r_busy_us); frm_put32(out + 16, r->gpu_us); frm_put32(out + 20, r->compile_us);
    frm_put16(out + 24, r->async_installed); frm_put16(out + 26, r->map);
    out[28] = r->game_state; out[29] = r->flags; frm_put16(out + 30, r->reserved);
}
static inline void frm_unpack_rec(const uint8_t in[FRM_REC_BYTES], FrmRec *r)
{
    r->present = frm_get32(in + 0); r->wall_us = frm_get32(in + 4); r->g_cpu_us = frm_get32(in + 8);
    r->r_busy_us = frm_get32(in + 12); r->gpu_us = frm_get32(in + 16); r->compile_us = frm_get32(in + 20);
    r->async_installed = frm_get16(in + 24); r->map = frm_get16(in + 26);
    r->game_state = in[28]; r->flags = in[29]; r->reserved = frm_get16(in + 30);
}
#endif
