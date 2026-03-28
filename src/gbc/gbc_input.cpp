#include "gbc_input.h"
#include <M5Cardputer.h>
#include <Arduino.h>
#include "share/emu_controls.h"
#include "share/input.h"

extern "C" {
  #include "gnuboy/gnuboy.h"
}

extern bool gbcFullScreen;
extern int  gbcZoomPercent;
extern int  gbPalette;

extern "C" int gbc_input_poll(void)
{   
    const int dummy_ret = -1; 

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(ks);

    int pad = 0x00;

    // I2C PAD (M5Stack JoyV2)
    if ( share::hasI2cPad()) {
        int i2cPad = share::pollI2cPad();
        if (i2cPad & share::PAD_LEFT)  pad |= GB_PAD_LEFT;
        if (i2cPad & share::PAD_RIGHT) pad |= GB_PAD_RIGHT;
        if (i2cPad & share::PAD_UP)    pad |= GB_PAD_UP;
        if (i2cPad & share::PAD_DOWN)  pad |= GB_PAD_DOWN;
        if (i2cPad & share::PAD_A)     pad |= GB_PAD_A;
    }

    // ================== SCREEN MODE ==================

    // Screen mode
    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        
        // -1 is gameboy color mode
        if (gbPalette == -1) {
            //
            // fullscreen / zoom (used by GBC only)
            //
            if (!gbcFullScreen) {
                gbcFullScreen  = true;
                gbcZoomPercent = 100;
            } else {
                gbcZoomPercent += 10;
                if (gbcZoomPercent > 150) {
                    gbcZoomPercent = 100;
                    gbcFullScreen  = false;
                }
            }
        } else {
            //
            // palette toggle (used by GB only)
            //
            gbPalette++;
            if (gbPalette > 36) {
                gbPalette = 0;
            }

            gnuboy_set_palette((gb_palette_t)gbPalette);
            printf("[GBC] Palette -> %d\n", gbPalette);
        }

        return dummy_ret;
    }

    // ================== DIRECTIONS ==================
    // Left : 'a' or ','
    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Left)) {
        pad |=  GB_PAD_LEFT;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Right)) {
        pad |=  GB_PAD_RIGHT;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Up)) {
        pad |=  GB_PAD_UP;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Down)) {
        pad |=  GB_PAD_DOWN;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::A)) {
        pad |=  GB_PAD_A;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::B)) {
        pad |=  GB_PAD_B;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Start)) {
        pad |=  GB_PAD_START;
    }

    if (share::emuControlPressed(share::EmuProfile::Gbc, share::EmuAction::Select)) {
        pad |=  GB_PAD_SELECT;
    }

    // ================== ZOOM  ==================
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        if (!gbcFullScreen) gbcFullScreen = true;
        gbcZoomPercent = (gbcZoomPercent < 150) ? (gbcZoomPercent + 1) : 150;
        return dummy_ret;
    }
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        if (!gbcFullScreen) gbcFullScreen = true;
        gbcZoomPercent = (gbcZoomPercent > 100) ? (gbcZoomPercent - 1) : 100;
        return dummy_ret;
    }

    return pad;
}
