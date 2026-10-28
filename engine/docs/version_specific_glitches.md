# Version-specific glitches

Likely bugs and fixes found by comparing the game's version-specific code.
These are leads for testing, not reproduced glitches. The code is taken as
written; unfinished ports may still contain inaccuracies.

**Patched in** lists the builds containing the relevant fix. Regional releases
do not form a single revision sequence: JP v1.0 already fixes two EN v1.0 bugs,
while PAL v1.0 has some, but not all, of the revision-1 fixes.

The entries start with the strongest test leads, followed by less certain
findings and presentation changes. Finding IDs are unchanged from the original
audit. Click an ID for its code notes; placement details are in the appendices.

## Glitches and fixes

### [G01](#code-g01) — Actors can keep outdated routes when areas load or unload

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

The game can mistake a changed set of loaded areas for the previous set and
skip rebuilding actor routes. Actors may then refuse a route or navigate using
outdated connections. Revision 1 checks each area's loading state directly.

### [G02](#code-g02) — A zero animation rate can break enemy animation

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

If a script or action supplies a zero animation rate, several enemy routines
calculate an invalid playback speed. Animations or their events may then
advance incorrectly. Revision 1 substitutes a small fallback rate. No specific
enemy or normal gameplay trigger has been identified.

### [G03](#code-g03) — Tricky fails to recover when his target disappears

**Affected:** EN v1.0. **Patched in:** JP v1.0, PAL v1.0, EN rev1, PAL rev1.

If an object Tricky is following disappears during a scripted sequence, he may
stay stuck in his previous action. Later versions reset him to waiting.

### [G04](#code-g04) — Tricky's commands can stay locked after returning to the Queen

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

In Thorntail Hollow, the game can record that Tricky returned to the Queen
without having unlocked his commands. Earlier versions treat the return as
complete and do nothing to recover. Revision 1 checks both flags and lets the
sequence retry. Whether an ordinary save or interrupted sequence can produce
this mismatch still needs testing.

### [G07](#code-g07) — Putting down the ball can corrupt its collision data

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

The generic put-down code treats Tricky's ball like an ordinary carryable object
and overwrites part of its collision data. This could cause erratic movement
or a crash if the ball goes through that path. Revision 1 excludes the ball
from this cleanup.

### [G10](#code-g10) — Volcano Force Point: pressure plate stays pressed after reloading

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

One Tricky-operated pressure plate can become permanently pressed if the area
loads while its saved press flag is set. Later versions allow it to release
normally. Try pressing it, reloading the area, then moving Tricky or removing
the weight. The effect on puzzle progression is still unclear; the appendix
identifies the exact plate.

### [G12](#code-g12) — Krazoa Palace: switch camera can remain active after walking away

**Affected:** EN v1.0, JP v1.0, PAL v1.0, EN rev1. **Patched in:** PAL rev1.

During act 1, the pressure-switch camera only updates while the player is
within 100 units of the switch. Walking away can leave its camera flag set
after the switch releases, or prevent it activating from a distance.
PAL rev1 removes that distance restriction.

### [G18](#code-g18) — Drakor: missiles can receive an invalid launch velocity

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

A particular launch alignment can make the missile's direction calculation
break down, potentially leaving it motionless or disappearing. Later versions
guard against a zero-length direction. This concerns the Drakor fight; a
reliable way to produce that alignment has not been found.

### [G21](#code-g21) — The shop's Fuel Cell display leaks effect memory

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Leaving ThornTail Store while the Fuel Cell display's lightning effects are
active can leave their memory allocated. Repeated visits could accumulate
the leak. Later versions free outstanding effects when the display unloads.
No crash threshold has been measured.

### [G23](#code-g23) — Models and animations can be requested before their tables finish loading

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

The loader can consult model or animation tables before their disc read has
finished. That may request the wrong data or cause a load failure. Later
versions wait for the read to complete. Fast area transitions or delayed disc
reads are plausible triggers.

### [G08](#code-g08) — Barrels can repeatedly play their landing sound

**Affected:** EN v1.0. **Patched in:** JP v1.0, PAL v1.0, EN rev1, PAL rev1.

Tiny bounces or uneven ground can repeatedly trigger a barrel's landing sound.
Later versions allow a short gap in ground contact before treating the next
contact as another landing.

### [G14](#code-g14) — DarkIce Mines: SnowHorn interaction remains available during damage

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

The rideable SnowHorn remains interactable while reacting to a hit. A mount or
interaction request could overlap the damage reaction. Revision 1 disables
interaction until the reaction finishes.

Ice Mountain also uses this controller, but its SnowHorns have different
settings; the same trigger has not been established there.

### [G19](#code-g19) — Staff glow can linger after backing away from a magic cave

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Approaching a magic cave activates a staff effect, but backing away does not
explicitly turn the glow off. Later versions add that cleanup. How long it
lingers depends on what else updates the staff. This code is shared by cave
entrances in Cape Claw, Thorntail Hollow (including its underground area),
Moon Mountain Pass, Volcano Force Point, Walled City, and SnowHorn Wastes.

### [G20](#code-g20) — Timer beeping can continue while the timer is paused or hidden

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

The timer's looping sound keeps playing while the timer is paused or the HUD
is hidden. Later versions stop refreshing the sound in those conditions.
Opening the pause menu alone may not trigger this behavior.

