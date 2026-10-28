# Retail ROM / version / Dinosaur Planet / E3-demo census

Deep-dive over the five verified redump SFA discs (US 1.0 = GSAE01 v0 = decomp target, US 1.1,
EU 1.0, EU 1.1, JP 1.0), the Dinosaur Planet decomp's `baserom.z64`, and the E3-2002 kiosk demo
(§6), hunting evidence usable for file names, TU splits, and identifier names. This complements `docs/orig/` (which mines the
US 1.0 bundle only) and `docs/dp_file_map.md` (DP->SFA file correspondence): everything below is
either cross-version or DP-side evidence those passes could not see.

Method notes, reproducible without the throwaway scripts:
- Filesystems: raw `sys/fst.bin` parsed per disc (`dtk vfs` verifies; fst.txt listings are lossy).
- DOL strings: ASCII runs (len>=5) with file offsets, then every cross-version candidate
  re-verified against the other DOL's raw bytes. The naive set-diff is dominated by a
  **pointer-low-byte artifact**: a literal preceded by a pointer whose low byte is printable
  extracts with a bogus leading character that differs per version (see §3).
- DP ROM: strings swept per region (core / DLLs / MAPINFO / trailer), object table parsed from
  OBJECTS.bin (name field at def+0x5F, 15 chars; dll_id +0x58, controlNo +0x5A, folder +0x6F).

Evidence files in this directory:

| file | contents |
|---|---|
| `fs_size_matrix.tsv` | every file path x exact byte size on all 5 discs |
| `dol_strings_catalogue.tsv` | curated identifier-bearing DOL strings, versions + US10 offset + repo-known status |
| `dol_strings_new_vs_US10.txt` / `dol_strings_US10_only.txt` | byte-verified cross-version string diffs |
| `dp_object_names_vs_manifest.txt` | DP's 1270 ROM object names vs `dll_naming_manifest.md` rows |
| `dp_rom_identifier_strings.txt` | curated DP ROM debug strings bearing original identifiers |
| `dp_official_name_claims.txt` | all 173 "official name" annotations in the DP repo, with provenance |
| `dp_rom_trailer_strings.txt` | the DP ROM stale-debug-trailer strings (see §4) |
| `xver/` | cross-version DOL diff engine (`xdiff.py`, portable: expects sibling `isos/<VER>/sys/main.dol`), per-region listings, per-TU summaries, JSONs (§5) |

## 1. Version identity and lineage

SHA-1 identity against repo configs: `config/GSAE01` = US 1.0 (target), `config/GSAE01_rev1` =
US 1.1, `config/GSAJ01` = JP, **`config/GSAP01` = EU 1.0** (hash c5bb4a7f…),
**`config/GSAP01_rev1` = EU 1.1** (hash c1a6ccdc… matches the Rev 1 EU DOL). EU 1.0 was the
one retail build the project had never looked at before this census; its config landed as a
follow-up (§7 item 7).

Code lineage by patch-set inclusion (§5, the strongest evidence — beats `.text` size ordering,
since PAL builds are bigger merely for PAL code):
**US 1.0 -> JP (batch 0) -> EU 1.0 (batches 0+1 + PAL) -> US 1.1 (0+1+2) -> EU 1.1 (0+1+2 +
EU-only fixes)**. Corroborating string markers: `ERROR: asset index overflow ` (referenced from
`getTableFileEntry`, pi_dolphin.c) exists only in US10+JP10; ` DRAKOR SPEED %f ` first ships in
EU 1.0 and stays. **JP tracks the US 1.0 codebase** (a near-clone: 288 DOL bytes, 23 disc files,
all fonts/textures/gametext plus a UTF-16BE<->SJIS charmap table), and **US 1.1 is a
mid-PAL-localization build**, proven independently by the disc filesystem (§2).

All five discs share a byte-identical apploader (2002/04/10), the same 14-section DOL layout,
entry 0x80003140, `Dolphin OS $Revision: 54` (Jun 5 2002), and DSP build Dec 17 2001. No MWCC
version banner exists in any DOL; per-version compiler drift cannot be read from strings.

## 2. Disc filesystems

Full detail in `fs_size_matrix.tsv`. Highlights:

- **US 1.1's 665 extra FST entries are a PAL localization drop mastered at fs root by
  mistake**: root `Sequences/` (654 five-language text bins), root `Boot/` (5 `.c.new`), and
  three *empty* dirs `BOSSAndross/ Communicator/ GameMaze/`. Byte-identical across
  US11/EU10/EU11 but matching *neither* disc's real `gametext/` (210/654 differ from US11's,
  226/654 from EU10's) — a third, intermediate localization snapshot. Smoking gun: root
  `Boot/English.c.new` differs from `gametext/Boot/English.c.new` in exactly one line,
  `File: D:\binary/Boot/...` vs `File: D:\binary\gametext/Boot/...` — the text converter was
  run with the wrong output root and the batch got mastered.
