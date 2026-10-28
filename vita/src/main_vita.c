/* main_vita.c - the Vita entry point for the Stairfax RHI bring-up.
 *
 * This runs the SAME sequence as upstream's src/tools/rhi_smoketest.c, in the
 * same order, because the point of the milestone is to prove THEIR seam rather
 * than a rewrite of it:
 *
 *   plat_window_create -> rhi_create -> rhi_swapchainCreate
 *   loop { plat_window_pump, rhi_beginFrame, rhi_clear, rhi_endFrame, rhi_present }
 *   rhi_swapchainDestroy -> rhi_destroy -> plat_window_destroy
 *
 * ⚠ IT IS NOT THEIR FILE, AND HERE IS EVERY WAY IT DIFFERS, so nobody later
 * mistakes this for having run upstream's binary:
 *
 *   1. No argv. A Vita app gets no command line, so the backend is AUTO (which
 *      falls through to GL once rhi_vulkan_available() is false) and the frame
 *      count is fixed rather than --frames.
 *   2. The capture is UNCONDITIONAL, not behind --capture. On this console a
 *      headless run has no window to screenshot, so the capture is the only
 *      evidence that anything was drawn.
 *   3. It writes a report file. See below -- this is the part that makes the run
 *      falsifiable instead of merely uncrashy.
 *
 * ⚠ THE REPORT EXISTS BECAUSE "IT DID NOT CRASH" IS NOT A RESULT. A clear-and-
 * present milestone can fail in a way that looks identical to success from the
 * outside: the process runs its frames, exits 0, and the panel shows black. So
 * the report reads PIXELS BACK and grades them.
 *
 * It now grades GEOMETRY, not just the clear: inside the triangle, outside it,
 * and whether those two differ at all. The clear-only check that shipped first
 * would still PASS with drawColored doing nothing, which is the wrong way for a
 * test to fail. (This paragraph replaced one describing that older check -- a
 * comment whose premise has expired is its own defect class here.)
 *
 * The expected value is computed from the same expression that fed glClearColor,
 * and the tolerance is loose (+-6 codes) on purpose: 8-bit quantisation plus any
 * colour-space handling in the display chain can move a channel by a step or
 * two, and this repo has already lost nine device rounds to a look defect that
 * was really a 6/255 signal wall. A tight bound here would fail for reasons that
 * have nothing to do with whether the seam works.
 */

#include "port/renderer/rhi.h"
#include "port/plat_window.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

#define REPORT "ux0:data/stairfax_rhi.txt"
#define SHOT   "ux0:data/stairfax_frame.bmp"

/* 8 MB. vita-renderer measured its own peak at ~1.4 MB and settled on 8 as
 * ~5.7x headroom; this app allocates less than that (one 2 MB readback buffer
 * plus a 1.5 MB BMP row staging path, both transient), and the same reasoning
 * applies: memory reserved here is memory FTPVita cannot get a transfer buffer
 * from while the app is foregrounded. */
unsigned int _newlib_heap_size_user = 8 * 1024 * 1024;

static void report(const char *line)
{
    SceUID fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, (unsigned) strlen(line));
    sceIoClose(fd);
}

static void reportf(const char *fmt, int a, int b, int c, int d)
{
    char buf[192];
    int n = snprintf(buf, sizeof buf, fmt, a, b, c, d);
    if (n > 0) report(buf);
}

/* The clear colour for frame f -- upstream's exact expression, kept identical so
 * the expected value below is derived from the same source as the actual one. */
static void clear_colour_for(int f, float *r, float *g, float *b)
{
    float t = (float) f / 60.0f;
    *r = 0.15f + 0.10f * (float) sin(t * 1.7f);
    *g = 0.45f + 0.15f * (float) sin(t * 1.1f + 1.0f);
    *b = 0.80f + 0.15f * (float) sin(t * 0.9f + 2.0f);
}

/* The test triangle, in CLIP SPACE with an identity MVP (rhi.h: positions are
 * clip-space, w=1, until setColorTransform is called). Apex up, base below
 * centre, so the screen centre is comfortably INSIDE and the screen corners are
 * comfortably OUTSIDE -- which is what makes the geometry test below able to
 * fail.
 *
 * rhi.h specifies rgba as MEMORY BYTE ORDER R,G,B,A == r | g<<8 | b<<16 | a<<24,
 * so this is written with the shifts rather than a hex literal: a literal here
 * would be a silent endianness bet, and "the triangle came out blue" is exactly
 * the kind of plausible-looking wrongness that costs a session.
 */
