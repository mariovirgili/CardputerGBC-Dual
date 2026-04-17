#include "display_target.h"
#include <Preferences.h>

// Global display target - default to external
emu_display_target_t g_emu_display_target = EMU_DISPLAY_EXTERNAL;

// Global color depth - default to 16-bit
emu_color_depth_t g_emu_color_depth = EMU_COLOR_16BIT;

// Secondary screen redraw lock - default to unlocked
bool g_emu_aux_screen_locked = false;

// ROM type values from select_rom.h RomType enum
// UNKNOWN=0, MSX=1, MSX_DISK=2, COLECO=3, MSX_CAS=4
static constexpr int kRomMsx = 1;
static constexpr int kRomMsxDisk = 2;
static constexpr int kRomColeco = 3;
static constexpr int kRomMsxCas = 4;

bool emu_has_external_display_support(int romType)
{
    switch (romType) {
        case kRomMsx:
        case kRomMsxDisk:
        case kRomColeco:
        case kRomMsxCas:
            return true;
        default:
            return false;
    }
}

static const char* romTypeToKey(int romType)
{
    switch (romType) {
        case kRomMsx: return "msx";
        case kRomMsxDisk: return "dsk";
        case kRomColeco: return "coleco";
        case kRomMsxCas: return "cas";
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
        case kRomMsx:
        case kRomMsxDisk:
        case kRomColeco:
        case kRomMsxCas:
            // MSX1 uses a tiny fixed palette and MSX2 tops out at 9-bit RGB,
            // ColecoVision uses the same small VDP palette family,
            // so RGB444 is enough and cheaper to push on the external TFT.
            return EMU_COLOR_12BIT;
        default:
            return EMU_COLOR_16BIT;
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
