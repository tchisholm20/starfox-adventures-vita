# Star Fox Adventures for PS Vita

A native PS Vita port of Star Fox Adventures (GameCube), built on the
[Star Fox Adventures decompilation](https://github.com/zcanann/SFA-Decomp) and its PC
port branch ([`Cassieandstuff/SFA-Decomp@pc-port-continued`](https://github.com/Cassieandstuff/SFA-Decomp/tree/pc-port-continued)),
with a graphics backend written for the Vita's GPU.

**This repository and its downloads contain no game data.** The port runs from a disc
image you make from your own copy of the game. Nothing from the game is included or
downloaded.

Download the latest build from **[Releases](../../releases)**. The source is in this
repository; [BUILDING.md](BUILDING.md) explains how to build it yourself.

## What you need

- A PS Vita or PS TV on custom firmware (HENkaku / Enso).
- `libshacccg.suprx` at `ur0:data/libshacccg.suprx` (the shader compiler most Vita
  homebrew already asks for). Without it the game still runs, but shaders that are not
  included in the package draw through a slower fallback.
- Your own copy of **Star Fox Adventures for GameCube, USA, Rev 1** (game id `GSAE01`,
  disc version 1), dumped to a full `.iso` (about 1.4 GB).

## Making the disc image (from your own disc)

- Wii (models that play GameCube discs) + Homebrew Channel: CleanRip.
- GameCube + Swiss: dump the disc to SD.
- Already have a Dolphin image? `.rvz` / `.gcz` / `.ciso`: in Dolphin, right-click the game
  -> Convert File -> ISO. NKit images must be restored to a full ISO.

The port checks the image when it starts. A different game, a different revision, or a
modified or truncated image is refused with a message on screen.

## Install

1. Copy the `ux0` folder from the download to the root of `ux0:` on the Vita (it creates
   `ux0:data/stairfax` and `ux0:data/STFX00001`: settings and 274 pre-compiled shaders).
2. Put your disc image at `ux0:data/stairfax/sfa_rev1.iso`.
3. Install `StarFoxAdventures.vpk` with VitaShell, then launch it.

## What to expect

- Renders at 640x368, upscaled to the screen, capped at 30 fps. Most areas hold a steady
  30; ThornTail Hollow, the busiest area, can dip below it. To run uncapped, delete
  `ux0:data/stairfax/frame_cap.txt`.
- The Vita runs at its highest clock speeds (CPU 444 MHz, GPU 222 MHz) while the game is
  open, as most homebrew ports do.
- The first time an area needs a shader that is not in the package, it compiles in the
  background: the game keeps running instead of freezing, and the shader is saved to the
  memory card for next time.

## Known issues

- The camera can fly away from Fox during some fights.
- Fox and Tricky do not cast the soft round shadow under their feet that the original game
  draws.
- Short pauses when loading a new area or starting some cutscenes; the opening cutscene
  stutters for its first few seconds.
- The boot logos are not shown yet.
- Holding L to recentre the camera can briefly dip the view.

## Reporting a problem

Open an issue and attach `ux0:data/stairfax_game.txt` (the game's log) from the session.

---

## How the graphics work

The GameCube's GPU ("GX", the Flipper chip) and the Vita's (a PowerVR SGX543MP4+, driven
through Sony's GXM API) have almost nothing in common, and this port does not emulate the
GameCube. The decompiled game runs natively on the Vita's ARM CPU and still calls the GX
API; the port answers those calls with a backend that talks to GXM directly, with no
OpenGL layer in between. (Early builds went through vitaGL; the native backend replaced it.)

**The colour combiner becomes shaders.** GX colours a pixel with TEV, a configurable
pipeline of up to 16 stages that mixes textures, vertex colours and constants. GXM has no
such hardware, so every distinct TEV setup the game uses is turned into a generated Cg
shader, compiled on the Vita by the system shader compiler (`libshacccg`) and cached on the
memory card. The release ships 274 pre-compiled programs. A setup the cache has never seen
compiles on a low-priority background thread while a slower CPU path draws in its place
(a GPU-skinned character is skipped until its shader is ready), so a new shader no longer
stops the game: before that change, one play session logged 45
compiles of 1.5 to 8.2 seconds each, every one of them a freeze.

**Two threads, two cores.** The game thread runs the game and records its GX calls into a
ring buffer; a render thread on another CPU core replays them into GXM, so game logic and
GPU submission overlap instead of taking turns. A lint on every build checks that the code each
thread runs touches no mutable state the other owns, and the build refuses to package if it
finds any.

**Display lists are converted once.** GameCube models are drawn from display lists, a
big-endian command format the GPU reads directly. The backend turns each list into Vita
vertex and index buffers the first time it is drawn, and rebuilds it only when the game
writes to or frees that memory.

**Characters skin on the GPU.** Models whose vertices blend between bones (the ThornTail
Hollow creatures, for example) are skinned in a vertex shader instead of on the CPU, and the
game's glow sprites are drawn in batches instead of one call each.

**GameCube tricks, rebuilt.** GX makes some effects, shadows among them, by copying its
framebuffer into a texture; those become render-to-texture passes. Torch flares and
glowing lights hide behind walls by reading back the depth buffer; the backend keeps a
small ring of finished frames' depth buffers so those reads always see a complete frame.

**Even frame pacing.** The display thread waits for the screen's vertical blank so frames
come out at an even 30. The counter it reads wraps around every 18.2 minutes; the first
release candidate froze exactly there, and the fix is covered by a host test that runs the
old and new logic through ten wraps.

**No game data in the binary.** The decompilation's data tables (constants, string pools)
are filled from your disc image at boot. A build gate checks that none of those tables'
retail contents ended up in the finished binary, and refuses to package it otherwise.

The backend lives in [`vita/src`](vita/src) (`rhi_gxm_*.c` for GXM, `gx_thread.c` and
`gx_rt_*.c` for the render thread); the GX translation it builds on is in
[`engine/src/port/src/hal`](engine/src/port/src/hal).

## Credits

This port exists because of other people's work.

- **Star Fox Adventures decompilation**: [zcanann/SFA-Decomp](https://github.com/zcanann/SFA-Decomp),
  by Zachary Canann and Jack Price-Burns, with Goal3-14, Rena Kunisaki, Deses and other
  contributors (CC0).
- **PC port branch** this port is built on:
  [Cassieandstuff/SFA-Decomp `pc-port-continued`](https://github.com/Cassieandstuff/SFA-Decomp/tree/pc-port-continued),
  by Cassieandstuff (CC0).
- **[vitaGL](https://github.com/Rinnegatamante/vitaGL)**, **[vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK)**
  and the Vita build of **[math-neon](https://github.com/Rinnegatamante/math-neon)** by Rinnegatamante
  (math-neon originally by Lachlan Tychsen-Smith), LGPL-3.0.
- **[SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt)** by bythos14, LGPL-2.1.
- **[VitaSDK](https://vitasdk.org)** and its contributors: the toolchain and system libraries.
- **[Aurora](https://github.com/encounter/aurora)** by encounter: the GX implementation this port
  checked GX behaviour against (no code from it is included).
- **[Dolphin](https://dolphin-emu.org)**: used to compare against the original game.

[CREDITS.md](CREDITS.md) lists the third-party licenses and where to get each library's source.

## License

The Vita port's own code is released under the [MIT License](LICENSE). Code that comes from
the decompilation keeps its CC0 dedication, and the third-party libraries keep their own
licenses (see [CREDITS.md](CREDITS.md)).

Star Fox Adventures is a trademark of Nintendo. This is an unofficial fan project, not
affiliated with or endorsed by Nintendo or Rare.
