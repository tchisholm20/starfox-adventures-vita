# objprint_dolphin matching

Current status (2026-09-27): all 32 functions and all assigned data report 100%
in all five versions. The TU remains `NonMatching` because the EN source-link
check loses eight bytes of small-data storage; see the final section below.

## objFuzzRenderCb

`objFuzzRenderCb` matches all 2,780 bytes (695 instructions) under the existing
GC/1.3 TU profile in EN v1.0, EN rev1, JP, PAL v1.0, and PAL rev1.

The remaining EN difference was 24 register operands: the projected-light flag
used `r30` instead of `r28`, while the TEV stage index used `r28` instead of
`r30`. Extracting the short-circuit light check into the explicit inline
`objFuzzHasProjectedLight` helper reproduces retail allocation. Its `u8` return
preserves the byte-width tests; inlining adds no call. The local is named
`hasProjectedLight` to describe its role in both stage setup and final counts.
This establishes a matching source spelling, not proof of the original helper.

Validation on 2026-09-26:

- Input DOL hashes verified against each version's configuration.
- `tools/unitfuzzy.py`: callback 100% in all five versions; EN improved from
  99.82734%. No other EN function changed score or instruction bytes.
- Allocated section sizes, alignments, and non-text contents unchanged.
- `clang-format --dry-run --Werror` passed for the TU and its internal header.
- `ninja all_source` and the EN strict retail checksum target passed.

The complete TU remains `NonMatching`; other functions still have differences.

## modelCalcVtxGroupMtxs

The 2026-09-27 pass brings `modelCalcVtxGroupMtxs` from **99.15094% to 100%**
in EN v1.0, EN rev1, JP, PAL v1.0, and PAL rev1. All 636 bytes (159
instructions) match under the existing GC/1.3 TU profile.

The two inverse-bind translations now use separate `jointA` and `jointB`
bone pointers. The second input matrix pointer is declared after those locals,
and the redundant model-header and model-byte aliases are removed. Calls,
field accesses, arithmetic, and their order are unchanged.

The verified compiler trace explains why declaration permutations alone did
not resolve this function. Splitting the bone pointer raises the second
matrix's initial interference degree from 28 to 29. Its declaration order
places it before the bone pointers in the compiler's scan, so it survives the
first low-degree sweep, whose threshold is strictly below 29. Removing the
model-byte alias gives the model parameter the required position in the later
sweep. The resulting allocation reproduces all 27 previously differing
instructions. This establishes a matching source spelling, not the original
local declarations.

Reproduce with:

```sh
python3 tools/unitfuzzy.py objprint_dolphin --symbol modelCalcVtxGroupMtxs
python3 tools/tricky_backend_trace.py --unit main/main/objprint_dolphin \
    --function modelCalcVtxGroupMtxs --graph --output build/vtx_group_matching_trace
```

Validation:

- All five input DOL hashes verified against their version configurations.
- Fresh before/after objects change only this function's 31 instruction bytes
  in each version. Other function scores and bytes, allocated section layouts,
  non-text contents, named-symbol layouts, and relocation records are unchanged.
- Instrumented and ordinary compilation produce identical raw objects; the
  trace aligns all 159 instructions and replays all 22 physical register choices.
- EN `ninja all_source` and the strict retail checksum target pass.
- Formatting preserves the raw object; the TU and internal header pass
  `clang-format --dry-run --Werror`.

The complete TU remains `NonMatching` at 99.99440%; only
`objSetupRenderOpGxState` still differs. No regional completion manifest is
promoted. Compiler settings and TU boundaries are unchanged.

## modelDoRenderInstrs

The 2026-09-27 pass brings `modelDoRenderInstrs` from **99.94304% to 100%**
in EN v1.0, EN rev1, JP, PAL v1.0, and PAL rev1. All 3,160 bytes (790
instructions) match under the existing GC/1.3 TU profile.

In the unanimated-model path, the first joint matrix now has an explicit
`ObjModelJointMatrix*` local before it is passed to `PSMTXCopy`. The calls and
their order are unchanged. This resolves the nine instructions that exchanged
`owner` and `builtSkinMatrices` between `r21` and `r22`, including the later
conversion constant that reuses the owner's register.

The verified compiler graph grows from 256 to 257 nodes. The extra matrix
local changes coalescing and keeps the skin-matrix flag above the low-degree
threshold until the owner is removed. This reverses their coloring order and
reproduces retail allocation. The instrumented compiler emits the same raw
object as ordinary compilation and replays all 208 physical register choices.
This establishes a matching source spelling, not the original local declaration.

Reproduce with:

```sh
python3 tools/unitfuzzy.py objprint_dolphin --symbol modelDoRenderInstrs
python3 tools/tricky_backend_trace.py --unit main/main/objprint_dolphin \
    --function modelDoRenderInstrs --graph --output build/model_render_matching_trace
```

Validation:

- All five input DOL hashes verified against their version configurations.
- Fresh before/after objects change only this function's ten instruction bytes
  in each version. Other function scores and bytes, allocated section layouts,
  non-text contents, named-symbol layouts, and relocation records are unchanged.
- EN `ninja all_source` and the strict retail checksum target pass.
- Formatting preserves the raw object; the TU and internal header pass
  `clang-format --dry-run --Werror`.

The complete TU remains `NonMatching`; no regional completion manifest is
promoted. Compiler settings and TU boundaries are unchanged.

