#include "videopac_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include "cardputer/CardputerView.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "tft_setup.h"
#include "videopac_config.h"
#include "videopac_trace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr int kInternalTargetW = 240;
constexpr int kInternalTargetH = 135;
constexpr int kExternalTargetW = 320;
constexpr int kExternalTargetH = 240;
constexpr uint32_t kVideopacExternalSpiFrequency = 60000000u;
constexpr int kExternalRgb444StripRows = 1;

static TFT_eSPI s_extTft;
static bool s_extTftPrepared = false;
static bool s_extTftRgb444Configured = false;
static bool s_extTftColorModeKnown = false;
static uint16_t* s_lineBuf = nullptr;
static uint8_t* s_lineBuf12 = nullptr;
static uint16_t* s_xMap = nullptr;
static int s_lineCap = 0;
static int s_lineBuf12Cap = 0;
static int s_xMapCap = 0;
static int s_xMapSrcW = -1;
static int s_xMapDstW = -1;
static int s_lastDstW = -1;
static int s_lastDstH = -1;
static int s_lastX = -1;
static int s_lastY = -1;
static bool s_lastExternal = false;

struct VideoPlan {
    int dstW;
    int dstH;
    int x;
    int y;
};

void prepare_external_tft()
{
    if (!s_extTftPrepared) {
        VideopacTraceScope scope("display", "external_tft_prepare");
        s_extTft.begin();
        s_extTft.setRotation(3);
        s_extTft.setTextWrap(false);
        s_extTftPrepared = true;
        s_extTftColorModeKnown = false;
        std::printf("[VIDEOPAC][VIDEO] external SPI write=%u read=%u\n",
                    static_cast<unsigned>(kVideopacExternalSpiFrequency),
                    static_cast<unsigned>(SPI_READ_FREQUENCY));
    }

    const bool useRgb444 = (g_emu_color_depth == EMU_COLOR_12BIT);
    if (!s_extTftColorModeKnown || s_extTftRgb444Configured != useRgb444) {
        s_extTft.startWrite();
        s_extTft.writecommand(0x3A);
        s_extTft.writedata(useRgb444 ? 0x53 : 0x55);
        s_extTft.endWrite();
        s_extTftRgb444Configured = useRgb444;
        s_extTftColorModeKnown = true;
    }
    s_extTft.setSwapBytes(!useRgb444);
}

bool use_external_rgb444(bool useExternal)
{
    return useExternal && (g_emu_color_depth == EMU_COLOR_12BIT);
}

void raise_external_spi_clock()
{
#if defined(SPI_HAS_TRANSACTION) && defined(SUPPORT_TRANSACTIONS) && \
    !defined(TFT_PARALLEL_8_BIT) && !defined(RP2040_PIO_INTERFACE)
    SPIClass& spi = TFT_eSPI::getSPIinstance();
    spi.endTransaction();
    spi.beginTransaction(SPISettings(kVideopacExternalSpiFrequency, MSBFIRST, TFT_SPI_MODE));
#endif
}

void begin_external_pixels(const VideoPlan& plan)
{
    prepare_external_tft();
    s_extTft.startWrite();
    s_extTft.setAddrWindow(plan.x, plan.y, plan.dstW, plan.dstH);
    raise_external_spi_clock();
}

void end_external_pixels()
{
    s_extTft.endWrite();
}

void release_line_buffer()
{
    free(s_lineBuf);
    free(s_lineBuf12);
    free(s_xMap);
    s_lineBuf = nullptr;
    s_lineBuf12 = nullptr;
    s_xMap = nullptr;
    s_lineCap = 0;
    s_lineBuf12Cap = 0;
    s_xMapCap = 0;
    s_xMapSrcW = -1;
    s_xMapDstW = -1;
}

