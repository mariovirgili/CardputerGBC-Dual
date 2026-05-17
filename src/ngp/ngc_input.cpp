#include "ngc_input.h"
#include <M5Cardputer.h>
#include "compat/arduino_compat.h"
#include "race/input.h"
#include "share/input.h"

#ifndef NGP_INPUT_ACTIVE_LOW
#define NGP_INPUT_ACTIVE_LOW 0
#endif

#define NGP_BTN_UP       (1u << 0)
#define NGP_BTN_DOWN     (1u << 1)
#define NGP_BTN_LEFT     (1u << 2)
#define NGP_BTN_RIGHT    (1u << 3)
#define NGP_BTN_A        (1u << 4)
#define NGP_BTN_B        (1u << 5)
#define NGP_BTN_OPTION   (1u << 6)

extern bool ngpFullscreen;
extern int  ngpZoomPercent;
static bool prevBs = false;
static uint32_t lastToggle = 0;

extern "C" void ngc_input_init(void) {
  ngpInputState = 0;
}

extern "C" uint32_t ngc_input_poll(void) {
  uint32_t dummy_ret = 0xFFFFFFFF;

  // Read Keyboard matrix
  M5Cardputer.update();
  Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
  share::checkCommonInput(ks);

// --- state de base ---
#if NGP_INPUT_ACTIVE_LOW
  ngpInputState = 0xFF;
#else
  ngpInputState = 0x00;
#endif

  // I2C PAD (M5Stack JoyV2)
  if (share::hasI2cPad()) {
      int i2cPad = share::pollI2cPad();
      if (i2cPad & share::PAD_LEFT)  ngpInputState |= NGP_BTN_LEFT;
      if (i2cPad & share::PAD_RIGHT) ngpInputState |= NGP_BTN_RIGHT;
      if (i2cPad & share::PAD_UP)    ngpInputState |= NGP_BTN_UP;
      if (i2cPad & share::PAD_DOWN)  ngpInputState |= NGP_BTN_DOWN;
      if (i2cPad & share::PAD_A)     ngpInputState |= NGP_BTN_A;
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
    ngpFullscreen = !ngpFullscreen;
  }

  // -------- DIRECTIONS --------
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_LEFT;
#else
    ngpInputState |=  NGP_BTN_LEFT;
#endif
  }
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_RIGHT;
#else
    ngpInputState |=  NGP_BTN_RIGHT;
#endif
  }
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_UP;
#else
    ngpInputState |=  NGP_BTN_UP;
#endif
  }
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
      M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
      M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_DOWN;
#else
    ngpInputState |=  NGP_BTN_DOWN;
#endif
  }

  // -------- BOUTONS --------
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1) || M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_2)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_A;
#else
    ngpInputState |=  NGP_BTN_A;
#endif
  }
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_B;
#else
    ngpInputState |=  NGP_BTN_B;
#endif
  }
  if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START) || (ks.fn && M5Cardputer.Keyboard.isKeyPressed(' '))) {
#if NGP_INPUT_ACTIVE_LOW
    ngpInputState &= ~NGP_BTN_OPTION;
#else
    ngpInputState |=  NGP_BTN_OPTION;
#endif
  }

  // Zoom
  if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
    if (!ngpFullscreen) ngpFullscreen = true;
    ngpZoomPercent = (ngpZoomPercent < 150) ? (ngpZoomPercent + 1) : 150;
    return dummy_ret;
  }
  if (ks.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
    if (!ngpFullscreen) ngpFullscreen = true;
    ngpZoomPercent = (ngpZoomPercent > 100) ? (ngpZoomPercent - 1) : 100;
    return dummy_ret;
  }

  return ngpInputState;
}
