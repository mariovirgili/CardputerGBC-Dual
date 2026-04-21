#pragma once

#include <stdbool.h>

#include "msx_config.h"
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
    bool basicKeyboardEnabled;
    bool vausEnabled;
    bool menuVisible;
    bool quitRequested;
    bool toggleViewRequested;
};

struct MsxInputOverlayState {
    bool menuVisible;
    bool performanceSubmenuVisible;
    bool machineIsMsx2;
    bool joystickEnabled;
    bool keyboardEnabled;
    bool basicKeyboardEnabled;
    bool vausEnabled;
    bool performanceMode;
    bool perfDisableSliceRendering;
    bool perfDisableSpriteCollision;
    bool perfSimplifySpriteOverflow;
    bool perfInstantVdpCommands;
    bool casChangeAvailable;
    uint8_t selectedIndex;
};

void msx_input_init(void);
void msx_input_set_basic_keyboard_enabled(bool enabled);
void msx_input_set_cas_change_available(bool available);
void msx_input_set_runtime_machine_mode(MsxMachineMode mode);
void msx_input_poll(MsxInputState* state);
void msx_input_get_overlay_state(MsxInputOverlayState* state);
uint8_t msx_input_get_state_slot(void);
uint8_t msx_input_get_scroll_index(void);
bool msx_input_get_save_requested(void);
bool msx_input_get_load_requested(void);
bool msx_input_get_change_cas_requested(void);
