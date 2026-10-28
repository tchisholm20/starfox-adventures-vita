// game_effects_stubs.c - link stand-ins for the effects tail of intersect_render.c.
//
// intersect_render.c (src/track) is SFA's whole 2D/HUD/effects render TU. The port
// brings it up for its TEXT/2D draw primitives (textRenderChar/Setup, drawTexture,
// hudDrawRect) - the only part on the boot-to-menu text path. The rest of the TU is
// a large effects subsystem (fog, TEV state, reflections, water caustics, distortion,
// spirit-vision, newshadows) whose data globals + texture helpers are DEFINED in other
// effects TUs (newshadows.c, shader.c, rcp_dolphin.c) we don't compile yet. None of it
// RUNS on the text path; these definitions only need to LINK. Superseded one file at a
// time as the effects/shadow subsystems come up (see SCAFFOLD_MANIFEST.md).
//
// Types are the faithful header types where it matters for storage size (GXColor is 4
// bytes); pointer params use void* (bare-symbol linkage) like the other *_stubs.

#include "dolphin/types.h"
#include "dolphin/gx/GXStruct.h"   // GXColor (4-byte {r,g,b,a})

// --- effects color registers (GXColor storage, read by TEV setup that never runs) ---
GXColor gBlurFilterKColor, gFogColor, gFrozenTintColor, gHeatEffectKColor;
GXColor gMotionBlurKColor, gObjectShadowTevColor, gProjectedShadowFogColor;
GXColor gReflectionBumpKColor, gReflectionBumpTintColor, gReflectionKColor, gReflectionTintColor;
GXColor gScreenImageKColor0, gScreenImageKColor1, gScreenImageKColor2, gScreenImageKColor3;
GXColor gScreenImageRegColor;
GXColor gSpiritVisionKColor0, gSpiritVisionKColor1, gSpiritVisionKColor2, gSpiritVisionRegColor;
GXColor gWaterCausticKColor, gWaterReflectionKColorB, gWaterReflectionKColorG, gWaterReflectionKColorR;
GXColor gWhirlpoolReflectionKColor, gWhirlpoolReflectionTintColor;

// --- effects scalars (fog planes, reflection/distortion scales - effect paths only) ---
f32 gCausticReflectionDiskScale, gDistortionAlphaRadius, gDistortionIndMtxRadius, gDistortionTexCoordScale;
f32 gFogEndZ, gFogFarZ, gFogNearZ, gFogStartZ;
f32 gFrozenReflectionNormalScale, gFrozenWhirlpoolTexScale, gSnowFlashOverlayAngle;
f32 gTrackNormalTexScale, gTrackProjectedTexScale;

// --- GX render-state cache (read/written by the real gxSetZMode_/gxTev* helpers in
//     intersect_render). Zeroed storage links + gives default state; migrate to a live
//     HAL GX-state layer when text rendering needs correct Z/TEV state. ---
int gGxZModeCompareFunc;
u32 gTevStageCursor, gTevTexCoordCursor, gTevTexMapCursor, gProjectedShadowFogColorBits;
u8 gGxZCompLocCached, gGxZCompLocValid, gGxZModeCompareEnable, gGxZModeUpdateEnable, gGxZModeValid;
u8 gHudTintAlpha, gMoonFxDayNo, gReflectionTintAlpha;
u8 gTevChanCount, gTevIndStageCount, gTevStageCount, gTevTexGenCount;

// --- pool constants (const f32 defs read via extern; text-fade limit + two lbl_ floats).
//     NOTE: gGameTextFadeLimit is on the TEXT path (a fade bound) - 255 = fully visible;
//     tune if text fades wrong. lbl_803DE70x are intersect_render pool consts (effects). ---
const f32 gGameTextFadeLimit = 255.0f;
f32 lbl_803DE704, lbl_803DE708;
const f32 lbl_803DE70C = 0.0f;
const f32 lbl_803DE710 = 0.0f;

// --- newshadows texture subsystem (getters/loaders; out-params zeroed so callers on
//     effect paths never read garbage). Real when newshadows.c comes up. ---
static u8 gEffDummyTexture[0x40];   // zeroed stand-in Texture for the Texture** getters
void newshadows_captureReflectionTextures(void) {}
void newshadows_getDiskTexture(u32* out)               { if (out) *out = 0; }
void newshadows_getRampTexture(u32* out)               { if (out) *out = 0; }
void newshadows_getReflectionDepthTexture(u32* out)    { if (out) *out = 0; }
void newshadows_getSnowFlashTexture(u32* out)          { if (out) *out = 0; }
void newshadows_getDistortionTexture(void** out)       { if (out) *out = gEffDummyTexture; }
void newshadows_getRadialTexture(void** out)           { if (out) *out = gEffDummyTexture; }
u32  newshadows_getReflectionGradientTexture(void)     { return 0; }
void newshadows_loadBumpTexture(int texMapId)          { (void)texMapId; }
void newshadows_loadReflectionColorTexture(int id)     { (void)id; }
void newshadows_loadWhirlpoolTexture(int id)           { (void)id; }

// --- HUD / texture-select / alpha helpers (effect + HUD paths, not basic text) ---
void drawHudBox(short x, short y, short w, short h, u8 alpha, u8 flags) { (void)x;(void)y;(void)w;(void)h;(void)alpha;(void)flags; }
void selectTextureWithSecondary(void* texture, int mapId)              { (void)texture; (void)mapId; }
int  objGetAlphaCompareThreshold(void)                                { return 0; }

// --- text color setter (real home lightmap_draw.c, uncompilable). On the TEXT path:
//     sets the current glyph color. No-op links; a string still renders in whatever the
//     TEV default color is. Give a real impl (store the color for textRenderChar) if the
//     first text milestone shows wrong/invisible glyph color. ---
void _textSetColor(void* context, int red, int green, int blue, int alpha) {
    (void)context; (void)red; (void)green; (void)blue; (void)alpha;
}
