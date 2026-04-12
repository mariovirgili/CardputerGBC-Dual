#include "msx_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>

#include "../cardputer/CardputerView.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../tft_setup.h"

#include "msx_config.h"
#include "msx_input.h"
#include "msx_video.h"

#include <cstdio>
#include <cctype>
#include <string>

static constexpr uint32_t kPlaceholderRedrawMs = 150;
static constexpr uint32_t kDisplayDiagIntervalMs = 2000;
static constexpr int kExternalDisplayW = 320;
static constexpr int kExternalDisplayH = 240;
static uint32_t s_lastPlaceholderMs = 0;
static uint32_t s_lastDisplayDiagMs = 0;
static MsxInternalViewMode s_lastViewMode = MsxInternalViewMode::Wide;
static MsxInputOverlayState s_lastMenuOverlay = {};
static bool s_lastMenuVisible = false;
static constexpr uint8_t kRuntimeMenuRowCount = 5u;
static constexpr int kRuntimeMenuBoxW = 168;
static constexpr int kRuntimeMenuBoxH = 104;
static constexpr int kRuntimeMenuInnerPad = 8;
static constexpr int kRuntimeMenuRowH = 13;
static constexpr int kRuntimeMenuSelectionRadius = 3;
static constexpr int kRuntimeMenuShadowOffset = 4;

static bool msx_display_game_on_external(void)
{
    return g_emu_display_target == EMU_DISPLAY_EXTERNAL;
}

static int msx_display_runtime_menu_box_y(void)
{
    return msx_display_game_on_external() ? 42 : 22;
}

static int msx_display_runtime_menu_first_row_y(void)
{
    return msx_display_runtime_menu_box_y() + 22;
}

static void msx_display_prepare_external_tft(void)
{
    msx_video_prepare_external_ui();
}

static TFT_eSPI& msx_display_external_tft(void)
{
    return msx_video_external_tft();
}

static int msx_display_active_width(void)
{
    return msx_display_game_on_external() ? kExternalDisplayW : M5Cardputer.Display.width();
}

static int msx_display_active_height(void)
{
    return msx_display_game_on_external() ? kExternalDisplayH : M5Cardputer.Display.height();
}

static int msx_display_external_font_from_legacy_id(uint8_t font)
{
    switch (font) {
        case 4:
        case 6:
        case 7:
        case 8:
            return 4;
        case 2:
            return 2;
        case 0:
        case 1:
        case 3:
        case 5:
        default:
            return 1;
    }
}

static std::string msx_display_truncate(const char* text, size_t maxChars)
{
    if (!text) {
        return {};
    }

    std::string value(text);
    if (value.size() <= maxChars) {
        return value;
    }

    if (maxChars <= 3u) {
        return value.substr(0u, maxChars);
    }

    return value.substr(0u, maxChars - 3u) + "...";
}

static bool msx_display_has_suffix_ignore_case(const std::string& value, const char* suffix)
{
    if (!suffix) {
        return false;
    }

    const size_t suffixLen = std::strlen(suffix);
    if (value.size() < suffixLen) {
        return false;
    }

    const size_t start = value.size() - suffixLen;
    for (size_t i = 0; i < suffixLen; ++i) {
        const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(value[start + i])));
        const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (lhs != rhs) {
            return false;
        }
    }

    return true;
}

static std::string msx_display_clean_title(const char* romTitle)
{
    if (!romTitle || romTitle[0] == '\0') {
        return "MSX";
    }

    std::string title(romTitle);
    const size_t slashPos = title.find_last_of("/\\");
    if (slashPos != std::string::npos) {
        title.erase(0u, slashPos + 1u);
    }

    if (msx_display_has_suffix_ignore_case(title, ".rom") ||
        msx_display_has_suffix_ignore_case(title, ".dsk")) {
        title.erase(title.size() - 4u);
    }

    if (title.empty()) {
        return "MSX";
    }

    return title;
}

