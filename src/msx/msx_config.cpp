#include "msx_config.h"

#include <Preferences.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kMsxConfigNs = "msx_cfg";
constexpr const char* kMsxViewKey = "int_view";
constexpr const char* kMsxMachineKey = "machine";
constexpr const char* kMsxPerformanceKey = "perf_mode";
constexpr const char* kMsxPerformanceFlagsKey = "perf_flags";
constexpr const char* kMsxPerformancePresetKey = "perf_preset";
constexpr const char* kMsxFrameskipKey = "frameskip";
constexpr const char* kMsxFpsOverlayKey = "fps_hud";
constexpr const char* kMsxFpsOverlayModeKey = "fps_hud_mode";
constexpr const char* kMsxVirtualSccKey = "virt_scc";
constexpr const char* kMsxSccHardwareDetectKey = "scc_hdw";
constexpr const char* kMsxRegionModeKey = "region";
constexpr const char* kMsxSoundVolumeKey = "snd_vol";
constexpr const char* kMsxSccGainKey = "scc_gain";
constexpr const char* kMsxSccGainDefaultV2Key = "scc_gain_v2";
constexpr const char* kMsxBiosPathKey = "bios_path";
constexpr const char* kMsx1BiosPathKey = "bios_msx1";
constexpr const char* kMsx2BiosPathKey = "bios_msx2";
constexpr const char* kMsx2SubRomPathKey = "bios_sub2";
constexpr MsxInternalViewMode kMsxDefaultInternalViewMode = MsxInternalViewMode::Wide;
constexpr MsxMachineMode kMsxDefaultMachineMode = MsxMachineMode::MSX2;
constexpr uint8_t kMsxLegacyFastPerformancePresetFlags =
    static_cast<uint8_t>(MsxPerformanceFlag::DisableSpriteCollision) |
    static_cast<uint8_t>(MsxPerformanceFlag::SimplifySpriteOverflow) |
    static_cast<uint8_t>(MsxPerformanceFlag::InstantVdpCommands);
constexpr uint8_t kMsxFastPerformancePresetFlags =
    static_cast<uint8_t>(MsxPerformanceFlag::DisableSliceRendering) |
    static_cast<uint8_t>(MsxPerformanceFlag::DisableSpriteCollision) |
    static_cast<uint8_t>(MsxPerformanceFlag::SimplifySpriteOverflow) |
    static_cast<uint8_t>(MsxPerformanceFlag::InstantVdpCommands) |
    static_cast<uint8_t>(MsxPerformanceFlag::ExternalFixed30Fps);
constexpr uint8_t kMsxManbowPerformancePresetFlags =
    kMsxLegacyFastPerformancePresetFlags |
    static_cast<uint8_t>(MsxPerformanceFlag::ExternalFixed30Fps);
constexpr MsxPerformanceMode kMsxDefaultPerformanceMode = MsxPerformanceMode::Performance;
constexpr MsxPerformancePreset kMsxDefaultPerformancePreset = MsxPerformancePreset::Fast;
constexpr uint8_t kMsxDefaultPerformanceFlags = kMsxFastPerformancePresetFlags;
constexpr MsxFrameskipMode kMsxDefaultFrameskipMode = MsxFrameskipMode::Adaptive;
constexpr bool kMsxDefaultFpsOverlayEnabled = true;
constexpr MsxFpsOverlayMode kMsxDefaultFpsOverlayMode = MsxFpsOverlayMode::Simple;
constexpr MsxVirtualSccMode kMsxDefaultVirtualSccMode = MsxVirtualSccMode::Off;
constexpr bool kMsxDefaultSccHardwareDetectEnabled = false;
constexpr MsxRegionMode kMsxDefaultRegionMode = MsxRegionMode::Auto;
constexpr uint8_t kMsxDefaultSoundVolume = 112;
constexpr uint16_t kMsxLegacyDefaultSccGainPercent = 150;
constexpr uint16_t kMsxDefaultSccGainPercent = 300;
constexpr uint16_t kMsxMinSccGainPercent = 0;
constexpr uint16_t kMsxMaxSccGainPercent = 300;

