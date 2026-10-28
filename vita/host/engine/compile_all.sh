#!/bin/bash
# compile_all.sh -- compile EVERY game source (src/main, src/dlls, src/track) and the
# port layer for PS Vita, one object per file, J=4 (shared machine: see memory
# multi-agent-machine-limits). Output: $OUT/obj/*.o, $OUT/log/*.log, $OUT/fails.txt.
# The objects feed closure.py, which builds an exact nm definition index from them.
export VITASDK=/usr/local/vitasdk PATH=/usr/local/vitasdk/bin:$PATH
. "$(dirname "${BASH_SOURCE[0]}")/paths.sh"   # V (this project, from this file's location), R (STFX_SRC), OUT, C3 -- BUILDING.md
P=$R/src/port
# The retail revision the game source is compiled AS (upstream configure.py: -DVERSION_<v>
# -DBUILD_VERSION=<index in VERSIONS>). 59 upstream files branch on it since 2026-09; with no macro
# they silently take the #else (PAL) branch. The Vita disc is GSAE01 Rev 1.
[ -d $R/src/MSL_C ] && DEFV=GSAE01_rev1 || DEFV=GSAE01   # same rule as stfx_paths.VERSION
STFX_VERSION=${STFX_VERSION:-$DEFV}
case $STFX_VERSION in GSAE01) VN=0;; GSAJ01) VN=1;; GSAP01) VN=2;; GSAE01_rev1) VN=3;; GSAP01_rev1) VN=4;;
  *) echo "unknown STFX_VERSION $STFX_VERSION"; exit 2;; esac
export VFLAGS="-DVERSION_$STFX_VERSION -DBUILD_VERSION=$VN"
mkdir -p $OUT/obj $OUT/log
# src/main/boot_logo.c #includes gLoadingScreenTextures.inc: extracted from the BUILDER's own disc (STFX_ISO / STAIRFAX_ISO, default
# <STFX_SRC>/orig/GSAE01/sfa_rev1.iso) into $OUT/gen at build time, never into the source tree. The file is a build intermediate
# (boot_logo.o is compiled but not linked: closure.py does not seed it, and retail_gate.py checks the ELF). Without a disc boot_logo.c
# simply fails to compile, as it always did before the .inc existed.
[ -f $OUT/gen/gLoadingScreenTextures.inc ] || python "$(cygpath -w $V/host/engine/gen_embedded.py)" "$(cygpath -m $OUT/gen)" || echo "compile_all: no gLoadingScreenTextures.inc (no disc image?) -- boot_logo.c will fail to compile"
export R P V OUT
{ find $R/src/main $R/src/dlls $R/src/track -name '*.c' 2>/dev/null
  ls $R/src/dolphin/MSL_C/PPCEABI/bare/H/math_80292*.c $R/src/dolphin/MSL_C/PPCEABI/bare/H/math_80293*.c $R/src/dolphin/MSL_C/PPCEABI/bare/H/math_80294*.c 2>/dev/null  # game MSL math: portable C (closure ALLOWED_DOLPHIN)
  # upstream 2026-09 moved MSL (and the game's own math from src/main) to src/MSL_C/. All of it is
  # compiled; closure.py bans src_MSL_C_* except ALLOWED_MSL, so newlib keeps libc.
  [ -d $R/src/MSL_C ] && find $R/src/MSL_C -name '*.c'
  echo $R/src/dolphin/axfx/reverb_std_create.c   # portable C (closure ALLOWED_DOLPHIN)
  # MusyX: EXACTLY the decomp's configure.py list (snd3dgroup.c #includes snd3d*.c, which
  # are never units of their own). Globbing the directory would compile them twice.
  grep -o '"musyx/runtime/[^"]*"' $R/configure.py | tr -d '"' | sort -u | sed "s|^|$R/src/|"
  find $P/src/hal $P/src/bridge $P/src/scaffold $P/src/data -name "*.c" -o -name "*.cpp" -o -name "*.S" 2>/dev/null | grep -v '/hal/renderer/rhi_\(d3d\|vulkan\|gl\)'
} | sort > $OUT/all_srcs.txt
# .c units the decomp compiles as C++ (configure.py `-lang=c++`, e.g. dlls/engine/2/2.c opens with
# extern "C" {). Read from configure.py so the list follows upstream; absent -> empty.
python -c "
import re,sys
s=open(sys.argv[1],encoding='utf-8').read()
for m in re.finditer(r'Object\((?:[^()]|\([^()]*\))*\)', s):
    o=m.group(0); f=re.search(r'\"([^\"]+\.c)\"', o)
    if f and '-lang=c++' in o: print(f.group(1))
