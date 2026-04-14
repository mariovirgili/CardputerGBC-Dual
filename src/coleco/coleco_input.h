#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ColecoInputState {
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

struct ColecoInputOverlayState {
    bool menuVisible;
    bool joystickEnabled;
    bool keyboardEnabled;
    bool vausEnabled;
    uint8_t selectedIndex;
};

void coleco_input_init(void);
void coleco_input_poll(ColecoInputState* state);
void coleco_input_get_overlay_state(ColecoInputOverlayState* state);
uint8_t coleco_input_get_scroll_index(void);
uint8_t coleco_input_get_state_slot(void);
