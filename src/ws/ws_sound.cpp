#include "ws_sound.h"
extern "C" {
  #include "oswan/WSApu.h" 
}
#include "compat/arduino_compat.h"
#include <M5Cardputer.h>
#include <string.h>
#include "cardputer/CardputerAudio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef WS_DIRECT_I2S_AUDIO
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#ifndef I2S_PIN_NO_CHANGE
#define I2S_PIN_NO_CHANGE (-1)
#endif
#if __has_include(<driver/i2s_std.h>)
#include "driver/i2s_std.h"
#define WS_DIRECT_I2S_STD 1
#else
#include "driver/i2s.h"
#define WS_DIRECT_I2S_STD 0
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif
#endif
#endif

static constexpr int kNativeSampleRate = 24000;
static int           g_sample_rate = kNativeSampleRate;
#ifndef WS_AUDIO_PERIOD_MS
#define WS_AUDIO_PERIOD_MS 8
#endif
#ifndef WS_AUDIO_DMA_LEN
#define WS_AUDIO_DMA_LEN 320
#endif
#ifndef WS_AUDIO_DMA_COUNT
#define WS_AUDIO_DMA_COUNT 6
#endif
#ifndef WS_AUDIO_POLL_MS
#define WS_AUDIO_POLL_MS 0
#endif
static constexpr int kDefaultPeriodMs = WS_AUDIO_PERIOD_MS;
static int           g_chunk       = 0; 
static constexpr int kChannel      = 0;
static constexpr int kMaxChunk     = 320;
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {};
static uint8_t  s_flip   = 0;
static int16_t  s_lastSample = 0;
static uint8_t  s_volume = 80;
#ifdef WS_DIRECT_I2S_AUDIO
static int16_t* s_directOut = nullptr;
static bool     s_directReady = false;
#if WS_DIRECT_I2S_STD
static i2s_chan_handle_t s_i2sTx = nullptr;
#endif
#endif
#ifdef WS_BENCHMARK_LOGS
static volatile uint32_t s_statBlocks = 0;
static volatile uint32_t s_statUnderflows = 0;
static volatile uint32_t s_statMaxAvailable = 0;
static volatile uint32_t s_statMaxQueueDepth = 0;
static volatile uint32_t s_statMinAvailable = 0xffffffffu;
static volatile uint32_t s_statAvailableSum = 0;
static volatile uint32_t s_statMissingTotal = 0;
static volatile uint32_t s_statMissingMax = 0;
static volatile uint32_t s_statQueueDepth[3] = { 0, 0, 0 };
static volatile uint32_t s_statPostQueueDepth[3] = { 0, 0, 0 };
static volatile uint32_t s_statPlayFails = 0;
static inline void ws_sound_bench_inc(volatile uint32_t& field)
{
  field = (uint32_t)(field + 1u);
}
#define WS_SOUND_BENCH_INC(field)       ws_sound_bench_inc(field)
#define WS_SOUND_BENCH_ADD(field, val)  (field += (uint32_t)(val))
#define WS_SOUND_BENCH_MAX(field, val)  do { uint32_t _v = (uint32_t)(val); if (_v > field) field = _v; } while (0)
#define WS_SOUND_BENCH_MIN(field, val)  do { uint32_t _v = (uint32_t)(val); if (_v < field) field = _v; } while (0)
#else
#define WS_SOUND_BENCH_INC(field)       ((void)0)
#define WS_SOUND_BENCH_ADD(field, val)  ((void)0)
#define WS_SOUND_BENCH_MAX(field, val)  ((void)0)
#define WS_SOUND_BENCH_MIN(field, val)  ((void)0)
#endif

// Task
static TaskHandle_t s_taskAudio  = nullptr;
static volatile bool s_runAudio  = false;
static volatile bool s_pauseAudio = false;
static TickType_t    s_periodTicks = 0;

