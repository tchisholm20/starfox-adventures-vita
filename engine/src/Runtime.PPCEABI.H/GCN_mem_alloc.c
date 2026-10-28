#include "dolphin/os.h"
#include "Runtime.PPCEABI.H/GCN_mem_alloc.h"

int gDefaultHeapInitialized;

inline static void InitDefaultHeap(void) {
    void* arenaLo;
    void* arenaHi;

    arenaLo = OSGetArenaLo();
    arenaHi = OSGetArenaHi();
    arenaLo = OSInitAlloc(arenaLo, arenaHi, 1);
    OSSetArenaLo(arenaLo);
    arenaLo = (void*)(((u32)arenaLo + 0x1F) & ~0x1F);
    arenaHi = (void*)((u32)arenaHi & ~0x1F);
    OSSetCurrentHeap(OSCreateHeap(arenaLo, arenaHi));
    OSSetArenaLo(arenaHi);
    gDefaultHeapInitialized = 1;
}

void __sys_free(void* p) {
    if (!gDefaultHeapInitialized) {
        InitDefaultHeap();
    }

    OSFreeToHeap(0, p);
}
