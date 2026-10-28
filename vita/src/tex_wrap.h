/* tex_wrap.h -- GX texture wrap modes -> what the Vita backend applies (2026-10-11).
 *
 * Pure C, no GL: rhi_vita.c includes it for the real thing, host/tests/build_tex_wrap_test.sh for the check.
 *
 * GX: GX_CLAMP = 0, GX_REPEAT = 1, GX_MIRROR = 2 (GXInitTexObj's wrap_s / wrap_t). Until now every game texture was
 * GL_REPEAT (rhi_vita.c's createTexture hard-coded it), so a texture the game clamps -- the sky backdrop gradient whose V
 * window slides outside [0,1] with camera pitch, light falloff ramps, projected lights/shadows -- wrapped instead, and a
 * hard seam travelled with the camera.
 *
 * vitaGL maps GL_CLAMP_TO_EDGE / GL_REPEAT / GL_MIRRORED_REPEAT to SCE_GXM_TEXTURE_ADDR_CLAMP / REPEAT / MIRROR
 * (tools/_download/vitaGL/source/textures.c:222-263). Every glTexImage2D RGBA8 texture is a LINEAR GXM texture
 * (utils/gpu_utils.c:431 vglInitLinearTexture), including the power-of-two ones. REPEAT on linear textures is what the
 * game has always run on (POT and NPOT). MIRROR on a linear texture has never run here: UNVERIFIED on hardware, so
 * it is only requested for a power-of-two axis, and an NPOT axis asked to MIRROR degrades to REPEAT (today's behaviour,
 * counted) rather than to something nobody has run. CLAMP is always legal.
 */
#ifndef STFX_TEX_WRAP_H
#define STFX_TEX_WRAP_H

enum { TW_CLAMP = 0, TW_REPEAT = 1, TW_MIRROR = 2 };   /* == GX_CLAMP, GX_REPEAT, GX_MIRROR */

static inline int tw_is_pot(int n) { return n > 0 && (n & (n - 1)) == 0; }

/* The mode to apply on one axis of size `dim` for the GX mode `gx`. *downgraded (if non-NULL) is incremented when the
 * request could not be honoured (MIRROR on an NPOT axis -> REPEAT). An invalid GX mode reads as REPEAT (the old behaviour). */
static inline int tw_effective(int gx, int dim, unsigned* downgraded)
{
    switch (gx) {
#if defined(TW_BREAK_1)   /* host negative controls (build_tex_wrap_test.sh): clamp left as repeat / NPOT mirror kept / mirror and clamp swapped */
    case TW_CLAMP:  return TW_REPEAT;
#else
    case TW_CLAMP:  return TW_CLAMP;
#endif
#if defined(TW_BREAK_2)
    case TW_MIRROR: return TW_MIRROR;
#elif defined(TW_BREAK_3)
    case TW_MIRROR: return TW_CLAMP;
#else
    case TW_MIRROR: if (tw_is_pot(dim)) return TW_MIRROR; if (downgraded) ++*downgraded; return TW_REPEAT;
#endif
    default:        return TW_REPEAT;
    }
}

#endif
