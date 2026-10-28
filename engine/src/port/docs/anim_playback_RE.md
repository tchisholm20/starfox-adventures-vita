# modelAnimBuildJointMatrices — reverse-engineering (0x80006C6C, GSAE01, size 0x130C)

Reversed from the retail disassembly (`build/GSAE01_rev1/asm/main/render.s`, emitted as the
undecompiled gap `gap_03_80006C6C_text`). This is the function that turns a model's compressed
animation keyframes into the per-joint 3x4 matrices used to skin the mesh. It is self-contained
(no external `bl`s; the decode is inlined), heavily paired-single.

## Signature (render_internal.h)
`void modelAnimBuildJointMatrices(int* out, u8* dst, ObjAnimState* work, u8* jointData,
                                  int jointCount, u8* scratch, int flags, u8 mode);`
- `out`   = &jointMatrixBase (a pointer written to r13-globals; the matrices go into `dst`)
- `dst`   = the joint-matrix output buffer: `jointCount` entries of a **3x4 Mtx (0x30 bytes)**,
  row-major, rotation in cols 0..2, translation in col 3 (offsets 0x0/0x4/0x8 | 0xc,
  0x10/0x14/0x18 | 0x1c, 0x20/0x24/0x28 | 0x2c).
- `work`  = ObjAnimState (objanim_internal.h): framePhase@0x04, frameStreamCursors@0x2c/0x30,
  moveFrameData(ObjAnimFrameCommand*)@0x34/0x38, frameStreamStrides@0x4c/0x4e, eventCountdown@0x58.
- `jointData` = ModelBone[jointCount] (model.h): {s8 parent@0, u8 idx[3]@1, f32 head[3]@4,
  f32 tail[3]@0x10}, stride 0x1c. `idx[]` are the matrix-slot indices this bone writes/reads.
- `mode` bits: 0x40 = single-frame/no-interp path; 0x1/0x2 = which channel (move A / move B);
  0xc = blend present; 0x10/0x20 = event/loop flags.

## Pipeline (three stages)

### 1. Per-slot sample decode  (.L_800074EC full path; .L_80007738 = single-frame mode 0x40)
Unpacks the packed keyframe stream into a scratch **slot buffer** (0x40-byte stride per matrix
slot; rotation angles at +0x0/+0x6/+0xc, translation at +0x18, plus interp state).
- `moveFrameData` (ObjAnimFrameCommand): byte[0] = slot count N; from +4, an array of u16
  **component descriptors**, 3 rotation + (opt) translation per slot. Each descriptor =
  `(base << 4) | bitWidth`  — high 12 bits = base value, low 4 bits = per-frame delta bit width.
- Two 32-bit word cursors are read: `r12` = current-frame packed bits (frameStreamCursor),
  `r20` = next-frame bits (cursor + frameStride). Bits are consumed MSB-first with refill across
  32-bit boundaries (`r27` = bit position).
- Per component: if bitWidth==0 -> value = base (constant, no animation). Else extract `bitWidth`
  signed bits from cur and next, `sample = cur + ((next-cur) * subFrac >> 14)` (linear subframe
  interpolation; subFrac = fractional part of framePhase * scale, held in r29), value = base+sample.
- Output the 3 rotation angles (s16) + translation (s16) into the slot buffer.

### 2. Per-joint quaternion + local matrix  (.L_80006E34, inner .L_800072C4)
For each bone, `idx[]` selects its decoded slot(s). Each rotation is **3 angle-encoded s16**
(`.L_800072C4`): `angle_s16 -> a = (s16>>1)` dequantized; sin & cos computed by 4-term polynomials
with a 2-bit **quadrant flag** `((a + 0x2000) & 0xc000)` doing the ±sin/±cos reduction. The three
(cos,sin) HALF-angle pairs (c0,s0),(c1,s1),(c2,s2) combine to a quaternion:
```
w = c0c1c2 + s0s1s2
x = s0c1c2 - c0s1s2
y = c0s1c2 + s0c1s2
z = c0c1s2 - s0s1c2
```
When a blend channel is present (mode 0xc): decode both channels' quaternions, take the dot
product, **negate the second for the shortest path** (`if dot<0`), and NLERP by the blend weight
`f28`/`(1-f28)`, then renormalise (`1/sqrt`). The quaternion is expanded to a 3x3 rotation matrix
(standard `1-2(y2+z2)` form, built via the `f10*f0..` products) and written with the slot's
translation into the joint's local 3x4 matrix (`dst` entry, 0x30 stride). Rows can be selectively
skipped when a component's descriptor was constant (the `cmpwi r18,0; bne` guards).

### 3. Forward kinematics  (.L_80007D74)
Walks the bones; for each with a parent, `PSMTXConcat(parentMtx, localMtx) -> jointMtx` using
paired-single `ps_muls0/ps_madds1/ps_madds0` (the classic Gekko 3x4 concat). Root bones keep their
local matrix. Result: `dst[j]` = model-space 3x4 matrix for joint j. These are exactly the matrices
the port's `gx_draw_setJointMatrices` applies per-vertex by PNMTXIDX/3.

## What real playback needs (prerequisites, in order)
The build function is only the LAST stage. To feed it real data the port still needs:
1. **objanim move setup** — populate ObjAnimState.frameData (the ObjAnimFrameCommand descriptors)
   + cacheSlots for a selected animation/move. This is the objanim/sequence layer (currently
   stubbed); it turns an animation id + the loaded ANIM.BIN atlas into frame commands.
2. **modelAnimUpdateChannels** (already REAL in model.c) — computes frameStreamCursors[i] +
   frameStreamStrides[i] + the subframe fraction from framePhase. Just needs (1) done + the atlas.
3. **the build** — reimplement stages 1-3 above (this doc) in the port, OR keep driving
   gx_draw_setJointMatrices from a port-side decoder. The render half (matrix skinning) is DONE.

Stage 2's math (euler->quat->3x4 + FK) is portable with standard sinf/cosf (the polynomials are
just a fast sin/cos); the angle SOURCE is the packed-stream decode (stage 1) fed by (1).
