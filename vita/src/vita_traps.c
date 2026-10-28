/* vita_traps.c - symbols upstream references on paths this build must never take.
 *
 * Each one ABORTS with a trace instead of returning something plausible. A
 * no-op here would be exactly the fabricated stub this repo has been burned by
 * (docs/29: a `return 0` that silently replaced real code for a whole binary).
 * If one of these fires, a path this project believes is unreachable is not.
 */

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <string.h>

static void trap(const char *what)
{
    SceUID fd = sceIoOpen("ux0:data/stairfax_model.txt",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, "TRAP ", 5);
        sceIoWrite(fd, what, (unsigned) strlen(what));
        sceIoWrite(fd, " reached\n", 9);
        sceIoClose(fd);
    }
    sceKernelExitProcess(99);
}

/* dvd_shim.c byte-swaps OBJECTS.bin ObjDefs through this (file id 0x3e). The
 * real one is src/bridge/game_objdef_swap.c, which needs the game header tree.
 * model_view only reads MODELS/TEX1 and never reaches it. */
void bswapObjDef(void *p)
{
    (void) p;
    trap("bswapObjDef");
}
