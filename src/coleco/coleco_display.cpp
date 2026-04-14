#include "coleco_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>

#include "../cardputer/CardputerView.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../tft_setup.h"

#include "coleco_config.h"
#include "coleco_input.h"
#include "coleco_video.h"

#include <cstdio>
#include <cctype>
#include <string>
#include <cmath>

extern uint8_t coleco_input_get_state_slot(void);
extern uint8_t coleco_input_get_scroll_index(void);

static constexpr uint32_t kPlaceholderRedrawMs = 150;
static constexpr uint32_t kDisplayDiagIntervalMs = 2000;
static constexpr int kExternalDisplayW = 320;
static constexpr int kExternalDisplayH = 240;
static uint32_t s_lastPlaceholderMs = 0;
static uint32_t s_lastDisplayDiagMs = 0;
static ColecoInternalViewMode s_lastViewMode = ColecoInternalViewMode::Wide;
static ColecoInputOverlayState s_lastMenuOverlay = {};
static bool s_lastMenuVisible = false;
static uint8_t s_lastScrollIndex = 0;
static uint8_t s_lastStateSlot = 0;
static constexpr uint8_t kRuntimeMenuRowCount = 5u;
static constexpr int kRuntimeMenuBoxW = 168;
static constexpr int kRuntimeMenuBoxH = 104;
static constexpr int kRuntimeMenuInnerPad = 8;
static constexpr int kRuntimeMenuRowH = 13;
static constexpr int kRuntimeMenuSelectionRadius = 3;
static constexpr int kRuntimeMenuShadowOffset = 4;

static bool coleco_display_game_on_external(void)
{
    return g_emu_display_target == EMU_DISPLAY_EXTERNAL;
}

static int coleco_display_runtime_menu_box_y(void)
{
    return coleco_display_game_on_external() ? 42 : 22;
}

static int coleco_display_runtime_menu_first_row_y(void)
{
    return coleco_display_runtime_menu_box_y() + 22;
}

static void coleco_display_prepare_external_tft(void)
{
    coleco_video_prepare_external_ui();
}

static TFT_eSPI& coleco_display_external_tft(void)
{
    return coleco_video_external_tft();
}

static int coleco_display_active_width(void)
{
    return coleco_display_game_on_external() ? kExternalDisplayW : M5Cardputer.Display.width();
}

static int coleco_display_active_height(void)
{
    return coleco_display_game_on_external() ? kExternalDisplayH : M5Cardputer.Display.height();
}

static int coleco_display_external_font_from_legacy_id(uint8_t font)
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

static const lgfx::IFont* coleco_display_font_from_legacy_id(uint8_t font)
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

static void coleco_display_draw_line(const char* text, int y, uint16_t color, uint8_t font = 2)
{
    if (coleco_display_game_on_external()) {
        coleco_display_prepare_external_tft();
        auto& tft = coleco_display_external_tft();
        tft.setTextColor(color, TFT_BLACK);
        tft.drawCentreString(
            text ? text : "",
            kExternalDisplayW / 2,
            y,
            coleco_display_external_font_from_legacy_id(font)
        );
        return;
    }

    M5Cardputer.Display.setTextColor(color, TFT_BLACK);
    M5Cardputer.Display.drawCenterString(
        text ? text : "",
        M5Cardputer.Display.width() / 2,
        y,
        coleco_display_font_from_legacy_id(font)
    );
}

