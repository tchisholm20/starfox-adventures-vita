#include "MSL_C/PPCEABI/bare/H/reciprocal.h"
#include "MSL_C/PPCEABI/bare/H/math_api.h"   /* vita port: __fres (reapply_hooks.py) */

float fastReciprocal(float value) {
    float reciprocal;

    reciprocal = __fres(value);
    reciprocal *= 2.0f - value * reciprocal;
    reciprocal *= 2.0f - value * reciprocal;

    return reciprocal;
}
