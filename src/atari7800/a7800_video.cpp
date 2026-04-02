#include "a7800_video.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>

#include <string>

#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../tft_setup.h"
#include "esp_heap_caps.h"

static TFT_eSPI s_tft;

static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;
static constexpr int kA7800PalWideVisibleHeight = 240;

bool a7800FullScreen = false;
int a7800ZoomPercent = 100;

static bool s_use_ext = false;
static bool s_use_12bit = false;
static double s_targetFps = 60.0;
static float s_aspectRatio = 4.0f / 3.0f;
static unsigned s_baseWidth = 320;
static unsigned s_baseHeight = 223;

static int16_t s_xmapStatic[EXT_W];
static int16_t s_ymapStatic[272];
static uint16_t s_lineBufStatic[EXT_W];

static uint16_t* s_lineBuf = nullptr;
static int s_lineCap = 0;
static int s_lineRows = 0;
/* Target rows batched per pushPixels call; low-memory fallback can shrink to 1. */
static constexpr int kVideoBatchLinesTarget = 8;
static int16_t* s_xmap = s_xmapStatic;
static int s_xmapCap = EXT_W;
static int16_t* s_ymap = s_ymapStatic;
static int s_ymapCap = 272;
static bool s_lineBufOwned = false;

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
static bool s_skipFrame = false;

/* Timing stats for the video submit path (reset each second by the run loop). */
static int64_t  s_videoTotalUs = 0;
static uint32_t s_videoCount   = 0;

