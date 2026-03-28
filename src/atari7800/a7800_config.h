#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class A7800InternalViewMode : uint8_t {
    PixelPerfect = 0,
    Wide = 1,
};

A7800InternalViewMode a7800_config_load_internal_view_mode(void);
A7800InternalViewMode a7800_config_get_internal_view_mode(void);
const char* a7800_config_internal_view_mode_label(A7800InternalViewMode mode);
const char* a7800_config_get_internal_view_mode_label(void);
void a7800_config_save_internal_view_mode(A7800InternalViewMode mode);
void a7800_config_set_internal_view_mode(A7800InternalViewMode mode, bool persist);
void a7800_config_toggle_internal_view_mode(void);
