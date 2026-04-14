#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class ColecoInternalViewMode : uint8_t {
    PixelPerfect = 0,
    Wide = 1,
};

enum class ColecoMachineMode : uint8_t {
    Auto = 0,
    MSX1 = 1,
    MSX2 = 2,
};

ColecoInternalViewMode coleco_config_load_internal_view_mode(void);
ColecoInternalViewMode coleco_config_get_internal_view_mode(void);
ColecoInternalViewMode coleco_config_get_active_view_mode(void);
const char* coleco_config_internal_view_mode_label(ColecoInternalViewMode mode);
const char* coleco_config_get_internal_view_mode_label(void);
const char* coleco_config_get_active_view_mode_label(void);
void coleco_config_set_internal_view_mode(ColecoInternalViewMode mode, bool persist);
void coleco_config_toggle_internal_view_mode(void);
void coleco_config_set_view_mode_override(ColecoInternalViewMode mode);
void coleco_config_clear_view_mode_override(void);
void coleco_config_toggle_active_view_mode(void);

ColecoMachineMode coleco_config_load_machine_mode(void);
ColecoMachineMode coleco_config_get_machine_mode(void);
const char* coleco_config_machine_mode_label(ColecoMachineMode mode);
const char* coleco_config_get_machine_mode_label(void);
void coleco_config_set_machine_mode(ColecoMachineMode mode, bool persist);

const char* coleco_config_load_bios_path(void);
const char* coleco_config_get_bios_path(void);
void coleco_config_set_bios_path(const char* path, bool persist);

const char* coleco_config_load_msx1_bios_path(void);
const char* coleco_config_get_msx1_bios_path(void);
void coleco_config_set_msx1_bios_path(const char* path, bool persist);

const char* coleco_config_load_msx2_bios_path(void);
const char* coleco_config_get_msx2_bios_path(void);
void coleco_config_set_msx2_bios_path(const char* path, bool persist);

const char* coleco_config_load_msx2_subrom_path(void);
const char* coleco_config_get_msx2_subrom_path(void);
void coleco_config_set_msx2_subrom_path(const char* path, bool persist);
