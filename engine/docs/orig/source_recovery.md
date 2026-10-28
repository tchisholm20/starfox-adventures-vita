# Retail source-recovery notes

This pass is focused on one narrow question: which bundled retail leftovers already give us real source-file or function labels that can be turned into EN decomp progress quickly.

## Tool

- `python tools/orig/source_recovery.py`
  - Starts from retail EN `orig/GSAE01/sys/main.dol` source-tagged strings and direct string xrefs.
  - Resolves those xrefs through the current `config/GSAE01/symbols.txt` so each retail clue lands in a concrete EN function range.
  - Extracts retail-authored context labels from the string itself when present, for example `setBlendMove` from `<objanim.c -- setBlendMove> ...` or `Init` from `<laser.c Init>...`.
  - Cross-references the same source basename against local `reference_projects/rena-tools/sfadebug` split paths and function names, but keeps that debug-side data clearly separate from retail EN evidence.

The important distinction is that the retail string and EN xref are the hard evidence. The debug-side path and function names are only a naming index to help choose the next recovery target.

## Strongest recovery targets

These are the cleanest first-pass source recovery targets because they have:

- a retail source tag in EN `main.dol`
- a direct EN xref into a current `fn_...` range
- a matching debug-side source path with usable function names

### `camcontrol.c`

- retail string: `<camcontrol.c> failed to load triggered camaction actionno %d`
- EN xref: `fn_80102D3C+0x288`
- debug-side path: `dll/CAM/camcontrol.c`
- debug-side names: `camcontrol_initialise`, `camcontrol_release`, `Camera_moveBy`

This is still one of the cleanest file-boundary candidates for camera-control DLL work.

### `curves.c`

- retail string: `curves.c: MAX_ROMCURVES exceeded!!`
- EN xref: `fn_800E556C+0x18`
- debug-side path: `dll/curves.c`
- debug-side names: `curves_clear`, `curves_addCurveDef`, `curves_remove`, `curves_find`, `RomCurve_getById`

This is a high-leverage target because the retail string, romlist work, and debug-side function inventory all point at the same subsystem.

### `objanim.c`

- retail string: `<objanim.c -- setBlendMove> WARNING tried to load anim -1 from cache modno %d`
- retail function label: `setBlendMove`
- EN xrefs:
  - `fn_8002EC4C+0xAC`
  - `fn_8002F334+0x174`
  - `fn_8003042C+0x1DC`
- debug-side path: `main/objanim.c`
- debug-side name: `Object_ObjAnimSetMove`

The new retail label extraction matters here. Even without trusting the debug-side symbol as ground truth, the retail string already says this cluster is about `setBlendMove`.

### `DIMBoss.c`

- retail strings:
  - `<DIMBoss.c> freeing assets for DIMBoss`
  - `<DIMBoss.c> loading assets for DIMTop`
- EN xrefs:
  - `fn_801BD0E8+0x374`
  - `fn_801BD0E8+0x3C4`
- debug-side path: `dll/DIM/DIMboss.c`
- debug-side names: `DIMboss_init`, `DIMboss_update`, `DIMboss_hitDetect`, `DIMboss_render`, `DIMboss_free`

This gives one concrete boss-file anchor with two live EN callsites already tied to asset lifecycle strings.

### `laser.c`

- retail string: `<laser.c Init>No Longer supported`
- retail function label: `Init`
- EN xref: `fn_802096AC+0xC`
- debug-side path: `dll/CF/laser.c`
- debug-side names: `laser_init`, `laser_update`, `laser_hitDetect`, `laser_render`, `laser_free`

This is a good object-DLL interface target because the retail string already gives both a file name and a likely lifecycle function label.

### `SHthorntail.c`

- retail string: `SHthorntail.c`
- EN xref: `fn_801D5764+0x364`
- debug-side path: `dll/SH/SHthorntail.c`
- debug-side names: `SHthorntail_init`, `SHthorntail_update`, `SHthorntail_hitDetect`, `SHthorntail_render`, `SHthorntail_free`

This is still a clean object DLL target with a nearly complete object-style interface already named on the debug side.

## Partial matches still worth chasing

### `expgfx.c`

- retail strings:
  - `expgfx.c: mismatch in add/remove in exptab`
  - `expgfx.c: addToTable usage overflow`
  - `expgfx.c: exptab is FULL`
  - `expgfx.c: invalid tabindex`
- EN xref cluster:
  - `fn_8009B36C+0xE4`
  - `fn_8009B4E0+0xE0`
  - `fn_8009E078+0x74`
  - `fn_8009E078+0xFC`
  - `fn_8009F558+0x374`
  - `fn_8009F558+0x434`
- no exact debug-side split-path match is bundled here
- debug-side symbol hits still expose `expgfx_initialise`, `expgfx_release`, and a named function cluster

The main win here is that the retail warnings already carve out one coherent EN recovery island with several live callsites.

### `objHitReact.c`

- retail string: `objHitReact.c: sphere overflow! %d`
- EN xref: `fn_8003549C+0x140`
- no exact debug-side split-path match is bundled here
- debug-side symbol hit: `objHitReactFn_80089890`

This is enough to justify naming the EN function cluster after `objHitReact` instead of leaving it anonymous.

### `textblock.c`

- retail strings:
  - `<textblock.c Init>No Longer supported`
  - `H<textblock.c Init>No Longer supported`
- retail function label: `Init`
- EN xrefs:
  - `fn_80209624+0xC`
  - `fn_80209650+0xC`
  - `fn_80209680+0xC`
- no direct debug-side path or symbol hit is bundled here

Even without any external debug-side path, this is still actionable because the retail warning already names the file and one likely lifecycle entrypoint.

### `dvdfs.c`

- retail string: `dvdfs.c`
- no direct EN text xref recovered yet
- medium-confidence retail source leak in `main.dol`
- useful as an SDK matching aid when paired with the `dvd.c` / `CRCMain.c` / `BS2Mach.c` apploader names from [source_leaks.md](/C:/Projects/SFA-Decomp/docs/orig/source_leaks.md)

## Practical use

- summary:
  - `python tools/orig/source_recovery.py`
- focus the strongest exact source matches:
  - `python tools/orig/source_recovery.py --search curves camcontrol SHthorntail`
- focus retail function labels:
  - `python tools/orig/source_recovery.py --search setBlendMove Init`
- focus partial-but-usable cases:
  - `python tools/orig/source_recovery.py --search expgfx objHitReact textblock`
- dump a spreadsheet-friendly inventory:
  - `python tools/orig/source_recovery.py --format csv`

## Why this matters

This tool narrows the "which file should I recover next?" question down to targets that already have retail EN evidence behind them. It still does not prove that the debug-side function names map one-to-one to EN v1.0, but it does give grounded source-file names, retail-authored function labels, and concrete EN code anchors that are much better than inventing fresh placeholder file names.
