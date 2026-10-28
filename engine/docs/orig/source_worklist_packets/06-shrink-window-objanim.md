# Retail Source Boundary Packet: `objanim.c`

## Summary
- action: `shrink-window`
- confidence: `medium`
- suggested path: `main/objanim.c`
- split status: `unsplit`
- retail bundles: `4`
- current seed: `0x8002EC4C-0x80030780` size=`0x1B34`
- debug target size: `0x3A8`
- fit status: `seed-too-wide`
- suggested window: `0x8002EC4C-0x8002F604` size=`0x9B8` delta=`+0x610` xref_coverage=`2/3`
- retail labels: `setBlendMove`
- xref count: `3`

## Why
- Seed is wider than the debug split size; the best compact candidate is `0x8002EC4C-0x8002F604`.

## EN Xref Functions
- `fn_8002EC4C@0x8002EC4C-0x8002EE10`
- `fn_8002F334@0x8002F334-0x8002F604`
- `fn_8003042C@0x8003042C-0x80030780`

## Current Seed Functions
- `fn_8002EC4C@0x8002EC4C-0x8002EE10` size=`0x1C4`
- `fn_8002EE10@0x8002EE10-0x8002EE64` size=`0x54`
- `fn_8002EE64@0x8002EE64-0x8002EEB8` size=`0x54`
- `fn_8002EEB8@0x8002EEB8-0x8002F304` size=`0x44C`
- `fn_8002F304@0x8002F304-0x8002F334` size=`0x30`
- `fn_8002F334@0x8002F334-0x8002F604` size=`0x2D0`
- `fn_8002F604@0x8002F604-0x8002F624` size=`0x20`
- `fn_8002F624@0x8002F624-0x8002F66C` size=`0x48`
- `fn_8002F66C@0x8002F66C-0x8002F6CC` size=`0x60`
- `fn_8002F6CC@0x8002F6CC-0x8002FB40` size=`0x474`
- `fn_8002FB40@0x8002FB40-0x800303FC` size=`0x8BC`
- `fn_800303FC@0x800303FC-0x8003042C` size=`0x30`
- `fn_8003042C@0x8003042C-0x80030780` size=`0x354`

## Suggested Inspection Window
- `fn_8002EC4C@0x8002EC4C-0x8002EE10` size=`0x1C4`
- `fn_8002EE10@0x8002EE10-0x8002EE64` size=`0x54`
- `fn_8002EE64@0x8002EE64-0x8002EEB8` size=`0x54`
- `fn_8002EEB8@0x8002EEB8-0x8002F304` size=`0x44C`
- `fn_8002F304@0x8002F304-0x8002F334` size=`0x30`
- `fn_8002F334@0x8002F334-0x8002F604` size=`0x2D0`

## Corridor Context
- previous corridor: none
- next corridor: `objhits.c`
- debug neighbors before: `SKNControl.c`, `objects.c`
- debug neighbors after: `objhits.c`, `objlib.c`

## Recommended Next Steps
- Trim the current seed around the retail-xref functions before materializing a file boundary.
- Prefer the compact suggested window as the first hypothesis, then validate surrounding rodata and call patterns.
