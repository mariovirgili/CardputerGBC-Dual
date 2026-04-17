#include <M5Cardputer.h>
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
#include "msx/run_msx.h"
#include "msx/msx_config.h"
#include "msx/msx_display.h"
#include "coleco/run_coleco.h"
#include "last_game.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "share/input.h"
#include "share/emu_controls.h"
#include "share/display_target.h"
#include <TFT_eSPI.h>
#include "tft_setup.h"
#include "cardputer/Welcome.h"
#include "cardputer/WelcomeExternalImage.h"

static void showExternalRomSelectorTft()
{
  emu_set_aux_screen_locked(false);
  struct ExternalRomBadge {
    const char* label;
    uint16_t color;
  };

  static const ExternalRomBadge badges[] = {
    {"MSX", PRIMARY_COLOR},
    {"ColecoVision", COLECO_COLOR}
  };

  TFT_eSPI extTft;
  extTft.begin();
  extTft.setRotation(3);
  extTft.fillScreen(TFT_BLACK);

  extTft.setTextColor(TFT_WHITE, TFT_BLACK);
  extTft.drawCentreString("Select Rom!", 160, 16, 4);
  extTft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  extTft.drawCentreString("Supported systems", 160, 52, 2);

  const int cols = 2;
  const int badgeW = 116;
  const int badgeH = 30;
  const int gapX = 8;
  const int gapY = 10;
  const int totalBadgeCount = static_cast<int>(sizeof(badges) / sizeof(badges[0]));
  const int usedCols = totalBadgeCount < cols ? totalBadgeCount : cols;
  const int rowWidth = usedCols * badgeW + (usedCols - 1) * gapX;
  const int startX = (320 - rowWidth) / 2;
  const int startY = 82;

  for (int i = 0; i < (int)(sizeof(badges) / sizeof(badges[0])); ++i) {
    const int row = i / cols;
    const int col = i % cols;
    const int x = startX + col * (badgeW + gapX);
    const int y = startY + row * (badgeH + gapY);

    extTft.fillRoundRect(x, y, badgeW, badgeH, 6, RECT_COLOR_DARK);
    extTft.drawRoundRect(x, y, badgeW, badgeH, 6, badges[i].color);
    extTft.setTextColor(TFT_WHITE, RECT_COLOR_DARK);
    extTft.drawCentreString(badges[i].label, x + badgeW / 2, y + 8, 2);
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
    romPath = getRomPath(sd, display, input, browserFolder, true);
  }

  return false;
}

