# Port architecture

How the PC port is organized, and the rules that keep it that way. A port is a designed system,
not a pile of shims — the point of this document is that the organization is **enforced by the
directory layout and the build**, so it holds without relying on anyone remembering it.

## The one idea

The port runs the game's **own recompiled decomp code** (`src/main`, `src/dlls`) compiled
*unmodified*, with a host layer standing in for everything the GameCube provided. Every file we
write for the port is one of exactly four kinds, distinguished by **what it does and when it dies**:

| Role | Directory | Lifecycle | Contract |
|------|-----------|-----------|----------|
| **HAL** | `src/port/src/hal/` | permanent | Reimplements a Dolphin SDK / hardware / OS API that game code calls **by its original name** (`GXBegin`, `OSGetTime`, `DVDRead`, `PADRead`, `mmAlloc`, the RHI backends, on-disc format/inflate/texture decode). Contract: **match the observable behavior** of the thing it replaces. |
| **Bridge** | `src/port/src/bridge/` | permanent | Port-native glue with **new** names that *activates* real game systems and *byte-order / ABI translates* across the GC↔host seam (scene render, camera wire, resource/DLL registry, romlist, save-state, the `bswap*` shims). Contract: **translate faithfully**. |
| **Scaffold** | `src/port/src/scaffold/` | **temporary** | Stubs standing in for a real game system not yet brought up. Contract: **be provisional, and be deleted on a known trigger.** Every file is inventoried in [`SCAFFOLD_MANIFEST.md`](SCAFFOLD_MANIFEST.md). |
| **Platform** | `src/port/src/platform/` | permanent | Host OS window / raw-input layer (`plat_window_win32.c`). |

The real game TUs are **not port code**. They live in `src/main` / `src/dlls`, are compiled with no
changes, and the port only *references* them (via `GAME_ROOT` in the CMakeLists). The two exceptions
— `lightmap_draw.c` (2 lines) and `pi_videoinit.c` (1 line) — carry tiny local edits and **must never
be committed** (see `CLAUDE.md`).

## The decision rule (which directory does a new file go in?)

Ask, in order:

1. **Does game code call it by its original GC/SDK symbol name?** → `hal/`. (It replaces a boundary.)
2. **Is it a stub that a real game TU will later supersede?** → `scaffold/`. (It has a death date.)
3. **Otherwise** — new port-native code that connects/translates real systems → `bridge/`.

Edge calls, resolved and recorded so they stay consistent:

- **`hal/assets.c`** is the generic on-disc *format* reader (ZLB/DIR/texture/map-block) — permanent
  storage HAL, same family as `dvd_shim`/`stfx_inflate`/`tex_decode`. **`bridge/game_assetfile.c`**
  is the game-*schema*-specific MLDF resident-file table (knows which file id needs which
  byte-swapper) — that game knowledge makes it a bridge.
- **`bridge/game_model_support.c`** contains some stubs but is a **bridge**: its core is a permanent
  substitute for `modelEngine.c`, a TU that can *never* be host-compiled (it redefines
  `Resource_Acquire` and would pull ~600 DLLs) — the same "faithful reimpl of an uncompilable TU"
  contract as `mm_shim`. Its residual stubs are noted in the manifest, not the whole file.
- **The `game_boot_*` cluster is all scaffold.** Every one is a "boot milestone" stand-in that a real
  subsystem will replace. The real `game_engine` currently stands on that boot scaffolding; saying so
  plainly beats hiding it behind a `_shims` name that reads as permanent HAL.

## Dependency direction

Dependencies point **toward** permanence. Enforced structurally by the static-library graph in the
CMakeLists:

```
real game TUs ──calls──►  hal/  ◄──uses── bridge/  ◄──fills gaps── scaffold/
     ▲                                      │
     └──────────────activated by───────────┘
```

- **HAL depends on nothing above it** — not on bridge, not on scaffold, not on game TUs. It is the
  floor. (RHI backends → `rhi_common` → the `vi`/`gx`/`dvd`/`assets` shims is the existing clean
  spine; keep it.)
- **Bridge may use HAL and reference real game TUs**, never scaffold.
- **Scaffold may reference anything** — it is the leaf that disappears. Nothing permanent should ever
  grow a dependency *on* a scaffold symbol; when a stub is deleted, only other scaffold or the newly
  arrived real TU should have been calling it.

The litmus test for a healthy bring-up: **deleting a scaffold file breaks only scaffold and the real
TU that replaced it** — never a bridge or HAL file. If a bridge starts depending on a stub, the stub
graduated to a bridge and should move.

## Bringing up a subsystem (the workflow that keeps this organized)

This is the loop the whole port advances by. It mirrors the camera and player bring-ups:

1. **Add the real game TU(s)** to the right `GAME_*` group in the CMakeLists (`GAME_CORE_TUS`,
   `GAME_CAMERA_DLLS`, `GAME_PLAYER_DLLS`, or a new named group). Compile; read the new
   unresolved-symbol / duplicate-symbol surface.
2. **Provide the seam in `bridge/`** — the byte-swap of any BE-on-disc data the TU reads
   (`port/byteswap.h`), the ABI adapter for any cross-mode vtable call (PPC separate FP/GP files vs
   x86 cdecl — watch `(double)` casts), the activation call that wires it into the live loop.
3. **Delete the scaffold it replaces** — remove the file(s) from `PORT_SCAFFOLD_SRCS` and their rows
   from `SCAFFOLD_MANIFEST.md`. Watch for **duplicate-symbol** link errors: those pinpoint exactly
   which stub the real TU supersedes (this is how you find, e.g., that `track_dolphin.c` supersedes
   `hitDetect_calcSweptSphereBounds`).
4. **Build green + verify behavior**, then commit. One subsystem per commit where practical.

Adding a stub is never silent anymore: it lands in `scaffold/`, shows up in the build log as
`src\scaffold\...`, and owes a row in the manifest. That visibility is the whole point.

## Naming

- HAL SDK reimplementations end `_shim.c` (`os_shim`, `mm_shim`, `vi_shim`, …).
- Bridge byte-swap glue is `game_<thing>_swap.c`; bridge wiring is `game_<thing>_wire.c` /
  `game_<thing>.c` / `game_<thing>_support.c`.
- Scaffold is always `game_<thing>_stubs.*` (or the `game_boot_*` boot cluster).
- Static libraries are `stairfax_<subsystem>` / `rhi_<backend>`.

## Authored-but-unwired HAL

`hal/pad_shim.c`, `ar_shim.c`, `card_shim.c`, `thp_shim.c`, `mtx_shim.c` are real HAL boundaries
(pad, audio, memory card, THP video, matrix) that **no live target compiles yet** — they are
authored ahead of need. They are HAL by nature, so they live in `hal/`, but do not assume a bug lives
in them: they are not in any build until their subsystem comes up and they are added to a target.

## See also

- [`SCAFFOLD_MANIFEST.md`](SCAFFOLD_MANIFEST.md) — every scaffold TU and its removal trigger.
- [`anim_playback_RE.md`](anim_playback_RE.md) — animation-playback reverse-engineering notes.
- `CLAUDE.md` (repo root) — decomp-side rules; the never-commit list for the two edited game TUs.
