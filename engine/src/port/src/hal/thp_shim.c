#include <stdio.h>
// THP -> pl_mpeg / ffmpeg shim stub
// SFA uses THP for opening video and some cutscenes (src/main/thp/*)
int THPInit(void){ return 0; }
int THPPlayerPlay(void* p, int loop){(void)p;(void)loop; return 0;}
int THPPlayerStop(void* p){(void)p; return 0;}
