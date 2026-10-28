#include "musyx/sal_dsp.h"
#include "musyx/hw_samplemem.h"
#include "dolphin/os.h"
#include "dolphin/os/OSInterrupt.h"

void hwEnableIrq(void) {
}

/* vita fork: on the GameCube "interrupts off" keeps the synth tick (the AI interrupt) out of these sections. On the
   Vita the tick runs on the audio thread (hal/dsp_ai_shim.c), so the same sections take its lock. Lock BEFORE reading
   hwIrqLevel and unlock AFTER writing it: the counter is then only ever touched under the lock. Weak: the viewers
   link MusyX without the audio backend. A no-op unless the audio level is FULL. */
extern int  stfx_audio_lock(void) __attribute__((weak));
extern void stfx_audio_unlock(void) __attribute__((weak));

void sndEnd(void) {
    u16 count = hwIrqLevel - 1;
    hwIrqLevel = count;
    if (count == 0) {
        OSRestoreInterrupts(oldState);
    }
    if (stfx_audio_unlock) stfx_audio_unlock();
}

void sndBegin(void) {
    u16 count;
    if (stfx_audio_lock) stfx_audio_lock();
    count = hwIrqLevel;
    hwIrqLevel = count + 1;
    if (count == 0) {
        oldState = OSDisableInterrupts();
    }
}

void hwIRQEnterCritical(void) {
    OSDisableInterrupts();
}

void hwIRQLeaveCritical(void) {
    OSEnableInterrupts();
}

void* salMalloc(u32 size) {
    return salHooks.mallocHook(size);
}
