#pragma GCC optimize ("Os")

#include "run_msx.h"

#include <M5Cardputer.h>
#ifdef word
#undef word
#endif
#define word arduino_word
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "msx_host.h"
#include "share/emu_log_cpp.h"

extern "C" {
#include "fMSX/MSX.h"
}
#undef word

namespace {

static const char* const kMsxBiosPaths[] = {
    "/sd/msx/MSX.ROM",
    "/sd/roms/MSX.ROM",
    "/sd/bios/MSX.ROM",
    "/sd/MSX.ROM",
};

static const char* const kMsxBiosDisplayPaths[] = {
    "/msx/MSX.ROM",
    "/roms/MSX.ROM",
    "/bios/MSX.ROM",
    "/MSX.ROM",
};

static void show_missing_bios_screen()
{
    auto& display = M5Cardputer.Display;
    display.setRotation(1);
    display.setSwapBytes(false);
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(8, 10);
    display.print("MSX BIOS MISSING");

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(8, 30);
    display.print("Copy MSX.ROM to one of:");

    for (unsigned i = 0; i < sizeof(kMsxBiosDisplayPaths) / sizeof(kMsxBiosDisplayPaths[0]); ++i) {
        display.setCursor(12, 48 + (int)i * 14);
        display.print(kMsxBiosDisplayPaths[i]);
    }

    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.setCursor(8, 116);
    display.print("Then restart the emulator");
}

int choose_mode_from_extension(const char* romName)
{
    const char* dot = romName ? std::strrchr(romName, '.') : nullptr;
    if (!dot) return -1;

    if (!strcasecmp(dot, ".mx1")) return MSX_MSX1;
    return -1;
}

int choose_best_available_mode(int preferred)
{
    const int base = MSX_NTSC | MSX_GUESSA | MSX_GUESSB;

    if (preferred >= 0) {
        const int mode = base | preferred;
        if (msx_host_load_bios_for_mode(mode)) return mode;
        EMU_LOG("[MSX] Preferred BIOS set missing for requested mode\n");
    }

        const int candidates[] = { MSX_MSX1};
    for (int model : candidates) {
        const int mode = base | model;
        if (msx_host_load_bios_for_mode(mode)) return mode;
    }

    return -1;
}

} // namespace

void run_msx(const uint8_t* rom, size_t len, const char* romName, const char* romPath)
{
    EMU_LOG("[MSX] ===== fMSX Start =====\n");
    EMU_LOG("[MSX] ROM size: %u bytes (%s)\n", (unsigned)len, romName ? romName : "(null)");

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);

    msx_host_set_game(rom, (unsigned int)len, romName, romPath);
    msx_host_set_view_mode(MSX_HOST_VIEW_FIT43);
    EMU_LOG("[MSX] Default debug zoom enabled (~20%%)\n");

    const int preferred = choose_mode_from_extension(romName);
    const int mode = choose_best_available_mode(preferred);
    if (mode < 0) {
        EMU_LOG("[MSX][ERR] Missing BIOS. Expected MSX.ROM in:\n");
        for (unsigned i = 0; i < sizeof(kMsxBiosPaths) / sizeof(kMsxBiosPaths[0]); ++i) {
            EMU_LOG("[MSX][ERR]   %s\n", kMsxBiosPaths[i]);
        }
        show_missing_bios_screen();
        for (;;) delay(1000);
    }

    msx_host_set_model_mode(mode);
    EMU_LOG("[MSX] Selected model: %s\n",
           (mode & MSX_MODEL) == MSX_MSX2P ? "MSX2+" :
           (mode & MSX_MODEL) == MSX_MSX2  ? "MSX2"  : "MSX1");

    Verbose = 1;
    UPeriod = 100;

    if (!InitMachine()) {
        EMU_LOG("[MSX][ERR] InitMachine failed\n");
        for (;;) delay(1000);
    }

#ifdef EMU_LOGS_ENABLED
    msx_host_perf_reset();
#endif

    if (!StartMSX(mode, 4, 2)) {
        EMU_LOG("[MSX][ERR] StartMSX failed\n");
        for (;;) delay(1000);
    }

    TrashMSX();
    TrashMachine();
    msx_host_unload_bios();
    msx_host_clear_game();
}
