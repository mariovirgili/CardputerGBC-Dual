#include <M5Cardputer.h>
#include <select_rom.h>
#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/VerticalSelector.h"
#include "cardputer/SdService.h"
#include "vfs/vfs_xip.h"
#include "vfs/rom_flash_io.h"
#include "vfs/rom_xip.h"
#include "vfs/partitioner.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef NES_CORE_ENABLED
#include "nes/run_nes.h"
#endif
#ifdef SMS_CORE_ENABLED
#include "sms/run_sms.h"
#endif
#ifdef NGP_CORE_ENABLED
#include "ngp/run_ngp.h"
#endif
#ifdef WS_CORE_ENABLED
#include "ws/run_ws.h"
#endif
#ifdef PCE_CORE_ENABLED
#include "pce/run_pce.h"
#endif
#ifdef LYNX_CORE_ENABLED
#include "lynx/run_lynx.h"
#endif
#ifdef MD_CORE_ENABLED
#include "genesis/run_genesis.h"
#endif
#ifdef GB_CORE_ENABLED
#include "gbc/run_gbc.h"
#endif
#ifdef MSX_CORE_ENABLED
#include "msx/run_msx.h"
#endif
#ifdef SNES_CORE_ENABLED
#include "snes/run_snes.h"
#endif
#ifdef A7800_CORE_ENABLED
#include "atari7800/run_a7800.h"
#endif
#ifdef A2600_CORE_ENABLED
#include "atari2600/run_a2600.h"
#endif
#ifdef GX4000_CORE_ENABLED
#include "gx4000/run_gx4000.h"
#endif
#include "last_game.h"
#ifdef NGP_CORE_ENABLED
#define RETRO_COMPAT_IMPLEMENTATION
#include "ngp/race/retro_compat.h"
#endif
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/input.h"
#include "share/emu_log_cpp.h"
#include "share/boot_log.h"
#include "share/sd_control.h"

static constexpr size_t COLECO_BIOS_SIZE = 8192;

#if EMU_LOG_MASTER_ENABLED
static const char* emulatorNameForLog(RomType type) {
  switch (type) {
#ifdef NES_CORE_ENABLED
    case ROM_TYPE_NES:       return "NES";
#endif
#ifdef SMS_CORE_ENABLED
    case ROM_TYPE_SMS:       return "SMS";
    case ROM_TYPE_GAMEGEAR:  return "Game Gear";
    case ROM_TYPE_SG1000:    return "SG-1000";
    case ROM_TYPE_COLECO:    return "ColecoVision";
#endif
#ifdef NGP_CORE_ENABLED
    case ROM_TYPE_NGP:       return "Neo Geo Pocket";
#endif
#ifdef MD_CORE_ENABLED
    case ROM_TYPE_GENESIS:   return "Mega Drive";
#endif
#ifdef WS_CORE_ENABLED
    case ROM_TYPE_WS:        return "WonderSwan";
#endif
#ifdef PCE_CORE_ENABLED
    case ROM_TYPE_PCE:       return "PC Engine";
#endif
#ifdef GB_CORE_ENABLED
    case ROM_TYPE_GB:        return "Game Boy";
#endif
#ifdef LYNX_CORE_ENABLED
    case ROM_TYPE_LYNX:      return "Lynx";
#endif
#ifdef SNES_CORE_ENABLED
    case ROM_TYPE_SNES:      return "SNES";
#endif
#ifdef MSX_CORE_ENABLED
    case ROM_TYPE_MSX:       return "MSX";
#endif
#ifdef A7800_CORE_ENABLED
    case ROM_TYPE_ATARI7800: return "Atari 7800";
#endif
#ifdef A2600_CORE_ENABLED
    case ROM_TYPE_ATARI2600: return "Atari 2600";
#endif
#ifdef GX4000_CORE_ENABLED
    case ROM_TYPE_GX4000:    return "GX4000";
#endif
    case ROM_TYPE_UNKNOWN:
    default:                 return "Unknown";
  }
}

