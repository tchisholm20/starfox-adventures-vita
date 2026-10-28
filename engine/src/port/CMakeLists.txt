cmake_minimum_required(VERSION 3.20)
project(StairfaxTemperatures C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The game freely stores pointers in u32 fields (e.g. gResourceFileBuffers[id] =
# (u32)mmAlloc(...)), so the port MUST be built 32-bit or every stored pointer is
# silently truncated. Fail loudly rather than miscompile.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Stairfax port must be built 32-bit (x86): the game stores pointers in u32 "
        "fields. Configure with an x86 toolchain (vcvarsall x64_x86) or -A Win32.")
endif()

# ============================================================================
#  Port source taxonomy (see src/port/docs/PORT_ARCHITECTURE.md)
# ============================================================================
# Every port translation unit lives in one of four role directories so the build
# itself documents its role and lifecycle:
#
#   src/hal/       PERMANENT  - reimplements a Dolphin SDK / hardware / OS API
#                              that real game TUs call by their original name
#                              (GX, VI, OS, PAD, DVD, mm, the RHI backends, the
#                              on-disc format/inflate/texture-decode facilities).
#   src/bridge/    PERMANENT  - port-native glue that activates real game systems
#                              and byte-order / ABI-translates across the seam
#                              (scene render, camera wire, resource registry,
#                              romlist, save-state, the byte-swap shims).
#   src/scaffold/  TEMPORARY  - stubs standing in for not-yet-ported game systems.
#                              Each is deleted when its real system comes up; every
#                              one is inventoried in docs/SCAFFOLD_MANIFEST.md.
#   src/platform/  PERMANENT  - host OS window / raw-input layer.
#
# The REAL game TUs (src/main, src/dlls) are NOT port code: they are compiled
# unmodified and only referenced here. GAME_ROOT points at the repo root so those
# references read cleanly.
set(GAME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# --- Backend toggles -------------------------------------------------------
option(STAIRFAX_RHI_VULKAN "Enable Vulkan backend (primary)"                 ON)
option(STAIRFAX_RHI_D3D12  "Enable D3D12 backend (primary on Windows)"        ON)
option(STAIRFAX_RHI_D3D11  "Enable D3D11 backend (best-effort fallback)"      ON)
option(STAIRFAX_RHI_GL     "Enable GL/GLES backend (best-effort, Android)"    ON)
option(STAIRFAX_BUILD_GAME "Compile the game/shim TUs (off until shims land)" OFF)

set(RHI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX)
endif()

# Make MSVC report a correct __cplusplus so headers guarded on it (e.g. the repo's
# types.h nullptr shim, which must stay out of real C++ TUs) behave as written.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
endif()

# --- Host platform layer (window/input glue) -------------------------------
add_library(stairfax_platform STATIC src/platform/plat_window_win32.c)
target_include_directories(stairfax_platform PUBLIC ${RHI_INCLUDE})

# ============================================================================
#  HAL: renderer backends (src/hal/renderer)
# ============================================================================
# Each renderer is its own static library. rhi_common (below) selects among them
# through the vtable; every backend provides rhi_<name>_available/create so the
# selector always links, even for a disabled/stubbed backend.

# D3D11 (real)
add_library(rhi_d3d11 STATIC src/hal/renderer/rhi_d3d11.cpp)
target_include_directories(rhi_d3d11 PUBLIC ${RHI_INCLUDE})
target_link_libraries(rhi_d3d11 PRIVATE d3d11 dxgi d3dcompiler)

# D3D12 (real)
add_library(rhi_d3d12 STATIC src/hal/renderer/rhi_d3d12.cpp)
target_include_directories(rhi_d3d12 PUBLIC ${RHI_INCLUDE})
target_link_libraries(rhi_d3d12 PRIVATE d3d12 dxgi d3dcompiler)

# Vulkan: real if the SDK headers are found, otherwise a link-satisfying stub.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_library(rhi_vulkan STATIC src/hal/renderer/rhi_vulkan.cpp)
    target_compile_definitions(rhi_vulkan PRIVATE STAIRFAX_HAVE_VULKAN=1 VK_USE_PLATFORM_WIN32_KHR=1)
    target_include_directories(rhi_vulkan PRIVATE ${Vulkan_INCLUDE_DIRS})
    message(STATUS "Vulkan SDK found: ${Vulkan_INCLUDE_DIRS} (dynamic loader, no link)")
else()
    add_library(rhi_vulkan STATIC src/hal/renderer/rhi_vulkan.c)
    message(STATUS "Vulkan SDK NOT found - vulkan backend stubbed (install LunarG SDK to enable)")
endif()
target_include_directories(rhi_vulkan PUBLIC ${RHI_INCLUDE})

# GL (stub for now)
add_library(rhi_gl STATIC src/hal/renderer/rhi_gl.c)
target_include_directories(rhi_gl PUBLIC ${RHI_INCLUDE})

# Owns backend selection + vtable dispatch; depends on the backend libs.
add_library(rhi_common STATIC src/hal/renderer/rhi.c)
target_include_directories(rhi_common PUBLIC ${RHI_INCLUDE})
target_link_libraries(rhi_common PUBLIC rhi_d3d11 rhi_d3d12 rhi_vulkan rhi_gl)

# ============================================================================
#  HAL: SDK / hardware / storage shims (src/hal)
# ============================================================================

# VI shim (video interface -> RHI + window)
add_library(stairfax_vi STATIC src/hal/vi_shim.c)
target_include_directories(stairfax_vi PUBLIC ${RHI_INCLUDE})
target_link_libraries(stairfax_vi PUBLIC rhi_common stairfax_platform)

# GX shim (GX -> RHI draw path)
add_library(stairfax_gx STATIC src/hal/gx_shim.c src/hal/tex_decode.c)
target_include_directories(stairfax_gx PUBLIC ${RHI_INCLUDE})
target_link_libraries(stairfax_gx PUBLIC rhi_common)

# DVD/fileio shim (disc reads -> host filesystem) + asset inflate
add_library(stairfax_dvd STATIC src/hal/dvd_shim.c src/hal/stfx_inflate.c)
target_include_directories(stairfax_dvd PUBLIC ${RHI_INCLUDE})

# Asset readers (on-disc format layer: ZLB/DIR, textures, map blocks). Shared by
# the viewer tools; keeps format knowledge in one place. tex_decode.c provides
# gxTexDecode (GX texel -> RGBA8); stairfax_dvd provides inflate + file IO.
add_library(stairfax_assets STATIC src/hal/assets.c src/hal/tex_decode.c)
target_include_directories(stairfax_assets PUBLIC ${RHI_INCLUDE})
target_link_libraries(stairfax_assets PUBLIC stairfax_dvd)

# Include layout + defines for compiling GAME code (and shims that shadow dolphin
# headers). Order is deliberate:
#   1. src/port/include  - host shadows (dolphin/os.h etc.) win first
#   2. MSVC system dirs  - so <string.h>/<stddef.h>/<stdlib.h> resolve to the real
#                          libc, NOT the repo's freestanding stubs (meant for MWCC)
#   3. repo include/     - provides dolphin/*, main/*, types.h (non-libc)
#   4. repo root
# Without (2) the repo's freestanding libc headers shadow the system ones and break
# size_t / malloc declarations. This only affects the host build; MWCC is untouched.
set(MSVC_SYS_INCLUDES "")
if(MSVC AND DEFINED ENV{INCLUDE})
    file(TO_CMAKE_PATH "$ENV{INCLUDE}" MSVC_SYS_INCLUDES)
endif()
set(GAME_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include        # src/port/include - shadows dolphin/*
    ${MSVC_SYS_INCLUDES}                         # real libc before repo stubs
    ${GAME_ROOT}/include                         # repo include/
    ${GAME_ROOT}                                 # repo root
)
set(GAME_DEFS TARGET_PC=1 STAIRFAX_PORT=1 _CRT_SECURE_NO_WARNINGS=1)

# OS shim (Dolphin OS API)
add_library(stairfax_os STATIC src/hal/os_shim.c)
target_include_directories(stairfax_os PUBLIC ${GAME_INCLUDES})
target_compile_definitions(stairfax_os PUBLIC ${GAME_DEFS})

# mm shim (malloc-backed memory manager). The real src/main/mm.c can't be compiled
# on host (it aliases named .bss globals as one MmGlobalLayout via
# (MmGlobalLayout*)gMmStoreArray - a GC link-order overlay, like pad.c). This shim
# reimplements mm's observable contract, including faithful deferred-free timing.
# Its own lib so a future clean mm reimpl can drop in.
add_library(stairfax_mm STATIC src/hal/mm_shim.c)
target_include_directories(stairfax_mm PUBLIC ${GAME_INCLUDES})
target_compile_definitions(stairfax_mm PUBLIC ${GAME_DEFS})

# GX draw path (real GX vertex API + write-gather pipe -> RHI). The game submits
# geometry by writing to GXWGFifo; this library implements the real GX signatures
# + a FIFO interpreter. gx_wgpipe.cpp is C++ so the PPCWGPipe write-proxy overloads
# are in effect (see the ppcwgpipe_struct.h host shadow).
add_library(stairfax_gxdraw STATIC src/hal/gx_draw.c src/hal/gx_wgpipe.cpp)
target_include_directories(stairfax_gxdraw PUBLIC ${GAME_INCLUDES})
target_compile_definitions(stairfax_gxdraw PUBLIC ${GAME_DEFS})
target_link_libraries(stairfax_gxdraw PUBLIC rhi_common)

# NOTE: these HAL shims are authored but not yet wired into any target -
# src/hal/pad_shim.c, ar_shim.c, card_shim.c, thp_shim.c, mtx_shim.c. They cover
# boundaries (pad, audio, memory card, THP video, matrix) that no live target
# exercises yet. Add them to a library / target when their subsystem comes up.

# --- First real game TU compiled on host (milestone / regression guard) -----
# OBJECT library: compiles objanim.c through the shadow headers. Not linked into an
# exe here (it references sibling game TUs); compiling clean is the milestone.
add_library(game_objanim OBJECT ${GAME_ROOT}/src/main/objanim.c)
target_include_directories(game_objanim PRIVATE ${GAME_INCLUDES})
target_compile_definitions(game_objanim PRIVATE ${GAME_DEFS})

# --- Real text subsystem TUs (gametext + textrender cluster) ----------------
# The real gameText path, wired into game_engine below (replacing the text no-ops
# in scaffold/game_engine_stubs.cpp). All compile clean against the shadow headers.
# See the text bring-up plan in memory (port-text).
set(GAME_TEXT_TUS
    ${GAME_ROOT}/src/main/textrender.c
    ${GAME_ROOT}/src/main/textrender_run.c
    ${GAME_ROOT}/src/main/textrender_boxtex.c
    ${GAME_ROOT}/src/main/textrender_drawbox.c
    ${GAME_ROOT}/src/main/textrender_gettext.c
    ${GAME_ROOT}/src/main/gametext.c
    ${GAME_ROOT}/src/main/gametext_measurebyid.c
    ${GAME_ROOT}/src/main/gametext_tail.c
    ${GAME_ROOT}/src/main/subtitle.c)
# The text/2D DRAW primitives (textRenderChar/Setup, drawTexture/ScaledTexture,
# hudDrawRect) live in src/track/intersect_render.c. It submits 2D geometry by storing
# directly into GXWGFifo (the write-gather pipe) 434 times - as C those stores hit the
# raw hardware address 0xCC008000 and crash at runtime. It CANNOT be compiled as C++ (the
# C++ write-proxy path) because the TU has ~100 C-isms (implicit void*->T* and int->enum)
# that C++ rejects, and we compile decomp TUs unmodified. Resolving this needs a WGPIPE
# MMIO trap (a VEH that intercepts stores to 0xCC008000). Until then it links but crashes
# once text render runs. See port-text memory note.
set(GAME_TEXT_DRAW_TUS
    ${GAME_ROOT}/src/track/intersect_render.c)

# ============================================================================
#  Smoketests / viewer tools (src/tools)
# ============================================================================
add_executable(rhi_smoketest src/tools/rhi_smoketest.c)
target_include_directories(rhi_smoketest PRIVATE ${RHI_INCLUDE})
target_link_libraries(rhi_smoketest PRIVATE rhi_common stairfax_platform)

add_executable(vi_smoketest src/tools/vi_smoketest.c)
target_include_directories(vi_smoketest PRIVATE ${RHI_INCLUDE})
target_link_libraries(vi_smoketest PRIVATE stairfax_vi)

add_executable(dvd_smoketest src/tools/dvd_smoketest.c)
target_include_directories(dvd_smoketest PRIVATE ${RHI_INCLUDE})
target_link_libraries(dvd_smoketest PRIVATE stairfax_dvd)

add_executable(disc_ls src/tools/disc_ls.c)
target_include_directories(disc_ls PRIVATE ${RHI_INCLUDE})
target_link_libraries(disc_ls PRIVATE stairfax_dvd)

add_executable(os_mm_smoketest src/tools/os_mm_smoketest.c)
target_include_directories(os_mm_smoketest PRIVATE ${GAME_INCLUDES})
target_compile_definitions(os_mm_smoketest PRIVATE ${GAME_DEFS})
target_link_libraries(os_mm_smoketest PRIVATE stairfax_os stairfax_mm)

add_executable(gx_smoketest src/tools/gx_smoketest.c)
target_include_directories(gx_smoketest PRIVATE ${RHI_INCLUDE})
target_link_libraries(gx_smoketest PRIVATE stairfax_gx stairfax_platform)

add_executable(gx_tex_smoketest src/tools/gx_tex_smoketest.c)
target_include_directories(gx_tex_smoketest PRIVATE ${RHI_INCLUDE})
target_link_libraries(gx_tex_smoketest PRIVATE stairfax_gx stairfax_platform)

add_executable(tex_view src/tools/tex_view.c)
target_include_directories(tex_view PRIVATE ${RHI_INCLUDE})
target_link_libraries(tex_view PRIVATE stairfax_gx stairfax_dvd stairfax_platform)

add_executable(model_view src/tools/model_view.c)
target_include_directories(model_view PRIVATE ${RHI_INCLUDE})
target_link_libraries(model_view PRIVATE stairfax_gx stairfax_dvd stairfax_platform)

# Real map geometry from the ISO -> MapBlockData -> gx_draw.
add_executable(map_view src/tools/map_view.cpp)
target_include_directories(map_view PRIVATE ${GAME_INCLUDES})
target_compile_definitions(map_view PRIVATE ${GAME_DEFS})
target_link_libraries(map_view PRIVATE stairfax_gxdraw stairfax_assets rhi_common stairfax_platform)

add_executable(gx_draw_smoketest src/tools/gx_draw_smoketest.cpp)
target_include_directories(gx_draw_smoketest PRIVATE ${GAME_INCLUDES})
target_compile_definitions(gx_draw_smoketest PRIVATE ${GAME_DEFS})
target_link_libraries(gx_draw_smoketest PRIVATE stairfax_gxdraw rhi_common stairfax_platform)

# Real game drawing TU: lightmap_draw.c rendering through gx_draw. Compiled as C++
# so its GXWGFifo.* stores hit the write-proxy. /Gy + /OPT:REF let the linker drop
# the file's heavy unreferenced functions (sceneDraw, objRender, ...) so only
# lightmapDrawTriangleList + its deps link.
add_executable(lightmap_smoketest
    src/tools/lightmap_smoketest.cpp
    ${GAME_ROOT}/src/main/lightmap_draw.c)
set_source_files_properties(${GAME_ROOT}/src/main/lightmap_draw.c
    PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "/TP;/Gy;/FImain/dll/ppcwgpipe_struct.h")
target_include_directories(lightmap_smoketest PRIVATE ${GAME_INCLUDES})
target_compile_definitions(lightmap_smoketest PRIVATE ${GAME_DEFS})
target_link_libraries(lightmap_smoketest PRIVATE stairfax_gxdraw rhi_common stairfax_platform)
if(MSVC)
    # We call ONLY lightmapDrawTriangleList out of lightmap_draw.c. The file's other
    # functions (sceneDraw, objRender, mapBlockRender*, ...) pull in the whole game
    # and are never invoked here, so leave their externs unresolved rather than stub
    # the entire scene graph. Safe because those code paths never execute in this test.
    target_link_options(lightmap_smoketest PRIVATE /FORCE:UNRESOLVED /OPT:NOREF)
endif()

# ============================================================================
#  game_engine - the real port executable
# ============================================================================
# Assembled from four TU groups (see the taxonomy note at the top of this file).
# Bringing up a new subsystem means: add its real TUs to a GAME_* group, add any
# new bridge glue to PORT_BRIDGE_SRCS, and DELETE the scaffold it replaces from
# PORT_SCAFFOLD_SRCS (and its row in docs/SCAFFOLD_MANIFEST.md).

# --- Real game TUs, compiled unmodified (the point of the port) -------------
set(GAME_CORE_TUS
    ${GAME_ROOT}/src/main/gameloop_main.c     # main() / init() / gameLoop() entry
    ${GAME_ROOT}/src/main/pi_videoinit.c      # videoInit / waitNextFrame (frame timing)
    ${GAME_ROOT}/src/main/object.c            # GameObject spawn / update / render
    ${GAME_ROOT}/src/main/model.c             # ObjModel load / render
    ${GAME_ROOT}/src/main/objanim.c           # joint animation decode
    ${GAME_ROOT}/src/main/objprint_dolphin.c  # object debug print
    ${GAME_ROOT}/src/main/curves.c            # motion curves
    ${GAME_ROOT}/src/main/camera.c            # Camera_GetCurrent / view state
    ${GAME_ROOT}/src/main/voxmaps.c)          # VOXOBJ voxel object maps
set(GAME_CAMERA_DLLS
    ${GAME_ROOT}/src/dlls/engine/1_camcontrol/camcontrol.c  # id 1: mode-dispatch
    ${GAME_ROOT}/src/dlls/engine/66/66.c      # 0x42 Normal (stand + locomotion)
    ${GAME_ROOT}/src/dlls/engine/67/67.c      # 0x43 StaffAnim
    ${GAME_ROOT}/src/dlls/engine/68/68.c      # 0x44 Viewfinder
    ${GAME_ROOT}/src/dlls/engine/69/69.c      # 0x45 Talk
    ${GAME_ROOT}/src/dlls/engine/72/72.c      # 0x48 Static
    ${GAME_ROOT}/src/dlls/engine/73/73.c      # 0x49 Combat
    ${GAME_ROOT}/src/dlls/engine/75/75.c      # 0x4B Climb
    ${GAME_ROOT}/src/dlls/engine/80/80.c      # 0x50 Crawl
    ${GAME_ROOT}/src/dlls/engine/82/82.c)     # 0x52 ForceBehind
set(GAME_PLAYER_DLLS
    ${GAME_ROOT}/src/dlls/engine/5/5.c            # sky / time-of-day
    ${GAME_ROOT}/src/dlls/engine/15/15.c          # player physics
    ${GAME_ROOT}/src/dlls/engine/21/21.c          # curves collision advance
    ${GAME_ROOT}/src/dlls/objects/195_Player/player.c)  # the playable character

# --- HAL sources compiled directly into the exe (not via a static lib) ------
set(PORT_HAL_SRCS
    src/hal/pad_input.c                        # host controller -> game PAD API
    src/hal/wgpipe_trap.c)                      # WGPIPE MMIO trap (auto-arms at CRT init)

# --- Bridge: permanent port-native integration ------------------------------
set(PORT_BRIDGE_SRCS
    src/bridge/game_scene.cpp                  # render from the active game camera + map
    src/bridge/game_camera_wire.c              # activate camcontrol; PPC->x86 vtable ABI fix
    src/bridge/game_player.c                   # player DLL harness / tick
    src/bridge/game_resource.c                 # Resource_Acquire DLL registry
    src/bridge/game_savestate.c                # gamebits / save buffer
    src/bridge/game_assetfile.c                # MLDF resident data-file loader + swap
    src/bridge/game_romlist.c                  # per-map object placements
    src/bridge/game_mapcells.c                 # terrain cell grid -> block placement
    src/bridge/game_model_support.c            # modelEngine.c substitute (uncompilable TU)
    src/bridge/game_model_swap.c               # ModelFileHeader BE->host
    src/bridge/game_objdef_swap.c              # OBJECTS.bin ObjDef BE->host
    src/bridge/game_sky.c)                     # sky DLL vertical-slice harness

# --- Scaffold: TEMPORARY stubs (see docs/SCAFFOLD_MANIFEST.md) ---------------
set(PORT_SCAFFOLD_SRCS
    src/scaffold/game_boot_shims.c             # host render mode + hw globals
    src/scaffold/game_boot_gx.c                # boot-path GX no-ops
    src/scaffold/game_boot_externs.c           # pi_dolphin.c global stand-ins
    src/scaffold/game_boot_stubs.c             # PPC/PS/flip leaf no-ops
    src/scaffold/game_object_stubs.c           # object subsystem gaps
    src/scaffold/game_sky_stubs.c              # sky render (sun/moon/lighting)
    src/scaffold/game_camera_stubs.c           # camera subsystem gaps
    src/scaffold/game_camcontrol_stubs.c       # camcontrol cosmetic deps
    src/scaffold/game_objanim_stubs.c          # objanim gaps
    src/scaffold/game_objprint_stubs.c         # objprint deps
    src/scaffold/game_player_stubs.c           # flat-floor collision (-> real track_dolphin.c)
    src/scaffold/game_effects_stubs.c          # intersect_render.c effects tail (fog/reflections/newshadows)
    src/scaffold/game_engine_stubs.cpp)        # remaining engine leaf deps

add_executable(game_engine EXCLUDE_FROM_ALL
    ${GAME_CORE_TUS}
    ${GAME_CAMERA_DLLS}
    ${GAME_PLAYER_DLLS}
    ${GAME_TEXT_TUS}
    ${GAME_TEXT_DRAW_TUS}
    ${PORT_HAL_SRCS}
    ${PORT_BRIDGE_SRCS}
    ${PORT_SCAFFOLD_SRCS})
target_include_directories(game_engine PRIVATE ${GAME_INCLUDES})
target_compile_definitions(game_engine PRIVATE ${GAME_DEFS})
# mm: the real src/main/mm.c casts (MmGlobalLayout*)gMmStoreArray, reading regions at
# +0x3F00 past a 0x80-byte global - a GC .bss-adjacency overlay (like pad.c) that is not
# host-safe and can't be fixed byte-neutrally. So game_engine uses the faithful mm shim.
target_link_libraries(game_engine PRIVATE stairfax_os stairfax_mm stairfax_vi stairfax_dvd stairfax_assets stairfax_gxdraw rhi_common stairfax_platform)
if(MSVC)
    target_link_options(game_engine PRIVATE /LARGEADDRESSAWARE /MAP)
    # intersect_render.c (and other game TUs) call sprintf via the game's printf.h - a
    # bare decl with no <stdio.h>, so the modern UCRT inline never fires. Link the legacy
    # definition object that exports the real sprintf/vsprintf symbols.
    target_link_libraries(game_engine PRIVATE legacy_stdio_definitions)
endif()

# ============================================================================
#  Standalone smoketests for individual port modules
# ============================================================================
# Host input state-machine smoketest (no hardware needed)
add_executable(pad_smoketest EXCLUDE_FROM_ALL
    src/tools/pad_smoketest.c
    src/hal/pad_input.c)

# Resource/DLL registry smoketest
add_executable(resource_smoketest EXCLUDE_FROM_ALL
    src/tools/resource_smoketest.c
    src/bridge/game_resource.c)

# Endianness facility smoketest (port/byteswap.h)
add_executable(byteswap_smoketest EXCLUDE_FROM_ALL
    src/tools/byteswap_smoketest.c)
target_include_directories(byteswap_smoketest PRIVATE ${GAME_INCLUDES})

# Boot real game code (pi_videoinit.c) against the HAL
add_executable(game_boot
    src/tools/game_boot.c
    src/scaffold/game_boot_shims.c
    src/scaffold/game_boot_gx.c
    src/scaffold/game_boot_externs.c
    src/scaffold/game_boot_stubs.c
    src/scaffold/game_boot_draw.cpp
    ${GAME_ROOT}/src/main/pi_videoinit.c)
target_include_directories(game_boot PRIVATE ${GAME_INCLUDES})
target_compile_definitions(game_boot PRIVATE ${GAME_DEFS})
target_link_libraries(game_boot PRIVATE stairfax_os stairfax_mm stairfax_vi stairfax_dvd stairfax_gxdraw rhi_common stairfax_platform)
# The game maps MEM1 at its real address 0x80000000; a 32-bit process needs
# /LARGEADDRESSAWARE to own that part of the address space on 64-bit Windows.
if(MSVC)
    target_link_options(game_boot PRIVATE /LARGEADDRESSAWARE)
endif()

if(MSVC)
    target_compile_options(rhi_d3d11 PRIVATE /W3)
    target_compile_options(rhi_common PRIVATE /W3)
    target_compile_options(rhi_smoketest PRIVATE /W3)
endif()

message(STATUS "StairfaxTemperatures RHI configured")
message(STATUS "  backends: VK=${STAIRFAX_RHI_VULKAN} D3D12=${STAIRFAX_RHI_D3D12} D3D11=${STAIRFAX_RHI_D3D11} GL=${STAIRFAX_RHI_GL}")
message(STATUS "  target:   rhi_smoketest  (run: rhi_smoketest --gfx d3d11|d3d12|vk)")
