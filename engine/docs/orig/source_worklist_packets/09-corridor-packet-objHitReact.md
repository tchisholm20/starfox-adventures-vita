# Retail Source Boundary Packet: `objHitReact.c`

## Summary
- action: `corridor-packet`
- confidence: `medium`
- suggested path: `objHitReact.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x8003549C-0x80035728` size=`0x28C`
- xref count: `1`

## Why
- Retail seed is best treated as one packet inside a debug-side source corridor containing `objhits.c`, `objlib.c`, `objprint.c`, `objprint_dolphin.c`, `pi_dolphin.c`.

## EN Xref Functions
- `fn_8003549C@0x8003549C-0x80035728`

## Current Seed Functions
- `fn_8003549C@0x8003549C-0x80035728` size=`0x28C`

## Suggested Inspection Window
- `fn_8003549C@0x8003549C-0x80035728` size=`0x28C`

## Corridor Context
- previous corridor: `objhits.c`
- next corridor: `objlib.c`, `objprint.c`, `objprint_dolphin.c`, `pi_dolphin.c`, `rcp_dolphin.c`, ... (+11 more)

## Recommended Next Steps
- Work the whole corridor packet instead of asserting a narrow final boundary immediately.
- Use the listed gap neighbors to decide whether this source should become one file or part of a larger missing cluster.
