/* rhi_gxm_shader.c - the SHADER module of the native GXM backend, milestone M2 (design: host/engine/GXM_M2.md section 6; contract: rhi_gxm_int.h "shader module").
 *
 * WHAT IT OWNS. GxmProg get-or-create for the TEV programs (gxm_shader_tev), the GX vertex+TEV pairs (gxm_shader_gx), the three fixed-function
 * equivalents (gxm_shader_builtin: synthesised TevKeys through the TEV path, no new Cg), and the util programs (clear, blit, EFB copy). Where a
 * program comes from, in this order, for a key the table has not seen:
 *   1. THE BAD LIST   ux0:data/STFX00001/GXM_BAD (a compile that took the process down on an earlier boot): never compiled again.
 *   2. THE PACK       ux0:data/STFX00001/shc/pack.bin, the vitaGL program binaries of earlier boots, indexed once at init, a record read on first use.
 *                     Its identity is the DEVICE's own (canonical key, td_get; hash exactly as tev_program / gxp_program) or it never hits.
 *   3. THE GXP CACHE  ux0:data/STFX00001/gxm/<kind><hash>.gxp, written after a console compile (so a console boot feeds an emulator).
 *   4. THE COMPILER   console only: vitaShaRK, fenced by a crash sentinel (GXM_COMPILING -> GXM_BAD). Since 2026-10-27 on a BACKGROUND worker (stfx_gxc,
 *                     below the game threads' priority): the key answers NOPROG (the HAL's fallback draws it) until its program lands. gxm_async.txt 0 =
 *                     synchronously on the calling thread, as before.
 * Nothing found = GXM_SK_NOPROG, counted. A hit that cannot be split / checked / registered = GXM_SK_COMPILE (a defect, not a missing input).
 *
 * NOT HERE: eviction (a full table refuses with PROGFULL), the AoT compiler. Single-OWNER by contract: every entry point runs on the backend's thread
 * (the game thread, or R in threaded mode; the Cg buffers, the pool and the one-key memos are the owner's). The compile worker touches only its jobs,
 * the compiler (under the shark lock), the sentinel and the GXP cache files.
 *
 * LAYOUT OF THIS FILE. Part 1 is PURE (no SCE, no GXM): the pack indexer, the record checks, the GXP splitter, the program identity, the builtin keys, the
 * uniform name classifier. `-DGXM_SHADER_HOST` compiles ONLY part 1, so host/tests/gxm_shader_test.c can #include this file and run the real code on the
 * real pack (pack_probe.bin) with mutants. Part 2 is the device code.
 *
 * Evidence tags in comments: [S] read in a file, [M] measured, [E] inference. Nothing here has run on the console. Emulator numbers are never timing claims. */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/gx_tev_key.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "tev_dedupe.h"
#include "shc_queue.h"
#pragma GCC diagnostic pop

#define GXS_PURE static __attribute__((unused))

/* ======================================================================================================================================== PART 1: PURE */
#define GXS_SHC_MAGIC   0x31434853u                                          /* "SHC1": the image inside a pack record (rhi_vita_tev.inc:212) */
#define GXS_SHC_VERSION ((uint32_t) TEVGEN_VERSION << 8 | 1u)                /* 0x301 (rhi_vita_tev.inc:213) */
#define GXS_PK_MAGIC    0x4B504853u                                          /* "SHPK": a pack record header (rhi_vita_tev.inc:437) */
#define GXS_PK_HDR      20u                                                  /* 5 words: magic, h, kind, len, sum */
#define GXS_FAST_SALT   TD_FAST_SALT                                         /* 0x0FA57A57 = TEV_FAST_SALT (rhi_vita_tev.inc:661) */
#define GXS_GXPC_MAGIC  0x31435847u                                          /* "GXC1": a GXP-cache file */
#define GXS_VGL_ATTRS 16                                                     /* vitaGL program binary: u32 attr_highest, 16 x 8-byte attributes, u32 vlen, vertex image, u32 flen, fragment image (c2floor gxd_load.c:15-32) */
#define GXS_VGL_ATTR_BYTES 8
#define GXS_WIN (64u * 1024u)                                                /* the pack indexer's sequential read window */
#define GXS_BI_ATEST_REF 128                                                 /* the alpha reference TEXTURED_ATEST needs in TevUniforms.acRef0 (a uniform, not part of the key) */

/* GXEnum.h values used by the builtin keys (the host test asserts each against include/dolphin/gx/GXEnum.h) */
#define GXS_CC_TEXC 8
#define GXS_CC_RASC 10
#define GXS_CC_ZERO 15
#define GXS_CA_TEXA 4
#define GXS_CA_RASA 5
#define GXS_CA_ZERO 7
#define GXS_GEQUAL  6
#define GXS_ALWAYS  7
#define GXS_AOP_AND 0

static uint32_t gxs_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t gxs_pad4(uint32_t n) { return (n + 3u) & ~3u; }

/* ---- the pack: record sum, index, fetch ------------------------------------------------------------------------------------------------ */
GXS_PURE uint32_t gxs_pk_sum(const uint32_t *w, unsigned nwords)       /* shc_pk_sum (rhi_vita_tev.inc:449) */
{
    uint32_t a = 2166136261u;
    for (unsigned i = 0; i < nwords; ++i) a = (a ^ w[i]) * 16777619u;
    return a;
}
typedef struct { uint32_t h, kind, off, len, sum; } GxsPk;              /* off/len: the file image (after the 20-byte record header), len unpadded */
typedef struct {
    GxsPk *e; unsigned n, cap;
    uint32_t *tab; unsigned tabsz;                                      /* open addressing, entry = index + 1 */
    unsigned dups;                                                      /* records whose (kind, h) was already indexed: skipped (the first wins, as shc_pk_read_file) */
    unsigned reads;                                                     /* read calls the indexer made */
    int stop;                                                           /* 0 clean end, 1 bad record header, 2 truncated record, 3 read error, 4 trailing bytes, 5 out of memory */
    uint32_t stop_off, bytes;
} GxsPkIdx;
typedef int (*GxsReadFn)(void *ctx, uint32_t off, void *dst, uint32_t n);   /* must return exactly n, else the read counts as failed */

static unsigned gxs_pk_hash(uint32_t h, uint32_t kind) { uint32_t x = h * 0x9E3779B1u ^ (kind + 0x7F4A7C15u) * 0x85EBCA6Bu; x ^= x >> 15; return x; }
GXS_PURE const GxsPk *gxs_pk_find(const GxsPkIdx *ix, uint32_t h, uint32_t kind)
{
    if (!ix->tabsz) return NULL;
    unsigned p = gxs_pk_hash(h, kind) & (ix->tabsz - 1);
    for (unsigned c = 0; c < ix->tabsz; ++c, p = (p + 1) & (ix->tabsz - 1)) {
        if (!ix->tab[p]) return NULL;
        const GxsPk *e = &ix->e[ix->tab[p] - 1];
        if (e->h == h && e->kind == kind) return e;
    }
    return NULL;
}
static int gxs_pk_rehash(GxsPkIdx *ix, unsigned want)
{
    unsigned sz = 1024; while (sz < want * 2u) sz <<= 1;
    uint32_t *t = (uint32_t *) calloc(sz, sizeof *t);
    if (!t) return -1;
    for (unsigned i = 0; i < ix->n; ++i) {
        unsigned p = gxs_pk_hash(ix->e[i].h, ix->e[i].kind) & (sz - 1);
        while (t[p]) p = (p + 1) & (sz - 1);
        t[p] = i + 1;
    }
    free(ix->tab); ix->tab = t; ix->tabsz = sz;
    return 0;
}
static int gxs_pk_add(GxsPkIdx *ix, const GxsPk *r)   /* 0 added, 1 duplicate, -1 out of memory */
{
    if (gxs_pk_find(ix, r->h, r->kind)) { ++ix->dups; return 1; }
    if (ix->n == ix->cap) {
        const unsigned nc = ix->cap ? ix->cap * 2u : 4096u;
        GxsPk *ne = (GxsPk *) realloc(ix->e, nc * sizeof *ne);
        if (!ne) return -1;
        ix->e = ne; ix->cap = nc;
    }
    if ((ix->n + 1u) * 2u > ix->tabsz && gxs_pk_rehash(ix, ix->n + 1u)) return -1;
    ix->e[ix->n] = *r;
    unsigned p = gxs_pk_hash(r->h, r->kind) & (ix->tabsz - 1);
    while (ix->tab[p]) p = (p + 1) & (ix->tabsz - 1);
    ix->tab[p] = ++ix->n;
    return 0;
}
GXS_PURE void gxs_pk_free(GxsPkIdx *ix) { free(ix->e); free(ix->tab); memset(ix, 0, sizeof *ix); }
/* Index the whole pack in sequential windows of `wincap` bytes (64 KB on the device): headers only are interpreted, a payload is skipped, its sum is
 * checked when the record is first USED (gxs_pk_fetch). Returns the number of records indexed; ix->stop says why it ended. */
GXS_PURE int gxs_pk_index(GxsPkIdx *ix, GxsReadFn rd, void *ctx, uint32_t size, uint8_t *win, uint32_t wincap)
{
    memset(ix, 0, sizeof *ix);
    if (gxs_pk_rehash(ix, 512)) { ix->stop = 5; return 0; }
    uint32_t pos = 0, wo = 0, wl = 0;
    while (pos + GXS_PK_HDR <= size) {
        if (!(pos >= wo && pos + GXS_PK_HDR <= wo + wl)) {
            const uint32_t n = size - pos < wincap ? size - pos : wincap;
            if (rd(ctx, pos, win, n) != (int) n) { ix->stop = 3; ix->stop_off = pos; return (int) ix->n; }
            ++ix->reads; wo = pos; wl = n;
        }
        const uint8_t *b = win + (pos - wo);
        const uint32_t magic = gxs_rd32(b), h = gxs_rd32(b + 4), kind = gxs_rd32(b + 8), len = gxs_rd32(b + 12), sum = gxs_rd32(b + 16);
        if (magic != GXS_PK_MAGIC || !len) { ix->stop = 1; ix->stop_off = pos; return (int) ix->n; }
        const uint32_t next = pos + GXS_PK_HDR + gxs_pad4(len);
        if (next < pos || next > size) { ix->stop = 2; ix->stop_off = pos; return (int) ix->n; }
        GxsPk r; r.h = h; r.kind = kind; r.off = pos + GXS_PK_HDR; r.len = len; r.sum = sum;
        if (gxs_pk_add(ix, &r) < 0) { ix->stop = 5; ix->stop_off = pos; return (int) ix->n; }
        pos = next;
    }
    ix->bytes = pos;
    if (pos != size) { ix->stop = 4; ix->stop_off = pos; }
    return (int) ix->n;
}
/* Read one record's image (padded to 4 bytes) into a malloc'd buffer and check its sum. NULL: read error (*bad = 0) or sum mismatch (*bad = 1). Caller frees. */
GXS_PURE uint8_t *gxs_pk_fetch(const GxsPk *e, GxsReadFn rd, void *ctx, int *bad)
{
    const uint32_t pl = gxs_pad4(e->len);
    *bad = 0;
    uint8_t *buf = (uint8_t *) malloc(pl + 4u);
    if (!buf) return NULL;
    if (rd(ctx, e->off, buf, pl) != (int) pl) { free(buf); return NULL; }
    if (gxs_pk_sum((const uint32_t *) (const void *) buf, pl / 4u) != e->sum) { free(buf); *bad = 1; return NULL; }   /* malloc'd: 4-aligned */
    return buf;
}
/* The vitaGL program binary inside a record image: the image is valid for these key bytes (shc_queue.h:137, the device's own check) and where the binary is. 0 = no. */
GXS_PURE int gxs_img_binary(const uint8_t *img, uint32_t len, const void *k1, unsigned n1, const void *k2, unsigned n2, const uint8_t **bin, uint32_t *binlen)
{
    if (!shc_image_valid(img, len, GXS_SHC_MAGIC, GXS_SHC_VERSION, k1, n1, k2, n2)) return 0;
    const uint32_t hdr = 16u + gxs_pad4(n1 + n2);
    *bin = img + hdr; *binlen = gxs_rd32(img + 12);
    return 1;
}
/* vitaGL program binary -> the vertex and fragment GXPs (c2floor gxd_prog_split, hardware-verified). 0 ok; 1..6 where it failed. */
GXS_PURE int gxs_prog_split(const uint8_t *blob, uint32_t bloblen, const uint8_t **vgxp, uint32_t *vlen, const uint8_t **fgxp, uint32_t *flen)
{
    const uint32_t a = 4u + GXS_VGL_ATTRS * GXS_VGL_ATTR_BYTES;
    if (bloblen < a + 8u) return 1;
    const uint32_t vl = gxs_rd32(blob + a);
    if (vl < 16u || a + 4u + vl + 4u > bloblen) return 2;
    const uint8_t *v = blob + a + 4u;
    const uint32_t fl = gxs_rd32(v + vl);
    if (fl < 16u || a + 4u + vl + 4u + fl > bloblen) return 3;
    const uint8_t *f = v + vl + 4u;
    for (int k = 0; k < 2; ++k) {
        const uint8_t *img = k ? f : v; const uint32_t il = k ? fl : vl;
        uint32_t o = 0;
        for (; o <= 60u && o + 8u <= il; o += 4u) if (!memcmp(img + o, "GXP\0", 4)) break;
        if (o > 60u || o + 8u > il) return 4 + k;
        if (k) { *fgxp = img + o; *flen = il - o; } else { *vgxp = img + o; *vlen = il - o; }
    }
    return 0;
}
/* a GX record's key bytes -> VsKey then TevKey (the host test needs it; the device already has both keys in hand) */
GXS_PURE int gxs_split_gx_key(const uint8_t *key, unsigned klen, VsKey *vk, TevKey *tk)
{
    const unsigned cand[3] = { (unsigned) offsetof(VsKey, projFrag), (unsigned) offsetof(VsKey, hasBin), (unsigned) sizeof(VsKey) };
    for (int i = 0; i < 3; ++i) {
        const unsigned n1 = cand[i];
        if (n1 > klen || klen - n1 > sizeof(TevKey)) continue;
        memset(vk, 0, sizeof *vk); memcpy(vk, key, n1);
        if (tev_vs_key_len(vk) != n1) continue;
        memset(tk, 0, sizeof *tk); memcpy(tk, key + n1, klen - n1);
        if (tev_key_len(tk) != klen - n1) continue;
        return (int) n1;
    }
    return -1;
}

