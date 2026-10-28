/* gx_dump.h - the ONE-FRAME DRAW-STREAM DUMP WRITER (docs/54 s5.1 H1-pre (b)).  Header-only, platform-free: no GL, no psp2, no stdio.
 *
 * Lives in the Stairfax game as P/src/gx_dump.h (the B1-c2floor patch adds it) and, byte-identical, in projects/gxaot/probe/c2floor/src
 * (tests/check_sync.sh fails if the two differ).  The game side (P/src/rhi_vita_gxdump.inc) does the GL reads (program binary, mesh bytes) and
 * hands plain pointers to this file; this file only formats and checksums.  The format is gxd_format.h.
 *
 * OFF BY DEFAULT, ZERO WORK WHEN OFF: every hook is `if (__builtin_expect(g_gxd_on, 0)) ...` -- one load and one predicted branch per draw
 * (NOT measured; the claim is the instruction count, ~2).  Nothing is allocated until gxd_begin().
 *
 * BUDGET.  GXD_MAX_BYTES / GXD_MAX_DRAWS bound one frame; past either, the dump is marked TRUNCATED (header + END flag) and the replay app
 * REFUSES it -- a partial frame must never be mistaken for the ch.2 frame.
 */
#ifndef GX_DUMP_H
#define GX_DUMP_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "port/gx_tev_key.h"
#include "gxd_format.h"

#ifndef GXD_MAX_BYTES
#define GXD_MAX_BYTES (56u << 20)
#endif
#ifndef GXD_MAX_DRAWS
#define GXD_MAX_DRAWS 4096u
#endif
#ifndef GXD_MAXP
#define GXD_MAXP 768u
#endif
#ifndef GXD_MAXM
#define GXD_MAXM 4096u
#endif
#ifndef GXD_MAXT
#define GXD_MAXT 2048u
#endif

typedef int (*GxdWriteFn)(void *ctx, const void *p, unsigned n);   /* returns the bytes written; fewer than n = failure */

static int g_gxd_on;                      /* 1 while a frame is being recorded; the ONLY thing the hooks read */
static struct {
    unsigned char *buf; size_t len, cap;
    int truncated;
    uint32_t nprog, nmesh, ntex, ndraw, nevent, dropped;
    uint32_t pkey[GXD_MAXP];
    const void *mkey[GXD_MAXM];
    const void *tkey[GXD_MAXT];
} gxd_s;

static uint32_t gxd_fnv(const unsigned char *p, size_t n)
{
    uint32_t a = 2166136261u;
    for (size_t i = 0; i < n; ++i) a = (a ^ p[i]) * 16777619u;
    return a;
}

/* a record of `len` payload bytes (any len; the file keeps it padded to 4); returns the payload pointer or NULL when over budget */
static unsigned char *gxd_rec_(uint32_t tag, uint32_t len, int is_end)
{
    const uint32_t pl = GXD_PAD4(len);
    if (gxd_s.truncated && !is_end) return NULL;
    /* every ordinary record leaves room for the END record; the END itself is only bounded by the buffer */
    if (!is_end && gxd_s.len + sizeof(GxdRecHdr) + pl + sizeof(GxdRecHdr) + sizeof(GxdEnd) > GXD_MAX_BYTES) { gxd_s.truncated = 1; return NULL; }
    if (gxd_s.len + sizeof(GxdRecHdr) + pl > gxd_s.cap) {
        size_t nc = gxd_s.cap ? gxd_s.cap : (1u << 20);
        while (nc < gxd_s.len + sizeof(GxdRecHdr) + pl) nc *= 2u;
        unsigned char *nb = (unsigned char *) realloc(gxd_s.buf, nc);
        if (!nb) { gxd_s.truncated = 1; return NULL; }
        gxd_s.buf = nb; gxd_s.cap = nc;
    }
    GxdRecHdr h = { tag, pl };
    memcpy(gxd_s.buf + gxd_s.len, &h, sizeof h);
    unsigned char *p = gxd_s.buf + gxd_s.len + sizeof h;
    memset(p, 0, pl);                       /* padding is zero: a dump is a pure function of its inputs */
    gxd_s.len += sizeof h + pl;
    return p;
}
#define gxd_rec(tag, len) gxd_rec_((tag), (len), 0)

