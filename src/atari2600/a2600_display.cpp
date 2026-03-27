#include "a2600_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

#include <string>

#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../tft_setup.h"
#include "esp_heap_caps.h"

static TFT_eSPI s_tft;

static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;

static bool s_use_ext = false;
static bool s_use_12bit = false;
static A2600InternalViewMode s_internalViewMode = A2600InternalViewMode::Wide;

bool a2600FullScreen = false;
int a2600ZoomPercent = 100;

static uint16_t* s_lineBuf = nullptr;
static int s_lineCap = 0;
static int16_t* s_xmap = nullptr;
static int s_xmapCap = 0;
static int16_t* s_ymap = nullptr;
static int s_ymapCap = 0;

static int s_lastSrcW = -1;
static int s_lastSrcH = -1;
static int s_lastDstW = -1;
static int s_lastDstH = -1;
static int s_lastXOff = -1;
static int s_lastYOff = -1;
static int s_lastSrcX0 = -1;
static int s_lastSrcY0 = -1;
static int s_lastRoiW = -1;
static int s_lastRoiH = -1;
static bool s_lastFull = false;
static int s_lastZoom = -1;
static int s_lastViewMode = -1;

static constexpr const char* kA2600DisplayNs = "a2600_disp";
static constexpr const char* kA2600ViewKey = "int_view";
static constexpr int kA2600NtscVisibleHeight = 192;
static constexpr int kA2600PalVisibleHeight = 228;

struct A2600RenderPlan {
    int srcX0;
    int srcY0;
    int roiW;
    int roiH;
    int dstW;
    int dstH;
    int xOff;
    int yOff;
};

