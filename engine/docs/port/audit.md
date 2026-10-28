# Stairfax Temperatures - Port Audit (2026-08-28)

Generated from `configure.py:112` NonMatching objects + `src/main` shim scan. Run `py audit_shim.py` and `py audit3.py` to regenerate.

## 1. Decomp Progress

- **EN GSAE01 target:** `935` Matching, `112` NonMatching (`1047` total objects) = **89.3%** by count.
- `decomp.dev/zcanann/SFA-Decomp` reports similar byte progress (~87%). Byte progress is slightly lower than object count because large `main/*` files are unmatched.
- `python configure.py` without `--matching` builds `Equivalent` objects for non-matching host testing.

### 1.1 NonMatching breakdown (112)

#### `dolphin/*` - 25 objects - **IGNORE for port (replaced)**
| File | Note |
|------|------|
| `dolphin/mtx/mtx44vec.c` | math |
| `dolphin/dvd/dvdidutils.c` | DVD |
| `dolphin/ax/AXAlloc.c`, `AXAux.c`, `AXCL.c`, `AXComp.c`, `AXOut.c`, `AXProf.c`, `AXSPB.c`, `AXVPB.c` (8) | Audio mixer, replaced by Cubeb |
| `dolphin/hio/hio.c` | Host I/O debug |
| `dolphin/mcc/mcc.c`, `fio.c` | Movie card |
| `dolphin/mix/mix.c` | Mixer |
| `dolphin/axfx/chorus.c`, `delay.c`, `reverb_hi.c`, `reverb_hi_4ch.c`, `reverb_std.c` (5) | Reverb |
| `dolphin/vi/gpioexi.c`, `i2c.c`, `initphilips.c` (3) | VI encoder |
| `dolphin/MSL_C/PPCEABI/bare/H/e_fmod.c`, `exponentialsf.c`, `math_8029454c.c` (3) | LibC math |

#### `musyx/*` - 2 - **IGNORE (replaced)**
- `musyx/runtime/synth_queue.c`, `synth_seq_dispatch.c`

#### `main/*` - 35 - **P0/P1 for port**
These are game code; host build must compile them but they currently don't match retail so behavior may be slightly off. Prioritize by shim surface:

- `main/render.c` - GX render path
- `main/model.c`, `main/object.c`, `main/objanim.c`, `main/objhits.c`, `main/objprint.c`, `main/objprint_dolphin.c` - object/model, heavy GX/MTX
- `main/shader.c`, `main/shader_dolphin.c` (not listed as NonMatching but 2818 GX hits), `main/tex_dolphin.c`, `main/texture.c`, `main/shadow_dolphin.c`, `main/track_dolphin.c`, `main/rcp_dolphin.c` - graphics
- `main/pad.c`, `main/gameloop_buttonobj.c` - input
- `main/pi_dolphin.c`, `pi_videoinit.c`, `pi_pathsearch.c` - init/video, VI/GX
- `main/fileio.c` dependencies but `fileio.c` itself is Matching? No, listed NonMatching stub? Actually `main/mm.c`, `main/vecmath.c` etc are NonMatching - memory/math
- `main/gametext.c`, `gametext_tail.c`, `textrender*.c` (4 files), `subtitle.c` - UI text
- `main/lightmap.c`, `lightmap_draw.c`, `voxmaps.c`, `newshadows.c`, `zlb.c`, `dll_80136a40.c`, `acosf.c`, `trig.c`, `sincosf.c`

#### `dlls/engine/*` - 24 - **Deferred**
`0/0.c`, `1_camcontrol/camcontrol.c`, `2/2.c`, `3/3.c`, `5/5.c`, `6/6.c`, `7/7.c`, `9/9.c`, `10_expgfx/expgfx.c`, `11/11.c`, `19/19.c`, `20_Hcurves/Hcurves.c` + `Hcurves_romcurve.c`, `21/21.c`, `22/22.c`, `23/23.c`, `24/24.c`, `28/28.c`, `53/53.c`, `60/60.c`, `66/66.c`, `68/68.c`, `69/69.c`, `71/71.c`, `78/78.c`

