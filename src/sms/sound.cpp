#include "sound.h"
#include <M5Cardputer.h>
#include <algorithm>
#include "cardputer/CardputerAudio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t gAudioTask = nullptr;
static volatile bool gRunAudio = true;

static constexpr int kSampleRate = 22050;   // Hz
static constexpr int kChannel    = 0;
static constexpr int   kChunk      = 128; 
static constexpr int kBufSize = 888;
static int16_t* s_buf[cardputer_audio::kRuntimeAudioBufferCount] = {};
static uint8_t s_flip = 0;

static inline int clamp16(int x){ return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }
static void mix_lr_to(int16_t* dst, const int16_t* L, const int16_t* R, int n){
  for (int i=0;i<n;i++) dst[i] = (int16_t)clamp16(((int)L[i] + (int)R[i]) >> 1);
}

void sms_audio_init(){
  cardputer_audio::beginSpeaker(kSampleRate, false, 512, 8, 80, "sms", 4, 0);

  cardputer_audio::allocRuntimeAudioBuffers(s_buf, kBufSize, "sms");

  M5Cardputer.Speaker.stop(kChannel);
}

void sms_audio_stop(){
  M5Cardputer.Speaker.stop(kChannel);
  cardputer_audio::freeRuntimeAudioBuffers(s_buf);
}

void sms_audio_frame(){
  if (!snd.buffer[0] || !snd.buffer[1] || snd.bufsize <= 0) return;
  if (!s_buf[0] || !s_buf[1] || !s_buf[2] || !s_buf[3]) return;

  int n = std::min((int)snd.bufsize, kBufSize);
  n &= ~1;

  size_t st = M5Cardputer.Speaker.isPlaying(kChannel);

  if (st == 0) {
    mix_lr_to(s_buf[s_flip], snd.buffer[0], snd.buffer[1], n);
    cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, (size_t)n, (uint32_t)kSampleRate, false, kChannel);

    mix_lr_to(s_buf[s_flip], snd.buffer[0], snd.buffer[1], n);
    cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, (size_t)n, (uint32_t)kSampleRate, false, kChannel);
    return;
  }

  if (st == 1) {
    mix_lr_to(s_buf[s_flip], snd.buffer[0], snd.buffer[1], n);
    cardputer_audio::queueRuntimeAudioBuffer(s_buf, s_flip, (size_t)n, (uint32_t)kSampleRate, false, kChannel);
  }
}

static void audio_task(void*){
  const TickType_t period =
      std::max<TickType_t>(1, pdMS_TO_TICKS((kChunk * 1000) / kSampleRate)); // ~11-12 ms
  TickType_t last = xTaskGetTickCount();

  for(;;){
    if (!gRunAudio) break;
    sms_audio_frame();
    vTaskDelayUntil(&last, period); // synchro sur la durée du chunk
  }
  vTaskDelete(NULL);
}

void sms_audio_start_task(){
  gRunAudio = true;
  if (!gAudioTask){
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, &gAudioTask, 0);
  }
}

void sms_audio_stop_task(){
  gRunAudio = false;
  // on ne delete pas explicitement ici; la task s’auto-delete à la sortie
  gAudioTask = nullptr;
}
