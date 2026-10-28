# How GC/1.3 orders a TU's `.sdata2`, and what the orphan pool ranges were

Traced 2026-09-27 against the stock GC/1.3 `mwcceppc.exe`
(`4e502c38465500d4fda8d966b268151a6c74c730508e3d9b7efd23d1a6083715`) under LLDB in the
`../mwcc` offline Docker/QEMU sandbox; every traced object was byte-identical to an untraced
compile. Addresses below are compiler virtual addresses.

## Section choice for a named `const`

`GC13_Storage_Prepare(object, size, alternate)` (`0x4b31d0`, recovered in
`../mwcc/src/versions/GC_1_3/StoragePrepare.c`) picks the read-only small section
(`.sdata2`) only when `alternate` is 1, and it returns early once `object->section > 0`, so
the first caller wins. The definition path (`0x4b2f7b`) passes 1. The code-reference path
in the storage classifier (`0x4b4030`) always pushes 0. A named constant that code
references before its definition is processed is therefore placed in `.sdata`.

## Folding

The front end folds every read of a `const` scalar whose initializer it has already seen.
`-opt off` does not change this; neither does a braced initializer, an integer
initializer, taking the address, or `static`. A const aggregate is not folded, but under
CSE its second field is reached through a base register rather than `sym+4`.

## Creation order

A literal's slot follows its creation: float literals are interned by the expression
preparation pass (call at `0x433739`, return `0x43373e`); int-to-float conversion doubles
are created later in lowering (call at `0x433936`, via `0x44f631` -> `0x4e007d` ->
`0x4e0192`). Within one function, floats always precede that function's conversion
doubles. Named definitions enter the section when they are processed.

## Deferred TUs

`-inline ...,deferred` generates code only after the whole TU is parsed, emitting
functions in reverse source order. That reconciles everything retail shows for several
DLLs:

- The source runs top-down (descriptor callbacks first, helpers last). Retail `.text`
  order is the reverse.
- Named constants are declared `extern const` at the top and defined just before the
  `ObjectDescriptor`. Reads are parsed before the initializer is known, so they stay
  unfolded, and the definition runs before any code reference, so they land in `.sdata2`
  **ahead of** every literal.
- With `auto` inlining, a small `static` helper is inlined at every call site, but its
  out-of-line copy is still generated, and generated first when it is last in the file.
  mwld strips the copy (class A in `dead_strip_census.md`); its pool entries remain. That
  is why a retail pool can open with conversion doubles that no live function creates
  first. Such a helper must create only what the pool shows first: a bare
  `(f32)value` helper for a leading conversion double.

Recovered this way: `MagicPlant` (two stripped event helpers), `284` (the shared
`gStaffReaction*` constants that `283` reads), `LanternFire`, `SpiritDoorL`,
`BombPlantSp`, `75`. `InvisibleHi` and `SpiritDoorL` also needed `x / 64.0f`: MWCC turns a
power-of-two division into a multiply by the reciprocal, which retail's `0.015625` came
from, and a `* 0.015625f` literal is hoisted differently.

## Constants defined outside the reading TU

C rejects `extern f32 x;` followed by `const f32 x = ...;` in one TU. For
`WCBouncyCra`, plain `extern f32` declarations reproduce retail's code exactly, and
`extern const` does not, even without a local definition: the qualifier alone changes
register allocation. Its named block therefore was defined in another TU. The
same shape (a named block between one DLL's literals and the next DLL's) covers
`WCPushBlock`, `650`, `WORLDAstero`'s trailing pair and `ARWArwing`'s 39 tuning values.
Assigning those definitions is a TU-boundary decision and is not made here.
