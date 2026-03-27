#include "a2600_input.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "Event.hxx"
#include "a2600_display.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "share/input.h"

void a2600_input_init(void)
{
}

void a2600_input_update(Event& event)
{
    M5Cardputer.update();
    const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(keys);

    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool fire = false;
    bool select = false;
    bool reset = false;

    if (share::hasI2cPad()) {
        const uint32_t pad = share::pollI2cPad();
        up |= (pad & share::PAD_UP) != 0;
        down |= (pad & share::PAD_DOWN) != 0;
        left |= (pad & share::PAD_LEFT) != 0;
        right |= (pad & share::PAD_RIGHT) != 0;
        fire |= (pad & share::PAD_A) != 0;
    }

    up |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Up);
    down |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Down);
    left |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Left);
    right |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Right);
    fire |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::A);
    select |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Select);
    reset |= share::emuControlPressed(share::EmuProfile::A2600, share::EmuAction::Start);

    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        if (keys.fn && g_emu_display_target != EMU_DISPLAY_EXTERNAL) {
            a2600_display_toggle_internal_view_mode();
        } else {
            if (!a2600FullScreen) {
                a2600FullScreen = true;
                a2600ZoomPercent = 100;
            } else {
                a2600ZoomPercent += 10;
                if (a2600ZoomPercent > 150) {
                    a2600ZoomPercent = 100;
                    a2600FullScreen = false;
                }
            }
        }
    }

    if (keys.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        if (!a2600FullScreen) {
            a2600FullScreen = true;
        }
        if (a2600ZoomPercent < 150) {
            a2600ZoomPercent++;
        }
    }

    if (keys.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        if (!a2600FullScreen) {
            a2600FullScreen = true;
        }
        if (a2600ZoomPercent > 100) {
            a2600ZoomPercent--;
        }
    }

    event.set(Event::Type(Event::JoystickZeroUp), up ? 1 : 0);
    event.set(Event::Type(Event::JoystickZeroDown), down ? 1 : 0);
    event.set(Event::Type(Event::JoystickZeroLeft), left ? 1 : 0);
    event.set(Event::Type(Event::JoystickZeroRight), right ? 1 : 0);
    event.set(Event::Type(Event::JoystickZeroFire), fire ? 1 : 0);
    event.set(Event::Type(Event::ConsoleSelect), select ? 1 : 0);
    event.set(Event::Type(Event::ConsoleReset), reset ? 1 : 0);
}