struct A7800RenderPlan {
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

static void a7800_clear_target(void)
{
    if (s_use_ext) {
        s_tft.fillScreen(TFT_BLACK);
    } else {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
}

static void a7800_reset_layout_cache(void)
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

static bool a7800_layout_changed(const A7800RenderPlan& plan, int srcW, int srcH)
{
    const int viewMode = static_cast<int>(a7800_config_get_internal_view_mode());
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
        s_lastFull != a7800FullScreen ||
        s_lastZoom != a7800ZoomPercent ||
        s_lastViewMode != viewMode;

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
    s_lastFull = a7800FullScreen;
    s_lastZoom = a7800ZoomPercent;
    s_lastViewMode = viewMode;

    return changed;
}

static void a7800_compute_internal_plan(int srcW, int srcH, bool isPal, A7800RenderPlan& plan)
{
    const int targetW = M5Cardputer.Display.width();
    const int targetH = M5Cardputer.Display.height();

    if (a7800FullScreen) {
        const int zoom = clampi(a7800ZoomPercent, 100, 150);
        plan.roiW = clampi((srcW * 100) / zoom, 32, srcW);
        plan.roiH = clampi((srcH * 100) / zoom, 32, srcH);
        plan.srcX0 = (srcW - plan.roiW) / 2;
        plan.srcY0 = (srcH - plan.roiH) / 2;
        plan.dstW = targetW;
        plan.dstH = targetH;
        plan.xOff = 0;
        plan.yOff = 0;
        return;
    }

    const A7800InternalViewMode mode = a7800_config_get_internal_view_mode();
    plan.srcX0 = 0;
    plan.roiW = srcW;

    if (mode == A7800InternalViewMode::PixelPerfect) {
        plan.roiH = srcH;
        if (isPal && plan.roiH > 270) {
            plan.roiH = 270;
        }
        if ((plan.roiH & 1) != 0) {
            plan.roiH -= 1;
        }
        plan.srcY0 = (srcH - plan.roiH) / 2;
        plan.dstW = clampi(plan.roiW / 2, 1, targetW);
        plan.dstH = clampi(plan.roiH / 2, 1, targetH);
        plan.xOff = (targetW - plan.dstW) / 2;
        plan.yOff = (targetH - plan.dstH) / 2;
        return;
    }

    plan.roiH = isPal ? clampi(kA7800PalWideVisibleHeight, 64, srcH) : srcH;
    plan.srcY0 = (srcH - plan.roiH) / 2;

    float scaleX = (float)targetW / (float)plan.roiW;
    float scaleY = (float)targetH / (float)plan.roiH;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    plan.dstW = clampi((int)(plan.roiW * scale), 1, targetW);
    plan.dstH = clampi((int)(plan.roiH * scale), 1, targetH);
    plan.xOff = (targetW - plan.dstW) / 2;
    plan.yOff = (targetH - plan.dstH) / 2;
}

static void a7800_compute_plan(int srcW, int srcH, bool isPal, A7800RenderPlan& plan)
{
    if (s_use_ext) {
        const int zoom = clampi(a7800ZoomPercent, 100, 150);
        if (a7800FullScreen) {
            plan.roiW = clampi((srcW * 100) / zoom, 32, srcW);
            plan.roiH = clampi((srcH * 100) / zoom, 32, srcH);
            plan.srcX0 = (srcW - plan.roiW) / 2;
            plan.srcY0 = (srcH - plan.roiH) / 2;
            plan.dstW = EXT_W;
            plan.dstH = EXT_H;
            plan.xOff = 0;
            plan.yOff = 0;
        } else {
            plan.srcX0 = 0;
            plan.roiW = srcW;
            plan.roiH = (srcH > EXT_H) ? EXT_H : srcH;
            plan.srcY0 = (srcH - plan.roiH) / 2;
            plan.dstW = EXT_W;
            plan.dstH = plan.roiH;
            plan.xOff = 0;
            plan.yOff = (EXT_H - plan.dstH) / 2;
        }
        return;
    }

    a7800_compute_internal_plan(srcW, srcH, isPal, plan);
}

static bool a7800_prepare_luts(const A7800RenderPlan& plan)
{
    if (plan.dstW > s_xmapCap || plan.dstH > s_ymapCap) {
        return false;
    }

    if (plan.dstW > s_lineCap) {
        /* Prefer multi-line batches, but gracefully fall back to fewer rows
         * when the heap is too fragmented to hold the larger scratch buffer. */
        if (s_lineBufOwned && s_lineBuf) {
            free(s_lineBuf);
        }
        s_lineBuf = nullptr;
        s_lineCap = 0;
        s_lineRows = 0;
        s_lineBufOwned = false;

        for (int rows = kVideoBatchLinesTarget; rows >= 1; rows /= 2) {
            s_lineBuf = (uint16_t*)heap_caps_malloc(
                (size_t)plan.dstW * rows * sizeof(uint16_t),
                MALLOC_CAP_DMA | MALLOC_CAP_8BIT
            );
            if (!s_lineBuf) {
                s_lineBuf = (uint16_t*)malloc((size_t)plan.dstW * rows * sizeof(uint16_t));
            }
            if (s_lineBuf) {
                s_lineCap = plan.dstW;
                s_lineRows = rows;
                s_lineBufOwned = true;
                break;
            }
            if (rows == 1) {
                break;
            }
        }

        if (!s_lineBuf && plan.dstW <= EXT_W) {
            s_lineBuf = s_lineBufStatic;
            s_lineCap = EXT_W;
            s_lineRows = 1;
            s_lineBufOwned = false;
        }
    }

    if (!s_xmap || !s_ymap || !s_lineBuf) {
        return false;
    }

    for (int x = 0; x < plan.dstW; ++x) {
        s_xmap[x] = (int16_t)(plan.srcX0 + ((int64_t)x * plan.roiW) / plan.dstW);
    }

    for (int y = 0; y < plan.dstH; ++y) {
        if (plan.dstH == plan.roiH && s_use_ext) {
            s_ymap[y] = (int16_t)(plan.srcY0 + y);
        } else {
            s_ymap[y] = (int16_t)(plan.srcY0 + ((int64_t)y * plan.roiH) / plan.dstH);
        }
    }

    return true;
}

static void a7800_draw_line_12bit_565(const uint16_t* srcLine, int dstW)
{
    uint8_t* buf12 = (uint8_t*)s_lineBuf;
    const int pairs = dstW / 2;

    for (int pair = 0; pair < pairs; ++pair) {
        const uint16_t color1 = srcLine[s_xmap[pair * 2 + 0]];
        const uint16_t color2 = srcLine[s_xmap[pair * 2 + 1]];
        const int offset = pair * 3;
        buf12[offset + 0] = ((color1 >> 8) & 0xF0) | ((color1 >> 7) & 0x0F);
        buf12[offset + 1] = ((color1 << 3) & 0xF0) | ((color2 >> 12) & 0x0F);
        buf12[offset + 2] = ((color2 >> 3) & 0xF0) | ((color2 >> 1) & 0x0F);
    }

    if (dstW & 1) {
        const uint16_t color = srcLine[s_xmap[dstW - 1]];
        const int offset = pairs * 3;
        buf12[offset + 0] = ((color >> 8) & 0xF0) | ((color >> 7) & 0x0F);
        buf12[offset + 1] = ((color << 3) & 0xF0);
        buf12[offset + 2] = 0;
    }

    const int byteCount = ((dstW + 1) / 2) * 3;
    s_tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
}

static void a7800_draw_line_12bit_indexed(const uint8_t* srcLine, const uint16_t* palette565, int dstW)
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

static void a7800_draw_line_16bit_565(const uint16_t* srcLine, int dstW)
{
    for (int x = 0; x < dstW; ++x) {
        s_lineBuf[x] = srcLine[s_xmap[x]];
    }

    if (s_use_ext) {
        s_tft.pushColors(s_lineBuf, dstW, true);
    } else {
        M5Cardputer.Display.pushPixels(s_lineBuf, dstW);
    }
}

static void a7800_draw_line_16bit_indexed(const uint8_t* srcLine, const uint16_t* palette565, int dstW)
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

void a7800_video_show_external_info(const char* romTitle)
{
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    s_tft.setTextWrap(false);

    s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

    std::string title = romTitle ? romTitle : "";
    const char* drawTitle = title.empty() ? "ATARI 7800" : title.c_str();
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
    s_tft.drawCentreString("ATARI 7800", EXT_W / 2, 46, 2);

    s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
    s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

    s_tft.drawRoundRect(12, 86, EXT_W - 24, 108, 6, TFT_DARKGREY);
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

    const auto actions = share::emuControlActionLabels(share::EmuProfile::A7800);
    const auto keys = share::emuControlKeyLabels(share::EmuProfile::A7800);
    const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
    const size_t rowsPerCol = 5;

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

    s_tft.drawFastHLine(18, 200, EXT_W - 36, TFT_DARKGREY);
    s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    s_tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_W / 2, 208, 1);
    s_tft.drawCentreString("\\ = SCREEN  FN+\\ = VIEW", EXT_W / 2, 220, 1);
}

void a7800_video_init(double fps, unsigned baseWidth, unsigned baseHeight, float aspectRatio)
{
    s_use_ext = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    s_use_12bit = s_use_ext && (g_emu_color_depth == EMU_COLOR_12BIT);
    s_targetFps = fps;
    s_baseWidth = baseWidth;
    s_baseHeight = baseHeight;
    s_aspectRatio = aspectRatio > 0.0f ? aspectRatio : (4.0f / 3.0f);
    a7800_config_load_internal_view_mode();

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
        M5Cardputer.Display.setSwapBytes(true);
    }