MsxInternalViewMode s_internalViewMode = kMsxDefaultInternalViewMode;
MsxInternalViewMode s_viewModeOverride = kMsxDefaultInternalViewMode;
bool s_viewModeOverrideEnabled = false;
MsxMachineMode s_machineMode = kMsxDefaultMachineMode;
MsxMachineMode s_machineModeSessionOverride = kMsxDefaultMachineMode;
bool s_machineModeSessionOverrideEnabled = false;
MsxPerformanceMode s_performanceMode = kMsxDefaultPerformanceMode;
MsxPerformancePreset s_performancePreset = kMsxDefaultPerformancePreset;
uint8_t s_performanceFlags = kMsxDefaultPerformanceFlags;
MsxFrameskipMode s_frameskipMode = kMsxDefaultFrameskipMode;
MsxFpsOverlayMode s_fpsOverlayMode = kMsxDefaultFpsOverlayMode;
MsxVirtualSccMode s_virtualSccMode = kMsxDefaultVirtualSccMode;
bool s_sccHardwareDetectEnabled = kMsxDefaultSccHardwareDetectEnabled;
MsxRegionMode s_regionMode = kMsxDefaultRegionMode;
uint8_t s_soundVolume = kMsxDefaultSoundVolume;
uint16_t s_sccGainPercent = kMsxDefaultSccGainPercent;
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

MsxPerformancePreset msx_sanitize_performance_preset(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxPerformancePreset::Normal):
            return MsxPerformancePreset::Normal;
        case static_cast<uint8_t>(MsxPerformancePreset::Fast):
            return MsxPerformancePreset::Fast;
        case static_cast<uint8_t>(MsxPerformancePreset::Manbow):
            return MsxPerformancePreset::Manbow;
        case static_cast<uint8_t>(MsxPerformancePreset::Custom):
            return MsxPerformancePreset::Custom;
        default:
            return kMsxDefaultPerformancePreset;
    }
}

uint8_t msx_performance_preset_flags(MsxPerformancePreset preset)
{
    switch (preset) {
        case MsxPerformancePreset::Normal:
            return 0u;
        case MsxPerformancePreset::Fast:
            return kMsxFastPerformancePresetFlags;
        case MsxPerformancePreset::Manbow:
            return kMsxManbowPerformancePresetFlags;
        case MsxPerformancePreset::Custom:
        default:
            return s_performanceFlags;
    }
}

MsxPerformancePreset msx_performance_preset_from_flags(uint8_t flags)
{
    flags = msx_sanitize_performance_flags(flags);
    if (flags == 0u) {
        return MsxPerformancePreset::Normal;
    }
    if (flags == kMsxFastPerformancePresetFlags) {
        return MsxPerformancePreset::Fast;
    }
    if (flags == kMsxManbowPerformancePresetFlags) {
        return MsxPerformancePreset::Manbow;
    }
    if (flags == kMsxLegacyFastPerformancePresetFlags) {
        return MsxPerformancePreset::Manbow;
    }
    return MsxPerformancePreset::Custom;
}

MsxFrameskipMode msx_sanitize_frameskip_mode(uint8_t value)
{
    if (value < static_cast<uint8_t>(MsxFrameskipMode::Count)) {
        return static_cast<MsxFrameskipMode>(value);
    }
    return kMsxDefaultFrameskipMode;
}

MsxFpsOverlayMode msx_sanitize_fps_overlay_mode(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxFpsOverlayMode::Off):
            return MsxFpsOverlayMode::Off;
        case static_cast<uint8_t>(MsxFpsOverlayMode::Dual):
            return MsxFpsOverlayMode::Dual;
        case static_cast<uint8_t>(MsxFpsOverlayMode::Simple):
        default:
            return MsxFpsOverlayMode::Simple;
    }
}

