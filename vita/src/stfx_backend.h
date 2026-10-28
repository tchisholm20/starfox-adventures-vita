/* stfx_backend.h - which renderer backend this process runs: vitaGL (the default, what the console ships) or the
 * native GXM skeleton (src/rhi_gxm.c, milestone M1).
 *
 * The choice is made ONCE, before vglInit*, by rhi_gl_create() (rhi_vita.c) calling stfx_backend_select(), which reads
 * ux0:data/stairfax/backend.txt: its first word "gxm" selects GXM; an absent file or ANYTHING ELSE is vitaGL.
 * g_stfx_backend is zero (vitaGL) until then, so any code that runs before the choice takes the vitaGL path.
 *
 * THE RULE for every entry point of the vitaGL backend: the first statement is
 *     if (g_stfx_backend == STFX_BK_GXM) return rhi_gxm_<name>(...);
 * so on the gxm path no gl* / vgl* function can run from the backend. The default path pays one load and one compare. */
#ifndef STFX_BACKEND_H
#define STFX_BACKEND_H

#define STFX_BK_VITAGL 0
#define STFX_BK_GXM    1

extern int g_stfx_backend;

/* Idempotent. Reads backend.txt once, prints "[backend] gxm" or "[backend] vitagl" to the game log. */
void stfx_backend_select(void);

#endif