/* Start a frame.  `build` may be NULL.  Returns 1 if recording began. */
static int gxd_begin(uint32_t render_w, uint32_t render_h, uint32_t present_no, const char *build, uint32_t meta_flags)
{
    if (g_gxd_on) return 0;
    free(gxd_s.buf);
    memset(&gxd_s, 0, sizeof gxd_s);
    gxd_s.buf = (unsigned char *) malloc(1u << 20);
    if (!gxd_s.buf) return 0;
    gxd_s.cap = 1u << 20;
    GxdFileHdr fh = { GXD_MAGIC, GXD_VERSION, 0, 0 };
    memcpy(gxd_s.buf, &fh, sizeof fh);
    gxd_s.len = sizeof fh;
    GxdMeta m;
    memset(&m, 0, sizeof m);
    m.size_VsKey = (uint32_t) sizeof(VsKey); m.size_TevKey = (uint32_t) sizeof(TevKey);
    m.size_VsUniforms = (uint32_t) sizeof(VsUniforms); m.size_TevUniforms = (uint32_t) sizeof(TevUniforms);
    m.size_TevVertex = (uint32_t) sizeof(TevVertex); m.size_GxMeshLayout = (uint32_t) sizeof(GxMeshLayout);
    m.flags = meta_flags;
    m.tevgen_version = TEVGEN_VERSION; m.render_w = render_w; m.render_h = render_h; m.present_no = present_no;
    if (build) { size_t n = strlen(build); if (n > sizeof m.build - 1) n = sizeof m.build - 1; memcpy(m.build, build, n); }
    unsigned char *p = gxd_rec(GXD_T_META, (uint32_t) sizeof m);
    if (!p) { free(gxd_s.buf); memset(&gxd_s, 0, sizeof gxd_s); return 0; }
    memcpy(p, &m, sizeof m);
    g_gxd_on = 1;
    return 1;
}

/* ordinals: -1 = not recorded yet */
static int gxd_prog_find(uint32_t key) { for (uint32_t i = 0; i < gxd_s.nprog; ++i) if (gxd_s.pkey[i] == key) return (int) i; return -1; }
static int gxd_prog_add(uint32_t key, uint32_t hash, uint32_t tevhash, uint32_t kind, const void *blob, uint32_t bloblen)
{
    if (!g_gxd_on) return -1;
    if (gxd_s.nprog >= GXD_MAXP) { gxd_s.truncated = 1; return -1; }
    unsigned char *p = gxd_rec(GXD_T_PROG, (uint32_t) sizeof(GxdProg) + bloblen);
    if (!p) return -1;
    GxdProg h = { key, hash, tevhash, kind, bloblen, 0 };
    memcpy(p, &h, sizeof h);
    if (bloblen) memcpy(p + sizeof h, blob, bloblen);
    gxd_s.pkey[gxd_s.nprog] = key;
    return (int) gxd_s.nprog++;
}
static int gxd_mesh_find(const void *key) { for (uint32_t i = 0; i < gxd_s.nmesh; ++i) if (gxd_s.mkey[i] == key) return (int) i; return -1; }
static int gxd_mesh_add(const void *key, const GxMeshLayout *lay, const void *verts, uint32_t vbytes, const uint16_t *idx, uint32_t nidx)
{
    if (!g_gxd_on) return -1;
    if (gxd_s.nmesh >= GXD_MAXM) { gxd_s.truncated = 1; return -1; }
    const uint32_t lb = GXD_PAD4(sizeof *lay), vb = GXD_PAD4(vbytes), ib = GXD_PAD4(nidx * 2u);
    unsigned char *p = gxd_rec(GXD_T_MESH, (uint32_t) sizeof(GxdMesh) + lb + vb + ib);
    if (!p) return -1;
    GxdMesh h = { (uint32_t) (uintptr_t) key, vbytes, nidx, 0 };
    memcpy(p, &h, sizeof h); p += sizeof h;
    memcpy(p, lay, sizeof *lay); p += lb;
    if (vbytes) memcpy(p, verts, vbytes);
    p += vb;
    if (nidx) memcpy(p, idx, nidx * 2u);
    gxd_s.mkey[gxd_s.nmesh] = key;
    return (int) gxd_s.nmesh++;
}
static int gxd_tex_find(const void *key) { for (uint32_t i = 0; i < gxd_s.ntex; ++i) if (gxd_s.tkey[i] == key) return (int) i; return -1; }
static int gxd_tex_add(const void *key, const GxdTex *t)
{
    if (!g_gxd_on) return -1;
    if (gxd_s.ntex >= GXD_MAXT) { gxd_s.truncated = 1; return -1; }
    unsigned char *p = gxd_rec(GXD_T_TEX, (uint32_t) sizeof *t);
    if (!p) return -1;
    GxdTex c = *t; c.key_lo = (uint32_t) (uintptr_t) key;
    memcpy(p, &c, sizeof c);
    gxd_s.tkey[gxd_s.ntex] = key;
    return (int) gxd_s.ntex++;
}