## addShaderLayerStages

`addShaderLayerStages` matches all 1,128 bytes (282 instructions) under the
existing GC/1.3 TU profile in EN v1.0, EN rev1, JP, PAL v1.0, and PAL rev1.
The 2026-09-26 follow-up improves it from 99.57447% to 100%.

The final change removes five redundant `(u8)` casts from `blendMode`, which
already has type `u8`, and declares the layer index, current layer, and previous
layer before the alpha and texture locals. The rendering operations and control
flow are unchanged. No compiler settings or TU boundaries change.

The casts were significant to allocation despite being redundant in C. With
them, the compiler graph contained 137 nodes, including five additional
coalesced temporaries. Simplification selected the channel-color pointer and
then the light-count argument as high-degree removals, leaving 22 instructions
with different register operands. Removing the casts reduces the graph to 132
nodes and eliminates those forced choices. A replay of this graph predicts the
retained local declaration order; ordinary compilation confirms the exact match.
Removing the casts alone does not match.

The final LLDB capture agrees with the ordinary compiler's complete raw object
hash, aligns all 282 instructions with retail, and replays all 95 physical
register choices with no high-degree removals. Reproduce it with:

```sh
python3 tools/tricky_backend_trace.py --unit main/main/objprint_dolphin \
    --function addShaderLayerStages --graph --output build/add_shader_layers_trace
python3 tools/unitfuzzy.py objprint_dolphin --symbol addShaderLayerStages
```

Validation on 2026-09-26:

- Each of the five input DOL hashes verified against its version configuration.
- Fresh before/after objects report 100% for this function in all five versions.
  Only its 25 instruction bytes change; all other functions, allocated section
  layouts, non-text contents, named-symbol layouts, and relocations are unchanged.
- `objFuzzRenderCb` remains exact; `objSetupRenderOpGxState` remains 99.67612%.
- The EN `ninja all_source`, strict retail checksum target, and formatter checks
  pass. Formatting preserves the raw object.

The complete TU remains `NonMatching`; other functions still have differences.
The exact function does not establish the original local declaration order.

## objSetupRenderOpGxState

The 2026-09-27 follow-up brings `objSetupRenderOpGxState` from **99.92915% to
100%** in EN v1.0, EN rev1, JP, PAL v1.0, and PAL rev1. All 1,976 bytes
(494 instructions) match under the existing GC/1.3 TU profile.

The remaining six instructions exchanged the projected-light loop index and
texture pointer between `r19` and `r20`. The matching source keeps the shader
index as `int` for the lookup and callback APIs, and captures the unsigned
`renderOpIndex` result after the lighting setup. The projected-light loop also
uses an explicit `s32 shadowColorMode` snapshot for its shadow test, while the
stage call still reads `colorMode` and `alphaMode` after the matrix getter.
The texture pointer, loop index, and snapshot retain their declaration order.
These copies emit no additional instructions and preserve the rendering logic.

In the previous compiler graph, the texture started with degree 28 and was
removed in the first simplification sweep, while the index started with degree
30 and waited for a later sweep. The threshold is strictly below 29. Using the
signed shader index removes an overlapping conversion temporary from the loop;
the early removal of the mode snapshot then allows the index to be removed
before the texture in the same sweep. Deferring the unsigned result copy also
preserves retail allocation in the later channel-color and decal-layer code.
Removing either copy, changing the snapshot to `int`, or passing the unsigned
result to the post-render callback changes code generation. This establishes a
matching source spelling, not proof of the original local variables.

The final instrumented compiler and ordinary compile produce the same raw
object. The capture aligns all 494 instructions and replays all 128 physical
register choices with zero retail differences. Reproduce with:

```sh
python3 tools/unitfuzzy.py objprint_dolphin --symbol objSetupRenderOpGxState
python3 tools/tricky_backend_trace.py --unit main/main/objprint_dolphin \
    --function objSetupRenderOpGxState --graph --output build/setup_gx_trace
```

Validation:

- All five input DOL hashes verified against their version configurations.
- Fresh before/after objects change only this function's seven instruction bytes
  in each version. Other function scores and bytes, allocated section layouts,
  non-text contents, and named-symbol layouts are unchanged. Anonymous literal
  names change; relocation types, sites, addends, and target offsets stay fixed.
- `objFuzzRenderCb` and `addShaderLayerStages` remain 100% in all five versions.
- EN `ninja all_source` and the strict retail checksum target pass.
- Formatting preserves the raw object; the TU and its internal header pass
  `clang-format --dry-run --Werror`.

The complete TU reports 100% for all 32 functions (25,004 code bytes) and all
12,768 assigned data bytes in each version, and is `MatchingFor("GSAE01")`.
Nothing references `lbl_803DCC68` and `lbl_803DCC6C`, so `mwldeppc` strips them
unless retained; without them `gForceLoadImmediately` and later small-data
symbols move eight bytes earlier. Both are listed under `force_active` in
`config/GSAE01/config.yml`, as for other unreferenced retail `.sbss` tail words.
A single 12-byte `gObjSelectedLights[3]` is not an alternative: no retail
small-data object exceeds eight bytes, and MWCC places the array in `.bss`.
Compiler settings and TU boundaries are unchanged.

Reproduce the additional link check with:

```sh
python3 tools/verify_source_link.py GSAE01 main/objprint_dolphin.c
```
