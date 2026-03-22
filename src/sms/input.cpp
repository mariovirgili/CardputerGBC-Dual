#include "input.h"
#include <algorithm>
#include <M5Cardputer.h>
#include "share/emu_controls.h"
#include "share/input.h"

extern bool fullscreen;
extern bool scanline;
extern int smsZoomPercent;

static inline bool key(char c) {
    return M5Cardputer.Keyboard.isKeyPressed(c);
}

void cardputer_input_init() {
}

void cardputer_read_input(bool isGG) {
    int smsButtons = 0; // -> input.pad[0]
    int smsSystem  = 0; // -> input.system

    M5Cardputer.update();
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(status);

    // I2C PAD (M5Stack JoyV2)
    if (share::hasI2cPad()) {
        int i2cPad = share::pollI2cPad();
        if (i2cPad & share::PAD_LEFT)  smsButtons |= INPUT_LEFT;
        if (i2cPad & share::PAD_RIGHT) smsButtons |= INPUT_RIGHT;
        if (i2cPad & share::PAD_UP)    smsButtons |= INPUT_UP;
        if (i2cPad & share::PAD_DOWN)  smsButtons |= INPUT_DOWN;
        if (i2cPad & share::PAD_A)     smsButtons |= INPUT_BUTTON1;
    }

    // --- fullscreen / zoom cycle ---
    if (M5Cardputer.Keyboard.isChange() && key(CARDPUTER_SCREEN_TOGGLE)) {
        if (!fullscreen) {
            fullscreen = true;
            scanline   = true;
            smsZoomPercent = 100;
        } else {
            smsZoomPercent += 10;
            if (smsZoomPercent > 150) {
                smsZoomPercent = 100;
                fullscreen = false;
                scanline   = false;
            }
        }

        // Recalculer le scaler immédiatement
        if (fullscreen) video_compute_scaler_full();
        else            video_compute_scaler_square();

        input.pad[0] = 0; input.system = 0;
        return;
    }

    // --- Zoom fin (FN + , ou FN + /) ---
    if (status.fn && key(CARDPUTER_ZOOM_PLUS)) {
        smsZoomPercent += 1;
        if (smsZoomPercent > 150) smsZoomPercent = 150;
        video_compute_scaler_full();
        input.pad[0] = 0; input.system = 0;
        return;
    }

    if (status.fn && key(CARDPUTER_ZOOM_MINUS)) {
        smsZoomPercent -= 1;
        if (smsZoomPercent < 100) smsZoomPercent = 100;
        video_compute_scaler_full();
        input.pad[0] = 0; input.system = 0;
        return;
    }
    
    // ---------- Mapping  ----------
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Left)) smsButtons |= INPUT_LEFT;
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Right)) smsButtons |= INPUT_RIGHT;
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Up)) smsButtons |= INPUT_UP;
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Down)) smsButtons |= INPUT_DOWN;
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::A)) smsButtons |= INPUT_BUTTON1;
    if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::B)) smsButtons |= INPUT_BUTTON2;

    if (isGG) {
        if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Start)) smsSystem |= INPUT_START;
    } else {
        if (share::emuControlPressed(share::EmuProfile::Sms, share::EmuAction::Start)) smsSystem |= INPUT_PAUSE;
    }

    input.pad[0]  = smsButtons;
    input.system  = smsSystem;
}
