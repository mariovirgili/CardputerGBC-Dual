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

#ifdef MD_DIRECT_I2S_AUDIO
#include "driver/gpio.h"
#ifndef I2S_PIN_NO_CHANGE
#define I2S_PIN_NO_CHANGE (-1)
#endif
#if __has_include(<driver/i2s_std.h>)
#include "driver/i2s_std.h"
#define MD_DIRECT_I2S_STD 1
#else
#include "driver/i2s.h"
#define MD_DIRECT_I2S_STD 0
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif
#endif
#endif

extern "C" {
  void YM2612Init(void);
  void YM2612ResetChip(void);
  void YM2612Config(unsigned char dac_bits);
  void YM2612SetDivisor(int divisor);
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
static QueueHandle_t s_audioFreeQ = nullptr;
static int16_t* s_audioPool = nullptr;  // AUDIO_POOL * AUDIO_CHUNK
static StaticQueue_t s_ymQueueStruct;
static uint8_t*      s_ymQueueStorage = nullptr;    // 128 * sizeof(YmWrite)
static QueueHandle_t s_ymQ = nullptr;

// Cardputer speaker runtime buffers. M5Unified keeps raw pointers until played.
static uint8_t s_flip = 0;
static bool s_primed = false;
static portMUX_TYPE g_ymMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {};
uint8_t genesis_audio_volume = 50;
static int s_audioRefreshRate = GWENESIS_REFRESH_RATE_NTSC;
static int s_audioCoreRate = GWENESIS_AUDIO_FREQ_NTSC;
static int s_audioCoreDivisor = GWENESIS_AUDIO_DIVISOR_NTSC;
static int s_audioCoreSamples = GWENESIS_AUDIO_BUFFER_LENGTH_NTSC;
static int s_audioOutSamples = AUDIO_CHUNK_NTSC;

void genesis_sound_configure_timing(int refresh_rate, int core_sample_rate, int core_divisor, int lines_per_frame)
{
  if (refresh_rate != GWENESIS_REFRESH_RATE_PAL) {
    refresh_rate = GWENESIS_REFRESH_RATE_NTSC;
  }
  if (core_sample_rate <= 0) {
    core_sample_rate = (refresh_rate == GWENESIS_REFRESH_RATE_PAL) ? GWENESIS_AUDIO_FREQ_PAL : GWENESIS_AUDIO_FREQ_NTSC;
  }
  if (core_divisor <= 0) {
    core_divisor = (refresh_rate == GWENESIS_REFRESH_RATE_PAL) ? GWENESIS_AUDIO_DIVISOR_PAL : GWENESIS_AUDIO_DIVISOR_NTSC;
  }
  if (lines_per_frame <= 0) {
    lines_per_frame = (refresh_rate == GWENESIS_REFRESH_RATE_PAL) ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
  }

  s_audioRefreshRate = refresh_rate;
  s_audioCoreRate = core_sample_rate;
  s_audioCoreDivisor = core_divisor;
  s_audioCoreSamples = ((lines_per_frame * VDP_CYCLES_PER_LINE) + core_divisor - 1) / core_divisor;
  if (s_audioCoreSamples > AUDIO_CORE_CHUNK_CAP) {
    s_audioCoreSamples = AUDIO_CORE_CHUNK_CAP;
  }
  s_audioOutSamples = (AUDIO_SR + refresh_rate / 2) / refresh_rate;
  if (s_audioOutSamples > AUDIO_CHUNK_CAP) {
    s_audioOutSamples = AUDIO_CHUNK_CAP;
  }
}

int genesis_sound_get_refresh_rate(void) { return s_audioRefreshRate; }
int genesis_sound_get_core_rate(void) { return s_audioCoreRate; }
int genesis_sound_get_core_samples_per_frame(void) { return s_audioCoreSamples; }
int genesis_sound_get_output_samples_per_frame(void) { return s_audioOutSamples; }

// Audio config
static constexpr uint8_t kChannel= 0;
static uint16_t s_psgGainQ15 = 32768;  // x1.0
static uint16_t s_fmGainQ15  = 32768;  // x1.0

#ifdef MD_DIRECT_I2S_AUDIO
#ifndef MD_AUDIO_TASK_CORE
#define MD_AUDIO_TASK_CORE 0
#endif
#ifndef MD_AUDIO_TASK_PRIORITY
#define MD_AUDIO_TASK_PRIORITY 6
#endif
#ifndef MD_AUDIO_DMA_LEN
#define MD_AUDIO_DMA_LEN 256
#endif
#ifndef MD_AUDIO_DMA_COUNT
#define MD_AUDIO_DMA_COUNT 8
#endif

static int16_t* s_directOut = nullptr;
static bool s_directReady = false;
#if MD_DIRECT_I2S_STD
static i2s_chan_handle_t s_i2sTx = nullptr;
#endif
#endif

#if MD_AUDIO_LOGS_ENABLED
struct MdAudioDiagStats {
  uint64_t lastLogMs = 0;
  uint32_t frames = 0;
  uint32_t queued = 0;
  uint32_t droppedDepth = 0;
  uint32_t droppedBuffer = 0;
  uint32_t clippedSamples = 0;
  uint32_t depthMax = 0;
  uint64_t depthTotal = 0;
  uint32_t emptyDepth = 0;
  uint32_t coreMin = UINT32_MAX;
  uint32_t coreMax = 0;
  uint64_t coreTotal = 0;
  uint32_t ymMin = UINT32_MAX;
  uint32_t ymMax = 0;
  uint64_t ymTotal = 0;
  uint32_t psgMin = UINT32_MAX;
  uint32_t psgMax = 0;
  uint64_t psgTotal = 0;
  int targetMin = INT32_MAX;
  int targetMax = INT32_MIN;
  int targetLast = 0;
  uint64_t targetTotal = 0;
  uint32_t targetCount = 0;
  uint32_t i2sWriteCalls = 0;
  uint32_t i2sWriteOk = 0;
  uint32_t i2sWriteShort = 0;
  uint32_t i2sWriteFail = 0;
  uint32_t i2sWriteUsMin = UINT32_MAX;
  uint32_t i2sWriteUsMax = 0;
  uint64_t i2sWriteUsTotal = 0;
  uint32_t writerBacklogMax = 0;
  uint64_t writerBacklogTotal = 0;
  uint32_t writerBacklogCount = 0;
};

static MdAudioDiagStats s_mdAudioDiag;
static portMUX_TYPE s_mdAudioDiagMux = portMUX_INITIALIZER_UNLOCKED;

static inline uint64_t md_audio_now_ms()
{
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline void md_audio_diag_reset()
{
  taskENTER_CRITICAL(&s_mdAudioDiagMux);
  s_mdAudioDiag = MdAudioDiagStats{};
  s_mdAudioDiag.lastLogMs = md_audio_now_ms();
  taskEXIT_CRITICAL(&s_mdAudioDiagMux);
}

static inline void md_audio_diag_samples(int core_n, int ym_n, int psg_n, size_t depth)
{
  taskENTER_CRITICAL(&s_mdAudioDiagMux);
  MdAudioDiagStats& d = s_mdAudioDiag;
  ++d.frames;
  if (depth == 0) ++d.emptyDepth;
  if (depth > d.depthMax) d.depthMax = (uint32_t)depth;
  d.depthTotal += (uint32_t)depth;

  const uint32_t cn = core_n > 0 ? (uint32_t)core_n : 0;
  const uint32_t yn = ym_n > 0 ? (uint32_t)ym_n : 0;
  const uint32_t pn = psg_n > 0 ? (uint32_t)psg_n : 0;

  if (cn < d.coreMin) d.coreMin = cn;
  if (cn > d.coreMax) d.coreMax = cn;
  d.coreTotal += cn;
  if (yn < d.ymMin) d.ymMin = yn;
  if (yn > d.ymMax) d.ymMax = yn;
  d.ymTotal += yn;
  if (pn < d.psgMin) d.psgMin = pn;
  if (pn > d.psgMax) d.psgMax = pn;
  d.psgTotal += pn;
  taskEXIT_CRITICAL(&s_mdAudioDiagMux);
}

static inline void md_audio_diag_target(int target)
{
  taskENTER_CRITICAL(&s_mdAudioDiagMux);
  MdAudioDiagStats& d = s_mdAudioDiag;
  d.targetLast = target;
  if (target < d.targetMin) d.targetMin = target;
  if (target > d.targetMax) d.targetMax = target;
  d.targetTotal += (uint32_t)(target > 0 ? target : 0);
  ++d.targetCount;
  taskEXIT_CRITICAL(&s_mdAudioDiagMux);
}

static inline void md_audio_diag_direct_write(uint32_t writeUs,
                                              bool ok,
                                              bool shortWrite,
                                              size_t backlogAfterReceive)
{
  taskENTER_CRITICAL(&s_mdAudioDiagMux);
  MdAudioDiagStats& d = s_mdAudioDiag;
  ++d.i2sWriteCalls;
  if (ok) {
    ++d.i2sWriteOk;
  } else if (shortWrite) {
    ++d.i2sWriteShort;
  } else {
    ++d.i2sWriteFail;
  }
  if (writeUs < d.i2sWriteUsMin) d.i2sWriteUsMin = writeUs;
  if (writeUs > d.i2sWriteUsMax) d.i2sWriteUsMax = writeUs;
  d.i2sWriteUsTotal += writeUs;
  if (backlogAfterReceive > d.writerBacklogMax) {
    d.writerBacklogMax = (uint32_t)backlogAfterReceive;
  }
  d.writerBacklogTotal += (uint32_t)backlogAfterReceive;
  ++d.writerBacklogCount;
  taskEXIT_CRITICAL(&s_mdAudioDiagMux);
}

static inline void md_audio_diag_log_if_due(bool force = false)
{
  const uint64_t now = md_audio_now_ms();
  MdAudioDiagStats snap;
  bool shouldLog = false;

  taskENTER_CRITICAL(&s_mdAudioDiagMux);
  if (!force && (now - s_mdAudioDiag.lastLogMs) < 1000ULL) {
    taskEXIT_CRITICAL(&s_mdAudioDiagMux);
    return;
  }
  if (s_mdAudioDiag.frames == 0 &&
      s_mdAudioDiag.targetCount == 0 &&
      s_mdAudioDiag.i2sWriteCalls == 0) {
    s_mdAudioDiag.lastLogMs = now;
    taskEXIT_CRITICAL(&s_mdAudioDiagMux);
    return;
  }
  snap = s_mdAudioDiag;
  s_mdAudioDiag = MdAudioDiagStats{};
  s_mdAudioDiag.lastLogMs = now;
  shouldLog = true;
  taskEXIT_CRITICAL(&s_mdAudioDiagMux);

  if (!shouldLog) {
    return;
  }

  const uint32_t coreMin = (snap.coreMin == UINT32_MAX) ? 0 : snap.coreMin;
  const uint32_t ymMin = (snap.ymMin == UINT32_MAX) ? 0 : snap.ymMin;
  const uint32_t psgMin = (snap.psgMin == UINT32_MAX) ? 0 : snap.psgMin;
  const uint32_t coreAvg = snap.frames ? (uint32_t)(snap.coreTotal / snap.frames) : 0;
  const uint32_t ymAvg = snap.frames ? (uint32_t)(snap.ymTotal / snap.frames) : 0;
  const uint32_t psgAvg = snap.frames ? (uint32_t)(snap.psgTotal / snap.frames) : 0;
  const uint32_t depthAvg = snap.frames ? (uint32_t)(snap.depthTotal / snap.frames) : 0;
  const int targetMin = (snap.targetMin == INT32_MAX) ? 0 : snap.targetMin;
  const int targetMax = (snap.targetMax == INT32_MIN) ? 0 : snap.targetMax;
  const uint32_t targetAvg = snap.targetCount ? (uint32_t)(snap.targetTotal / snap.targetCount) : 0;
  const uint32_t i2sWriteMin = (snap.i2sWriteUsMin == UINT32_MAX) ? 0 : snap.i2sWriteUsMin;
  const uint32_t i2sWriteAvg = snap.i2sWriteCalls ? (uint32_t)(snap.i2sWriteUsTotal / snap.i2sWriteCalls) : 0;
  const uint32_t writerBacklogAvg = snap.writerBacklogCount ? (uint32_t)(snap.writerBacklogTotal / snap.writerBacklogCount) : 0;

  MD_AUDIO_LOG("frames=%lu queued=%lu loss depth/buf=%lu/%lu queueDepth avg/max=%lu/%lu underrunEmpty=%lu coreRate=%d outRate=%d coreSamples min/avg/max=%lu/%lu/%lu outSamples=%d ym=%lu/%lu/%lu psg=%lu/%lu/%lu target min/avg/max/last=%d/%lu/%d/%d writerBacklog avg/max=%lu/%lu i2sWrite calls/ok/short/fail=%lu/%lu/%lu/%lu us min/avg/max=%lu/%lu/%lu clipped=%lu",
               (unsigned long)snap.frames,
               (unsigned long)snap.queued,
               (unsigned long)snap.droppedDepth,
               (unsigned long)snap.droppedBuffer,
               (unsigned long)depthAvg,
               (unsigned long)snap.depthMax,
               (unsigned long)snap.emptyDepth,
               s_audioCoreRate,
               AUDIO_SR,
               (unsigned long)coreMin,
               (unsigned long)coreAvg,
               (unsigned long)snap.coreMax,
               s_audioOutSamples,
               (unsigned long)ymMin,
               (unsigned long)ymAvg,
               (unsigned long)snap.ymMax,
               (unsigned long)psgMin,
               (unsigned long)psgAvg,
               (unsigned long)snap.psgMax,
               targetMin,
               (unsigned long)targetAvg,
               targetMax,
               snap.targetLast,
               (unsigned long)writerBacklogAvg,
               (unsigned long)snap.writerBacklogMax,
               (unsigned long)snap.i2sWriteCalls,
               (unsigned long)snap.i2sWriteOk,
               (unsigned long)snap.i2sWriteShort,
               (unsigned long)snap.i2sWriteFail,
               (unsigned long)i2sWriteMin,
               (unsigned long)i2sWriteAvg,
               (unsigned long)snap.i2sWriteUsMax,
               (unsigned long)snap.clippedSamples);
}
#else
static inline void md_audio_diag_reset() {}
static inline void md_audio_diag_samples(int, int, int, size_t) {}
static inline void md_audio_diag_target(int) {}
static inline void md_audio_diag_direct_write(uint32_t, bool, bool, size_t) {}
static inline void md_audio_diag_log_if_due(bool = false) {}
#endif

static inline int16_t mix_sample_at(int idx, int ym_n, int psg_n) {
  int32_t s = 0;
  if (idx < ym_n)  s += (int32_t)gwenesis_ym2612_buffer[idx];
  if (idx < psg_n) s += (int32_t)gwenesis_sn76489_buffer[idx];
  if (s >  32767) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.clippedSamples;
#endif
    s =  32767;
  }
  if (s < -32768) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.clippedSamples;
#endif
    s = -32768;
  }
  return (int16_t)s;
}

