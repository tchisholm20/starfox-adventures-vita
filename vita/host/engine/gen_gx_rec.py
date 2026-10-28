#!/usr/bin/env python3
"""gen_gx_rec.py -- the GX boundary table, its generated wrappers, and the lints that keep them honest.
docs/53 section 2.3 / 3 / 10 (T0-a): record-and-discard wrappers.  This file is the SINGLE SOURCE for

    src/gx_rec_wraps.inc          __wrap_X for all 133 boundary symbols (+ __real_X decls + the report tables)
    host/engine/gx_wraps.txt      one `-Wl,--wrap=X` per line; build_game_vita.sh splices it into WRAPS

    python gen_gx_rec.py gen                 rewrite both generated files from TABLE
    python gen_gx_rec.py lint  [--elf E | --nm-file F] [--wraps-file F] [--build-sh F] [--inc-file F]   (overrides: for mutation demos)
                                             (1) TABLE == the ELF's defined GX*/VI* + gxfifo_push_* (nothing missing, nothing extra,
                                                 no duplicate), class counts == docs/53 section 3 (49/12/16/7/11/31 + 7 pipe);
                                             (2) gx_wraps.txt == TABLE, the build script reads it and carries no hand-written GX/VI
                                                 --wrap of its own, the generated .inc is not stale (hash + text);
                                             exit 0 clean, 9 on any finding
    python gen_gx_rec.py prelink             (2) only (no ELF yet) -- the build script runs this before the link
    python gen_gx_rec.py postlink ELF [--wraps-used F]
                                             (3) after a build: __wrap_X defined and X defined for every row, no undefined
                                                 __real_*/__wrap_* left, gx_rec.o's __real_ set == TABLE, the effective --wrap set
                                                 of the link (wraps_used.txt written by the build script) == TABLE, and the static
                                                 count of call sites that really reach each __wrap_ (informational)
    python gen_gx_rec.py selftest            watches (1), (2) and the freshness check FAIL on mutated tables / files, and pass on
                                             the real ones.  A lint that has never failed is not a lint (repo standard).

The boundary is the 126 defined GX*/VI* symbols of docs/53 Appendix A (nm of the game ELF, T and W) + the 7 gxfifo_push_*.
--wrap redirects references from OTHER objects only: calls inside gx_draw.c (e.g. gxfifo_push_f32 -> gxfifo_push_u32,
GXSetTevIndRepeat -> GXSetTevIndirect) are NOT wrapped, which is intended: T0-a prices the GAME's calls.

THREADED LAYER (RT_THREAD.md 6.2, module "gen").  The same table now also drives the render-thread wrappers.  Input:
host/engine/gx_thr_table.txt (frozen by the planner: thr R/L/X/P/E/T + edge per row, plus the 19 [extra] rows with a full
prototype).  Extra outputs (python gen_gx_rec.py gen writes ALL of them):

    src/gx_rec_wraps.inc    + one line per __wrap_X, after the mode-0 test:  if (g_rec_mode == GXR_MODE_THREAD) { <action> }
    src/gx_rt_gen.inc       gxt_G_X (R rows: record by value), the 19 extras' __wrap_, int gxt_r_exec(op, p, nw) (the R dispatch)
    src/gx_rt_ops.h         GXT_OP_* (rows sorted by name, op = index + 1; extras op = 200 + index)
    src/gx_rt_special.h     gxts_G_X / gxts_R_X prototypes of the X rows
    host/engine/gx_wraps.txt  133 + 19 --wrap lines (rhi_present / rhi_create stay hand-wired in build_game_vita.sh)

    python gen_gx_rec.py stubs F             host test only: argument-capturing __real_X stand-ins (gx_rec_test)
    python gen_gx_rec.py drivers F           host test only: one random-argument driver per R row (gx_rec_test)
    python gen_gx_rec.py standins F          host replay test only: plain record-by-value stand-ins for the custom (X) rows
    python gen_gx_rec.py hoststubs F NMFILE  host replay test only: strong stand-ins for the symbols the host HAL objects do not define
    python gen_gx_rec.py sweep F             host state test only (gx_rt_state_test): one random-argument driver per S row, ENUMERATED FROM THE TABLE (a new S row is covered with no edit)
"""
import hashlib, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__)).replace('\\', '/')
V = os.path.normpath(os.path.join(HERE, '..', '..')).replace('\\', '/')          # projects/stairfax-vita
INC = V + '/src/gx_rec_wraps.inc'
GEN_INC = V + '/src/gx_rt_gen.inc'
OPS_H = V + '/src/gx_rt_ops.h'
SPECIAL_H = V + '/src/gx_rt_special.h'
THR_TXT = HERE + '/gx_thr_table.txt'
WRAPS_TXT = HERE + '/gx_wraps.txt'
BUILD_SH = HERE + '/build_game_vita.sh'
GX_REC_C = V + '/src/gx_rec.c'
# the isolated render-thread copy (RT_THREAD.md rule 0); STFX_SRC / OUT override
import devcfg                                    # dev_defaults.env: the developer's own paths (absent in the public tree)
DEFAULT_ELF = devcfg.dev('TOOLS_ELF')            # None in the public tree: pass --elf / GXR_ELF
STFX_DEFAULT = devcfg.dev('TOOLS_SRC')           # None in the public tree: export STFX_SRC

# nm filter of docs/53 Appendix A, plus the pipe
NM_RE = re.compile(r'^[0-9a-fA-F]+\s+[TtWw]\s+((?:GX|VI)[A-Za-z0-9_]*|gxfifo_push_(?:u8|s8|u16|s16|u32|s32|f32))$')

# --- THE TABLE -------------------------------------------------------------------------------------------------------
# cls|rec|return type|name|parameters (the caller-visible prototype, copied from include/dolphin/{gx,vi}/*.h)|inline payload
#   cls  A state/command write  A2 object fill  B reads state back  C references game memory  D sync point  E no-op today
#        (docs/53 section 3; P = the 7 gxfifo_push_*, appended below).
#   rec  1 = a record is written in mode 2 (design: A, C, D-fence; A2 only for GXInitTexObjLOD's TEXLOD_NOTE).  0 = counted only.
#        GXEndDisplayList/getters/E/D-G (break-point trio, retrace callbacks, VIInit) are not recorded -- they run G-locally.
#   inline  `arg:bytes` = the record carries `bytes` of what the pointer `arg` points at (by value, as the real record must:
#        the pointer is read at call time), instead of the pointer itself.  Bytes may be a C expression over the parameters.
TABLE_TEXT = """
A |1|void             |GXBegin                 |GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts|
C |1|void             |GXBeginDisplayList      |void* list, u32 size|
C |1|void             |GXCallDisplayList       |void* list, u32 nbytes|
A |1|void             |GXClearVtxDesc          |void|
E |0|void             |GXCopyDisp              |void* dest, GXBool clear|
C |1|void             |GXCopyTex               |void* dest, GXBool clear|
D |0|void             |GXDisableBreakPt        |void|
E |0|void             |GXDrawTorus             |f32 rc, u8 numc, u8 numt|
D |0|void             |GXEnableBreakPt         |void* break_pt|
E |0|void             |GXEnableTexOffsets      |GXTexCoordID coord, u8 line_enable, u8 point_enable|
C |0|u32              |GXEndDisplayList        |void|
E |0|void             |GXFlush                 |void|
D |1|int              |GXFlush_                |u8 visible, int unused|
B |0|void             |GXGetFifoPtrs           |GXFifoObj* fifo, void** readPtr, void** writePtr|
B |0|void             |GXGetGPStatus           |GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt|
B |0|void             |GXGetLightAttnK         |const GXLightObj* lt_obj, f32* k0, f32* k1, f32* k2|
B |0|void             |GXGetScissor            |u32* left, u32* top, u32* wd, u32* ht|
B |0|u32              |GXGetTexBufferSize      |u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod|
B |0|void*            |GXGetTexObjData         |const GXTexObj* to|
B |0|GXTexFmt         |GXGetTexObjFmt          |const GXTexObj* to|
B |0|u16              |GXGetTexObjHeight       |const GXTexObj* to|
B |0|void*            |GXGetTexObjUserData     |const GXTexObj* obj|
B |0|u16              |GXGetTexObjWidth        |const GXTexObj* to|
E |0|GXFifoObj*       |GXInit                  |void* base, u32 size|
E |0|void             |GXInitFifoBase          |GXFifoObj* fifo, void* base, u32 size|
E |0|void             |GXInitFifoLimits        |GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark|
A2|0|void             |GXInitLightAttn         |GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2|
A2|0|void             |GXInitLightAttnA        |GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2|
A2|0|void             |GXInitLightAttnK        |GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2|
A2|0|void             |GXInitLightColor        |GXLightObj* lt_obj, GXColor color|
A2|0|void             |GXInitLightDir          |GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz|
A2|0|void             |GXInitLightDistAttn     |GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func|
A2|0|void             |GXInitLightPos          |GXLightObj* lt_obj, f32 x, f32 y, f32 z|
A2|0|void             |GXInitLightSpot         |GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func|
A2|0|void             |GXInitSpecularDir       |GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz|
A2|0|void             |GXInitTexObj            |GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, GXBool mipmap|
A2|1|void             |GXInitTexObjLOD         |GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso|obj:32
A2|0|void             |GXInitTexObjUserData    |GXTexObj* obj, void* user_data|
E |0|void             |GXInvalidateTexAll      |void|
E |0|void             |GXInvalidateVtxCache    |void|
A |1|void             |GXLoadLightObjImm       |const GXLightObj* lt_obj, GXLightID light|lt_obj:64
A |1|void             |GXLoadNrmMtxImm         |const f32 mtx[3][4], u32 id|mtx:48
A |1|void             |GXLoadPosMtxImm         |const f32 mtx[3][4], u32 id|mtx:48
A |1|void             |GXLoadTexMtxImm         |const f32 mtx[][4], u32 id, GXTexMtxType type|mtx:(type==GX_MTX2x4?32:48)
C |1|void             |GXLoadTexObj            |GXTexObj* obj, GXTexMapID id|obj:32
C |1|void             |GXLoadTexObjPreLoaded   |GXTexObj* obj, GXTexRegion* region, GXTexMapID id|obj:32
B |0|void             |GXPeekZ                 |u16 x, u16 y, u32* z|
E |0|void             |GXPixModeSync           |void|
E |0|void             |GXPreLoadEntireTexture  |GXTexObj* tex_obj, GXTexRegion* region|
B |0|u16              |GXReadDrawSync          |void|
B |0|void             |GXReadXfRasMetric       |u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks|
E |0|void             |GXResetWriteGatherPipe  |void|
A |1|void             |GXSetAlphaCompare       |GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1|
E |0|void             |GXSetAlphaUpdate        |GXBool update_enable|
C |1|void             |GXSetArray              |GXAttr attr, void* base_ptr, u8 stride|
A |1|void             |GXSetBlendMode          |GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op|
D |0|GXBreakPtCallback|GXSetBreakPtCallback    |GXBreakPtCallback cb|
A |1|void             |GXSetChanAmbColor       |GXChannelID chan, GXColor amb_color|
A |1|void             |GXSetChanCtrl           |GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn|
A |1|void             |GXSetChanMatColor       |GXChannelID chan, GXColor mat_color|
A |1|void             |GXSetColorUpdate        |GXBool update_enable|
A |1|void             |GXSetCopyClear          |GXColor clear_clr, u32 clear_z|
E |0|void             |GXSetCopyFilter         |GXBool aa, const u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7]|
E |0|void             |GXSetCPUFifo            |GXFifoObj* fifo|
A |1|void             |GXSetCullMode           |GXCullMode mode|
A |1|void             |GXSetCurrentMtx         |u32 id|
E |0|void             |GXSetDispCopyDst        |u16 wd, u16 ht|
E |0|void             |GXSetDispCopyGamma      |GXGamma gamma|
E |0|void             |GXSetDispCopySrc        |u16 left, u16 top, u16 wd, u16 ht|
E |0|u32              |GXSetDispCopyYScale     |f32 vscale|
E |0|void             |GXSetDither             |GXBool dither|
D |1|void             |GXSetDrawSync           |u16 token|
E |0|void             |GXSetFieldMode          |GXBool field_mode, GXBool half_aspect_ratio|
A |1|void             |GXSetFog                |GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color|
E |0|void             |GXSetGPFifo             |GXFifoObj* fifo|
E |0|void             |GXSetGPMetric           |GXPerf0 perf0, GXPerf1 perf1|
A |1|void             |GXSetIndTexCoordScale   |GXIndTexStageID ind_state, GXIndTexScale scale_s, GXIndTexScale scale_t|
A |1|void             |GXSetIndTexMtx          |GXIndTexMtxID mtx_id, const f32 offset[2][3], s8 scale_exp|offset:24
A |1|void             |GXSetIndTexOrder        |GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map|
E |0|void             |GXSetLineWidth          |u8 width, GXTexOffset texOffsets|
E |0|void             |GXSetMisc               |GXMiscToken token, u32 val|
A |1|void             |GXSetNumChans           |u8 nChans|
A |1|void             |GXSetNumIndStages       |u8 nIndStages|
A |1|void             |GXSetNumTevStages       |u8 nStages|
A |1|void             |GXSetNumTexGens         |u8 nTexGens|
E |0|void             |GXSetPixelFmt           |GXPixelFmt pix_fmt, GXZFmt16 z_fmt|
E |0|void             |GXSetPointSize          |u8 pointSize, GXTexOffset texOffsets|
A |1|void             |GXSetProjection         |const f32 mtx[4][4], GXProjectionType type|mtx:64
A |1|void             |GXSetScissor            |u32 left, u32 top, u32 wd, u32 ht|
A |1|void             |GXSetTevAlphaIn         |GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d|
A |1|void             |GXSetTevAlphaOp         |GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg|
A |1|void             |GXSetTevColor           |GXTevRegID id, GXColor color|
A |1|void             |GXSetTevColorIn         |GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d|
A |1|void             |GXSetTevColorOp         |GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg|
A |1|void             |GXSetTevColorS10        |GXTevRegID id, GXColorS10 color|
A |1|void             |GXSetTevDirect          |GXTevStageID tev_stage|
A |1|void             |GXSetTevIndirect        |GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format, GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t, GXBool add_prev, GXBool utc_lod, GXIndTexAlphaSel alpha_sel|
A |1|void             |GXSetTevIndRepeat       |GXTevStageID tev_stage|
A |1|void             |GXSetTevKAlphaSel       |GXTevStageID stage, GXTevKAlphaSel sel|
A |1|void             |GXSetTevKColor          |GXTevKColorID id, GXColor color|
A |1|void             |GXSetTevKColorSel       |GXTevStageID stage, GXTevKColorSel sel|
A |1|void             |GXSetTevOp              |GXTevStageID id, GXTevMode mode|
A |1|void             |GXSetTevOrder           |GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color|
A |1|void             |GXSetTevSwapMode        |GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel|
A |1|void             |GXSetTevSwapModeTable   |GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha|
A |1|void             |GXSetTexCoordGen2       |GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize, u32 pt_texmtx|
A |1|void             |GXSetTexCopyDst         |u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap|
A |1|void             |GXSetTexCopySrc         |u16 left, u16 top, u16 wd, u16 ht|
A |1|void             |GXSetViewport           |f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz|
A |1|void             |GXSetViewportJitter     |f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field|
A |1|void             |GXSetVtxAttrFmt         |GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac|
A |1|void             |GXSetVtxDesc            |GXAttr attr, GXAttrType type|
E |0|void             |GXSetZCompLoc           |GXBool before_tex|
A |1|void             |GXSetZMode              |GXBool compare_enable, GXCompare func, GXBool update_enable|
A |1|void             |GXStfxSkinBind          |const GxSkinTable* t, const f32* palette, u32 nb|palette:(nb*48)
E |0|void             |GXTexModeSync           |void|
E |0|void             |VIConfigure             |const GXRenderModeObj* rm|
E |0|void             |VIFlush                 |void|
B |0|u32              |VIGetDTVStatus          |void|
B |0|u32              |VIGetNextField          |void|
B |0|u32              |VIGetTvFormat           |void|
D |0|void             |VIInit                  |void|
D |1|void             |VISetBlack              |BOOL black|
D |1|void             |VISetNextFrameBuffer    |void* fb|
D |0|VIRetraceCallback|VISetPostRetraceCallback|VIRetraceCallback callback|
D |0|VIRetraceCallback|VISetPreRetraceCallback |VIRetraceCallback callback|
D |1|void             |VIWaitForRetrace        |void|
"""

