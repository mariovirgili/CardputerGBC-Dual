#include "msx_input.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "../cardputer/CardputerInput.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../share/input.h"
#include "msx_config.h"
#include "core/msx_keyboard.h"
#include "../cardputer/CardputerView.h"
#include "../cardputer/ConfirmationSelector.h"
#include "msx_video.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef MSX_RUNTIME_MENU_LOG_ENABLED
#define MSX_RUNTIME_MENU_LOG_ENABLED 0
#endif

namespace {

struct MsxInputBindingCache {
    char up;
    char down;
    char left;
    char right;
    char fire1;
    char fire2;
    char start;
    char select;
    bool hasI2cPad;
};

} // namespace

static constexpr uint32_t kBacktickLongPressMs = 700;
static constexpr uint32_t kGoLongPressMs = 700;
static constexpr uint32_t kViewToggleDebounceMs = 250;
static uint32_t s_backtickPressedMs = 0;
static bool s_backtickLongHandled = false;
static bool s_goLongHandled = false;
static bool s_suppressGoClick = false;
static uint32_t s_suppressGoUntilMs = 0;
static bool s_viewToggleHeld = false;
static uint32_t s_lastViewToggleMs = 0;

struct MsxRuntimeOptions {
    bool joystickEnabled;
    bool keyboardEnabled;
    bool basicKeyboardEnabled;
    bool vausEnabled;
    uint8_t stateSlot;
    bool saveRequested;
    bool loadRequested;
    bool changeCasAvailable;
    bool changeCasRequested;
};

enum class MsxRuntimeMenuItem : uint8_t {
    Performance = 0,
    Joystick,
    Keyboard,
    BasicKeyboard,
    Vaus,
    View,
    StateSlot,
    SaveState,
    LoadState,
    ChangeCas,
    Close,
    Count,
};

enum class MsxRuntimeMenuPage : uint8_t {
    Main = 0,
    Performance,
};

enum class MsxPerformanceMenuItem : uint8_t {
    ExternalFixed30Fps = 0,
    Frameskip,
    FpsOverlay,
    SliceRendering,
    SpriteCollision,
    SpriteOverflow,
    InstantCommands,
    Back,
    Count,
};

struct MsxRuntimeMenuState {
    bool visible;
    MsxRuntimeMenuPage page;
    uint8_t selectedIndex;
    uint8_t scroll;
    uint8_t mainSelectedIndex;
    uint8_t performanceSelectedIndex;
    bool prevHeld;
    bool nextHeld;
    bool acceptHeld;
    bool backHeld;
    bool leftHeld;
    bool rightHeld;
};

static constexpr uint8_t kRuntimeMenuVisibleRows = 5u;
static MsxRuntimeOptions s_runtimeOptions = {false, true, false, false, 0, false, false, false, false};
static MsxRuntimeMenuState s_runtimeMenu = {
    false,
    MsxRuntimeMenuPage::Main,
    0,
    0,
    0,
    0,
    false,
    false,
    false,
    false,
    false,
    false
};
static const char* s_textMacro = nullptr;
static size_t s_textMacroIndex = 0;
static uint8_t s_textMacroPhase = 0;
static MsxMachineMode s_runtimeMachineMode = MsxMachineMode::MSX2;

static bool msx_view_toggle_allowed(void)
{
    return true;
}

static const char* msx_runtime_view_label(void)
{
    return msx_config_get_active_view_mode_label_for_target(
        g_emu_display_target == EMU_DISPLAY_EXTERNAL
    );
}

static bool msx_runtime_menu_in_performance_page(void)
{
    return s_runtimeMenu.page == MsxRuntimeMenuPage::Performance;
}

static uint8_t msx_get_main_menu_item_count(void)
{
    return s_runtimeOptions.changeCasAvailable ? 11 : 10;
}

static uint8_t msx_get_performance_menu_item_count(void)
{
    return static_cast<uint8_t>(MsxPerformanceMenuItem::Count);
}

static uint8_t msx_get_menu_item_count(void)
{
    return msx_runtime_menu_in_performance_page()
               ? msx_get_performance_menu_item_count()
               : msx_get_main_menu_item_count();
}

static MsxRuntimeMenuItem msx_get_menu_item(uint8_t index)
{
    switch (index) {
        case 0: return MsxRuntimeMenuItem::Performance;
        case 1: return MsxRuntimeMenuItem::Joystick;
        case 2: return MsxRuntimeMenuItem::Keyboard;
        case 3: return MsxRuntimeMenuItem::BasicKeyboard;
        case 4: return MsxRuntimeMenuItem::Vaus;
        case 5: return MsxRuntimeMenuItem::View;
        case 6: return MsxRuntimeMenuItem::StateSlot;
        case 7: return MsxRuntimeMenuItem::SaveState;
        case 8: return MsxRuntimeMenuItem::LoadState;
        case 9: return s_runtimeOptions.changeCasAvailable
                       ? MsxRuntimeMenuItem::ChangeCas
                       : MsxRuntimeMenuItem::Close;
        case 10: return MsxRuntimeMenuItem::Close;
        default: return MsxRuntimeMenuItem::Close;
    }
}

static MsxPerformanceMenuItem msx_get_performance_menu_item(uint8_t index)
{
    switch (index) {
        case 0: return MsxPerformanceMenuItem::ExternalFixed30Fps;
        case 1: return MsxPerformanceMenuItem::Frameskip;
        case 2: return MsxPerformanceMenuItem::FpsOverlay;
        case 3: return MsxPerformanceMenuItem::SliceRendering;
        case 4: return MsxPerformanceMenuItem::SpriteCollision;
        case 5: return MsxPerformanceMenuItem::SpriteOverflow;
        case 6: return MsxPerformanceMenuItem::InstantCommands;
        case 7: return MsxPerformanceMenuItem::Back;
        default: return MsxPerformanceMenuItem::Back;
    }
}

