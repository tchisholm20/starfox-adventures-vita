/* gxd_format.h - the ONE-FRAME DRAW-STREAM DUMP ("GXDM" v1): what the writer (P/src/gx_dump.h, in the Stairfax game) puts on the card
 * and what the c2floor replay app (projects/gxaot/probe/c2floor) reads. docs/54 s5.1 H1-pre (b), risk R1.
 *
 * WHY THIS EXISTS.  The C2 kill test must replay a REAL chapter-2 draw list (real state sequence, real mesh addresses, 72-program and
 * texture churn, per-draw uniform fill from RECORDED state), not a synthetic same-state loop. So the dump carries everything a native
 * sceGxmDraw needs and nothing the GPU would need to make a picture: texture TEXELS are not recorded (the CPU never reads them on
 * submit), vertex/index bytes are (the meshes are the working set), uniforms are (the assembly cost depends on them).
 *
 * LAYOUT.  File = 16-byte header {magic, version, flags, 0} then records {u32 tag, u32 len, payload[len]}; len is a multiple of 4.
 * Every integer is a little-endian u32 (the Vita and the host are both LE); floats are IEEE binary32. Nothing here is a pointer.
 * The raw game structs (VsKey, TevKey, VsUniforms, TevUniforms, TevVertex, GxMeshLayout) travel as OPAQUE BYTES whose sizeof is in META;
 * a reader built against a different gx_tev_key.h refuses the file (sizes are compared), it does not misread it.
 *
 *   META  (1)  GxdMeta                                           first record, once
 *   PROG  (2)  GxdProg   + program binary (vitaGL's glGetProgramBinary image)
 *   MESH  (3)  GxdMesh   + GxMeshLayout bytes + vertex bytes + u16 index bytes (each padded to 4)
 *   TEX   (4)  GxdTex                                            texture IDENTITY (dimensions, sampler state); no texels
 *   DRAW  (5)  GxdDraw   + kind 1 (GPU path): VsKey TevKey VsUniforms TevUniforms
 *                          kind 2 (immediate/CPU path): TevKey TevUniforms mvp[16] nverts x TevVertex
 *   EVENT (6)  GxdEvent                                          non-draw events in stream order (EFB copy ...)
 *   END   (127) GxdEnd                                           last record; fnv32 covers every byte before this record header
 *
 * The three index fields of a DRAW (prog, mesh, tex[]) are RECORD ORDINALS: the n-th PROG/MESH/TEX record of the file, 0-based.
 */
#ifndef GXD_FORMAT_H
#define GXD_FORMAT_H
#include <stdint.h>

#define GXD_MAGIC    0x4D445847u      /* bytes 'G','X','D','M' */
#define GXD_VERSION  1u
#define GXD_FLAGS_TRUNCATED 1u        /* header/END flag: the writer ran out of its byte or draw budget, the frame is INCOMPLETE */

enum { GXD_T_META = 1, GXD_T_PROG = 2, GXD_T_MESH = 3, GXD_T_TEX = 4, GXD_T_DRAW = 5, GXD_T_EVENT = 6, GXD_T_END = 127 };
enum { GXD_K_GX = 1, GXD_K_TEV = 2 };                 /* DRAW kinds: drawTevGx (mesh, GPU vertex path) / drawTev (immediate vertices) */
enum { GXD_EV_EFB_COPY = 1 };                          /* a = x, b = y, c = w, d = h, e = dw, f = dh; the copy ends the open scene */

#define GXD_MAX_TEX 8
#define GXD_META_SYNTHETIC 1u         /* META.flags bit 0: the stream was GENERATED (tools/mkdump), not recorded from the game.  A synthetic list can only KILL C2, never confirm it (docs/54 s5.1) */

typedef struct { uint32_t magic, version, flags, reserved; } GxdFileHdr;
typedef struct { uint32_t tag, len; } GxdRecHdr;

typedef struct {
    uint32_t size_VsKey, size_TevKey, size_VsUniforms, size_TevUniforms, size_TevVertex, size_GxMeshLayout;
    uint32_t tevgen_version;
    uint32_t render_w, render_h;          /* rhi_vita_render_size(): M's size */
    uint32_t present_no;                  /* the present the dumped frame was recorded in */
    uint32_t flags;
    uint32_t reserved[5];
    char     build[32];                   /* free text: the game's build id if the writer knows it */
} GxdMeta;

typedef struct {
    uint32_t key;                         /* the writer's identity (kind << 16 | pair index): informational */
    uint32_t hash, tevhash;               /* GxProg.hash / .tevHash (informational: names the pair in the game's log) */
    uint32_t kind;                        /* 'g' exact, 'h' fast (the pack's kind letters), 't' CPU-path TEV program */
    uint32_t bloblen;                     /* bytes of the program binary that follow (padded to 4 in the file) */
    uint32_t reserved;
} GxdProg;

typedef struct { uint32_t key_lo; uint32_t vbytes, nidx, reserved; } GxdMesh;   /* then GxMeshLayout, vbytes (pad4), nidx*2 (pad4) */

typedef struct {
    uint32_t key_lo;
    uint32_t w, h, fmt;                   /* VitaTexture.w/.h/.fmt */
    uint32_t filtMin, filtMagNear, hasMips, wrapS, wrapT;
    int32_t  lodBias8;
    uint32_t flags;                       /* bit 0: an EFB copy texture */
    uint32_t reserved;
} GxdTex;

typedef struct {
    uint32_t kind;                        /* GXD_K_* */
    uint32_t seq;                         /* 0-based position among the frame's DRAW records */
    uint32_t prog;                        /* PROG ordinal */
    uint32_t mesh;                        /* MESH ordinal (kind 1); 0xFFFFFFFF for kind 2 */
    uint32_t alpha_mode;                  /* RhiAlphaMode | exact-factor bits, exactly as drawTevGx received it */
    uint32_t depth;                       /* g_depth: bit0 test, bits1-3 GX func, bit4 write; 0xFFFFFFFF = "reference" (-1) */
    uint32_t scene_kind;                  /* rhi_vita_scene_kind at the draw: 'M','C','Z','S','D','B' ... the target the draw lands on */
    uint32_t ntex;
    uint32_t tex[GXD_MAX_TEX];            /* TEX ordinals */
    uint32_t nverts;                      /* kind 2 */
    uint32_t reserved[3];
} GxdDraw;

typedef struct { uint32_t kind, seq, a, b, c, d, e, f; } GxdEvent;   /* seq = number of DRAW records written before it */

typedef struct {
    uint32_t ndraws, nprogs, nmeshes, ntex, nevents;
    uint32_t flags;                       /* GXD_FLAGS_TRUNCATED */
    uint32_t dropped_draws;               /* draws the writer could not record (budget) */
    uint32_t fnv32;                       /* FNV-1a over every file byte before this record's header */
} GxdEnd;

#define GXD_PAD4(n) (((uint32_t)(n) + 3u) & ~3u)

#endif
