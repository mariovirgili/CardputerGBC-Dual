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
#include "nes/run_nes.h"
#include "sms/run_sms.h"
#include "sms/display.h"
#include "ngp/run_ngp.h"
#include "ngp/ngc_display.h"
#include "ws/run_ws.h"
#include "ws/ws_display.h"
#include "pce/run_pce.h"
#include "pce/pce_display.h"
#include "lynx/run_lynx.h"
#include "lynx/lynx_display.h"
#include "atari2600/run_a2600.h"
#include "atari2600/a2600_display.h"
#include "atari7800/run_a7800.h"
#include "atari7800/a7800_video.h"
#include "genesis/run_genesis.h"
#include "genesis/genesis_display.h"
#include "gbc/run_gbc.h"
#include "gbc/gbc_display.h"
#include "snes/run_snes.h"
#include "last_game.h"
#define RETRO_COMPAT_IMPLEMENTATION
#include "ngp/race/retro_compat.h"
#include "esp_task_wdt.h"
#include "share/input.h"
#include "share/emu_controls.h"
#include "share/display_target.h"
#include <TFT_eSPI.h>
#include "tft_setup.h"
#include "cardputer/Welcome.h"
#include "cardputer/WelcomeExternalDs.h"

void nes_display_show_external_info(const char* romTitle);
static bool getProfileForRomType(RomType romType, share::EmuProfile& outProfile);

static bool detectGameBoyColorFromRomData(const uint8_t* romData, size_t romLen)
{
  if (!romData || romLen <= 0x143) {
    return false;
  }

  const uint8_t cgbFlag = romData[0x143];
  return cgbFlag == 0x80 || cgbFlag == 0xC0;
}

static bool detectWonderSwanVerticalModeFromRomData(const uint8_t* romData, size_t romLen)
{
  if (!romData || romLen < 10) {
    return false;
  }

  return (romData[romLen - 4] & 0x01) != 0;
}

static void showInternalControlsPreview(
    RomType romType,
    const std::string& romPath,
    const std::string& romName)
{
  if (romType == ROM_TYPE_WS && g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
    ws_display_show_internal_info(
        detectWonderSwanFromRom(romPath),
        detectWonderSwanVerticalModeFromRomData(get_rom_ptr(), get_rom_size()));
    emu_set_internal_screen_locked(true);
    return;
  }

  CardputerView display;
  display.initialize();
  if (romType == ROM_TYPE_NES || romType == ROM_TYPE_SMS || romType == ROM_TYPE_GAMEGEAR ||
      romType == ROM_TYPE_NGP || romType == ROM_TYPE_GENESIS || romType == ROM_TYPE_PCE ||
      romType == ROM_TYPE_GB || romType == ROM_TYPE_LYNX || romType == ROM_TYPE_SNES) {
    share::EmuProfile emuProfile = share::EmuProfile::Nes;
    if (getProfileForRomType(romType, emuProfile)) {
      display.topBar("- + SOUND [ ] BRIGHT", false, false);
      display.showControlBindings(
        share::emuControlActionLabels(emuProfile),
        share::emuControlKeyLabels(emuProfile),
        "HOLD GO = CFG"
      );
      emu_set_internal_screen_locked(true);
    }
  }
}

static void showExternalControlsPreview(RomType romType, const std::string& romPath, const std::string& romName)
{
  switch (romType) {
    case ROM_TYPE_NES:
      nes_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_SMS:
    case ROM_TYPE_GAMEGEAR:
      sms_display_show_external_info(romName.c_str(), romType == ROM_TYPE_GAMEGEAR);
      break;
    case ROM_TYPE_NGP:
      ngc_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_GENESIS:
      genesis_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_WS:
      ws_display_show_external_info(romName.c_str(), detectWonderSwanFromRom(romPath));
      break;
    case ROM_TYPE_PCE:
      pce_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_GB:
      gbc_display_show_external_info(romName.c_str(), detectGameBoyColorFromRomData(get_rom_ptr(), get_rom_size()));
      break;
    case ROM_TYPE_LYNX:
      lynx_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_A2600:
      a2600_display_show_external_info(romName.c_str());
      break;
    case ROM_TYPE_A7800:
      a7800_video_show_external_info(romName.c_str());
      break;
    default:
      return;
  }

  emu_set_aux_screen_locked(true);
}

