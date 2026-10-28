# Stairfax Temperatures - `src/port` Layer

> Host abstraction for the PC port. This directory is **never** merged into the decomp's `src/main` / `src/dolphin` - it *replaces* Dolphin SDK at link time.

## Philosophy

The SFA decomp (`config/GSAE01/symbols.txt:1`, `configure.py:935` Matching) is treated as **reference source**, not as a GameCube binary to match. `src/port` provides host implementations for every `include/dolphin/*.h` that the game includes, so `src/main` and `src/dlls` can compile natively with MSVC/Clang/GCC.

```
src/main/*.c ──┐
src/dlls/**/*.c ├─► #include "dolphin/gx.h"  ──► src/port/include/port/gx_shim.h (on PC)
                                         └─► src/dolphin/gx/*.c            (on GC, matching build)
```

Build modes:
- `python configure.py --matching && ninja` -> retail `main.dol` (unchanged)
- `cmake -S src/port -B build/port && cmake --build build/port` -> `StairfaxTemperatures.exe` (host)

## Directory Layout

```
src/port/
  README.md                    # this file
  CMakeLists.txt               # host build
  include/port/
    gx_shim.h      # GX -> RHI (dispatches to Vulkan/D3D12/D3D11/GL)
    renderer/
      rhi.h        # Abstract RHI (like Dusklight's RHI)
      rhi_vulkan.h
      rhi_d3d12.h
      rhi_d3d11.h  # best-effort fallback
      rhi_gl.h     # best-effort, required for Android GLES
    vi_shim.h      # VI -> SDL_Window + swapchain (per-backend)
    pad_shim.h     # PAD/SI -> SDL_GameController
    dvd_shim.h     # DVD/fileio -> stdio + extracted disc
    os_shim.h      # OS thread/cache/report -> STL + SDL
    ar_shim.h      # AR/ARQ -> malloc
    card_shim.h    # CARD -> filesystem save
    mtx_shim.h     # PSMTX -> glm/cglm (or keep dolphin/mtx)
    thp_shim.h     # THP -> ffmpeg/pl_mpeg
  src/
    gx_shim.c      # GX->RHI translation (TEV->pipelines)
    renderer/
      rhi.c
      rhi_vulkan.c
      rhi_d3d12.c
      rhi_d3d11.c
      rhi_gl.c
    vi_shim.c
    pad_shim.c
    ...
```

Include order on host: `src/port/include` **before** `include/` so `-I src/port/include` shadows `include/dolphin/gx.h` with a shim that forwards to the real header or reimplements it.

## Audit Snapshot (2026-08-28)

**Decomp progress:** `935/1047` objects Matching (89.3% by object count). See `docs/port/audit.md` for the full `112` NonMatching breakdown.

**Shim surface in `src/main` (from `src/main/*.c`):**
| Subsystem | Hits | Representative symbols | Port strategy | Priority |
|-----------|------|------------------------|---------------|----------|
| GX | 5719 | `GXSetTevColorIn`, `GXLoadTexObj`, `GXWGFifo`, `GXSetVtxAttrFmt` (`src/main/shader_dolphin.c:2818` hits) | `gx_shim` -> `RHI` abstraction. TEV fixed-function emulated via ubershaders (SPIR-V/HLSL cross-compiled). Backends: Vulkan/D3D12 primary, D3D11/GL best-effort (Dusklight model) | P0 |
| OS | 508 | `OSReport`, `OSDisableInterrupts`, `OSMessageQueue`, `OSCache` | `OSReport->log`, `OSCache->noop`, threads->SDL_thread | P0 |
| DVD/fileio | 378 | `DVDOpen/Read/Close`, `fileLoad`, `DVDReadAsyncPrio`, `dvdCheckError` (`src/main/pi_dolphin.c:223`, `src/main/fileio.c:31`) | Replace `src/main/fileio.c` + `src/main/pi_dolphin.c:323 fileLoad()` with host `fopen` + async thread pool | P0 |
| PAD | 218 | `padUpdate`, `PADRead`, `PADControlMotor`, `getButtonsHeld` | SDL_GameController + rumble | P0 |
| MTX | 201 | `PSMTX*` | Keep `src/dolphin/mtx/*.c` or swap to `glm` - ABI compatible, low risk | P1 |
| VI | 58 | `VIWaitForRetrace`, `VIConfigure`, `VISetBlack`, `VISetNextFrameBuffer` (`src/main/gameloop.c:17`, `src/main/pi_videoinit.c:11`) | `vi_shim` owns per-backend swapchain (VkSwapchainKHR / DXGI / GLX). `VIWaitForRetrace->rhi_present()` | P0 |
| THP | 48 | `THPPlayer*`, `THPRead`, `THPVideoDecode` | ffmpeg `pl_mpeg` or stub -> skip cutscenes initially | P2 |
| AR/ARQ | 13 | `ARInit`, `ARQPostRequest` | Map to heap | P1 |
| CARD | 6 | `cardShowMessage`, `CARD*` | Filesystem `%APPDATA%/StairfaxTemperatures/save.bin` | P1 |

