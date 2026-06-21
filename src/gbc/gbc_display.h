// gbc_display.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool gbcFullScreen;
extern int  gbcZoomPercent;
extern int  gbPalette;

void gbc_display_init(void);
void gbc_display_start(void);
void gbc_display_stop(void);

// Called from the GNUBOY video callback
void gbc_display_submit_frame(const uint16_t *fb,
                              int pitch,   // in pixels
                              int width,
                              int height);

#if GB_BENCHMARK_LOGS_ENABLED
void gbc_display_get_and_reset_bench(uint32_t *submitted,
                                     uint32_t *rendered,
                                     uint32_t *dropped,
                                     uint32_t *avg_us,
                                     uint32_t *max_us);
#endif

#ifdef __cplusplus
}
#endif