static std::string msx_display_fit_external_title(const std::string& title, uint8_t* fontOut)
{
    if (!fontOut) {
        return title;
    }

    static constexpr uint8_t kCandidateFonts[] = {4u, 2u, 1u};
    static constexpr int kMaxTitleWidth = kExternalDisplayW - 28;
    auto& tft = msx_display_external_tft();

    for (uint8_t font : kCandidateFonts) {
        if (tft.textWidth(title.c_str(), font) <= kMaxTitleWidth) {
            *fontOut = font;
            return title;
        }
    }

    std::string fitted = title;
    while (!fitted.empty()) {
        const std::string candidate = fitted + "...";
        if (tft.textWidth(candidate.c_str(), 1) <= kMaxTitleWidth) {
            *fontOut = 1u;
            return candidate;
        }
        fitted.pop_back();
    }

    *fontOut = 1u;
    return "MSX";
}

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
    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        tft.setTextColor(color, TFT_BLACK);
        tft.drawCentreString(
            text ? text : "",
            kExternalDisplayW / 2,
            y,
            msx_display_external_font_from_legacy_id(font)
        );
        return;
    }

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
    const int displayW = msx_display_active_width();
    const int displayH = msx_display_active_height();
    const int boxX = 6;
    const int boxY = 10;
    const int boxW = displayW - 12;
    const int boxH = displayH - 20;

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        tft.fillScreen(TFT_BLACK);
        tft.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        tft.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
    } else {
        auto& display = M5Cardputer.Display;
        display.fillScreen(TFT_BLACK);
        display.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        display.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
    }

    msx_display_draw_line("MSX VIDEO", 16, PRIMARY_COLOR, 4);
    msx_display_draw_line(status && status->romName ? status->romName : "No ROM name", 36, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->coreLine ? status->coreLine : "CORE: waiting for VDP", 54, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->cartLine ? status->cartLine : "CART: not loaded", 70, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->machineLine ? status->machineLine : "MACHINE: MSX1", 86, TEXT_COLOR, 2);
    msx_display_draw_line(status && status->biosLine ? status->biosLine : "BIOS: not loaded", 102, TEXT_COLOR, 1);
    msx_display_draw_line(status && status->audioLine ? status->audioLine : "AUDIO: hook ready", 116, TEXT_COLOR, 1);

    char footer[48];
    std::snprintf(footer,
                  sizeof(footer),
                  "VIEW %s  FRAME %lu",
                  msx_config_get_active_view_mode_label(),
                  static_cast<unsigned long>(status ? status->frameCounter : 0));
    msx_display_draw_line(footer, 128, PRIMARY_COLOR, 1);
}

static const char* msx_display_runtime_menu_label(uint8_t index)
{
    switch (index) {
        case 0u:
            return "JOY";
        case 1u:
            return "KEYBOARD";
        case 2u:
            return "VAUS";
        case 3u:
            return "VIEW";
        case 4u:
            return "CLOSE";
        default:
            return "";
    }
}

static const char* msx_display_runtime_menu_value(const MsxInputOverlayState& overlay, uint8_t index)
{
    switch (index) {
        case 0u:
            return overlay.joystickEnabled ? "ON" : "OFF";
        case 1u:
            return overlay.keyboardEnabled ? "ON" : "OFF";
        case 2u:
            return overlay.vausEnabled ? "ON" : "OFF";
        case 3u:
            return msx_display_game_on_external() ? "1:1" : msx_config_get_active_view_mode_label();
        default:
            return nullptr;
    }
}

static int msx_display_runtime_menu_box_x(void)
{
    return (msx_display_active_width() - kRuntimeMenuBoxW) / 2;
}