/* ---- program identity (the device's own; rhi_vita_tev.inc:1030-1041 and :1455-1461) ------------------------------------------------------ */
/* TEV: canonical key through td_get, h = th ^ (fast ? salt : 0). The keys and hash are COPIED out (td_get's answer is valid until its next call). */
GXS_PURE void gxs_tev_identity(Td *td, const TevKey *raw, int fastOn, TevKey *ck, uint32_t *h, int *fast)
{
    const TdCan *cn = td_get(td, raw, fastOn);
    *ck = cn->ck; *fast = cn->fast; *h = cn->th ^ (cn->fast ? GXS_FAST_SALT : 0u);
}
/* GX pair: persp_on() sets vkp.projFrag and tkp.projMask = tev_vs_proj_mask(raw vk); h = (th ^ saltIfFast) ^ (tev_vs_key_hash(&vk) * 0x9E3779B9). */
GXS_PURE void gxs_gx_identity(Td *td, const VsKey *rvk, const TevKey *rtk, int fastOn, int persp, VsKey *vk, TevKey *ck, uint32_t *h, int *fast)
{
    TevKey tkp = *rtk;
    *vk = *rvk;
    if (persp) { const unsigned pm = tev_vs_proj_mask(rvk); if (pm) { vk->projFrag = 1; tkp.projMask = (uint8_t) pm; } }
    const TdCan *cn = td_get(td, &tkp, fastOn);
    *ck = cn->ck; *fast = cn->fast;
    *h = (cn->th ^ (cn->fast ? GXS_FAST_SALT : 0u)) ^ (tev_vs_key_hash(vk) * 0x9E3779B9u);
}
/* a second, independent hash of a canonical key (the negative cache holds no key bytes: (family, fast, h, h2) is its identity) */
GXS_PURE uint32_t gxs_key_h2(const VsKey *vk, const TevKey *tk)
{
    uint32_t a = 0x811C9DC5u ^ 0x5BD1E995u;
    if (vk) { const uint8_t *p = (const uint8_t *) vk; for (size_t i = 0, n = tev_vs_key_len(vk); i < n; ++i) a = (a ^ p[i]) * 16777619u; }
    { const uint8_t *p = (const uint8_t *) tk; for (size_t i = 0, n = tev_key_len(tk); i < n; ++i) a = (a ^ p[i]) * 16777619u; }
    return a;
}

/* ---- builtin keys: GXM_BI_COLORED / TEXTURED / TEXTURED_ATEST (GXM_M2.md section 6; field fill as hal/gx_tev.c:243-262) ------------------------ */
GXS_PURE void gxs_builtin_key(int which, TevKey *k)    /* which: GXM_BI_* = 0 colored, 1 textured, 2 textured + alpha test */
{
    static const uint8_t swap[4][4] = { {0, 1, 2, 3}, {0, 0, 0, 3}, {1, 1, 1, 3}, {2, 2, 2, 3} };   /* gx_tev.c:48 gSwapTbl */
    memset(k, 0, sizeof *k);
    k->numStages = 1;
    memcpy(k->swap, swap, sizeof k->swap);
    k->acComp0 = GXS_ALWAYS; k->acComp1 = GXS_ALWAYS; k->acOp = GXS_AOP_AND;     /* a zeroed key would be GX_NEVER and discard everything */
    TevKeyStage *s = &k->st[0];
    s->ras = 0;                                                                   /* GX_COLOR0A0 */
    s->texSlot = 0xff;
    s->cop[0] = 0; s->cop[1] = 0; s->cop[2] = 0; s->cop[3] = 1; s->cop[4] = 0;     /* ADD, bias ZERO, scale 1, clamp, out TEVPREV */
    s->aop[0] = 0; s->aop[1] = 0; s->aop[2] = 0; s->aop[3] = 1; s->aop[4] = 0;
    if (which == 0) {      /* tev = RASC (d + a*(1-c) + b*c with a=b=c=0, d=RASC) */
        s->cin[0] = GXS_CC_ZERO; s->cin[1] = GXS_CC_ZERO; s->cin[2] = GXS_CC_ZERO; s->cin[3] = GXS_CC_RASC;
        s->ain[0] = GXS_CA_ZERO; s->ain[1] = GXS_CA_ZERO; s->ain[2] = GXS_CA_ZERO; s->ain[3] = GXS_CA_RASA;
    } else {               /* tev = TEX * RAS (a*(1-c) + b*c + d with a=d=0, b=TEX, c=RAS) */
        k->numTex = 1; s->texSlot = 0;
        s->cin[0] = GXS_CC_ZERO; s->cin[1] = GXS_CC_TEXC; s->cin[2] = GXS_CC_RASC; s->cin[3] = GXS_CC_ZERO;
        s->ain[0] = GXS_CA_ZERO; s->ain[1] = GXS_CA_TEXA; s->ain[2] = GXS_CA_RASA; s->ain[3] = GXS_CA_ZERO;
        if (which == 2) { k->acComp0 = GXS_GEQUAL; }                              /* a >= acRef0 (GXS_BI_ATEST_REF, a uniform); acOp AND, acComp1 ALWAYS */
    }
}

/* ---- uniform names -> kinds (c2floor classify, hardware-verified); the kind numbers are the GU_* of rhi_gxm_int.h, repeated here so part 1 stays pure ---- */
enum { GXS_U_NONE = 0, GXS_U_M, GXS_U_POS, GXS_U_NRM, GXS_U_TMX, GXS_U_MAT, GXS_U_AMB, GXS_U_LT, GXS_U_TM, GXS_U_PT, GXS_U_R, GXS_U_K, GXS_U_AREF, GXS_U_FOG, GXS_U_FOGC, GXS_U_IMA, GXS_U_IMB, GXS_U_TS, GXS_U_BONE };
GXS_PURE int gxs_classify(const char *n, int frag, uint8_t *idx)
{
    int a = 0; char c = 0;
    *idx = 0;
    if (!frag) {
        if (sscanf(n, "uM%1d%c", &a, &c) == 1 && a < 4 && strlen(n) == 3) { *idx = (uint8_t) a; return GXS_U_M; }
        if (!strcmp(n, "uPos")) return GXS_U_POS;
        if (!strcmp(n, "uNrm")) return GXS_U_NRM;
        if (!strcmp(n, "uTMX")) return GXS_U_TMX;
        if (!strcmp(n, "uBone")) return GXS_U_BONE;   /* R8 GPU skinning: the bone palette + scale row */
        if (!strcmp(n, "uMat")) return GXS_U_MAT;
        if (!strcmp(n, "uAmb")) return GXS_U_AMB;
        if (n[0] == 'u' && (n[1] == 'C' || n[1] == 'A') && n[2] == 'L') {
            static const char *const f[5] = { "pos", "dir", "a", "k", "col" };
            for (int q = 0; q < 5; ++q) if (!strcmp(n + 3, f[q])) { *idx = (uint8_t) (q | (n[1] == 'A' ? 16 : 0)); return GXS_U_LT; }
        }
        if (sscanf(n, "uTM%1d%c", &a, &c) == 1 && a < 8 && strlen(n) == 4) { *idx = (uint8_t) a; return GXS_U_TM; }
        if (sscanf(n, "uPT%1d%c", &a, &c) == 1 && a < 8 && strlen(n) == 4) { *idx = (uint8_t) a; return GXS_U_PT; }
    } else {
        if (sscanf(n, "uR%1d%c", &a, &c) == 1 && a < 4 && strlen(n) == 3) { *idx = (uint8_t) a; return GXS_U_R; }
        if (sscanf(n, "uK%1d%c", &a, &c) == 1 && a < 4 && strlen(n) == 3) { *idx = (uint8_t) a; return GXS_U_K; }
        if (!strcmp(n, "uAref")) return GXS_U_AREF;
        if (!strcmp(n, "uFog")) return GXS_U_FOG;
        if (!strcmp(n, "uFogColor")) return GXS_U_FOGC;
        if (strlen(n) == 5 && n[0] == 'u' && n[1] == 'I' && n[2] == 'M' && n[3] >= '0' && n[3] <= '2' && (n[4] == 'a' || n[4] == 'b')) { *idx = (uint8_t) (n[3] - '0'); return n[4] == 'a' ? GXS_U_IMA : GXS_U_IMB; }
        if (sscanf(n, "uTS%1d%c", &a, &c) == 1 && a < 8 && strlen(n) == 4) { *idx = (uint8_t) a; return GXS_U_TS; }
    }
    return GXS_U_NONE;
}
/* which sampler slots the generator actually samples (c2floor c2f_slots.h, 2026-10-20): a slot named only by GX_TEXMAP_DISABLE indirect stages has no sampler BY DESIGN */
GXS_PURE unsigned gxs_slots_read(const TevKey *k)
{
    unsigned disabled = 0, other = 0;
    const int n = k->numStages < TEVK_STAGES ? k->numStages : TEVK_STAGES;
    for (int s = 0; s < n; ++s) {
        const int act = k->ik.on && k->ik.st[s][0];
        if (act) { const unsigned sl = k->ik.slot[k->ik.st[s][1] & 3]; if (sl < 32) other |= 1u << sl; }
        const unsigned ts = k->st[s].texSlot;
        if (ts < 32) { if (act && (k->ik.st[s][4] & 4)) disabled |= 1u << ts; else other |= 1u << ts; }
    }
    const unsigned all = k->numTex >= 32 ? 0xffffffffu : (1u << k->numTex) - 1u;
    return all & ~(disabled & ~other);
}
/* the GXP-cache file: {magic, version, klen (n1 + n2), vlen, flen} + key bytes + vertex GXP + fragment GXP, each part padded to 4. 1 = valid for these key bytes. */
GXS_PURE int gxs_gxpc_valid(const uint8_t *img, uint32_t n, const void *k1, unsigned n1, const void *k2, unsigned n2, const uint8_t **v, uint32_t *vl, const uint8_t **f, uint32_t *fl)
{
    if (n < 20u) return 0;
    if (gxs_rd32(img) != GXS_GXPC_MAGIC || gxs_rd32(img + 4) != GXS_SHC_VERSION || gxs_rd32(img + 8) != n1 + n2) return 0;
    const uint32_t kl = gxs_pad4(n1 + n2), a = gxs_rd32(img + 12), b = gxs_rd32(img + 16);
    if (a < 16u || b < 16u || a > n || b > n) return 0;
    if ((uint64_t) 20u + kl + gxs_pad4(a) + gxs_pad4(b) != n) return 0;
    if (memcmp(img + 20, k1, n1) || (n2 && memcmp(img + 20 + n1, k2, n2))) return 0;
    *v = img + 20 + kl; *vl = a; *f = *v + gxs_pad4(a); *fl = b;
    return 1;
}

/* ======================================================================================================================================== PART 2: DEVICE */
#ifndef GXM_SHADER_HOST
#include "rhi_gxm_int.h"
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitashark.h>

#define GXS_DIR        "ux0:data/STFX00001/"
#define GXS_PACK       GXS_DIR "shc/pack.bin"
#define GXS_NOPACK     GXS_DIR "NOPACK"
#define GXS_SENT       GXS_DIR "GXM_COMPILING"
#define GXS_SENT_CG    GXS_DIR "gxm_compiling.cg"
#define GXS_BADFILE    GXS_DIR "GXM_BAD"
#define GXS_GXPDIR     GXS_DIR "gxm/"
/* THE APP'S OWN SHADERS (2026-10-28, the public release): the vpk carries the release pack and GXP cache under app0:shaders/, read-only.
 * A key the memory-card cache does not have is looked up there next, so "vpk + disc image" is the whole install. New compiles are still
 * written to the memory card (GXS_GXPDIR); a card copy wins over the app's, so a later shader fix only needs the card file deleted. */
#define GXS_APP_DIR    "app0:shaders/"
#define GXS_APP_PACK   GXS_APP_DIR "pack.bin"
#define GXS_APP_GXPDIR GXS_APP_DIR "gxm/"
#define GXS_FASTFILE   "ux0:data/stairfax/tev_fast.txt"
#define GXS_PERSPFILE  GXS_DIR "texgen_persp_off.txt"
#define GXS_DEDUPEFILE GXS_DIR "tev_dedupe_off.txt"
#define GXS_NOSHARK    "ux0:data/stairfax/gxm_noshark.txt"     /* first byte '1': never call shark_init (a safety switch for an emulator that misbehaves on the module load) */
#define GXS_BAD_MAX    1024
#define GXS_NEG_N      16384
#define GXS_CHUNK      64                                       /* GxmProg per allocation chunk: pointers stay stable, memory follows use */

typedef struct { char name[12]; uint16_t off; uint8_t fmt, comps; } GxsAttrC;
typedef struct { SceGxmVertexProgram *vp; int stride, n; GxsAttrC *a; } GxsVp;
typedef struct { SceGxmFragmentProgram *fp; uint32_t bk; } GxsFp;
typedef struct {
    GxsVp *vp; int nvp, capvp;
    GxsFp *fp; int nfp, capfp;
    SceGxmFragmentProgram *nowrite;
    int owned;          /* vg / fg are memalign'd copies this module frees */
    int reg;            /* both programs registered with the patcher */
} GxsPriv;
typedef struct { GxmProg p; GxsPriv pv; char nm[24]; } GxsSlot;
typedef struct { uint32_t h, h2; uint8_t fam, fast, why, used; } GxsNeg;

