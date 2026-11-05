/* frames_log.inc -- the always-on PER-FRAME log (2026-11-03). Included by vita_tool_main.c (so no build-script change); format in frames_log.h.
 * One 32-byte record per present -> ux0:data/stairfax/frames.bin, read by host/engine/frames_judge.py. Why it exists: Tyler played a session with 26% of
 * frames >= 50 ms and one 1.88 s frame; the 60-present means ([win] [gxt] [gxm-gpu]) hid it. v0.1.2 is gated on "no hitches the player feels".
 *
 * HOT PATH (game thread, inside __wrap_rhi_present): frames_log_present() reads ~8 counters, packs 32 bytes into an in-RAM ring and publishes the head
 * with one release store. No file I/O, no lock, no printf except the one [frames] line per 600 presents: a single-producer / single-consumer ring, so
 * the writer can never make the game wait. The hook's own cost is measured on the console and printed in that line (avg / max us; budget 20 us).
 * WRITER (thread stfx_frmw, priority BELOW every game thread, not pinned): owns all file I/O. At start it renames an existing frames.bin to
 * frames_prev.bin (replacing it); then every FL_BLOCK records (the game thread signals a semaphore) OR every 2 s (timed wait: bounds what a crash or a
 * quit loses to ~2 s -- there is no exit hook, deliberately: this adds nothing to the exit path) it appends whatever is complete. Each append is an
 * open/write/close on the writer thread, never on the game thread (card opens measured 20-28 ms). If the ring is full the record is DROPPED and counted
 * (the present-index gap in the file shows it). Size cap FRM_MAX_BYTES, then the writer stops and the producer stops filling.
 * MARKER ux0:data/stairfax/frames_log.txt (read once, present 1): absent or "1" = ON (the default, release included); "0" = OFF (no thread, no ring use);
 * "stall P MS" = ON, and record P (P >= 3) carries flag bit0 with MS ms of busy-wait on the game thread inside it (the busy-wait runs in the hook of
 * present P-1, after that record is stored, so it lands in the wall of record P: the flagged record IS the long one). */
#include "frames_log.h"

#define FL_RING  1024u   /* records; power of two. 32 KB; ~34 s at 30 fps of writer outage before the first drop */
#define FL_BLOCK 256u    /* the game thread wakes the writer every this many records (~8 s at 30 fps) */
#define FL_PATH      "ux0:data/stairfax/frames.bin"
#define FL_PATH_PREV "ux0:data/stairfax/frames_prev.bin"
#define FL_MARKER    "ux0:data/stairfax/frames_log.txt"

extern volatile uint32_t g_gg_busy_total_us __attribute__((weak));   /* rhi_gxm_frame.c: cumulative GPU busy of the gxm_gpu.txt poller, modular u32; 0 until the poller has seen a scene */
extern int  gShaderCurMapEventId __attribute__((weak));              /* engine/src/main/shader.c:158: the map the game is in (see frames_log_map) */
extern unsigned long long rhi_vita_gx_compile_us __attribute__((weak)), rhi_vita_tev_compile_us __attribute__((weak));   /* rhi_vita_tev.inc */
extern unsigned char gameState __attribute__((weak));                /* the same global vt_game.c externs */
extern void gxm_shader_async_counts(unsigned o[4]) __attribute__((weak));   /* rhi_gxm_shader.c: queued, installed, pending_hits, failed (cumulative) */

static uint8_t           fl_ring[FL_RING][FRM_REC_BYTES];
static uint8_t           fl_wbuf[FRM_HDR_BYTES + FL_RING * FRM_REC_BYTES];   /* the writer's copy: header + the records it takes in one pass */
static volatile uint32_t fl_head, fl_tail;      /* head: game thread (release store); tail: writer (release store) -- modular counts of records */
static volatile uint32_t fl_drops, fl_bytes;    /* drops: game thread; bytes written to the card: writer */
static volatile uint32_t fl_hdr_flags;          /* FRM_HF_*: set by the game thread before the first record is published, read by the writer after it sees one */
static volatile int      fl_full;               /* writer: cap reached or a write failed -> the producer stops */
static volatile int      fl_werr;               /* writer: first failing sceIo return, 0 = none */
static int               fl_on;                 /* 1 = ring + writer live */
static SceUID            fl_sema = -1;
static unsigned          fl_stall_p, fl_stall_ms;   /* positive control: record fl_stall_p carries the stall; 0 = none */
static int               fl_stall_flag;