# The 7 pipe functions: (name, C type, bytes)
PIPE = [('gxfifo_push_u8', 'unsigned char', 1), ('gxfifo_push_s8', 'signed char', 1),
        ('gxfifo_push_u16', 'unsigned short', 2), ('gxfifo_push_s16', 'short', 2),
        ('gxfifo_push_u32', 'unsigned int', 4), ('gxfifo_push_s32', 'int', 4), ('gxfifo_push_f32', 'float', 4)]

CLS_ORDER = ['A', 'A2', 'B', 'C', 'D', 'E', 'P']
DOC_CLASS_COUNTS = {'A': 50, 'A2': 12, 'B': 16, 'C': 7, 'D': 11, 'E': 31, 'P': 7}      # docs/53 section 3 table
OP_VBYTES, OP_PAD = 1000, 1001                                                           # record ops that are not an entry point

# N_tev: TEV / texgen / indirect setters (docs/53 section 10 T0-a).  By name prefix, as the task fixes it; the lint prints the set.
TEV_RE = re.compile(r'^(GXSetTev\w+|GXSetTexCoordGen\w*|GXSetNumTexGens|GXSetNumTevStages|GXSetInd\w+|GXSetNumInd\w+)$')
MTX_NAMES = {'GXLoadPosMtxImm', 'GXLoadNrmMtxImm', 'GXLoadTexMtxImm'}                    # "matrix loads" (docs/53 section 6.2)
TEX_NAMES = {'GXLoadTexObj', 'GXLoadTexObjPreLoaded'}
K_TEV, K_DL, K_PRIM, K_TEX, K_MTX, K_COPYTEX, K_PUSH = 1, 2, 4, 8, 16, 32, 64


def parse_table(text=None, attach=True):
    rows = []
    for ln in (TABLE_TEXT if text is None else text).splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        c = [x.strip() for x in ln.split('|')]
        if len(c) != 6:
            raise SystemExit('bad table row (want 6 fields): ' + ln)
        cls, rec, ret, name, params, inl = c
        rows.append(dict(cls=cls, rec=int(rec), ret=ret, name=name, params=params, inline=inl))
    for n, t, b in PIPE:
        rows.append(dict(cls='P', rec=1, ret='void', name=n, params='%s v' % t, inline='', pipe_bytes=b))
    if attach:
        attach_thr(rows, parse_thr()[0])           # the threaded disposition (gx_thr_table.txt) rides on the rows
    return rows


def canon(rows):
    return '\n'.join('|'.join([r['name'], r['ret'], r['params'], r['cls'], str(r['rec']), r['inline']])
                     for r in sorted(rows, key=lambda r: r['name']))


def table_hash(rows):
    h = 0x811C9DC5                                    # FNV-1a 32
    for b in canon(rows).encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return '%08x' % h


def argnames(params):
    if params.strip() in ('', 'void'):
        return []
    out = []
    for p in params.split(','):
        m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\]\s*)*$', p.strip())
        if not m:
            raise SystemExit('cannot find the argument name in: ' + p)
        out.append(m.group(1))
    return out


def sorted_rows(rows):
    return sorted(rows, key=lambda r: r['name'])


def wraps_lines(rows, extras=None):
    """the --wrap set: the 133 table symbols + the 19 [extra] symbols (rhi_present / rhi_create stay hand-wired in build_game_vita.sh)"""
    if extras is None:
        extras = parse_thr()[1]
    return ['-Wl,--wrap=%s' % r['name'] for r in all_entries(rows, extras)]


# --- C generation ----------------------------------------------------------------------------------------------------
HOOKS = {   # class-specific bookkeeping that is not a plain counter (all of it cheap; only runs in mode > 0)
    'GXCallDisplayList': '    g_dl_bytes += nbytes;\n',
    'GXBeginDisplayList': '    g_in_list = 1;\n',
    'GXEndDisplayList': '    g_in_list = 0;\n',
    'GXSetTexCopyDst': '    g_dst_fmt = (unsigned) fmt;\n',
    'GXCopyTex': '    if (g_dst_fmt == 0x2Au) ++g_copytex_b8;\n',
}


def kind_of(r):
    k = 0
    if TEV_RE.match(r['name']): k |= K_TEV
    if r['name'] == 'GXCallDisplayList': k |= K_DL
    if r['name'] == 'GXBegin': k |= K_PRIM
    if r['name'] in TEX_NAMES: k |= K_TEX
    if r['name'] in MTX_NAMES: k |= K_MTX
    if r['name'] == 'GXCopyTex': k |= K_COPYTEX
    if r['cls'] == 'P': k |= K_PUSH
    return k


def gen_wrapper(r, idx):
    name, ret, params = r['name'], r['ret'], r['params']
    names = argnames(params)
    call = '__real_%s(%s)' % (name, ', '.join(names))
    # the instrumented body is its own out-of-line function so the mode-0 wrapper is a LEAF: load, branch, tail call (no frame)
    s = 'static __attribute__((noinline)) %s gxr_on_%s(%s)\n{\n' % (ret, name, params)
    s += '    GXR_T0;\n    ++g_cnt[%d];\n' % idx
    s += HOOKS.get(name, '')
    if r['cls'] == 'P':
        n = r['pipe_bytes']
        s += '    if (g_in_list) ++g_push_in_list;\n'
        if name.endswith('f32'):
            s += '    if (g_rec_mode == 2 && !g_in_list) { unsigned u_; memcpy(&u_, &v, 4); gxr_vb_put(u_, 4); }\n'
        else:
            s += '    if (g_rec_mode == 2 && !g_in_list) gxr_vb_put((unsigned) v%s, %d);\n' % \
                 ('' if n == 4 else ' & %s' % ('0xFFu' if n == 1 else '0xFFFFu'), n)
    elif r['rec']:
        inl = r['inline']
        ia, ib = (inl.split(':', 1) if inl else (None, None))
        byval = [a for a in names if a != ia]
        nw = ' + '.join(['GXR_W(%s)' % a for a in byval] + (['(((unsigned)(%s) + 3u) >> 2)' % ib] if inl else [])) or '0'
        s += '    if (g_rec_mode == 2) {\n        unsigned *p_ = gxr_alloc(%d, %s);\n        if (p_) {\n' % (idx + 1, nw)
        for a in byval:
            s += '            GXR_PUT(p_, %s);\n' % a
        if inl:
            s += '            { const unsigned nb_ = (unsigned)(%s); memcpy(p_, %s, nb_); p_ += (nb_ + 3u) >> 2; }\n' % (ib, ia)
        s += '        }\n    }\n'
    s += '    GXR_T1;\n    return %s;\n}\n' % call
    s += '%s __wrap_%s(%s)\n{\n    if (__builtin_expect(!g_rec_mode, 1)) return %s;\n    if (g_rec_mode == GXR_MODE_THREAD) { %s }\n    return gxr_on_%s(%s);\n}\n' % \
         (ret, name, params, call, thr_action(r), name, ', '.join(names))
    return s


def gen_inc(rows):
    rows = sorted_rows(rows)
    h = table_hash(rows)
    o = []
    o.append('/* gx_rec_wraps.inc -- GENERATED by host/engine/gen_gx_rec.py from its TABLE.  DO NOT EDIT: edit the table and rerun\n'
             ' *   python host/engine/gen_gx_rec.py gen\n'
             ' * `gen_gx_rec.py lint` fails on a stale copy (hash + text).  Included by gx_rec.c AFTER its runtime helpers. */\n')
    o.append('#define GXR_N %d\n#define GXR_TABLE_HASH 0x%sU\n' % (len(rows), h))
    for hd in ROW_HEADERS:
        o.append('#include "%s"' % hd)
    o.append('/* real entry points (the linker resolves __real_X to X when X is --wrap\'d) */')
    for r in rows:
        o.append('extern %s __real_%s(%s);' % (r['ret'], r['name'], r['params']))
    o.append('\n/* every row\'s wrapper has EXACTLY the caller-visible type of the function it wraps: checked by the compiler */\n')
    for i, r in enumerate(rows):
        o.append(gen_wrapper(r, i))
    for r in rows:
        o.append('_Static_assert(__builtin_types_compatible_p(__typeof__(__wrap_%s), __typeof__(%s)), "%s: wrapper type differs from the header\'s");'
                 % (r['name'], r['name'], r['name']))
    o.append('\n/* report tables: index = row (sorted by name); op in a record = index + 1 */')
    o.append('static const char *const gxr_name[GXR_N] = {\n    ' + ',\n    '.join('"%s"' % r['name'] for r in rows) + ' };')
    o.append('static const unsigned char gxr_cls[GXR_N] = { ' + ', '.join(str(CLS_ORDER.index(r['cls'])) for r in rows) + ' };   /* A A2 B C D E P */')
    o.append('static const unsigned char gxr_kind[GXR_N] = { ' + ', '.join(str(kind_of(r)) for r in rows) + ' };   /* 1 tev 2 dl 4 prim 8 tex 16 mtx 32 copytex 64 push */')
    o.append('static const unsigned char gxr_push_bytes[GXR_N] = { ' + ', '.join(str(r.get('pipe_bytes', 0)) for r in rows) + ' };')
    o.append('static const unsigned char gxr_recorded[GXR_N] = { ' + ', '.join(str(r['rec']) for r in rows) + ' };')
    return '\n'.join(o) + '\n'


# --- THE THREADED LAYER (RT_THREAD.md 6.2) -------------------------------------------------------------------------------------
# gx_thr_table.txt is the planner's frozen disposition table.  Everything below is derived from it and from TABLE_TEXT; nothing here
# is hand-maintained per row.
GXR_MODE_THREAD = 3
THR_KINDS = 'RSLXPET'
REC_THR = ('R', 'S')      # dispositions whose legacy path is "record by value on G, replay __real_X on R" (S: the same, until R2 takes the setters; R2_STATE.md)
OP_EXTRA_BASE = 200
E_COUNTER = {'os_ext_poll': 'r_ext_poll_dropped', 'vi_set_clear_color': 'r_clear_color_dropped'}   # E extras -> the GxtStats counter R bumps when it drops the call
R_SCRATCH_WORDS_ROW = {'GXStfxSkinBind': 12 * 32}   # R8: the skin palette, nb*48 bytes with nb <= GX_SKIN_MAX_BONES (32) = 1536 bytes; the R scratch of that row is sized for it
ROW_HEADERS = ('port/gx_skin.h',)     # R8: headers the generated .inc files must include so a row's parameter types (GxSkinTable) exist where gxt_G_X / __wrap_X are written
SKIN_PAL_BOUND = {'GXStfxSkinBind': ('nb', 32)}     # inline-length parameter -> its bound: the host drivers clamp the random value to it (GX_SKIN_MAX_BONES); gx_rec_test.c's image buffers are 2048 bytes (32 * 48 + 12 fits)
assert all(SKIN_PAL_BOUND[k][1] * 12 <= R_SCRATCH_WORDS_ROW[k] for k in SKIN_PAL_BOUND)   # the driver's largest payload fits the R scratch
R_SCRATCH_WORDS = 16                                                                              # 64 bytes: the largest inline payload (light object, projection)
G_RESIDENT_X = ('gx_breakpoint_deliver',)    # X extras whose REAL body runs on G (gated, not recorded): their closure is G-side for the G-M1 check