/* ---- state (module-private; `up` is the only gate) ---- */
static int g_up;
static int g_fast, g_persp, g_dedupe;
static Td g_td;                                                  /* ~380 KB: the canonical-key map */
static GxsSlot *g_chunk[(GXM_PROG_MAX + GXS_CHUNK - 1) / GXS_CHUNK];
static int g_np;                                                 /* programs alive (pool entries) */
static uint16_t g_tab[4096];                                     /* program hash -> pool index + 1 (families TEV and GX only) */
static GxsNeg *g_neg;
static uint32_t g_bad[GXS_BAD_MAX]; static unsigned g_bad_n;
static GxsPkIdx g_pk; static SceUID g_pk_fd = -1; static uint32_t g_pk_size;
static int g_shark_on;
static int g_pack_on;
static unsigned c_pack_hit, c_pack_miss, c_pack_stale, c_pack_reads, c_pack_bad, c_shark_compiles, c_shark_fail, c_gxpc_hit, c_gxpc_app, c_vp_made, c_fp_made, c_vp_fail, c_fp_fail;
static unsigned c_unmapped_unif, c_unmapped_attr, c_missing_samplers, c_refused_gen, c_noprog_bad, c_prog_full, c_arm_noprog, c_sent_fail, c_gxpc_write, c_noop;
static unsigned c_compile_fail, c_split_fail;
static char g_unmapped_log[8][40]; static unsigned g_unmapped_logged;

static GxsSlot *gxs_slot(int i) { return &g_chunk[i / GXS_CHUNK][i % GXS_CHUNK]; }
static void gxs_forget_memos(void);
static void gxs_mm_clear(void);

/* ---- small file helpers ---- */
static int gxs_read_small(const char *path, char *buf, int cap)       /* bytes read (0 if absent / empty); buf is NUL-terminated */
{
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    int n = 0;
    if (f >= 0) { n = sceIoRead(f, buf, (unsigned) (cap - 1)); sceIoClose(f); }
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}
static int gxs_marker_is_1(const char *path) { char b[8]; return gxs_read_small(path, b, sizeof b) > 0 && b[0] == '1'; }
static int gxs_exists(const char *path) { SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777); if (f < 0) return 0; sceIoClose(f); return 1; }
static int gxs_write_file(const char *path, const void *a, unsigned na, const void *b, unsigned nb, const void *c, unsigned nc)   /* 0 ok */
{
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f < 0) return -1;
    int w = 0, want = 0;
    if (na) { w += sceIoWrite(f, a, na); want += (int) na; }
    if (nb) { w += sceIoWrite(f, b, nb); want += (int) nb; }
    if (nc) { w += sceIoWrite(f, c, nc); want += (int) nc; }
    sceIoClose(f);
    return w == want ? 0 : -1;
}

/* ---- the bad list and the crash sentinel (pattern: rhi_vita_tev.inc:162-196, rhi_vita_efb.inc:225-253) ---- */
static int gxs_is_bad(uint32_t h) { for (unsigned i = 0; i < g_bad_n; ++i) if (g_bad[i] == h) return 1; return 0; }
static void gxs_bad_add(uint32_t h, int persist)
{
    if (g_bad_n < GXS_BAD_MAX) g_bad[g_bad_n++] = h;
    if (!persist) return;
    SceUID f = sceIoOpen(GXS_BADFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (f >= 0) { char b[16]; const int n = snprintf(b, sizeof b, "%08x\n", (unsigned) h); sceIoWrite(f, b, (unsigned) n); sceIoClose(f); }
}
static void gxs_sentinel_recover(void)
{
    char b[24];
    if (gxs_read_small(GXS_SENT, b, sizeof b) > 0 || gxs_exists(GXS_SENT)) {
        const uint32_t h = (uint32_t) strtoul(b, NULL, 16);
        sceIoRemove(GXS_SENT);
        if (h) { gxs_bad_add(h, 1); gxm_log("[gxm-shader] the previous boot died compiling key %08x: marked bad (GXM_BAD); its Cg is %s", (unsigned) h, GXS_SENT_CG); }
    }
    static char lb[GXS_BAD_MAX * 9 + 16];
    const int n = gxs_read_small(GXS_BADFILE, lb, (int) sizeof lb);
    if (n > 0) {
        for (const char *p = lb; *p;) {
            char *e; const uint32_t h = (uint32_t) strtoul(p, &e, 16);
            if (e == p) break;
            if (h && !gxs_is_bad(h)) gxs_bad_add(h, 0);
            p = e; while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
        }
    }
}

/* ---- the pack ---- */
static int gxs_pk_rd(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    (void) ctx;
    if (g_pk_fd < 0) return -1;
    if (sceIoLseek(g_pk_fd, (SceOff) off, SCE_SEEK_SET) != (SceOff) off) return -1;
    return sceIoRead(g_pk_fd, dst, n);
}
static void gxs_pack_open(void)
{
    if (gxs_exists(GXS_NOPACK)) { gxm_log("[gxm-shader] pack: OFF (NOPACK marker)"); return; }
    const char *pk_path = GXS_PACK;
    g_pk_fd = sceIoOpen(pk_path, SCE_O_RDONLY, 0777);
    if (g_pk_fd < 0) { pk_path = GXS_APP_PACK; g_pk_fd = sceIoOpen(pk_path, SCE_O_RDONLY, 0777); }
    if (g_pk_fd < 0) { gxm_log("[gxm-shader] pack: none (%s and %s absent)", GXS_PACK, GXS_APP_PACK); return; }
    gxm_log("[gxm-shader] pack: %s", pk_path);
    const SceOff sz = sceIoLseek(g_pk_fd, 0, SCE_SEEK_END);
    if (sz <= 0 || sz > (SceOff) 0x7fffffff) { gxm_log("[gxm-shader] pack: %lld bytes, not used", (long long) sz); sceIoClose(g_pk_fd); g_pk_fd = -1; return; }
    g_pk_size = (uint32_t) sz;
    uint8_t *win = (uint8_t *) malloc(GXS_WIN);
    if (!win) { gxm_log("[gxm-shader] pack: no memory for the index window"); sceIoClose(g_pk_fd); g_pk_fd = -1; return; }
    const uint64_t t0 = gxm_now_us();
    gxs_pk_index(&g_pk, gxs_pk_rd, NULL, g_pk_size, win, GXS_WIN);
    free(win);
    g_pack_on = 1;
    gxm_log("[gxm-shader] pack: %u records (%u duplicates skipped), %u bytes, %u window reads, stop=%d at %u, indexed in %u ms (informational)",
            g_pk.n, g_pk.dups, (unsigned) g_pk_size, g_pk.reads, g_pk.stop, (unsigned) g_pk.stop_off, (unsigned) ((gxm_now_us() - t0) / 1000u));
}

/* ---- the compiler ---- */
static SceUID g_shark_lock = -1;                                 /* every compile holds it (the async worker, the synchronous path, util_cg): vitaShaRK is not reentrant */
static void gxs_shark_take(void) { if (g_shark_lock >= 0) sceKernelWaitSema(g_shark_lock, 1, NULL); }
static void gxs_shark_give(void) { if (g_shark_lock >= 0) sceKernelSignalSema(g_shark_lock, 1); }
static unsigned g_shark_logged;
static char *g_cg_vs, *g_cg_fs;                                  /* the Cg buffers (32 KB / 64 KB, as rhi_vita_tev.inc:1086 / :1479), allocated on the first compile */
static void gxs_shark_log(const char *msg, shark_log_level lvl, int line)
{
    if (lvl != SHARK_LOG_ERROR && g_shark_logged >= 8) return;
    if (g_shark_logged < 40) gxm_log("[gxm-shader] shark[%d] line %d: %.200s", (int) lvl, line, msg ? msg : "(null)");
    ++g_shark_logged;
}
static void gxs_shark_start(void)
{
    if (gxs_marker_is_1(GXS_NOSHARK)) { gxm_log("[gxm-shader] shark: OFF (gxm_noshark.txt 1)"); return; }
    shark_install_log_cb(gxs_shark_log);
    shark_set_warnings_level(SHARK_WARN_HIGH);
    int rc = shark_init(NULL);                                                       /* ur0:data/libshacccg.suprx (S1 console run) */
    if (rc < 0) rc = shark_init("ur0:data/external/libshacccg.suprx");
    g_shark_on = rc >= 0;
    if (g_shark_on) gxm_log("[gxm-shader] shark: ONLINE (SHARK_OPT_FAST, fastmath 1, fastprecision 0, fastint 1 = vitaGL's defaults)");
    else gxm_log("[gxm-shader] shark: OFF (shark_init 0x%08X; expected on an emulator): a key not in the pack gets NOPROG", (unsigned) rc);
}
/* One stage -> a 16-byte-aligned private copy of the GXP, or NULL. `size` is IN/OUT: in, the SOURCE LENGTH (never 0: S1 run 1, 31 of 31 programs died on 0 [M]). */
static SceGxmProgram *gxs_compile_stage(const char *src, int frag, uint32_t *outsz)
{
    uint32_t sz = (uint32_t) strlen(src);
    ++c_shark_compiles;
    SceGxmProgram *p = shark_compile_shader_extended(src, &sz, frag ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER, SHARK_OPT_FAST, 1, 0, 1);
    if (!p || !sz) { ++c_shark_fail; shark_clear_output(); return NULL; }
    SceGxmProgram *cp = (SceGxmProgram *) memalign(16, sz);
    if (cp) memcpy(cp, p, sz); else ++c_shark_fail;
    shark_clear_output();                                                            /* never shark_end(): it unloads before unhooking and kills the process (S1 run 1) */
    *outsz = sz;
    return cp;
}
static int gxs_sentinel_arm(uint32_t h, const char *hdr, const char *vs, const char *fs)   /* 0 armed; -1: cannot fence, so do not compile */
{
    char b[16]; const int n = snprintf(b, sizeof b, "%08x", (unsigned) h);
    if (gxs_write_file(GXS_SENT, b, (unsigned) n, NULL, 0, NULL, 0)) { ++c_sent_fail; return -1; }
    gxs_write_file(GXS_SENT_CG, hdr, (unsigned) strlen(hdr), vs, (unsigned) strlen(vs), "//----\n", 7);     /* best effort: the text about to be compiled */
    { SceUID f = sceIoOpen(GXS_SENT_CG, SCE_O_WRONLY | SCE_O_APPEND, 0777); if (f >= 0) { sceIoWrite(f, fs, (unsigned) strlen(fs)); sceIoClose(f); } }
    return 0;
}
static void gxs_sentinel_clear(void) { sceIoRemove(GXS_SENT); }

/* ---- registering a vertex + fragment GXP pair ---- */
static void gxs_unreg(GxsPriv *pv, GxmProg *p)
{
    if (!pv->reg || !g_gx.patcher) return;
    GXM_CHK(sceGxmShaderPatcherUnregisterProgram(g_gx.patcher, p->vid));
    GXM_CHK(sceGxmShaderPatcherUnregisterProgram(g_gx.patcher, p->fid));
    pv->reg = 0;
}
/* takes `vg`/`fg` (owned: this module frees them). 0 ok; -1 failed with everything undone. */
static int gxs_register(GxmProg *p, GxsPriv *pv, SceGxmProgram *vg, SceGxmProgram *fg, int owned)
{
    int bad = 0;
    p->vg = vg; p->fg = fg; pv->owned = owned;
    bad |= GXM_CHK(sceGxmProgramCheck(vg)) != 0;
    bad |= GXM_CHK(sceGxmProgramCheck(fg)) != 0;
    if (!bad) {
        if (GXM_CHK(sceGxmShaderPatcherRegisterProgram(g_gx.patcher, vg, &p->vid))) bad = 1;
        else if (GXM_CHK(sceGxmShaderPatcherRegisterProgram(g_gx.patcher, fg, &p->fid))) { GXM_CHK(sceGxmShaderPatcherUnregisterProgram(g_gx.patcher, p->vid)); bad = 1; }
        else pv->reg = 1;
    }
    if (bad) {
        if (owned) { free(vg); free(fg); }
        p->vg = p->fg = NULL; pv->owned = 0;
        return -1;
    }
    return 0;
}
/* copies from memory the caller owns (a pack buffer, a cache file) into aligned private blocks, then registers */
static int gxs_register_copy(GxmProg *p, GxsPriv *pv, const uint8_t *v, uint32_t vl, const uint8_t *f, uint32_t fl)
{
    SceGxmProgram *vg = (SceGxmProgram *) memalign(16, vl), *fg = (SceGxmProgram *) memalign(16, fl);
    if (!vg || !fg) { free(vg); free(fg); return -1; }
    memcpy(vg, v, vl); memcpy(fg, f, fl);
    return gxs_register(p, pv, vg, fg, 1);
}

/* ---- parameters: uniforms by NAME (c2floor bind_params), samplers by name ---- */
static void gxs_bind_params(const SceGxmProgram *g, int frag, GxmUBind *out, int *nout, uint32_t h, int *fast_ok)
{
    const unsigned np = sceGxmProgramGetParameterCount(g);
    int n = 0;
    for (unsigned i = 0; i < np; ++i) {
        const SceGxmProgramParameter *pp = sceGxmProgramGetParameter(g, i);
        if (sceGxmProgramParameterGetCategory(pp) != SCE_GXM_PARAMETER_CATEGORY_UNIFORM) continue;
        const char *nm = sceGxmProgramParameterGetName(pp);
        uint8_t idx; const int k = nm ? gxs_classify(nm, frag, &idx) : GXS_U_NONE;
        if (k == GXS_U_NONE || n >= GXM_UB_MAX) {
            ++c_unmapped_unif;
            if (g_unmapped_logged < 8) { snprintf(g_unmapped_log[g_unmapped_logged], sizeof g_unmapped_log[0], "%s", nm ? nm : "(null)"); ++g_unmapped_logged; gxm_log("[gxm-shader] unmapped %s uniform '%.30s' in program %08x (counted, not bound)", frag ? "fragment" : "vertex", nm ? nm : "(null)", (unsigned) h); }
            continue;
        }
        const unsigned as = sceGxmProgramParameterGetArraySize(pp);
        out[n].p = pp; out[n].kind = (uint8_t) k; out[n].idx = idx;
        out[n].maxc = (uint16_t) ((as ? as : 1u) * sceGxmProgramParameterGetComponentCount(pp));
        {   /* R3a C1: the word offset in the default uniform buffer (sceGxmSetUniformDataF writes `count` floats at buffer + 4 * (resource index + component offset)).  Only plain F32 uniforms
             * qualify (an F16 parameter is converted by the API); anything else leaves the program on the API path. */
            const unsigned ri = sceGxmProgramParameterGetResourceIndex(pp);
            out[n].off = (uint16_t) ri;
            if (sceGxmProgramParameterGetType(pp) != SCE_GXM_PARAMETER_TYPE_F32 || ri > 0xFFFFu || out[n].maxc == 0) *fast_ok = 0;
        }
        ++n;
    }
    *nout = n;
}
static void gxs_finish(GxmProg *p, int util)
{
    int fast_ok_local = 1;   /* R3a C1: cleared by a binding that cannot be written by offset */
    for (int s = 0; s < TEVK_TEX; ++s) p->texunit[s] = -1;
    if (!util) {
        gxs_bind_params(p->vg, 0, p->vb, &p->nvb, p->hash, &fast_ok_local);
        gxs_bind_params(p->fg, 1, p->fb, &p->nfb, p->hash, &fast_ok_local);
        for (int s = 0; s < TEVK_TEX; ++s) {
            char nm[8]; snprintf(nm, sizeof nm, "tex%d", s);
            const SceGxmProgramParameter *sp = sceGxmProgramFindParameterByName(p->fg, nm);
            p->texunit[s] = sp ? (int) sceGxmProgramParameterGetResourceIndex(sp) : -1;
        }
        const unsigned rd = gxs_slots_read(&p->tk);
        for (int s = 0; s < p->tk.numTex && s < TEVK_TEX; ++s) if ((rd >> s & 1u) && p->texunit[s] < 0) ++c_missing_samplers;
    } else {   /* util programs: uniforms/attributes are found by the target module by name; the blit sampler "tex" is exposed as texunit[0] for convenience */
        const SceGxmProgramParameter *sp = sceGxmProgramFindParameterByName(p->fg, "tex");
        p->texunit[0] = sp ? (int) sceGxmProgramParameterGetResourceIndex(sp) : -1;
    }
    p->vub_bytes = sceGxmProgramGetDefaultUniformBufferSize(p->vg);
    p->fub_bytes = sceGxmProgramGetDefaultUniformBufferSize(p->fg);
    p->ufast = 0; p->ucheck = 0;
    if (!util) {   /* R3a C1: every binding's [off, off + maxc) words must lie inside its stage's default uniform buffer, else the program keeps the API path */
        int ok = fast_ok_local;
        for (int i = 0; ok && i < p->nvb; ++i) if (((uint32_t) p->vb[i].off + p->vb[i].maxc) * 4u > p->vub_bytes) ok = 0;
        for (int i = 0; ok && i < p->nfb; ++i) if (((uint32_t) p->fb[i].off + p->fb[i].maxc) * 4u > p->fub_bytes) ok = 0;
        p->ufast = (uint8_t) ok;
    }
    p->ok = 1;
}

/* ---- the negative cache and the lookup table ---- */
static const GxsNeg *gxs_neg_find(int fam, int fast, uint32_t h, uint32_t h2)
{
    if (!g_neg) return NULL;
    unsigned i = (h ^ h2 * 0x9E3779B1u) & (GXS_NEG_N - 1);
    for (int c = 0; c < 32; ++c, i = (i + 1) & (GXS_NEG_N - 1)) {
        const GxsNeg *e = &g_neg[i];
        if (!e->used) return NULL;
        if (e->h == h && e->h2 == h2 && e->fam == fam && e->fast == fast) return e;
    }
    return NULL;
}
static void gxs_neg_add(int fam, int fast, uint32_t h, uint32_t h2, int why)
{
    if (!g_neg) return;
    unsigned i = (h ^ h2 * 0x9E3779B1u) & (GXS_NEG_N - 1), first = i;
    for (int c = 0; c < 32; ++c, i = (i + 1) & (GXS_NEG_N - 1)) if (!g_neg[i].used) { first = i; break; }   /* none free in 32 probes: overwrite the first (an old answer is only re-derived) */
    GxsNeg *e = &g_neg[first];
    e->h = h; e->h2 = h2; e->fam = (uint8_t) fam; e->fast = (uint8_t) fast; e->why = (uint8_t) why; e->used = 1;
}
static GxmProg *gxs_find(int fam, int fast, uint32_t h, const VsKey *vk, const TevKey *ck)
{
    unsigned p = h & (4096u - 1u);
    for (int c = 0; c < 4096; ++c, p = (p + 1) & (4096u - 1u)) {
        if (!g_tab[p]) return NULL;
        GxmProg *q = &gxs_slot(g_tab[p] - 1)->p;
        if (q->hash == h && q->family == fam && q->fast == fast && !memcmp(&q->tk, ck, sizeof *ck) && (!vk || !memcmp(&q->vk, vk, sizeof *vk))) return q;
    }
    return NULL;
}
static void gxs_insert(int idx, uint32_t h)
{
    unsigned p = h & (4096u - 1u);
    while (g_tab[p]) p = (p + 1) & (4096u - 1u);
    g_tab[p] = (uint16_t) (idx + 1);
}
static GxsSlot *gxs_new_slot(void)    /* the next pool entry, zeroed, or NULL when the table is full / memory is out */
{
    if (g_np >= GXM_PROG_MAX) return NULL;
    if (!g_chunk[g_np / GXS_CHUNK]) {
        g_chunk[g_np / GXS_CHUNK] = (GxsSlot *) calloc(GXS_CHUNK, sizeof(GxsSlot));
        if (!g_chunk[g_np / GXS_CHUNK]) return NULL;
    }
    GxsSlot *s = gxs_slot(g_np);
    memset(s, 0, sizeof *s);
    s->p.priv = &s->pv;
    return s;
}

/* ---- the GXP cache ---- */
static void gxs_gxpc_path(char *out, size_t cap, char kind, uint32_t h) { snprintf(out, cap, GXS_GXPDIR "%c%08x.gxp", kind, (unsigned) h); }
static int gxs_try_gxpc(GxmProg *p, GxsPriv *pv, char kind, uint32_t h, const void *k1, unsigned n1, const void *k2, unsigned n2)   /* 1 loaded, 0 none / stale, -1 hit but unusable */
{
    char path[96]; gxs_gxpc_path(path, sizeof path, kind, h);
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0777);
    int from_app = 0;
    if (f < 0) { snprintf(path, sizeof path, GXS_APP_GXPDIR "%c%08x.gxp", kind, (unsigned) h); f = sceIoOpen(path, SCE_O_RDONLY, 0777); from_app = 1; }
    if (f < 0) return 0;
    const SceOff sz = sceIoLseek(f, 0, SCE_SEEK_END);
    if (sz < 20 || sz > (SceOff) (1 << 20)) { sceIoClose(f); return 0; }
    sceIoLseek(f, 0, SCE_SEEK_SET);
    uint8_t *buf = (uint8_t *) malloc((size_t) sz + 4u);
    if (!buf) { sceIoClose(f); return 0; }
    const int rn = sceIoRead(f, buf, (unsigned) sz); sceIoClose(f);
    const uint8_t *v, *fr; uint32_t vl, fl;
    if (rn != (int) sz || !gxs_gxpc_valid(buf, (uint32_t) sz, k1, n1, k2, n2, &v, &vl, &fr, &fl)) { free(buf); return 0; }
    const int r = gxs_register_copy(p, pv, v, vl, fr, fl);
    free(buf);
    if (r) return -1;
    ++c_gxpc_hit; if (from_app) ++c_gxpc_app; p->from = GXM_FROM_GXPCACHE;
    return 1;
}
static void gxs_save_gxpc(char kind, uint32_t h, const void *k1, unsigned n1, const void *k2, unsigned n2, const void *v, uint32_t vl, const void *f, uint32_t fl)
{
    const uint32_t kl = gxs_pad4(n1 + n2), total = 20u + kl + gxs_pad4(vl) + gxs_pad4(fl);
    uint8_t *buf = (uint8_t *) calloc(1, total);
    if (!buf) return;
    const uint32_t hd[5] = { GXS_GXPC_MAGIC, GXS_SHC_VERSION, n1 + n2, vl, fl };
    memcpy(buf, hd, sizeof hd);
    memcpy(buf + 20, k1, n1); if (n2) memcpy(buf + 20 + n1, k2, n2);
    memcpy(buf + 20 + kl, v, vl); memcpy(buf + 20 + kl + gxs_pad4(vl), f, fl);
    char path[96], tmp[100]; gxs_gxpc_path(path, sizeof path, kind, h); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!gxs_write_file(tmp, buf, total, NULL, 0, NULL, 0)) { sceIoRemove(path); if (sceIoRename(tmp, path) >= 0) ++c_gxpc_write; else sceIoRemove(tmp); }
    else sceIoRemove(tmp);
    free(buf);
}

