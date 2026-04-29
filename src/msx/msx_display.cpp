#include "msx_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>

#include "../cardputer/CardputerView.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../tft_setup.h"

#include "msx_config.h"
#include "msx_external_title_images.h"
#include "msx_input.h"
#include "msx_video.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <cmath>

extern uint8_t msx_input_get_state_slot(void);
extern uint8_t msx_input_get_scroll_index(void);

static constexpr uint32_t kPlaceholderRedrawMs = 150;
static constexpr uint32_t kDisplayDiagIntervalMs = 2000;
static constexpr int kExternalDisplayW = 320;
static constexpr int kExternalDisplayH = 240;
static constexpr int kExternalTitleTextY = MSX_EXTERNAL_TITLE_HEIGHT;
static constexpr int kExternalTitleTextH = kExternalDisplayH - MSX_EXTERNAL_TITLE_HEIGHT;
static uint32_t s_lastPlaceholderMs = 0;
static uint32_t s_lastDisplayDiagMs = 0;
static MsxInternalViewMode s_lastViewMode = MsxInternalViewMode::Wide;
static MsxInputOverlayState s_lastMenuOverlay = {};
static bool s_lastMenuVisible = false;
static bool s_lastVirtualKeyPickerVisible = false;
static char s_lastVirtualKeyPickerLabel[8] = "";
static uint8_t s_lastScrollIndex = 0;
static uint8_t s_lastStateSlot = 0;
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

static const char* msx_display_active_view_label(void)
{
    return msx_config_get_active_view_mode_label_for_target(msx_display_game_on_external());
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
        tft.drawRect(boxX, boxY, boxW, boxH, PRIMARY_COLOR);
        tft.drawRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, RECT_COLOR_DARK);
    } else {
        auto& display = M5Cardputer.Display;
        display.fillScreen(TFT_BLACK);
        display.drawRect(boxX, boxY, boxW, boxH, PRIMARY_COLOR);
        display.drawRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, RECT_COLOR_DARK);
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
                  msx_display_active_view_label(),
                  static_cast<unsigned long>(status ? status->frameCounter : 0));
    msx_display_draw_line(footer, 128, PRIMARY_COLOR, 1);
}

static const char* msx_display_runtime_menu_label(const MsxInputOverlayState& overlay, uint8_t index)
{
    if (overlay.performanceSubmenuVisible) {
        switch (index) {
            case 0u: return "EXT 30FPS";
            case 1u: return "FRAMESKP";
            case 2u: return "FPS HUD";
            case 3u: return "SLICE RENDER";
            case 4u: return "SPR COLL";
            case 5u: return "8-SPR FLAGS";
            case 6u: return "INSTANT CMD";
            case 7u: return "BACK";
            default: return "";
        }
    }

    if (overlay.casSubmenuVisible) {
        switch (index) {
            case 0u: return "RUN CAS";
            case 1u: return "BLOAD CAS";
            case 2u: return "CHANGE CAS";
            case 3u: return "BACK";
            default: return "";
        }
    }

    if (overlay.soundSubmenuVisible) {
        switch (index) {
            case 0u: return "VIRTUAL SCC";
            case 1u: return "MASTER VOL";
            case 2u: return "SCC VOL";
            case 3u: return "BACK";
            default: return "";
        }
    }

    if (msx_display_game_on_external()) {
        switch (index) {
            case 0u: return "PERF TUNE";
            case 1u: return "SOUND";
            case 2u: return "JOY EXTEND";
            case 3u: return "KEYB/JOY";
            case 4u: return "BASIC KBD";
            case 5u: return "VAUS";
            case 6u: return "VIEW";
            case 7u: return "SELECT SLOT";
            case 8u: return "SAVE SLOT";
            case 9u: return "LOAD SLOT";
            case 10u: return overlay.casChangeAvailable ? "CAS MENU" : "CLOSE";
            case 11u: return overlay.casChangeAvailable ? "CLOSE" : "";
            default: return "";
        }
    } else {
        switch (index) {
            case 0u: return "PERF TUNE";
            case 1u: return "SOUND";
            case 2u: return "JOY EXTEND";
            case 3u: return "KEYB/JOY";
            case 4u: return "BASIC KBD";
            case 5u: return "VAUS";
            case 6u: return "VIEW";
            case 7u: return "STATE SLOT";
            case 8u: return "SAVE STATE";
            case 9u: return "LOAD STATE";
            case 10u: return overlay.casChangeAvailable ? "CAS MENU" : "CLOSE";
            case 11u: return overlay.casChangeAvailable ? "CLOSE" : "";
            default: return "";
        }
    }
}

