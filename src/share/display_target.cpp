#include "display_target.h"
#include <Preferences.h>

// Global display target — default to external
emu_display_target_t g_emu_display_target = EMU_DISPLAY_EXTERNAL;

// Global color depth — default to 16-bit
emu_color_depth_t g_emu_color_depth = EMU_COLOR_16BIT;

// ROM type values from select_rom.h RomType enum
// UNKNOWN=0, NES=1, SMS=2, GG=3, NGP=4, GENESIS=5, WS=6, PCE=7, GB=8, LYNX=9, SNES=10
static constexpr int kRomNes     = 1;
static constexpr int kRomSms     = 2;
static constexpr int kRomGg      = 3;
static constexpr int kRomNgp     = 4;
static constexpr int kRomGenesis = 5;
static constexpr int kRomPce     = 7;
static constexpr int kRomGb      = 8;
static constexpr int kRomLynx    = 9;

bool emu_has_external_display_support(int romType)
{
    switch (romType) {
        case kRomNes:
        case kRomSms:
        case kRomGg:
        case kRomNgp:
        case kRomGenesis:
        case kRomPce:
        case kRomGb:
        case kRomLynx:
            return true;
        default:
            return false;
    }
}

static const char* romTypeToKey(int romType)
{
    switch (romType) {
        case kRomNes:     return "nes";
        case kRomSms:     return "sms";
        case kRomGg:      return "gg";
        case kRomNgp:     return "ngp";
        case kRomGenesis: return "gen";
        case kRomPce:     return "pce";
        case kRomGb:      return "gb";
        case kRomLynx:    return "lynx";
        default:          return "unk";
    }
}

static constexpr const char* kNvsNamespace = "disp_tgt";

emu_display_target_t emu_load_display_target(int romType)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, true);  // read-only
    int val = prefs.getInt(romTypeToKey(romType), (int)EMU_DISPLAY_EXTERNAL);
    prefs.end();
    return (emu_display_target_t)val;
}

void emu_save_display_target(int romType, emu_display_target_t target)
{
    Preferences prefs;
    prefs.begin(kNvsNamespace, false);  // read-write
    prefs.putInt(romTypeToKey(romType), (int)target);
    prefs.end();
}

// ---- Color depth ----

emu_color_depth_t emu_recommended_color_depth(int romType)
{
    switch (romType) {
        case kRomNes:     return EMU_COLOR_12BIT;   // 64 native colors → 4K plenty
        case kRomSms:     return EMU_COLOR_12BIT;   // 64 native colors (6-bit RGB) → 4K plenty
        case kRomGg:      return EMU_COLOR_12BIT;   // 4096 native colors (12-bit RGB exact match)
        case kRomNgp:     return EMU_COLOR_12BIT;   // 146 native colors → 4K plenty
        case kRomGenesis: return EMU_COLOR_12BIT;   // 512 native colors (9-bit RGB) → 4K plenty
        case kRomPce:     return EMU_COLOR_12BIT;   // 512 native colors → 4K plenty
        case kRomGb:      return EMU_COLOR_16BIT;   // up to 32K colors, 12-bit loses nuance
        case kRomLynx:    return EMU_COLOR_12BIT;   // 4096 native colors (12-bit RGB)
        default:          return EMU_COLOR_16BIT;
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