static inline bool msx_key_pressed(char key)
{
    return M5Cardputer.Keyboard.isKeyPressed(key);
}

static inline bool msx_key_pressed_any(char a, char b)
{
    return msx_key_pressed(a) || msx_key_pressed(b);
}

static inline bool msx_is_view_toggle_key(char ch)
{
    return ch == CARDPUTER_SCREEN_TOGGLE || ch == '|';
}

static bool msx_keys_contain_view_toggle_char(const Keyboard_Class::KeysState& keys)
{
    for (char ch : keys.word) {
        if (msx_is_view_toggle_key(ch)) {
            return true;
        }
    }
    return false;
}

static bool msx_poll_view_toggle_request(const Keyboard_Class::KeysState& keys)
{
    if (!msx_view_toggle_allowed()) {
        s_viewToggleHeld = false;
        return false;
    }

    const bool viewTogglePressed = msx_key_pressed_any(CARDPUTER_SCREEN_TOGGLE, '|');
    const bool viewToggleTyped = msx_keys_contain_view_toggle_char(keys);
    const bool typedEdge = viewToggleTyped && (!viewTogglePressed || !s_viewToggleHeld);
    const bool pressedEdge = viewTogglePressed && !s_viewToggleHeld;

    bool toggleRequested = false;
    if (typedEdge || pressedEdge) {
        const uint32_t nowMs = millis();
        if (s_lastViewToggleMs == 0u ||
            static_cast<uint32_t>(nowMs - s_lastViewToggleMs) >= kViewToggleDebounceMs) {
            toggleRequested = true;
            s_lastViewToggleMs = nowMs;
        }
    }

    s_viewToggleHeld = viewTogglePressed;
    return toggleRequested;
}

static char msx_normalize_char(char ch)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

static void msx_start_text_macro(const char* text)
{
    s_textMacro = text;
    s_textMacroIndex = 0;
    s_textMacroPhase = 0;
}

static bool msx_text_macro_active(void)
{
    return s_textMacro && s_textMacro[s_textMacroIndex] != '\0';
}

static bool msx_text_macro_char_needs_shift(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return true;
    }

    switch (ch) {
        case '!':
        case '@':
        case '#':
        case '$':
        case '%':
        case '^':
        case '&':
        case '*':
        case '(':
        case ')':
        case '_':
        case '+':
        case '{':
        case '}':
        case '|':
        case '"':
        case ':':
        case '<':
        case '>':
        case '?':
            return true;
        default:
            return false;
    }
}

static void msx_keyboard_matrix_press_text_char(MsxKeyboardMatrix* matrix, char ch)
{
    if (!matrix) {
        return;
    }

    if (msx_text_macro_char_needs_shift(ch)) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Shift);
    }
    msx_keyboard_matrix_press_ascii(matrix, ch);
}

static bool msx_apply_text_macro_step(MsxKeyboardMatrix* matrix)
{
    if (!matrix || !msx_text_macro_active()) {
        return false;
    }

    const char ch = s_textMacro[s_textMacroIndex];
    if (s_textMacroPhase < 2u) {
        msx_keyboard_matrix_press_text_char(matrix, ch);
        ++s_textMacroPhase;
        return true;
    }

    s_textMacroPhase = 0;
    ++s_textMacroIndex;
    if (!msx_text_macro_active()) {
        s_textMacro = nullptr;
    }
    return true;
}

static char msx_load_bound_char(share::EmuAction action)
{
    return msx_normalize_char(share::emuControlKey(share::EmuProfile::MSX, action));
}

static void msx_load_binding_cache(MsxInputBindingCache* cache)
{
    if (!cache) {
        return;
    }

    cache->up = msx_load_bound_char(share::EmuAction::Up);
    cache->down = msx_load_bound_char(share::EmuAction::Down);
    cache->left = msx_load_bound_char(share::EmuAction::Left);
    cache->right = msx_load_bound_char(share::EmuAction::Right);
    cache->fire1 = msx_load_bound_char(share::EmuAction::A);
    cache->fire2 = msx_load_bound_char(share::EmuAction::B);
    cache->start = msx_load_bound_char(share::EmuAction::Start);
    cache->select = msx_load_bound_char(share::EmuAction::Select);
    cache->hasI2cPad = share::hasI2cPad();
}

static void msx_reset_menu_latches(void)
{
    s_runtimeMenu.prevHeld = false;
    s_runtimeMenu.nextHeld = false;
    s_runtimeMenu.acceptHeld = false;
    s_runtimeMenu.backHeld = false;
    s_runtimeMenu.leftHeld = false;
    s_runtimeMenu.rightHeld = false;
}

static bool msx_menu_edge(bool pressed, bool* held)
{
    if (!held) {
        return false;
    }

    const bool fired = pressed && !*held;
    *held = pressed;
    return fired;
}

static void msx_runtime_menu_open_main_page(void)
{
    s_runtimeMenu.page = MsxRuntimeMenuPage::Main;
    s_runtimeMenu.selectedIndex = s_runtimeMenu.mainSelectedIndex;
}