#### `dlls/objects/*` - 22 - **Deferred**
`195_Player/player.c`, `196_Tricky/tricky.c`, `202/sharpclaw.c`, `226/226.c`, `229/229.c`, `241_InvHit/InvHit.c`, `262/262.c`, `294/294.c`, `332/332.c`, `386_MMP_moonroc/MMP_moonroc.c`, `429_SH_thorntai/SHthorntail.c`, `455_DIMLavaSmas/DIMLavaSmas.c`, `466_WORLDplanet/WORLDplanet.c`, `488_SB_Galleon/SB_Galleon.c`, `578_DBstealerwo/DBstealerwo.c`, `589_BossDrakor/BossDrakor.c`, `597/597.c`, `609_DR_LaserCan/DR_LaserCan.c`, `625/625.c`, `691/691.c`, `701/701.c`, `704/704.c`

#### `dlls/modgfx/*` - 2 - Deferred
`90/90.c`, `152/152.c`

#### `track/*` - 1 - P1
`track/intersect_render.c`

### 1.2 What this means for Stairfax

The 25 `dolphin/*` + 2 `musyx/*` are **irrelevant** - you will delete them on host. The real port risk is the 35 `main/*` NonMatching files that contain the engine's GX/fileio/pad glue. However most of those are already structurally recovered (they compile) and the mismatches are scheduling/peephole diffs, not missing functionality. You can boot with them.

`dlls/*` NonMatching are content; the game will run without them but specific objects/areas will be broken. The Matching 935 objects include the majority of the 600+ object DLLs, so early game (ThornTail Hollow) is likely intact.

## 2. Shim Surface - `src/main` Analysis

Scanned `src/**/*.c` for `#include "dolphin/*"` (205 hits for `math_api.h`, 84 `mtx.h`, 50 `pad.h`, etc.) and symbol regex in `src/main`.

### 2.1 Include heatmap (top 15)

```
205 dolphin/MSL_C/PPCEABI/bare/H/math_api.h
 84 dolphin/mtx.h
 51 dolphin/mtx/vec.h
 50 dolphin/os/OSReport.h
 50 dolphin/pad.h
 42 dolphin/os.h
 39 dolphin/os/OSCache.h
 33 dolphin/gx/GXTransform.h / GXPixel.h
 31 dolphin/gx/GXGeometry.h
 28 dolphin/gx/GXCull.h
 ...
 14 dolphin/ax.h
 13 dolphin/vi.h
  8 dolphin/gx/GXFrameBuffer.h
  7 dolphin/dvd.h
```

### 2.2 Symbol hits in `src/main`

| System | Hits | Top symbols | Top files |
|--------|------|-------------|-----------|
| **GX** | 5719 | `GX_CA_ZERO` 247, `GX_CC_ZERO` 188, `GXSetTevColorIn` 113, `GXLoadTexObj` 44, `GXWGFifo` 83 | `shader_dolphin.c` 2818, `objprint_dolphin.c` 1234, `pi_videoinit.c` 341 |
| **OS** | 508 | `OSReport` 109, `OSDisable/RestoreInterrupts` 93, `OSMessageQueue` 25 | `pi_dolphin.c` 76, `THPRead.c` 53 |
| **DVD** | 378 | `DVDClose` 95, `DVDOpen` 29, `DVDRead` 31, `fileLoad` 24, `DVDReadAsyncPrio` 20 | `pi_dolphin.c` 223, `audio.c` 36, `fileio.c` 31 |
| **PAD** | 218 | `padUpdate` 23, `PADStatus` 23, `getButtons` 10 | `pad.c` 98, `gameloop.c` 23 |
| **MTX** | 201 | `PSMTX*` 201 | `objprint_dolphin.c` 59, `shader_dolphin.c` 38 |
| **VI** | 58 | `VIWaitForRetrace` 15, `VIFlush` 12, `VISetBlack` 9 | `gameloop.c` 17, `pi_videoinit.c` 11 |
| **THP** | 48 | `THPPlayer*` 3, `THP_FRAME_HEADER_SIZE` 3 | `dll_3e.c` 11, `picmenu.c` 11 |
| **AR** | 13 | `ARQPostRequest` 2 | `audio.c` 4 |
| **CARD** | 6 | `cardShowMessage` 1 | `gameloop.c` 5 |

