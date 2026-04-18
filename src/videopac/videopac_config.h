#pragma once

#include <stdint.h>

enum class VideopacVideoMode : uint8_t {
    Fit = 0,
    Fast = 1,
    FitFast = 2,
};

VideopacVideoMode videopac_config_load_video_mode(void);
VideopacVideoMode videopac_config_get_video_mode(void);
const char* videopac_config_video_mode_label(VideopacVideoMode mode);
const char* videopac_config_get_video_mode_label(void);
void videopac_config_set_video_mode(VideopacVideoMode mode, bool persist);
void videopac_config_toggle_video_mode(void);
void videopac_config_step_video_mode(int direction);
