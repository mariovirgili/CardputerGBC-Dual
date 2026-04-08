#include "msx_video.h"

#include <M5Cardputer.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "msx_config.h"
#include "msx_display.h"

namespace {

constexpr int kTargetW = 240;
constexpr int kTargetH = 135;
constexpr int kWideAspectW = 4;
constexpr int kWideAspectH = 3;
constexpr int kBatchLines = 6;

struct MsxVideoPlan {
    int srcX0;
    int srcY0;
    int roiW;
    int roiH;
    int dstW;
    int dstH;
    int xOff;
    int yOff;
    bool cropOnly;
};

static uint16_t* s_lineBuf = nullptr;
static int s_lineCap = 0;
static int16_t* s_xmap = nullptr;
static int s_xmapCap = 0;
static int16_t* s_ymap = nullptr;
static int s_ymapCap = 0;
static int s_lastMode = -1;
static int s_lastSrcW = -1;
static int s_lastSrcH = -1;
static int s_lastDstW = -1;
static int s_lastDstH = -1;
static int s_lastSrcX0 = -1;
static int s_lastSrcY0 = -1;
static int s_lastRoiW = -1;
static int s_lastRoiH = -1;
static int s_lastXOff = -1;
static int s_lastYOff = -1;
static bool s_swapBytesConfigured = false;
static uint16_t s_palette565[16] = {};

constexpr uint16_t msx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

void msx_video_init_palette(void)
{
    static constexpr uint8_t kPalette[16][3] = {
        {0x00, 0x00, 0x00},
        {0x00, 0x00, 0x00},
        {0x21, 0xC8, 0x42},
        {0x5E, 0xDC, 0x78},
        {0x54, 0x55, 0xED},
        {0x7D, 0x76, 0xFC},
        {0xD4, 0x52, 0x4D},
        {0x42, 0xEB, 0xF5},
        {0xFC, 0x55, 0x54},
        {0xFF, 0x79, 0x78},
        {0xD4, 0xC1, 0x54},
        {0xE6, 0xCE, 0x80},
        {0x21, 0xB0, 0x3B},
        {0xC9, 0x5B, 0xBA},
        {0xCC, 0xCC, 0xCC},
        {0xFF, 0xFF, 0xFF},
    };

    for (int i = 0; i < 16; ++i) {
        s_palette565[i] = msx_rgb565(kPalette[i][0], kPalette[i][1], kPalette[i][2]);
    }
}

void msx_video_reset_layout_cache(void)
{
    s_lastMode = -1;
    s_lastSrcW = -1;
    s_lastSrcH = -1;
    s_lastDstW = -1;
    s_lastDstH = -1;
    s_lastSrcX0 = -1;
    s_lastSrcY0 = -1;
    s_lastRoiW = -1;
    s_lastRoiH = -1;
    s_lastXOff = -1;
    s_lastYOff = -1;
}

void msx_video_clear_target(void)
{
    M5Cardputer.Display.fillScreen(TFT_BLACK);
}

void msx_video_compute_plan(unsigned srcW, unsigned srcH, MsxVideoPlan* plan)
{
    if (!plan) {
        return;
    }

    std::memset(plan, 0, sizeof(*plan));
    if (srcW == 0 || srcH == 0) {
        return;
    }

    const MsxInternalViewMode mode = msx_config_get_internal_view_mode();
    if (mode == MsxInternalViewMode::PixelPerfect) {
        plan->srcX0 = static_cast<int>((srcW > static_cast<unsigned>(kTargetW)) ? (srcW - kTargetW) / 2u : 0u);
        plan->srcY0 = static_cast<int>((srcH > static_cast<unsigned>(kTargetH)) ? (srcH - kTargetH) / 2u : 0u);
        plan->roiW = static_cast<int>((srcW > static_cast<unsigned>(kTargetW)) ? kTargetW : srcW);
        plan->roiH = static_cast<int>((srcH > static_cast<unsigned>(kTargetH)) ? kTargetH : srcH);
        plan->dstW = plan->roiW;
        plan->dstH = plan->roiH;
        plan->xOff = (kTargetW - plan->dstW) / 2;
        plan->yOff = (kTargetH - plan->dstH) / 2;
        plan->cropOnly = true;
        return;
    }

    plan->srcX0 = 0;
    plan->srcY0 = 0;
    plan->roiW = static_cast<int>(srcW);
    plan->roiH = static_cast<int>(srcH);
    plan->dstH = kTargetH;
    plan->dstW = (kTargetH * kWideAspectW) / kWideAspectH;
    if (plan->dstW > kTargetW) {
        plan->dstW = kTargetW;
    }
    plan->xOff = (kTargetW - plan->dstW) / 2;
    plan->yOff = 0;
    plan->cropOnly = false;
}

bool msx_video_layout_changed(const MsxVideoPlan& plan, unsigned srcW, unsigned srcH)
{
    const int mode = static_cast<int>(msx_config_get_internal_view_mode());
    const bool changed =
        s_lastMode != mode ||
        s_lastSrcW != static_cast<int>(srcW) ||
        s_lastSrcH != static_cast<int>(srcH) ||
        s_lastDstW != plan.dstW ||
        s_lastDstH != plan.dstH ||
        s_lastSrcX0 != plan.srcX0 ||
        s_lastSrcY0 != plan.srcY0 ||
        s_lastRoiW != plan.roiW ||
        s_lastRoiH != plan.roiH ||
        s_lastXOff != plan.xOff ||
        s_lastYOff != plan.yOff;

    s_lastMode = mode;
    s_lastSrcW = static_cast<int>(srcW);
    s_lastSrcH = static_cast<int>(srcH);
    s_lastDstW = plan.dstW;
    s_lastDstH = plan.dstH;
    s_lastSrcX0 = plan.srcX0;
    s_lastSrcY0 = plan.srcY0;
    s_lastRoiW = plan.roiW;
    s_lastRoiH = plan.roiH;
    s_lastXOff = plan.xOff;
    s_lastYOff = plan.yOff;
    return changed;
}

bool msx_video_prepare_buffers(const MsxVideoPlan& plan, bool layoutChanged)
{
    const int neededLineWidth = plan.dstW;
    if (neededLineWidth <= 0 || plan.dstH <= 0) {
        return false;
    }

    if (neededLineWidth > s_lineCap) {
        free(s_lineBuf);
        s_lineBuf = static_cast<uint16_t*>(heap_caps_malloc(
            static_cast<size_t>(neededLineWidth) * kBatchLines * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        ));
        if (!s_lineBuf) {
            s_lineBuf = static_cast<uint16_t*>(malloc(static_cast<size_t>(neededLineWidth) * kBatchLines * sizeof(uint16_t)));
        }
        s_lineCap = s_lineBuf ? neededLineWidth : 0;
    }

    if (!plan.cropOnly) {
        if (plan.dstW > s_xmapCap) {
            free(s_xmap);
            s_xmap = static_cast<int16_t*>(malloc(static_cast<size_t>(plan.dstW) * sizeof(int16_t)));
            s_xmapCap = s_xmap ? plan.dstW : 0;
            layoutChanged = true;
        }

        if (plan.dstH > s_ymapCap) {
            free(s_ymap);
            s_ymap = static_cast<int16_t*>(malloc(static_cast<size_t>(plan.dstH) * sizeof(int16_t)));
            s_ymapCap = s_ymap ? plan.dstH : 0;
            layoutChanged = true;
        }
    }

    if (!s_lineBuf) {
        return false;
    }
    if (!plan.cropOnly && (!s_xmap || !s_ymap)) {
        return false;
    }

    if (!plan.cropOnly && layoutChanged) {
        for (int x = 0; x < plan.dstW; ++x) {
            s_xmap[x] = static_cast<int16_t>(plan.srcX0 + (static_cast<int64_t>(x) * plan.roiW) / plan.dstW);
        }
        for (int y = 0; y < plan.dstH; ++y) {
            s_ymap[y] = static_cast<int16_t>(plan.srcY0 + (static_cast<int64_t>(y) * plan.roiH) / plan.dstH);
        }
    }

    return true;
}

void msx_video_draw_crop_frame(const MsxDisplayFrame* frame, const MsxVideoPlan& plan)
{
    const uint16_t* palette = frame->palette565 ? frame->palette565 : s_palette565;

    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);