    a7800_reset_layout_cache();

    /* Pre-allocate s_lineBuf NOW â€” before a7800_audio_init fragments the DMA
     * heap with I2S buffers.  At this point MARIA + flat_buf are already
     * allocated but audio is not, so a contiguous DMA block is still
     * available.  External path needs only 1 line; internal batches 8. */
    {
        const int maxDstW = s_use_ext ? EXT_W : M5Cardputer.Display.width();
        if (maxDstW > s_lineCap) {
            if (s_lineBufOwned && s_lineBuf) {
                free(s_lineBuf);
            }
            s_lineBuf = nullptr;
            s_lineCap = 0;
            s_lineRows = 0;
            s_lineBufOwned = false;
            for (int rows = kVideoBatchLinesTarget; rows >= 1; rows /= 2) {
                s_lineBuf = (uint16_t*)heap_caps_malloc(
                    (size_t)maxDstW * rows * sizeof(uint16_t),
                    MALLOC_CAP_DMA | MALLOC_CAP_8BIT
                );
                if (!s_lineBuf) {
                    s_lineBuf = (uint16_t*)malloc((size_t)maxDstW * rows * sizeof(uint16_t));
                }
                if (s_lineBuf) {
                    s_lineCap = maxDstW;
                    s_lineRows = rows;
                    s_lineBufOwned = true;
                    break;
                }
                if (rows == 1) {
                    break;
                }
            }
            if (!s_lineBuf && maxDstW <= EXT_W) {
                s_lineBuf = s_lineBufStatic;
                s_lineCap = EXT_W;
                s_lineRows = 1;
                s_lineBufOwned = false;
            }
            if (!s_lineBuf) {
                printf("[A7800][DISP] lineBuf pre-alloc failed (heap=%u)\n",
                       esp_get_free_heap_size());
            } else if (s_lineRows < kVideoBatchLinesTarget) {
                printf("[A7800][DISP] low-memory lineBuf rows=%d\n", s_lineRows);
            }
        }
    }

