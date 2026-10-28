# allocLotsOfTextures matching notes

`allocLotsOfTextures` in `src/main/newshadows.c` matches retail byte for byte
and the unit links as `MatchingFor("GSAE01")`. The last three regions (two I8
ramps, the lightning fill and the reflection gradient) only matched once their
source shape changed; declaration-order sweeps over the earlier spelling were
provably flat. This note records why each shape is required.

## Compiler mechanisms (GC/1.3, `-O4,p -opt nopeephole,noschedule,nodead`)

- The IRO loop unroller fully unrolls 4-iteration inner loops into uniform
  copies (`y` becomes `y + k`, then a dead `y += 4`). The body size threshold is
  tight; almost any extra statement disables the IRO unroll and the backend
  unrolls by two instead.
- IRO common-subexpression temps are created at an expression's second
  occurrence. Statements are visited in order, an assignment's right side before
  its left, a binary node's right child before its left. The temp is placed at
  the first occurrence, or before the common ancestor when both occurrences sit
  in one statement (`../mwcc/src/versions/GC_1_3/IROSharedPlacement.c`).
- Named pointer definitions are propagated into their uses but stay behind as
  dead definitions, giving the second occurrence that creates address temps.
- A store evaluates its value before its address unless an earlier statement
  already computed the address.
- Backend virtual registers: named locals first (reverse declaration order),
  IRO temps next (reverse creation order), lowering scratches last. Coloring
  assigns the highest register number first, lowest free physical register.

## Ramps

Retail colors need the column mask (`x & 7`) below the tile offset below the
stored byte in register number. Uniformly unrolled rows always create the mask
temp before the tile temp, so no loop spelling reaches it. The first row is
peeled: it computes `columnOffset`, `tileColumnOffset` and `value` into named
locals declared as `value`, `tileColumnOffset`, `columnOffset`, `x` (the only
one of the 24 orders that matches). Rows 1-3 reuse them, so the IRO never finds
a second occurrence and the backend keeps the named registers.

## Lightning

Retail colors the `0x4330` int-to-float constant after the texture base and the
combined low offset. Both must therefore be lowering scratches in the inner
loop rather than named locals, so the address is a single pointer expression
`gNewShadowLightningTexture + lowoff + rowoff + ...`, with `rowoff` declared
before `lowoff`.

## Reflection gradient

Retail computes the address before the value in rows 0-2 and after it in row 3,
which a uniformly unrolled loop cannot produce. Rows 0-2 are a 3-iteration loop
and row 3 follows it, assigning `pixelBase` and `texel` inside its store address
(other placements leave 2-43 differing rows). The column offsets and the high
byte are named x-level locals declared `value`, `tileColumnOffset`,
`columnOffset`, `x`, `y` before the pointers.
