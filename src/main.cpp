#include <M5Cardputer.h>
#include <Preferences.h>
#include <select_rom.h>
#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/SdService.h"
#include "vfs/vfs_xip.h"
#include "vfs/rom_flash_io.h"
#include "vfs/rom_xip.h"
#include "vfs/partitioner.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cctype>
#include "msx/run_msx.h"
#include "msx/msx_config.h"
#include "msx/msx_display.h"
#include "msx/msx_input.h"
#include "last_game.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "share/input.h"
#include "share/emu_controls.h"
#include "share/display_target.h"
#include <TFT_eSPI.h>
#include <algorithm>
#include "tft_setup.h"
#include "cardputer/Welcome.h"
#include "cardputer/WelcomeExternalImage.h"
#include "cardputer/VerticalSelector.h"

static TFT_eSPI& startupExternalTft();

static void showExternalRomSelectorTft()
{
  emu_set_aux_screen_locked(false);
  TFT_eSPI& extTft = startupExternalTft();
  extTft.setSwapBytes(true);
  extTft.pushImage(0, 0, BGGAMESTATION_DS_EXT_WIDTH, BGGAMESTATION_DS_EXT_HEIGHT, bggamestation_ds_ext);
  extTft.setSwapBytes(false);
}

static TFT_eSPI& startupExternalTft()
{
  static TFT_eSPI extTft;
  static bool initialized = false;
  emu_set_aux_screen_locked(false);
  if (!initialized) {
    extTft.begin();
    extTft.setRotation(3);
    initialized = true;
  }
  return extTft;
}

static std::string lowerCopy(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

static bool containsToken(const std::string& text, const char* token)
{
  return text.find(token) != std::string::npos;
}

static void appendInfoToken(std::string& line, const char* token)
{
  if (!token || token[0] == '\0') {
    return;
  }
  const std::string paddedLine = " " + line + " ";
  const std::string paddedToken = " " + std::string(token) + " ";
  if (paddedLine.find(paddedToken) != std::string::npos) {
    return;
  }
  if (!line.empty()) {
    line += " ";
  }
  line += token;
}

static void appendInfoToken(std::string& line, const std::string& token)
{
  if (!token.empty()) {
    appendInfoToken(line, token.c_str());
  }
}

static std::string trimInfoTag(const std::string& tag)
{
  size_t start = 0;
  while (start < tag.size() && std::isspace(static_cast<unsigned char>(tag[start]))) {
    ++start;
  }

  size_t end = tag.size();
  while (end > start && std::isspace(static_cast<unsigned char>(tag[end - 1]))) {
    --end;
  }

  return tag.substr(start, end - start);
}

static bool parseUnsignedAt(const std::string& text, size_t& pos, int& value)
{
  if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
    return false;
  }

  int parsed = 0;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
    parsed = parsed * 10 + (text[pos] - '0');
    ++pos;
  }

  value = parsed;
  return true;
}

static std::string normalizeInfoTag(const std::string& tag)
{
  std::string normalized;
  normalized.reserve(tag.size());
  for (char c : tag) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return normalized;
}

static std::string formatDiskTag(const std::string& tag)
{
  const std::string normalized = normalizeInfoTag(tag);
  const size_t diskPos = normalized.find("disk");
  if (diskPos == std::string::npos) {
    return "";
  }

  size_t pos = diskPos + 4;
  int diskNumber = 0;
  int diskTotal = 0;
  if (!parseUnsignedAt(normalized, pos, diskNumber)) {
    return "";
  }
  if (normalized.compare(pos, 2, "of") != 0) {
    return "";
  }
  pos += 2;
  if (!parseUnsignedAt(normalized, pos, diskTotal)) {
    return "";
  }

  char buffer[24];
  snprintf(buffer, sizeof(buffer), "Disk %d of %d", diskNumber, diskTotal);
  return std::string(buffer);
}

static std::string formatRomInfoTag(const std::string& rawTag)
{
  const std::string tag = trimInfoTag(rawTag);
  if (tag.empty()) {
    return "";
  }

  const std::string diskTag = formatDiskTag(tag);
  if (!diskTag.empty()) {
    return diskTag;
  }

  const std::string normalized = normalizeInfoTag(tag);
  if (normalized == "scc+") {
    return "SCC+";
  }
  if (normalized == "scc") {
    return "SCC";
  }
  if (normalized == "t") {
    return "Trained";
  }
  if (normalized == "a") {
    return "Altern";
  }
  if (normalized == "a2") {
    return "Altern2";
  }
  if (normalized == "a3") {
    return "Altern3";
  }
  if (normalized == "b") {
    return "Bad";
  }
  if (normalized == "o") {
    return "Overdump";
  }
  if (normalized.rfind("cr", 0) == 0) {
    return "Cracked";
  }
  if (normalized == "beta") {
    return "Beta";
  }
  if (normalized == "jp" ||
      normalized == "ja" ||
      normalized == "j" ||
      normalized == "jap" ||
      normalized == "japan") {
    return "Jap";
  }
  if (normalized == "es") {
    return "Espanol";
  }
  if (normalized == "it") {
    return "Italiano";
  }
  if (normalized == "kr") {
    return "Korean";
  }

  return tag;
}

static void appendRomInfoTags(std::string& line, const std::string& name)
{
  for (size_t start = 0; start < name.size(); ++start) {
    if (name[start] != '(' && name[start] != '[') {
      continue;
    }

    const char close = name[start] == '(' ? ')' : ']';
    const size_t end = name.find(close, start + 1);
    if (end == std::string::npos) {
      continue;
    }

    appendInfoToken(line, formatRomInfoTag(name.substr(start + 1, end - start - 1)));
    start = end;
  }
}

static std::string buildRomBrowserInfoLine(const std::string& folder, const std::string& entry)
{
  if (entry == "..") {
    return "Parent";
  }

  const std::string combined = folder + "/" + entry;
  const std::string lower = lowerCopy(combined);
  std::string line;

  if (containsToken(lower, "scc+")) {
    appendInfoToken(line, "SCC+");
  } else if (containsToken(lower, "scc")) {
    appendInfoToken(line, "SCC");
  }

  appendRomInfoTags(line, entry);

  return line.empty() ? "No tags" : line;
}

static std::string fitExternalInfoText(TFT_eSPI& tft, const std::string& text, int font, int maxWidth)
{
  if (tft.textWidth(text.c_str(), font) <= maxWidth) {
    return text;
  }

  std::string clipped = text;
  while (!clipped.empty() && tft.textWidth((clipped + "...").c_str(), font) > maxWidth) {
    clipped.pop_back();
  }
  return clipped.empty() ? std::string("...") : clipped + "...";
}