    if (!s_use_ext) {
        printf("[A7800][DISP] internal view=%s fps=%.2f base=%ux%u aspect=%.3f\n",
               a7800_config_get_internal_view_mode_label(),
               s_targetFps,
               s_baseWidth,
               s_baseHeight,
               s_aspectRatio);
    }
}

void a7800_video_shutdown(void)
{
    if (s_lineBufOwned && s_lineBuf) {
        free(s_lineBuf);
    }
    s_lineBuf = s_lineBufStatic;
    s_lineCap = 0;
    s_lineRows = 0;
    s_lineBufOwned = false;
    s_xmap = s_xmapStatic;
    s_xmapCap = EXT_W;
    s_ymap = s_ymapStatic;
    s_ymapCap = 272;

    a7800_reset_layout_cache();
}

void a7800_video_toggle_fullscreen(void)
{
    if (!a7800FullScreen) {
        a7800FullScreen = true;
        a7800ZoomPercent = 100;
    } else {
        a7800ZoomPercent += 10;
        if (a7800ZoomPercent > 150) {
            a7800ZoomPercent = 100;
            a7800FullScreen = false;
        }
    }
    a7800_reset_layout_cache();
}

void a7800_video_adjust_zoom(int delta)
{
    if (!a7800FullScreen) {
        a7800FullScreen = true;
    }

    a7800ZoomPercent = clampi(a7800ZoomPercent + delta, 100, 150);
    a7800_reset_layout_cache();
}

void a7800_video_set_frame_skip(bool skip) { s_skipFrame = skip; }

void a7800_video_get_and_reset_stats(int64_t* totalUs, uint32_t* count)
{
    *totalUs = s_videoTotalUs;
    *count   = s_videoCount;
    s_videoTotalUs = 0;
    s_videoCount   = 0;
}