static void coleco_display_draw_placeholder(const ColecoDisplayStatus* status)
{
    const int displayW = coleco_display_active_width();
    const int displayH = coleco_display_active_height();
    const int boxX = 6;
    const int boxY = 10;
    const int boxW = displayW - 12;
    const int boxH = displayH - 20;

    if (coleco_display_game_on_external()) {
        coleco_display_prepare_external_tft();
        auto& tft = coleco_display_external_tft();
        tft.fillScreen(TFT_BLACK);
        tft.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        tft.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
    } else {
        auto& display = M5Cardputer.Display;
        display.fillScreen(TFT_BLACK);
        display.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        display.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
    }

    coleco_display_draw_line("MSX VIDEO", 16, PRIMARY_COLOR, 4);
    coleco_display_draw_line(status && status->romName ? status->romName : "No ROM name", 36, TEXT_COLOR, 2);
    coleco_display_draw_line(status && status->coreLine ? status->coreLine : "CORE: waiting for VDP", 54, TEXT_COLOR, 2);
    coleco_display_draw_line(status && status->cartLine ? status->cartLine : "CART: not loaded", 70, TEXT_COLOR, 2);
    coleco_display_draw_line(status && status->machineLine ? status->machineLine : "MACHINE: MSX1", 86, TEXT_COLOR, 2);
    coleco_display_draw_line(status && status->biosLine ? status->biosLine : "BIOS: not loaded", 102, TEXT_COLOR, 1);
    coleco_display_draw_line(status && status->audioLine ? status->audioLine : "AUDIO: hook ready", 116, TEXT_COLOR, 1);

    char footer[48];
    std::snprintf(footer,
                  sizeof(footer),
                  "VIEW %s  FRAME %lu",
                  coleco_config_get_active_view_mode_label(),
                  static_cast<unsigned long>(status ? status->frameCounter : 0));
    coleco_display_draw_line(footer, 128, PRIMARY_COLOR, 1);
}

static const char* coleco_display_runtime_menu_label(uint8_t index)
{
    if (coleco_display_game_on_external()) {
        switch (index) {
            case 0u: return "JOY";
            case 1u: return "KEYBOARD";
            case 2u: return "VAUS";
            case 3u: return "VIEW";
            case 4u: return "CLOSE";
            default: return "";
        }
    } else {
        switch (index) {
            case 0u: return "JOY";
            case 1u: return "KEYBOARD";
            case 2u: return "VAUS";
            case 3u: return "VIEW";
            case 4u: return "STATE SLOT";
            case 5u: return "SAVE STATE";
            case 6u: return "LOAD STATE";
            case 7u: return "CLOSE";
            default: return "";
        }
    }
}

static const char* coleco_display_runtime_menu_value(const ColecoInputOverlayState& overlay, uint8_t index)
{
    static char slotStr[8];
    uint8_t mappedIndex = coleco_display_game_on_external() ? (index == 4 ? 7 : index) : index;
    switch (mappedIndex) {
        case 0u:
            return overlay.joystickEnabled ? "ON" : "OFF";
        case 1u:
            return overlay.keyboardEnabled ? "ON" : "OFF";
        case 2u:
            return overlay.vausEnabled ? "ON" : "OFF";
        case 3u:
            return coleco_display_game_on_external() ? "1:1" : coleco_config_get_active_view_mode_label();
        case 4u:
            std::snprintf(slotStr, sizeof(slotStr), "< %u >", static_cast<unsigned>(coleco_input_get_state_slot()));
            return slotStr;
        default:
            return nullptr;
    }
}

static int coleco_display_runtime_menu_box_x(void)
{
    return (coleco_display_active_width() - kRuntimeMenuBoxW) / 2;
}

