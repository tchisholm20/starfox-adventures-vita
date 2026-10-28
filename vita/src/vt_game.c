/* vt_game.c - vita_tool_main.c configured for THE GAME: the decomp's own main()
 * (src/main/gameloop_main.c), linked by host/engine/closure.py and packaged by
 * host/engine/build_game_vita.sh, which renames that main to stfx_game_main. */
#define VT_TOOL          stfx_game_main
#define VT_NAME          "game"
#define VT_LOG           "ux0:data/stairfax_game.txt"
#define VT_SHOT          "ux0:data/stairfax_game.bmp"
#define VT_ARGS          "ux0:data/stairfax/game_args.txt"
#define VT_DEFAULT_ARGS  "game"
#define VT_SETENV_ISO    1
/* The WHOLE disc as one image (dvd_shim mounts a GameCube ISO directly): the directory
 * held 8 of the disc's 4,070 files and run 7 logged 64+ misses. */
#define VT_DISC          "ux0:data/stairfax/sfa_rev1.iso"
#define VT_EXIT_ON_STOP  1   /* the game loops forever: the pump ends the process */
#define VT_WRAP_IO       1   /* log vfprintf/fwrite/fputs/fputc to the console + every exit route */
#define VT_WATCHDOG      300 /* s without a present -> deliberate snapshot fault. Was 20: run 20 measured the
                                first game frame as SLOW (24,265 display lists in 5 s, CPU GX decode), not stuck */
/* The first real game frame is present 5 (presents 1-4 are init). Capture it; run long enough. */
#define CAPTURE_AT       600   /* run 22: present 6 was solid black (still blanked?) -- look later */
#define TIME_FROM        300
#define RUN_SECONDS      86400   /* PLAY build (2026-09-24): no practical wall-clock limit */
#define RUN_FRAMES       100000000
#define VT_COLORED_HONORS_ALPHA 1 /* coloured draws take the game's GX blend mode (rhi_vita.c) */
#define VT_PRESENT_LOG   1   /* [frame] lines: dt + GX display-list work per present */
/* The game's heaps come from mm_shim's malloc (the console had 24 MB main RAM),
 * plus 16 MB of ARAM (ar_shim) and the loaded files. */
#define VT_HEAP_MB       128
#include "vita_tool_main.c"

/* The game's own state, once a second with the [frame] log (vita_tool_main.c calls this weak hook):
 * the title screen rendered but never advanced (run 34: identical GX work for 170,000 frames). Is
 * time frozen (timeDelta 0: e.g. gDvdErrorPauseActive), is input arriving, which state is it in? */