static void drawExternalRomBrowserInfo(const std::string& folder,
                                       const std::string& entry,
                                       void* context)
{
  (void)context;
  if (entry.empty()) {
    return;
  }

  TFT_eSPI& tft = startupExternalTft();
  constexpr int kInfoY = 210;
  constexpr int kInfoH = 30;
  constexpr int kInfoW = 320;
  tft.fillRect(0, kInfoY, kInfoW, kInfoH, TFT_BLACK);
  tft.drawFastHLine(0, kInfoY, kInfoW, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const std::string rawInfoLine = buildRomBrowserInfoLine(folder, entry);
  const int infoFont = tft.textWidth(rawInfoLine.c_str(), 2) <= kInfoW - 8 ? 2 : 1;
  const std::string infoLine = fitExternalInfoText(tft, rawInfoLine, infoFont, kInfoW - 8);
  const int textW = tft.textWidth(infoLine.c_str(), infoFont);
  const int textX = std::max(4, (kInfoW - textW) / 2);
  tft.drawString(infoLine.c_str(), textX, kInfoY + (infoFont == 2 ? 8 : 10), infoFont);
}

namespace {
constexpr int kSelectorResultEditControls = -2;
constexpr int kSelectorResultBackToRomBrowser = -3;

struct MsxMachineSelectionResult {
  bool backToRomBrowser = false;
  MsxMachineMode mode = MsxMachineMode::MSX2;
};

struct MsxBoolSelectionResult {
  bool backToRomBrowser = false;
  bool value = false;
};

struct MsxDisplayTargetSelectionResult {
  bool backToRomBrowser = false;
  emu_display_target_t target = EMU_DISPLAY_EXTERNAL;
};

struct MsxVirtualSccSelectionResult {
  bool backToRomBrowser = false;
  MsxVirtualSccMode mode = MsxVirtualSccMode::Off;
};

enum class StartupMainMenuAction {
  RomSelector = 0,
  ConfigMenu = 1,
  ConfigKeys = 2,
  About = 3,
};

constexpr const char* kStartupMenuPrefsNs = "cardputer_emu";
constexpr const char* kStartupMenuIndexKey = "startup_menu";
} // namespace

static MsxMachineSelectionResult selectMsxLaunchSystem(CardputerView& display, CardputerInput& input)
{
  VerticalSelector selector(display, input);
  const MsxMachineMode persistedMode = msx_config_load_machine_mode();
  const std::vector<std::string> options = {
      "MSX1",
      "MSX2",
  };

  const int initialIndex = (persistedMode == MsxMachineMode::MSX1) ? 0 : 1;
  const int selected = selector.select("MSX launch system",
                                       options,
                                       false,
                                       false,
                                       {},
                                       {},
                                       false,
                                       true,
                                       true,
                                       initialIndex,
                                       -1,
                                       kSelectorResultBackToRomBrowser);

  if (selected == kSelectorResultBackToRomBrowser) {
    MsxMachineSelectionResult result;
    result.backToRomBrowser = true;
    result.mode = persistedMode;
    return result;
  }

  const int chosen = selected >= 0 ? selected : initialIndex;
  MsxMachineSelectionResult result;
  result.backToRomBrowser = false;
  result.mode = chosen == 0 ? MsxMachineMode::MSX1 : MsxMachineMode::MSX2;
  return result;
}

static MsxDisplayTargetSelectionResult selectMsxDisplayTarget(CardputerView& display,
                                                              CardputerInput& input,
                                                              SdService& sd,
                                                              share::EmuProfile emuProfile,
                                                              bool hasProfile,
                                                              emu_display_target_t savedTarget)
{
  VerticalSelector selector(display, input);
  const std::vector<std::string> options = {"External TFT", "Internal LCD"};
  const int initialIndex = (savedTarget == EMU_DISPLAY_EXTERNAL) ? 0 : 1;

  for (;;) {
    display.topBar("SELECT DISPLAY", false, false);
    const int selected = selector.select("Display target",
                                         options,
                                         false,
                                         false,
                                         {},
                                         {},
                                         false,
                                         true,
                                         true,
                                         initialIndex,
                                         hasProfile ? kSelectorResultEditControls : -1,
                                         kSelectorResultBackToRomBrowser);
    if (selected == kSelectorResultEditControls && hasProfile) {
      share::emuControlsEdit(sd, emuProfile, display, input);
      input.flushInput(150);
      continue;
    }
    if (selected == kSelectorResultBackToRomBrowser) {
      MsxDisplayTargetSelectionResult result;
      result.backToRomBrowser = true;
      result.target = savedTarget;
      return result;
    }

    const int chosen = selected >= 0 ? selected : initialIndex;
    MsxDisplayTargetSelectionResult result;
    result.backToRomBrowser = false;
    result.target = chosen == 0 ? EMU_DISPLAY_EXTERNAL : EMU_DISPLAY_INTERNAL;
    return result;
  }
}

static MsxBoolSelectionResult selectMsxExternalFpsLock(CardputerView& display,
                                                       CardputerInput& input,
                                                       bool persistedLocked)
{
  VerticalSelector selector(display, input);
  const std::vector<std::string> options = {
      "Unlocked / 60 FPS if possible",
      "Lock external TFT to 30 FPS",
  };

  const int initialIndex = persistedLocked ? 1 : 0;
  const int selected = selector.select("MSX external FPS lock",
                                       options,
                                       false,
                                       false,
                                       {},
                                       {},
                                       false,
                                       true,
                                       true,
                                       initialIndex,
                                       -1,
                                       kSelectorResultBackToRomBrowser);

  if (selected == kSelectorResultBackToRomBrowser) {
    MsxBoolSelectionResult result;
    result.backToRomBrowser = true;
    result.value = persistedLocked;
    return result;
  }

  const int chosen = selected >= 0 ? selected : initialIndex;
  MsxBoolSelectionResult result;
  result.backToRomBrowser = false;
  result.value = chosen == 1;
  return result;
}

static MsxVirtualSccSelectionResult selectMsxVirtualSccMode(CardputerView& display, CardputerInput& input)
{
  VerticalSelector selector(display, input);
  const MsxVirtualSccMode persistedMode = msx_config_load_virtual_scc_mode();
  const std::vector<std::string> options = {
      "OFF",
      "SCC",
      "SCC-I",
  };

  const int initialIndex = persistedMode == MsxVirtualSccMode::Scc
                               ? 1
                               : (persistedMode == MsxVirtualSccMode::SccI ? 2 : 0);
  const int selected = selector.select("MSX virtual SCC",
                                       options,
                                       false,
                                       false,
                                       {},
                                       {},
                                       false,
                                       true,
                                       true,
                                       initialIndex,
                                       -1,
                                       kSelectorResultBackToRomBrowser);
  if (selected == kSelectorResultBackToRomBrowser) {
    MsxVirtualSccSelectionResult result;
    result.backToRomBrowser = true;
    result.mode = persistedMode;
    return result;
  }

  const int chosen = selected >= 0 ? selected : initialIndex;
  MsxVirtualSccSelectionResult result;
  result.backToRomBrowser = false;
  result.mode = chosen == 1
                    ? MsxVirtualSccMode::Scc
                    : (chosen == 2 ? MsxVirtualSccMode::SccI : MsxVirtualSccMode::Off);
  return result;
}

static const char* startupBoolLabel(bool enabled)
{
  return enabled ? "ON" : "OFF";
}

static uint8_t loadStartupMenuIndex()
{
  Preferences prefs;
  prefs.begin(kStartupMenuPrefsNs, true);
  const uint8_t index = prefs.getUChar(kStartupMenuIndexKey, 0);
  prefs.end();
  return index < 4 ? index : 0;
}

static void saveStartupMenuIndex(uint8_t index)
{
  Preferences prefs;
  prefs.begin(kStartupMenuPrefsNs, false);
  prefs.putUChar(kStartupMenuIndexKey, index < 4 ? index : 0);
  prefs.end();
}

static StartupMainMenuAction selectStartupMainMenu(CardputerView& display, CardputerInput& input)
{
  VerticalSelector selector(display, input);
  const std::vector<std::string> options = {
      "Rom selector",
      "Config Menu",
      "Config Keys",
      "About",
  };

  const uint8_t initialIndex = loadStartupMenuIndex();
  int selected = selector.select("STARTUP MENU",
                                 options,
                                 false,
                                 false,
                                 {},
                                 {},
                                 false,
                                 true,
                                 true,
                                 initialIndex,
                                 -1,
                                 0);
  if (selected < 0 || selected > 3) {
    selected = 0;
  }
  saveStartupMenuIndex(static_cast<uint8_t>(selected));
  return static_cast<StartupMainMenuAction>(selected);
}

static bool startupKeyWordPressed(char lower)
{
  const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
  for (char ch : keys.word) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) == lower) {
      return true;
    }
  }
  return false;
}

