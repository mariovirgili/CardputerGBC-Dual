#pragma GCC optimize ("Os")

#include <M5Cardputer.h>
#include "cardputer/CardputerAudio.h"

extern "C" {
  #include <osd.h>
  #include <stdbool.h>
}

// Param audio 
static constexpr int kSampleRate = 22050; // Hz
static constexpr int kNesHz      = 60;    // 60 FPS
static constexpr int kChunk      = (kSampleRate + kNesHz/2) / kNesHz; // 368 samples per frame

// Canal audio
static constexpr int kChannel    = 0;

// Callback Nofrendo
static void (*s_audio_cb)(void *buffer, int length) = nullptr;

// Double buffer mono (Nofrendo to HP)
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = { nullptr, nullptr, nullptr };
static uint8_t s_flip = 0;

extern "C" {

int osd_init_sound() {
  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
  cardputer_audio::allocRuntimeAudioBuffers(s_buf, kChunk, "nes");
  s_flip = 0;

  cardputer_audio::beginSpeaker(kSampleRate, false, 512, 8, 80, "nes", 4, 0);

  M5Cardputer.Speaker.stop(kChannel);
  return 0;
}

void osd_stopsound() {
  s_audio_cb = nullptr;
  M5Cardputer.Speaker.stop(kChannel);
  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
}

void osd_setsound(void (*playfunc)(void *buffer, int length)) {
  s_audio_cb = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = kSampleRate;
  info->bps         = 16;
}

void do_audio_frame() {
  if (!s_audio_cb) return;
  if (!s_buf[0] || !s_buf[1] || !s_buf[2]) return;

  size_t st = M5Cardputer.Speaker.isPlaying(kChannel);

  // if nothing is playing, we queue 2 chunks to avoid gaps
  if (st == 0) {
    for (int i = 0; i < 2; ++i) {
      s_audio_cb((void*)s_buf[s_flip], kChunk);
      cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, (size_t)kChunk, (uint32_t)kSampleRate, false, kChannel);
    }
    return;
  }

  // if we have space, we add exactly 1 chunk
  if (st == 1) {
    s_audio_cb((void*)s_buf[s_flip], kChunk);
    cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, (size_t)kChunk, (uint32_t)kSampleRate, false, kChannel);
  }

  // st == 2, we have 2 chunks playing, nothing to do
}

} // extern "C"