### [G05](#code-g05) — Tricky's ball keeps updating collision while inactive

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

The ball keeps advancing its collision state while inactive or being
repositioned. This may cause an unexpected bounce or position correction when
it becomes active again. Later versions keep collision aligned with its current
position instead.

### [G06](#code-g06) — Throws can start with old ball collision data

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

Throwing the ball does not reset its collision tracking. After carrying it
some distance, the first movement could snap back, hit something incorrectly,
or follow an odd trajectory. Revision 1 resets that tracking at launch.

### [G09](#code-g09) — Barrel explosion collision may be out of date

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

A barrel explosion may use old collision-position data, causing its hits to
disagree with the visible blast. Later versions explicitly request a collision
refresh. Detonating a barrel immediately after moving or throwing it is a
useful test.

### [G11](#code-g11) — DarkIce Mines: Dinosaur Horn interaction can stop responding

**Affected:** EN v1.0, JP v1.0, PAL v1.0, EN rev1. **Patched in:** PAL rev1.

An interaction in upper DarkIce Mines can mark itself complete when the
Dinosaur Horn is used, independently of the surrounding sequence's completion
state. It then stops accepting interaction. PAL rev1 changes both the code
and the placement's completion flag to address this mismatch.

The horn is not consumed by this code. Whether the older behavior blocks
progression still needs testing.

### [G13](#code-g13) — SnowHorn Wastes: mammoths may move incorrectly after a sequence

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

After a scripted reposition, the mammoth can retain collision data from its
old position. A reset was requested but never performed. Later versions carry
out the reset, potentially fixing snapping, sticking, or incorrect first
movement after the sequence.

### [G15](#code-g15) — Dragon Rock: HighTop can miss a scripted transition

**Affected:** EN v1.0, JP v1.0; PAL v1.0 has part of the fix.
**Patched in:** EN rev1, PAL rev1; partial patch in PAL v1.0.

HighTop may remain in his previous movement state when the rescue/escort
sequence advances. Later versions make him respond to the rescue flag while
already active. Revision 1 also adds a sequence event that sets the flag.
The exact timing needed to expose the older behavior remains unclear.

### [G16](#code-g16) — Dragon Rock: missile sequence can remain enabled after HighTop dies

**Affected:** EN v1.0, JP v1.0, PAL v1.0. **Patched in:** EN rev1, PAL rev1.

When HighTop loses his last health point, earlier versions leave a nearby
missile sequence's enable flag set. Revision 1 clears it. The old behavior
could leave the attack running or interfere with a retry; dying during the
escort is the useful comparison.

### [G17](#code-g17) — Dragon Rock: EarthWalker movement is repeatedly reset

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

One EarthWalker action repeatedly clears movement and animation-speed values,
which may make the action stall or behave oddly while riding. Later versions
clear them only when the action begins. The exact visible effect is unresolved.

### [G25](#code-g25) — Paid guards may show the wrong A-button prompt

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

The SnowHorn Wastes BribeClaw and a Cape Claw guard accept payment without
explicitly requesting the Hint icon. Later versions request it when the player
is in range. Interaction still works; the visible difference depends on whether
other HUD code supplies an icon.

### [G26](#code-g26) — CloudRunner Fortress timer switches may lack an idle prompt

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Three timer interactions omit an explicit A-button prompt while ready to use.
Later versions request the prompt when the player approaches and no timer is
running. This is a prompt fix; the timer behavior itself is unchanged.

### [G28](#code-g28) — Memory-card messages can be too closely spaced

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Earlier versions use fixed spacing between messages even when text wraps.
Later versions measure the text height before placing the next message.
The change may mainly accommodate longer translations; no specific EN/JP
dialog has been shown to overlap.

### [G29](#code-g29) — Wrapped pause-menu hints can have cramped spacing

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Successive multi-line hints may be unevenly spaced or overlap. Later versions
round each hint's height up to a whole number of lines before placing the next.
Short, single-line hints may look unchanged.

### [G22](#code-g22) — Chapter selection leaks a loaded save buffer

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

The chapter-start path loads save data and never frees the temporary buffer.
Later versions free it after copying the data. This concerns chapter selection,
whose retail accessibility is uncertain, rather than ordinary save loading.

### [G24](#code-g24) — Possible model-loading cache problem

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Later versions add a cache operation before loading model data into reused
memory. It may prevent old data from being read, but the audit did not establish
whether another part of the loader already prevents that problem. This is a
weak lead without a known visible symptom.

### [G30](#code-g30) — Save repair can perform unnecessary memory-card writes

**Affected:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

When repairing save-image checksums, earlier versions can write save blocks
before handing control to an operation that manages its own writing. Later
versions skip those preliminary writes. Extra delay or another opportunity for
a write error is plausible; save corruption has not been established.

### [G31](#code-g31) — PAL save-file creation can leave options out of date

**Affected:** PAL v1.0. **Patched in:** PAL rev1.

PAL rev1 reloads game options after successfully creating a save file. Without
that step, some options may remain out of date during initial language setup
or a card-retry flow. The affected options and visible consequence are unknown.
The non-PAL versions do not use the same language-setup flow.

### [G27](#code-g27) — Walled City gains an area-name banner

**Absent in:** EN v1.0, JP v1.0. **Patched in:** PAL v1.0, EN rev1, PAL rev1.