static bool startupEscapePressed()
{
  return M5Cardputer.BtnA.wasClicked() ||
         startupKeyWordPressed('`') ||
         startupKeyWordPressed('~');
}

static bool startupKonamiStepPressed(size_t step)
{
  switch (step) {
    case 0:
    case 1:
      return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP) ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Up);
    case 2:
    case 3:
      return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN) ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Down);
    case 4:
    case 6:
      return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Left);
    case 5:
    case 7:
      return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Right);
    case 8:
      return startupKeyWordPressed('b') ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::B);
    case 9:
      return startupKeyWordPressed('a') ||
             share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::A);
    default:
      return false;
  }
}

static bool startupAnyKonamiInputPressed()
{
  return M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_UP) ||
         M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_DOWN) ||
         M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
         M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
         startupKeyWordPressed('a') ||
         startupKeyWordPressed('b') ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Up) ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Down) ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Left) ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::Right) ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::A) ||
         share::emuControlPressed(share::EmuProfile::MSX, share::EmuAction::B);
}

static MsxRuntimeOptionConfig sanitizeStartupTesterConfig(MsxRuntimeOptionConfig config)
{
  config.stateSlot = static_cast<uint8_t>(config.stateSlot % 10u);
  if (config.basicKeyboardEnabled) {
    config.keyboardEnabled = false;
    config.joystickEnabled = false;
    config.vausEnabled = false;
  }
  return config;
}

static void drawStartupInputTester(const MsxRuntimeOptionConfig& config,
                                   const MsxInputDiagnosticState& state)
{
  auto& tft = M5Cardputer.Display;
  tft.fillRect(0, TOP_BAR_HEIGHT, tft.width(), tft.height() - TOP_BAR_HEIGHT, TFT_BLACK);
  tft.setTextDatum(top_left);
  tft.setTextSize(TEXT_SMALL);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  tft.drawString("FN+J/K/B/V toggles", 8, 34);
  tft.setTextColor(TEXT_COLOR, TFT_BLACK);

  char modeLine[48];
  snprintf(modeLine,
           sizeof(modeLine),
           "J:%s K:%s B:%s V:%s",
           config.joystickEnabled ? "ON" : "OFF",
           config.keyboardEnabled ? "ON" : "OFF",
           config.basicKeyboardEnabled ? "ON" : "OFF",
           config.vausEnabled ? "ON" : "OFF");
  tft.drawString(modeLine, 8, 48);
  tft.drawString((std::string("RAW: ") + state.rawLabel).c_str(), 8, 64);
  tft.drawString((std::string("EMU: ") + state.actionLabel).c_str(), 8, 78);
  tft.drawString((std::string("JOY: ") + state.joystickLabel).c_str(), 8, 92);
  tft.drawString((std::string("KBD: ") + state.keyboardLabel).c_str(), 8, 106);
  tft.drawString((std::string("VAUS: ") + state.vausLabel).c_str(), 8, 120);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  tft.drawString("GO = back", 8, 136);
  tft.setTextDatum(middle_center);
}

static void drawStartupInputTesterExternal(const MsxRuntimeOptionConfig& config,
                                           const MsxInputDiagnosticState& state)
{
  auto& tft = startupExternalTft();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  const char* title = "MSX INPUT TEST";
  tft.drawString(title, (320 - tft.textWidth(title, 2)) / 2, 12, 2);
  tft.drawString("FN+J/K/B/V toggles", 14, 42, 2);
  tft.setTextColor(TEXT_COLOR, TFT_BLACK);

  char modeLine[48];
  snprintf(modeLine,
           sizeof(modeLine),
           "J:%s  K:%s  B:%s  V:%s",
           config.joystickEnabled ? "ON" : "OFF",
           config.keyboardEnabled ? "ON" : "OFF",
           config.basicKeyboardEnabled ? "ON" : "OFF",
           config.vausEnabled ? "ON" : "OFF");
  tft.drawString(modeLine, 14, 68, 2);
  tft.drawString((std::string("RAW:  ") + state.rawLabel).c_str(), 14, 94, 2);
  tft.drawString((std::string("EMU:  ") + state.actionLabel).c_str(), 14, 118, 2);
  tft.drawString((std::string("JOY:  ") + state.joystickLabel).c_str(), 14, 142, 2);
  tft.drawString((std::string("KBD:  ") + state.keyboardLabel).c_str(), 14, 166, 2);
  tft.drawString((std::string("VAUS: ") + state.vausLabel).c_str(), 14, 190, 2);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  tft.drawString("GO = back", 14, 216, 2);
  tft.setTextDatum(MC_DATUM);
}

