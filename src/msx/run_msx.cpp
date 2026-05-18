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

void draw_centered(const char* text, int y, float size, uint16_t color)
{
    auto& display = M5Cardputer.Display;
    display.setTextSize(size);
    display.setTextColor(color);
    const int x = (display.width() - display.textWidth(text)) / 2;
    display.setCursor(x < 0 ? 0 : x, y);
    display.printf("%s", text);
}

void show_missing_bios_message()
{
    auto& display = M5Cardputer.Display;
    display.setRotation(1);
    display.setSwapBytes(true);
    display.fillScreen(TFT_BLACK);
    display.drawRect(1, 1, display.width() - 2, display.height() - 2, TFT_ORANGE);

    draw_centered("MSX.ROM REQUIRED", 7, 1.45f, TFT_ORANGE);
    draw_centered("File: MSX.ROM", 25, 1.0f, TFT_WHITE);
    draw_centered("Folders:", 39, 1.0f, TFT_ORANGE);
    draw_centered("ROM folder", 52, 0.95f, TFT_WHITE);
    draw_centered("msx/", 64, 0.95f, TFT_WHITE);
    draw_centered("msx_bios/", 76, 0.95f, TFT_WHITE);
    draw_centered("bios/msx/", 88, 0.95f, TFT_WHITE);
    draw_centered("bios/", 100, 0.95f, TFT_WHITE);
    draw_centered("roms/msx/ or roms/", 112, 0.95f, TFT_WHITE);
    draw_centered("or SD root", 124, 0.95f, TFT_ORANGE);
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
        EMU_LOG("[MSX][ERR] Missing BIOS. Expected MSX.ROM in ROM folder, msx, msx_bios, bios/msx, bios, SD root, roms/msx, or roms\n");
        show_missing_bios_message();
        for (;;) delay(1000);
    }

    msx_host_set_model_mode(mode);
    EMU_LOG("[MSX] Selected model: %s\n",
           (mode & MSX_MODEL) == MSX_MSX2P ? "MSX2+" :
           (mode & MSX_MODEL) == MSX_MSX2  ? "MSX2"  : "MSX1");

#if MSX_LOGS_ENABLED
    Verbose = 1;
#else
    Verbose = 0;
#endif
    UPeriod = 100;

    if (!InitMachine()) {
        EMU_LOG("[MSX][ERR] InitMachine failed\n");
        for (;;) delay(1000);
    }

    if (!StartMSX(mode, 4, 2)) {
        EMU_LOG("[MSX][ERR] StartMSX failed\n");
        for (;;) delay(1000);
    }

    TrashMSX();
    TrashMachine();
    msx_host_unload_bios();
    msx_host_clear_game();
}