static void coleco_display_draw_runtime_menu_shell(void)
{
    const int boxX = coleco_display_runtime_menu_box_x();
    const int boxY = coleco_display_runtime_menu_box_y();
    const int innerX = boxX + kRuntimeMenuInnerPad;

    if (coleco_display_game_on_external()) {
        coleco_display_prepare_external_tft();
        auto& tft = coleco_display_external_tft();
        tft.fillRoundRect(boxX + kRuntimeMenuShadowOffset, boxY + kRuntimeMenuShadowOffset, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
        tft.fillRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, TFT_BLACK);
        tft.drawRoundRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
        tft.drawRoundRect(boxX + 2, boxY + 2, kRuntimeMenuBoxW - 4, kRuntimeMenuBoxH - 4, DEFAULT_ROUND_RECT, RECT_COLOR_DARK);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
        tft.drawString("MSX MENU", innerX, boxY + 6, 2);
        tft.setTextColor(TEXT_COLOR, TFT_BLACK);
        tft.drawString(coleco_display_game_on_external() ? "EXT TFT fixed 1:1" : "\\ quick view  GO toggle",
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

static void coleco_display_draw_runtime_menu_row_state(const ColecoInputOverlayState& overlay, uint8_t index, bool selected)
{
    const uint8_t scrollIndex = coleco_input_get_scroll_index();
    const int displayIndex = static_cast<int>(index) - static_cast<int>(scrollIndex);
    if (displayIndex < 0 || displayIndex >= kRuntimeMenuRowCount) {
        return;
    }

    const int boxX = coleco_display_runtime_menu_box_x();
    const int innerX = boxX + kRuntimeMenuInnerPad;
    const int valueX = boxX + kRuntimeMenuBoxW - 44;
    const int y = coleco_display_runtime_menu_first_row_y() + displayIndex * kRuntimeMenuRowH;
    const int rowX = innerX - 4;
    const int rowY = y - 2;
    const int rowW = kRuntimeMenuBoxW - 16;
    const int rowH = kRuntimeMenuRowH - 1;
    const char* label = coleco_display_runtime_menu_label(index);
    const char* value = coleco_display_runtime_menu_value(overlay, index);

    if (coleco_display_game_on_external()) {
        coleco_display_prepare_external_tft();
        auto& tft = coleco_display_external_tft();
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

static void coleco_display_draw_runtime_menu_row(const ColecoInputOverlayState& overlay, uint8_t index)
{
    coleco_display_draw_runtime_menu_row_state(overlay, index, overlay.selectedIndex == index);
}

static void coleco_display_draw_runtime_menu(const ColecoInputOverlayState& overlay)
{
    if (!overlay.menuVisible) {
        return;
    }

    coleco_display_draw_runtime_menu_shell();
    for (uint8_t i = 0; i < kRuntimeMenuRowCount; ++i) {
        coleco_display_draw_runtime_menu_row(overlay, coleco_input_get_scroll_index() + i);
    }
}

void coleco_display_init(void)
{
    s_lastPlaceholderMs = 0;
    s_lastDisplayDiagMs = 0;
    s_lastViewMode = coleco_config_get_active_view_mode();
    s_lastStateSlot = coleco_input_get_state_slot();
    s_lastMenuOverlay = {};
    s_lastMenuVisible = false;
    coleco_video_init();
}

void coleco_display_shutdown(void)
{
    coleco_video_shutdown();
}

void coleco_display_submit_frame(const ColecoDisplayFrame* frame, const ColecoDisplayStatus* status)
{
    const uint32_t nowMs = millis();
    ColecoInputOverlayState overlay = {};
    coleco_input_get_overlay_state(&overlay);

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

    const ColecoInternalViewMode currentViewMode = coleco_config_get_active_view_mode();
    const uint8_t currentStateSlot = coleco_input_get_state_slot();
    if (overlay.menuVisible) {
        coleco_video_lock();
        coleco_video_set_runtime_menu_active(true);
        const uint8_t scrollIndex = coleco_input_get_scroll_index();
        
        if (!s_lastMenuVisible || scrollIndex != s_lastScrollIndex) {
            coleco_display_draw_runtime_menu(overlay);
        } else {
            if (overlay.selectedIndex != s_lastMenuOverlay.selectedIndex) {
                coleco_display_draw_runtime_menu_row_state(overlay, s_lastMenuOverlay.selectedIndex, false);
                coleco_display_draw_runtime_menu_row(overlay, overlay.selectedIndex);
            }

            if (overlay.joystickEnabled != s_lastMenuOverlay.joystickEnabled) {
                coleco_display_draw_runtime_menu_row(overlay, 0u);
            }
            if (overlay.keyboardEnabled != s_lastMenuOverlay.keyboardEnabled) {
                coleco_display_draw_runtime_menu_row(overlay, 1u);
            }
            if (overlay.vausEnabled != s_lastMenuOverlay.vausEnabled) {
                coleco_display_draw_runtime_menu_row(overlay, 2u);
            }
            if (currentViewMode != s_lastViewMode) {
                coleco_display_draw_runtime_menu_row(overlay, 3u);
            }
            if (currentStateSlot != s_lastStateSlot && !coleco_display_game_on_external()) {
                coleco_display_draw_runtime_menu_row(overlay, 4u);
            }
        }

        s_lastViewMode = currentViewMode;
        s_lastStateSlot = currentStateSlot;
        s_lastMenuOverlay = overlay;
        s_lastMenuVisible = true;
        s_lastScrollIndex = scrollIndex;
        coleco_video_unlock();
        return;
    }

    if (s_lastMenuVisible) {
        coleco_video_lock();
        if (coleco_display_game_on_external()) {
            coleco_video_finish_external_ui();
        }
        coleco_video_set_runtime_menu_active(false);
        s_lastPlaceholderMs = 0;
        coleco_video_unlock();

        coleco_video_request_full_redraw();
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
    
    coleco_video_lock();
    coleco_display_draw_placeholder(status);
    coleco_video_unlock();
}

// ================== EXTERNAL INFO SCREEN ==================

static std::string coleco_truncate(const char* text, size_t maxChars)
{
    if (!text) return "";
    std::string v(text);
    if (v.size() <= maxChars) return v;
    if (maxChars <= 3) return v.substr(0, maxChars);
    return v.substr(0, maxChars - 3) + "...";
}

static void coleco_draw_key_badge(TFT_eSPI& tft, int x, int y, const std::string& key)
{
    const int bw = 34, bh = 18;
    tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawCentreString(key.c_str(), x + bw / 2, y + 1, 2);
    tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void coleco_display_show_external_info(const char* romTitle)
{
    coleco_display_prepare_external_tft();
    auto& tft = coleco_display_external_tft();
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextWrap(false);

    tft.drawRoundRect(8, 8, kExternalDisplayW - 16, kExternalDisplayH - 16, 8, TFT_DARKGREY);

    std::string title = romTitle ? romTitle : "";
    const char* drawTitle = title.empty() ? "COLECOVISION" : title.c_str();
    int titleFont = 4;
    int maxTitleW = kExternalDisplayW - 32;
    if (tft.textWidth(drawTitle, 4) > maxTitleW) {
        titleFont = 2;
        if (tft.textWidth(drawTitle, 2) > maxTitleW) {
            title = coleco_truncate(romTitle, 36);
            drawTitle = title.c_str();
        }
    }
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString(drawTitle, kExternalDisplayW / 2, 16, titleFont);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("EMULATED SYSTEMS", kExternalDisplayW / 2, 36, 1);

    // Box MSX
    tft.drawRoundRect(kExternalDisplayW / 2 - 130, 46, 120, 18, 4, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("MSX: .rom .dsk", kExternalDisplayW / 2 - 70, 50, 1);

    // Box ColecoVision
    tft.drawRoundRect(kExternalDisplayW / 2 + 10, 46, 120, 18, 4, TFT_DARKGREY);
    tft.drawCentreString("ColecoVision: .col", kExternalDisplayW / 2 + 70, 50, 1);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("VIDEO ON INTERNAL LCD", kExternalDisplayW / 2, 70, 2);

    tft.drawRoundRect(12, 86, kExternalDisplayW - 24, 98, 6, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("CONTROLS", kExternalDisplayW / 2, 92, 2);

    const auto actions = share::emuControlActionLabels(share::EmuProfile::MSX);
    const auto keys    = share::emuControlKeyLabels(share::EmuProfile::MSX);
    const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
    const size_t rowsPerCol = 4;

    for (size_t i = 0; i < count; ++i) {
        const int col = (int)(i / rowsPerCol);
        const int row = (int)(i % rowsPerCol);
        const int baseX = 24 + col * 146;
        const int baseY = 112 + row * 16;

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(actions[i].c_str(), baseX, baseY, 2);
        coleco_draw_key_badge(tft, baseX + 88, baseY - 3, keys[i]);
    }

    tft.drawFastHLine(18, 190, kExternalDisplayW - 36, TFT_DARKGREY);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("GO / HOLD ESC = QUIT", kExternalDisplayW / 2, 198, 1);
    tft.drawCentreString("\\ = SCREEN  FN+,/ = ZOOM", kExternalDisplayW / 2, 210, 1);
}

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(actions[i].c_str(), baseX, baseY, 2);
        coleco_draw_key_badge(tft, baseX + 88, baseY - 3, keys[i]);
    }

    tft.drawFastHLine(18, 190, kExternalDisplayW - 36, TFT_DARKGREY);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("GO / HOLD ESC = QUIT", kExternalDisplayW / 2, 198, 1);
    tft.drawCentreString("\\ = SCREEN  FN+,/ = ZOOM", kExternalDisplayW / 2, 210, 1);
}
