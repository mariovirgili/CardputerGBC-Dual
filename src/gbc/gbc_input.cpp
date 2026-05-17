#include "gbc_input.h"
#include <M5Cardputer.h>
#include "compat/arduino_compat.h"
#include "share/input.h"

extern "C" {
  #include "gnuboy/gnuboy.h"
#include "share/emu_log_cpp.h"
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
            EMU_LOG("[GBC] Palette -> %d\n", gbPalette);
        }

        return dummy_ret;
    }

    // ================== DIRECTIONS ==================
    // Left : 'a' or ','
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) {
        pad |=  GB_PAD_LEFT;
    }

    // Right : 'd' or '/'
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) {
        pad |=  GB_PAD_RIGHT;
    }

    // Up : 'e' or ';'
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) {
        pad |=  GB_PAD_UP;
    }

    // Down : 's', '.' or 'z'
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) {
        pad |=  GB_PAD_DOWN;
    }

    // ================== BOUTONS GBC ==================
    // A
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_2)) {
        pad |=  GB_PAD_A;
    }

    // B
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) {
        pad |=  GB_PAD_B;
    }

    // START
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START)) {
        pad |=  GB_PAD_START;
    }

    // SELECT
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_SELECT)) {
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