/* ---- where a program comes from ---- */
/* the pack. 1 loaded; 0 no usable record (miss / stale / unreadable); -1 a VALID record that cannot be split / checked / registered (GXM_SK_COMPILE) */
static int gxs_try_pack(GxmProg *p, GxsPriv *pv, char kind, uint32_t h, const void *k1, unsigned n1, const void *k2, unsigned n2)
{
    if (!g_pack_on) return 0;
    const GxsPk *e = gxs_pk_find(&g_pk, h, (uint32_t) (unsigned char) kind);
    if (!e) { ++c_pack_miss; return 0; }
    int bad = 0;
    uint8_t *buf = gxs_pk_fetch(e, gxs_pk_rd, NULL, &bad);
    ++c_pack_reads;
    if (!buf) { if (bad) ++c_pack_stale; else ++c_pack_bad; return 0; }                       /* checksum mismatch = stale; a read error is counted apart */
    const uint8_t *bin; uint32_t binlen;
    if (!gxs_img_binary(buf, e->len, k1, n1, k2, n2, &bin, &binlen)) { ++c_pack_stale; free(buf); return 0; }   /* magic / version / key bytes differ */
    const uint8_t *v, *f; uint32_t vl, fl;
    if (gxs_prog_split(bin, binlen, &v, &vl, &f, &fl)) { ++c_split_fail; free(buf); return -1; }
    const int r = gxs_register_copy(p, pv, v, vl, f, fl);
    free(buf);
    if (r) return -1;
    ++c_pack_hit; p->from = GXM_FROM_PACK;
    return 1;
}
/* the compiler. Generates the Cg (refusal = NOPROG, counted), fences, compiles both stages synchronously, registers, caches. 1 ok; 0 no program (*why set); -1 failed (COMPILE) */
static int gxs_try_compile(GxmProg *p, GxsPriv *pv, int fam, int fast, char kind, uint32_t h, const VsKey *vk, const TevKey *ck,
                           const void *k1, unsigned n1, const void *k2, unsigned n2, int *why)
{
    char *vs = g_cg_vs, *fs = g_cg_fs;
    if (!g_shark_on) { *why = GXM_SK_NOPROG; return 0; }
    if (!vs || !fs) { vs = g_cg_vs = (char *) malloc(32768); fs = g_cg_fs = (char *) malloc(65536); }
    if (!vs || !fs) { *why = GXM_SK_NOPROG; return 0; }
    const int gv = fam == GXM_PK_TEV ? tev_gen_vertex_ex(ck->numTex, ck->fogType, vs, 32768) : tev_gen_vertex_gx(vk, vs, 32768);
    const int gf = fast ? tev_gen_fragment_fast(ck, fs, 65536) : tev_gen_fragment(ck, fs, 65536);
    if (gv < 0 || gf < 0) { ++c_refused_gen; *why = GXM_SK_NOPROG; return 0; }
    char hl[80]; snprintf(hl, sizeof hl, "// gxm %c%08x\n", kind, (unsigned) h);
    if (gxs_sentinel_arm(h, hl, vs, fs)) { *why = GXM_SK_NOPROG; return 0; }                   /* cannot fence it: refuse to compile unfenced */
    const uint64_t t0 = gxm_now_us();
    uint32_t vsz = 0, fsz = 0;
    gxs_shark_take();
    SceGxmProgram *vg = gxs_compile_stage(vs, 0, &vsz);
    SceGxmProgram *fg = vg ? gxs_compile_stage(fs, 1, &fsz) : NULL;
    gxs_shark_give();
    gxs_sentinel_clear();
    if (!vg || !fg) { free(vg); free(fg); ++c_compile_fail; gxm_log("[gxm-shader] COMPILE FAILED %c%08x (%s stage)", kind, (unsigned) h, vg ? "fragment" : "vertex"); *why = GXM_SK_COMPILE; return -1; }
    gxm_log("[gxm-shader] compiled %c%08x: vertex %u B, fragment %u B (%u ms, informational)", kind, (unsigned) h, (unsigned) vsz, (unsigned) fsz, (unsigned) ((gxm_now_us() - t0) / 1000u));
    if (gxs_register(p, pv, vg, fg, 1)) { *why = GXM_SK_COMPILE; return -1; }
    gxs_save_gxpc(kind, h, k1, n1, k2, n2, p->vg, vsz, p->fg, fsz);       /* only a pair that checked and registered is worth keeping */
    p->from = GXM_FROM_SHARK;
    return 1;
}