static void msx_runtime_menu_open_performance_page(void)
{
    s_runtimeMenu.page = MsxRuntimeMenuPage::Performance;
    s_runtimeMenu.selectedIndex = s_runtimeMenu.performanceSelectedIndex;
    s_runtimeMenu.scroll =
        s_runtimeMenu.selectedIndex >= kRuntimeMenuVisibleRows
            ? static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u))
            : 0u;
}

static void msx_toggle_runtime_menu(void)
{
    s_runtimeMenu.visible = !s_runtimeMenu.visible;
    if (s_runtimeMenu.visible) {
        msx_runtime_menu_open_main_page();
    } else {
        msx_runtime_menu_open_main_page();
    }
    msx_reset_menu_latches();
}

static void msx_runtime_menu_move(int delta)
{
    const int count = msx_get_menu_item_count();
    int selected = s_runtimeMenu.selectedIndex;
    selected = (selected + delta + count) % count;
    s_runtimeMenu.selectedIndex = static_cast<uint8_t>(selected);

    if (msx_runtime_menu_in_performance_page()) {
        s_runtimeMenu.performanceSelectedIndex = s_runtimeMenu.selectedIndex;
        if (selected < s_runtimeMenu.scroll) {
            s_runtimeMenu.scroll = static_cast<uint8_t>(selected);
        } else if (selected >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
            s_runtimeMenu.scroll =
                static_cast<uint8_t>(selected - (kRuntimeMenuVisibleRows - 1u));
        }
    } else {
        s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
        if (selected < s_runtimeMenu.scroll) {
            s_runtimeMenu.scroll = static_cast<uint8_t>(selected);
        } else if (selected >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
            s_runtimeMenu.scroll =
                static_cast<uint8_t>(selected - (kRuntimeMenuVisibleRows - 1u));
        }
    }
}

static void msx_runtime_toggle_performance_item(MsxPerformanceMenuItem item)
{
    const bool msx2OnlyItem =
        item == MsxPerformanceMenuItem::SliceRendering ||
        item == MsxPerformanceMenuItem::InstantCommands;
    if (msx2OnlyItem && s_runtimeMachineMode != MsxMachineMode::MSX2) {
        return;
    }

    switch (item) {
        case MsxPerformanceMenuItem::ExternalFixed30Fps:
            msx_config_set_performance_flag(
                MsxPerformanceFlag::ExternalFixed30Fps,
                !msx_config_get_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps),
                true
            );
            break;
        case MsxPerformanceMenuItem::Frameskip:
            msx_config_cycle_frameskip_mode(1, true);
            break;
        case MsxPerformanceMenuItem::FpsOverlay:
            msx_config_set_fps_overlay_enabled(
                !msx_config_get_fps_overlay_enabled(),
                true
            );
            break;
        case MsxPerformanceMenuItem::SliceRendering:
            msx_config_set_performance_flag(
                MsxPerformanceFlag::DisableSliceRendering,
                !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSliceRendering),
                true
            );
            break;
        case MsxPerformanceMenuItem::SpriteCollision:
            msx_config_set_performance_flag(
                MsxPerformanceFlag::DisableSpriteCollision,
                !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision),
                true
            );
            break;
        case MsxPerformanceMenuItem::SpriteOverflow:
            msx_config_set_performance_flag(
                MsxPerformanceFlag::SimplifySpriteOverflow,
                !msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow),
                true
            );
            break;
        case MsxPerformanceMenuItem::InstantCommands:
            msx_config_set_performance_flag(
                MsxPerformanceFlag::InstantVdpCommands,
                !msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands),
                true
            );
            break;
        case MsxPerformanceMenuItem::Back:
        case MsxPerformanceMenuItem::Count:
        default:
            break;
    }
}

static void msx_runtime_menu_adjust(int delta)
{
    if (msx_runtime_menu_in_performance_page()) {
        const MsxPerformanceMenuItem item = msx_get_performance_menu_item(s_runtimeMenu.selectedIndex);
        if (item == MsxPerformanceMenuItem::Frameskip) {
            msx_config_cycle_frameskip_mode(delta >= 0 ? 1 : -1, true);
        } else {
            msx_runtime_toggle_performance_item(item);
        }
        return;
    }

    if (msx_get_menu_item(s_runtimeMenu.selectedIndex) == MsxRuntimeMenuItem::StateSlot) {
        int slot = s_runtimeOptions.stateSlot;
        slot = (slot + delta + 10) % 10;
        s_runtimeOptions.stateSlot = static_cast<uint8_t>(slot);
    }
}

static void msx_clamp_runtime_menu_selection(void)
{
    const uint8_t count = msx_get_menu_item_count();
    if (s_runtimeMenu.selectedIndex >= count) {
        s_runtimeMenu.selectedIndex = static_cast<uint8_t>(count > 0 ? count - 1 : 0);
    }

    if (msx_runtime_menu_in_performance_page()) {
        s_runtimeMenu.performanceSelectedIndex = s_runtimeMenu.selectedIndex;
        if (s_runtimeMenu.scroll > s_runtimeMenu.selectedIndex) {
            s_runtimeMenu.scroll = s_runtimeMenu.selectedIndex;
        }
        if (s_runtimeMenu.selectedIndex >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
            s_runtimeMenu.scroll =
                static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u));
        }
        return;
    }

    s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
    if (s_runtimeMenu.scroll > s_runtimeMenu.selectedIndex) {
        s_runtimeMenu.scroll = s_runtimeMenu.selectedIndex;
    }
    if (s_runtimeMenu.selectedIndex >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
        s_runtimeMenu.scroll =
            static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u));
    }
}