static std::string reopenRomBrowser(
    SdService& sd,
    CardputerView& display,
    CardputerInput& input,
    std::string browserFolder,
    bool skipWelcome = true
) {
  browserFolder = browserFolder.empty() ? "/" : browserFolder;

  while (true) {
    showExternalRomSelectorTft();
    std::string romPath = getRomPath(sd, display, input, browserFolder, skipWelcome);
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
      msx_config_set_machine_mode(static_cast<MsxMachineMode>(pendingLaunch.machineMode), false);
    }
  }

  if (pendingLaunch.valid()) {
    romPath = pendingLaunch.romPath;
    display.topBar("STARTING PENDING ROM", false, false);
    display.subMessage("Clean boot complete", "Launching in 1 second", 0);
    delay(1000);
  } else if (forceRomSelector || quittingGame) {
    // Returning from a game or forcing recovery - show "Select Rom!" on external, go to browser.
    romPath = reopenRomBrowser(sd, display, input, romFolder, true);
    selectedFromBrowser = !romPath.empty();
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
      romPath = reopenRomBrowser(sd, display, input, romFolder, false);
      selectedFromBrowser = !romPath.empty();
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

  while (!ensureSelectedRomFitsPartition(sd, display, input, romPart, romPath)) {
    std::string browserFolder = getRomFolderFromSd(sd);
    if (browserFolder.empty()) {
      browserFolder = extractRomFolder(romPath);
    }
    romPath = reopenRomBrowser(sd, display, input, browserFolder, true);
  }

  const RomType ext = getRomType(romPath);
  if (!pendingLaunch.valid() && selectedFromBrowser && ext != ROM_TYPE_UNKNOWN) {
    msx_config_set_machine_mode(MsxMachineMode::MSX1, false);
    restartForPendingLaunch(display, sd, romPath, static_cast<int>(MsxMachineMode::MSX1));
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

  // Check the extension to choose the emulator
  const share::EmuProfile emuProfile = share::EmuProfile::MSX;
  const bool hasProfile = (ext == ROM_TYPE_MSX || ext == ROM_TYPE_MSX_DISK);
  if (hasProfile) {
    share::emuControlsLoad(sd, emuProfile);
  }

  // Prepare ROM filename early so we can preview the secondary info screen
  auto pos = romPath.find_last_of("/\\");
  std::string romName = (pos == std::string::npos) ? romPath : romPath.substr(pos + 1);

  // Display target selection (for cores that support external TFT)
  if (emu_has_external_display_support((int)ext)) {
    emu_set_aux_screen_locked(false);
    emu_display_target_t savedTarget = emu_load_display_target((int)ext);

    VerticalSelector displaySelector(display, input);
    std::vector<std::string> displayOptions = {"External TFT", "Internal LCD"};
    int initialIdx = (savedTarget == EMU_DISPLAY_EXTERNAL) ? 0 : 1;
    int sel = initialIdx;

    for (;;) {
      display.topBar("SELECT DISPLAY", false, false);
      sel = displaySelector.select("Display target", displayOptions,
                  false, false, {}, {}, false, true, true, initialIdx,
                  hasProfile ? -2 : -1);
      if (sel == -2 && hasProfile) {
        share::emuControlsEdit(sd, emuProfile, display, input);
        input.flushInput(150);
        continue;
      }
      if (sel < 0) {
        sel = initialIdx;
      }
      break;
    }

    emu_display_target_t chosen;
    if (sel == 0) {
      chosen = EMU_DISPLAY_EXTERNAL;
    } else {
      chosen = EMU_DISPLAY_INTERNAL;
    }

    g_emu_display_target = chosen;
    if (chosen != savedTarget) {
      emu_save_display_target((int)ext, chosen);
    }

    // Color depth selection (only when external display is chosen)
    if (chosen == EMU_DISPLAY_EXTERNAL) {
      emu_color_depth_t savedDepth = emu_load_color_depth((int)ext);
      emu_color_depth_t recommended = emu_recommended_color_depth((int)ext);

      std::string recommendedLabel =
        (recommended == EMU_COLOR_12BIT)
          ? "12-bit 4K colors (recommended)"
          : "16-bit 65K colors (recommended)";
      std::string alternateLabel =
        (recommended == EMU_COLOR_12BIT)
          ? "16-bit 65K colors"
          : "12-bit 4K colors";

      VerticalSelector depthSelector(display, input);
      std::vector<std::string> depthOptions = {recommendedLabel, alternateLabel};
      display.topBar("COLOR DEPTH", false, false);

      int depthInitial = 0;
      int dsel = depthSelector.select("Color depth", depthOptions,
                    false, false, {}, {}, false, true, true, depthInitial);
      if (dsel < 0) {
        dsel = depthInitial;
      }
      emu_color_depth_t chosenDepth = (dsel == 0)
        ? recommended
        : (recommended == EMU_COLOR_12BIT ? EMU_COLOR_16BIT : EMU_COLOR_12BIT);

      g_emu_color_depth = chosenDepth;
      if (chosenDepth != savedDepth) {
        emu_save_color_depth((int)ext, chosenDepth);
      }
    } else {
      g_emu_color_depth = EMU_COLOR_16BIT;
    }

    display.initialize();
    emu_set_aux_screen_locked(false);
  } else {
    g_emu_display_target = EMU_DISPLAY_INTERNAL;
    g_emu_color_depth = EMU_COLOR_16BIT;
  }

  // Show keymapping
  display.topBar("- + SOUND [ ] BRIGHT", false, false);
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
      display.topBar("- + SOUND [ ] BRIGHT", false, false);
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
        case 4: display.topBar("- + SOUND [ ] BRIGHT",     false, false); break;
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
  else if (ext == ROM_TYPE_COLECO) {
      // ColecoVision cartridge ROM
      run_coleco(get_rom_ptr(), get_rom_size(), romName.c_str(), sd);
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