static std::string startupInputTesterSnapshot(const MsxRuntimeOptionConfig& config,
                                              const MsxInputDiagnosticState& state)
{
  char modeLine[64];
  snprintf(modeLine,
           sizeof(modeLine),
           "J:%u K:%u B:%u V:%u",
           config.joystickEnabled ? 1u : 0u,
           config.keyboardEnabled ? 1u : 0u,
           config.basicKeyboardEnabled ? 1u : 0u,
           config.vausEnabled ? 1u : 0u);
  return std::string(modeLine) + "|" +
         state.rawLabel + "|" +
         state.actionLabel + "|" +
         state.joystickLabel + "|" +
         state.keyboardLabel + "|" +
         state.vausLabel;
}

static void showStartupInputTester(CardputerView& display, CardputerInput& input)
{
  MsxRuntimeOptionConfig config = sanitizeStartupTesterConfig(msx_input_load_runtime_option_config());
  MsxInputDiagnosticState diagnostic = {};
  uint32_t lastDraw = 0;
  uint32_t nextToggleAllowedMs = 0;
  std::string lastSnapshot;
  bool toggleLatch = false;

  display.topBar("MSX INPUT TEST", false, false);
  input.flushInput(150);

  for (;;) {
    msx_input_poll_diagnostic(config, &diagnostic);
    const Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    if (diagnostic.exitRequested) {
      input.flushInput(150);
      return;
    }

    bool togglePressed = false;
    const uint32_t now = millis();
    if (keys.fn && !toggleLatch && now >= nextToggleAllowedMs) {
      if (startupKeyWordPressed('j')) {
        config.joystickEnabled = !config.joystickEnabled;
        if (config.joystickEnabled) {
          config.basicKeyboardEnabled = false;
        }
        togglePressed = true;
      } else if (startupKeyWordPressed('k')) {
        config.keyboardEnabled = !config.keyboardEnabled;
        config.basicKeyboardEnabled = false;
        togglePressed = true;
      } else if (startupKeyWordPressed('b')) {
        config.basicKeyboardEnabled = !config.basicKeyboardEnabled;
        togglePressed = true;
      } else if (startupKeyWordPressed('v')) {
        config.vausEnabled = !config.vausEnabled;
        if (config.vausEnabled) {
          config.basicKeyboardEnabled = false;
        }
        togglePressed = true;
      }
    }

    if (togglePressed) {
      config = sanitizeStartupTesterConfig(config);
      msx_input_poll_diagnostic(config, &diagnostic);
      lastSnapshot.clear();
      nextToggleAllowedMs = now + 450u;
    }
    toggleLatch = togglePressed;

    const std::string snapshot = startupInputTesterSnapshot(config, diagnostic);
    if ((lastDraw == 0 || now - lastDraw >= 180u) && snapshot != lastSnapshot) {
      drawStartupInputTester(config, diagnostic);
      drawStartupInputTesterExternal(config, diagnostic);
      lastSnapshot = snapshot;
      lastDraw = now;
    }
    delay(8);
  }
}

static void drawStartupAboutPage()
{
  auto& tft = M5Cardputer.Display;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.setTextColor(TEXT_COLOR, TFT_BLACK);
  tft.setTextSize(TEXT_WIDE);
  tft.drawCenterString("Msx ADV Emulators v0.5", tft.width() / 2, 16);
  tft.setTextSize(TEXT_SMALL);
  tft.drawCenterString("MSX is a registered", tft.width() / 2, 48);
  tft.drawCenterString("trademark owned by", tft.width() / 2, 64);
  tft.drawCenterString("MSX Licensing", tft.width() / 2, 80);
  tft.drawCenterString("Corporation", tft.width() / 2, 96);
  tft.setTextSize(TEXT_SMALL);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  tft.drawCenterString("GO / LEFT / ` = back", tft.width() / 2, 124);
}

static void drawStartupAboutPageExternal()
{
  auto& tft = startupExternalTft();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TEXT_COLOR, TFT_BLACK);
  const char* title = "Msx ADV Emulators v0.5";
  tft.drawString(title, (320 - tft.textWidth(title, 2)) / 2, 20, 2);
  const char* line1 = "MSX is a registered trademark";
  const char* line2 = "owned by MSX Licensing";
  const char* line3 = "Corporation";
  tft.drawString(line1, (320 - tft.textWidth(line1, 2)) / 2, 76, 2);
  tft.drawString(line2, (320 - tft.textWidth(line2, 2)) / 2, 104, 2);
  tft.drawString(line3, (320 - tft.textWidth(line3, 2)) / 2, 132, 2);
  tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  const char* hint = "GO / LEFT / ` = back";
  tft.drawString(hint, (320 - tft.textWidth(hint, 2)) / 2, 196, 2);
  tft.setTextDatum(MC_DATUM);
}

static void showStartupAboutPage(SdService& sd, CardputerView& display, CardputerInput& input)
{
  share::emuControlsLoad(sd, share::EmuProfile::MSX);
  static constexpr size_t kKonamiLength = 10;
  size_t konamiProgress = 0;

  drawStartupAboutPage();
  drawStartupAboutPageExternal();
  input.flushInput(150);

  for (;;) {
    M5Cardputer.update();
    if (startupEscapePressed() ||
        (konamiProgress == 0 && M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT))) {
      showExternalRomSelectorTft();
      input.flushInput(150);
      return;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (startupKonamiStepPressed(konamiProgress)) {
        ++konamiProgress;
      } else if (startupAnyKonamiInputPressed()) {
        konamiProgress = startupKonamiStepPressed(0) ? 1u : 0u;
      }

      if (konamiProgress >= kKonamiLength) {
        showStartupInputTester(display, input);
        drawStartupAboutPage();
        drawStartupAboutPageExternal();
        konamiProgress = 0;
        input.flushInput(150);
      }
    }
    delay(8);
  }
}

