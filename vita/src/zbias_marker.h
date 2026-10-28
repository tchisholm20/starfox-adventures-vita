/* zbias_marker.h -- ux0:data/stairfax/shadow_zbias.txt: "factor units" for the shadow receiver's depth bias (pure; host/tests/build_zbias_test.sh).
 * Absent / unparsable = the default below; "0 0" = off (today's behaviour). Values are vitaGL glPolygonOffset arguments, which it casts to
 * int for sceGxmSetFrontDepthBias, so they are read as integers (a fraction is truncated toward zero, as vitaGL will). */
#ifndef STFX_ZBIAS_MARKER_H
#define STFX_ZBIAS_MARKER_H
#include <stdlib.h>
#define ZB_DEFAULT_FACTOR (-1)
#define ZB_DEFAULT_UNITS  (-2)
/* returns 1 if the text held two numbers (*f, *u set), 0 otherwise (*f, *u = the defaults) */
static inline int zb_parse(const char *buf, int len, int *f, int *u)
{
    char t[64]; double a, b; char *e1, *e2;
    *f = ZB_DEFAULT_FACTOR; *u = ZB_DEFAULT_UNITS;
    if (!buf || len <= 0) return 0;
    if (len > 63) len = 63;
    for (int i = 0; i < len; ++i) t[i] = buf[i];
    t[len] = 0;
    a = strtod(t, &e1);
    if (e1 == t) return 0;
    b = strtod(e1, &e2);
    if (e2 == e1) return 0;
    if (a < -1000 || a > 1000 || b < -100000 || b > 100000) return 0;
    *f = (int) a; *u = (int) b;
    return 1;
}
#endif