OPS_HEAD = '''/* gx_rt_ops.h -- record op numbers of the GX render thread.  GENERATED once by the planner from gen_gx_rec.py TABLE (rows sorted by name, op = index + 1) and host/engine/gx_thr_table.txt [extra]
 * (sorted by name, op = 200 + index).  Module "gen" (host/engine/gen_gx_rec.py) OWNS it from now on and must reproduce its NON-COMMENT lines identically (its lint fails on a stale copy).
 * Modules "ring" and "special" include it read-only. */
#ifndef GX_RT_OPS_H
#define GX_RT_OPS_H

'''
OPS_TAIL = '''
/* control ops (not entry points).  VBYTES and ABANDON are replayed by the generated gxt_r_exec (they call __real_gxfifo_push_u8 / __real_GXBeginDisplayList); PAD, PRESENT, SYNC, QUIT, MODE_LEAVE, HAMMER by the ring loop in src/gx_thread.c */
#define GXT_OP_VBYTES     1000   /* payload: byte count, then the big-endian vertex bytes (padded to 4) */
#define GXT_OP_PAD        1001   /* fills the ring to its end; nwords covers the filler */
#define GXT_OP_PRESENT    1002   /* payload: seq, arm, rhi, swapchain */
#define GXT_OP_SYNC       1003   /* payload: mark.  R publishes tail through the record and wakes G (blocking B8 copy, drains) */
#define GXT_OP_QUIT       1004   /* R drains to here, acknowledges and exits its loop */
#define GXT_OP_MODE_LEAVE 1005   /* threaded -> direct: R publishes tail through this record and parks */
#define GXT_OP_ABANDON    1006   /* G started a display list: R calls __real_GXBeginDisplayList(NULL, 0) (an unfinished primitive is abandoned, gx_draw.c:2676) */
#define GXT_OP_STATE      1007   /* R2 (R2_STATE.md): the dirty groups of G's HAL instance, by value; handled by the ring loop -> gxs_r_apply (src/gx_rt_state.c) */
#define GXT_OP_HAMMER     1008   /* device ring-hammer payload: seq, checksum (src/gx_thread.c only) */
#define GXT_OP_MAX        1023

#endif
'''
SPECIAL_HEAD = '''/* gx_rt_special.h -- the GX-typed prototypes of the CUSTOM rows (thr = X in host/engine/gx_thr_table.txt).  WRITTEN BY THE PLANNER from that table; module "gen" re-derives it (lint) and must
 * reproduce its non-comment lines identically; module "special" (src/gx_rt_special.c) DEFINES every function here; module "gen" CALLS them from the generated wrappers / the dispatch switch.
 *   gxts_G_<name>(params)      runs on G in threaded mode INSTEAD of the real function: records (gxt_rec_alloc ...) and/or does G-local work; returns what the real function returns
 *   gxts_R_<name>(p, nw)       runs on R: p = payload words of the record gxts_G_<name> wrote (the layout is PRIVATE to special: gen never looks inside); calls __real_<name> or the HAL
 * A custom row's record is opened with gxt_rec_alloc(GXT_OP_<name>, nwords). */
#ifndef GX_RT_SPECIAL_H
#define GX_RT_SPECIAL_H
#include <stdint.h>
#include <stddef.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/gx/GXCpu2Efb.h>
#include <main/pi_flush_api.h>
#include "port/renderer/rhi.h"
#include "gx_rt.h"
#ifdef __cplusplus
extern "C" {
#endif

'''
SPECIAL_TAIL = '''
#ifdef __cplusplus
}
#endif
#endif /* GX_RT_SPECIAL_H */
'''


def read_text(p):
    try:
        return open(p, encoding='utf-8', newline='').read().replace('\r\n', '\n')
    except OSError:
        return None


def parse_thr(text=None):
    """gx_thr_table.txt -> (thr {name: {thr, edge, why}}, extras [row dicts with the same keys as TABLE rows + thr/edge/why/extra])."""
    if text is None:
        text = read_text(THR_TXT)
        if text is None:
            raise SystemExit('cannot read ' + THR_TXT)
    thr, extras, in_extra = {}, [], False
    for ln in text.replace('\r\n', '\n').splitlines():
        t = ln.strip()
        if t.startswith('[extra]'):
            in_extra = True
            continue
        if not t or t.startswith('#'):
            continue
        c = [x.strip() for x in ln.split('|')]
        if in_extra:
            if len(c) != 9:
                raise SystemExit('bad [extra] row (want 9 fields cls|rec|ret|name|params|inline|thr|edge|why): ' + ln)
            cls, rec, ret, name, params, inl, th, edge, why = c
            extras.append(dict(cls=cls, rec=int(rec), ret=ret, name=name, params=params, inline=inl, thr=th, edge=int(edge), why=why, extra=True))
        else:
            if len(c) != 4:
                raise SystemExit('bad thr row (want 4 fields name|thr|edge|why): ' + ln)
            if c[0] in thr:
                raise SystemExit('gx_thr_table.txt lists %s twice' % c[0])
            thr[c[0]] = dict(thr=c[1], edge=int(c[2]), why=c[3])
    return thr, extras


def attach_thr(rows, thr):
    """copy thr/edge/why onto the TABLE rows (in place); a row with no entry keeps thr = None (lint_thr reports it)."""
    for r in rows:
        t = thr.get(r['name'])
        r['thr'], r['edge'], r['why'] = (t['thr'], t['edge'], t['why']) if t else (None, 0, '')
    return rows


def all_entries(rows, extras):
    """table rows + extras, sorted by name: the --wrap set"""
    return sorted(list(rows) + list(extras), key=lambda r: r['name'])


def op_numbers(rows, extras):
    ops = {r['name']: i + 1 for i, r in enumerate(sorted_rows(rows))}
    for i, e in enumerate(sorted(extras, key=lambda r: r['name'])):
        ops[e['name']] = OP_EXTRA_BASE + i
    return ops


# ---- parameter analysis --------------------------------------------------------------------------------------------------------
PARAM_RE = re.compile(r'^(?P<t>.*?)(?P<n>[A-Za-z_]\w*)\s*(?P<a>(?:\[[^\]]*\]\s*)*)$', re.S)


def split_params(params):
    if params.strip() in ('', 'void'):
        return []
    return [p.strip() for p in params.split(',')]


def param_info(params):
    """[(type text, name, array suffix)] in order"""
    out = []
    for p in split_params(params):
        m = PARAM_RE.match(p)
        if not m or not m.group('t').strip():
            raise SystemExit('cannot split the parameter: ' + p)
        out.append((m.group('t').strip(), m.group('n'), m.group('a').strip()))
    return out


def type_only(p):
    """a parameter with its name removed, whitespace normalised (the prelink prototype comparison)"""
    m = PARAM_RE.match(p.strip())
    if not m:
        return re.sub(r'\s+', ' ', p.strip())
    t = (m.group('t') + m.group('a')).strip()
    t = re.sub(r'\s*\*\s*', '*', t)
    t = re.sub(r'\s*\[', '[', t)
    return re.sub(r'\s+', ' ', t)


def proto_norm(ret, params):
    ps = split_params(params)
    r = re.sub(r'\s*\*\s*', '*', ret.strip())
    return re.sub(r'\s+', ' ', r) + '(' + ','.join(type_only(p) for p in ps) + ')'


def inline_of(r):
    """(arg name, byte-count expression) or (None, None)"""
    inl = r.get('inline', '')
    if not inl:
        return None, None
    a, b = inl.split(':', 1)
    return a, b


def inline_sizes(expr):
    """the byte counts an inline expression can take: a literal, or `(x==K?A:B)` -> {A, B}; None = unknown (no exactness check on R)"""
    e = expr.strip()
    if re.fullmatch(r'\d+', e):
        return [int(e)]
    m = re.fullmatch(r'\(\s*\w+\s*==\s*\w+\s*\?\s*(\d+)\s*:\s*(\d+)\s*\)', e)
    if m:
        return sorted({int(m.group(1)), int(m.group(2))})
    return None


def thr_table_hash(rows, extras):
    h = 0x811C9DC5
    txt = canon(rows) + '\n--\n' + '\n'.join('|'.join([r['name'], r.get('thr') or '?', str(r.get('edge', 0))]) for r in sorted_rows(rows)) + '\n--\n' + \
        '\n'.join('|'.join([e['name'], e['ret'], e['params'], e['cls'], str(e['rec']), e['inline'], e['thr'], str(e['edge'])]) for e in sorted(extras, key=lambda r: r['name']))
    for b in txt.encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return '%08x' % h


# ---- lint of the disposition table itself (each rule is watched failing in selftest) ----------------------------------------
def lint_thr(rows, extras):
    f = []
    names = [r['name'] for r in rows]
    ex_names = [e['name'] for e in extras]
    for r in rows:
        t = r.get('thr')
        if t is None:
            f.append('(T) %s has no row in gx_thr_table.txt' % r['name'])
            continue
        if t not in 'RSLXP':
            f.append('(T) %s: thr %r is not one of R S L X P for a table row' % (r['name'], t))
        if (t == 'P') != (r['cls'] == 'P'):
            f.append('(T) %s: thr P is for the 7 gxfifo_push_* rows only (class %s, thr %s)' % (r['name'], r['cls'], t))
        if r.get('edge') not in (0, 1):
            f.append('(T) %s: edge must be 0 or 1' % r['name'])
        if t in REC_THR and r['ret'].strip() != 'void':
            f.append('(T) %s: an R / S row must return void (R cannot answer G); make it X' % r['name'])
        if t == 'S' and r['cls'] != 'A':
            f.append('(T) %s: an S row is a pure state write: class A, not %s' % (r['name'], r['cls']))
        if t in REC_THR:
            ia, ib = inline_of(r)
            if ia and ia not in [a for _, a, _ in param_info(r['params'])]:
                f.append('(T) %s: inline argument %s is not a parameter' % (r['name'], ia))
    for n in sorted(set(names)):
        if names.count(n) > 1:
            f.append('(T) table lists %s more than once' % n)
    for n in sorted({x for x in ex_names if ex_names.count(x) > 1}):
        f.append('(T) [extra] lists %s more than once' % n)
    for n in sorted(set(ex_names) & set(names)):
        f.append('(T) [extra] %s is also a table row' % n)
    for e in extras:
        if e['thr'] not in 'RXET':
            f.append('(T) [extra] %s: thr %r is not one of R X E T' % (e['name'], e['thr']))
        if e['thr'] == 'R' and e['ret'].strip() != 'void':
            f.append('(T) [extra] %s: an R row must return void' % e['name'])
        if e['thr'] == 'E' and e['name'] not in E_COUNTER:
            f.append('(T) [extra] %s: an E row needs a GxtStats drop counter (E_COUNTER in the generator)' % e['name'])
        if e['thr'] == 'E' and e['ret'].strip() != 'void':
            f.append('(T) [extra] %s: an E row must return void' % e['name'])
        try:
            param_info(e['params'])
        except SystemExit as x:
            f.append('(T) [extra] %s: %s' % (e['name'], x))
    if len(extras) != 19:
        f.append('(T) %d [extra] rows, the contract has 19' % len(extras))
    return f


# ---- the threaded part of each __wrap_X ---------------------------------------------------------------------------------------
def pipe_push_expr(r):
    n, b = r['name'], r['pipe_bytes']
    if n.endswith('f32'):
        return 'gxt_push_f32(v);'
    if b == 1:
        return 'gxt_push_u8((uint32_t)(uint8_t) v);'
    if b == 2:
        return 'gxt_push_u16((uint32_t)(uint16_t) v);'
    return 'gxt_push_u32((uint32_t) v);'


def thr_action(r):
    """the text inside `if (g_rec_mode == GXR_MODE_THREAD) { ... }` of a table row's wrapper"""
    name, ret = r['name'], r['ret'].strip()
    args = ', '.join(argnames(r['params']))
    real = '__real_%s(%s)' % (name, args)
    t = r.get('thr')
    s = ''
    if r.get('edge'):
        s += 'if (gxt_on_R()) { ++g_gxt.st.r_edge_calls; return %s; } ' % real
    if t == 'R':
        s += 'gxt_G_%s(%s); return;' % (name, args)
    elif t == 'S':
        # R2 (R2_STATE.md 4.3): R2 active -> count the call (the "[gxt] state:" line's s_calls/f: one increment, the only per-call extra besides the body), close an open VBYTES chunk (INV-2: the HAL decodes a primitive when its byte count completes, so a setter splits the chunk exactly where a record would),
        # run the HAL body on G's instance (it marks the dirty groups) and return -- nothing is recorded.  R2 off -> the legacy record (+ the filter).
        s += 'if (GXT_LIKELY(gxt_state_on)) { ++gxt_state_calls; if (GXT_UNLIKELY(g_gxt.chunk_data != NULL)) gxt_chunk_close(); gxh_g_%s(%s); return; } gxt_G_%s(%s); return;' % (name, args, name, args)
    elif t == 'L':
        s += 'return %s;' % real
    elif t == 'X':
        s += 'return gxts_G_%s(%s);' % (name, args)
    elif t == 'P':
        s += pipe_push_expr(r) + ' return;'
    else:
        raise SystemExit('%s: no threaded disposition (thr=%r)' % (name, t))
    return s


# ---- gx_rt_gen.inc ---------------------------------------------------------------------------------------------------------------
def gen_g_rec(r):
    """static noinline gxt_G_X: the G side of an R row -- one gxt_rec_alloc, the stores and the filter's commit (D14; the commit is one branch when the filter is off)"""
    name = r['name']
    pi = param_info(r['params'])
    ia, ib = inline_of(r)
    byval = [n for _, n, _ in pi if n != ia]
    nw = ' + '.join(['GXT_W(%s)' % a for a in byval] + (['GXT_WORDS(%s)' % ib] if ia else [])) or '0'
    s = 'static __attribute__((noinline)) void gxt_G_%s(%s)\n{\n' % (name, r['params'])
    # The G-side redundant-state filter (gx_rt_filter.c, v2 "compare before write"): while it is armed and the module classifies this op FILTER (GXT_FLT_WANTS: one load + one
    # branch otherwise), the payload is built on the STACK and checked by gxt_flt_check (gx_rt.h, inline; the module only on rare paths) BEFORE anything is allocated; a redundant call returns having touched nothing.  The module owns
    # the classification; the generator offers every R row.  X rows (gen_custom_standins, gx_rt_special.c) are deliberately never offered: they are never filtered (GXSetArray's G side
    # swaps in shadow copies whose content changes behind an identical pointer); they still reach the filter's FORGET/NEUTRAL hook inside gxt_rec_alloc.
    if not byval and not ia:
        s += '    if (GXT_FLT_WANTS(GXT_OP_%s) && gxt_flt_check(GXT_OP_%s, NULL, 0)) return;\n' % (name, name)
        s += '    (void) gxt_rec_alloc(GXT_OP_%s, 0);\n}\n' % name
        return s
    s += '    const uint32_t nw_ = %s;\n' % nw
    s += '    uint32_t *q_;\n'
    s += '    if (GXT_FLT_WANTS(GXT_OP_%s) && nw_ <= GXT_FLT_PRE_MAXW) {\n' % name
    s += '        uint32_t w_[GXT_FLT_PRE_MAXW], *p_ = w_;\n'
    for a in byval:
        s += '        GXT_PUT(p_, %s);\n' % a
    if ia:
        s += '        memcpy(p_, %s, (size_t)(%s));\n' % (ia, ib)
        s += '        if (((%s) & 3u) != 0u) memset((uint8_t *) p_ + (%s), 0, 4u - ((%s) & 3u));\n' % (ib, ib, ib)
    s += '        if (gxt_flt_check(GXT_OP_%s, w_, nw_)) return;\n' % name
    s += '        q_ = gxt_rec_alloc_flt(GXT_OP_%s, nw_);\n' % name
    s += '        memcpy(q_, w_, (size_t) nw_ * 4u);\n'
    s += '        return;\n'
    s += '    }\n'
    s += '    q_ = gxt_rec_alloc(GXT_OP_%s, nw_);\n' % name
    for a in byval:
        s += '    GXT_PUT(q_, %s);\n' % a
    if ia:
        s += '    memcpy(q_, %s, (size_t)(%s));\n' % (ia, ib)
    s += '}\n'
    return s


