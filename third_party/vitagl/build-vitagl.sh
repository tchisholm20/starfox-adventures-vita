#!/bin/bash
# Build and install vitaGL and its dependency chain.
#
# docs/07 listed vitaGL as "not in vdpm's package set, builds from source".
# That undersells it: it is four libraries deep and two of them do not build
# against VitaSDK 2026.08 without help.
#
#     vitaGL  ->  vitaShaRK  ->  SceShaccCgExt
#             ->  math-neon
#
# THE C23 PROBLEM. arm-vita-eabi-gcc 15.2.0 defaults to C23
# (__STDC_VERSION__ 202311L), where `type (*)()` declares a function taking NO
# arguments. taiHEN's TAI_CONTINUE macro casts through exactly that form and
# relies on the pre-C23 meaning, "unspecified arguments". So SceShaccCgExt
# fails with "too many arguments to function" on a macro that has not changed
# in years. Forcing -std=gnu17 fixes it, and the same flag is applied down the
# chain for consistency.
#
# Run from anywhere; everything lands in $VITASDK.
set -e

export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH="$VITASDK/bin:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tools/: the patches live here
ROOT="${VITAGL_ROOT:-$HERE/_download}"                   # clone + build dir (the cloud setup sets /opt/stfx/vitagl-src)
# vitaGL is PINNED: the commit eboot 282be076 (2026-10-27) was built with. Upstream HEAD may not take our patches.
VITAGL_COMMIT="${VITAGL_COMMIT:-065d046884522c2ce1624e1f70c7e2fdbc54cbb0}"
mkdir -p "$ROOT"
cd "$ROOT"

clone() {   # clone() <url> <dir>
    if [ ! -d "$2" ]; then
        git clone --depth 1 "$1" "$2"
    else
        echo "  $2 already present"
    fi
}

echo "=== 1/4  SceShaccCgExt (shader compiler extensions) ==="
clone https://github.com/bythos14/SceShaccCgExt.git SceShaccCgExt
cd SceShaccCgExt
rm -rf build && mkdir -p build && cd build
# -std=gnu17: see the C23 note above.
cmake -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DCMAKE_C_FLAGS="-std=gnu17" .. >/dev/null
make -j4 && make install
cd "$ROOT"

echo "=== 2/4  vitaShaRK (runtime shader compiler wrapper) ==="
clone https://github.com/Rinnegatamante/vitaShaRK.git vitaShaRK
cd vitaShaRK
make clean >/dev/null 2>&1 || true
make -j4 CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -std=gnu17"
make install
cd "$ROOT"

echo "=== 3/4  math-neon ==="
clone https://github.com/Rinnegatamante/math-neon.git math-neon
cd math-neon
make clean >/dev/null 2>&1 || true
make -j4 && make install
cd "$ROOT"

echo "=== 4/4  vitaGL ==="
if [ ! -d vitaGL ]; then
    git clone https://github.com/Rinnegatamante/vitaGL.git vitaGL
    git -C vitaGL checkout -q "$VITAGL_COMMIT"
fi
cd vitaGL
# OUR FORK: GL_ARB_sync.
#
# vitaGL exposes no CPU/GPU sync finer than glFinish, which drains the whole
# queue -- that forces frame time to CPU + GPU for any renderer whose CPU
# depends on one early GPU result. The patch implements glFenceSync /
# glClientWaitSync / glWaitSync / glDeleteSync / glIsSync on top of the
# monotonic completed-scene counter vitaGL already keeps for occlusion queries
# (query_fence). Standard names, because GLsync was already typedef'd and the
# intent to add this was clearly there.
#
# Proposed upstream -- see tools/vitagl-arb-sync-PR.md.
#
# Worth 31.5 -> 18.0 ms at three denoise passes, and it is what makes frame
# time flat in denoise quality. See docs/15 section 5.
#
# Applied unconditionally after clone; re-running is safe because the clone is
# fresh. If it ever fails to apply, upstream has moved and the fence needs
# re-porting -- do not silently continue without it.
git apply --check "$HERE/vitagl-arb-sync.patch" 2>/dev/null     && git apply "$HERE/vitagl-arb-sync.patch"     && echo "vitaGL: scene-fence patch applied"     || echo "vitaGL: WARNING scene-fence patch did NOT apply -- pipelining will regress"

# OUR FORK, second patch: scissor dedupe.
#
# Upstream marks the scissor dirty on every glEnable/glDisable(GL_SCISSOR_TEST)
# and glScissor, changed or not, and the next draw then issues one or two hidden
# full-target GXM draws to rebuild the mask. Stairfax re-applies its scissor
# before every draw, so it paid that on every draw. The patch dirties only on a
# real change and re-arms an active scissor once per scene.
#
# Measured on hardware, Stairfax runs 176-178 (docs/51 section 8): gameplay
# 56.4 -> 53.1 / 53.4 ms a frame, glDrawElements 33.7 -> 21.5 us a draw,
# letterbox verified by captures. An app that never touches the scissor (the
# renderer, vita-blade) is unaffected either way.
git apply --check "$HERE/vitagl-scissor-dedupe.patch" 2>/dev/null     && git apply "$HERE/vitagl-scissor-dedupe.patch"     && echo "vitaGL: scissor-dedupe patch applied"     || echo "vitaGL: WARNING scissor-dedupe patch did NOT apply -- Stairfax pays hidden scissor draws on every draw again"

# OUR FORK, third patch: vertex-program memo (Stairfax, 2026-10-14, docs/47 runs 269-272).
#
# Upstream calls sceGxmShaderPatcherCreateVertexProgram on EVERY glDrawElements,
# even when the program's attribute/stream layout is the one it was patched for
# last time. The patch remembers (shader id, attributes, streams) -> vprog per
# program and skips the patcher on a match. vitaGL never releases a patched
# vertex program, so the remembered pointer stays valid; the memo is cleared on
# glLinkProgram / glDeleteProgram. OFF by default (`int vgl_vprog_memo = 0` =
# upstream behaviour) until an A/B on hardware says what it buys; Stairfax
# turns it on per arm. Counters: vgl_vprog_memo_hits / _misses.
git apply --check "$HERE/vitagl-vprog-memo.patch" 2>/dev/null     && git apply "$HERE/vitagl-vprog-memo.patch"     && echo "vitaGL: vprog-memo patch applied"     || echo "vitaGL: WARNING vprog-memo patch did NOT apply -- Stairfax links vgl_vprog_memo and will FAIL to link"

make clean >/dev/null 2>&1 || true
# NO_DEBUG=1 strips the error-handling paths; this is the shipping config.
make -j4 NO_DEBUG=1
make install
cd "$ROOT"

echo
echo "=== installed ==="
ls -la "$VITASDK/arm-vita-eabi/lib/libvitaGL.a" \
       "$VITASDK/arm-vita-eabi/lib/libvitashark.a" \
       "$VITASDK/arm-vita-eabi/lib/libmathneon.a" \
       "$VITASDK/arm-vita-eabi/lib/libSceShaccCgExt.a"
echo
echo "On-device requirement: libshacccg.suprx in ur0:/data/ (see docs/04)."
echo "Without it vitaGL links fine and fails at runtime when it compiles a shader."