// -----------------------------------------------------------------------------
// Utils
// -----------------------------------------------------------------------------
static inline int16_t clamp16(int32_t v) {
  if (v >  32767) return  32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static bool buffers_ok() {
  return g_chunk <= kMaxChunk;
}

static void set_chunk_for_period(uint32_t period_ms) {
  if (period_ms == 0) period_ms = kDefaultPeriodMs;
  int chunk = (int)(((uint32_t)g_sample_rate * period_ms + 500u) / 1000u);
  if (chunk < 64) chunk = 64;
  if (chunk > kMaxChunk) chunk = kMaxChunk;
  g_chunk = chunk;
}

static inline void build_block_from_apu(int16_t* dst) {
  int need = g_chunk;
  int have = apuBufLen();
  WS_SOUND_BENCH_INC(s_statBlocks);
  WS_SOUND_BENCH_MIN(s_statMinAvailable, have);
  WS_SOUND_BENCH_ADD(s_statAvailableSum, have);
  WS_SOUND_BENCH_MAX(s_statMaxAvailable, have);
  if (have < need) {
    const uint32_t missing = (uint32_t)(need - have);
    WS_SOUND_BENCH_INC(s_statUnderflows);
    WS_SOUND_BENCH_ADD(s_statMissingTotal, missing);
    WS_SOUND_BENCH_MAX(s_statMissingMax, missing);
  }

  // Si pas assez
  int to_read = (have >= need) ? need : have;

  // Consommation 
  for (int i = 0; i < to_read; ++i) {
    int16_t L = 0;
    int16_t R = 0;
    if (!apuReadStereo(&L, &R)) {
      dst[i] = 0;
      continue;
    }

    int32_t m = ((int32_t)L + (int32_t)R) / 2;
    dst[i] = clamp16(m);
    s_lastSample = dst[i];
  }

  // Pad with a short decaying hold instead of hard silence to soften underruns.
  for (int i = to_read; i < need; ++i) {
    s_lastSample = (int16_t)((int32_t)s_lastSample * 15 / 16);
    dst[i] = s_lastSample;
  }
}

#ifdef WS_DIRECT_I2S_AUDIO
static bool ws_direct_i2s_begin()
{
  if (s_directReady) {
    return true;
  }

  M5Cardputer.Speaker.end();
  vTaskDelay(pdMS_TO_TICKS(2));

#if WS_DIRECT_I2S_STD
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = WS_AUDIO_DMA_COUNT;
  chan_cfg.dma_frame_num = WS_AUDIO_DMA_LEN;
  chan_cfg.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2sTx, nullptr);
  if (err != ESP_OK) {
    s_i2sTx = nullptr;
    return false;
  }

  i2s_std_config_t i2s_config;
  memset(&i2s_config, 0, sizeof(i2s_config));
  i2s_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)g_sample_rate);
  i2s_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_MONO);
  i2s_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  i2s_config.gpio_cfg.bclk = (gpio_num_t)cardputer_audio::kSpeakerBck;
  i2s_config.gpio_cfg.ws = (gpio_num_t)cardputer_audio::kSpeakerWs;
  i2s_config.gpio_cfg.dout = (gpio_num_t)cardputer_audio::kSpeakerData;
  i2s_config.gpio_cfg.din = (gpio_num_t)I2S_PIN_NO_CHANGE;
  i2s_config.gpio_cfg.mclk = (gpio_num_t)I2S_PIN_NO_CHANGE;

  err = i2s_channel_init_std_mode(s_i2sTx, &i2s_config);
  if (err == ESP_OK) {
    err = i2s_channel_enable(s_i2sTx);
  }
  if (err != ESP_OK) {
    i2s_del_channel(s_i2sTx);
    s_i2sTx = nullptr;
    return false;
  }