static void gxd_fill_draw(GxdDraw *d, uint32_t kind, int prog, int mesh, uint32_t alpha, uint32_t depth, uint32_t scene_kind, const int *tex, uint32_t ntex, uint32_t nverts)
{
    memset(d, 0, sizeof *d);
    d->kind = kind; d->seq = gxd_s.ndraw; d->prog = (uint32_t) prog; d->mesh = (uint32_t) mesh;
    d->alpha_mode = alpha; d->depth = depth; d->scene_kind = scene_kind;
    d->ntex = ntex > GXD_MAX_TEX ? GXD_MAX_TEX : ntex;
    for (uint32_t i = 0; i < d->ntex; ++i) d->tex[i] = (uint32_t) tex[i];
    d->nverts = nverts;
}

/* kind 1: the GPU-path draw (rhi_vita_drawTevGx).  prog/mesh/tex[] are the ordinals the add/find calls returned (all >= 0). */
static int gxd_draw_gx(int prog, int mesh, const int *tex, uint32_t ntex, uint32_t alpha, uint32_t depth, uint32_t scene_kind,
                       const VsKey *vk, const TevKey *tk, const VsUniforms *vu, const TevUniforms *u)
{
    if (!g_gxd_on) return -1;
    if (gxd_s.ndraw >= GXD_MAX_DRAWS) { ++gxd_s.dropped; gxd_s.truncated = 1; return -1; }
    const uint32_t n = (uint32_t) (sizeof(GxdDraw) + GXD_PAD4(sizeof *vk) + GXD_PAD4(sizeof *tk) + GXD_PAD4(sizeof *vu) + GXD_PAD4(sizeof *u));
    unsigned char *p = gxd_rec(GXD_T_DRAW, n);
    if (!p) { ++gxd_s.dropped; return -1; }
    GxdDraw d; gxd_fill_draw(&d, GXD_K_GX, prog, mesh, alpha, depth, scene_kind, tex, ntex, 0);
    memcpy(p, &d, sizeof d); p += sizeof d;
    memcpy(p, vk, sizeof *vk); p += GXD_PAD4(sizeof *vk);
    memcpy(p, tk, sizeof *tk); p += GXD_PAD4(sizeof *tk);
    memcpy(p, vu, sizeof *vu); p += GXD_PAD4(sizeof *vu);
    memcpy(p, u, sizeof *u);
    return (int) gxd_s.ndraw++;
}