static int clampi(int value, int minValue, int maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static A2600InternalViewMode a2600_sanitize_internal_view_mode(uint8_t value)
{
    return value == static_cast<uint8_t>(A2600InternalViewMode::PixelPerfect)
               ? A2600InternalViewMode::PixelPerfect
               : A2600InternalViewMode::Wide;
}

const char* a2600_display_internal_view_mode_label(A2600InternalViewMode mode)
{
    return mode == A2600InternalViewMode::PixelPerfect ? "PIXEL" : "WIDE";
}

A2600InternalViewMode a2600_display_load_internal_view_mode(void)
{
    Preferences prefs;
    prefs.begin(kA2600DisplayNs, true);
    const uint8_t saved = prefs.getUChar(
        kA2600ViewKey,
        static_cast<uint8_t>(A2600InternalViewMode::Wide)
    );
    prefs.end();

    s_internalViewMode = a2600_sanitize_internal_view_mode(saved);
    return s_internalViewMode;
}

static void a2600_save_internal_view_mode(void)
{
    Preferences prefs;
    prefs.begin(kA2600DisplayNs, false);
    prefs.putUChar(kA2600ViewKey, static_cast<uint8_t>(s_internalViewMode));
    prefs.end();
}

static void a2600_clear_target(void)
{
    if (s_use_ext) {
        s_tft.fillScreen(TFT_BLACK);
    } else {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
}

static void a2600_reset_layout_cache(void)
{
    s_lastSrcW = -1;
    s_lastSrcH = -1;
    s_lastDstW = -1;
    s_lastDstH = -1;
    s_lastXOff = -1;
    s_lastYOff = -1;
    s_lastSrcX0 = -1;
    s_lastSrcY0 = -1;
    s_lastRoiW = -1;
    s_lastRoiH = -1;
    s_lastFull = false;
    s_lastZoom = -1;
    s_lastViewMode = -1;
}

static bool a2600_layout_changed(const A2600RenderPlan& plan, int srcW, int srcH)
{
    const bool changed =
        s_lastSrcW != srcW ||
        s_lastSrcH != srcH ||
        s_lastDstW != plan.dstW ||
        s_lastDstH != plan.dstH ||
        s_lastXOff != plan.xOff ||
        s_lastYOff != plan.yOff ||
        s_lastSrcX0 != plan.srcX0 ||
        s_lastSrcY0 != plan.srcY0 ||
        s_lastRoiW != plan.roiW ||
        s_lastRoiH != plan.roiH ||
        s_lastFull != a2600FullScreen ||
        s_lastZoom != a2600ZoomPercent ||
        s_lastViewMode != static_cast<int>(s_internalViewMode);

    s_lastSrcW = srcW;
    s_lastSrcH = srcH;
    s_lastDstW = plan.dstW;
    s_lastDstH = plan.dstH;
    s_lastXOff = plan.xOff;
    s_lastYOff = plan.yOff;
    s_lastSrcX0 = plan.srcX0;
    s_lastSrcY0 = plan.srcY0;
    s_lastRoiW = plan.roiW;
    s_lastRoiH = plan.roiH;
    s_lastFull = a2600FullScreen;
    s_lastZoom = a2600ZoomPercent;
    s_lastViewMode = static_cast<int>(s_internalViewMode);

    return changed;
}

static void a2600_compute_internal_visible_roi(int srcW, int srcH, bool isPal, A2600RenderPlan& plan)
{
    const int preferredVisibleH = isPal ? kA2600PalVisibleHeight : kA2600NtscVisibleHeight;

    plan.srcX0 = 0;
    plan.roiW = srcW;
    plan.roiH = clampi(preferredVisibleH, 64, srcH);
    plan.srcY0 = (srcH - plan.roiH) / 2;
}

static void a2600_compute_plan(int srcW, int srcH, bool isPal, A2600RenderPlan& plan)
{
    const int targetW = s_use_ext ? EXT_W : M5Cardputer.Display.width();
    const int targetH = s_use_ext ? EXT_H : M5Cardputer.Display.height();
    const int zoom = clampi(a2600ZoomPercent, 100, 150);

    if (s_use_ext) {
        if (a2600FullScreen) {
            plan.roiW = clampi((srcW * 100) / zoom, 64, srcW);
            plan.roiH = clampi((srcH * 100) / zoom, 64, srcH);
            plan.srcX0 = (srcW - plan.roiW) / 2;
            plan.srcY0 = (srcH - plan.roiH) / 2;
            plan.dstW = targetW;
            plan.dstH = targetH;
            plan.xOff = 0;
            plan.yOff = 0;
        } else {
            plan.srcX0 = 0;
            plan.srcY0 = 0;
            plan.roiW = srcW;
            plan.roiH = srcH;

            if (isPal && plan.roiH > targetH) {
                plan.srcY0 = (plan.roiH - targetH) / 2;
                plan.roiH = targetH;
            }

            plan.dstW = targetW;
            plan.dstH = (plan.roiH > targetH) ? targetH : plan.roiH;
            plan.xOff = 0;
            plan.yOff = (targetH - plan.dstH) / 2;
        }
        return;
    }

    if (a2600FullScreen) {
        plan.roiW = clampi((srcW * 100) / zoom, 64, srcW);
        plan.roiH = clampi((srcH * 100) / zoom, 64, srcH);
        plan.srcX0 = (srcW - plan.roiW) / 2;
        plan.srcY0 = (srcH - plan.roiH) / 2;
        plan.dstW = targetW;
        plan.dstH = targetH;
        plan.xOff = 0;
        plan.yOff = 0;
    } else {
        a2600_compute_internal_visible_roi(srcW, srcH, isPal, plan);

        if (s_internalViewMode == A2600InternalViewMode::Wide) {
            plan.dstW = clampi((targetH * 4) / 3, 1, targetW);
            plan.dstH = targetH;
            plan.xOff = (targetW - plan.dstW) / 2;
            plan.yOff = 0;
        } else {
            const float scaleX = (float)targetW / (float)plan.roiW;
            const float scaleY = (float)targetH / (float)plan.roiH;
            float scale = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale <= 0.0f) {
                scale = 1.0f;
            }

            plan.dstW = clampi((int)(plan.roiW * scale), 1, targetW);
            plan.dstH = clampi((int)(plan.roiH * scale), 1, targetH);
            plan.xOff = (targetW - plan.dstW) / 2;
            plan.yOff = (targetH - plan.dstH) / 2;
        }
    }
}

static bool a2600_prepare_luts(const A2600RenderPlan& plan)
{
    if (plan.dstW > s_xmapCap) {
        free(s_xmap);
        s_xmap = (int16_t*)malloc((size_t)plan.dstW * sizeof(int16_t));
        s_xmapCap = s_xmap ? plan.dstW : 0;
    }

    if (plan.dstH > s_ymapCap) {
        free(s_ymap);
        s_ymap = (int16_t*)malloc((size_t)plan.dstH * sizeof(int16_t));
        s_ymapCap = s_ymap ? plan.dstH : 0;
    }

    if (plan.dstW > s_lineCap) {
        free(s_lineBuf);
        s_lineBuf = (uint16_t*)heap_caps_malloc(
            (size_t)plan.dstW * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        );
        if (!s_lineBuf) {
            s_lineBuf = (uint16_t*)malloc((size_t)plan.dstW * sizeof(uint16_t));
        }
        s_lineCap = s_lineBuf ? plan.dstW : 0;
    }

    if (!s_xmap || !s_ymap || !s_lineBuf) {
        return false;
    }

    for (int x = 0; x < plan.dstW; ++x) {
        s_xmap[x] = (int16_t)(plan.srcX0 + ((int64_t)x * plan.roiW) / plan.dstW);
    }

    for (int y = 0; y < plan.dstH; ++y) {
        if (plan.dstH == plan.roiH && !a2600FullScreen && s_use_ext) {
            s_ymap[y] = (int16_t)(plan.srcY0 + y);
        } else {
            s_ymap[y] = (int16_t)(plan.srcY0 + ((int64_t)y * plan.roiH) / plan.dstH);
        }
    }

    return true;
}

static void a2600_draw_line_12bit(const uint8_t* srcLine,
                                  const uint16_t* palette565,
                                  int dstW)
{
    uint8_t* buf12 = (uint8_t*)s_lineBuf;
    const int pairs = dstW / 2;

    for (int pair = 0; pair < pairs; ++pair) {
        const uint16_t color1 = palette565[srcLine[s_xmap[pair * 2 + 0]]];
        const uint16_t color2 = palette565[srcLine[s_xmap[pair * 2 + 1]]];
        const int offset = pair * 3;
        buf12[offset + 0] = ((color1 >> 8) & 0xF0) | ((color1 >> 7) & 0x0F);
        buf12[offset + 1] = ((color1 << 3) & 0xF0) | ((color2 >> 12) & 0x0F);
        buf12[offset + 2] = ((color2 >> 3) & 0xF0) | ((color2 >> 1) & 0x0F);
    }

    if (dstW & 1) {
        const uint16_t color = palette565[srcLine[s_xmap[dstW - 1]]];
        const int offset = pairs * 3;
        buf12[offset + 0] = ((color >> 8) & 0xF0) | ((color >> 7) & 0x0F);
        buf12[offset + 1] = ((color << 3) & 0xF0);
        buf12[offset + 2] = 0;
    }

    const int byteCount = ((dstW + 1) / 2) * 3;
    s_tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
}

static void a2600_draw_line_16bit(const uint8_t* srcLine,
                                  const uint16_t* palette565,
                                  int dstW)
{
    for (int x = 0; x < dstW; ++x) {
        s_lineBuf[x] = palette565[srcLine[s_xmap[x]]];
    }

    if (s_use_ext) {
        s_tft.pushColors(s_lineBuf, dstW, true);
    } else {
        M5Cardputer.Display.pushPixels(s_lineBuf, dstW);
    }
}

void a2600_display_show_external_info(const char* romTitle)
{
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    s_tft.setTextWrap(false);

    s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

    std::string title = romTitle ? romTitle : "";
    const char* drawTitle = title.empty() ? "ATARI 2600" : title.c_str();
    int titleFont = 4;
    const int maxTitleW = EXT_W - 32;
    if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
        titleFont = 2;
        if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
            if (title.size() > 36) {
                title = title.substr(0, 33) + "...";
            }
            drawTitle = title.c_str();
        }
    }

    s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
    s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

    s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_tft.drawCentreString("ATARI 2600 / VCS", EXT_W / 2, 46, 2);

    s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
    s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

    s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

    const auto actions = share::emuControlActionLabels(share::EmuProfile::A2600);
    const auto keys = share::emuControlKeyLabels(share::EmuProfile::A2600);
    const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
    const size_t rowsPerCol = 4;

    for (size_t i = 0; i < count; ++i) {
        const int col = (int)(i / rowsPerCol);
        const int row = (int)(i % rowsPerCol);
        const int baseX = 24 + col * 146;
        const int baseY = 112 + row * 16;

        s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
        s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);

        const int badgeW = 34;
        const int badgeH = 18;
        s_tft.fillRoundRect(baseX + 88, baseY - 3, badgeW, badgeH, 4, TFT_DARKGREY);
        s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
        s_tft.drawCentreString(keys[i].c_str(), baseX + 88 + badgeW / 2, baseY - 2, 2);
        s_tft.drawRoundRect(baseX + 88, baseY - 3, badgeW, badgeH, 4, TFT_YELLOW);
    }

    s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
    s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    s_tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_W / 2, 198, 1);
    s_tft.drawCentreString("\\ = SCREEN  FN+\\ = VIEW", EXT_W / 2, 210, 1);
    s_tft.drawCentreString("FN+,/ = ZOOM", EXT_W / 2, 222, 1);
}

