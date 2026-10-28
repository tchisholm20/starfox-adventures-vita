#include "dolphin/dvd/__dvd.h"

void __DVDPrintFatalMessage(s32 result, DVDFileInfo* fileInfo) {
    if (fileInfo->callback != NULL) {
        fileInfo->callback(result, fileInfo);
    }
}
