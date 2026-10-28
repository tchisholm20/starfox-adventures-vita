#include "MSL_C/PPCEABI/bare/H/trig_reduce.h"
#include "PowerPC_EABI_Support/Runtime/runtime.h"   /* vita port: __cvt_fp2unsigned (reapply_hooks.py) */
#include "MSL_C/PPCEABI/bare/H/math_api.h"   /* vita port: __fabsf (reapply_hooks.py) */

double trigReduceQuadrantHighPrecision(int* quadrant, float angle) {
    double absoluteAngle = __fabsf(angle);
    double scaledAngle = 1.2732395447351628 * absoluteAngle;
    unsigned int roundedQuadrant = __cvt_fp2unsigned(scaledAngle) + 1 & ~1U;
    *quadrant = roundedQuadrant;
    return absoluteAngle - 0.7853981633974483 * (double)roundedQuadrant;
}