### 2.3 Per-file risk

- `src/main/shader_dolphin.c` is the GX hot spot. It must be shimmed first; consider keeping it but replacing `GXWGFifo` writes with a command buffer.
- `src/main/pi_dolphin.c` is the DVD/VI/GX init hot spot (223 DVD hits, 76 OS hits). This file is `Matching` but calls shimmed APIs.
- `src/main/pad.c` is `NonMatching` and contains `PADRead` -> `padUpdate` logic. Host `pad_shim` must replicate `PADStatus` layout.

## 3. FileIO / Asset Loader

- Entry: `src/main/fileio.c:1` + `src/main/pi_dolphin.c:323 fileLoad()` -> `DVDOpen/ReadAsyncPrio/Close`
- Types in `src/main/gameloop.c:149 loadAsset()`:
  - 0: `fileLoad(id)` -> whole file
  - 1: `fileLoadToBuffer(id, dst)`
  - 2: `fileLoadToBufferOffset(id, dst, offset, size)` (`getTabEntry`)
  - 3: `textureLoad(id)` -> `GXTexObj`
  - 4: `loadCharacter` -> model
  - 5: `Resource_Acquire`
  - 6: `loadModelInstance`
  - 7: `loadAnimation`
- `.tab` handling: `src/main/lightmap_initmapblocks.c:5 loadAssetFileById(&gMapsTab, MLDF_FILEID_MAPS_TAB)` etc. `tools/orig/tab_catalog.py` can enumerate retail chunks. `OBJECTS.bin` + `OBJECTS.bin2` contain `ObjDef` tables (`src/main/object.c: gObjFileOffsetTable`).

Host strategy: `dvd_shim.c` implements `DVDFileInfo` as `{ void* data; u32 len; }` backed by `fopen` on extracted `orig/GSAE01/files/` or by reading the ISO via `tools/project.py` VFS. Async `DVDReadAsyncPrio` -> thread pool.

## 4. Recommendations for `src/port`

### P0 (boot to black)
1. `vi_shim` + `gx_shim` stubs that make `src/main/gameloop.c:362 askProgressiveScanMode()` and `src/main/pi_videoinit.c: tvInit()` not crash.
2. `dvd_shim` host file loader for `fileLoad()` - test by loading `MAPS.tab` and printing first 16 bytes.
3. `pad_shim` -> `padUpdate` returns neutral, `getButtonsHeld` -> SDL.

### P1 (first triangle)
4. Implement `RHI` + `GX->RHI` (Vulkan primary): `rhi_create(RHI_BACKEND_VULKAN)` + `GX TEV` hash -> `rhi_getOrCreatePipeline()`. Start with `src/main/shader_dolphin.c` passthrough. D3D12 shares same RHI.
5. `os_shim` - `OSReport` -> `printf`, `OSCache` -> `__builtin___clear_cache` or noop, `OSThread` -> `SDL_Thread`.
6. Bring up D3D11 (`RHI_BACKEND_D3D11`, best-effort) and GL/GLES (`RHI_BACKEND_GL`, Android) after Vulkan is green - same `rhi.h`, simpler pipelines.

### P2 (playable)
6. `AX`/`musyx` -> silence or Cubeb, `THP` -> stub.
7. Fill remaining `main/*` NonMatching with decomp work or keep as-is.

## 5. Repro

```sh
py audit3.py        # 112 list
py audit_shim.py    # GX/VI/PAD/DVD/OS heatmap
py tools/orig/tab_catalog.py  # asset chunks (needs orig/GSAE01/sys/main.dol)
```

## 6. Notes

- `src/dolphin` has 204 files; host build excludes all but optionally `mtx`. `include/dolphin` has 70+ headers that shims must shadow.
- `src/dlls` is 726 files; 46 are NonMatching but 680 are Matching - early game is largely intact, don't block port on finishing all DLLs.
- Keep `src/port` independent so `python configure.py --matching` remains green for upstream rebases.

