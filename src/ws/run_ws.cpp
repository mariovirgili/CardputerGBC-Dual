extern "C" {
  #include "oswan/WS.h"
  #include "oswan/WSRender.h"
  #include "oswan/WSFileio.h"
}

#include <M5Cardputer.h>
#include "esp_timer.h"
#include "ws_display.h"
#include "ws_profiler.h"
#include "ws_sound.h"
#include "ws_save.h"
#include "../share/display_target.h"

extern "C" void run_ws(const uint8_t* rom, size_t len, const char* rom_name, bool is_color)
{
  printf("[WS] ===== WonderSwan Start =====\n");
  printf("[WS] ROM size: %u bytes, color mode: %s\n",
         (unsigned)len, is_color ? "COLOR" : "MONO");

  const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  ws_profiler_begin_run(useExternal ? 1 : 0, xPortGetCoreID());

  if (!useExternal) {
    // Game on internal LCD
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  }

  // Core/Display/Sound init
  ws_display_init();
  ws_display_start();
  ws_sound_init(48000);
  ws_sound_start_task(16, 0);
  WsInit(); // splash screen
  printf("[WS] Init done\n");

  // Load ROM
  if (WsCreateFromMemory(rom, len) != 0) {
    printf("[WS][ERR] Cart load failed! len=%u\n, SRAM could be too big", (unsigned)len);
    for(;;) delay(1000);
  }
  printf("[WS] Cart loaded successfully\n");

  // Dual-screen: show info on the screen not used for game
  if (!useExternal) {
    // Game on internal LCD -> show ROM info + controls on external TFT
    if (!emu_is_aux_screen_locked()) {
      ws_display_show_external_info(rom_name, is_color);
    }
  }

  // SRAM save/load
  ws_save_init(rom_name);
  ws_save_load();

  // Timing
  const uint32_t frame_us = 1000000u / 75u; // 13.3 ms
  uint64_t next = esp_timer_get_time();
  printf("[WS] Frame pacing: %uus/frame\n", frame_us);
  uint32_t frameCount = 0;
  uint32_t lastLog = millis();

  for (;;) {
    // Run one frame
    WsRun();
    const uint64_t saveStart = esp_timer_get_time();
    ws_save_tick();
    ws_profiler_add_save_us((uint32_t)(esp_timer_get_time() - saveStart));
    frameCount++;

    // Frame pacing (75Hz)
    next += frame_us;
    const int64_t now_us = (int64_t)esp_timer_get_time();
    int64_t remain = (int64_t)next - now_us;
    if (remain > 2000) {
      ws_profiler_add_sleep_us((uint32_t)remain);
      vTaskDelay(remain / 1000 / portTICK_PERIOD_MS);
    } else if (remain > 0) {
      ws_profiler_add_sleep_us((uint32_t)remain);
      ets_delay_us((uint32_t)remain);
    } else {
      ws_profiler_note_frame_late();
      ws_profiler_note_main_yield();
      taskYIELD();
      next = esp_timer_get_time();
    }

    ws_profiler_log_if_due();
  }
}
