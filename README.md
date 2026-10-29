# Star Fox Adventures for PS Vita

A native PS Vita port of Star Fox Adventures (GameCube), built on the
[Star Fox Adventures decompilation](https://github.com/zcanann/SFA-Decomp) and its PC
port branch ([`Cassieandstuff/SFA-Decomp@pc-port-continued`](https://github.com/Cassieandstuff/SFA-Decomp/tree/pc-port-continued)),
with a graphics backend written for the Vita's GPU.

**This repository and its downloads contain no game data.** The port runs from a disc
image you make from your own copy of the game. Nothing from the game is included or
downloaded.

Video of it running on a real Vita: https://youtu.be/RpsG4do0GE8

Download the latest build from **[Releases](../../releases)**. The source is in this
repository; [BUILDING.md](BUILDING.md) explains how to build it yourself.

## What you need

- A PS Vita or PS TV on custom firmware (HENkaku / Enso).
- `libshacccg.suprx` at `ur0:data/libshacccg.suprx` (the shader compiler that many
  Vita homebrew ports also need). Without it the game still runs, but shaders that are not
  included in the package draw through a slower fallback.
- A disc image of your own copy of **Star Fox Adventures for GameCube, USA, Rev 1** (game id
  `GSAE01`, disc version 1), as a full, uncompressed `.iso` (about 1.4 GB).

When it starts, the port checks the image and shows a message on screen if it's missing,
unreadable, a different game, or a different version of this one.

## Install

1. Install `StarFoxAdventures.vpk` with VitaShell.
2. Copy your disc image to `ux0:data/stairfax/sfa_rev1.iso` (create the `stairfax` folder if
   it isn't there).
3. Launch the game.

The first launch writes the game's settings files to `ux0:data/stairfax/`. The download
includes 274 pre-compiled shaders inside the vpk.

## What to expect

- Renders at 640x368, upscaled to the screen, capped at 30 fps. Most areas hold a steady
  30; ThornTail Hollow, the busiest area, can dip below it. To run uncapped, delete
  `ux0:data/stairfax/frame_cap.txt` after the first launch.
- The game sets the Vita to its highest standard clock speeds (CPU 444 MHz, GPU 222 MHz).
- **It's meant to be played overclocked.** The video was recorded with the CPU at 500 MHz
  using an overclocking plugin. Without one, the game runs at 444 MHz.
- If an area needs a shader that isn't included, it compiles in the background: the game
  keeps running while it does, and the shader is saved to the memory card for next time.

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

## How it works

The game isn't emulated. The decompiled code is compiled for the Vita's ARM CPU and runs
natively. It still talks to the GameCube's graphics API (GX), so the port includes a
backend that turns those GX calls into GXM, the Vita's own graphics API.

- **Shaders.** The GameCube colours pixels with a configurable fixed-function unit called
  TEV. The backend writes a shader for each TEV setup the game uses, compiles it on the Vita
  and saves it to the memory card. The release includes 274 already-compiled shaders.
- **Two threads.** The game's main loop runs on one CPU core and a separate render thread on
  another core sends the drawing work to the GPU, so the two overlap.
- **Models.** GameCube models are stored as display lists. Each one is converted to Vita
  vertex buffers the first time it's drawn and reused until the game changes it.
- **Animation.** Some animated characters are skinned on the GPU to save CPU time.
- **Game data.** Data the game needs from its own executable is read from your disc image
  at startup, so none of it is in the download. The build checks for it and stops if it
  finds any.

The backend is in [`vita/src`](vita/src) and the GX translation layer is in
[`engine/src/port/src/hal`](engine/src/port/src/hal).

## Credits

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