- **QA bug-repro saves shipped on Rev-1 discs**: `BUG_canon_fall` (US11+EU11) and `Bug4`
  (EU11 only) are raw 24640 B memory-card file images (0x40 GCI header + 3x8192 blocks) — both
  stamped with the **Japanese** game code GSAJ01. Rare QA repro'd bugs on a JP build during the
  Rev-1 cycle; `BUG_canon_fall` presumably reproduces a DIM cannon fall (cf. `dimcannon.romlist`).
- **`gametext/Boot/<Lang>.c.new` is machine-generated C source on every disc** ("Project:
  Starfox Adventures on Dinosuar Planet" [sic], "Copyright 2001 Rare Ltd", built from
  `D:\binary`). Original identifiers not previously in the repo: `GameTextData.h`,
  `characterStruct` (character/xpos/ypos/left/right/top/bottom/width/height/font/texture),
  `gametextStruct` (identifier/numPhrases/window/alignH/alignV/language/phrases),
  `GameTextFonts[]`, `Textures[]`, `Boot_<Lang>_characters[]`/`Boot_<Lang>_texts[]`,
  `TEXT_ERROR_CRITICAL`(825) `TEXT_ERROR_DISC_UNREADABLE`(826) `TEXT_READING_DISC`(827)
  `TEXT_CLOSE_DISC_COVER`(828) `TEXT_INSERT_GAME_DISC`(829) `TEXT_ERROR_WRONG_DISC`(830),
  `LANGUAGE_ENGLISH`..`LANGUAGE_JAPANESE`, typedefs `UTF8`/`UCS4`.
- `modules/dino.str` (all discs) contains exactly `baddies/testmod.plf` — modules were built
  as **`.plf`** out of a **`baddies/`** source dir; `testmod.rel` is the SDK OSLink sample.
- EU-only content exists: `gametext/Sequences/2003{7,8}_{French,German,Italian,Spanish}.bin`.
  NTSC-U and NTSC-J discs carry all six languages including the full Japanese script.
- Stale twins at root on every disc (one of each pair is a dead revision): `CAMACTIO.bin` /
  `CAMACTIONS.bin`, `ENVFXACT.bin`/`ENVFXACTIONS.bin`, `globalma.bin`/`globalmap.bin`, and
  `OBJECTS.bin`/`OBJECTS.bin2` (bin2 adds `CFAnimTreas CFBigTreasu CFExplodeTu CFTreasureC`;
  bin alone has `WC_LandingP WarpCigar WarpGasCyli`). The 8-char names are DKR-era truncation.
- Test content on all discs: `animtest/` (complete map), `kraztest.romlist.zlb`,
  `streams/LV/ttest.adp`, 28 zero-byte stubs (25 map `VOXMAP.bin` + `VOXOBJ.bin` +
  `CACHEFONTSTAB/TEX.bin`). 124 romlists preserve DP-era map names (`goldplains`, `mikelava`,
  `willow`, `duster`, `diamondbay`, `swapstore`, `hightop`...). `FONTS.bin` names ten fonts
  (`FunFont`, `DinoSubtitleFont1/2`, `SubtitleFont`, `DinoMediumFontIn/Out`,
  `CommunicatorFont`, `TeenyFont`, `LargeFont`, `KanjiFont`) — candidates for the
  `GameTextFonts[]` table above. `gametext/` sections: `TaskTexts000`-`024` with 020 missing.
- `DLLS.tab` is 32 B on every disc; the real DLL bodies live in the DOL. `audio/starfox.h.bak`
  (already imported as `include/main/audio/sfx_ids.h`) is byte-identical on all five discs —
  no per-region audio-ID drift.

## 3. DOL strings across five versions

The **complete retail `.c` census is closed at 11 names**, identical in all five DOLs:
`objanim.c objHitReact.c expgfx.c curves.c camcontrol.c textblock.c laser.c n_attractmode.c
DIMBoss.c SHthorntail.c` + SDK `dvdfs.c`. Two corrections landed in `docs/dp_file_map.md`:
retail says **`curves.c`**, not `Hcurves.c` — the 'H' at US10 offset 0x30e6bb is the low byte
(0x48) of the preceding pointer 0x800DD648 and `curves_addCurveDef` references 0x803116BC (the
'c') — and `snd3d*.c` appears in no retail DOL (it came from our own musyx tree). The same
artifact class explains phantom `*KPFAIL...`/`U PUSHPULL...` variants. **Warning:** the
generated xref packets (`src/xref/packets/hcurves.json`, `docs/xref/packets/hcurves.md`,
`src/xref/index.json`) still carry the H-prefixed label; `tools/xref/asset_clusters.py` should
take string starts from NUL boundaries, not printable-run starts, next regeneration.

Real cross-version differences reduce to five items (everything else in a naive diff is the
pointer-low-byte artifact):

1. **` DRAKOR SPEED %f `** — US11/EU only, referenced from the range matching US10's
   `bossdrakor_update`: a debug speed printf added after US 1.0. New to the repo.