static const char* emulatorHelperCoresForLog(RomType type) {
  switch (type) {
#ifdef NES_CORE_ENABLED
    case ROM_TYPE_NES:
      return "display=CPU0";
#endif
#ifdef SMS_CORE_ENABLED
    case ROM_TYPE_SMS:
    case ROM_TYPE_GAMEGEAR:
    case ROM_TYPE_SG1000:
    case ROM_TYPE_COLECO:
      return "audio=CPU0";
#endif
#ifdef NGP_CORE_ENABLED
    case ROM_TYPE_NGP:
      return "input/audio/video=CPU0";
#endif
#ifdef MD_CORE_ENABLED
    case ROM_TYPE_GENESIS:
      return "display/audio/save=CPU0";
#endif
#if defined(WS_CORE_ENABLED) || defined(PCE_CORE_ENABLED) || defined(LYNX_CORE_ENABLED) || defined(A7800_CORE_ENABLED) || defined(A2600_CORE_ENABLED) || defined(GX4000_CORE_ENABLED)
#ifdef WS_CORE_ENABLED
    case ROM_TYPE_WS:
#endif
#ifdef PCE_CORE_ENABLED
    case ROM_TYPE_PCE:
#endif
#ifdef LYNX_CORE_ENABLED
    case ROM_TYPE_LYNX:
#endif
#ifdef A7800_CORE_ENABLED
    case ROM_TYPE_ATARI7800:
#endif
#ifdef A2600_CORE_ENABLED
    case ROM_TYPE_ATARI2600:
#endif
#ifdef GX4000_CORE_ENABLED
    case ROM_TYPE_GX4000:
#endif
      return "display/audio=CPU0";
#endif
#ifdef GB_CORE_ENABLED
    case ROM_TYPE_GB:
      return "display/audio/save=CPU0";
#endif
#ifdef SNES_CORE_ENABLED
    case ROM_TYPE_SNES:
#if defined(SNES_DISPLAY_ON_MAIN)
      return "display/input/save=main";
#else
      return "display=CPU0,input/save=main";
#endif
#endif
#ifdef MSX_CORE_ENABLED
    case ROM_TYPE_MSX:
      return "none";
#endif
    case ROM_TYPE_UNKNOWN:
    default:
      return "unknown";
  }
}

static const char* configuredMainTaskAffinityForLog() {
#if defined(CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0)
  return "CPU0";
#elif defined(CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1)
  return "CPU1";
#elif defined(CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY)
  return "no-affinity";
#else
  return "unknown";
#endif
}

static void logEmulatorCoreUsage(RomType type) {
  EMU_LOG("[CORE] emulator=%s main=CPU%d configured-main=%s helper-tasks=%s\n",
          emulatorNameForLog(type),
          xPortGetCoreID(),
          configuredMainTaskAffinityForLog(),
          emulatorHelperCoresForLog(type));
}
#endif

#if EMU_HEAP_LOGS_ENABLED
extern "C" uint8_t _data_start;
extern "C" uint8_t _data_end;
extern "C" uint8_t _bss_start;
extern "C" uint8_t _bss_end;
extern "C" uint8_t _heap_start;
extern "C" uint8_t _heap_end;

static void logStartupHeapLayoutOnce() {
  static bool printed = false;
  if (printed) return;
  printed = true;

  const uintptr_t dataStart = (uintptr_t)&_data_start;
  const uintptr_t dataEnd = (uintptr_t)&_data_end;
  const uintptr_t bssStart = (uintptr_t)&_bss_start;
  const uintptr_t bssEnd = (uintptr_t)&_bss_end;
  const uintptr_t heapStart = (uintptr_t)&_heap_start;
  const uintptr_t heapEnd = (uintptr_t)&_heap_end;

  EMU_LOG("[HEAP][DRAM] static sections in 0x3fc... reduce/shape the initial contiguous heap\n");
  EMU_LOG("[HEAP][DRAM] .data %p-%p size=%lu\n",
          &_data_start, &_data_end, (unsigned long)(dataEnd - dataStart));
  EMU_LOG("[HEAP][DRAM] .bss  %p-%p size=%lu\n",
          &_bss_start, &_bss_end, (unsigned long)(bssEnd - bssStart));
  EMU_LOG("[HEAP][DRAM] heap  %p-%p span=%lu\n",
          &_heap_start, &_heap_end, (unsigned long)(heapEnd - heapStart));
  EMU_LOG("[HEAP][CONFIG] main_task_stack=%u main_task_affinity=%s\n",
          (unsigned)CONFIG_ESP_MAIN_TASK_STACK_SIZE,
          configuredMainTaskAffinityForLog());
  EMU_LOG("[HEAP][REGIONS] internal heap regions follow:\n");
  heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
}

