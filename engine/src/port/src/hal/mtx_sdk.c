// mtx_sdk.c - the game's matrix/vector library, from the REAL Dolphin SDK (dolsdk2004 src/mtx,
// the SDK generation the game was built against), compiled from its portable C_ versions.
//
// Why (Vita run 26): the camera came out wrong because the link had NO real provider for
// C_MTXPerspective / C_MTXOrtho / PSMTXIdentity / PSMTXMultVec / PSVEC* -- upstream's scaffold
// stand-ins (a left-handed, +Z-forward projection written for upstream's own renderer) filled
// them, weakly. Retail's GXSetProjection (host/engine/stfx_oracle.py, Dolphin) has
// [3][2] = -1; the port's had +1, so everything in front of the camera got w < 0.
// The game's own src/dolphin/mtx is Gekko paired-single asm (matching retail) and cannot build
// for ARM; dolsdk2004's C_ bodies are the same formulas. Each PS* name the game calls is an
// alias of its C_ twin. (PS* ran in paired-single with frsqrte Newton steps: low bits may differ.)
// Supersedes hal/mtx_c.c, whose bodies were copied from these same files.
#define ASSERTMSGLINE(...) ((void)0)
#define ASSERTLINE(...) ((void)0)
#include "sdk_mtx/mtx.inc"
#include "sdk_mtx/mtx44.inc"
#include "sdk_mtx/mtxvec.inc"
#include "sdk_mtx/vec.inc"

#define PS_ALIAS(ps, c) __typeof__(c) ps __attribute__((alias(#c)))
PS_ALIAS(PSMTXConcat, C_MTXConcat);
PS_ALIAS(PSMTXCopy, C_MTXCopy);
PS_ALIAS(PSMTXIdentity, C_MTXIdentity);
PS_ALIAS(PSMTXInverse, C_MTXInverse);
PS_ALIAS(PSMTXMultVec, C_MTXMultVec);
PS_ALIAS(PSMTXMultVecArray, C_MTXMultVecArray);
PS_ALIAS(PSMTXMultVecSR, C_MTXMultVecSR);
PS_ALIAS(PSMTXRotAxisRad, C_MTXRotAxisRad);
PS_ALIAS(PSMTXRotRad, C_MTXRotRad);
PS_ALIAS(PSMTXScale, C_MTXScale);
PS_ALIAS(PSMTXTrans, C_MTXTrans);
PS_ALIAS(PSMTXTranspose, C_MTXTranspose);
PS_ALIAS(PSVECAdd, C_VECAdd);
PS_ALIAS(PSVECCrossProduct, C_VECCrossProduct);
PS_ALIAS(PSVECDistance, C_VECDistance);
PS_ALIAS(PSVECDotProduct, C_VECDotProduct);
PS_ALIAS(PSVECMag, C_VECMag);
PS_ALIAS(PSVECNormalize, C_VECNormalize);
PS_ALIAS(PSVECScale, C_VECScale);
PS_ALIAS(PSVECSquareDistance, C_VECSquareDistance);
PS_ALIAS(PSVECSquareMag, C_VECSquareMag);
PS_ALIAS(PSVECSubtract, C_VECSubtract);
PS_ALIAS(PSMTX44Copy, C_MTX44Copy);
PS_ALIAS(PSMTX44Identity, C_MTX44Identity);

// PSMTXReorder has NO C_ twin in the SDK (dolsdk2004 psmtx.c is paired-single asm only), so the
// vendoring above could not supply it and the link took the bridge's stand-in, `PSMTXCopy(s,d)`
// ("approx; used off the load path", bridge/game_model_support.c). It is ON the render path: the
// game reorders every bone matrix into gObjBoneMtxBuffer (model.c modelInitBoneMtxs*), and the CPU
// skinning reads that buffer the retail way -- the Rev 1 DOL's ObjModel_TransformVerticesWith-
// Translation (0x80029EF0) computes x' = t[0]x + t[3]y + t[6]z + t[9]. A plain copy fed it row-major
// matrices: the title's skinned characters drew as huge exploding shells (Vita runs 44-47).
// Semantics decoded from the SDK asm (its ps_merge00/01/11 pairs): dest[c][r] = src[r][c].
void PSMTXReorder(const Mtx src, ROMtx dest)
{
    Mtx t;   // alias-safe: copy first
    int r, c;
    C_MTXCopy(src, t);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            dest[c][r] = t[r][c];
}
