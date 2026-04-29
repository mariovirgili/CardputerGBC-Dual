#include "msx_input.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>

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
#include <cstdarg>

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
static constexpr int kBrightnessStep = 24;
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
    bool changeDskAvailable;
    bool changeDskRequested;
};

enum class MsxRuntimeMenuItem : uint8_t {
    Performance = 0,
    Sound,
    Joystick,
    Keyboard,
    BasicKeyboard,
    Vaus,
    View,
    StateSlot,
    SaveState,
    LoadState,
    ChangeDsk,
    CasMenu,
    Close,
    Count,
};

enum class MsxRuntimeMenuPage : uint8_t {
    Main = 0,
    Performance,
    Sound,
    Cas,
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

enum class MsxCasMenuItem : uint8_t {
    RunCas = 0,
    BloadCas,
    ChangeCas,
    Back,
    Count,
};

enum class MsxSoundMenuItem : uint8_t {
    VirtualScc = 0,
    SoundVolume,
    SccVolume,
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
    uint8_t soundSelectedIndex;
    uint8_t casSelectedIndex;
    bool prevHeld;
    bool nextHeld;
    bool acceptHeld;
    bool backHeld;
    bool leftHeld;
    bool rightHeld;
};

static constexpr uint8_t kRuntimeMenuVisibleRows = 5u;
static MsxRuntimeOptions s_runtimeOptions = {false, true, false, false, 0, false, false, false, false, false, false};
static MsxRuntimeMenuState s_runtimeMenu = {
    false,
    MsxRuntimeMenuPage::Main,
    0,
    0,
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
static bool s_virtualKeyPickerVisible = false;
static uint8_t s_virtualKeyPickerIndex = 0;
static bool s_virtualKeyPickerTriggerHeld = false;
static bool s_virtualKeyPickerLeftHeld = false;
static bool s_virtualKeyPickerRightHeld = false;
static bool s_virtualKeyPickerAcceptHeld = false;
static bool s_virtualKeyPickerBackHeld = false;
static bool s_virtualKeyPickerAwaitRelease = false;
static bool s_virtualKeyPickerSuppressUntilRelease = false;
static int8_t s_virtualKeyPickerRepeatDir = 0;
static uint32_t s_virtualKeyPickerNextRepeatMs = 0;
static char s_virtualKeyChar = 0;
static uint8_t s_virtualKeyFrames = 0;
static MsxMachineMode s_runtimeMachineMode = MsxMachineMode::MSX2;

static constexpr const char* kMsxInputConfigNs = "msx_input";
static constexpr const char* kMsxInputJoystickKey = "joy";
static constexpr const char* kMsxInputKeyboardKey = "kbd";
static constexpr const char* kMsxInputBasicKey = "basic";
static constexpr const char* kMsxInputVausKey = "vaus";
static constexpr const char* kMsxInputStateSlotKey = "state";
static constexpr MsxRuntimeOptionConfig kDefaultRuntimeOptionConfig = {
    false,
    true,
    false,
    false,
    0,
};

static MsxRuntimeOptionConfig msx_sanitize_runtime_option_config(MsxRuntimeOptionConfig config)
{
    config.stateSlot = static_cast<uint8_t>(config.stateSlot % 10u);
    if (config.basicKeyboardEnabled) {
        config.keyboardEnabled = false;
        config.joystickEnabled = false;
        config.vausEnabled = false;
    }
    return config;
}

static void msx_persist_runtime_option_config(const MsxRuntimeOptionConfig& config)
{
    Preferences prefs;
    prefs.begin(kMsxInputConfigNs, false);
    prefs.putBool(kMsxInputJoystickKey, config.joystickEnabled);
    prefs.putBool(kMsxInputKeyboardKey, config.keyboardEnabled);
    prefs.putBool(kMsxInputBasicKey, config.basicKeyboardEnabled);
    prefs.putBool(kMsxInputVausKey, config.vausEnabled);
    prefs.putUChar(kMsxInputStateSlotKey, config.stateSlot);
    prefs.end();
}

static MsxRuntimeOptionConfig msx_current_runtime_option_config(void)
{
    return {
        s_runtimeOptions.joystickEnabled,
        s_runtimeOptions.keyboardEnabled,
        s_runtimeOptions.basicKeyboardEnabled,
        s_runtimeOptions.vausEnabled,
        s_runtimeOptions.stateSlot,
    };
}

static void msx_apply_runtime_option_config(MsxRuntimeOptionConfig config, bool persist)
{
    config = msx_sanitize_runtime_option_config(config);
    s_runtimeOptions.joystickEnabled = config.joystickEnabled;
    s_runtimeOptions.keyboardEnabled = config.keyboardEnabled;
    s_runtimeOptions.basicKeyboardEnabled = config.basicKeyboardEnabled;
    s_runtimeOptions.vausEnabled = config.vausEnabled;
    s_runtimeOptions.stateSlot = config.stateSlot;
    if (persist) {
        msx_persist_runtime_option_config(config);
    }
}

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

static bool msx_runtime_menu_in_sound_page(void)
{
    return s_runtimeMenu.page == MsxRuntimeMenuPage::Sound;
}

static bool msx_runtime_menu_in_cas_page(void)
{
    return s_runtimeMenu.page == MsxRuntimeMenuPage::Cas;
}

static uint8_t msx_get_main_menu_item_count(void)
{
    uint8_t count = 11u;
    if (s_runtimeOptions.changeDskAvailable) {
        ++count;
    }
    if (s_runtimeOptions.changeCasAvailable) {
        ++count;
    }
    return count;
}

static uint8_t msx_get_performance_menu_item_count(void)
{
    return static_cast<uint8_t>(MsxPerformanceMenuItem::Count);
}

static uint8_t msx_get_cas_menu_item_count(void)
{
    return static_cast<uint8_t>(MsxCasMenuItem::Count);
}

static uint8_t msx_get_sound_menu_item_count(void)
{
    return static_cast<uint8_t>(MsxSoundMenuItem::Count);
}

static uint8_t msx_get_menu_item_count(void)
{
    if (msx_runtime_menu_in_performance_page()) {
        return msx_get_performance_menu_item_count();
    }
    if (msx_runtime_menu_in_sound_page()) {
        return msx_get_sound_menu_item_count();
    }
    if (msx_runtime_menu_in_cas_page()) {
        return msx_get_cas_menu_item_count();
    }
    return msx_get_main_menu_item_count();
}

static MsxRuntimeMenuItem msx_get_menu_item(uint8_t index)
{
    switch (index) {
        case 0: return MsxRuntimeMenuItem::Performance;
        case 1: return MsxRuntimeMenuItem::Sound;
        case 2: return MsxRuntimeMenuItem::Joystick;
        case 3: return MsxRuntimeMenuItem::Keyboard;
        case 4: return MsxRuntimeMenuItem::BasicKeyboard;
        case 5: return MsxRuntimeMenuItem::Vaus;
        case 6: return MsxRuntimeMenuItem::View;
        case 7: return MsxRuntimeMenuItem::StateSlot;
        case 8: return MsxRuntimeMenuItem::SaveState;
        case 9: return MsxRuntimeMenuItem::LoadState;
        default: break;
    }

    uint8_t dynamicIndex = 10u;
    if (s_runtimeOptions.changeDskAvailable) {
        if (index == dynamicIndex) {
            return MsxRuntimeMenuItem::ChangeDsk;
        }
        ++dynamicIndex;
    }
    if (s_runtimeOptions.changeCasAvailable) {
        if (index == dynamicIndex) {
            return MsxRuntimeMenuItem::CasMenu;
        }
        ++dynamicIndex;
    }
    return MsxRuntimeMenuItem::Close;
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

static MsxCasMenuItem msx_get_cas_menu_item(uint8_t index)
{
    switch (index) {
        case 0: return MsxCasMenuItem::RunCas;
        case 1: return MsxCasMenuItem::BloadCas;
        case 2: return MsxCasMenuItem::ChangeCas;
        case 3: return MsxCasMenuItem::Back;
        default: return MsxCasMenuItem::Back;
    }
}

static MsxSoundMenuItem msx_get_sound_menu_item(uint8_t index)
{
    switch (index) {
        case 0: return MsxSoundMenuItem::VirtualScc;
        case 1: return MsxSoundMenuItem::SoundVolume;
        case 2: return MsxSoundMenuItem::SccVolume;
        case 3: return MsxSoundMenuItem::Back;
        default: return MsxSoundMenuItem::Back;
    }
}

static bool msx_key_position_pressed(int x, int y)
{
    const auto& keys = M5Cardputer.Keyboard.keyList();
    for (const auto& key : keys) {
        if (key.x == x && key.y == y) {
            return true;
        }
    }
    return false;
}

static constexpr int kMsxViewToggleKeyX = 13;
static constexpr int kMsxViewToggleKeyY = 1;

static bool msx_physical_view_toggle_pressed(void)
{
    return msx_key_position_pressed(kMsxViewToggleKeyX, kMsxViewToggleKeyY);
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

    const bool viewToggleDown =
        msx_physical_view_toggle_pressed() ||
        msx_key_pressed_any(CARDPUTER_SCREEN_TOGGLE, '|') ||
        msx_keys_contain_view_toggle_char(keys);

    bool toggleRequested = false;
    if (viewToggleDown && !s_viewToggleHeld) {
        const uint32_t nowMs = millis();
        if (s_lastViewToggleMs == 0u ||
            static_cast<uint32_t>(nowMs - s_lastViewToggleMs) >= kViewToggleDebounceMs) {
            toggleRequested = true;
            s_lastViewToggleMs = nowMs;
        }
    }

    s_viewToggleHeld = viewToggleDown;
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

static void msx_restore_saved_runtime_option_config(void)
{
    msx_apply_runtime_option_config(msx_input_load_runtime_option_config(), false);
}

static void msx_start_cas_text_macro(const char* text)
{
    msx_restore_saved_runtime_option_config();
    msx_start_text_macro(text);
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

    if (ch == '\n' || ch == '\r') {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Enter);
        return;
    }

    if (msx_text_macro_char_needs_shift(ch)) {
        msx_keyboard_matrix_press_special(matrix, MsxKeyboardSpecialKey::Shift);
    }
    msx_keyboard_matrix_press_ascii(matrix, ch);
}

static constexpr char kMsxVirtualKeyPickerChars[] = "0123456789'abcdefghijklmnopqrstuvwxyz \n";
static constexpr uint8_t kMsxVirtualKeyPickerCount =
    static_cast<uint8_t>(sizeof(kMsxVirtualKeyPickerChars) - 1u);
static constexpr int kMsxVirtualKeyPickerTriggerX = 6;
static constexpr int kMsxVirtualKeyPickerTriggerY = 3;

static bool msx_virtual_key_picker_allowed(bool keyboardEnabled,
                                           bool joystickEnabled,
                                           bool basicKeyboardEnabled)
{
    return !basicKeyboardEnabled && (keyboardEnabled || joystickEnabled);
}

static bool msx_virtual_key_picker_trigger_pressed(void)
{
    return msx_key_position_pressed(kMsxVirtualKeyPickerTriggerX, kMsxVirtualKeyPickerTriggerY) ||
           msx_key_pressed_any('v', 'V');
}

static char msx_virtual_key_picker_current_char(void)
{
    return kMsxVirtualKeyPickerChars[s_virtualKeyPickerIndex % kMsxVirtualKeyPickerCount];
}

static const char* msx_virtual_key_picker_current_label(void)
{
    switch (msx_virtual_key_picker_current_char()) {
        case ' ':
            return "_";
        case '\n':
        case '\r':
            return "E";
        default:
            break;
    }

    static char label[2];
    label[0] = msx_virtual_key_picker_current_char();
    label[1] = '\0';
    return label;
}

static void msx_apply_virtual_key_step(MsxKeyboardMatrix* matrix)
{
    if (!matrix || s_virtualKeyFrames == 0u || s_virtualKeyChar == 0) {
        return;
    }

    msx_keyboard_matrix_press_text_char(matrix, s_virtualKeyChar);
    --s_virtualKeyFrames;
    if (s_virtualKeyFrames == 0u) {
        s_virtualKeyChar = 0;
    }
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

static void msx_runtime_menu_open_sound_page(void)
{
    s_runtimeMenu.page = MsxRuntimeMenuPage::Sound;
    s_runtimeMenu.selectedIndex = s_runtimeMenu.soundSelectedIndex;
    s_runtimeMenu.scroll =
        s_runtimeMenu.selectedIndex >= kRuntimeMenuVisibleRows
            ? static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u))
            : 0u;
}

static void msx_runtime_menu_open_cas_page(void)
{
    s_runtimeMenu.page = MsxRuntimeMenuPage::Cas;
    s_runtimeMenu.selectedIndex = s_runtimeMenu.casSelectedIndex;
    s_runtimeMenu.scroll =
        s_runtimeMenu.selectedIndex >= kRuntimeMenuVisibleRows
            ? static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u))
            : 0u;
}

