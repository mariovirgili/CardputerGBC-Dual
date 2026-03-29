#include "ws_profiler.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {

struct SampleStat {
  uint32_t count = 0;
  uint32_t sum_us = 0;
  uint32_t max_us = 0;
};

struct CounterStat {
  uint32_t count = 0;
  uint32_t sum = 0;
  uint32_t max = 0;
};

struct WsProfileWindow {
  uint32_t started_ms = 0;
  bool     use_external = false;

  int main_core    = -1;
  int display_core = -1;
  int audio_core   = -1;
  int speaker_core = -1;

  SampleStat core_total;
  SampleStat cpu;
  SampleStat video;
  SampleStat input;
  SampleStat notify;
  SampleStat save;
  SampleStat sleep;
  SampleStat display;

  CounterStat display_burst;
  CounterStat audio_apu_available;
  CounterStat audio_queued_blocks;

  uint32_t audio_ticks      = 0;
  uint32_t audio_underflows = 0;
  uint32_t frame_late_count = 0;
  uint32_t main_yield_count = 0;
};

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
WsProfileWindow s_window;

static void reset_window_locked(uint32_t now_ms)
{
  const bool use_external = s_window.use_external;
  const int main_core = s_window.main_core;
  const int display_core = s_window.display_core;
  const int audio_core = s_window.audio_core;
  const int speaker_core = s_window.speaker_core;

  s_window = WsProfileWindow{};
  s_window.started_ms = now_ms;
  s_window.use_external = use_external;
  s_window.main_core = main_core;
  s_window.display_core = display_core;
  s_window.audio_core = audio_core;
  s_window.speaker_core = speaker_core;
}

static inline void add_sample(SampleStat& stat, uint32_t value_us)
{
  stat.count++;
  stat.sum_us += value_us;
  if (value_us > stat.max_us) {
    stat.max_us = value_us;
  }
}

static inline void add_counter(CounterStat& stat, uint32_t value)
{
  stat.count++;
  stat.sum += value;
  if (value > stat.max) {
    stat.max = value;
  }
}

static float avg_us(const SampleStat& stat)
{
  return stat.count ? (float)stat.sum_us / (float)stat.count : 0.0f;
}

static float avg_counter(const CounterStat& stat)
{
  return stat.count ? (float)stat.sum / (float)stat.count : 0.0f;
}

}  // namespace