**Remaining NonMatching by bucket (112 total):**
- `dolphin/*`: 25 (8x `ax`, 5x `axfx`, 3x `vi`, 3x `MSL_C`, 2x `mcc`, etc.) - **all replaced on PC, ignore**
- `musyx/*`: 2 - replaced
- `main/*`: 35 (`render.c`, `model.c`, `object.c`, `gametext.c`, `shader.c`, `tex_dolphin.c`, etc.) - **P0 to finish or stub**
- `dlls/engine/*`: 24 - gameplay DLLs, can be deferred
- `dlls/objects/*`: 22 (`195_Player`, `196_Tricky`, etc.) - deferred
- `dlls/modgfx/*`: 2
- `track/*`: 1

## Graphics Backends (Dusklight model)

Primary (fully supported):
- **Vulkan** - primary on Windows/Linux/Android. Owns `GX` -> SPIR-V ubershaders (TEV stages in `src/main/shader_dolphin.c:2818` become pipelines).
- **D3D12** - primary on Windows 10+. Same RHI, HLSL 6.x via DXC, DXGI swapchain in `vi_shim`.

Best-effort fallbacks (like `Dusklight`'s `d3d11`/`opengl`):
- **D3D11** - for Win7/8 + older GPUs / handhelds. Feature level 11_0, no mesh shaders. Marked `BEST_EFFORT` - may lack some TEV combos.
- **OpenGL 4.6 / GLES 3.1** - required for Android (GLES) and as fallback for old desktops. Same RHI, GLSL 460. Not performance-targeted; exists so Android doesn't need Vulkan-only.

Selection at runtime (`--gfx vk|d3d12|d3d11|gl` or auto: `vk` > `d3d12` on Win, `vk`/`gles` on Android). All backends share `src/port/include/port/renderer/rhi.h` so `gx_shim.c` never touches `vulkan.h` directly.

Shader pipeline: `GX TEV` -> `rhi_createPipeline()` with hash of TEV state (`GXSetTevColorIn`, `GXSetTevOrder`, etc.). Cross-compile once: HLSL -> SPIR-V (DXC) -> GLSL (spirv-cross) for GL. No runtime GLSL compile on Vulkan/D3D12.

## Build Instructions (host)

```sh
# Prerequisites: CMake 3.20+, SDL2/3, Vulkan SDK, DXC (for D3D12/D3D11), glslang
cmake -S src/port -B build/port -DCMAKE_BUILD_TYPE=Release -DSTAIRFAX_RHI_VULKAN=ON -DSTAIRFAX_RHI_D3D12=ON -DSTAIRFAX_RHI_D3D11=ON -DSTAIRFAX_RHI_GL=ON
cmake --build build/port -j
./build/port/StairfaxTemperatures --gfx vk --disc path/to/SFA.iso
./build/port/StairfaxTemperatures --gfx d3d12 --disc path/to/SFA.iso  # Windows
./build/port/StairfaxTemperatures --gfx gl --disc path/to/SFA.iso     # Android / fallback
```

The host build must:
1. Add `src/port/include` before `include`
2. Compile `src/main/*.c` + `src/dlls/**/*.c` that are Matching with host flags (not MWCC)
3. Exclude `src/dolphin/*` except `src/dolphin/mtx` (optional) - link shims instead
4. Define `STAIRFAX_PORT` to gate `#ifdef __MWERKS__` paths
5. Select RHI backend at CMake (`STAIRFAX_RHI_*`) and at runtime (`--gfx`)

## Naming

`Stairfax Temperatures` keeps the `SFA` acronym. Window title: `Stairfax Temperatures (PC Port)`.

## Next Steps

See `docs/port/audit.md:1` for the full audit. Immediate tasks:
1. Implement `src/port/include/port/renderer/rhi.h` + `gx_shim -> rhi` translation (TEV hash -> pipelines).
2. Stub `VI/GX/DVD/PAD` shims to get `gameloop` `src/main/gameloop.c:59` to boot to black screen via `rhi_present()`.
3. Implement host `fileLoad()` using extracted `orig/GSAE01` layout (no ISO at runtime after extraction).
4. Bring up Vulkan first (validation layers), then D3D12 (same RHI), then D3D11/GL as best-effort (Android GLES).