static const char* msx_display_runtime_menu_value(const MsxInputOverlayState& overlay,
                                                  uint8_t index,
                                                  uint8_t stateSlot)
{
    static char slotStr[8];
    if (overlay.performanceSubmenuVisible) {
        switch (index) {
            case 0u:
                return overlay.perfExternalFixed30Fps ? "ON" : "OFF";
            case 1u:
                return msx_config_frameskip_mode_label(overlay.perfFrameskipMode);
            case 2u:
                return overlay.perfShowFpsOverlay ? "ON" : "OFF";
            case 3u:
                if (!overlay.machineIsMsx2) {
                    return "N/A";
                }
                return overlay.perfDisableSliceRendering ? "OFF" : "ON";
            case 4u:
                return overlay.perfDisableSpriteCollision ? "OFF" : "ON";
            case 5u:
                return overlay.perfSimplifySpriteOverflow ? "OFF" : "ON";
            case 6u:
                if (!overlay.machineIsMsx2) {
                    return "N/A";
                }
                return overlay.perfInstantVdpCommands ? "ON" : "OFF";
            default:
                return nullptr;
        }
    }

    if (overlay.casSubmenuVisible) {
        return nullptr;
    }

    if (overlay.soundSubmenuVisible) {
        switch (index) {
            case 0u:
                return msx_config_virtual_scc_mode_label(overlay.virtualSccMode);
            case 1u:
                return msx_config_sound_volume_label(overlay.soundVolume);
            case 2u:
                return msx_config_scc_gain_label(overlay.sccGainPercent);
            default:
                return nullptr;
        }
    }

    switch (index) {
        case 0u:
            return msx_config_performance_preset_label(overlay.performancePreset);
        case 1u:
            return nullptr;
        case 2u:
            return overlay.joystickEnabled ? "ON" : "OFF";
        case 3u:
            return overlay.keyboardEnabled ? "ON" : "OFF";
        case 4u:
            return overlay.basicKeyboardEnabled ? "ON" : "OFF";
        case 5u:
            return overlay.vausEnabled ? "ON" : "OFF";
        case 6u:
            return msx_display_active_view_label();
        case 7u:
            std::snprintf(slotStr, sizeof(slotStr), "< %u >", static_cast<unsigned>(stateSlot));
            return slotStr;
        default:
            return nullptr;
    }
}

static int msx_display_runtime_menu_box_x(void)
{
    return (msx_display_active_width() - kRuntimeMenuBoxW) / 2;
}

