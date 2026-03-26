#include "run_nes.h"

#include <M5Cardputer.h>
#include <stdio.h>
#include <string>

#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "share/display_target.h"
#include "share/emu_controls.h"

// Defined in nes_display.cpp
void nes_display_show_external_info(const char* romTitle);

void run_nes(const char* xipPath)
{
  CardputerView display;
  display.initialize();

  const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);

  if (useExternal) {
    // Game on external TFT → show control bindings on internal LCD
    display.topBar("NES ON EXTERNAL TFT", false, false);
    display.showControlBindings(
      share::emuControlActionLabels(share::EmuProfile::Nes),
      share::emuControlKeyLabels(share::EmuProfile::Nes),
      "GO / HOLD ESC = QUIT"
    );
  } else {
    // Game on internal LCD → show ROM info + controls on external TFT
    // Extract ROM filename from xipPath (e.g. "/xip/Super_Mario.nes")
    std::string path(xipPath ? xipPath : "");
    auto pos = path.find_last_of("/\\");
    std::string romName = (pos == std::string::npos) ? path : path.substr(pos + 1);
    if (!emu_is_aux_screen_locked()) {
      nes_display_show_external_info(romName.c_str());
    }
  }

  // Call the NES core
  // Note: the core expects argv to be the ROM path
  char* argv_[1] = { const_cast<char*>(xipPath) };
  nofrendo_main(1, argv_);
}
