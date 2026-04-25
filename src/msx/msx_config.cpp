#include "msx_config.h"

#include <Preferences.h>

#include <cstring>

namespace {

constexpr const char* kMsxConfigNs = "msx_cfg";
constexpr const char* kMsxViewKey = "int_view";
constexpr const char* kMsxMachineKey = "machine";
constexpr const char* kMsxPerformanceKey = "perf_mode";
constexpr const char* kMsxPerformanceFlagsKey = "perf_flags";
constexpr const char* kMsxBiosPathKey = "bios_path";
constexpr const char* kMsx1BiosPathKey = "bios_msx1";
constexpr const char* kMsx2BiosPathKey = "bios_msx2";
constexpr const char* kMsx2SubRomPathKey = "bios_sub2";
constexpr MsxInternalViewMode kMsxDefaultInternalViewMode = MsxInternalViewMode::Wide;
constexpr MsxMachineMode kMsxDefaultMachineMode = MsxMachineMode::MSX2;
constexpr MsxPerformanceMode kMsxDefaultPerformanceMode = MsxPerformanceMode::Accurate;
constexpr uint8_t kMsxDefaultPerformanceFlags = 0u;
constexpr uint8_t kMsxFastPerformancePresetFlags =
    static_cast<uint8_t>(MsxPerformanceFlag::DisableSpriteCollision) |
    static_cast<uint8_t>(MsxPerformanceFlag::SimplifySpriteOverflow) |
    static_cast<uint8_t>(MsxPerformanceFlag::InstantVdpCommands);

MsxInternalViewMode s_internalViewMode = kMsxDefaultInternalViewMode;
MsxInternalViewMode s_viewModeOverride = kMsxDefaultInternalViewMode;
bool s_viewModeOverrideEnabled = false;
MsxMachineMode s_machineMode = kMsxDefaultMachineMode;
MsxPerformanceMode s_performanceMode = kMsxDefaultPerformanceMode;
uint8_t s_performanceFlags = kMsxDefaultPerformanceFlags;
char s_genericBiosPath[96] = {0};
char s_msx1BiosPath[96] = {0};
char s_msx2BiosPath[96] = {0};
char s_msx2SubRomPath[96] = {0};

MsxInternalViewMode msx_sanitize_internal_view_mode(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxInternalViewMode::PixelPerfect):
            return MsxInternalViewMode::PixelPerfect;
        case static_cast<uint8_t>(MsxInternalViewMode::FastPlus):
            return MsxInternalViewMode::FastPlus;
        case static_cast<uint8_t>(MsxInternalViewMode::Wide):
        default:
            return MsxInternalViewMode::Wide;
    }
}

MsxMachineMode msx_sanitize_machine_mode(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxMachineMode::MSX1):
            return MsxMachineMode::MSX1;
        case static_cast<uint8_t>(MsxMachineMode::MSX2):
            return MsxMachineMode::MSX2;
        default:
            return kMsxDefaultMachineMode;
    }
}

MsxPerformanceMode msx_sanitize_performance_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(MsxPerformanceMode::Performance)
               ? MsxPerformanceMode::Performance
               : MsxPerformanceMode::Accurate;
}

uint8_t msx_sanitize_performance_flags(uint8_t value)
{
    const uint8_t supportedFlags =
        static_cast<uint8_t>(MsxPerformanceFlag::DisableSliceRendering) |
        static_cast<uint8_t>(MsxPerformanceFlag::DisableSpriteCollision) |
        static_cast<uint8_t>(MsxPerformanceFlag::SimplifySpriteOverflow) |
        static_cast<uint8_t>(MsxPerformanceFlag::InstantVdpCommands) |
        static_cast<uint8_t>(MsxPerformanceFlag::ExternalFixed30Fps);
    return static_cast<uint8_t>(value & supportedFlags);
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

void msx_store_performance_mode(MsxPerformanceMode mode)
{
    s_performanceMode = msx_sanitize_performance_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxPerformanceKey, static_cast<uint8_t>(s_performanceMode));
    prefs.end();
}

void msx_update_performance_mode_from_flags(void)
{
    s_performanceFlags = msx_sanitize_performance_flags(s_performanceFlags);
    s_performanceMode = s_performanceFlags == 0u
                            ? MsxPerformanceMode::Accurate
                            : MsxPerformanceMode::Performance;
}

void msx_store_performance_flags(uint8_t flags)
{
    s_performanceFlags = msx_sanitize_performance_flags(flags);
    msx_update_performance_mode_from_flags();

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxPerformanceFlagsKey, s_performanceFlags);
    prefs.putUChar(kMsxPerformanceKey, static_cast<uint8_t>(s_performanceMode));
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
    switch (mode) {
        case MsxInternalViewMode::PixelPerfect:
            return "CROP";
        case MsxInternalViewMode::FastPlus:
            return "WIDE+";
        case MsxInternalViewMode::Wide:
        default:
            return "WIDE";
    }
}

