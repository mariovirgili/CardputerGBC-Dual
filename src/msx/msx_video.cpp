#include "msx_video.h"

#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../share/display_target.h"
#include "../tft_setup.h"
#include "msx_config.h"
#include "msx_display.h"

#ifndef MSX_VIDEO_PERF_LOG_ENABLED
#define MSX_VIDEO_PERF_LOG_ENABLED 0
#endif

namespace {

constexpr int kInternalTargetW = 240;
constexpr int kInternalTargetH = 135;
constexpr int kExternalTargetW = 320;
constexpr int kExternalTargetH = 240;
constexpr int kExternalFastTargetW = 240;
constexpr int kExternalFastTargetH = 180;
constexpr int kExternalFastPlusTargetW = 192;
constexpr int kWideAspectW = 4;
constexpr int kWideAspectH = 3;
constexpr int kBatchLines = 8;
constexpr unsigned kMsxVisibleSafeHeight = 212u;

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

struct MsxLineStreamState {
    bool active;
    bool cropOnly;
    bool useRgb444;
    int srcX0;
    int srcY0;
    int roiH;
    int dstW;
    int dstH;
    const uint16_t* palette;
    uint16_t paletteEntries;
    int nextDstY;
};

void msx_video_pack_indexed_rgb444_line(const uint8_t* src,
                                        uint8_t* dst,
                                        int pixelCount,
                                        uint16_t paletteEntries);
void msx_video_pack_mapped_rgb444_line(const uint8_t* src,
                                       uint8_t* dst,
                                       int pixelCount,
                                       const int16_t* xmap,
                                       uint16_t paletteEntries);

static uint16_t* s_lineBuf = nullptr;
static int s_lineCap = 0;
static uint8_t* s_lineBuf12 = nullptr;
static int s_lineBuf12Cap = 0;
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
static bool s_firstPresentLogged = false;
static uint16_t s_palette565[16] = {};
static uint32_t s_palettePairs565[256] = {};
static uint16_t s_palette444[256] = {};
static uint32_t s_palettePairs444[256] = {};
static TFT_eSPI s_extTft;
static bool s_extTftPrepared = false;
static bool s_extTftRgb444Configured = false;
static bool s_extTftColorModeKnown = false;
static bool s_extTftClockLogged = false;
static bool s_extTftColorModeLogged = false;
static bool s_externalUiActive = false;
static bool s_runtimeMenuActive = false;
static bool s_stateOverlayActive = false;
static bool s_externalFrameskipState = false;
static MsxLineStreamState s_lineStream = {};

static uint32_t s_spiPushFrames = 0;
static uint32_t s_spiPushUs = 0;
static uint32_t s_lastPresentUs = 0;
static MsxVideoPerfSummary s_videoPerfSummary = {};
static SemaphoreHandle_t s_videoMutex = nullptr;

bool msx_video_game_on_external(void)
{
    return g_emu_display_target == EMU_DISPLAY_EXTERNAL;
}

bool msx_video_use_external_rgb444(void)
{
    return msx_video_game_on_external() && (g_emu_color_depth == EMU_COLOR_12BIT);
}

int msx_video_target_w(void)
{
    return msx_video_game_on_external() ? kExternalTargetW : kInternalTargetW;
}

int msx_video_target_h(void)
{
    return msx_video_game_on_external() ? kExternalTargetH : kInternalTargetH;
}

void msx_video_prepare_external_tft(void)
{
    if (!s_extTftPrepared) {
        s_extTft.begin();
        s_extTft.setRotation(3);
        s_extTft.setTextWrap(false);
        s_extTftPrepared = true;
        s_extTftColorModeKnown = false;
        if (!s_extTftClockLogged) {
            std::printf("[MSX][VIDEO] external SPI write=%u read=%u\n",
                        static_cast<unsigned>(SPI_FREQUENCY),
                        static_cast<unsigned>(SPI_READ_FREQUENCY));
            s_extTftClockLogged = true;
        }
    }

    const bool useRgb444 = msx_video_use_external_rgb444();
    if (!s_extTftColorModeKnown || s_extTftRgb444Configured != useRgb444) {
        s_extTft.startWrite();
        s_extTft.writecommand(0x3A);
        s_extTft.writedata(useRgb444 ? 0x53 : 0x55);
        s_extTft.endWrite();
        s_extTftRgb444Configured = useRgb444;
        s_extTftColorModeKnown = true;
        std::printf("[MSX][VIDEO] external color=%s\n",
                    useRgb444 ? "RGB444 12-bit" : "RGB565 16-bit");
        s_extTftColorModeLogged = true;
    } else if (!s_extTftColorModeLogged) {
        std::printf("[MSX][VIDEO] external color=%s\n",
                    s_extTftRgb444Configured ? "RGB444 12-bit" : "RGB565 16-bit");
        s_extTftColorModeLogged = true;
    }
}

constexpr uint16_t msx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>((r & 0xF8u) | (g >> 5) | ((g & 0x1Cu) << 11) | ((b & 0xF8u) << 5));
}

inline uint16_t msx_rgb565_to_rgb444(uint16_t raw)
{
    const uint16_t c = static_cast<uint16_t>((raw >> 8) | (raw << 8));
    return static_cast<uint16_t>(((c >> 4) & 0x0F00u) |
                                 ((c >> 3) & 0x00F0u) |
                                 ((c >> 1) & 0x000Fu));
}

constexpr uint32_t msx_pack_rgb444_pair(uint16_t c0, uint16_t c1)
{
    return static_cast<uint32_t>((c0 >> 4) & 0xFFu)
         | (static_cast<uint32_t>(((c0 & 0x0Fu) << 4) | ((c1 >> 8) & 0x0Fu)) << 8)
         | (static_cast<uint32_t>(c1 & 0x00FFu) << 16);
}

