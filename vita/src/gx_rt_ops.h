/* gx_rt_ops.h -- record op numbers of the GX render thread.  GENERATED once by the planner from gen_gx_rec.py TABLE (rows sorted by name, op = index + 1) and host/engine/gx_thr_table.txt [extra]
 * (sorted by name, op = 200 + index).  Module "gen" (host/engine/gen_gx_rec.py) OWNS it from now on and must reproduce its NON-COMMENT lines identically (its lint fails on a stale copy).
 * Modules "ring" and "special" include it read-only. */
#ifndef GX_RT_OPS_H
#define GX_RT_OPS_H

#define GXT_OP_TABLE_N 134
#define GXT_OP_EXTRA_BASE 200
#define GXT_OP_EXTRA_N 19
#define GXT_OP_GXBegin                    1
#define GXT_OP_GXBeginDisplayList         2
#define GXT_OP_GXCallDisplayList          3
#define GXT_OP_GXClearVtxDesc             4
#define GXT_OP_GXCopyDisp                 5
#define GXT_OP_GXCopyTex                  6
#define GXT_OP_GXDisableBreakPt           7
#define GXT_OP_GXDrawTorus                8
#define GXT_OP_GXEnableBreakPt            9
#define GXT_OP_GXEnableTexOffsets         10
#define GXT_OP_GXEndDisplayList           11
#define GXT_OP_GXFlush                    12
#define GXT_OP_GXFlush_                   13
#define GXT_OP_GXGetFifoPtrs              14
#define GXT_OP_GXGetGPStatus              15
#define GXT_OP_GXGetLightAttnK            16
#define GXT_OP_GXGetScissor               17
#define GXT_OP_GXGetTexBufferSize         18
#define GXT_OP_GXGetTexObjData            19
#define GXT_OP_GXGetTexObjFmt             20
#define GXT_OP_GXGetTexObjHeight          21
#define GXT_OP_GXGetTexObjUserData        22
#define GXT_OP_GXGetTexObjWidth           23
#define GXT_OP_GXInit                     24
#define GXT_OP_GXInitFifoBase             25
#define GXT_OP_GXInitFifoLimits           26
#define GXT_OP_GXInitLightAttn            27
#define GXT_OP_GXInitLightAttnA           28
#define GXT_OP_GXInitLightAttnK           29
#define GXT_OP_GXInitLightColor           30
#define GXT_OP_GXInitLightDir             31
#define GXT_OP_GXInitLightDistAttn        32
#define GXT_OP_GXInitLightPos             33
#define GXT_OP_GXInitLightSpot            34
#define GXT_OP_GXInitSpecularDir          35
#define GXT_OP_GXInitTexObj               36
#define GXT_OP_GXInitTexObjLOD            37
#define GXT_OP_GXInitTexObjUserData       38
#define GXT_OP_GXInvalidateTexAll         39
#define GXT_OP_GXInvalidateVtxCache       40
#define GXT_OP_GXLoadLightObjImm          41
#define GXT_OP_GXLoadNrmMtxImm            42
#define GXT_OP_GXLoadPosMtxImm            43
#define GXT_OP_GXLoadTexMtxImm            44
#define GXT_OP_GXLoadTexObj               45
#define GXT_OP_GXLoadTexObjPreLoaded      46
#define GXT_OP_GXPeekZ                    47
#define GXT_OP_GXPixModeSync              48
#define GXT_OP_GXPreLoadEntireTexture     49
#define GXT_OP_GXReadDrawSync             50
#define GXT_OP_GXReadXfRasMetric          51
#define GXT_OP_GXResetWriteGatherPipe     52
#define GXT_OP_GXSetAlphaCompare          53
#define GXT_OP_GXSetAlphaUpdate           54
#define GXT_OP_GXSetArray                 55
#define GXT_OP_GXSetBlendMode             56
#define GXT_OP_GXSetBreakPtCallback       57
#define GXT_OP_GXSetCPUFifo               58
#define GXT_OP_GXSetChanAmbColor          59
#define GXT_OP_GXSetChanCtrl              60
#define GXT_OP_GXSetChanMatColor          61
#define GXT_OP_GXSetColorUpdate           62
#define GXT_OP_GXSetCopyClear             63
#define GXT_OP_GXSetCopyFilter            64
#define GXT_OP_GXSetCullMode              65
#define GXT_OP_GXSetCurrentMtx            66
#define GXT_OP_GXSetDispCopyDst           67
#define GXT_OP_GXSetDispCopyGamma         68
#define GXT_OP_GXSetDispCopySrc           69
#define GXT_OP_GXSetDispCopyYScale        70
#define GXT_OP_GXSetDither                71
#define GXT_OP_GXSetDrawSync              72
#define GXT_OP_GXSetFieldMode             73
#define GXT_OP_GXSetFog                   74
#define GXT_OP_GXSetGPFifo                75
#define GXT_OP_GXSetGPMetric              76
#define GXT_OP_GXSetIndTexCoordScale      77
#define GXT_OP_GXSetIndTexMtx             78
#define GXT_OP_GXSetIndTexOrder           79
#define GXT_OP_GXSetLineWidth             80
#define GXT_OP_GXSetMisc                  81
#define GXT_OP_GXSetNumChans              82
#define GXT_OP_GXSetNumIndStages          83
#define GXT_OP_GXSetNumTevStages          84
#define GXT_OP_GXSetNumTexGens            85
#define GXT_OP_GXSetPixelFmt              86
#define GXT_OP_GXSetPointSize             87
#define GXT_OP_GXSetProjection            88
#define GXT_OP_GXSetScissor               89
#define GXT_OP_GXSetTevAlphaIn            90
#define GXT_OP_GXSetTevAlphaOp            91
#define GXT_OP_GXSetTevColor              92
#define GXT_OP_GXSetTevColorIn            93
#define GXT_OP_GXSetTevColorOp            94
#define GXT_OP_GXSetTevColorS10           95
#define GXT_OP_GXSetTevDirect             96
#define GXT_OP_GXSetTevIndRepeat          97
#define GXT_OP_GXSetTevIndirect           98
#define GXT_OP_GXSetTevKAlphaSel          99
#define GXT_OP_GXSetTevKColor             100
#define GXT_OP_GXSetTevKColorSel          101
#define GXT_OP_GXSetTevOp                 102
#define GXT_OP_GXSetTevOrder              103
#define GXT_OP_GXSetTevSwapMode           104
#define GXT_OP_GXSetTevSwapModeTable      105
#define GXT_OP_GXSetTexCoordGen2          106
#define GXT_OP_GXSetTexCopyDst            107
#define GXT_OP_GXSetTexCopySrc            108
#define GXT_OP_GXSetViewport              109
#define GXT_OP_GXSetViewportJitter        110
#define GXT_OP_GXSetVtxAttrFmt            111
#define GXT_OP_GXSetVtxDesc               112
#define GXT_OP_GXSetZCompLoc              113
#define GXT_OP_GXSetZMode                 114
#define GXT_OP_GXStfxSkinBind             115
#define GXT_OP_GXTexModeSync              116
#define GXT_OP_VIConfigure                117
#define GXT_OP_VIFlush                    118
#define GXT_OP_VIGetDTVStatus             119
#define GXT_OP_VIGetNextField             120
#define GXT_OP_VIGetTvFormat              121
#define GXT_OP_VIInit                     122
#define GXT_OP_VISetBlack                 123
#define GXT_OP_VISetNextFrameBuffer       124
#define GXT_OP_VISetPostRetraceCallback   125
#define GXT_OP_VISetPreRetraceCallback    126
#define GXT_OP_VIWaitForRetrace           127
#define GXT_OP_gxfifo_push_f32            128
#define GXT_OP_gxfifo_push_s16            129
#define GXT_OP_gxfifo_push_s32            130
#define GXT_OP_gxfifo_push_s8             131
#define GXT_OP_gxfifo_push_u16            132
#define GXT_OP_gxfifo_push_u32            133
#define GXT_OP_gxfifo_push_u8             134
#define GXT_OP_gx_breakpoint_deliver      200
#define GXT_OP_gx_dlk_event               201
#define GXT_OP_gx_draw_setAlphaMode       202
#define GXT_OP_gx_draw_setForceLayout     203
#define GXT_OP_gx_draw_setJointMatrices   204
#define GXT_OP_gx_draw_setJointOffsets    205
#define GXT_OP_gx_draw_setRealSkin        206
#define GXT_OP_gx_draw_setShadowRecv      207
#define GXT_OP_gx_draw_setSourceBounds    208
#define GXT_OP_gx_draw_setTexture         209
#define GXT_OP_gx_tex_invalidate          210
#define GXT_OP_gx_tex_release             211
#define GXT_OP_gx_tex_yuv_register        212
#define GXT_OP_gx_tex_yuv_register_x      213
#define GXT_OP_os_ext_poll                214
#define GXT_OP_rhi_beginFrame             215
#define GXT_OP_rhi_clear                  216
#define GXT_OP_rhi_endFrame               217
#define GXT_OP_vi_set_clear_color         218

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