2. **`Corner is too far out (%f)`** — US11+EU11 only, dead (unreferenced even there), sits in
   the walkgroup-patch string cluster: a compiled-out corner check in the curves/walkgroup code.
3. **`ERROR: asset index overflow `** — US10+JP10 only (already in repo as
   `sAssetIndexOverflowError`); removed later. Lineage marker.
4. `STARFOX ADVENTURES` dropped from `gMemoryCardBannerAssetNames` on EU; the adjacent
   **`Dinosaur Planet`** banner-comment string survives in ALL five retail versions.
5. ~50 EU-only DE/FR/ES/IT disc-error strings (game-facing, no naming value).

New identifier-bearing strings present in all five DOLs (full rows with offsets and referencing
functions in `dol_strings_catalogue.tsv`): mm-store dump naming fields
`thisStore->handle/size/ptrStore/ptrCurrent`, allocator args `wantsize/largestsize/region/col`
(keep Rare's "suitble" typo), `mmUniqueIdent`, `mmShowInfoFBMemoryStore` (function, not yet in
repo), `UPLINK/DOWNLINK/LEFTLINK/RIGHTLINK overflow=%d` (menu link-slot fields in
src/dlls/engine/60/60.c — the original PICMENU DLL — where `upLink/downLink/leftLink/
rightLink` are already adopted), a Tricky-AI
walkgroup state dump (`targetWalkGroup/trickyWalkGroup/tricky last walkGroup/tricky in patch`),
`MAX_COMM_PRESENT` (constant, via `sideCommandEnable`), `numMatrices` (`<renderOpMatrix>`).

Identical in all five: the gameloop banner `Version 2.8 14/12/98 15.30 L.Schuneman` — the
gameloop core predates even N64 Dinosaur Planet.

## 4. Dinosaur Planet ROM

DP's decomp filenames for `.bin/.tab` came FROM SFA's retail string table, so the ROM
filesystem yields no new file names. What the ROM does yield:

- **OBJECTS.bin with a 15-char name field** (SFA retail truncates at 11): 1270 original object
  names. Versus `dll_naming_manifest.md`: 191 exact matches, **239 rows where DP extends our
  11-char truncation**, and **61 of our 146 RAW/GUESSED rows get direct evidence** (e.g. 0x28F
  `WCPressureS` -> `wcpressureswitc` + the full `WCpressureswitch.c` below; 0x151 `CFScalesGal`
  -> `cfscalesgalleon`; 0x286 `SPShopKeepe` -> `spshopkeeper`). Full table:
  `dp_object_names_vs_manifest.txt`. Caveats: DP's names are themselves 15-char truncated,
  DP-era casing, two years of drift — each import needs SFA-side arbitration.
- **The ROM trailer (~0x3B1F000+) is stale DEBUG-build DLL rodata** — the single best find.
  `__FILE__` paths prove the object-source layout **`objects/<area>/<file>.c`** with FULL
  untruncated names: `objects/wallcity/WCpressureswitch.c`, `objects/wallcity/WClevcontrol.c`,
  `objects/shop/SPshopkeeper.c`, `objects/shop/SPdrape.c`, `objects/dragrock/DRcollpieace.c`
  [sic], `objects/vehicles/BWlog.c` (15 files). Stringized original audio enums:
  `SFX_DR_CaveinRumble1`, `SFX_SHOP_CurtainRustle`, `SFX_BLOCK_MOVE/LOCK/HIT/APPEAR/DISAPPEAR`,
  `MUSIC_WALLEDCITY_MAIN/CLOCK`, `AMSFX_DEFAULT_CORRECT_ACTION`, and `mathRnd` used as
  `SFX_BigSlabHit1+mathRnd(0,2)`. This is the naming pattern for the SFX id space (`starfox.h.bak`
  gives SFA-era `SFX*` defines; the trailer shows how source spelled and used them).
- **~160 DP-only debug strings carry original function names** SFA retail stripped:
  `ObjSetupObject objFreeObject objAddEffectBox objGetNearestType TrackGetHeight trackFreeMap
  trackSet/GetLoaded romdefMove_Set depthSortObjects texFreeTexture maketex createModelInstance
  makeModelAnimation modLoadAnimActual blend_frames mmAlloc rzipUncompress amSeq*/amSfx*/amSnd*
  gplayLoadGame/LoadOptions/AddTime flashLoad/SaveGame addCurveDef startObjSequence/
  endObjSequence/preemptSequenceTime segSetBase` ... (curated: `dp_rom_identifier_strings.txt`).
  ROM-attested DP-era source files: `main/main.c`, `game/amsfx.c`, `track/track.c`,
  `track/intersect.c`, `objects/objects.c`, `objects/objprint.c`, `gameplay.c`, `krystal.c`,
  `newlfx.c`, `curves.c`, `objhits.c`.