static void msx_runtime_log_options(void)
{
#if MSX_RUNTIME_MENU_LOG_ENABLED
    std::printf("[MSX][MENU] joy=%s keyboard=%s basic=%s vaus=%s perf=%s page=%s cas=%s view=%s menu=%s\n",
                s_runtimeOptions.joystickEnabled ? "on" : "off",
                s_runtimeOptions.keyboardEnabled ? "on" : "off",
                s_runtimeOptions.basicKeyboardEnabled ? "on" : "off",
                s_runtimeOptions.vausEnabled ? "on" : "off",
                msx_config_get_performance_mode_label(),
                msx_runtime_menu_in_performance_page() ? "perf" : "main",
                s_runtimeOptions.changeCasAvailable ? "on" : "off",
                msx_runtime_view_label(),
                s_runtimeMenu.visible ? "open" : "closed");
#endif
}

static void msx_runtime_menu_accept(void)
{
    if (msx_runtime_menu_in_performance_page()) {
        if (msx_get_performance_menu_item(s_runtimeMenu.selectedIndex) == MsxPerformanceMenuItem::Back) {
            msx_runtime_menu_open_main_page();
            msx_clamp_runtime_menu_selection();
        } else {
            msx_runtime_toggle_performance_item(msx_get_performance_menu_item(s_runtimeMenu.selectedIndex));
        }
        msx_runtime_log_options();
        return;
    }

    switch (msx_get_menu_item(s_runtimeMenu.selectedIndex)) {
        case MsxRuntimeMenuItem::Joystick:
            s_runtimeOptions.joystickEnabled = !s_runtimeOptions.joystickEnabled;
            if (s_runtimeOptions.joystickEnabled) {
                s_runtimeOptions.basicKeyboardEnabled = false;
            }
            break;
        case MsxRuntimeMenuItem::Keyboard:
            s_runtimeOptions.keyboardEnabled = !s_runtimeOptions.keyboardEnabled;
            if (!s_runtimeOptions.keyboardEnabled) {
                s_runtimeOptions.basicKeyboardEnabled = false;
            }
            break;
        case MsxRuntimeMenuItem::BasicKeyboard:
            s_runtimeOptions.basicKeyboardEnabled = !s_runtimeOptions.basicKeyboardEnabled;
            if (s_runtimeOptions.basicKeyboardEnabled) {
                s_runtimeOptions.keyboardEnabled = true;
                s_runtimeOptions.joystickEnabled = false;
                s_runtimeOptions.vausEnabled = false;
            }
            break;
        case MsxRuntimeMenuItem::Vaus:
            s_runtimeOptions.vausEnabled = !s_runtimeOptions.vausEnabled;
            if (s_runtimeOptions.vausEnabled) {
                s_runtimeOptions.basicKeyboardEnabled = false;
            }
            break;
        case MsxRuntimeMenuItem::View:
            if (msx_view_toggle_allowed()) {
                msx_config_toggle_active_view_mode_for_target(
                    g_emu_display_target == EMU_DISPLAY_EXTERNAL
                );
            }
            break;
        case MsxRuntimeMenuItem::Performance:
            s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
            msx_runtime_menu_open_performance_page();
            break;
        case MsxRuntimeMenuItem::StateSlot:
            msx_runtime_menu_adjust(1);
            break;
        case MsxRuntimeMenuItem::SaveState:
            s_runtimeOptions.saveRequested = true;
            s_runtimeMenu.visible = false;
            break;
        case MsxRuntimeMenuItem::LoadState:
            {
                CardputerView view;
                CardputerInput cinput;
                ConfirmationSelector confirm(view, cinput);
                char confirmTitle[32];
                std::snprintf(confirmTitle, sizeof(confirmTitle), "LOAD STATE <%u>", static_cast<unsigned>(s_runtimeOptions.stateSlot));
                bool sure = confirm.select(confirmTitle, "Are you sure?", 92);
                s_runtimeMenu.visible = false;
                if (sure) {
                    s_runtimeOptions.loadRequested = true;
                }
                msx_video_request_full_redraw();
                M5Cardputer.Display.fillScreen(TFT_BLACK);
                break;
            }
        case MsxRuntimeMenuItem::ChangeCas:
            if (s_runtimeOptions.changeCasAvailable) {
                s_runtimeOptions.changeCasRequested = true;
                s_runtimeMenu.visible = false;
            }
            break;
        case MsxRuntimeMenuItem::Close:
            s_runtimeMenu.visible = false;
            break;
        case MsxRuntimeMenuItem::Count:
        default:
            break;
    }

    msx_runtime_log_options();
    if (!s_runtimeMenu.visible) {
        msx_reset_menu_latches();
    }
}

static inline bool msx_binding_pressed(char key)
{
    return key != 0 && msx_key_pressed(key);
}

static void msx_apply_system_keys(const Keyboard_Class::KeysState& status)
{
    if (status.fn && msx_key_pressed_any('=', '+')) {
        const int volume = M5Cardputer.Speaker.getVolume();
        M5Cardputer.Speaker.setVolume(std::min(volume + 3, 255));
    }

    if (status.fn && msx_key_pressed_any('-', '_')) {
        const int volume = M5Cardputer.Speaker.getVolume();
        M5Cardputer.Speaker.setVolume(std::max(volume - 3, 0));
    }

    if (status.fn && msx_key_pressed_any(']', '}')) {
        const int brightness = M5Cardputer.Display.getBrightness();
        M5Cardputer.Display.setBrightness(std::min(brightness + 2, 255));
    }

    if (status.fn && msx_key_pressed_any('[', '{')) {
        const int brightness = M5Cardputer.Display.getBrightness();
        M5Cardputer.Display.setBrightness(std::max(brightness - 2, 0));
    }
}

