# Star Fox Adventures for PS Vita

A native PS Vita port of Star Fox Adventures, built on the Star Fox Adventures
decompilation project (the PC port branch `Cassieandstuff/SFA-Decomp@pc-port-continued`).

**This repository and its downloads contain no game data.** The port runs from a disc
image you make from your own copy of the game. Nothing from the game is included or
downloaded.

Download the latest build from **[Releases](../../releases)**.

## What you need

- A PS Vita or PS TV on custom firmware (HENkaku / Enso).
- `libshacccg.suprx` at `ur0:data/libshacccg.suprx` (the shader compiler most vitaGL
  homebrew already asks for). Without it the game still runs, but shaders not included in
  the package draw through a slower fallback.
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
   `ux0:data/stairfax` and `ux0:data/STFX00001`: settings and 200 pre-compiled shaders).
2. Put your disc image at `ux0:data/stairfax/sfa_rev1.iso`.
3. Install `StarFoxAdventures.vpk` with VitaShell, then launch it.

## What to expect

- Renders at 640x368, upscaled to the screen, capped at 30 fps. Most areas hold a steady
  30; ThornTail Hollow runs around 25 fps. To run uncapped, delete
  `ux0:data/stairfax/frame_cap.txt`.
- The first time an area needs a new shader, it compiles in the background: the game keeps
  running (a few seconds slower) instead of freezing, and the shader is saved for next time.

## Known issues

- The camera can fly away from Fox during some fights.
- Fox and Tricky do not cast the soft round shadow under their feet that the original game
  draws.

## Reporting a problem

Open an issue and attach `ux0:data/stairfax_game.txt` (the game's log) from the session.

---

Star Fox Adventures is a trademark of Nintendo. This is an unofficial fan project, not
affiliated with or endorsed by Nintendo or Rare.