const char* msx_config_view_mode_label_for_target(MsxInternalViewMode mode, bool useExternal)
{
    if (useExternal) {
        switch (mode) {
            case MsxInternalViewMode::PixelPerfect:
                return "1:1";
            case MsxInternalViewMode::FastPlus:
                return "FAST+";
            case MsxInternalViewMode::Wide:
            default:
                return "FAST";
        }
    }

    return msx_config_internal_view_mode_label(mode);
}

const char* msx_config_get_internal_view_mode_label(void)
{
    return msx_config_internal_view_mode_label(s_internalViewMode);
}

const char* msx_config_get_active_view_mode_label(void)
{
    return msx_config_internal_view_mode_label(msx_config_get_active_view_mode());
}

const char* msx_config_get_active_view_mode_label_for_target(bool useExternal)
{
    return msx_config_view_mode_label_for_target(msx_config_get_active_view_mode(), useExternal);
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

void msx_config_toggle_active_view_mode_for_target(bool useExternal)
{
    if (!useExternal) {
        msx_config_toggle_active_view_mode();
        return;
    }

    const MsxInternalViewMode currentMode = msx_config_get_active_view_mode();
    const MsxInternalViewMode nextMode =
        (currentMode == MsxInternalViewMode::PixelPerfect)
            ? MsxInternalViewMode::Wide
            : (currentMode == MsxInternalViewMode::Wide)
                  ? MsxInternalViewMode::FastPlus
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

    const bool needsMigration =
        !hasSavedValue ||
        saved != static_cast<uint8_t>(s_machineMode);
    if (needsMigration) {
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

MsxPerformanceMode msx_config_load_performance_mode(void)
{
    msx_config_load_performance_flags();
    return s_performanceMode;
}

MsxPerformanceMode msx_config_get_performance_mode_value(void)
{
    return s_performanceMode;
}

bool msx_config_get_performance_mode(void)
{
    return s_performanceFlags != 0u;
}

const char* msx_config_performance_mode_label(MsxPerformanceMode mode)
{
    return mode == MsxPerformanceMode::Performance ? "FAST" : "NORMAL";
}

const char* msx_config_get_performance_mode_label(void)
{
    if (s_performanceFlags == 0u) {
        return "NORMAL";
    }
    if (s_performanceFlags == kMsxFastPerformancePresetFlags) {
        return "FAST";
    }
    return "CUSTOM";
}

uint8_t msx_config_load_performance_flags(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedFlags = prefs.isKey(kMsxPerformanceFlagsKey);
    const bool hasLegacyMode = prefs.isKey(kMsxPerformanceKey);
    const uint8_t savedFlags = prefs.getUChar(kMsxPerformanceFlagsKey, kMsxDefaultPerformanceFlags);
    const uint8_t legacyMode = prefs.getUChar(
        kMsxPerformanceKey,
        static_cast<uint8_t>(kMsxDefaultPerformanceMode)
    );
    prefs.end();

    if (hasSavedFlags) {
        s_performanceFlags = msx_sanitize_performance_flags(savedFlags);
    } else if (hasLegacyMode &&
               msx_sanitize_performance_mode(legacyMode) == MsxPerformanceMode::Performance) {
        s_performanceFlags = kMsxFastPerformancePresetFlags;
    } else {
        s_performanceFlags = kMsxDefaultPerformanceFlags;
    }
    msx_update_performance_mode_from_flags();

    if (!hasSavedFlags) {
        msx_store_performance_flags(s_performanceFlags);
    }

    return s_performanceFlags;
}

uint8_t msx_config_get_performance_flags(void)
{
    return s_performanceFlags;
}

bool msx_config_get_performance_flag(MsxPerformanceFlag flag)
{
    const uint8_t mask = static_cast<uint8_t>(flag);
    return (s_performanceFlags & mask) != 0u;
}

void msx_config_set_performance_flags(uint8_t flags, bool persist)
{
    s_performanceFlags = msx_sanitize_performance_flags(flags);
    msx_update_performance_mode_from_flags();
    if (persist) {
        msx_store_performance_flags(s_performanceFlags);
    }
}

void msx_config_set_performance_flag(MsxPerformanceFlag flag, bool enabled, bool persist)
{
    const uint8_t mask = static_cast<uint8_t>(flag);
    const uint8_t nextFlags = enabled
                                  ? static_cast<uint8_t>(s_performanceFlags | mask)
                                  : static_cast<uint8_t>(s_performanceFlags & ~mask);
    msx_config_set_performance_flags(nextFlags, persist);
}

void msx_config_set_performance_mode(bool enabled, bool persist)
{
    msx_config_set_performance_flags(enabled ? kMsxFastPerformancePresetFlags
                                             : kMsxDefaultPerformanceFlags,
                                     persist);
}

void msx_config_toggle_performance_mode(void)
{
    msx_config_set_performance_mode(!msx_config_get_performance_mode(), true);
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
