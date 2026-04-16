#include "coleco_input.h"
#include <M5Cardputer.h>
#include "share/input.h"
#include "share/emu_controls.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/ConfirmationSelector.h"
#include "coleco_config.h"
#include "coleco_video.h"
#include "share/display_target.h"

static constexpr uint32_t kGoLongPressMs = 700;
static bool s_goLongHandled = false;
static bool s_suppressGoClick = false;
static uint32_t s_suppressGoUntilMs = 0;
static bool s_fnSaveLoadHandled = false;

struct ColecoRuntimeOptions {
    bool joystickEnabled;
    bool keyboardEnabled;
    bool vausEnabled;
    uint8_t stateSlot;
    bool saveRequested;
    bool loadRequested;
};

enum class ColecoRuntimeMenuItem : uint8_t {
    View = 0,
    StateSlot,
    SaveState,
    LoadState,
    Close,
    Count,
};

struct ColecoRuntimeMenuState {
    bool visible;
    uint8_t selectedIndex;
    uint8_t scroll;
    bool prevHeld;
    bool nextHeld;
    bool acceptHeld;
    bool backHeld;
    bool leftHeld;
    bool rightHeld;
};

static ColecoRuntimeOptions s_runtimeOptions = {true, false, false, 0, false, false};
static ColecoRuntimeMenuState s_runtimeMenu = {false, 0, 0, false, false, false, false, false, false};

static bool coleco_view_toggle_allowed(void) {
    return g_emu_display_target != EMU_DISPLAY_EXTERNAL;
}

static uint8_t coleco_get_menu_item_count(void) {
    return coleco_view_toggle_allowed() ? 5 : 4;
}

static ColecoRuntimeMenuItem coleco_get_menu_item(uint8_t index) {
    if (!coleco_view_toggle_allowed()) {
        switch (index) {
            case 0: return ColecoRuntimeMenuItem::StateSlot;
            case 1: return ColecoRuntimeMenuItem::SaveState;
            case 2: return ColecoRuntimeMenuItem::LoadState;
            case 3: return ColecoRuntimeMenuItem::Close;
            default: return ColecoRuntimeMenuItem::Close;
        }
    }
    return static_cast<ColecoRuntimeMenuItem>(index);
}

static void coleco_reset_menu_latches(void) {
    s_runtimeMenu.prevHeld = false;
    s_runtimeMenu.nextHeld = false;
    s_runtimeMenu.acceptHeld = false;
    s_runtimeMenu.backHeld = false;
    s_runtimeMenu.leftHeld = false;
    s_runtimeMenu.rightHeld = false;
}

static bool coleco_menu_edge(bool pressed, bool* held) {
    if (!held) return false;
    const bool fired = pressed && !*held;
    *held = pressed;
    return fired;
}

static void coleco_toggle_runtime_menu(void) {
    s_runtimeMenu.visible = !s_runtimeMenu.visible;
    coleco_reset_menu_latches();
}

static void coleco_runtime_menu_move(int delta) {
    const int count = coleco_get_menu_item_count();
    int selected = s_runtimeMenu.selectedIndex;
    selected = (selected + delta + count) % count;
    s_runtimeMenu.selectedIndex = static_cast<uint8_t>(selected);
    if (selected < s_runtimeMenu.scroll) {
        s_runtimeMenu.scroll = selected;
    } else if (selected >= s_runtimeMenu.scroll + 5) {
        s_runtimeMenu.scroll = selected - 4;
    }
}

static void coleco_runtime_menu_adjust(int delta) {
    if (coleco_get_menu_item(s_runtimeMenu.selectedIndex) == ColecoRuntimeMenuItem::StateSlot) {
        int slot = s_runtimeOptions.stateSlot;
        slot = (slot + delta + 10) % 10;
        s_runtimeOptions.stateSlot = static_cast<uint8_t>(slot);
    }
}

static void coleco_runtime_menu_accept(void) {
    switch (coleco_get_menu_item(s_runtimeMenu.selectedIndex)) {
        case ColecoRuntimeMenuItem::View:
            if (coleco_view_toggle_allowed()) {
                coleco_config_toggle_active_view_mode();
            }
            break;
        case ColecoRuntimeMenuItem::StateSlot:
            coleco_runtime_menu_adjust(1);
            break;
        case ColecoRuntimeMenuItem::SaveState:
            s_runtimeOptions.saveRequested = true;
            s_runtimeMenu.visible = false;
            break;
        case ColecoRuntimeMenuItem::LoadState:
            {
                CardputerView view;
                CardputerInput cinput;
                ConfirmationSelector confirm(view, cinput);
                char confirmTitle[32];
                std::snprintf(confirmTitle, sizeof(confirmTitle), "LOAD STATE <%u>", static_cast<unsigned>(s_runtimeOptions.stateSlot));
                bool sure = confirm.select(confirmTitle, "Are you sure?", 89);
                s_runtimeMenu.visible = false;
                if (sure) {
                    s_runtimeOptions.loadRequested = true;
                }
                coleco_video_request_full_redraw();
                M5Cardputer.Display.fillScreen(TFT_BLACK);
                break;
            }
        case ColecoRuntimeMenuItem::Close:
            s_runtimeMenu.visible = false;
            break;
        default:
            break;
    }
    if (!s_runtimeMenu.visible) {
        coleco_reset_menu_latches();
    }
}