Later versions add a roughly five-second "Walled City" title when the area
controller starts. The earlier controller omits that display. This appears to
be a presentation addition; other possible ways of displaying the title were
not ruled out.

## Unresolved findings

### [U01](#code-u01) — The timer-cancellation patch does nothing

**Attempted patch in:** PAL v1.0, EN rev1, PAL rev1. **Successful patch:** none identified.

Later versions try to keep a timer object's countdown and progress flags in
sync when the global timer is stopped. The new condition can never be true,
so the patch does not run. The two timers may still disagree. The existing
[timer audit](timer_controls_and_revisions.md) confirms the faulty comparison
in the checked retail binaries.

### [U02](#code-u02) — Later audio allocation cannot be freed through the normal path

**Introduced in:** PAL v1.0, EN rev1, PAL rev1. **Patched in:** none identified.

Later versions reserve extra space before an audio buffer, for an unknown
reason. The cleanup code then tries to free the wrong address. This looks
like a leak, but the known caller shuts audio down during system reset, so
there may be no lasting gameplay effect. It does not explain an earlier-version
audio glitch. See the [audio allocation audit](audio_voice_allocation_prefix.md).

### [U03](#code-u03) — Japanese disc-status text may use the wrong width for a blank space

**Affected:** EN v1.0 and JP v1.0 on the Japanese system-font path.
**Patched in:** EN rev1's Japanese system-font path.

A missing character mapping makes a full-width blank inherit the preceding
character's width. This could affect spacing on the Japanese wrong-disc screen,
although equal character widths would hide the difference. Normal English-font
startup and PAL's localized setup use different paths.

## Appendix A: code references

Technical notes for readers checking the implementation. Shorthand here and
in the remaining appendices: **E0** = EN v1.0, **J0** = JP v1.0, **P0** = PAL
v1.0, **E1** = EN rev1, **P1** = PAL rev1.

<a id="code-g01"></a>

**G01.** The old check multiplies loaded-map indices. Index 0 forces zero, index 1 has no effect, and different sets can share a product. Revision 1 compares all flag bytes. Reachable map combinations still need checking.