/* ---- the BACKGROUND compiler (2026-10-27, docs/54 "first hand-play" item 1) --------------------------------------------------------------------------------
 * play_1.txt: 45 runtime compiles of 1.5-8.2 s each on the backend's thread = "freezes every couple of seconds" (worst stall 21 s).  Now a key that misses
 * the pack AND the GXP cache is generated here (owner thread: the Cg buffers are the owner's), QUEUED, and answered NOPROG -- the HAL's existing fallback (the
 * CPU path; a skinned list is skipped) draws it meanwhile.  The worker "stfx_gxc" compiles at a priority BELOW every game thread, so it only takes idle time;
 * it checks both stages and writes the GXP cache.  The owner INSTALLS finished jobs (register / finish / insert: the patcher and the pool stay owner-only) at
 * the top of its next lookup, then forgets the memos (they hold the NULL answers of pending keys).  A job never sits in the negative cache while pending.
 * ux0:data/stairfax/gxm_async.txt starting with 0 = the synchronous compiler (as before).  The shark lock serialises every compile (worker, util_cg). */
#define GXS_ASYNCFILE  "ux0:data/stairfax/gxm_async.txt"
#define GXS_JOB_MAX    32
#define GXS_JOB_STACK  0x200000                                        /* as stfx_asc (rhi_vita_tev.inc ASC_STACK_BYTES): the compiler recurses deeply */
enum { GXS_J_FREE, GXS_J_QUEUED, GXS_J_COMPILING, GXS_J_READY, GXS_J_FAILED };
typedef struct {
    volatile int state;
    int fam, fast; char kind; uint32_t h, h2, seq;
    int has_vk; VsKey vk; TevKey ck;
    char *vs, *fs;                                                     /* owner allocates, owner frees (install) */
    SceGxmProgram *vg, *fg; uint32_t vsz, fsz, ms;                     /* worker output */
    int fail_stage;                                                    /* 0 vertex, 1 fragment, 2 check */
} GxsJob;
static GxsJob g_job[GXS_JOB_MAX];
static int g_async;                                                    /* 1 = the worker is running and takes compiles */
static SceUID g_job_wake = -1, g_job_thr = -1;
static volatile uint32_t g_job_done;                                   /* worker: +1 per finished job (ready or failed) */
static uint32_t g_job_seen, g_job_seq;                                 /* owner */
static volatile int g_job_stop;
static unsigned c_async_queued, c_async_installed, c_async_failed, c_async_full, c_async_pending, c_async_inflight_max;
static volatile unsigned c_async_ms_max, c_async_ms_sum;
static int gxs_job_worker(SceSize args, void *argp)
{
    (void) args; (void) argp;
    for (;;) {
        sceKernelWaitSema(g_job_wake, 1, NULL);
        for (;;) {
            if (g_job_stop) return 0;
            GxsJob *j = NULL;
            for (int i = 0; i < GXS_JOB_MAX; ++i)
                if (__atomic_load_n(&g_job[i].state, __ATOMIC_ACQUIRE) == GXS_J_QUEUED && (!j || (int32_t) (g_job[i].seq - j->seq) < 0)) j = &g_job[i];
            if (!j) break;
            __atomic_store_n(&j->state, GXS_J_COMPILING, __ATOMIC_RELEASE);
            char hl[80]; snprintf(hl, sizeof hl, "// gxm %c%08x (async)\n", j->kind, (unsigned) j->h);
            int ok = 0;
            const uint64_t t0 = gxm_now_us();
            gxs_shark_take();
            if (!gxs_sentinel_arm(j->h, hl, j->vs, j->fs)) {                                  /* cannot fence it: refuse, as the synchronous path does */
                j->vg = gxs_compile_stage(j->vs, 0, &j->vsz);
                j->fg = j->vg ? gxs_compile_stage(j->fs, 1, &j->fsz) : NULL;
                gxs_sentinel_clear();
                j->fail_stage = !j->vg ? 0 : !j->fg ? 1 : 2;
                ok = j->vg && j->fg && sceGxmProgramCheck(j->vg) == 0 && sceGxmProgramCheck(j->fg) == 0;
            } else j->fail_stage = 2;
            gxs_shark_give();
            j->ms = (uint32_t) ((gxm_now_us() - t0) / 1000u);
            if (ok) {
                const void *k1 = j->has_vk ? (const void *) &j->vk : (const void *) &j->ck;
                const unsigned n1 = j->has_vk ? (unsigned) tev_vs_key_len(&j->vk) : (unsigned) tev_key_len(&j->ck);
                const void *k2 = j->has_vk ? (const void *) &j->ck : NULL; const unsigned n2 = j->has_vk ? (unsigned) tev_key_len(&j->ck) : 0u;
                gxs_save_gxpc(j->kind, j->h, k1, n1, k2, n2, j->vg, j->vsz, j->fg, j->fsz);   /* both stages passed sceGxmProgramCheck */
            } else { free(j->vg); free(j->fg); j->vg = j->fg = NULL; }
            if (j->ms > c_async_ms_max) c_async_ms_max = j->ms;
            c_async_ms_sum += j->ms;
            __atomic_store_n(&j->state, ok ? GXS_J_READY : GXS_J_FAILED, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_job_done, 1u, __ATOMIC_RELEASE);
        }
    }
    return 0;
}
static void gxs_async_start(void)
{
    g_async = 0;
    if (!g_shark_on) return;
    { char b[8]; if (gxs_read_small(GXS_ASYNCFILE, b, sizeof b) > 0 && b[0] == '0') { gxm_log("[gxm-shader] async compile: OFF (gxm_async.txt 0): a new key compiles on the backend thread (a freeze)"); return; } }
    if (g_job_thr >= 0) { g_async = 1; return; }                                         /* a re-init: the worker from the first init is still waiting */
    g_shark_lock = sceKernelCreateSema("stfx_gxc_l", 0, 1, 1, NULL);
    g_job_wake = sceKernelCreateSema("stfx_gxc_w", 0, 0, 1 << 20, NULL);
    /* BELOW the game threads (all 0x10000100): it must never delay a frame, only use idle time.  +32 is illegal (run 171: 0x80028023); +16 is tried, then
     * the default (equal priority: the kernel does not time-slice, so a compile could then hold a core for seconds -- logged loudly). */
    int pri = 0x10000100 + 16;
    g_job_thr = sceKernelCreateThread("stfx_gxc", gxs_job_worker, pri, GXS_JOB_STACK, 0, 0, NULL);
    if (g_job_thr < 0) { pri = 0x10000100; g_job_thr = sceKernelCreateThread("stfx_gxc", gxs_job_worker, pri, GXS_JOB_STACK, 0, 0, NULL); }
    if (g_shark_lock < 0 || g_job_wake < 0 || g_job_thr < 0 || sceKernelStartThread(g_job_thr, 0, NULL) < 0) {
        gxm_log("[gxm-shader] async compile: NOT started (sema %08x %08x thread %08x): synchronous, as before", (unsigned) g_shark_lock, (unsigned) g_job_wake, (unsigned) g_job_thr);
        if (g_job_thr >= 0) { sceKernelDeleteThread(g_job_thr); g_job_thr = -1; }
        return;
    }
    g_async = 1;
    gxm_log("[gxm-shader] async compile: ON (worker stfx_gxc, priority %s, %u jobs, 2 MB stack): a new key draws through the HAL's fallback until it lands",
            pri == 0x10000100 ? "DEFAULT (+16 refused: EQUAL to the game threads)" : "default+16", (unsigned) GXS_JOB_MAX);
}
static GxsJob *gxs_job_find(int fam, int fast, uint32_t h, const VsKey *vk, const TevKey *ck)
{
    for (int i = 0; i < GXS_JOB_MAX; ++i) {
        GxsJob *j = &g_job[i];
        if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == GXS_J_FREE) continue;
        if (j->h == h && j->fam == fam && j->fast == fast && !memcmp(&j->ck, ck, sizeof *ck) && (vk ? j->has_vk && !memcmp(&j->vk, vk, sizeof *vk) : !j->has_vk)) return j;
    }
    return NULL;
}
static unsigned gxs_jobs_busy(void) { unsigned n = 0; for (int i = 0; i < GXS_JOB_MAX; ++i) n += __atomic_load_n(&g_job[i].state, __ATOMIC_ACQUIRE) != GXS_J_FREE; return n; }
/* owner: generate + queue. 1 queued; 0 refused (*why set: NOPROG for a generator refusal or a full queue, which is NOT remembered for the queue) */
static int gxs_async_queue(int fam, int fast, char kind, uint32_t h, uint32_t h2, const VsKey *vk, const TevKey *ck, int *why, int *remember)
{
    *remember = 1;
    GxsJob *j = NULL;
    for (int i = 0; i < GXS_JOB_MAX; ++i) if (__atomic_load_n(&g_job[i].state, __ATOMIC_ACQUIRE) == GXS_J_FREE) { j = &g_job[i]; break; }
    if (!j) { ++c_async_full; *why = GXM_SK_NOPROG; *remember = 0; return 0; }                    /* asked again on a later draw */
    char *vs = g_cg_vs, *fs = g_cg_fs;
    if (!vs || !fs) { vs = g_cg_vs = (char *) malloc(32768); fs = g_cg_fs = (char *) malloc(65536); }
    if (!vs || !fs) { *why = GXM_SK_NOPROG; *remember = 0; return 0; }
    const int gv = fam == GXM_PK_TEV ? tev_gen_vertex_ex(ck->numTex, ck->fogType, vs, 32768) : tev_gen_vertex_gx(vk, vs, 32768);
    const int gf = fast ? tev_gen_fragment_fast(ck, fs, 65536) : tev_gen_fragment(ck, fs, 65536);
    if (gv < 0 || gf < 0) { ++c_refused_gen; *why = GXM_SK_NOPROG; return 0; }
    j->vs = strdup(vs); j->fs = strdup(fs);
    if (!j->vs || !j->fs) { free(j->vs); free(j->fs); j->vs = j->fs = NULL; *why = GXM_SK_NOPROG; *remember = 0; return 0; }
    j->fam = fam; j->fast = fast; j->kind = kind; j->h = h; j->h2 = h2; j->seq = ++g_job_seq;
    j->has_vk = vk != NULL; if (vk) j->vk = *vk; else memset(&j->vk, 0, sizeof j->vk);
    j->ck = *ck; j->vg = j->fg = NULL; j->vsz = j->fsz = j->ms = 0; j->fail_stage = 0;
    __atomic_store_n(&j->state, GXS_J_QUEUED, __ATOMIC_RELEASE);
    sceKernelSignalSema(g_job_wake, 1);
    ++c_async_queued;
    { const unsigned b = gxs_jobs_busy(); if (b > c_async_inflight_max) c_async_inflight_max = b; }
    *why = GXM_SK_NOPROG;
    return 1;
}
/* owner: every finished job becomes a pool program (or a remembered COMPILE failure), then the memos are forgotten */
static void gxs_async_install(void)
{
    g_job_seen = __atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE);
    int any = 0;
    for (int i = 0; i < GXS_JOB_MAX; ++i) {
        GxsJob *j = &g_job[i];
        const int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
        if (st != GXS_J_READY && st != GXS_J_FAILED) continue;
        any = 1;
        const VsKey *vk = j->has_vk ? &j->vk : NULL;
        if (st == GXS_J_FAILED) {
            ++c_compile_fail; ++c_async_failed;
            gxm_log("[gxm-shader] COMPILE FAILED %c%08x (%s, async, %u ms)", j->kind, (unsigned) j->h, j->fail_stage == 0 ? "vertex stage" : j->fail_stage == 1 ? "fragment stage" : "program check / sentinel", (unsigned) j->ms);
            gxs_neg_add(j->fam, j->fast, j->h, j->h2, GXM_SK_COMPILE);
        } else {
            GxsSlot *s = gxs_new_slot();
            if (!s) { ++c_prog_full; free(j->vg); free(j->fg); gxs_neg_add(j->fam, j->fast, j->h, j->h2, GXM_SK_PROGFULL); }   /* remembered here: else it would compile again */
            else {
                GxmProg *p = &s->p;
                p->family = (uint8_t) j->fam; p->fast = (uint8_t) j->fast; p->hash = j->h; p->name = j->fam == GXM_PK_TEV ? "tev" : "gx"; p->tk = j->ck;
                if (vk) p->vk = *vk;
                if (gxs_register(p, &s->pv, j->vg, j->fg, 1)) { ++c_compile_fail; ++c_async_failed; gxs_neg_add(j->fam, j->fast, j->h, j->h2, GXM_SK_COMPILE); }   /* frees vg / fg */
                else {
                    p->from = GXM_FROM_SHARK;
                    gxs_finish(p, 0);
                    gxs_insert(g_np, j->h);
                    ++g_np; ++c_async_installed;
                    gxm_log("[gxm-shader] compiled %c%08x: vertex %u B, fragment %u B (%u ms, informational, async)", j->kind, (unsigned) j->h, (unsigned) j->vsz, (unsigned) j->fsz, (unsigned) j->ms);
                }
            }
        }
        free(j->vs); free(j->fs); j->vs = j->fs = NULL; j->vg = j->fg = NULL;
        __atomic_store_n(&j->state, GXS_J_FREE, __ATOMIC_RELEASE);
    }
    if (any) gxs_forget_memos();
}
static inline void gxs_async_poll(void) { if (__builtin_expect(g_async && __atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE) != g_job_seen, 0)) gxs_async_install(); }

