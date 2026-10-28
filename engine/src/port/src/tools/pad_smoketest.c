// pad_smoketest.c - drives pad_input.c's state machine with scripted frames and
// asserts the held/pressed/released + C-stick-synthesis chain the engine reads
// via getButtonsHeld()/getButtonsJustPressed(). No real hardware needed: it
// installs pad_input's test hook to feed deterministic PADStatus frames.

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct PADStatus {
    uint16_t button;
    int8_t   stickX, stickY, substickX, substickY;
    uint8_t  triggerLeft, triggerRight, analogA, analogB;
    int8_t   err;
} PADStatus;

#define PAD_BUTTON_A     0x0100
#define PAD_BUTTON_B     0x0200
#define PAD_BUTTON_START 0x1000
#define CSTICK_RIGHT     0x80000

extern void (*pad_input_test_hook)(PADStatus* status4);
extern int      initControllers(void);
extern void     padUpdate(void);
extern uint32_t getButtonsHeld(int);
extern uint32_t getButtonsJustPressed(int);
extern uint32_t getButtonsJustPressedIfNotBusy(int);
extern int      padGetStickX(int);
extern int      padGetCX(int);

static PADStatus gFeed[4];
static void feedHook(PADStatus* s) { memcpy(s, gFeed, sizeof gFeed); }

static void set0(uint16_t btn, int8_t sx, int8_t cx) {
    memset(gFeed, 0, sizeof gFeed);
    gFeed[0].button = btn; gFeed[0].stickX = sx; gFeed[0].substickX = cx;
    gFeed[1].err = gFeed[2].err = gFeed[3].err = -1; // PAD_ERR_NO_CONTROLLER
}

static int gFails;
static void check(const char* what, uint32_t got, uint32_t want) {
    if (got != want) { printf("  FAIL %-22s got=%lX want=%lX\n", what, (unsigned long)got, (unsigned long)want); gFails++; }
    else               printf("  ok   %-22s = %lX\n", what, (unsigned long)got);
}

int main(void) {
    pad_input_test_hook = feedHook;
    initControllers();

    printf("frame 1: press A\n");
    set0(PAD_BUTTON_A, 0, 0); padUpdate();
    check("held A", getButtonsHeld(0), PAD_BUTTON_A);
    check("justPressed A", getButtonsJustPressed(0), PAD_BUTTON_A);

    printf("frame 2: hold A (no new press)\n");
    set0(PAD_BUTTON_A, 0, 0); padUpdate();
    check("held A", getButtonsHeld(0), PAD_BUTTON_A);
    check("justPressed none", getButtonsJustPressed(0), 0);

    printf("frame 3: release A -> B, full stick + C-stick right\n");
    set0(PAD_BUTTON_B, 90, 90); padUpdate();
    check("held B", getButtonsHeld(0), PAD_BUTTON_B | CSTICK_RIGHT);
    check("justPressed B", getButtonsJustPressed(0), PAD_BUTTON_B | CSTICK_RIGHT);
    check("released has A", getButtonsJustPressedIfNotBusy(0) & PAD_BUTTON_A, PAD_BUTTON_A);
    check("stickX", (uint32_t)(int32_t)padGetStickX(0), 90);
    check("cstickX", (uint32_t)(int32_t)padGetCX(0), 90);

    printf("frame 4: port 1 (no controller) reads clean\n");
    check("held p1", getButtonsHeld(1), 0);
    check("stick p1", (uint32_t)(int32_t)padGetStickX(1), 0);

    printf("\n%s (%d failure%s)\n", gFails ? "SMOKETEST FAILED" : "SMOKETEST PASSED",
           gFails, gFails == 1 ? "" : "s");
    return gFails ? 1 : 0;
}
