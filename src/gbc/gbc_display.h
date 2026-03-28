// gbc_display.h
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool gbcFullScreen;
extern int  gbcZoomPercent;
extern int  gbPalette;

typedef enum {
    GBC_DISPLAY_EXTERNAL = 0,
    GBC_DISPLAY_INTERNAL = 1,
} gbc_display_target_t;

void gbc_display_set_target(gbc_display_target_t target);
void gbc_display_init(void);
void gbc_display_start(void);
void gbc_display_stop(void);
void gbc_display_show_external_info(const char* romTitle, bool colorGame);

// Called from the GNUBOY video callback
void gbc_display_submit_frame(const uint16_t *fb,
                              int pitch,   // in pixels
                              int width,
                              int height);

#ifdef __cplusplus
}
#endif