static void showStartupMsxPerformanceMenu(CardputerView& display, CardputerInput& input)
{
  VerticalSelector selector(display, input);
  int selectedIndex = 0;

  for (;;) {
    msx_config_load_performance_flags();
    msx_config_load_frameskip_mode();
    msx_config_load_fps_overlay_enabled();
    const bool machineIsMsx2 = msx_config_load_machine_mode() != MsxMachineMode::MSX1;

    const std::vector<std::string> options = {
        "ExternalFixed30Fps",
        "Frameskip",
        "FpsOverlay",
        "SliceRendering",
        "SpriteCollision",
        "SpriteOverflow",
        "InstantCommands",
        "Back",
    };
    const std::vector<std::string> values = {
        startupBoolLabel(msx_config_get_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps)),
        msx_config_get_frameskip_mode_label(),
        startupBoolLabel(msx_config_get_fps_overlay_enabled()),
        machineIsMsx2 ? (msx_config_get_performance_flag(MsxPerformanceFlag::DisableSliceRendering) ? "OFF" : "ON") : "N/A",
        msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision) ? "OFF" : "ON",
        msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow) ? "OFF" : "ON",
        machineIsMsx2 ? startupBoolLabel(msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) : "N/A",
        "",
    };

    const int selected = selector.select("Performance",
                                         options,
                                         false,
                                         false,
                                         values,
                                         {},
                                         false,
                                         true,
                                         true,
                                         selectedIndex,
                                         -1,
                                         kSelectorResultBackToRomBrowser);
    if (selected == kSelectorResultBackToRomBrowser || selected < 0 || selected == 7) {
      input.flushInput(120);
      return;
    }

    selectedIndex = selected;
    switch (selected) {
      case 0:
        msx_config_set_performance_flag(
            MsxPerformanceFlag::ExternalFixed30Fps,
            !msx_config_get_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps),
            true);
        break;
      case 1:
        msx_config_cycle_frameskip_mode(1, true);
        break;
      case 2:
        msx_config_set_fps_overlay_enabled(!msx_config_get_fps_overlay_enabled(), true);
        break;
      case 3:
        if (machineIsMsx2) {
          msx_config_set_performance_flag(
              MsxPerformanceFlag::DisableSliceRendering,
              !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSliceRendering),
              true);
        }
        break;
      case 4:
        msx_config_set_performance_flag(
            MsxPerformanceFlag::DisableSpriteCollision,
            !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision),
            true);
        break;
      case 5:
        msx_config_set_performance_flag(
            MsxPerformanceFlag::SimplifySpriteOverflow,
            !msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow),
            true);
        break;
      case 6:
        if (machineIsMsx2) {
          msx_config_set_performance_flag(
              MsxPerformanceFlag::InstantVdpCommands,
              !msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands),
              true);
        }
        break;
      default:
        break;
    }
  }
}

static void showStartupMsxConfigMenu(CardputerView& display, CardputerInput& input)
{
  VerticalSelector selector(display, input);
  int selectedIndex = 0;

  for (;;) {
    msx_config_load_internal_view_mode();
    msx_config_load_performance_flags();
    msx_config_load_virtual_scc_mode();
    msx_config_load_region_mode();
    MsxRuntimeOptionConfig config = msx_input_load_runtime_option_config();
    char stateSlotValue[8];
    snprintf(stateSlotValue, sizeof(stateSlotValue), "< %u >", static_cast<unsigned>(config.stateSlot));

    const std::vector<std::string> options = {
        "Performance",
        "Virtual SCC",
        "JOY EXTEND",
        "KEYB/JOY",
        "BasicKeyboard",
        "Vaus",
        "View",
        "MSX REGION",
        "StateSlot",
        "Back",
    };
    const std::vector<std::string> values = {
        msx_config_get_performance_mode_label(),
        msx_config_get_virtual_scc_mode_label(),
        startupBoolLabel(config.joystickEnabled),
        startupBoolLabel(config.keyboardEnabled),
        startupBoolLabel(config.basicKeyboardEnabled),
        startupBoolLabel(config.vausEnabled),
        msx_config_get_active_view_mode_label_for_target(g_emu_display_target == EMU_DISPLAY_EXTERNAL),
        msx_config_region_mode_label(msx_config_get_region_mode()),
        stateSlotValue,
        "",
    };

    const int selected = selector.select("Config Menu",
                                         options,
                                         false,
                                         false,
                                         values,
                                         {},
                                         false,
                                         true,
                                         true,
                                         selectedIndex,
                                         -1,
                                         kSelectorResultBackToRomBrowser);
    if (selected == kSelectorResultBackToRomBrowser || selected < 0 || selected == 9) {
      input.flushInput(120);
      return;
    }

    selectedIndex = selected;
    switch (selected) {
      case 0:
        if (msx_config_get_performance_preset() == MsxPerformancePreset::Custom) {
          showStartupMsxPerformanceMenu(display, input);
        } else {
          msx_config_cycle_performance_preset(1, true);
        }
        break;
      case 1:
        msx_config_cycle_virtual_scc_mode(1, true);
        break;
      case 2:
        config.joystickEnabled = !config.joystickEnabled;
        if (config.joystickEnabled) {
          config.basicKeyboardEnabled = false;
        }
        msx_input_set_runtime_option_config(config, true);
        break;
      case 3:
        config.keyboardEnabled = !config.keyboardEnabled;
        config.basicKeyboardEnabled = false;
        msx_input_set_runtime_option_config(config, true);
        break;
      case 4:
        config.basicKeyboardEnabled = !config.basicKeyboardEnabled;
        msx_input_set_runtime_option_config(config, true);
        break;
      case 5:
        config.vausEnabled = !config.vausEnabled;
        if (config.vausEnabled) {
          config.basicKeyboardEnabled = false;
        }
        msx_input_set_runtime_option_config(config, true);
        break;
      case 6:
        msx_config_toggle_active_view_mode_for_target(g_emu_display_target == EMU_DISPLAY_EXTERNAL);
        break;
      case 7:
        msx_config_cycle_region_mode(1, true);
        break;
      case 8:
        config.stateSlot = static_cast<uint8_t>((config.stateSlot + 1u) % 10u);
        msx_input_set_runtime_option_config(config, true);
        break;
      default:
        break;
    }
  }
}

