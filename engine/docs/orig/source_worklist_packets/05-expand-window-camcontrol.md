# Retail Source Boundary Packet: `camcontrol.c`

## Summary
- action: `expand-window`
- confidence: `high`
- suggested path: `dll/CAM/camcontrol.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x80102D3C-0x80103130` size=`0x3F4`
- debug target size: `0x10E0`
- fit status: `seed-too-small`
- suggested window: `0x801024E8-0x80103648` size=`0x1160` delta=`+0x80` xref_coverage=`1/1`
- xref count: `1`

## Why
- Seed is smaller than the debug split size; expand toward `0x801024E8-0x80103648` first.

## EN Xref Functions
- `fn_80102D3C@0x80102D3C-0x80103130`

## Current Seed Functions
- `fn_80102D3C@0x80102D3C-0x80103130` size=`0x3F4`

## Suggested Inspection Window
- `fn_801024E8@0x801024E8-0x80102B5C` size=`0x674`
- `fn_80102B5C@0x80102B5C-0x80102B78` size=`0x1C`
- `fn_80102B78@0x80102B78-0x80102B84` size=`0xC`
- `fn_80102B84@0x80102B84-0x80102B98` size=`0x14`
- `fn_80102B98@0x80102B98-0x80102BA4` size=`0xC`
- `fn_80102BA4@0x80102BA4-0x80102BB0` size=`0xC`
- `fn_80102BB0@0x80102BB0-0x80102CA0` size=`0xF0`
- `fn_80102CA0@0x80102CA0-0x80102CC8` size=`0x28`
- `fn_80102CC8@0x80102CC8-0x80102CFC` size=`0x34`
- `fn_80102CFC@0x80102CFC-0x80102D24` size=`0x28`
- `fn_80102D24@0x80102D24-0x80102D3C` size=`0x18`
- `fn_80102D3C@0x80102D3C-0x80103130` size=`0x3F4`
- `fn_80103130@0x80103130-0x801031A4` size=`0x74`
- `fn_801031A4@0x801031A4-0x801031E0` size=`0x3C`
- `fn_801031E0@0x801031E0-0x80103224` size=`0x44`
- `fn_80103224@0x80103224-0x801032F0` size=`0xCC`
- `fn_801032F0@0x801032F0-0x80103344` size=`0x54`
- `fn_80103344@0x80103344-0x8010334C` size=`0x8`
- `fn_8010334C@0x8010334C-0x80103354` size=`0x8`
- `fn_80103354@0x80103354-0x8010335C` size=`0x8`
- `fn_8010335C@0x8010335C-0x80103648` size=`0x2EC`

## Corridor Context
- previous corridor: `gameplay.c`, `pickup.c`, `modanimeflash1.c`, `modcloudrunner2.c`
- next corridor: `attention.c`, `firstperson.c`, `baddieControl.c`, `n_POST.c`, `n_options.c`, ... (+106 more)
- debug neighbors before: `dll_CD.c`, `dll_CE.c`
- debug neighbors after: `cutCam.c`, `attention.c`

## Recommended Next Steps
- Inspect the suggested window as one candidate file before naming the final boundary.
- Verify that the retail xref function lands inside the expanded range and that adjacent functions do not obviously belong to the corridor neighbors.
