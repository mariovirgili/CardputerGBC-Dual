#pragma once

#include <stdbool.h>

// Display target for emulator cores
typedef enum {
    EMU_DISPLAY_INTERNAL = 0,
    EMU_DISPLAY_EXTERNAL = 1,
} emu_display_target_t;

// Color depth for external display
typedef enum {
    EMU_COLOR_16BIT = 0,
    EMU_COLOR_12BIT = 1,
} emu_color_depth_t;

#ifdef __cplusplus
extern "C" {
#endif

// Global display target — set before launching each emulator
extern emu_display_target_t g_emu_display_target;

// Global color depth — set before launching each emulator
extern emu_color_depth_t g_emu_color_depth;

// True when the secondary screen has already been drawn and must not be
// refreshed again during the current game session.
extern bool g_emu_aux_screen_locked;

// Check if a ROM type supports external display rendering
// romType corresponds to the RomType enum values in select_rom.h
bool emu_has_external_display_support(int romType);

// Recommended color depth for a given core (based on native palette size)
emu_color_depth_t emu_recommended_color_depth(int romType);

// Load/save per-core display target preference from NVS
emu_display_target_t emu_load_display_target(int romType);
void emu_save_display_target(int romType, emu_display_target_t target);

// Load/save per-core color depth preference from NVS
emu_color_depth_t emu_load_color_depth(int romType);
void emu_save_color_depth(int romType, emu_color_depth_t depth);

// Lock/unlock the secondary info/controls screen for the current session.
void emu_set_aux_screen_locked(bool locked);
bool emu_is_aux_screen_locked(void);

#ifdef __cplusplus
}
#endif
