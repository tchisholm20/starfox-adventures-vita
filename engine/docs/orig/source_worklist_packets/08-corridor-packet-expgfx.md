# Retail Source Boundary Packet: `expgfx.c`

## Summary
- action: `corridor-packet`
- confidence: `medium`
- suggested path: `dll/expgfx.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x8009B36C-0x8009FF68` size=`0x4BFC`
- xref count: `6`

## Why
- Retail seed is best treated as one packet inside a debug-side source corridor containing `objlib.c`, `objprint.c`, `objprint_dolphin.c`, `pi_dolphin.c`, `rcp_dolphin.c`.

## EN Xref Functions
- `fn_8009B36C@0x8009B36C-0x8009B4E0`
- `fn_8009B4E0@0x8009B4E0-0x8009B648`
- `fn_8009E078@0x8009E078-0x8009E198`
- `fn_8009F558@0x8009F558-0x8009FF68`

## Current Seed Functions
- `fn_8009B36C@0x8009B36C-0x8009B4E0` size=`0x174`
- `fn_8009B4E0@0x8009B4E0-0x8009B648` size=`0x168`
- `fn_8009B648@0x8009B648-0x8009B960` size=`0x318`
- `fn_8009B960@0x8009B960-0x8009BC54` size=`0x2F4`
- `fn_8009BC54@0x8009BC54-0x8009E078` size=`0x2424`
- `fn_8009E078@0x8009E078-0x8009E198` size=`0x120`
- `fn_8009E198@0x8009E198-0x8009E290` size=`0xF8`
- `fn_8009E290@0x8009E290-0x8009E2B0` size=`0x20`
- `fn_8009E2B0@0x8009E2B0-0x8009E2B4` size=`0x4`
- `fn_8009E2B4@0x8009E2B4-0x8009E2B8` size=`0x4`
- `fn_8009E2B8@0x8009E2B8-0x8009E2C0` size=`0x8`
- `fn_8009E2C0@0x8009E2C0-0x8009E3C8` size=`0x108`
- `fn_8009E3C8@0x8009E3C8-0x8009EF70` size=`0xBA8`
- `fn_8009EF70@0x8009EF70-0x8009F144` size=`0x1D4`
- `fn_8009F144@0x8009F144-0x8009F164` size=`0x20`
- `fn_8009F164@0x8009F164-0x8009F268` size=`0x104`
- `fn_8009F268@0x8009F268-0x8009F438` size=`0x1D0`
- `fn_8009F438@0x8009F438-0x8009F558` size=`0x120`
- `fn_8009F558@0x8009F558-0x8009FF68` size=`0xA10`

## Suggested Inspection Window
- `fn_8009B36C@0x8009B36C-0x8009B4E0` size=`0x174`
- `fn_8009B4E0@0x8009B4E0-0x8009B648` size=`0x168`
- `fn_8009B648@0x8009B648-0x8009B960` size=`0x318`
- `fn_8009B960@0x8009B960-0x8009BC54` size=`0x2F4`
- `fn_8009BC54@0x8009BC54-0x8009E078` size=`0x2424`
- `fn_8009E078@0x8009E078-0x8009E198` size=`0x120`
- `fn_8009E198@0x8009E198-0x8009E290` size=`0xF8`
- `fn_8009E290@0x8009E290-0x8009E2B0` size=`0x20`
- `fn_8009E2B0@0x8009E2B0-0x8009E2B4` size=`0x4`
- `fn_8009E2B4@0x8009E2B4-0x8009E2B8` size=`0x4`
- `fn_8009E2B8@0x8009E2B8-0x8009E2C0` size=`0x8`
- `fn_8009E2C0@0x8009E2C0-0x8009E3C8` size=`0x108`
- `fn_8009E3C8@0x8009E3C8-0x8009EF70` size=`0xBA8`
- `fn_8009EF70@0x8009EF70-0x8009F144` size=`0x1D4`
- `fn_8009F144@0x8009F144-0x8009F164` size=`0x20`
- `fn_8009F164@0x8009F164-0x8009F268` size=`0x104`
- `fn_8009F268@0x8009F268-0x8009F438` size=`0x1D0`
- `fn_8009F438@0x8009F438-0x8009F558` size=`0x120`
- `fn_8009F558@0x8009F558-0x8009FF68` size=`0xA10`

## Corridor Context
- previous corridor: `objlib.c`, `objprint.c`, `objprint_dolphin.c`, `pi_dolphin.c`, `rcp_dolphin.c`, ... (+11 more)
- next corridor: `modgfx.c`, `modelfx.c`, `dim_partfx.c`, `df_partfx.c`, `objfsa.c`

## Recommended Next Steps
- Work the whole corridor packet instead of asserting a narrow final boundary immediately.
- Use the listed gap neighbors to decide whether this source should become one file or part of a larger missing cluster.