MsxRegionMode msx_sanitize_region_mode(uint8_t value)
{
    if (value < static_cast<uint8_t>(MsxRegionMode::Count)) {
        return static_cast<MsxRegionMode>(value);
    }
    return kMsxDefaultRegionMode;
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

MsxVirtualSccMode msx_sanitize_virtual_scc_mode(uint8_t value)
{
    switch (value) {
        case static_cast<uint8_t>(MsxVirtualSccMode::Scc):
            return MsxVirtualSccMode::Scc;
        case static_cast<uint8_t>(MsxVirtualSccMode::SccI):
            return MsxVirtualSccMode::SccI;
        case static_cast<uint8_t>(MsxVirtualSccMode::Off):
        default:
            return MsxVirtualSccMode::Off;
    }
}

uint16_t msx_sanitize_scc_gain_percent(uint16_t value)
{
    if (value < kMsxMinSccGainPercent) {
        return kMsxMinSccGainPercent;
    }
    if (value > kMsxMaxSccGainPercent) {
        return kMsxMaxSccGainPercent;
    }
    return value;
}

void msx_store_performance_flags(uint8_t flags)
{
    s_performanceFlags = msx_sanitize_performance_flags(flags);
    msx_update_performance_mode_from_flags();

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxPerformanceFlagsKey, s_performanceFlags);
    prefs.putUChar(kMsxPerformanceKey, static_cast<uint8_t>(s_performanceMode));
    prefs.putUChar(kMsxPerformancePresetKey, static_cast<uint8_t>(s_performancePreset));
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

    if (s_machineModeSessionOverrideEnabled) {
        s_machineMode = s_machineModeSessionOverride;
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

void msx_config_set_machine_mode_session_override(MsxMachineMode mode, bool enabled)
{
    s_machineModeSessionOverride = msx_sanitize_machine_mode(static_cast<uint8_t>(mode));
    s_machineModeSessionOverrideEnabled = enabled;
    if (enabled) {
        s_machineMode = s_machineModeSessionOverride;
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

MsxPerformancePreset msx_config_get_performance_preset(void)
{
    return s_performancePreset;
}

const char* msx_config_performance_preset_label(MsxPerformancePreset preset)
{
    switch (preset) {
        case MsxPerformancePreset::Normal:
            return "NORMAL";
        case MsxPerformancePreset::Fast:
            return "FAST";
        case MsxPerformancePreset::Manbow:
            return "SCLINE";
        case MsxPerformancePreset::Custom:
        default:
            return "CUSTOM";
    }
}

const char* msx_config_get_performance_mode_label(void)
{
    return msx_config_performance_preset_label(s_performancePreset);
}

uint8_t msx_config_load_performance_flags(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedFlags = prefs.isKey(kMsxPerformanceFlagsKey);
    const bool hasLegacyMode = prefs.isKey(kMsxPerformanceKey);
    const bool hasSavedPreset = prefs.isKey(kMsxPerformancePresetKey);
    const uint8_t savedFlags = prefs.getUChar(kMsxPerformanceFlagsKey, kMsxDefaultPerformanceFlags);
    const uint8_t savedPreset = prefs.getUChar(
        kMsxPerformancePresetKey,
        static_cast<uint8_t>(kMsxDefaultPerformancePreset)
    );
    const uint8_t legacyMode = prefs.getUChar(
        kMsxPerformanceKey,
        static_cast<uint8_t>(kMsxDefaultPerformanceMode)
    );
    prefs.end();

    if (hasSavedFlags) {
        s_performanceFlags = msx_sanitize_performance_flags(savedFlags);
        s_performancePreset = hasSavedPreset
                                  ? msx_sanitize_performance_preset(savedPreset)
                                  : msx_performance_preset_from_flags(s_performanceFlags);
    } else if (hasLegacyMode &&
               msx_sanitize_performance_mode(legacyMode) == MsxPerformanceMode::Performance) {
        s_performanceFlags = kMsxFastPerformancePresetFlags;
        s_performancePreset = MsxPerformancePreset::Fast;
    } else {
        s_performanceFlags = kMsxDefaultPerformanceFlags;
        s_performancePreset = kMsxDefaultPerformancePreset;
    }

    if (s_performancePreset != MsxPerformancePreset::Custom) {
        s_performanceFlags = msx_performance_preset_flags(s_performancePreset);
    }
    msx_update_performance_mode_from_flags();

    if (!hasSavedFlags || !hasSavedPreset) {
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
    s_performancePreset = msx_performance_preset_from_flags(s_performanceFlags);
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
    msx_config_set_performance_preset(enabled ? MsxPerformancePreset::Fast
                                              : MsxPerformancePreset::Normal,
                                      persist);
}

void msx_config_set_performance_preset(MsxPerformancePreset preset, bool persist)
{
    s_performancePreset = msx_sanitize_performance_preset(static_cast<uint8_t>(preset));
    if (s_performancePreset != MsxPerformancePreset::Custom) {
        s_performanceFlags = msx_performance_preset_flags(s_performancePreset);
    }
    msx_update_performance_mode_from_flags();
    if (persist) {
        msx_store_performance_flags(s_performanceFlags);
    }
}

void msx_config_cycle_performance_preset(int delta, bool persist)
{
    int preset = static_cast<int>(s_performancePreset);
    preset = (preset + (delta >= 0 ? 1 : -1) + 4) % 4;
    msx_config_set_performance_preset(
        static_cast<MsxPerformancePreset>(preset),
        persist
    );
}

void msx_config_toggle_performance_mode(void)
{
    msx_config_set_performance_mode(!msx_config_get_performance_mode(), true);
}

MsxFrameskipMode msx_config_load_frameskip_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxFrameskipKey);
    const uint8_t savedValue = prefs.getUChar(
        kMsxFrameskipKey,
        static_cast<uint8_t>(kMsxDefaultFrameskipMode)
    );
    prefs.end();

    s_frameskipMode = hasSavedValue
                          ? msx_sanitize_frameskip_mode(savedValue)
                          : kMsxDefaultFrameskipMode;

    if (!hasSavedValue) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUChar(kMsxFrameskipKey, static_cast<uint8_t>(s_frameskipMode));
        writePrefs.end();
    }

    return s_frameskipMode;
}

MsxFrameskipMode msx_config_get_frameskip_mode(void)
{
    return s_frameskipMode;
}

const char* msx_config_frameskip_mode_label(MsxFrameskipMode mode)
{
    switch (mode) {
        case MsxFrameskipMode::Ratio12: return "1/1.2";
        case MsxFrameskipMode::Ratio13: return "1/1.3";
        case MsxFrameskipMode::Ratio14: return "1/1.4";
        case MsxFrameskipMode::Ratio15: return "1/1.5";
        case MsxFrameskipMode::Ratio2: return "1/2";
        case MsxFrameskipMode::Ratio3: return "1/3";
        case MsxFrameskipMode::Ratio4: return "1/4";
        case MsxFrameskipMode::Adaptive:
        default:
            return "ADAPT";
    }
}

const char* msx_config_get_frameskip_mode_label(void)
{
    return msx_config_frameskip_mode_label(s_frameskipMode);
}

uint8_t msx_config_frameskip_mode_skip_numerator(MsxFrameskipMode mode)
{
    switch (mode) {
        case MsxFrameskipMode::Ratio12: return 5u;
        case MsxFrameskipMode::Ratio13: return 10u;
        case MsxFrameskipMode::Ratio14: return 5u;
        case MsxFrameskipMode::Ratio15: return 2u;
        case MsxFrameskipMode::Ratio2:
        case MsxFrameskipMode::Ratio3:
        case MsxFrameskipMode::Ratio4:
            return 1u;
        case MsxFrameskipMode::Adaptive:
        default:
            return 0u;
    }
}

uint8_t msx_config_frameskip_mode_skip_denominator(MsxFrameskipMode mode)
{
    switch (mode) {
        case MsxFrameskipMode::Ratio12: return 6u;
        case MsxFrameskipMode::Ratio13: return 13u;
        case MsxFrameskipMode::Ratio14: return 7u;
        case MsxFrameskipMode::Ratio15: return 3u;
        case MsxFrameskipMode::Ratio2: return 2u;
        case MsxFrameskipMode::Ratio3: return 3u;
        case MsxFrameskipMode::Ratio4: return 4u;
        case MsxFrameskipMode::Adaptive:
        default:
            return 0u;
    }
}

void msx_config_set_frameskip_mode(MsxFrameskipMode mode, bool persist)
{
    s_frameskipMode = msx_sanitize_frameskip_mode(static_cast<uint8_t>(mode));
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxFrameskipKey, static_cast<uint8_t>(s_frameskipMode));
    prefs.end();
}

void msx_config_cycle_frameskip_mode(int delta, bool persist)
{
    constexpr MsxFrameskipMode kOrder[] = {
        MsxFrameskipMode::Adaptive,
        MsxFrameskipMode::Ratio12,
        MsxFrameskipMode::Ratio13,
        MsxFrameskipMode::Ratio14,
        MsxFrameskipMode::Ratio15,
        MsxFrameskipMode::Ratio2,
        MsxFrameskipMode::Ratio3,
        MsxFrameskipMode::Ratio4,
    };
    constexpr int count = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if (kOrder[i] == s_frameskipMode) {
            current = i;
            break;
        }
    }

    int next = current + delta;
    while (next < 0) {
        next += count;
    }
    next %= count;
    msx_config_set_frameskip_mode(kOrder[next], persist);
}