    for (int y = 0; y < plan.dstH; y += kBatchLines) {
        const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
        for (int row = 0; row < batch; ++row) {
            const uint8_t* src = frame->indexed8
                + static_cast<size_t>(plan.srcY0 + y + row) * frame->pitchBytes
                + static_cast<size_t>(plan.srcX0);
            uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
            for (int x = 0; x < plan.dstW; ++x) {
                dst[x] = palette[src[x]];
            }
        }
        M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
    }

    M5Cardputer.Display.endWrite();
}

void msx_video_draw_scaled_frame(const MsxDisplayFrame* frame, const MsxVideoPlan& plan)
{
    const uint16_t* palette = frame->palette565 ? frame->palette565 : s_palette565;

    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);

    for (int y = 0; y < plan.dstH; y += kBatchLines) {
        const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
        for (int row = 0; row < batch; ++row) {
            const uint8_t* src = frame->indexed8 + static_cast<size_t>(s_ymap[y + row]) * frame->pitchBytes;
            uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
            for (int x = 0; x < plan.dstW; ++x) {
                dst[x] = palette[src[s_xmap[x]]];
            }
        }
        M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
    }

    M5Cardputer.Display.endWrite();
}

} // namespace

void msx_video_init(void)
{
    msx_video_init_palette();
    if (!s_swapBytesConfigured) {
        M5Cardputer.Display.setSwapBytes(true);
        s_swapBytesConfigured = true;
    }
    msx_video_reset_layout_cache();
    msx_video_clear_target();
}

void msx_video_shutdown(void)
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

    msx_video_reset_layout_cache();
}

bool msx_video_present_frame(const MsxDisplayFrame* frame)
{
    if (!frame || !frame->indexed8 || frame->width == 0 || frame->height == 0 || frame->pitchBytes < frame->width) {
        return false;
    }

    MsxVideoPlan plan = {};
    msx_video_compute_plan(frame->width, frame->height, &plan);
    const bool layoutChanged = msx_video_layout_changed(plan, frame->width, frame->height);
    if (!msx_video_prepare_buffers(plan, layoutChanged)) {
        return false;
    }

    if (layoutChanged) {
        msx_video_clear_target();
    }

    if (plan.cropOnly) {
        msx_video_draw_crop_frame(frame, plan);
    } else {
        msx_video_draw_scaled_frame(frame, plan);
    }

    return true;
}
