#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "compat/arduino_compat.h"
#include <M5Cardputer.h>
#include "cardputer/CardputerAudio.h"
#include "genesis_sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
  void YM2612Init(void);
  void YM2612ResetChip(void);
  void YM2612Config(unsigned char dac_bits);
  void ym2612_run(int target);
  void gwenesis_ym2612_tables_alloc(void);
}

// Globals
int      ym2612_index  = 0;
int      ym2612_clock  = 0;
int16_t* gwenesis_ym2612_buffer = nullptr;

int sn76489_index = 0;
int sn76489_clock = 0;
int16_t* gwenesis_sn76489_buffer = nullptr;

// Task state
TaskHandle_t  s_ymTaskHandle      = nullptr;
volatile int  s_ym_target_clock   = 0;
volatile int g_ym_target_clock = 0;
static TaskHandle_t  s_audioTask = nullptr;
static QueueHandle_t s_audioQ    = nullptr;
static int16_t* s_audioPool = nullptr;  // AUDIO_POOL * AUDIO_CHUNK
static volatile int s_poolWrite = 0;
static volatile int s_poolRead  = 0;
static StaticQueue_t s_ymQueueStruct;
static uint8_t*      s_ymQueueStorage = nullptr;    // 128 * sizeof(YmWrite)
static QueueHandle_t s_ymQ = nullptr;

// Cardputer speaker runtime buffers. M5Unified keeps raw pointers until played.
static uint8_t s_flip = 0;
static bool s_primed = false;
static portMUX_TYPE g_ymMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = { nullptr, nullptr, nullptr };
uint8_t genesis_audio_volume = 50;

// Audio config
static constexpr int kSampleRate = 53267;    // Hz
static constexpr int kFps        = 60;      // frames per second
static constexpr int kChunk      = (kSampleRate + kFps/2) / kFps;
static constexpr uint8_t kChannel= 0;
static uint16_t s_psgGainQ15 = 32768;  // x1.0
static uint16_t s_fmGainQ15  = 32768;  // x1.0

/* Allocate SN76489, YM2612 buffers and audio pool */
void genesis_alloc_audio_buffers(void) {
  // FM / PSG
  gwenesis_sn76489_buffer = (int16_t*) heap_caps_calloc(
    1024, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  gwenesis_ym2612_buffer  = (int16_t*) heap_caps_calloc(
    2048, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

  // Pool audio
  s_audioPool = (int16_t*) heap_caps_calloc(
      (size_t)AUDIO_POOL * (size_t)AUDIO_CHUNK, sizeof(int16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  cardputer_audio::allocRuntimeAudioBuffers(s_buf, AUDIO_CHUNK, "genesis");

  // Storage queue YM
  s_ymQueueStorage = (uint8_t*) heap_caps_calloc(
      128, sizeof(YmWrite), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_ymQ = xQueueCreateStatic(128, sizeof(YmWrite), s_ymQueueStorage, &s_ymQueueStruct);

  // Tables YM + reset index/clocks
  gwenesis_ym2612_tables_alloc();
  sn76489_index = sn76489_clock = 0;
  ym2612_index  = ym2612_clock  = 0;
}

/* Push mixed audio to cardputer speaker */
static void audio_task(void*) {
  vTaskDelete(nullptr);
}

/* Initialize cardputer speaker and audio task */
void genesis_sound_init() {
  sn76489_index = sn76489_clock = 0;
  ym2612_index  = ym2612_clock  = 0;

  cardputer_audio::beginSpeaker(AUDIO_SR, AUDIO_STEREO, 512, 8, genesis_audio_volume, "genesis", 4, 0);
  M5Cardputer.Speaker.setVolume(genesis_audio_volume);

  s_flip = 0;
}

/* Submit a frame of audio to the cardputer speaker */
void genesis_sound_submit_frame(void) {
  // Snapshot des index 
  int ym_n, psg_n;
  taskENTER_CRITICAL(&g_ymMux);
  ym_n  = ym2612_index;
  psg_n = sn76489_index;
  taskEXIT_CRITICAL(&g_ymMux);

  int n = ym_n > psg_n ? ym_n : psg_n;
  if (n <= 0) return;
  if (n > AUDIO_CHUNK) n = AUDIO_CHUNK;

  if (M5Cardputer.Speaker.isPlaying(kChannel) >= 2 || !s_buf[0] || !s_buf[1] || !s_buf[2]) {
    taskENTER_CRITICAL(&g_ymMux);
    ym2612_index  = 0;
    sn76489_index = 0;
    taskEXIT_CRITICAL(&g_ymMux);
    return;
  }

  // Mix dans current buffer
  int16_t *dst = s_buf[s_flip];
  for (int i = 0; i < n; ++i) {
    int32_t s = 0;
    if (i < ym_n)  s += (int32_t)gwenesis_ym2612_buffer[i];
    if (i < psg_n) s += (int32_t)gwenesis_sn76489_buffer[i];
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    dst[i] = (int16_t)s;
  }
  for (int i = n; i < AUDIO_CHUNK; ++i) dst[i] = 0;

  // Reset des index
  taskENTER_CRITICAL(&g_ymMux);
  ym2612_index  = 0;
  sn76489_index = 0;
  taskEXIT_CRITICAL(&g_ymMux);

  cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, AUDIO_CHUNK, AUDIO_SR, AUDIO_STEREO, kChannel);
}


// ==== YM2612  ====


/* Allocate YM2612 buffer */
static void ym_alloc_buffer_once() {
  if (!gwenesis_ym2612_buffer) {
    gwenesis_ym2612_buffer = (int16_t*) heap_caps_calloc(
        1024, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  ym2612_index = 0;
  ym2612_clock = 0;
}

/* Initialize YM2612 chip */
extern "C" void genesis_sound_ym_init(void) {
  ym_alloc_buffer_once();
  YM2612Init();
  YM2612ResetChip();
  YM2612Config(9);
}

/* Task for YM2612 audio processing */
static void ym_task(void*){
  for(;;){
    int target = s_ym_target_clock;      // snapshot
    ym2612_run(target);                  // avance core
    vTaskDelay(1);
  }
}

/* Start the YM2612 audio processing task */
extern "C" void genesis_sound_ym_start(void) {
  if (s_ymTaskHandle) return; // déjà démarré
  BaseType_t ok = xTaskCreatePinnedToCore(
    ym_task, "YMTask",
    1024, nullptr, 6, &s_ymTaskHandle,
    0 // core
  );
  if (ok != pdPASS) {
    s_ymTaskHandle = nullptr;
  }
}

/* Stop the YM2612 audio processing task */
extern "C" void genesis_sound_ym_stop(void) {
  if (s_ymTaskHandle) {
    vTaskDelete(s_ymTaskHandle);
    s_ymTaskHandle = nullptr;
  }
}

/* Set the target clock for YM2612 */
extern "C" void genesis_sound_ym_set_target_clock(int target) {
  s_ym_target_clock = target;
}

/* Get the target clock for YM2612 */
extern "C" int  genesis_sound_ym_get_target_clock(void) {
  return s_ym_target_clock;
}
