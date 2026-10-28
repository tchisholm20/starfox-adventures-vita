#pragma once
// pad_shim.h - PAD/SI -> SDL_GameController
// Shadows include/dolphin/pad.h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_BUTTON_A        0x0001
#define PAD_BUTTON_B        0x0002
#define PAD_BUTTON_X        0x0004
#define PAD_BUTTON_Y        0x0008
#define PAD_TRIGGER_Z       0x0010
#define PAD_TRIGGER_R       0x0020
#define PAD_TRIGGER_L       0x0040
#define PAD_BUTTON_START    0x1000
// keep other defines from dolphin/pad.h

typedef struct PADStatus {
    uint16_t button;
    int8_t  stickX, stickY;
    int8_t  substickX, substickY;
    uint8_t triggerL, triggerR;
    uint8_t analogA, analogB;
    int8_t  err;
} PADStatus;

// Original API
void PADInit(void);
int  PADRead(PADStatus* status);
void PADControlMotor(int chan, int cmd);
int  PADGetCorrectField(void);

// Game's wrapper (src/main/pad.c)
void padUpdate(void);
uint32_t getButtonsHeld(int chan);
uint32_t getButtonsJustPressed(int chan);
int  padGetStickX(int chan);
int  padGetCX(int chan);
void buttonDisable(int chan, uint32_t mask);

// Host helpers
void pad_shim_poll(void);
void pad_shim_setRumble(int chan, bool on);

#ifdef __cplusplus
}
#endif
