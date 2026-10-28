/* vita_compat.h - force-included (-include) into upstream TUs that are compiled
 * UNMODIFIED but carry a _WIN32-only helper.
 *
 * model_view.c defines `static void sleep_ms(int)` only under _WIN32 and calls
 * it unconditionally, so on any other target it is an implicit declaration,
 * which gcc 15 rejects. This supplies the same helper for Vita. Nothing else
 * belongs here: every addition is a place where upstream is being quietly
 * patched, and each should be named.
 */
#ifndef STAIRFAX_VITA_COMPAT_H
#define STAIRFAX_VITA_COMPAT_H

#include <psp2/kernel/threadmgr.h>

static inline void sleep_ms(int ms) { sceKernelDelayThread((unsigned) ms * 1000u); }

#endif
