/* gx_rt_split_be.h -- R5 (host/engine/R5_SPLIT.md): the TYPED surface of the render-thread split.
 *   GxspApi      what the shims (rhi_vita.c, rhi_vita_tev.inc, rhi_vita_efb.inc) and the C9 dispatcher (rhi_gxm_draw.c) call when g_gxsp_cap is set.  Defined by gx_rt_split.c; every entry
 *                point works in BOTH modes (ON: record; OFF: unwrap the proxies and call the backend directly).
 *   GxspBackend  what a backend provides: the real calls (executed by the thread that owns the backend: the back thread when ON, the caller when OFF) and the PURE queries the front
 *                answers without touching the backend (constants only, safe on any thread).  The console's table is src/rhi_gxm_split.c; the host tests supply their own.
 *   GxspProxy    the handle the HAL holds for a texture / copy texture / mesh while capable. */
#ifndef GX_RT_SPLIT_BE_H
#define GX_RT_SPLIT_BE_H

#include <stdint.h>
#include <stddef.h>
#include "port/renderer/rhi.h"
#include "port/renderer/rhi_internal.h"
#include "port/gx_tev_key.h"
#include "gx_rt_split.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GxspBackend {
    RhiInstance *inst;                 /* the shared generic instance (the HAL's gRhi points at it) */
    const RhiOps *ops;                 /* the REAL ops table (the gRhi->ops pointer is replaced by the split's own while capable) */
    RhiInstance *exec_inst;            /* a private instance the back thread passes to the generic ops (alphaMode / bound texture are per-thread state while ON); NULL: use inst */
    /* ---- PURE queries: any thread, constants only ---- */
    int  tex_max_dim;
    int  (*layout_ok)(const GxMeshLayout *);                                                                              /* rhi_gxm_draw.c gxm_layout_ok */
    int  (*mesh_check)(uint32_t stride, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, uint32_t *need);             /* gxm_mesh_check */
    int  (*copy_ok)(int x, int y, int w, int h, int dw, int dh, int gxfmt);                                               /* 1: rhi_gxm_copyEfb would not refuse on the arguments alone */
    /* ---- the real calls (handles are REAL pointers) ---- */
    void (*setDepthState)(int test, int func, int write);
    void (*setViewport)(float x, float y, float w, float h);
    void (*setScissor)(int x, int y, int w, int h);
    void (*setShadowZBias)(int on);
    void (*clearRect)(int x, int y, int w, int h, uint32_t rgba, float z);
    void (*texInfo)(RhiTexture *h, int out[4]);
    void (*texSetWrap)(RhiTexture *h, int gxS, int gxT);
    void (*texSetFilter)(RhiTexture *h, int minGX, int magNearest, int wantMips);
    void (*texSetLodBias)(RhiTexture *h, int bias8);
    int  (*texSetMips)(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel);
    RhiTexture *(*copyEfb)(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt);
    void (*copyTexSetFilter)(RhiTexture *h, int gxMin, int gxMag);
    void (*copyTexSetSampler)(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT);
    void (*copyEfbFree)(RhiTexture *t);
    unsigned char *(*readEfbRect)(int x, int y, int w, int h, int *ow, int *oh);
    int  (*drawTev)(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16], int alphaMode);
    void *(*meshCreate)(const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, const GxMeshLayout *lay);
    void (*meshUpdate)(void *mesh, const void *verts, uint32_t vbytes);
    void (*meshDestroy)(void *mesh);
    void *(*meshBegin)(void *mesh, uint32_t vbytes, int *fresh);       /* R6 C9: the real two-half update (rhi_gxm_gx_mesh_update_begin_real) */
    void (*meshCommit)(void *mesh);
    int  (*drawTevGx)(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode, unsigned long long hint);
    /* D4: would the backend answer 0 (NOPROG) for this key?  Executed by the backend's owner thread; the front caches the answer per key.  1 = a draw proceeds (a program, or a failure that still returns 1). */
    int  (*verdictGx)(const VsKey *vk, const TevKey *tk);
    int  (*verdictTev)(const TevKey *tk);
    void (*setR3a)(int on, unsigned mask);                              /* the back's private copy of the cut switches (gx_r3a is G-written) */
} GxspBackend;

