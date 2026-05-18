
#pragma GCC optimize ("Os")

#include "pce_sound.h"
#include <M5Cardputer.h>
#include "cardputer/CardputerAudio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
  #include "pce-go/psg.h"
#include "share/emu_log_cpp.h"
}

static constexpr int kChannel = 0;
static constexpr size_t kNumFrames = 62;
static constexpr size_t kNumSamples = kNumFrames;
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {};
static uint8_t  s_flip   = 0;
static TaskHandle_t s_audioTaskHandle = nullptr;
static volatile bool s_paused  = false;
static volatile bool s_running = false;
static int s_sampleRate        = 22050;

// ---------- Task audio ----------

static void pce_audio_task(void* arg)
{
  EMU_LOG("[PCE][AUDIO] task started. frames=%d, samples=%d, rate=%d\n",
         (int)kNumFrames, (int)kNumSamples, s_sampleRate);

  while (s_running) {
    // Pause
    while (s_paused && s_running) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!s_running) break;

    // too much sound queued
    while (s_running &&
           M5Cardputer.Speaker.isPlaying(kChannel) >= cardputer_audio::kRuntimeAudioQueueDepth) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!s_running) break;

    int16_t* out = s_buf[s_flip];
    psg_update(out, (int)kNumFrames, 0xFF);

    cardputer_audio::queueRuntimeAudioBuffer(
      s_buf, s_flip, kNumSamples, (uint32_t)s_sampleRate, false, kChannel);
  }

  EMU_LOG("[PCE][AUDIO] task exit\n");
  vTaskDelete(nullptr);
}

// ---------- Public API ----------

extern "C" bool pce_sound_init(int sample_rate)
{
  if (s_audioTaskHandle) {
    return true;
  }

  s_sampleRate = sample_rate > 0 ? sample_rate : s_sampleRate;

  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
  if (!cardputer_audio::allocRuntimeAudioBuffers(s_buf, kNumSamples, "pce")) {
    EMU_LOG("[PCE][AUDIO] buffer alloc failed\n");
    return false;
  }

  cardputer_audio::beginSpeaker(s_sampleRate, false, 512, 8, 80, "pce", 6, 0);

  M5Cardputer.Speaker.stop(kChannel);

  s_flip    = 0;
  s_paused  = false;
  s_running = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
    pce_audio_task,
    "pce_sound",
    3192,
    nullptr,
    6,              // prio
    &s_audioTaskHandle,
    0               // core 0
  );

  if (ok != pdPASS) {
    EMU_LOG("[PCE][AUDIO] xTaskCreate failed\n");
    s_audioTaskHandle = nullptr;
    s_running = false;
    return false;
  }

  return true;
}

extern "C" void pce_sound_set_paused(bool paused)
{
  s_paused = paused;
  if (paused) {
    M5Cardputer.Speaker.stop(kChannel);
  }
}

extern "C" void pce_sound_deinit(void)
{
  if (!s_audioTaskHandle) return;

  s_running = false;
  s_audioTaskHandle = nullptr;

  M5Cardputer.Speaker.stop(kChannel);

  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
}