bool msx_config_load_fps_overlay_enabled(void)
{
    return msx_config_load_fps_overlay_mode() != MsxFpsOverlayMode::Off;
}

bool msx_config_get_fps_overlay_enabled(void)
{
    return s_fpsOverlayMode != MsxFpsOverlayMode::Off;
}

void msx_config_set_fps_overlay_enabled(bool enabled, bool persist)
{
    msx_config_set_fps_overlay_mode(
        enabled ? MsxFpsOverlayMode::Simple : MsxFpsOverlayMode::Off,
        persist
    );
}

MsxFpsOverlayMode msx_config_load_fps_overlay_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedMode = prefs.isKey(kMsxFpsOverlayModeKey);
    const uint8_t savedMode = prefs.getUChar(
        kMsxFpsOverlayModeKey,
        static_cast<uint8_t>(kMsxDefaultFpsOverlayMode)
    );
    const bool hasLegacyValue = prefs.isKey(kMsxFpsOverlayKey);
    const bool legacyValue = prefs.getBool(kMsxFpsOverlayKey, kMsxDefaultFpsOverlayEnabled);
    prefs.end();

    if (hasSavedMode) {
        s_fpsOverlayMode = msx_sanitize_fps_overlay_mode(savedMode);
    } else if (hasLegacyValue) {
        s_fpsOverlayMode = legacyValue ? MsxFpsOverlayMode::Simple : MsxFpsOverlayMode::Off;
    } else {
        s_fpsOverlayMode = kMsxDefaultFpsOverlayMode;
    }

    if (!hasSavedMode || savedMode != static_cast<uint8_t>(s_fpsOverlayMode)) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUChar(kMsxFpsOverlayModeKey, static_cast<uint8_t>(s_fpsOverlayMode));
        writePrefs.end();
    }

    return s_fpsOverlayMode;
}