static bool msx_is_reserved_fn_char(char ch)
{
    switch (ch) {
        case '1':
        case '!':
        case '2':
        case '@':
        case '3':
        case '#':
        case '4':
        case '$':
        case '5':
        case '%':
        case '-':
        case '_':
        case '=':
        case '+':
        case '[':
        case '{':
        case ']':
        case '}':
        case '\\':
        case '|':
        case ',':
        case '<':
        case '.':
        case '>':
        case '/':
        case '?':
        case ';':
        case ':':
        case ' ':
        case '`':
        case '~':
            return true;
        default:
            return false;
    }
}

// Keys consumed for joystick in joystick mode: never pass to keyboard matrix.
static bool msx_is_joystick_key(char ch, const MsxInputBindingCache& cache)
{
    const char normalized = msx_normalize_char(ch);
    return normalized == cache.up ||
           normalized == cache.down ||
           normalized == cache.left ||
           normalized == cache.right ||
           normalized == cache.fire1 ||
           normalized == cache.fire2;
}

// Set joystick direction/fire state from the configured emulator bindings.
static void msx_apply_joystick_mode_actions(MsxInputState* state,
                                            const MsxInputBindingCache& cache)
{
    if (!state) {
        return;
    }

    state->up    |= msx_binding_pressed(cache.up);
    state->down  |= msx_binding_pressed(cache.down);
    state->left  |= msx_binding_pressed(cache.left);
    state->right |= msx_binding_pressed(cache.right);
    state->fire1 |= msx_binding_pressed(cache.fire1);
    state->fire2 |= msx_binding_pressed(cache.fire2);
}

// Inject cursor keys from ;.,./ and F1-F5 from 1-5 without needing FN (joystick mode).
static void msx_apply_joystick_mode_extras(MsxKeyboardMatrix* matrix)
{
    if (!matrix) {
        return;
    }

    // ; or : → Up
    if (msx_key_pressed_any(';', ':')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Up);
    }
    // . or > → Down
    if (msx_key_pressed_any('.', '>')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Down);
    }
    // , or < → Left
    if (msx_key_pressed_any(',', '<')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Left);
    }
    // / or ? → Right
    if (msx_key_pressed_any('/', '?')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Right);
    }

    // 1–5 → F1–F5
    if (msx_key_pressed_any('1', '!')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F1);
    }
    if (msx_key_pressed_any('2', '@')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F2);
    }
    if (msx_key_pressed_any('3', '#')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F3);
    }
    if (msx_key_pressed_any('4', '$')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F4);
    }
    if (msx_key_pressed_any('5', '%')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F5);
    }
}

static inline bool msx_is_bound_action_char(char ch, char bound)
{
    return bound != 0 && bound == msx_normalize_char(ch);
}

static bool msx_is_bound_emulator_control_char(char ch, const MsxInputBindingCache& cache)
{
    const char normalized = msx_normalize_char(ch);
    return normalized == cache.up ||
           normalized == cache.down ||
           normalized == cache.left ||
           normalized == cache.right ||
           normalized == cache.fire1 ||
           normalized == cache.fire2 ||
           normalized == cache.start ||
           normalized == cache.select;
}

static bool msx_is_basic_fn_shortcut_char(char ch)
{
    switch (msx_normalize_char(ch)) {
        case 'b':
        case 'c':
        case 'l':
        case 's':
            return true;
        default:
            return false;
    }
}

static void msx_apply_printable_keys(MsxKeyboardMatrix* matrix,
                                     const Keyboard_Class::KeysState& keys,
                                     const MsxInputBindingCache& cache,
                                     bool joystickEnabled,
                                     bool basicKeyboardEnabled)
{
    if (!matrix) {
        return;
    }

    for (char ch : keys.word) {
        if (basicKeyboardEnabled) {
            if (keys.fn && msx_is_basic_fn_shortcut_char(ch)) {
                continue;
            }
            if (msx_is_view_toggle_key(ch) || ch == '`' || ch == '~') {
                continue;
            }

            msx_keyboard_matrix_press_ascii(matrix, ch);
            continue;
        }

        // In joystick mode all reserved-fn chars (digits 1-5, punctuation cursors,
        // space, etc.) are handled elsewhere; skip them from printable injection.
        if ((keys.fn || joystickEnabled) && msx_is_reserved_fn_char(ch)) {
            continue;
        }

        if (msx_is_view_toggle_key(ch)) {
            continue;
        }

        if (ch == '`' || ch == '~') {
            continue;
        }

        // Emulator control bindings are handled as virtual joystick or
        // synthetic MSX keys; never leak them through as printable chars.
        if (msx_is_bound_emulator_control_char(ch, cache)) {
            continue;
        }

        msx_keyboard_matrix_press_ascii(matrix, ch);
    }
}

static void msx_apply_modifier_keys(MsxKeyboardMatrix* matrix, const Keyboard_Class::KeysState& keys)
{
    if (!matrix) {
        return;
    }

    if (keys.shift) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Shift);
    }

    if (keys.ctrl) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Ctrl);
    }

    if (keys.opt) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Graph);
    }

    if (keys.alt) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Code);
    }

    if (M5Cardputer.Keyboard.capslocked()) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Caps);
    }
}