static void welcomeExternalTft()
{
  emu_set_aux_screen_locked(false);
  TFT_eSPI extTft;
  extTft.begin();
  extTft.setRotation(3);
  extTft.setSwapBytes(true);
  extTft.pushImage(0, 0, BGGAMESTATION_DS_EXT_WIDTH, BGGAMESTATION_DS_EXT_HEIGHT, bggamestation_ds_ext);
  extTft.setSwapBytes(false);
}

enum class StartupBootAction {
  None = 0,
  ForceRomSelector,
  ResetRomHistory,
};

static StartupBootAction getStartupBootAction(CardputerView& display)
{
  static constexpr uint32_t kForceSelectorHoldMs = 900;
  static constexpr uint32_t kResetHistoryHoldMs = 2200;

  M5Cardputer.update();
  if (!M5Cardputer.BtnA.isPressed()) {
    return StartupBootAction::None;
  }

  uint8_t shownStage = 0;
  const uint32_t startMs = millis();

  while (true) {
    M5Cardputer.update();
    const uint32_t heldMs = millis() - startMs;

    uint8_t nextStage = 0;
    if (heldMs >= kResetHistoryHoldMs) {
      nextStage = 2;
    } else if (heldMs >= kForceSelectorHoldMs) {
      nextStage = 1;
    }

    if (nextStage != shownStage) {
      shownStage = nextStage;
      switch (shownStage) {
        case 0:
          display.topBar("BOOT OPTIONS", false, false);
          display.subMessage("Hold GO for selector", "Hold longer to clear history", 0);
          break;
        case 1:
          display.topBar("FORCE ROM SELECTOR", false, false);
          display.subMessage("Release to skip auto launch", "Keep holding to clear history", 0);
          break;
        default:
          display.topBar("RESET ROM HISTORY", false, false);
          display.subMessage("Release to clear last game", "and browser path", 0);
          break;
      }
    }

    if (!M5Cardputer.BtnA.isPressed()) {
      M5Cardputer.update();
      (void)M5Cardputer.BtnA.wasClicked();

      if (shownStage >= 2) {
        display.topBar("ROM HISTORY RESET", false, false);
        display.subMessage("Saved game and folder cleared", 600);
        return StartupBootAction::ResetRomHistory;
      }
      if (shownStage >= 1) {
        display.topBar("FORCE ROM SELECTOR", false, false);
        display.subMessage("Saved game skipped", 600);
        return StartupBootAction::ForceRomSelector;
      }
      return StartupBootAction::None;
    }

    delay(10);
  }
}

static void restartForPendingLaunch(CardputerView& display,
                                    SdService& sd,
                                    const std::string& romPath,
                                    int machineMode = -1)
{
  if (!savePendingLaunchToNvs(romPath, machineMode)) {
    printf("[LAUNCH] pending launch save failed, continuing in current session\n");
    return;
  }

  display.topBar("RESTARTING LAUNCH", false, false);
  display.subMessage("Clean boot prepared", "Rebooting now", 0);
  sd.close();
  delay(120);
  esp_restart();
}

static std::string formatRomSizeLabel(size_t bytes) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return std::string(buffer);
}

static bool ensureSelectedRomFitsPartition(
    SdService& sd,
    CardputerView& display,
    CardputerInput& input,
    const esp_partition_t* romPart,
    std::string& romPath
) {
  if (!romPart) {
    return false;
  }

  while (!romPath.empty()) {
    size_t romFileSize = 0;
    std::string browserPath = normalizeRomBrowserPath(romPath);
    std::string browserFolder = extractRomFolder(browserPath);

    if (sd.getFileSize(browserPath, romFileSize)) {
      if (romFileSize <= romPart->size) {
        return true;
      }

      display.topBar("ROM IS TOO HEAVY", false, false);
      display.subMessage(formatRomSizeLabel(romFileSize) + " > " + formatRomSizeLabel(romPart->size), 1500);
      display.subMessage("Choose another ROM", 0);
    } else {
      display.topBar("ROM FILE ERROR", false, false);
      display.subMessage("Select another ROM", 0);
    }

    saveRomFolderToSd(sd, browserFolder);
    input.waitPress();
    showExternalRomSelectorTft();
    romPath = getRomPath(sd, display, input, browserFolder, true, drawExternalRomBrowserInfo, nullptr);
  }

  return false;
}

static std::string reopenRomBrowser(
    SdService& sd,
    CardputerView& display,
    CardputerInput& input,
    std::string browserFolder,
    bool skipWelcome = true,
    bool* backToStartupMenu = nullptr
) {
  browserFolder = browserFolder.empty() ? "/" : browserFolder;
  if (backToStartupMenu) {
    *backToStartupMenu = false;
  }

  while (true) {
    showExternalRomSelectorTft();
    bool browserBackToStartupMenu = false;
    std::string romPath = getRomPath(sd,
                                     display,
                                     input,
                                     browserFolder,
                                     skipWelcome,
                                     drawExternalRomBrowserInfo,
                                     nullptr,
                                     &browserBackToStartupMenu);
    if (browserBackToStartupMenu) {
      if (backToStartupMenu) {
        *backToStartupMenu = true;
        return "";
      }
      browserFolder = getRomFolderFromSd(sd);
      if (browserFolder.empty()) {
        browserFolder = "/";
      }
      display.topBar("SELECT A ROM", false, false);
      display.subMessage("Browsing SD card", 400);
      skipWelcome = true;
      continue;
    }
    if (!romPath.empty()) {
      return romPath;
    }

    browserFolder = getRomFolderFromSd(sd);
    if (browserFolder.empty()) {
      browserFolder = "/";
    }

    display.topBar("SELECT A ROM", false, false);
    display.subMessage("Browsing SD card", 400);
    skipWelcome = true;
  }
}

