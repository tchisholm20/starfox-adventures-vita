# Model skinning kernels: provenance of the assembly bodies

Target: EN v1.0 (`GSAE01`), `main/model.c`, game compiler GC/1.3
(`mwcceppc.exe` SHA-256 `4e502c38465500d4fda8d966b268151a6c74c730508e3d9b7efd23d1a6083715`).

`ObjModel_TransformVerticesWithTranslation`, `ObjModel_TransformVerticesLinear` and
`ObjModel_TransformNormalTriplets` are MWCC function-level `asm` bodies (`nofralloc`) in
`src/main/model.c`, beside the morph-target pair already represented that way in the same TU
(`docs/model_morph_targets.md`). The ordinary-C description of the same blend is kept in
`model_skinning_reference.c`. This page records why the C form was rejected as the matching source.

## What retail does

All three share one shape: `stwu r1,-0xA0(r1)`, then `stfd f14..f27` at `8(r1)..0x70(r1)`, and
no save of f28–f31. The body uses f0–f5, f8, f9, f11, f12, f15, f16 and f19–f27, never
writes f14, f17 or f18, and does not touch f28–f31. The vertex loop is software-pipelined: the
prologue sets `ctr = count - 1` with no zero guard, each pass stores the previous vertex while
reading one ahead with `psq_lu`, and the final vertex is stored after `bdnz`. Weights are loaded
through GQR6 (`psq_lu f27,2(r5),0,6`), and vertices/normals load and store through GQR7.

## The compiler cannot emit that save set

GC/1.3's prologue calls a GPR-save emitter at `0x4f6ff0` and an FPR-save emitter at
`0x4f72b0` (from `0x4f7b7b`/`0x4f7b90`, gated on the saved-register counts at `0x5e675c` and
`0x5e6758`). The FPR emitter's loop at `0x4f72e2..0x4f7415` runs `k = 1..N` over the count at
`0x5e6758` and emits a save of register `0x20 - k`: an `stfd` (opcode `0x9A`) paired with a
`psq_st` (`0x197`/`0x199`, quantizer `0x390` = GQR0) when paired-single saves are enabled, and
otherwise a single `_savefpr_%d` call. The restore emitter at `0x4f70e0` mirrors it and ends in
`_restfpr_%d`. The vector-register loop at `0x4f7d30` is a separate AltiVec path (`_savevr%d`).
Every C-generated frame therefore saves exactly `f(32-N)..f31`. `{f14..f27}` is not of that form
for any N.

Live check: `python3 tools/model_skinning_fpr/run_trace.py` compiles
`model_skinning_reference.c` with `main/model.c`'s exact flags inside the `../mwcc` offline
Docker/QEMU sandbox, with host LLDB attached, breaking on both emitters and every instruction
constructor inside them. The traced object is byte-identical to an untraced compile. The C bodies
save and restore `f31..f25` (N = 7) and `f31..f29` (N = 3), each as `stfd` + `psq_st` GQR0 pairs,
as the static reading predicts. Retail's frames also carry no `psq_st` halves, which this TU's
C frames always emit.

Whole-DOL check: `python3 tools/fpr_save_band_scan.py` examines all 935 retail functions that save
nonvolatile FPRs. Exactly four do not save a contiguous band ending at f31. They are these three
kernels and `HandleReverb2` (`dolphin/axfx/reverb_std_callback`), which is itself a `nofralloc`
SDK assembly routine.

## Supporting evidence

- `../mwcc/docs/QUANTIZED_MEMORY_CALLERS.md`: every direct C-lowering call site of GC/1.3's PSQ
  emitter passes a constant GQR0/W=0 or GQR1/W=1. The only table-driven quantizer operand is the
  inline assembler's `P3` field. Retail's GQR6/GQR7 `psq_lu`/`psq_stu` have no C route in that audit.
- Saved-but-unused f14/f17/f18 and the unguarded `count - 1` trip count are consistent with a
  hand-scheduled loop and are not consistent with MWCC's C loop lowering.

## Verification

The three assembled bodies (97, 93 and 173 instructions) are byte-identical to retail, and objdiff
scores each 100%. Removing the C bodies also removes their `1.0f`/`1/128` literal pool entries,
which lifts `.sdata2` and the unit's data to 100%. Retail never had those constants.
