# Retail Source Boundary Packet: `textblock.c`

## Summary
- action: `shared-island`
- confidence: `medium`
- suggested path: `textblock.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x80209624-0x802096AC` size=`0x88`
- retail labels: `Init`
- xref count: `3`

## Why
- Retail tags already define a shared EN island with `textblock.c`, `laser.c`; split the island before naming leaf functions.

## EN Xref Functions
- `fn_80209624@0x80209624-0x80209650`
- `fn_80209650@0x80209650-0x8020967C`
- `fn_80209680@0x80209680-0x802096AC`

## Current Seed Functions
- `fn_80209624@0x80209624-0x80209650` size=`0x2C`
- `fn_80209650@0x80209650-0x8020967C` size=`0x2C`
- `fn_8020967C@0x8020967C-0x80209680` size=`0x4`
- `fn_80209680@0x80209680-0x802096AC` size=`0x2C`

## Suggested Inspection Window
- `fn_80209624@0x80209624-0x80209650` size=`0x2C`
- `fn_80209650@0x80209650-0x8020967C` size=`0x2C`
- `fn_8020967C@0x8020967C-0x80209680` size=`0x4`
- `fn_80209680@0x80209680-0x802096AC` size=`0x2C`

## Corridor Context
- previous corridor: none
- next corridor: none
- shared island sources: `textblock.c`, `laser.c`
- shared island span: `0x80209624-0x802096D8` size=`0xB4`

## Recommended Next Steps
- Treat the shared island as one small packet first, then separate the leaf files once constructor or registration boundaries are clearer.
- Open every tiny function in the island together; splitting them independently is likely to overfit weak evidence.
