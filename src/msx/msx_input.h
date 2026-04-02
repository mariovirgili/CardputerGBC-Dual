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
    bool quitRequested;
    bool toggleViewRequested;
};

void msx_input_init(void);
void msx_input_poll(MsxInputState* state);