MsxFpsOverlayMode msx_config_get_fps_overlay_mode(void)
{
    return s_fpsOverlayMode;
}

const char* msx_config_fps_overlay_mode_label(MsxFpsOverlayMode mode)
{
    switch (mode) {
        case MsxFpsOverlayMode::Off:
            return "OFF";
        case MsxFpsOverlayMode::Dual:
            return "DUAL";
        case MsxFpsOverlayMode::Simple:
        default:
            return "SIMPLE";
    }
}

const char* msx_config_get_fps_overlay_mode_label(void)
{
    return msx_config_fps_overlay_mode_label(s_fpsOverlayMode);
}

void msx_config_set_fps_overlay_mode(MsxFpsOverlayMode mode, bool persist)
{
    s_fpsOverlayMode = msx_sanitize_fps_overlay_mode(static_cast<uint8_t>(mode));
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxFpsOverlayModeKey, static_cast<uint8_t>(s_fpsOverlayMode));
    prefs.end();
}

void msx_config_cycle_fps_overlay_mode(int delta, bool persist)
{
    constexpr MsxFpsOverlayMode kOrder[] = {
        MsxFpsOverlayMode::Off,
        MsxFpsOverlayMode::Simple,
        MsxFpsOverlayMode::Dual,
    };
    constexpr int count = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if (kOrder[i] == s_fpsOverlayMode) {
            current = i;
            break;
        }
    }

    int next = current + delta;
    while (next < 0) {
        next += count;
    }
    next %= count;
    msx_config_set_fps_overlay_mode(kOrder[next], persist);
}