static void msx_runtime_menu_reset_to_first_main_item(void)
{
    s_runtimeMenu.page = MsxRuntimeMenuPage::Main;
    s_runtimeMenu.selectedIndex = 0;
    s_runtimeMenu.scroll = 0;
    s_runtimeMenu.mainSelectedIndex = 0;
}

static void msx_toggle_runtime_menu(void)
{
    s_runtimeMenu.visible = !s_runtimeMenu.visible;
    s_virtualKeyPickerVisible = false;
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
    } else if (msx_runtime_menu_in_sound_page()) {
        s_runtimeMenu.soundSelectedIndex = s_runtimeMenu.selectedIndex;
        if (selected < s_runtimeMenu.scroll) {
            s_runtimeMenu.scroll = static_cast<uint8_t>(selected);
        } else if (selected >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
            s_runtimeMenu.scroll =
                static_cast<uint8_t>(selected - (kRuntimeMenuVisibleRows - 1u));
        }
    } else if (msx_runtime_menu_in_cas_page()) {
        s_runtimeMenu.casSelectedIndex = s_runtimeMenu.selectedIndex;
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

static void msx_runtime_toggle_sound_item(MsxSoundMenuItem item, int delta)
{
    const int step = delta >= 0 ? 1 : -1;
    switch (item) {
        case MsxSoundMenuItem::VirtualScc:
            msx_config_cycle_virtual_scc_mode(step, true);
            break;
        case MsxSoundMenuItem::SoundVolume:
            msx_config_cycle_sound_volume(step, true);
            break;
        case MsxSoundMenuItem::SccVolume:
            msx_config_cycle_scc_gain_percent(step, true);
            break;
        case MsxSoundMenuItem::Back:
        case MsxSoundMenuItem::Count:
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

    if (msx_runtime_menu_in_sound_page()) {
        msx_runtime_toggle_sound_item(msx_get_sound_menu_item(s_runtimeMenu.selectedIndex), delta);
        return;
    }

    if (msx_get_menu_item(s_runtimeMenu.selectedIndex) == MsxRuntimeMenuItem::Performance) {
        msx_config_cycle_performance_preset(delta, true);
        return;
    }

    if (msx_get_menu_item(s_runtimeMenu.selectedIndex) == MsxRuntimeMenuItem::StateSlot) {
        int slot = s_runtimeOptions.stateSlot;
        slot = (slot + delta + 10) % 10;
        s_runtimeOptions.stateSlot = static_cast<uint8_t>(slot);
        msx_persist_runtime_option_config(msx_input_get_runtime_option_config());
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

    if (msx_runtime_menu_in_sound_page()) {
        s_runtimeMenu.soundSelectedIndex = s_runtimeMenu.selectedIndex;
        if (s_runtimeMenu.scroll > s_runtimeMenu.selectedIndex) {
            s_runtimeMenu.scroll = s_runtimeMenu.selectedIndex;
        }
        if (s_runtimeMenu.selectedIndex >= s_runtimeMenu.scroll + kRuntimeMenuVisibleRows) {
            s_runtimeMenu.scroll =
                static_cast<uint8_t>(s_runtimeMenu.selectedIndex - (kRuntimeMenuVisibleRows - 1u));
        }
        return;
    }

    if (msx_runtime_menu_in_cas_page()) {
        s_runtimeMenu.casSelectedIndex = s_runtimeMenu.selectedIndex;
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
                msx_runtime_menu_in_performance_page()
                    ? "perf"
                    : (msx_runtime_menu_in_cas_page() ? "cas" : "main"),
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

    if (msx_runtime_menu_in_sound_page()) {
        const MsxSoundMenuItem item = msx_get_sound_menu_item(s_runtimeMenu.selectedIndex);
        if (item == MsxSoundMenuItem::Back) {
            msx_runtime_menu_open_main_page();
            msx_clamp_runtime_menu_selection();
        } else {
            msx_runtime_toggle_sound_item(item, 1);
        }
        msx_runtime_log_options();
        return;
    }

    if (msx_runtime_menu_in_cas_page()) {
        switch (msx_get_cas_menu_item(s_runtimeMenu.selectedIndex)) {
            case MsxCasMenuItem::RunCas:
                msx_runtime_menu_reset_to_first_main_item();
                msx_start_cas_text_macro("RUN\"CAS:\"\n");
                s_runtimeMenu.visible = false;
                break;
            case MsxCasMenuItem::BloadCas:
                msx_runtime_menu_reset_to_first_main_item();
                msx_start_cas_text_macro("BLOAD\"CAS:\",R\n");
                s_runtimeMenu.visible = false;
                break;
            case MsxCasMenuItem::ChangeCas:
                if (s_runtimeOptions.changeCasAvailable) {
                    s_runtimeOptions.changeCasRequested = true;
                    s_runtimeMenu.visible = false;
                }
                break;
            case MsxCasMenuItem::Back:
            case MsxCasMenuItem::Count:
            default:
                msx_runtime_menu_open_main_page();
                msx_clamp_runtime_menu_selection();
                break;
        }
        msx_runtime_log_options();
        if (!s_runtimeMenu.visible) {
            msx_reset_menu_latches();
        }
        return;
    }

    switch (msx_get_menu_item(s_runtimeMenu.selectedIndex)) {
        case MsxRuntimeMenuItem::Joystick:
            s_runtimeOptions.joystickEnabled = !s_runtimeOptions.joystickEnabled;
            if (s_runtimeOptions.joystickEnabled) {
                s_runtimeOptions.basicKeyboardEnabled = false;
            }
            msx_persist_runtime_option_config(msx_input_get_runtime_option_config());
            break;
        case MsxRuntimeMenuItem::Keyboard:
            s_runtimeOptions.keyboardEnabled = !s_runtimeOptions.keyboardEnabled;
            s_runtimeOptions.basicKeyboardEnabled = false;
            msx_persist_runtime_option_config(msx_input_get_runtime_option_config());
            break;
        case MsxRuntimeMenuItem::BasicKeyboard:
            s_runtimeOptions.basicKeyboardEnabled = !s_runtimeOptions.basicKeyboardEnabled;
            if (s_runtimeOptions.basicKeyboardEnabled) {
                s_runtimeOptions.keyboardEnabled = false;
                s_runtimeOptions.joystickEnabled = false;
                s_runtimeOptions.vausEnabled = false;
            }
            msx_persist_runtime_option_config(msx_input_get_runtime_option_config());
            break;
        case MsxRuntimeMenuItem::Vaus:
            s_runtimeOptions.vausEnabled = !s_runtimeOptions.vausEnabled;
            if (s_runtimeOptions.vausEnabled) {
                s_runtimeOptions.basicKeyboardEnabled = false;
            }
            msx_persist_runtime_option_config(msx_input_get_runtime_option_config());
            break;
        case MsxRuntimeMenuItem::View:
            if (msx_view_toggle_allowed()) {
                msx_config_toggle_active_view_mode_for_target(
                    g_emu_display_target == EMU_DISPLAY_EXTERNAL
                );
            }
            break;
        case MsxRuntimeMenuItem::Performance:
            if (msx_config_get_performance_preset() == MsxPerformancePreset::Custom) {
                s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
                msx_runtime_menu_open_performance_page();
            }
            break;
        case MsxRuntimeMenuItem::Sound:
            s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
            msx_runtime_menu_open_sound_page();
            break;
        case MsxRuntimeMenuItem::ChangeDsk:
            if (s_runtimeOptions.changeDskAvailable) {
                s_runtimeOptions.changeDskRequested = true;
                s_runtimeMenu.visible = false;
            }
            break;
        case MsxRuntimeMenuItem::CasMenu:
            if (s_runtimeOptions.changeCasAvailable) {
                s_runtimeMenu.mainSelectedIndex = s_runtimeMenu.selectedIndex;
                msx_runtime_menu_open_cas_page();
            }
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
                if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
                    s_runtimeOptions.loadRequested = true;
                    s_runtimeMenu.visible = false;
                    break;
                }
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

static bool msx_binding_pressed_for_gameplay(char key, bool reserveVirtualKeyTrigger)
{
    if (reserveVirtualKeyTrigger && msx_normalize_char(key) == 'v') {
        return false;
    }
    return msx_binding_pressed(key);
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
        M5Cardputer.Display.setBrightness(std::min(brightness + kBrightnessStep, 255));
    }

    if (status.fn && msx_key_pressed_any('[', '{')) {
        const int brightness = M5Cardputer.Display.getBrightness();
        M5Cardputer.Display.setBrightness(std::max(brightness - kBrightnessStep, 0));
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
                                            const MsxInputBindingCache& cache,
                                            bool reserveVirtualKeyTrigger)
{
    if (!state) {
        return;
    }

    state->up    |= msx_binding_pressed_for_gameplay(cache.up, reserveVirtualKeyTrigger);
    state->down  |= msx_binding_pressed_for_gameplay(cache.down, reserveVirtualKeyTrigger);
    state->left  |= msx_binding_pressed_for_gameplay(cache.left, reserveVirtualKeyTrigger);
    state->right |= msx_binding_pressed_for_gameplay(cache.right, reserveVirtualKeyTrigger);
    state->fire1 |= msx_binding_pressed_for_gameplay(cache.fire1, reserveVirtualKeyTrigger);
    state->fire2 |= msx_binding_pressed_for_gameplay(cache.fire2, reserveVirtualKeyTrigger);
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
                                     bool basicKeyboardEnabled,
                                     bool virtualKeyPickerAllowed)
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

        if (virtualKeyPickerAllowed && msx_normalize_char(ch) == 'v') {
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
                                     const MsxInputBindingCache& cache,
                                     bool reserveVirtualKeyTrigger)
{
    if (!state || keys.fn) {
        return;
    }

    state->up |= msx_binding_pressed_for_gameplay(cache.up, reserveVirtualKeyTrigger);
    state->down |= msx_binding_pressed_for_gameplay(cache.down, reserveVirtualKeyTrigger);
    state->left |= msx_binding_pressed_for_gameplay(cache.left, reserveVirtualKeyTrigger);
    state->right |= msx_binding_pressed_for_gameplay(cache.right, reserveVirtualKeyTrigger);
    state->fire1 |= msx_binding_pressed_for_gameplay(cache.fire1, reserveVirtualKeyTrigger);
    state->fire2 |= msx_binding_pressed_for_gameplay(cache.fire2, reserveVirtualKeyTrigger);
    state->start |= msx_binding_pressed_for_gameplay(cache.start, reserveVirtualKeyTrigger);
    state->select |= msx_binding_pressed_for_gameplay(cache.select, reserveVirtualKeyTrigger);
}

static void msx_apply_emulator_actions_to_matrix(MsxKeyboardMatrix* matrix,
                                                 const MsxInputState* state,
                                                 bool joystickEnabled)
{
    if (!matrix || !state) {
        return;
    }

    if (!joystickEnabled) {
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
                                      bool basicKeyboardEnabled,
                                      bool virtualKeyPickerAllowed)
{
    msx_keyboard_matrix_clear(matrix);
    msx_apply_virtual_key_step(matrix);

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
        msx_apply_emulator_actions_to_matrix(matrix, state, joystickEnabled);
    }
    msx_apply_printable_keys(matrix,
                             keys,
                             cache,
                             joystickEnabled,
                             basicKeyboardEnabled,
                             virtualKeyPickerAllowed);
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

static bool msx_picker_left_pressed(const Keyboard_Class::KeysState& keys,
                                    const MsxInputBindingCache& bindings,
                                    uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
           msx_binding_pressed_for_gameplay(bindings.left, true) ||
           ((padState & share::PAD_LEFT) != 0u);
}

static bool msx_picker_right_pressed(const Keyboard_Class::KeysState& keys,
                                     const MsxInputBindingCache& bindings,
                                     uint32_t padState)
{
    return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
           msx_binding_pressed_for_gameplay(bindings.right, true) ||
           ((padState & share::PAD_RIGHT) != 0u);
}

static bool msx_picker_accept_pressed(const Keyboard_Class::KeysState& keys,
                                      const MsxInputBindingCache& bindings,
                                      uint32_t padState)
{
    return keys.enter ||
           keys.space ||
           msx_binding_pressed_for_gameplay(bindings.fire1, true) ||
           msx_binding_pressed_for_gameplay(bindings.start, true) ||
           ((padState & (share::PAD_A | share::PAD_START)) != 0u);
}

static bool msx_picker_back_pressed(const Keyboard_Class::KeysState& keys,
                                    const MsxInputBindingCache& bindings,
                                    uint32_t padState)
{
    return keys.del ||
           msx_binding_pressed_for_gameplay(bindings.fire2, true) ||
           msx_binding_pressed_for_gameplay(bindings.select, true) ||
           ((padState & (share::PAD_B | share::PAD_SELECT)) != 0u);
}

static void msx_reset_virtual_key_picker_latches(void)
{
    s_virtualKeyPickerLeftHeld = false;
    s_virtualKeyPickerRightHeld = false;
    s_virtualKeyPickerAcceptHeld = false;
    s_virtualKeyPickerBackHeld = false;
    s_virtualKeyPickerRepeatDir = 0;
    s_virtualKeyPickerNextRepeatMs = 0;
}

static void msx_close_virtual_key_picker(void)
{
    s_virtualKeyPickerVisible = false;
    msx_reset_virtual_key_picker_latches();
}

static void msx_move_virtual_key_picker(int delta)
{
    const int count = static_cast<int>(kMsxVirtualKeyPickerCount);
    int index = static_cast<int>(s_virtualKeyPickerIndex);
    index = (index + delta + count) % count;
    s_virtualKeyPickerIndex = static_cast<uint8_t>(index);
}

static void msx_apply_virtual_key_picker_repeat(int8_t dir, uint32_t nowMs)
{
    static constexpr uint32_t kInitialRepeatDelayMs = 420u;
    static constexpr uint32_t kRepeatIntervalMs = 180u;

    if (dir == 0) {
        s_virtualKeyPickerRepeatDir = 0;
        s_virtualKeyPickerNextRepeatMs = 0;
        return;
    }

    if (s_virtualKeyPickerRepeatDir != dir) {
        s_virtualKeyPickerRepeatDir = dir;
        s_virtualKeyPickerNextRepeatMs = nowMs + kInitialRepeatDelayMs;
        return;
    }

    if (s_virtualKeyPickerNextRepeatMs != 0u &&
        static_cast<int32_t>(nowMs - s_virtualKeyPickerNextRepeatMs) >= 0) {
        msx_move_virtual_key_picker(dir);
        s_virtualKeyPickerNextRepeatMs = nowMs + kRepeatIntervalMs;
    }
}

static bool msx_poll_virtual_key_picker(const Keyboard_Class::KeysState& keys,
                                        const MsxInputBindingCache& bindings,
                                        uint32_t padState,
                                        bool allowed)
{
    const bool triggerPressed = allowed && msx_virtual_key_picker_trigger_pressed();
    const bool triggerEdge = triggerPressed && !s_virtualKeyPickerTriggerHeld;
    s_virtualKeyPickerTriggerHeld = triggerPressed;

    if (!allowed) {
        msx_close_virtual_key_picker();
        s_virtualKeyPickerSuppressUntilRelease = false;
        return false;
    }

    const bool leftPressed = msx_picker_left_pressed(keys, bindings, padState);
    const bool rightPressed = msx_picker_right_pressed(keys, bindings, padState);
    const bool acceptPressed = msx_picker_accept_pressed(keys, bindings, padState);
    const bool backPressed = msx_picker_back_pressed(keys, bindings, padState) || triggerEdge;

    if (s_virtualKeyPickerSuppressUntilRelease) {
        if (!triggerPressed && !leftPressed && !rightPressed && !acceptPressed && !backPressed) {
            s_virtualKeyPickerSuppressUntilRelease = false;
            return false;
        }
        return true;
    }

    if (!s_virtualKeyPickerVisible && triggerEdge) {
        s_virtualKeyPickerVisible = true;
        s_virtualKeyPickerIndex = 0;
        s_virtualKeyPickerAwaitRelease = true;
        msx_reset_virtual_key_picker_latches();
        return true;
    }

    if (!s_virtualKeyPickerVisible) {
        return false;
    }

    if (s_virtualKeyPickerAwaitRelease) {
        s_virtualKeyPickerLeftHeld = leftPressed;
        s_virtualKeyPickerRightHeld = rightPressed;
        s_virtualKeyPickerAcceptHeld = acceptPressed;
        s_virtualKeyPickerBackHeld = backPressed;
        s_virtualKeyPickerRepeatDir = 0;
        s_virtualKeyPickerNextRepeatMs = 0;
        if (!triggerPressed && !leftPressed && !rightPressed && !acceptPressed && !backPressed) {
            s_virtualKeyPickerAwaitRelease = false;
        }
        return true;
    }

    if (msx_menu_edge(leftPressed, &s_virtualKeyPickerLeftHeld)) {
        msx_move_virtual_key_picker(-1);
        s_virtualKeyPickerRepeatDir = -1;
        s_virtualKeyPickerNextRepeatMs = millis() + 420u;
    }
    if (msx_menu_edge(rightPressed, &s_virtualKeyPickerRightHeld)) {
        msx_move_virtual_key_picker(1);
        s_virtualKeyPickerRepeatDir = 1;
        s_virtualKeyPickerNextRepeatMs = millis() + 420u;
    }
    msx_apply_virtual_key_picker_repeat((leftPressed == rightPressed) ? 0 : (leftPressed ? -1 : 1),
                                        millis());
    if (msx_menu_edge(acceptPressed, &s_virtualKeyPickerAcceptHeld)) {
        s_virtualKeyChar = msx_virtual_key_picker_current_char();
        s_virtualKeyFrames = 3u;
        s_virtualKeyPickerSuppressUntilRelease = true;
        msx_close_virtual_key_picker();
        return true;
    }
    if (msx_menu_edge(backPressed, &s_virtualKeyPickerBackHeld)) {
        s_virtualKeyPickerSuppressUntilRelease = true;
        msx_close_virtual_key_picker();
        return true;
    }

    return true;
}

static void msx_diag_append(char* dst, size_t dstSize, const char* text)
{
    if (!dst || dstSize == 0 || !text || text[0] == '\0') {
        return;
    }

    const size_t used = std::strlen(dst);
    if (used >= dstSize - 1) {
        return;
    }

    std::snprintf(dst + used,
                  dstSize - used,
                  "%s%s",
                  used == 0 ? "" : " ",
                  text);
}

static void msx_diag_appendf(char* dst, size_t dstSize, const char* fmt, ...)
{
    if (!dst || dstSize == 0 || !fmt) {
        return;
    }

    char buffer[24];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    msx_diag_append(dst, dstSize, buffer);
}

static const char* msx_diag_keyboard_bit_label(uint8_t row, uint8_t bit)
{
    static const char* const labels[kMsxKeyboardRowCount][8] = {
        {"0", "1", "2", "3", "4", "5", "6", "7"},
        {"8", "9", "-", "=", "\\", "[", "]", ";"},
        {"'", "", ",", ".", "/", "", "A", "B"},
        {"C", "D", "E", "F", "G", "H", "I", "J"},
        {"K", "L", "M", "N", "O", "P", "Q", "R"},
        {"S", "T", "U", "V", "W", "X", "Y", "Z"},
        {"SHIFT", "CTRL", "GRAPH", "CAPS", "CODE", "F1", "F2", "F3"},
        {"F4", "F5", "ESC", "TAB", "STOP", "BKSP", "SELECT", "ENTER"},
        {"SPACE", "HOME", "INS", "DEL", "LEFT", "UP", "DOWN", "RIGHT"},
        {"", "", "", "", "", "", "", ""},
        {"", "", "", "", "", "", "", ""},
    };

    if (row >= kMsxKeyboardRowCount || bit >= 8u) {
        return "";
    }
    return labels[row][bit];
}

static void msx_diag_keyboard_label(const MsxKeyboardMatrix& matrix, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = '\0';
    for (uint8_t row = 0; row < kMsxKeyboardRowCount; ++row) {
        const uint8_t value = matrix.rows[row];
        if (value == 0xFFu) {
            continue;
        }
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            if ((value & (1u << bit)) != 0u) {
                continue;
            }
            const char* label = msx_diag_keyboard_bit_label(row, bit);
            if (label[0] != '\0') {
                msx_diag_append(dst, dstSize, label);
            } else {
                msx_diag_appendf(dst, dstSize, "R%uB%u", row, bit);
            }
        }
    }

    if (dst[0] == '\0') {
        std::snprintf(dst, dstSize, "IDLE");
    }
}

static void msx_diag_raw_label(const Keyboard_Class::KeysState& keys, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = '\0';
    if (M5Cardputer.BtnA.isPressed()) {
        msx_diag_append(dst, dstSize, "GO");
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP)) {
        msx_diag_append(dst, dstSize, "UP");
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN)) {
        msx_diag_append(dst, dstSize, "DOWN");
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT)) {
        msx_diag_append(dst, dstSize, "LEFT");
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT)) {
        msx_diag_append(dst, dstSize, "RIGHT");
    }
    if (keys.enter) {
        msx_diag_append(dst, dstSize, "ENTER");
    }
    if (keys.del) {
        msx_diag_append(dst, dstSize, "DEL");
    }
    if (keys.tab) {
        msx_diag_append(dst, dstSize, "TAB");
    }
    if (keys.space) {
        msx_diag_append(dst, dstSize, "SPACE");
    }
    for (char ch : keys.word) {
        char label[2] = {ch, '\0'};
        msx_diag_append(dst, dstSize, label);
    }

    if (dst[0] == '\0') {
        std::snprintf(dst, dstSize, "NONE");
    }
}

static void msx_diag_action_label(const MsxInputBindingCache& bindings, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = '\0';
    if (msx_binding_pressed(bindings.up)) {
        msx_diag_append(dst, dstSize, "UP");
    }
    if (msx_binding_pressed(bindings.down)) {
        msx_diag_append(dst, dstSize, "DOWN");
    }
    if (msx_binding_pressed(bindings.left)) {
        msx_diag_append(dst, dstSize, "LEFT");
    }
    if (msx_binding_pressed(bindings.right)) {
        msx_diag_append(dst, dstSize, "RIGHT");
    }
    if (msx_binding_pressed(bindings.fire1)) {
        msx_diag_append(dst, dstSize, "PRIMARY");
    }
    if (msx_binding_pressed(bindings.fire2)) {
        msx_diag_append(dst, dstSize, "SECONDARY");
    }
    if (msx_binding_pressed(bindings.start)) {
        msx_diag_append(dst, dstSize, "START");
    }
    if (msx_binding_pressed(bindings.select)) {
        msx_diag_append(dst, dstSize, "MENU");
    }

    if (dst[0] == '\0') {
        std::snprintf(dst, dstSize, "NONE");
    }
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
        if (msx_runtime_menu_in_performance_page() ||
            msx_runtime_menu_in_sound_page() ||
            msx_runtime_menu_in_cas_page()) {
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

MsxRuntimeOptionConfig msx_input_load_runtime_option_config(void)
{
    Preferences prefs;
    prefs.begin(kMsxInputConfigNs, true);
    MsxRuntimeOptionConfig config = {
        prefs.getBool(kMsxInputJoystickKey, kDefaultRuntimeOptionConfig.joystickEnabled),
        prefs.getBool(kMsxInputKeyboardKey, kDefaultRuntimeOptionConfig.keyboardEnabled),
        prefs.getBool(kMsxInputBasicKey, kDefaultRuntimeOptionConfig.basicKeyboardEnabled),
        prefs.getBool(kMsxInputVausKey, kDefaultRuntimeOptionConfig.vausEnabled),
        prefs.getUChar(kMsxInputStateSlotKey, kDefaultRuntimeOptionConfig.stateSlot),
    };
    prefs.end();
    return msx_sanitize_runtime_option_config(config);
}

MsxRuntimeOptionConfig msx_input_get_runtime_option_config(void)
{
    return msx_sanitize_runtime_option_config(msx_current_runtime_option_config());
}

void msx_input_set_runtime_option_config(const MsxRuntimeOptionConfig& config, bool persist)
{
    msx_apply_runtime_option_config(config, persist);
}

void msx_input_poll_diagnostic(const MsxRuntimeOptionConfig& requestedConfig,
                               MsxInputDiagnosticState* diagnostic)
{
    if (!diagnostic) {
        return;
    }

    std::memset(diagnostic, 0, sizeof(*diagnostic));
    std::snprintf(diagnostic->rawLabel, sizeof(diagnostic->rawLabel), "NONE");
    std::snprintf(diagnostic->actionLabel, sizeof(diagnostic->actionLabel), "NONE");
    std::snprintf(diagnostic->joystickLabel, sizeof(diagnostic->joystickLabel), "OFF");
    std::snprintf(diagnostic->keyboardLabel, sizeof(diagnostic->keyboardLabel), "OFF");
    std::snprintf(diagnostic->vausLabel, sizeof(diagnostic->vausLabel), "OFF");

    M5Cardputer.update();
    const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    MsxInputBindingCache bindings = {};
    msx_load_binding_cache(&bindings);

    MsxRuntimeOptionConfig config = msx_sanitize_runtime_option_config(requestedConfig);
    const bool basicKeyboardEnabled = config.basicKeyboardEnabled;
    const bool joystickEnabled = config.joystickEnabled && !basicKeyboardEnabled;
    const bool keyboardEnabled = config.keyboardEnabled;
    const bool effectiveKeyboardEnabled = keyboardEnabled || basicKeyboardEnabled;
    const bool vausEnabled = config.vausEnabled && !basicKeyboardEnabled;

    msx_diag_raw_label(keys, diagnostic->rawLabel, sizeof(diagnostic->rawLabel));
    msx_diag_action_label(bindings, diagnostic->actionLabel, sizeof(diagnostic->actionLabel));
    diagnostic->hasInput = std::strcmp(diagnostic->rawLabel, "NONE") != 0;
    diagnostic->exitRequested = M5Cardputer.BtnA.wasClicked();

    MsxInputState state = {};
    msx_keyboard_matrix_clear(&state.keyboardMatrix);
    state.joystickEnabled = joystickEnabled;
    state.keyboardEnabled = effectiveKeyboardEnabled;
    state.basicKeyboardEnabled = basicKeyboardEnabled;
    state.vausEnabled = vausEnabled;

    if (!basicKeyboardEnabled && (joystickEnabled || vausEnabled)) {
        msx_apply_joystick_mode_actions(&state, bindings, false);
    }
    if (keyboardEnabled && !basicKeyboardEnabled) {
        msx_apply_shared_actions(&state, keys, bindings, false);
    }

    msx_build_keyboard_matrix(&state.keyboardMatrix,
                              keys,
                              &state,
                              bindings,
                              effectiveKeyboardEnabled,
                              config.joystickEnabled,
                              basicKeyboardEnabled,
                              false);

    if (joystickEnabled) {
        char buttons[24] = "";
        if (state.up) {
            msx_diag_append(buttons, sizeof(buttons), "UP");
        }
        if (state.down) {
            msx_diag_append(buttons, sizeof(buttons), "DOWN");
        }
        if (state.left) {
            msx_diag_append(buttons, sizeof(buttons), "LEFT");
        }
        if (state.right) {
            msx_diag_append(buttons, sizeof(buttons), "RIGHT");
        }
        if (state.fire1) {
            msx_diag_append(buttons, sizeof(buttons), "FIRE1");
        }
        if (state.fire2) {
            msx_diag_append(buttons, sizeof(buttons), "FIRE2");
        }
        std::snprintf(diagnostic->joystickLabel,
                      sizeof(diagnostic->joystickLabel),
                      "%s %s",
                      vausEnabled ? "PORT B" : "PORT A",
                      buttons[0] != '\0' ? buttons : "IDLE");
    }

    if (effectiveKeyboardEnabled) {
        msx_diag_keyboard_label(state.keyboardMatrix,
                                diagnostic->keyboardLabel,
                                sizeof(diagnostic->keyboardLabel));
    }

    if (vausEnabled) {
        char vaus[24] = "";
        if (state.left) {
            msx_diag_append(vaus, sizeof(vaus), "LEFT");
        }
        if (state.right) {
            msx_diag_append(vaus, sizeof(vaus), "RIGHT");
        }
        if (state.fire1 || state.fire2 || state.start) {
            msx_diag_append(vaus, sizeof(vaus), "BUTTON");
        }
        std::snprintf(diagnostic->vausLabel,
                      sizeof(diagnostic->vausLabel),
                      "%s",
                      vaus[0] != '\0' ? vaus : "IDLE");
    }
}

void msx_input_init(void)
{
    s_backtickPressedMs = 0;
    s_backtickLongHandled = false;
    s_goLongHandled = false;
    s_suppressGoClick = false;
    s_suppressGoUntilMs = 0;
    const MsxRuntimeOptionConfig config = msx_input_load_runtime_option_config();
    s_runtimeOptions = {
        config.joystickEnabled,
        config.keyboardEnabled,
        config.basicKeyboardEnabled,
        config.vausEnabled,
        config.stateSlot,
        false,
        false,
        false,
        false,
        false,
        false
    };
    s_runtimeMenu = {
        false,
        MsxRuntimeMenuPage::Main,
        0,
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
    if (!enabled) {
        return;
    }
    s_runtimeOptions.basicKeyboardEnabled = enabled;
    if (enabled) {
        s_runtimeOptions.keyboardEnabled = false;
        s_runtimeOptions.joystickEnabled = false;
        s_runtimeOptions.vausEnabled = false;
    }
}

void msx_input_set_cas_change_available(bool available)
{
    s_runtimeOptions.changeCasAvailable = available;
    if (!available) {
        s_runtimeOptions.changeCasRequested = false;
        if (msx_runtime_menu_in_cas_page()) {
            msx_runtime_menu_open_main_page();
        }
    }
    msx_clamp_runtime_menu_selection();
}

void msx_input_set_dsk_change_available(bool available)
{
    s_runtimeOptions.changeDskAvailable = available;
    if (!available) {
        s_runtimeOptions.changeDskRequested = false;
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
                msx_start_cas_text_macro("BLOAD\"CAS:\",R\n");
            } else {
                msx_start_cas_text_macro("RUN\"CAS:\"\n");
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
    const bool textMacroActive = msx_text_macro_active();
    const bool effectiveKeyboardEnabled =
        s_runtimeOptions.keyboardEnabled || basicKeyboardEnabled || textMacroActive;
    state->joystickMode = s_runtimeOptions.joystickEnabled && !basicKeyboardEnabled;
    state->joystickEnabled = s_runtimeOptions.joystickEnabled && !basicKeyboardEnabled;
    state->keyboardEnabled = effectiveKeyboardEnabled;
    state->basicKeyboardEnabled = basicKeyboardEnabled;
    state->vausEnabled = s_runtimeOptions.vausEnabled && !basicKeyboardEnabled;
    state->menuVisible = s_runtimeMenu.visible;
    state->virtualKeyPickerVisible = s_virtualKeyPickerVisible;

    if (menuWasVisible || s_runtimeMenu.visible || goLongToggledMenu) {
        return;
    }

    const bool virtualKeyPickerAllowed =
        msx_virtual_key_picker_allowed(s_runtimeOptions.keyboardEnabled,
                                       s_runtimeOptions.joystickEnabled,
                                       basicKeyboardEnabled);
    if (msx_poll_virtual_key_picker(keys, bindings, padState, virtualKeyPickerAllowed)) {
        state->virtualKeyPickerVisible = s_virtualKeyPickerVisible;
        if (s_virtualKeyFrames != 0u) {
            state->keyboardEnabled = true;
        }
        msx_keyboard_matrix_clear(&state->keyboardMatrix);
        msx_apply_virtual_key_step(&state->keyboardMatrix);
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
        msx_apply_joystick_mode_actions(state, bindings, virtualKeyPickerAllowed);
    }

    if (s_runtimeOptions.keyboardEnabled && !basicKeyboardEnabled) {
        msx_apply_shared_actions(state, keys, bindings, virtualKeyPickerAllowed);
    }

    msx_build_keyboard_matrix(&state->keyboardMatrix,
                              keys,
                              state,
                              bindings,
                              effectiveKeyboardEnabled,
                              s_runtimeOptions.joystickEnabled,
                              basicKeyboardEnabled,
                              virtualKeyPickerAllowed);
}

void msx_input_get_overlay_state(MsxInputOverlayState* state)
{
    if (!state) {
        return;
    }

    state->menuVisible = s_runtimeMenu.visible;
    state->performanceSubmenuVisible = msx_runtime_menu_in_performance_page();
    state->soundSubmenuVisible = msx_runtime_menu_in_sound_page();
    state->casSubmenuVisible = msx_runtime_menu_in_cas_page();
    state->machineIsMsx2 = (s_runtimeMachineMode == MsxMachineMode::MSX2);
    state->joystickEnabled = s_runtimeOptions.joystickEnabled;
    state->keyboardEnabled = s_runtimeOptions.keyboardEnabled;
    state->basicKeyboardEnabled = s_runtimeOptions.basicKeyboardEnabled;
    state->vausEnabled = s_runtimeOptions.vausEnabled;
    state->performanceMode = msx_config_get_performance_mode();
    state->performancePreset = msx_config_get_performance_preset();
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
    state->virtualSccMode = msx_config_get_virtual_scc_mode();
    state->soundVolume = msx_config_get_sound_volume();
    state->sccGainPercent = msx_config_get_scc_gain_percent();
    state->casChangeAvailable = s_runtimeOptions.changeCasAvailable;
    state->dskChangeAvailable = s_runtimeOptions.changeDskAvailable;
    state->virtualKeyPickerVisible = s_virtualKeyPickerVisible;
    std::snprintf(state->virtualKeyPickerLabel,
                  sizeof(state->virtualKeyPickerLabel),
                  "%s",
                  msx_virtual_key_picker_current_label());
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

bool msx_input_get_change_dsk_requested(void) {
    bool r = s_runtimeOptions.changeDskRequested;
    s_runtimeOptions.changeDskRequested = false;
    return r;
}