static void logStartupHeap(const char* label) {
  logStartupHeapLayoutOnce();
  EMU_LOG("[HEAP] %-24s free=%lu internal=%lu largest=%lu min_free=%lu\n",
          label,
          (unsigned long)esp_get_free_heap_size(),
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (unsigned long)esp_get_minimum_free_heap_size());
}
#else
static inline void logStartupHeap(const char*) {}
#endif

enum class ColecoFlashStatus {
  Ok,
  BiosOpenFailed,
  BiosSizeInvalid,
  RomOpenFailed,
  RomSizeInvalid,
  TooLarge,
  EraseFailed,
  AllocFailed,
  ReadFailed,
  WriteFailed
};

static bool getFileSizeBytes(const char* path, size_t* outSize) {
  if (outSize) *outSize = 0;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long size = ftell(f);
  fclose(f);
  if (size <= 0) return false;
  if (outSize) *outSize = (size_t)size;
  return true;
}

static bool checkColecoBiosCandidate(
    const std::string& path,
    std::string* outPath,
    size_t* outSize,
    bool* sawInvalidSize
) {
  size_t size = 0;
  if (!getFileSizeBytes(path.c_str(), &size)) return false;
  if (size != COLECO_BIOS_SIZE) {
    if (sawInvalidSize) *sawInvalidSize = true;
    return false;
  }
  if (outPath) *outPath = path;
  if (outSize) *outSize = size;
  return true;
}

static bool findColecoBiosPathForRom(
    const std::string& romPath,
    std::string* outPath,
    size_t* outSize,
    bool* sawInvalidSize
) {
  if (outPath) outPath->clear();
  if (outSize) *outSize = 0;
  if (sawInvalidSize) *sawInvalidSize = false;

  size_t pos = romPath.find_last_of("/\\");
  if (pos != std::string::npos) {
    std::string sameRomFolder = romPath.substr(0, pos + 1) + "coleco.rom";
    if (checkColecoBiosCandidate(sameRomFolder, outPath, outSize, sawInvalidSize)) {
      return true;
    }
  } else if (checkColecoBiosCandidate("coleco.rom", outPath, outSize, sawInvalidSize)) {
    return true;
  }

  static const char* const kColecoBiosDirs[] = {
      "/sd/bios/coleco",
      "/sd/bios/Coleco",
      "/sd/bios",
      "/sd/roms/Coleco",
      "/sd/roms/coleco",
      "/sd/roms",
      "/sd"
  };

  for (size_t i = 0; i < sizeof(kColecoBiosDirs) / sizeof(kColecoBiosDirs[0]); ++i) {
    std::string path = std::string(kColecoBiosDirs[i]) + "/coleco.rom";
    if (checkColecoBiosCandidate(path, outPath, outSize, sawInvalidSize)) {
      return true;
    }
  }
  return false;
}

static ColecoFlashStatus appendFileToPartition(
    const char* path,
    const esp_partition_t* part,
    size_t offset,
    size_t fileSize,
    size_t progressTotal,
    CopyProgressCallback progressCb,
    void* progressCtx
) {
  FILE* f = fopen(path, "rb");
  if (!f) return ColecoFlashStatus::RomOpenFailed;

  uint8_t* buf = (uint8_t*)malloc(8192);
  if (!buf) {
    fclose(f);
    return ColecoFlashStatus::AllocFailed;
  }

  size_t written = 0;
  while (written < fileSize) {
    size_t toRead = fileSize - written;
    if (toRead > 8192) toRead = 8192;

    size_t got = fread(buf, 1, toRead, f);
    if (got == 0) {
      free(buf);
      fclose(f);
      return ColecoFlashStatus::ReadFailed;
    }

    esp_err_t err = esp_partition_write(part, offset + written, buf, got);
    if (err != ESP_OK) {
      free(buf);
      fclose(f);
      return ColecoFlashStatus::WriteFailed;
    }

    written += got;
    if (progressCb) progressCb(progressTotal, offset + written, progressCtx);
  }

  free(buf);
  fclose(f);
  return ColecoFlashStatus::Ok;
}