extern float timeDelta; extern unsigned char framesThisStep, gameState, gDvdErrorPauseActive;
extern int gDvdLastDriveStatus;
extern unsigned int getButtonsHeld(int port);   /* hal/pad_input.c: the game reads input through these */
#include <psp2/ctrl.h>
void vt_game_state_log(void)
{
    SceCtrlData raw; int got = sceCtrlPeekBufferPositive(0, &raw, 1);
    printf("[game] timeDelta %.3f framesThisStep %u gameState %u dvdErrorPause %u dvdStatus %d held %08x raw %08x(%d) lx %u ly %u\n",
           timeDelta, framesThisStep, gameState, gDvdErrorPauseActive, gDvdLastDriveStatus,
           getButtonsHeld(0), got > 0 ? raw.buttons : 0u, got, got > 0 ? raw.lx : 0, got > 0 ? raw.ly : 0);
    {   /* the title menu's gates (n_attractmode.c TitleMenu_run): movie state/playback, input-ready */
        extern int gAttractMovieState; extern unsigned char gAttractMoviePlaybackEnabled, gAttractMoviePreparePending,
            gTitleMenuReadyForInput, gAttractMovieAutoplayEnabled; extern int gTitleMenuInputCooldown;
        extern void** gCameraInterface;   /* CameraInterface**: getMode at vtable +0x10 (camera_interface.h) */
        int camMode = (gCameraInterface && *gCameraInterface) ? ((int (**)(void)) *gCameraInterface)[4]() : -1;
        printf("[title] camMode 0x%x ", camMode);
        printf("[title] movieState %d playback %u preparePending %u readyForInput %u autoplay %u cooldown %d\n",
               gAttractMovieState, gAttractMoviePlaybackEnabled, gAttractMoviePreparePending,
               gTitleMenuReadyForInput, gAttractMovieAutoplayEnabled, gTitleMenuInputCooldown);
        {   /* the THP player's own counters (2026-10-09, docs/47 "the frozen laptop"): PlayControl shows a decoded frame only
               while video frame - audio frame <= 1, and the audio frame number moves only when the DMA mixer pops a decoded audio
               buffer. Raw words of gAttractMoviePlayer at the offsets vt_thp_words reports. */
            extern void vt_thp_words(int out[8]) __attribute__((weak));
            if (vt_thp_words) { int w[8]; vt_thp_words(w);
                printf("[thp] state %d internalState %d | video frame %d audio frame %d | videoDecodeCount %d | audioExists %d playFlags %d retraceCount %d\n",
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
        }
    }
    {   /* the baton scheduler (hal/os_threads.c) and the THP decoder (hal/thp_decode.c), 2026-09-27;
           weak so the pre-merge tree (which has neither) still links */
        extern unsigned os_thread_switches __attribute__((weak)), os_deadlocks __attribute__((weak)),
            os_main_irq_waits __attribute__((weak)), ai_ticks __attribute__((weak)),
            ai_ticks_dropped __attribute__((weak)),
            thp_video_decode_calls __attribute__((weak)), thp_video_decode_errors __attribute__((weak)),
            gx_tex_uploads __attribute__((weak)), gx_tex_cache_entries __attribute__((weak)),
            gx_tex_redecodes __attribute__((weak)), gx_tex_released __attribute__((weak)),
            gx_tex_yuv_frames __attribute__((weak));
#define W_(v) (&(v) ? (v) : 0u)
        printf("[os] switches %u deadlocks %u main-irq-waits %u | ai ticks %u dropped %u | thp video decodes %u errors %u"
               " | tex uploads %u cached %u redecodes %u released %u yuv %u\n",
               W_(os_thread_switches), W_(os_deadlocks), W_(os_main_irq_waits), W_(ai_ticks), W_(ai_ticks_dropped),
               W_(thp_video_decode_calls), W_(thp_video_decode_errors),
               W_(gx_tex_uploads), W_(gx_tex_cache_entries), W_(gx_tex_redecodes), W_(gx_tex_released), W_(gx_tex_yuv_frames));
        {   /* texture wrap modes (rhi_vita.c rhi_vita_texSetWrap) and position-matrix texgens (gx_draw.c), 2026-10-11 */
            extern unsigned rhi_vita_wrap_tex[2][3] __attribute__((weak)), rhi_vita_wrap_changes __attribute__((weak)),
                rhi_vita_wrap_npot_downgrades __attribute__((weak)), gx_texgen_pn_cpu __attribute__((weak)), gx_texgen_pn_gpu __attribute__((weak));
            unsigned z[2][3] = { { 0 } }; const unsigned (*wt)[3] = rhi_vita_wrap_tex ? (const unsigned (*)[3]) rhi_vita_wrap_tex : (const unsigned (*)[3]) z;
            printf("[wrap] textures by applied mode (clamp/repeat/mirror): S %u/%u/%u T %u/%u/%u | GL changes %u | NPOT mirror->repeat %u"
                   " | [texgen] draws with a position-matrix texgen: CPU path %u, GPU path %u\n",
                   wt[0][0], wt[0][1], wt[0][2], wt[1][0], wt[1][1], wt[1][2], W_(rhi_vita_wrap_changes), W_(rhi_vita_wrap_npot_downgrades),
                   W_(gx_texgen_pn_cpu), W_(gx_texgen_pn_gpu));
        }
        { extern void stfx_audio_report(void) __attribute__((weak)); if (stfx_audio_report) stfx_audio_report(); }   /* two [audio] lines: hal/stfx_audio.c */
        {   /* the GX FIFO breakpoint and the depth probes it delivers (gx_draw.c, 2026-10-09, docs/47 "Run 157") */
            extern unsigned gx_bp_delivered __attribute__((weak)), gx_peekz_calls __attribute__((weak)),
                gx_peekz_real __attribute__((weak)), gx_peekz_near __attribute__((weak));
            { extern unsigned stfx_nested_presents __attribute__((weak)); printf("[vi] frames presented from the game's own frame loops (not main's): %u\n", W_(stfx_nested_presents)); }
            printf("[bp] breakpoints delivered %u | depth probes %u, from depth memory %u, nearer than far %u\n",
                   W_(gx_bp_delivered), W_(gx_peekz_calls), W_(gx_peekz_real), W_(gx_peekz_near));
        }
        {   /* EFB->texture copies (gx_draw.c GXCopyTex, 2026-09-28): made absent until reproduced */
            extern unsigned gx_copytex_calls __attribute__((weak)), gx_copy_draws_skipped __attribute__((weak)),
                gx_copy_clears __attribute__((weak)), rhi_vita_clear_rects __attribute__((weak)),
                gx_movie_loads __attribute__((weak)), gx_movie_draws __attribute__((weak)),
                gx_copy_draws_alt __attribute__((weak)), gx_copy_draws_screenfx __attribute__((weak)),
                gx_copy_real __attribute__((weak)), gx_copy_zfmt __attribute__((weak)), gx_copy_whole_screen __attribute__((weak)),
                gx_copy_unsupported __attribute__((weak)), gx_copy_real_kpx __attribute__((weak)),
                gx_copy_gpu __attribute__((weak)), gx_copy_mem __attribute__((weak)),
                rhi_vita_efb_mode __attribute__((weak)), rhi_vita_efb_gpu_copies __attribute__((weak)),
                rhi_vita_efb_refused_fmt __attribute__((weak)), rhi_vita_efb_switches __attribute__((weak)),
                rhi_vita_efb_alive __attribute__((weak)), rhi_vita_efb_hack __attribute__((weak)),
                gx_copy_z __attribute__((weak)), gx_copy_z_idle __attribute__((weak)),
                rhi_vita_efb_z_copies __attribute__((weak)), rhi_vita_efb_z_kpx __attribute__((weak));
            extern unsigned rhi_vita_efb_copy_below480 __attribute__((weak)), rhi_vita_efb_copy_clamped __attribute__((weak)),
                rhi_vita_efb_vp_below480 __attribute__((weak)), rhi_vita_efb_clears_below480 __attribute__((weak)),
                rhi_vita_efb_refused_conv __attribute__((weak)), rhi_vita_efb_refused_rect __attribute__((weak)),
                rhi_vita_efb_refused_new __attribute__((weak)), rhi_vita_efb_recreated __attribute__((weak)),
                rhi_vita_efb_fmt_changed __attribute__((weak)), rhi_vita_copyfilter_calls __attribute__((weak)),
                rhi_vita_copyfilter_nearest __attribute__((weak)), rhi_vita_copyfilter_changes __attribute__((weak)),
                gx_copy_dst_absent __attribute__((weak)), gx_copy_dst_table_full __attribute__((weak)),
                gx_copy_refused __attribute__((weak)),
                gx_copy_samplers __attribute__((weak));
            extern unsigned long long gx_copy_real_us __attribute__((weak));
            extern int gx_movie_last_map __attribute__((weak));
            extern unsigned gx_copy_draws_tevless __attribute__((weak)), gx_scissor_sets __attribute__((weak)),
                rhi_vita_scissor_changes __attribute__((weak));
            printf("[copy] copy draws dropped for want of a combiner %u | scissor sets %u (backend changes %u)\n",
                   W_(gx_copy_draws_tevless), W_(gx_scissor_sets), W_(rhi_vita_scissor_changes));
            printf("[copy] copytex %u draws skipped %u alt %u screenfx %u clears %u (backend %u)\n", W_(gx_copytex_calls),
                   W_(gx_copy_draws_skipped), W_(gx_copy_draws_alt), W_(gx_copy_draws_screenfx), W_(gx_copy_clears), W_(rhi_vita_clear_rects));
            printf("[copyreal] real %u (%u kpx read, %.1f ms total) | not done: z %u whole-screen %u unsupported %u\n", W_(gx_copy_real),
                   W_(gx_copy_real_kpx), (&gx_copy_real_us ? gx_copy_real_us : 0ull) / 1000.0, W_(gx_copy_zfmt), W_(gx_copy_whole_screen), W_(gx_copy_unsupported));
            printf("[efb] mode %u | gpu copies %u memory-path copies %u refused-by-format %u | fbo switches %u copy textures alive %u | depth hack %u\n",
                   W_(rhi_vita_efb_mode), W_(gx_copy_gpu), W_(gx_copy_mem), W_(rhi_vita_efb_refused_fmt), W_(rhi_vita_efb_switches),
                   W_(rhi_vita_efb_alive), W_(rhi_vita_efb_hack));
            printf("[efb] 528 lines: copies ending below line 480 %u, CLAMPED %u (want 0 with the fix on) | viewports below 480 %u clears below 480 %u |"
                   " copy kinds: refused (no conversion %u, empty rect %u, no texture %u; backend NULLs seen by gx %u) destinations marked absent %u (table full %u) draws skipped for an absent copy %u"
                   " re-created at another size %u other format %u | copy sampler: calls %u nearest %u GL changes %u (gx binds %u)\n",
                   W_(rhi_vita_efb_copy_below480), W_(rhi_vita_efb_copy_clamped), W_(rhi_vita_efb_vp_below480), W_(rhi_vita_efb_clears_below480),
                   W_(rhi_vita_efb_refused_conv), W_(rhi_vita_efb_refused_rect), W_(rhi_vita_efb_refused_new), W_(gx_copy_refused),
                   W_(gx_copy_dst_absent), W_(gx_copy_dst_table_full), W_(gx_copy_draws_skipped),
                   W_(rhi_vita_efb_recreated), W_(rhi_vita_efb_fmt_changed),
                   W_(rhi_vita_copyfilter_calls), W_(rhi_vita_copyfilter_nearest), W_(rhi_vita_copyfilter_changes), W_(gx_copy_samplers));
            printf("[efbz] z8 copies %u (backend %u, %u kpx resolved) | whole-screen idle (nothing samples it) %u | z not done %u\n",
                   W_(gx_copy_z), W_(rhi_vita_efb_z_copies), W_(rhi_vita_efb_z_kpx), W_(gx_copy_z_idle), W_(gx_copy_zfmt));
            printf("[movie] plane loads %u draws that sampled it %u last map %d\n", W_(gx_movie_loads), W_(gx_movie_draws),
                   &gx_movie_last_map ? gx_movie_last_map : -9);
        }
        {   /* TEV programs (rhi_vita_tev.inc, hal/gx_draw.c drawTev, 2026-09-28): every fallback counted */
            extern unsigned rhi_vita_tev_programs __attribute__((weak)), rhi_vita_tev_compile_fail __attribute__((weak)),
                rhi_vita_tev_bad_skipped __attribute__((weak)), rhi_vita_tev_refused __attribute__((weak)),
                rhi_vita_tev_table_full __attribute__((weak)), rhi_vita_tev_draws __attribute__((weak)),
                rhi_vita_tev_batches __attribute__((weak)), gx_tev_draws __attribute__((weak)),
                gx_tev_fb_state __attribute__((weak)), gx_tev_fb_notex __attribute__((weak)),
                gx_tev_fb_copytex __attribute__((weak)), gx_tev_fb_backend __attribute__((weak)),
                gx_tev_texgen_unsupported __attribute__((weak)), gx_tev_refused_indirect __attribute__((weak)),
                gx_tev_refused_slots __attribute__((weak));
            printf("[tevp] programs %u (link fail %u, bad-list %u, refused %u, table full %u) | draws %u batches %u |"
                   " fallback: state %u (indirect %u, slots %u) notex %u copytex %u backend %u | texgen unsupported %u\n",
                   W_(rhi_vita_tev_programs), W_(rhi_vita_tev_compile_fail), W_(rhi_vita_tev_bad_skipped), W_(rhi_vita_tev_refused),
                   W_(rhi_vita_tev_table_full), W_(gx_tev_draws), W_(rhi_vita_tev_batches), W_(gx_tev_fb_state),
                   W_(gx_tev_refused_indirect), W_(gx_tev_refused_slots), W_(gx_tev_fb_notex), W_(gx_tev_fb_copytex),
                   W_(gx_tev_fb_backend), W_(gx_tev_texgen_unsupported));
        }
#undef W_
        { extern void vt_print_gametext_slots(void) __attribute__((weak)); /* hal/stfx_debug.c: game headers */
          if (vt_print_gametext_slots) vt_print_gametext_slots(); }
        { extern void vt_print_menu_state(void) __attribute__((weak)); if (vt_print_menu_state) vt_print_menu_state(); }
        { extern void vt_print_start_at(void) __attribute__((weak)); if (vt_print_start_at) vt_print_start_at(); }   /* hal/stfx_start_at.c: what start_at.txt made the game do (3 lines at most) */
        {   /* gx_draw.c's sampled phase timers (1 primitive in 8, scaled here): cumulative ms */
            extern unsigned long long gx_t_decode __attribute__((weak)), gx_t_light __attribute__((weak)),
                gx_t_submit __attribute__((weak));
            extern unsigned gx_prims __attribute__((weak)), gx_prim_verts __attribute__((weak));
            if (&gx_t_decode)
                printf("[gxt] cumulative ms (x8 sampled): decode %.1f light %.1f submit %.1f | prims %u verts %u\n",
                       gx_t_decode * 8 / 1000.0, gx_t_light * 8 / 1000.0, gx_t_submit * 8 / 1000.0,
                       gx_prims, gx_prim_verts);
            {   /* the submit split (2026-10-07): gx_draw's parts x8 sampled; the flush timers are exact */
                extern unsigned long long gx_t_sub_key __attribute__((weak)), gx_t_sub_texgen __attribute__((weak)),
                    gx_t_sub_expand __attribute__((weak)), gx_t_sub_backend __attribute__((weak)),
                    rhi_vita_tev_flush_us __attribute__((weak)), rhi_vita_tev_gldraw_us __attribute__((weak)),
                    rhi_vita_tev_lookup_us __attribute__((weak)), rhi_vita_tev_batch_us __attribute__((weak));
                extern unsigned rhi_vita_tev_flush_verts __attribute__((weak));
                if (&gx_t_sub_key && &rhi_vita_tev_flush_us)
                    printf("[gxsub] cumulative ms: key %.1f texgen %.1f expand %.1f backend %.1f (x8 sampled) | in backend: lookup %.1f batch %.1f (x8 sampled) | flush %.1f of which glDrawArrays %.1f (exact, %u verts)\n",
                           gx_t_sub_key * 8 / 1000.0, gx_t_sub_texgen * 8 / 1000.0, gx_t_sub_expand * 8 / 1000.0, gx_t_sub_backend * 8 / 1000.0,
                           rhi_vita_tev_lookup_us * 8 / 1000.0, rhi_vita_tev_batch_us * 8 / 1000.0,
                           rhi_vita_tev_flush_us / 1000.0, rhi_vita_tev_gldraw_us / 1000.0, rhi_vita_tev_flush_verts);
            }
            { extern void gx_dlc_report(void) __attribute__((weak)); if (gx_dlc_report) gx_dlc_report(); }   /* the display-list census (dl_census.txt) */
            { extern void rhi_vita_gx_report(void) __attribute__((weak)); if (rhi_vita_gx_report) rhi_vita_gx_report(); }   /* the GPU vertex path's programs and draws */
            { extern void gx_dlk_report(void) __attribute__((weak)); if (gx_dlk_report) gx_dlk_report(); }   /* the per-list cache (docs/47 fast path stage 1; dl_nocache.txt = off) */
        }
    }
}

/* Every file the game asks for BY PATH, and what it got (2026-09-27: the title menu drew no text
 * and no game-text file was ever finalised -- was one ever requested?). Link-time --wrap
 * (build_game_vita.sh): the callers (gametext.c ...) are in other objects than fileio.c. */
typedef void (*StfxDvdCb)(int, void *);
void *__real_loadFileByPathAsync(char *path, int *outSize, int unused, StfxDvdCb cb);
void *__wrap_loadFileByPathAsync(char *path, int *outSize, int unused, StfxDvdCb cb)
{
    int sz = 0;   /* audio.c passes NULL for its sound files; fileio.c:172,198 fill whatever it is given */
    void *r = __real_loadFileByPathAsync(path, outSize ? outSize : &sz, unused, cb);
    if (outSize) sz = *outSize;
    printf("[load] async %s -> %p (%d bytes)\n", path ? path : "(null)", r, sz);
    {   /* MusyX sound data is big-endian on the disc: swapped here, once per load (hal/stfx_audio.c -> musyx_endian.c) */
        extern void stfx_audio_file_loaded(const char *, void *, int) __attribute__((weak));
        if (stfx_audio_file_loaded && r && path) stfx_audio_file_loaded(path, r, sz);
    }
    return r;
}

/* A group or a song reaches MusyX only if its data was swapped (stfx_audio_group_gate / _song_gate): the old shim's
 * "decline every group" is now a decision per call. Refused = the return value audio.c already handles
 * (audio.c:674-677 reports a failed group; a song handle of -1 is what it has always been given). */
int __real_sndPushGroup(void *prj, unsigned short gid, void *samples, void *sdir, void *pool);
int __wrap_sndPushGroup(void *prj, unsigned short gid, void *samples, void *sdir, void *pool)
{
    extern int  stfx_audio_group_gate(const void *, const void *, const void *) __attribute__((weak));
    extern void stfx_audio_group_result(int) __attribute__((weak));
    int r;
    if (!stfx_audio_group_gate || !stfx_audio_group_gate(prj, sdir, pool)) return 0;
    r = __real_sndPushGroup(prj, gid, samples, sdir, pool);
    if (stfx_audio_group_result) stfx_audio_group_result(r);
    return r;
}
unsigned __real_sndSeqPlayEx(unsigned short sgid, unsigned short sid, void *arrfile, void *para, unsigned char studio);
unsigned __wrap_sndSeqPlayEx(unsigned short sgid, unsigned short sid, void *arrfile, void *para, unsigned char studio)
{
    extern int stfx_audio_song_gate(void) __attribute__((weak));
    if (!stfx_audio_song_gate || !stfx_audio_song_gate()) return 0xffffffffu;
    return __real_sndSeqPlayEx(sgid, sid, arrfile, para, studio);
}

