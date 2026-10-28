# Retail Source Boundary Packet: `curves.c`

## Summary
- action: `expand-window`
- confidence: `high`
- suggested path: `dll/curves.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x800E556C-0x800E56A4` size=`0x138`
- debug target size: `0x634C`
- fit status: `seed-too-small`
- suggested window: `0x800E1DA8-0x800E8118` size=`0x6370` delta=`+0x24` xref_coverage=`1/1`
- xref count: `1`

## Why
- Seed is smaller than the debug split size; expand toward `0x800E1DA8-0x800E8118` first.

## EN Xref Functions
- `fn_800E556C@0x800E556C-0x800E56A4`

## Current Seed Functions
- `fn_800E556C@0x800E556C-0x800E56A4` size=`0x138`

## Suggested Inspection Window
- `fn_800E1DA8@0x800E1DA8-0x800E21C0` size=`0x418`
- `fn_800E21C0@0x800E21C0-0x800E2278` size=`0xB8`
- `fn_800E2278@0x800E2278-0x800E2498` size=`0x220`
- `fn_800E2498@0x800E2498-0x800E260C` size=`0x174`
- `fn_800E260C@0x800E260C-0x800E2B40` size=`0x534`
- `fn_800E2B40@0x800E2B40-0x800E2B94` size=`0x54`
- `fn_800E2B94@0x800E2B94-0x800E31E0` size=`0x64C`
- `fn_800E31E0@0x800E31E0-0x800E341C` size=`0x23C`
- `fn_800E341C@0x800E341C-0x800E34E8` size=`0xCC`
- `fn_800E34E8@0x800E34E8-0x800E35B4` size=`0xCC`
- `fn_800E35B4@0x800E35B4-0x800E3664` size=`0xB0`
- `fn_800E3664@0x800E3664-0x800E3734` size=`0xD0`
- `fn_800E3734@0x800E3734-0x800E3968` size=`0x234`
- `fn_800E3968@0x800E3968-0x800E397C` size=`0x14`
- `fn_800E397C@0x800E397C-0x800E3A00` size=`0x84`
- `fn_800E3A00@0x800E3A00-0x800E45B4` size=`0xBB4`
- `fn_800E45B4@0x800E45B4-0x800E4854` size=`0x2A0`
- `fn_800E4854@0x800E4854-0x800E4A48` size=`0x1F4`
- `fn_800E4A48@0x800E4A48-0x800E4C84` size=`0x23C`
- `fn_800E4C84@0x800E4C84-0x800E4E68` size=`0x1E4`
- `fn_800E4E68@0x800E4E68-0x800E4FAC` size=`0x144`
- `fn_800E4FAC@0x800E4FAC-0x800E50E8` size=`0x13C`
- `fn_800E50E8@0x800E50E8-0x800E5184` size=`0x9C`
- `fn_800E5184@0x800E5184-0x800E52C0` size=`0x13C`
- `fn_800E52C0@0x800E52C0-0x800E5330` size=`0x70`
- `fn_800E5330@0x800E5330-0x800E545C` size=`0x12C`
- `fn_800E545C@0x800E545C-0x800E5470` size=`0x14`
- `fn_800E5470@0x800E5470-0x800E556C` size=`0xFC`
- `fn_800E556C@0x800E556C-0x800E56A4` size=`0x138`
- `fn_800E56A4@0x800E56A4-0x800E56B0` size=`0xC`
- `fn_800E56B0@0x800E56B0-0x800E56B4` size=`0x4`
- `fn_800E56B4@0x800E56B4-0x800E56B8` size=`0x4`
- `fn_800E56B8@0x800E56B8-0x800E5928` size=`0x270`
- `fn_800E5928@0x800E5928-0x800E5B80` size=`0x258`
- `fn_800E5B80@0x800E5B80-0x800E5F40` size=`0x3C0`
- `fn_800E5F40@0x800E5F40-0x800E60BC` size=`0x17C`
- `fn_800E60BC@0x800E60BC-0x800E61A0` size=`0xE4`
- `fn_800E61A0@0x800E61A0-0x800E6410` size=`0x270`
- `fn_800E6410@0x800E6410-0x800E6778` size=`0x368`
- `fn_800E6778@0x800E6778-0x800E6A30` size=`0x2B8`
- ... (+15 more functions)

## Corridor Context
- previous corridor: `modgfx.c`, `modelfx.c`, `dim_partfx.c`, `df_partfx.c`, `objfsa.c`
- next corridor: `gameplay.c`, `pickup.c`, `modanimeflash1.c`, `modcloudrunner2.c`
- debug neighbors before: `Dummy15.c`, `TrickyWalk.c`
- debug neighbors after: `gameplay.c`, `foodbag.c`

## Recommended Next Steps
- Inspect the suggested window as one candidate file before naming the final boundary.
- Verify that the retail xref function lands inside the expanded range and that adjacent functions do not obviously belong to the corridor neighbors.
