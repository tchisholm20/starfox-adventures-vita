#ifndef _DOLPHIN_THP
#define _DOLPHIN_THP

#include "dolphin/types.h"
#include "dolphin/thp/THPAudio.h"
#include "dolphin/thp/THPVideoDecode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef u8 THPSample;
typedef s16 THPCoeff;
typedef f32 THPQuantTab[64];

typedef struct _THPHuffmanTab {
  u8 quick[32];
  u8 increment[32];
  u8* Vij;
  s32 maxCode[18];
  s32 valPtr[18];
  u8 Vij1;
  u8 pad[11];
} THPHuffmanTab;

typedef struct _THPComponent {
  u8 quantizationTableSelector;
  u8 DCTableSelector;
  u8 ACTableSelector;
  THPCoeff predDC;
} THPComponent;

typedef struct _THPFileInfo {
  THPQuantTab quantTabs[3];
  THPHuffmanTab huffmanTabs[4];
  THPComponent components[3];
  u16 xPixelSize;
  u16 yPixelSize;
  u16 MCUsPerRow;
  u16 decompressedY;
  u8* c;
  u32 currByte;
  u32 cnt;
  u8 validHuffmanTabs;
  u8 RST;
  u16 nMCU;
  u16 currMCU;
  u8* dLC[3];
} THPFileInfo;

void __THPDecompressiMCURow640x480(void);
void __THPDecompressiMCURowNxN(void);


#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_THP
