# Credits and third-party licenses

## The game code

| Project | Authors | License |
|---|---|---|
| [Star Fox Adventures decompilation](https://github.com/zcanann/SFA-Decomp) | Zachary Canann, Jack Price-Burns, Goal3-14, Rena Kunisaki, Deses and other contributors | CC0 1.0 |
| [PC port branch `pc-port-continued`](https://github.com/Cassieandstuff/SFA-Decomp/tree/pc-port-continued) | Cassieandstuff | CC0 1.0 |

The `engine/` directory is that tree with the Vita port's changes on top. Its
[`LICENSE`](engine/LICENSE) is the decompilation's CC0 dedication. The Vita port's own
code (`vita/`, and the port's additions under `engine/`) is MIT; see [LICENSE](LICENSE).

## Code from other projects inside this repository

| What | Where | From | License |
|---|---|---|---|
| Four precompiled shaders (clear and blit) | `vita/src/gxm_shaders/` | [vitaGL](https://github.com/Rinnegatamante/vitaGL), copied unchanged | LGPL-3.0 |
| Matrix and vector math, controller stick clamping | `engine/src/port/src/hal/sdk_mtx/`, `engine/src/port/src/hal/pad_input.c` | [doldecomp/dolsdk2004](https://github.com/doldecomp/dolsdk2004), the community decompilation of the GameCube SDK | none stated by that project |
| fdlibm math functions | `engine/src/MSL_C/` | Sun Microsystems, via the upstream decompilation | the notice in each file |

## Libraries in the release binary

The Vita build statically links these libraries. They are used unmodified unless noted.
To build the game against your own copy of any of them, follow [BUILDING.md](BUILDING.md):
the full source of the port is in this repository, and
[`third_party/vitagl/build-vitagl.sh`](third_party/vitagl/build-vitagl.sh) fetches and
builds the libraries.

| Library | Authors | License | Source used for the release build |
|---|---|---|---|
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | Rinnegatamante and contributors | LGPL-3.0 | commit `065d046884522c2ce1624e1f70c7e2fdbc54cbb0` **with three patches**, in [`third_party/vitagl/`](third_party/vitagl/): `vitagl-arb-sync.patch`, `vitagl-scissor-dedupe.patch`, `vitagl-vprog-memo.patch`. Built with `NO_DEBUG=1`. The patches are under LGPL-3.0, like vitaGL. |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | Rinnegatamante | LGPL-3.0 | commit `807cfc66b6f1ed438f4c68d0aac1262b4e5ace8a`, unmodified |
| [math-neon](https://github.com/Rinnegatamante/math-neon) (Vita build) | Lachlan Tychsen-Smith; Vita build by Rinnegatamante | LGPL-3.0 | commit `0faab814782c071ff4015527f1ca955ab1ccc470`, unmodified |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | bythos14 | LGPL-2.1 | commit `fb0e9d338525b067f3679ab33571323336493cca`, unmodified |
| [VitaSDK](https://vitasdk.org) newlib, libstdc++ and system stubs | VitaSDK contributors | their own (newlib: BSD-style; libstdc++: GPL-3.0 with the GCC Runtime Library Exception) | the VitaSDK toolchain |

License texts: [`licenses/LGPL-3.0.txt`](licenses/LGPL-3.0.txt) (which builds on
[`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt)) and [`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt).

## Not included, but required on the console

- `libshacccg.suprx`, Sony's shader compiler, which the player supplies at
  `ur0:data/libshacccg.suprx`. Nothing from it is in this repository or the release.

## References

- [Aurora](https://github.com/encounter/aurora) by encounter: a GX implementation for PC.
  It was read to check how GX behaves (for example, which vertex attribute carries the normal,
  and how texture LOD bias and depth range work). No code from it is included.
- [Dolphin](https://dolphin-emu.org): the original game, run in Dolphin, was the reference
  for how scenes should look.