static void msx_apply_direct_special_keys(MsxKeyboardMatrix* matrix, const Keyboard_Class::KeysState& keys)
{
    if (!matrix) {
        return;
    }

    if (msx_key_pressed_any('`', '~')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Esc);
    }

    if (!keys.fn) {
        if (keys.tab) {
            msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Tab);
        }
        if (keys.del) {
            msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Backspace);
        }
        if (keys.enter) {
            msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Enter);
        }
        if (keys.space) {
            msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Space);
        }
    }
}

static void msx_apply_fn_combos(MsxKeyboardMatrix* matrix, const Keyboard_Class::KeysState& keys)
{
    if (!matrix || !keys.fn) {
        return;
    }

    if (msx_key_pressed_any('1', '!')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F1);
    }
    if (msx_key_pressed_any('2', '@')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F2);
    }
    if (msx_key_pressed_any('3', '#')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F3);
    }
    if (msx_key_pressed_any('4', '$')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F4);
    }
    if (msx_key_pressed_any('5', '%')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::F5);
    }

    if (keys.tab) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Stop);
    }
    if (keys.del) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Delete);
    }
    if (keys.enter) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Select);
    }
    if (keys.space) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Home);
    }

    if (msx_key_pressed_any(',', '<')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Left);
    }
    if (msx_key_pressed_any('.', '>')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Down);
    }
    if (msx_key_pressed_any('/', '?')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Right);
    }
    if (msx_key_pressed_any(';', ':')) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Up);
    }
}

static void msx_apply_shared_actions(MsxInputState* state,
                                     const Keyboard_Class::KeysState& keys,
                                     const MsxInputBindingCache& cache)
{
    if (!state || keys.fn) {
        return;
    }

    state->up |= msx_binding_pressed(cache.up);
    state->down |= msx_binding_pressed(cache.down);
    state->left |= msx_binding_pressed(cache.left);
    state->right |= msx_binding_pressed(cache.right);
    state->fire1 |= msx_binding_pressed(cache.fire1);
    state->fire2 |= msx_binding_pressed(cache.fire2);
    state->start |= msx_binding_pressed(cache.start);
    state->select |= msx_binding_pressed(cache.select);
}

static void msx_apply_emulator_actions_to_matrix(MsxKeyboardMatrix* matrix, const MsxInputState* state)
{
    if (!matrix || !state) {
        return;
    }

    if (state->up) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Up);
    }
    if (state->down) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Down);
    }
    if (state->left) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Left);
    }
    if (state->right) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Right);
    }
    if (state->fire1) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Space);
    }
    if (state->fire2) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Graph);
    }
    if (state->start) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Enter);
    }
    if (state->select) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Esc);
    }
}

static void msx_build_keyboard_matrix(MsxKeyboardMatrix* matrix,
                                      const Keyboard_Class::KeysState& keys,
                                      const MsxInputState* state,
                                      const MsxInputBindingCache& cache,
                                      bool keyboardEnabled,
                                      bool joystickEnabled,
                                      bool basicKeyboardEnabled)
{
    msx_keyboard_matrix_clear(matrix);
    if (!keyboardEnabled) {
        return;
    }

    if (msx_text_macro_active()) {
        msx_apply_text_macro_step(matrix);
        return;
    }

    msx_apply_modifier_keys(matrix, keys);
    msx_apply_direct_special_keys(matrix, keys);

    if (!basicKeyboardEnabled) {
        msx_apply_fn_combos(matrix, keys);
    }

    if (joystickEnabled && !basicKeyboardEnabled) {
        // When joystick emulation is enabled, keep the convenience overlay keys
        // (cursor punctuation / function digits) available too.
        msx_apply_joystick_mode_extras(matrix);
    }

    if (!basicKeyboardEnabled) {
        msx_apply_emulator_actions_to_matrix(matrix, state);
    }
    msx_apply_printable_keys(matrix, keys, cache, joystickEnabled, basicKeyboardEnabled);
}

static bool msx_menu_prev_pressed(const Keyboard_Class::KeysState& keys,
                                  const MsxInputBindingCache& bindings,
                                  uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP) ||
           msx_binding_pressed(bindings.up) ||
           ((padState & share::PAD_UP) != 0u) ||
           keys.tab;
}

static bool msx_menu_next_pressed(const Keyboard_Class::KeysState& keys,
                                  const MsxInputBindingCache& bindings,
                                  uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN) ||
           msx_binding_pressed(bindings.down) ||
           ((padState & share::PAD_DOWN) != 0u);
}

static bool msx_menu_left_pressed(const Keyboard_Class::KeysState& keys,
                                  const MsxInputBindingCache& bindings,
                                  uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
           msx_binding_pressed(bindings.left) ||
           ((padState & share::PAD_LEFT) != 0u);
}

static bool msx_menu_right_pressed(const Keyboard_Class::KeysState& keys,
                                   const MsxInputBindingCache& bindings,
                                   uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
           msx_binding_pressed(bindings.right) ||
           ((padState & share::PAD_RIGHT) != 0u);
}

static bool msx_menu_accept_pressed(const Keyboard_Class::KeysState& keys,
                                    const MsxInputBindingCache& bindings,
                                    uint32_t padState)
{
    return keys.enter ||
           keys.space ||
           msx_binding_pressed(bindings.fire1) ||
           msx_binding_pressed(bindings.start) ||
           ((padState & (share::PAD_A | share::PAD_START)) != 0u);
}

