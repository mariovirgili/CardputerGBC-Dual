#include "videopac_input.h"

#include <M5Cardputer.h>

#include <cctype>
#include <cstring>

#include "cardputer/CardputerInput.h"
#include "share/emu_controls.h"
#include "share/input.h"
#include "videopac_trace.h"

namespace {

constexpr uint32_t kGoLongPressMs = 700;
static bool s_goLongHandled = false;
static bool s_menuVisible = false;
static bool s_menuLeftHeld = false;
static bool s_menuRightHeld = false;
static bool s_menuCloseKeyHeld = false;
static bool s_keyboardOnlyMode = false;
static bool s_inputModeToggleHeld = false;

bool key_pressed(const Keyboard_Class::KeysState& keys, char key)
{
    if (key == 0) {
        return false;
    }

    for (char pressed : keys.word) {
        if (pressed == key) {
            return true;
        }

        if (std::isalpha(static_cast<unsigned char>(pressed)) &&
            std::isalpha(static_cast<unsigned char>(key)) &&
            std::tolower(static_cast<unsigned char>(pressed)) ==
                std::tolower(static_cast<unsigned char>(key))) {
            return true;
        }
    }

    return false;
}

void set_ascii_key(VideopacInputState* state, char key)
{
    if (!state || key <= 0 || key >= 128) {
        return;
    }
    state->keys[static_cast<int>(key)] = true;
}

void set_ascii_key_from_status(VideopacInputState* state, char key)
{
    if (key == CARDPUTER_SCREEN_TOGGLE) {
        return;
    }
    set_ascii_key(state, key);
    if (std::isalpha(static_cast<unsigned char>(key))) {
        set_ascii_key(state, static_cast<char>(std::tolower(static_cast<unsigned char>(key))));
    }
}

} // namespace

void videopac_input_init(void)
{
    s_goLongHandled = false;
    s_menuVisible = false;
    s_menuLeftHeld = false;
    s_menuRightHeld = false;
    s_menuCloseKeyHeld = false;
    s_keyboardOnlyMode = false;
    s_inputModeToggleHeld = false;
    videopac_trace_mark("input", "init");
}

