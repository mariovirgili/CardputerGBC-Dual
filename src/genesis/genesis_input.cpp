#include "genesis_input.h"
#include "share/input.h"

#include <M5Cardputer.h>
#include "compat/arduino_compat.h"

extern "C" {
  // Gwenesis APIs 
  void gwenesis_io_pad_press_button(int pad, int idx);
  void gwenesis_io_pad_release_button(int pad, int idx);
  void gwenesis_io_get_buttons(void);
}

// Shared state
extern bool fullscreenMode;
extern uint8_t genesis_audio_volume;
extern int  genesisZoomPercent;

// Buttons mapping
enum {
  BTN_UP = 0,
  BTN_DOWN,
  BTN_LEFT,
  BTN_RIGHT,
  BTN_C,
  BTN_B,
  BTN_A,
  BTN_START,
};

/* Button state management */
static inline void set_button(int idx, bool pressed) {
  if (pressed) gwenesis_io_pad_press_button(0, idx);
  else         gwenesis_io_pad_release_button(0, idx);
}

/* Polling cardputer keyboard */
extern "C" void genesis_controller_poll() {

    // limit polling rate
    if (share::shouldPollInput() == false) {
        return;
    }

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    // ------ Common input (volume, brightness...) -------
    share::checkCommonInput(ks);

    // --------- Cumulated I2C + keyboard ----------
    bool up      = false;
    bool down    = false;
    bool left    = false;
    bool right   = false;
    bool btnA    = false;
    bool btnB    = false;
    bool btnC    = false;
    bool btnStart= false;

    // --------- I2C PAD ---------
    if (share::hasI2cPad()) {
        uint32_t padState = share::pollI2cPad();

        // Fusion I2C
        up      = up      || (padState & share::PAD_UP);
        down    = down    || (padState & share::PAD_DOWN);
        left    = left    || (padState & share::PAD_LEFT);
        right   = right   || (padState & share::PAD_RIGHT);
        btnA    = btnA    || (padState & share::PAD_A);
    }

    // --------- SCREEN MODE / ZOOM ----------
    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {

        if (!fullscreenMode) {
            fullscreenMode = true;
            genesisZoomPercent = 100;
        } else {
            genesisZoomPercent += 10;
            if (genesisZoomPercent > 150) {
                genesisZoomPercent = 100;
                fullscreenMode = false;
            }
        }
        return;
    }

    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
        if (!fullscreenMode) fullscreenMode = true;
        genesisZoomPercent += 1;
        if (genesisZoomPercent > 150) genesisZoomPercent = 150;
        return;
    }
    if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
        if (!fullscreenMode) fullscreenMode = true;
        genesisZoomPercent -= 1;
        if (genesisZoomPercent < 100) genesisZoomPercent = 100;
        return;
    }

    // --------- CLAVIER : ZQSD / ,./ ----------
    const bool leftKey  =
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2);

    const bool rightKey =
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2);

    const bool upKey =
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2);

    const bool downKey =
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3);

    const bool btnAKey     = M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_2);
    const bool btnBKey     = M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B);
    const bool btnCKey     = M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1);
    const bool btnStartKey = M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START);

    // Fusion clavier + I2C
    left    = left    || leftKey;
    right   = right   || rightKey;
    up      = up      || upKey;
    down    = down    || downKey;
    btnA    = btnA    || btnAKey;
    btnB    = btnB    || btnBKey;
    btnC    = btnC    || btnCKey;
    btnStart= btnStart|| btnStartKey;

    // Push states to Gwenesis
    set_button(BTN_UP,    up);
    set_button(BTN_DOWN,  down);
    set_button(BTN_LEFT,  left);
    set_button(BTN_RIGHT, right);
    set_button(BTN_A,     btnA);
    set_button(BTN_B,     btnB);
    set_button(BTN_C,     btnC);
    set_button(BTN_START, btnStart);
}

/* Called by Gwenesis to poll button states */
extern "C" void gwenesis_io_get_buttons(void) {
  genesis_controller_poll();
}
