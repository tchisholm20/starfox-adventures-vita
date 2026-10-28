/* efb_rect.h -- the EFB-line -> render-target-pixel mapping, and the copy quantiser's model (2026-10-12).
 *
 * Pure C, no GL: rhi_vita.c / rhi_vita_efb.inc include it for the real thing, host/tests/build_efb_rect_test.sh for the check.
 *
 * THE GEOMETRY. GX addresses the embedded frame buffer as 640 x 528 lines, top-left origin. The game's display is 640x480 but
 * it legitimately renders AND copies down to line 512 (newshadows.c renderShadows: GXSetViewport(0,0,512,512) + a (0,0,512,512)
 * GXCopyTex for the biggest caster). The Vita side holds the picture in a target M of rw x mh pixels:
 *     rw = the render width           (render_res.txt; 960 at full res)
 *     vh = the VISIBLE render height  (the pixel rows that GX lines 0..480 occupy; 544 at full res, 368 at 640x368)
 *     mh = M's allocated height       (== vh with the fix off; efb528_total_h(vh) with it on)
 * One GX line is vh/480 pixels tall; line y starts at pixel row floor(y * vh / 480) counted from the TOP of M. M's extra rows
 * (mh - vh of them) are BELOW the picture: top-origin pixel rows [vh, mh). GL rows count from the bottom, so GL row = mh - y_px.
 * With mh == vh this is exactly the old mapping (y * rh / 480, GL row rh - y, clamp to rh) -- the host test keeps the old
 * functions and compares every rect.
 */
#ifndef STFX_EFB_RECT_H
#define STFX_EFB_RECT_H

#include <math.h>

/* M's allocated height for a visible height vh: 528/480 = 1.1 of it, rounded UP to a multiple of 8 (vitaGL loses the last rows
 * of an FBO that is not -- docs/47 "render_res"). 544 -> 600, 368 -> 408, 272 -> 304. Never below vh. */
static inline int efb528_total_h(int vh)
{
    int mh = (((vh * 11 + 9) / 10) + 7) & ~7;   /* ceil(vh * 1.1), then up to a multiple of 8 */
    return mh < vh ? vh : mh;
}

typedef struct {
    int x0, y0, x1, y1;       /* pixels, top-left origin, after the clamp */
    int below480;             /* the request ends below GX line 480 */
    int clamped;              /* the clamp changed a coordinate (the rect lost pixels the request asked for) */
} EfbRectPx;

/* A GX rectangle (x, y, w, h in EFB pixels/lines) as M's pixels, by the integer maths every user has always shared:
 * x * rw / 640, y * vh / 480, clamped to [0, rw] x [0, mh]. */
static inline EfbRectPx efb_rect_px(int x, int y, int w, int h, int rw, int vh, int mh)
{
    EfbRectPx r;
    r.below480 = y + h > 480;
    if (r.below480 && mh > vh) {
        /* A rect that reaches below line 480 is a shadow-map pass: its viewport (efb_vp_px) rounds to nearest, so the copy of it must too, or the
         * copy takes one row fewer than was rendered (512 lines at 640x368: viewport 393 rows, a floored copy 392) and the map is scaled by
         * 392/393. Rects inside the picture keep the floor every user always had, so nothing that already worked moves. */
        r.x0 = (x * rw * 2 + 640) / 1280; r.x1 = ((x + w) * rw * 2 + 640) / 1280;
        r.y0 = (y * vh * 2 + 480) / 960;  r.y1 = ((y + h) * vh * 2 + 480) / 960;
    } else {
        r.x0 = x * rw / 640; r.x1 = (x + w) * rw / 640;
        r.y0 = y * vh / 480; r.y1 = (y + h) * vh / 480;
    }
    r.clamped = 0;
    if (r.x0 < 0) { r.x0 = 0; r.clamped = 1; }
    if (r.y0 < 0) { r.y0 = 0; r.clamped = 1; }
    if (r.x1 > rw) { r.x1 = rw; r.clamped = 1; }
    if (r.y1 > mh) { r.y1 = mh; r.clamped = 1; }
    return r;
}