MsxVirtualSccMode msx_config_load_virtual_scc_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxVirtualSccKey);
    const uint8_t savedValue = prefs.getUChar(
        kMsxVirtualSccKey,
        static_cast<uint8_t>(kMsxDefaultVirtualSccMode)
    );
    prefs.end();

    s_virtualSccMode = hasSavedValue
                           ? msx_sanitize_virtual_scc_mode(savedValue)
                           : kMsxDefaultVirtualSccMode;

    if (!hasSavedValue || savedValue != static_cast<uint8_t>(s_virtualSccMode)) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUChar(kMsxVirtualSccKey, static_cast<uint8_t>(s_virtualSccMode));
        writePrefs.end();
    }

    return s_virtualSccMode;
}

MsxVirtualSccMode msx_config_get_virtual_scc_mode(void)
{
    return s_virtualSccMode;
}

const char* msx_config_virtual_scc_mode_label(MsxVirtualSccMode mode)
{
    switch (mode) {
        case MsxVirtualSccMode::Scc:
            return "SCC";
        case MsxVirtualSccMode::SccI:
            return "SCC-I";
        case MsxVirtualSccMode::Off:
        default:
            return "OFF";
    }
}

const char* msx_config_get_virtual_scc_mode_label(void)
{
    return msx_config_virtual_scc_mode_label(s_virtualSccMode);
}

void msx_config_set_virtual_scc_mode(MsxVirtualSccMode mode, bool persist)
{
    s_virtualSccMode = msx_sanitize_virtual_scc_mode(static_cast<uint8_t>(mode));
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxVirtualSccKey, static_cast<uint8_t>(s_virtualSccMode));
    prefs.end();
}

void msx_config_cycle_virtual_scc_mode(int delta, bool persist)
{
    int mode = static_cast<int>(s_virtualSccMode);
    mode = (mode + (delta >= 0 ? 1 : -1) + static_cast<int>(MsxVirtualSccMode::Count)) %
           static_cast<int>(MsxVirtualSccMode::Count);
    msx_config_set_virtual_scc_mode(static_cast<MsxVirtualSccMode>(mode), persist);
}

bool msx_config_load_scc_hardware_detect_enabled(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxSccHardwareDetectKey);
    const bool savedValue = prefs.getBool(kMsxSccHardwareDetectKey,
                                          kMsxDefaultSccHardwareDetectEnabled);
    prefs.end();

    s_sccHardwareDetectEnabled =
        hasSavedValue ? savedValue : kMsxDefaultSccHardwareDetectEnabled;
    if (!hasSavedValue) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putBool(kMsxSccHardwareDetectKey, s_sccHardwareDetectEnabled);
        writePrefs.end();
    }
    return s_sccHardwareDetectEnabled;
}

bool msx_config_get_scc_hardware_detect_enabled(void)
{
    return s_sccHardwareDetectEnabled;
}

void msx_config_set_scc_hardware_detect_enabled(bool enabled, bool persist)
{
    s_sccHardwareDetectEnabled = enabled;
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putBool(kMsxSccHardwareDetectKey, s_sccHardwareDetectEnabled);
    prefs.end();
}

MsxRegionMode msx_config_load_region_mode(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxRegionModeKey);
    const uint8_t savedValue = prefs.getUChar(
        kMsxRegionModeKey,
        static_cast<uint8_t>(kMsxDefaultRegionMode)
    );
    prefs.end();

    s_regionMode = hasSavedValue
                       ? msx_sanitize_region_mode(savedValue)
                       : kMsxDefaultRegionMode;

    if (!hasSavedValue || savedValue != static_cast<uint8_t>(s_regionMode)) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUChar(kMsxRegionModeKey, static_cast<uint8_t>(s_regionMode));
        writePrefs.end();
    }

    return s_regionMode;
}

MsxRegionMode msx_config_get_region_mode(void)
{
    return s_regionMode;
}

const char* msx_config_region_mode_label(MsxRegionMode mode)
{
    switch (mode) {
        case MsxRegionMode::World:
            return "WORLD";
        case MsxRegionMode::Japan:
            return "JAPAN";
        case MsxRegionMode::Auto:
        default:
            return "AUTO";
    }
}

const char* msx_config_region_profile_label(MsxRegionProfile profile)
{
    return profile == MsxRegionProfile::Japan ? "JAPAN" : "WORLD";
}

void msx_config_set_region_mode(MsxRegionMode mode, bool persist)
{
    s_regionMode = msx_sanitize_region_mode(static_cast<uint8_t>(mode));
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxRegionModeKey, static_cast<uint8_t>(s_regionMode));
    prefs.end();
}