#ifdef MD_DIRECT_I2S_AUDIO
static bool md_direct_i2s_begin()
{
  if (s_directReady) {
    return true;
  }

  M5Cardputer.Speaker.end();
  vTaskDelay(pdMS_TO_TICKS(2));

#if MD_DIRECT_I2S_STD
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = MD_AUDIO_DMA_COUNT;
  chan_cfg.dma_frame_num = MD_AUDIO_DMA_LEN;
  chan_cfg.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2sTx, nullptr);
  if (err != ESP_OK) {
    s_i2sTx = nullptr;
    return false;
  }

  i2s_std_config_t i2s_config;
  memset(&i2s_config, 0, sizeof(i2s_config));
  i2s_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)AUDIO_SR);
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
  i2s_config.sample_rate = AUDIO_SR;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.tx_desc_auto_clear = true;
  i2s_config.dma_buf_count = MD_AUDIO_DMA_COUNT;
  i2s_config.dma_buf_len = MD_AUDIO_DMA_LEN;

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

static void md_direct_i2s_end()
{
#if MD_DIRECT_I2S_STD
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

static inline int md_audio_pool_slot_from_ptr(const int16_t* pcm)
{
  if (!s_audioPool || !pcm) {
    return -1;
  }
  ptrdiff_t samples = pcm - s_audioPool;
  if (samples < 0) {
    return -1;
  }
  const ptrdiff_t slot = samples / AUDIO_CHUNK_CAP;
  if (slot < 0 || slot >= AUDIO_POOL) {
    return -1;
  }
  return (int)slot;
}

struct MdDirectWriteResult {
  bool ok = false;
  bool shortWrite = false;
  uint32_t writeUs = 0;
};

static MdDirectWriteResult md_direct_i2s_write(const int16_t* pcm, size_t samples)
{
  MdDirectWriteResult result;
  if (!s_directReady || !s_directOut || !pcm || samples == 0 || samples > (size_t)AUDIO_CHUNK_CAP) {
    return result;
  }

  const int volume = (int)genesis_audio_volume;
  for (size_t i = 0; i < samples; ++i) {
    int32_t sample = ((int32_t)pcm[i] * volume) / 255;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    s_directOut[i] = (int16_t)sample;
  }

  size_t written = 0;
  const size_t bytes = samples * sizeof(int16_t);
  const uint64_t t0 = (uint64_t)esp_timer_get_time();
#if MD_DIRECT_I2S_STD
  const esp_err_t err = i2s_channel_write(s_i2sTx, s_directOut, bytes, &written, portMAX_DELAY);
#else
  const esp_err_t err = i2s_write(I2S_NUM_1, s_directOut, bytes, &written, portMAX_DELAY);
#endif
  result.writeUs = (uint32_t)((uint64_t)esp_timer_get_time() - t0);
  result.ok = (err == ESP_OK) && (written == bytes);
  result.shortWrite = (written != bytes);
  return result;
}
#endif

/* Allocate SN76489, YM2612 buffers and audio pool */
void genesis_alloc_audio_buffers(void) {
  // FM / PSG
  gwenesis_sn76489_buffer = (int16_t*) heap_caps_calloc(
    AUDIO_CORE_CHUNK_CAP, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  gwenesis_ym2612_buffer  = (int16_t*) heap_caps_calloc(
    2048, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

  // Pool audio
  s_audioPool = (int16_t*) heap_caps_calloc(
      (size_t)AUDIO_POOL * (size_t)AUDIO_CHUNK_CAP, sizeof(int16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

#ifndef MD_DIRECT_I2S_AUDIO
  cardputer_audio::allocRuntimeAudioBuffers(s_buf, AUDIO_CHUNK_CAP, "genesis");
#endif

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
#ifdef MD_DIRECT_I2S_AUDIO
  AudioMsg msg = {};
  for (;;) {
    if (xQueueReceive(s_audioQ, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (!msg.buf || msg.n == 0) {
      break;
    }

    const size_t backlogAfterReceive = s_audioQ ? (size_t)uxQueueMessagesWaiting(s_audioQ) : 0;
    const MdDirectWriteResult writeResult = md_direct_i2s_write(msg.buf, msg.n);
    md_audio_diag_direct_write(writeResult.writeUs,
                               writeResult.ok,
                               writeResult.shortWrite,
                               backlogAfterReceive);

    const int slot = md_audio_pool_slot_from_ptr(msg.buf);
    if (slot >= 0 && s_audioFreeQ) {
      xQueueSend(s_audioFreeQ, &slot, 0);
    }
  }
  s_audioTask = nullptr;
#endif
  vTaskDelete(nullptr);
}

/* Initialize cardputer speaker and audio task */
void genesis_sound_init() {
  sn76489_index = sn76489_clock = 0;
  ym2612_index  = ym2612_clock  = 0;

#ifdef MD_DIRECT_I2S_AUDIO
  if (!s_audioQ) {
    s_audioQ = xQueueCreate(AUDIO_Q_DEPTH, sizeof(AudioMsg));
  }
  if (!s_audioFreeQ) {
    s_audioFreeQ = xQueueCreate(AUDIO_POOL, sizeof(int));
  }
  if (s_audioQ) {
    xQueueReset(s_audioQ);
  }
  if (s_audioFreeQ) {
    xQueueReset(s_audioFreeQ);
    for (int i = 0; i < AUDIO_POOL; ++i) {
      xQueueSend(s_audioFreeQ, &i, 0);
    }
  }
  if (!s_directOut) {
    s_directOut = static_cast<int16_t*>(
        heap_caps_malloc((size_t)AUDIO_CHUNK_CAP * sizeof(int16_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  }
  if (!s_audioTask && s_audioQ && s_audioFreeQ && s_directOut && md_direct_i2s_begin()) {
    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "MDAudioTask",
        2048, nullptr, MD_AUDIO_TASK_PRIORITY, &s_audioTask,
        MD_AUDIO_TASK_CORE);
    if (ok != pdPASS) {
      s_audioTask = nullptr;
    }
  }
#else
  cardputer_audio::beginSpeaker(AUDIO_SR, AUDIO_STEREO, 512, 8, genesis_audio_volume, "genesis", 4, 0);
  M5Cardputer.Speaker.setVolume(genesis_audio_volume);
#endif

  s_flip = 0;
  md_audio_diag_reset();
}

void genesis_sound_shutdown()
{
#ifdef MD_DIRECT_I2S_AUDIO
  if (s_audioTask && s_audioQ) {
    AudioMsg stop = {};
    if (xQueueSend(s_audioQ, &stop, 0) != pdTRUE) {
      xQueueReset(s_audioQ);
      xQueueSend(s_audioQ, &stop, 0);
    }
    for (int i = 0; i < 20 && s_audioTask; ++i) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (s_audioTask) {
      vTaskDelete(s_audioTask);
      s_audioTask = nullptr;
    }
  }
  md_direct_i2s_end();
  if (s_audioQ) {
    vQueueDelete(s_audioQ);
    s_audioQ = nullptr;
  }
  if (s_audioFreeQ) {
    vQueueDelete(s_audioFreeQ);
    s_audioFreeQ = nullptr;
  }
  if (s_directOut) {
    heap_caps_free(s_directOut);
    s_directOut = nullptr;
  }
#endif
  M5Cardputer.Speaker.stop(kChannel);
  M5Cardputer.Speaker.end();
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
  if (n > s_audioCoreSamples) n = s_audioCoreSamples;
  if (ym_n > s_audioCoreSamples) ym_n = s_audioCoreSamples;
  if (psg_n > s_audioCoreSamples) psg_n = s_audioCoreSamples;

#ifdef MD_DIRECT_I2S_AUDIO
  const size_t depth = s_audioQ ? (size_t)uxQueueMessagesWaiting(s_audioQ) : 0;
#else
  const size_t depth = M5Cardputer.Speaker.isPlaying(kChannel);
#endif
  md_audio_diag_samples(n, ym_n, psg_n, depth);

#ifdef MD_DIRECT_I2S_AUDIO
  int poolSlot = -1;
  if (!s_audioTask || !s_audioQ || !s_audioFreeQ || !s_audioPool ||
      xQueueReceive(s_audioFreeQ, &poolSlot, 0) != pdTRUE) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.droppedBuffer;
    md_audio_diag_log_if_due();
#endif
    taskENTER_CRITICAL(&g_ymMux);
    ym2612_index  = 0;
    sn76489_index = 0;
    taskEXIT_CRITICAL(&g_ymMux);
    return;
  }
  int16_t *dst = s_audioPool + ((size_t)poolSlot * (size_t)AUDIO_CHUNK_CAP);
#else
  if (depth >= cardputer_audio::kRuntimeAudioQueueDepth ||
      !s_buf[0] || !s_buf[1] || !s_buf[2] || !s_buf[3]) {
#if MD_AUDIO_LOGS_ENABLED
    if (depth >= cardputer_audio::kRuntimeAudioQueueDepth) {
      ++s_mdAudioDiag.droppedDepth;
    } else {
      ++s_mdAudioDiag.droppedBuffer;
    }
    md_audio_diag_log_if_due();
#endif
    taskENTER_CRITICAL(&g_ymMux);
    ym2612_index  = 0;
    sn76489_index = 0;
    taskEXIT_CRITICAL(&g_ymMux);
    return;
  }

  // Mix at the core's native MD rate, then resample the frame to the I2S rate.
  int16_t *dst = s_buf[s_flip];
#endif
  if (n == 1) {
    int16_t sample = mix_sample_at(0, ym_n, psg_n);
    for (int i = 0; i < s_audioOutSamples; ++i) dst[i] = sample;
  } else {
    const uint32_t step = (uint32_t)(((uint64_t)(n - 1) << 16) / (s_audioOutSamples - 1));
    uint32_t pos = 0;
    for (int i = 0; i < s_audioOutSamples; ++i) {
      int idx = (int)(pos >> 16);
      uint32_t frac = pos & 0xFFFFu;
      int32_t a = mix_sample_at(idx, ym_n, psg_n);
      int32_t b = mix_sample_at((idx + 1 < n) ? idx + 1 : idx, ym_n, psg_n);
      dst[i] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)frac) >> 16));
      pos += step;
    }
  }

  // Reset des index
  taskENTER_CRITICAL(&g_ymMux);
  ym2612_index  = 0;
  sn76489_index = 0;
  taskEXIT_CRITICAL(&g_ymMux);

#ifdef MD_DIRECT_I2S_AUDIO
  AudioMsg msg = { dst, (size_t)s_audioOutSamples };
  if (xQueueSend(s_audioQ, &msg, 0) == pdTRUE) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.queued;
#endif
  } else {
    xQueueSend(s_audioFreeQ, &poolSlot, 0);
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.droppedDepth;
#endif
  }
#else
  if (cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, s_audioOutSamples, AUDIO_SR, AUDIO_STEREO, kChannel)) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.queued;
#endif
  }
#endif
  md_audio_diag_log_if_due();
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
  YM2612SetDivisor(s_audioCoreDivisor);
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
  md_audio_diag_target(target);
  s_ym_target_clock = target;
}

/* Get the target clock for YM2612 */
extern "C" int  genesis_sound_ym_get_target_clock(void) {
  return s_ym_target_clock;
}