#define GXSP_PROXY_MAGIC 0x58505347u    /* "GSPX" */
enum { GXSP_K_TEX = 1, GXSP_K_COPY = 2, GXSP_K_MESH = 3 };
typedef struct GxspProxy {
    uint32_t magic; uint32_t kind;
    struct GxspProxy *self;            /* magic + self: a real GxmTex / GxmMesh never has both */
    void *real;                        /* the backend object.  Written by whoever executes the create (the back when ON); read by that same side */
    /* ---- back-private (written/read by the executing side only) ---- */
    uint8_t *img; uint32_t imgn;       /* C9: the CPU copy of the mesh's full image (owned by the back) */
    /* ---- front-private ---- */
    int w, h; uint32_t fmt;            /* texture facts */
    uint8_t wrap_v, filt_v, lod_v, csamp_v, cfilt_v;   /* D8: the sampler shadows below are valid */
    int wrap_s, wrap_t, filt_min, filt_mag, filt_mips, lod, cs[4], cf[2];
    uint32_t vbytes, nidx, need;       /* mesh facts, as the backend would hold them */
    int bad, c9_primed;
    GxMeshLayout lay;
} GxspProxy;

typedef struct GxspApi {
    void (*setDepthState)(int test, int func, int write);
    void (*setViewport)(float x, float y, float w, float h);
    void (*setScissor)(int x, int y, int w, int h);
    void (*setShadowZBias)(int on);
    void (*clearRect)(int x, int y, int w, int h, uint32_t rgba, float z);
    void (*texInfo)(RhiTexture *h, int out[4]);
    void (*texSetWrap)(RhiTexture *h, int gxS, int gxT);
    void (*texSetFilter)(RhiTexture *h, int minGX, int magNearest, int wantMips);
    void (*texSetLodBias)(RhiTexture *h, int bias8);
    int  (*texSetMips)(RhiTexture *h, int nlev, unsigned char *const *rgba, int maxLevel);
    RhiTexture *(*copyEfb)(RhiTexture *reuse, int x, int y, int w, int h, int dw, int dh, int gxfmt);
    void (*copyTexSetFilter)(RhiTexture *h, int gxMin, int gxMag);
    void (*copyTexSetSampler)(RhiTexture *h, int gxMin, int gxMag, int gxWrapS, int gxWrapT);
    void (*copyEfbFree)(RhiTexture *t);
    unsigned char *(*readEfbRect)(int x, int y, int w, int h, int *ow, int *oh);
    int  (*drawTev)(const TevKey *k, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const TevVertex *verts, uint32_t count, const float mvp[16], int alphaMode);
    void *(*meshCreate)(const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx, const GxMeshLayout *lay);
    void (*meshUpdate)(void *mesh, const void *verts, uint32_t vbytes);
    void (*meshDestroy)(void *mesh);
    void *(*meshBegin)(void *mesh, uint32_t vbytes, int *fresh);
    void (*meshCommit)(void *mesh);
    int  (*drawTevGx)(const TevKey *tk, const TevUniforms *u, RhiTexture *const tex[TEVK_TEX], const VsKey *vk, const VsUniforms *vu, void *mesh, int alphaMode);
} GxspApi;

/* Bring the capable machinery up: install the backend table, replace inst->ops with the split's own table (it forwards to be->ops while OFF), publish g_gxsp_api, set g_gxsp_cap.
 * Called ONCE, before the first handle exists (rhi_gxm_create on the console, the harness on the host).  Allocation happens lazily at the first enable.  1 ok. */
int gxsp_cap_init(const GxspBackend *be);
/* host tests: configure sizes before the first enable (0 = default), enable the audit (a hash of every struct travels with the draw and the back compares its mirror) */
void gxsp_host_config(uint32_t ring_bytes, uint32_t arena_bytes, int audit);
#ifdef GXT_HOST
/* host harness only: called by the executor on the back before each record (the A1 jitter on the front/back boundary) */
extern void (*gxsp_hook_back_record)(uint32_t op);
#endif

#ifdef __cplusplus
}
#endif
#endif /* GX_RT_SPLIT_BE_H */
