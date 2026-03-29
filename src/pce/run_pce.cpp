extern "C" {
  #include "pce-go/pce.h"
  #include "pce-go/psg.h"
  void* PalettePCE(int brightness);
}

#include <M5Cardputer.h>
#include "esp_timer.h"
#include "pce_display.h"
#include "pce_sound.h"
#include "pce_input.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../cardputer/CardputerView.h"

// ================== SAVE STATE (NYI) ==================

extern "C" bool pce_save_state_to_file(const char *path)
{
  // todo
  return SaveState(path) == 0;
}

extern "C" bool pce_load_state_from_file(const char *path)
{
  if (LoadState(path) != 0) {
    ResetPCE(false);
    return false;
  }
  return true;
}

// ================== RUN PCE ==================

void run_pce(const uint8_t* rom, size_t len, const char* rom_name)
{
  printf("[PCE] ===== PC Engine Start =====\n");
  printf("[PCE] ROM size: %u bytes (%s)\n", (unsigned)len, rom_name);
  const int sampleRate = 22050;

  const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);

  if (!useExternal) {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  }

  pce_display_init();
  pce_display_start();

  // Initialize PCE core BEFORE starting audio (psg_update needs PSG init)
  InitPCE(sampleRate, false);
  printf("[PCE] Core initialized\n");

  if (LoadCard((uint8_t*)rom, len) != 0) {
    printf("[PCE][ERR] ROM loading failed (len=%u)\n", (unsigned)len);
    for (;;) delay(1000);
  }
  printf("[PCE] ROM loaded successfully\n");

  // Start audio AFTER InitPCE so PSG is ready for psg_update()
  pce_sound_init(sampleRate);

  if (useExternal) {
    // Game on external TFT → show control bindings on internal LCD
    CardputerView intDisplay;
    if (!emu_is_internal_screen_locked()) {
      intDisplay.initialize();
      intDisplay.topBar("PCE ON EXTERNAL TFT", false, false);
      intDisplay.showControlBindings(
        share::emuControlActionLabels(share::EmuProfile::Pce),
        share::emuControlKeyLabels(share::EmuProfile::Pce),
        "GO / HOLD ESC = QUIT"
      );
      emu_set_internal_screen_locked(true);
    }
  } else {
    // Game on internal LCD → show ROM info + controls on external TFT
    if (!emu_is_aux_screen_locked()) {
      pce_display_show_external_info(rom_name);
    }
  }

  printf("[PCE] Entering RunPCE() loop | display => %s\n",
         useExternal ? "external" : "internal");
  RunPCE();
}
