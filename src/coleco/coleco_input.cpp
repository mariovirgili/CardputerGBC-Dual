#include "coleco_input.h"
#include <M5Cardputer.h>
#include "share/input.h"
#include "share/emu_controls.h"
#include "cardputer/CardputerInput.h"

// Key mapping state
static bool menuVisible = false;

void coleco_input_init(void) {
    menuVisible = false;
}

void coleco_input_poll(ColecoInputState* state) {
    if (!state) return;

    state->up = false;
    state->down = false;
    state->left = false;
    state->right = false;
    state->fire1 = false;
    state->fire2 = false;
    state->start = false;
    state->select = false;
    state->menuVisible = menuVisible;
    state->quitRequested = false;
    state->toggleViewRequested = false;

    M5Cardputer.update();
    auto status = M5Cardputer.Keyboard.keysState();
    share::checkCommonInput(status);

    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) state->up = true;
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) state->down = true;
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) state->left = true;
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) state->right = true;
    
    // Mapped via standard emu controls
    char k_a = share::emuControlKey(share::EmuProfile::MSX, share::EmuAction::A);
    char k_b = share::emuControlKey(share::EmuProfile::MSX, share::EmuAction::B);
    char k_start = share::emuControlKey(share::EmuProfile::MSX, share::EmuAction::Start);
    char k_select = share::emuControlKey(share::EmuProfile::MSX, share::EmuAction::Select);

    if (M5Cardputer.Keyboard.isKeyPressed(k_a) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_2)) state->fire1 = true;
    if (M5Cardputer.Keyboard.isKeyPressed(k_b) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) state->fire2 = true;
    if (M5Cardputer.Keyboard.isKeyPressed(k_start) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START)) state->start = true;
    if (M5Cardputer.Keyboard.isKeyPressed(k_select) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_SELECT)) state->select = true;
    
    if (M5Cardputer.Keyboard.isKeyPressed('\\')) state->toggleViewRequested = true;

    uint32_t padState = share::pollI2cPad();
    if (padState & share::PAD_UP) state->up = true;
    if (padState & share::PAD_DOWN) state->down = true;
    if (padState & share::PAD_LEFT) state->left = true;
    if (padState & share::PAD_RIGHT) state->right = true;
    if (padState & share::PAD_A) state->fire1 = true;
    if (padState & share::PAD_B) state->fire2 = true;
    if (padState & share::PAD_START) state->start = true;
    if (padState & share::PAD_SELECT) state->select = true;
}

void coleco_input_get_overlay_state(ColecoInputOverlayState* state) {
    if (!state) return;
    state->menuVisible = menuVisible;
    state->joystickEnabled = true;
    state->keyboardEnabled = false;
    state->vausEnabled = false;
    state->selectedIndex = 0;
}

uint8_t coleco_input_get_scroll_index(void) {
    return 0;
}

uint8_t coleco_input_get_state_slot(void) {
    return 0;
}