/* kind 2: an immediate-path call (rhi_vita_drawTev): the vertices ride along. */
static int gxd_draw_tev(int prog, const int *tex, uint32_t ntex, uint32_t alpha, uint32_t depth, uint32_t scene_kind,
                        const TevKey *tk, const TevUniforms *u, const float mvp[16], const TevVertex *verts, uint32_t nverts)
{
    if (!g_gxd_on) return -1;
    if (gxd_s.ndraw >= GXD_MAX_DRAWS) { ++gxd_s.dropped; gxd_s.truncated = 1; return -1; }
    const uint32_t n = (uint32_t) (sizeof(GxdDraw) + GXD_PAD4(sizeof *tk) + GXD_PAD4(sizeof *u) + 64u + GXD_PAD4(nverts * sizeof *verts));
    unsigned char *p = gxd_rec(GXD_T_DRAW, n);
    if (!p) { ++gxd_s.dropped; return -1; }
    GxdDraw d; gxd_fill_draw(&d, GXD_K_TEV, prog, -1, alpha, depth, scene_kind, tex, ntex, nverts);
    memcpy(p, &d, sizeof d); p += sizeof d;
    memcpy(p, tk, sizeof *tk); p += GXD_PAD4(sizeof *tk);
    memcpy(p, u, sizeof *u); p += GXD_PAD4(sizeof *u);
    memcpy(p, mvp, 64u); p += 64u;
    if (nverts) memcpy(p, verts, nverts * sizeof *verts);
    return (int) gxd_s.ndraw++;
}

static int gxd_event(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    if (!g_gxd_on) return -1;
    unsigned char *p = gxd_rec(GXD_T_EVENT, (uint32_t) sizeof(GxdEvent));
    if (!p) return -1;
    GxdEvent ev = { kind, gxd_s.ndraw, a, b, c, d, e, f };
    memcpy(p, &ev, sizeof ev);
    ++gxd_s.nevent;
    return 0;
}

/* Close the frame: END record (counts, TRUNCATED flag, checksum), then hand the bytes to `w` in <= 1 MB writes.  Returns the file size, or -1
 * if `w` failed or nothing was recorded.  Always leaves the writer idle (g_gxd_on = 0, memory freed).  *out_truncated gets the TRUNCATED flag. */
static long gxd_end(GxdWriteFn w, void *ctx, int *out_truncated)
{
    long rc = -1;
    const int trunc = gxd_s.truncated;
    g_gxd_on = 0;                           /* stop recording first: the hooks must not append while the END goes in */
    if (out_truncated) *out_truncated = trunc;
    if (gxd_s.buf) {
        if (trunc) { GxdFileHdr fh; memcpy(&fh, gxd_s.buf, sizeof fh); fh.flags |= GXD_FLAGS_TRUNCATED; memcpy(gxd_s.buf, &fh, sizeof fh); }
        unsigned char *p = gxd_rec_(GXD_T_END, (uint32_t) sizeof(GxdEnd), 1);
        if (p) {
            GxdEnd e = { gxd_s.ndraw, gxd_s.nprog, gxd_s.nmesh, gxd_s.ntex, gxd_s.nevent, trunc ? GXD_FLAGS_TRUNCATED : 0u, gxd_s.dropped, 0u };
            e.fnv32 = gxd_fnv(gxd_s.buf, gxd_s.len - sizeof(GxdRecHdr) - sizeof e);
            memcpy(p, &e, sizeof e);
            size_t off = 0;
            rc = (long) gxd_s.len;
            while (off < gxd_s.len) {
                const unsigned n = (unsigned) ((gxd_s.len - off) > (1u << 20) ? (1u << 20) : (gxd_s.len - off));
                if (w(ctx, gxd_s.buf + off, n) != (int) n) { rc = -1; break; }
                off += n;
            }
        }
    }
    free(gxd_s.buf);
    memset(&gxd_s, 0, sizeof gxd_s);
    return rc;
}

static uint32_t gxd_ndraws(void) { return gxd_s.ndraw; }
static uint32_t gxd_nbytes(void) { return (uint32_t) gxd_s.len; }

#endif