void videopac_input_poll(VideopacInputState* state)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    state->keyboardOnlyMode = s_keyboardOnlyMode;

    M5Cardputer.update();
    const auto status = M5Cardputer.Keyboard.keysState();
    const uint32_t padState = share::hasI2cPad() ? share::pollI2cPad() : 0u;
    const bool escPressed = key_pressed(status, KEY_ESC_CUSTOM);
    const bool closeKeyPressed = status.enter || status.del || escPressed;
    if (!closeKeyPressed) {
        s_menuCloseKeyHeld = false;
    }

    const char leftKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Left);
    const char rightKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Right);
    const bool menuLeftPressed = key_pressed(status, leftKey) ||
                                 key_pressed(status, KEY_ARROW_LEFT) ||
                                 key_pressed(status, CARDPUTER_LEFT_1) ||
                                 key_pressed(status, CARDPUTER_LEFT_2) ||
                                 ((padState & share::PAD_LEFT) != 0u);
    const bool menuRightPressed = key_pressed(status, rightKey) ||
                                  key_pressed(status, KEY_ARROW_RIGHT) ||
                                  key_pressed(status, CARDPUTER_RIGHT_1) ||
                                  key_pressed(status, CARDPUTER_RIGHT_2) ||
                                  ((padState & share::PAD_RIGHT) != 0u);

    if (M5Cardputer.BtnA.isPressed()) {
        if (!s_goLongHandled && M5Cardputer.BtnA.pressedFor(kGoLongPressMs)) {
            s_goLongHandled = true;
            s_menuVisible = !s_menuVisible;
            s_menuLeftHeld = false;
            s_menuRightHeld = false;
            state->menuVisible = s_menuVisible;
            state->menuChanged = true;
            videopac_trace_printf("input", "runtime_menu %s", s_menuVisible ? "open" : "close");
        }
    } else {
        if (s_goLongHandled) {
            s_goLongHandled = false;
        } else if (M5Cardputer.BtnA.wasClicked()) {
            if (!s_menuVisible) {
                state->quitRequested = true;
            }
        }
    }

    state->menuVisible = s_menuVisible;
    if (s_menuVisible) {
        if (closeKeyPressed) {
            s_menuVisible = false;
            s_menuLeftHeld = false;
            s_menuRightHeld = false;
            s_menuCloseKeyHeld = true;
            state->menuChanged = true;
            state->menuVisible = false;
            videopac_trace_mark("input", "runtime_menu close_key");
            return;
        }

        if (menuLeftPressed != menuRightPressed) {
            if ((menuLeftPressed && !s_menuLeftHeld) ||
                (menuRightPressed && !s_menuRightHeld)) {
                state->videoModeToggleRequested = true;
                state->videoModeDirection = menuLeftPressed ? -1 : 1;
                videopac_trace_printf("input", "runtime_menu video_toggle dir=%s",
                                      menuLeftPressed ? "left" : "right");
            }
        }
        s_menuLeftHeld = menuLeftPressed;
        s_menuRightHeld = menuRightPressed;
        return;
    }

    if (s_menuCloseKeyHeld && closeKeyPressed) {
        return;
    }

    if (escPressed) {
        state->quitRequested = true;
    }

    if (!state->quitRequested) {
        share::checkCommonInput(status);
    }

    const bool inputModeTogglePressed = key_pressed(status, CARDPUTER_SCREEN_TOGGLE);
    if (inputModeTogglePressed && !s_inputModeToggleHeld) {
        s_keyboardOnlyMode = !s_keyboardOnlyMode;
        state->keyboardOnlyMode = s_keyboardOnlyMode;
        state->inputModeChanged = true;
        videopac_trace_printf("input", "control_mode %s",
                              s_keyboardOnlyMode ? "keyboard" : "joystick");
    }
    s_inputModeToggleHeld = inputModeTogglePressed;

    const char upKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Up);
    const char downKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Down);
    const char actionKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::A);
    const char startKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Start);
    const char selectKey = share::emuControlKey(share::EmuProfile::Videopac, share::EmuAction::Select);

    if (!s_keyboardOnlyMode) {
        state->up = key_pressed(status, upKey) ||
                    key_pressed(status, KEY_ARROW_UP) ||
                    key_pressed(status, CARDPUTER_UP_1) ||
                    key_pressed(status, CARDPUTER_UP_2);
        state->down = key_pressed(status, downKey) ||
                      key_pressed(status, KEY_ARROW_DOWN) ||
                      key_pressed(status, CARDPUTER_DOWN_1) ||
                      key_pressed(status, CARDPUTER_DOWN_2) ||
                      key_pressed(status, CARDPUTER_DOWN_3);
        state->left = key_pressed(status, leftKey) ||
                      key_pressed(status, KEY_ARROW_LEFT) ||
                      key_pressed(status, CARDPUTER_LEFT_1) ||
                      key_pressed(status, CARDPUTER_LEFT_2);
        state->right = key_pressed(status, rightKey) ||
                       key_pressed(status, KEY_ARROW_RIGHT) ||
                       key_pressed(status, CARDPUTER_RIGHT_1) ||
                       key_pressed(status, CARDPUTER_RIGHT_2);
        state->action = key_pressed(status, actionKey) ||
                        key_pressed(status, CARDPUTER_BTN_A_1) ||
                        key_pressed(status, CARDPUTER_BTN_A_2) ||
                        status.enter ||
                        status.space;

        if (padState & share::PAD_UP) state->up = true;
        if (padState & share::PAD_DOWN) state->down = true;
        if (padState & share::PAD_LEFT) state->left = true;
        if (padState & share::PAD_RIGHT) state->right = true;
        if (padState & share::PAD_A) state->action = true;
    } else {
        for (char c : status.word) {
            if (c > 0 && c < 128) {
                set_ascii_key_from_status(state, c);
            }
        }

        if (status.enter) set_ascii_key(state, '\n');
        if (status.space) set_ascii_key(state, ' ');
        if (status.del) set_ascii_key(state, '\b');
        if (key_pressed(status, startKey)) set_ascii_key_from_status(state, startKey);
        if (key_pressed(status, selectKey)) set_ascii_key_from_status(state, selectKey);
    }

    if (state->left && state->right) {
        state->left = false;
        state->right = false;
    }
    if (state->up && state->down) {
        state->up = false;
        state->down = false;
    }
}
