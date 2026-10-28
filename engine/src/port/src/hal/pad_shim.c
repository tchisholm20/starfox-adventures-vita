#include "port/pad_shim.h"
#include <string.h>

void PADInit(void) {}
int PADRead(PADStatus* s) { if(s) memset(s,0,sizeof(*s)); return 0; }
void PADControlMotor(int c, int cmd) { (void)c;(void)cmd; }
int PADGetCorrectField(void) { return 0; }

void padUpdate(void) {}
uint32_t getButtonsHeld(int c) { (void)c; return 0; }
uint32_t getButtonsJustPressed(int c) { (void)c; return 0; }
int padGetStickX(int c) { (void)c; return 0; }
int padGetCX(int c) { (void)c; return 0; }
void buttonDisable(int c, uint32_t m) { (void)c;(void)m; }

void pad_shim_poll(void) {}
void pad_shim_setRumble(int c, bool on) { (void)c;(void)on; }
