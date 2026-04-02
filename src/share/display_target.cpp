#include "display_target.h"
#include <Preferences.h>

// Global display target - default to external
emu_display_target_t g_emu_display_target = EMU_DISPLAY_EXTERNAL;

// Global color depth - default to 16-bit
emu_color_depth_t g_emu_color_depth = EMU_COLOR_16BIT;

// Secondary screen redraw lock - default to unlocked
bool g_emu_aux_screen_locked = false;

// ROM type values from select_rom.h RomType enum
// UNKNOWN=0, NES=1, SMS=2, GG=3, NGP=4, GENESIS=5, WS=6, PCE=7, GB=8, LYNX=9, SNES=10, A2600=11, A7800=12
static constexpr int kRomA2600 = 11;
static constexpr int kRomA7800 = 12;

bool emu_has_external_display_support(int romType)
{
    switch (romType) {
        case kRomA2600:
        case kRomA7800:
            return true;
        default:
            return false;
    }
}

static const char* romTypeToKey(int romType)
{
    switch (romType) {
        case kRomA2600: return "a26";
        case kRomA7800: return "a78";
        default:        return "unk";
    }
}

static constexpr const char* kNvsNamespace = "disp_tgt";

emu_display_target_t emu_load_display_target(int romType)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, true);
    int val = prefs.getInt(romTypeToKey(romType), (int)EMU_DISPLAY_EXTERNAL);
    prefs.end();
    return (emu_display_target_t)val;
}

void emu_save_display_target(int romType, emu_display_target_t target)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);
    prefs.putInt(romTypeToKey(romType), (int)target);
    prefs.end();
}

emu_color_depth_t emu_recommended_color_depth(int romType)
{
    switch (romType) {
        case kRomA2600: return EMU_COLOR_12BIT;
        case kRomA7800: return EMU_COLOR_12BIT;
        default:        return EMU_COLOR_16BIT;
    }
}

static constexpr const char* kNvsColorNs = "color_dep";

emu_color_depth_t emu_load_color_depth(int romType)
{
    Preferences prefs;
    prefs.begin(kNvsColorNs, true);
    int val = prefs.getInt(romTypeToKey(romType), (int)emu_recommended_color_depth(romType));
    prefs.end();
    return (emu_color_depth_t)val;
}

void emu_save_color_depth(int romType, emu_color_depth_t depth)
{
    Preferences prefs;
    prefs.begin(kNvsColorNs, false);
    prefs.putInt(romTypeToKey(romType), (int)depth);
    prefs.end();
}

void emu_set_aux_screen_locked(bool locked)
{
    g_emu_aux_screen_locked = locked;
}

bool emu_is_aux_screen_locked(void)
{
    return g_emu_aux_screen_locked;
}