static bool msx_menu_back_pressed(const Keyboard_Class::KeysState& keys,
                                  const MsxInputBindingCache& bindings,
                                  uint32_t padState)
{
    return keys.del ||
           msx_binding_pressed(bindings.fire2) ||
           msx_binding_pressed(bindings.select) ||
           ((padState & (share::PAD_B | share::PAD_SELECT)) != 0u);
}

static void msx_poll_runtime_menu(const Keyboard_Class::KeysState& keys,
                                  const MsxInputBindingCache& bindings,
                                  uint32_t padState,
                                  bool goShortClicked)
{
    if (!s_runtimeMenu.visible) {
        msx_reset_menu_latches();
        return;
    }

    if (goShortClicked || msx_menu_edge(msx_menu_accept_pressed(keys, bindings, padState), &s_runtimeMenu.acceptHeld)) {
        msx_runtime_menu_accept();
    }
    if (!s_runtimeMenu.visible) {
        return;
    }

    if (msx_menu_edge(msx_menu_prev_pressed(keys, bindings, padState), &s_runtimeMenu.prevHeld)) {
        msx_runtime_menu_move(-1);
    }
    if (msx_menu_edge(msx_menu_next_pressed(keys, bindings, padState), &s_runtimeMenu.nextHeld)) {
        msx_runtime_menu_move(1);
    }
    if (msx_menu_edge(msx_menu_left_pressed(keys, bindings, padState), &s_runtimeMenu.leftHeld)) {
        msx_runtime_menu_adjust(-1);
    }
    if (msx_menu_edge(msx_menu_right_pressed(keys, bindings, padState), &s_runtimeMenu.rightHeld)) {
        msx_runtime_menu_adjust(1);
    }
    if (msx_menu_edge(msx_menu_back_pressed(keys, bindings, padState), &s_runtimeMenu.backHeld)) {
        if (msx_runtime_menu_in_performance_page()) {
            msx_runtime_menu_open_main_page();
            msx_clamp_runtime_menu_selection();
        } else {
            s_runtimeMenu.visible = false;
            msx_runtime_menu_open_main_page();
            msx_reset_menu_latches();
        }
        msx_runtime_log_options();
    }
}

void msx_input_init(void)
{
    s_backtickPressedMs = 0;
    s_backtickLongHandled = false;
    s_goLongHandled = false;
    s_suppressGoClick = false;
    s_suppressGoUntilMs = 0;
    s_runtimeOptions = {false, true, false, false, 0, false, false, false, false};
    s_runtimeMenu = {
        false,
        MsxRuntimeMenuPage::Main,
        0,
        0,
        0,
        0,
        false,
        false,
        false,
        false,
        false,
        false
    };
    s_textMacro = nullptr;
    s_textMacroIndex = 0;
    s_textMacroPhase = 0;
    s_runtimeMachineMode = MsxMachineMode::MSX2;
}

void msx_input_set_basic_keyboard_enabled(bool enabled)
{
    s_runtimeOptions.basicKeyboardEnabled = enabled;
    if (enabled) {
        s_runtimeOptions.keyboardEnabled = true;
        s_runtimeOptions.joystickEnabled = false;
        s_runtimeOptions.vausEnabled = false;
    }
}

void msx_input_set_cas_change_available(bool available)
{
    s_runtimeOptions.changeCasAvailable = available;
    if (!available) {
        s_runtimeOptions.changeCasRequested = false;
    }
    msx_clamp_runtime_menu_selection();
}

void msx_input_set_runtime_machine_mode(MsxMachineMode mode)
{
    s_runtimeMachineMode = mode;
}