- **Provenance of DP's "official name" claims** (all 173 catalogued with sources in
  `dp_official_name_claims.txt`): backed by (a) DP ROM strings, (b) the **SFA Kiosk demo
  `default.dol`** — which DP mines for filenames (`9slcommandmenu.c`, `game/anim.c`,
  `EXPLODER.c`, `attention.c`, `firstperson.c`, `screenoverlay.c`, `objlib.c`), struct fields
  (`Fullbartex/Emptybartex`), parameter names (`placeNum`, `nofeet`) and STUBBED_PRINTF bodies
  retail stubs — or (c) JFG kiosk symbols (the shared Rare `mm*` allocator API). Everything
  else in DP is community guesswork, same rank as our own body-reading.
- MAPINFO.bin holds ~50 display map names + ~50 lowercase internal map names; core .data has a
  dev-username array (dwise, mpenny, kbayliss, ...).

**We do not have the SFA Kiosk demo disc** in the redump set. DP's decomp demonstrates its
`default.dol` is dense with pre-release naming evidence; acquiring it is the highest-leverage
missing material identified by this census.

## 5. Cross-version code diff (TU-boundary evidence)

Alignment-aware diff of all five DOLs (instruction words normalized: branch displacements and
16-bit immediates masked; anchors on unique 8-word windows; every region list reconciles
exactly against the section-size delta). Caveat: immediate-only patches are invisible to this
method. All five DOLs share identical section lists and base addresses;
extab/extabindex/.ctors/.dtors/.rodata are pointer-masked-identical in every pair — Rare never
touched exception data post-US1.0.

- **US1.0 -> US1.1: 139 regions, 57 functions, 36 TUs** (net .text +0x780). Heaviest: Hcurves.c
  `Objfsa_UpdateWalkGroupPatches` (15 regions — the walkgroup-patch rework that also added the
  dead `Corner is too far out (%f)` string), textrender_run.c `gameTextBuildSystemFontAtlas`
  (9), intersect_memcard.c `showMemCardError` (8), plus memcard/save flow (maketex.c),
  JP-language memcard titles, and a long tail of one-function object-DLL fixes
  (BossDrakor, gunpowderBarrel, SidekickBal, Baddie/Tricky, HighTop, wclevelcont, suntemple,
  guardClaw, NW_mammoth, MagicCaveTop, DR_EarthWarrior, DIMSnowHorn1, Timer, shopitem...).
- **Exactly three whole-function insertions, all US1.1**, sizes independently corroborated by
  `config/GSAE01_rev1/symbols.txt`: `fn_8007F918` (0x1B0, maketex.c — memcard title builder;
  its body existed inline in US1.0's `loadMemCardImages` as a 0x180 block deleted in Rev 1, so
  its source is recoverable nearly verbatim from US 1.0), `fn_8012EBBC` (0xFC, engine 0.c),
  `fn_80245FF4` (0x6C, dolphin OSRtc.c, WirelessID/SRAM-ex family).
- **US1.0 -> JP is exactly 288 bytes**: `Obj_FreeObject` +0x20 guard (+warning string),
  `tricky_SeqFn` +0xA0 block, the gunpowderBarrel physics subset of the later US1.1 patch, two
  string edits, tail padding. A GSAJ01 matching build needs source variants for only
  object.c, tricky.c, and 344.c.
- **EU1.0 -> EU1.1: 40 regions, 18 fns, 12 TUs**; no new functions; adds EU-only
  `DoorLock_update` (273.c) and `PressureSwitch_update` (510.c) fixes. US1.1-vs-EU1.1 residue
  (56 regions) is the PAL census: 50/60Hz + progressive (gameloop, pi_videoinit, vi, OSRtc),
  language menus, memcard multi-language, 7 PAL-only functions, EU-only gameplay fixes
  (273, 510, 611 GM_MazeWell, 625, 626 `HighTop_update`, BossDrakor).
- **TU-boundary verdict: zero .text straddles in all five pairwise diffs.** Every patch cluster
  — including multi-function clusters of 3-4 functions — lies wholly inside one current GSAE01
  TU. The single .data anomaly is an alignment *proof*, not a violation: inter-descriptor
  padding at 0x803230BC shrank 0x14->0x4, exactly `-addr mod 0x20` in both builds, pinning the
  358.c TU .data boundary precisely where splits.txt draws it (0x803230C0). The US1.1 memcard
  title strings insert at exactly 0x8030EC00, confirming the maketex.c .data boundary.
- The DOLs are installed under `orig/{GSAE01_rev1,GSAJ01,GSAP01,GSAP01_rev1}/sys/main.dol`
  (orig/GSAE01 untouched); `tools/version_progress.py` projects **1000/1005 coherent units**
  for all four secondary targets with no skip reasons.

