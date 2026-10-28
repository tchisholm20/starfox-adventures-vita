/* gxm_copyedge.h -- pure ring scan of an EFB copy texture (diagnostic, marker ux0:data/stairfax/gxm_copyedge.txt).
 * Question: are the outermost texels of a shadow copy texture non-zero?  The game projects the texture over whole ground triangles and relies on
 * CLAMP + a transparent 2 px border; one hot edge texel smears across the triangle as a "dangling line".
 * Texel = one 32-bit A8B8G8R8 word; "non-zero" = any channel non-zero.  Only the valid dw x dh region is read (aw >= dw is the row stride in
 * texels; the padding to a multiple of 8 is never looked at).  ring r = texels whose min(x, y, dw-1-x, dh-1-y) == r.  No SCE, no globals. */
#ifndef GXM_COPYEDGE_H
#define GXM_COPYEDGE_H
#include <stdint.h>

typedef struct { uint32_t r0_nz, r0_total, r0_max, r1_nz, r1_total; } GxmCopyEdge;

static inline void gxm_copyedge_px(GxmCopyEdge *o, uint32_t v, int ring)
{
    if (ring == 0) {
        ++o->r0_total;
        if (v) {
            uint32_t m = v & 0xffu;
            if (((v >> 8) & 0xffu) > m) m = (v >> 8) & 0xffu;
            if (((v >> 16) & 0xffu) > m) m = (v >> 16) & 0xffu;
            if ((v >> 24) > m) m = v >> 24;
            ++o->r0_nz;
            if (m > o->r0_max) o->r0_max = m;
        }
    } else {
        ++o->r1_total;
        if (v) ++o->r1_nz;
    }
}

static inline GxmCopyEdge gxm_copyedge_scan(const uint32_t *px, int aw, int dw, int dh)
{
    GxmCopyEdge o = { 0, 0, 0, 0, 0 };
    if (!px || dw <= 0 || dh <= 0 || aw < dw) return o;
    for (int y = 0; y < dh; ++y) {
        const int dy = y < dh - 1 - y ? y : dh - 1 - y;
        const uint32_t *row = px + (size_t) y * (size_t) aw;
        if (dy <= 1) {                                    /* the two outer rows at each end: every texel is ring dy or 0 */
            for (int x = 0; x < dw; ++x) {
                int dx = x < dw - 1 - x ? x : dw - 1 - x;
                gxm_copyedge_px(&o, row[x], dx < dy ? dx : dy);
            }
        } else {                                          /* interior rows: only the two columns at each end */
            for (int x = 0; x < 2 && x < dw; ++x) gxm_copyedge_px(&o, row[x], x);
            for (int x = dw - 2 > 2 ? dw - 2 : 2; x < dw; ++x) gxm_copyedge_px(&o, row[x], dw - 1 - x);
        }
    }
    return o;
}
#endif