static void clearExternalTft(const char* message = "Select Rom!")
{
  emu_set_aux_screen_locked(false);
  emu_set_internal_screen_locked(false);
  TFT_eSPI extTft;
  extTft.begin();
  extTft.setRotation(3);
  extTft.fillScreen(TFT_BLACK);
  if (message) {
    extTft.setTextColor(TFT_WHITE, TFT_BLACK);
    extTft.drawCentreString(message, 160, 110, 4);
  }
}

static void showExternalRomSelectorTft()
{
  emu_set_aux_screen_locked(false);
  emu_set_internal_screen_locked(false);
  struct ExternalRomBadge {
    const char* label;
    uint16_t color;
  };

  static const ExternalRomBadge badges[] = {
    {"NES",  NES_COLOR},
    {"GB",   GAMEBOY_COLOR},
    {"GBC",  GAMEBOY_COLOR},
    {"SNES", SNES_COLOR},
    {"SMS",  SMS_COLOR},
    {"MD",   GENESIS_COLOR},
    {"GG",   GAMEGEAR_COLOR},
    {"NGP",  NEOGEO_COLOR},
    {"WS",   WS_COLOR},
    {"WSC",  WS_COLOR},
    {"PCE",  PCE_COLOR},
    {"LYNX", LYNX_COLOR},
    {"A26",  LYNX_COLOR},
    {"A78",  LYNX_COLOR},
  };

  TFT_eSPI extTft;
  extTft.begin();
  extTft.setRotation(3);
  extTft.fillScreen(TFT_BLACK);

  extTft.setTextColor(TFT_WHITE, TFT_BLACK);
  extTft.drawCentreString("Select Rom!", 160, 16, 4);
  extTft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
  extTft.drawCentreString("Supported systems", 160, 52, 2);

  const int cols = 4;
  const int badgeW = 68;
  const int badgeH = 30;
  const int gapX = 8;
  const int gapY = 10;
  const int startX = 12;
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
  emu_set_internal_screen_locked(false);
  TFT_eSPI extTft;
  extTft.begin();
  extTft.setRotation(3);
  extTft.setSwapBytes(true);
  extTft.pushImage(0, 0, BGGAMESTATION_DS_EXT_WIDTH, BGGAMESTATION_DS_EXT_HEIGHT, bggamestation_ds_ext);
  extTft.setSwapBytes(false);
}

