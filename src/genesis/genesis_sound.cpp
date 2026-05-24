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
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {};
uint8_t genesis_audio_volume = 50;

// Audio config
static constexpr uint8_t kChannel= 0;
static uint16_t s_psgGainQ15 = 32768;  // x1.0
static uint16_t s_fmGainQ15  = 32768;  // x1.0

#if MD_AUDIO_LOGS_ENABLED
struct MdAudioDiagStats {
  uint64_t lastLogMs = 0;
  uint32_t frames = 0;
  uint32_t queued = 0;
  uint32_t droppedDepth = 0;
  uint32_t droppedBuffer = 0;
  uint32_t clippedSamples = 0;
  uint32_t depthMax = 0;
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
};

static MdAudioDiagStats s_mdAudioDiag;

static inline uint64_t md_audio_now_ms()
{
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline void md_audio_diag_reset()
{
  s_mdAudioDiag = MdAudioDiagStats{};
  s_mdAudioDiag.lastLogMs = md_audio_now_ms();
}

static inline void md_audio_diag_samples(int core_n, int ym_n, int psg_n, size_t depth)
{
  MdAudioDiagStats& d = s_mdAudioDiag;
  ++d.frames;
  if (depth == 0) ++d.emptyDepth;
  if (depth > d.depthMax) d.depthMax = (uint32_t)depth;

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
}

static inline void md_audio_diag_target(int target)
{
  MdAudioDiagStats& d = s_mdAudioDiag;
  d.targetLast = target;
  if (target < d.targetMin) d.targetMin = target;
  if (target > d.targetMax) d.targetMax = target;
  d.targetTotal += (uint32_t)(target > 0 ? target : 0);
  ++d.targetCount;
}

static inline void md_audio_diag_log_if_due(bool force = false)
{
  MdAudioDiagStats& d = s_mdAudioDiag;
  const uint64_t now = md_audio_now_ms();
  if (!force && (now - d.lastLogMs) < 1000ULL) return;
  if (d.frames == 0 && d.targetCount == 0) {
    d.lastLogMs = now;
    return;
  }

  const uint32_t coreMin = (d.coreMin == UINT32_MAX) ? 0 : d.coreMin;
  const uint32_t ymMin = (d.ymMin == UINT32_MAX) ? 0 : d.ymMin;
  const uint32_t psgMin = (d.psgMin == UINT32_MAX) ? 0 : d.psgMin;
  const uint32_t coreAvg = d.frames ? (uint32_t)(d.coreTotal / d.frames) : 0;
  const uint32_t ymAvg = d.frames ? (uint32_t)(d.ymTotal / d.frames) : 0;
  const uint32_t psgAvg = d.frames ? (uint32_t)(d.psgTotal / d.frames) : 0;
  const int targetMin = (d.targetMin == INT32_MAX) ? 0 : d.targetMin;
  const int targetMax = (d.targetMax == INT32_MIN) ? 0 : d.targetMax;
  const uint32_t targetAvg = d.targetCount ? (uint32_t)(d.targetTotal / d.targetCount) : 0;

  MD_AUDIO_LOG("frames=%lu queued=%lu loss depth/buf=%lu/%lu queueDepthMax=%lu underrunEmpty=%lu coreRate=%d outRate=%d coreSamples min/avg/max=%lu/%lu/%lu outSamples=%d ym=%lu/%lu/%lu psg=%lu/%lu/%lu target min/avg/max/last=%d/%lu/%d/%d clipped=%lu",
               (unsigned long)d.frames,
               (unsigned long)d.queued,
               (unsigned long)d.droppedDepth,
               (unsigned long)d.droppedBuffer,
               (unsigned long)d.depthMax,
               (unsigned long)d.emptyDepth,
               AUDIO_CORE_SR,
               AUDIO_SR,
               (unsigned long)coreMin,
               (unsigned long)coreAvg,
               (unsigned long)d.coreMax,
               AUDIO_CHUNK,
               (unsigned long)ymMin,
               (unsigned long)ymAvg,
               (unsigned long)d.ymMax,
               (unsigned long)psgMin,
               (unsigned long)psgAvg,
               (unsigned long)d.psgMax,
               targetMin,
               (unsigned long)targetAvg,
               targetMax,
               d.targetLast,
               (unsigned long)d.clippedSamples);

  d = MdAudioDiagStats{};
  d.lastLogMs = now;
}
#else
static inline void md_audio_diag_reset() {}
static inline void md_audio_diag_samples(int, int, int, size_t) {}
static inline void md_audio_diag_target(int) {}
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
  md_audio_diag_reset();
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
  if (n > AUDIO_CORE_CHUNK) n = AUDIO_CORE_CHUNK;
  if (ym_n > AUDIO_CORE_CHUNK) ym_n = AUDIO_CORE_CHUNK;
  if (psg_n > AUDIO_CORE_CHUNK) psg_n = AUDIO_CORE_CHUNK;

  const size_t depth = M5Cardputer.Speaker.isPlaying(kChannel);
  md_audio_diag_samples(n, ym_n, psg_n, depth);

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
  if (n == 1) {
    int16_t sample = mix_sample_at(0, ym_n, psg_n);
    for (int i = 0; i < AUDIO_CHUNK; ++i) dst[i] = sample;
  } else {
    const uint32_t step = (uint32_t)(((uint64_t)(n - 1) << 16) / (AUDIO_CHUNK - 1));
    uint32_t pos = 0;
    for (int i = 0; i < AUDIO_CHUNK; ++i) {
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

  if (cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, AUDIO_CHUNK, AUDIO_SR, AUDIO_STEREO, kChannel)) {
#if MD_AUDIO_LOGS_ENABLED
    ++s_mdAudioDiag.queued;
#endif
  }
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
