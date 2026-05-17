#include "a7800_input.h"

#include "compat/arduino_compat.h"
#include <M5Cardputer.h>

#include <algorithm>

#include "../share/input.h"
#include "a7800_config.h"
#include "a7800_video.h"
#include "share/emu_log_cpp.h"

static constexpr uint32_t kBacktickLongPressMs = 700;
static uint32_t s_backtickPressedMs = 0;
static bool s_backtickLongHandled = false;

static inline bool a7800_key(char key)
{
    return M5Cardputer.Keyboard.isKeyPressed(key);
}

static void a7800_apply_system_keys(const Keyboard_Class::KeysState& keys)
{
    if (a7800_key(CARDPUTER_VOL_UP_1) || (keys.fn && a7800_key(CARDPUTER_VOL_UP_2))) {
        int volume = M5Cardputer.Speaker.getVolume();
        M5Cardputer.Speaker.setVolume(std::min(volume + 3, 255));
    }

    if (a7800_key(CARDPUTER_VOL_DOWN_1) || (keys.fn && a7800_key(CARDPUTER_VOL_DOWN_2))) {
        int volume = M5Cardputer.Speaker.getVolume();
        M5Cardputer.Speaker.setVolume(std::max(volume - 3, 0));
    }

    if (a7800_key(CARDPUTER_BRIGHT_UP)) {
        int brightness = M5Cardputer.Display.getBrightness();
        M5Cardputer.Display.setBrightness(std::min(brightness + 2, 255));
    }

    if (a7800_key(CARDPUTER_BRIGHT_DOWN) && !a7800_key(CARDPUTER_UP_1)) {
        int brightness = M5Cardputer.Display.getBrightness();
        M5Cardputer.Display.setBrightness(std::max(brightness - 2, 0));
    }
}

void a7800_input_init(void)
{
    s_backtickPressedMs = 0;
    s_backtickLongHandled = false;
}

void a7800_input_poll(A7800InputState* state)
{
    if (!state) {
        return;
    }

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(ks);
    a7800_apply_system_keys(ks);

    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool fire1 = false;
    bool fire2 = false;
    bool select = false;
    bool reset = false;
    bool pause = false;
    bool quitRequested = false;

    if (a7800_key('`')) {
        if (s_backtickPressedMs == 0) {
            s_backtickPressedMs = millis();
            s_backtickLongHandled = false;
        } else if (!s_backtickLongHandled &&
                   (uint32_t)(millis() - s_backtickPressedMs) >= kBacktickLongPressMs) {
            s_backtickLongHandled = true;
            quitRequested = true;
        }
    } else {
        s_backtickPressedMs = 0;
        s_backtickLongHandled = false;
    }

    // ================== I2C PAD ==================
    if (share::hasI2cPad()) {
        const uint32_t pad = share::pollI2cPad();

        if (pad & share::PAD_LEFT)   left = true;
        if (pad & share::PAD_RIGHT)  right = true;
        if (pad & share::PAD_UP)     up = true;
        if (pad & share::PAD_DOWN)   down = true;
        if (pad & share::PAD_A)      fire1 = true;
        if (pad & share::PAD_B)      fire2 = true;
        if (pad & share::PAD_SELECT) select = true;
        if (pad & share::PAD_START)  pause = true;
    }

    // ================== DIRECTIONS ==================
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) {
        left = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) {
        right = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) {
        up = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) {
        down = true;
    }

    // ================== BOUTONS 7800 ==================
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_2)) {
        fire2 = true;
    }
        
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) {
        fire1 = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_SELECT)) {
        select = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START)) {
        reset = true;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('3')) {
        pause = true;
    }

    // ================== SCREEN MODE ==================
    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        if (ks.fn) {
            a7800_config_toggle_internal_view_mode();
            EMU_LOG("[A7800][DISP] internal view=%s\n",
                   a7800_config_get_internal_view_mode_label());
        } else {
            a7800_video_toggle_fullscreen();
        }
    }

    // ================== ZOOM ==================
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        a7800_video_adjust_zoom(+1);
    }

    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        a7800_video_adjust_zoom(-1);
    }

    state->up = up;
    state->down = down;
    state->left = left;
    state->right = right;
    state->fire1 = fire1;
    state->fire2 = fire2;
    state->select = select;
    state->reset = reset;
    state->pause = pause;
    state->quitRequested = quitRequested;
}