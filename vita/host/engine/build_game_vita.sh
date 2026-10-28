#!/bin/bash
# build_game_vita.sh -- package THE GAME (the decomp's own main(), linked by closure.py)
# as a Vita eboot.bin. Run closure.py first: this consumes its link list verbatim.
#
#   bash projects/stairfax-vita/host/engine/build_game_vita.sh      (MSYS2 bash)
#
# 1. the game's main() is renamed stfx_game_main in a COPY of its object: the wrapper
#    (src/vt_game.c -> vita_tool_main.c) owns the process entry -- log redirection,
#    STAIRFAX_ISO, the run limit, and the stop hook that ends a loop which never returns;
# 2. link = closure's list + the wrapper, with the same --wrap set the viewers use;
# 3. vita-elf-create + vita-make-fself -c (COMPRESSED: FTPVita refuses the uncompressed
#    eboot -- memory vita-deploy-artifact-traps).
set -e
export VITASDK=/usr/local/vitasdk PATH=/usr/local/vitasdk/bin:$PATH
. "$(dirname "${BASH_SOURCE[0]}")/paths.sh"   # V (this project, from this file's location), R (STFX_SRC), OUT, C3 -- BUILDING.md
P=$R/src/port
G=$OUT/game; mkdir -p "$G"

LIST=$OUT/link_objs.txt
[ -f "$LIST" ] || { echo "no $LIST -- run closure.py first"; exit 2; }
# the link list must be newer than every Vita platform source it was built with
# (2026-10-19, build 287: closure.py died on rhi_vita.c -- a missing include dir for an .inc it pulls in -- and this check, which
# only looked at the .c files, let the link use build 286's rhi_vita.o; the C3/B1 hooks bound weak to 0. The .inc/.h it includes count too.)
for f in "$V/src/plat_window_vita.c" "$V/src/rhi_vita.c" "$V/src/rhi_gxm.c" "$V"/src/rhi_gxm_*.c "$V"/src/rhi_vita*.inc "$V"/src/*.h "$C3"/c3_cuts.*; do
  [ -e "$f" ] || continue
  [ "$LIST" -nt "$f" ] || { echo "STALE: $f is newer than $LIST -- rerun closure.py"; exit 3; }
done

# The game object that DEFINES main (gameloop_main.c before upstream's 2026-09 rewrite, gameloop.c
# after): found by its symbol, not its file name. Exactly one, or refuse.
MAINOBJ=$(for o in $(grep 'src_main_' "$LIST"); do if arm-vita-eabi-nm "$o" 2>/dev/null | grep -q ' T main$'; then echo "$o"; fi; done)   # if/then: under set -e a trailing failed && would abort here, silently
[ "$(echo "$MAINOBJ" | grep -c .)" -eq 1 ] || { echo "want exactly one game object defining main, got: $MAINOBJ"; exit 4; }
arm-vita-eabi-objcopy --redefine-sym main=stfx_game_main "$MAINOBJ" "$G/gameloop_main.o"
arm-vita-eabi-nm "$G/gameloop_main.o" | grep -q ' T stfx_game_main$' || { echo "rename failed"; exit 5; }
grep -vF "$MAINOBJ" "$LIST" > "$G/link.txt"
cygpath -m "$G/gameloop_main.o" >> "$G/link.txt"   # ld is native: D:/ paths, not /d/

# BUILD ID (milestone M1): the game prints "[build] id=<this>" at boot (vita_tool_main.c) and the end of this script writes the same
# id plus the eboot's sha256 to $G/build_id.txt, so a game log can be bound to the eboot that produced it (tools/stfx_gxm_check.py).
STFX_BUILD_ID="stfx-$(date -u +%Y%m%dT%H%M%SZ)-$(cat "$V"/src/rhi_gxm*.c "$V/src/rhi_gxm_int.h" | sha256sum | cut -c1-8)-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"
arm-vita-eabi-gcc -std=gnu11 -O2 -fno-strict-aliasing -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -DTARGET_PC=1 -DSTAIRFAX_PORT=1 -DSTFX_BUILD_ID="\"$STFX_BUILD_ID\"" -I"$P/include" -I"$V/src" -idirafter "$R/include" \
  -c "$V/src/vt_game.c" -o "$G/vt_game.o"
cygpath -m "$G/vt_game.o" >> "$G/link.txt"
# The per-scene GPU-time instrument (src/scene_gpu.c; off on the console unless scene_gpu.txt asks). It holds the
# __wrap_sceGxmBeginScene/EndScene the WRAPS below name, so it is built and linked HERE, with the wrapper, not by
# closure.py (whose trial link has no --wrap). The backend (rhi_vita.c) calls nothing in it.
arm-vita-eabi-gcc -std=gnu11 -O2 -fno-strict-aliasing -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -Wall -I"$V/src" -c "$V/src/scene_gpu.c" -o "$G/scene_gpu.o"
cygpath -m "$G/scene_gpu.o" >> "$G/link.txt"
# The sceGxm call timers (src/gxm_stall.c; docs/54 R15; off on the console unless gxm_stall.txt asks). Same reason as scene_gpu.c: it defines
# __wrap_sceGxmDraw / DrawInstanced / Finish / NotificationWait and calls __real_X, which only resolve under the --wrap flags below, so it is
# built and linked HERE and not by closure.py. -Werror: it is a measurement instrument and a warning in it is a wrong number.
arm-vita-eabi-gcc -std=gnu11 -O2 -fno-strict-aliasing -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -Wall -Wextra -Werror -I"$V/src" -c "$V/src/gxm_stall.c" -o "$G/gxm_stall.o"
cygpath -m "$G/gxm_stall.o" >> "$G/link.txt"
# The GX-boundary record-and-discard wrappers (src/gx_rec.c + its generated gx_rec_wraps.inc; docs/53 T0-a). Same reason as
# scene_gpu.c: it defines __wrap_X and calls __real_X, which only resolve under --wrap, and closure.py's trial link has none, so
# it is built and linked HERE. -fsigned-char = the game's compile flags (the wrappers take the game's `char` arguments).
# Wrappers are inert (one branch + tail call) until a perf_ab arm 10/11 turns them on. The --wrap list is gx_wraps.txt, GENERATED
# from the same table (host/engine/gen_gx_rec.py), never typed here: its lint fails on a hand-written GX/VI/gxfifo --wrap.
arm-vita-eabi-gcc -std=gnu11 -O2 -fno-strict-aliasing -fsigned-char -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -DTARGET_PC=1 -DSTAIRFAX_PORT=1 -Wall -Wextra -Werror -I"$P/include" -I"$V/src" -idirafter "$R/include" -idirafter "$R" \
  -c "$V/src/gx_rec.c" -o "$G/gx_rec.o"
cygpath -m "$G/gx_rec.o" >> "$G/link.txt"
# THE GX RENDER THREAD (docs/53 revision 2, host/engine/RT_THREAD.md; default DIRECT: no marker = no thread, no ring). Two more modules, compiled exactly like
# gx_rec.c and -Werror (a warning in the ring is a wrong number): gx_thread.c (the SPSC ring, R's loop, the mode switch, the [gxt] instruments) and
# gx_rt_special.c (the custom rows: display lists, texture objects, lifetimes, fences). gx_rec.o references g_gxt / gxt_* in them STRONGLY (inline helpers in
# gx_rt.h), so they must be in every link that has gx_rec.o: a stale or missing one is a link error here, not a weak 0 (the symbol check below also names them).
for m in gx_thread gx_rt_special gx_rt_filter gx_rt_state gx_rt_split; do   # gx_rt_split.c = R5, the render-thread split (host/engine/R5_SPLIT.md; its control hooks are WEAK in gx_thread.c / rhi_gxm_split.c, so the R5 symbol gate below names them). gx_rt_state.c = the R2 state record (host/engine/R2_STATE.md; its hooks are WEAK in gx_thread.c / gx_rt.h, so a stale or missing gx_rt_state.o would silently run the legacy path: the symbol check below names gxs_flush / gxs_r_apply; boot line `state=1`). gx_rt_filter.c = the G-side redundant-state filter (its hooks are WEAK in gx_thread.c / gx_rt.h: a missing or stale gx_rt_filter.o would silently run unfiltered, so it is built and linked here with the others; boot line `filt=1` proves it)
  arm-vita-eabi-gcc -std=gnu11 -O2 -fno-strict-aliasing -fsigned-char -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
    -DTARGET_PC=1 -DSTAIRFAX_PORT=1 -Wall -Wextra -Werror -I"$P/include" -I"$V/src" -I"$P/src/hal" -idirafter "$R/include" -idirafter "$R" \
    -c "$V/src/$m.c" -o "$G/$m.o"
  cygpath -m "$G/$m.o" >> "$G/link.txt"
done

LIBS='-lvitaGL -lvitashark -lSceShaccCgExt -lstdc++ -lSceShaccCg_stub -ltaihen_stub -lmathneon
  -lSceGxm_stub -lSceCommonDialog_stub -lSceAppMgr_stub -lSceSysmodule_stub -lSceKernelDmacMgr_stub
  -lSceDisplay_stub -lSceCtrl_stub -lSceSysmem_stub -lSceLibKernel_stub -lSceKernelThreadMgr_stub
  -lSceAudio_stub -lScePower_stub -lm'
WRAPS='-Wl,--wrap=rhi_create -Wl,--wrap=rhi_present
  -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=fprintf -Wl,--wrap=puts -Wl,--wrap=putchar
  -Wl,--wrap=vfprintf -Wl,--wrap=fwrite -Wl,--wrap=fputs -Wl,--wrap=fputc
  -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=abort -Wl,--wrap=__assert_func
  -Wl,--wrap=loadFileByPathAsync -Wl,--wrap=sndPushGroup -Wl,--wrap=sndSeqPlayEx
  -Wl,--wrap=shark_compile_shader_extended -Wl,--wrap=shark_clear_output
  -Wl,--wrap=sceGxmBeginScene -Wl,--wrap=sceGxmEndScene
  -Wl,--wrap=sceGxmDraw -Wl,--wrap=sceGxmDrawInstanced -Wl,--wrap=sceGxmFinish -Wl,--wrap=sceGxmNotificationWait'
# the GX/VI/gxfifo --wrap set, from the generated table (gx_wraps.txt); the table, the wrappers and this list are one source
python "$(cygpath -w "$V/host/engine/gen_gx_rec.py")" prelink || { echo "GATE: gx_rec table / generated files / wrap list disagree -- refusing to link (exit 13)"; exit 13; }
WRAPS="$WRAPS $(tr -d '\r' < "$V/host/engine/gx_wraps.txt" | tr '\n' ' ')"
echo "$WRAPS" > "$G/wraps_used.txt"     # the EFFECTIVE --wrap set of this link, for the postlink lint
arm-vita-eabi-g++ -Wl,-q @"$G/link.txt" -o "$G/stairfax_game.elf" $WRAPS $LIBS
arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -q ' T main$' || { echo "no wrapper main"; exit 6; }
# every boundary symbol: __wrap_X defined, X defined, no __real_ left undefined, the link passed the table's --wrap set, and some
# call site really reaches a wrapper (gen_gx_rec.py postlink; watched failing by gen_gx_rec.py selftest + the T0-a session)
python "$(cygpath -w "$V/host/engine/gen_gx_rec.py")" postlink "$(cygpath -m "$G/stairfax_game.elf")" --wraps-used "$(cygpath -m "$G/wraps_used.txt")" --gx-rec-o "$(cygpath -m "$G/gx_rec.o")" \
  || { echo "GATE: the GX boundary wraps did not link as the table says -- refusing to package (exit 13)"; exit 13; }
# THE RENDER THREAD'S LINT (G-M1 / G-M2, RT_THREAD.md 6.2): the proof that G's bodies and R's bodies share no mutable state and that R reaches no game / OS symbol.
# It reads the HAL / backend objects (--objdir), this link's gx_thread.o / gx_rt_special.o (picked up from $G) and the ELF. Watched failing by gxt_lint.py selftest.
python "$(cygpath -w "$V/host/engine/gxt_lint.py")" lint --elf "$(cygpath -m "$G/stairfax_game.elf")" --objdir "$(cygpath -m "$OUT/obj")" --vobj "$(cygpath -m "$OUT/vobj")" --dup "$(cygpath -m "$OUT/dup_resolution.txt")" \
  || { echo "GATE: gxt_lint (G-M1/G-M2) found a split violation -- refusing to package (exit 15)"; exit 15; }
# NO RETAIL BYTES (2026-10-27, the public release): the game data the decomp does not commit is filled at boot from the PLAYER's disc
# (retail_runtime.py + dvd_shim.c stfx_retail_load). A re-run of gen_data.py / gen_pools.py without retail_runtime.py would embed it again.
# Watched failing on eboot 2dbc5504's ELF (14 runs found). The loader and its table must be linked (strong + weak references).
python "$(cygpath -w "$V/host/engine/retail_gate.py")" "$(cygpath -m "$G/stairfax_game.elf")" --src "$(cygpath -m "$R")"   || { echo "GATE: retail bytes in the ELF -- refusing to package (exit 16)"; exit 16; }
for s in stfx_retail_load stfx_retail_table stfx_retail_n; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -q " [TRDB] $s$" || { echo "GATE: $s not defined in the ELF -- the game would run on zeroed data (exit 16)"; exit 16; }
done
# the symbols the integration expects: a failed closure links a stale .o silently and the hooks bind weak to 0 (memory stale-vita-object)
for s in gxt_boot gxt_frame_edge gxt_present gxt_quit gxt_free gxt_r_exec gxt_stall_report gxt_is_active gx_rec_set_threaded gxm_issued_draws gxts_G_GXBegin gxts_refuse_reason gxs_flush gxs_begin gxs_end gxs_r_apply gxs_config gxs_print gxh_post_apply gxh_collect gxh_kdirty_set gxh_kdirty_all gx_r3a_set gxt_c3_arm gxt_r3a_edge gxt_r3a_mask_for gxm_shader_gx_hint gx_rsplit_times rhi_gxm_c6_census gxm_c6_flush gxm_c6_flush_tex gx_tev_build_uniforms gx_tev_replay_stats gxts_imm_census_flush; do   # hw_Rsplit: gx_rsplit_times (weak-referenced by vita_tool_main.c). R3a: the C0 / C3 hooks (all bind weak or strong across objects: a stale .o must not link silently)
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [TW] $s\$" || { echo "GATE: $s is not defined in the ELF -- a stale object or a failed closure (exit 15)"; exit 15; }
done
arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE ' [BbDdC] g_gxm_ptc$' || { echo "GATE: g_gxm_ptc (hw_Rsplit backend steps) is not in the ELF (exit 15)"; exit 15; }
# R3c (host/engine/R3C_CACHE.md): the census counters the [r3c] line reads and the C6 flags the sceGxm wrappers of rhi_gxm_int.h test are DATA symbols: a stale object leaves them undefined and the weak reads bind to 0 (the census would print zeros)
for s in gx_r3c_c5_total gx_r3c_c5_clean gx_r3c_c5_bumps gx_r3c_c7_vat gx_r3c_c7_key gx_r3c_c7_calls g_gxm_c6_pending g_gxm_c6_busy; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [BbDdC] $s\$" || { echo "GATE: $s (R3c) is not in the ELF (exit 15)"; exit 15; }
done
# R6 (host/engine/R6_SKIN.md): the C8 / C9 census counters (HAL, gx_draw.c) are DATA symbols the [r6] line reads weakly; the two-half mesh update (rhi_gxm_draw.c) is what the HAL binds weakly: a stale object leaves them undefined / null and C9 would silently take the old path
for s in gx_r6_c8_calls gx_r6_c8_unch gx_r6_c8_pred gx_r6_c8_hash gx_r6_c8_ovf gx_r6_c8_miss gx_r6_c9_part gx_r6_c9_fresh gx_r6_c9_ref; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [BbDdC] $s\$" || { echo "GATE: $s (R6) is not in the ELF (exit 15)"; exit 15; }
done
for s in rhi_gxm_gx_mesh_update_begin rhi_gxm_gx_mesh_update_commit; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [TW] $s\$" || { echo "GATE: $s (R6) is not defined in the ELF (exit 15)"; exit 15; }
done
# R5 (host/engine/R5_SPLIT.md): the split's control hooks bind WEAK across objects (gx_thread.c -> gx_rt_split.c, rhi_gxm.c -> rhi_gxm_split.c -> gx_rt_split.c), and the backend entry points it calls are new: a stale object would run the render thread WHOLE and the A/B would measure nothing.
for s in gxsp_g_enable gxsp_g_disable gxsp_g_drain gxsp_f_publish gxsp_f_present gxsp_f_quit gxsp_set_present_done gxsp_print gxsp_stall_report gxsp_cap_init gxsp_cap_attach gxsp_gxm_init rhi_gxm_drawTevGx_h rhi_gxm_verdict_gx rhi_gxm_verdict_tev rhi_gxm_set_r3a_ovr rhi_gxm_pure_layout_ok rhi_gxm_pure_mesh_check rhi_gxm_copy_precheck rhi_gxm_tex_max_dim rhi_gxm_gx_mesh_update_begin_real rhi_gxm_gx_mesh_update_commit_real; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [TW] $s\$" || { echo "GATE: $s (R5) is not defined in the ELF (exit 15)"; exit 15; }
done
for s in g_gxsp_cap g_gxsp_on g_gxsp_api g_gxm_r3a_ovr_on; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -qE " [BbDdC] $s\$" || { echo "GATE: $s (R5) is not in the ELF (exit 15)"; exit 15; }
done
echo "render-thread symbols: $(arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -cE ' T (gxt_|gxts_)') gxt_/gxts_ functions, data: $(arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -E ' [TWBbDd] (g_gxt|gxt_r_unsafe)$' | tr '\n' ' ')"
# G-D1, the named-diff register: the HAL and backend objects the thread must not touch are byte-identical to the baseline build (only N1, N2 and 3g may differ)
if [ -f "$OUT/rt_integ/base_obj.sha" ]; then
  python "$(cygpath -w "$V/host/engine/named_diff_gate.py")" || { echo "GATE: a HAL / backend object outside the named diffs changed -- refusing to package (exit 14)"; exit 14; }
fi
# every scene vitaGL makes must pass through the instrument's wraps (scene_gpu.c), or its rows are not the frame
arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -q ' T __wrap_sceGxmEndScene$' || { echo "no __wrap_sceGxmEndScene: scene_gpu.o not linked"; exit 12; }
# ...and every draw/finish/notification-wait vitaGL makes must pass through gxm_stall.c's wraps (or its [stall] line is not the frame): all four
# __wrap_ symbols defined AND the link was handed all four --wrap flags (without the flag the wrapper links but nothing calls it).
for s in sceGxmDraw sceGxmDrawInstanced sceGxmFinish sceGxmNotificationWait; do
  arm-vita-eabi-nm "$G/stairfax_game.elf" | grep -q " T __wrap_$s\$" || { echo "no __wrap_$s: gxm_stall.o not linked"; exit 12; }
  grep -q -- "--wrap=$s" "$G/wraps_used.txt" || { echo "link did not pass --wrap=$s"; exit 12; }
done
# No code may build the write-gather-pipe address: on the Vita one store there is a data
# abort. Watched failing: 29 sites in the pre-fix ELF, hudDrawRect (the crash) among them.
python "$(cygpath -w "$V/host/engine/wgpipe_gate.py")" "$(cygpath -m "$G/stairfax_game.elf")" \
  || { echo "GATE: write-gather-pipe stores remain -- refusing to package"; exit 7; }
# String-pool offsets must land on the SAME strings as retail (pool_gate.py, device run 6).
python "$(cygpath -w "$V/host/engine/pool_gate.py")" "$(cygpath -m "$G/stairfax_game.elf")"   || { echo "GATE: string-pool offsets differ from retail -- refusing to package"; exit 8; }
# No struct may be laid over a global the port holds smaller, and every retail neighbour the
# code reaches that way must sit at its retail offset (overlay_gate.py, device run 9).
# Watched failing on run 9's ELF: 26 flags, both run-9 overlays among them.
python "$(cygpath -w "$V/host/engine/overlay_gate.py")" "$(cygpath -m "$G/stairfax_game.elf")"   --allow "$(cygpath -w "$V/host/engine/overlay_open.txt")"   || { echo "GATE: struct overlays over short globals -- refusing to package"; exit 9; }
# Every byte-order ARRAY rule in mldf_endian.c must beat its other width on the real disc
# (mldf_rules_check.py; watched failing with --flip on MAPS.tab/MODANIM.TAB/OBJECTS.tab).
python "$(cygpath -w "$V/host/engine/mldf_rules_check.py")" > "$G/mldf_rules.txt"   || { cat "$G/mldf_rules.txt"; echo "GATE: a byte-order rule has the wrong width -- refusing to package"; exit 10; }

# No aligned NEON access in game code (compile_all.sh -fno-tree-vectorize): game .bss sits at
# RETAIL offsets, which guarantee only retail alignment. Watched failing: run 18's objects (85).
NEON=0
for o in $(grep -E 'src_(main|dlls|track)_' "$G/link.txt"); do
  c=$(arm-vita-eabi-objdump -d "$o" | grep -cE '\[r[0-9]+ ?:(64|128|256)\]' || true); NEON=$((NEON + c))
done
[ "$NEON" -eq 0 ] || { echo "GATE: $NEON aligned NEON accesses in game objects -- refusing to package"; exit 11; }
vita-elf-create "$G/stairfax_game.elf" "$G/stairfax_game.velf" > "$G/velf.log"
vita-make-fself -c "$G/stairfax_game.velf" "$G/eboot.bin" > "$G/fself.log"
ls -l "$G/eboot.bin"
sha256sum "$G/eboot.bin"
printf 'id=%s\neboot_sha256=%s\n' "$STFX_BUILD_ID" "$(sha256sum "$G/eboot.bin" | cut -d' ' -f1)" > "$G/build_id.txt"
cat "$G/build_id.txt"