/* get-or-create for the TEV (fam GXM_PK_TEV, vk NULL) and GX (vk set) families. `ck`: the canonical TevKey. NULL with *why on any failure. */
static GxmProg *gxs_make(int fam, int fast, uint32_t h, const VsKey *vk, const TevKey *ck, int *why)
{
    const uint32_t h2 = gxs_key_h2(vk, ck);
    const GxsNeg *ng = gxs_neg_find(fam, fast, h, h2);
    if (ng) { *why = ng->why; return NULL; }
    if (gxs_is_bad(h)) { ++c_noprog_bad; *why = GXM_SK_NOPROG; gxs_neg_add(fam, fast, h, h2, GXM_SK_NOPROG); return NULL; }
    if (g_async && gxs_job_find(fam, fast, h, vk, ck)) { ++c_async_pending; *why = GXM_SK_NOPROG; return NULL; }   /* queued / compiling: the HAL's fallback draws it */
    if (g_async && (unsigned) g_np + gxs_jobs_busy() >= (unsigned) GXM_PROG_MAX) { ++c_prog_full; *why = GXM_SK_PROGFULL; return NULL; }
    GxsSlot *s = gxs_new_slot();
    if (!s) { ++c_prog_full; *why = GXM_SK_PROGFULL; return NULL; }                                   /* not remembered: nothing to learn from a full table */
    GxmProg *p = &s->p; GxsPriv *pv = &s->pv;
    p->family = (uint8_t) fam; p->fast = (uint8_t) fast; p->hash = h; p->name = fam == GXM_PK_TEV ? "tev" : "gx"; p->tk = *ck;
    if (vk) p->vk = *vk;
    const char kind = fam == GXM_PK_TEV ? (fast ? 'f' : 't') : (fast ? 'h' : 'g');
    const void *k1 = vk ? (const void *) vk : (const void *) ck; const unsigned n1 = vk ? (unsigned) tev_vs_key_len(vk) : (unsigned) tev_key_len(ck);
    const void *k2 = vk ? (const void *) ck : NULL;              const unsigned n2 = vk ? (unsigned) tev_key_len(ck) : 0u;
    int r = gxs_try_pack(p, pv, kind, h, k1, n1, k2, n2);
    int w = GXM_SK_NOPROG;
    if (r < 0) w = GXM_SK_COMPILE;
    else if (r == 0) {
        /* TRAP (R8, 2026-10-26): the GXP cache is validated by the KEY bytes only, never the generated source -- a generator change with an
         * unchanged key reuses a stale program (R8 lost hours to it: aSkP/aSkN absent from a cached GXP -> 0x805B0003; and a uTMX-palette
         * program reused after the palette moved to uBone).  Skinned programs never read the cache -- until 2026-10-28: that recompiled every
         * skinned program on every boot, 6-10 s each (the opening hitch Tyler saw: had5fff22 / g59bcbdd6 / ha909d2ba in three sessions of
         * eboot 76c12fa3).  Lifted after a byte check: the 11 skinned GXPs compiled 2026-10-28 are IDENTICAL to the release snapshot's.
         * The trap is open again for generator edits: delete ux0:data/STFX00001/gxm/g*,h*.gxp after one.  General fix (fold a hash of the
         * generator source into the cache validity) still to do. */
        if (g_async && gxs_jobs_busy() >= GXS_JOB_MAX) { ++c_async_full; *why = GXM_SK_NOPROG; return NULL; }   /* queue full: not even the cache file (one sceIoOpen per draw) -- asked again later */
        r = gxs_try_gxpc(p, pv, kind, h, k1, n1, k2, n2);
        if (r < 0) w = GXM_SK_COMPILE;
        else if (r == 0 && g_async) {
            int remember = 1;
            const int queued = gxs_async_queue(fam, fast, kind, h, h2, vk, ck, &w, &remember);   /* queued: NOPROG now, the program at a later lookup */
            *why = w;
            if (!queued && remember) gxs_neg_add(fam, fast, h, h2, w);              /* the generator refused it: remembered, as the synchronous path does */
            return NULL;                                                            /* a queued key is never in the negative cache */
        }
        else if (r == 0) r = gxs_try_compile(p, pv, fam, fast, kind, h, vk, ck, k1, n1, k2, n2, &w);
    }
    if (r != 1) {
        *why = w;
        gxs_neg_add(fam, fast, h, h2, w);
        return NULL;
    }
    gxs_finish(p, 0);
    gxs_insert(g_np, h);
    ++g_np;
    return p;
}

/* ======================================================================================================================================== public: lifetime */
int gxm_shader_init(void)
{
    if (g_up) return 0;
    if (!g_gx.patcher) { gxm_log("[gxm-shader] init: no shader patcher"); return -1; }
    memset(&g_td, 0, sizeof g_td);
    g_np = 0; g_bad_n = 0; memset(g_tab, 0, sizeof g_tab); memset(g_chunk, 0, sizeof g_chunk);
    g_pack_on = 0; g_shark_on = 0; g_pk_fd = -1; g_pk_size = 0;
    c_pack_hit = c_pack_miss = c_pack_stale = c_pack_reads = c_pack_bad = c_shark_compiles = c_shark_fail = c_gxpc_hit = c_gxpc_app = c_vp_made = c_fp_made = c_vp_fail = c_fp_fail = 0;
    c_unmapped_unif = c_unmapped_attr = c_missing_samplers = c_refused_gen = c_noprog_bad = c_prog_full = c_arm_noprog = c_sent_fail = c_gxpc_write = c_noop = 0;
    c_compile_fail = c_split_fail = 0; g_unmapped_logged = 0;
    g_neg = (GxsNeg *) calloc(GXS_NEG_N, sizeof *g_neg);
    if (!g_neg) { gxm_log("[gxm-shader] init: out of memory (negative cache)"); return -1; }
    /* the three markers that decide a program's identity: they must be what the pack's run had, or pack_miss explodes (GXM_M2.md section 12) */
    { char b[8]; g_fast = gxs_read_small(GXS_FASTFILE, b, sizeof b) > 0 && b[0] == '1'; }
    g_persp = !gxs_marker_is_1(GXS_PERSPFILE);
    g_dedupe = !gxs_marker_is_1(GXS_DEDUPEFILE);
    g_td.on = g_dedupe;
    gxm_log("[gxm-shader] identity: fast=%d (tev_fast.txt) persp=%d (texgen_persp_off.txt) dedupe=%d (tev_dedupe_off.txt) | table %d programs, TEVGEN_VERSION %d, pack version %#x",
            g_fast, g_persp, g_dedupe, GXM_PROG_MAX, TEVGEN_VERSION, (unsigned) GXS_SHC_VERSION);
    sceIoMkdir("ux0:data/STFX00001", 0777); sceIoMkdir("ux0:data/STFX00001/shc", 0777); sceIoMkdir("ux0:data/STFX00001/gxm", 0777);
    if (g_gx.arm & GXM_ARM_NOPROG) {
        gxm_log("[gxm-shader] ARM noprog: the pack and the compiler are treated as absent - every TEV / GX / builtin program is NOPROG");
        g_up = 1;
        return 0;
    }
    gxs_sentinel_recover();
    gxs_pack_open();
    gxs_shark_start();
    g_up = 1;
    gxs_async_start();
    return 0;
}