bool ensure_line_buffer(int pixelCount)
{
    if (pixelCount <= s_lineCap) {
        return true;
    }

    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;

    s_lineBuf = static_cast<uint16_t*>(heap_caps_malloc(pixelCount * sizeof(uint16_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_lineBuf) {
        s_lineBuf = static_cast<uint16_t*>(heap_caps_malloc(pixelCount * sizeof(uint16_t),
                                                            MALLOC_CAP_8BIT));
    }
    if (!s_lineBuf) {
        std::printf("[VIDEOPAC][VIDEO] line buffer alloc failed for %d pixels\n", pixelCount);
        videopac_trace_printf("display", "line_buffer_alloc_failed pixels=%d", pixelCount);
        return false;
    }

    s_lineCap = pixelCount;
    videopac_trace_printf("display", "line_buffer_alloc pixels=%d bytes=%u",
                          pixelCount,
                          static_cast<unsigned>(pixelCount * sizeof(uint16_t)));
    return true;
}

bool ensure_rgb444_buffer(int pixelCount)
{
    const int bytes = ((pixelCount + 1) / 2) * 3;
    const int allocBytes = (bytes + 1) & ~1;
    if (allocBytes <= s_lineBuf12Cap) {
        return true;
    }

    free(s_lineBuf12);
    s_lineBuf12 = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(allocBytes),
                                                         MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!s_lineBuf12) {
        s_lineBuf12 = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(allocBytes),
                                                             MALLOC_CAP_8BIT));
    }
    if (!s_lineBuf12) {
        std::printf("[VIDEOPAC][VIDEO] RGB444 buffer alloc failed for %d bytes\n", allocBytes);
        videopac_trace_printf("display", "rgb444_buffer_alloc_failed bytes=%d", allocBytes);
        s_lineBuf12Cap = 0;
        return false;
    }

    s_lineBuf12Cap = allocBytes;
    videopac_trace_printf("display", "rgb444_buffer_alloc bytes=%d", allocBytes);
    return true;
}