[Hcurves.c:1019](../src/dlls/engine/20_Hcurves/Hcurves.c#L1019).

<a id="code-g02"></a>

**G02.** Four enemy paths divide by `60 * rateScale`. E1/P1 substitute `0.1f` for a zero rate. The relevant sites are at lines 158, 224, 1123, and 2031 in the audited source.

[Baddie.c:148](../src/dlls/objects/201_Baddie/Baddie.c#L148), [Baddie.c:2027](../src/dlls/objects/201_Baddie/Baddie.c#L2027).

<a id="code-g03"></a>

**G03.** Recovery requires `TRICKY_STATE_FLAG_SEQUENCE_KEEP_STATE` and a freed `followObj`. The patch resets command state, selects `TRICKY_MOVE_WALK_WAIT`, and clears movement speeds.

[tricky.c:1912](../src/dlls/objects/196_Tricky/tricky.c#L1912).

<a id="code-g04"></a>

**G04.** `shTricky_init` used to accept `SH_ReturnedToQueen` alone. E1/P1 also require `Tricky_Unlocked_Sidekick_Commands`; otherwise they clear the return bit so the wait-for-return path can recover.

[SH_tricky.c:57](../src/dlls/objects/422_SH_tricky/SH_tricky.c#L57).

<a id="code-g05"></a>

**G05.** Both ball update paths now advance collision only when `hittableLatch == 1`; otherwise they reattach it to the current position.

[SidekickBal.c:308](../src/dlls/objects/245_SidekickBal/SidekickBal.c#L308), [SidekickBal.c:398](../src/dlls/objects/245_SidekickBal/SidekickBal.c#L398).

<a id="code-g06"></a>

**G06.** E1/P1 add `attachObject` after setting launch velocity and previous position in both throw paths. P0 has G05's change but lacks this reset.

[SidekickBal.c:75](../src/dlls/objects/245_SidekickBal/SidekickBal.c#L75), [SidekickBal.c:205](../src/dlls/objects/245_SidekickBal/SidekickBal.c#L205).

<a id="code-g07"></a>

**G07.** The two carryable put-down paths gain an exception for ball ID `0x112`. Their writes to state bytes 5 and 6 overlap the ball collision pointer at offset 4; they may also save an inappropriate position.

[47.c:21](../src/dlls/engine/47/47.c#L21), [47.c:195](../src/dlls/engine/47/47.c#L195), [tricky.c:759](../src/dlls/objects/196_Tricky/tricky.c#L759), [245_SidekickBal.h:20](../include/dlls/objects/245_SidekickBal.h#L20), [curves_collision_state.h:27](../include/main/dll/curves_collision_state.h#L27).

<a id="code-g08"></a>

**G08.** E0 tracks only the previous update's ground contact. Later builds use a three-update `groundGraceFrames` countdown before permitting another landing sound.

[344.c:441](../src/dlls/objects/344/344.c#L441).

<a id="code-g09"></a>

**G09.** Later builds call `ObjHits_MarkObjectPositionDirty` after configuring the blast hit volume. A missed hit depends on collision update order.

[344.c:341](../src/dlls/objects/344/344.c#L341).

<a id="code-g10"></a>

**G10.** The init routine exempts placement `0x41996` from restoring `latched = 1`. Its pressed bit is `0x4F6`, enable bit is `0x9FD`, and `drivesTricky` is set. Stored position: approximately `(-318.25, 282.30, 2314.41)`.

[251.c:355](../src/dlls/objects/251/251.c#L355), [251.h](../include/dlls/objects/251.h).

<a id="code-g11"></a>

**G11.** Placement `0x4B13A` has flags 0, unlock sequence -1, and horn bit `0x1EE` as both trigger and requirement. The old branch sets the unlocked bit and `userData1`, then disables A for that update; it does not clear the horn bit. P1 excludes this placement and changes its unlocked bit from `0x1A` to `0x3EF`. Stored position: approximately `(788.68, -1042.00, 2254.41)`.

[273.c:127](../src/dlls/objects/273/273.c#L127), [gamebit_ids.h:492](../include/main/gamebit_ids.h#L492).

<a id="code-g12"></a>

**G12.** P1 removes the 100-unit proximity condition from the `WM_SwitchCamActive` (`0x905`) update in map slot 11, act 1. The other distance checks remain. Slot 11 / `warlock` is Krazoa Palace.

[510.c:125](../src/dlls/objects/510/510.c#L125).

<a id="code-g13"></a>

**G13.** E0/J0 clear the requested path reset without calling the attach routine. DLL 417's placed actors are the `NW_mammothh/w/g/b` family in `wastes`.

[417.c:166](../src/dlls/objects/417/417.c#L166), [417.c:659](../src/dlls/objects/417/417.c#L659).

<a id="code-g14"></a>

**G14.** E1/P1 disable interaction before `ObjHitReact_Update` can return early, then enable it after the reaction. DLL 598 appears in `snowmines` and `newicemount`; the similarly named DLL-273 lock prop is unrelated.

[DIMSnowHorn.c:1046](../src/dlls/objects/598_DIMSnowHorn/DIMSnowHorn.c#L1046).

<a id="code-g15"></a>

**G15.** P0/E1/P1 add a `0x631` check to active handler 2, selecting state 8, and remove the old motion-event-7 transition. E1/P1 additionally set `0x631` on creator event 10 when its spawn bit is set. The Dragon Rock sequence uses trigger `0x631`, completion bit `0x632` (`DR_RescuedHighTop`), and sequence 11. Cape Claw's HighTop instead starts with variant 1 / state 10.

[626.c:546](../src/dlls/objects/626/626.c#L546), [626.c:493](../src/dlls/objects/626/626.c#L493), [DR_Creator.c:39](../src/dlls/objects/613_DR_Creator/DR_Creator.c#L39), [626.c:672](../src/dlls/objects/626/626.c#L672), [gamebit_ids.h:672](../include/main/gamebit_ids.h#L672).

<a id="code-g16"></a>

**G16.** On death in control mode 2 or 8, E1/P1 clear `0xBF7`. Dragon Rock creator `0x493D3` uses it as `spawnGameBit`, with mode 9 requesting sequence 4 and a callback spawning `DRHomingMis`. The health display uses the air-meter API, but loses points on hits.

[626.c:892](../src/dlls/objects/626/626.c#L892), [DR_Creator.c:123](../src/dlls/objects/613_DR_Creator/DR_Creator.c#L123).

<a id="code-g17"></a>

**G17.** Handler 3 clears `animSpeedA/B/C` and XYZ velocity every update in E0/J0. Later builds restrict that to `moveJustStartedA`, alongside starting move 7 or 8. The separate `moveSpeed` value is not cleared.

[DR_EarthWar.c:216](../src/dlls/objects/599_DR_EarthWar/DR_EarthWar.c#L216).

<a id="code-g18"></a>

**G18.** The launch calculation normalizes a direction after subtracting a projection. The SDK normalization has no zero-vector guard; later game code adds one. Map 44 / `finalboss` is labeled BOSS Drakor.

[BossDrakor.c:308](../src/dlls/objects/589_BossDrakor/BossDrakor.c#L308), [vec.c:53](../src/dolphin/mtx/vec.c#L53).

<a id="code-g19"></a>

**G19.** Later cave-proximity code explicitly disables the staff glow on leaving the approach radius. The rest of the exit cleanup already existed.

[MagicCaveTo.c:184](../src/dlls/objects/287_MagicCaveTo/MagicCaveTo.c#L184).

<a id="code-g20"></a>

**G20.** `gameTimerRun` sets `dt = 0` when paused or HUD-hidden. Later versions guard the loop-sound keepalive with `if (dt)`; volume and pan updates remain.

[modelEngine.c:899](../src/main/modelEngine.c#L899), [modelEngine.c:959](../src/main/modelEngine.c#L959), [timer_controls_and_revisions.md](timer_controls_and_revisions.md).

<a id="code-g21"></a>

**G21.** The new free loop releases non-null `lightningHandles[10]` for raw object ID `0x468` (`SPFuelCell`). Normally the render path frees each effect on expiry; unloading early bypasses that cleanup.

[644.c:232](../src/dlls/objects/644/644.c#L232), [644.c:127](../src/dlls/objects/644/644.c#L127).

<a id="code-g22"></a>

**G22.** The save-select path for chapter values greater than 1 copies `0x6EC` bytes from a loaded save. E0/J0 omit the subsequent buffer free.

[53.c:640](../src/dlls/engine/53/53.c#L640).

<a id="code-g23"></a>

**G23.** `getTableFileEntry` now waits on pending loads for `MODELS.tab` (ID `0x2A`, mask `0xC`) and `ANIMCURV.tab` (ID `0x0E`, mask `0xA0000000`).

[pi_dolphin.c:1273](../src/main/pi_dolphin.c#L1273), [mldf_fileid.h:22](../include/main/mldf_fileid.h#L22).

<a id="code-g24"></a>

**G24.** The added call is `DCInvalidateRange(model, totalSize)`, between allocation and loading/decompression. The full transfer path's cache guarantees were not established.

[model.c:2246](../src/main/model.c#L2246).

<a id="code-g25"></a>

**G25.** The added Hint-icon request runs when `userData1 == 2`, `gameBitD` is clear, and the player is in range. The two `GuardClaw` records use raw ID `0xD8` and DLL 201, which dispatches to the DLL-202 handler. Their completion bits are `0xD83` (SnowHorn Wastes) and `0xE00` (Cape Claw).

[guardclaw.c:163](../src/dlls/objects/202/guardclaw.c#L163), [gamebit_ids.h:933](../include/main/gamebit_ids.h#L933).

<a id="code-g26"></a>

**G26.** The added idle `A_BUTTON_ICON_CONTEXT_B` request applies to raw ID `0x830`, `CFSunTemple`. The same DLL's Walled City object has ID `0x526` and does not enter this branch.

[659.c:157](../src/dlls/objects/659/659.c#L157).

<a id="code-g27"></a>

**G27.** The controller initializes `messageTimer` to 300 nominal frames and displays text 1401 while positive. That text reads Walled City; the constant's TILE_MESSAGE spelling is misleading.

[WCLevelCont.c:739](../src/dlls/objects/653_WCLevelCont/WCLevelCont.c#L739), [WCLevelCont.c:695](../src/dlls/objects/653_WCLevelCont/WCLevelCont.c#L695).

<a id="code-g28"></a>

**G28.** Fixed 24-pixel spacing becomes measured text height, at least one language line-height, plus 5 pixels.

[intersect_memcard.c:532](../src/track/intersect_memcard.c#L532).

<a id="code-g29"></a>

**G29.** The hint loop rounds measured text height up to a full line-height multiple instead of merely taking the greater of glyph bounds and one line-height.

[0.c:5005](../src/dlls/engine/0/0.c#L5005).

<a id="code-g30"></a>

**G30.** During save-image checksum repair, later `saveGame_prepareAndWrite` performs preliminary block writes only when `cb == NULL`. The earlier path can write before invoking a supplied callback.

[maketex.c:238](../src/dlls/engine/2/maketex.c#L238).

<a id="code-g31"></a>

**G31.** P1 adds `loadGameOptions()` after successful `cardCreateSaveFile(1)`. E0/J0/E1 also omit it, but their callers do not expose PAL's language-setup flow.

[n_attractmode.c:338](../src/dlls/engine/52_n_attractmode/n_attractmode.c#L338).

<a id="code-u01"></a>

**U01.** The new condition compares `isGameTimerDisabled() == 1`, but the accessor returns `state & 2`: either 0 or 2. The start/expiry bits and local countdown may therefore remain out of sync with a global stop.

[Timer.c:117](../src/dlls/objects/693_Timer/Timer.c#L117), [modelEngine.c:1033](../src/main/modelEngine.c#L1033), [timer_controls_and_revisions.md](timer_controls_and_revisions.md).

<a id="code-u02"></a>

**U02.** For the 48-voice DSP array, `_audioAlloc` turns a `0x2DC0` request into `0x2EC0` and returns base + `0x100`. `salExitDspCtrl` passes that interior pointer to `mmFree`, which requires an allocation-base match. Allocation failure also yields an unchecked interior-pointer result. The prefix's purpose is unresolved.

[audio.c:702](../src/main/audio.c#L702), [audio_voice_allocation_prefix.md](audio_voice_allocation_prefix.md), [sal_studio.c:130](../src/musyx/runtime/sal_studio.c#L130), [audio.c:698](../src/main/audio.c#L698), [mm.c:594](../src/main/mm.c#L594), [gameloop.c:775](../src/main/gameloop.c#L775).

<a id="code-u03"></a>

**U03.** The old SJIS lookup omits U+3000; E1 maps it to `0x8140`. For an empty input, `OSGetFontWidth` and `OSGetFontTexel` leave the width unchanged, so the blank inherits the previous glyph's width. Its pixels remain blank. This was the only changed mapping among the 85 resident Japanese glyphs.

[gametext.c:621](../src/main/gametext.c#L621), [gametext_data.c:378](../src/main/gametext_data.c#L378), [gametext.c:1659](../src/main/gametext.c#L1659), [gametext.c:767](../src/main/gametext.c#L767), [OSFont.c:336](../src/dolphin/os/OSFont.c#L336).

## Appendix B: placement records

These records identify the objects discussed above. Map labels came from
`MAPINFO.bin` and the DOL's romlist table; object names came from `OBJINDEX.bin`
and `OBJECTS.bin`. The tools used were [map_catalog.py](../tools/orig/map_catalog.py)
and [romlist_params.py](../tools/orig/romlist_params.py).

Offsets below refer to E1's **decompressed** romlists. Use placement identities
when comparing versions, because offsets can move. Parameter offsets are
relative to the [placement record](../include/game/objects/object_setup.h).
Stored coordinates in Appendix A may differ from runtime world coordinates.

| Finding | Romlist | E1 decoded offset | Placement identity / object | Evidence to inspect |
| --- | --- | --- | --- | --- |
| G04 | `hollow` | `0x4E90` | `0x435F2` / `SH_tricky` | DLL 422, raw ID `0x2EE` |
| G10 | `temple` | `0x799C` | `0x41996` / `VFP_PuzzleP` | `+0x1A = 0x4F6`, `+0x1E = 1`, `+0x20 = 0x9FD` |
| G10 | `temple` | `0x797C` | `0x4AC27` / `HitAnimator` | Target bit at `+0x18 = 0x4F6` |
| G10 | `temple` | `0x7A68` | `0x4C35F` / `VFPSeqObj` | Trigger at `+0x1A = 0x9FD`; open bit `0xCF4` |
| G11 | `snowmines` | `0x3FF8` | `0x4B13A` / `DIMUseObjec` | Unlocked bit `+0x1C`; horn bit `0x1EE` at `+0x1E` and `+0x22` |
| G11 | `snowmines` | `0x7914`, `0x796C` | `0x30329`, `0x30386` / `DIMSeqObjec` | Both open bits at `+0x18 = 0x3EF`; triggers `0x1FE`, `0x3F1` |
| G12 | `warlock` | `0x2CC8`, `0x4B6C` | `0x1F1A`, `0x47293` / `WM_Pressure` | DLL 510, raw ID `0x144` |
| G12 | `warlock` | `0x520` | `0x42D45` / `WM_seqpoint` | `+0x1E = 0x905`, the camera/sequence-start bit |
| G15/G16 | `dragrock` | `0x6F40` | `0x3460C` / `DR_HighTop` | DLL 626; variant at `+0x19 = 0` |
| G15 | `dragrock` | `0xA4C` | `0x4555F` / `DR_Seqobj` | Open bit `0x632`, trigger `0x631`, sequence 11, flags `0x08` |
| G16 | `dragrock` | `0xFE0` | `0x493D3` / `DR_Creator` | Spawn-enable bit `+0x18 = 0xBF7`; behavior `+0x1A = 9` |
| G17 | `dragrock` | `0x63F8` | `0x4C117` / `DR_EarthWar` | DLL 599, raw ID `0x416` |
| G21 | `swapstore` | `0x1D4` | `0x45B6F` / `SPFuelCell` | DLL 644, raw ID `0x468`, definition index `0x3D1` |
| G25 | `wastes` | `0x5214` | `0x305D4` / `GuardClaw` | `gameBitD` at `+0x1C = 0xD83` |
| G25 | `capeclaw` | `0x5F4C` | `0x4B939` / `GuardClaw` | `gameBitD` at `+0x1C = 0xE00` |
| G26 | `fortress` | `0x7970`, `0x7998`, `0x79C0` | `0x48186`, `0x4817C`, `0x48188` / `CFSunTemple` | Raw ID `0x830`; activation bits `0xB1B`/`0xB1C`/`0xB1D` |
| G26 | `fortress` | `0x7A0C`, `0x79E8`, `0x7A30` | `0x48187`, `0x4817F`, `0x48189` / `CNTstopwatc` | Timer-enable bits `0xB1B`/`0xB1C`/`0xB1D` respectively |

### Asset differences

The G10 plate, G12 switches and camera point, G16 creator, G21 Fuel Cell,
G25 guards, and G26 interaction records are byte-identical across J0/E1/P0/P1.
These nearby records differ:

| Record | J0 | E1 / P0 | P1 | Interpretation |
| --- | --- | --- | --- | --- |
| G11 `0x4B13A`, `+0x1C` | `0x001A` | `0x001A` | `0x03EF` | Concrete P1 asset correction paired with the code exception; changes which bit makes the horn interaction inactive |
| G10-associated `VFPSeqObj` `0x4C35F`, `+0x20` | `0x0095` | `0x008C` | `0x008C` | Different preempt-sequence ID; could be regional sequence numbering, so not independently labeled a fix |
| G15 `DR_Seqobj` `0x4555F`, header `+0x04` / `+0x06` | `0x04` / `0x6C` | `0x01` / `0xD4` | `0x01` / `0xD4` | Loading metadata differs; its rescue/trigger parameters are unchanged. Runtime impact not established |

G11's two associated sequence objects are unchanged. Their stream offsets are
`0x7804`/`0x785C` in J0/P0 and `0x79B4`/`0x7A0C` in P1.

For G27, entry 1401 in E1/P0/P1's `gametext/WallCity/English.bin` contains
formatting bytes followed by "Walled City". Its definition is at `0xA4C`;
[gameTextFinalizeLoad](../src/main/gametext.c#L1133) describes the table layout.

## Appendix C: other version differences

These branches were reviewed but do not establish additional glitches.

**N01.** **PAL video modes.** PAL50/EURGB60 use different display heights. The viewport differences follow those modes; they do not establish an NTSC viewport bug.

[video_viewport.h](../include/main/video_viewport.h#L6); [game UI](../src/dlls/engine/0/0.c#L4089); [display-mode audit](gameloop_regional_display_mode.md).

**N02.** **DVD-error screen layout.** PAL repositions and dims its display-mode prompt while an error is shown. An overlap test may be useful, but the regional layouts differ.

[gameloop.c](../src/main/gameloop.c#L328); [fileio.c](../src/main/fileio.c#L37).

**N03.** **Regional options and resources.** PAL language choices, defaults, title art, SRAM access, and selected startup dialogs are expected regional differences.

[Options](../src/dlls/engine/55/55.c#L101); [save defaults](../src/dlls/engine/21/21.c#L1451); [language switching](../src/main/gametext.c#L1918); [card dialogs](../src/track/intersect_memcard.c#L503).

**N04.** **Dinosaur-language cheat.** PAL hides the option and omits the fourth maze-well cheat reward. Non-PAL grants it only outside the Japanese system-font path. This appears intentional.

[Maze well](../src/dlls/objects/611_GM_MazeWell/GM_MazeWell.c#L131); [options](../src/dlls/engine/55/55.c#L319).

**N05.** **PAL50 movement tuning.** Drakor, his hoverpad, and HighTop adjust speeds for the render mode. P0 and P1 share the changes.

[Drakor](../src/dlls/objects/589_BossDrakor/BossDrakor.c#L718); [hoverpad](../src/dlls/objects/625/625.c#L674); [HighTop](../src/dlls/objects/626/626.c#L938).

**N06.** **UI presentation.** Task bullets, WarpStone art, save-prompt placement, text-box borders, and NPC box widths change. No specific clipping bug was established.

[NPC boxes](../src/dlls/engine/0/0.c#L2086); [text-box definitions](../src/main/gametext_data.c#L166); [border](../src/main/textrender_drawbox.c#L18); [WarpStone](../src/dlls/engine/65/65.c#L19); [menu line height](../src/dlls/engine/60/60.c#L24).

**N07.** **Diagnostics and source layout.** Logging, assert lines, unused constants, storage layouts, and renamed or moved data do not establish gameplay changes.

[object logging](../src/main/object.c#L1287); [descriptor tail](../include/dlls/objects/358.h#L9); [adjacency](../src/main/pi_dolphin.c#L174).

**N08.** **Misleading language constant.** The Italian SDK constant used by E1's card-comment code has the same numeric value as the game's Japanese enum. It does not imply Japanese titles on Italian saves. Font changes are covered by U03.

[comment builder](../src/dlls/engine/2/maketex.c#L394); [font setup](../src/main/gametext.c#L1271).

## Appendix D: audit scope and coverage

The original source audit was performed on 2026-09-26 against
`f21cfbc660a7643fa030ae91df9d5ad3513f8aeb`. It found 190 version conditionals in
59 files, grouped here into 31 findings and three unresolved issues. Counts
include nested guards and `#elif` branches, so they are not bug counts.

The location follow-up scanned 124 romlists in each of J0, E1, P0, and P1 and
checked their DOL hashes against the configured originals. E0's placement
assets were unavailable locally; its locations are inferred from matching
source IDs and the other releases. Rena's game-bit annotations supplied leads
that were then checked against placement bytes and source.

This was a code and targeted asset review, without gameplay reproduction.
It does not cover every script or asset difference, bugs shared by all versions,
or differences still missing from the unfinished ports.

| File | Directives | Disposition |
| --- | ---: | --- |
| [engine/0/0.c](../src/dlls/engine/0/0.c) | 16 | G29; N01, N06, N07 |
| [engine/2/maketex.c](../src/dlls/engine/2/maketex.c) | 11 | G30; N03, N08 |
| [engine/20_Hcurves/Hcurves.c](../src/dlls/engine/20_Hcurves/Hcurves.c) | 4 | G01; diagnostic/storage changes |
| [engine/21/21.c](../src/dlls/engine/21/21.c) | 2 | N03 |
| [engine/23/23.c](../src/dlls/engine/23/23.c) | 4 | N03 |
| [engine/47/47.c](../src/dlls/engine/47/47.c) | 2 | G07 |
| [engine/52_n_attractmode/n_attractmode.c](../src/dlls/engine/52_n_attractmode/n_attractmode.c) | 11 | G31; N03, N07 |
| [engine/53/53.c](../src/dlls/engine/53/53.c) | 3 | G22; task bullets in N06 |
| [engine/55/55.c](../src/dlls/engine/55/55.c) | 8 | N03, N04; descriptor padding in N07 |
| [engine/60/60.c](../src/dlls/engine/60/60.c) | 1 | N06 |
| [engine/65/65.c](../src/dlls/engine/65/65.c) | 1 | N06 |
| [objects/196_Tricky/tricky.c](../src/dlls/objects/196_Tricky/tricky.c) | 1 | G03 |
| [objects/201_Baddie/Baddie.c](../src/dlls/objects/201_Baddie/Baddie.c) | 5 | G02; unused constant in N07 |
| [objects/202/guardclaw.c](../src/dlls/objects/202/guardclaw.c) | 1 | G25 |
| [objects/245_SidekickBal/SidekickBal.c](../src/dlls/objects/245_SidekickBal/SidekickBal.c) | 4 | G05, G06 |
| [objects/251/251.c](../src/dlls/objects/251/251.c) | 1 | G10 |
| [objects/273/273.c](../src/dlls/objects/273/273.c) | 1 | G11 |
| [objects/287_MagicCaveTo/MagicCaveTo.c](../src/dlls/objects/287_MagicCaveTo/MagicCaveTo.c) | 1 | G19 |
| [objects/344/344.c](../src/dlls/objects/344/344.c) | 3 | G08, G09 |
| [objects/417/417.c](../src/dlls/objects/417/417.c) | 1 | G13 |
| [objects/422_SH_tricky/SH_tricky.c](../src/dlls/objects/422_SH_tricky/SH_tricky.c) | 1 | G04 |
| [objects/510/510.c](../src/dlls/objects/510/510.c) | 1 | G12 |
| [objects/589_BossDrakor/BossDrakor.c](../src/dlls/objects/589_BossDrakor/BossDrakor.c) | 5 | G18; N05 and logging |
| [objects/598_DIMSnowHorn/DIMSnowHorn.c](../src/dlls/objects/598_DIMSnowHorn/DIMSnowHorn.c) | 2 | G14 |
| [objects/599_DR_EarthWar/DR_EarthWar.c](../src/dlls/objects/599_DR_EarthWar/DR_EarthWar.c) | 1 | G17 |
| [objects/611_GM_MazeWell/GM_MazeWell.c](../src/dlls/objects/611_GM_MazeWell/GM_MazeWell.c) | 1 | N04 |
| [objects/613_DR_Creator/DR_Creator.c](../src/dlls/objects/613_DR_Creator/DR_Creator.c) | 2 | G15 |
| [objects/625/625.c](../src/dlls/objects/625/625.c) | 1 | N05 |
| [objects/626/626.c](../src/dlls/objects/626/626.c) | 4 | G15, G16; N05 |
| [objects/644/644.c](../src/dlls/objects/644/644.c) | 2 | G21 |
| [objects/653_WCLevelCont/WCLevelCont.c](../src/dlls/objects/653_WCLevelCont/WCLevelCont.c) | 2 | G27 |
| [objects/659/659.c](../src/dlls/objects/659/659.c) | 1 | G26 |
| [objects/693_Timer/Timer.c](../src/dlls/objects/693_Timer/Timer.c) | 1 | U01 |
| [objects/704/704.c](../src/dlls/objects/704/704.c) | 1 | N03: regional title texture |
| [dolphin/os/OSRtc.c](../src/dolphin/os/OSRtc.c) | 3 | N01, N03: language/display accessors |
| [main/audio.c](../src/main/audio.c) | 1 | U02 |
| [main/fileio.c](../src/main/fileio.c) | 3 | N02: DVD check result |
| [main/gameloop.c](../src/main/gameloop.c) | 18 | N01, N02 |
| [main/gametext.c](../src/main/gametext.c) | 16 | U03; N03, N08 |
| [main/gametext_data.c](../src/main/gametext_data.c) | 5 | U03; N03, N06, N08 |
| [main/mm.c](../src/main/mm.c) | 1 | N01: progressive flag storage absent in PAL |
| [main/model.c](../src/main/model.c) | 1 | G24 |
| [main/modelEngine.c](../src/main/modelEngine.c) | 2 | G20 |
| [main/object.c](../src/main/object.c) | 2 | N07 |
| [main/pi_dolphin.c](../src/main/pi_dolphin.c) | 7 | G23; N07 |
| [main/pi_videoinit.c](../src/main/pi_videoinit.c) | 1 | N01: PAL display-copy filtering |
| [main/textrender_drawbox.c](../src/main/textrender_drawbox.c) | 1 | N06 |
| [track/intersect_memcard.c](../src/track/intersect_memcard.c) | 6 | G28; N03 |
| [include/dlls/objects/344.h](../include/dlls/objects/344.h) | 3 | G08 state/bitfield layout |
| [include/dlls/objects/358.h](../include/dlls/objects/358.h) | 2 | N07 |
| [include/main/dll/dll_0015_save_settings.h](../include/main/dll/dll_0015_save_settings.h) | 1 | N03 declarations |
| [include/main/dll/savegame.h](../include/main/dll/savegame.h) | 1 | N03 declarations |
| [include/main/dll/WC/dll_028D_wclevelcont.h](../include/main/dll/WC/dll_028D_wclevelcont.h) | 2 | G27 state layout |
| [include/main/fileio.h](../include/main/fileio.h) | 1 | N02 declaration |
| [include/main/gameloop_internal.h](../include/main/gameloop_internal.h) | 3 | N01 declarations |
| [include/main/gametext_api.h](../include/main/gametext_api.h) | 1 | N03 declaration |
| [include/main/gametext_data.h](../include/main/gametext_data.h) | 1 | N03 declarations |
| [include/main/video_viewport.h](../include/main/video_viewport.h) | 1 | N01 |
| [include/track/intersect_card_api.h](../include/track/intersect_card_api.h) | 1 | N03 declaration |
| **Total** | **190** | **59 files** |

SDK/compiler switches such as `SDK_REVISION`, `VERSION_GCCP01`, and `__MWERKS__`
were excluded unless they described an actual SFA release difference.
To refresh the source inventory:

```sh
rg -n '^\s*#\s*(if|ifdef|ifndef|elif)\b.*VERSION_GSA' src include
```
