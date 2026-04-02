#include "msx_input.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "../cardputer/CardputerInput.h"
#include "../share/emu_controls.h"
#include "../share/input.h"
#include "core/msx_keyboard.h"

#include <algorithm>
#include <cctype>
#include <cstring>

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
static uint32_t s_backtickPressedMs = 0;
static bool s_backtickLongHandled = false;

static inline bool msx_key_pressed(char key)
{
    return M5Cardputer.Keyboard.isKeyPressed(key);
}

static inline bool msx_key_pressed_any(char a, char b)
{
    return msx_key_pressed(a) || msx_key_pressed(b);
}

static char msx_normalize_char(char ch)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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

static void msx_apply_printable_keys(MsxKeyboardMatrix* matrix,
                                     const Keyboard_Class::KeysState& keys,
                                     const MsxInputBindingCache& cache)
{
    if (!matrix) {
        return;
    }

    for (char ch : keys.word) {
        if (keys.fn && msx_is_reserved_fn_char(ch)) {
            continue;
        }

        if (ch == '`' || ch == '~') {
            continue;
        }

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
                                      const MsxInputBindingCache& cache)
{
    msx_keyboard_matrix_clear(matrix);
    msx_apply_modifier_keys(matrix, keys);
    msx_apply_direct_special_keys(matrix, keys);
    msx_apply_fn_combos(matrix, keys);
    msx_apply_emulator_actions_to_matrix(matrix, state);
    msx_apply_printable_keys(matrix, keys, cache);
}

void msx_input_init(void)
{
    s_backtickPressedMs = 0;
    s_backtickLongHandled = false;
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

    if (M5Cardputer.BtnA.wasClicked()) {
        state->quitRequested = true;
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

    if (keys.fn &&
        M5Cardputer.Keyboard.isChange() &&
        msx_key_pressed_any(CARDPUTER_SCREEN_TOGGLE, '|')) {
        state->toggleViewRequested = true;
    }

    if (bindings.hasI2cPad) {
        const uint32_t pad = share::pollI2cPad();
        state->up |= (pad & share::PAD_UP) != 0;
        state->down |= (pad & share::PAD_DOWN) != 0;
        state->left |= (pad & share::PAD_LEFT) != 0;
        state->right |= (pad & share::PAD_RIGHT) != 0;
        state->fire1 |= (pad & share::PAD_A) != 0;
        state->fire2 |= (pad & share::PAD_B) != 0;
        state->start |= (pad & share::PAD_START) != 0;
        state->select |= (pad & share::PAD_SELECT) != 0;
    }

    msx_apply_shared_actions(state, keys, bindings);
    msx_build_keyboard_matrix(&state->keyboardMatrix, keys, state, bindings);
}