static void msx_display_draw_runtime_menu_shell(const MsxInputOverlayState& overlay)
{
    const int boxX = msx_display_runtime_menu_box_x();
    const int boxY = msx_display_runtime_menu_box_y();
    const int innerX = boxX + kRuntimeMenuInnerPad;
    const char* title = overlay.performanceSubmenuVisible
                            ? "PERF TUNING"
                            : (overlay.soundSubmenuVisible
                                   ? "SOUND MENU"
                                   : (overlay.casSubmenuVisible ? "CAS MENU" : "CONFIG MENU"));
    const char* hint1 = overlay.performanceSubmenuVisible
                            ? "GO toggle  DEL back"
                            : (overlay.soundSubmenuVisible
                                   ? "GO toggle  DEL back"
                                   : (overlay.casSubmenuVisible
                                   ? "GO select  DEL back"
                                      : "\\ switch view  GO toggle"));
    const char* hint2 = "Hold GO closes menu";

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        tft.fillRect(boxX + kRuntimeMenuShadowOffset, boxY + kRuntimeMenuShadowOffset, kRuntimeMenuBoxW, kRuntimeMenuBoxH, TFT_BLACK);
        tft.fillRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, TFT_BLACK);
        tft.drawRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, PRIMARY_COLOR);
        tft.drawRect(boxX + 2, boxY + 2, kRuntimeMenuBoxW - 4, kRuntimeMenuBoxH - 4, RECT_COLOR_DARK);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
        const int titleX = boxX + (kRuntimeMenuBoxW - tft.textWidth(title, 2)) / 2;
        tft.drawString(title, titleX, boxY + 4, 2);
        tft.setTextColor(TEXT_COLOR, TFT_BLACK);
        tft.drawString(hint1, innerX, boxY + kRuntimeMenuBoxH - 19, 1);
        tft.drawString(hint2, innerX, boxY + kRuntimeMenuBoxH - 10, 1);
        return;
    }

    auto& display = M5Cardputer.Display;
    display.fillRect(boxX + kRuntimeMenuShadowOffset, boxY + kRuntimeMenuShadowOffset, kRuntimeMenuBoxW, kRuntimeMenuBoxH, TFT_BLACK);
    display.fillRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, TFT_BLACK);
    display.drawRect(boxX, boxY, kRuntimeMenuBoxW, kRuntimeMenuBoxH, PRIMARY_COLOR);
    display.drawRect(boxX + 2, boxY + 2, kRuntimeMenuBoxW - 4, kRuntimeMenuBoxH - 4, RECT_COLOR_DARK);
    display.setTextDatum(top_left);
    display.setFont(&fonts::Font2);
    display.setTextColor(PRIMARY_COLOR, TFT_BLACK);
    const int titleX = boxX + (kRuntimeMenuBoxW - display.textWidth(title)) / 2;
    display.drawString(title, titleX, boxY + 4);
    display.setFont(&fonts::Font0);
    display.setTextColor(TEXT_COLOR, TFT_BLACK);
    display.drawString(hint1, innerX, boxY + kRuntimeMenuBoxH - 19);
    display.drawString(hint2, innerX, boxY + kRuntimeMenuBoxH - 10);
}

static void msx_display_draw_runtime_menu_row_state(const MsxInputOverlayState& overlay, uint8_t index, bool selected)
{
    const uint8_t scrollIndex = msx_input_get_scroll_index();
    const uint8_t stateSlot = msx_input_get_state_slot();
    const int displayIndex = static_cast<int>(index) - static_cast<int>(scrollIndex);
    if (displayIndex < 0 || displayIndex >= kRuntimeMenuRowCount) {
        return;
    }

    const int boxX = msx_display_runtime_menu_box_x();
    const int innerX = boxX + kRuntimeMenuInnerPad;
    const int valueX = boxX + kRuntimeMenuBoxW - 44;
    const int y = msx_display_runtime_menu_first_row_y() + displayIndex * kRuntimeMenuRowH;
    const int rowX = innerX - 4;
    const int rowY = y - 2;
    const int rowW = kRuntimeMenuBoxW - 16;
    const int rowH = kRuntimeMenuRowH - 1;
    const int valueClearW = (boxX + kRuntimeMenuBoxW - 4) - valueX;
    const char* label = msx_display_runtime_menu_label(overlay, index);
    const char* value = msx_display_runtime_menu_value(overlay, index, stateSlot);

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        const uint16_t rowBg = selected ? RECT_COLOR_DARK : TFT_BLACK;
        const uint16_t rowFg = selected ? TFT_ORANGE : TFT_WHITE;
        tft.fillRect(rowX, rowY, rowW, rowH, rowBg);
        tft.setTextColor(rowFg, rowBg);
        tft.drawString(label, innerX, y, 1);
        if (value) {
            tft.fillRect(valueX, rowY, valueClearW, rowH, rowBg);
            tft.drawString(value, valueX, y, 1);
        }
        return;
    }

    auto& display = M5Cardputer.Display;
    display.fillRect(rowX, rowY, rowW, rowH, selected ? RECT_COLOR_DARK : TFT_BLACK);
    display.setFont(&fonts::Font0);
    display.setTextColor(selected ? PRIMARY_COLOR : TEXT_COLOR, selected ? RECT_COLOR_DARK : TFT_BLACK);
    display.drawString(label, innerX, y);
    if (value) {
        display.fillRect(valueX, rowY, valueClearW, rowH, selected ? RECT_COLOR_DARK : TFT_BLACK);
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

    msx_display_draw_runtime_menu_shell(overlay);
    for (uint8_t i = 0; i < kRuntimeMenuRowCount; ++i) {
        msx_display_draw_runtime_menu_row(overlay, msx_input_get_scroll_index() + i);
    }
}

