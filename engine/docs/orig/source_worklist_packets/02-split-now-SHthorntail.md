# Retail Source Boundary Packet: `SHthorntail.c`

## Summary
- action: `split-now`
- confidence: `high`
- suggested path: `dll/SH/SHthorntail.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x801D5764-0x801D5AFC` size=`0x398`
- debug target size: `0x3B8`
- fit status: `seed-near-fit`
- suggested window: `0x801D5764-0x801D5AFC` size=`0x398` delta=`-0x20` xref_coverage=`1/1`
- xref count: `1`

## Why
- Seed is already close to the debug split size and has enough retail evidence to start a first-pass split.

## EN Xref Functions
- `fn_801D5764@0x801D5764-0x801D5AFC`

## Current Seed Functions
- `fn_801D5764@0x801D5764-0x801D5AFC` size=`0x398`

## Suggested Inspection Window
- `fn_801D5764@0x801D5764-0x801D5AFC` size=`0x398`

## Corridor Context
- previous corridor: `DIMbosstonsil.c`, `DIMbossspit.c`, `DFcradle.c`, `DFpulley.c`, `DFbarrel.c`, ... (+18 more)
- next corridor: `SHroot.c`, `SClevelcontrol.c`, `SClightfoot.c`, `SCchieflightfoot.c`, `SClantern.c`, ... (+106 more)
- debug neighbors before: `lily.c`, `dll_1E8.c`
- debug neighbors after: `SHroot.c`, `dll_1EC.c`

## Recommended Next Steps
- Claim the suggested EN window as a first-pass split candidate and verify the function count against nearby rodata/data ownership.
- Use the corridor neighbors only as edge guards; the retail evidence is already strong enough to start a real source file.
