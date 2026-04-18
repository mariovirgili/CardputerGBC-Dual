#include "videopac_config.h"

#include <Preferences.h>
#include <cstddef>

namespace {

constexpr const char* kVideopacConfigNs = "vpack_cfg";
constexpr const char* kVideoModeKey = "video";
constexpr const char* kConfigVersionKey = "ver";
constexpr uint8_t kConfigVersion = 4;
constexpr VideopacVideoMode kDefaultVideoMode = VideopacVideoMode::Fit;
constexpr VideopacVideoMode kVideoModeCycle[] = {
    VideopacVideoMode::Fit,
    VideopacVideoMode::FitFast,
    VideopacVideoMode::Fast,
};

VideopacVideoMode s_videoMode = kDefaultVideoMode;

VideopacVideoMode sanitize_video_mode(uint8_t value)
{
    switch (static_cast<VideopacVideoMode>(value)) {
    case VideopacVideoMode::Fit:
        return VideopacVideoMode::Fit;
    case VideopacVideoMode::Fast:
        return VideopacVideoMode::Fast;
    case VideopacVideoMode::FitFast:
        return VideopacVideoMode::FitFast;
    default:
        return kDefaultVideoMode;
    }
}

int video_mode_index(VideopacVideoMode mode)
{
    for (size_t i = 0; i < sizeof(kVideoModeCycle) / sizeof(kVideoModeCycle[0]); ++i) {
        if (kVideoModeCycle[i] == mode) {
            return static_cast<int>(i);
        }
    }
    return 0;
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

    if (hasSavedValue) {
        const VideopacVideoMode savedMode = sanitize_video_mode(saved);
        if (savedVersion >= kConfigVersion) {
            s_videoMode = savedMode;
        } else if (savedMode == VideopacVideoMode::FitFast) {
            // Migrate the old external default to the new FIT standard.
            s_videoMode = VideopacVideoMode::Fit;
        } else {
            s_videoMode = savedMode;
        }
    } else {
        s_videoMode = kDefaultVideoMode;
    }

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
    switch (mode) {
    case VideopacVideoMode::Fit:
        return "FIT";
    case VideopacVideoMode::FitFast:
        return "FIT FAST";
    case VideopacVideoMode::Fast:
        return "FAST";
    default:
        return "FIT";
    }
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
    videopac_config_step_video_mode(1);
}

void videopac_config_step_video_mode(int direction)
{
    constexpr int count = static_cast<int>(sizeof(kVideoModeCycle) / sizeof(kVideoModeCycle[0]));
    int index = video_mode_index(s_videoMode);
    index += direction < 0 ? -1 : 1;
    if (index < 0) {
        index = count - 1;
    } else if (index >= count) {
        index = 0;
    }
    videopac_config_set_video_mode(kVideoModeCycle[index], true);
}