" "$(cygpath -m $R/configure.py)" 2>/dev/null | sed "s|^|$R/src/|" > $OUT/cxx_srcs.txt   # $R-relative: all_srcs.txt is /d/..., python would print D:/...
export CXX_LIST=$OUT/cxx_srcs.txt
one() {
  f=$1
  id=$(echo "$f" | sed "s|$R/||; s|[/.]|_|g")
  case $f in *.cpp) cc="arm-vita-eabi-g++ -std=gnu++17" ;; *) cc="arm-vita-eabi-gcc -std=gnu11" ;; esac
  # -Wno-narrowing: MWCC C++ takes `u32 x[] = {-1}` as 0xFFFFFFFF (2.c's tables); that is the value
  grep -qxF "$f" "$CXX_LIST" && cc="arm-vita-eabi-g++ -std=gnu++17 -x c++ -Wno-narrowing"
  # Write-gather-pipe STORES become CALLS (wgpipe_rewrite.py): the Vita has no fault trap
  # for 0xCC008000. The rewritten copy compiles with -iquote to the original directory.
  src=$f; extra=
  # Game TUs: no auto-vectorization. The vectorizer may RAISE the alignment of arrays a TU defines
  # and emit aligned NEON accesses ([rN :64]); the retail-layout .bss block (gen_retail_blocks.py
  # --all) can only promise RETAIL alignment. Vita run 18: vst1.64 [r2 :64] at 0x...3c in
  # initTextures. build_game_vita.sh refuses any aligned NEON access in a game object.
  # -fno-toplevel-reorder: keep each TU's globals in SOURCE order. Upstream's tree matches retail
  # data layout per TU (2026-09), so source order IS retail order, and code that reaches a
  # neighbour through a struct over a smaller global (overlay_gate.py: audio_stream.c's
  # (AudioStreamDataLayout*)gAudioStreamFadeTable spans the next string) gets the retail neighbour.
  case $f in $P/*) ;; *) extra="-fno-tree-vectorize -fno-toplevel-reorder" ;; esac
  if [ "${f##*.}" = c ] && [ "${f#$P/}" = "$f" ] && grep -qE "GXWGFifo|0[xX][cC][cC]008000|^[[:space:]]*asm[[:space:]]|\basm[[:space:]]*\{" "$f"; then   # + MWCC asm (wgpipe_rewrite.py strip_mwcc_asm)   # game sources only, never src/port
    mkdir -p $OUT/rw
    python "$(cygpath -w $V/host/engine/wgpipe_rewrite.py)" "$(cygpath -m $f)" "$(cygpath -m $OUT/rw/$id.c)" > $OUT/log/$id.rw.log 2>&1       || { cat $OUT/log/$id.rw.log > $OUT/log/$id.log; rm -f $OUT/obj/$id.o; echo "$f"; return; }
    src=$OUT/rw/$id.c; extra="$extra -iquote $(dirname $f)"
  fi
  # -fsigned-char: the original builds with Metrowerks "-char signed" (configure.py cflags_game); ARM GCC defaults plain char to UNSIGNED.
  if $cc -O2 -fno-aggressive-loop-optimizations -fsigned-char -fno-strict-aliasing -fno-section-anchors -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DTARGET_PC=1 -DSTAIRFAX_PORT=1 $VFLAGS -Wno-error=incompatible-pointer-types \
       -I$P/include -I$V/src -I$OUT/gen $extra -idirafter $R/include -idirafter $R -w -c "$src" -o $OUT/obj/$id.o > $OUT/log/$id.log 2>&1
  then rm -f $OUT/log/$id.log; else rm -f $OUT/obj/$id.o; echo "$f"; fi
}
export -f one
if [ -n "${ONLY:-}" ]; then
  # ONLY=<file listing absolute source paths, as all_srcs.txt spells them>: recompile just those (2026-10-28). fails.txt keeps
  # every other unit's verdict; the listed units' verdicts are replaced. A listed unit that fails loses its object (as in a full run).
  xargs -a "$ONLY" -P 4 -I{} bash -c 'one "$@"' _ {} > $OUT/fails_only.txt
  { grep -vxF -f "$ONLY" $OUT/fails.txt 2>/dev/null; cat $OUT/fails_only.txt; } | sort -u > $OUT/fails.tmp && mv $OUT/fails.tmp $OUT/fails.txt
  echo "ONLY: $(wc -l < "$ONLY") sources, failed $(wc -l < $OUT/fails_only.txt)"; cat $OUT/fails_only.txt
  [ -s $OUT/fails_only.txt ] && exit 12
  exit 0
fi
xargs -a $OUT/all_srcs.txt -P 4 -I{} bash -c 'one "$@"' _ {} > $OUT/fails.txt
echo "sources: $(wc -l < $OUT/all_srcs.txt)  objects: $(ls $OUT/obj | wc -l)  failed: $(wc -l < $OUT/fails.txt)"
# A whole-tree failure must not exit 0 (2026-09-24: MSYS_NO_PATHCONV=1 in the caller's env made
# every cc1 miss its /d/... input -- 908 of 908 failed, and the script still reported success).
# Baseline is 1 known failure; more than 5 is a broken build, not a bad TU.
# src/MSL_C (upstream's 2026-09 layout) is counted apart: it is MSL libc, which newlib replaces and
# closure.py bans except ALLOWED_MSL; a needed file that fails shows up there as an undefined symbol.
MSLF=$(grep -c '/src/MSL_C/' $OUT/fails.txt); GAMEF=$(grep -vc '/src/MSL_C/' $OUT/fails.txt)
echo "failed outside src/MSL_C: $GAMEF  (src/MSL_C: $MSLF)"
[ "$GAMEF" -le 5 ] || { echo "compile_all: $GAMEF failures outside src/MSL_C -- refusing (exit 12)"; exit 12; }