static std::string msx_display_fit_external_info_title(TFT_eSPI& tft,
                                                       const char* text,
                                                       int font,
                                                       int maxWidth)
{
    std::string title = (text && text[0] != '\0') ? text : "MSX";
    if (tft.textWidth(title.c_str(), font) <= maxWidth) {
        return title;
    }

    while (!title.empty()) {
        title.pop_back();
        const std::string candidate = title + "...";
        if (tft.textWidth(candidate.c_str(), font) <= maxWidth) {
            return candidate;
        }
    }

    return "...";
}

void msx_display_show_external_info(const char* romTitle, bool machineIsMsx2)
{
    if (msx_display_game_on_external()) {
        return;
    }

    msx_video_lock();
    msx_display_prepare_external_tft();
    auto& tft = msx_display_external_tft();
    const uint16_t* image = machineIsMsx2 ? msx_external_title_msx2 : msx_external_title_msx1;

    tft.setSwapBytes(true);
    tft.pushImage(0, 0, MSX_EXTERNAL_TITLE_WIDTH, MSX_EXTERNAL_TITLE_HEIGHT, image);
    tft.setSwapBytes(false);
    tft.fillRect(0, kExternalTitleTextY, kExternalDisplayW, kExternalTitleTextH, TFT_BLACK);

    const int maxWidth = kExternalDisplayW - 10;
    int font = 2;
    if (tft.textWidth(romTitle && romTitle[0] != '\0' ? romTitle : "MSX", font) > maxWidth) {
        font = 1;
    }
    const std::string fitted = msx_display_fit_external_info_title(tft, romTitle, font, maxWidth);
    const int y = (font == 2) ? (kExternalTitleTextY + 5) : (kExternalTitleTextY + 9);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(fitted.c_str(), kExternalDisplayW / 2, y, font);
    msx_video_unlock();
}

static void msx_display_draw_virtual_key_picker(const MsxInputOverlayState& overlay)
{
    const int displayW = msx_display_active_width();
    const int displayH = msx_display_active_height();
    const int boxW = msx_display_game_on_external() ? 116 : 96;
    const int boxH = msx_display_game_on_external() ? 48 : 42;
    const int boxX = (displayW - boxW) / 2;
    const int boxY = (displayH - boxH) / 2;

    char value[16];
    std::snprintf(value,
                  sizeof(value),
                  "< %s >",
                  overlay.virtualKeyPickerLabel[0] ? overlay.virtualKeyPickerLabel : "0");

    if (msx_display_game_on_external()) {
        msx_display_prepare_external_tft();
        auto& tft = msx_display_external_tft();
        tft.fillRoundRect(boxX + 3, boxY + 3, boxW, boxH, 4, TFT_BLACK);
        tft.fillRoundRect(boxX, boxY, boxW, boxH, 4, TFT_BLACK);
        tft.drawRoundRect(boxX, boxY, boxW, boxH, 4, PRIMARY_COLOR);
        tft.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, 3, RECT_COLOR_DARK);
        tft.setTextColor(PRIMARY_COLOR, TFT_BLACK);
        tft.drawCentreString("KEY", displayW / 2, boxY + 6, 2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawCentreString(value, displayW / 2, boxY + 25, 2);
        return;
    }

    auto& display = M5Cardputer.Display;
    display.fillRoundRect(boxX + 3, boxY + 3, boxW, boxH, 4, TFT_BLACK);
    display.fillRoundRect(boxX, boxY, boxW, boxH, 4, TFT_BLACK);
    display.drawRoundRect(boxX, boxY, boxW, boxH, 4, PRIMARY_COLOR);
    display.drawRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, 3, RECT_COLOR_DARK);
    display.setTextColor(PRIMARY_COLOR, TFT_BLACK);
    display.drawCenterString("KEY", displayW / 2, boxY + 6, &fonts::Font0);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawCenterString(value, displayW / 2, boxY + 23, &fonts::Font2);
}

