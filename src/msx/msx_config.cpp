#include "msx_config.h"

#include <Preferences.h>

#include <cstring>

namespace {

constexpr const char* kMsxConfigNs = "msx_cfg";
constexpr const char* kMsxViewKey = "int_view";
constexpr const char* kMsxMachineKey = "machine";
constexpr const char* kMsxBiosPathKey = "bios_path";
constexpr const char* kMsx1BiosPathKey = "bios_msx1";
constexpr const char* kMsx2BiosPathKey = "bios_msx2";
constexpr const char* kMsx2SubRomPathKey = "bios_sub2";
constexpr MsxInternalViewMode kMsxDefaultInternalViewMode = MsxInternalViewMode::Wide;
constexpr MsxMachineMode kMsxDefaultMachineMode = MsxMachineMode::Auto;

MsxInternalViewMode s_internalViewMode = kMsxDefaultInternalViewMode;
MsxInternalViewMode s_viewModeOverride = kMsxDefaultInternalViewMode;
bool s_viewModeOverrideEnabled = false;
MsxMachineMode s_machineMode = kMsxDefaultMachineMode;
char s_genericBiosPath[96] = {0};
char s_msx1BiosPath[96] = {0};
char s_msx2BiosPath[96] = {0};
char s_msx2SubRomPath[96] = {0};

MsxInternalViewMode msx_sanitize_internal_view_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(MsxInternalViewMode::PixelPerfect)
               ? MsxInternalViewMode::PixelPerfect
               : MsxInternalViewMode::Wide;
}

MsxMachineMode msx_sanitize_machine_mode(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxMachineMode::MSX1):
            return MsxMachineMode::MSX1;
        case static_cast<uint8_t>(MsxMachineMode::MSX2):
            return MsxMachineMode::MSX2;
        default:
            return MsxMachineMode::Auto;
    }
}

void msx_copy_path(char* dst, size_t dstSize, const char* src)
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

const char* msx_load_path_key(const char* key, char* dst, size_t dstSize)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const String value = prefs.getString(key, "");
    prefs.end();

    msx_copy_path(dst, dstSize, value.c_str());
    return dst;
}

void msx_save_path_key(const char* key, char* dst, size_t dstSize, const char* path, bool persist)
{
    msx_copy_path(dst, dstSize, path ? path : "");
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putString(key, dst);
    prefs.end();
}

void msx_store_internal_view_mode(MsxInternalViewMode mode)
{
    s_internalViewMode = msx_sanitize_internal_view_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxViewKey, static_cast<uint8_t>(s_internalViewMode));
    prefs.end();
}

void msx_store_machine_mode(MsxMachineMode mode)
{
    s_machineMode = msx_sanitize_machine_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxMachineKey, static_cast<uint8_t>(s_machineMode));
    prefs.end();
}

} // namespace

MsxInternalViewMode msx_config_load_internal_view_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxViewKey);
    const uint8_t saved = prefs.getUChar(
        kMsxViewKey,
        static_cast<uint8_t>(kMsxDefaultInternalViewMode)
    );
    prefs.end();

    s_internalViewMode = hasSavedValue
                             ? msx_sanitize_internal_view_mode(saved)
                             : kMsxDefaultInternalViewMode;

    if (!hasSavedValue) {
        msx_store_internal_view_mode(s_internalViewMode);
    }

    return s_internalViewMode;
}

MsxInternalViewMode msx_config_get_internal_view_mode(void)
{
    return s_internalViewMode;
}

MsxInternalViewMode msx_config_get_active_view_mode(void)
{
    return s_viewModeOverrideEnabled ? s_viewModeOverride : s_internalViewMode;
}

const char* msx_config_internal_view_mode_label(MsxInternalViewMode mode)
{
    return mode == MsxInternalViewMode::PixelPerfect ? "CROP" : "WIDE";
}

const char* msx_config_get_internal_view_mode_label(void)
{
    return msx_config_internal_view_mode_label(s_internalViewMode);
}

const char* msx_config_get_active_view_mode_label(void)
{
    return msx_config_internal_view_mode_label(msx_config_get_active_view_mode());
}

void msx_config_set_internal_view_mode(MsxInternalViewMode mode, bool persist)
{
    s_internalViewMode = msx_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    if (persist) {
        msx_store_internal_view_mode(s_internalViewMode);
    }
}

