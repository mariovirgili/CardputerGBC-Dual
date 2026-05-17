#include "ngc_sound.h"

#include "compat/arduino_compat.h"
#include <M5Cardputer.h>
#include "cardputer/CardputerAudio.h"

extern "C" {
  #include "ngp/race/types.h"     // _u16
#include "share/emu_log_cpp.h"
  void sound_init(int SampleRate);
  void sound_update(_u16* chip_buffer, int length_bytes); // PSG
  void dac_update  (_u16* dac_buffer,  int length_bytes); // DAC
}

// -------- Settings --------
static constexpr int kSampleRate = 22050;  // Hz
static constexpr int kFps        = 60;     // NGP 60 Hz
static constexpr int kChunk      = (kSampleRate + kFps/2) / kFps; // 368 smp/frame
static constexpr int kChannel    = 0;

// -------- Buffers --------
static int16_t*  s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {nullptr, nullptr, nullptr};
static uint16_t* s_psg      = nullptr;
static uint16_t* s_dac      = nullptr;
static uint8_t   s_flip     = 0;

// -------- Helpers --------
static inline int16_t clamp16(int32_t v) {
  if (v >  32767) return  32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static inline void mix_build_block(int16_t* dst /*kChunk*/) {
  const int lenB = kChunk * (int)sizeof(uint16_t);
  sound_update(s_psg, lenB);
  dac_update  (s_dac, lenB);

  for (int i = 0; i < kChunk; ++i) {
    int32_t a = (int16_t)s_psg[i];
    int32_t b = (int16_t)s_dac[i];
    dst[i] = clamp16(a + b); // mono mix
  }
}

static inline void queue_block() {
  cardputer_audio::queueRuntimeAudioBuffer(
    s_buf, s_flip, (size_t)kChunk, (uint32_t)kSampleRate, false, kChannel);
}

static bool ngc_sound_alloc_buffers() {
  const size_t bytes_i16 = (size_t)kChunk * sizeof(int16_t);
  const size_t bytes_u16 = (size_t)kChunk * sizeof(uint16_t);

  if (!s_buf[0]) s_buf[0] = (int16_t*) heap_caps_malloc(bytes_i16, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_buf[1]) s_buf[1] = (int16_t*) heap_caps_malloc(bytes_i16, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_buf[2]) s_buf[2] = (int16_t*) heap_caps_malloc(bytes_i16, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_psg)    s_psg    = (uint16_t*)heap_caps_malloc(bytes_u16, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_dac)    s_dac    = (uint16_t*)heap_caps_malloc(bytes_u16, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

  if (!s_buf[0] || !s_buf[1] || !s_buf[2] || !s_psg || !s_dac) return false;

  memset(s_buf[0], 0, bytes_i16);
  memset(s_buf[1], 0, bytes_i16);
  memset(s_buf[2], 0, bytes_i16);
  memset(s_psg,    0, bytes_u16);
  memset(s_dac,    0, bytes_u16);
  return true;
}

// -------- API --------
extern "C" {

void ngc_sound_init(void) {
  sound_init(kSampleRate);

  cardputer_audio::beginSpeaker(kSampleRate, false, 512, 8, 80, "ngp", 4, 0);

  if (!ngc_sound_alloc_buffers()) {
    EMU_LOG("[AUDIO] buffer alloc failed (kChunk=%d)\n", kChunk);
  }

  s_flip = 0;
}

void ngc_sound_set_volume(uint8_t vol) {
  M5Cardputer.Speaker.setVolume(vol);
}

void ngc_sound_shutdown(void) {
  M5Cardputer.Speaker.stop(kChannel);
  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
}

void ngc_sound_frame(void) {
  if (!s_buf[0] || !s_buf[1] || !s_buf[2]) return;
  size_t queued = M5Cardputer.Speaker.isPlaying(kChannel);

  if (queued == 0) {
    // Prime with 2 blocks
    for (int i = 0; i < 2; ++i) {
      mix_build_block(s_buf[s_flip]);
      queue_block();
    }
  } else if (queued == 1) {
    // Keep at 2 blocks depth
    mix_build_block(s_buf[s_flip]);
    queue_block();
  } else {
    // queued == 2 → nothing, we’re buffered
  }
}

} // extern "C"