Per-TU roll-ups, per-region listings, JSONs, and the diff engine itself are preserved under
`xver/` in this directory. TUs patched in Rev 1 that
are still `NonMatching` for US 1.0 — textrender_run.c, model.c, object.c, pi_dolphin.c,
engine 0.c, Hcurves.c, 53.c, 60.c, tricky.c, BossDrakor.c — are exactly the ones where a
US1.1-side probe build could double-check a proposed US1.0 spelling (a candidate C that
explains BOTH versions' bytes with only the patch delta is much likelier the real source).

## 6. E3-2002 kiosk demo (`zz_StarFox051702_e3.tgc`)

Source: the `G95E01` "Interactive Multi Game Demo Disc" (July 2002 US kiosk rotation), extracted
at `~/Code/kiosk-extract`; the SFA demo is a TGC-embedded disc image readable directly with
`dtk vfs` (`.../files/zz_StarFox051702_e3.tgc`). boot.bin: game code GSAJ01, internal name
`08 2002.05.17 E3_2002_StarFox`; internal build tag in the DOL: **`dino_np_internalUTD_090502_1900`**
(project id `dino_np`, built May 9 2002 19:00). Kiosk-specific 111 KB "Apploader2" (BS2 reboot +
`crc9056.dat` media CRC check). Evidence files: `kiosk/` in this directory.

### 6a. Filesystem — a live dev sync tree burned to disc

2,914 files / 798.9 MiB; 580 demo-only, 1,076 retail-only, 1,433 shared-but-changed. The image
is a PC working tree, not a mastered layout: 4-byte file alignment, a literal Windows
`Copy of swaphol/` working-copy map dir, a captured Unix `ls -l` at root (`out`, owner
**ptossell** — Phil Tossell, DP lead engineer), a 0-byte file named `BIN`, root/map-dir duplicate
mod files, and 69 root `.changed` marker files.

- **`.changed` markers are big-endian Unix timestamps, all Jul 2000 - Jan 3 2001** — DP-era
  incremental-sync stamps frozen 16 months before the burn (`kiosk/changed_files.tsv`).
  **`changed.bat`** at root deletes them and decodes every retail 8.3 stale-twin: ANIMCURV→
  ANIMCURVES, CAMACTIO→CAMACTIONS, DLLSIMPO→DLLSIMPORTTAB, ENVFXACT→ENVFXACTIONS, MUSICACT→
  MUSICACTIONS, OBJEVENT→OBJEVENTS, OBJSEQ2C→OBJSEQ2CURVE, SAVEGAME→SAVEGAMES, WEAPONDA→
  WEAPONDATA — retail's stale-twin mystery (§2) closed: the 8.3 forms are the sync tool's copies.
- **A frozen N64 Dinosaur Planet asset tree is physically on the disc**, matching the `out`
  listing byte-for-byte: STORYBOARD.bin (794,292 B, Mar 1999), TRACKS.bin, LEVELS.bin,
  LEVELNAMES.bin (plaintext DP level names: Yeti Mountain, Warlock Mountain, Force Point 1,
  Discovery Falls, SwapStone Circle...), CAMANIMS.bin, OBJFSAS.bin, TRIGGERS.bin, SFXEVENTS.bin,
  and **INCLUDE.bin with 163 N64 `OBJ_*` object names** (`kiosk/dp_n64_include_bin_objnames.txt`).
  FRONTTEXT.bin is Diddy Kong Racing frontend text — on-disc proof of the DKR→DP lineage.
- **`.sqs` sequence scripts are plain text with original sequence identifiers**
  (`kiosk/sqs_identifiers.txt`): `$krystal_takeoutlantern`, `$sabre_mindread`,
  `$sabre_portalSpell`, `$dim_play_truth_horn`, `$cf_PickUpillusionKey`, `$hologram_blast`...
  (DP protagonist "Sabre" still alive in naming, May 2002). Retail ships NO .sqs; the demo's
  `animtest.sqs` is the only source. `SeqsCat.sqs` (0 B) names the catalogue concept.
- **`mod6.tex`/`mod36.tex` are plaintext build manifests** with original artist texture
  filenames (`$ASSET_MTEX <id> textures\TLwood24.rgb.bin`, `DIMlavawallboss.rgb.bin`,
  `GCSHfirtreek2.rgba.bin`...); `.blk` files are block manifests (`$blockbins\mod6.0`).
- **Demo `musyxbin/starfox/starfox.h`** (Feb 1 2001 DolphinExport, 861 defines) is an EARLIER
  generation of retail's `starfox.h.bak` (Jul 2002, 998): 5 `SNG*` names
  (`SNGdiscovery_falls_theme`, `SNGship_battle`...) + ~850 `SFX*` sample names. 51 `.son` music
  name files (retail dirs empty): `ewt_chase/inside/link/outside`, `test_of_sacrifice`,
  `mammoth_theme`, `ice_race`, `swapstone_circle`... — original music naming.
- **Five retail-empty map dirs are fully populated in the demo**: cloudtreasure, frontend,
  insidegal, linklevel, swapcircle (+ musyxbin/global + musyxbin/starfox) — cut content with
  full data. `dbay`/`dfalls` empty in both (cut before E3). MAPINFO.bin (117 entries) carries
  internal names: "LinkK - Nik Test", "ZNot Used - MikesLava", "Rolling Demo - Just In Case",
  "ZNot Used - BOSS Kamerian D" (`kiosk/mapinfo_names.txt`); `kiosk/mapdir_to_modid.tsv` maps
  every map dir to its modN id.
- **The demo has NO romlists** (all 124 retail `.romlist.zlb` are post-E3; demo MAPS.bin is
  +404 KB — placement data still inside). **Compression transition caught mid-flight**: 55
  `.lzo.bin` beside 104 `.zlb.bin`; retail is 100% ZLB. Demo-only debug font/sprite set
  `DB_PALETTES/DB_SPRITES/DB_SPRTAB/DB_TEXTAB/DB_TEXTURES`; `crap starfox.h4m` (7 MB HVQM4
  video, note the name) and `rebirth.thp` (32 MB).
- The DLL system ships on disc: `DLLS.bin` (2,979,392 B), `DLLSIMPORTTAB.bin` + stale 8.3 twin
  `DLLSIMPO.bin`, `LEMMINGDLLS.tab` (2 KB of 0xFF — an empty table whose name survives),
  `DLLS.tab` 32 B counts 0x5B/0xAE/0xC6 vs retail 0x58/0xAB/0xC3 (three more per bank cut by
  ship). Also a second DOL at `files/default.dol` (3,336,256 B).

### 6b. Demo `sys/main.dol` vs retail US 1.0

Same 13-section DOL, same entry, older SDK (OS Rev 49, Dec 2001 kernel). Demo .text only
58.6 KB smaller — the DLLs were ALREADY external at E3, so retail's DOL is the same engine half
with the DLLs statically linked back in at ship. 4,875 change regions; **395 of 943 .text TUs
are stream-identical** to retail; the rest is four months of iteration. Most-churned TUs =
what Rare worked on May-Sep 2002: player.c (56% churn), 597 SnowBike (ice race), engine/0
gameUI, tricky.c, pi_dolphin.c (map-load rework), sky, Andross.c, intersect_render.c
(`kiosk/dol_us10_kiosk_tu_summary.txt`).

- **Names retail stripped** (all byte-verified absent from retail;
  `kiosk/dol_demo_only_strings.txt`): the **`gplay*` function-name family** for the
  gameplay/save TU (`gplaySaveGame gplayNewGame gplaySetObjGroupStatus gplaySetAct gplayAddTime
  gplaySetPlayerno gplayClearRestartpoint gplayGotoRestartpoint gplayGotoSavegame
  gplaySavepoint` + `MAX_TIMESAVES`) — retail's `SaveGame_*` names in dlls/engine/23 have a
  confirmed original prefix; `GetFreeActiveSound()` + `ActiveSounds[]`; track internals
  `trackIntersect() TrackGetHeight() intersectModLineBuild insertPoint linefunctable MAX_LINES
  MAX_LINEPOINTS PLlist`; `texRestructRefs`; `WpError findBounds` + fields `romdefno
  lastpolyworld`; AI states `DISTRACT_SPOTFINDING/SPOTDISTRACTING/SPOTTOSPOT/SPOTRETURING`
  [sic]; new filename **`mmshrineshrine.c`** + module tags `objControl`/`holeControl`; a heap
  browser, object-free decision trace, map streaming trace (`SMAPNO`), and an SDK DVD
  queue dumper — all debug machinery retail stripped.
- **Post-E3 additions** (`kiosk/dol_retail_only_strings.txt`,
  `kiosk/dol_retail_added_functions.txt`): the FB memory-store subsystem, savegame rework
  (`/savegame/save%d.bin`), errorThreadFunc crash screen, the `%s.romlist.zlb` machinery + a
  94-entry map/object name table (the demo streamed maps BY NUMBER — `SMAPNO %d` — retail added
  load-by-name), `renderWhirlpool`, `playerEnterDeepWater`, `objDoTeleportAnim`,
  `trySaveGame`... Use the retail-added list to avoid hunting demo ancestors that don't exist.
- **Split-relevant link-order fact**: the retail .text-tail TU group {598_DIMSnowHorn,
  473_DIM2PrisonM, 599_DR_EarthWar, 600_DR_CloudRun} sits mid-image in the demo, right after
  597_SnowBike; retail re-linked it to the image tail — the youngest object TUs at E3.
- Cut music names in the demo's track table: `asteroids fox_arwing galleon_credits ice_race
  krazoa_doors_open krazoa_tunnel_1/2 starfox_fanfare starfox_theme trex_boss_1 surround_test`
  + 7 more; demo loads `warlock/voxmap.bin` (Krazoa Palace's DP name) and banks from
  `io/starfoxs.sam`.

### 6c. `files/default.dol` — a 2001 DEBUG build, the naming motherlode

The disc carries a SECOND, older DOL as a stray file: 12 sections (no .bss2), CW-runtime
`__start` at 0x80003100, Dolphin OS **Rev 36 (May 2001)**, MetroTRK "for Dolphin v0.5",
**asserts compiled in**. Only ~1.4% of its .text anchors against retail (different build
generation) — its value is names, not drift. This IS the "default.dol" the DP community mines
(all marker strings verified: `9slcommandmenu.c`, `EXPLODER.c`, `attention.c`, `firstperson.c`,
`screenoverlay.c`, `objlib.c`, `game/anim.c`, `Fullbartex`/`Emptybartex`, `placeNum`, `nofeet`).
6,202 strings — 4,523 byte-verified absent from retail. Curated:
`kiosk/curated_default_dol.txt`; full lists `kiosk/default_asserts.txt` (404 live assert
strings) and `kiosk/default_new_identifiers.tsv` (1,473 identifiers not in repo symbols/src).

- **404 live asserts carry real source expressions**: `object->stateFlags&OBJ_STATE_ISFROZEN`,
  `obj->objdata->noplacements/lockdata`, `modelInstance->mod->numJoints`,
  `model->numAnims/numVertexAnims/animCacheSize`, `ANIMMAP_SIZE`,
  `shader->attributes&GC_SHADERATTRIBUTE_WATER`, `shader->numMaterialLayers`, and the complete
  map-block struct: `block->GCpolygons/GCtextures/displayLists[i].displayList/.displayListSize/
  polygonGroups/renderStream/transparentRenderStream/waterRenderStream/vertexColours/
  vertexPositions/vertexTexCoords/shaders` bounds-checked against `blockBase/blockEnd`.
- **~300 real TU filenames** in asserts/errors: engine (`objects/objects.c`, `objhits.c`,
  `objprint(_dolphin).c`, `pi_dolphin.c`, `track.c`, `track/intersect.c`, `game/anim.c`,
  `gameplay.c`, `mm_dolphin.c`, `models_dolphin.c`, `SKNControl.c`, `sparsearray.c`,
  `newday.c`, `dino.c`), UI/tools (`9slcommandmenu.c`, `n_options.c`, `n_pausemenu.c`,
  `_Tn_POST.c`, `meter.c`, `listcode.c`, `objedit.c`, `perform_viewer.c`), and 200+ object
  sources (`tricky.c`, `krystal.c`, `sharpClaw.c`, `template.c`, `tchukachuck.c`,
  `deaddino.c`, `DIMboss.c`, `DRcloudrunner.c`, level-prefixed CF/MMP/IM/DIM/DIM2/DF/NW/SH/SC/
  SP/CR/DR/DB/WC/WM/VF/TREX/ARW/LGT families) + SDK + full MusyX set.
- **Function names**: `audioPlaySong/audioModifySong/audioPrepareStream`,
  `piRomLoadLevel/piRomFreeLevel/piRomGetGamNumber/piRomLoadSection(L)/piRomLoadAddr`,
  `piGetMapInfo/piGetModelInfo/piGetTEXTUREInfo`, `objGetMain`, `objDistObj2DefXZ`,
  `startObjSequence`, `preemptSequenceTime`, `texInitTextures`, `makeModelAnimation`,
  `modelLoadOffsetTables`, `salInitAi/Dsp/DspCtrl`, `gplayGetCurrentPlayerLactions/Envactions`.
- Debug frontend: map-select with DP-era names (`Warlock-Mountain`, `SwapStone Circle`,
  `Drakor Dragon Boss`, `VForce_Point/DForce_Point`, 8 `*_Shrine` entries), Object Editor
  (load modes `TRICKYAI CURVES`, `BADDIE CURVE OBJS`), perf viewer, display-list debugger,
  `Load/Save Game to Database`. Cut DP lore text (Krystal/Randorn/Sabre wormhole dialogue),
  `krystal.c: Krystal Has No Carry Object to Drop` (playable Krystal), `TM & ^ 2001 NINTENDO`.

### 6d. Demo DLLS.bin — a LATE-DP N64 fossil that names retail's anonymous DLL slots

`DLLS.bin` (2,979,392 B) contains **MIPS code** in the exact Dinosaur Planet DLL format — a
stale sync-tree leftover that cannot run on GC, but from a **later DP build than the Dec-2000
N64 ROM**: all 796 DP DLLs present in order, zero deletions, exactly 4 insertions (one new
modgfx — explaining modgfx 82→83, which retail still has — and three objects, one an egg-thief).
66/800 DLLs are byte-identical to the DP retail container. Parser + full per-DLL table:
`kiosk/parse_dlls.py`, `kiosk/dll_table_final.txt`; per-DLL strings with owning index and
DP-decomp name: `kiosk/dll_strings_final.txt`, curated `kiosk/identifiers.txt`.

- Format recovered exactly (header `0x20+4*exports`, GOT/reloc terminators FFFFFFFE/FD/FF).
  Bank tabs decoded: DLLS.tab words = [last engine, last modgfx, 0, first objects]; the demo's
  fresh GC tab (0x5B/0xAE/0xC6) = engine 92 / modgfx 83 / objects@198; retail = 89/83/@195;
  `DLLSIMPORTTAB.bin` is the live N64 import table (586 pointers, grown from DP-2000's 518);
  `DLLSIMPO.bin` is a zeroed GC-era fossil; LEMMINGDLLS.tab all-0xFF (unused, name only).
- **The demo GC descriptor table lives in `files/default.dol` @ 0x802e8b60: 764 slots**
  (92/83/23/566) vs retail's 705 (`gResourceDescriptors` @ 0x802C6300). Engine bank FULLY
  mapped demo↔retail with per-slot evidence (`kiosk/demo_retail_engine.txt`): retail's "+3
  engine slots" vs demo decomposes as **retail dropped 14 demo slots** (gametext, subtitles,
  old_mainmenu, old_levelselect, old_selection, swaphollow/`dswaphollow.c`...) **and added 11**
  (a partfx slot + restoring ~10 of DP's cam family).
- **DP-era names for anonymous retail engine slots** via the kidx↔DPid↔demo-slot↔retail-slot
  chain: r5=newday, r8=newstars, **r9=newlfx** (manifest currently proposes "cloudaction" —
  naming contradiction to arbitrate), r11=modgfx-manager, r13=DP16 (playershadow),
  r16=menu_gameplay, r23=gplay, r25=BaddieControl, r46=movelib, r47=pickup, r49=minimap,
  r50=post, r51=rareware, r52=mainmenu (→n_attractmode), r59-r61=old_picmenu/picmenu/frontend,
  r63=scarab, r64=credits, r65=selection, r66=camnormal. Open ambiguities flagged inline
  (d28/d29, d52/d53, d64/d67; cam block not per-slot resolved).
- New source-file names from the MIPS strings + demo attributions: **`Bpcurves.c`** (the
  pre-"Hcurves" curves-DLL name), `modelfx.c` (DP's anonymous engine id 32),
  `PRESSURESWITCH.c`/`USEOBJ.c`/`SEQOBJ.c`, `BWalphaanim.c`, `dswaphollow.c`; demo OBJECTS.bin
  stores names at def+0x58 (`kiosk/objdll_names.txt`).
- Objects bank: 566 demo vs 510 retail slots; **87 demo-only object DLLs by name** (cut
  content: Snaplack, Snipluck, TrickyGuard, CFIllusionClaw, CFForceField, SpellOverlay, Sea,
  icebeam, Trigger* family), 46 retail-only (`kiosk/obj_slot_pairs.txt` /
  `obj_slot_unmatched.txt`; tail-of-bank pairs noisy — demo OBJECTS.bin carries stale defs).
- Since the DP decomp is fully matched, **every kiosk MIPS DLL now has a named, decompiled C
  ancestor addressable per retail slot** — semantic reference for any retail DLL TU.

## 7. Actionable follow-ups

Status as of the Aug 2026 follow-up pass:

1. **RECORDED** — DP truncation-expansion evidence for the 61 RAW/GUESSED + 239 extension
   manifest rows lives in `dll_naming_manifest.md`'s "Dinosaur Planet evidence" appendix;
   per-row adoption stays owner-arbitrated.
2. **PARTIALLY APPLIED** — `TEXT_ERROR_*` ids, gametext struct/field names, `UTF8`/`UCS4`,
   and the mm/track/tricky constants are adopted (see `naming_worklist.md`, "Applied so far");
   `GameTextData.h` as a header name remains open (wide, low priority).
3. OPEN — SFX action-layer naming per the DP trailer pattern awaits the amsfx-layer decompile
   (sample-level `SFX*` ids are already in `include/main/audio/sfx_ids.h`).
4. **DONE** — `tools/xref/asset_clusters.py` trims pointer-glue at NUL/word boundaries;
   packets regenerated (hcurves -> curves; camcontrol packet newly formed).
5. **DONE** — the kiosk disc was acquired and fully mined (§6).
6. Standing note — ` DRAKOR SPEED %f ` (EU1.0+) confirms a speed f32 in BossDrakor state;
   `Corner is too far out (%f)` names a compiled-out walkgroup check.
7. **DONE** — `config/GSAP01` (EU 1.0) exists: config + orig DOL + projected splits
   (1000/1005 coherent units), report 86.31% matched / 646 exact units. The 181-unit exact
   gap vs the EU 1.1 sibling `GSAP01_rev1` is data-side curation (177 fail only on
   matched_data_percent).
8. OPEN — Rev-1 dual-explanation methodology for the patched NonMatching TUs (textrender_run,
   model, object, pi_dolphin, engine 0, Hcurves, 53, 60, tricky, BossDrakor);
   `fn_8007F918`'s US1.0 inline twin makes maketex.c's Rev-1 refactor recoverable verbatim.
9. **WORKLIST DERIVED** — `naming_worklist.md` (53 layout-verified items, evidence-graded,
   risk-flagged); SAFE batches being applied, GATED batches await single-owner passes.
10. **EVIDENCE RECORDED** — DP-chain engine-slot names in the manifest appendix; arbitration
   (incl. r9 newlfx-vs-cloudaction) owner-paced.
11. **DONE** — `demo_frontier_worklist.md`: 8 drift-free sub-100 TUs (Table A), the zlb.c
   4-byte oracle (Table B), and the churn anti-worklist (Table C).
