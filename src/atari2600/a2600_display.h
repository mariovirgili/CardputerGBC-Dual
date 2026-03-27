#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void a2600_display_init(void);
void a2600_display_start(void);
void a2600_display_stop(void);
void a2600_display_submit_frame(const uint8_t* indexedFrame,
                                int width,
                                int height,
                                const uint16_t* palette565,
                                bool isPal);

#ifdef __cplusplus
}

enum class A2600InternalViewMode : uint8_t {
    PixelPerfect = 0,
    Wide = 1,
};

void a2600_display_show_external_info(const char* romTitle);
A2600InternalViewMode a2600_display_load_internal_view_mode(void);
A2600InternalViewMode a2600_display_get_internal_view_mode(void);
const char* a2600_display_internal_view_mode_label(A2600InternalViewMode mode);
const char* a2600_display_get_internal_view_mode_label(void);
void a2600_display_set_internal_view_mode(A2600InternalViewMode mode, bool persist);
void a2600_display_toggle_internal_view_mode(void);

extern bool a2600FullScreen;
extern int a2600ZoomPercent;
#endif
