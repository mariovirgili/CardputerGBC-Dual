#include "coleco_config.h"

#include <Preferences.h>

#include <cstring>

namespace {

constexpr const char* kColecoConfigNs = "coleco_cfg";
constexpr const char* kColecoViewKey = "int_view";
constexpr const char* kColecoMachineKey = "machine";
constexpr const char* kColecoBiosPathKey = "bios_path";
constexpr const char* kColeco1BiosPathKey = "bios_msx1";
constexpr ColecoInternalViewMode kColecoDefaultInternalViewMode = ColecoInternalViewMode::Wide;
constexpr ColecoMachineMode kColecoDefaultMachineMode = ColecoMachineMode::MSX1;

ColecoInternalViewMode s_internalViewMode = kColecoDefaultInternalViewMode;
ColecoInternalViewMode s_viewModeOverride = kColecoDefaultInternalViewMode;
bool s_viewModeOverrideEnabled = false;
ColecoMachineMode s_machineMode = kColecoDefaultMachineMode;
char s_genericBiosPath[96] = {0};
char s_msx1BiosPath[96] = {0};

ColecoInternalViewMode coleco_sanitize_internal_view_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(ColecoInternalViewMode::PixelPerfect)
               ? ColecoInternalViewMode::PixelPerfect
               : ColecoInternalViewMode::Wide;
}

ColecoMachineMode coleco_sanitize_machine_mode(uint8_t value)
{
    return ColecoMachineMode::MSX1;
}

void coleco_copy_path(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    std::strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

const char* coleco_load_path_key(const char* key, char* dst, size_t dstSize)
{
    Preferences prefs;
    prefs.begin(kColecoConfigNs, true);
    const String value = prefs.getString(key, "");
    prefs.end();

    coleco_copy_path(dst, dstSize, value.c_str());
    return dst;
}

void coleco_save_path_key(const char* key, char* dst, size_t dstSize, const char* path, bool persist)
{
    coleco_copy_path(dst, dstSize, path ? path : "");
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kColecoConfigNs, false);
    prefs.putString(key, dst);
    prefs.end();
}

void coleco_store_internal_view_mode(ColecoInternalViewMode mode)
{
    s_internalViewMode = coleco_sanitize_internal_view_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kColecoConfigNs, false);
    prefs.putUChar(kColecoViewKey, static_cast<uint8_t>(s_internalViewMode));
    prefs.end();
}

void coleco_store_machine_mode(ColecoMachineMode mode)
{
    s_machineMode = coleco_sanitize_machine_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kColecoConfigNs, false);
    prefs.putUChar(kColecoMachineKey, static_cast<uint8_t>(s_machineMode));
    prefs.end();
}

} // namespace

ColecoInternalViewMode coleco_config_load_internal_view_mode(void)
{
    Preferences prefs;
    prefs.begin(kColecoConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kColecoViewKey);
    const uint8_t saved = prefs.getUChar(
        kColecoViewKey,
        static_cast<uint8_t>(kColecoDefaultInternalViewMode)
    );
    prefs.end();

    s_internalViewMode = hasSavedValue
                             ? coleco_sanitize_internal_view_mode(saved)
                             : kColecoDefaultInternalViewMode;

    if (!hasSavedValue) {
        coleco_store_internal_view_mode(s_internalViewMode);
    }

    return s_internalViewMode;
}

ColecoInternalViewMode coleco_config_get_internal_view_mode(void)
{
    return s_internalViewMode;
}

ColecoInternalViewMode coleco_config_get_active_view_mode(void)
{
    return s_viewModeOverrideEnabled ? s_viewModeOverride : s_internalViewMode;
}

const char* coleco_config_internal_view_mode_label(ColecoInternalViewMode mode)
{
    return mode == ColecoInternalViewMode::PixelPerfect ? "CROP" : "WIDE";
}

