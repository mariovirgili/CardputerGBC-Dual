#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws_sound_init(int sample_rate_hz);
void ws_sound_set_volume(uint8_t vol);
void ws_sound_shutdown(void);
void ws_sound_frame(void);
void ws_sound_start_task(uint32_t period_ms, int core);
void ws_sound_stop_task(void);
void ws_sound_pause_task(int pause);
void ws_sound_get_task_info(uint32_t* priority, uint32_t* core);
#ifdef WS_BENCHMARK_LOGS
void ws_sound_get_and_reset_stats(uint32_t* blocks,
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
                                  uint32_t* play_fails);
void ws_sound_get_and_reset_direct_stats(uint32_t* mode_direct,
                                         uint32_t* chunk_samples,
                                         uint32_t* dma_len,
                                         uint32_t* dma_count,
                                         uint32_t* write_calls,
                                         uint32_t* short_writes,
                                         uint32_t* write_wait_avg_us,
                                         uint32_t* write_wait_max_us,
                                         uint32_t* push_gap_avg_us,
                                         uint32_t* push_gap_max_us,
                                         uint32_t* late_pushes);
#endif

#ifdef __cplusplus
}
#endif