static ColecoFlashStatus copyColecoBundleToPartition(
    const std::string& romPath,
    const esp_partition_t* part,
    size_t* outMappedSize,
    size_t* outRomOffset,
    size_t* outRomSize,
    CopyProgressCallback progressCb,
    void* progressCtx
) {
  if (outMappedSize) *outMappedSize = 0;
  if (outRomOffset) *outRomOffset = 0;
  if (outRomSize) *outRomSize = 0;
  if (!part) return ColecoFlashStatus::WriteFailed;

  std::string biosPath;
  size_t biosSize = 0;
  size_t romSize = 0;
  bool sawInvalidBiosSize = false;
  if (!findColecoBiosPathForRom(romPath, &biosPath, &biosSize, &sawInvalidBiosSize)) {
    return sawInvalidBiosSize ? ColecoFlashStatus::BiosSizeInvalid
                              : ColecoFlashStatus::BiosOpenFailed;
  }
  if (!getFileSizeBytes(romPath.c_str(), &romSize)) return ColecoFlashStatus::RomOpenFailed;
  if (romSize == 0) return ColecoFlashStatus::RomSizeInvalid;

  const size_t totalSize = COLECO_BIOS_SIZE + romSize;
  if (totalSize > part->size) return ColecoFlashStatus::TooLarge;
  if (!eraseRomPartition(part, totalSize)) return ColecoFlashStatus::EraseFailed;
  if (progressCb) progressCb(totalSize, 0, progressCtx);

  ColecoFlashStatus st = appendFileToPartition(
      biosPath.c_str(), part, 0, COLECO_BIOS_SIZE, totalSize, progressCb, progressCtx);
  if (st != ColecoFlashStatus::Ok) return st;

  st = appendFileToPartition(
      romPath.c_str(), part, COLECO_BIOS_SIZE, romSize, totalSize, progressCb, progressCtx);
  if (st != ColecoFlashStatus::Ok) return st;

  if (outMappedSize) *outMappedSize = totalSize;
  if (outRomOffset) *outRomOffset = COLECO_BIOS_SIZE;
  if (outRomSize) *outRomSize = romSize;
  EMU_LOG("[COL][BIOS] loaded %s before ROM in XIP bundle (%u + %u bytes)\n",
          biosPath.c_str(), (unsigned)COLECO_BIOS_SIZE, (unsigned)romSize);
  return ColecoFlashStatus::Ok;
}

static const char* colecoFlashStatusText(ColecoFlashStatus st) {
  switch (st) {
    case ColecoFlashStatus::BiosOpenFailed: return "Missing coleco.rom";
    case ColecoFlashStatus::BiosSizeInvalid: return "coleco.rom must be 8192 bytes";
    case ColecoFlashStatus::RomOpenFailed: return "ROM open failed";
    case ColecoFlashStatus::RomSizeInvalid: return "Invalid ROM size";
    case ColecoFlashStatus::TooLarge: return "ROM + BIOS too large";
    case ColecoFlashStatus::EraseFailed: return "Flash erase failed";
    case ColecoFlashStatus::AllocFailed: return "Copy buffer failed";
    case ColecoFlashStatus::ReadFailed: return "File read failed";
    case ColecoFlashStatus::WriteFailed: return "Flash write failed";
    case ColecoFlashStatus::Ok:
    default: return "OK";
  }
}

static void showColecoFlashError(CardputerView& display, ColecoFlashStatus st) {
  if (st == ColecoFlashStatus::BiosOpenFailed) {
    static const char* const kMissingBiosLines[] = {
        "Put coleco.rom in:",
        "ROM folder",
        "bios/coleco/",
        "bios/",
        "roms/Coleco/",
        "roms/",
        "or SD root"
    };

    while (1) {
      display.topBar("COLECO BIOS REQUIRED", false, false);
      for (size_t i = 0; i < sizeof(kMissingBiosLines) / sizeof(kMissingBiosLines[0]); ++i) {
        display.subMessage(kMissingBiosLines[i], 1500);
      }
    }
  }

  if (st == ColecoFlashStatus::BiosSizeInvalid) {
    while (1) {
      display.topBar("COLECO BIOS ERROR", false, false);
      display.subMessage("coleco.rom must be", 1500);
      display.subMessage("8192 bytes", 1500);
      display.subMessage("Check BIOS folders", 1500);
    }
  }

  while (1) {
    display.topBar("COLECO BIOS ERROR", false, false);
    display.subMessage(colecoFlashStatusText(st), 1500);
  }
}

