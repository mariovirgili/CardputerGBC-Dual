#include "videopac_config.h"

#include <Preferences.h>

namespace {

constexpr const char* kVideopacConfigNs = "vpack_cfg";
constexpr const char* kVideoModeKey = "video";
constexpr const char* kConfigVersionKey = "ver";
constexpr uint8_t kConfigVersion = 2;
constexpr VideopacVideoMode kDefaultVideoMode = VideopacVideoMode::Fit;

VideopacVideoMode s_videoMode = kDefaultVideoMode;

VideopacVideoMode sanitize_video_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(VideopacVideoMode::Fit)
               ? VideopacVideoMode::Fit
               : VideopacVideoMode::Fast;
}

void store_video_mode(VideopacVideoMode mode)
{
    s_videoMode = sanitize_video_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kVideopacConfigNs, false);
    prefs.putUChar(kVideoModeKey, static_cast<uint8_t>(s_videoMode));
    prefs.putUChar(kConfigVersionKey, kConfigVersion);
    prefs.end();
}

} // namespace

VideopacVideoMode videopac_config_load_video_mode(void)
{
    Preferences prefs;
    prefs.begin(kVideopacConfigNs, true);
    const bool hasSavedValue = prefs.isKey(kVideoModeKey);
    const uint8_t savedVersion = prefs.getUChar(kConfigVersionKey, 0);
    const uint8_t saved = prefs.getUChar(
        kVideoModeKey,
        static_cast<uint8_t>(kDefaultVideoMode)
    );
    prefs.end();

    s_videoMode = (hasSavedValue && savedVersion >= kConfigVersion)
        ? sanitize_video_mode(saved)
        : kDefaultVideoMode;

    if (!hasSavedValue || savedVersion < kConfigVersion) {
        store_video_mode(s_videoMode);
    }

    return s_videoMode;
}

VideopacVideoMode videopac_config_get_video_mode(void)
{
    return s_videoMode;
}

const char* videopac_config_video_mode_label(VideopacVideoMode mode)
{
    return mode == VideopacVideoMode::Fit ? "FIT" : "FAST";
}

const char* videopac_config_get_video_mode_label(void)
{
    return videopac_config_video_mode_label(s_videoMode);
}

void videopac_config_set_video_mode(VideopacVideoMode mode, bool persist)
{
    s_videoMode = sanitize_video_mode(static_cast<uint8_t>(mode));
    if (persist) {
        store_video_mode(s_videoMode);
    }
}

void videopac_config_toggle_video_mode(void)
{
    const VideopacVideoMode nextMode =
        (s_videoMode == VideopacVideoMode::Fit)
            ? VideopacVideoMode::Fast
            : VideopacVideoMode::Fit;
    videopac_config_set_video_mode(nextMode, true);
}
