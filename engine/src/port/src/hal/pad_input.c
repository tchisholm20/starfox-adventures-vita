// pad_input.c - host-backed GameCube controller for the decomp-native port.
//
// This is a CLEAN reimplementation of the game's src/main/pad.c public surface
// (PADInit / initControllers / padUpdate / getButtonsHeld / ...), backed by real
// host input (Win32 keyboard + XInput pad 0). It lives in port code on purpose:
// the real pad.c reconstructs a single 0xA0-byte PadStateBlock by indexing off
// gPadButtonsPrevious (base+0x10 = "held", base+0x30 = "just pressed", base+0x40
// = the PADStatus ring), which only works because the GameCube link placed those
// four named u32[4] globals adjacently in that exact order. MSVC will not
// reproduce that adjacency, so compiling pad.c unmodified would write out of
// bounds. Here we keep the same behaviour (C-stick->direction synthesis, analog
// menu repeat, digital-trigger thresholds, pressed/released edges) but over
// plainly-declared arrays, so it is safe and identical in observable output.
//
// Keyboard map (port 0):
//   main stick   W A S D          (up/left/down/right)
//   C-stick      I J K L
//   D-pad        arrow keys
//   A Space   B LShift   X E   Y R
//   L-trigger Z   R-trigger C   Z-button Q   Start Enter
// XInput pad 0 (if present) is OR'd/maxed on top of the keyboard each frame.

#if defined(_WIN32)
#include <windows.h>
#include <xinput.h>
#elif defined(__vita__)
#include <psp2/ctrl.h>   /* PS Vita: sceCtrl (vita fork) */
#else
#error "pad_input.c: no host input backend for this platform"
#endif
#include <string.h>
extern float sqrtf(float);   // analog stick magnitude (radial deadzone), CRT without <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// --- GameCube button bits (dolphin/pad.h) ---------------------------------
#define PAD_BUTTON_LEFT  0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN  0x0004
#define PAD_BUTTON_UP    0x0008
#define PAD_TRIGGER_Z    0x0010
#define PAD_TRIGGER_R    0x0020
#define PAD_TRIGGER_L    0x0040
#define PAD_BUTTON_A     0x0100
#define PAD_BUTTON_B     0x0200
#define PAD_BUTTON_X     0x0400
#define PAD_BUTTON_Y     0x0800
#define PAD_BUTTON_START 0x1000

// Synthesized bits pad.c OR's into the extended (u32) button word.
#define PAD_BUTTON_CSTICK_UP    0x10000
#define PAD_BUTTON_CSTICK_DOWN  0x20000
#define PAD_BUTTON_CSTICK_LEFT  0x40000
#define PAD_BUTTON_CSTICK_RIGHT 0x80000
#define PAD_ANALOG_TRIGGER_R    0x20
#define PAD_ANALOG_TRIGGER_L    0x40

#define PAD_ERR_NONE           0
#define PAD_ERR_NO_CONTROLLER -1
#define PAD_ERR_TRANSFER      -3

typedef struct PADStatus {
    uint16_t button;
    int8_t   stickX, stickY;
    int8_t   substickX, substickY;
    uint8_t  triggerLeft, triggerRight;
    uint8_t  analogA, analogB;
    int8_t   err;
} PADStatus;

// --- clean state (no overlay) ---------------------------------------------
static uint32_t sHeld[4];       // current button word (with C-stick bits)
static uint32_t sPrev[4];       // previous button word
static uint32_t sPressed[4];    // rising edges this frame
static uint32_t sReleased[4];   // falling edges this frame
static uint32_t sMask[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };

static uint16_t sTriggers[4], sPrevTriggers[4], sTrigPressed[4], sTrigReleased[4];

static int8_t sStickX[4], sStickY[4], sSubX[4], sSubY[4];
static uint8_t sTrigL[4], sTrigR[4];

static int8_t sLastStickX[4], sLastStickY[4];
static int8_t sMenuXTimer[4], sMenuYTimer[4];
static int8_t sMenuXSign[4], sMenuYSign[4];
static uint8_t sStickRepeatDelay = 5;

static uint8_t sJoypadDisabled;
static uint8_t sRumbleEnabled;

