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
    Manbow = 2,
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

enum class MsxFpsOverlayMode : uint8_t {
    Off = 0,
    Simple = 1,
    Dual = 2,
    Count,
};

enum class MsxVirtualSccMode : uint8_t {
    Off = 0,
    Scc = 1,
    SccI = 2,
    Count,
};

enum class MsxRegionMode : uint8_t {
    Auto = 0,
    World,
    Japan,
    Count,
};

enum class MsxRegionProfile : uint8_t {
    World = 0,
    Japan,
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
void msx_config_set_machine_mode_session_override(MsxMachineMode mode, bool enabled);

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
MsxFpsOverlayMode msx_config_load_fps_overlay_mode(void);
MsxFpsOverlayMode msx_config_get_fps_overlay_mode(void);
const char* msx_config_fps_overlay_mode_label(MsxFpsOverlayMode mode);
const char* msx_config_get_fps_overlay_mode_label(void);
void msx_config_set_fps_overlay_mode(MsxFpsOverlayMode mode, bool persist);
void msx_config_cycle_fps_overlay_mode(int delta, bool persist);

MsxVirtualSccMode msx_config_load_virtual_scc_mode(void);
MsxVirtualSccMode msx_config_get_virtual_scc_mode(void);
const char* msx_config_virtual_scc_mode_label(MsxVirtualSccMode mode);
const char* msx_config_get_virtual_scc_mode_label(void);
void msx_config_set_virtual_scc_mode(MsxVirtualSccMode mode, bool persist);
void msx_config_cycle_virtual_scc_mode(int delta, bool persist);
bool msx_config_load_scc_hardware_detect_enabled(void);
bool msx_config_get_scc_hardware_detect_enabled(void);
void msx_config_set_scc_hardware_detect_enabled(bool enabled, bool persist);

MsxRegionMode msx_config_load_region_mode(void);
MsxRegionMode msx_config_get_region_mode(void);
const char* msx_config_region_mode_label(MsxRegionMode mode);
const char* msx_config_region_profile_label(MsxRegionProfile profile);
void msx_config_set_region_mode(MsxRegionMode mode, bool persist);
void msx_config_cycle_region_mode(int delta, bool persist);

uint8_t msx_config_load_sound_volume(void);
uint8_t msx_config_get_sound_volume(void);
const char* msx_config_sound_volume_label(uint8_t volume);
void msx_config_set_sound_volume(uint8_t volume, bool persist);
void msx_config_cycle_sound_volume(int delta, bool persist);
uint16_t msx_config_load_scc_gain_percent(void);
uint16_t msx_config_get_scc_gain_percent(void);
const char* msx_config_scc_gain_label(uint16_t gainPercent);
void msx_config_set_scc_gain_percent(uint16_t gainPercent, bool persist);
void msx_config_cycle_scc_gain_percent(int delta, bool persist);

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