bool ensure_x_map(int srcWidth, int dstWidth)
{
    if (srcWidth <= 0 || dstWidth <= 0) {
        return false;
    }

    if (s_xMap && s_xMapSrcW == srcWidth && s_xMapDstW == dstWidth) {
        return true;
    }

    if (dstWidth > s_xMapCap) {
        free(s_xMap);
        s_xMap = static_cast<uint16_t*>(heap_caps_malloc(dstWidth * sizeof(uint16_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!s_xMap) {
            s_xMap = static_cast<uint16_t*>(heap_caps_malloc(dstWidth * sizeof(uint16_t),
                                                            MALLOC_CAP_8BIT));
        }
        if (!s_xMap) {
            s_xMapCap = 0;
            videopac_trace_printf("display", "x_map_alloc_failed pixels=%d", dstWidth);
            return false;
        }
        s_xMapCap = dstWidth;
        videopac_trace_printf("display", "x_map_alloc pixels=%d bytes=%u",
                              dstWidth,
                              static_cast<unsigned>(dstWidth * sizeof(uint16_t)));
    }

    for (int dx = 0; dx < dstWidth; ++dx) {
        s_xMap[dx] = static_cast<uint16_t>((dx * srcWidth) / dstWidth);
    }
    s_xMapSrcW = srcWidth;
    s_xMapDstW = dstWidth;
    return true;
}

void pack_rgb444_line(const uint16_t* src, int pixelCount, uint8_t* dst)
{
    if (!src || !dst || pixelCount <= 0) {
        return;
    }

    const int pairs = pixelCount / 2;
    for (int p = 0; p < pairs; ++p) {
        const uint16_t c1 = src[p * 2];
        const uint16_t c2 = src[p * 2 + 1];
        const int j = p * 3;
        dst[j] = static_cast<uint8_t>(((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>(((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F));
        dst[j + 2] = static_cast<uint8_t>(((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F));
    }

    if (pixelCount & 1) {
        const uint16_t c = src[pixelCount - 1];
        const int j = pairs * 3;
        dst[j] = static_cast<uint8_t>(((c >> 8) & 0xF0) | ((c >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>((c << 3) & 0xF0);
        dst[j + 2] = 0u;
    }
}

void pack_indexed_rgb444_line(const uint8_t* src,
                              const uint16_t palette[256],
                              int pixelCount,
                              uint8_t* dst)
{
    if (!src || !palette || !dst || !s_xMap || pixelCount <= 0) {
        return;
    }

    const int pairs = pixelCount / 2;
    for (int p = 0; p < pairs; ++p) {
        const uint16_t c1 = palette[src[s_xMap[p * 2]]];
        const uint16_t c2 = palette[src[s_xMap[p * 2 + 1]]];
        const int j = p * 3;
        dst[j] = static_cast<uint8_t>(((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>(((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F));
        dst[j + 2] = static_cast<uint8_t>(((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F));
    }

    if (pixelCount & 1) {
        const uint16_t c = palette[src[s_xMap[pixelCount - 1]]];
        const int j = pairs * 3;
        dst[j] = static_cast<uint8_t>(((c >> 8) & 0xF0) | ((c >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>((c << 3) & 0xF0);
        dst[j + 2] = 0u;
    }
}

void push_external_rgb444_pixels(uint8_t* data, int count)
{
    const int bytes = ((count + 1) / 2) * 3;
    if (bytes & 1) {
        data[bytes] = 0u;
    }
    s_extTft.pushPixels(data, static_cast<uint32_t>((bytes + 1) / 2));
}

void push_external_rgb444_buffer(int count)
{
    push_external_rgb444_pixels(s_lineBuf12, count);
}

void push_external_line(const uint16_t* pixels, int count, bool useRgb444)
{
    if (useRgb444) {
        pack_rgb444_line(pixels, count, s_lineBuf12);
        push_external_rgb444_buffer(count);
    } else {
        s_extTft.pushPixels(pixels, static_cast<uint32_t>(count));
    }
}

VideoPlan make_plan(int width, int height, bool useExternal)
{
    const int targetW = useExternal ? kExternalTargetW : kInternalTargetW;
    const int targetH = useExternal ? kExternalTargetH : kInternalTargetH;

    VideoPlan plan = {};
    if (width <= 0 || height <= 0) {
        return plan;
    }

    if (useExternal && videopac_config_get_video_mode() == VideopacVideoMode::Fast) {
        plan.dstW = std::min(width, targetW);
        plan.dstH = std::min(height, targetH);
        plan.x = (targetW - plan.dstW) / 2;
        plan.y = (targetH - plan.dstH) / 2;
        return plan;
    }

    const int byWidthH = (height * targetW) / width;
    if (byWidthH <= targetH) {
        plan.dstW = targetW;
        plan.dstH = std::max(1, byWidthH);
    } else {
        plan.dstH = targetH;
        plan.dstW = std::max(1, (width * targetH) / height);
    }

    plan.x = (targetW - plan.dstW) / 2;
    plan.y = (targetH - plan.dstH) / 2;
    return plan;
}

void clear_target(bool useExternal)
{
    if (useExternal) {
        prepare_external_tft();
        s_extTft.fillScreen(TFT_BLACK);
    } else {
        M5Cardputer.Display.setSwapBytes(true);
        M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
}

bool plan_changed(const VideoPlan& plan, bool useExternal)
{
    return s_lastDstW != plan.dstW ||
           s_lastDstH != plan.dstH ||
           s_lastX != plan.x ||
           s_lastY != plan.y ||
           s_lastExternal != useExternal;
}

std::string truncate_title(const char* title, size_t maxChars)
{
    if (!title || title[0] == '\0') {
        return "VIDEOPAC";
    }

    std::string value(title);
    if (value.size() <= maxChars) {
        return value;
    }
    if (maxChars <= 3) {
        return value.substr(0, maxChars);
    }
    return value.substr(0, maxChars - 3) + "...";
}

void draw_key_badge(TFT_eSPI& tft, int x, int y, const std::string& key)
{
    const int bw = 34;
    const int bh = 18;
    tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
    tft.drawRoundRect(x, y, bw, bh, 4, TFT_CYAN);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawCentreString(key.c_str(), x + bw / 2, y + 1, 2);
}

} // namespace

void videopac_display_init(bool useExternal)
{
    videopac_trace_printf("display", "init target=%s", useExternal ? "external" : "internal");
    s_lastDstW = -1;
    s_lastDstH = -1;
    s_lastX = -1;
    s_lastY = -1;
    s_lastExternal = useExternal;
    clear_target(useExternal);
}

void videopac_display_render(const uint16_t* buffer, int width, int height, int pitchPixels, bool useExternal)
{
    if (!buffer || width <= 0 || height <= 0) {
        return;
    }

    if (pitchPixels < width) {
        pitchPixels = width;
    }

    const VideoPlan plan = make_plan(width, height, useExternal);
    if (plan.dstW <= 0 || plan.dstH <= 0) {
        return;
    }

    if (plan_changed(plan, useExternal)) {
        videopac_trace_printf("display", "plan size=%dx%d pitch=%d target=%s dst=%dx%d pos=%d,%d",
                              width,
                              height,
                              pitchPixels,
                              useExternal ? "external" : "internal",
                              plan.dstW,
                              plan.dstH,
                              plan.x,
                              plan.y);
        clear_target(useExternal);
        s_lastDstW = plan.dstW;
        s_lastDstH = plan.dstH;
        s_lastX = plan.x;
        s_lastY = plan.y;
        s_lastExternal = useExternal;
    }

    if (useExternal) {
        const bool useRgb444 = use_external_rgb444(useExternal);
        if (useRgb444 && !ensure_rgb444_buffer(plan.dstW)) {
            return;
        }

        const bool directCopy = plan.dstW == width && plan.dstH == height;
        if (!directCopy && !ensure_line_buffer(plan.dstW)) {
            return;
        }
        if (!directCopy && !ensure_x_map(width, plan.dstW)) {
            return;
        }

        begin_external_pixels(plan);
        int previousSy = -1;
        for (int dy = 0; dy < plan.dstH; ++dy) {
            if (directCopy) {
                push_external_line(buffer + dy * pitchPixels, plan.dstW, useRgb444);
                continue;
            }

            const int sy = (dy * height) / plan.dstH;
            if (sy != previousSy) {
                previousSy = sy;
                const uint16_t* src = buffer + sy * pitchPixels;
                for (int dx = 0; dx < plan.dstW; ++dx) {
                    s_lineBuf[dx] = src[s_xMap[dx]];
                }
            }
            push_external_line(s_lineBuf, plan.dstW, useRgb444);
        }
        end_external_pixels();
        return;
    } else {
        M5Cardputer.Display.setSwapBytes(true);
    }

    if (!ensure_line_buffer(plan.dstW)) {
        return;
    }
    if (!ensure_x_map(width, plan.dstW)) {
        return;
    }

    int previousSy = -1;
    for (int dy = 0; dy < plan.dstH; ++dy) {
        const int sy = (dy * height) / plan.dstH;
        if (sy != previousSy) {
            previousSy = sy;
            const uint16_t* src = buffer + sy * pitchPixels;
            for (int dx = 0; dx < plan.dstW; ++dx) {
                s_lineBuf[dx] = src[s_xMap[dx]];
            }
        }

        M5Cardputer.Display.pushImage(plan.x, plan.y + dy, plan.dstW, 1, s_lineBuf);
    }
}

void videopac_display_render_indexed(const uint8_t* buffer,
                                     int width,
                                     int height,
                                     int pitchPixels,
                                     const uint16_t palette[256],
                                     bool useExternal)
{
    if (!buffer || !palette || width <= 0 || height <= 0) {
        return;
    }

    if (pitchPixels < width) {
        pitchPixels = width;
    }

    const VideoPlan plan = make_plan(width, height, useExternal);
    if (plan.dstW <= 0 || plan.dstH <= 0) {
        return;
    }

    if (plan_changed(plan, useExternal)) {
        videopac_trace_printf("display", "indexed_plan size=%dx%d pitch=%d target=%s dst=%dx%d pos=%d,%d",
                              width,
                              height,
                              pitchPixels,
                              useExternal ? "external" : "internal",
                              plan.dstW,
                              plan.dstH,
                              plan.x,
                              plan.y);
        clear_target(useExternal);
        s_lastDstW = plan.dstW;
        s_lastDstH = plan.dstH;
        s_lastX = plan.x;
        s_lastY = plan.y;
        s_lastExternal = useExternal;
    }

    const bool useRgb444 = use_external_rgb444(useExternal);
    if (useExternal) {
        const int rgb444Pixels = useRgb444
            ? plan.dstW * std::min(plan.dstH, kExternalRgb444StripRows)
            : plan.dstW;
        if (useRgb444 && !ensure_rgb444_buffer(rgb444Pixels)) {
            return;
        }
    } else {
        M5Cardputer.Display.setSwapBytes(true);
    }

    if (!ensure_x_map(width, plan.dstW)) {
        return;
    }

    if (useExternal) {
        if (!useRgb444 && !ensure_line_buffer(plan.dstW)) {
            return;
        }

        begin_external_pixels(plan);
        if (useRgb444) {
            const int maxStripRows = std::min(plan.dstH, kExternalRgb444StripRows);
            const int rowBytes = ((plan.dstW + 1) / 2) * 3;
            int dy = 0;
            int previousSy = -1;
            uint8_t* previousPackedRow = nullptr;

            while (dy < plan.dstH) {
                const int rows = std::min(maxStripRows, plan.dstH - dy);
                for (int row = 0; row < rows; ++row) {
                    const int sy = ((dy + row) * height) / plan.dstH;
                    uint8_t* dst = s_lineBuf12 + row * rowBytes;
                    if (sy == previousSy && previousPackedRow) {
                        std::memcpy(dst, previousPackedRow, static_cast<size_t>(rowBytes));
                    } else {
                        previousSy = sy;
                        previousPackedRow = dst;
                        const uint8_t* src = buffer + sy * pitchPixels;
                        pack_indexed_rgb444_line(src, palette, plan.dstW, dst);
                    }
                }
                push_external_rgb444_pixels(s_lineBuf12, plan.dstW * rows);
                dy += rows;
            }
        } else {
            int previousSy = -1;
            for (int dy = 0; dy < plan.dstH; ++dy) {
                const int sy = (dy * height) / plan.dstH;
                if (sy != previousSy) {
                    previousSy = sy;
                    const uint8_t* src = buffer + sy * pitchPixels;
                    for (int dx = 0; dx < plan.dstW; ++dx) {
                        s_lineBuf[dx] = palette[src[s_xMap[dx]]];
                    }
                }
                push_external_line(s_lineBuf, plan.dstW, false);
            }
        }
        end_external_pixels();
        return;
    }

    if (!ensure_line_buffer(plan.dstW)) {
        return;
    }

    int previousSy = -1;
    for (int dy = 0; dy < plan.dstH; ++dy) {
        const int sy = (dy * height) / plan.dstH;
        if (sy != previousSy) {
            previousSy = sy;
            const uint8_t* src = buffer + sy * pitchPixels;

            for (int dx = 0; dx < plan.dstW; ++dx) {
                s_lineBuf[dx] = palette[src[s_xMap[dx]]];
            }
        }

        M5Cardputer.Display.pushImage(plan.x, plan.y + dy, plan.dstW, 1, s_lineBuf);
    }
}

void videopac_display_show_external_info(const char* romTitle, const char* biosName)
{
    VideopacTraceScope scope("display", "external_info");
    prepare_external_tft();
    auto& tft = s_extTft;
    tft.fillScreen(TFT_BLACK);
    tft.setTextWrap(false);

    tft.drawRoundRect(8, 8, kExternalTargetW - 16, kExternalTargetH - 16, 8, TFT_DARKGREY);

    const std::string title = truncate_title(romTitle, 34);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString(title.c_str(), kExternalTargetW / 2, 16, 4);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("VIDEOPAC / ODYSSEY2", kExternalTargetW / 2, 42, 2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("VIDEO ON INTERNAL LCD", kExternalTargetW / 2, 66, 2);

    tft.drawRoundRect(12, 88, kExternalTargetW - 24, 90, 6, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("CONTROLS", kExternalTargetW / 2, 94, 2);

    const auto actions = share::emuControlActionLabels(share::EmuProfile::Videopac);
    const auto keys = share::emuControlKeyLabels(share::EmuProfile::Videopac);
    const size_t count = std::min(actions.size(), keys.size());
    const size_t rowsPerCol = 4;

    for (size_t i = 0; i < count; ++i) {
        const int col = static_cast<int>(i / rowsPerCol);
        const int row = static_cast<int>(i % rowsPerCol);
        const int baseX = 24 + col * 146;
        const int baseY = 114 + row * 16;

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(actions[i].c_str(), baseX, baseY, 2);
        draw_key_badge(tft, baseX + 88, baseY - 3, keys[i]);
    }

    tft.drawFastHLine(18, 188, kExternalTargetW - 36, TFT_DARKGREY);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("GO = QUIT   HOLD GO = MENU", kExternalTargetW / 2, 198, 1);
    tft.drawCentreString("\\ toggles JOYSTICK / KEYBOARD", kExternalTargetW / 2, 210, 1);
    std::string biosLine = "BIOS: ";
    biosLine += biosName && biosName[0] ? biosName : "o2rom.bin";
    tft.drawCentreString(biosLine.c_str(), kExternalTargetW / 2, 222, 1);
}

void videopac_display_show_runtime_menu(bool useExternal)
{
    (void)useExternal;

    auto& tft = M5Cardputer.Display;
    tft.setSwapBytes(true);
    tft.fillRoundRect(20, 34, 200, 78, 6, TFT_BLACK);
    tft.drawRoundRect(20, 34, 200, 78, 6, TFT_CYAN);
    tft.drawFastHLine(30, 57, 180, TFT_DARKGREY);

    tft.setTextWrap(false);
    tft.setTextSize(1);
    const auto drawCentered = [&](const char* text, int y, uint16_t color) {
        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor((kInternalTargetW - static_cast<int>(tft.textWidth(text))) / 2, y);
        tft.print(text);
    };

    tft.setTextSize(2);
    drawCentered("VIDEOPAC MENU", 40, TFT_CYAN);

    tft.setTextSize(1);
    const char* modeLabel = videopac_config_get_video_mode_label();
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(38, 70);
    tft.print("VIDEO");
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(200 - static_cast<int>(tft.textWidth(modeLabel)), 70);
    tft.print(modeLabel);

    drawCentered("LEFT / RIGHT change", 91, TFT_DARKGREY);
    drawCentered("ENTER BACK ESC close", 103, TFT_DARKGREY);
}

void videopac_display_show_input_mode_overlay(bool keyboardOnlyMode, bool useExternal)
{
    const char* label = keyboardOnlyMode ? "KBD MODE ON" : "JOY MODE ON";
    const uint16_t accent = keyboardOnlyMode ? TFT_CYAN : TFT_YELLOW;

    if (useExternal) {
        prepare_external_tft();

        const bool wasRgb444 = use_external_rgb444(true);
        if (wasRgb444) {
            s_extTft.startWrite();
            s_extTft.writecommand(0x3A);
            s_extTft.writedata(0x55);
            s_extTft.endWrite();
            s_extTft.setSwapBytes(true);
            s_extTftColorModeKnown = false;
        }

        const int w = 156;
        const int h = 30;
        const int x = (kExternalTargetW - w) / 2;
        const int y = 14;
        s_extTft.fillRoundRect(x, y, w, h, 5, TFT_BLACK);
        s_extTft.drawRoundRect(x, y, w, h, 5, accent);
        s_extTft.setTextColor(accent, TFT_BLACK);
        s_extTft.drawCentreString(label, kExternalTargetW / 2, y + 6, 2);
        return;
    }

    auto& tft = M5Cardputer.Display;
    tft.setSwapBytes(true);
    const int w = 126;
    const int h = 24;
    const int x = (kInternalTargetW - w) / 2;
    const int y = 8;
    tft.fillRoundRect(x, y, w, h, 5, TFT_BLACK);
    tft.drawRoundRect(x, y, w, h, 5, accent);
    tft.setTextSize(2);
    tft.setTextColor(accent, TFT_BLACK);
    tft.drawCenterString(label, kInternalTargetW / 2, y + 13);
}

void videopac_display_shutdown(void)
{
    videopac_trace_mark("display", "shutdown");
    release_line_buffer();
    s_lastDstW = -1;
    s_lastDstH = -1;
    s_extTftPrepared = false;
}
