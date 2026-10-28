# Scaffold manifest

Every temporary stub TU in `src/port/src/scaffold/`, what it stands in for, and the **trigger that
deletes it**. Scaffold is temporary by contract (see [`PORT_ARCHITECTURE.md`](PORT_ARCHITECTURE.md));
this file is where its debt is tracked so no one has to reverse-engineer "which stubs do I remove?"
when a real system comes up.

**Rule:** when you bring up a real subsystem, the linker's **duplicate-symbol** errors point at
exactly the stubs it supersedes. Delete those stubs from `PORT_SCAFFOLD_SRCS` in the CMakeLists *and*
their row here, in the same change.

## Duplicate landmines (delete-on-contact)

These stub symbols are **exact duplicates** of a real game TU. The moment that TU is added to the
build, the link throws `already defined` and you must delete the stub. Known today:

| Stub symbol | Stub file | Real owner (add triggers deletion) |
|-------------|-----------|-------------------------------------|
| `hitDetect_calcSweptSphereBounds` | `game_camcontrol_stubs.c` | `src/main/track_dolphin.c` |
| `trackGetHeight`, `trackGetNearestGroundOffset`, `trackGetIntersect`, `trackGetLineIntersect`, `trackIntersectBroadphase`, `trackInitCollisionBuffers`, `Obj_SetParent` | `game_player_stubs.c` | `src/main/track_dolphin.c` (+ others) |

Full collision-bring-up plan (the biggest scaffold removal pending) is in the `port-collision`
memory note.

## Inventory

| File | Stands in for | Removal trigger | Notes |
|------|---------------|-----------------|-------|
| `game_player_stubs.c` | Flat-floor collision (`trackGetHeight`→constant, `trackGetNearestGroundOffset`, `trackGetIntersect`→flat plane, broadphase no-op) + player data globals | **Collision bring-up**: compile real `src/main/track_dolphin.c` + the map-block geometry bridge; then remove the ground-pin in `bridge/game_player.c` | The big one. Real `trackGetNearestGroundOffset` returns 0-on-hit (stub returns 1) — fix the convention when swapping. |
| `game_camcontrol_stubs.c` | Target-reticle / enemy-feedback / map-streaming cosmetics camcontrol calls on lock-on, + one vertical-bounds helper | Lock-on / targeting UI subsystem comes up (cosmetics); `hitDetect_calcSweptSphereBounds` on collision bring-up | Not on the plain "walk and camera follows" path. Return types match real prototypes exactly (float-stub-as-int → NaN). |
| `game_camera_stubs.c` | Voxel-collision / map-query / viewport bits for `camera.c`+`voxmaps.c`; zeroed output-matrix + vox-data globals | Voxel wall-avoidance / map-query refinement stage | Matrix/perspective math here is **real**; only the collision/query bits are stubbed. Camera works without wall-avoidance initially. |
| `game_object_stubs.c` | `object.c`'s siblings (objhits/objanim/objlib/objprint/player/map/model/math) for empty-list update | As real objects get spawned — replace with the real TUs one at a time | Vertical slice runs `Obj_UpdateAllObjects` over an empty list, so none of these run; they only need to link. |
| `game_objanim_stubs.c` | `ObjHitReact_LoadMoveEntries` (hit-react move table) | Object-DLL hit-react animation subsystem comes up | Just one no-op; the rest of objanim's deps are already real in `model.c`/`object.c`. |
| `game_objprint_stubs.c` | TEV / texture / light / shadow / fuzz shading-stage setup for the model render path | Compile `rcp_dolphin.c` / `shader_dolphin.c` / `modellight.c` / `newshadows.c` for real (shading pass) | Geometry decode + skinning is already **real**; only shading is stubbed. Safe zeroed-dummy returns. |
| `game_sky_stubs.c` | Sky render / lighting deps of engine/5 (sun/moon, environment lighting) | Object / texture / curve subsystems needed for sky rendering | Only the time-of-day path runs today (`skyResetState`/`skyUpdateTimeOfDay`/`getSunPos`); the 5 stubs that run return valid objects. |
| `game_engine_stubs.cpp` | ~169 `init()`/`gameLoop()` leaf symbols; no-op `Resource_Acquire` vtable + zeroed global storage | Catch-all — shrinks continuously as each subsystem's real TUs land | Never fully gone until the whole engine is native. `STAIRFAX_TRACE=1` logs stub calls to see what actually executes. |
| `game_effects_stubs.c` | `intersect_render.c`'s effects tail: ~60 effects data globals (fog / reflection / TEV-state / newshadows / caustic / spirit-vision) + the `newshadows_*` texture getters + `drawHudBox` / `objGetAlphaCompareThreshold` / `selectTextureWithSecondary` / `_textSetColor` | Removed piecewise as the effects / shadow subsystems come up (`newshadows.c`, `shader.c`, `rcp_dolphin.c`) | Link-only: none run on the text path. `_textSetColor` (glyph color) and the `gGxZ*`/`gTev*` GX-state globals ARE on the live text path — give them real behavior when text-render correctness is tackled. |
| `game_boot_gx.c` | Boot-path GX entry points as host no-ops | Header: "replace this file with a GX->RHI translator" — superseded once engine GX init flows through `stairfax_gxdraw` | |
| `game_boot_externs.c` | `pi_dolphin.c` module globals (`timeDelta`, frame timing, DVD pause flag) as standalone storage | When `pi_dolphin.c` (or a real owner TU) is compiled to provide these globals | Defines storage + 2 FIFO/error callback stubs that never fire on host. |
| `game_boot_stubs.c` | PPC MSR/HID0 supervisor regs (no-op) + **real** PS matrix math + flip-ring / PE no-ops | PS matrix helpers → real matrix TU (`mtx`) when brought up; MSR/HID0 are permanent host no-ops | Partially permanent (there is no PPC supervisor state on host). |
| `game_boot_shims.c` | Host `gHostRenderMode` (NTSC 480i 640×480) video config | When the engine's video-init path owns its render mode natively | Mostly benign permanent host config; lowest priority. |
| `game_boot_draw.cpp` | Proof-of-concept primitive submitted through the game's own GX state | Delete when the `game_boot` milestone target is retired | **Only in the `game_boot` test target, not `game_engine`.** Pure milestone artifact. |

## When this file is empty

The port compiles the real engine with no stubs — every boundary is either real game code or
permanent HAL/bridge. That is the finish line for engine bring-up. Until then, every row here is a
known, bounded piece of remaining work.