static int fl_writer(SceSize args, void *argp)
{
    int first = 1; (void) args; (void) argp;
    sceIoMkdir("ux0:data/stairfax", 0777);
    sceIoRemove(FL_PATH_PREV);
    sceIoRename(FL_PATH, FL_PATH_PREV);   /* fails when there was no previous file: fine */
    for (;;) {
        SceUInt to = 2000000u;
        sceKernelWaitSema(fl_sema, 1, &to);   /* signalled every FL_BLOCK records; a timeout (error return) is the 2 s tick */
        for (;;) {
            const uint32_t h = __atomic_load_n(&fl_head, __ATOMIC_ACQUIRE), t = fl_tail;
            uint32_t n = h - t, off = 0, len;
            if (n == 0 || n > FL_RING) break;   /* n > FL_RING cannot happen (the producer never overruns); refuse rather than write garbage */
            if (first) {
#ifdef STFX_BUILD_ID
                frm_pack_header(fl_wbuf, STFX_BUILD_ID, fl_hdr_flags);
#else
                frm_pack_header(fl_wbuf, "unknown", fl_hdr_flags);
#endif
                off = FRM_HDR_BYTES;
            }
            for (uint32_t i = 0; i < n; ++i) memcpy(fl_wbuf + off + i * FRM_REC_BYTES, fl_ring[(t + i) & (FL_RING - 1u)], FRM_REC_BYTES);
            __atomic_store_n(&fl_tail, t + n, __ATOMIC_RELEASE);   /* the slots are free again before the (slow) write */
            len = off + n * FRM_REC_BYTES;
            if (fl_bytes + len > FRM_MAX_BYTES) { len = (FRM_MAX_BYTES - fl_bytes) / FRM_REC_BYTES * FRM_REC_BYTES; fl_full = 1; }
            if (len) {
                SceUID fd = sceIoOpen(FL_PATH, SCE_O_WRONLY | SCE_O_CREAT | (first ? SCE_O_TRUNC : SCE_O_APPEND), 0777);
                int w = fd >= 0 ? sceIoWrite(fd, fl_wbuf, len) : fd;
                if (fd >= 0) sceIoClose(fd);
                if (w != (int) len) { fl_werr = w < 0 ? w : -1; fl_full = 1; }
                else { fl_bytes += len; first = 0; }
            }
            if (fl_full) return 0;   /* the file stays as it is; the game keeps running */
        }
    }
}

/* Where the map id comes from: gShaderCurMapEventId (engine/src/main/shader.c:158). mapSetup (shader.c:4382) assigns it
 * mapCoordsToId(origin + 7, ..) on every map (re)load -- the same id the game's own text-directory switch (shader.c:4314) and the rom-list load
 * (shader.c:4421) key on, and stfx_start_at.c's "map N" cross-check names -- so it changes when the player warps between areas. -1 = none -> 0xFFFF. */
static uint16_t frames_log_map(void)
{
    int m;
    if (!&gShaderCurMapEventId) return 0;
    m = gShaderCurMapEventId;
    return (m < 0 || m >= (int) FRM_MAP_NONE) ? FRM_MAP_NONE : (uint16_t) m;
}

static void frames_log_init(void)
{
    char b[64]; FILE *mf = fopen(FL_MARKER, "r"); size_t n = 0, i0 = 0; int p = 0, ms = 0;
    memset(b, 0, sizeof b);
    if (mf) { n = fread(b, 1, sizeof b - 1, mf); fclose(mf); }
    while (i0 < n && (b[i0] == ' ' || b[i0] == '\t' || b[i0] == '\r' || b[i0] == '\n')) ++i0;
    if (i0 < n && b[i0] == '0') { printf("[frames] OFF (frames_log.txt 0)\n"); return; }
    if (sscanf(b, "stall %d %d", &p, &ms) == 2 && p >= 1 && ms >= 1) { fl_stall_p = (unsigned) (p < 3 ? 3 : p); fl_stall_ms = (unsigned) (ms > 5000 ? 5000 : ms); }
    {   FILE *gf = fopen("ux0:data/stairfax/gxm_gpu.txt", "r"); if (gf) { fclose(gf); fl_hdr_flags |= FRM_HF_GPU_POLL; } }
    fl_sema = sceKernelCreateSema("stfx_frmw", 0, 0, 255, NULL);
    if (fl_sema < 0) { printf("[frames] OFF (semaphore failed 0x%08x)\n", (unsigned) fl_sema); return; }
    {   /* lower than every game thread. 0x10000100 + 32 was refused on hw (plat_window_vita.c), so +16 first, then the lowest user priority 191 (the idle meter's), then equal */
        static const int pri[3] = { 0x10000100 + 16, 191, 0x10000100 };
        static const char *const pn[3] = { "game+16", "191 (lowest user)", "EQUAL to the game threads" };
        SceUID t = -1; int k;
        for (k = 0; k < 3 && t < 0; ++k) t = sceKernelCreateThread("stfx_frmw", fl_writer, pri[k], 0x4000, 0, 0, NULL);
        if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) { printf("[frames] OFF (writer thread failed 0x%08x)\n", (unsigned) t); sceKernelDeleteSema(fl_sema); fl_sema = -1; return; }
        fl_on = 1;
        printf("[frames] ON -> %s (record %u B, header %u B, ring %u records, writer wakes every %u records or 2 s, priority %s, cap %u MB%s)\n", FL_PATH,
               FRM_REC_BYTES, FRM_HDR_BYTES, FL_RING, FL_BLOCK, pn[k - 1], FRM_MAX_BYTES >> 20, fl_stall_p ? "; POSITIVE CONTROL armed" : "");
        if (fl_stall_p) printf("[frames] positive control: record %u carries %u ms of busy-wait on the game thread (flag bit0)\n", fl_stall_p, fl_stall_ms);
    }
}