def gen_r_case(r):
    """one `case GXT_OP_X:` of gxt_r_exec for an R row or an R extra: decode by value, call the real body"""
    name = r['name']
    pi = param_info(r['params'])
    ia, ib = inline_of(r)
    byval = [(t, n) for t, n, _ in pi if n != ia]
    byw = ' + '.join('GXT_W(%s)' % n for _, n in byval) or '0'
    s = '    case GXT_OP_%s: {\n' % name
    for t, n in byval:
        s += '        %s %s;\n' % (t, n)
    if ia:
        scw = R_SCRATCH_WORDS_ROW.get(name, R_SCRATCH_WORDS)
        s += '        uint32_t sc_[%d] __attribute__((aligned(32)));\n' % scw
        sizes = inline_sizes(ib)
        if sizes:
            cond = ' && '.join('nw_ != (%s) + %du' % (byw, z // 4) for z in sizes)
            s += '        if (%s) return -1;\n' % cond
        else:
            s += '        if (nw_ < (%s) || nw_ - (%s) > %du) return -1;\n' % (byw, byw, scw)
    else:
        s += '        if (nw_ != %s) return -1;\n' % byw
    if byval or ia:
        s += '        const uint32_t *q_ = p_;\n'
    for t, n in byval:
        s += '        GXT_GET(q_, %s);\n' % n
    if ia and inline_sizes(ib) is None:      # a length that is a by-value parameter (R8: nb * 48): exact once the parameters are decoded, as a literal length is exact above
        s += '        if (nw_ - (%s) != GXT_WORDS(%s)) return -1;\n' % (byw, ib)
    if ia:
        s += '        memcpy(sc_, q_, (size_t)(nw_ - (%s)) * 4u);\n' % byw   # the payload length is the RECORD's, never recomputed on R
    call = ', '.join('(void *) sc_' if n == ia else n for _, n, _ in pi)
    s += '        __real_%s(%s);\n        return 0;\n    }\n' % (name, call)
    return s


def gen_extra_wrap(e):
    """__wrap_ of an [extra] row: acts ONLY in threaded mode; every other mode is `return __real_X(args)`"""
    name, ret, params, th = e['name'], e['ret'].strip(), e['params'], e['thr']
    args = ', '.join(argnames(params))
    real = '__real_%s(%s)' % (name, args)
    s = '%s __wrap_%s(%s)\n{\n' % (ret, name, params)
    s += '    if (__builtin_expect(g_rec_mode != GXR_MODE_THREAD, 1)) return %s;\n' % real
    if th == 'R':
        s += '    gxt_G_%s(%s);\n' % (name, args)
    elif th == 'X':
        s += '    return gxts_G_%s(%s);\n' % (name, args)
    elif th == 'E':
        s += '    if (gxt_on_R()) { ++g_gxt.st.%s; return; }   /* R drops it: G ran (or runs) the real one itself */\n    return %s;\n' % (E_COUNTER[name], real)
    elif th == 'T':
        s += '    gxt_viewer_trap("%s");\n    return %s;\n' % (name, real)
    else:
        raise SystemExit('%s: no threaded disposition for an extra (thr=%r)' % (name, th))
    s += '}\n'
    return s


GEN_PRELUDE = '''
/* ---- encode / decode helpers: ONE layout for both sides.  An argument takes ceil(sizeof/4) words (a pointer takes sizeof(void *)/4: 1 on the Vita,
 *      2 on the 64-bit host); a sub-word argument is zero-padded; an inline payload follows the by-value arguments and fills the rest of the record. */
#define GXT_W(x)        ((uint32_t) ((sizeof(x) + 3u) >> 2))
#define GXT_WORDS(nb)   (((uint32_t) (nb) + 3u) >> 2)
#define GXT_PUT(q, x)   do { __typeof__(x) v_ = (x); if (sizeof v_ & 3u) (q)[GXT_W(v_) - 1u] = 0u; memcpy((q), &v_, sizeof v_); (q) += GXT_W(v_); } while (0)
#define GXT_GET(q, x)   do { memcpy(&(x), (q), sizeof(x)); (q) += GXT_W(x); } while (0)
'''


def gen_rt_gen(rows, extras):
    rows = sorted_rows(rows)
    ex = sorted(extras, key=lambda r: r['name'])
    ops = op_numbers(rows, extras)
    o = []
    o.append('/* gx_rt_gen.inc -- GENERATED by host/engine/gen_gx_rec.py from its TABLE and host/engine/gx_thr_table.txt.  DO NOT EDIT: edit the table and rerun\n'
             ' *   python host/engine/gen_gx_rec.py gen\n'
             ' * `gen_gx_rec.py lint|prelink` fails on a stale copy (hash + text).  Included by gx_rec.c BEFORE gx_rec_wraps.inc.\n'
             ' * Holds: gxt_G_X (the G side of every R row), the %d [extra] wrappers, and gxt_r_exec (the R dispatch). */\n' % len(ex))
    o.append('#define GXT_GEN_HASH 0x%sU\n' % thr_table_hash(rows, extras))
    for hd in ROW_HEADERS:
        o.append('#include "%s"' % hd)
    o.append(GEN_PRELUDE)
    o.append('/* the op numbers of gx_rt_ops.h are the generator\'s: a stale header fails to compile HERE */')
    for n, v in sorted(ops.items(), key=lambda kv: kv[1]):
        o.append('_Static_assert(GXT_OP_%s == %d, "gx_rt_ops.h is stale: regenerate (gen_gx_rec.py gen)");' % (n, v))
    o.append('_Static_assert(GXT_OP_TABLE_N == %d && GXT_OP_EXTRA_N == %d && GXT_OP_EXTRA_BASE == %d, "gx_rt_ops.h counts are stale");' % (len(rows), len(ex), OP_EXTRA_BASE))
    o.append('\n/* real entry points the R dispatch and the extras call */')
    for r in rows:
        if r['thr'] in ('R', 'S', 'P') or r['name'] in ('GXBeginDisplayList',):
            o.append('extern %s __real_%s(%s);' % (r['ret'], r['name'], r['params']))
    for e in ex:
        o.append('extern %s __real_%s(%s);' % (e['ret'], e['name'], e['params']))
    o.append('\n/* R-VB: the bulk VBYTES replay (hal/gx_draw.c): gx_vb_bulk picks it (default 0 = the byte-by-byte replay); not a --wrap row */')
    o.append('extern int gx_vb_bulk;\nextern void gxfifo_push_bytes(const unsigned char *b, unsigned n);')
    o.append('\n/* the G variants of the S rows (R2): the HAL body on G\'s instance, defined in hal/gx_tev.c, gx_draw.c, gx_light.c */')
    for r in rows:
        if r['thr'] == 'S':
            o.append('extern %s gxh_g_%s(%s);' % (r['ret'], r['name'], r['params']))
    o.append('\n/* ---- G side of every R / S row (S: the legacy path, while R2 is off): record by value ---- */\n')
    for r in rows:
        if r['thr'] in REC_THR:
            o.append(gen_g_rec(r))
    for e in ex:
        if e['thr'] == 'R':
            o.append(gen_g_rec(e))
    o.append('/* ---- the [extra] wrappers: threaded mode only, every other mode is `return __real_X(args)` ---- */\n')
    for e in ex:
        o.append(gen_extra_wrap(e))
    o.append('/* ---- R side: decode a record by value and call the real body ---- */')
    o.append('int gxt_r_exec(uint32_t op_, const uint32_t *p_, uint32_t nw_)\n{\n'
             '    if (GXT_UNLIKELY(g_gxt.cfg.brk) && gxts_r_filter(op_)) return 0;   /* harness break arms (dropdraw) */\n'
             '    switch (op_) {')
    for r in rows:
        if r['thr'] in REC_THR:
            o.append(gen_r_case(r))
        elif r['thr'] == 'X':
            o.append('    case GXT_OP_%s: gxts_R_%s(p_, nw_); return 0;' % (r['name'], r['name']))
    for e in ex:
        if e['thr'] == 'R':
            o.append(gen_r_case(e))
        elif e['thr'] == 'X':
            o.append('    case GXT_OP_%s: gxts_R_%s(p_, nw_); return 0;' % (e['name'], e['name']))
    o.append('''    case GXT_OP_VBYTES: {   /* the vertex bytes of an immediate primitive, replayed BYTE BY BYTE: fifoByte is static in gx_draw.c and byte-exact replay keeps "bytes with no open primitive are dropped" */
        if (nw_ < 1u || p_[0] > (nw_ - 1u) * 4u) return -1;
        const uint8_t *b_ = (const uint8_t *) (p_ + 1);
        if (gx_vb_bulk) { gxfifo_push_bytes(b_, p_[0]); return 0; }   /* R-VB: the same bytes in one call (gx_draw.c), byte-exact */
        for (uint32_t i_ = 0; i_ < p_[0]; ++i_) __real_gxfifo_push_u8(b_[i_]);
        return 0;
    }
    case GXT_OP_ABANDON:    /* G started a display list: an unfinished primitive is abandoned (gx_draw.c GXBeginDisplayList) */
        if (nw_ != 0u) return -1;
        __real_GXBeginDisplayList(NULL, 0);
        return 0;
    default:
        return -1;
    }
}''')
    return '\n'.join(o) + '\n'


# ---- gx_rt_ops.h / gx_rt_special.h ----------------------------------------------------------------------------------------------
def gen_ops_h(rows, extras):
    rows = sorted_rows(rows)
    ex = sorted(extras, key=lambda r: r['name'])
    ops = op_numbers(rows, extras)
    o = [OPS_HEAD.rstrip('\n'), '', '#define GXT_OP_TABLE_N %d' % len(rows), '#define GXT_OP_EXTRA_BASE %d' % OP_EXTRA_BASE, '#define GXT_OP_EXTRA_N %d' % len(ex)]
    for r in rows:
        o.append('#define %-33s %d' % ('GXT_OP_' + r['name'], ops[r['name']]))
    for e in ex:
        o.append('#define %-33s %d' % ('GXT_OP_' + e['name'], ops[e['name']]))
    return '\n'.join(o) + '\n' + OPS_TAIL


def gen_special_h(rows, extras):
    rows = sorted_rows(rows)
    ex = sorted(extras, key=lambda r: r['name'])
    o = [SPECIAL_HEAD.rstrip('\n'), '']
    for r in rows:
        if r['thr'] == 'X':
            o.append('%s gxts_G_%s(%s);' % (r['ret'].strip(), r['name'], r['params']))
            o.append('void gxts_R_%s(const uint32_t *p, uint32_t nw);' % r['name'])
    o.append('')
    o.append('/* [extra] custom rows */')
    for e in ex:
        if e['thr'] == 'X':
            o.append('%s gxts_G_%s(%s);' % (e['ret'].strip(), e['name'], e['params']))
            o.append('void gxts_R_%s(const uint32_t *p, uint32_t nw);' % e['name'])
    return '\n'.join(o) + '\n' + SPECIAL_TAIL


def strip_comments(text):
    """C comments removed, trailing blanks trimmed, blank lines dropped: what `non-comment lines equal the planner's` compares"""
    t = text.replace('\r\n', '\n')
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return '\n'.join(l.rstrip() for l in t.split('\n') if l.strip()) + '\n'


# ---- host test generators ---------------------------------------------------------------------------------------------------------
def gen_stubs(rows, extras=()):
    """Host test only (gx_rec_test).  Index space: table row i (sorted by name) = i; extra j (sorted by name) = GXR_N + j.
      __real_X      counts its calls, returns zero, and (R rows) leaves a byte image of exactly what it was handed -- by-value bytes in argument
                    order, then the payload an inline pointer pointed at -- in g_img[idx]
      gxts_G_X / gxts_R_X   (X rows) count their calls and remember (p, nw) of the R call
      gxd_thr / gxd_edge / gxd_extra   the disposition, for the test's per-row expectations"""
    rs = sorted_rows(rows)
    ex = sorted(extras, key=lambda r: r['name'])
    o = ['/* GENERATED by gen_gx_rec.py stubs -- host test stand-ins for the real GX/VI bodies and the custom module. */',
         '#define GXD_NX %d' % len(ex),
         'static unsigned g_real_calls[GXR_N + GXD_NX];',
         'static unsigned char g_img[GXR_N + GXD_NX][2048]; static unsigned g_img_len[GXR_N + GXD_NX];',
         'static unsigned g_gxts_g[GXR_N + GXD_NX], g_gxts_r[GXR_N + GXD_NX]; static const uint32_t *g_gxts_r_p[GXR_N + GXD_NX]; static uint32_t g_gxts_r_nw[GXR_N + GXD_NX];',
         'static unsigned char g_pipe_u8[4096]; static unsigned g_pipe_n; static void *g_bdl_list; static unsigned g_bdl_size;',
         'int gx_vb_bulk; static unsigned g_bulk_calls; void gxfifo_push_bytes(const unsigned char *b, unsigned n) { ++g_bulk_calls; for (unsigned i = 0; i < n; ++i) if (g_pipe_n < sizeof g_pipe_u8) g_pipe_u8[g_pipe_n++] = b[i]; }',
         '#define IMG_ADD(row, x) do { memcpy(g_img[row] + g_img_len[row], &(x), sizeof(x)); g_img_len[row] += (unsigned) sizeof(x); } while (0)',
         '#define IMG_PAY(row, ptr, nb) do { memcpy(g_img[row] + g_img_len[row], (ptr), (nb)); g_img_len[row] += (unsigned) (nb); } while (0)']
    for i, r in enumerate(rs):
        o.append(_stub_one(r, i))
    for j, e in enumerate(ex):
        o.append(_stub_one(e, len(rs) + j))
    o.append('static unsigned g_hal_calls[GXR_N + GXD_NX];\nstatic unsigned char g_himg[GXR_N + GXD_NX][2048]; static unsigned g_himg_len[GXR_N + GXD_NX];   /* R2: the G variants (gxh_g_X) of the S rows */')
    for i, r in enumerate(rs):
        if r['thr'] == 'S':
            ia, ib = inline_of(r)
            body = '++g_hal_calls[%d]; g_himg_len[%d] = 0; ' % (i, i)
            for _, n, _ in param_info(r['params']):
                if n != ia:
                    body += 'memcpy(g_himg[%d] + g_himg_len[%d], &%s, sizeof %s); g_himg_len[%d] += (unsigned) sizeof %s; ' % (i, i, n, n, i, n)
            if ia:
                body += 'memcpy(g_himg[%d] + g_himg_len[%d], %s, (size_t)(%s)); g_himg_len[%d] += (unsigned) (%s); ' % (i, i, ia, ib, i, ib)
            o.append('void gxh_g_%s(%s) { %s}' % (r['name'], r['params'], body))
    for i, r in enumerate(rs + ex):
        if r['thr'] == 'X':
            o.append('%s gxts_G_%s(%s) { ++g_gxts_g[%d]; %s}' % (r['ret'], r['name'], r['params'], i,
                                                                  ('return (%s) 0; ' % r['ret']) if r['ret'].strip() != 'void' else ''))
            o.append('void gxts_R_%s(const uint32_t *p, uint32_t nw) { ++g_gxts_r[%d]; g_gxts_r_p[%d] = p; g_gxts_r_nw[%d] = nw; }' % (r['name'], i, i, i))
    o.append('static const char gxd_thr[GXR_N] = { ' + ', '.join("'%s'" % ('R' if r['thr'] == 'S' else r['thr']) for r in rs) + ' };   /* an S row is an R row while R2 is off (the default) */')
    o.append('static const unsigned char gxd_s[GXR_N] = { ' + ', '.join('1' if r['thr'] == 'S' else '0' for r in rs) + ' };   /* 1 = an S row: with gxt_state_on the wrapper calls gxh_g_X instead */')
    o.append('static const unsigned char gxd_edge[GXR_N] = { ' + ', '.join(str(r['edge']) for r in rs) + ' };')
    o.append('static const struct { const char *name; char thr; unsigned op; } gxd_extra[GXD_NX] = {\n    ' +
             ',\n    '.join('{ "%s", \'%s\', GXT_OP_%s }' % (e['name'], e['thr'], e['name']) for e in ex) + ' };')
    return '\n'.join(o) + '\n'


def _stub_one(r, idx):
    ia, ib = inline_of(r)
    pi = param_info(r['params'])
    body = '++g_real_calls[%d]; ' % idx
    if r['thr'] in REC_THR:
        body += 'g_img_len[%d] = 0; ' % idx
        for _, n, _ in pi:
            if n != ia:
                body += 'IMG_ADD(%d, %s); ' % (idx, n)
        if ia:
            body += 'IMG_PAY(%d, %s, (size_t)(%s)); ' % (idx, ia, ib)
    if r['name'] == 'gxfifo_push_u8':
        body += 'if (g_pipe_n < sizeof g_pipe_u8) g_pipe_u8[g_pipe_n++] = v; '          # VBYTES replay order
    if r['name'] == 'GXBeginDisplayList':
        body += 'g_bdl_list = list; g_bdl_size = size; '                                # ABANDON replay arguments
    if r['ret'].strip() != 'void':
        body += 'return (%s) 0; ' % r['ret']
    return '%s __real_%s(%s) { %s}' % (r['ret'], r['name'], r['params'], body)


def gen_drivers(rows, extras):
    """Host test only: gxd_<row>(variant, img, &nw) calls __wrap_<row> with random arguments of the row's declared types.  For an R row it returns
    the byte image the stub must see and sets nw to the payload words the record must have; for a P row the big-endian bytes it must push.
    Random bytes for integers / structs / pointers, random finite values for floats; an inline length of the form (x==K?A:B) is driven both ways
    by `variant` (that is how GXLoadTexMtxImm 2x4 vs 3x4 is exercised)."""
    all_ = sorted_rows(rows) + sorted(extras, key=lambda r: r['name'])
    o = ['/* GENERATED by gen_gx_rec.py drivers */',
         'static unsigned long long g_rng = 0x9E3779B97F4A7C15ull;',
         'static unsigned gxd_rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (unsigned) (g_rng >> 33); }',
         'static void gxd_fill_bytes(void *p, size_t n) { unsigned char *c = (unsigned char *) p; for (size_t i = 0; i < n; ++i) c[i] = (unsigned char) gxd_rnd(); }',
         'static void gxd_fill_f32(void *p, size_t n) { (void) n; const float f = (float) ((int) (gxd_rnd() % 200001u) - 100000) / 64.0f; memcpy(p, &f, 4); }',
         '#define GXD_FILL(x) _Generic((x), float: gxd_fill_f32, default: gxd_fill_bytes)(&(x), sizeof(x))']
    for r in all_:
        pi = param_info(r['params'])
        ia, ib = inline_of(r)
        sizes = inline_sizes(ib) if ia else None
        s = 'static unsigned gxd_%s(int variant, unsigned char *img, unsigned *nw)\n{\n    (void) variant; (void) img; unsigned n_ = 0; (void) n_; *nw = 0;\n' % r['name']
        for t, n, a in pi:
            if n == ia:
                s += '    unsigned char buf_%s[%d] __attribute__((aligned(32))); gxd_fill_bytes(buf_%s, sizeof buf_%s);\n' % (n, 4 * R_SCRATCH_WORDS_ROW.get(r['name'], R_SCRATCH_WORDS), n, n)
            elif a:
                s += '    void *%s; gxd_fill_bytes(&%s, sizeof %s);\n' % (n, n, n)       # an array parameter that is not an inline payload: an opaque pointer, never dereferenced
            else:
                s += '    %s %s; GXD_FILL(%s);\n' % (t, n, n)
        if r['name'] in SKIN_PAL_BOUND:      # the inline length is a parameter (nb * 48): clamp it to the bound the R scratch is sized for (odd variants: exactly the bound)
            bn, bb = SKIN_PAL_BOUND[r['name']]
            s += '    %s = (__typeof__(%s)) (variant & 1 ? %du : gxd_rnd() %% %du);\n' % (bn, bn, bb, bb + 1)
        if ia and sizes and len(sizes) == 2:
            m = re.fullmatch(r'\(\s*(\w+)\s*==\s*(\w+)\s*\?\s*(\d+)\s*:\s*(\d+)\s*\)', ib.strip())
            s += '    if (variant & 1) %s = %s; else %s = (__typeof__(%s)) 0x7;\n' % (m.group(1), m.group(2), m.group(1), m.group(1))
        if r['thr'] in REC_THR:
            byval = [n for _, n, _ in pi if n != ia]
            for n in byval:
                s += '    memcpy(img + n_, &%s, sizeof %s); n_ += (unsigned) sizeof %s; *nw += (unsigned) ((sizeof %s + 3u) >> 2);\n' % (n, n, n, n)
            if ia:
                s += '    { const unsigned nb_ = (unsigned) (%s); memcpy(img + n_, buf_%s, nb_); n_ += nb_; *nw += (nb_ + 3u) >> 2; }\n' % (ib, ia)
        if r['thr'] == 'P':
            b = r['pipe_bytes']
            s += '    uint32_t u_ = 0; memcpy(&u_, &v, sizeof v); u_ &= %s;\n' % ('0xFFu' if b == 1 else '0xFFFFu' if b == 2 else '0xFFFFFFFFu')
            for k in range(b):
                s += '    img[%d] = (unsigned char) (u_ >> %d);\n' % (k, 8 * (b - 1 - k))
            s += '    n_ = %d;\n' % b
        args = ', '.join(('(void *) buf_%s' % n) if n == ia else n for _, n, _ in pi)
        s += '    __wrap_%s(%s);\n    return n_;\n}\n' % (r['name'], args)
        o.append(s)
    o.append('typedef unsigned (*gxd_fn)(int, unsigned char *, unsigned *);')
    o.append('static const gxd_fn gxd_all[] = {   /* row i (sorted by name) = i, extra j = GXR_N + j */')
    for r in all_:
        o.append('    gxd_%s,' % r['name'])
    o.append('};')
    return '\n'.join(o) + '\n'


STANDIN_BLOCKING = ('GXEndDisplayList', 'GXGetScissor', 'GXPeekZ', 'GXReadDrawSync', 'gx_breakpoint_deliver')   # G needs the answer: record, drain, return R's
STANDIN_ON_G = ('GXInitTexObjLOD',)       # drain R, then run the real body on G (it writes the game's object AND R's gTexLod; not recordable by value)


def gen_custom_standins(rows, extras):
    """Host replay test only (gx_rt_replay_test): the simplest correct gxts_G_X / gxts_R_X for every X entry -- NOT the lifetime-aware special module.
    An X entry is recorded BY VALUE exactly like an R row (the same GXT_PUT/GXT_GET layout) and replayed through __real_X; entries whose
    answer G needs are BLOCKING (record, drain, return what R stored); GXInitTexObjLOD drains and runs on G.  What these stand-ins do NOT do
    (the written set, shadow arrays, dirty-image snapshots, the single-stepped draw sync, the G-local display list) is exactly what the test
    cannot check and says so."""
    o = ['/* GENERATED by gen_gx_rec.py standins -- plain record-by-value stand-ins for the custom rows (host replay test only) */',
         '#include <stdint.h>', '#include <string.h>', '#include "gx_rt.h"', '#include "gx_rt_special.h"', GEN_PRELUDE,
         'static volatile unsigned long long g_so_ret;   /* the answer R leaves for a blocking stand-in (G is parked in gxt_drain while R writes it) */']
    xs = [r for r in sorted_rows(rows) + sorted(extras, key=lambda r: r['name']) if r['thr'] == 'X']
    for r in xs:
        o.append('extern %s __real_%s(%s);' % (r['ret'], r['name'], r['params']))
    for r in xs:
        name, ret = r['name'], r['ret'].strip()
        pi = param_info(r['params'])
        ia, ib = inline_of(r)
        # GXLoadTexObj / PreLoaded and GXInitTexObjLOD carry the 32-byte object by value (the table's inline spec)
        byval = [n for _, n, _ in pi if n != ia]
        nw = ' + '.join(['GXT_W(%s)' % a for a in byval] + (['GXT_WORDS(%s)' % ib] if ia else [])) or '0'
        if name in STANDIN_ON_G:
            o.append('%s gxts_G_%s(%s)\n{\n    gxt_drain("standin %s");\n    %s__real_%s(%s);\n}' % (
                ret, name, r['params'], name, '' if ret == 'void' else 'return ', name, ', '.join(n for _, n, _ in pi)))
            o.append('void gxts_R_%s(const uint32_t *p, uint32_t nw) { (void) p; (void) nw; }' % name)
            continue
        g = '%s gxts_G_%s(%s)\n{\n' % (ret, name, r['params'])
        if byval or ia:
            g += '    uint32_t *q_ = gxt_rec_alloc(GXT_OP_%s, %s);\n' % (name, nw)
            for a in byval:
                g += '    GXT_PUT(q_, %s);\n' % a
            if ia:
                g += '    memcpy(q_, %s, (size_t)(%s));\n' % (ia, ib)
        else:
            g += '    (void) gxt_rec_alloc(GXT_OP_%s, 0);\n' % name
        if name in STANDIN_BLOCKING:
            g += '    gxt_drain("standin %s");\n' % name
            if ret != 'void':
                g += '    return (%s) g_so_ret;\n' % ret
        g += '}'
        o.append(g)
        # R side: decode and call the real body
        rr = 'void gxts_R_%s(const uint32_t *p_, uint32_t nw_)\n{\n' % name
        byw = ' + '.join('GXT_W(%s)' % n for n in byval) or '0'
        for t, n, _ in pi:
            if n != ia:
                rr += '    %s %s;\n' % (t, n)
        if ia:
            rr += '    uint32_t sc_[%d] __attribute__((aligned(32)));\n' % R_SCRATCH_WORDS_ROW.get(name, R_SCRATCH_WORDS)
        rr += '    const uint32_t *q_ = p_;\n    (void) nw_; (void) q_;\n'
        for t, n, _ in pi:
            if n != ia:
                rr += '    GXT_GET(q_, %s);\n' % n
        if ia:
            rr += '    memcpy(sc_, q_, (size_t)(nw_ - (%s)) * 4u);\n' % byw
        call = ', '.join('(void *) sc_' if n == ia else n for _, n, _ in pi)
        rr += '    %s__real_%s(%s);\n}' % ('g_so_ret = (unsigned long long) ' if ret != 'void' else '', name, call)
        o.append(rr)
    return '\n'.join(o) + '\n'


def gen_stand_ins(rows, extras, defined):
    """Host replay test only: strong stand-ins for the boundary symbols the host HAL objects do not define (the host links no VI / scaffold /
    os_threads), so every __real_X of the --wrap set resolves.  `defined` = the symbol names the host objects DO define."""
    o = ['/* GENERATED by gen_gx_rec.py hoststubs -- stand-ins for symbols the host HAL objects do not define (%d of %d) */' %
         (sum(1 for r in list(rows) + list(extras) if r['name'] not in defined), len(rows) + len(extras)),
         '#include <stdint.h>', '#include <stddef.h>', '#include <dolphin/gx.h>', '#include <dolphin/vi.h>', '#include <dolphin/gx/GXCpu2Efb.h>',
         '#include <dolphin/gx/GXDraw.h>', '#include <main/pi_flush_api.h>', '#include "port/renderer/rhi.h"']
    o += ['#include "%s"' % hd for hd in ROW_HEADERS]   # R8: a row's parameter types (GxSkinTable) must exist in the stand-ins too
    for r in all_entries(rows, extras):
        if r['name'] in defined:
            continue
        body = '{ '
        if r['ret'].strip() != 'void':
            body += 'return (%s) 0; ' % r['ret']
        o.append('%s %s(%s) %s}' % (r['ret'], r['name'], r['params'], body))
    return '\n'.join(o) + '\n'


# ---- host state-sweep generator (gx_rt_state_test): one random-argument driver per S row --------------------------------------------------
# The sweep is ENUMERATED FROM THE TABLE: a new S row gets a driver here with no edit, so the sweep covers it automatically.  Arguments are drawn from SMALL domains per parameter TYPE
# (the point is collisions: two spellings of one target, ids a little past the valid range), with per-(row, parameter) overrides where the id space is wider.  The test (gx_rt_state_test.c)
# defines the SW_* macros (SW_MTXID: ids a small-slot-biased draw over 0..200); every integer parameter goes through SW_P(row, parameter index, value) so the test can histogram what it really drew (an alias class that was never drawn is a
# failure there, not a pass).
SWEEP_TYPE_DOM = {
    'GXTevStageID': 'SW_I(0, 19)', 'GXTevRegID': 'SW_I(0, 5)', 'GXTevKColorID': 'SW_I(0, 5)', 'GXTevSwapSel': 'SW_I(0, 5)', 'GXIndTexStageID': 'SW_I(0, 5)',
    'GXTexCoordID': 'SW_I(0, 9)', 'GXTexMapID': 'SW_MAP()', 'GXChannelID': 'SW_I(0, 6)', 'GXAttr': 'SW_I(0, 27)', 'GXVtxFmt': 'SW_I(0, 9)', 'GXIndTexMtxID': 'SW_I(0, 12)',
    'GXTevColorArg': 'SW_I(0, 15)', 'GXTevAlphaArg': 'SW_I(0, 7)', 'GXTevOp': 'SW_I(0, 15)', 'GXTevBias': 'SW_I(0, 3)', 'GXTevScale': 'SW_I(0, 3)', 'GXBool': 'SW_I(0, 1)',
    'GXColorSrc': 'SW_I(0, 1)', 'GXDiffuseFn': 'SW_I(0, 2)', 'GXAttnFn': 'SW_I(0, 2)', 'GXAttrType': 'SW_I(0, 3)', 'GXLightID': 'SW_LIGHTID()', 'GXTexMtxType': 'SW_I(0, 1)',
    'GXProjectionType': 'SW_I(0, 1)', 'GXCullMode': 'SW_I(0, 3)', 'GXCompare': 'SW_I(0, 7)', 'GXAlphaOp': 'SW_I(0, 3)', 'GXBlendMode': 'SW_I(0, 3)', 'GXBlendFactor': 'SW_I(0, 8)',
    'GXLogicOp': 'SW_I(0, 15)', 'GXTevMode': 'SW_I(0, 5)', 'GXTevKColorSel': 'SW_I(0, 31)', 'GXTevKAlphaSel': 'SW_I(0, 31)', 'GXTexGenType': 'SW_I(0, 11)', 'GXTexGenSrc': 'SW_I(0, 20)',
    'GXTevColorChan': 'SW_I(0, 3)', 'GXIndTexFormat': 'SW_I(0, 3)', 'GXIndTexBiasSel': 'SW_I(0, 7)', 'GXIndTexWrap': 'SW_I(0, 6)', 'GXIndTexAlphaSel': 'SW_I(0, 4)',
    'GXIndTexScale': 'SW_I(0, 4)', 'GXCompCnt': 'SW_I(0, 3)', 'GXCompType': 'SW_I(0, 5)',
    'u8': 'SW_I(0, 40)', 'u16': 'SW_I(0, 700)', 'u32': 'SW_I(0, 130)', 's8': 'SW_I(-20, 20)', 'int': 'SW_I(0, 9)',
}
SWEEP_OP_DOM = {   # (row, parameter name) -> expression, where the id space is wider than the type's default
    ('GXLoadPosMtxImm', 'id'): 'SW_MTXID()', ('GXLoadNrmMtxImm', 'id'): 'SW_MTXID()', ('GXLoadTexMtxImm', 'id'): 'SW_TEXMTXID()',
    ('GXSetCurrentMtx', 'id'): 'SW_MTXID()', ('GXSetIndTexMtx', 'mtx_id'): 'SW_I(0, 4)', ('GXSetChanCtrl', 'light_mask'): 'SW_U32()',
}
SWEEP_ALL_NOTE = '/* every row, in table order */'


def _sweep_expr(row, ptype, pname):
    k = (row, pname)
    if k in SWEEP_OP_DOM:
        return SWEEP_OP_DOM[k]
    t = re.sub(r'\bconst\b', '', ptype).strip()
    if t in SWEEP_TYPE_DOM:
        return SWEEP_TYPE_DOM[t]
    return 'SW_I(0, 15)'      # an enum nobody listed: a small range (a new S row with a new enum type still gets driven)


def gen_sweep(rows):
    rs = [r for r in sorted_rows(rows) if r['thr'] == 'S']
    o = ['/* GENERATED by gen_gx_rec.py sweep -- one random-argument driver per S row (host/tests/gx_rt_state_test.c).  The test defines SW_I / SW_U32 / SW_MAP / SW_LIGHTID / SW_TEXMTXID / SW_COLOR /',
         ' * SW_S10 / SW_F / SW_FILLB / SW_P before including this. */',
         '#define SW_N %d' % len(rs), '#define SW_MAXP 12']
    for i, r in enumerate(rs):
        ia, ib = inline_of(r)
        pi = param_info(r['params'])
        s = 'static void sw_%s(void)\n{\n' % r['name']
        args = []
        for pidx, (t, n, a) in enumerate(pi):
            tt = re.sub(r'\bconst\b', '', t).strip()
            if n == ia or a or '*' in t:
                s += '    float buf_%s[32] __attribute__((aligned(32))); SW_FILLF(buf_%s, 32, %s);\n' % (n, n, '-1.2f, 1.2f' if 'offset' in n else '-3.0f, 3.0f')
                if n == ia and 'GXLightObj' in t:
                    s += '    SW_FILLB(buf_%s, sizeof buf_%s);\n' % (n, n)
                args.append('(void *) buf_%s' % n)
            elif tt == 'GXColor':
                s += '    GXColor %s = SW_COLOR();\n' % n
                args.append(n)
            elif tt == 'GXColorS10':
                s += '    GXColorS10 %s = SW_S10();\n' % n
                args.append(n)
            elif tt == 'f32':
                s += '    f32 %s = SW_F(-4.0f, 4.0f);\n' % n
                args.append(n)
            else:
                s += '    %s %s = (%s) SW_P(%d, %d, %s);\n' % (t, n, t, i, pidx, _sweep_expr(r['name'], t, n))
                args.append(n)
        s += '    %s(%s);\n}\n' % (r['name'], ', '.join(args))
        o.append(s)
    o.append('typedef struct { const char *name; void (*fn)(void); unsigned op; const char *pnames[SW_MAXP]; } SwOp;')
    o.append('static const SwOp sw_ops[SW_N] = {')
    for r in rs:
        pn = ', '.join('"%s"' % n for _, n, _ in param_info(r['params']))
        o.append('    { "%s", sw_%s, GXT_OP_%s, { %s } },' % (r['name'], r['name'], r['name'], pn if param_info(r['params']) else ''))
    o.append('};')
    return '\n'.join(o) + '\n'


# --- nm / build-script access ----------------------------------------------------------------------------------------
def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for d in ('/usr/local/vitasdk/bin', 'C:/dev/msys64/usr/local/vitasdk/bin', 'C:/vitasdk/bin'):
        for ext in ('', '.exe'):
            if os.path.exists('%s/%s%s' % (d, name, ext)):
                return '%s/%s%s' % (d, name, ext)
    raise SystemExit('cannot find ' + name)


def run_nm(path, extra=()):
    r = subprocess.run([find_tool('arm-vita-eabi-nm'), *extra, path], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('nm failed on %s: %s' % (path, r.stderr[-300:]))
    return r.stdout


def boundary_from_nm(text):
    names = []
    for ln in text.splitlines():
        m = NM_RE.match(ln.strip())
        if m:
            names.append(m.group(1))
    return names


# --- lints: each takes data, returns a list of findings (empty = pass), so selftest can feed mutations -----------------
def lint_table_vs_nm(rows, nm_names):
    f = []
    tn = [r['name'] for r in rows]
    for n in sorted({x for x in tn if tn.count(x) > 1}):
        f.append('(1) table lists %s more than once' % n)
    for n in sorted({x for x in nm_names if nm_names.count(x) > 1}):
        f.append('(1) nm lists %s more than once (two definitions?)' % n)
    miss, extra = sorted(set(nm_names) - set(tn)), sorted(set(tn) - set(nm_names))
    for n in miss:
        f.append('(1) %s is DEFINED in the ELF but has no row in the table (a new boundary symbol: classify and add it)' % n)
    for n in extra:
        f.append('(1) table has %s but the ELF defines no such symbol (stale row, or the symbol was removed)' % n)
    cnt = {c: sum(1 for r in rows if r['cls'] == c) for c in CLS_ORDER}
    if cnt != DOC_CLASS_COUNTS:
        f.append('(1) class counts %s differ from docs/53 section 3 %s' % (cnt, DOC_CLASS_COUNTS))
    if len(nm_names) and len(nm_names) < 100:
        f.append('(1) nm returned only %d boundary symbols: the filter or the ELF is wrong, refusing to call that a pass' % len(nm_names))
    return f


def lint_wraps_file(rows, wraps_text, extras=None):
    f = []
    want = wraps_lines(rows, extras)
    got = [l.strip() for l in wraps_text.splitlines() if l.strip()]
    for l in sorted(set(want) - set(got)):
        f.append('(2) gx_wraps.txt lacks %s' % l)
    for l in sorted(set(got) - set(want)):
        f.append('(2) gx_wraps.txt has %s, which is not in the table' % l)
    for l in sorted({x for x in got if got.count(x) > 1}):
        f.append('(2) gx_wraps.txt lists %s more than once' % l)
    return f


def lint_build_script(rows, sh_text, extras=None):
    f = []
    if 'gx_wraps.txt' not in sh_text:
        f.append('(2) build_game_vita.sh never reads gx_wraps.txt: the --wrap set of the build is not the table')
    if not re.search(r'gx_rec\.c', sh_text):
        f.append('(2) build_game_vita.sh never compiles src/gx_rec.c')
    if not re.search(r'gen_gx_rec\.py[^\n]*\bprelink\b', sh_text):
        f.append('(2) build_game_vita.sh never runs `gen_gx_rec.py prelink` before the link')
    if not re.search(r'gen_gx_rec\.py[^\n]*\bpostlink\b', sh_text):
        f.append('(2) build_game_vita.sh never runs `gen_gx_rec.py postlink` on the linked ELF')
    if extras is None:
        extras = parse_thr()[1]
    names = {r['name'] for r in rows} | {e['name'] for e in extras}
    hand = set(re.findall(r'--wrap=(\w+)', sh_text))
    for n in sorted(hand):
        if n in names or re.match(r'(GX|VI)\w*$|gxfifo_push_\w+$', n):
            f.append('(2) build_game_vita.sh hand-writes --wrap=%s: the generated file is the only source of the GX/VI/gxfifo/extra wraps '
                     '(only rhi_create and rhi_present are hand-wired)' % n)
    return f


def lint_fresh(rows, inc_text, wraps_text, extras=None, gen_text=None, ops_text=None, special_text=None):
    f = []
    if extras is None:
        extras = parse_thr()[1]
    h = table_hash(rows)
    m = re.search(r'GXR_TABLE_HASH 0x([0-9a-fA-F]+)U', inc_text or '')
    if not m:
        f.append('(2) gx_rec_wraps.inc is missing or has no GXR_TABLE_HASH')
    elif m.group(1).lower() != h:
        f.append('(2) gx_rec_wraps.inc is STALE: embeds table hash %s, the table hashes to %s -- run gen' % (m.group(1), h))
    elif inc_text != gen_inc(rows):
        f.append('(2) gx_rec_wraps.inc has the right hash but not the generated text (hand-edited, or gx_thr_table.txt changed?) -- run gen')
    if wraps_text != '\n'.join(wraps_lines(rows, extras)) + '\n':
        f.append('(2) gx_wraps.txt is not exactly the generated text -- run gen')
    if gen_text is not None:
        m = re.search(r'GXT_GEN_HASH 0x([0-9a-fA-F]+)U', gen_text)
        want = thr_table_hash(rows, extras)
        if not m:
            f.append('(2) gx_rt_gen.inc is missing or has no GXT_GEN_HASH')
        elif m.group(1).lower() != want:
            f.append('(2) gx_rt_gen.inc is STALE: embeds %s, the table + gx_thr_table.txt hash to %s -- run gen' % (m.group(1), want))
        elif gen_text != gen_rt_gen(rows, extras):
            f.append('(2) gx_rt_gen.inc has the right hash but not the generated text (hand-edited?) -- run gen')
    if ops_text is not None and strip_comments(ops_text) != strip_comments(gen_ops_h(rows, extras)):
        f.append('(2) gx_rt_ops.h: the non-comment lines differ from the generated ones (stale op numbers?) -- run gen')
    if special_text is not None and strip_comments(special_text) != strip_comments(gen_special_h(rows, extras)):
        f.append('(2) gx_rt_special.h: the non-comment lines differ from the generated prototypes -- run gen')
    return f


def read(p):
    try:
        return open(p, encoding='utf-8', newline='').read()
    except OSError:
        return None


def winpath(p):
    """/d/foo -> D:/foo for the native Windows Python (the build scripts hand us MSYS paths)"""
    m = re.match(r'^/([a-zA-Z])/(.*)$', p)
    return '%s:/%s' % (m.group(1).upper(), m.group(2)) if m and os.name == 'nt' else p


def hal_dir():
    return winpath(devcfg.need('STFX_SRC', os.environ.get('STFX_SRC') or STFX_DEFAULT).replace('\\', '/')) + '/src/port/src/hal'


HAL_PROTO_FILES = ('gx_draw.c', 'gx_tev.c', 'gx_light.c', 'vi_shim.c', 'renderer/rhi.c', 'os_threads.c')


def hal_sources(d=None):
    d = d or hal_dir()
    return {f: read_text(d + '/' + f) for f in HAL_PROTO_FILES}


def lint_protos(extras, srcs):
    """every [extra]'s prototype (return type + parameter TYPES) equals the HAL definition's, so a HAL signature change cannot silently desync the
    wrapper from the function it wraps (the linker would not notice: a wrapper with the wrong parameters just reads garbage)."""
    f = []
    for e in extras:
        pat = re.compile(r'^(?P<ret>[A-Za-z_][\w \t\*]*?)[ \t\*]%s[ \t]*\((?P<params>[^;{}]*?)\)[ \t\r\n]*\{' % re.escape(e['name']), re.M)
        found = None
        for fn, txt in srcs.items():
            if txt is None:
                continue
            m = pat.search(txt)
            if m:
                found = (fn, m)
                break
        if not found:
            f.append('(P) cannot find the HAL definition of the [extra] %s in %s' % (e['name'], ' '.join(srcs)))
            continue
        fn, m = found
        have = proto_norm(m.group('ret'), re.sub(r'\s+', ' ', m.group('params')))
        want = proto_norm(e['ret'], e['params'])
        if have != want:
            f.append('(P) [extra] %s: the table says %s, %s defines %s' % (e['name'], want, fn, have))
    for fn, txt in srcs.items():
        if txt is None:
            f.append('(P) cannot read the HAL source %s (STFX_SRC=%s)' % (fn, os.environ.get('STFX_SRC') or STFX_DEFAULT))
    return f


def lint_s_defs(rows, srcs):
    """every S row (R2_STATE.md 4.2) has its G variant gxh_g_<name> DEFINED in the HAL sources: either GXH_PAIR(<name>, ...) (the public setter and its G variant around one body)
    or a hand-written `gxh_g_<name>(...) {`.  A prototype alone does not count.  The linked ELF is checked again by expected_syms (nm)."""
    f = []
    txts = [t for t in srcs.values() if t is not None]
    for r in rows:
        if r.get('thr') != 'S':
            continue
        n = re.escape(r['name'])
        pair = re.compile(r'(?m)^GXH_PAIR\(\s*%s\s*,' % n)
        hand = re.compile(r'(?m)^[ \t]*void\s+gxh_g_%s\s*\([^;{}]*\)\s*\{' % n)
        if not any(pair.search(t) or hand.search(t) for t in txts):
            f.append('(S) %s is an S row but no HAL source defines gxh_g_%s (GXH_PAIR or a hand-written definition)' % (r['name'], r['name']))
    return f


def lint_sweep(rows, text):
    """the generated state sweep (gen_sweep) covers exactly the S rows of the table: a driver and an op-table entry for each, none for anything else, and the right SW_N"""
    f = []
    s_rows = [r['name'] for r in rows if r.get('thr') == 'S']
    have = set(re.findall(r'static void sw_(\w+)\(void\)', text))
    for n in s_rows:
        if n not in have or ('GXT_OP_%s,' % n) not in text:
            f.append('(S) the sweep has no driver / table entry for the S row %s' % n)
    for n in sorted(have - set(s_rows)):
        f.append('(S) the sweep has a driver for %s, which is not an S row' % n)
    m = re.search(r'#define SW_N (\d+)', text)
    if not m or int(m.group(1)) != len(s_rows):
        f.append('(S) the sweep says SW_N %s, the table has %d S rows' % (m.group(1) if m else '?', len(s_rows)))
    return f


def lint_all(rows, nm_names, wraps_text, sh_text, inc_text, extras=None, gen_text=None, ops_text=None, special_text=None, srcs=None):
    f = []
    if extras is None:
        extras = parse_thr()[1]
    f += lint_thr(rows, extras)
    if nm_names is not None:
        f += lint_table_vs_nm(rows, nm_names)
    f += lint_wraps_file(rows, wraps_text or '', extras)
    f += lint_build_script(rows, sh_text or '', extras)
    f += lint_fresh(rows, inc_text, wraps_text, extras, gen_text, ops_text, special_text)
    if srcs is not None:
        f += lint_protos(extras, srcs)
        f += lint_s_defs(rows, srcs)
    return f


# --- (3) after a build -------------------------------------------------------------------------------------------------
def expected_syms(rows, extras):
    """what the integrator expects the ELF to define: the ring API, the R dispatch, and BOTH halves of every custom row.  A failed closure links a
    stale .o silently and the hooks then bind weak to 0 (memory: stale-vita-object) -- this list is the check."""
    e = ['gxt_boot', 'gxt_frame_edge', 'gxt_present', 'gxt_quit', 'gxt_free', 'gxt_r_exec', 'gxs_flush', 'gxs_r_apply']   # gxs_*: the R2 state module (a stale object binds its weak hooks to 0)
    for r in all_entries(rows, extras):
        if r['thr'] == 'X':
            e += ['gxts_G_' + r['name'], 'gxts_R_' + r['name']]
        if r['thr'] == 'S':
            e += ['gxh_g_' + r['name']]
    return e


def parse_nm(out):
    defs, und = {}, set()
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3:
            defs[p[2]] = p[1]
        elif len(p) == 2 and p[0] == 'U':
            und.add(p[1])
    return defs, und


def lint_post_syms(rows, extras, defs, und):
    f = []
    for r in all_entries(rows, extras):
        n = r['name']
        if defs.get('__wrap_' + n) not in ('T', 't'):
            f.append('(3) __wrap_%s is not a defined function in the ELF (gx_rec.o not linked, or its row missing)' % n)
        if defs.get(n) not in ('T', 't', 'W', 'w'):
            f.append('(3) %s itself is not defined in the ELF: __real_%s cannot have resolved to anything' % (n, n))
    for s in sorted(x for x in und if x.startswith('__real_') or x.startswith('__wrap_')):
        f.append('(3) %s is still undefined in the ELF' % s)
    for s in expected_syms(rows, extras):
        if defs.get(s) not in ('T', 't'):
            f.append('(3) the ELF does not define %s (a stale or missing object: gx_thread.o / gx_rt_special.o / gx_rec.o)' % s)
    return f


def lint_wraps_used(rows, extras, text):
    f = []
    names = {r['name'] for r in all_entries(rows, extras)}
    used = set(re.findall(r'--wrap=(\S+)', text))
    for n in sorted(names - used):
        f.append('(3) the link did not pass --wrap=%s' % n)
    for n in sorted(x for x in used - names if re.match(r'(GX|VI|gxfifo_push_)', x)):
        f.append('(3) the link passed --wrap=%s, which is not in the table' % n)
    if 'rhi_present' not in used:
        f.append('(3) the link did not pass the hand-wired --wrap=rhi_present')
    return f


def postlink(rows, elf, wraps_used=None, gx_rec_o=None, extras=None):
    if extras is None:
        extras = parse_thr()[1]
    info = []
    names = [r['name'] for r in all_entries(rows, extras)]
    defs, und = parse_nm(run_nm(elf))
    f = lint_post_syms(rows, extras, defs, und)
    info.append('(3) expected symbols (every one must be defined): %s' % ' '.join(expected_syms(rows, extras)))
    if gx_rec_o and os.path.exists(gx_rec_o):
        o = run_nm(gx_rec_o)
        refs = {l.split()[1][7:] for l in o.splitlines() if len(l.split()) == 2 and l.split()[0] == 'U' and l.split()[1].startswith('__real_')}
        for n in sorted(set(names) - refs):
            f.append('(3) gx_rec.o never references __real_%s: its wrapper does not call through' % n)
        for n in sorted(refs - set(names)):
            f.append('(3) gx_rec.o references __real_%s, which is not in the table' % n)
    if wraps_used and os.path.exists(wraps_used):
        f += lint_wraps_used(rows, extras, read(wraps_used))
    else:
        info.append('(3) no wraps_used file: the effective --wrap set of the link was not compared')
    # who really calls each wrapper: static `bl __wrap_X` sites in the linked image.  Informational; zero total is a failure
    # (the wraps would be dead code: the link did not redirect anything).
    od = subprocess.run([find_tool('arm-vita-eabi-objdump'), '-d', '--no-show-raw-insn', elf], capture_output=True, text=True)
    sites = {}
    nameset = set(names)
    for m in re.finditer(r'\b(?:bl|b|blx)\s+[0-9a-fA-F]+ <__wrap_(\w+)>', od.stdout):
        if m.group(1) in nameset:
            sites[m.group(1)] = sites.get(m.group(1), 0) + 1
    total = sum(sites.values())
    if total == 0:
        f.append('(3) no call site anywhere in the image reaches a __wrap_ function: --wrap redirected nothing')
    zero = sorted(n for n in names if n not in sites)
    info.append('(3) static call sites that reach a wrapper: %d over %d of %d symbols; no caller (%d): %s%s' %
                (total, len(sites), len(names), len(zero), ' '.join(zero[:12]) or '-', ' ...' if len(zero) > 12 else ''))
    return f, info


# --- selftest: every lint is watched failing ---------------------------------------------------------------------------
def selftest():
    rows = parse_table()
    thr_text = read_text(THR_TXT)
    thr0, extras = parse_thr()
    nm = [r['name'] for r in rows]
    wraps = '\n'.join(wraps_lines(rows, extras)) + '\n'
    inc = gen_inc(rows)
    gen = gen_rt_gen(rows, extras)
    ops = gen_ops_h(rows, extras)
    spec = gen_special_h(rows, extras)
    sh_ok = ('arm-vita-eabi-gcc -c "$V/src/gx_rec.c"\nWRAPS="$WRAPS $(cat "$V/host/engine/gx_wraps.txt")"\n'
             'python "$V/host/engine/gen_gx_rec.py" prelink\npython "$V/host/engine/gen_gx_rec.py" postlink "$G/x.elf"\n'
             'WRAPS="-Wl,--wrap=rhi_create -Wl,--wrap=rhi_present"\n')
    bad = 0

    def expect(label, findings, want_fail):
        nonlocal bad
        ok = bool(findings) == want_fail
        print('%-70s %s%s' % (label, 'FAILS as it must' if want_fail and findings else ('passes' if not findings else 'FAILS'),
                              '' if ok else '   <== SELFTEST BROKEN'))
        if findings and want_fail:
            print('      e.g. ' + findings[0])
        if not ok:
            bad += 1

    def remut(text, old, new):          # a mutation that must change something, or the expectation below would be vacuous
        assert old in text, 'vacuous mutation: %r not found' % old
        return text.replace(old, new, 1)

    def table_with(thr_mut):
        t, e = parse_thr(thr_mut)
        r = attach_thr(parse_table(attach=False), t)
        return r, e

    expect('real table vs nm (133 symbols)', lint_table_vs_nm(rows, nm), False)
    expect('real gx_wraps.txt', lint_wraps_file(rows, wraps, extras), False)
    expect('good build script text', lint_build_script(rows, sh_ok, extras), False)
    expect('fresh generated files (wraps, gen, ops, special)', lint_fresh(rows, inc, wraps, extras, gen, ops, spec), False)
    expect('gx_thr_table.txt itself', lint_thr(rows, extras), False)
    # (1) mutated tables
    expect('(1) table with GXSetTevOrder DROPPED', lint_table_vs_nm([r for r in rows if r['name'] != 'GXSetTevOrder'], nm), True)
    expect('(1) table with bogus GXFrobnicate ADDED',
           lint_table_vs_nm(rows + [dict(cls='A', rec=1, ret='void', name='GXFrobnicate', params='void', inline='')], nm), True)
    expect('(1) table with GXBegin listed TWICE', lint_table_vs_nm(rows + [r for r in rows if r['name'] == 'GXBegin'], nm), True)
    expect('(1) nm gained GXNewThing (a new defined symbol)', lint_table_vs_nm(rows, nm + ['GXNewThing']), True)
    expect('(1) class of GXBegin changed A->E', lint_table_vs_nm([dict(r, cls='E') if r['name'] == 'GXBegin' else r for r in rows], nm), True)
    expect('(1) nm filter broken (3 symbols)', lint_table_vs_nm(rows, nm[:3]), True)
    # (S) the sweep is enumerated from the table: a NEW S row gets a driver with no edit, and a stale sweep is a finding
    sw_all = gen_sweep(rows)
    n_s = len([r for r in rows if r['thr'] == 'S'])
    expect('(S) the generated sweep covers exactly the S rows', lint_sweep(rows, sw_all), False)
    expect('(S) a sweep with one driver REMOVED (GXSetCullMode)', lint_sweep(rows, remut(sw_all, 'static void sw_GXSetCullMode(void)', 'static void sw_Gone(void)')), True)
    expect('(S) a sweep with a driver for a NON-S row (GXSetViewport)', lint_sweep(rows, sw_all + 'static void sw_GXSetViewport(void) { }\n'), True)
    expect('(S) a sweep whose row count is stale', lint_sweep(rows, remut(sw_all, '#define SW_N %d' % n_s, '#define SW_N %d' % (n_s - 1))), True)
    r_new = [dict(r, thr='S') if r['name'] == 'GXSetViewport' else r for r in rows]
    expect('(S) a NEW S row (GXSetViewport re-classified S): the OLD sweep is stale', lint_sweep(r_new, sw_all), True)
    expect('(S) ...and the regenerated sweep covers it with no edit', lint_sweep(r_new, gen_sweep(r_new)), False)
    # (S) the S rows: disposition rules and the HAL definitions of their G variants
    expect('(T) an S row that returns a value (GXFlush_ as S)', lint_thr(table_with(remut(thr_text, 'GXFlush_|L|0', 'GXFlush_|S|0'))[0], extras), True)
    expect('(T) an S row that is not a state write (GXCopyTex-class C as S: GXLoadTexObj|X -> S)', lint_thr(table_with(remut(thr_text, 'GXLoadTexObj|X|0', 'GXLoadTexObj|S|0'))[0], extras), True)
    s_good = {'gx_x.c': ''.join('GXH_PAIR(%s, (void))\n' % r['name'] for r in rows if r['thr'] == 'S')}
    expect('(S) every S row has a GXH_PAIR definition', lint_s_defs(rows, s_good), False)
    expect('(S) one S row without a G variant (GXSetCullMode)', lint_s_defs(rows, {'gx_x.c': s_good['gx_x.c'].replace('GXH_PAIR(GXSetCullMode, (void))\n', '')}), True)
    expect('(S) a G variant that is only a PROTOTYPE does not count', lint_s_defs(rows, {'gx_x.c': s_good['gx_x.c'].replace('GXH_PAIR(GXSetCullMode, (void))\n', 'void gxh_g_GXSetCullMode(int mode);\n')}), True)
    expect('(S) a hand-written definition counts (GXSetTevIndRepeat)', lint_s_defs(rows, {'gx_x.c': s_good['gx_x.c'].replace('GXH_PAIR(GXSetTevIndRepeat, (void))\n', 'void gxh_g_GXSetTevIndRepeat(GXTevStageID stage) { }\n')}), False)
    expect('(S) the HAL source unreadable', lint_s_defs(rows, {'gx_x.c': None}), True)
    srcs_hal = hal_sources()
    if all(v is not None for v in srcs_hal.values()):
        expect('(S) the real HAL sources define every S row\'s G variant', lint_s_defs(rows, srcs_hal), False)
        expect('(S) the real HAL with GXSetCullMode\'s pair removed', lint_s_defs(rows, {k: (v.replace('GXH_PAIR(GXSetCullMode,', 'GXH_PAIR_GONE(GXSetCullMode,') if v else v) for k, v in srcs_hal.items()}), True)
    # (2) the build's --wrap list
    expect('(2) gx_wraps.txt with one line DROPPED', lint_wraps_file(rows, '\n'.join(wraps_lines(rows, extras)[1:]) + '\n', extras), True)
    expect('(2) gx_wraps.txt with a bogus --wrap ADDED', lint_wraps_file(rows, wraps + '-Wl,--wrap=GXBogus\n', extras), True)
    expect('(2) gx_wraps.txt without the 19 extras (the T0-a file)', lint_wraps_file(rows, '\n'.join(wraps_lines(rows, [])) + '\n', extras), True)
    expect('(2) build script that never reads gx_wraps.txt', lint_build_script(rows, sh_ok.replace('gx_wraps.txt', 'nothing.txt'), extras), True)
    expect('(2) build script with a hand-written --wrap=GXBegin', lint_build_script(rows, sh_ok + 'WRAPS="$WRAPS -Wl,--wrap=GXBegin"\n', extras), True)
    expect('(2) build script with a hand-written --wrap=rhi_beginFrame', lint_build_script(rows, sh_ok + 'WRAPS="$WRAPS -Wl,--wrap=rhi_beginFrame"\n', extras), True)
    expect('(2) build script with a hand-written --wrap=os_ext_poll', lint_build_script(rows, sh_ok + 'WRAPS="$WRAPS -Wl,--wrap=os_ext_poll"\n', extras), True)
    expect('(2) build script without the postlink gate', lint_build_script(rows, sh_ok.replace('postlink', 'nolink'), extras), True)
    # freshness
    dropped = [r for r in rows if r['name'] != 'GXSetTevOrder']
    expect('(2) .inc generated from a table with a row dropped', lint_fresh(rows, gen_inc(dropped), wraps, extras), True)
    expect('(2) .inc hand-edited after generation', lint_fresh(rows, inc.replace('++g_cnt[3];', '++g_cnt[4];', 1), wraps, extras), True)
    expect('(2) gx_wraps.txt stale against the table', lint_fresh(rows, inc, '\n'.join(wraps_lines(dropped, extras)) + '\n', extras), True)
    # the disposition table: a re-classification changes the generated files, so the committed ones go stale
    r2, e2 = table_with(remut(thr_text, 'GXSetViewport|R|0', 'GXSetViewport|L|0'))
    expect('(T) GXSetViewport re-classified R->L: the committed gx_rec_wraps.inc is stale', lint_fresh(r2, inc, wraps, e2, gen, ops, spec), True)
    expect('(T) GXSetViewport re-classified R->L: the committed gx_rt_gen.inc is stale', lint_fresh(r2, gen_inc(r2), wraps, e2, gen, ops, spec), True)
    r2, e2 = table_with(remut(thr_text, 'GXSetTevIndirect|S|1', 'GXSetTevIndirect|S|0'))
    expect('(T) the edge flag of GXSetTevIndirect dropped: wraps.inc stale', lint_fresh(r2, inc, wraps, e2, gen, ops, spec), True)
    expect('(2) gx_rt_gen.inc hand-edited after generation', lint_fresh(rows, inc, wraps, extras, remut(gen, 'return -1;', 'return 0;'), ops, spec), True)
    expect('(2) gx_rt_ops.h with one op number changed', lint_fresh(rows, inc, wraps, extras, gen, remut(ops, 'GXT_OP_GXBegin                    1', 'GXT_OP_GXBegin                    9'), spec), True)
    expect('(2) gx_rt_ops.h with a COMMENT changed (prose is not compared)', lint_fresh(rows, inc, wraps, extras, gen, remut(ops, 'Modules "ring"', 'Modules (ring)'), spec), False)
    expect('(2) gx_rt_special.h with a prototype changed', lint_fresh(rows, inc, wraps, extras, gen, ops, remut(spec, 'u16 nverts', 'u32 nverts')), True)
    # lint_thr: the table's own consistency
    expect('(T) a table row without a thr entry', lint_thr(table_with(remut(thr_text, 'GXSetViewport|R|0|state write: args by value\n', ''))[0], extras), True)
    expect('(T) thr Z', lint_thr(table_with(remut(thr_text, 'GXSetViewport|R|0', 'GXSetViewport|Z|0'))[0], extras), True)
    expect('(T) thr P on a non-pipe row', lint_thr(table_with(remut(thr_text, 'GXSetViewport|R|0', 'GXSetViewport|P|0'))[0], extras), True)
    expect('(T) an R row that returns a value (GXFlush_ as R)', lint_thr(table_with(remut(thr_text, 'GXFlush_|L|0', 'GXFlush_|R|0'))[0], extras), True)
    expect('(T) an [extra] dropped (18 of 19)', lint_thr(rows, extras[:-1]), True)
    expect('(T) an [extra] that is also a table row', lint_thr(rows, extras + [dict(extras[0], name='GXBegin')]), True)
    expect('(T) an E extra with no drop counter', lint_thr(rows, [dict(e, name='os_other') if e['name'] == 'os_ext_poll' else e for e in extras]), True)
    # prototypes of the extras against the HAL text
    srcs_ok = {'a.c': 'void gx_tex_invalidate(const void* addr, u32 n) {\n}\nint gx_breakpoint_deliver(void) {\n'}
    sub = [e for e in extras if e['name'] in ('gx_tex_invalidate', 'gx_breakpoint_deliver')]
    expect('(P) prototype text equal (names ignored)', lint_protos(sub, srcs_ok), False)
    expect('(P) HAL parameter type changed (u32 -> u64)', lint_protos(sub, {'a.c': srcs_ok['a.c'].replace('u32 n', 'u64 n')}), True)
    expect('(P) HAL return type changed (int -> void)', lint_protos(sub, {'a.c': srcs_ok['a.c'].replace('int gx_breakpoint', 'void gx_breakpoint')}), True)
    expect('(P) HAL definition missing', lint_protos(sub, {'a.c': srcs_ok['a.c'].replace('gx_tex_invalidate', 'gx_tex_other')}), True)
    expect('(P) HAL source unreadable', lint_protos(sub, {'a.c': None}), True)
    srcs_real = hal_sources()
    if all(v is not None for v in srcs_real.values()):
        expect('(P) the real HAL sources (%s)' % hal_dir(), lint_protos(extras, srcs_real), False)
        mut = {k: (v.replace('void gx_tex_release(const void* addr, u32 n)', 'void gx_tex_release(const void* addr, u16 n)') if v else v) for k, v in srcs_real.items()}
        expect('(P) the real HAL with gx_tex_release(u32 -> u16)', lint_protos(extras, mut), True)
    else:
        print('(P) the real HAL sources are not readable here (%s): that check was NOT run' % hal_dir())
    # postlink
    ns = ' '.join(['gxt_boot', 'gxt_frame_edge', 'gxt_present', 'gxt_quit', 'gxt_free', 'gxt_r_exec', 'gxs_flush', 'gxs_r_apply'])
    defs = {}
    for r in all_entries(rows, extras):
        defs[r['name']] = 'T'
        defs['__wrap_' + r['name']] = 'T'
        if r['thr'] == 'X':
            defs['gxts_G_' + r['name']] = 'T'
            defs['gxts_R_' + r['name']] = 'T'
        if r['thr'] == 'S':
            defs['gxh_g_' + r['name']] = 'T'
    for s_ in ns.split():
        defs[s_] = 'T'
    expect('(3) a complete symbol set', lint_post_syms(rows, extras, defs, set()), False)
    expect('(3) gxt_r_exec missing from the ELF (stale gx_rec.o)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxt_r_exec'}, set()), True)
    expect('(3) gxt_free missing (gx_thread.o not linked)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxt_free'}, set()), True)
    expect('(3) gxh_g_GXSetTevOrder missing (a stale HAL object: the S row would bind its G variant to 0)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxh_g_GXSetTevOrder'}, set()), True)
    expect('(3) gxs_flush missing (gx_rt_state.o not linked: the ring\'s weak flush hook binds to 0)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxs_flush'}, set()), True)
    expect('(3) gxs_r_apply missing (R would drop every STATE record)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxs_r_apply'}, set()), True)
    expect('(3) gxts_G_GXBegin missing (special module not linked)', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'gxts_G_GXBegin'}, set()), True)
    expect('(3) gxts_R_gx_dlk_event only WEAK/undefined', lint_post_syms(rows, extras, dict(defs, gxts_R_gx_dlk_event='W'), set()), True)
    expect('(3) __wrap_os_ext_poll missing', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != '__wrap_os_ext_poll'}, set()), True)
    expect('(3) vi_set_clear_color itself undefined', lint_post_syms(rows, extras, {k: v for k, v in defs.items() if k != 'vi_set_clear_color'}, set()), True)
    expect('(3) __real_rhi_clear still undefined', lint_post_syms(rows, extras, defs, {'__real_rhi_clear'}), True)
    wu = ' '.join(l[len('-Wl,'):] for l in wraps_lines(rows, extras)) + ' --wrap=rhi_present --wrap=rhi_create'
    expect('(3) the link passed every --wrap', lint_wraps_used(rows, extras, wu), False)
    expect('(3) the link did not pass --wrap=gx_dlk_event', lint_wraps_used(rows, extras, wu.replace('--wrap=gx_dlk_event', '')), True)
    expect('(3) the link passed a stray --wrap=GXFoo', lint_wraps_used(rows, extras, wu + ' --wrap=GXFoo'), True)
    expect('(3) the link lost the hand-wired --wrap=rhi_present', lint_wraps_used(rows, extras, wu.replace('--wrap=rhi_present', '')), True)
    # the files on disk (what the build really uses)
    expect('on-disk generated files vs table', lint_fresh(rows, read(INC), read(WRAPS_TXT), extras, read(GEN_INC), read(OPS_H), read(SPECIAL_H)), False)
    expect('on-disk build_game_vita.sh', lint_build_script(rows, read(BUILD_SH) or '', extras), False)
    print('SELFTEST %s' % ('OK: every lint was watched failing' if not bad else 'BROKEN: %d expectation(s) wrong' % bad))
    return 1 if bad else 0


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else 'lint'
    rows = parse_table()
    extras = parse_thr()[1]
    opt = lambda k: (a[a.index(k) + 1] if k in a else None)
    if cmd == 'gen':
        outs = ((INC, gen_inc(rows)), (WRAPS_TXT, '\n'.join(wraps_lines(rows, extras)) + '\n'), (GEN_INC, gen_rt_gen(rows, extras)),
                (OPS_H, gen_ops_h(rows, extras)), (SPECIAL_H, gen_special_h(rows, extras)))
        for path, text in outs:
            open(path, 'w', encoding='utf-8', newline='').write(text)
        print('wrote %s: %d table symbols + %d extras, table hash %s, thr hash %s' %
              (', '.join(os.path.basename(p) for p, _ in outs), len(rows), len(extras), table_hash(rows), thr_table_hash(rows, extras)))
        ntev = [r['name'] for r in sorted_rows(rows) if kind_of(r) & K_TEV]
        print('N_tev set (%d): %s' % (len(ntev), ' '.join(ntev)))
        cnt = {k: sum(1 for r in rows if r['thr'] == k) for k in 'RSLXP'}
        cnt_e = {k: sum(1 for e in extras if e['thr'] == k) for k in 'RXET'}
        print('thr table rows %s, extras %s' % (cnt, cnt_e))
        return 0
    if cmd == 'stubs':
        open(a[1], 'w', encoding='utf-8', newline='').write(gen_stubs(rows, extras))
        return 0
    if cmd == 'drivers':
        open(a[1], 'w', encoding='utf-8', newline='').write(gen_drivers(rows, extras))
        return 0
    if cmd == 'standins':
        open(a[1], 'w', encoding='utf-8', newline='').write(gen_custom_standins(rows, extras))
        return 0
    if cmd == 'sweep':
        open(a[1], 'w', encoding='utf-8', newline='').write(gen_sweep(rows))
        return 0
    if cmd == 'hoststubs':
        defined = {l.strip() for l in (read(a[2]) or '').splitlines() if l.strip()}
        open(a[1], 'w', encoding='utf-8', newline='').write(gen_stand_ins(rows, extras, defined))
        return 0
    if cmd == 'selftest':
        return selftest()
    if cmd in ('lint', 'prelink'):
        nm = None
        if cmd == 'lint':
            if opt('--nm-file'):
                nm = boundary_from_nm(read(opt('--nm-file')))
            else:
                nm = boundary_from_nm(run_nm(devcfg.need('--elf (or GXR_ELF)', opt('--elf') or os.environ.get('GXR_ELF') or DEFAULT_ELF)))
        f = lint_all(rows, nm, read(opt('--wraps-file') or WRAPS_TXT), read(opt('--build-sh') or BUILD_SH), read(opt('--inc-file') or INC), extras,
                     read(opt('--gen-file') or GEN_INC), read(opt('--ops-file') or OPS_H), read(opt('--special-file') or SPECIAL_H),
                     hal_sources(opt('--hal-dir')))
        for x in f:
            print('LINT FAIL ' + x)
        print('gx_rec %s: %d table symbols + %d extras, table hash %s, thr hash %s, %s' %
              (cmd, len(rows), len(extras), table_hash(rows), thr_table_hash(rows, extras), 'FAIL (%d findings)' % len(f) if f else 'PASS'))
        return 9 if f else 0
    if cmd == 'postlink':
        elf = a[1]
        f, info = postlink(rows, elf, opt('--wraps-used'), opt('--gx-rec-o'), extras)
        for x in info:
            print(x)
        for x in f:
            print('LINT FAIL ' + x)
        print('gx_rec postlink: %s' % ('FAIL (%d findings)' % len(f) if f else 'PASS'))
        return 9 if f else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