void a2600_display_init(void)
{
    s_use_ext = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    s_use_12bit = s_use_ext && (g_emu_color_depth == EMU_COLOR_12BIT);
    a2600_display_load_internal_view_mode();

    if (s_use_ext) {
        s_tft.begin();
        s_tft.setRotation(3);
        s_tft.fillScreen(TFT_BLACK);
        if (s_use_12bit) {
            s_tft.startWrite();
            s_tft.writecommand(0x3A);
            s_tft.writedata(0x53);
            s_tft.endWrite();
        }
    } else {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
    }

    a2600_reset_layout_cache();

    if (!s_use_ext) {
        printf("[A2600][DISP] internal view=%s\n", a2600_display_internal_view_mode_label(s_internalViewMode));
    }
}

void a2600_display_start(void)
{
}

void a2600_display_stop(void)
{
    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;

    free(s_xmap);
    s_xmap = nullptr;
    s_xmapCap = 0;

    free(s_ymap);
    s_ymap = nullptr;
    s_ymapCap = 0;

    a2600_reset_layout_cache();
}

A2600InternalViewMode a2600_display_get_internal_view_mode(void)
{
    return s_internalViewMode;
}

void a2600_display_set_internal_view_mode(A2600InternalViewMode mode, bool persist)
{
    s_internalViewMode = a2600_sanitize_internal_view_mode(static_cast<uint8_t>(mode));
    if (persist) {
        a2600_save_internal_view_mode();
    }
    a2600_reset_layout_cache();
}