void msx_config_toggle_internal_view_mode(void)
{
    const MsxInternalViewMode nextMode =
        (s_internalViewMode == MsxInternalViewMode::PixelPerfect)
            ? MsxInternalViewMode::Wide
            : MsxInternalViewMode::PixelPerfect;
    msx_config_set_internal_view_mode(nextMode, true);
}

void msx_config_set_view_mode_override(MsxInternalViewMode mode)
{
    s_viewModeOverride = msx_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    s_viewModeOverrideEnabled = true;
}

void msx_config_clear_view_mode_override(void)
{
    s_viewModeOverrideEnabled = false;
    s_viewModeOverride = s_internalViewMode;
}

void msx_config_toggle_active_view_mode(void)
{
    const MsxInternalViewMode nextMode =
        (msx_config_get_active_view_mode() == MsxInternalViewMode::PixelPerfect)
            ? MsxInternalViewMode::Wide
            : MsxInternalViewMode::PixelPerfect;

    if (s_viewModeOverrideEnabled) {
        s_viewModeOverride = nextMode;
        return;
    }

    msx_config_set_internal_view_mode(nextMode, true);
}

MsxMachineMode msx_config_load_machine_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxMachineKey);
    const uint8_t saved = prefs.getUChar(
        kMsxMachineKey,
        static_cast<uint8_t>(kMsxDefaultMachineMode)
    );
    prefs.end();

    s_machineMode = hasSavedValue
                        ? msx_sanitize_machine_mode(saved)
                        : kMsxDefaultMachineMode;

    if (!hasSavedValue) {
        msx_store_machine_mode(s_machineMode);
    }

    return s_machineMode;
}

MsxMachineMode msx_config_get_machine_mode(void)
{
    return s_machineMode;
}

const char* msx_config_machine_mode_label(MsxMachineMode mode)
{
    switch (mode) {
        case MsxMachineMode::MSX1:
            return "MSX1";
        case MsxMachineMode::MSX2:
            return "MSX2";
        default:
            return "AUTO";
    }
}

const char* msx_config_get_machine_mode_label(void)
{
    return msx_config_machine_mode_label(s_machineMode);
}

void msx_config_set_machine_mode(MsxMachineMode mode, bool persist)
{
    s_machineMode = msx_sanitize_machine_mode(static_cast<uint8_t>(mode));
    if (persist) {
        msx_store_machine_mode(s_machineMode);
    }
}

const char* msx_config_load_bios_path(void)
{
    return msx_load_path_key(kMsxBiosPathKey, s_genericBiosPath, sizeof(s_genericBiosPath));
}

const char* msx_config_get_bios_path(void)
{
    return s_genericBiosPath;
}

void msx_config_set_bios_path(const char* path, bool persist)
{
    msx_save_path_key(kMsxBiosPathKey, s_genericBiosPath, sizeof(s_genericBiosPath), path, persist);
}

const char* msx_config_load_msx1_bios_path(void)
{
    return msx_load_path_key(kMsx1BiosPathKey, s_msx1BiosPath, sizeof(s_msx1BiosPath));
}

const char* msx_config_get_msx1_bios_path(void)
{
    return s_msx1BiosPath;
}

void msx_config_set_msx1_bios_path(const char* path, bool persist)
{
    msx_save_path_key(kMsx1BiosPathKey, s_msx1BiosPath, sizeof(s_msx1BiosPath), path, persist);
}

const char* msx_config_load_msx2_bios_path(void)
{
    return msx_load_path_key(kMsx2BiosPathKey, s_msx2BiosPath, sizeof(s_msx2BiosPath));
}

const char* msx_config_get_msx2_bios_path(void)
{
    return s_msx2BiosPath;
}

void msx_config_set_msx2_bios_path(const char* path, bool persist)
{
    msx_save_path_key(kMsx2BiosPathKey, s_msx2BiosPath, sizeof(s_msx2BiosPath), path, persist);
}

const char* msx_config_load_msx2_subrom_path(void)
{
    return msx_load_path_key(kMsx2SubRomPathKey, s_msx2SubRomPath, sizeof(s_msx2SubRomPath));
}

const char* msx_config_get_msx2_subrom_path(void)
{
    return s_msx2SubRomPath;
}

void msx_config_set_msx2_subrom_path(const char* path, bool persist)
{
    msx_save_path_key(kMsx2SubRomPathKey, s_msx2SubRomPath, sizeof(s_msx2SubRomPath), path, persist);
}
