# Retail Source Boundary Packet: `DIMBoss.c`

## Summary
- action: `split-now`
- confidence: `high`
- suggested path: `dll/DIM/DIMboss.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x801BD0E8-0x801BD7F4` size=`0x70C`
- debug target size: `0x900`
- fit status: `seed-near-fit`
- suggested window: `0x801BD0E8-0x801BDA04` size=`0x91C` delta=`+0x1c` xref_coverage=`1/1`
- xref count: `2`

## Why
- Seed is already close to the debug split size and has enough retail evidence to start a first-pass split.

## EN Xref Functions
- `fn_801BD0E8@0x801BD0E8-0x801BD7F4`

## Current Seed Functions
- `fn_801BD0E8@0x801BD0E8-0x801BD7F4` size=`0x70C`

## Suggested Inspection Window
- `fn_801BD0E8@0x801BD0E8-0x801BD7F4` size=`0x70C`
- `fn_801BD7F4@0x801BD7F4-0x801BD7F8` size=`0x4`
- `fn_801BD7F8@0x801BD7F8-0x801BD804` size=`0xC`
- `fn_801BD804@0x801BD804-0x801BD80C` size=`0x8`
- `fn_801BD80C@0x801BD80C-0x801BD814` size=`0x8`
- `fn_801BD814@0x801BD814-0x801BD918` size=`0x104`
- `fn_801BD918@0x801BD918-0x801BD9C8` size=`0xB0`
- `fn_801BD9C8@0x801BD9C8-0x801BDA04` size=`0x3C`

## Corridor Context
- previous corridor: `attention.c`, `firstperson.c`, `baddieControl.c`, `n_POST.c`, `n_options.c`, ... (+106 more)
- next corridor: `DIMbosstonsil.c`, `DIMbossspit.c`, `DFcradle.c`, `DFpulley.c`, `DFbarrel.c`, ... (+18 more)
- debug neighbors before: `dll_21E.c`, `dll_21F.c`
- debug neighbors after: `dll_221.c`, `DIMbosstonsil.c`

## Recommended Next Steps
- Claim the suggested EN window as a first-pass split candidate and verify the function count against nearby rodata/data ownership.
- Use the corridor neighbors only as edge guards; the retail evidence is already strong enough to start a real source file.