const char* a2600_display_get_internal_view_mode_label(void)
{
    return a2600_display_internal_view_mode_label(s_internalViewMode);
}

void a2600_display_toggle_internal_view_mode(void)
{
    const A2600InternalViewMode nextMode =
        (s_internalViewMode == A2600InternalViewMode::PixelPerfect)
            ? A2600InternalViewMode::Wide
            : A2600InternalViewMode::PixelPerfect;
    a2600_display_set_internal_view_mode(nextMode, true);
    printf("[A2600][DISP] internal view=%s\n", a2600_display_internal_view_mode_label(s_internalViewMode));
}

void a2600_display_submit_frame(const uint8_t* indexedFrame,
                                int width,
                                int height,
                                const uint16_t* palette565,
                                bool isPal)
{
    if (!indexedFrame || !palette565 || width <= 0 || height <= 0) {
        return;
    }

    A2600RenderPlan plan = {};
    a2600_compute_plan(width, height, isPal, plan);

    if (!a2600_prepare_luts(plan)) {
        printf("[A2600][DISP] buffer allocation failed\n");
        return;
    }

    if (a2600_layout_changed(plan, width, height)) {
        a2600_clear_target();
    }

    if (s_use_ext) {
        s_tft.startWrite();
        if (s_use_12bit) {
            s_tft.writecommand(0x3A);
            s_tft.writedata(0x53);
        }
        s_tft.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
    } else {
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
    }

    for (int y = 0; y < plan.dstH; ++y) {
        const uint8_t* srcLine = indexedFrame + (size_t)s_ymap[y] * (size_t)width;
        if (s_use_ext && s_use_12bit) {
            a2600_draw_line_12bit(srcLine, palette565, plan.dstW);
        } else {
            a2600_draw_line_16bit(srcLine, palette565, plan.dstW);
        }
    }

    if (s_use_ext) {
        s_tft.endWrite();
    } else {
        M5Cardputer.Display.endWrite();
    }
}
