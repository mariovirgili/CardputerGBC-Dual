#pragma once

#include <stdbool.h>
#include <stdint.h>

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
    bool virtualKeyPickerVisible;
    bool runtimePaused;
    bool quitRequested;
    bool resetRequested;
    bool toggleLogsRequested;
    bool toggleViewRequested;
};

struct MsxInputOverlayState {
    bool menuVisible;
    bool performanceSubmenuVisible;
    bool soundSubmenuVisible;
    bool casSubmenuVisible;
    bool debugSubmenuVisible;
    uint8_t debugMenuGroup;
    bool debugLogsEnabled;
    uint32_t debugLogMask;
    bool machineIsMsx2;
    bool joystickEnabled;
    bool keyboardEnabled;
    bool basicKeyboardEnabled;
    bool vausEnabled;
    bool zoomFollowEnabled;
    bool performanceMode;
    MsxPerformancePreset performancePreset;
    bool perfDisableSliceRendering;
    bool perfDisableSpriteCollision;
    bool perfSimplifySpriteOverflow;
    bool perfInstantVdpCommands;
    bool perfExternalFixed30Fps;
    MsxFpsOverlayMode perfFpsOverlayMode;
    MsxFrameskipMode perfFrameskipMode;
    MsxVirtualSccMode virtualSccMode;
    bool sccHardwareDetectEnabled;
    MsxRegionMode regionMode;
    uint8_t soundVolume;
    uint16_t sccGainPercent;
    bool casChangeAvailable;
    bool dskChangeAvailable;
    bool virtualKeyPickerVisible;
    bool runtimePaused;
    char virtualKeyPickerLabel[8];
    uint8_t selectedIndex;
};

struct MsxRuntimeOptionConfig {
    bool joystickEnabled;
    bool keyboardEnabled;
    bool basicKeyboardEnabled;
    bool vausEnabled;
    bool zoomFollowEnabled;
    uint8_t stateSlot;
};

struct MsxInputDiagnosticState {
    bool hasInput;
    bool exitRequested;
    char rawLabel[32];
    char actionLabel[64];
    char joystickLabel[48];
    char keyboardLabel[80];
    char vausLabel[48];
};

void msx_input_init(void);
void msx_input_set_basic_keyboard_enabled(bool enabled);
void msx_input_set_cas_change_available(bool available);
void msx_input_set_dsk_change_available(bool available);
void msx_input_set_runtime_machine_mode(MsxMachineMode mode);
MsxRuntimeOptionConfig msx_input_load_runtime_option_config(void);
MsxRuntimeOptionConfig msx_input_get_runtime_option_config(void);
void msx_input_set_runtime_option_config(const MsxRuntimeOptionConfig& config, bool persist);
void msx_input_poll_diagnostic(const MsxRuntimeOptionConfig& config, MsxInputDiagnosticState* state);
void msx_input_poll(MsxInputState* state);
void msx_input_get_overlay_state(MsxInputOverlayState* state);
uint8_t msx_input_get_state_slot(void);
uint8_t msx_input_get_scroll_index(void);
bool msx_input_get_save_requested(void);
bool msx_input_get_load_requested(void);
bool msx_input_get_change_cas_requested(void);
bool msx_input_get_change_dsk_requested(void);
