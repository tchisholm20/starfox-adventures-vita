// game_objprint_stubs.c - satisfy objprint_dolphin.c's dependency graph for an UNTEXTURED,
// UNLIT first pass of the REAL model render path (objRenderModel -> modelDoRenderInstrs).
//
// The geometry decode + skinning is fully REAL (objprint's render-instruction interpreter +
// gx_draw's DL decoder + model.c's ObjModel_UpdateAnimMatrices). What is stubbed here is only
// the TEV/texture/light/shadow/fuzz stage setup and a few camera/math bridges - i.e. shading,
// which comes later by compiling rcp_dolphin.c/shader_dolphin.c/modellight.c/newshadows.c for
// real. These stubs are SAFE no-ops: pointer-returning ones hand back a zeroed dummy so any
// field read by the real code is 0 and never faults. Signatures are intentionally (void) - the
// callers use the real prototypes from headers; cdecl lets the caller clean the stack.

// Forward-declare the CRT double math we need rather than <math.h> - that header defines
// asinf/atan2f as inlines, which collide with the real symbols the game references extern.
double atan2(double, double);
double asin(double);
double sin(double);
double cos(double);

static unsigned char gDummy[4096];   // zeroed; returned where real code may read fields

// --- camera bridge: the render path's view matrix -----------------------------
// game_scene still writes gPortViewMatrix each frame; Camera_GetViewMatrix / culling / projection
// are now the REAL camera.c versions (real-camera bring-up). gPortViewMatrix stays as the interim
// render bridge until Stage 2 rewires game_scene onto the real camera output.
float gPortViewMatrix[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
// gCameraLightPerspectiveMatrix: used to build projected-light tex matrices (unused untextured).
float gCameraLightPerspectiveMatrix[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};

// --- math bridges ------------------------------------------------------------
// setMatrixFromObjectPos / setMatrixFromObjectTransposed / Matrix_TransformPoint / getAngle are
// the REAL vecmath.c (now compiled). Only atan2f_fast and asinf (which vecmath references but
// doesn't define) are provided here.
float atan2f_fast(float y, float x) { return (float)atan2((double)y, (double)x); }
float asinf(float x) { return (float)asin((double)x); }

// Faithful reimpls of vecmath.c's object->matrix builders (compiling vecmath.c pulls a trig-helper
// subtree; these are the same math). MatrixTransform overlays the GameObject anim: rotX/Y/Z s16 @
// 0/2/4, scale f32 @0x08 (rootMotionScale), x/y/z f32 @0x0C/0x10/0x14. Angle: full circle = 65536.
static void angleToVec2_port(unsigned short a, float* s, float* c) {
    double rad = (double)(short)a * (3.14159265358979 * 2.0 / 65536.0);
    *s = (float)sin(rad); *c = (float)cos(rad);
}
void setMatrixFromObjectPos(float* m, const void* xf) {
    const unsigned char* t = (const unsigned char*)xf;
    unsigned short rotX = *(const unsigned short*)(t + 0), rotY = *(const unsigned short*)(t + 2),
                   rotZ = *(const unsigned short*)(t + 4);
    float scale = *(const float*)(t + 0x08);
    float x = *(const float*)(t + 0x0C), y = *(const float*)(t + 0x10), z = *(const float*)(t + 0x14);
    float s0,c0,s1,c1,s2,c2;
    angleToVec2_port(rotX, &s0, &c0); angleToVec2_port(rotY, &s1, &c1); angleToVec2_port(rotZ, &s2, &c2);
    m[0]=scale*(s2*(s1*s0)+c2*c0); m[1]=scale*(s2*c1); m[2]=scale*(s2*(s1*c0)-c2*s0); m[3]=0;
    m[4]=scale*(c2*(s1*s0)-s2*c0); m[5]=scale*(c2*c1); m[6]=scale*(c2*(s1*c0)+s2*s0); m[7]=0;
    m[8]=scale*(c1*s0);            m[9]=-s1*scale;     m[10]=scale*(c1*c0);            m[11]=0;
    m[12]=x; m[13]=y; m[14]=z; m[15]=1.0f;
}
void setMatrixFromObjectTransposed(void* obj, float* out) {
    float m[16]; setMatrixFromObjectPos(m, obj);
    out[0]=m[0]; out[1]=m[4]; out[2]=m[8];
    out[4]=m[1]; out[5]=m[5]; out[6]=m[9];
    out[8]=m[2]; out[9]=m[6]; out[10]=m[10];
    out[3]=m[12]; out[7]=m[13]; out[11]=m[14];
}
// Real signatures (vecmath.c): Matrix_TransformPoint(m3x4, x,y,z, &ox,&oy,&oz); getAngle(y,x)->s16.
void Matrix_TransformPoint(const float* m, float x, float y, float z, float* ox, float* oy, float* oz) {
    *ox = m[0]*x + m[1]*y + m[2]*z  + m[3];
    *oy = m[4]*x + m[5]*y + m[6]*z  + m[7];
    *oz = m[8]*x + m[9]*y + m[10]*z + m[11];
}
int getAngle(float y, float x) { return (int)(short)(int)(atan2((double)y,(double)x) * (32768.0/3.14159265358979)); }

// --- GX matrix/TEV/channel state: no-ops for untextured (RHI ignores TEV) -----
// (GXLoadNrmMtxImm is real in gx_draw.c.)
void GXSetChanAmbColor(void) {}
void GXSetChanMatColor(void) {}
void GXSetTevColor(void) {}
void GXSetTevColorS10(void) {}
void GXSetTevKColor(void) {}
void GXSetTevKColorSel(void) {}
void GXSetTevKAlphaSel(void) {}
void GXSetTevIndirect(void) {}
void GXSetIndTexMtx(void) {}
void GXSetIndTexCoordScale(void) {}
void GXSetIndTexOrder(void) {}
// _gxSetFogParams now real in intersect_render.c - removed.

// --- Rcp / TEV stage builders (rcp_dolphin.c / shader_dolphin.c): no-op -------
void Rcp_ResetTextureStageState(void) {}
void Rcp_ApplyTextureStageCounts(void) {}
void* Shader_getLayer(void) { return gDummy; }
int  textureGetAnimationFrame(void) { return 0; }
void addVertexColorStage(void) {}
void addVertexColorKAlphaStage(void) {}
// The real standard render path (objSetupRenderOpGxState -> addShaderLayerStages) binds each
// shader layer's texture through these. The first arg is the ModelTex handle textureLoad
// resolved (via textureIdxToPtr); route it to the RHI base sampler (single-texture) so the
// player and other real-path objects render textured. Remaining TEV/lighting stays stubbed.
struct RhiTexture;
extern void  gx_draw_setTexture(struct RhiTexture* tex);
extern void* stairfax_modeltex_rhi(void* modelTexHandle);
static void bindLayerTex(void* tex) { gx_draw_setTexture((struct RhiTexture*)stairfax_modeltex_rhi(tex)); }
void addTexLayerStage(void* tex) { bindLayerTex(tex); }
void addTexLayerStageKColor(void* tex) { bindLayerTex(tex); }
void addTexLayerStageKAlpha(void* tex) { bindLayerTex(tex); }
void addTexLayerStageSwizzled(void* tex) { bindLayerTex(tex); }
void addKColorModulateStage(void) {}
void addColorFadeStage(void) {}
void addLitColorStage(void) {}
void addAlphaLitColorReg2Stage(void) {}
void addLightTexReg2Stage(void) {}
void addTexModulateReg2Stage(void) {}
int  addEnvMapBumpStages(void) { return 0; }
void addEnvMapTexCoord(void) {}
void addSphereMapTexStage(void) {}
void addProjectedLightTevStage(void) {}
void addSmallReflectionTevStage(void) {}
void addShadowFalloffTevStages(void) {}
void addCastShadowTevStages(void) {}
void addWavyCausticTevStage(void) {}
void addWarpedNoiseTevStages(void) {}
void addRenderOpFadeStage(void) {}
void AttractMovie_AddVideoTevStages(void) {}
int  isHeavyFogEnabled(void) { return 0; }
void renderHeavyFog(void) {}
// getFogColorRgb now real in intersect_render.c - removed.

// --- model light channels (modellight.c): no-op (default flat lighting) -------
void lightGetColor(void) {}
void modelLightChannel_configure(void) {}
void modelLightChannels_applyGXControls(void) {}
void modelLightChannels_reset(void) {}
void ModelLightStruct_free(void) {}
void modelLightStruct_loadChannelLight(void) {}
int  modelLightStruct_getProjectedLightChannelPreference(void) { return 0; }
void* modelLightStruct_getProjectionTexMtx(void) { return gDummy; }
void* modelLightStruct_getProjectionTexture(void) { return gDummy; }
int  modelLightStruct_getProjectionTevModes(void) { return 0; }

// --- newshadows / shadow / hud (fuzz+shadow passes; not the normal pass) ------
void* newshadows_getCausticTexture(void) { return gDummy; }
void* newshadows_getReflectionScrollOffsets(void) { return gDummy; }
void* newshadows_getNoiseTextureFrames(void) { return gDummy; }
void* newshadows_getShadowTextureTable4x8(void) { return gDummy; }
void  getObjectShadowDrawParams(void) {}
int   depthReadRequestPoll(void) { return 0; }
// hudDrawColored now real in intersect_render.c - removed.