static bool getProfileForRomType(RomType romType, share::EmuProfile& outProfile) {
  switch (romType) {
    case ROM_TYPE_NES:
      outProfile = share::EmuProfile::Nes;
      return true;
    case ROM_TYPE_SMS:
    case ROM_TYPE_GAMEGEAR:
      outProfile = share::EmuProfile::Sms;
      return true;
    case ROM_TYPE_NGP:
      outProfile = share::EmuProfile::Ngp;
      return true;
    case ROM_TYPE_GENESIS:
      outProfile = share::EmuProfile::Genesis;
      return true;
    case ROM_TYPE_WS:
      outProfile = share::EmuProfile::Ws;
      return true;
    case ROM_TYPE_PCE:
      outProfile = share::EmuProfile::Pce;
      return true;
    case ROM_TYPE_GB:
      outProfile = share::EmuProfile::Gbc;
      return true;
    case ROM_TYPE_LYNX:
      outProfile = share::EmuProfile::Lynx;
      return true;
    case ROM_TYPE_SNES:
      outProfile = share::EmuProfile::Snes;
      return true;
    case ROM_TYPE_A2600:
      outProfile = share::EmuProfile::A2600;
      return true;
    case ROM_TYPE_A7800:
      outProfile = share::EmuProfile::A7800;
      return true;
    default:
      return false;
  }
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

  std::string romPath;
  auto romFolder = getRomFolderFromSd(sd);
  if (romFolder.empty()) {
    romFolder = getRomFolderFromNvs(display, input, sd);
  }
  romFolder = romFolder.empty() ? "/" : romFolder;

  if (isQuittingGame()) {
    // Returning from a game — show "Select Rom!" on external, go to browser
    romPath = reopenRomBrowser(sd, display, input, romFolder, true);
  } else {
    // Welcome on both screens
    display.welcome();
    welcomeExternalTft();
    input.waitPress(4000);

    // Try to get last game from NVS or select a new one
    romPath = getLastGameFromNvs(display, input, sd);
    if (romPath.empty()) {
      romPath = reopenRomBrowser(sd, display, input, romFolder, false);
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

  printf("Selected ROM: %s\n", romPath.c_str());

  display.topBar("COPYING ROM TO FLASH", false, false);
  display.subMessage("Loading...", 0);

  // Copy the ROM file to the partition
  size_t romSize = 0;
  if (!copyFileToPartition(romPath.c_str(), romPart, &romSize, CardputerView::copyProgress, &display)) {
    
    // --- MODIFICATION START: Disabled dynamic partition switching ---
    // The original code checked isLauncherLayout() and asked to flash partitions.
    // For Cardputer ADV / Double Screen, we disable this to avoid changing the partition table at runtime
    // which could break the dual screen layout or cause bootloops.
    
    /* // User is using the launcher
    if (isLauncherLayout()) {
      // Ask to flash the launcher Game Station partition to unlock full size
      ConfirmationSelector confirm(display, input);
      bool confirmed = confirm.select("ROM IS TOO HEAVY", "Change to 4.5MB layout?");
      
      if (confirmed) {
        display.topBar("FLASHING PARTITIONS", false, false);
        display.subMessage("Allow up to 4.5MB roms", 3000);
        auto ok = flashGameStationPartition();
        if (ok) {
          display.subMessage("Success, rebooting...", 3000);
          saveLastGameToNvs(romPath);
          esp_restart();
        } else {
          display.subMessage("Partition flashing failed", 3000);
        }
      }
    }
    */
    // --- MODIFICATION END ---
    
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
  auto ext = getRomType(romPath);
  share::EmuProfile emuProfile = share::EmuProfile::Nes;
  const bool hasProfile = getProfileForRomType(ext, emuProfile);
  if (hasProfile) {
    share::emuControlsLoad(sd, emuProfile);
  }

  // Prepare ROM filename early so we can preview the secondary info screen
  auto pos = romPath.find_last_of("/\\");
  std::string romName = (pos == std::string::npos) ? romPath : romPath.substr(pos + 1);

  // Display target selection (for cores that support external TFT)
  if (emu_has_external_display_support((int)ext)) {
    emu_set_aux_screen_locked(false);
    emu_set_internal_screen_locked(false);
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

      std::string opt16 = "16-bit 65K colors";
      std::string opt12 = "12-bit 4K colors";
      if (recommended == EMU_COLOR_16BIT) opt16 += " (recommended)";
      else                                 opt12 += " (recommended)";

      VerticalSelector depthSelector(display, input);
      std::vector<std::string> depthOptions = {opt16, opt12};
      display.topBar("COLOR DEPTH", false, false);

      int depthInitial = (savedDepth == EMU_COLOR_12BIT) ? 1 : 0;
      int dsel = depthSelector.select("Color depth", depthOptions,
                    false, false, {}, {}, false, true, true, depthInitial);
      if (dsel < 0) {
        dsel = depthInitial;
      }
      emu_color_depth_t chosenDepth = (dsel == 1) ? EMU_COLOR_12BIT : EMU_COLOR_16BIT;

      g_emu_color_depth = chosenDepth;
      if (chosenDepth != savedDepth) {
        emu_save_color_depth((int)ext, chosenDepth);
      }
    } else {
      g_emu_color_depth = EMU_COLOR_16BIT;
    }

    display.initialize();  // re-init display after selectors
    if (chosen == EMU_DISPLAY_INTERNAL) {
      showExternalControlsPreview(ext, romPath, romName);
    } else {
      emu_set_aux_screen_locked(false);
      emu_set_internal_screen_locked(false);
    }
  } else {
    g_emu_display_target = EMU_DISPLAY_INTERNAL;
    g_emu_color_depth = EMU_COLOR_16BIT;
  }

  // Show keymapping
  if (hasProfile && g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
    showInternalControlsPreview(ext, romPath, romName);
  } else if (hasProfile) {
    display.topBar("- + SOUND [ ] BRIGHT", false, false);
    display.showControlBindings(
      share::emuControlActionLabels(emuProfile),
      share::emuControlKeyLabels(emuProfile),
      "HOLD GO = CFG"
    );
    emu_set_internal_screen_locked(true);
  } else {
    display.topBar("- + SOUND [ ] BRIGHT", false, false);
    int numButtons = (ext == ROM_TYPE_GENESIS) ? 3 : 2;
    numButtons = (ext == ROM_TYPE_SNES) ? 6 : numButtons;
    display.showKeymapping(numButtons);
    emu_set_internal_screen_locked(true);
  }

  // Wait for key press or show tips
  uint32_t lastUpdate = millis();
  int state = 0;
  for (;;) {
    char key = input.handler();
    if (key == KEY_ESC_LONG_CUSTOM) {
      if (hasProfile) {
        share::emuControlsEdit(sd, emuProfile, display, input);
        input.flushInput(150);
        if (g_emu_display_target == EMU_DISPLAY_INTERNAL) {
          showExternalControlsPreview(ext, romPath, romName);
        }
        if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
          showInternalControlsPreview(ext, romPath, romName);
        } else {
          display.topBar("- + SOUND [ ] BRIGHT", false, false);
          display.showControlBindings(
            share::emuControlActionLabels(emuProfile),
            share::emuControlKeyLabels(emuProfile),
            "HOLD GO = CFG"
          );
          emu_set_internal_screen_locked(true);
        }
      }
      lastUpdate = millis();
      continue;
    }
    if (key != KEY_NONE) break;

    // Show tips
    uint32_t now = millis();
    const int stateCount = hasProfile ? 6 : 5;
    if (!emu_is_internal_screen_locked() && now - lastUpdate >= 2000) {
      lastUpdate = now;
      state = (state + 1) % stateCount;
      switch (state) {
        case 0: display.topBar("PRESS ANY KEY TO START", false, false); break;
        case 1: display.topBar("KEY \\ SCREEN MODE",       false, false); break;
        case 2: display.topBar("GO OR HOLD ESC TO QUIT",   false, false); break;
        case 3: display.topBar("FN + ARROWS FOR ZOOM",     false, false); break;
        case 4: display.topBar("- + SOUND [ ] BRIGHT",     false, false); break;
        case 5: display.topBar("HOLD GO FOR CONFIG",       false, false); break;
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
  if (ext == ROM_TYPE_NES) {
      // --- NES ---
      romName = "/xip/" + romName;
      run_nes(romName.c_str());
  }
  else if (ext == ROM_TYPE_GAMEGEAR || ext == ROM_TYPE_SMS) {
      // --- Master System / Game Gear ---
      bool isGG = (ext == ROM_TYPE_GAMEGEAR);
      run_sms(get_rom_ptr(), get_rom_size(), isGG, romName.c_str());
  }
  else if (ext == ROM_TYPE_NGP) {
      // --- Neo Geo Pocket / Color ---
      int machine = detectNeoGeoPocketFromRom(get_rom_ptr(), get_rom_size(), romPath);
      run_ngp(get_rom_ptr(), get_rom_size(), machine, romName.c_str());
  }
  else if (ext == ROM_TYPE_GENESIS) {
      // --- Megadrive / Genesis ---
      run_genesis(get_rom_ptr(), get_rom_size(), romName.c_str());
  }
  else if (ext == ROM_TYPE_WS) {
      // --- WonderSwan / Color ---
      run_ws(get_rom_ptr(), get_rom_size(), romName.c_str(), detectWonderSwanFromRom(romPath));
  }
  else if (ext == ROM_TYPE_PCE) {
      // --- PC Engine / TurboGrafx-16 ---
      run_pce(get_rom_ptr(), get_rom_size(), romName.c_str());
  }
  else if (ext == ROM_TYPE_GB) { 
      // --- Game Boy / Color ---
      run_gbc(get_rom_ptr(), get_rom_size(), romName.c_str());
  }
  else if (ext == ROM_TYPE_LYNX) {
      // --- Lynx ---
      run_lynx(get_rom_ptr(), get_rom_size(), romName.c_str());
  }
  else if (ext == ROM_TYPE_SNES) {
      // --- SNES / Super Famicom ---
      display.displaySnesInfo();
      input.waitPress();
      run_snes(get_rom_ptr(), get_rom_size());
  }
  else if (ext == ROM_TYPE_A2600) {
      // --- Atari 2600 ---
      run_a2600(get_rom_ptr(), get_rom_size(), romName.c_str());
  }
  else if (ext == ROM_TYPE_A7800) {
      // --- Atari 7800 ---
      run_a7800(get_rom_ptr(), get_rom_size(), romName.c_str());
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