extern "C" void ws_profiler_begin_run(int use_external, int main_core)
{
  const uint32_t now_ms = millis();
  portENTER_CRITICAL(&s_lock);
  s_window = WsProfileWindow{};
  s_window.started_ms = now_ms;
  s_window.use_external = (use_external != 0);
  s_window.main_core = main_core;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_note_display_core(int core)
{
  portENTER_CRITICAL(&s_lock);
  s_window.display_core = core;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_note_audio_core(int core)
{
  portENTER_CRITICAL(&s_lock);
  s_window.audio_core = core;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_note_speaker_core(int core)
{
  portENTER_CRITICAL(&s_lock);
  s_window.speaker_core = core;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_submit_core_frame(
    uint32_t total_us,
    uint32_t cpu_us,
    uint32_t video_us,
    uint32_t input_us,
    uint32_t notify_us)
{
  portENTER_CRITICAL(&s_lock);
  add_sample(s_window.core_total, total_us);
  add_sample(s_window.cpu, cpu_us);
  add_sample(s_window.video, video_us);
  add_sample(s_window.input, input_us);
  add_sample(s_window.notify, notify_us);
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_submit_display_frame(uint32_t render_us, uint32_t burst_count)
{
  portENTER_CRITICAL(&s_lock);
  add_sample(s_window.display, render_us);
  add_counter(s_window.display_burst, burst_count);
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_submit_audio_tick(uint32_t apu_available, uint32_t queued_blocks, uint32_t underflowed)
{
  portENTER_CRITICAL(&s_lock);
  s_window.audio_ticks++;
  if (underflowed != 0) {
    s_window.audio_underflows++;
  }
  add_counter(s_window.audio_apu_available, apu_available);
  add_counter(s_window.audio_queued_blocks, queued_blocks);
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_add_save_us(uint32_t us)
{
  portENTER_CRITICAL(&s_lock);
  add_sample(s_window.save, us);
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_add_sleep_us(uint32_t us)
{
  portENTER_CRITICAL(&s_lock);
  add_sample(s_window.sleep, us);
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_note_frame_late(void)
{
  portENTER_CRITICAL(&s_lock);
  s_window.frame_late_count++;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_note_main_yield(void)
{
  portENTER_CRITICAL(&s_lock);
  s_window.main_yield_count++;
  portEXIT_CRITICAL(&s_lock);
}

extern "C" void ws_profiler_log_if_due(void)
{
  WsProfileWindow snapshot;
  bool should_log = false;
  const uint32_t now_ms = millis();

  portENTER_CRITICAL(&s_lock);
  if (s_window.started_ms == 0) {
    s_window.started_ms = now_ms;
  }

  if ((now_ms - s_window.started_ms) >= 1000) {
    snapshot = s_window;
    reset_window_locked(now_ms);
    should_log = true;
  }
  portEXIT_CRITICAL(&s_lock);

  if (!should_log) {
    return;
  }

  const uint32_t window_ms = (snapshot.started_ms != 0) ? (now_ms - snapshot.started_ms) : 0;
  const float fps = (window_ms > 0)
      ? (1000.0f * (float)snapshot.core_total.count) / (float)window_ms
      : 0.0f;

  const float core_avg = avg_us(snapshot.core_total);
  const float cpu_avg = avg_us(snapshot.cpu);
  const float video_avg = avg_us(snapshot.video);
  const float input_avg = avg_us(snapshot.input);
  const float notify_avg = avg_us(snapshot.notify);
  const float save_avg = avg_us(snapshot.save);
  const float sleep_avg = avg_us(snapshot.sleep);
  float other_avg = core_avg - cpu_avg - video_avg - input_avg - notify_avg;
  if (other_avg < 0.0f) {
    other_avg = 0.0f;
  }

  printf(
      "[WS-PROF] target=%s fps=%.2f late=%u yields=%u cores main=%d disp=%d audio=%d speaker=%d\n",
      snapshot.use_external ? "external" : "internal",
      fps,
      (unsigned)snapshot.frame_late_count,
      (unsigned)snapshot.main_yield_count,
      snapshot.main_core,
      snapshot.display_core,
      snapshot.audio_core,
      snapshot.speaker_core);

  printf(
      "[WS-PROF] main avg/max ms: core=%.2f/%.2f cpu=%.2f/%.2f video=%.2f/%.2f input=%.2f/%.2f notify=%.2f/%.2f other=%.2f save=%.2f sleep=%.2f\n",
      core_avg / 1000.0f, (float)snapshot.core_total.max_us / 1000.0f,
      cpu_avg / 1000.0f, (float)snapshot.cpu.max_us / 1000.0f,
      video_avg / 1000.0f, (float)snapshot.video.max_us / 1000.0f,
      input_avg / 1000.0f, (float)snapshot.input.max_us / 1000.0f,
      notify_avg / 1000.0f, (float)snapshot.notify.max_us / 1000.0f,
      other_avg / 1000.0f,
      save_avg / 1000.0f,
      sleep_avg / 1000.0f);

  printf(
      "[WS-PROF] display avg/max ms: render=%.2f/%.2f burst=%.2f maxBurst=%u frames=%u\n",
      avg_us(snapshot.display) / 1000.0f,
      (float)snapshot.display.max_us / 1000.0f,
      avg_counter(snapshot.display_burst),
      (unsigned)snapshot.display_burst.max,
      (unsigned)snapshot.display.count);

  printf(
      "[WS-PROF] audio ticks=%u underflows=%u apuAvailAvg=%.1f queuedAvg=%.2f queuedMax=%u\n",
      (unsigned)snapshot.audio_ticks,
      (unsigned)snapshot.audio_underflows,
      avg_counter(snapshot.audio_apu_available),
      avg_counter(snapshot.audio_queued_blocks),
      (unsigned)snapshot.audio_queued_blocks.max);
}