#else
  i2s_config_t i2s_config;
  memset(&i2s_config, 0, sizeof(i2s_config));
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_config.sample_rate = (int)g_sample_rate;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.tx_desc_auto_clear = true;
  i2s_config.dma_buf_count = WS_AUDIO_DMA_COUNT;
  i2s_config.dma_buf_len = WS_AUDIO_DMA_LEN;

  i2s_driver_uninstall(I2S_NUM_1);
  esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, nullptr);
  if (err != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pin_config;
  memset(&pin_config, ~0u, sizeof(pin_config));
  pin_config.bck_io_num = cardputer_audio::kSpeakerBck;
  pin_config.ws_io_num = cardputer_audio::kSpeakerWs;
  pin_config.data_out_num = cardputer_audio::kSpeakerData;
  pin_config.data_in_num = I2S_PIN_NO_CHANGE;
  err = i2s_set_pin(I2S_NUM_1, &pin_config);
  if (err != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_1);
    return false;
  }
  i2s_start(I2S_NUM_1);
#endif

  s_directReady = true;
  return true;
}

static void ws_direct_i2s_end()
{
#if WS_DIRECT_I2S_STD
  if (s_i2sTx) {
    i2s_channel_disable(s_i2sTx);
    i2s_del_channel(s_i2sTx);
    s_i2sTx = nullptr;
  }
#else
  i2s_stop(I2S_NUM_1);
  i2s_driver_uninstall(I2S_NUM_1);
#endif
  s_directReady = false;
}

static inline bool ws_direct_i2s_write(const int16_t* pcm)
{
  if (!s_directReady || !s_directOut || !pcm) {
    return false;
  }

  s_volume = M5Cardputer.Speaker.getVolume();
  for (int i = 0; i < g_chunk; ++i) {
    int32_t sample = ((int32_t)pcm[i] * (int32_t)s_volume) / 255;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    s_directOut[i] = (int16_t)sample;
  }

  size_t written = 0;
  const size_t bytes = (size_t)g_chunk * sizeof(int16_t);
#if WS_DIRECT_I2S_STD
  esp_err_t err = i2s_channel_write(s_i2sTx, s_directOut, bytes, &written, portMAX_DELAY);
#else
  esp_err_t err = i2s_write(I2S_NUM_1, s_directOut, bytes, &written, portMAX_DELAY);
#endif
  const bool ok = (err == ESP_OK) && (written == bytes);
  if (!ok) WS_SOUND_BENCH_INC(s_statPlayFails);
  return ok;
}
#endif