const char* coleco_config_get_internal_view_mode_label(void)
{
    return coleco_config_internal_view_mode_label(s_internalViewMode);
}

const char* coleco_config_get_active_view_mode_label(void)
{
    return coleco_config_internal_view_mode_label(coleco_config_get_active_view_mode());
}

void coleco_config_set_internal_view_mode(ColecoInternalViewMode mode, bool persist)
{
    s_internalViewMode = coleco_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    if (persist) {
        coleco_store_internal_view_mode(s_internalViewMode);
    }
}

void coleco_config_toggle_internal_view_mode(void)
{
    const ColecoInternalViewMode nextMode =
        (s_internalViewMode == ColecoInternalViewMode::PixelPerfect)
            ? ColecoInternalViewMode::Wide
            : ColecoInternalViewMode::PixelPerfect;
    coleco_config_set_internal_view_mode(nextMode, true);
}

void coleco_config_set_view_mode_override(ColecoInternalViewMode mode)
{
    s_viewModeOverride = coleco_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    s_viewModeOverrideEnabled = true;
}

void coleco_config_clear_view_mode_override(void)
{
    s_viewModeOverrideEnabled = false;
    s_viewModeOverride = s_internalViewMode;
}

void coleco_config_toggle_active_view_mode(void)
{
    const ColecoInternalViewMode nextMode =
        (coleco_config_get_active_view_mode() == ColecoInternalViewMode::PixelPerfect)
            ? ColecoInternalViewMode::Wide
            : ColecoInternalViewMode::PixelPerfect;

    if (s_viewModeOverrideEnabled) {
        s_viewModeOverride = nextMode;
        return;
    }

    coleco_config_set_internal_view_mode(nextMode, true);
}

ColecoMachineMode coleco_config_load_machine_mode(void)
{
    Preferences prefs;
    prefs.begin(kColecoConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kColecoMachineKey);
    const uint8_t saved = prefs.getUChar(
        kColecoMachineKey,
        static_cast<uint8_t>(kColecoDefaultMachineMode)
    );
    prefs.end();

    s_machineMode = hasSavedValue
                        ? coleco_sanitize_machine_mode(saved)
                        : kColecoDefaultMachineMode;

    if (!hasSavedValue) {
        coleco_store_machine_mode(s_machineMode);
    }

    return s_machineMode;
}

ColecoMachineMode coleco_config_get_machine_mode(void)
{
    return s_machineMode;
}

const char* coleco_config_machine_mode_label(ColecoMachineMode mode)
{
    return "MSX1";
}

const char* coleco_config_get_machine_mode_label(void)
{
    return coleco_config_machine_mode_label(s_machineMode);
}

void coleco_config_set_machine_mode(ColecoMachineMode mode, bool persist)
{
    s_machineMode = coleco_sanitize_machine_mode(static_cast<uint8_t>(mode));
    if (persist) {
        coleco_store_machine_mode(s_machineMode);
    }
}

const char* coleco_config_load_bios_path(void)
{
    return coleco_load_path_key(kColecoBiosPathKey, s_genericBiosPath, sizeof(s_genericBiosPath));
}

const char* coleco_config_get_bios_path(void)
{
    return s_genericBiosPath;
}

void coleco_config_set_bios_path(const char* path, bool persist)
{
    coleco_save_path_key(kColecoBiosPathKey, s_genericBiosPath, sizeof(s_genericBiosPath), path, persist);
}

const char* coleco_config_load_msx1_bios_path(void)
{
    return coleco_load_path_key(kColeco1BiosPathKey, s_msx1BiosPath, sizeof(s_msx1BiosPath));
}

const char* coleco_config_get_msx1_bios_path(void)
{
    return s_msx1BiosPath;
}

void coleco_config_set_msx1_bios_path(const char* path, bool persist)
{
    coleco_save_path_key(kColeco1BiosPathKey, s_msx1BiosPath, sizeof(s_msx1BiosPath), path, persist);
}
