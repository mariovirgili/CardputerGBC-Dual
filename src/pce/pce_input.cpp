#include "pce_input.h"

#include <M5Cardputer.h>
#include "share/emu_controls.h"
#include "share/input.h"

extern "C" {
  #include "pce-go/pce.h" 
}

extern bool pceFullScreen;
extern int  pceZoomLevel;

void pce_input_read(uint8_t joypads[8])
{
  for (int i = 0; i < 8; ++i) {
    joypads[i] = 0;
  }

  uint32_t buttons = 0;

  M5Cardputer.update();
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  share::checkCommonInput(status);

  // I2C PAD (M5Stack JoyV2)
  if (share::hasI2cPad()) {
      int i2cPad = share::pollI2cPad();
      if (i2cPad & share::PAD_LEFT)  buttons |= JOY_LEFT;
      if (i2cPad & share::PAD_RIGHT) buttons |= JOY_RIGHT;
      if (i2cPad & share::PAD_UP)    buttons |= JOY_UP;
      if (i2cPad & share::PAD_DOWN)  buttons |= JOY_DOWN;
      if (i2cPad & share::PAD_A)     buttons |= JOY_A;
  }

  // Toggle / fullscreen
  if (M5Cardputer.Keyboard.isChange() &&
      M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {

    if (!pceFullScreen) {
      pceFullScreen  = true;
      pceZoomLevel  = 100;
    } else {
      pceZoomLevel += 10;
      if (pceZoomLevel > 150) {
        pceZoomLevel = 100;
        pceFullScreen = false;
      }
    }
    joypads[0] = 0;
    return;
  }

  // Zoom in
  if (status.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_PLUS)) {
    if (!pceFullScreen) pceFullScreen = true;
    pceZoomLevel += 1;
    if (pceZoomLevel > 150) pceZoomLevel = 150;
    joypads[0] = 0;
    return;
  }

  // Zoom out 
  if (status.fn && M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_ZOOM_MINUS)) {
    if (!pceFullScreen) pceFullScreen = true;
    pceZoomLevel -= 1;
    if (pceZoomLevel < 100) pceZoomLevel = 100;
    joypads[0] = 0;
    return;
  }

  // Gauche
  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Left)) {
    buttons |= JOY_LEFT;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Right)) {
    buttons |= JOY_RIGHT;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Up)) {
    buttons |= JOY_UP;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Down)) {
    buttons |= JOY_DOWN;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Select)) {
    buttons |= JOY_SELECT;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::Start)) {
    buttons |= JOY_RUN;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::A)) {
    buttons |= JOY_A;
  }

  if (share::emuControlPressed(share::EmuProfile::Pce, share::EmuAction::B)) {
    buttons |= JOY_B;
  }

  joypads[0] = (uint8_t)buttons;
}

// ================== HOOKS PCE-GO ==================

extern "C" void osd_input_read(uint8_t joypads[8])
{
  pce_input_read(joypads);
}