#ifndef WS_DIRECT_I2S_AUDIO
static inline bool queue_block(const int16_t* pcm) {
  const size_t depth = M5Cardputer.Speaker.isPlaying(kChannel);
  if (depth >= cardputer_audio::kRuntimeAudioQueueDepth) {
    cardputer_audio::recordQueueDiag((size_t)g_chunk,
                                     (uint32_t)g_sample_rate,
                                     false,
                                     kChannel,
                                     depth,
                                     false,
                                     false,
                                     true);
    return false;
  }

  const bool ok = M5Cardputer.Speaker.playRaw(
    pcm,
    (size_t)g_chunk,
    (uint32_t)g_sample_rate,
    false,     // mono
    1,         // play once
    kChannel,
    false // dont stop current sound
  );
  cardputer_audio::recordQueueDiag((size_t)g_chunk,
                                   (uint32_t)g_sample_rate,
                                   false,
                                   kChannel,
                                   depth,
                                   ok,
                                   false,
                                   false);
  if (!ok) WS_SOUND_BENCH_INC(s_statPlayFails);
#ifdef WS_BENCHMARK_LOGS
  size_t queued = M5Cardputer.Speaker.isPlaying(kChannel);
  WS_SOUND_BENCH_INC(s_statPostQueueDepth[queued < 2 ? queued : 2]);
#endif
  return ok;
}
#endif

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------
extern "C" void ws_sound_init(int sample_rate_hz) {
  g_sample_rate = sample_rate_hz > 0 ? sample_rate_hz : kNativeSampleRate;
  set_chunk_for_period(kDefaultPeriodMs);

  if (!buffers_ok()) {
    g_chunk = kMaxChunk;
  }

  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
  if (!cardputer_audio::allocRuntimeAudioBuffers(s_buf, kMaxChunk, "ws")) {
    return;
  }

#ifdef WS_DIRECT_I2S_AUDIO
  if (!s_directOut) {
    s_directOut = static_cast<int16_t*>(
      heap_caps_malloc(kMaxChunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  }
  if (!s_directOut || !ws_direct_i2s_begin()) {
    return;
  }
#else
  cardputer_audio::beginSpeaker(g_sample_rate, false, WS_AUDIO_DMA_LEN, WS_AUDIO_DMA_COUNT, 80, "ws", 4, 0);
#endif
  s_flip = 0;
  s_lastSample = 0;
#ifdef WS_BENCHMARK_LOGS
  s_statBlocks = 0;
  s_statUnderflows = 0;
  s_statMaxAvailable = 0;
  s_statMaxQueueDepth = 0;
  s_statMinAvailable = 0xffffffffu;
  s_statAvailableSum = 0;
  s_statMissingTotal = 0;
  s_statMissingMax = 0;
  s_statQueueDepth[0] = 0;
  s_statQueueDepth[1] = 0;
  s_statQueueDepth[2] = 0;
  s_statPostQueueDepth[0] = 0;
  s_statPostQueueDepth[1] = 0;
  s_statPostQueueDepth[2] = 0;
  s_statPlayFails = 0;
#endif
}

extern "C" void ws_sound_set_volume(uint8_t vol) {
  s_volume = vol;
#ifndef WS_DIRECT_I2S_AUDIO
  M5Cardputer.Speaker.setVolume(vol);
#endif
}

extern "C" void ws_sound_shutdown(void) {
  ws_sound_stop_task();
#ifdef WS_DIRECT_I2S_AUDIO
  ws_direct_i2s_end();
  if (s_directOut) {
    heap_caps_free(s_directOut);
    s_directOut = nullptr;
  }
#else
  M5Cardputer.Speaker.stop(kChannel);
#endif
  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
}

extern "C" void ws_sound_frame(void) {
  if (!s_buf[0] || !s_buf[1] || !s_buf[2] || !s_buf[3]) return;

#ifdef WS_DIRECT_I2S_AUDIO
  if (!s_directReady && !ws_direct_i2s_begin()) {
    WS_SOUND_BENCH_INC(s_statPlayFails);
    return;
  }
  WS_SOUND_BENCH_MAX(s_statMaxQueueDepth, cardputer_audio::kRuntimeAudioBufferCount);
#ifdef WS_BENCHMARK_LOGS
  WS_SOUND_BENCH_INC(s_statQueueDepth[2]);
#endif
  build_block_from_apu(s_buf[s_flip]);
  if (ws_direct_i2s_write(s_buf[s_flip])) {
    s_flip = (uint8_t)((s_flip + 1) % cardputer_audio::kRuntimeAudioBufferCount);
  } else {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
#else
  size_t queued = M5Cardputer.Speaker.isPlaying(kChannel);
  WS_SOUND_BENCH_MAX(s_statMaxQueueDepth, queued);
#ifdef WS_BENCHMARK_LOGS
  WS_SOUND_BENCH_INC(s_statQueueDepth[queued < 2 ? queued : 2]);
#endif

  if (queued == 0) {
    // Prime up to the shared runtime queue depth.
    while (queued < cardputer_audio::kRuntimeAudioQueueDepth) {
      build_block_from_apu(s_buf[s_flip]);
      if (queue_block(s_buf[s_flip])) {
        s_flip = (uint8_t)((s_flip + 1) % cardputer_audio::kRuntimeAudioBufferCount);
        ++queued;
      } else {
        break;
      }
    }
  } else if (queued < cardputer_audio::kRuntimeAudioQueueDepth) {
    build_block_from_apu(s_buf[s_flip]);
    if (queue_block(s_buf[s_flip])) {
      s_flip = (uint8_t)((s_flip + 1) % cardputer_audio::kRuntimeAudioBufferCount);
    }
  } else {
    // queue already at target depth
  }
#endif
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------
static void ws_audio_task(void* arg) {
  (void)arg;
#ifdef WS_DIRECT_I2S_AUDIO
  while (s_runAudio) {
    if (s_pauseAudio) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    ws_sound_frame();
  }
#else
  TickType_t last = xTaskGetTickCount();

  while (s_runAudio) {
    if (!s_pauseAudio) {
      ws_sound_frame();
    }
    vTaskDelayUntil(&last, s_periodTicks);
  }
#endif
  s_taskAudio = nullptr;
  vTaskDelete(nullptr);
}

extern "C" void ws_sound_start_task(uint32_t period_ms, int core) {
  if (s_taskAudio) return;

  if (period_ms == 0) period_ms = kDefaultPeriodMs;
  set_chunk_for_period(period_ms);
  uint32_t poll_ms = WS_AUDIO_POLL_MS ? WS_AUDIO_POLL_MS : period_ms;
  if (poll_ms == 0) poll_ms = 1;
  s_periodTicks = pdMS_TO_TICKS(poll_ms);
  if (s_periodTicks == 0) s_periodTicks = 1;

  s_runAudio = true;
  xTaskCreatePinnedToCore(ws_audio_task, "ws_audio", 2048, nullptr, 6, &s_taskAudio, core);
}

extern "C" void ws_sound_stop_task(void) {
  if (!s_taskAudio) return;
  s_runAudio = false;
  for (int i = 0; i < 20 && s_taskAudio; ++i) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  s_taskAudio = nullptr;
}

extern "C" void ws_sound_pause_task(int pause) {
  s_pauseAudio = pause != 0;
#ifndef WS_DIRECT_I2S_AUDIO
  if (s_pauseAudio) {
    M5Cardputer.Speaker.stop(kChannel);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
#endif
}

#ifdef WS_BENCHMARK_LOGS
extern "C" void ws_sound_get_and_reset_stats(uint32_t* blocks,
                                              uint32_t* underflows,
                                              uint32_t* max_available,
                                              uint32_t* max_queue_depth,
                                              uint32_t* min_available,
                                              uint32_t* avg_available,
                                              uint32_t* missing_total,
                                              uint32_t* missing_max,
                                              uint32_t* queue0,
                                              uint32_t* queue1,
                                              uint32_t* queue2,
                                              uint32_t* post_queue0,
                                              uint32_t* post_queue1,
                                              uint32_t* post_queue2,
                                              uint32_t* play_fails) {
  const uint32_t blockCount = s_statBlocks;
  if (blocks) *blocks = blockCount;
  if (underflows) *underflows = s_statUnderflows;
  if (max_available) *max_available = s_statMaxAvailable;
  if (max_queue_depth) *max_queue_depth = s_statMaxQueueDepth;
  if (min_available) *min_available = (s_statMinAvailable == 0xffffffffu) ? 0 : s_statMinAvailable;
  if (avg_available) *avg_available = blockCount ? (s_statAvailableSum / blockCount) : 0;
  if (missing_total) *missing_total = s_statMissingTotal;
  if (missing_max) *missing_max = s_statMissingMax;
  if (queue0) *queue0 = s_statQueueDepth[0];
  if (queue1) *queue1 = s_statQueueDepth[1];
  if (queue2) *queue2 = s_statQueueDepth[2];
  if (post_queue0) *post_queue0 = s_statPostQueueDepth[0];
  if (post_queue1) *post_queue1 = s_statPostQueueDepth[1];
  if (post_queue2) *post_queue2 = s_statPostQueueDepth[2];
  if (play_fails) *play_fails = s_statPlayFails;
  s_statBlocks = 0;
  s_statUnderflows = 0;
  s_statMaxAvailable = 0;
  s_statMaxQueueDepth = 0;
  s_statMinAvailable = 0xffffffffu;
  s_statAvailableSum = 0;
  s_statMissingTotal = 0;
  s_statMissingMax = 0;
  s_statQueueDepth[0] = 0;
  s_statQueueDepth[1] = 0;
  s_statQueueDepth[2] = 0;
  s_statPostQueueDepth[0] = 0;
  s_statPostQueueDepth[1] = 0;
  s_statPostQueueDepth[2] = 0;
  s_statPlayFails = 0;
}
#endif
