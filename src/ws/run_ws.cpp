extern "C" {
  #include "oswan/WS.h"
  #include "oswan/WSRender.h"
  #include "oswan/WSFileio.h"
  #include "oswan/cpu/necintrf.h"
}

#include <M5Cardputer.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/task.h"
#ifdef WS_BENCHMARK_LOGS
#include "esp_heap_caps.h"
#endif
#include "ws_display.h"
#include "ws_input.h"
#include "ws_sound.h"
#include "ws_save.h"
#include "ws_state.h"
#include "share/emu_log_cpp.h"
#include "share/input.h"
#include "share/sd_control.h"
#include "share/sd_gameplay_guard.h"

#include <stdlib.h>

#ifdef WS_LOGS_ENABLED
#define WS_LOG(...) EMU_LOG(__VA_ARGS__)
#else
#define WS_LOG(...) ((void)0)
#endif

#ifndef WS_AUDIO_PERIOD_MS
#define WS_AUDIO_PERIOD_MS 8
#endif
#ifndef WS_AUDIO_TASK_CORE
#define WS_AUDIO_TASK_CORE 0
#endif

static void ws_reset_quit_controls()
{
  share::setRestartRequestMode(false);
  share::clearRestartRequest();
  share::clearBeforeRestartCallback();
}

static void ws_enter_sd_off_gameplay()
{
#ifdef WS_SD_OFF_DURING_GAMEPLAY
  share::clearRestartRequest();
  share::setRestartRequestMode(true);
  share::clearBeforeRestartCallback();
  ws_save_suspend_background();

  if (ws_save_uses_sd_backing()) {
    WS_LOG("[WS][SD] keep SD mounted: SRAM backing active\n");
    return;
  }

  share_sd_gameplay_close("WS");
#else
  share::setBeforeRestartCallback(ws_save_force_flush);
#endif
}

static void ws_load_sram_with_sd()
{
  if (!ws_save_has_sram()) {
    ws_save_load();
    return;
  }

  const bool was_mounted = share_sd_is_mounted();
  if (!was_mounted && !share_sd_gameplay_mount("WS", "load")) {
    WS_LOG("[WS][SAVE] SD remount failed, SRAM load skipped\n");
    return;
  }

  ws_save_load();

#ifdef WS_SD_OFF_DURING_GAMEPLAY
  if (!was_mounted && !ws_save_uses_sd_backing()) {
    share_sd_gameplay_close_if_mounted("WS", "load");
  }
#endif
}

static void ws_clean_teardown_and_save()
{
  WS_LOG("[WS][QUIT] clean teardown start\n");
  ws_save_suspend_background();

  uint8_t* sramSnapshot = nullptr;
  size_t sramSnapshotSize = 0;
  const bool hadSram = ws_save_has_sram();
  const bool sdBacked = ws_save_uses_sd_backing();
  const bool haveSnapshot = hadSram && !sdBacked &&
      ws_save_snapshot_sram(&sramSnapshot, &sramSnapshotSize);

  if (hadSram && !sdBacked && !haveSnapshot) {
    WS_LOG("[WS][SAVE] SRAM snapshot failed, using live-buffer fallback\n");
  }

  ws_input_stop();
  ws_display_stop();
  ws_sound_shutdown();

  if (hadSram) {
    if (share_sd_gameplay_mount("WS", "save")) {
      if (haveSnapshot) {
        ws_save_force_flush_buffer(sramSnapshot, sramSnapshotSize);
      } else {
        ws_save_force_flush();
      }
    } else {
      WS_LOG("[WS][SAVE] SD remount failed, SRAM not saved\n");
    }
  } else {
    WS_LOG("[WS][SD] no SRAM, skip SD remount/save\n");
  }

  if (sramSnapshot) {
    free(sramSnapshot);
  }

  if (sdBacked) {
    WsSramBackingClose();
  }

#ifdef WS_SD_OFF_DURING_GAMEPLAY
  share_sd_gameplay_close_if_mounted("WS", "save");
#endif

  ws_save_shutdown();
  ws_reset_quit_controls();
  WS_LOG("[WS][QUIT] clean teardown done\n");
}