static void msx_display_draw_runtime_menu_shell(void)
{
    const int boxX = msx_display_runtime_menu_box_x();
    const int boxY = msx_display_runtime_menu_box_y();
    const int innerX = boxX + kRuntimeMenuInnerPad;

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        tft.fillRoundRect(boxX + kRuntimeMenuShadowOffset, boxY + kRuntimeMenuShadowOffset, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
        tft.fillRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
        tft.drawRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        tft.drawRoundRect(boxX + 2, boxY + 2, kRuntimeMenuBoxW - 4, kRuntimeMenuBoxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
        tft.drawString("MSX MENU", innerX, boxY + 6, 2);
        tft.setTextColor(TEXT_COLOR, TFT_BLACK);
        tft.drawString(msx_display_game_on_external() ? "EXT TFT fixed 1:1" : "\\ quick view  GO toggle",
                       innerX, boxY + kRuntimeMenuBoxH - 19, 1);
        tft.drawString("Hold GO closes menu", innerX, boxY + kRuntimeMenuBoxH - 10, 1);
        return;
    }

    auto& display = M5Cardputer.Display;
    display.fillRoundRect(boxX + kRuntimeMenuShadowOffset, boxY + kRuntimeMenuShadowOffset, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
    display.fillRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
    display.drawRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
    display.drawRoundRect(boxX + 2, boxY + 2, kRuntimeMenuBoxW - 4, kRuntimeMenuBoxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
    display.setTextDatum(top_left);
    display.setFont(&fonts::Font2);
    display.setTextColor(PRIMARY_COLOR, TFT_BLACK);
    display.drawString("MSX MENU", innerX, boxY + 6);
    display.setFont(&fonts::Font0);
    display.setTextColor(TEXT_COLOR, TFT_BLACK);
    display.drawString("\\ quick view  GO toggle", innerX, boxY + kRuntimeMenuBoxH - 19);
    display.drawString("Hold GO closes menu", innerX, boxY + kRuntimeMenuBoxH - 10);
}

static void msx_display_draw_runtime_menu_row_state(const MsxInputOverlayState& overlay, uint8_t index, bool selected)
{
    if (index >= kRuntimeMenuRowCount) {
        return;
    }

    const int boxX = msx_display_runtime_menu_box_x();
    const int innerX = boxX + kRuntimeMenuInnerPad;
    const int valueX = boxX + kRuntimeMenuBoxW - 44;
    const int y = msx_display_runtime_menu_first_row_y() + static_cast<int>(index) * kRuntimeMenuRowH;
    const int rowX = innerX - 4;
    const int rowY = y - 2;
    const int rowW = kRuntimeMenuBoxW - 16;
    const int rowH = kRuntimeMenuRowH - 1;
    const char* label = msx_display_runtime_menu_label(index);
    const char* value = msx_display_runtime_menu_value(overlay, index);

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        const uint16_t rowBg = selected ? RECT_COLOR_DARK : TFT_BLACK;
        const uint16_t rowFg = selected ? TFT_ORANGE : TFT_WHITE;
        tft.fillRoundRect(rowX,
                          rowY,
                          rowW,
                          rowH,
                          kRuntimeMenuSelectionRadius,
                          rowBg);
        tft.setTextColor(rowFg, rowBg);
        tft.drawString(label, innerX, y, 1);
        if (value) {
            tft.drawString(value, valueX, y, 1);
        }
        return;
    }

    auto& display = M5Cardputer.Display;
    display.fillRoundRect(rowX,
                          rowY,
                          rowW,
                          rowH,
                          kRuntimeMenuSelectionRadius,
                          selected ? RECT_COLOR_DARK : TFT_BLACK);
    display.setFont(&fonts::Font0);
    display.setTextColor(selected ? PRIMARY_COLOR : TEXT_COLOR, selected ? RECT_COLOR_DARK : TFT_BLACK);
    display.drawString(label, innerX, y);
    if (value) {
        display.drawString(value, valueX, y);
    }
}

static void msx_display_draw_runtime_menu_row(const MsxInputOverlayState& overlay, uint8_t index)
{
    msx_display_draw_runtime_menu_row_state(overlay, index, overlay.selectedIndex == index);
}

static void msx_display_draw_runtime_menu(const MsxInputOverlayState& overlay)
{
    if (!overlay.menuVisible) {
        return;
    }

    msx_display_draw_runtime_menu_shell();
    for (uint8_t i = 0; i < kRuntimeMenuRowCount; ++i) {
        msx_display_draw_runtime_menu_row(overlay, i);
    }
}

static void msx_display_draw_key_badge(int x, int y, const std::string& key)
{
    const int badgeW = 34;
    const int badgeH = 18;

    auto& tft = msx_display_external_tft();
    tft.fillRoundRect(x, y, badgeW, badgeH, 4, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawCentreString(key.c_str(), x + badgeW / 2, y + 1, 2);
    tft.drawRoundRect(x, y, badgeW, badgeH, 4, TFT_YELLOW);
}

void msx_display_init(void)
{
    s_lastPlaceholderMs = 0;
    s_lastDisplayDiagMs = 0;
    s_lastViewMode = msx_config_get_active_view_mode();
    s_lastMenuOverlay = {};
    s_lastMenuVisible = false;
    msx_video_init();
}

void msx_display_shutdown(void)
{
    msx_video_shutdown();
}

void msx_display_submit_frame(const MsxDisplayFrame* frame, const MsxDisplayStatus* status)
{
    const uint32_t nowMs = millis();
    MsxInputOverlayState overlay = {};
    msx_input_get_overlay_state(&overlay);

    if (s_lastDisplayDiagMs == 0 || (uint32_t)(nowMs - s_lastDisplayDiagMs) >= kDisplayDiagIntervalMs) {
        s_lastDisplayDiagMs = nowMs;
        std::printf("[MSX][DISP] frame=%p indexed8=%p w=%u h=%u pitch=%u fc=%lu\n",
                    static_cast<const void*>(frame),
                    frame ? static_cast<const void*>(frame->indexed8) : nullptr,
                    frame ? frame->width : 0u,
                    frame ? frame->height : 0u,
                    frame ? static_cast<unsigned>(frame->pitchBytes) : 0u,
                    static_cast<unsigned long>(status ? status->frameCounter : 0u));
    }

    const MsxInternalViewMode currentViewMode = msx_config_get_active_view_mode();
    if (overlay.menuVisible) {
        msx_video_lock();
        msx_video_set_runtime_menu_active(true);
        if (!s_lastMenuVisible) {
            msx_display_draw_runtime_menu(overlay);
        } else {
            if (overlay.selectedIndex != s_lastMenuOverlay.selectedIndex) {
                msx_display_draw_runtime_menu_row_state(overlay, s_lastMenuOverlay.selectedIndex, false);
                msx_display_draw_runtime_menu_row(overlay, overlay.selectedIndex);
            }

            if (overlay.joystickEnabled != s_lastMenuOverlay.joystickEnabled) {
                msx_display_draw_runtime_menu_row(overlay, 0u);
            }
            if (overlay.keyboardEnabled != s_lastMenuOverlay.keyboardEnabled) {
                msx_display_draw_runtime_menu_row(overlay, 1u);
            }
            if (overlay.vausEnabled != s_lastMenuOverlay.vausEnabled) {
                msx_display_draw_runtime_menu_row(overlay, 2u);
            }
            if (currentViewMode != s_lastViewMode) {
                msx_display_draw_runtime_menu_row(overlay, 3u);
            }
        }

        s_lastViewMode = currentViewMode;
        s_lastMenuOverlay = overlay;
        s_lastMenuVisible = true;
        msx_video_unlock();
        return;
    }

    if (s_lastMenuVisible) {
        msx_video_lock();
        if (msx_display_game_on_external()) {
            msx_video_finish_external_ui();
        }
        msx_video_set_runtime_menu_active(false);
        s_lastPlaceholderMs = 0;
        msx_video_unlock();

        msx_video_request_full_redraw();
    }
    s_lastMenuVisible = false;

    if (frame && frame->indexed8) {
        return;
    }

    const bool forceRedraw =
        (s_lastPlaceholderMs == 0) ||
        (currentViewMode != s_lastViewMode) ||
        (status && status->frameCounter < 2);

    if (!forceRedraw && (uint32_t)(nowMs - s_lastPlaceholderMs) < kPlaceholderRedrawMs) {
        return;
    }

    s_lastPlaceholderMs = nowMs;
    s_lastViewMode = currentViewMode;
    
    msx_video_lock();
    msx_display_draw_placeholder(status);
    msx_video_unlock();
}

void msx_display_show_external_info(const char* romTitle)
{
    msx_video_lock();
    msx_display_prepare_external_tft();
    auto& tft = msx_display_external_tft();
    tft.fillScreen(TFT_BLACK);
    tft.setTextWrap(false);

    tft.drawRoundRect(8, 8, kExternalDisplayW - 16, kExternalDisplayH - 16, 8, TFT_DARKGREY);

    uint8_t titleFont = 4u;
    const std::string cleanedTitle = msx_display_clean_title(romTitle);
    const std::string title = msx_display_fit_external_title(cleanedTitle, &titleFont);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString(title.c_str(), kExternalDisplayW / 2, 16, titleFont);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("MSX", kExternalDisplayW / 2, 46, 2);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("VIDEO ON INTERNAL LCD", kExternalDisplayW / 2, 64, 2);

    tft.drawRoundRect(12, 86, kExternalDisplayW - 24, 98, 6, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("CONTROLS", kExternalDisplayW / 2, 92, 2);

    const auto actions = share::emuControlActionLabels(share::EmuProfile::MSX);
    const auto keys = share::emuControlKeyLabels(share::EmuProfile::MSX);
    const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
    const size_t rowsPerCol = 4u;

    for (size_t i = 0; i < count; ++i) {
        const int col = static_cast<int>(i / rowsPerCol);
        const int row = static_cast<int>(i % rowsPerCol);
        const int baseX = 24 + col * 146;
        const int baseY = 112 + row * 16;

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(actions[i].c_str(), baseX, baseY, 2);
        msx_display_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
    }

    tft.drawFastHLine(18, 190, kExternalDisplayW - 36, TFT_DARKGREY);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("GO = QUIT   HOLD GO = MENU", kExternalDisplayW / 2, 198, 1);
    tft.drawCentreString("\\ = VIEW", kExternalDisplayW / 2, 210, 1);
    tft.drawCentreString("FN+ARROWS = ZOOM", kExternalDisplayW / 2, 222, 1);
    msx_video_unlock();
}
