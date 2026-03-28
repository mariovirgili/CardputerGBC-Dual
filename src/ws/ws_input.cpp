#include "ws_input.h"
#include <M5Cardputer.h>
#include <stdint.h>
#include "ws_input.h"
#include "share/emu_controls.h"
#include "share/input.h"

extern bool ws_fullscreen;
extern int  ws_zoomPercent;
extern uint32_t lastPadState;

extern "C" int ws_input_poll(int mode)
{
  (void)mode;
  M5Cardputer.update();
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
  uint16_t state = 0;

  share::checkCommonInput(status);

  // I2C PAD (M5Stack JoyV2)
  if (share::hasI2cPad()) {
      int i2cPad = share::pollI2cPad();
      if (i2cPad & share::PAD_LEFT)  state |= WS_X4; 
      if (i2cPad & share::PAD_RIGHT) state |= WS_X2; 
      if (i2cPad & share::PAD_UP)    state |= WS_X1; 
      if (i2cPad & share::PAD_DOWN)  state |= WS_X3;
      if (i2cPad & share::PAD_A)     state |= WS_A;
  }

  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::X1)) state |= WS_X1;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::X2)) state |= WS_X2;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::X3)) state |= WS_X3;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::X4)) state |= WS_X4;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Y1)) state |= WS_Y1;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Y2)) state |= WS_Y2;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Y3)) state |= WS_Y3;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Y4)) state |= WS_Y4;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::A)) state |= WS_A;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::B)) state |= WS_B;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Start)) state |= WS_START;
  if (share::emuControlPressed(share::EmuProfile::Ws, share::EmuAction::Option)) state |= WS_OPTION;

  // Zoom / fullscreen toggle
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
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
    return (int)state;
  }

  if (status.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
    ws_zoomPercent = std::min(150, ws_zoomPercent + 1);
    return (int)state;
  }

  if (status.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
    ws_zoomPercent = std::max(100, ws_zoomPercent - 1);
    return (int)state;
  }
  
  lastPadState = state;
  return (int)state;
}