#if defined(_WIN32)
// XInput may be delay-loaded / unavailable; resolve it dynamically so the exe
// still starts on a machine without the DLL.
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION*);
static XInputGetState_t pXInputGetState;
static XInputSetState_t pXInputSetState;
static int sXInputTried;

static void xinputInit(void) {
    if (sXInputTried) return;
    sXInputTried = 1;
    const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3; ++i) {
        HMODULE h = LoadLibraryW(dlls[i]);
        if (h) {
            pXInputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
            pXInputSetState = (XInputSetState_t)GetProcAddress(h, "XInputSetState");
            if (pXInputGetState) return;
        }
    }
}

static int keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Sample real host input into one PADStatus (port 0 only has a device).
static void sampleHost(PADStatus* s) {
    uint16_t b = 0;
    int lx = 0, ly = 0, cx = 0, cy = 0;
    int tl = 0, tr = 0;
    int axa = 0, aya = 0, haveAnalog = 0;   // analog left-stick (XInput), scaled to GC range

    // keyboard
    if (keyDown('A')) lx -= 1;
    if (keyDown('D')) lx += 1;
    if (keyDown('W')) ly += 1;
    if (keyDown('S')) ly -= 1;
    if (keyDown('J')) cx -= 1;
    if (keyDown('L')) cx += 1;
    if (keyDown('I')) cy += 1;
    if (keyDown('K')) cy -= 1;
    if (keyDown(VK_LEFT))  b |= PAD_BUTTON_LEFT;
    if (keyDown(VK_RIGHT)) b |= PAD_BUTTON_RIGHT;
    if (keyDown(VK_UP))    b |= PAD_BUTTON_UP;
    if (keyDown(VK_DOWN))  b |= PAD_BUTTON_DOWN;
    if (keyDown(VK_SPACE))  b |= PAD_BUTTON_A;
    if (keyDown(VK_LSHIFT)) b |= PAD_BUTTON_B;
    if (keyDown('E')) b |= PAD_BUTTON_X;
    if (keyDown('R')) b |= PAD_BUTTON_Y;
    if (keyDown('Q')) b |= PAD_TRIGGER_Z;
    if (keyDown(VK_RETURN)) b |= PAD_BUTTON_START;
    if (keyDown('Z')) { b |= PAD_TRIGGER_L; tl = 255; }
    if (keyDown('C')) { b |= PAD_TRIGGER_R; tr = 255; }

    // XInput pad 0 on top
    xinputInit();
    if (pXInputGetState) {
        XINPUT_STATE xs;
        if (pXInputGetState(0, &xs) == ERROR_SUCCESS) {
            WORD g = xs.Gamepad.wButtons;
            if (g & XINPUT_GAMEPAD_A) b |= PAD_BUTTON_A;
            if (g & XINPUT_GAMEPAD_B) b |= PAD_BUTTON_B;
            if (g & XINPUT_GAMEPAD_X) b |= PAD_BUTTON_X;
            if (g & XINPUT_GAMEPAD_Y) b |= PAD_BUTTON_Y;
            if (g & XINPUT_GAMEPAD_START) b |= PAD_BUTTON_START;
            if (g & XINPUT_GAMEPAD_DPAD_LEFT)  b |= PAD_BUTTON_LEFT;
            if (g & XINPUT_GAMEPAD_DPAD_RIGHT) b |= PAD_BUTTON_RIGHT;
            if (g & XINPUT_GAMEPAD_DPAD_UP)    b |= PAD_BUTTON_UP;
            if (g & XINPUT_GAMEPAD_DPAD_DOWN)  b |= PAD_BUTTON_DOWN;
            if (g & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= PAD_TRIGGER_Z;
            if (xs.Gamepad.bLeftTrigger  > 30) { b |= PAD_TRIGGER_L; if (xs.Gamepad.bLeftTrigger  > tl) tl = xs.Gamepad.bLeftTrigger; }
            if (xs.Gamepad.bRightTrigger > 30) { b |= PAD_TRIGGER_R; if (xs.Gamepad.bRightTrigger > tr) tr = xs.Gamepad.bRightTrigger; }
            // Analog left stick -> GC range: preserve magnitude so gentle input walks and full input
            // runs (the game clamps |input|>=56 to full). Radial deadzone; scale the throw past it to
            // +-100 (matching the old digital "full"). Digitizing this to +-1 was the "always sprints"
            // / "no walk" bug. Keyboard/dpad fall back to the digital +-1 (lx/ly) below.
            {
                int lxr = xs.Gamepad.sThumbLX, lyr = xs.Gamepad.sThumbLY;
                const int DZ = 7849;                 // XInput left-stick deadzone
                float mag = sqrtf((float)lxr*lxr + (float)lyr*lyr);
                if (mag > (float)DZ) {
                    float scaled = ((mag - DZ) / (32767.0f - DZ)) * 100.0f;   // 0..100 past the deadzone
                    if (scaled > 100.0f) scaled = 100.0f;
                    float inv = scaled / mag;
                    axa = (int)(lxr * inv);
                    aya = (int)(lyr * inv);
                    haveAnalog = 1;
                }
            }
            if (xs.Gamepad.sThumbRX >  8000) cx = 1; else if (xs.Gamepad.sThumbRX < -8000) cx = -1;
            if (xs.Gamepad.sThumbRY >  8000) cy = 1; else if (xs.Gamepad.sThumbRY < -8000) cy = -1;
        }
    }

    s->button = b;
    s->stickX = (int8_t)(haveAnalog ? axa : lx * 100);
    s->stickY = (int8_t)(haveAnalog ? aya : ly * 100);
    s->substickX = (int8_t)(cx * 100);
    s->substickY = (int8_t)(cy * 100);
    s->triggerLeft  = (uint8_t)tl;
    s->triggerRight = (uint8_t)tr;
    s->analogA = (b & PAD_BUTTON_A) ? 255 : 0;
    s->analogB = (b & PAD_BUTTON_B) ? 255 : 0;
    s->err = PAD_ERR_NONE;
}

#elif defined(__vita__)
// --- PS Vita host sampling (vita fork) -------------------------------------
// Same PADStatus contract as the Windows path above: port 0 only.
//   main stick  left stick  -- ANALOG, radial deadzone, throw past it scaled to
//                             +-100 exactly like the XInput path (the game clamps
//                             |input|>=56 to full, so walk vs run survives)
//   C-stick     right stick -- digital +-100, as the XInput path does
//   A B X Y     Cross Circle Square Triangle
//   L R         L R shoulders (digital bit + analog 255)
//   Z           Select   (PCH-2000 has no Z, and no L2/R2 without the rear pad)
//   Start       Start;  D-pad -> D-pad
// Vita sticks are 0..255 with 128 centre and Y DOWN-positive; GC Y is up-positive.
static int sCtrlInited;
static void xinputInit(void) {   /* name kept so PADInit is shared; Vita: set analog mode */
    if (sCtrlInited) return;
    sCtrlInited = 1;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
}

static void vitaStick(int rx, int ry, int* ox, int* oy) {
    float x = (float)(rx - 128), y = (float)(128 - ry);      /* up-positive */
    const float DZ = 24.0f;                                   /* of 127 */
    float mag = sqrtf(x * x + y * y);
    if (mag <= DZ) { *ox = 0; *oy = 0; return; }
    float scaled = ((mag - DZ) / (127.0f - DZ)) * 100.0f;
    if (scaled > 100.0f) scaled = 100.0f;
    *ox = (int)(x * scaled / mag);
    *oy = (int)(y * scaled / mag);
}

// Scripted input (vita fork, 2026-09-27): drive the port past the title with nobody at the
// console. ux0:data/stairfax/pad_script.txt, one press per line: "<first PADRead#> <count> <mask>",
// mask in GameCube PAD_BUTTON_* bits (0x1000 START, 0x100 A, 0x200 B), OR'd into the real pad.
// Absent file = no script. Every press is logged ([pad] script ...), so a run says what it was fed.
#define PAD_SCRIPT "ux0:data/stairfax/pad_script.txt"
static struct { unsigned from, count, mask; } sScript[64];
static int sScriptN = -1;
static unsigned sPadReads;
static unsigned sScriptStick;
static uint16_t padScriptButtons(void) {
    unsigned m = 0;
    int i;
    if (sScriptN < 0) {
        FILE* f = fopen(PAD_SCRIPT, "r");
        char line[128];
        sScriptN = 0;
        if (f) {
            while (sScriptN < 64 && fgets(line, sizeof line, f)) {
                unsigned a, n, k;
                if (line[0] != '#' && sscanf(line, "%u %u %x", &a, &n, &k) == 3) {
                    sScript[sScriptN].from = a; sScript[sScriptN].count = n; sScript[sScriptN].mask = k;
                    sScriptN++;
                }
            }
            fclose(f);
            printf("[pad] script: %d presses from %s\n", sScriptN, PAD_SCRIPT);
        }
    }
    for (i = 0; i < sScriptN; i++)
        if (sPadReads >= sScript[i].from && sPadReads < sScript[i].from + sScript[i].count) {
            if (sPadReads == sScript[i].from) printf("[pad] script: read %u press 0x%04x for %u\n", sPadReads, sScript[i].mask, sScript[i].count);
            m |= sScript[i].mask;
        }
    sPadReads++;
    sScriptStick = m >> 16;   /* bits 16..19: main stick left / right / up / down at full deflection (2026-10-28, camera tests) */
    return (uint16_t)m;
}

static void sampleHost(PADStatus* s) {
    SceCtrlData c;
    uint16_t b = 0;
    int ax = 0, ay = 0, cx = 0, cy = 0, tl = 0, tr = 0;
    xinputInit();
    memset(&c, 0, sizeof c);
    if (sceCtrlPeekBufferPositive(0, &c, 1) > 0) {
        unsigned v = c.buttons;
        if (v & SCE_CTRL_CROSS)    b |= PAD_BUTTON_A;
        if (v & SCE_CTRL_CIRCLE)   b |= PAD_BUTTON_B;
        if (v & SCE_CTRL_SQUARE)   b |= PAD_BUTTON_X;
        if (v & SCE_CTRL_TRIANGLE) b |= PAD_BUTTON_Y;
        if (v & SCE_CTRL_START)    b |= PAD_BUTTON_START;
        if (v & SCE_CTRL_SELECT)   b |= PAD_TRIGGER_Z;
        if (v & SCE_CTRL_LEFT)     b |= PAD_BUTTON_LEFT;
        if (v & SCE_CTRL_RIGHT)    b |= PAD_BUTTON_RIGHT;
        if (v & SCE_CTRL_UP)       b |= PAD_BUTTON_UP;
        if (v & SCE_CTRL_DOWN)     b |= PAD_BUTTON_DOWN;
        if (v & SCE_CTRL_LTRIGGER) { b |= PAD_TRIGGER_L; tl = 255; }
        if (v & SCE_CTRL_RTRIGGER) { b |= PAD_TRIGGER_R; tr = 255; }
        vitaStick(c.lx, c.ly, &ax, &ay);
        if (c.rx > 128 + 48) cx = 1; else if (c.rx < 128 - 48) cx = -1;
        if (c.ry < 128 - 48) cy = 1; else if (c.ry > 128 + 48) cy = -1;   /* up-positive */
    }
    b |= padScriptButtons();
    if (sScriptStick & 1) ax = -100;
    if (sScriptStick & 2) ax = 100;
    if (sScriptStick & 4) ay = 100;
    if (sScriptStick & 8) ay = -100;
    if (b & PAD_TRIGGER_L) tl = 255;   /* a scripted L/R is a full trigger pull: the game reads the analog value (padGetTriggersPressed) */
    if (b & PAD_TRIGGER_R) tr = 255;
    s->button = b;
    s->stickX = (int8_t)ax;
    s->stickY = (int8_t)ay;
    s->substickX = (int8_t)(cx * 100);
    s->substickY = (int8_t)(cy * 100);
    s->triggerLeft  = (uint8_t)tl;
    s->triggerRight = (uint8_t)tr;
    s->analogA = (b & PAD_BUTTON_A) ? 255 : 0;
    s->analogB = (b & PAD_BUTTON_B) ? 255 : 0;
    s->err = PAD_ERR_NONE;
}

#endif

// Test seam: when non-null this replaces the real host sample and fills all
// four PADStatus entries. Null in production (real keyboard/XInput sampling).
void (*pad_input_test_hook)(PADStatus* status4) = 0;

// --- original API ----------------------------------------------------------
int PADInit(void) { xinputInit(); return 1; }

// Fill all four ports; only port 0 has a device.
uint32_t PADRead(PADStatus* status) {
    if (!status) return 0;
    if (pad_input_test_hook) { pad_input_test_hook(status); return 0; }
    sampleHost(&status[0]);
    for (int i = 1; i < 4; ++i) {
        memset(&status[i], 0, sizeof(PADStatus));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0;
}

void PADControlMotor(int chan, int cmd) {
    (void)chan;
#if !defined(_WIN32)
    (void)cmd;   /* PS Vita PCH-2000 has no rumble motor (vita fork) */
#else
    xinputInit();
    if (pXInputSetState) {
        XINPUT_VIBRATION v;
        WORD lvl = (cmd == 1 /*PAD_MOTOR_RUMBLE*/) ? 40000 : 0;
        v.wLeftMotorSpeed = lvl;
        v.wRightMotorSpeed = lvl;
        pXInputSetState(0, &v);
    }
#endif
}

int PADGetCorrectField(void) { return 0; }

int initControllers(void) {
    memset(sHeld, 0, sizeof sHeld);
    memset(sPrev, 0, sizeof sPrev);
    memset(sPressed, 0, sizeof sPressed);
    memset(sReleased, 0, sizeof sReleased);
    memset(sTriggers, 0, sizeof sTriggers);
    memset(sPrevTriggers, 0, sizeof sPrevTriggers);
    memset(sTrigPressed, 0, sizeof sTrigPressed);
    memset(sTrigReleased, 0, sizeof sTrigReleased);
    memset(sStickX, 0, sizeof sStickX);
    memset(sStickY, 0, sizeof sStickY);
    memset(sSubX, 0, sizeof sSubX);
    memset(sSubY, 0, sizeof sSubY);
    memset(sLastStickX, 0, sizeof sLastStickX);
    memset(sLastStickY, 0, sizeof sLastStickY);
    memset(sMenuXTimer, 0, sizeof sMenuXTimer);
    memset(sMenuYTimer, 0, sizeof sMenuYTimer);
    memset(sMenuXSign, 0, sizeof sMenuXSign);
    memset(sMenuYSign, 0, sizeof sMenuYSign);
    for (int i = 0; i < 4; ++i) sMask[i] = 0xFFFFFFFFu;
    sRumbleEnabled = 1;
    PADInit();
    return 0;
}

// Who is polling (2026-09-27): a wait loop that keeps calling padUpdate without presenting is a
// stall the watchdog's stall report can then NAME without faulting (vita_tool_main.c).
void* gStfxPadCaller;
unsigned gStfxPadReads;
void padUpdate(void) {
    PADStatus pads[4];
    gStfxPadCaller = __builtin_return_address(0);
    ++gStfxPadReads;
    PADRead(pads);
    /* THE CLAMP (vita fork, 2026-10-10, docs/47 "the platform-layer audit"): the game's padUpdate (main/pad.c:96) calls PADClamp
       right after PADRead. This one never did, so the sticks reached the game raw (0..100, triggers 0..255) where retail
       sees the dead zone removed and 0..57 on the stick, 0..44 on the C-stick, 0..150 on the triggers: every speed scaled
       by stick deflection ran ~1.75x. ux0:data/stairfax/padclamp_off.txt starting with 1 = raw, as before run 181. */
    {   static int sRaw = -1;
        if (sRaw < 0) { sRaw = 0;
#ifdef __vita__
            { FILE* f = fopen("ux0:data/stairfax/padclamp_off.txt", "r"); sRaw = f && fgetc(f) == 49; if (f) fclose(f); }
            printf("[pad] PADClamp after PADRead: %s\n", sRaw ? "OFF (padclamp_off.txt)" : "on (retail)");
#endif
        }
        if (!sRaw) { extern void PADClamp(PADStatus* status); PADClamp(pads); }
    }
    sJoypadDisabled = 0;

    for (int i = 0; i < 4; ++i) {
        PADStatus* p = &pads[i];
        if (p->err == PAD_ERR_NO_CONTROLLER) {
            sHeld[i] = sPrev[i] = sPressed[i] = sReleased[i] = 0;
            sTriggers[i] = sPrevTriggers[i] = sTrigPressed[i] = sTrigReleased[i] = 0;
            sStickX[i] = sStickY[i] = sSubX[i] = sSubY[i] = 0;
            sTrigL[i] = sTrigR[i] = 0;
            continue;
        }

        uint32_t cur = p->button;
        if (p->substickY < -40) cur |= PAD_BUTTON_CSTICK_DOWN;
        if (p->substickY >  40) cur |= PAD_BUTTON_CSTICK_UP;
        if (p->substickX < -40) cur |= PAD_BUTTON_CSTICK_LEFT;
        if (p->substickX >  40) cur |= PAD_BUTTON_CSTICK_RIGHT;

        sPressed[i]  = cur & (cur ^ sPrev[i]);
        sReleased[i] = sPrev[i] & (cur ^ sPrev[i]);
        sPrev[i] = cur;
        sHeld[i] = cur;

        sStickX[i] = p->stickX;
        sStickY[i] = p->stickY;
        sSubX[i] = p->substickX;
        sSubY[i] = p->substickY;
        sTrigL[i] = p->triggerLeft;
        sTrigR[i] = p->triggerRight;

        uint16_t t = 0;
        if (p->triggerRight > 10) t |= PAD_ANALOG_TRIGGER_R;
        if (p->triggerLeft  > 10) t |= PAD_ANALOG_TRIGGER_L;
        sTrigPressed[i]  = t & (t ^ sPrevTriggers[i]);
        sTrigReleased[i] = sPrevTriggers[i] & (t ^ sPrevTriggers[i]);
        sPrevTriggers[i] = t;
        sTriggers[i] = t;

        // analog "menu" one-shot + repeat, mirroring pad.c
        int sx = p->stickX, sy = p->stickY;
        sMenuXSign[i] = 0;
        sMenuYSign[i] = 0;
        if (sx < -35 && sLastStickX[i] >= -35) { sMenuXSign[i] = -1; sMenuXTimer[i] = 0; }
        if (sx >  35 && sLastStickX[i] <=  35) { sMenuXSign[i] =  1; sMenuXTimer[i] = 0; }
        if (sy < -35 && sLastStickY[i] >= -35) { sMenuYSign[i] = -1; sMenuYTimer[i] = 0; }
        if (sy >  35 && sLastStickY[i] <=  35) { sMenuYSign[i] =  1; sMenuYTimer[i] = 0; }
        sLastStickY[i] = (int8_t)sy;
        if (sy < -35 || sy > 35) sMenuYTimer[i]++; else sMenuYTimer[i] = 0;
        if (sMenuYTimer[i] > (int8_t)sStickRepeatDelay) { sLastStickY[i] = 0; sMenuYTimer[i] = 0; }
        sLastStickX[i] = (int8_t)sx;
        if (sx < -35 || sx > 35) sMenuXTimer[i]++; else sMenuXTimer[i] = 0;
        if (sMenuXTimer[i] > (int8_t)sStickRepeatDelay) { sLastStickX[i] = 0; sMenuXTimer[i] = 0; }
        // pad.c: `gPadButtonMask[port] = -1;` on EVERY update -- buttonDisable() suppresses a button
        // only until the next frame. Missing here (vita fork, 2026-09-27): the attract loop's
        // buttonDisable(0, buttons) masked START/A for good, and the title menu, with every one of its
        // own gates open (run 78's [menu] line), never saw A.
        sMask[i] = 0xFFFFFFFFu;
    }

    if ((sHeld[0] || sPressed[0]) && getenv("STAIRFAX_PAD_TRACE")) {
        fprintf(stderr, "[pad0] held=%05lX pressed=%05lX stick=%d,%d cstick=%d,%d L=%d R=%d\n",
                (unsigned long)sHeld[0], (unsigned long)sPressed[0],
                sStickX[0], sStickY[0], sSubX[0], sSubY[0], sTrigL[0], sTrigR[0]);
    }
}

// --- game wrappers (clean readers) ----------------------------------------
// Headless test hook: STAIRFAX_FORCE_BUTTONS (hex, e.g. 0x10=Z 0x40=L 0x20=R) OR'd into held/pressed
// so camera-mode context switches (engine/66 Z->viewfinder etc.) can be triggered without a controller.
static uint32_t sfForcedButtons(void) { const char* f = getenv("STAIRFAX_FORCE_BUTTONS"); return f ? (uint32_t)strtoul(f, 0, 0) : 0u; }
uint32_t getButtonsHeld(int port)            { return (port > 0 || sJoypadDisabled) ? 0 : ((sHeld[port] | sfForcedButtons()) & sMask[port]); }
uint32_t getButtonsJustPressed(int port)     { return (port > 0 || sJoypadDisabled) ? 0 : ((sPressed[port] | sfForcedButtons()) & sMask[port]); }
uint32_t getButtonsJustPressedIfNotBusy(int port) { if (port > 0) return 0; if (sJoypadDisabled) return 0xFFFFFFFFu; return sReleased[port] & sMask[port]; }
uint32_t getNewInputs(int port)              { return port > 0 ? 0 : sHeld[port]; }

// Headless turn test: STAIRFAX_STICK_ROTATE=<period frames> sweeps the stick direction in a circle so
// the player continuously TURNS (a straight forced stick just converges to one heading). Reproduces
// camera behaviour during turns. sfStickFrame advances once per frame (in padGetStickY, read second).
extern float sinf(float); extern float cosf(float);
static int sfStickFrame = 0;
static int sfRotatePeriod(void) { const char* r = getenv("STAIRFAX_STICK_ROTATE"); if (!r) return 0; int p = atoi(r); return p < 10 ? 120 : p; }
int  padGetStickX(int port) { int p; const char* f;
                              if (port==0 && (p=sfRotatePeriod())) return (int)(100.0f * cosf(6.2831853f*(float)(sfStickFrame % p)/(float)p));
                              if (port==0 && (f=getenv("STAIRFAX_FORCE_STICKX"))) return atoi(f);
                              return (port > 0 || sJoypadDisabled) ? 0 : sStickX[port]; }
int  padGetStickY(int port) { int p; const char* f;
                              if (port==0 && (p=sfRotatePeriod())) { int fr = sfStickFrame++; return (int)(100.0f * sinf(6.2831853f*(float)(fr % p)/(float)p)); }
                              if (port==0 && (f=getenv("STAIRFAX_FORCE_STICKY"))) return atoi(f);
                              return (port > 0 || sJoypadDisabled) ? 0 : sStickY[port]; }
int  padGetCX(int port)     { return (port > 0 || sJoypadDisabled) ? 0 : sSubX[port]; }
int  padGetCY(int port)     { return (port > 0 || sJoypadDisabled) ? 0 : sSubY[port]; }
unsigned char padGetLTrigger(int port) { return sJoypadDisabled ? 0 : sTrigL[port]; }
unsigned char padGetRTrigger(int port) { return sJoypadDisabled ? 0 : sTrigR[port]; }
// STAIRFAX_FORCE_TRIGGERS (hex, L=0x40 Z=0x10 R=0x20) OR'd in so the analog-trigger camera switches
// (engine/66 L->staffanim uses padGetTriggersPressed, not getButtonsHeld) can be triggered headlessly.
static uint16_t sfForcedTriggers(void) { const char* f = getenv("STAIRFAX_FORCE_TRIGGERS"); return f ? (uint16_t)strtoul(f, 0, 0) : 0u; }
uint16_t padGetTriggers(int port)        { if (port > 0) port = 0; return sJoypadDisabled ? 0 : (sTriggers[port] | sfForcedTriggers()); }
uint16_t padGetTriggersPressed(int port) { if (port > 0) port = 0; return sJoypadDisabled ? 0 : (sTrigPressed[port] | sfForcedTriggers()); }

void padGetAnalogInput(int port, int8_t* x, int8_t* y) {
    if (sJoypadDisabled || port > 0) { *x = 0; *y = 0; return; }
    *x = sMenuXSign[port];
    *y = sMenuYSign[port];
}
void padClearAnalogInputX(int port) { sMenuXSign[port] = 0; }
void padClearAnalogInputY(int port) { sMenuYSign[port] = 0; }

uint32_t buttonGetDisabled(int port) { return ~sMask[port]; }
void buttonDisable(int port, uint32_t mask) { sMask[port] &= ~mask; }
void setJoypadDisabled(void) { sJoypadDisabled = 1; }
void padSetStickRepeatDelay(int delay) { sStickRepeatDelay = (uint8_t)delay; }

// --- rumble ---------------------------------------------------------------
void setRumbleEnabled(unsigned char enabled) { sRumbleEnabled = enabled; }
void stopRumble(void)  { if (sRumbleEnabled) PADControlMotor(0, 0 /*STOP*/); }
void stopRumble2(void) { if (sRumbleEnabled) PADControlMotor(0, 2 /*STOP_HARD*/); }
void doRumble(float duration) { (void)duration; if (sRumbleEnabled) PADControlMotor(0, 1 /*RUMBLE*/); }

// --- PADClamp / PADReset / PADRecalibrate (vita fork) -------------------------------
// PADClamp: VERBATIM from dolsdk2004 src/pad/Padclamp.c (ClampCircle unused, dropped).
// The game's pad.c calls it right after PADRead, as on the console, so the backends
// above hand it RAW +-100 sticks -- the same domain a GameCube controller produces.
typedef int8_t s8_pc; typedef uint8_t u8_pc;
#define s8 s8_pc
#define u8 u8_pc
static const struct { uint8_t minTrigger, maxTrigger; int8_t minStick, maxStick, xyStick, minSubstick, maxSubstick, xySubstick, radStick, radSubstick; } ClampRegion = {
    // Triggers
    30,
    180,

    // Left stick
    15,
    72,
    40,

    // Right stick
    15,
    59,
    31,

    // Stick radii
    56,
    44,
};

static void ClampStick(s8* px, s8* py, s8 max, s8 xy, s8 min) {
    int x = *px;
    int y = *py;
    int signX;
    int signY;
    int d;

    if (0 <= x) {
        signX = 1;
    } else {
        signX = -1;
        x = -x;
    }

    if (0 <= y) {
        signY = 1;
    } else {
        signY = -1;
        y = -y;
    }

    if (x <= min) {
        x = 0;
    } else {
        x -= min;
    }
    if (y <= min) {
        y = 0;
    } else {
        y -= min;
    }

    if (x == 0 && y == 0) {
        *px = *py = 0;
        return;
    }

    if (xy * y <= xy * x) {
        d = xy * x + (max - xy) * y;
        if (xy * max < d) {
            x = (s8)(xy * max * x / d);
            y = (s8)(xy * max * y / d);
        }
    } else {
        d = xy * y + (max - xy) * x;
        if (xy * max < d) {
            x = (s8)(xy * max * x / d);
            y = (s8)(xy * max * y / d);
        }
    }

    *px = (s8)(signX * x);
    *py = (s8)(signY * y);
}

static void ClampTrigger(u8* trigger, u8 min, u8 max) {
    if (*trigger <= min) {
        *trigger = 0;
    } else {
        if (max < *trigger) {
            *trigger = max;
        }
        *trigger -= min;
    }
}

void PADClamp(PADStatus * status) {
    int i;

    for (i = 0; i < 4; i++, status++) {
        if (status->err == 0) {
            ClampStick(&status->stickX, &status->stickY, ClampRegion.maxStick, ClampRegion.xyStick, ClampRegion.minStick);
            ClampStick(&status->substickX, &status->substickY, ClampRegion.maxSubstick, ClampRegion.xySubstick, ClampRegion.minSubstick);
            ClampTrigger(&status->triggerLeft, ClampRegion.minTrigger, ClampRegion.maxTrigger);
            ClampTrigger(&status->triggerRight, ClampRegion.minTrigger, ClampRegion.maxTrigger);
        }
    }
}
#undef s8
#undef u8
// No controller hardware to reset or recalibrate: report success, as a present pad does.
int PADReset(uint32_t mask) { (void)mask; return 1; }
int PADRecalibrate(uint32_t mask) { (void)mask; return 1; }