/* One call per present, at the [ft] histogram: us = this present's present-to-present time, 0 on present 1 (init + baselines only). */
static void frames_log_present(int presents, unsigned us)
{
    static SceUInt64 p_cpu; static unsigned long long p_cmp; static uint32_t p_r, p_gpu, p_async, p_w; static int gpu_seen, hdr_set;
    static unsigned long long h_sum, h_max, h_n;   /* the hook's own cost (us), this 600-present window */
    SceUInt64 t[4], c0, c1; unsigned long long cmp; unsigned ac[4] = { 0, 0, 0, 0 }; uint32_t r, g;
    if (presents == 1) frames_log_init();
    if (!fl_on) return;
    c0 = sceKernelGetProcessTimeWide();
    rhi_vita_timing(t);
    cmp = (&rhi_vita_gx_compile_us ? rhi_vita_gx_compile_us : 0ull) + (&rhi_vita_tev_compile_us ? rhi_vita_tev_compile_us : 0ull);
    r = g_gxt.st.r_busy_us;
    const uint32_t w = g_gxt.st.present_wait_us + g_gxt.st.ring_full_us + g_gxt.st.block_us;   /* threaded: G's waits on R */
    g = &g_gg_busy_total_us ? g_gg_busy_total_us : 0u;
    if (gxm_shader_async_counts) gxm_shader_async_counts(ac);
    if (us && !fl_full) {
        FrmRec rec; const uint32_t h = fl_head;
        if (!hdr_set) { if (g_gxt.on) fl_hdr_flags |= FRM_HF_THREADED; hdr_set = 1; }
        if (g) gpu_seen = 1;
        rec.present = (uint32_t) presents; rec.wall_us = us; rec.g_cpu_us = g_gxt.on ? (us > w - p_w ? us - (w - p_w) : 0u) : (uint32_t) (t[0] - p_cpu);
        rec.r_busy_us = g_gxt.on ? r - p_r : FRM_NONE;
        rec.gpu_us = (&g_gg_busy_total_us && gpu_seen) ? g - p_gpu : FRM_NONE;
        rec.compile_us = (uint32_t) (cmp - p_cmp);
        rec.async_installed = frm_sat16(ac[1] - p_async); rec.map = frames_log_map();
        rec.game_state = &gameState ? gameState : 0; rec.flags = (uint8_t) (fl_stall_flag ? FRM_RF_STALL : 0); rec.reserved = 0;
        fl_stall_flag = 0;
        if (h - __atomic_load_n(&fl_tail, __ATOMIC_ACQUIRE) >= FL_RING) ++fl_drops;   /* writer behind: drop, never wait */
        else {
            frm_pack_rec(fl_ring[h & (FL_RING - 1u)], &rec);
            __atomic_store_n(&fl_head, h + 1u, __ATOMIC_RELEASE);
            if (((h + 1u) & (FL_BLOCK - 1u)) == 0) sceKernelSignalSema(fl_sema, 1);
        }
    }
    p_cpu = t[0]; p_cmp = cmp; p_r = r; p_gpu = g; p_async = ac[1]; p_w = w;
    c1 = sceKernelGetProcessTimeWide();
    if (us) { const unsigned long long d = c1 - c0; h_sum += d; ++h_n; if (d > h_max) h_max = d; }
    if (presents % 600 == 0) {
        printf("[frames] presents %d: %u records published, %u dropped (ring %u), %u KB on the card%s%s; hook %.1f us avg, %llu us max over %llu frames (budget 20)\n", presents,
               (unsigned) fl_head, (unsigned) fl_drops, FL_RING, (unsigned) (fl_bytes >> 10), fl_full ? " -- WRITER STOPPED" : "", fl_werr ? " (write error)" : "",
               h_n ? (double) h_sum / (double) h_n : 0.0, h_max, h_n);
        if (fl_werr) printf("[frames] first write error 0x%08x\n", (unsigned) fl_werr);
        h_sum = 0; h_max = 0; h_n = 0;
    }
    if (fl_stall_p && (unsigned) presents + 1u == fl_stall_p && us) {   /* after this present's record is stored: the next record is the long one */
        const SceUInt64 s0 = sceKernelGetProcessTimeWide();
        while (sceKernelGetProcessTimeWide() - s0 < (SceUInt64) fl_stall_ms * 1000u) { }
        fl_stall_flag = 1;
        printf("[frames] positive control: %u ms stall injected after present %d (record %u carries flag bit0)\n", fl_stall_ms, presents, fl_stall_p);
    }
}