inline void msx_store_rgb444_pair(uint8_t*& dst, uint32_t packed)
{
    dst[0] = static_cast<uint8_t>(packed & 0xFFu);
    dst[1] = static_cast<uint8_t>((packed >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((packed >> 16) & 0xFFu);
    dst += 3;
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

void msx_video_release_scratch_buffers(void)
{
    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;

    free(s_lineBuf12);
    s_lineBuf12 = nullptr;
    s_lineBuf12Cap = 0;

    free(s_xmap);
    s_xmap = nullptr;
    s_xmapCap = 0;

    free(s_ymap);
    s_ymap = nullptr;
    s_ymapCap = 0;
}

void msx_video_clear_target(void)
{
    if (msx_video_game_on_external()) {
        msx_video_prepare_external_tft();
        s_extTft.fillScreen(TFT_BLACK);
    } else {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
}

void msx_video_init_palette_pairs(const uint16_t* palette, uint16_t paletteEntries)
{
    if (!palette) {
        return;
    }

    const unsigned entryCount = paletteEntries == 0u
                                    ? 16u
                                    : (paletteEntries > 256u ? 256u : paletteEntries);

    for (unsigned i = 0; i < 256u; ++i) {
        s_palette444[i] = 0u;
    }
    for (unsigned i = 0; i < entryCount; ++i) {
        s_palette444[i] = msx_rgb565_to_rgb444(palette[i]);
    }

    for (unsigned key = 0; key < 256u; ++key) {
        const uint8_t c0 = static_cast<uint8_t>(key & 0x0Fu);
        const uint8_t c1 = static_cast<uint8_t>((key >> 4) & 0x0Fu);
        s_palettePairs565[key] = static_cast<uint32_t>(palette[c0])
                               | (static_cast<uint32_t>(palette[c1]) << 16);
        s_palettePairs444[key] = msx_pack_rgb444_pair(s_palette444[c0], s_palette444[c1]);
    }
}

void msx_video_expand_indexed_line(const uint8_t* src,
                                   uint16_t* dst,
                                   int pixelCount,
                                   const uint16_t* palette,
                                   uint16_t paletteEntries)
{
    if (!src || !dst || pixelCount <= 0 || !palette) {
        return;
    }

    if (paletteEntries <= 16u) {
        auto* dst32 = reinterpret_cast<uint32_t*>(dst);
        int x = 0;
        for (; x + 3 < pixelCount; x += 4) {
            const uint8_t s0 = src[x];
            const uint8_t s1 = src[x + 1];
            const uint8_t s2 = src[x + 2];
            const uint8_t s3 = src[x + 3];
            dst32[x >> 1] = s_palettePairs565[(s0 & 0x0Fu) | ((s1 & 0x0Fu) << 4)];
            dst32[(x >> 1) + 1] = s_palettePairs565[(s2 & 0x0Fu) | ((s3 & 0x0Fu) << 4)];
        }
        for (; x + 1 < pixelCount; x += 2) {
            dst32[x >> 1] = s_palettePairs565[(src[x] & 0x0Fu) | ((src[x + 1] & 0x0Fu) << 4)];
        }
        if (x < pixelCount) {
            dst[x] = palette[src[x] & 0x0Fu];
        }
        return;
    }

    int x = 0;
    auto* dst32 = reinterpret_cast<uint32_t*>(dst);
    for (; x + 3 < pixelCount; x += 4) {
        dst32[x >> 1] = (static_cast<uint32_t>(palette[src[x + 1]]) << 16) | palette[src[x]];
        dst32[(x >> 1) + 1] = (static_cast<uint32_t>(palette[src[x + 3]]) << 16) | palette[src[x + 2]];
    }
    for (; x + 1 < pixelCount; x += 2) {
        dst32[x >> 1] = (static_cast<uint32_t>(palette[src[x + 1]]) << 16) | palette[src[x]];
    }
    if (x < pixelCount) {
        dst[x] = palette[src[x]];
    }
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

    const MsxInternalViewMode mode = msx_config_get_active_view_mode();
    const int targetW = msx_video_target_w();
    const int targetH = msx_video_target_h();
    const bool cropVerticalOverscan = srcH > kMsxVisibleSafeHeight;
    const unsigned effectiveSrcH = cropVerticalOverscan ? kMsxVisibleSafeHeight : srcH;
    const unsigned effectiveSrcY0 = cropVerticalOverscan ? ((srcH - kMsxVisibleSafeHeight) / 2u) : 0u;
    if (mode == MsxInternalViewMode::PixelPerfect) {
        plan->srcX0 = static_cast<int>((srcW > static_cast<unsigned>(targetW)) ? (srcW - targetW) / 2u : 0u);
        plan->srcY0 = static_cast<int>(
            effectiveSrcY0 +
            ((effectiveSrcH > static_cast<unsigned>(targetH)) ? (effectiveSrcH - targetH) / 2u : 0u)
        );
        plan->roiW = static_cast<int>((srcW > static_cast<unsigned>(targetW)) ? targetW : srcW);
        plan->roiH = static_cast<int>((effectiveSrcH > static_cast<unsigned>(targetH)) ? targetH : effectiveSrcH);
        plan->dstW = plan->roiW;
        plan->dstH = plan->roiH;
        plan->xOff = (targetW - plan->dstW) / 2;
        plan->yOff = (targetH - plan->dstH) / 2;
        plan->cropOnly = true;
        return;
    }

    if (msx_video_game_on_external() && mode == MsxInternalViewMode::FastPlus) {
        plan->srcX0 = 0;
        plan->srcY0 = static_cast<int>(effectiveSrcY0);
        plan->roiW = static_cast<int>(srcW);
        plan->roiH = static_cast<int>(effectiveSrcH);
        plan->dstW = kExternalFastPlusTargetW;
        plan->dstH = static_cast<int>(
            (static_cast<uint64_t>(plan->dstW) * static_cast<uint64_t>(effectiveSrcH) +
             (static_cast<uint64_t>(srcW) / 2u)) /
            static_cast<uint64_t>(srcW)
        );
        if (plan->dstH <= 0) {
            plan->dstH = 1;
        }
        if (plan->dstW > targetW) {
            plan->dstW = targetW;
        }
        if (plan->dstH > targetH) {
            plan->dstH = targetH;
        }
        plan->xOff = (targetW - plan->dstW) / 2;
        plan->yOff = (targetH - plan->dstH) / 2;
        plan->cropOnly = false;
        return;
    }

    if (msx_video_game_on_external()) {
        plan->srcX0 = 0;
        plan->srcY0 = static_cast<int>(effectiveSrcY0);
        plan->roiW = static_cast<int>(srcW);
        plan->roiH = static_cast<int>(effectiveSrcH);
        plan->dstW = kExternalFastTargetW;
        plan->dstH = kExternalFastTargetH;
        if (plan->dstW > targetW) {
            plan->dstW = targetW;
        }
        if (plan->dstH > targetH) {
            plan->dstH = targetH;
        }
        plan->xOff = (targetW - plan->dstW) / 2;
        plan->yOff = (targetH - plan->dstH) / 2;
        plan->cropOnly = false;
        return;
    }

    plan->srcX0 = 0;
    plan->srcY0 = static_cast<int>(effectiveSrcY0);
    plan->roiW = static_cast<int>(srcW);
    plan->roiH = static_cast<int>(effectiveSrcH);
    plan->dstH = targetH;
    plan->dstW = (targetH * kWideAspectW) / kWideAspectH;
    if (plan->dstW > targetW) {
        plan->dstW = targetW;
    }
    plan->xOff = (targetW - plan->dstW) / 2;
    plan->yOff = 0;
    plan->cropOnly = false;
}

bool msx_video_layout_changed(const MsxVideoPlan& plan, unsigned srcW, unsigned srcH)
{
    const int mode = static_cast<int>(msx_config_get_active_view_mode());
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
        if (!s_lineBuf) s_lineBuf = static_cast<uint16_t*>(malloc(static_cast<size_t>(neededLineWidth) * kBatchLines * sizeof(uint16_t)));
        s_lineCap = s_lineBuf ? neededLineWidth : 0;
    }

    if (msx_video_use_external_rgb444()) {
        const int neededBytes = ((neededLineWidth * kBatchLines + 1) / 2) * 3;
        if (neededBytes > s_lineBuf12Cap) {
            free(s_lineBuf12);
            s_lineBuf12 = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(neededBytes), MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            if (!s_lineBuf12) s_lineBuf12 = static_cast<uint8_t*>(malloc(static_cast<size_t>(neededBytes)));
            s_lineBuf12Cap = s_lineBuf12 ? neededBytes : 0;
        }
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
    if (msx_video_use_external_rgb444() && !s_lineBuf12) {
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

void msx_video_pack_rgb444_line(const uint16_t* src, int pixelCount, uint8_t* dst)
{
    if (!src || !dst || pixelCount <= 0) {
        return;
    }

    const int pairs = pixelCount / 2;
    for (int p = 0; p < pairs; ++p) {
        const uint16_t raw1 = src[p * 2];
        const uint16_t raw2 = src[p * 2 + 1];
        const uint16_t c1 = static_cast<uint16_t>((raw1 >> 8) | (raw1 << 8));
        const uint16_t c2 = static_cast<uint16_t>((raw2 >> 8) | (raw2 << 8));
        const int j = p * 3;
        dst[j]     = static_cast<uint8_t>(((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>(((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F));
        dst[j + 2] = static_cast<uint8_t>(((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F));
    }

    if (pixelCount & 1) {
        const uint16_t raw = src[pixelCount - 1];
        const uint16_t c = static_cast<uint16_t>((raw >> 8) | (raw << 8));
        const int j = pairs * 3;
        dst[j]     = static_cast<uint8_t>(((c >> 8) & 0xF0) | ((c >> 7) & 0x0F));
        dst[j + 1] = static_cast<uint8_t>((c << 3) & 0xF0);
        dst[j + 2] = 0u;
    }
}

void msx_video_begin_active_write(const MsxVideoPlan& plan)
{
    if (msx_video_game_on_external()) {
        msx_video_prepare_external_tft();
        s_extTft.startWrite();
        s_extTft.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
    } else {
        M5Cardputer.Display.startWrite();
        M5Cardputer.Display.setAddrWindow(plan.xOff, plan.yOff, plan.dstW, plan.dstH);
    }
}

void msx_video_end_active_write(void)
{
    if (msx_video_game_on_external()) {
        s_extTft.endWrite();
    } else {
        M5Cardputer.Display.endWrite();
    }
}

static void msx_video_emit_stream_line(const MsxLineStreamState& stream,
                                      const uint8_t* srcLine)
{
    if (!stream.active || !srcLine || stream.dstW <= 0 || !stream.palette) {
        return;
    }

    if (msx_video_game_on_external() && stream.useRgb444) {
        const int bytesPerLine = ((stream.dstW + 1) / 2) * 3;
        if (stream.cropOnly) {
            msx_video_pack_indexed_rgb444_line(srcLine + static_cast<size_t>(stream.srcX0),
                                               s_lineBuf12,
                                               stream.dstW,
                                               stream.paletteEntries);
        } else {
            if (!s_xmap) {
                return;
            }
            msx_video_pack_mapped_rgb444_line(srcLine,
                                              s_lineBuf12,
                                              stream.dstW,
                                              s_xmap,
                                              stream.paletteEntries);
        }
        s_extTft.pushColors(reinterpret_cast<uint16_t*>(s_lineBuf12), (bytesPerLine + 1) / 2, false);
        return;
    }

    if (stream.cropOnly) {
        const uint8_t* src = srcLine + static_cast<size_t>(stream.srcX0);
        msx_video_expand_indexed_line(src, s_lineBuf, stream.dstW, stream.palette, stream.paletteEntries);
    } else {
        const uint8_t* src = srcLine;
        if (!s_xmap) {
            return;
        }
        for (int x = 0; x < stream.dstW; ++x) {
            s_lineBuf[x] = stream.palette[src[s_xmap[x]]];
        }
    }

    if (msx_video_game_on_external()) {
        if (stream.useRgb444) {
            msx_video_pack_rgb444_line(s_lineBuf, stream.dstW, s_lineBuf12);
            const int bytesPerLine = ((stream.dstW + 1) / 2) * 3;
            s_extTft.pushColors(reinterpret_cast<uint16_t*>(s_lineBuf12), (bytesPerLine + 1) / 2, false);
        } else {
            s_extTft.pushColors(s_lineBuf, stream.dstW, false);
        }
        return;
    }

    M5Cardputer.Display.pushPixels(s_lineBuf, stream.dstW);
    taskYIELD();
}

bool msx_video_begin_line_stream_impl(const MsxDisplayFrame* frame)
{
    if (!frame || !frame->indexed8 || frame->width == 0u || frame->height == 0u || frame->pitchBytes < frame->width) {
        return false;
    }

    s_lineStream.active = false;
    if (frame->paletteEntryCount == 0u) {
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

    const uint16_t* palette = frame->palette565 ? frame->palette565 : s_palette565;
    msx_video_init_palette_pairs(palette, frame->paletteEntryCount);

    s_lineStream.active = true;
    s_lineStream.cropOnly = plan.cropOnly;
    s_lineStream.useRgb444 = msx_video_use_external_rgb444();
    s_lineStream.srcX0 = plan.srcX0;
    s_lineStream.srcY0 = plan.srcY0;
    s_lineStream.roiH = plan.roiH;
    s_lineStream.dstW = plan.dstW;
    s_lineStream.dstH = plan.dstH;
    s_lineStream.palette = palette;
    s_lineStream.paletteEntries = frame->paletteEntryCount;
    s_lineStream.nextDstY = 0;

    msx_video_begin_active_write(plan);
    return true;
}

bool msx_video_stream_line_impl(const MsxDisplayFrame* frame, const uint8_t* srcLine, unsigned srcLineIndex)
{
    if (!s_lineStream.active || !frame || !srcLine || frame->width == 0u) {
        return false;
    }

    if (frame->pitchBytes < frame->width || srcLineIndex >= frame->height) {
        return false;
    }

    bool emitted = false;
    if (s_lineStream.cropOnly) {
        const int dstLine = static_cast<int>(srcLineIndex) - s_lineStream.srcY0;
        if (dstLine >= 0 && dstLine < s_lineStream.roiH) {
            msx_video_emit_stream_line(s_lineStream, srcLine);
            emitted = true;
        }
        return emitted;
    }

    if (!s_ymap) {
        return false;
    }

    const int16_t mappedSrcLine = static_cast<int16_t>(srcLineIndex);
    if (s_lineStream.nextDstY > 0 &&
        mappedSrcLine < s_ymap[s_lineStream.nextDstY - 1]) {
        for (int y = 0; y < s_lineStream.dstH; ++y) {
            if (s_ymap[y] == mappedSrcLine) {
                msx_video_emit_stream_line(s_lineStream, srcLine);
                emitted = true;
            }
        }
        return emitted;
    }

    while (s_lineStream.nextDstY < s_lineStream.dstH &&
           s_ymap[s_lineStream.nextDstY] < mappedSrcLine) {
        ++s_lineStream.nextDstY;
    }
    while (s_lineStream.nextDstY < s_lineStream.dstH &&
           s_ymap[s_lineStream.nextDstY] == mappedSrcLine) {
        msx_video_emit_stream_line(s_lineStream, srcLine);
        emitted = true;
        ++s_lineStream.nextDstY;
    }

    return emitted;
}

void msx_video_end_line_stream_impl(void)
{
    if (!s_lineStream.active) {
        return;
    }

    msx_video_end_active_write();
    s_lineStream = {};
}

void msx_video_draw_crop_frame(const MsxDisplayFrame* frame, const MsxVideoPlan& plan)
{
    const uint16_t* palette = frame->palette565 ? frame->palette565 : s_palette565;
    const bool useRgb444 = msx_video_use_external_rgb444();
    msx_video_init_palette_pairs(palette, frame->paletteEntryCount);

    if (msx_video_game_on_external()) {
        msx_video_begin_active_write(plan);
        if (useRgb444) {
            const int bytesPerLine = ((plan.dstW + 1) / 2) * 3;
            for (int y = 0; y < plan.dstH; y += kBatchLines) {
                const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
                for (int row = 0; row < batch; ++row) {
                    const uint8_t* src = frame->indexed8
                        + static_cast<size_t>(plan.srcY0 + y + row) * frame->pitchBytes
                        + static_cast<size_t>(plan.srcX0);
                    uint8_t* dst = s_lineBuf12 + static_cast<size_t>(row) * bytesPerLine;
                    msx_video_pack_indexed_rgb444_line(src, dst, plan.dstW, frame->paletteEntryCount);
                }
                s_extTft.pushColors(reinterpret_cast<uint16_t*>(s_lineBuf12), (bytesPerLine * batch + 1) / 2, false);
            }
        } else {
            for (int y = 0; y < plan.dstH; y += kBatchLines) {
                const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
                for (int row = 0; row < batch; ++row) {
                    const uint8_t* src = frame->indexed8
                        + static_cast<size_t>(plan.srcY0 + y + row) * frame->pitchBytes
                        + static_cast<size_t>(plan.srcX0);
                    uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
                    msx_video_expand_indexed_line(src, dst, plan.dstW, palette, frame->paletteEntryCount);
                }
                s_extTft.pushColors(s_lineBuf, plan.dstW * batch, false);
            }
        }
        msx_video_end_active_write();
        return;
    }

    msx_video_begin_active_write(plan);
    for (int y = 0; y < plan.dstH; y += kBatchLines) {
        const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
        for (int row = 0; row < batch; ++row) {
            const uint8_t* src = frame->indexed8
                + static_cast<size_t>(plan.srcY0 + y + row) * frame->pitchBytes
                + static_cast<size_t>(plan.srcX0);
            uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
            msx_video_expand_indexed_line(src, dst, plan.dstW, palette, frame->paletteEntryCount);
        }
        M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        taskYIELD();
    }
    msx_video_end_active_write();
}

void msx_video_draw_scaled_frame(const MsxDisplayFrame* frame, const MsxVideoPlan& plan)
{
    const uint16_t* palette = frame->palette565 ? frame->palette565 : s_palette565;
    const bool useRgb444 = msx_video_use_external_rgb444();
    msx_video_init_palette_pairs(palette, frame->paletteEntryCount);

    if (msx_video_game_on_external()) {
        msx_video_begin_active_write(plan);
        if (useRgb444) {
            const int bytesPerLine = ((plan.dstW + 1) / 2) * 3;
            for (int y = 0; y < plan.dstH; y += kBatchLines) {
                const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
                for (int row = 0; row < batch; ++row) {
                    const uint8_t* src = frame->indexed8 + static_cast<size_t>(s_ymap[y + row]) * frame->pitchBytes;
                    uint8_t* dst = s_lineBuf12 + static_cast<size_t>(row) * bytesPerLine;
                    msx_video_pack_mapped_rgb444_line(src,
                                                      dst,
                                                      plan.dstW,
                                                      s_xmap,
                                                      frame->paletteEntryCount);
                }
                s_extTft.pushColors(reinterpret_cast<uint16_t*>(s_lineBuf12), (bytesPerLine * batch + 1) / 2, false);
            }
        } else {
            for (int y = 0; y < plan.dstH; y += kBatchLines) {
                const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
                for (int row = 0; row < batch; ++row) {
                    const uint8_t* src = frame->indexed8 + static_cast<size_t>(s_ymap[y + row]) * frame->pitchBytes;
                    uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
                    int x = 0;
                    for (; x + 3 < plan.dstW; x += 4) {
                        dst[x] = palette[src[s_xmap[x]]];
                        dst[x + 1] = palette[src[s_xmap[x + 1]]];
                        dst[x + 2] = palette[src[s_xmap[x + 2]]];
                        dst[x + 3] = palette[src[s_xmap[x + 3]]];
                    }
                    for (; x < plan.dstW; ++x) {
                        dst[x] = palette[src[s_xmap[x]]];
                    }
                }
                s_extTft.pushColors(s_lineBuf, plan.dstW * batch, false);
            }
        }
        msx_video_end_active_write();
        return;
    }

    msx_video_begin_active_write(plan);
    for (int y = 0; y < plan.dstH; y += kBatchLines) {
        const int batch = (y + kBatchLines <= plan.dstH) ? kBatchLines : (plan.dstH - y);
        for (int row = 0; row < batch; ++row) {
            const uint8_t* src = frame->indexed8 + static_cast<size_t>(s_ymap[y + row]) * frame->pitchBytes;
            uint16_t* dst = s_lineBuf + static_cast<size_t>(row) * plan.dstW;
            int x = 0;
            for (; x + 3 < plan.dstW; x += 4) {
                dst[x] = palette[src[s_xmap[x]]];
                dst[x + 1] = palette[src[s_xmap[x + 1]]];
                dst[x + 2] = palette[src[s_xmap[x + 2]]];
                dst[x + 3] = palette[src[s_xmap[x + 3]]];
            }
            for (; x < plan.dstW; ++x) {
                dst[x] = palette[src[s_xmap[x]]];
            }
        }
        M5Cardputer.Display.pushPixels(s_lineBuf, plan.dstW * batch);
        taskYIELD();
    }
    msx_video_end_active_write();
}

bool msx_video_render_frame_now(const MsxDisplayFrame* frame)
{
    if (!frame || !frame->indexed8 || frame->width == 0 || frame->height == 0 || frame->pitchBytes < frame->width) {
        std::printf("[MSX][VIDEO] present_frame SKIP: frame=%p i8=%p w=%u h=%u pitch=%u\n",
                    static_cast<const void*>(frame),
                    frame ? static_cast<const void*>(frame->indexed8) : nullptr,
                    frame ? frame->width : 0u,
                    frame ? frame->height : 0u,
                    frame ? static_cast<unsigned>(frame->pitchBytes) : 0u);
        return false;
    }

    MsxVideoPlan plan = {};
    msx_video_compute_plan(frame->width, frame->height, &plan);
    const bool layoutChanged = msx_video_layout_changed(plan, frame->width, frame->height);

    if (!s_firstPresentLogged) {
        s_firstPresentLogged = true;
        std::printf("[MSX][VIDEO] first present: w=%u h=%u pitch=%u dstW=%d dstH=%d xOff=%d yOff=%d crop=%d\n",
                    frame->width, frame->height,
                    static_cast<unsigned>(frame->pitchBytes),
                    plan.dstW, plan.dstH, plan.xOff, plan.yOff,
                    static_cast<int>(plan.cropOnly));
    }

    if (!msx_video_prepare_buffers(plan, layoutChanged)) {
        std::printf("[MSX][VIDEO] prepare_buffers FAILED dstW=%d dstH=%d\n", plan.dstW, plan.dstH);
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

} // namespace

static uint32_t s_videoPerfOverBudgetFrames = 0;
static uint32_t s_videoPerfOverHalfRateFrames = 0;
static uint32_t s_videoPerfWorstUs = 0;
static uint32_t s_videoPerfPresentFails = 0;
static uint32_t s_videoPerfWindowFrames = 0;
static uint32_t s_videoPerfSkippedFrames = 0;
static uint16_t s_autoFrameskipStep256 = 0;
static uint16_t s_autoFrameskipAccum256 = 0;

bool msx_video_begin_line_stream(const MsxDisplayFrame* frame)
{
    return msx_video_begin_line_stream_impl(frame);
}

bool msx_video_stream_line(const MsxDisplayFrame* frame, const uint8_t* srcLine, unsigned srcLineIndex)
{
    return msx_video_stream_line_impl(frame, srcLine, srcLineIndex);
}

void msx_video_end_line_stream(void)
{
    msx_video_end_line_stream_impl();
}

void msx_video_init(void)
{
    if (!s_videoMutex) {
        s_videoMutex = xSemaphoreCreateMutex();
    }
    msx_video_init_palette();
    if (msx_video_game_on_external()) {
        msx_video_prepare_external_tft();
    }
    s_firstPresentLogged = false;
    s_spiPushUs = 0;
    s_spiPushFrames = 0;
    s_videoPerfOverBudgetFrames = 0;
    s_videoPerfOverHalfRateFrames = 0;
    s_videoPerfWorstUs = 0;
    s_videoPerfPresentFails = 0;
    s_videoPerfWindowFrames = 0;
    s_videoPerfSkippedFrames = 0;
    s_autoFrameskipStep256 = 0;
    s_autoFrameskipAccum256 = 0;
    s_lastPresentUs = 0;
    s_externalFrameskipState = false;
    msx_video_reset_layout_cache();
    msx_video_clear_target();
}

void msx_video_shutdown(void)
{
    msx_video_release_scratch_buffers();
    s_extTftColorModeKnown = false;
    s_externalUiActive = false;
    s_stateOverlayActive = false;
    s_lastPresentUs = 0;
    msx_video_reset_layout_cache();
}

void msx_video_lock(void)
{
    if (s_videoMutex) xSemaphoreTake(s_videoMutex, portMAX_DELAY);
}

void msx_video_unlock(void)
{
    if (s_videoMutex) xSemaphoreGive(s_videoMutex);
}

void msx_video_prepare_external_ui(void)
{
    if (msx_video_game_on_external() && !s_externalUiActive) {
        s_externalUiActive = true;
    }

    if (!s_extTftPrepared) {
        s_extTft.begin();
        s_extTft.setRotation(3);
        s_extTft.setTextWrap(false);
        s_extTftPrepared = true;
        if (!s_extTftClockLogged) {
            std::printf("[MSX][VIDEO] external SPI write=%u read=%u\n",
                        static_cast<unsigned>(SPI_FREQUENCY),
                        static_cast<unsigned>(SPI_READ_FREQUENCY));
            s_extTftClockLogged = true;
        }
    }

    if (!s_extTftColorModeKnown || s_extTftRgb444Configured) {
        s_extTft.startWrite();
        s_extTft.writecommand(0x3A);
        s_extTft.writedata(0x55);
        s_extTft.endWrite();
        s_extTftRgb444Configured = false;
        s_extTftColorModeKnown = true;
        s_extTftColorModeLogged = false;
    }
}

namespace {

void msx_video_pack_indexed_rgb444_line(const uint8_t* src,
                                        uint8_t* dst,
                                        int pixelCount,
                                        uint16_t paletteEntries)
{
    if (!src || !dst || pixelCount <= 0) {
        return;
    }

    int x = 0;
    uint8_t* out = dst;

    if (paletteEntries <= 16u) {
        for (; x + 3 < pixelCount; x += 4) {
            const uint8_t s0 = src[x];
            const uint8_t s1 = src[x + 1];
            const uint8_t s2 = src[x + 2];
            const uint8_t s3 = src[x + 3];
            const uint32_t p0 = s_palettePairs444[(s0 & 0x0Fu) | ((s1 & 0x0Fu) << 4)];
            const uint32_t p1 = s_palettePairs444[(s2 & 0x0Fu) | ((s3 & 0x0Fu) << 4)];
            out[0] = static_cast<uint8_t>(p0);
            out[1] = static_cast<uint8_t>(p0 >> 8);
            out[2] = static_cast<uint8_t>(p0 >> 16);
            out[3] = static_cast<uint8_t>(p1);
            out[4] = static_cast<uint8_t>(p1 >> 8);
            out[5] = static_cast<uint8_t>(p1 >> 16);
            out += 6;
        }
        if (x + 1 < pixelCount) {
            const uint32_t p0 = s_palettePairs444[(src[x] & 0x0Fu) | ((src[x + 1] & 0x0Fu) << 4)];
            out[0] = static_cast<uint8_t>(p0);
            out[1] = static_cast<uint8_t>(p0 >> 8);
            out[2] = static_cast<uint8_t>(p0 >> 16);
            x += 2;
            out += 3;
        }
        if (x < pixelCount) {
            const uint16_t color = s_palette444[src[x] & 0x0Fu];
            out[0] = static_cast<uint8_t>((color >> 4) & 0xFFu);
            out[1] = static_cast<uint8_t>((color & 0x0Fu) << 4);
            out[2] = 0u;
        }
        return;
    }

    for (; x + 3 < pixelCount; x += 4) {
        const uint32_t p0 = msx_pack_rgb444_pair(s_palette444[src[x]], s_palette444[src[x + 1]]);
        const uint32_t p1 = msx_pack_rgb444_pair(s_palette444[src[x + 2]], s_palette444[src[x + 3]]);
        out[0] = static_cast<uint8_t>(p0);
        out[1] = static_cast<uint8_t>(p0 >> 8);
        out[2] = static_cast<uint8_t>(p0 >> 16);
        out[3] = static_cast<uint8_t>(p1);
        out[4] = static_cast<uint8_t>(p1 >> 8);
        out[5] = static_cast<uint8_t>(p1 >> 16);
        out += 6;
    }
    if (x + 1 < pixelCount) {
        const uint32_t p0 = msx_pack_rgb444_pair(s_palette444[src[x]], s_palette444[src[x + 1]]);
        out[0] = static_cast<uint8_t>(p0);
        out[1] = static_cast<uint8_t>(p0 >> 8);
        out[2] = static_cast<uint8_t>(p0 >> 16);
        x += 2;
        out += 3;
    }
    if (x < pixelCount) {
        const uint16_t color = s_palette444[src[x]];
        out[0] = static_cast<uint8_t>((color >> 4) & 0xFFu);
        out[1] = static_cast<uint8_t>((color & 0x0Fu) << 4);
        out[2] = 0u;
    }
}

void msx_video_pack_mapped_rgb444_line(const uint8_t* src,
                                       uint8_t* dst,
                                       int pixelCount,
                                       const int16_t* xmap,
                                       uint16_t paletteEntries)
{
    if (!src || !dst || pixelCount <= 0 || !xmap) {
        return;
    }

    int x = 0;
    uint8_t* out = dst;

    if (paletteEntries <= 16u) {
        for (; x + 3 < pixelCount; x += 4) {
            const uint8_t key0 = static_cast<uint8_t>((src[xmap[x]] & 0x0Fu) |
                                                      ((src[xmap[x + 1]] & 0x0Fu) << 4));
            const uint8_t key1 = static_cast<uint8_t>((src[xmap[x + 2]] & 0x0Fu) |
                                                      ((src[xmap[x + 3]] & 0x0Fu) << 4));
            msx_store_rgb444_pair(out, s_palettePairs444[key0]);
            msx_store_rgb444_pair(out, s_palettePairs444[key1]);
        }
        if (x + 1 < pixelCount) {
            const uint8_t key = static_cast<uint8_t>((src[xmap[x]] & 0x0Fu) |
                                                     ((src[xmap[x + 1]] & 0x0Fu) << 4));
            msx_store_rgb444_pair(out, s_palettePairs444[key]);
            x += 2;
        }
        if (x < pixelCount) {
            const uint16_t color = s_palette444[src[xmap[x]] & 0x0Fu];
            out[0] = static_cast<uint8_t>((color >> 4) & 0xFFu);
            out[1] = static_cast<uint8_t>((color & 0x0Fu) << 4);
            out[2] = 0u;
        }
        return;
    }

    for (; x + 3 < pixelCount; x += 4) {
        const uint32_t p0 = msx_pack_rgb444_pair(s_palette444[src[xmap[x]]], s_palette444[src[xmap[x + 1]]]);
        const uint32_t p1 = msx_pack_rgb444_pair(s_palette444[src[xmap[x + 2]]], s_palette444[src[xmap[x + 3]]]);
        out[0] = static_cast<uint8_t>(p0);
        out[1] = static_cast<uint8_t>(p0 >> 8);
        out[2] = static_cast<uint8_t>(p0 >> 16);
        out[3] = static_cast<uint8_t>(p1);
        out[4] = static_cast<uint8_t>(p1 >> 8);
        out[5] = static_cast<uint8_t>(p1 >> 16);
        out += 6;
    }
    if (x + 1 < pixelCount) {
        const uint32_t p0 = msx_pack_rgb444_pair(s_palette444[src[xmap[x]]], s_palette444[src[xmap[x + 1]]]);
        out[0] = static_cast<uint8_t>(p0);
        out[1] = static_cast<uint8_t>(p0 >> 8);
        out[2] = static_cast<uint8_t>(p0 >> 16);
        x += 2;
        out += 3;
    }
    if (x < pixelCount) {
        const uint16_t color = s_palette444[src[xmap[x]]];
        out[0] = static_cast<uint8_t>((color >> 4) & 0xFFu);
        out[1] = static_cast<uint8_t>((color & 0x0Fu) << 4);
        out[2] = 0u;
    }
}

} // namespace

void msx_video_finish_external_ui(void)
{
    if (!s_externalUiActive) {
        return;
    }

    s_externalUiActive = false;
    s_extTftColorModeKnown = false;
}

TFT_eSPI& msx_video_external_tft(void)
{
    if (!s_extTftPrepared) {
        if (s_externalUiActive) {
            msx_video_prepare_external_ui();
        } else {
            msx_video_prepare_external_tft();
        }
    }
    return s_extTft;
}

void msx_video_set_runtime_menu_active(bool active)
{
    s_runtimeMenuActive = active;
}

void msx_video_set_state_overlay_active(bool active)
{
    s_stateOverlayActive = active;
}

void msx_video_prepare_sd_access(void)
{
    msx_video_lock();

    if (s_extTftPrepared) {
        s_extTft.endWrite();
#if defined(TFT_CS) && (TFT_CS >= 0)
        pinMode(TFT_CS, OUTPUT);
        digitalWrite(TFT_CS, HIGH);
#endif
        s_extTftPrepared = false;
        s_extTftColorModeKnown = false;
        s_extTftRgb444Configured = false;
        s_extTftColorModeLogged = false;
        s_externalUiActive = false;
    }

    msx_video_release_scratch_buffers();
    s_firstPresentLogged = false;
    s_spiPushUs = 0;
    s_spiPushFrames = 0;
    s_videoPerfOverBudgetFrames = 0;
    s_videoPerfOverHalfRateFrames = 0;
    s_videoPerfWorstUs = 0;
    s_videoPerfPresentFails = 0;
    s_videoPerfWindowFrames = 0;
    s_videoPerfSkippedFrames = 0;
    s_autoFrameskipStep256 = 0;
    s_autoFrameskipAccum256 = 0;
    s_lastPresentUs = 0;
    s_externalFrameskipState = false;

    msx_video_unlock();
}

bool msx_video_present_frame(const MsxDisplayFrame* frame)
{
    constexpr uint32_t kFrameBudgetUs = 16667u;
    constexpr uint32_t kHalfRateBudgetUs = 33333u;
    constexpr uint32_t kFrameskipDeadbandUs = 17500u;

    msx_video_lock();
    s_lastPresentUs = 0;

    if (s_runtimeMenuActive || s_stateOverlayActive) {
        msx_video_unlock();
        return false;
    }

    if (msx_video_game_on_external()) {
        if (s_externalUiActive) {
            s_externalUiActive = false;
            s_extTftColorModeKnown = false;
        }
    }

    bool skipPresent = false;
    if (msx_video_game_on_external()) {
        s_externalFrameskipState = !s_externalFrameskipState;
        skipPresent = s_externalFrameskipState;
    } else {
        if (s_autoFrameskipStep256 != 0u) {
            s_autoFrameskipAccum256 = static_cast<uint16_t>(s_autoFrameskipAccum256 + s_autoFrameskipStep256);
            if (s_autoFrameskipAccum256 >= 256u) {
                s_autoFrameskipAccum256 = static_cast<uint16_t>(s_autoFrameskipAccum256 - 256u);
                skipPresent = true;
            }
        }
    }

    bool result = true;
    uint32_t frameUs = 0;
    if (!skipPresent) {
        int64_t t0 = esp_timer_get_time();
        result = msx_video_render_frame_now(frame);
        int64_t t1 = esp_timer_get_time();
        frameUs = static_cast<uint32_t>(t1 - t0);
    }

    if (result) {
        s_videoPerfWindowFrames++;
        if (skipPresent) {
            s_videoPerfSkippedFrames++;
        } else {
            s_lastPresentUs = frameUs;
            s_spiPushUs += frameUs;
            s_spiPushFrames++;
            if (frameUs > s_videoPerfWorstUs) {
                s_videoPerfWorstUs = frameUs;
            }
            if (frameUs > kFrameBudgetUs) {
                s_videoPerfOverBudgetFrames++;
            }
            if (frameUs > kHalfRateBudgetUs) {
                s_videoPerfOverHalfRateFrames++;
            }
        }
        if (s_videoPerfWindowFrames >= 60u) {
            const uint32_t avgUs = s_spiPushFrames
                                     ? static_cast<uint32_t>(s_spiPushUs / s_spiPushFrames)
                                     : 0u;
            const uint32_t fps10 = avgUs ? static_cast<uint32_t>((10000000ull + (avgUs / 2u)) / avgUs) : 0u;
            if (avgUs > kFrameskipDeadbandUs) {
                const uint32_t keep256 = static_cast<uint32_t>((static_cast<uint64_t>(kFrameBudgetUs) * 256u) / avgUs);
                s_autoFrameskipStep256 = static_cast<uint16_t>(keep256 >= 256u ? 0u : (256u - keep256));
            } else {
                s_autoFrameskipStep256 = 0u;
                s_autoFrameskipAccum256 = 0u;
            }
            const uint32_t frameskipPct = static_cast<uint32_t>((static_cast<uint32_t>(s_autoFrameskipStep256) * 100u + 128u) / 256u);
            s_videoPerfSummary.valid = true;
            s_videoPerfSummary.windowFrames = s_videoPerfWindowFrames;
            s_videoPerfSummary.pushedFrames = s_spiPushFrames;
            s_videoPerfSummary.avgPresentUs = avgUs;
            s_videoPerfSummary.worstPresentUs = s_videoPerfWorstUs;
            s_videoPerfSummary.overBudgetFrames = s_videoPerfOverBudgetFrames;
            s_videoPerfSummary.overHalfRateFrames = s_videoPerfOverHalfRateFrames;
            s_videoPerfSummary.presentFails = s_videoPerfPresentFails;
            s_videoPerfSummary.skippedFrames = s_videoPerfSkippedFrames;
            s_videoPerfSummary.frameskipPercent = static_cast<uint16_t>(frameskipPct);
#if MSX_VIDEO_PERF_LOG_ENABLED
            std::printf("[MSX][VIDEO-PERF] 60f avg=%u us est=%u.%u fps >16.7ms=%u >33.3ms=%u worst=%u us fail=%u skip=%u fs=%u%%\n",
                        static_cast<unsigned>(avgUs),
                        static_cast<unsigned>(fps10 / 10u),
                        static_cast<unsigned>(fps10 % 10u),
                        static_cast<unsigned>(s_videoPerfOverBudgetFrames),
                        static_cast<unsigned>(s_videoPerfOverHalfRateFrames),
                        static_cast<unsigned>(s_videoPerfWorstUs),
                        static_cast<unsigned>(s_videoPerfPresentFails),
                        static_cast<unsigned>(s_videoPerfSkippedFrames),
                        static_cast<unsigned>(frameskipPct));
#endif
            s_videoPerfWindowFrames = 0;
            s_videoPerfSkippedFrames = 0;
            s_spiPushFrames = 0;
            s_spiPushUs = 0;
            s_videoPerfOverBudgetFrames = 0;
            s_videoPerfOverHalfRateFrames = 0;
            s_videoPerfWorstUs = 0;
            s_videoPerfPresentFails = 0;
        }
    } else {
        s_videoPerfPresentFails++;
        if (s_videoPerfPresentFails <= 4u || (s_videoPerfPresentFails % 30u) == 0u) {
            std::printf("[MSX][VIDEO-PERF] present failed #%u render=%u us frame=%p w=%u h=%u pitch=%u\n",
                        static_cast<unsigned>(s_videoPerfPresentFails),
                        static_cast<unsigned>(frameUs),
                        static_cast<const void*>(frame),
                        frame ? frame->width : 0u,
                        frame ? frame->height : 0u,
                        frame ? static_cast<unsigned>(frame->pitchBytes) : 0u);
        }
    }

    msx_video_unlock();
    return result;
}

MsxVideoPerfSummary msx_video_get_perf_summary(void)
{
    return s_videoPerfSummary;
}

uint32_t msx_video_get_last_present_us(void)
{
    return s_lastPresentUs;
}

void msx_video_clear_last_present_us(void)
{
    s_lastPresentUs = 0u;
}

void msx_video_request_full_redraw(void)
{
    msx_video_lock();
    msx_video_reset_layout_cache();
    msx_video_unlock();
}