static void ws_update_adaptive_frameskip(uint32_t core_us, uint32_t frame_us)
{
  static uint32_t samples = 0;
  static uint64_t total_us = 0;
  static uint32_t over_budget = 0;

  samples++;
  total_us += core_us;
  if (core_us > frame_us) over_budget++;

  if (samples < 30) return;

  const uint32_t avg_us = (uint32_t)(total_us / samples);
  const int oldSkip = FrameSkip;

  if ((avg_us > (frame_us * 105u) / 100u || over_budget > samples / 3) && FrameSkip < 4) {
    FrameSkip++;
  } else if (avg_us < (frame_us * 70u) / 100u && over_budget == 0 && FrameSkip > 0) {
    FrameSkip--;
  }

#ifdef WS_BENCHMARK_LOGS
  if (FrameSkip != oldSkip) {
    EMU_LOG("[WS][BENCH] adaptive frameskip %d -> %d (avg %.2f ms, overBudget %lu/%lu)\n",
            oldSkip, FrameSkip, (float)avg_us / 1000.0f,
            (unsigned long)over_budget, (unsigned long)samples);
  }
#endif

  samples = 0;
  total_us = 0;
  over_budget = 0;
}

extern "C" void run_ws(const uint8_t* rom, size_t len, const char* rom_name, bool is_color)
{
  WS_LOG("[WS] ===== WonderSwan Start =====\n");
  WS_LOG("[WS] ROM size: %u bytes, color mode: %s\n",
         (unsigned)len, is_color ? "COLOR" : "MONO");

  // LCD
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setSwapBytes(true);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  // Core/Display/Sound init
  ws_display_init();
  ws_display_start();
  ws_sound_init(24000);
  WsInit(); // splash screen
  WS_LOG("[WS] Init done\n");
  
  // Load ROM
  if (WsCreateFromMemory(rom, len) != 0) {
    WS_LOG("[WS][ERR] Cart load failed! len=%u\n, SRAM could be too big", (unsigned)len);
    for(;;) delay(1000);
  }
  WS_LOG("[WS] Cart loaded successfully\n");

  // SRAM save/load
  ws_save_init(rom_name);
  ws_load_sram_with_sd();
  ws_state_init(rom_name);
  ws_enter_sd_off_gameplay();
  ws_sound_start_task(WS_AUDIO_PERIOD_MS, WS_AUDIO_TASK_CORE);
  ws_input_start();
#ifdef WS_BENCHMARK_LOGS
  uint32_t displayPrio = 0, displayCore = 0;
  uint32_t audioPrio = 0, audioCore = 0;
  ws_display_get_task_info(&displayPrio, &displayCore);
  ws_sound_get_task_info(&audioPrio, &audioCore);
  EMU_LOG("[WS][TASK] main core=%d prio=%lu display core=%lu prio=%lu audio core=%lu prio=%lu\n",
          xPortGetCoreID(),
          (unsigned long)uxTaskPriorityGet(nullptr),
          (unsigned long)displayCore,
          (unsigned long)displayPrio,
          (unsigned long)audioCore,
          (unsigned long)audioPrio);
#endif

  // Timing
  const uint32_t frame_us = 1000000u / 75u; // 13.3 ms
  uint64_t next = esp_timer_get_time();
  WS_LOG("[WS] Frame pacing: %uus/frame\n", (unsigned)frame_us);
#if defined(WS_LOGS_ENABLED) || defined(WS_BENCHMARK_LOGS)
  uint32_t frameCount = 0;
  uint32_t lastLog = millis();
#endif
#ifdef WS_BENCHMARK_LOGS
  uint64_t benchCoreTotalUs = 0;
  uint32_t benchCoreMaxUs = 0;
  uint32_t benchLateFrames = 0;
  uint32_t benchMaxLateUs = 0;
  uint64_t benchIdleDelayUs = 0;
  uint64_t benchIdleSpinUs = 0;
  uint32_t benchLastLargestInternal = 0;
#endif

  for (;;) {
    // Run one frame
    ws_input_tick();
    if (share::restartRequested()) {
      break;
    }
    int64_t tRun0 = esp_timer_get_time();
    WsRun();
    uint32_t coreUs = (uint32_t)(esp_timer_get_time() - tRun0);
    ws_update_adaptive_frameskip(coreUs, frame_us);
#ifdef WS_BENCHMARK_LOGS
    benchCoreTotalUs += coreUs;
    if (coreUs > benchCoreMaxUs) benchCoreMaxUs = coreUs;
#endif
    ws_state_tick();
    ws_save_tick();
#if defined(WS_LOGS_ENABLED) || defined(WS_BENCHMARK_LOGS)
    frameCount++;

    // Log framerate every 2 seconds
    uint32_t now = millis();
    if (now - lastLog >= 2000) {
      WS_LOG("[WS] %lu frames rendered (%.2f FPS)\n",
             (unsigned long)frameCount,
             (float)frameCount / ((now - lastLog) / 1000.0f));
#ifdef WS_BENCHMARK_LOGS
      WsCoreStats coreStats;
      WsGetAndResetStats(&coreStats);

      uint32_t dispFrames = 0, dispTotalUs = 0, dispMaxUs = 0, dispPending = 0;
      ws_display_get_and_reset_stats(&dispFrames, &dispTotalUs, &dispMaxUs, &dispPending);

      uint32_t audioBlocks = 0, audioUnderflows = 0, audioMaxAvailable = 0, audioMaxQueue = 0;
      uint32_t audioMinAvailable = 0, audioAvgAvailable = 0, audioMissingTotal = 0, audioMissingMax = 0;
      uint32_t audioQueue0 = 0, audioQueue1 = 0, audioQueue2 = 0;
      uint32_t audioPostQueue0 = 0, audioPostQueue1 = 0, audioPostQueue2 = 0, audioPlayFails = 0;
      uint32_t audioDirectMode = 0, audioChunkSamples = 0, audioDmaLen = 0, audioDmaCount = 0;
      uint32_t audioWriteCalls = 0, audioShortWrites = 0, audioWriteWaitAvgUs = 0, audioWriteWaitMaxUs = 0;
      uint32_t audioPushGapAvgUs = 0, audioPushGapMaxUs = 0, audioLatePushes = 0;
      ws_sound_get_and_reset_stats(&audioBlocks, &audioUnderflows, &audioMaxAvailable, &audioMaxQueue,
                                   &audioMinAvailable, &audioAvgAvailable,
                                   &audioMissingTotal, &audioMissingMax,
                                   &audioQueue0, &audioQueue1, &audioQueue2,
                                   &audioPostQueue0, &audioPostQueue1, &audioPostQueue2,
                                   &audioPlayFails);
      ws_sound_get_and_reset_direct_stats(&audioDirectMode, &audioChunkSamples,
                                          &audioDmaLen, &audioDmaCount,
                                          &audioWriteCalls, &audioShortWrites,
                                          &audioWriteWaitAvgUs, &audioWriteWaitMaxUs,
                                          &audioPushGapAvgUs, &audioPushGapMaxUs,
                                          &audioLatePushes);

      const float coreAvgMs = frameCount ? (float)benchCoreTotalUs / (float)frameCount / 1000.0f : 0.0f;
      const float dispAvgMs = dispFrames ? (float)dispTotalUs / (float)dispFrames / 1000.0f : 0.0f;
      EMU_LOG("[WS][BENCH] core avg/max %.2f/%.2f ms, late %lu maxLate %.2f ms, idle delay/spin %.1f/%.1f ms\n",
              coreAvgMs, (float)benchCoreMaxUs / 1000.0f,
              (unsigned long)benchLateFrames, (float)benchMaxLateUs / 1000.0f,
              (double)benchIdleDelayUs / 1000.0, (double)benchIdleSpinUs / 1000.0);
      EMU_LOG("[WS][BENCH] core frames=%u steps=%u lines=%u paints=%u fs=%d apu=%u gdma=%u/%uB\n",
              coreStats.frames, coreStats.cpuSteps, coreStats.refreshLines,
              coreStats.paintRequests, coreStats.frameSkip, coreStats.apuTicks,
              coreStats.gdmaTransfers, coreStats.gdmaBytes);
#ifdef WS_MODEL_LOGS
      const uint8_t colctl = IO ? IO[0x60] : 0;
      EMU_LOG("[WS][BENCH] model colctl=%02X mode=%s color16=%u packed=%u\n",
              colctl, (colctl & 0x80) ? "WSC" : "WS",
              (unsigned)((colctl >> 6) & 1u),
              (unsigned)((colctl >> 5) & 1u));
#endif
#if defined(WS_RENDER_PROFILE)
      if (coreStats.renderLines) {
        const double renderLines = (double)coreStats.renderLines;
        EMU_LOG("[WS][RND] lines=%u avg_us clear/bg/fg/swin/sscan/sdraw=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f max_us=%u/%u/%u/%u/%u/%u\n",
                coreStats.renderLines,
                (double)coreStats.renderClearUs / renderLines,
                (double)coreStats.renderBgUs / renderLines,
                (double)coreStats.renderFgUs / renderLines,
                (double)coreStats.renderSpriteWindowUs / renderLines,
                (double)coreStats.renderSpriteScanUs / renderLines,
                (double)coreStats.renderSpriteDrawUs / renderLines,
                coreStats.renderClearMaxUs,
                coreStats.renderBgMaxUs,
                coreStats.renderFgMaxUs,
                coreStats.renderSpriteWindowMaxUs,
                coreStats.renderSpriteScanMaxUs,
                coreStats.renderSpriteDrawMaxUs);
        EMU_LOG("[WS][RND] decode calls bg/fg/spr=%u/%u/%u\n",
                coreStats.renderBgDecodeCalls,
                coreStats.renderFgDecodeCalls,
                coreStats.renderSpriteDecodeCalls);
      }
#endif
      EMU_LOG("[WS][BENCH] irq key=%u htm=%u vtm=%u vblank=%u line=%u\n",
              coreStats.keyIrqs, coreStats.htimerIrqs, coreStats.vtimerIrqs,
              coreStats.vblankIrqs, coreStats.lineIrqs);
      if (coreStats.sramBankSwitches) {
        EMU_LOG("[WS][BENCH] sram bank switches=%u\n", coreStats.sramBankSwitches);
      }
#if defined(WS_CPU_PROFILE) || defined(WS_CPU_BRANCH_PROFILE)
      nec_profile_log_and_reset();
#endif
      if (coreStats.spritePixels || coreStats.spriteLimitedLines || coreStats.spriteClipLeft || coreStats.spriteClipRight) {
        EMU_LOG("[WS][SPR] base=%04X first=%u cnt=%u cached=%u wrap=%u px=%u vis=%u/%u limit=%u clip=%u/%u skip=%u/%u/%u dsp=%02X\n",
                coreStats.spriteTableBase, coreStats.spriteFirst, coreStats.spriteCountReg,
                coreStats.spriteCached, coreStats.spriteWrapped,
                coreStats.spritePixels, coreStats.spriteVisible, coreStats.spriteCandidates,
                coreStats.spriteLimitedLines, coreStats.spriteClipLeft, coreStats.spriteClipRight,
                coreStats.spriteWindowSkips, coreStats.spritePrioritySkips,
                coreStats.spriteTransparentSkips, IO[0x00]);
      }
      EMU_LOG("[WS][BENCH] display frames=%lu avg/max %.2f/%.2f ms pending=%lu\n",
              (unsigned long)dispFrames, dispAvgMs, (float)dispMaxUs / 1000.0f,
              (unsigned long)dispPending);
      EMU_LOG("[WS][BENCH] audio blocks=%lu underflows=%lu maxAvail=%lu maxQueue=%lu\n",
              (unsigned long)audioBlocks, (unsigned long)audioUnderflows,
              (unsigned long)audioMaxAvailable, (unsigned long)audioMaxQueue);
      EMU_LOG("[WS][AUD] avail min/avg/max=%lu/%lu/%lu missing total/max=%lu/%lu q0/q1/q2=%lu/%lu/%lu post=%lu/%lu/%lu playFail=%lu\n",
              (unsigned long)audioMinAvailable, (unsigned long)audioAvgAvailable,
              (unsigned long)audioMaxAvailable,
              (unsigned long)audioMissingTotal, (unsigned long)audioMissingMax,
              (unsigned long)audioQueue0, (unsigned long)audioQueue1,
              (unsigned long)audioQueue2,
              (unsigned long)audioPostQueue0, (unsigned long)audioPostQueue1,
              (unsigned long)audioPostQueue2, (unsigned long)audioPlayFails);
      EMU_LOG("[WS][AUDX] mode=%s chunk=%lu dma=%lux%lu writes=%lu short=%lu wait avg/max=%.3f/%.3f ms push avg/max=%.3f/%.3f ms late=%lu\n",
              audioDirectMode ? "direct" : "m5",
              (unsigned long)audioChunkSamples,
              (unsigned long)audioDmaLen,
              (unsigned long)audioDmaCount,
              (unsigned long)audioWriteCalls,
              (unsigned long)audioShortWrites,
              (double)audioWriteWaitAvgUs / 1000.0,
              (double)audioWriteWaitMaxUs / 1000.0,
              (double)audioPushGapAvgUs / 1000.0,
              (double)audioPushGapMaxUs / 1000.0,
              (unsigned long)audioLatePushes);
      const uint32_t heapFree = esp_get_free_heap_size();
      const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      EMU_LOG("[WS][BENCH] heap free=%lu largest8=%lu largestInternal=%lu minFree=%lu\n",
              (unsigned long)heapFree,
              (unsigned long)largest8,
              (unsigned long)largestInternal,
              (unsigned long)minFree);
      if (benchLastLargestInternal && largestInternal + 16384u < benchLastLargestInternal) {
        EMU_LOG("[WS][BENCH][HEAP] largestInternal drop %lu -> %lu bytes\n",
                (unsigned long)benchLastLargestInternal,
                (unsigned long)largestInternal);
      }
      benchLastLargestInternal = largestInternal;
      benchCoreTotalUs = 0;
      benchCoreMaxUs = 0;
      benchLateFrames = 0;
      benchMaxLateUs = 0;
      benchIdleDelayUs = 0;
      benchIdleSpinUs = 0;
#else
      WS_LOG("[WS] HEAP: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
#endif
      frameCount = 0;
      lastLog = now;
    }
#endif

    // Frame pacing (75Hz)
    next += frame_us;
    int64_t remain = (int64_t)next - (int64_t)esp_timer_get_time();
#ifdef WS_BENCHMARK_LOGS
    if (remain < 0) {
      uint32_t lateUs = (uint32_t)(-remain);
      benchLateFrames++;
      if (lateUs > benchMaxLateUs) benchMaxLateUs = lateUs;
    } else if (remain > 2000) {
      benchIdleDelayUs += (uint32_t)remain;
    } else {
      benchIdleSpinUs += (uint32_t)remain;
    }
#endif
    if (remain > 2000) vTaskDelay(remain / 1000 / portTICK_PERIOD_MS);
    else if (remain > 0) ets_delay_us((uint32_t)remain);
    else next = esp_timer_get_time();
  }

#ifdef WS_SD_OFF_DURING_GAMEPLAY
  ws_clean_teardown_and_save();
  esp_restart();
#endif
}