void gxm_shader_shutdown(void)
{
    if (!g_up && !g_neg && !g_np) return;
    if (g_async) {                           /* the worker: no new job is taken; one in flight is waited for (bounded), then every job is dropped */
        g_async = 0;
        for (int i = 0; i < GXS_JOB_MAX; ++i) { int q = GXS_J_QUEUED; __atomic_compare_exchange_n(&g_job[i].state, &q, GXS_J_FAILED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
        for (int t = 0; t < 1500; ++t) {
            int busy = 0;
            for (int i = 0; i < GXS_JOB_MAX; ++i) busy |= __atomic_load_n(&g_job[i].state, __ATOMIC_ACQUIRE) == GXS_J_COMPILING;
            if (!busy) break;
            sceKernelDelayThread(10000);
        }
        for (int i = 0; i < GXS_JOB_MAX; ++i) {
            GxsJob *j = &g_job[i];
            const int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
            if (st == GXS_J_COMPILING) continue;   /* still compiling after 15 s: leaked, the worker owns it */
            if (st == GXS_J_READY) { free(j->vg); free(j->fg); }
            free(j->vs); free(j->fs); j->vs = j->fs = NULL; j->vg = j->fg = NULL;
            __atomic_store_n(&j->state, GXS_J_FREE, __ATOMIC_RELEASE);
        }
        g_job_seen = __atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE);
    }
    gxs_forget_memos();                      /* the memos and the builtin cache point into the pool */
    const int n = g_np;
    for (int i = 0; i < n; ++i) {            /* 1. every vertex / fragment program */
        GxsSlot *s = gxs_slot(i); GxsPriv *pv = &s->pv;
        for (int k = 0; k < pv->nvp; ++k) { if (pv->vp[k].vp && g_gx.patcher) GXM_CHK(sceGxmShaderPatcherReleaseVertexProgram(g_gx.patcher, pv->vp[k].vp)); free(pv->vp[k].a); }
        for (int k = 0; k < pv->nfp; ++k) if (pv->fp[k].fp && g_gx.patcher) GXM_CHK(sceGxmShaderPatcherReleaseFragmentProgram(g_gx.patcher, pv->fp[k].fp));
        if (pv->nowrite && g_gx.patcher) GXM_CHK(sceGxmShaderPatcherReleaseFragmentProgram(g_gx.patcher, pv->nowrite));
        free(pv->vp); free(pv->fp); pv->vp = NULL; pv->fp = NULL; pv->nvp = pv->nfp = pv->capvp = pv->capfp = 0; pv->nowrite = NULL;
    }
    for (int i = 0; i < n; ++i) {            /* 2. unregister, 3. free the GXP copies */
        GxsSlot *s = gxs_slot(i); GxsPriv *pv = &s->pv;
        gxs_unreg(pv, &s->p);
        if (pv->owned) { free(s->p.vg); free(s->p.fg); }
        s->p.ok = 0; s->p.vg = s->p.fg = NULL;
    }
    for (unsigned i = 0; i < sizeof g_chunk / sizeof g_chunk[0]; ++i) { free(g_chunk[i]); g_chunk[i] = NULL; }
    g_np = 0;
    free(g_neg); g_neg = NULL;
    free(g_cg_vs); free(g_cg_fs); g_cg_vs = g_cg_fs = NULL;
    gxs_pk_free(&g_pk);
    if (g_pk_fd >= 0) { sceIoClose(g_pk_fd); g_pk_fd = -1; }
    g_pack_on = 0;
    g_up = 0;
    /* shark_end() is NOT called: it unloads the module before unhooking and the process dies (S1 console run 1 [M]) */
}

/* ======================================================================================================================================== public: programs */
static TevKey s_tk_raw; static int s_tk_valid; static GxmProg *s_tk_prog; static int s_tk_why;
/* The one-key memo of the GX pair and its HINT (R3a C3, R3c C5).  GXM_HINT_BEGIN .. GXM_HINT_END is cut out of this file by host/tests/gxm_hint_test (and mutated there): the rules are
 *   - a hint is a nonzero 64-bit name the HAL gives to the (VsKey, TevKey) pair of the call: EQUAL HINTS MEAN EQUAL RAW KEYS (gx_draw.c: bit 63 | key serials under C3, bit 62 | the program epoch's key id under C5);
 *   - the memo stores the hint of the call that stored its keys, and every path that stores keys stores the hint of ITS call (0 = none), so the stored hint always names the stored keys;
 *   - a call whose hint is nonzero and equals the stored one is answered by the memo without comparing the keys. */
/* GXM_HINT_BEGIN */
typedef struct { VsKey vk; TevKey tk; int valid; GxmProg *prog; int why; unsigned long long hint; } GxsOneKey;
static GxsOneKey s_ok;
static inline void gxs_ok_forget(GxsOneKey *m) { m->valid = 0; m->prog = NULL; m->hint = 0ull; }
static inline int gxs_ok_hint_hit(const GxsOneKey *m, unsigned long long hint) { return hint && m->valid && hint == m->hint; }
static inline int gxs_ok_keys_hit(GxsOneKey *m, const VsKey *vk, const TevKey *tk, unsigned long long hint)
{
    if (m->valid && !memcmp(vk, &m->vk, sizeof *vk) && !memcmp(tk, &m->tk, sizeof *tk)) { m->hint = hint; return 1; }
    return 0;
}
static inline void gxs_ok_store(GxsOneKey *m, const VsKey *vk, const TevKey *tk, GxmProg *p, int why, unsigned long long hint)
{
    m->vk = *vk; m->tk = *tk; m->valid = 1; m->prog = p; m->why = why; m->hint = hint;
}
/* GXM_HINT_END */
static unsigned c_hint_hit;            /* calls answered by the hint alone (no compare, no hash) */
static GxmProg *g_bi[GXM_BI_N]; static int g_bi_why[GXM_BI_N]; static int g_bi_done[GXM_BI_N];
static void gxs_forget_memos(void)
{
    s_tk_valid = 0; s_tk_prog = NULL; gxs_ok_forget(&s_ok);
    memset(g_bi, 0, sizeof g_bi); memset(g_bi_done, 0, sizeof g_bi_done);
    gxs_mm_clear();
}

/* ---- raw-key memo (2026-10-20, hardware profile: lookup 8.4 us per display-list draw, 3.2 per immediate draw, behind a ONE-key memo).
 * 1024 direct-mapped entries, key = a 128-bit hash of the RAW key bytes (the C3 arm-100 hash, gxaot/release/c3/c3_cuts.h c3_hash2), value =
 * the GxmProg the full path returned. Sound without a generation check: pool entries are permanent until gxm_shader_shutdown (no eviction),
 * which clears the memo. Only OK programs are stored; a miss falls through to the unchanged full path. */
#define GXS_MM_N 1024u
typedef struct { uint64_t h0, h1; GxmProg *p; } GxsMm;
static GxsMm s_mm[2][GXS_MM_N];                /* [0] TEV, [1] GX pair */
static unsigned c_mm_hit[2], c_mm_miss[2], c_mm_ver, c_mm_bad;   /* ver/bad: 1 hit in 64 re-runs the full path; bad = it named another program */
/* hw_Mm2 (docs/54 2026-10-24): hw_Rsplit measured the GX-pair lookup at 6.4 us/draw with this memo missing 17% (498,580 / 2,995,733) while only
 * 94 programs were live -- raw keys that canonicalise to the same program evict each other from a direct-mapped slot. v2 = 4-way x 2048 sets
 * (8192 entries, round-robin victim), same 128-bit raw hash, same soundness argument (pool entries permanent; cleared with v1). Switched per
 * draw by g_gxs_mm_v2 (vita_tool_main.c: perf arm 140). g_gxs_mm_st: [0] one-key hits [1] memo hits [2] misses [3] miss, slot empty
 * [4] miss, slot held the SAME program (raw-key aliasing) [5] miss, slot held another program (conflict) -- of the GX-pair path only. */
int g_gxs_mm_v2;
unsigned g_gxs_mm_st[6];
#define GXS_MM2_SETS 2048u
typedef struct { uint64_t h0[4], h1[4]; GxmProg *p[4]; uint8_t rr; } GxsMm2;
static GxsMm2 *s_mm2;
static void gxs_mm_clear(void) { memset(s_mm, 0, sizeof s_mm); if (s_mm2) memset(s_mm2, 0, GXS_MM2_SETS * sizeof *s_mm2); }
static inline GxmProg *gxs_mm2_find(const uint64_t h[2])
{
    if (!s_mm2) { s_mm2 = (GxsMm2 *) calloc(GXS_MM2_SETS, sizeof *s_mm2); if (!s_mm2) return NULL; }
    const GxsMm2 *e = &s_mm2[h[0] & (GXS_MM2_SETS - 1u)];
    for (int w = 0; w < 4; ++w) if (e->p[w] && e->h0[w] == h[0] && e->h1[w] == h[1]) return e->p[w];
    return NULL;
}
static inline void gxs_mm2_put(const uint64_t h[2], GxmProg *p)
{
    if (!s_mm2) return;
    GxsMm2 *e = &s_mm2[h[0] & (GXS_MM2_SETS - 1u)]; int w = -1;
    for (int i = 0; i < 4; ++i) if (!e->p[i]) { w = i; break; }
    if (w < 0) { w = e->rr & 3; e->rr = (uint8_t) (e->rr + 1); }
    e->h0[w] = h[0]; e->h1[w] = h[1]; e->p[w] = p;
}
static inline uint32_t gxs_rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static void gxs_lanes(const void *p, size_t n, uint32_t l[4])
{
    const unsigned char *b = (const unsigned char *) p;
    for (; n >= 16; n -= 16, b += 16) {
        uint32_t w[4]; memcpy(w, b, 16);
        l[0] = gxs_rotl((l[0] ^ w[0]) * 0x9E3779B1u, 13); l[1] = gxs_rotl((l[1] ^ w[1]) * 0x85EBCA6Bu, 11);
        l[2] = gxs_rotl((l[2] ^ w[2]) * 0xC2B2AE35u, 15); l[3] = gxs_rotl((l[3] ^ w[3]) * 0x27D4EB2Fu, 9);
    }
    if (n) {
        uint32_t w[4] = { 0, 0, 0, 0 }; memcpy(w, b, n);
        for (int i = 0; i < 4; ++i) l[i] = gxs_rotl((l[i] ^ w[i]) * (i & 1 ? 0x85EBCA6Bu : 0x9E3779B1u), 7 + 2 * i);
    }
}
static inline uint64_t gxs_mix64(uint64_t x) { x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33; return x; }
static void gxs_hash2(const void *a, size_t na, const void *b, size_t nb, uint64_t out[2])
{
#ifdef GXS_BREAK_MEMOHASH   /* negative control: every key collides, so the memo answers the wrong program */
    (void) a; (void) na; (void) b; (void) nb; out[0] = out[1] = 0x1234567u; return;
#endif
    uint32_t l[4] = { 0x243F6A88u ^ (uint32_t) na, 0x85A308D3u ^ (uint32_t) nb, 0x13198A2Eu, 0x03707344u };
    gxs_lanes(a, na, l); if (nb) gxs_lanes(b, nb, l);
    out[0] = gxs_mix64(((uint64_t) l[0] << 32 | l[1]) ^ gxs_mix64((uint64_t) l[2] << 32 | l[3]));
    out[1] = gxs_mix64(((uint64_t) l[3] << 32 | l[2]) + 0x9E3779B97F4A7C15ull * out[0]);
}
static inline GxmProg *gxs_mm_find(int t, const uint64_t h[2])
{
    const GxsMm *e = &s_mm[t][h[0] & (GXS_MM_N - 1u)];
    if (e->p && e->h0 == h[0] && e->h1 == h[1]) { ++c_mm_hit[t]; return e->p; }
    ++c_mm_miss[t]; return NULL;
}
static inline void gxs_mm_put(int t, const uint64_t h[2], GxmProg *p)
{
    GxsMm *e = &s_mm[t][h[0] & (GXS_MM_N - 1u)]; e->h0 = h[0]; e->h1 = h[1]; e->p = p;
}

static GxmProg *gxs_tev_core(const TevKey *raw, int *why, int use_memo)
{
    int w = GXM_SK_NOPROG;
    GxmProg *r = NULL;
    if (!g_up) { if (why) *why = GXM_SK_DEAD; return NULL; }
    if (g_gx.arm & GXM_ARM_NOPROG) { ++c_arm_noprog; if (why) *why = GXM_SK_NOPROG; return NULL; }
    gxs_async_poll();                         /* before any memo: they hold the NULL answers of keys that may have just landed */
    if (use_memo && s_tk_valid && !memcmp(raw, &s_tk_raw, sizeof *raw)) { if (why) *why = s_tk_why; return s_tk_prog; }   /* the last RAW key (rhi_vita_tev.inc:1236-1243) */
    uint64_t mh[2] = { 0, 0 };
    if (use_memo) {
        gxs_hash2(raw, sizeof *raw, NULL, 0, mh);
        GxmProg *m = gxs_mm_find(0, mh);
        if (m && !(c_mm_hit[0] & 63u)) {
            TevKey vck; uint32_t vh; int vf;
            gxs_tev_identity(&g_td, raw, g_fast, &vck, &vh, &vf);
            ++c_mm_ver; if (gxs_find(GXM_PK_TEV, vf, vh, NULL, &vck) != m) ++c_mm_bad;
        }
        if (m) { s_tk_raw = *raw; s_tk_valid = 1; s_tk_prog = m; s_tk_why = 0; if (why) *why = 0; return m; }
    }
    TevKey ck; uint32_t h; int fast;
    gxs_tev_identity(&g_td, raw, g_fast, &ck, &h, &fast);
    r = gxs_find(GXM_PK_TEV, fast, h, NULL, &ck);
    if (!r) r = gxs_make(GXM_PK_TEV, fast, h, NULL, &ck, &w);
    if (use_memo && (r || w != GXM_SK_PROGFULL)) { s_tk_raw = *raw; s_tk_valid = 1; s_tk_prog = r; s_tk_why = r ? 0 : w; }
    if (use_memo && r && r->ok) gxs_mm_put(0, mh, r);
    if (why) *why = r ? 0 : w;
    return r;
}
GxmProg *gxm_shader_tev(const TevKey *raw, int *why) { return gxs_tev_core(raw, why, 1); }

void gxs_td_stats(unsigned o[4]) { o[0] = g_td.seen; o[1] = g_td.aliased; o[2] = g_td.over; o[3] = g_td.nraw; }   /* hw_Mm2: distinct raw TEV keys, of them aliased, table-full, raw entries */
GxmProg *gxm_shader_gx(const VsKey *raw_vk, const TevKey *raw_tk, int *why) { return gxm_shader_gx_hint(raw_vk, raw_tk, 0ull, why); }
GxmProg *gxm_shader_gx_hint(const VsKey *raw_vk, const TevKey *raw_tk, unsigned long long hint, int *why)
{
    int w = GXM_SK_NOPROG;
    GxmProg *r = NULL;
    if (!g_up) { if (why) *why = GXM_SK_DEAD; return NULL; }
    if (g_gx.arm & GXM_ARM_NOPROG) { ++c_arm_noprog; if (why) *why = GXM_SK_NOPROG; return NULL; }
    gxs_async_poll();                         /* before any memo: they hold the NULL answers of keys that may have just landed */
    /* R3a C3 / R3c C5: the HAL's hint is equal to the one stored with the one-key memo: neither key was rebuilt (C3: serials + fog type; C5: the same key id), so the raw bytes are the stored ones.  s_ok.hint is
     * 0 whenever the stored keys came from a call without a hint, and every place that stores a key stores the hint of ITS call. */
    if (gxs_ok_hint_hit(&s_ok, hint)) { ++c_hint_hit; if (why) *why = s_ok.why; return s_ok.prog; }
    if (gxs_ok_keys_hit(&s_ok, raw_vk, raw_tk, hint)) { ++g_gxs_mm_st[0]; if (why) *why = s_ok.why; return s_ok.prog; }   /* rhi_vita_tev.inc:1445-1453 */
    uint64_t mh[2];
    gxs_hash2(raw_vk, sizeof *raw_vk, raw_tk, sizeof *raw_tk, mh);
    const int v2 = g_gxs_mm_v2;
    {   GxmProg *m = v2 ? gxs_mm2_find(mh) : gxs_mm_find(1, mh);
        if (v2) { if (m) ++c_mm_hit[1]; else ++c_mm_miss[1]; }
        if (m && !(c_mm_hit[1] & 63u)) {
            VsKey vvk; TevKey vck; uint32_t vh; int vf;
            gxs_gx_identity(&g_td, raw_vk, raw_tk, g_fast, g_persp, &vvk, &vck, &vh, &vf);
            ++c_mm_ver; if (gxs_find(GXM_PK_GX, vf, vh, &vvk, &vck) != m) ++c_mm_bad;
        }
        if (m) { ++g_gxs_mm_st[1]; gxs_ok_store(&s_ok, raw_vk, raw_tk, m, 0, hint); if (why) *why = 0; return m; }
    }
    ++g_gxs_mm_st[2];
    VsKey vk; TevKey ck; uint32_t h; int fast;
    gxs_gx_identity(&g_td, raw_vk, raw_tk, g_fast, g_persp, &vk, &ck, &h, &fast);
    r = gxs_find(GXM_PK_GX, fast, h, &vk, &ck);
    if (!r) r = gxs_make(GXM_PK_GX, fast, h, &vk, &ck, &w);
    if (r || w != GXM_SK_PROGFULL) gxs_ok_store(&s_ok, raw_vk, raw_tk, r, r ? 0 : w, hint);
    if (!v2) {   /* hw_Mm2: what the direct-mapped slot held when it missed */
        const GxsMm *e = &s_mm[1][mh[0] & (GXS_MM_N - 1u)];
        ++g_gxs_mm_st[!e->p ? 3 : e->p == r ? 4 : 5];
    } else {
        const GxsMm2 *e = s_mm2 ? &s_mm2[mh[0] & (GXS_MM2_SETS - 1u)] : NULL; int any = 0, same = 0;
        if (e) for (int i = 0; i < 4; ++i) { any |= e->p[i] != NULL; same |= e->p[i] == r && r; }
        ++g_gxs_mm_st[!any ? 3 : same ? 4 : 5];
    }
    if (r && r->ok) { if (v2) gxs_mm2_put(mh, r); else gxs_mm_put(1, mh, r); }
    if (why) *why = r ? 0 : w;
    return r;
}

GxmProg *gxm_shader_builtin(int which, int *why)
{
    if (which < 0 || which >= GXM_BI_N) { if (why) *why = GXM_SK_NOPROG; return NULL; }
    gxs_async_poll();
    if (g_bi_done[which]) { if (why) *why = g_bi_why[which]; return g_bi[which]; }
    TevKey k; int w = 0;
    gxs_builtin_key(which, &k);
    GxmProg *p = gxs_tev_core(&k, &w, 0);                       /* through the TEV path: same identity, same pack, same compiler */
    if (p || w != GXM_SK_PROGFULL) { g_bi[which] = p; g_bi_why[which] = p ? 0 : w; g_bi_done[which] = 1; }
    if (why) *why = p ? 0 : w;
    return p;
}

/* util programs: no key, no pack, no memo. The pool entry is permanent until shutdown. */
GxmProg *gxm_shader_util_precompiled(const char *name, const void *vgxp, const void *fgxp)
{
    if (!g_up || !name || !vgxp || !fgxp) { gxm_log("[gxm-shader] util_precompiled(%s): refused (module %s)", name ? name : "(null)", g_up ? "up" : "down"); return NULL; }
    GxsSlot *s = gxs_new_slot();
    if (!s) { ++c_prog_full; gxm_log("[gxm-shader] util_precompiled(%s): table full", name); return NULL; }
    snprintf(s->nm, sizeof s->nm, "%s", name);
    s->p.family = GXM_PK_UTIL; s->p.from = GXM_FROM_PRECOMPILED; s->p.name = s->nm;
    /* the arrays are static and stay alive: registered in place, not copied (owned = 0) */
    if (gxs_register(&s->p, &s->pv, (SceGxmProgram *) (uintptr_t) vgxp, (SceGxmProgram *) (uintptr_t) fgxp, 0)) { gxm_log("[gxm-shader] util_precompiled(%s): check / register FAILED", name); return NULL; }
    gxs_finish(&s->p, 1);
    ++g_np;
    return &s->p;
}

GxmProg *gxm_shader_util_cg(const char *name, const char *vs_src, const char *fs_src)
{
    if (!g_up || !name || !vs_src || !fs_src) return NULL;
    if (!g_shark_on || (g_gx.arm & GXM_ARM_NOPROG)) { gxm_log("[gxm-shader] util_cg(%s): compiler unavailable", name); return NULL; }
    uint32_t h = 2166136261u;
    for (const unsigned char *q = (const unsigned char *) name; *q; ++q) h = (h ^ *q) * 16777619u;
    if (gxs_is_bad(h)) { ++c_noprog_bad; gxm_log("[gxm-shader] util_cg(%s): key %08x is on the bad list", name, (unsigned) h); return NULL; }
    GxsSlot *s = gxs_new_slot();
    if (!s) { ++c_prog_full; return NULL; }
    snprintf(s->nm, sizeof s->nm, "%s", name);
    s->p.family = GXM_PK_UTIL; s->p.name = s->nm; s->p.hash = 0;
    const unsigned n1 = (unsigned) strlen(vs_src), n2 = (unsigned) strlen(fs_src);
    int r = gxs_try_gxpc(&s->p, &s->pv, 'u', h, vs_src, n1, fs_src, n2);
    if (r <= 0) {
        char hl[80]; snprintf(hl, sizeof hl, "// gxm util %s %08x\n", name, (unsigned) h);
        if (gxs_sentinel_arm(h, hl, vs_src, fs_src)) return NULL;
        uint32_t vsz = 0, fsz = 0;
        gxs_shark_take();
        SceGxmProgram *vg = gxs_compile_stage(vs_src, 0, &vsz);
        SceGxmProgram *fg = vg ? gxs_compile_stage(fs_src, 1, &fsz) : NULL;
        gxs_shark_give();
        gxs_sentinel_clear();
        if (!vg || !fg) { free(vg); free(fg); ++c_compile_fail; gxm_log("[gxm-shader] util_cg(%s): COMPILE FAILED (%s stage)", name, vg ? "fragment" : "vertex"); return NULL; }
        if (gxs_register(&s->p, &s->pv, vg, fg, 1)) return NULL;
        gxs_save_gxpc('u', h, vs_src, n1, fs_src, n2, s->p.vg, vsz, s->p.fg, fsz);
        s->p.from = GXM_FROM_SHARK;
        r = 1;
    }
    if (r != 1) return NULL;
    gxs_finish(&s->p, 1);
    ++g_np;
    gxm_log("[gxm-shader] util_cg(%s): ready (%s)", name, s->p.from == GXM_FROM_SHARK ? "compiled now" : "GXP cache");
    return &s->p;
}

/* ======================================================================================================================================== public: variants */
static int gxs_attrs_equal(const GxsVp *v, const GxmAttr *a, int n, int stride)
{
    if (v->stride != stride || v->n != n) return 0;
    for (int i = 0; i < n; ++i) {
        const GxsAttrC *c = &v->a[i];
        if (c->off != a[i].off || c->fmt != a[i].fmt || c->comps != a[i].comps || strncmp(c->name, a[i].name ? a[i].name : "", sizeof c->name - 1)) return 0;
    }
    return 1;
}
SceGxmVertexProgram *gxm_shader_vp(GxmProg *p, const GxmAttr *a, int n, int stride)
{
    /* n == 0 && a == NULL && stride == 0 is the INDEX-driven clear program (no attributes, no stream): header contract of the target module */
    const int noattr = (n == 0 && !a && stride == 0);
    if (!g_up || !p || !p->ok || (!noattr && (!a || n <= 0 || n > GXM_MAX_GPU_ATTRS || stride <= 0 || stride > 0xffff)) || n < 0 || !g_gx.patcher) { ++c_vp_fail; return NULL; }
    GxsPriv *pv = (GxsPriv *) p->priv;
    for (int i = 0; i < pv->nvp; ++i) if (gxs_attrs_equal(&pv->vp[i], a, n, stride)) return pv->vp[i].vp;
    SceGxmVertexAttribute at[GXM_MAX_GPU_ATTRS]; int na = 0;
    for (int k = 0; k < n; ++k) {
        const SceGxmProgramParameter *pp = a[k].name ? sceGxmProgramFindParameterByName(p->vg, a[k].name) : NULL;
        if (!pp) { ++c_unmapped_attr; continue; }                 /* the compiler dropped an attribute the key nominally reads */
        at[na].streamIndex = 0; at[na].offset = a[k].off; at[na].format = a[k].fmt; at[na].componentCount = a[k].comps;
        at[na].regIndex = (uint16_t) sceGxmProgramParameterGetResourceIndex(pp);
        ++na;
    }
    if (!na && !noattr) { ++c_vp_fail; return NULL; }
    const SceGxmVertexStream st = { (uint16_t) stride, SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    SceGxmVertexProgram *vp = NULL;
    const int vprc_ = noattr ? 0 : sceGxmShaderPatcherCreateVertexProgram(g_gx.patcher, p->vid, at, (unsigned) na, &st, 1u, &vp);
    {   /* R8 diagnostic (2026-10-26): every vertex program whose attributes include an aSk* -- the parameters as the GXP declares them */
        int sk_ = 0; for (int k = 0; k < n; ++k) if (a[k].name && !strncmp(a[k].name, "aSk", 3)) sk_ = 1;
        static unsigned nSkPrint_;
        if (sk_ && nSkPrint_ < 12) { ++nSkPrint_;
            printf("[r8-vp] rc 0x%08X stride %d na %d:", (unsigned) vprc_, stride, na);
            for (int k = 0; k < n; ++k) { const SceGxmProgramParameter *pp = a[k].name ? sceGxmProgramFindParameterByName(p->vg, a[k].name) : NULL;
                if (pp) printf(" %s(off %u fmt %u n %u | gxp type %d comps %u array %u reg %u)", a[k].name, a[k].off, a[k].fmt, a[k].comps, (int) sceGxmProgramParameterGetType(pp),
                               sceGxmProgramParameterGetComponentCount(pp), sceGxmProgramParameterGetArraySize(pp), sceGxmProgramParameterGetResourceIndex(pp)); }
            printf("\n"); }
    }
    if (noattr) { if (GXM_CHK(sceGxmShaderPatcherCreateVertexProgram(g_gx.patcher, p->vid, NULL, 0u, NULL, 0u, &vp)) || !vp) { ++c_vp_fail; return NULL; } }
    else if (GXM_CHK(vprc_) || !vp) { ++c_vp_fail; return NULL; }
    if (pv->nvp == pv->capvp) {
        const int nc = pv->capvp ? pv->capvp * 2 : 4;
        GxsVp *nv = (GxsVp *) realloc(pv->vp, (size_t) nc * sizeof *nv);
        if (!nv) { GXM_CHK(sceGxmShaderPatcherReleaseVertexProgram(g_gx.patcher, vp)); ++c_vp_fail; return NULL; }
        pv->vp = nv; pv->capvp = nc;
    }
    GxsAttrC *ac = (GxsAttrC *) calloc((size_t) (n ? n : 1), sizeof *ac);
    if (!ac) { GXM_CHK(sceGxmShaderPatcherReleaseVertexProgram(g_gx.patcher, vp)); ++c_vp_fail; return NULL; }
    for (int i = 0; i < n; ++i) { snprintf(ac[i].name, sizeof ac[i].name, "%s", a[i].name ? a[i].name : ""); ac[i].off = a[i].off; ac[i].fmt = a[i].fmt; ac[i].comps = a[i].comps; }
    pv->vp[pv->nvp].vp = vp; pv->vp[pv->nvp].stride = stride; pv->vp[pv->nvp].n = n; pv->vp[pv->nvp].a = ac;
    ++pv->nvp; ++c_vp_made;
    return vp;
}

/* blend: c2floor blend_of / rhi_vita.c:646-657. TEST / OPAQUE (and anything not BLEND / ADD) = no blend. */
static int gxs_blend_of(uint32_t am, SceGxmBlendInfo *b)
{
    static const SceGxmBlendFactor kSrc[8] = { SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                               SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO };
    static const SceGxmBlendFactor kDst[8] = { SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_SRC_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                               SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO };
    memset(b, 0, sizeof *b);
    const uint32_t mode = am & 0xffu;
    if (mode != RHI_ALPHA_BLEND && mode != RHI_ALPHA_ADD) return 0;
    b->colorMask = SCE_GXM_COLOR_MASK_ALL;
    b->colorFunc = b->alphaFunc = (am & 0x200u) ? SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT : SCE_GXM_BLEND_FUNC_ADD;
    if (am & 0x100u) { b->colorSrc = b->alphaSrc = kSrc[(am >> 12) & 7u]; b->colorDst = b->alphaDst = kDst[(am >> 16) & 7u]; }
    else { b->colorSrc = b->alphaSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; b->colorDst = b->alphaDst = mode == RHI_ALPHA_ADD ? SCE_GXM_BLEND_FACTOR_ONE : SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; }
    return 1;
}
SceGxmFragmentProgram *gxm_shader_fp(GxmProg *p, uint32_t alphaMode)
{
    if (!g_up || !p || !p->ok || !g_gx.patcher) { ++c_fp_fail; return NULL; }
    GxsPriv *pv = (GxsPriv *) p->priv;
    const uint32_t bk = gxm_blend_key(alphaMode);
    for (int i = 0; i < pv->nfp; ++i) if (pv->fp[i].bk == bk) return pv->fp[i].fp;
    SceGxmBlendInfo bi; const int has = gxs_blend_of(alphaMode, &bi);
    SceGxmFragmentProgram *fp = NULL;
    if (GXM_CHK(sceGxmShaderPatcherCreateFragmentProgram(g_gx.patcher, p->fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, has ? &bi : NULL, p->vg, &fp)) || !fp) { ++c_fp_fail; return NULL; }
    if (pv->nfp == pv->capfp) {
        const int nc = pv->capfp ? pv->capfp * 2 : 4;
        GxsFp *nf = (GxsFp *) realloc(pv->fp, (size_t) nc * sizeof *nf);
        if (!nf) { GXM_CHK(sceGxmShaderPatcherReleaseFragmentProgram(g_gx.patcher, fp)); ++c_fp_fail; return NULL; }
        pv->fp = nf; pv->capfp = nc;
    }
    pv->fp[pv->nfp].fp = fp; pv->fp[pv->nfp].bk = bk; ++pv->nfp; ++c_fp_made;
    return fp;
}
SceGxmFragmentProgram *gxm_shader_fp_nowrite(GxmProg *p)
{
    if (!g_up || !p || !p->ok || !g_gx.patcher) { ++c_fp_fail; return NULL; }
    GxsPriv *pv = (GxsPriv *) p->priv;
    if (pv->nowrite) return pv->nowrite;
    SceGxmBlendInfo bi; memset(&bi, 0, sizeof bi);
    bi.colorMask = SCE_GXM_COLOR_MASK_NONE;
    bi.colorFunc = bi.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    bi.colorSrc = bi.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE; bi.colorDst = bi.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    SceGxmFragmentProgram *fp = NULL;
    if (GXM_CHK(sceGxmShaderPatcherCreateFragmentProgram(g_gx.patcher, p->fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, &bi, p->vg, &fp)) || !fp) { ++c_fp_fail; return NULL; }
    pv->nowrite = fp; ++c_fp_made;
    return fp;
}

/* ======================================================================================================================================== public: housekeeping + report */
void rhi_gxm_asc_tick(void) { ++c_noop; }                       /* the background compiler's results are installed by the lookups themselves (gxs_async_poll) */
void rhi_gxm_shc_flush(unsigned ms) { (void) ms; ++c_noop; }    /* M2: the pack is read-only */
void rhi_gxm_shc_shutdown(void) { ++c_noop; }

int gxm_shader_report(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    int n = snprintf(buf, (size_t) cap,
                     "pack_records=%u pack_hit=%u pack_miss=%u pack_stale=%u pack_reads=%u shark_online=%d shark_compiles=%u shark_fail=%u gxpc_hit=%u gxpc_app=%u prog_live=%d "
                     "vp_made=%u fp_made=%u unmapped_unif=%u unmapped_attr=%u missing_samplers=%u refused_gen=%u noprog_bad=%u "
                     "pack_bad=%u split_fail=%u compile_fail=%u vp_fail=%u fp_fail=%u prog_full=%u arm_noprog=%u sent_fail=%u gxpc_write=%u shader_noop=%u ",
                     g_pk.n, c_pack_hit, c_pack_miss, c_pack_stale, c_pack_reads, g_shark_on, c_shark_compiles, c_shark_fail, c_gxpc_hit, c_gxpc_app, g_np,
                     c_vp_made, c_fp_made, c_unmapped_unif, c_unmapped_attr, c_missing_samplers, c_refused_gen, c_noprog_bad,
                     c_pack_bad, c_split_fail, c_compile_fail, c_vp_fail, c_fp_fail, c_prog_full, c_arm_noprog, c_sent_fail, c_gxpc_write, c_noop);
    if (n > 0 && n < cap) n += snprintf(buf + n, (size_t) (cap - n), "async_on=%d async_queued=%u async_installed=%u async_failed=%u async_full=%u async_pending_hits=%u async_inflight_max=%u async_ms_max=%u async_ms_sum=%u ",
                                        g_async, c_async_queued, c_async_installed, c_async_failed, c_async_full, c_async_pending, c_async_inflight_max, (unsigned) c_async_ms_max, (unsigned) c_async_ms_sum);
    if (n > 0 && n < cap) n += snprintf(buf + n, (size_t) (cap - n), "memo_tev_hit=%u memo_tev_miss=%u memo_gx_hit=%u memo_gx_miss=%u memo_verified=%u memo_bad=%u memo_hint_hit=%u ",
                                        c_mm_hit[0], c_mm_miss[0], c_mm_hit[1], c_mm_miss[1], c_mm_ver, c_mm_bad, c_hint_hit);
    {   /* R3a C3 (orchestrator, 2026-10-24): the HAL's own generation memos, and the GX_C3_SELFCHECK counters when the HAL was built with it (weak: 0 otherwise) */
        extern unsigned gx_tev_memo_hits __attribute__((weak)), gx_tev_memo_miss __attribute__((weak)), gx_vs_memo_hits __attribute__((weak)), gx_vs_memo_miss __attribute__((weak));
        extern unsigned gx_c3_chk_tev __attribute__((weak)), gx_c3_chk_vs_ok __attribute__((weak)), gx_c3_chk_bad __attribute__((weak));
#define W_(s) (&(s) ? (s) : 0u)
        if (n > 0 && n < cap) n += snprintf(buf + n, (size_t) (cap - n), "c3_tev_hit=%u c3_tev_miss=%u c3_vs_hit=%u c3_vs_miss=%u c3_chk_tev=%u c3_chk_vs_ok=%u c3_chk_bad=%u ",
                                            W_(gx_tev_memo_hits), W_(gx_tev_memo_miss), W_(gx_vs_memo_hits), W_(gx_vs_memo_miss), W_(gx_c3_chk_tev), W_(gx_c3_chk_vs_ok), W_(gx_c3_chk_bad));
#undef W_
    }
    if (n < 0) return 0;
    if (n >= cap) n = cap - 1;
    return n;
}
#endif /* !GXM_SHADER_HOST */
