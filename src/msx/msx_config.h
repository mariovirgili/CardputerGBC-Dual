#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef MSX_PROFILE_LOG_ENABLED
#define MSX_PROFILE_LOG_ENABLED 0
#endif

#ifndef MSX_PROFILE_LOG_INTERVAL_FRAMES
#define MSX_PROFILE_LOG_INTERVAL_FRAMES 60u
#endif

enum class MsxInternalViewMode : uint8_t {
    PixelPerfect = 0,
    Wide = 1,
    FastPlus = 2,
};

enum class MsxMachineMode : uint8_t {
    Auto = 0,
    MSX1 = 1,
    MSX2 = 2,
};

enum class MsxPerformanceMode : uint8_t {
    Accurate = 0,
    Performance = 1,
};

enum class MsxPerformancePreset : uint8_t {
    Normal = 0,
    Fast = 1,
    Aleste = 2,
    Custom = 3,
};

enum class MsxPerformanceFlag : uint8_t {
    DisableSliceRendering = 0x01,
    DisableSpriteCollision = 0x02,
    SimplifySpriteOverflow = 0x04,
    InstantVdpCommands = 0x08,
    ExternalFixed30Fps = 0x10,
};

enum class MsxFrameskipMode : uint8_t {
    Adaptive = 0,
    Ratio15,
    Ratio2,
    Ratio3,
    Ratio4,
    Ratio12,
    Ratio13,
    Ratio14,
    Count,
};

MsxInternalViewMode msx_config_load_internal_view_mode(void);
MsxInternalViewMode msx_config_get_internal_view_mode(void);
MsxInternalViewMode msx_config_get_active_view_mode(void);
const char* msx_config_internal_view_mode_label(MsxInternalViewMode mode);
const char* msx_config_view_mode_label_for_target(MsxInternalViewMode mode, bool useExternal);
const char* msx_config_get_internal_view_mode_label(void);
const char* msx_config_get_active_view_mode_label(void);
const char* msx_config_get_active_view_mode_label_for_target(bool useExternal);
void msx_config_set_internal_view_mode(MsxInternalViewMode mode, bool persist);
void msx_config_toggle_internal_view_mode(void);
void msx_config_set_view_mode_override(MsxInternalViewMode mode);
void msx_config_clear_view_mode_override(void);
void msx_config_toggle_active_view_mode(void);
void msx_config_toggle_active_view_mode_for_target(bool useExternal);

MsxMachineMode msx_config_load_machine_mode(void);
MsxMachineMode msx_config_get_machine_mode(void);
const char* msx_config_machine_mode_label(MsxMachineMode mode);
const char* msx_config_get_machine_mode_label(void);
void msx_config_set_machine_mode(MsxMachineMode mode, bool persist);

MsxPerformanceMode msx_config_load_performance_mode(void);
MsxPerformanceMode msx_config_get_performance_mode_value(void);
bool msx_config_get_performance_mode(void);
const char* msx_config_performance_mode_label(MsxPerformanceMode mode);
MsxPerformancePreset msx_config_get_performance_preset(void);
const char* msx_config_performance_preset_label(MsxPerformancePreset preset);
const char* msx_config_get_performance_mode_label(void);
uint8_t msx_config_load_performance_flags(void);
uint8_t msx_config_get_performance_flags(void);
bool msx_config_get_performance_flag(MsxPerformanceFlag flag);
void msx_config_set_performance_flags(uint8_t flags, bool persist);
void msx_config_set_performance_flag(MsxPerformanceFlag flag, bool enabled, bool persist);
void msx_config_set_performance_mode(bool enabled, bool persist);
void msx_config_set_performance_preset(MsxPerformancePreset preset, bool persist);
void msx_config_cycle_performance_preset(int delta, bool persist);
void msx_config_toggle_performance_mode(void);
MsxFrameskipMode msx_config_load_frameskip_mode(void);
MsxFrameskipMode msx_config_get_frameskip_mode(void);
const char* msx_config_frameskip_mode_label(MsxFrameskipMode mode);
const char* msx_config_get_frameskip_mode_label(void);
uint8_t msx_config_frameskip_mode_skip_numerator(MsxFrameskipMode mode);
uint8_t msx_config_frameskip_mode_skip_denominator(MsxFrameskipMode mode);
void msx_config_set_frameskip_mode(MsxFrameskipMode mode, bool persist);
void msx_config_cycle_frameskip_mode(int delta, bool persist);
bool msx_config_load_fps_overlay_enabled(void);
bool msx_config_get_fps_overlay_enabled(void);
void msx_config_set_fps_overlay_enabled(bool enabled, bool persist);

const char* msx_config_load_bios_path(void);
const char* msx_config_get_bios_path(void);
void msx_config_set_bios_path(const char* path, bool persist);

const char* msx_config_load_msx1_bios_path(void);
const char* msx_config_get_msx1_bios_path(void);
void msx_config_set_msx1_bios_path(const char* path, bool persist);

const char* msx_config_load_msx2_bios_path(void);
const char* msx_config_get_msx2_bios_path(void);
void msx_config_set_msx2_bios_path(const char* path, bool persist);

const char* msx_config_load_msx2_subrom_path(void);
const char* msx_config_get_msx2_subrom_path(void);
void msx_config_set_msx2_subrom_path(const char* path, bool persist);
