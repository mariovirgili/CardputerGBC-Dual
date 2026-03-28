#include "a7800_config.h"

#include <Preferences.h>

static constexpr const char* kA7800DisplayNs = "a7800_disp";
static constexpr const char* kA7800ViewKey = "int_view";
static constexpr A7800InternalViewMode kA7800DefaultInternalViewMode = A7800InternalViewMode::Wide;
static A7800InternalViewMode s_internalViewMode = A7800InternalViewMode::Wide;

static A7800InternalViewMode a7800_sanitize_internal_view_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(A7800InternalViewMode::PixelPerfect)
               ? A7800InternalViewMode::PixelPerfect
               : A7800InternalViewMode::Wide;
}

A7800InternalViewMode a7800_config_load_internal_view_mode(void)
{
    Preferences prefs;
    prefs.begin(kA7800DisplayNs, true);
    const bool hasSavedValue = prefs.isKey(kA7800ViewKey);
    const uint8_t saved = prefs.getUChar(
        kA7800ViewKey,
        static_cast<uint8_t>(kA7800DefaultInternalViewMode)
    );
    prefs.end();

    s_internalViewMode = hasSavedValue
                             ? a7800_sanitize_internal_view_mode(saved)
                             : kA7800DefaultInternalViewMode;

    if (!hasSavedValue) {
        a7800_config_save_internal_view_mode(s_internalViewMode);
    }

    return s_internalViewMode;
}

A7800InternalViewMode a7800_config_get_internal_view_mode(void)
{
    return s_internalViewMode;
}

const char* a7800_config_internal_view_mode_label(A7800InternalViewMode mode)
{
    return mode == A7800InternalViewMode::PixelPerfect ? "PIXEL" : "WIDE";
}

const char* a7800_config_get_internal_view_mode_label(void)
{
    return a7800_config_internal_view_mode_label(s_internalViewMode);
}

void a7800_config_save_internal_view_mode(A7800InternalViewMode mode)
{
    s_internalViewMode = a7800_sanitize_internal_view_mode(static_cast<uint8_t>(mode));

    Preferences prefs;
    prefs.begin(kA7800DisplayNs, false);
    prefs.putUChar(kA7800ViewKey, static_cast<uint8_t>(s_internalViewMode));
    prefs.end();
}

void a7800_config_set_internal_view_mode(A7800InternalViewMode mode, bool persist)
{
    s_internalViewMode = a7800_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    if (persist) {
        a7800_config_save_internal_view_mode(s_internalViewMode);
    }
}

void a7800_config_toggle_internal_view_mode(void)
{
    const A7800InternalViewMode nextMode =
        (s_internalViewMode == A7800InternalViewMode::PixelPerfect)
            ? A7800InternalViewMode::Wide
            : A7800InternalViewMode::PixelPerfect;
    a7800_config_set_internal_view_mode(nextMode, true);
}