void a7800_video_submit_rows(const uint8_t* const* rows,
                             unsigned width,
                             unsigned height,
                             const uint16_t* palette565,
                             bool isPal)
{
    if (s_skipFrame) {
        return;
    }
    if (!rows || width == 0 || height == 0) {
        return;
    }
    if (!palette565) {
        return;
    }

    const int64_t tVideoStart = esp_timer_get_time();
    const int batchLines = (s_lineRows > 0) ? s_lineRows : 1;

    A7800RenderPlan plan = {};
    a7800_compute_plan((int)width, (int)height, isPal, plan);

    if (!a7800_prepare_luts(plan)) {
        printf("[A7800][DISP] buffer allocation failed\n");
        return;
    }

    if (a7800_layout_changed(plan, (int)width, (int)height)) {
        a7800_clear_target();
    }

    const bool isPixelPerfectHalf = !s_use_ext
        && (plan.dstW * 2 == plan.roiW)
        && (plan.dstH * 2 == plan.roiH);

    if (isPixelPerfectHalf) {
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = rows[plan.srcY0 + (y + row) * 2];
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[x * 2]];
                }
            }
            M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        }
        M5Cardputer.Display.endWrite();
    } else if (!s_use_ext) {
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = rows[s_ymap[y + row]];
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[s_xmap[x]]];
                }
            }
            M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        }
        M5Cardputer.Display.endWrite();
    } else if (!s_use_12bit) {
        s_tft.startWrite();
        s_tft.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = rows[s_ymap[y + row]];
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[s_xmap[x]]];
                }
            }
            s_tft.pushColors(s_lineBuf, plan.dstW * batch, true);
        }
        s_tft.endWrite();
    } else {
        s_tft.startWrite();
        s_tft.writecommand(0x3A);
        s_tft.writedata(0x53);
        s_tft.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);

        for (int y = 0; y < plan.dstH; ++y) {
            const uint8_t* srcLine = rows[s_ymap[y]];
            a7800_draw_line_12bit_indexed(srcLine, palette565, plan.dstW);
        }

        s_tft.endWrite();
    }

    s_videoTotalUs += (esp_timer_get_time() - tVideoStart);
    s_videoCount++;
}
void a7800_video_submit_frame(const void* frame,
                              unsigned width,
                              unsigned height,
                              size_t pitch,
                              const uint16_t* palette565,
                              bool indexed,
                              bool isPal)
{
    if (s_skipFrame) {
        return;
    }
    if (!frame || width == 0 || height == 0 || pitch == 0) {
        return;
    }
    if (indexed && !palette565) {
        return;
    }

    const int64_t tVideoStart = esp_timer_get_time();
    const int batchLines = (s_lineRows > 0) ? s_lineRows : 1;

    A7800RenderPlan plan = {};
    a7800_compute_plan((int)width, (int)height, isPal, plan);

    if (!a7800_prepare_luts(plan)) {
        printf("[A7800][DISP] buffer allocation failed\n");
        return;
    }

    if (a7800_layout_changed(plan, (int)width, (int)height)) {
        a7800_clear_target();
    }

    /* ------------------------------------------------------------------ */
    /* Fast path: internal LCD, indexed color, PixelPerfect 1:2 downscale.
     * dstW = roiW/2 and dstH = roiH/2, so every dest pixel maps to
     * srcLine[x*2] and every dest row maps to src row (srcY0 + y*2).
     * This eliminates the s_xmap[] and s_ymap[] array loads entirely.   */
    const bool isPixelPerfectHalf = !s_use_ext && indexed
        && (plan.dstW * 2 == plan.roiW)
        && (plan.dstH * 2 == plan.roiH);

    if (isPixelPerfectHalf) {
        /* Batch as many rows as the current scratch buffer can hold. */
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = (const uint8_t*)frame
                    + (size_t)(plan.srcY0 + (y + row) * 2) * pitch;
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[x * 2]];
                }
            }
            M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        }
        M5Cardputer.Display.endWrite();
    } else {
    /* ------------------------------------------------------------------ */

    if (!s_use_ext && indexed) {
        /* Batched internal indexed path: fill as many rows as the scratch
         * buffer currently allows, then push in one call. */
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = (const uint8_t*)frame + (size_t)s_ymap[y + row] * pitch;
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[s_xmap[x]]];
                }
            }
            M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        }
        M5Cardputer.Display.endWrite();
    } else if (s_use_ext && indexed && !s_use_12bit) {
        /* Batched external 16-bit indexed path with adaptive row count. */
        s_tft.startWrite();
        s_tft.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
        for (int y = 0; y < plan.dstH; y += batchLines) {
            const int batch = (y + batchLines <= plan.dstH)
                ? batchLines : (plan.dstH - y);
            for (int row = 0; row < batch; ++row) {
                const uint8_t* srcLine = (const uint8_t*)frame + (size_t)s_ymap[y + row] * pitch;
                uint16_t* dst = s_lineBuf + (size_t)row * plan.dstW;
                for (int x = 0; x < plan.dstW; ++x) {
                    dst[x] = palette565[srcLine[s_xmap[x]]];
                }
            }
            s_tft.pushColors(s_lineBuf, plan.dstW * batch, true);
        }
        s_tft.endWrite();
    } else {
        /* External 12-bit or internal non-indexed: per-line fallback. */
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
            const uint8_t* srcBytes = (const uint8_t*)frame + (size_t)s_ymap[y] * pitch;
            if (indexed) {
                const uint8_t* srcLine = srcBytes;
                if (s_use_ext && s_use_12bit) {
                    a7800_draw_line_12bit_indexed(srcLine, palette565, plan.dstW);
                } else {
                    a7800_draw_line_16bit_indexed(srcLine, palette565, plan.dstW);
                }
            } else {
                const uint16_t* srcLine = (const uint16_t*)srcBytes;
                if (s_use_ext && s_use_12bit) {
                    a7800_draw_line_12bit_565(srcLine, plan.dstW);
                } else {
                    a7800_draw_line_16bit_565(srcLine, plan.dstW);
                }
            }
        }

        if (s_use_ext) {
            s_tft.endWrite();
        } else {
            M5Cardputer.Display.endWrite();
        }
    }
    } /* end else (general path) */

    s_videoTotalUs += (esp_timer_get_time() - tVideoStart);
    s_videoCount++;
}

