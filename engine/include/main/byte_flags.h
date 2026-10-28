#ifndef MAIN_BYTE_FLAGS_H_
#define MAIN_BYTE_FLAGS_H_

#include "global.h"

typedef struct ByteFlags
{
/* port: names ARE the byte's masks; GCC/ARM allocates bitfields from the LSB, PowerPC from the MSB (reapply_hooks.py) */
#if defined(TARGET_PC)
    u8 b01 : 1;
    u8 b02 : 1;
    u8 b04 : 1;
    u8 b08 : 1;
    u8 b10 : 1;
    u8 b20 : 1;
    u8 b40 : 1;
    u8 b80 : 1;
} ByteFlags;
#else
    u8 b80 : 1;
    u8 b40 : 1;
    u8 b20 : 1;
    u8 b10 : 1;
    u8 b08 : 1;
    u8 b04 : 1;
    u8 b02 : 1;
    u8 b01 : 1;
} ByteFlags;
#endif

#endif /* MAIN_BYTE_FLAGS_H_ */
