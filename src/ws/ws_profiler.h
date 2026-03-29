#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws_profiler_begin_run(int use_external, int main_core);
void ws_profiler_note_display_core(int core);
void ws_profiler_note_audio_core(int core);
void ws_profiler_note_speaker_core(int core);

void ws_profiler_submit_core_frame(
    uint32_t total_us,
    uint32_t cpu_us,
    uint32_t video_us,
    uint32_t input_us,
    uint32_t notify_us);
void ws_profiler_submit_display_frame(uint32_t render_us, uint32_t burst_count);
void ws_profiler_submit_audio_tick(uint32_t apu_available, uint32_t queued_blocks, uint32_t underflowed);

void ws_profiler_add_save_us(uint32_t us);
void ws_profiler_add_sleep_us(uint32_t us);
void ws_profiler_note_frame_late(void);
void ws_profiler_note_main_yield(void);
void ws_profiler_log_if_due(void);

#ifdef __cplusplus
}
#endif