#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) \
                          | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const RhiColorVertex kTri[3] = {
    {  0.0f,  0.8f, 0.0f, RGBA(255,   0,   0, 255) },   /* apex   */
    { -0.8f, -0.8f, 0.0f, RGBA(255,   0,   0, 255) },   /* bottom-left  */
    {  0.8f, -0.8f, 0.0f, RGBA(255,   0,   0, 255) },   /* bottom-right */
};

/* ---- The textured test (milestone 3) --------------------------------------
 *
 * A 64x64 texture in four 32x32 quadrants, each a colour that no plausible
 * mistake maps onto another:
 *
 *     memory rows  0-31:  cols 0-31 GREEN     cols 32-63 BLUE
 *     memory rows 32-63:  cols 0-31 WHITE     cols 32-63 YELLOW, ALPHA 0
 *
 *   - a V flip swaps green <-> white; a U flip swaps green <-> blue
 *   - an R/B channel swap turns blue into red
 *   - texture REPLACE instead of MODULATE ignores the vertex tint below
 *   - the alpha-0 quadrant separates OPAQUE (drawn, yellow) from TEST (discarded)
 *
 * The same texture is drawn as two quads with UV (0,0) at the TOP-LEFT vertex
 * (the D3D reference convention: memory row 0 at the top of the screen):
 * LEFT quad in OPAQUE mode, RIGHT quad in TEST mode. Both are tinted
 * (255,255,128), so blue comes out (0,0,128) and white (255,255,128).
 *
 * Quads span clip y -0.5..0.5 and x -0.95..-0.5 / 0.5..0.95, which keeps them
 * clear of the triangle's centre sample and the two "outside" samples. */
#define TEX_N 64
static uint32_t g_texels[TEX_N * TEX_N];

static void build_test_texture(void)
{
    for (int y = 0; y < TEX_N; ++y)
        for (int x = 0; x < TEX_N; ++x) {
            int right = x >= TEX_N / 2, bottom = y >= TEX_N / 2;
            uint32_t c;
            if (!bottom) c = right ? RGBA(0, 0, 255, 255)   : RGBA(0, 255, 0, 255);
            else         c = right ? RGBA(255, 255, 0, 0)   : RGBA(255, 255, 255, 255);
            g_texels[y * TEX_N + x] = c;
        }
}

static void make_quad(RhiTexVertex q[6], float x0, float x1, float y0, float y1)
{
    const uint32_t tint = RGBA(255, 255, 128, 255);
    /* y1 is the TOP edge (clip +y is up), and carries v = 0. */
    const RhiTexVertex tl = { x0, y1, 0.0f, tint, 0.0f, 0.0f };
    const RhiTexVertex tr = { x1, y1, 0.0f, tint, 1.0f, 0.0f };
    const RhiTexVertex bl = { x0, y0, 0.0f, tint, 0.0f, 1.0f };
    const RhiTexVertex br = { x1, y0, 0.0f, tint, 1.0f, 1.0f };
    q[0] = tl; q[1] = bl; q[2] = tr;
    q[3] = tr; q[4] = bl; q[5] = br;
}

/* Read one pixel and grade it against an expected colour, +-6 codes. */
static int check_px(const char *name, int x, int y, int er, int eg, int eb,
                    unsigned char out[4])
{
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    int dr = (int) out[0] - er, dg = (int) out[1] - eg, db = (int) out[2] - eb;
    int ok = dr >= -6 && dr <= 6 && dg >= -6 && dg <= 6 && db >= -6 && db <= 6;
    char b[200];
    snprintf(b, sizeof b, "%s %-22s (%3d,%3d) got %3d,%3d,%3d  want %3d,%3d,%3d\n",
             ok ? "PASS" : "FAIL", name, x, y, out[0], out[1], out[2], er, eg, eb);
    report(b);
    return ok;
}