static void showRomLimitThenReturn(CardputerView& display) {
  for (int i = 0; i < 2; ++i) {
    display.topBar("ROM WON'T WORK", false, false);
    display.subMessage("This ROM won't work", 1500);
    display.subMessage("Too large for layout", 1500);
  }
  display.topBar("LOAD ROM CARTRIDGE", false, false);
  display.subMessage("Returning to selector", 1000);
}

#if defined(CONFIG_BT_ENABLED)
extern "C" bool btInUse(void) {
  return false;
}
#endif

static void initialize_nvs()
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}
extern "C" void app_main(void) {
  BOOT_LOG("BOOT", "app_main entered");
  logStartupHeap("app_main entry");
  initialize_nvs();
  BOOT_LOG("BOOT", "NVS initialized");
  logStartupHeap("after NVS");

  // Keep the emulator task below the audio/I2S service tasks. A very high
  // priority starves M5Unified's speaker task and causes broken, sporadic audio.
  vTaskPrioritySet(NULL, 5);
  BOOT_LOG("BOOT", "main task priority set=5");

  // Copied from Gameboy Enhanced Firmware setup
#ifdef DISABLE_WATCHDOGS
  BOOT_LOG("BOOT", "disabling task watchdog");
  esp_err_t currentTaskWdt = esp_task_wdt_status(NULL);
  if (currentTaskWdt == ESP_OK) {
    BOOT_LOG("BOOT", "current task is in TWDT, deleting");
    esp_task_wdt_delete(NULL);
  } else {
    BOOT_LOG("BOOT", "current task not in TWDT status=%d", (int)currentTaskWdt);
  }
  esp_err_t deinitWdt = esp_task_wdt_deinit(); // fully disables and removes TWDT
  BOOT_LOG("BOOT", "task watchdog deinit status=%d", (int)deinitWdt);
#endif

  auto cfg = M5.config();
  cfg.output_power = true;
  cfg.external_display_value = 0;
  cfg.external_speaker_value = 0;
  cfg.external_imu = false;
  cfg.external_rtc = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  BOOT_LOG("HW", "M5Cardputer.begin start");
  M5Cardputer.begin(cfg);
  BOOT_LOG("HW", "M5Cardputer.begin done");
  logStartupHeap("after M5 begin");
  BOOT_LOG("INPUT", "creating CardputerInput");
  CardputerInput input;
  BOOT_LOG("SD", "creating SdService");
  SdService sd;
  share::sdRegisterService(&sd);
  BOOT_LOG("DISPLAY", "creating CardputerView");
  CardputerView display;
  BOOT_LOG("DISPLAY", "initialize start");
  display.initialize();
  BOOT_LOG("DISPLAY", "initialize done");
  logStartupHeap("after display init");

  // SD
  BOOT_LOG("SD", "mount start");
  unsigned sdRetry = 0;
  while (!sd.begin()) {
    BOOT_LOG("SD", "mount failed retry=%u", ++sdRetry);
    display.topBar("SD CARD FOR ROMS", false, false);
    display.subMessage("No SD card found", 1000);
    display.subMessage("Insert SD card", 0);
  }
  BOOT_LOG("SD", "mounted");
  logStartupHeap("after SD mount");

  auto romFolder = getRomFolderFromNvs(display, input, sd);
  romFolder = romFolder.empty() ? "/" : romFolder;
  BOOT_LOG("MENU", "ROM folder='%s'", romFolder.c_str());

  std::string romPath;
  RomType ext = ROM_TYPE_UNKNOWN;
  const esp_partition_t* romPart = nullptr;
  size_t mappedSize = 0;
  size_t xipRomOffset = 0;
  size_t xipRomSize = 0;
  ColecoFlashStatus colecoStatus = ColecoFlashStatus::Ok;
  bool copiedToFlash = false;
  bool forceRomSelector = false;
#ifdef SNES_CORE_ENABLED
  SnesInterlaceMode snesInterlaceMode = SNES_INTERLACE_AUTO;
#endif

  for (;;) {
    if (forceRomSelector) {
      romPath = getRomPath(sd, display, input, romFolder, true);
      forceRomSelector = false;
    } else if (isQuittingGame()) {
      romPath = getRomPath(sd, display, input, romFolder, true);
    } else {
      // Welcome
      display.welcome();
      logStartupHeap("after welcome");

      // Try to get last game from NVS or select a new one
      romPath = getLastGameFromNvs(display, input, sd);
      if (romPath.empty()) {
        auto skipWelcome = romFolder == "/" ? false : true;
        romPath = getRomPath(sd, display, input, romFolder, skipWelcome);
      } else {
        romPath = "/sd" + romPath; // ensure sd prefix
      }
    }

    EMU_LOG("Selected ROM: %s\n", romPath.c_str());
    BOOT_LOG("MENU", "selected ROM='%s'", romPath.c_str());
    logStartupHeap("after ROM selection");

    display.topBar("COPYING ROM TO FLASH", false, false);
    display.subMessage("Loading...", 0);
    ext = getRomType(romPath);
    BOOT_LOG("ROM", "type=%d", (int)ext);
    if (ext == ROM_TYPE_UNKNOWN) {
      while (1) {
        display.topBar("UNSUPPORTED ROM", false, false);
        display.subMessage("Select a supported file", 1500);
      }
    }
  
    // Find the rom partition (SPIFFS)
    BOOT_LOG("FLASH", "finding ROM partition");
    romPart = findRomPartition("spiffs");
    if (!romPart) {
      BOOT_LOG("FLASH", "ROM partition not found");
      while (1) {
        display.topBar("ERROR", false, false);
        display.subMessage("No ROM partition", 0);
        delay(1500);
      }
    }
    BOOT_LOG("FLASH", "ROM partition offset=0x%08lx size=%lu",
             (unsigned long)romPart->address, (unsigned long)romPart->size);

    // Copy the ROM file to the partition
    mappedSize = 0;
    xipRomOffset = 0;
    xipRomSize = 0;
    colecoStatus = ColecoFlashStatus::Ok;
    copiedToFlash = false;
    BOOT_LOG("FLASH", "copy to partition start");
    if (ext == ROM_TYPE_COLECO) {
      colecoStatus = copyColecoBundleToPartition(
          romPath, romPart, &mappedSize, &xipRomOffset, &xipRomSize,
          CardputerView::copyProgress, &display);
      copiedToFlash = (colecoStatus == ColecoFlashStatus::Ok);
    } else {
      copiedToFlash = copyFileToPartition(
          romPath.c_str(), romPart, &mappedSize, CardputerView::copyProgress, &display);
      xipRomSize = mappedSize;
    }
    BOOT_LOG("FLASH", "copy done ok=%d mapped=%lu rom_offset=%lu rom_size=%lu status=%d",
             copiedToFlash ? 1 : 0,
             (unsigned long)mappedSize,
             (unsigned long)xipRomOffset,
             (unsigned long)xipRomSize,
             (int)colecoStatus);
    logStartupHeap("after ROM copy");

    if (!copiedToFlash) {
      if (ext == ROM_TYPE_COLECO && colecoStatus != ColecoFlashStatus::TooLarge) {
        showColecoFlashError(display, colecoStatus);
      }
      // User is using the launcher
      if (isLauncherLayout()) {
        // Ask to flash the launcher Game Station partition to unlock full size
        ConfirmationSelector confirm(display, input);
        bool confirmed = confirm.select("ROM IS TOO HEAVY", "Change to 4MB layout?");
        
        if (confirmed) {
          display.topBar("FLASHING PARTITIONS", false, false);
          display.subMessage("Allow up to 4MB roms", 3000);
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
      // ROM limit reached (launcher default 1MB/4MB or normal 5.5MB): show
      // the error twice and return to the selector instead of trapping boot.
      showRomLimitThenReturn(display);
      input.flushInput(10);
      forceRomSelector = true;
      continue;
    }

    break;
  }

  BOOT_LOG("INPUT", "flush input before XIP");
  input.flushInput(10); // flush any input just in case

  // Map the ROM partition in XIP
  BOOT_LOG("XIP", "map start size=%lu", (unsigned long)mappedSize);
  if (xip_map_rom_partition("spiffs", mappedSize) != 0) {
    BOOT_LOG("XIP", "map failed");
    while (1) {
      display.topBar("ERROR", false, false);
      display.subMessage("Map ROM failed", 0);
      delay(1500);
    }
  }
  BOOT_LOG("XIP", "map done base=%p size=%lu", get_rom_ptr(), (unsigned long)get_rom_size());
  // Register the XIP VFS
  BOOT_LOG("XIP", "register VFS");
  vfs_xip_register();
  logStartupHeap("after XIP map");
  const uint8_t* xipBase = get_rom_ptr();
  const uint8_t* xipRomPtr = xipBase ? xipBase + xipRomOffset : nullptr;
  const size_t xipRunSize = xipRomSize ? xipRomSize : get_rom_size();
  const uint8_t* colecoBiosPtr = (ext == ROM_TYPE_COLECO) ? xipBase : nullptr;

#ifdef SNES_CORE_ENABLED
  if (ext == ROM_TYPE_SNES) {
    const std::vector<std::string> interlaceOptions = { "AUTO", "OFF", "ON" };
    const std::vector<std::string> interlaceDescriptions = {
      "FPS-based switching",
      "Stable progressive",
      "Half-line speed mode"
    };
    VerticalSelector selector(display, input);
    const int choice = selector.select(
        "SNES INTERLACE",
        interlaceOptions,
        false,
        false,
        interlaceDescriptions,
        {},
        false,
        true,
        0);
    snesInterlaceMode = (choice == 1)
        ? SNES_INTERLACE_OFF
        : (choice == 2 ? SNES_INTERLACE_ON : SNES_INTERLACE_AUTO);
    BOOT_LOG("SNES", "interlace=%d", (int)snesInterlaceMode);
    input.flushInput(10);
  }
#endif

  // Show keymapping
  display.topBar("- + SOUND [ ] BRIGHT", false, false);
  int numButtons = 2;
#ifdef MD_CORE_ENABLED
  numButtons = (ext == ROM_TYPE_GENESIS) ? 3 : numButtons;
#endif
#ifdef SNES_CORE_ENABLED
  numButtons = (ext == ROM_TYPE_SNES) ? 6 : numButtons;
#endif
  display.showKeymapping(numButtons);

  // Wait for key press or show tips
  BOOT_LOG("INPUT", "waiting for start key");
  uint32_t lastUpdate = millis();
  int state = 0;
  for (;;) {
    char key = input.readChar();
    if (key != KEY_NONE) {
      BOOT_LOG("INPUT", "start key received code=%d", (int)key);
      break;
    }

    // Show tips
    uint32_t now = millis();
    if (now - lastUpdate >= 2000) {
      lastUpdate = now;
      switch (state) {
        case 0: display.topBar("PRESS ANY KEY TO START", false, false); break;
        case 1: display.topBar("KEY \\ SCREEN MODE",       false, false); break;
        case 2: display.topBar("G0 1SEC TO QUIT GAME",     false, false); break;
        case 3: display.topBar("FN + ARROWS FOR ZOOM",     false, false); break;
        case 4: display.topBar("- + SOUND [ ] BRIGHT",     false, false); break;
      }
      state = (state + 1) % 5;
    }
    delay(1);
  }

  // Save last game to nvs
  if (ext != ROM_TYPE_UNKNOWN) {
      saveLastGameToNvs(romPath);
  }

  // Prepare rom filename for emulators
  auto pos = romPath.find_last_of("/\\");
  std::string romName = (pos == std::string::npos) ? romPath : romPath.substr(pos + 1);

  // Initialize I2C M5Stack JoyV2 if any
  BOOT_LOG("INPUT", "external I2C joypad detect start");
  share::detectI2cPad();
  BOOT_LOG("INPUT", "external I2C joypad detect done");
  
  EMU_LOG("HEAP BEFORE EMU: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
  EMU_LOG("MAX BLOCK BEFORE EMU: %lu bytes\n", (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  logStartupHeap("before emulator");
#if EMU_LOG_MASTER_ENABLED
  logEmulatorCoreUsage(ext);
#endif
  BOOT_LOG("EMU", "launch ext=%d name='%s'", (int)ext, romName.c_str());

  // Run the emulator
  bool launched = false;
#ifdef NES_CORE_ENABLED
  if (ext == ROM_TYPE_NES) {
      // --- NES ---
      romName = "/xip/" + romName;
      run_nes(romName.c_str());
      launched = true;
  }
#endif
#ifdef SMS_CORE_ENABLED
  if (!launched && (ext == ROM_TYPE_GAMEGEAR || ext == ROM_TYPE_SMS || ext == ROM_TYPE_SG1000 || ext == ROM_TYPE_COLECO)) {
      // --- SMS family (SMS / GG / SG-1000 / ColecoVision) ---
      SmsConsoleMode mode = SMS_MODE_SMS;
      if (ext == ROM_TYPE_GAMEGEAR) mode = SMS_MODE_GG;
      else if (ext == ROM_TYPE_SG1000) mode = SMS_MODE_SG1000;
      else if (ext == ROM_TYPE_COLECO) mode = SMS_MODE_COLECO;
      run_sms(xipRomPtr, xipRunSize, mode, romName.c_str(),
              colecoBiosPtr, (ext == ROM_TYPE_COLECO) ? COLECO_BIOS_SIZE : 0);
      launched = true;
  }
#endif
#ifdef NGP_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_NGP) {
      // --- Neo Geo Pocket / Color ---
      int machine = detectNeoGeoPocketFromRom(get_rom_ptr(), get_rom_size(), romPath);
      run_ngp(get_rom_ptr(), get_rom_size(), romName.c_str(), machine);
      launched = true;
  }
#endif
#ifdef MD_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_GENESIS) {
      // --- Megadrive / Genesis ---
      run_genesis(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
      BOOT_LOG("EMU", "MD returned, restarting");
      esp_restart();
  }
#endif
#ifdef WS_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_WS) {
      // --- WonderSwan / Color ---
      run_ws(get_rom_ptr(), get_rom_size(), romName.c_str(), detectWonderSwanFromRom(romPath));
      launched = true;
  }
#endif
#ifdef PCE_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_PCE) {
      // --- PC Engine / TurboGrafx-16 ---
      run_pce(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
  }
#endif
#ifdef GB_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_GB) {
      // --- Game Boy / Color ---
      run_gbc(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
  }
#endif
#ifdef LYNX_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_LYNX) {
      // --- Lynx ---
      run_lynx(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
  }
#endif
#ifdef SNES_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_SNES) {
      // --- SNES / Super Famicom ---
      display.displaySnesInfo();
      input.waitPress();
#ifdef SNES_SD_OFF_DURING_GAMEPLAY
      BOOT_LOG("SD", "close before SNES init");
      share_sd_close();
      logStartupHeap("after SNES SD close");
#endif
      run_snes(get_rom_ptr(), get_rom_size(), romName.c_str(), snesInterlaceMode);
      launched = true;
      BOOT_LOG("EMU", "SNES returned, restarting");
      esp_restart();
  }
#endif
#ifdef MSX_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_MSX) {
      // --- MSX ---
      display.displayMsxInfo();
      input.waitPress();
      run_msx(get_rom_ptr(), get_rom_size(), romName.c_str(), romPath.c_str());
      launched = true;
  }
#endif
#ifdef A7800_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_ATARI7800) {
      // --- Atari 7800 ---
      run_a7800(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
  }
#endif
#ifdef A2600_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_ATARI2600) {
      // --- Atari 2600 ---
      run_a2600(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
  }
#endif
#ifdef GX4000_CORE_ENABLED
  if (!launched && ext == ROM_TYPE_GX4000) {
      // --- Amstrad GX4000 ---
      run_gx4000(get_rom_ptr(), get_rom_size(), romName.c_str());
      launched = true;
    }
#endif
  if (!launched) {
      display.topBar("ERROR", false, false);
      display.subMessage("Unsupported ROM type", 0);
      while (1) delay(1000);
  }
}

