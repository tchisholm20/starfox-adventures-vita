#include "dolphin.h"
#include "stdlib.h"

static u32 sRandState;

#if defined(TARGET_PC)
int rand(void) {   /* vita fork: libc prototype; same bits, same ABI */
#else
u32 rand(void) {
#endif
    sRandState = sRandState * 0x19660D + 0x3C6EF35F;
    return sRandState;
}

void srand(u32 seed) {
    sRandState = seed;
}