static bool msx_display_runtime_menu_overlay_equals(const MsxInputOverlayState& a,
                                                    const MsxInputOverlayState& b)
{
    return a.menuVisible == b.menuVisible &&
           a.performanceSubmenuVisible == b.performanceSubmenuVisible &&
           a.soundSubmenuVisible == b.soundSubmenuVisible &&
           a.casSubmenuVisible == b.casSubmenuVisible &&
           a.machineIsMsx2 == b.machineIsMsx2 &&
           a.joystickEnabled == b.joystickEnabled &&
           a.keyboardEnabled == b.keyboardEnabled &&
           a.basicKeyboardEnabled == b.basicKeyboardEnabled &&
           a.vausEnabled == b.vausEnabled &&
           a.performanceMode == b.performanceMode &&
           a.performancePreset == b.performancePreset &&
           a.perfDisableSliceRendering == b.perfDisableSliceRendering &&
           a.perfDisableSpriteCollision == b.perfDisableSpriteCollision &&
           a.perfSimplifySpriteOverflow == b.perfSimplifySpriteOverflow &&
           a.perfInstantVdpCommands == b.perfInstantVdpCommands &&
           a.perfExternalFixed30Fps == b.perfExternalFixed30Fps &&
           a.perfFrameskipMode == b.perfFrameskipMode &&
           a.perfShowFpsOverlay == b.perfShowFpsOverlay &&
           a.virtualSccMode == b.virtualSccMode &&
           a.soundVolume == b.soundVolume &&
           a.sccGainPercent == b.sccGainPercent &&
           a.casChangeAvailable == b.casChangeAvailable &&
           a.selectedIndex == b.selectedIndex;
}

static bool msx_display_runtime_menu_row_changed(const MsxInputOverlayState& previousOverlay,
                                                 uint8_t previousStateSlot,
                                                 const MsxInputOverlayState& currentOverlay,
                                                 uint8_t currentStateSlot,
                                                 uint8_t index)
{
    const bool previousSelected = previousOverlay.selectedIndex == index;
    const bool currentSelected = currentOverlay.selectedIndex == index;
    if (previousSelected != currentSelected) {
        return true;
    }

    char previousLabelCopy[32];
    const char* previousLabel = msx_display_runtime_menu_label(previousOverlay, index);
    std::snprintf(previousLabelCopy,
                  sizeof(previousLabelCopy),
                  "%s",
                  previousLabel ? previousLabel : "");

    const char* currentLabel = msx_display_runtime_menu_label(currentOverlay, index);
    if (std::strcmp(previousLabelCopy, currentLabel ? currentLabel : "") != 0) {
        return true;
    }

    char previousValueCopy[32];
    const char* previousValue = msx_display_runtime_menu_value(previousOverlay, index, previousStateSlot);
    std::snprintf(previousValueCopy,
                  sizeof(previousValueCopy),
                  "%s",
                  previousValue ? previousValue : "");

    const char* currentValue = msx_display_runtime_menu_value(currentOverlay, index, currentStateSlot);
    if (previousValue == nullptr || currentValue == nullptr) {
        return previousValue != currentValue;
    }

    return std::strcmp(previousValueCopy, currentValue) != 0;
}