void msx_config_cycle_region_mode(int delta, bool persist)
{
    int mode = static_cast<int>(s_regionMode);
    mode = (mode + (delta >= 0 ? 1 : -1) + static_cast<int>(MsxRegionMode::Count)) %
           static_cast<int>(MsxRegionMode::Count);
    msx_config_set_region_mode(static_cast<MsxRegionMode>(mode), persist);
}

uint8_t msx_config_load_sound_volume(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxSoundVolumeKey);
    const uint8_t savedValue = prefs.getUChar(kMsxSoundVolumeKey, kMsxDefaultSoundVolume);
    prefs.end();

    s_soundVolume = hasSavedValue ? savedValue : kMsxDefaultSoundVolume;
    if (!hasSavedValue) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUChar(kMsxSoundVolumeKey, s_soundVolume);
        writePrefs.end();
    }

    return s_soundVolume;
}

uint8_t msx_config_get_sound_volume(void)
{
    return s_soundVolume;
}

const char* msx_config_sound_volume_label(uint8_t volume)
{
    static char label[8];
    std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(volume));
    return label;
}

void msx_config_set_sound_volume(uint8_t volume, bool persist)
{
    s_soundVolume = volume;
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUChar(kMsxSoundVolumeKey, s_soundVolume);
    prefs.end();
}

void msx_config_cycle_sound_volume(int delta, bool persist)
{
    int volume = static_cast<int>(s_soundVolume);
    volume += delta >= 0 ? 8 : -8;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 255) {
        volume = 255;
    }
    msx_config_set_sound_volume(static_cast<uint8_t>(volume), persist);
}

uint16_t msx_config_load_scc_gain_percent(void)
{
    Preferences prefs;
    prefs.begin(kMsxConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kMsxSccGainKey);
    const bool defaultV2Applied = prefs.getBool(kMsxSccGainDefaultV2Key, false);
    const uint16_t savedValue = prefs.getUShort(kMsxSccGainKey, kMsxDefaultSccGainPercent);
    prefs.end();

    const bool migrateLegacyDefault =
        hasSavedValue &&
        !defaultV2Applied &&
        savedValue == kMsxLegacyDefaultSccGainPercent;
    s_sccGainPercent = migrateLegacyDefault
                           ? kMsxDefaultSccGainPercent
                           : (hasSavedValue
                                  ? msx_sanitize_scc_gain_percent(savedValue)
                                  : kMsxDefaultSccGainPercent);

    if (!hasSavedValue || savedValue != s_sccGainPercent || !defaultV2Applied) {
        Preferences writePrefs;
        writePrefs.begin(kMsxConfigNs, false);
        writePrefs.putUShort(kMsxSccGainKey, s_sccGainPercent);
        writePrefs.putBool(kMsxSccGainDefaultV2Key, true);
        writePrefs.end();
    }

    return s_sccGainPercent;
}

uint16_t msx_config_get_scc_gain_percent(void)
{
    return s_sccGainPercent;
}

const char* msx_config_scc_gain_label(uint16_t gainPercent)
{
    static char label[8];
    std::snprintf(label,
                  sizeof(label),
                  "%u.%02ux",
                  static_cast<unsigned>(gainPercent / 100u),
                  static_cast<unsigned>(gainPercent % 100u));
    return label;
}

void msx_config_set_scc_gain_percent(uint16_t gainPercent, bool persist)
{
    s_sccGainPercent = msx_sanitize_scc_gain_percent(gainPercent);
    if (!persist) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxConfigNs, false);
    prefs.putUShort(kMsxSccGainKey, s_sccGainPercent);
    prefs.end();
}

void msx_config_cycle_scc_gain_percent(int delta, bool persist)
{
    int gain = static_cast<int>(s_sccGainPercent);
    gain += delta >= 0 ? 25 : -25;
    if (gain < static_cast<int>(kMsxMinSccGainPercent)) {
        gain = static_cast<int>(kMsxMinSccGainPercent);
    } else if (gain > static_cast<int>(kMsxMaxSccGainPercent)) {
        gain = static_cast<int>(kMsxMaxSccGainPercent);
    }
    msx_config_set_scc_gain_percent(static_cast<uint16_t>(gain), persist);
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
