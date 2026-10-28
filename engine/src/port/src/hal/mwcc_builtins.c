// mwcc_builtins.c - host definitions of CodeWarrior/PPC builtins the game calls as
// ordinary functions. On GameCube they are single instructions; the game's MSL
// headers declare them extern (math_api.h, MSL_Common/math.h), so the host must
// provide real external definitions with those exact signatures. (vita fork)
#include <math.h>

double __fabs(double x)    { return fabs(x); }
float  __fabsf(float x)    { return fabsf(x); }
double __frsqrte(double x) { return 1.0 / sqrt(x); }   /* exact, not the PPC ~1/32 estimate */
float  __fres(float x)     { return 1.0f / x; }        /* exact, not the PPC ~1/4096 estimate */
int    __cntlzw(unsigned int x) { return x ? __builtin_clz(x) : 32; }   /* PPC cntlzw: 32 for 0 */
/* CodeWarrior runtime double->unsigned: SATURATING -- negative -> 0, >= 2^32 -> 0xFFFFFFFF,
 * else truncate (a plain C cast is undefined out of range). Semantics recalled from the CW
 * runtime, not read from source here: confirm with tools/sfa2/oracle.py when it matters. */
unsigned int __cvt_fp2unsigned(double d) {
    if (!(d > 0.0)) return 0u;                 /* negative, zero and NaN -> 0 */
    if (d >= 4294967296.0) return 0xFFFFFFFFu;
    return (unsigned int)d;
}
/* OSFastCast paired-single quantized load/store (math_float_helpers.c is Gekko asm).
 * OSInitFastCast programs GQR2-5 with the type only and scale 0, so a load is an
 * exact integer->float conversion. A store SATURATES to the integer range and then
 * truncates toward zero -- recalled from Dolphin's interpreter (ScaleAndClamp), not
 * read here: confirm with tools/sfa2/oracle.py if a caller depends on the edges.
 * NaN stores 0 (unspecified here; a C cast of NaN is undefined). */
float fastCastS16ToFloat(const short* input) { return (float)*input; }
float fastCastU16ToFloat(const unsigned short* input) { return (float)*input; }
void fastCastFloatToS16(float v, short* out) {
    *out = (v != v) ? 0 : v <= -32768.0f ? (short)-32768 : v >= 32767.0f ? (short)32767 : (short)v;
}
void fastCastFloatToU16(float v, unsigned short* out) {
    *out = (v != v) ? 0 : v <= 0.0f ? (unsigned short)0 : v >= 65535.0f ? (unsigned short)65535 : (unsigned short)v;
}
