#include "msx_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "../cardputer/CardputerView.h"

#include "msx_config.h"
#include "msx_video.h"

#include <cstdio>

static constexpr uint32_t kPlaceholderRedrawMs = 150;
static uint32_t s_lastPlaceholderMs = 0;
static MsxInternalViewMode s_lastViewMode = MsxInternalViewMode::Wide;
static const lgfx::IFont* msx_display_font_from_legacy_id(uint8_t font)
{
    switch (font) {
        case 2:
            return &fonts::Font2;
        case 4:
            return &fonts::Font4;
        case 6:
            return &fonts::Font6;
        case 7:
            return &fonts::Font7;
        case 8:
            return &fonts::Font8;
        case 0:
        case 1:
        case 3:
        case 5:
        default:
            return &fonts::Font0;
    }
}

static void msx_display_draw_line(const char* text, int y, uint16_t color, uint8_t font = 2)
{
    M5Cardputer.Display.setTextColor(color, TFT_BLACK);
    M5Cardputer.Display.drawCenterString(
        text ? text : "",
        M5Cardputer.Display.width() / 2,
        y,
        msx_display_font_from_legacy_id(font)
    );
}

static void msx_display_draw_placeholder(const MsxDisplayStatus* status)
{
    auto& display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);

    const int boxX = 6;
    const int boxY = 10;
    const int boxW = display.width() - 12;
    const int boxH = display.height() - 20;

    display.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
    display.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);

    msx_display_draw_line("MSX VIDEO", 16, PRIMARY_COLOR, 4);
    msx_display_draw_line(status && status->romName ? status->romName : "No ROM name", 36, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->coreLine ? status->coreLine : "CORE: waiting for VDP", 54, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->cartLine ? status->cartLine : "CART: not loaded", 70, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->machineLine ? status->machineLine : "MACHINE: AUTO", 86, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->biosLine ? status->biosLine : "BIOS: not loaded", 102, TEXT_COLOR, 1);
    msx_display_draw_line(status && status->audioLine ? status->audioLine : "AUDIO: hook ready", 116, TEXT_COLOR, 1);

    char footer[48];
    std::snprintf(footer,
                  sizeof(footer),
                  "VIEW %s  FRAME %lu",
                  msx_config_get_internal_view_mode_label(),
                  static_cast<unsigned long>(status ? status->frameCounter : 0));
    msx_display_draw_line(footer, 128, PRIMARY_COLOR, 1);
}

void msx_display_init(void)
{
    s_lastPlaceholderMs = 0;
    s_lastViewMode = msx_config_get_internal_view_mode();
    msx_video_init();
}

void msx_display_shutdown(void)
{
    msx_video_shutdown();
}

void msx_display_submit_frame(const MsxDisplayFrame* frame, const MsxDisplayStatus* status)
{
    if (frame && frame->indexed8 && msx_video_present_frame(frame)) {
        return;
    }

    const MsxInternalViewMode currentViewMode = msx_config_get_internal_view_mode();
    const uint32_t nowMs = millis();
    const bool forceRedraw =
        (s_lastPlaceholderMs == 0) ||
        (currentViewMode != s_lastViewMode) ||
        (status && status->frameCounter < 2);

    if (!forceRedraw && (uint32_t)(nowMs - s_lastPlaceholderMs) < kPlaceholderRedrawMs) {
        return;
    }

    s_lastPlaceholderMs = nowMs;
    s_lastViewMode = currentViewMode;
    msx_display_draw_placeholder(status);
}


