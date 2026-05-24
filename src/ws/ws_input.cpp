#include "ws_input.h"
#include <M5Cardputer.h>
#include <algorithm>
#include <stdint.h>
#include "share/input.h"
#include "ws_save.h"
#include "ws_state.h"

extern bool ws_fullscreen;
extern int  ws_zoomPercent;
extern uint32_t lastPadState;

static volatile uint16_t s_cachedState[2] = {0, 0};

static bool ws_key_pressed(char c)
{
  return M5Cardputer.Keyboard.isKeyPressed(c);
}

static uint16_t ws_input_compute_state(int mode, uint32_t i2cPad)
{
  uint16_t state = 0;

  // I2C PAD (M5Stack JoyV2)
  if (i2cPad) {
      if (i2cPad & share::PAD_LEFT)  state |= WS_X4; 
      if (i2cPad & share::PAD_RIGHT) state |= WS_X2; 
      if (i2cPad & share::PAD_UP)    state |= WS_X1; 
      if (i2cPad & share::PAD_DOWN)  state |= WS_X3;
      if (i2cPad & share::PAD_A)     state |= WS_A;
  }

  // Directional pad
  const bool left  = ws_key_pressed('e');
  const bool right = ws_key_pressed('z') || ws_key_pressed('s');
  const bool up    = ws_key_pressed('d');
  const bool down  = ws_key_pressed('a');

  // mode == 0 : horizontal
  if (mode == 0) {
    if (left)  state |= WS_X1;
    if (up)    state |= WS_X2;
    if (right) state |= WS_X3;
    if (down)  state |= WS_X4;
  // mode == 1 : vertical 
  } else {
    if (left)  state |= WS_Y1;
    if (up)    state |= WS_Y2;
    if (right) state |= WS_Y3;
    if (down)  state |= WS_Y4;
  }

  // Secondary directional pad 
  const bool a = ws_key_pressed(CARDPUTER_UP_2);
  const bool w = ws_key_pressed(CARDPUTER_DOWN_2);
  const bool d = ws_key_pressed(CARDPUTER_RIGHT_2);
  const bool s = ws_key_pressed(CARDPUTER_LEFT_2);

  if (mode == 0) {
    // In horizontal mode, WASD -> Y cross
    if (a) state |= WS_Y1;
    if (w) state |= WS_Y2;
    if (d) state |= WS_Y3;
    if (s) state |= WS_Y4;
  } else {
    // In vertical mode, WASD -> X cross
    if (a) state |= WS_X1;
    if (w) state |= WS_X2;
    if (d) state |= WS_X3;
    if (s) state |= WS_X4;
  }

  // Buttons stay active in both orientations; vertical games still read A/B.
  if (ws_key_pressed(CARDPUTER_BTN_A_1)) state |= WS_A;
  if (ws_key_pressed(CARDPUTER_BTN_A_2)) state |= WS_A;
  if (ws_key_pressed(CARDPUTER_BTN_B)) state |= WS_B;

  if (ws_key_pressed(CARDPUTER_BTN_START)) state |= WS_START;  // START 
  if (ws_key_pressed(CARDPUTER_BTN_SELECT))  state |= WS_OPTION; // OPTION 
  
  return state;
}

extern "C" void ws_input_start(void)
{
#ifndef WS_SD_OFF_DURING_GAMEPLAY
  share::setBeforeRestartCallback(ws_save_force_flush);
#endif
  ws_input_tick();
}

extern "C" void ws_input_tick(void)
{
  static bool quitFlushDone = false;
  static bool stateSaveLatch = false;
  static bool stateLoadLatch = false;

  if (!share::shouldPollInput()) {
    return;
  }

  M5Cardputer.update();
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  if (M5Cardputer.BtnA.pressedFor(1000) && !quitFlushDone) {
#ifndef WS_SD_OFF_DURING_GAMEPLAY
    ws_save_force_flush();
#endif
    quitFlushDone = true;
  }

  share::checkCommonInput(status);

  const bool saveStateCombo = status.fn && ws_key_pressed('s');
  const bool loadStateCombo = status.fn && ws_key_pressed('l');
  if (saveStateCombo && !stateSaveLatch) {
    ws_state_request_save();
    stateSaveLatch = true;
  } else if (!saveStateCombo) {
    stateSaveLatch = false;
  }
  if (loadStateCombo && !stateLoadLatch) {
    ws_state_request_load();
    stateLoadLatch = true;
  } else if (!loadStateCombo) {
    stateLoadLatch = false;
  }
  if (saveStateCombo || loadStateCombo) {
    s_cachedState[0] = 0;
    s_cachedState[1] = 0;
    lastPadState = 0;
    return;
  }

  // Zoom / fullscreen toggle
  static bool screenToggleLatch = false;
  const bool screenToggle = ws_key_pressed(CARDPUTER_SCREEN_TOGGLE);
  if (screenToggle && !screenToggleLatch) {
    if (!ws_fullscreen) {
      ws_fullscreen  = true;
      ws_zoomPercent = 100;
    } else {
      ws_zoomPercent += 10;
      if (ws_zoomPercent > 150) {
        ws_zoomPercent = 100;
        ws_fullscreen  = false;
      }
    }
  }
  screenToggleLatch = screenToggle;

  if (status.fn && ws_key_pressed(CARDPUTER_ZOOM_PLUS)) {
    ws_zoomPercent = std::min(150, ws_zoomPercent + 1);
  }

  if (status.fn && ws_key_pressed(CARDPUTER_ZOOM_MINUS)) {
    ws_zoomPercent = std::max(100, ws_zoomPercent - 1);
  }

  const uint32_t i2cPad = share::hasI2cPad() ? share::pollI2cPad() : 0;
  const uint16_t horizontal = ws_input_compute_state(0, i2cPad);
  const uint16_t vertical = ws_input_compute_state(1, i2cPad);
  s_cachedState[0] = horizontal;
  s_cachedState[1] = vertical;
  lastPadState = horizontal;
}

extern "C" void ws_input_stop(void)
{
  s_cachedState[0] = 0;
  s_cachedState[1] = 0;
  share::setBeforeRestartCallback(nullptr);
}

extern "C" int ws_input_poll(int mode)
{
  return (int)s_cachedState[mode ? 1 : 0];
}
