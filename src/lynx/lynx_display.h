// lynx_display.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lynx_display_init(void);
void lynx_display_start(void);
void lynx_display_stop(void);
void lynx_display_submit_frame(const uint16_t *fb,
                               int width,
                               int height);

#ifdef __cplusplus
}

// Show ROM info + controls on external TFT (when game renders on internal)
void lynx_display_show_external_info(const char* romTitle);
#endif
