#pragma once

#include <stdbool.h>

#include "core/msx_keyboard.h"

struct MsxInputState {
    MsxKeyboardMatrix keyboardMatrix;
    bool up;
    bool down;
    bool left;
    bool right;
    bool fire1;
    bool fire2;
    bool start;
    bool select;
    bool joystickMode;
    bool joystickEnabled;
    bool keyboardEnabled;
    bool vausEnabled;
    bool menuVisible;
    bool quitRequested;
    bool toggleViewRequested;
};

struct MsxInputOverlayState {
    bool menuVisible;
    bool joystickEnabled;
    bool keyboardEnabled;
    bool vausEnabled;
    uint8_t selectedIndex;
};

void msx_input_init(void);
void msx_input_poll(MsxInputState* state);
void msx_input_get_overlay_state(MsxInputOverlayState* state);