/* The GL viewport for a GX viewport (floats; the old rounding, +0.5, and NO clamp: a viewport may legitimately slide off the
 * target, renderObjectShadowTexture's does). vp = { x, GL row, w, h }. 0 = empty (the caller ignores the call). */
static inline int efb_vp_px(float x, float y, float w, float h, int rw, int vh, int mh, int vp[4])
{
    int x0 = (int) (x * rw / 640.0f + 0.5f), x1 = (int) ((x + w) * rw / 640.0f + 0.5f);
    int y0 = (int) (y * vh / 480.0f + 0.5f), y1 = (int) ((y + h) * vh / 480.0f + 0.5f);
    vp[0] = x0; vp[1] = mh - y1; vp[2] = x1 - x0; vp[3] = y1 - y0;
    return vp[2] > 0 && vp[3] > 0;
}

/* The GL scissor for a GX scissor: the rect clamp above, then { x, GL row, w, h } (an empty box stays empty). */
static inline void efb_sc_px(int x, int y, int w, int h, int rw, int vh, int mh, int sc[4])
{
    EfbRectPx r = efb_rect_px(x, y, w, h, rw, vh, mh);
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    sc[0] = r.x0; sc[1] = mh - r.y1; sc[2] = r.x1 - r.x0; sc[3] = r.y1 - r.y0;
}

/* The texture v of the BOTTOM of the visible picture when M is composited onto a panel scrH rows tall (the top is v = 1).
 * With no extra rows it is 0, exactly as before. With extra rows it is the visible region's lower edge pulled up by the
 * distance that puts the first panel row's sample at the centre of the picture's last texel row: LINEAR filtering would
 * otherwise blend ~16% of the extra row (stale shadow-map pixels) into the bottom row of the picture when M is stretched. */
static inline float efb_composite_v0(int vh, int mh, int scrH)
{
    if (mh <= vh) return 0.0f;
    float k = (float) vh / (float) scrH;                      /* M texels per panel row */
    float a = (float) (mh - vh) + 0.5f - 0.5f * k;            /* texel row (from M's bottom) where the picture starts */
    if (a < (float) (mh - vh)) a = (float) (mh - vh);         /* never include an extra row; k > 1 (a panel smaller than M) keeps the edge */
    return a / (float) mh;
}

/* ---- the copy quantiser -------------------------------------------------------------------------------------------------
 * A GX copy to a sub-8-bit format keeps the TOP n bits of each 8-bit channel (the port's CPU encoder, tex_decode.c
 * gxTexEncodeCopy: v >> 4 for I4/R4, p >> 3 for the 5-bit channels of RGB565, >> 2 for its 6-bit one), and the texture is read back
 * with n scaled to 0..1 (n / levels, levels = 2^bits - 1). L = levels (15, 31, 63); 0 = full precision.
 * The GPU copy program (rhi_vita_efb.inc EFB_FS_COPY_Q) computes, per channel, with the SAME operations:
 *     q = floor(floor(v * 255 + 0.5) * ((L + 1) / 256)) / max(L, 1);   out = v + (q - v) * min(L, 1)
 * efb_quant_model is that line in float, for the host test. */
static inline float efb_quant_model(float v, float L)
{
    float q = floorf(floorf(v * 255.0f + 0.5f) * ((L + 1.0f) * 0.00390625f)) / (L > 1.0f ? L : 1.0f);
    float m = L < 1.0f ? L : 1.0f;
    return v + (q - v) * m;
}
/* the same as an 8-bit code, as the 8-bit render target stores it (round to nearest) */
static inline int efb_quant_code(int v8, int L)
{
    return (int) floorf(efb_quant_model((float) v8 / 255.0f, (float) L) * 255.0f + 0.5f);
}

#endif
