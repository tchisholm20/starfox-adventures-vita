#ifndef _TYPES_H_
#define _TYPES_H_

#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))

#include "dolphin/types.h"

#ifndef __cplusplus
typedef int bool;
#ifndef false
#define false 0
#endif
#ifndef true
#define true 1
#endif
#endif

typedef u32 (*VtableFn)();

#endif