void msx_display_init(void)
{
    s_lastPlaceholderMs = 0;
    s_lastDisplayDiagMs = 0;
    s_lastViewMode = msx_config_get_active_view_mode();
    s_lastStateSlot = msx_input_get_state_slot();
    s_lastMenuOverlay = {};
    s_lastMenuVisible = false;
    s_lastVirtualKeyPickerVisible = false;
    s_lastVirtualKeyPickerLabel[0] = '\0';
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
    const bool streamFrame = frame &&
                            frame->indexed8 == nullptr &&
                            frame->width != 0u &&
                            frame->height != 0u &&
                            frame->pitchBytes == 0u;

    if (s_lastDisplayDiagMs == 0 || (uint32_t)(nowMs - s_lastDisplayDiagMs) >= kDisplayDiagIntervalMs) {
        s_lastDisplayDiagMs = nowMs;
    }

    const MsxInternalViewMode currentViewMode = msx_config_get_active_view_mode();
    const uint8_t currentStateSlot = msx_input_get_state_slot();
    if (overlay.menuVisible) {
        msx_video_lock();
        msx_video_set_runtime_menu_active(true);
        const uint8_t scrollIndex = msx_input_get_scroll_index();
        const bool fullMenuRedraw =
            !s_lastMenuVisible ||
            overlay.performanceSubmenuVisible != s_lastMenuOverlay.performanceSubmenuVisible ||
            overlay.soundSubmenuVisible != s_lastMenuOverlay.soundSubmenuVisible ||
            overlay.casSubmenuVisible != s_lastMenuOverlay.casSubmenuVisible;
        const bool overlayChanged =
            !msx_display_runtime_menu_overlay_equals(overlay, s_lastMenuOverlay);
        const bool scrollChanged = scrollIndex != s_lastScrollIndex;
        const bool stateSlotChanged = currentStateSlot != s_lastStateSlot;
        const bool viewChanged = currentViewMode != s_lastViewMode;

        if (fullMenuRedraw) {
            msx_display_draw_runtime_menu(overlay);
        } else if (overlayChanged || scrollChanged || stateSlotChanged || viewChanged) {
            if (scrollChanged || stateSlotChanged || viewChanged) {
                for (uint8_t i = 0; i < kRuntimeMenuRowCount; ++i) {
                    msx_display_draw_runtime_menu_row(overlay, scrollIndex + i);
                }
            } else {
                for (uint8_t i = 0; i < kRuntimeMenuRowCount; ++i) {
                    const uint8_t rowIndex = scrollIndex + i;
                    if (msx_display_runtime_menu_row_changed(
                            s_lastMenuOverlay,
                            s_lastStateSlot,
                            overlay,
                            currentStateSlot,
                            rowIndex)) {
                        msx_display_draw_runtime_menu_row(overlay, rowIndex);
                    }
                }
            }
        }

        s_lastViewMode = currentViewMode;
        s_lastStateSlot = currentStateSlot;
        s_lastMenuOverlay = overlay;
        s_lastMenuVisible = true;
        s_lastScrollIndex = scrollIndex;
        s_lastVirtualKeyPickerVisible = false;
        s_lastVirtualKeyPickerLabel[0] = '\0';
        msx_video_unlock();
        return;
    }

    const bool menuClosed = s_lastMenuVisible;
    if (menuClosed) {
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

    const bool pickerClosed = s_lastVirtualKeyPickerVisible && !overlay.virtualKeyPickerVisible;
    if (pickerClosed) {
        msx_video_lock();
        if (msx_display_game_on_external()) {
            msx_video_finish_external_ui();
        }
        s_lastVirtualKeyPickerVisible = false;
        s_lastVirtualKeyPickerLabel[0] = '\0';
        msx_video_unlock();
        msx_video_request_full_redraw();
    }

    if (frame && (frame->indexed8 || streamFrame)) {
        const bool pickerVisible = overlay.virtualKeyPickerVisible;
        const bool pickerChanged =
            pickerVisible &&
            (!s_lastVirtualKeyPickerVisible ||
             std::strcmp(s_lastVirtualKeyPickerLabel, overlay.virtualKeyPickerLabel) != 0);
        if (pickerVisible) {
            msx_video_lock();
            if (pickerChanged) {
                msx_display_draw_virtual_key_picker(overlay);
                std::snprintf(s_lastVirtualKeyPickerLabel,
                              sizeof(s_lastVirtualKeyPickerLabel),
                              "%s",
                              overlay.virtualKeyPickerLabel);
            }
            s_lastVirtualKeyPickerVisible = true;
            msx_video_unlock();
            return;
        }

        if (menuClosed || pickerClosed) {
            if (frame->indexed8) {
                msx_video_present_frame(frame);
            }
        }
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
    if (overlay.virtualKeyPickerVisible) {
        msx_display_draw_virtual_key_picker(overlay);
        std::snprintf(s_lastVirtualKeyPickerLabel,
                      sizeof(s_lastVirtualKeyPickerLabel),
                      "%s",
                      overlay.virtualKeyPickerLabel);
    }
    msx_video_unlock();
    s_lastVirtualKeyPickerVisible = overlay.virtualKeyPickerVisible;
}
