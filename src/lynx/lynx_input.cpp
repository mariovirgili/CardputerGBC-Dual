#include "lynx_input.h"

#include <M5Cardputer.h>
#include <Arduino.h>
#include "share/emu_controls.h"
#include "share/input.h"
#include "handy/handy.h"
#include "handy/susie.h"

// from lynx_display.cpp
extern bool lynxFullScreen;
extern int  lynxZoomPercent;

extern "C" int lynx_input_poll(void)
{
    const int dummy_ret = -1;

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(ks);

    uint32_t pad = 0x00;

    // I2C PAD (M5Stack JoyV2)
    if (share::hasI2cPad()) {
        int i2cPad = share::pollI2cPad();
        if (i2cPad & share::PAD_LEFT)  pad |= BUTTON_LEFT;
        if (i2cPad & share::PAD_RIGHT) pad |= BUTTON_RIGHT;
        if (i2cPad & share::PAD_UP)    pad |= BUTTON_UP;
        if (i2cPad & share::PAD_DOWN)  pad |= BUTTON_DOWN;
        if (i2cPad & share::PAD_A)     pad |= BUTTON_A;
    }

    // ================== SCREEN MODE (touche '\') ==================
    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {

        if (!lynxFullScreen) {
            lynxFullScreen  = true;
            lynxZoomPercent = 100;
        } else {
            lynxZoomPercent += 10;
            if (lynxZoomPercent > 150) {
                lynxZoomPercent = 100;
                lynxFullScreen  = false;
            }
        }

        return dummy_ret;
    }

    // ================== DIRECTIONS ==================
    // Left : 'a' ou ','
    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Left)) {
        pad |= BUTTON_LEFT;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Right)) {
        pad |= BUTTON_RIGHT;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Up)) {
        pad |= BUTTON_UP;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Down)) {
        pad |= BUTTON_DOWN;
    }

    // ================== LYNX BUTTONS ==================
    // A  (main fire)
    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::A)) {
        pad |= BUTTON_A;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::B)) {
        pad |= BUTTON_B;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Pause)) {
        pad |= BUTTON_PAUSE;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Option)) {
        pad |= BUTTON_OPT1;
    }

    if (share::emuControlPressed(share::EmuProfile::Lynx, share::EmuAction::Option2)) {
        pad |= BUTTON_OPT2;
    }

    // ================== ZOOM (FN + arrows) ==================
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        if (!lynxFullScreen) lynxFullScreen = true;
        lynxZoomPercent = (lynxZoomPercent < 150) ? (lynxZoomPercent + 1) : 150;
        return dummy_ret;
    }
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        if (!lynxFullScreen) lynxFullScreen = true;
        lynxZoomPercent = (lynxZoomPercent > 100) ? (lynxZoomPercent - 1) : 100;
        return dummy_ret;
    }

    return (int)pad;
}
