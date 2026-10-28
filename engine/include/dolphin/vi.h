#ifndef _DOLPHIN_VI
#define _DOLPHIN_VI

#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi/vifuncs.h>

#ifdef __cplusplus
extern "C" {
#endif

void VIInit(void);
void VIConfigure(const GXRenderModeObj* rm);
void VIFlush(void);
u32 VIGetTvFormat(void);
u32 VIGetCurrentLine(void);
void VISetNextFrameBuffer(void* fb);
void VIWaitForRetrace(void);
void VISetBlack(BOOL black);

#ifdef TARGET_PC
void VISetWindowTitle(const char* title);
void VISetWindowFullscreen(int fullscreen);   /* vita fork: int, not C99 bool -- game bool is int; */
int VIGetWindowFullscreen(void);              /* these two are declared nowhere else and never called */
#endif



#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_VI