int main(void)
{
    const int W = 960, H = 544;          /* the panel; see plat_window_vita.c */
    const int frames = 180;              /* ~3 s at 60 Hz */
    const int captureFrame = 45;         /* upstream's default */

    sceIoRemove(REPORT);
    report("stairfax RHI bring-up (Vita)\n");

    PlatWindow *win = plat_window_create("Stairfax Temperatures (Vita)", W, H);
    if (!win) { report("FAIL plat_window_create\n"); sceKernelExitProcess(2); return 2; }
    report("ok   plat_window_create\n");

    RhiCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.backend      = RHI_BACKEND_AUTO;  /* -> GL, since vulkan/d3d are absent */
    ci.windowHandle = plat_window_native_handle(win);   /* NULL, correctly */
    ci.width        = W;
    ci.height       = H;
    ci.vsync        = true;
    ci.debug        = false;
    ci.appName      = "Stairfax Temperatures";

    RhiInstance *rhi = rhi_create(&ci);
    if (!rhi) { report("FAIL rhi_create\n"); sceKernelExitProcess(3); return 3; }

    RhiBackend got = rhi_getBackend(rhi);
    {
        char b[160];
        snprintf(b, sizeof b, "ok   rhi_create  backend=%s%s  panel=%dx%d\n",
                 rhi_backendName(got), rhi_isBestEffort(got) ? " (best-effort)" : "",
                 W, H);
        report(b);
    }

    RhiSwapchain *sc = rhi_swapchainCreate(rhi, ci.windowHandle, W, H, ci.vsync);
    report(sc ? "ok   rhi_swapchainCreate\n" : "WARN rhi_swapchainCreate -> NULL\n");

    build_test_texture();
    RhiTexture *tex = rhi_createTexture(rhi, TEX_N, TEX_N, 1, 0, g_texels);
    report(tex ? "ok   rhi_createTexture 64x64\n" : "FAIL rhi_createTexture -> NULL\n");
    RhiTexVertex quadL[6], quadR[6];
    make_quad(quadL, -0.95f, -0.50f, -0.5f, 0.5f);
    make_quad(quadR,  0.50f,  0.95f, -0.5f, 0.5f);

    int captured = 0, presented = 0, pumpQuit = 0;
    int expR = -1, expG = -1, expB = -1;

    for (int f = 0; f < frames; ++f) {
        if (!plat_window_pump(win)) { pumpQuit = 1; break; }

        float r, g, b;
        clear_colour_for(f, &r, &g, &b);

        /* Frame 0 only: name the exact call that dies. Vita3K reported
         * sceGxmCreateContext -> SCE_GXM_ERROR_ALREADY_INITIALIZED followed by a
         * read violation at 0x78, which says vitaGL asks for a SECOND GXM
         * context somewhere in the first frame -- but not which call does it.
         * Without these four lines "it crashed in the loop" is the whole of what
         * is known, and the loop has four steps. */
        const int trace0 = (f == 0);
        if (trace0) report("     f0 before beginFrame\n");
        rhi_beginFrame(rhi);
        if (trace0) report("     f0 before clear\n");
        rhi_clear(rhi, r, g, b, 1.0f);
        if (trace0) report("     f0 after clear\n");

        /* The second GX brick: an immediate-mode coloured triangle. Identity MVP
         * is set explicitly every frame rather than relied upon -- the backend
         * defaults to identity, but a test that depends on a default is testing
         * the default. */
        static const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        rhi_setColorTransform(rhi, kIdentity);
        rhi_drawColored(rhi, kTri, 3);
        if (trace0) report("     f0 after drawColored\n");

        rhi_setTexture(rhi, 0, tex);
        rhi_setAlphaMode(rhi, RHI_ALPHA_OPAQUE);
        rhi_drawTextured(rhi, quadL, 6);
        rhi_setAlphaMode(rhi, RHI_ALPHA_TEST);
        rhi_drawTextured(rhi, quadR, 6);
        rhi_setAlphaMode(rhi, RHI_ALPHA_OPAQUE);
        if (trace0) report("     f0 after drawTextured x2\n");

        rhi_endFrame(rhi);
        if (trace0) report("     f0 after endFrame\n");

        /* ⚠ THE READBACK MUST HAPPEN BEFORE THE SWAP. After vglSwapBuffers the
         * buffer that was just cleared is no longer the one bound for reading,
         * so a capture taken after presenting is a capture of the WRONG frame --
         * and it would still produce a plausible-looking image. */
        if (!captured && f == captureFrame) {
            expR = (int) (r * 255.0f + 0.5f);
            expG = (int) (g * 255.0f + 0.5f);
            expB = (int) (b * 255.0f + 0.5f);

            /* ⚠ THE CLEAR-COLOUR CHECK HAD TO MOVE, AND THAT IS THE POINT.
             * Until this frame drew anything, "centre pixel == clear colour" was
             * the right assertion. With a triangle over the centre it is now the
             * WRONG one -- and note which way it fails: if drawColored did
             * nothing, the old check would still PASS. A test that goes green on
             * a blank screen is worse than no test.
             *
             * So this asserts GEOMETRY, three ways, and each one can fail
             * independently:
             *   inside  == the triangle's colour   (it drew)
             *   outside == the clear colour        (it did not cover everything,
             *                                       e.g. a degenerate full-screen
             *                                       triangle from a bad matrix)
             *   inside  != outside                 (something is actually there) */
            unsigned char in_px[4]  = { 0, 0, 0, 0 };
            unsigned char out_px[4] = { 0, 0, 0, 0 };
            unsigned char top_px[4] = { 0, 0, 0, 0 };
            /* (480,272) is clip (0,0): inside. (40,40) is clip (-0.92,-0.85),
             * below the base at y=-0.8. (40,500) is clip y=+0.84, above the apex
             * at y=+0.8. glReadPixels' origin is the LOWER-left. */
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, in_px);
            glReadPixels(40,     40,    1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out_px);
            glReadPixels(40,     500,   1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top_px);

            reportf("     frame %d  clear was R=%d G=%d B=%d\n", f, expR, expG, expB);
            reportf("     inside  (480,272) R=%d G=%d B=%d\n",
                    in_px[0], in_px[1], in_px[2], 0);
            reportf("     outside ( 40, 40) R=%d G=%d B=%d\n",
                    out_px[0], out_px[1], out_px[2], 0);
            reportf("     outside ( 40,500) R=%d G=%d B=%d\n",
                    top_px[0], top_px[1], top_px[2], 0);

            /* The triangle is opaque red: R=255, G=0, B=0. Tolerance stays loose
             * (+-6 codes) for the same reason as before -- 8-bit quantisation and
             * any display-chain handling can move a channel a step or two, and a
             * tight bound would fail for reasons unrelated to whether the draw
             * path works. */
            int tri_ok = (in_px[0] >= 249 && in_px[1] <= 6 && in_px[2] <= 6);

            int dr = (int) out_px[0] - expR, dg = (int) out_px[1] - expG,
                db = (int) out_px[2] - expB;
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            int clear_ok = (dr <= 6 && dg <= 6 && db <= 6);

            int differ = (in_px[0] != out_px[0]) || (in_px[1] != out_px[1])
                       || (in_px[2] != out_px[2]);

            report(tri_ok   ? "PASS inside  is the triangle colour (255,0,0)\n"
                            : "FAIL inside  is NOT the triangle colour\n");
            report(clear_ok ? "PASS outside is the clear colour\n"
                            : "FAIL outside is NOT the clear colour\n");
            report(differ   ? "PASS inside differs from outside -- geometry drew\n"
                            : "FAIL inside == outside -- NOTHING was drawn\n");

            /* Textured quads. Quadrant centres: x clip +-0.8375 / +-0.6125,
             * y clip +-0.25, i.e. pixels x 78/186 (left) 774/882 (right), y 340
             * (top half) / 204 (bottom half), lower-left origin. Each is ~50 px
             * from any quadrant edge, so LINEAR filtering cannot blend colours. */
            unsigned char p[4], lbr[4], rbr[4];
            int t = 1;
            report("     -- textured, LEFT quad, OPAQUE --\n");
            t &= check_px("L top-left  green",   78, 340,   0, 255,   0, p);
            t &= check_px("L top-right blue*t", 186, 340,   0,   0, 128, p);
            t &= check_px("L bot-left  white*t", 78, 204, 255, 255, 128, p);
            t &= check_px("L bot-right yellow a0", 186, 204, 255, 255, 0, lbr);
            report("     -- textured, RIGHT quad, ALPHA TEST --\n");
            t &= check_px("R top-left  green",  774, 340,   0, 255,   0, p);
            t &= check_px("R top-right blue*t", 882, 340,   0,   0, 128, p);
            t &= check_px("R bot-left  white*t",774, 204, 255, 255, 128, p);
            t &= check_px("R bot-right discard",882, 204, expR, expG, expB, rbr);
            /* The alpha-0 quadrant, same texels, two modes: if alpha test did
             * nothing these two would be identical. */
            int modes_differ = lbr[0] != rbr[0] || lbr[1] != rbr[1] || lbr[2] != rbr[2];
            report(modes_differ ? "PASS alpha-0 texel: OPAQUE != TEST -- alpha mode is live\n"
                                : "FAIL alpha-0 texel: OPAQUE == TEST -- alpha mode ignored\n");
            report(t && modes_differ ? "PASS textured path: all 9 checks\n"
                                     : "FAIL textured path\n");

            captured = plat_window_capture_bmp(win, SHOT) ? 1 : 0;
            report(captured ? "ok   capture -> " SHOT "\n"
                            : "WARN capture failed\n");
            if (!captured) captured = 1;   /* do not retry every frame */
        }

        if (trace0) report("     f0 before present\n");
        if (!rhi_present(rhi, sc)) { report("FAIL rhi_present\n"); break; }
        if (trace0) report("     f0 after present -- first frame survived\n");
        presented++;
    }

    reportf("     presented %d frames (pump-quit=%d)\n", presented, pumpQuit, 0, 0);

    rhi_destroyTexture(rhi, tex);
    rhi_swapchainDestroy(rhi, sc);
    rhi_destroy(rhi);
    plat_window_destroy(win);
    report("done\n");

    sceKernelExitProcess(0);
    return 0;
}