void msx_input_poll(MsxInputState* state)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    msx_keyboard_matrix_clear(&state->keyboardMatrix);

    M5Cardputer.update();
    const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    MsxInputBindingCache bindings = {};
    msx_load_binding_cache(&bindings);
    msx_apply_system_keys(keys);

    static bool s_fnSLHandled = false;
    if (keys.fn && (msx_key_pressed('s') || msx_key_pressed('l'))) {
        if (!s_fnSLHandled) {
            s_fnSLHandled = true;
            if (msx_key_pressed('s')) {
                s_runtimeOptions.saveRequested = true;
            }
            if (msx_key_pressed('l')) {
                s_runtimeOptions.loadRequested = true;
            }
        }
    } else {
        s_fnSLHandled = false;
    }

    static bool s_fnCasMacroHandled = false;
    if (keys.fn && (msx_key_pressed('c') || msx_key_pressed('b'))) {
        if (!s_fnCasMacroHandled) {
            s_fnCasMacroHandled = true;
            if (msx_key_pressed('b')) {
                msx_start_text_macro("BLOAD\"CAS:\",R");
            } else {
                msx_start_text_macro("RUN\"CAS:\"");
            }
        }
    } else {
        s_fnCasMacroHandled = false;
    }

    uint32_t padState = 0u;
    if (bindings.hasI2cPad) {
        padState = share::pollI2cPad();
    }

    bool goShortClicked = false;
    bool goLongToggledMenu = false;
    if (M5Cardputer.BtnA.isPressed()) {
        if (!s_goLongHandled && M5Cardputer.BtnA.pressedFor(kGoLongPressMs)) {
            s_goLongHandled = true;
            s_suppressGoClick = true;
            s_suppressGoUntilMs = millis() + 250u;
            msx_toggle_runtime_menu();
            msx_runtime_log_options();
            goLongToggledMenu = true;
        }
    } else {
        if (s_goLongHandled) {
            s_goLongHandled = false;
        }
        if (s_suppressGoClick) {
            (void)M5Cardputer.BtnA.wasClicked();
            if (millis() >= s_suppressGoUntilMs) {
                s_suppressGoClick = false;
            }
        } else if (M5Cardputer.BtnA.wasClicked()) {
            goShortClicked = true;
        }
    }

    if (msx_key_pressed('`')) {
        if (s_backtickPressedMs == 0) {
            s_backtickPressedMs = millis();
            s_backtickLongHandled = false;
        } else if (!s_backtickLongHandled &&
                   (uint32_t)(millis() - s_backtickPressedMs) >= kBacktickLongPressMs) {
            s_backtickLongHandled = true;
            state->quitRequested = true;
        }
    } else {
        s_backtickPressedMs = 0;
        s_backtickLongHandled = false;
    }

    state->toggleViewRequested = msx_poll_view_toggle_request(keys);

    const bool menuWasVisible = s_runtimeMenu.visible;
    if (menuWasVisible) {
        msx_poll_runtime_menu(keys, bindings, padState, goShortClicked);
    }

    if (!menuWasVisible && goShortClicked) {
        state->quitRequested = true;
    }

    const bool basicKeyboardEnabled = s_runtimeOptions.basicKeyboardEnabled;
    state->joystickMode = s_runtimeOptions.joystickEnabled && !basicKeyboardEnabled;
    state->joystickEnabled = s_runtimeOptions.joystickEnabled && !basicKeyboardEnabled;
    state->keyboardEnabled = s_runtimeOptions.keyboardEnabled;
    state->basicKeyboardEnabled = basicKeyboardEnabled;
    state->vausEnabled = s_runtimeOptions.vausEnabled && !basicKeyboardEnabled;
    state->menuVisible = s_runtimeMenu.visible;

    if (menuWasVisible || s_runtimeMenu.visible || goLongToggledMenu) {
        return;
    }

    const bool gameplayInputEnabled =
        (s_runtimeOptions.joystickEnabled && !basicKeyboardEnabled) ||
        s_runtimeOptions.keyboardEnabled ||
        (s_runtimeOptions.vausEnabled && !basicKeyboardEnabled);

    if (bindings.hasI2cPad && gameplayInputEnabled) {
        state->up |= (padState & share::PAD_UP) != 0u;
        state->down |= (padState & share::PAD_DOWN) != 0u;
        state->left |= (padState & share::PAD_LEFT) != 0u;
        state->right |= (padState & share::PAD_RIGHT) != 0u;
        state->fire1 |= (padState & share::PAD_A) != 0u;
        state->fire2 |= (padState & share::PAD_B) != 0u;
        state->start |= (padState & share::PAD_START) != 0u;
        state->select |= (padState & share::PAD_SELECT) != 0u;
    }

    if (!basicKeyboardEnabled && (s_runtimeOptions.joystickEnabled || s_runtimeOptions.vausEnabled)) {
        // Configurable emulator bindings drive the PSG joystick path and Vaus.
        msx_apply_joystick_mode_actions(state, bindings);
    }

    if (s_runtimeOptions.keyboardEnabled && !basicKeyboardEnabled) {
        msx_apply_shared_actions(state, keys, bindings);
    }

    msx_build_keyboard_matrix(&state->keyboardMatrix,
                              keys,
                              state,
                              bindings,
                              s_runtimeOptions.keyboardEnabled,
                              s_runtimeOptions.joystickEnabled,
                              basicKeyboardEnabled);
}

void msx_input_get_overlay_state(MsxInputOverlayState* state)
{
    if (!state) {
        return;
    }

    state->menuVisible = s_runtimeMenu.visible;
    state->performanceSubmenuVisible = msx_runtime_menu_in_performance_page();
    state->machineIsMsx2 = (s_runtimeMachineMode == MsxMachineMode::MSX2);
    state->joystickEnabled = s_runtimeOptions.joystickEnabled;
    state->keyboardEnabled = s_runtimeOptions.keyboardEnabled;
    state->basicKeyboardEnabled = s_runtimeOptions.basicKeyboardEnabled;
    state->vausEnabled = s_runtimeOptions.vausEnabled;
    state->performanceMode = msx_config_get_performance_mode();
    state->perfDisableSliceRendering =
        msx_config_get_performance_flag(MsxPerformanceFlag::DisableSliceRendering);
    state->perfDisableSpriteCollision =
        msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision);
    state->perfSimplifySpriteOverflow =
        msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow);
    state->perfInstantVdpCommands =
        msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands);
    state->perfExternalFixed30Fps =
        msx_config_get_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps);
    state->perfShowFpsOverlay = msx_config_get_fps_overlay_enabled();
    state->perfFrameskipMode = msx_config_get_frameskip_mode();
    state->casChangeAvailable = s_runtimeOptions.changeCasAvailable;
    state->selectedIndex = s_runtimeMenu.selectedIndex;
}

uint8_t msx_input_get_state_slot(void) {
    return s_runtimeOptions.stateSlot;
}

uint8_t msx_input_get_scroll_index(void) {
    return s_runtimeMenu.scroll;
}

bool msx_input_get_save_requested(void) {
    bool r = s_runtimeOptions.saveRequested;
    s_runtimeOptions.saveRequested = false;
    return r;
}

bool msx_input_get_load_requested(void) {
    bool r = s_runtimeOptions.loadRequested;
    s_runtimeOptions.loadRequested = false;
    return r;
}

bool msx_input_get_change_cas_requested(void) {
    bool r = s_runtimeOptions.changeCasRequested;
    s_runtimeOptions.changeCasRequested = false;
    return r;
}
