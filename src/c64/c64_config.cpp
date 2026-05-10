#include "c64_config.h"

#include <Preferences.h>

namespace {
constexpr const char* kC64ConfigNs = "c64_cfg";
constexpr const char* kC64FpsHudKey = "fps_hud";
constexpr bool kC64DefaultFpsHud = true;
bool s_fpsHudEnabled = kC64DefaultFpsHud;
} // namespace

bool c64_config_load_fps_hud_enabled()
{
  Preferences prefs;
  prefs.begin(kC64ConfigNs, true);
  s_fpsHudEnabled = prefs.getBool(kC64FpsHudKey, kC64DefaultFpsHud);
  prefs.end();
  return s_fpsHudEnabled;
}

bool c64_config_get_fps_hud_enabled()
{
  return s_fpsHudEnabled;
}

void c64_config_set_fps_hud_enabled(bool enabled, bool persist)
{
  s_fpsHudEnabled = enabled;
  if (!persist) {
    return;
  }

  Preferences prefs;
  prefs.begin(kC64ConfigNs, false);
  prefs.putBool(kC64FpsHudKey, enabled);
  prefs.end();
}
