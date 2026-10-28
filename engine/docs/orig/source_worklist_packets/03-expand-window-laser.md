# Retail Source Boundary Packet: `laser.c`

## Summary
- action: `expand-window`
- confidence: `high`
- suggested path: `dll/CF/laser.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x802096AC-0x802096D8` size=`0x2C`
- debug target size: `0x934`
- fit status: `seed-too-small`
- suggested window: `0x802093B4-0x80209D38` size=`0x984` delta=`+0x50` xref_coverage=`1/1`
- retail labels: `Init`
- xref count: `1`

## Why
- Seed is smaller than the debug split size; expand toward `0x802093B4-0x80209D38` first.

## EN Xref Functions
- `fn_802096AC@0x802096AC-0x802096D8`

## Current Seed Functions
- `fn_802096AC@0x802096AC-0x802096D8` size=`0x2C`

## Suggested Inspection Window
- `fn_802093B4@0x802093B4-0x8020960C` size=`0x258`
- `fn_8020960C@0x8020960C-0x80209610` size=`0x4`
- `fn_80209610@0x80209610-0x80209614` size=`0x4`
- `fn_80209614@0x80209614-0x8020961C` size=`0x8`
- `fn_8020961C@0x8020961C-0x80209624` size=`0x8`
- `fn_80209624@0x80209624-0x80209650` size=`0x2C`
- `fn_80209650@0x80209650-0x8020967C` size=`0x2C`
- `fn_8020967C@0x8020967C-0x80209680` size=`0x4`
- `fn_80209680@0x80209680-0x802096AC` size=`0x2C`
- `fn_802096AC@0x802096AC-0x802096D8` size=`0x2C`
- `fn_802096D8@0x802096D8-0x802096DC` size=`0x4`
- `fn_802096DC@0x802096DC-0x802096E0` size=`0x4`
- `fn_802096E0@0x802096E0-0x802096E8` size=`0x8`
- `fn_802096E8@0x802096E8-0x802096F0` size=`0x8`
- `fn_802096F0@0x802096F0-0x802096F4` size=`0x4`
- `fn_802096F4@0x802096F4-0x802096F8` size=`0x4`
- `fn_802096F8@0x802096F8-0x802096FC` size=`0x4`
- `fn_802096FC@0x802096FC-0x802098A4` size=`0x1A8`
- `fn_802098A4@0x802098A4-0x8020993C` size=`0x98`
- `fn_8020993C@0x8020993C-0x80209940` size=`0x4`
- `fn_80209940@0x80209940-0x80209944` size=`0x4`
- `fn_80209944@0x80209944-0x80209D38` size=`0x3F4`

## Corridor Context
- previous corridor: `SHroot.c`, `SClevelcontrol.c`, `SClightfoot.c`, `SCchieflightfoot.c`, `SClantern.c`, ... (+106 more)
- next corridor: none
- debug neighbors before: `CFcrystal.c`, `CFBaby.c`
- debug neighbors after: `CFPrisonGuard.c`, `dll_163.c`
- shared island sources: `textblock.c`, `laser.c`
- shared island span: `0x80209624-0x802096D8` size=`0xB4`

## Recommended Next Steps
- Inspect the suggested window as one candidate file before naming the final boundary.
- Verify that the retail xref function lands inside the expanded range and that adjacent functions do not obviously belong to the corridor neighbors.
