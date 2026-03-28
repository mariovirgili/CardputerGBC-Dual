#include "a7800_input.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>

#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../share/input.h"
#include "a7800_config.h"
#include "a7800_video.h"

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

    if (a7800_key(CARDPUTER_BRIGHT_DOWN)) {
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
    const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

    state->up = false;
    state->down = false;
    state->left = false;
    state->right = false;
    state->fire1 = false;
    state->fire2 = false;
    state->select = false;
    state->reset = false;
    state->pause = false;
    state->quitRequested = false;

    a7800_apply_system_keys(keys);

    if (M5Cardputer.BtnA.wasClicked()) {
        state->quitRequested = true;
    }

    if (a7800_key('`')) {
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

    if (share::hasI2cPad()) {
        const uint32_t pad = share::pollI2cPad();
        state->up |= (pad & share::PAD_UP) != 0;
        state->down |= (pad & share::PAD_DOWN) != 0;
        state->left |= (pad & share::PAD_LEFT) != 0;
        state->right |= (pad & share::PAD_RIGHT) != 0;
        state->fire1 |= (pad & share::PAD_A) != 0;
        state->fire2 |= (pad & share::PAD_B) != 0;
        state->pause |= (pad & share::PAD_START) != 0;
        state->select |= (pad & share::PAD_SELECT) != 0;
    }

    state->up |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Up);
    state->down |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Down);
    state->left |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Left);
    state->right |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Right);
    state->fire1 |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::A);
    state->fire2 |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::B);
    state->select |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Select);
    state->reset |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Start);
    state->pause |= share::emuControlPressed(share::EmuProfile::A7800, share::EmuAction::Pause);

    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        if (keys.fn && g_emu_display_target == EMU_DISPLAY_INTERNAL) {
            a7800_config_toggle_internal_view_mode();
            printf("[A7800][DISP] internal view=%s\n", a7800_config_get_internal_view_mode_label());
        } else {
            a7800_video_toggle_fullscreen();
        }
    }

    if (keys.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        a7800_video_adjust_zoom(+1);
    }

    if (keys.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        a7800_video_adjust_zoom(-1);
    }
}