static bool coleco_menu_prev_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1); }
static bool coleco_menu_next_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1); }
static bool coleco_menu_left_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1); }
static bool coleco_menu_right_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1); }
static bool coleco_menu_accept_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1); }
static bool coleco_menu_back_pressed() { return M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B); }

static void coleco_poll_runtime_menu(bool goShortClicked) {
    if (!s_runtimeMenu.visible) {
        coleco_reset_menu_latches();
        return;
    }
    if (goShortClicked || coleco_menu_edge(coleco_menu_accept_pressed(), &s_runtimeMenu.acceptHeld)) {
        coleco_runtime_menu_accept();
    }
    if (!s_runtimeMenu.visible) return;
    if (coleco_menu_edge(coleco_menu_prev_pressed(), &s_runtimeMenu.prevHeld)) coleco_runtime_menu_move(-1);
    if (coleco_menu_edge(coleco_menu_next_pressed(), &s_runtimeMenu.nextHeld)) coleco_runtime_menu_move(1);
    if (coleco_menu_edge(coleco_menu_left_pressed(), &s_runtimeMenu.leftHeld)) coleco_runtime_menu_adjust(-1);
    if (coleco_menu_edge(coleco_menu_right_pressed(), &s_runtimeMenu.rightHeld)) coleco_runtime_menu_adjust(1);
    if (coleco_menu_edge(coleco_menu_back_pressed(), &s_runtimeMenu.backHeld)) {
        s_runtimeMenu.visible = false;
        coleco_reset_menu_latches();
    }
}

void coleco_input_init(void) {
    s_goLongHandled = false;
    s_suppressGoClick = false;
    s_suppressGoUntilMs = 0;
    s_fnSaveLoadHandled = false;
    s_runtimeOptions = {true, false, false, 0, false, false};
    s_runtimeMenu = {false, 0, 0, false, false, false, false, false, false};
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
    state->menuVisible = s_runtimeMenu.visible;
    state->quitRequested = false;
    state->toggleViewRequested = false;

    M5Cardputer.update();
    auto status = M5Cardputer.Keyboard.keysState();
    share::checkCommonInput(status);

    const bool fnSavePressed = status.fn && (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S'));
    const bool fnLoadPressed = status.fn && (M5Cardputer.Keyboard.isKeyPressed('l') || M5Cardputer.Keyboard.isKeyPressed('L'));
    if (fnSavePressed || fnLoadPressed) {
        if (!s_fnSaveLoadHandled) {
            s_fnSaveLoadHandled = true;
            if (fnSavePressed) {
                s_runtimeOptions.saveRequested = true;
            }
            if (fnLoadPressed) {
                s_runtimeOptions.loadRequested = true;
            }
        }
    } else {
        s_fnSaveLoadHandled = false;
    }

    bool goShortClicked = false;
    bool goLongToggledMenu = false;
    if (M5Cardputer.BtnA.isPressed()) {
        if (!s_goLongHandled && M5Cardputer.BtnA.pressedFor(kGoLongPressMs)) {
            s_goLongHandled = true;
            s_suppressGoClick = true;
            s_suppressGoUntilMs = millis() + 250u;
            coleco_toggle_runtime_menu();
            goLongToggledMenu = true;
        }
    } else {
        if (s_goLongHandled) s_goLongHandled = false;
        if (s_suppressGoClick) {
            (void)M5Cardputer.BtnA.wasClicked();
            if (millis() >= s_suppressGoUntilMs) {
                s_suppressGoClick = false;
            }
        } else if (M5Cardputer.BtnA.wasClicked()) {
            goShortClicked = true;
        }
    }

    const bool menuWasVisible = s_runtimeMenu.visible;
    if (menuWasVisible) coleco_poll_runtime_menu(goShortClicked);
    if (!menuWasVisible && goShortClicked) state->quitRequested = true;

    state->menuVisible = s_runtimeMenu.visible;
    if (menuWasVisible || s_runtimeMenu.visible || goLongToggledMenu) {
        return; // Blocca i controlli fisici se il menu è aperto
    }

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
    state->menuVisible = s_runtimeMenu.visible;
    state->joystickEnabled = s_runtimeOptions.joystickEnabled;
    state->keyboardEnabled = s_runtimeOptions.keyboardEnabled;
    state->vausEnabled = s_runtimeOptions.vausEnabled;
    state->selectedIndex = s_runtimeMenu.selectedIndex;
}

uint8_t coleco_input_get_scroll_index(void) {
    return s_runtimeMenu.scroll;
}

uint8_t coleco_input_get_state_slot(void) {
    return s_runtimeOptions.stateSlot;
}

bool coleco_input_get_save_requested(void) {
    bool r = s_runtimeOptions.saveRequested;
    s_runtimeOptions.saveRequested = false;
    return r;
}

bool coleco_input_get_load_requested(void) {
    bool r = s_runtimeOptions.loadRequested;
    s_runtimeOptions.loadRequested = false;
    return r;
}
