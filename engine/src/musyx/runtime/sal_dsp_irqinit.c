#include "musyx/sal_dsp.h"
#include "dolphin/os.h"
#include "dolphin/os/OSInterrupt.h"

extern int stfx_audio_lock(void) __attribute__((weak));
void hwInitIrq(void)
{
    if (stfx_audio_lock) stfx_audio_lock();   /* vita fork: level 1 = one sndBegin; hwInit's sndEnd (hw_init.c:87) is its pair */
    oldState = OSDisableInterrupts();
    hwIrqLevel = 1;
}
