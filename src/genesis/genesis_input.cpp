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

#ifndef MD_DETERMINISTIC_BENCH
#define MD_DETERMINISTIC_BENCH 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_LOCK_INPUT
#define MD_DETERMINISTIC_BENCH_LOCK_INPUT 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_START_AT_FRAME
#define MD_DETERMINISTIC_BENCH_START_AT_FRAME 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES
#define MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES 0
#endif

#if MD_DETERMINISTIC_BENCH_LOCK_INPUT
extern "C" uint32_t md_deterministic_bench_frame(void);
#endif

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

static inline void set_all_buttons(bool up, bool down, bool left, bool right,
                                   bool btnA, bool btnB, bool btnC, bool btnStart) {
  set_button(BTN_UP,    up);
  set_button(BTN_DOWN,  down);
  set_button(BTN_LEFT,  left);
  set_button(BTN_RIGHT, right);
  set_button(BTN_A,     btnA);
  set_button(BTN_B,     btnB);
  set_button(BTN_C,     btnC);
  set_button(BTN_START, btnStart);
}

static inline uint8_t clamp_volume(int volume) {
  if (volume < 0) return 0;
  if (volume > 255) return 255;
  return (uint8_t)volume;
}

/* Polling cardputer keyboard */
extern "C" void genesis_controller_poll() {

#if MD_DETERMINISTIC_BENCH_LOCK_INPUT
    const uint32_t frame = md_deterministic_bench_frame();
    const uint32_t startAt = (uint32_t)MD_DETERMINISTIC_BENCH_START_AT_FRAME;
    const uint32_t startHold = (uint32_t)MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES;
    const bool start =
        (startHold > 0u) &&
        (frame >= startAt) &&
        (frame < (startAt + startHold));
    set_all_buttons(false, false, false, false, false, false, false, start);
    return;
#else
    // limit polling rate
    if (share::shouldPollInput() == false) {
        return;
    }

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    // ------ Common input (volume, brightness...) -------
    const int volumeBefore = M5Cardputer.Speaker.getVolume();
    share::checkCommonInput(ks);
    const int volumeAfter = M5Cardputer.Speaker.getVolume();
    if (volumeAfter != volumeBefore) {
        genesis_audio_volume = clamp_volume(volumeAfter);
    }

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
    set_all_buttons(up, down, left, right, btnA, btnB, btnC, btnStart);
#endif
}

/* Called by Gwenesis to poll button states */
extern "C" void gwenesis_io_get_buttons(void) {
  genesis_controller_poll();
}
