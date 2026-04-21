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
};

enum class MsxMachineMode : uint8_t {
    Auto = 0,
    MSX1 = 1,
    MSX2 = 2,
};

MsxInternalViewMode msx_config_load_internal_view_mode(void);
MsxInternalViewMode msx_config_get_internal_view_mode(void);
MsxInternalViewMode msx_config_get_active_view_mode(void);
const char* msx_config_internal_view_mode_label(MsxInternalViewMode mode);
const char* msx_config_get_internal_view_mode_label(void);
const char* msx_config_get_active_view_mode_label(void);
void msx_config_set_internal_view_mode(MsxInternalViewMode mode, bool persist);
void msx_config_toggle_internal_view_mode(void);
void msx_config_set_view_mode_override(MsxInternalViewMode mode);
void msx_config_clear_view_mode_override(void);
void msx_config_toggle_active_view_mode(void);

MsxMachineMode msx_config_load_machine_mode(void);
MsxMachineMode msx_config_get_machine_mode(void);
const char* msx_config_machine_mode_label(MsxMachineMode mode);
const char* msx_config_get_machine_mode_label(void);
void msx_config_set_machine_mode(MsxMachineMode mode, bool persist);

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