void setup() {
  // Set high priority for the current task (where the emulator will run)
  vTaskPrioritySet(NULL, 19);

  // Copied from Gameboy Enhanced Firmware setup
#ifdef DISABLE_WATCHDOGS
  M5.Log.printf("Disabling all WatchDogs...\n");
  esp_task_wdt_deinit(); // fully disables and removes TWDT
  disableCore0WDT(); // disable WDT
  disableCore1WDT(); // disable WDT
  esp_task_wdt_delete(NULL); // disable WDT on this therad - legacy
#endif

  auto cfg = M5.config();
  cfg.output_power = true;
  M5Cardputer.begin(cfg);

  CardputerInput input;
  SdService sd;
  CardputerView display;
  display.initialize();

  // SD
  while (!sd.begin()) {
    display.topBar("SD CARD FOR ROMS", false, false);
    display.subMessage("No SD card found", 1000);
    display.subMessage("Insert SD card", 0);
  }

  const StartupBootAction startupBootAction = getStartupBootAction(display);
  const bool resetSavedRomState = startupBootAction == StartupBootAction::ResetRomHistory;
  const bool forceRomSelector = startupBootAction == StartupBootAction::ForceRomSelector;
  if (resetSavedRomState) {
    clearSavedRomState(sd);
  } else if (forceRomSelector) {
    clearPendingLaunchFromNvs();
  }

  std::string romPath;
  std::string romFolder = "/";
  bool selectedFromBrowser = false;
  if (!resetSavedRomState) {
    romFolder = getRomFolderFromSd(sd);
    if (romFolder.empty()) {
      romFolder = getRomFolderFromNvs(display, input, sd);
    }
    romFolder = romFolder.empty() ? "/" : romFolder;
  }

  const bool quittingGame = isQuittingGame();
  PendingLaunchState pendingLaunch;
  if (!forceRomSelector && !resetSavedRomState && !quittingGame) {
    pendingLaunch = consumePendingLaunchFromNvs(sd);
    if (pendingLaunch.valid() && pendingLaunch.machineMode >= 0) {
      // Keep the selected machine mode for this boot instead of reloading an old
      // persisted value in the launcher flow.
      msx_config_set_machine_mode(static_cast<MsxMachineMode>(pendingLaunch.machineMode), true);
    }
  }

  if (pendingLaunch.valid()) {
    romPath = pendingLaunch.romPath;
    display.topBar("STARTING PENDING ROM", false, false);
    display.subMessage("Clean boot complete", "Launching in 1 second", 0);
    delay(1000);
  } else if (forceRomSelector || quittingGame) {
    // Returning from a game or forcing recovery - show "Select Rom!" on external, go to browser.
    bool backToStartupMenu = false;
    romPath = reopenRomBrowser(sd, display, input, romFolder, true, &backToStartupMenu);
    selectedFromBrowser = !romPath.empty();
    if (backToStartupMenu) {
      bool skipRomSelectorWelcome = true;
      while (romPath.empty()) {
        switch (selectStartupMainMenu(display, input)) {
          case StartupMainMenuAction::RomSelector:
            backToStartupMenu = false;
            romPath = reopenRomBrowser(sd, display, input, romFolder, skipRomSelectorWelcome, &backToStartupMenu);
            selectedFromBrowser = !romPath.empty();
            skipRomSelectorWelcome = true;
            break;
          case StartupMainMenuAction::ConfigMenu:
            showStartupMsxConfigMenu(display, input);
            break;
          case StartupMainMenuAction::ConfigKeys:
            share::emuControlsLoad(sd, share::EmuProfile::MSX);
            share::emuControlsEdit(sd, share::EmuProfile::MSX, display, input);
            input.flushInput(150);
            break;
          case StartupMainMenuAction::About:
            showStartupAboutPage(sd, display, input);
            break;
        }
      }
    }
  } else {
    // Welcome on both screens
    display.welcome();
    welcomeExternalTft();
    input.waitPress(4000);

    // Try to get last game from NVS or select a new one.
    if (!resetSavedRomState) {
      romPath = getLastGameFromNvs(display, input, sd);
    }
    if (romPath.empty()) {
      bool skipRomSelectorWelcome = false;
      while (romPath.empty()) {
        switch (selectStartupMainMenu(display, input)) {
          case StartupMainMenuAction::RomSelector:
            {
              bool backToStartupMenu = false;
              romPath = reopenRomBrowser(sd, display, input, romFolder, skipRomSelectorWelcome, &backToStartupMenu);
              (void)backToStartupMenu;
            }
            selectedFromBrowser = !romPath.empty();
            skipRomSelectorWelcome = true;
            break;
          case StartupMainMenuAction::ConfigMenu:
            showStartupMsxConfigMenu(display, input);
            break;
          case StartupMainMenuAction::ConfigKeys:
            share::emuControlsLoad(sd, share::EmuProfile::MSX);
            share::emuControlsEdit(sd, share::EmuProfile::MSX, display, input);
            input.flushInput(150);
            break;
          case StartupMainMenuAction::About:
            showStartupAboutPage(sd, display, input);
            break;
        }
      }
    } else {
      romPath = "/sd" + romPath; // ensure sd prefix
    }
  }

  // Find the rom partition (SPIFFS)
  const esp_partition_t* romPart = findRomPartition("spiffs");
  if (!romPart) {
    while (1) {
      display.topBar("ERROR", false, false);
      display.subMessage("No ROM partition", 0);
      delay(1500);
    }
  }

  auto reopenMsxLaunchBrowser = [&](const std::string& currentRomPath) {
    std::string browserFolder = getRomFolderFromSd(sd);
    if (browserFolder.empty()) {
      browserFolder = extractRomFolder(normalizeRomBrowserPath(currentRomPath));
    }
    pendingLaunch = PendingLaunchState{};
    romPath = reopenRomBrowser(sd, display, input, browserFolder, true);
    selectedFromBrowser = !romPath.empty();
  };

  RomType ext = ROM_TYPE_UNKNOWN;
  const share::EmuProfile emuProfile = share::EmuProfile::MSX;
  bool hasProfile = false;
  std::string romName;

  for (;;) {
    while (!ensureSelectedRomFitsPartition(sd, display, input, romPart, romPath)) {
      std::string browserFolder = getRomFolderFromSd(sd);
      if (browserFolder.empty()) {
        browserFolder = extractRomFolder(romPath);
      }
      romPath = reopenRomBrowser(sd, display, input, browserFolder, true);
      selectedFromBrowser = !romPath.empty();
      pendingLaunch = PendingLaunchState{};
    }

    ext = getRomType(romPath);
    hasProfile = (ext == ROM_TYPE_MSX || ext == ROM_TYPE_MSX_DISK || ext == ROM_TYPE_MSX_CAS);
    if (hasProfile) {
      share::emuControlsLoad(sd, emuProfile);
    }

    auto pos = romPath.find_last_of("/\\");
    romName = (pos == std::string::npos) ? romPath : romPath.substr(pos + 1);

    if (!pendingLaunch.valid() && selectedFromBrowser && ext != ROM_TYPE_UNKNOWN) {
      MsxMachineMode chosenMachineMode = msx_config_load_machine_mode();
      if (hasProfile) {
        const MsxMachineSelectionResult machineSelection = selectMsxLaunchSystem(display, input);
        if (machineSelection.backToRomBrowser) {
          reopenMsxLaunchBrowser(romPath);
          continue;
        }
        chosenMachineMode = machineSelection.mode;
      }
      msx_config_set_machine_mode(chosenMachineMode, false);
      restartForPendingLaunch(display, sd, romPath, static_cast<int>(chosenMachineMode));
    }

    if (emu_has_external_display_support((int)ext)) {
      emu_set_aux_screen_locked(false);
      const emu_display_target_t savedTarget = emu_load_display_target((int)ext);
      const MsxDisplayTargetSelectionResult displaySelection =
        selectMsxDisplayTarget(display, input, sd, emuProfile, hasProfile, savedTarget);
      if (displaySelection.backToRomBrowser) {
        reopenMsxLaunchBrowser(romPath);
        continue;
      }

      g_emu_display_target = displaySelection.target;
      if (g_emu_display_target != savedTarget) {
        emu_save_display_target((int)ext, g_emu_display_target);
      }

      if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
        g_emu_color_depth = EMU_COLOR_12BIT;
        if (emu_load_color_depth((int)ext) != EMU_COLOR_12BIT) {
          emu_save_color_depth((int)ext, EMU_COLOR_12BIT);
        }
      } else {
        g_emu_color_depth = EMU_COLOR_16BIT;
      }

      if (hasProfile) {
        msx_config_load_performance_flags();
        if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
          const bool savedExternalFpsLock =
            msx_config_get_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps);
          const MsxBoolSelectionResult fpsLockSelection =
            selectMsxExternalFpsLock(display, input, savedExternalFpsLock);
          if (fpsLockSelection.backToRomBrowser) {
            reopenMsxLaunchBrowser(romPath);
            continue;
          }
          if (fpsLockSelection.value != savedExternalFpsLock) {
            msx_config_set_performance_flag(MsxPerformanceFlag::ExternalFixed30Fps,
                                            fpsLockSelection.value,
                                            true);
          }
        }
        const MsxVirtualSccSelectionResult sccSelection = selectMsxVirtualSccMode(display, input);
        if (sccSelection.backToRomBrowser) {
          reopenMsxLaunchBrowser(romPath);
          continue;
        }
        msx_config_set_virtual_scc_mode(sccSelection.mode, true);
      }

      display.initialize();
      emu_set_aux_screen_locked(false);
    } else {
      g_emu_display_target = EMU_DISPLAY_INTERNAL;
      g_emu_color_depth = EMU_COLOR_16BIT;
    }

    break;
  }

  printf("Selected ROM: %s\n", romPath.c_str());

  display.topBar("COPYING ROM TO FLASH", false, false);
  display.subMessage("Loading...", 0);

  // Copy the ROM file to the partition
  size_t romSize = 0;
  if (!copyFileToPartition(romPath.c_str(), romPart, &romSize, CardputerView::copyProgress, &display)) {

    // Generic flash copy failure
    while (1) {
        display.topBar("ROM COPY ERROR", false, false);
        display.subMessage("Copy ROM to flash failed", 1500);
        display.subMessage("Restart and retry", 1500);
        delay(1500);
    }
  }

  input.flushInput(10); // flush any input just in case

  // Map the ROM partition in XIP
  if (xip_map_rom_partition("spiffs", romSize) != 0) {
    while (1) {
      display.topBar("ERROR", false, false);
      display.subMessage("Map ROM failed", 0);
      delay(1500);
    }
  }

  // Register the XIP VFS
  vfs_xip_register();

  // Show keymapping
  display.topBar("FN +/- SOUND FN [] BRIGHT", false, false);
  display.showControlBindings(
    share::emuControlActionLabels(emuProfile),
    share::emuControlKeyLabels(emuProfile),
    "HOLD GO = CFG"
  );

  // Wait for key press or show tips
  uint32_t lastUpdate = millis();
  int state = 0;
  for (;;) {
    char key = input.handler();
    if (key == KEY_ESC_LONG_CUSTOM) {
      share::emuControlsEdit(sd, emuProfile, display, input);
      input.flushInput(150);
      display.topBar("FN +/- SOUND FN [] BRIGHT", false, false);
      display.showControlBindings(
        share::emuControlActionLabels(emuProfile),
        share::emuControlKeyLabels(emuProfile),
        "HOLD GO = CFG"
      );
      emu_set_aux_screen_locked(false);
      lastUpdate = millis();
      continue;
    }
    if (key != KEY_NONE) break;

    // Show tips
    uint32_t now = millis();
    const int stateCount = 6;
    if (now - lastUpdate >= 2000) {
      lastUpdate = now;
      state = (state + 1) % stateCount;
      switch (state) {
        case 0: display.topBar("PRESS ANY KEY TO START", false, false); break;
        case 1: display.topBar("KEY \\ SCREEN MODE",       false, false); break;
        case 2: display.topBar("IN GAME: GO = QUIT",       false, false); break;
        case 3: display.topBar("FN + ARROWS FOR ZOOM",     false, false); break;
        case 4: display.topBar("FN +/- SOUND FN [] BRIGHT", false, false); break;
        case 5: display.topBar("IN GAME: HOLD GO = MENU",  false, false); break;
      }
    }
    delay(1);
  }

  // Save last game to nvs
  if (ext != ROM_TYPE_UNKNOWN) {
      saveLastGameToNvs(romPath);
  }

  printf("HEAP BEFORE EMU: %u bytes\n", esp_get_free_heap_size());

  // Initialize I2C M5Stack JoyV2 if any
  share::detectI2cPad();
  
  // Run the emulator
  if (ext == ROM_TYPE_MSX) {
      // MSX cartridge ROM
      run_msx(get_rom_ptr(), get_rom_size(), romName.c_str(), sd);
  }
  else if (ext == ROM_TYPE_MSX_DISK) {
      // MSX disk image (.dsk) â€” ROM partition holds the DSK data via XIP
      run_msx_disk(get_rom_ptr(), get_rom_size(), romName.c_str(), sd);
  }
  else if (ext == ROM_TYPE_MSX_CAS) {
      // MSX cassette tape image (.cas) — ROM partition holds CAS data via XIP
      run_msx_cas(get_rom_ptr(), get_rom_size(), romName.c_str(), sd);
  }
  else {
      display.topBar("ERROR", false, false);
      display.subMessage("Unsupported ROM type", 0);
      while (1) delay(1000);
  }
}

void loop() {
  /* run_emulator is blocking */
}
