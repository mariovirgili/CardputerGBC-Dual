#include "coleco_vdp.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

#include "../coleco_display.h"
#include "../coleco_video.h"
#include "../../share/emu_static_pool.h"

#ifndef COLECO_VDP_TRACE_ENABLED
#define COLECO_VDP_TRACE_ENABLED 0
#endif

static void coleco_vdp_render_internal(ColecoVdpState* state);

namespace {

constexpr size_t kColeco1VramSize = 0x4000;
constexpr size_t kColeco2VramSize = 0x10000;
constexpr unsigned kColecoFrameWidth = 256;
constexpr unsigned kColecoFrameHeightColeco1 = 192;
constexpr unsigned kColecoFrameHeightColeco2 = 212;
constexpr unsigned kColecoSpriteColorLineWidth = kColecoFrameWidth + 64u;
constexpr size_t kColecoFramePixels = static_cast<size_t>(kColecoFrameWidth) * kColecoFrameHeightColeco2;
static_assert(EMU_STATIC_POOL_SIZE >= kColecoFramePixels, "Coleco framebuffer must fit in shared emulator pool");
constexpr uint8_t kColecoMaxSpritesLineColeco1 = 4u;
constexpr uint8_t kColecoMaxSpritesLineColeco2 = 8u;
constexpr uint8_t kColecoVdpRegsInit[64] = {
    0x00, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kColecoVdpStatusInit[10] = {
    0x9F, 0x00, 0x6C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// The Cardputer runs one emulator at a time. Reuse the shared pool for the
// indexed framebuffer so Coleco does not depend on a large contiguous heap block.
static uint8_t* s_msxFrameBuffer = nullptr;
static uint8_t* s_msx1Vram = nullptr;
static uint8_t* s_msx1VramB = nullptr;
static uint8_t s_msxSpriteOccupancy[kColecoFrameWidth];
static uint8_t s_msxColorSpriteLine[kColecoSpriteColorLineWidth];
static uint32_t s_msxPatternNibbleExpand[256][16];
static bool s_msxPatternNibbleExpandReady = false;

struct ColecoVdpTableMasks {
    uint8_t r2;
    uint8_t r3;
    uint8_t r4;
    uint8_t r5;
    uint8_t m2;
    uint8_t m3;
    uint8_t m4;
    uint8_t m5;
    uint8_t nameShift;
};

bool coleco_vdp_register_affects_output(uint8_t reg);

static TaskHandle_t s_vdpRenderTask = nullptr;
static SemaphoreHandle_t s_vdpRenderSem = nullptr;
static ColecoVdpState s_vdpStateSnapshot;
static ColecoVdpState* s_vdpOriginalState = nullptr;
static bool s_vdpTaskRunning = false;

static uint32_t s_vdpStatFrames = 0;
static uint32_t s_vdpStatRenderUs = 0;
static uint32_t s_vdpStatCopyUs = 0;
static uint32_t s_vdpStatDrops = 0;

static void coleco_vdp_render_task(void* arg) {
    while (s_vdpTaskRunning) {
        if (xSemaphoreTake(s_vdpRenderSem, portMAX_DELAY) == pdTRUE) {
            if (!s_vdpTaskRunning) break;
            int64_t t0 = esp_timer_get_time();
            coleco_vdp_render_internal(&s_vdpStateSnapshot);
            int64_t t1 = esp_timer_get_time();
            s_vdpStatRenderUs += static_cast<uint32_t>(t1 - t0);
            s_vdpStatFrames++;
            if (s_vdpStatFrames >= 60) {
                std::printf("[MSX][VDP-CORE0] 60fps | RenderAvg: %u us | CopyAvg: %u us | Drops: %u\n",
                            static_cast<unsigned>(s_vdpStatRenderUs / 60u),
                            static_cast<unsigned>(s_vdpStatCopyUs / 60u),
                            static_cast<unsigned>(s_vdpStatDrops));
                s_vdpStatFrames = 0;
                s_vdpStatRenderUs = 0;
                s_vdpStatCopyUs = 0;
                s_vdpStatDrops = 0;
            }
            
            ColecoDisplayFrame frame = {};
            coleco_vdp_get_display_frame(&s_vdpStateSnapshot, &frame);
            coleco_video_present_frame(&frame);
        }
    }
    vTaskDelete(nullptr);
}

constexpr uint16_t coleco_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>((r & 0xF8u) | (g >> 5) | ((g & 0x1Cu) << 11) | ((b & 0xF8u) << 5));
}

bool coleco_vdp_is_msx2(const ColecoVdpState* state)
{
    return state && state->machineMode == ColecoMachineMode::MSX2;
}

const ColecoVdpTableMasks* coleco_vdp_table_masks(ColecoVdpMode mode)
{
    static constexpr ColecoVdpTableMasks kText40   = {0x7F, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr ColecoVdpTableMasks kGraphics1 = {0x7F, 0xFF, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr ColecoVdpTableMasks kGraphics2 = {0x7F, 0x80, 0x3C, 0xFF, 0x00, 0x7F, 0x03, 0x00, 10};
    static constexpr ColecoVdpTableMasks kMulticolor = {0x7F, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr ColecoVdpTableMasks kGraphics3 = {0x7F, 0x80, 0x3C, 0xFC, 0x00, 0x7F, 0x03, 0x03, 10};
    static constexpr ColecoVdpTableMasks kBitmap4  = {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 10};
    static constexpr ColecoVdpTableMasks kBitmap6  = {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 11};
    static constexpr ColecoVdpTableMasks kBitmap7  = {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 11};
    static constexpr ColecoVdpTableMasks kBitmap8  = {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 11};
    static constexpr ColecoVdpTableMasks kText80   = {0x7C, 0xF8, 0x3F, 0x00, 0x03, 0x07, 0x00, 0x00, 10};

    switch (mode) {
        case ColecoVdpMode::Text40:
            return &kText40;
        case ColecoVdpMode::Graphics1:
            return &kGraphics1;
        case ColecoVdpMode::Graphics2:
            return &kGraphics2;
        case ColecoVdpMode::Multicolor:
            return &kMulticolor;
        case ColecoVdpMode::Graphics3:
            return &kGraphics3;
        case ColecoVdpMode::Bitmap4:
            return &kBitmap4;
        case ColecoVdpMode::Bitmap6:
            return &kBitmap6;
        case ColecoVdpMode::Bitmap7:
            return &kBitmap7;
        case ColecoVdpMode::Bitmap8:
            return &kBitmap8;
        case ColecoVdpMode::Text80:
            return &kText80;
        case ColecoVdpMode::Unsupported:
        default:
            return nullptr;
    }
}

inline uint8_t coleco_vdp_expand3(uint8_t value)
{
    value &= 0x07u;
    return static_cast<uint8_t>((value << 5) | (value << 2) | (value >> 1));
}

inline uint8_t coleco_vdp_read_vram_fast(const uint8_t* vram, uint32_t mask, uint32_t address)
{
    return vram[address & mask];
}

} // namespace

void coleco_vdp_apply_palette_entry(ColecoVdpState* state, uint8_t index)
{
    if (!state || index >= 16u) {
        return;
    }

    const uint8_t raw1 = state->paletteRaw[index][0];
    const uint8_t raw2 = state->paletteRaw[index][1];
    const uint8_t red = coleco_vdp_expand3(static_cast<uint8_t>((raw1 >> 4) & 0x07u));
    const uint8_t blue = coleco_vdp_expand3(static_cast<uint8_t>(raw1 & 0x07u));
    const uint8_t green = coleco_vdp_expand3(static_cast<uint8_t>(raw2 & 0x07u));
    state->palette565[index] = coleco_rgb565(red, green, blue);
}

namespace {

void coleco_vdp_init_palette(ColecoVdpState* state)
{
    static constexpr uint8_t kColeco1Palette[16][2] = {
        {0x00, 0x00}, {0x00, 0x00}, {0x11, 0x05}, {0x33, 0x06},
        {0x26, 0x02}, {0x37, 0x03}, {0x52, 0x02}, {0x27, 0x06},
        {0x62, 0x02}, {0x63, 0x03}, {0x52, 0x05}, {0x63, 0x06},
        {0x11, 0x04}, {0x55, 0x02}, {0x55, 0x05}, {0x77, 0x07},
    };

    for (uint8_t i = 0; i < 16u; ++i) {
        state->paletteRaw[i][0] = kColeco1Palette[i][0];
        state->paletteRaw[i][1] = kColeco1Palette[i][1];
        coleco_vdp_apply_palette_entry(state, i);
    }

    for (unsigned i = 0; i < 256u; ++i) {
        const uint8_t r = static_cast<uint8_t>(((i >> 2) & 0x07u) * 255u / 7u);
        const uint8_t g = static_cast<uint8_t>(((i >> 5) & 0x07u) * 255u / 7u);
        uint8_t b = static_cast<uint8_t>(i & 0x03u);
        b = (b == 0u) ? 0u : (b == 1u ? 73u : (b == 2u ? 146u : 255u));
        state->screen8Palette565[i] = coleco_rgb565(r, g, b);
    }
}

void coleco_vdp_init_pattern_expand_table(void)
{
    if (s_msxPatternNibbleExpandReady) {
        return;
    }

    for (unsigned colorKey = 0; colorKey < 256u; ++colorKey) {
        const uint8_t fg = static_cast<uint8_t>((colorKey >> 4) & 0x0Fu);
        const uint8_t bg = static_cast<uint8_t>(colorKey & 0x0Fu);
        for (unsigned nibble = 0; nibble < 16u; ++nibble) {
            uint32_t packed = 0u;
            for (unsigned bit = 0; bit < 4u; ++bit) {
                const uint8_t color = (nibble & (0x08u >> bit)) != 0u ? fg : bg;
                packed |= static_cast<uint32_t>(color) << (bit * 8u);
            }
            s_msxPatternNibbleExpand[colorKey][nibble] = packed;
        }
    }

    s_msxPatternNibbleExpandReady = true;
}

inline void coleco_vdp_write_pattern_pixels(uint8_t* dst, uint8_t pattern, uint8_t fg, uint8_t bg)
{
    const uint32_t colorKey = (static_cast<uint32_t>(fg) << 4) | static_cast<uint32_t>(bg);
    auto* dst32 = reinterpret_cast<uint32_t*>(dst);
    dst32[0] = s_msxPatternNibbleExpand[colorKey][pattern >> 4];
    dst32[1] = s_msxPatternNibbleExpand[colorKey][pattern & 0x0Fu];
}

inline uint8_t coleco_vdp_reg_backdrop(const ColecoVdpState* state)
{
    return state ? static_cast<uint8_t>(state->regs[7] & 0x0Fu) : 1u;
}

inline uint8_t coleco_vdp_resolve_color(const ColecoVdpState* state, uint8_t color)
{
    const uint8_t backdrop = coleco_vdp_reg_backdrop(state);
    if ((color & 0x0Fu) == 0u) {
        return backdrop == 0u ? 1u : backdrop;
    }
    return static_cast<uint8_t>(color & 0x0Fu);
}

inline void coleco_vdp_write_vram(ColecoVdpState* state, uint32_t address, uint8_t value)
{
    const uint32_t wrapped = address & state->vramMask;
    if (state->vram[wrapped] != value) {
        state->vram[wrapped] = value;
        state->dirty = true;
    }
}

uint32_t coleco_vdp_name_base(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }
    return static_cast<uint32_t>(state->regs[2] & masks->r2) << masks->nameShift;
}

uint32_t coleco_vdp_color_base(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }
    const uint32_t high = coleco_vdp_is_msx2(state) ? static_cast<uint32_t>(state->regs[10] & 0x07u) << 14 : 0u;
    const uint32_t low = static_cast<uint32_t>(state->regs[3] & masks->r3) << 6;
    return high | low;
}

uint32_t coleco_vdp_pattern_base(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }
    return static_cast<uint32_t>(state->regs[4] & masks->r4) << 11;
}

uint32_t coleco_vdp_graphics2_pattern_base(const ColecoVdpState* state)
{
    if (!state) {
        return 0u;
    }

    // SCREEN 2 style modes use the wider pattern-table selection used by fMSX.
    // Restricting MSX1 to bit 2 only causes visible tile corruption in games
    // such as Arkanoid because the BIOS/game can relocate the 6KB pattern area.
    return static_cast<uint32_t>(state->regs[4] & 0x3Cu) << 11;
}

uint32_t coleco_vdp_name_mask(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[2]) | ~static_cast<uint32_t>(masks->m2)) << masks->nameShift)
          | ((1u << masks->nameShift) - 1u));
}

uint32_t coleco_vdp_color_mask(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[3]) | ~static_cast<uint32_t>(masks->m3)) << 6) | 0x1C03Fu);
}

uint32_t coleco_vdp_pattern_mask(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[4]) | ~static_cast<uint32_t>(masks->m4)) << 11) | 0x007FFu);
}

uint32_t coleco_vdp_sprite_attr_base(const ColecoVdpState* state)
{
    const ColecoVdpTableMasks* masks = state ? coleco_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }

    const uint32_t high = coleco_vdp_is_msx2(state) ? static_cast<uint32_t>(state->regs[11] & 0x03u) << 15 : 0u;
    return high | (static_cast<uint32_t>(state->regs[5] & masks->r5) << 7);
}

uint32_t coleco_vdp_sprite_pattern_base(const ColecoVdpState* state)
{
    return state ? static_cast<uint32_t>(state->regs[6]) << 11 : 0u;
}

inline uint8_t coleco_vdp_vscroll(const ColecoVdpState* state)
{
    return (state && coleco_vdp_is_msx2(state)) ? state->regs[23] : 0u;
}

inline bool coleco_vdp_sprites_disabled(const ColecoVdpState* state)
{
    return state && coleco_vdp_is_msx2(state) && (state->regs[8] & 0x02u) != 0u;
}

inline bool coleco_vdp_mode_yjk(const ColecoVdpState* state)
{
    return state && coleco_vdp_is_msx2(state) && (state->regs[25] & 0x08u) != 0u;
}

inline bool coleco_vdp_mode_yae(const ColecoVdpState* state)
{
    return coleco_vdp_mode_yjk(state) && (state->regs[25] & 0x10u) != 0u;
}

inline bool coleco_vdp_hscroll512(const ColecoVdpState* state)
{
    return state && coleco_vdp_is_msx2(state) && (state->regs[25] & 0x01u) != 0u;
}

inline uint16_t coleco_vdp_hscroll(const ColecoVdpState* state)
{
    if (!state || !coleco_vdp_is_msx2(state)) {
        return 0u;
    }

    return static_cast<uint16_t>((state->regs[27] & 0x07u) | ((state->regs[26] & 0x3Fu) << 3));
}

inline uint8_t coleco_vdp_screen8_mapped_color(uint8_t color)
{
    static constexpr uint8_t kSpriteToScreen8[16] = {
        0x00, 0x02, 0x10, 0x12, 0x80, 0x82, 0x90, 0x92,
        0x49, 0x4B, 0x59, 0x5B, 0xC9, 0xCB, 0xD9, 0xDB,
    };

    return kSpriteToScreen8[color & 0x0Fu];
}

uint8_t coleco_vdp_yjk_color_index(int y, int j, int k)
{
    int r = y + j;
    int g = y + k;
    int b = (5 * y - 2 * j - k) / 4;

    r = r < 0 ? 0 : (r > 31 ? 31 : r);
    g = g < 0 ? 0 : (g > 31 ? 31 : g);
    b = b < 0 ? 0 : (b > 31 ? 31 : b);

    return static_cast<uint8_t>((r & 0x1Cu) | ((g & 0x1Cu) << 3) | (b >> 3));
}

int coleco_vdp_command_mode_index(const ColecoVdpState* state)
{
    if (!state || !coleco_vdp_is_msx2(state)) {
        return -1;
    }

    switch (state->mode) {
        case ColecoVdpMode::Bitmap4:
            return 0;
        case ColecoVdpMode::Bitmap6:
            return 1;
        case ColecoVdpMode::Bitmap7:
            return 2;
        case ColecoVdpMode::Bitmap8:
            return 3;
        default:
            return -1;
    }
}

uint8_t coleco_vdp_command_mask(uint8_t screenMode)
{
    static constexpr uint8_t kMasks[4] = {0x0Fu, 0x03u, 0x0Fu, 0xFFu};
    return kMasks[screenMode & 0x03u];
}

uint16_t coleco_vdp_command_ppb(uint8_t screenMode)
{
    static constexpr uint16_t kPixelsPerByte[4] = {2u, 4u, 2u, 1u};
    return kPixelsPerByte[screenMode & 0x03u];
}

uint16_t coleco_vdp_command_ppl(uint8_t screenMode)
{
    static constexpr uint16_t kPixelsPerLine[4] = {256u, 512u, 512u, 256u};
    return kPixelsPerLine[screenMode & 0x03u];
}

uint32_t coleco_vdp_command_addr(uint8_t screenMode, int x, int y)
{
    switch (screenMode & 0x03u) {
        case 0:
            return (static_cast<uint32_t>(y & 1023) << 7) + static_cast<uint32_t>((x & 255) >> 1);
        case 1:
            return (static_cast<uint32_t>(y & 1023) << 7) + static_cast<uint32_t>((x & 511) >> 2);
        case 2:
            return (static_cast<uint32_t>(y & 511) << 8) + static_cast<uint32_t>((x & 511) >> 1);
        case 3:
        default:
            return (static_cast<uint32_t>(y & 511) << 8) + static_cast<uint32_t>(x & 255);
    }
}

uint8_t coleco_vdp_command_point(const ColecoVdpState* state, uint8_t screenMode, int x, int y)
{
    const uint32_t addr = coleco_vdp_command_addr(screenMode, x, y) & state->vramMask;
    const uint8_t value = state->vram[addr];

    switch (screenMode & 0x03u) {
        case 0:
            return static_cast<uint8_t>((value >> (((~x) & 1) << 2)) & 0x0Fu);
        case 1:
            return static_cast<uint8_t>((value >> (((~x) & 3) << 1)) & 0x03u);
        case 2:
            return static_cast<uint8_t>((value >> (((~x) & 1) << 2)) & 0x0Fu);
        case 3:
        default:
            return value;
    }
}

void coleco_vdp_command_psetlowlevel(uint8_t* dst, uint8_t color, uint8_t preserveMask, uint8_t op)
{
    switch (op & 0x0Fu) {
        case 0x00:
            *dst = static_cast<uint8_t>((*dst & preserveMask) | color);
            break;
        case 0x01:
            *dst = static_cast<uint8_t>(*dst & (color | preserveMask));
            break;
        case 0x02:
            *dst = static_cast<uint8_t>(*dst | color);
            break;
        case 0x03:
            *dst = static_cast<uint8_t>(*dst ^ color);
            break;
        case 0x04:
            *dst = static_cast<uint8_t>((*dst & preserveMask) | ~(color | preserveMask));
            break;
        case 0x08:
            if (color != 0u) {
                *dst = static_cast<uint8_t>((*dst & preserveMask) | color);
            }
            break;
        case 0x09:
            if (color != 0u) {
                *dst = static_cast<uint8_t>(*dst & (color | preserveMask));
            }
            break;
        case 0x0A:
            if (color != 0u) {
                *dst = static_cast<uint8_t>(*dst | color);
            }
            break;
        case 0x0B:
            if (color != 0u) {
                *dst = static_cast<uint8_t>(*dst ^ color);
            }
            break;
        case 0x0C:
            if (color != 0u) {
                *dst = static_cast<uint8_t>((*dst & preserveMask) | ~(color | preserveMask));
            }
            break;
        default:
            break;
    }
}

void coleco_vdp_command_pset(ColecoVdpState* state, uint8_t screenMode, int x, int y, uint8_t color, uint8_t op)
{
    const uint32_t addr = coleco_vdp_command_addr(screenMode, x, y) & state->vramMask;
    uint8_t* const dst = state->vram + addr;

    switch (screenMode & 0x03u) {
        case 0: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 1) << 2);
            coleco_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x0Fu) << shift),
                                         static_cast<uint8_t>(~(0x0Fu << shift)), op);
            break;
        }
        case 1: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 3) << 1);
            coleco_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x03u) << shift),
                                         static_cast<uint8_t>(~(0x03u << shift)), op);
            break;
        }
        case 2: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 1) << 2);
            coleco_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x0Fu) << shift),
                                         static_cast<uint8_t>(~(0x0Fu << shift)), op);
            break;
        }
        case 3:
        default:
            coleco_vdp_command_psetlowlevel(dst, color, 0x00u, op);
            break;
    }

    state->dirty = true;
}

uint8_t coleco_vdp_read_reg16(const ColecoVdpState* state, uint8_t lowReg)
{
    return static_cast<uint8_t>(state->regs[lowReg] | ((state->regs[lowReg + 1u] & 0x03u) << 8));
}

uint16_t coleco_vdp_read_reg10(const ColecoVdpState* state, uint8_t lowReg)
{
    return static_cast<uint16_t>(state->regs[lowReg] | ((state->regs[lowReg + 1u] & 0x03u) << 8));
}

void coleco_vdp_write_reg10(ColecoVdpState* state, uint8_t lowReg, uint16_t value)
{
    value &= 0x03FFu;
    state->regs[lowReg] = static_cast<uint8_t>(value & 0xFFu);
    state->regs[lowReg + 1u] = static_cast<uint8_t>((state->regs[lowReg + 1u] & ~0x03u) | ((value >> 8) & 0x03u));
}

void coleco_vdp_command_finish(ColecoVdpState* state)
{
    state->command.transfer = ColecoVdpTransferCommand::None;
    state->status[2] &= static_cast<uint8_t>(~0x01u);
}

void coleco_vdp_command_prepare_transfer(ColecoVdpState* state, ColecoVdpTransferCommand transfer, uint8_t screenMode, uint8_t opcode)
{
    ColecoVdpCommandState& command = state->command;
    const uint16_t nxRaw = coleco_vdp_read_reg10(state, 40u);
    const uint16_t nyRaw = coleco_vdp_read_reg10(state, 42u);
    const uint16_t ppl = coleco_vdp_command_ppl(screenMode);
    const uint16_t ppb = coleco_vdp_command_ppb(screenMode);
    const bool byteTransfer = ((opcode >> 4) & 0x0Cu) == 0x0Cu;

    command.transfer = transfer;
    command.screenMode = screenMode;
    command.logicOp = static_cast<uint8_t>(opcode & 0x0Fu);
    command.sx = coleco_vdp_read_reg10(state, 32u);
    command.sy = coleco_vdp_read_reg10(state, 34u);
    command.dx = coleco_vdp_read_reg10(state, 36u);
    command.dy = coleco_vdp_read_reg10(state, 38u);
    command.ny = nyRaw == 0u ? 1024u : nyRaw;
    command.ty = (state->regs[45] & 0x08u) != 0u ? -1 : 1;
    command.mx = ppl;
    if (byteTransfer) {
        const uint16_t nxBytes = nxRaw == 0u ? 1024u : nxRaw;
        command.tx = (state->regs[45] & 0x04u) != 0u ? -static_cast<int16_t>(ppb) : static_cast<int16_t>(ppb);
        command.nx = static_cast<uint16_t>(nxBytes / ppb);
    } else {
        command.tx = (state->regs[45] & 0x04u) != 0u ? -1 : 1;
        command.nx = nxRaw == 0u ? 1024u : nxRaw;
    }
    command.asx = command.sx;
    command.adx = command.dx;
    command.anx = command.nx;
    state->status[2] |= 0x01u;
}

void coleco_vdp_command_continue(ColecoVdpState* state)
{
    ColecoVdpCommandState& command = state->command;
    if (!state || command.transfer == ColecoVdpTransferCommand::None || (state->status[2] & 0x80u) != 0u) {
        return;
    }

    switch (command.transfer) {
        case ColecoVdpTransferCommand::Lmcm: {
            const uint8_t value = coleco_vdp_command_point(state, command.screenMode, command.asx, command.sy);
            state->regs[44] = value;
            state->status[7] = value;
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.asx = static_cast<uint16_t>(command.asx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.sy = static_cast<uint16_t>(command.sy + command.ty)) == 0xFFFFu) {
                    coleco_vdp_write_reg10(state, 42u, command.ny);
                    coleco_vdp_write_reg10(state, 34u, command.sy);
                    coleco_vdp_command_finish(state);
                } else {
                    command.asx = command.sx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case ColecoVdpTransferCommand::Lmmc: {
            const uint8_t value = static_cast<uint8_t>(state->regs[44] & coleco_vdp_command_mask(command.screenMode));
            state->regs[44] = value;
            state->status[7] = value;
            coleco_vdp_command_pset(state, command.screenMode, command.adx, command.dy, value, command.logicOp);
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    coleco_vdp_write_reg10(state, 42u, command.ny);
                    coleco_vdp_write_reg10(state, 38u, command.dy);
                    coleco_vdp_command_finish(state);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case ColecoVdpTransferCommand::Hmmc: {
            const uint32_t addr = coleco_vdp_command_addr(command.screenMode, command.adx, command.dy) & state->vramMask;
            const uint8_t value = state->regs[44];
            state->vram[addr] = value;
            state->status[7] = value;
            state->dirty = true;
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    coleco_vdp_write_reg10(state, 42u, command.ny);
                    coleco_vdp_write_reg10(state, 38u, command.dy);
                    coleco_vdp_command_finish(state);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case ColecoVdpTransferCommand::None:
        default:
            break;
    }
}

uint8_t coleco_vdp_command_read(ColecoVdpState* state)
{
    if (!state) {
        return 0xFFu;
    }

    state->status[2] &= static_cast<uint8_t>(~0x80u);
    coleco_vdp_command_continue(state);
    return state->regs[44];
}

void coleco_vdp_command_write(ColecoVdpState* state, uint8_t value)
{
    if (!state) {
        return;
    }

    state->status[2] &= static_cast<uint8_t>(~0x80u);
    state->regs[44] = value;
    state->status[7] = value;
    coleco_vdp_command_continue(state);
}

void coleco_vdp_command_execute(ColecoVdpState* state, uint8_t opcode)
{
    if (!state || !coleco_vdp_is_msx2(state)) {
        return;
    }

    const int sm = coleco_vdp_command_mode_index(state);
    state->command.transfer = ColecoVdpTransferCommand::None;
    state->status[2] &= static_cast<uint8_t>(~0x81u);
    if (sm < 0) {
        return;
    }

    const uint8_t screenMode = static_cast<uint8_t>(sm);
    const uint8_t command = static_cast<uint8_t>(opcode >> 4);
    const uint8_t logicOp = static_cast<uint8_t>(opcode & 0x0Fu);
    const uint8_t colorMask = coleco_vdp_command_mask(screenMode);
    uint16_t sx = coleco_vdp_read_reg10(state, 32u);
    uint16_t sy = coleco_vdp_read_reg10(state, 34u);
    uint16_t dx = coleco_vdp_read_reg10(state, 36u);
    uint16_t dy = coleco_vdp_read_reg10(state, 38u);
    uint16_t nx = coleco_vdp_read_reg10(state, 40u);
    uint16_t ny = coleco_vdp_read_reg10(state, 42u);
    const int txDot = (state->regs[45] & 0x04u) != 0u ? -1 : 1;
    const int ty = (state->regs[45] & 0x08u) != 0u ? -1 : 1;
    const bool searchNotEqual = (state->regs[45] & 0x02u) != 0u;
    const bool lineYMajor = (state->regs[45] & 0x01u) != 0u;
    const uint16_t ppl = coleco_vdp_command_ppl(screenMode);
    const uint16_t ppb = coleco_vdp_command_ppb(screenMode);

    if ((command & 0x0Cu) != 0x0Cu && command != 0u) {
        state->regs[44] = static_cast<uint8_t>(state->regs[44] & colorMask);
        state->status[7] = state->regs[44];
    }

    switch (command) {
        case 0x0:
            return;
        case 0x4: {
            const uint8_t value = coleco_vdp_command_point(state, screenMode, sx, sy);
            state->regs[44] = value;
            state->status[7] = value;
            return;
        }
        case 0x5:
            coleco_vdp_command_pset(state, screenMode, dx, dy, state->regs[44], logicOp);
            return;
        case 0x6: {
            const uint8_t color = static_cast<uint8_t>(state->regs[44] & colorMask);
            int x = sx;
            state->status[2] &= static_cast<uint8_t>(~0x10u);
            for (;;) {
                if (((coleco_vdp_command_point(state, screenMode, x, sy) == color) ? 1 : 0) ^ (searchNotEqual ? 1 : 0)) {
                    state->status[2] |= 0x10u;
                    break;
                }
                x += txDot;
                if ((x & ppl) != 0) {
                    state->status[2] &= static_cast<uint8_t>(~0x10u);
                    break;
                }
            }
            state->status[8] = static_cast<uint8_t>(x & 0xFFu);
            state->status[9] = static_cast<uint8_t>(((x >> 8) & 0x01u) | 0xFEu);
            return;
        }
        case 0x7: {
            const int major = nx == 0u ? 1024 : nx;
            const int minor = ny == 0u ? 1024 : ny;
            int acc = ((major - 1) >> 1);
            int count = 0;
            int x = dx;
            int y = dy;

            if (!lineYMajor) {
                while (count++ != major && (x & ppl) == 0) {
                    coleco_vdp_command_pset(state, screenMode, x, y, state->regs[44], logicOp);
                    x += txDot;
                    if ((acc -= minor) < 0) {
                        acc += major;
                        y += ty;
                    }
                    acc &= 1023;
                }
            } else {
                while (count++ != major && (x & ppl) == 0) {
                    coleco_vdp_command_pset(state, screenMode, x, y, state->regs[44], logicOp);
                    y += ty;
                    if ((acc -= minor) < 0) {
                        acc += major;
                        x += txDot;
                    }
                    acc &= 1023;
                }
            }

            coleco_vdp_write_reg10(state, 38u, static_cast<uint16_t>(y));
            return;
        }
        case 0x8: {
            if (nx == 0u) nx = 1024u;
            if (ny == 0u) ny = 1024u;
            uint16_t startX = dx;
            for (uint16_t row = 0; row < ny; ++row) {
                uint16_t x = startX;
                for (uint16_t col = 0; col < nx; ++col) {
                    coleco_vdp_command_pset(state, screenMode, x, dy, state->regs[44], logicOp);
                    x = static_cast<uint16_t>(x + txDot);
                    if ((x & ppl) != 0) {
                        break;
                    }
                }
                dy = static_cast<uint16_t>(dy + ty);
                if (dy == 0xFFFFu) {
                    break;
                }
            }
            coleco_vdp_write_reg10(state, 42u, 0u);
            coleco_vdp_write_reg10(state, 38u, dy);
            return;
        }
        case 0x9: {
            if (nx == 0u) nx = 1024u;
            if (ny == 0u) ny = 1024u;
            const uint16_t srcStartX = sx;
            const uint16_t dstStartX = dx;
            for (uint16_t row = 0; row < ny; ++row) {
                uint16_t srcX = srcStartX;
                uint16_t dstX = dstStartX;
                for (uint16_t col = 0; col < nx; ++col) {
                    const uint8_t pixel = coleco_vdp_command_point(state, screenMode, srcX, sy);
                    coleco_vdp_command_pset(state, screenMode, dstX, dy, pixel, logicOp);
                    srcX = static_cast<uint16_t>(srcX + txDot);
                    dstX = static_cast<uint16_t>(dstX + txDot);
                    if ((srcX & ppl) != 0 || (dstX & ppl) != 0) {
                        break;
                    }
                }
                sy = static_cast<uint16_t>(sy + ty);
                dy = static_cast<uint16_t>(dy + ty);
                if (sy == 0xFFFFu || dy == 0xFFFFu) {
                    break;
                }
            }
            coleco_vdp_write_reg10(state, 42u, 0u);
            coleco_vdp_write_reg10(state, 34u, sy);
            coleco_vdp_write_reg10(state, 38u, dy);
            return;
        }
        case 0xA:
            coleco_vdp_command_prepare_transfer(state, ColecoVdpTransferCommand::Lmcm, screenMode, opcode);
            coleco_vdp_command_continue(state);
            return;
        case 0xB:
            coleco_vdp_command_prepare_transfer(state, ColecoVdpTransferCommand::Lmmc, screenMode, opcode);
            coleco_vdp_command_continue(state);
            return;
        case 0xC: {
            const uint16_t nxBytes = nx == 0u ? 1024u : nx;
            if (ny == 0u) ny = 1024u;
            const int tx = (state->regs[45] & 0x04u) != 0u ? -static_cast<int>(ppb) : static_cast<int>(ppb);
            const uint16_t startX = dx;
            for (uint16_t row = 0; row < ny; ++row) {
                uint16_t x = startX;
                for (uint16_t col = 0; col < static_cast<uint16_t>(nxBytes / ppb); ++col) {
                    const uint32_t addr = coleco_vdp_command_addr(screenMode, x, dy) & state->vramMask;
                    state->vram[addr] = state->regs[44];
                    state->dirty = true;
                    x = static_cast<uint16_t>(x + tx);
                    if ((x & ppl) != 0) {
                        break;
                    }
                }
                dy = static_cast<uint16_t>(dy + ty);
                if (dy == 0xFFFFu) {
                    break;
                }
            }
            coleco_vdp_write_reg10(state, 42u, 0u);
            coleco_vdp_write_reg10(state, 38u, dy);
            return;
        }
        case 0xD: {
            const uint16_t nxBytes = nx == 0u ? 1024u : nx;
            if (ny == 0u) ny = 1024u;
            const int tx = (state->regs[45] & 0x04u) != 0u ? -static_cast<int>(ppb) : static_cast<int>(ppb);
            const uint16_t srcStartX = sx;
            const uint16_t dstStartX = dx;
            for (uint16_t row = 0; row < ny; ++row) {
                uint16_t srcX = srcStartX;
                uint16_t dstX = dstStartX;
                for (uint16_t col = 0; col < static_cast<uint16_t>(nxBytes / ppb); ++col) {
                    const uint32_t srcAddr = coleco_vdp_command_addr(screenMode, srcX, sy) & state->vramMask;
                    const uint32_t dstAddr = coleco_vdp_command_addr(screenMode, dstX, dy) & state->vramMask;
                    state->vram[dstAddr] = state->vram[srcAddr];
                    state->dirty = true;
                    srcX = static_cast<uint16_t>(srcX + tx);
                    dstX = static_cast<uint16_t>(dstX + tx);
                    if ((srcX & ppl) != 0 || (dstX & ppl) != 0) {
                        break;
                    }
                }
                sy = static_cast<uint16_t>(sy + ty);
                dy = static_cast<uint16_t>(dy + ty);
                if (sy == 0xFFFFu || dy == 0xFFFFu) {
                    break;
                }
            }
            coleco_vdp_write_reg10(state, 42u, 0u);
            coleco_vdp_write_reg10(state, 34u, sy);
            coleco_vdp_write_reg10(state, 38u, dy);
            return;
        }
        case 0xE: {
            const uint16_t nxBytes = nx == 0u ? 1024u : nx;
            if (ny == 0u) ny = 1024u;
            const int tx = (state->regs[45] & 0x04u) != 0u ? -static_cast<int>(ppb) : static_cast<int>(ppb);
            const uint16_t startX = dx;
            for (uint16_t row = 0; row < ny; ++row) {
                uint16_t x = startX;
                for (uint16_t col = 0; col < static_cast<uint16_t>(nxBytes / ppb); ++col) {
                    const uint32_t srcAddr = coleco_vdp_command_addr(screenMode, x, sy) & state->vramMask;
                    const uint32_t dstAddr = coleco_vdp_command_addr(screenMode, x, dy) & state->vramMask;
                    state->vram[dstAddr] = state->vram[srcAddr];
                    state->dirty = true;
                    x = static_cast<uint16_t>(x + tx);
                    if ((x & ppl) != 0) {
                        break;
                    }
                }
                sy = static_cast<uint16_t>(sy + ty);
                dy = static_cast<uint16_t>(dy + ty);
                if (sy == 0xFFFFu || dy == 0xFFFFu) {
                    break;
                }
            }
            coleco_vdp_write_reg10(state, 42u, 0u);
            coleco_vdp_write_reg10(state, 34u, sy);
            coleco_vdp_write_reg10(state, 38u, dy);
            return;
        }
        case 0xF:
            coleco_vdp_command_prepare_transfer(state, ColecoVdpTransferCommand::Hmmc, screenMode, opcode);
            coleco_vdp_command_continue(state);
            return;
        default:
            return;
    }
}

void coleco_vdp_write_register(ColecoVdpState* state, uint8_t reg, uint8_t value)
{
    if (!state || reg >= sizeof(state->regs)) {
        return;
    }

    if (reg == 44u && coleco_vdp_is_msx2(state)) {
        coleco_vdp_command_write(state, value);
        return;
    }

#if COLECO_VDP_TRACE_ENABLED
    static int s_regWriteCount = 0;
    if ((state->regs[reg] != value) && (s_regWriteCount < 32)) {
        ++s_regWriteCount;
        std::printf("[MSX][VDP] REG%u <- 0x%02X (prev=0x%02X irq=%s) #%d\n",
                    static_cast<unsigned>(reg),
                    static_cast<unsigned>(value),
                    static_cast<unsigned>(state->regs[reg]),
                    (reg == 1u && (value & 0x20u) == 0u) ? "off" :
                    (reg == 1u && (value & 0x20u) != 0u) ? "on" : "-",
                    s_regWriteCount);
    }
#endif

    if (state->regs[reg] != value) {
        state->regs[reg] = value;
        if (coleco_vdp_register_affects_output(reg)) {
            state->dirty = true;
        }
    }

    if (reg == 16u) {
        state->palettePending = false;
    }
    if (reg == 14u) {
        state->address = (state->address & 0x3FFFu) | (static_cast<uint32_t>(state->regs[14] & 0x07u) << 14);
        state->address &= state->vramMask;
    }
    if (reg == 46u && coleco_vdp_is_msx2(state)) {
        coleco_vdp_command_execute(state, value);
    }
}

void coleco_vdp_reset_sprite_status(ColecoVdpState* state)
{
    if (!state) {
        return;
    }

    // Keep VBLANK flag (bit 7); clear overflow (bit 6) and collision (bit 5);
    // reset 5th-sprite number to 0x1F (31 = "no 5th sprite").
    state->status[0] = static_cast<uint8_t>((state->status[0] & 0x80u) | 0x1Fu);
}

void coleco_vdp_record_sprite_index(ColecoVdpState* state, uint8_t index)
{
    if (!state) {
        return;
    }

    state->status[0] = static_cast<uint8_t>((state->status[0] & ~0x1Fu) | (index & 0x1Fu));
    if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
        s_vdpOriginalState->status[0] = static_cast<uint8_t>((s_vdpOriginalState->status[0] & ~0x1Fu) | (index & 0x1Fu));
    }
}

void coleco_vdp_record_sprite_overflow(ColecoVdpState* state, uint8_t index)
{
    if (!state || (state->status[0] & 0x40u) != 0u) {
        return;
    }

    coleco_vdp_record_sprite_index(state, index);
    state->status[0] |= 0x40u;
    if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
        s_vdpOriginalState->status[0] |= 0x40u;
    }
}

inline void coleco_vdp_record_sprite_collision(ColecoVdpState* state)
{
    if (state) {
        state->status[0] |= 0x20u;
        if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
            s_vdpOriginalState->status[0] |= 0x20u;
        }
    }
}

void coleco_vdp_plot_sprite_bits(ColecoVdpState* state,
                              uint8_t* dst,
                              uint8_t* occupancy,
                              int x,
                              uint8_t pattern,
                              uint8_t color,
                              unsigned scale)
{
    for (unsigned bit = 0; bit < 8u; ++bit) {
        if ((pattern & static_cast<uint8_t>(0x80u >> bit)) == 0u) {
            continue;
        }

        const int pixelBase = x + static_cast<int>(bit * scale);
        for (unsigned repeat = 0; repeat < scale; ++repeat) {
            const int px = pixelBase + static_cast<int>(repeat);
            if (px < 0 || px >= static_cast<int>(kColecoFrameWidth)) {
                continue;
            }
            if (occupancy[px] != 0u) {
                coleco_vdp_record_sprite_collision(state);
            }
            occupancy[px] = 1u;
            dst[px] = color;
        }
    }
}

void coleco_vdp_render_mono_sprites_line(ColecoVdpState* state, unsigned y, uint8_t* dst)
{
    if (!state || !dst || coleco_vdp_sprites_disabled(state)) {
        return;
    }

    static constexpr uint8_t kSpriteHeights[4] = {8u, 16u, 16u, 32u};
    const uint8_t outputHeight = kSpriteHeights[state->regs[1] & 0x03u];
    const uint8_t inputHeight = kSpriteHeights[state->regs[1] & 0x02u];
    const uint32_t attrBase = coleco_vdp_sprite_attr_base(state);
    const uint32_t patternBase = coleco_vdp_sprite_pattern_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    uint8_t selected[32];
    uint8_t count = 0u;
    uint8_t lastIndex = 31u;
    const unsigned spriteLimit = kColecoMaxSpritesLineColeco1;

    for (uint8_t index = 0; index < 32u; ++index) {
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(coleco_vdp_read_vram_fast(vram, mask, attr));
        if (spriteY == 208) {
            lastIndex = index;
            break;
        }
        if (spriteY > (256 - inputHeight)) {
            spriteY -= 256;
        }

        const int scanY = static_cast<int>(y);
        if (scanY > spriteY && scanY <= spriteY + outputHeight) {
            if (count >= spriteLimit) {
                coleco_vdp_record_sprite_overflow(state, index);
                lastIndex = index;
                break;
            }
            selected[count++] = index;
        }

        lastIndex = index;
    }

    coleco_vdp_record_sprite_index(state, lastIndex);
    std::memset(s_msxSpriteOccupancy, 0, sizeof(s_msxSpriteOccupancy));

    const unsigned scale = (outputHeight > inputHeight) ? 2u : 1u;
    for (int selectedIndex = static_cast<int>(count) - 1; selectedIndex >= 0; --selectedIndex) {
        const uint8_t index = selected[static_cast<size_t>(selectedIndex)];
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(coleco_vdp_read_vram_fast(vram, mask, attr));
        if (spriteY > (256 - inputHeight)) {
            spriteY -= 256;
        }

        const uint8_t xRaw = coleco_vdp_read_vram_fast(vram, mask, attr + 1u);
        const uint8_t patternId = coleco_vdp_read_vram_fast(vram, mask, attr + 2u);
        const uint8_t attrColor = coleco_vdp_read_vram_fast(vram, mask, attr + 3u);
        const uint8_t color = static_cast<uint8_t>(attrColor & 0x0Fu);
        if (color == 0u) {
            continue;
        }

        const int x = static_cast<int>(xRaw) - (((attrColor & 0x80u) != 0u) ? 32 : 0);
        int line = static_cast<int>(y) - spriteY - 1;
        if (scale == 2u) {
            line >>= 1;
        }
        if (line < 0) {
            continue;
        }

        const uint8_t spriteColor = coleco_vdp_resolve_color(state, color);
        const uint8_t basePattern = (inputHeight > 8u) ? static_cast<uint8_t>(patternId & 0xFCu) : patternId;
        const uint32_t patternRow = patternBase + static_cast<uint32_t>(basePattern) * 8u + static_cast<uint32_t>(line);
        const uint8_t leftBits = coleco_vdp_read_vram_fast(vram, mask, patternRow);
        coleco_vdp_plot_sprite_bits(state, dst, s_msxSpriteOccupancy, x, leftBits, spriteColor, scale);

        if (inputHeight > 8u) {
            const uint8_t rightBits = coleco_vdp_read_vram_fast(vram, mask, patternRow + 16u);
            coleco_vdp_plot_sprite_bits(state, dst, s_msxSpriteOccupancy, x + static_cast<int>(8u * scale), rightBits, spriteColor, scale);
        }
    }
}

void coleco_vdp_plot_color_sprite_bits(ColecoVdpState* state,
                                    uint8_t* line,
                                    int x,
                                    uint8_t pattern,
                                    uint8_t color,
                                    unsigned scale,
                                    bool orColors)
{
    for (unsigned bit = 0; bit < 8u; ++bit) {
        if ((pattern & static_cast<uint8_t>(0x80u >> bit)) == 0u) {
            continue;
        }

        const int pixelBase = x + static_cast<int>(bit * scale);
        for (unsigned repeat = 0; repeat < scale; ++repeat) {
            const int px = pixelBase + static_cast<int>(repeat);
            if (px < -32 || px >= static_cast<int>(kColecoFrameWidth + 32u)) {
                continue;
            }
            uint8_t& dst = line[static_cast<size_t>(px + 32)];
            if (dst != 0u) {
                coleco_vdp_record_sprite_collision(state);
                if (orColors) {
                    dst = static_cast<uint8_t>(dst | color);
                }
            } else {
                dst = color;
            }
        }
    }
}

void coleco_vdp_render_color_sprites_line(ColecoVdpState* state, unsigned y, uint8_t* line)
{
    if (!state || !line) {
        return;
    }

    std::memset(line, 0, kColecoSpriteColorLineWidth);
    if (coleco_vdp_sprites_disabled(state)) {
        return;
    }

    static constexpr uint8_t kSpriteHeights[4] = {8u, 16u, 16u, 32u};
    const uint8_t outputHeight = kSpriteHeights[state->regs[1] & 0x03u];
    const uint8_t inputHeight = kSpriteHeights[state->regs[1] & 0x02u];
    const uint32_t attrBase = coleco_vdp_sprite_attr_base(state);
    const uint32_t patternBase = coleco_vdp_sprite_pattern_base(state);
    const uint32_t colorBase = static_cast<uint32_t>((attrBase - 0x200u) & state->vramMask);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    uint8_t selected[32];
    uint8_t count = 0u;
    uint8_t lastIndex = 31u;

    for (uint8_t index = 0; index < 32u; ++index) {
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(coleco_vdp_read_vram_fast(vram, mask, attr));
        if (spriteY == 216) {
            lastIndex = index;
            break;
        }

        spriteY = static_cast<uint8_t>(spriteY - coleco_vdp_vscroll(state));
        if (spriteY > (256 - inputHeight)) {
            spriteY -= 256;
        }

        const int scanY = static_cast<int>(y);
        if (scanY > spriteY && scanY <= spriteY + outputHeight) {
            if (count >= kColecoMaxSpritesLineColeco2) {
                coleco_vdp_record_sprite_overflow(state, index);
                lastIndex = index;
                break;
            }
            selected[count++] = index;
        }

        lastIndex = index;
    }

    coleco_vdp_record_sprite_index(state, lastIndex);

    const unsigned scale = (outputHeight > inputHeight) ? 2u : 1u;
    for (int selectedIndex = static_cast<int>(count) - 1; selectedIndex >= 0; --selectedIndex) {
        const uint8_t index = selected[static_cast<size_t>(selectedIndex)];
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(coleco_vdp_read_vram_fast(vram, mask, attr));
        spriteY = static_cast<uint8_t>(spriteY - coleco_vdp_vscroll(state));
        if (spriteY > (256 - inputHeight)) {
            spriteY -= 256;
        }

        const uint8_t xRaw = coleco_vdp_read_vram_fast(vram, mask, attr + 1u);
        const uint8_t patternId = coleco_vdp_read_vram_fast(vram, mask, attr + 2u);
        int lineIndex = static_cast<int>(y) - spriteY - 1;
        if (scale == 2u) {
            lineIndex >>= 1;
        }
        if (lineIndex < 0) {
            continue;
        }

        const uint8_t colorAttr = coleco_vdp_read_vram_fast(vram, mask, colorBase + static_cast<uint32_t>(index) * 4u + static_cast<uint32_t>(lineIndex));
        const uint8_t color = static_cast<uint8_t>(colorAttr & 0x0Fu);
        if (color == 0u) {
            continue;
        }

        const bool orColors = (colorAttr & 0x40u) != 0u;
        const int x = static_cast<int>(xRaw) - (((colorAttr & 0x80u) != 0u) ? 32 : 0);
        const uint8_t basePattern = (inputHeight > 8u) ? static_cast<uint8_t>(patternId & 0xFCu) : patternId;
        const uint32_t patternRow = patternBase + static_cast<uint32_t>(basePattern) * 8u + static_cast<uint32_t>(lineIndex);
        coleco_vdp_plot_color_sprite_bits(state,
                                       line,
                                       x,
                                       coleco_vdp_read_vram_fast(vram, mask, patternRow),
                                       color,
                                       scale,
                                       orColors);

        if (inputHeight > 8u) {
            coleco_vdp_plot_color_sprite_bits(state,
                                           line,
                                           x + static_cast<int>(8u * scale),
                                           coleco_vdp_read_vram_fast(vram, mask, patternRow + 16u),
                                           color,
                                           scale,
                                           orColors);
        }
    }
}

uint32_t coleco_vdp_bitmap4_base(const ColecoVdpState* state)
{
    return static_cast<uint32_t>((state->regs[2] >> 5) & 0x03u) << 15;
}

ColecoVdpMode coleco_vdp_decode_mode(const ColecoVdpState* state)
{
    if (!state) {
        return ColecoVdpMode::Unsupported;
    }

    const uint8_t m1 = static_cast<uint8_t>((state->regs[1] >> 4) & 0x01u);
    const uint8_t m2 = static_cast<uint8_t>((state->regs[1] >> 3) & 0x01u);
    const uint8_t m3 = static_cast<uint8_t>((state->regs[0] >> 1) & 0x01u);
    const uint8_t m4 = static_cast<uint8_t>((state->regs[0] >> 2) & 0x01u);
    const uint8_t m5 = static_cast<uint8_t>((state->regs[0] >> 3) & 0x01u);

    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Graphics1;
    }
    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 1u) {
        return ColecoVdpMode::Text40;
    }
    if (m5 == 0u && m4 == 0u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Graphics2;
    }
    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 1u && m1 == 0u) {
        return ColecoVdpMode::Multicolor;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Graphics3;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Bitmap4;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 1u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Bitmap6;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 1u && m4 == 0u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Bitmap7;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 1u && m4 == 1u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return ColecoVdpMode::Bitmap8;
    }
    if (coleco_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 0u && m2 == 0u && m1 == 1u) {
        return ColecoVdpMode::Text80;
    }
    return ColecoVdpMode::Unsupported;
}

void coleco_vdp_update_mode_geometry(ColecoVdpState* state)
{
    if (!state) {
        return;
    }

    state->mode = coleco_vdp_decode_mode(state);
    state->activeWidth = kColecoFrameWidth;
    state->activeHeight = kColecoFrameHeightColeco1;
    if (state->mode == ColecoVdpMode::Text80) {
        state->activeWidth = 240u;
    }
    if (coleco_vdp_is_msx2(state) &&
        state->mode != ColecoVdpMode::Text40 &&
        state->mode != ColecoVdpMode::Text80 &&
        state->mode != ColecoVdpMode::Unsupported &&
        (state->regs[9] & 0x80u) != 0u) {
        state->activeHeight = kColecoFrameHeightColeco2;
    }
}

void coleco_vdp_clear_active_frame(ColecoVdpState* state, uint8_t color)
{
    if (!state || !state->frameBuffer) {
        return;
    }

    const size_t activePixels = static_cast<size_t>(state->activeHeight) * kColecoFrameWidth;
    std::memset(state->frameBuffer, color, activePixels);
}

void coleco_vdp_render_graphics1(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t colorBase = static_cast<uint32_t>(state->regs[3]) << 6;
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = coleco_vdp_vscroll(state);

    for (unsigned y = 0; y < kColecoFrameHeightColeco1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + vscroll);
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;
        const uint32_t nameRowBase = nameBase + row * 32u;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = coleco_vdp_read_vram_fast(vram, mask, nameRowBase + tileX);
            const uint8_t pattern = coleco_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const uint8_t color = coleco_vdp_read_vram_fast(vram, mask, colorBase + (name >> 3));
            const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            coleco_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        coleco_vdp_render_mono_sprites_line(state, y, dst);
    }
}

void coleco_vdp_render_text40(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    coleco_vdp_clear_active_frame(state, bg);

    for (unsigned y = 0; y < kColecoFrameHeightColeco1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth + 8u;

        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t name = coleco_vdp_read_vram_fast(vram, mask, nameBase + row * 40u + col);
            const uint8_t pattern = coleco_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const unsigned pixelBase = col * 6u;

            for (unsigned bit = 0; bit < 6; ++bit) {
                dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }
    }
}

void coleco_vdp_render_graphics2_like(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t colorBase = coleco_vdp_color_base(state);
    const uint32_t patternBase = coleco_vdp_graphics2_pattern_base(state);
    const uint32_t colorMask = coleco_vdp_color_mask(state);
    const uint32_t patternMask = coleco_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = coleco_vdp_vscroll(state);

    for (unsigned y = 0; y < kColecoFrameHeightColeco1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + vscroll);
        // Follow the fMSX SCREEN 2 addressing scheme directly:
        // T = ChrTab + ((Y & 0xF8) << 2)
        // I = ((Y & 0xC0) << 5) + (Y & 0x07)
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;
        const uint32_t nameLineBase = nameBase + nameIndex;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = coleco_vdp_read_vram_fast(vram, mask, nameLineBase + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = coleco_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = coleco_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            coleco_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        coleco_vdp_render_mono_sprites_line(state, y, dst);
    }
}

void coleco_vdp_render_text80(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t nameMask = coleco_vdp_name_mask(state);
    const uint32_t colorBase = coleco_vdp_color_base(state);
    const uint32_t colorMask = coleco_vdp_color_mask(state);
    const uint32_t patternBase = coleco_vdp_pattern_base(state);
    const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t altFg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] >> 4));
    const uint8_t altBg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    coleco_vdp_clear_active_frame(state, bg);

    for (unsigned y = 0; y < kColecoFrameHeightColeco1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t patternLine = static_cast<uint32_t>(scrolledY & 0x07u);
        const uint32_t nameOffset = (static_cast<uint32_t>(80u) * (scrolledY >> 3)) & nameMask;
        const uint32_t colorOffset = (static_cast<uint32_t>(10u) * (scrolledY >> 3)) & colorMask;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth + 8u;

        for (unsigned col = 0; col < 80u; ++col) {
            const uint8_t glyph = coleco_vdp_read_vram_fast(vram, mask, nameBase + nameOffset + col);
            const uint8_t attrMask = coleco_vdp_read_vram_fast(vram, mask, colorBase + colorOffset + (col >> 3));
            const bool useAlt = (attrMask & static_cast<uint8_t>(0x80u >> (col & 0x07u))) != 0u;
            const uint8_t pixel = coleco_vdp_read_vram_fast(vram, mask, patternBase + static_cast<uint32_t>(glyph) * 8u + patternLine);
            const uint8_t drawFg = useAlt ? altFg : fg;
            const uint8_t drawBg = useAlt ? altBg : bg;
            const unsigned pixelBase = col * 3u;
            dst[pixelBase + 0u] = (pixel & 0xC0u) != 0u ? drawFg : drawBg;
            dst[pixelBase + 1u] = (pixel & 0x30u) != 0u ? drawFg : drawBg;
            dst[pixelBase + 2u] = (pixel & 0x0Cu) != 0u ? drawFg : drawBg;
        }
    }
}

void coleco_vdp_render_multicolor(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < kColecoFrameHeightColeco1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned lineBlock = (scrolledY & 0x04u) != 0 ? 4u : 0u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = coleco_vdp_read_vram_fast(vram, mask, nameBase + row * 32u + tileX);
            const uint8_t color = coleco_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + lineBlock);
            const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            for (unsigned bit = 0; bit < 4; ++bit) {
                dst[pixelBase + bit] = fg;
                dst[pixelBase + 4u + bit] = bg;
            }
        }

        coleco_vdp_render_mono_sprites_line(state, y, dst);
    }
}

void coleco_vdp_render_graphics3(ColecoVdpState* state)
{
    const uint32_t nameBase = coleco_vdp_name_base(state);
    const uint32_t colorBase = coleco_vdp_color_base(state);
    const uint32_t patternBase = coleco_vdp_pattern_base(state);
    const uint32_t colorMask = coleco_vdp_color_mask(state);
    const uint32_t patternMask = coleco_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < state->activeHeight; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = coleco_vdp_read_vram_fast(vram, mask, nameBase + nameIndex + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = coleco_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = coleco_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = coleco_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            for (unsigned bit = 0; bit < 8u; ++bit) {
                const unsigned px = pixelBase + bit;
                const uint8_t spriteColor = spriteLine[px];
                dst[px] = spriteColor != 0u
                    ? coleco_vdp_resolve_color(state, spriteColor)
                    : (((pattern << bit) & 0x80u) != 0 ? fg : bg);
            }
        }
    }
}

void coleco_vdp_render_bitmap4(ColecoVdpState* state)
{
    const unsigned height = state->activeHeight;
    const uint32_t base = coleco_vdp_bitmap4_base(state);
    const uint32_t lineMask = coleco_vdp_name_mask(state) & 0x7FFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < height; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t lineBase = base + ((static_cast<uint32_t>(scrolledY) << 7) & lineMask);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;
        for (unsigned x = 0; x < kColecoFrameWidth; x += 2u) {
            const uint8_t packed = coleco_vdp_read_vram_fast(vram, mask, lineBase + (x >> 1));
            const uint8_t left = static_cast<uint8_t>((packed >> 4) & 0x0Fu);
            const uint8_t right = static_cast<uint8_t>(packed & 0x0Fu);
            dst[x] = spriteLine[x] != 0u ? coleco_vdp_resolve_color(state, spriteLine[x]) : coleco_vdp_resolve_color(state, left);
            dst[x + 1u] = spriteLine[x + 1u] != 0u ? coleco_vdp_resolve_color(state, spriteLine[x + 1u]) : coleco_vdp_resolve_color(state, right);
        }
    }
}

void coleco_vdp_render_bitmap6(ColecoVdpState* state)
{
    const unsigned height = state->activeHeight;
    const uint32_t base = coleco_vdp_name_base(state);
    const uint32_t lineMask = coleco_vdp_name_mask(state) & 0x7FFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < height; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t lineBase = base + ((static_cast<uint32_t>(scrolledY) << 7) & lineMask);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned block = 0; block < 32u; ++block) {
            const uint32_t blockBase = lineBase + block * 4u;
            const uint8_t b0 = coleco_vdp_read_vram_fast(vram, mask, blockBase + 0u);
            const uint8_t b1 = coleco_vdp_read_vram_fast(vram, mask, blockBase + 1u);
            const uint8_t b2 = coleco_vdp_read_vram_fast(vram, mask, blockBase + 2u);
            const uint8_t b3 = coleco_vdp_read_vram_fast(vram, mask, blockBase + 3u);
            const unsigned px = block * 8u;
            const uint8_t pixels[8] = {
                static_cast<uint8_t>(b0 >> 6),
                static_cast<uint8_t>((b0 >> 2) & 0x03u),
                static_cast<uint8_t>(b1 >> 6),
                static_cast<uint8_t>((b1 >> 2) & 0x03u),
                static_cast<uint8_t>(b2 >> 6),
                static_cast<uint8_t>((b2 >> 2) & 0x03u),
                static_cast<uint8_t>(b3 >> 6),
                static_cast<uint8_t>((b3 >> 2) & 0x03u),
            };

            for (unsigned i = 0; i < 8u; ++i) {
                dst[px + i] = spriteLine[px + i] != 0u
                    ? coleco_vdp_resolve_color(state, spriteLine[px + i])
                    : coleco_vdp_resolve_color(state, pixels[i]);
            }
        }
    }
}

void coleco_vdp_render_bitmap7(ColecoVdpState* state)
{
    const unsigned height = state->activeHeight;
    const uint32_t base = coleco_vdp_name_base(state);
    const uint32_t lineMask = coleco_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < height; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t lineBase = base + ((static_cast<uint32_t>(scrolledY) << 8) & lineMask);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned block = 0; block < 32u; ++block) {
            const uint32_t blockBase = lineBase + block * 8u;
            const unsigned px = block * 8u;
            for (unsigned i = 0; i < 8u; ++i) {
                const uint8_t pixel = static_cast<uint8_t>(coleco_vdp_read_vram_fast(vram, mask, blockBase + i) >> 4);
                dst[px + i] = spriteLine[px + i] != 0u
                    ? coleco_vdp_resolve_color(state, spriteLine[px + i])
                    : coleco_vdp_resolve_color(state, pixel);
            }
        }
    }
}

void coleco_vdp_render_bitmap8(ColecoVdpState* state)
{
    const unsigned height = state->activeHeight;
    const uint32_t base = coleco_vdp_name_base(state);
    const uint32_t lineMask = coleco_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < height; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        const uint32_t lineBase = base + ((static_cast<uint32_t>(scrolledY) << 8) & lineMask);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned block = 0; block < 32u; ++block) {
            const uint32_t blockBase = lineBase + block * 8u;
            const unsigned px = block * 8u;
            for (unsigned i = 0; i < 8u; ++i) {
                const uint8_t sprite = spriteLine[px + i];
                dst[px + i] = sprite != 0u
                    ? coleco_vdp_screen8_mapped_color(sprite)
                    : coleco_vdp_read_vram_fast(vram, mask, blockBase + i);
            }
        }
    }
}

void coleco_vdp_render_yjk(ColecoVdpState* state, bool yae)
{
    const unsigned height = state->activeHeight;
    uint32_t base = coleco_vdp_name_base(state);
    const uint32_t lineMask = coleco_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t backdrop = coleco_vdp_reg_backdrop(state);
    const uint16_t hscroll = yae ? 0u : coleco_vdp_hscroll(state);
    const bool hscroll512 = !yae && coleco_vdp_hscroll512(state) && (hscroll > 255u);

    for (unsigned y = 0; y < height; ++y) {
        coleco_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t scrolledY = static_cast<uint8_t>(y + coleco_vdp_vscroll(state));
        uint32_t lineBase = base + ((static_cast<uint32_t>(scrolledY) << 8) & lineMask);
        if (hscroll512) {
            lineBase += 0x10000u;
        }
        lineBase += static_cast<uint32_t>(hscroll & 0xFCu);
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kColecoFrameWidth;

        for (unsigned i = 0; i < 4u; ++i) {
            const uint8_t sprite = spriteLine[i];
            dst[i] = sprite != 0u ? sprite : backdrop;
        }

        for (unsigned group = 0; group < 63u; ++group) {
            const uint32_t groupBase = lineBase + group * 4u;
            const uint8_t t0 = coleco_vdp_read_vram_fast(vram, mask, groupBase + 0u);
            const uint8_t t1 = coleco_vdp_read_vram_fast(vram, mask, groupBase + 1u);
            const uint8_t t2 = coleco_vdp_read_vram_fast(vram, mask, groupBase + 2u);
            const uint8_t t3 = coleco_vdp_read_vram_fast(vram, mask, groupBase + 3u);
            int k = static_cast<int>((t0 & 0x07u) | ((t1 & 0x07u) << 3));
            int j = static_cast<int>((t2 & 0x07u) | ((t3 & 0x07u) << 3));
            if ((k & 0x20) != 0) {
                k -= 64;
            }
            if ((j & 0x20) != 0) {
                j -= 64;
            }

            const uint8_t pixels[4] = {t0, t1, t2, t3};
            const unsigned pixelBase = 4u + group * 4u;
            for (unsigned i = 0; i < 4u; ++i) {
                const uint8_t sprite = spriteLine[pixelBase + i];
                if (sprite != 0u) {
                    dst[pixelBase + i] = sprite;
                    continue;
                }

                const uint8_t yv = static_cast<uint8_t>(pixels[i] >> 3);
                if (yae && (yv & 0x01u) != 0u) {
                    dst[pixelBase + i] = static_cast<uint8_t>(yv >> 1);
                } else {
                    dst[pixelBase + i] = coleco_vdp_yjk_color_index(static_cast<int>(yv), j, k);
                }
            }
        }
    }
}

bool coleco_vdp_register_affects_output(uint8_t reg)
{
    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 7:
        case 8:
        case 9:
        case 10:
        case 16:
        case 23:
        case 25:
        case 26:
        case 27:
            return true;
        case 5:
        case 6:
        case 11:
        case 12:
        case 13:
            return true;
        default:
            return false;
    }
}

uint32_t coleco_vdp_compose_address(const ColecoVdpState* state, uint16_t low14)
{
    uint32_t address = static_cast<uint32_t>(low14 & 0x3FFFu);
    if (coleco_vdp_is_msx2(state)) {
        address |= static_cast<uint32_t>(state->regs[14] & 0x07u) << 14;
    }
    return address & state->vramMask;
}

void coleco_vdp_advance_address(ColecoVdpState* state)
{
    if (!state) {
        return;
    }

    state->address = (state->address + 1u) & state->vramMask;
    if (coleco_vdp_is_msx2(state) && state->vramSize > 0x4000u) {
        state->regs[14] = static_cast<uint8_t>((state->address >> 14) & 0x07u);
    }
}

} // namespace

bool coleco_vdp_init(ColecoVdpState* state, ColecoMachineMode machineMode)
{
    if (!state) {
        return false;
    }

    coleco_vdp_init_pattern_expand_table();

    std::memset(state, 0, sizeof(*state));

    if (!s_msxFrameBuffer) {
        s_msxFrameBuffer = g_emu_static_pool;
        s_msx1Vram = (uint8_t*)heap_caps_malloc(kColeco1VramSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_msx1VramB = nullptr; // Fallback to sync rendering to save 16KB of internal RAM
        std::printf("[VDP] alloc: frameBuffer=%p(static) vram=%p vramB=%p freeInternal=%u largestBlock=%u\n",
            (void*)s_msxFrameBuffer, (void*)s_msx1Vram, (void*)s_msx1VramB,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (!s_msx1Vram) {
            std::printf("[VDP] init failed: Coleco VRAM alloc failed size=%u freeInternal=%u largestBlock=%u\n",
                        static_cast<unsigned>(kColeco1VramSize),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            s_msxFrameBuffer = nullptr;
            std::memset(state, 0, sizeof(*state));
            return false;
        }
    }

    state->machineMode = machineMode;
    state->vramSize = (machineMode == ColecoMachineMode::MSX2) ? kColeco2VramSize : kColeco1VramSize;
    state->vramMask = static_cast<uint32_t>(state->vramSize - 1u);
    state->ownsVram = false;

    if (machineMode == ColecoMachineMode::MSX2) {
        // MSX2 still uses heap-backed VRAM because the 64 KB image is too large to keep
        // permanently in static RAM on Cardputer ADV.
        state->vram = static_cast<uint8_t*>(heap_caps_malloc(state->vramSize, MALLOC_CAP_INTERNAL));
        if (!state->vram) {
            std::printf("[MSX] vdp init: vram alloc failed size=%u freeInternal=%u largestInternal=%u\n",
                        static_cast<unsigned>(state->vramSize),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            std::memset(state, 0, sizeof(*state));

            return false;
        }
        state->ownsVram = true;
    } else {
        state->vram = s_msx1Vram;

        if (!s_vdpRenderSem) {
            s_vdpRenderSem = xSemaphoreCreateBinary();
        }
        if (!s_vdpRenderTask) {
            s_vdpTaskRunning = true;
            xTaskCreatePinnedToCore(coleco_vdp_render_task, "COLECO_VDP", 4096, nullptr, 2, &s_vdpRenderTask, 0);
        }
    }

    state->frameBuffer = s_msxFrameBuffer;

    coleco_vdp_reset(state);
    return true;
}

void coleco_vdp_shutdown(ColecoVdpState* state)
{
    if (!state) {
        return;
    }

    if (s_vdpRenderTask) {
        s_vdpTaskRunning = false;
        xSemaphoreGive(s_vdpRenderSem);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_vdpRenderTask = nullptr;
    }
    if (s_vdpRenderSem) {
        vSemaphoreDelete(s_vdpRenderSem);
        s_vdpRenderSem = nullptr;
    }

    if (state->ownsVram && state->vram) {
        heap_caps_free(state->vram);
    }

    std::memset(state, 0, sizeof(*state));
}

void coleco_vdp_reset(ColecoVdpState* state)
{
    if (!state || !state->vram || !state->frameBuffer) {
        return;
    }

    std::memset(state->vram, 0x00, state->vramSize);
    std::memcpy(state->regs, kColecoVdpRegsInit, sizeof(state->regs));
    std::memcpy(state->status, kColecoVdpStatusInit, sizeof(state->status));
    std::memset(state->frameBuffer, 0x01, kColecoFramePixels);
    std::memset(state->paletteRaw, 0x00, sizeof(state->paletteRaw));

    state->readBuffer = 0;
    state->address = 0;
    state->latchedControl = 0;
    state->paletteLatch = 0;
    state->palettePending = false;
    state->controlPending = false;
    state->dirty = true;
    state->frameReady = false;
    state->frameCounter = 0;
    if (coleco_vdp_is_msx2(state)) {
        state->regs[8] = 0x08u;
        state->regs[16] = 0u;
        state->regs[17] = 0u;
    }
    coleco_vdp_init_palette(state);
    coleco_vdp_update_mode_geometry(state);
}

bool coleco_vdp_begin_frame(ColecoVdpState* state)
{
    if (!state) {
        return false;
    }

    state->status[0] |= 0x80u;
    state->status[2] = 0x5Cu;
    state->frameCounter++;
    return (state->regs[1] & 0x20u) != 0u;
}

extern bool g_emu_skip_video;

static void coleco_vdp_render_internal(ColecoVdpState* state)
{
    if (!coleco_vdp_display_enabled(state)) {
        coleco_vdp_clear_active_frame(state, coleco_vdp_resolve_color(state, 0));
        return;
    }

    switch (state->mode) {
        case ColecoVdpMode::Graphics1:
            coleco_vdp_render_graphics1(state);
            break;
        case ColecoVdpMode::Text40:
            coleco_vdp_render_text40(state);
            break;
        case ColecoVdpMode::Graphics2:
            coleco_vdp_render_graphics2_like(state);
            break;
        case ColecoVdpMode::Graphics3:
            coleco_vdp_render_graphics3(state);
            break;
        case ColecoVdpMode::Multicolor:
            coleco_vdp_render_multicolor(state);
            break;
        case ColecoVdpMode::Bitmap4:
            coleco_vdp_render_bitmap4(state);
            break;
        case ColecoVdpMode::Bitmap6:
            coleco_vdp_render_bitmap6(state);
            break;
        case ColecoVdpMode::Bitmap7:
            if (coleco_vdp_mode_yjk(state)) {
                coleco_vdp_render_yjk(state, coleco_vdp_mode_yae(state));
            } else {
                coleco_vdp_render_bitmap7(state);
            }
            break;
        case ColecoVdpMode::Bitmap8:
            if (coleco_vdp_mode_yjk(state)) {
                coleco_vdp_render_yjk(state, coleco_vdp_mode_yae(state));
            } else {
                coleco_vdp_render_bitmap8(state);
            }
            break;
        case ColecoVdpMode::Text80:
            coleco_vdp_render_text80(state);
            break;
        case ColecoVdpMode::Unsupported:
        default:
            coleco_vdp_clear_active_frame(state, coleco_vdp_resolve_color(state, 0));
            break;
    }
}

void coleco_vdp_render(ColecoVdpState* state)
{
    if (!state || !state->frameBuffer) {
        return;
    }

    if (!state->dirty && state->frameReady) {
        return;
    }

    coleco_vdp_update_mode_geometry(state);
    coleco_vdp_reset_sprite_status(state);

    if (s_vdpRenderSem && state->machineMode == ColecoMachineMode::MSX1 && s_msx1VramB) {
        int64_t t0 = esp_timer_get_time();
        std::memcpy(s_msx1VramB, state->vram, kColeco1VramSize);
        std::memcpy(&s_vdpStateSnapshot, state, sizeof(ColecoVdpState));
        int64_t t1 = esp_timer_get_time();
        s_vdpStatCopyUs += static_cast<uint32_t>(t1 - t0);

        s_vdpStateSnapshot.vram = s_msx1VramB;
        s_vdpOriginalState = state;
        state->dirty = false;
        state->frameReady = true;
        if (xSemaphoreGive(s_vdpRenderSem) != pdTRUE) {
            s_vdpStatDrops++;
        }
    } else {
        if (g_emu_skip_video) {
            return;
        }

        coleco_vdp_render_internal(state);
        state->dirty = false;
        state->frameReady = true;

        ColecoDisplayFrame frame = {};
        coleco_vdp_get_display_frame(state, &frame);
        coleco_video_present_frame(&frame);
    }
}

uint8_t coleco_vdp_in_data(ColecoVdpState* state)
{
    if (!state || !state->vram) {
        return 0xFFu;
    }

    const uint8_t value = state->readBuffer;
    state->readBuffer = coleco_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
    coleco_vdp_advance_address(state);
    state->controlPending = false;
    return value;
}

uint8_t coleco_vdp_in_status(ColecoVdpState* state)
{
    if (!state) {
        return 0xFFu;
    }

    uint8_t index = 0u;
    if (coleco_vdp_is_msx2(state)) {
        index = static_cast<uint8_t>(state->regs[15] & 0x0Fu);
        if (index >= sizeof(state->status)) {
            index = 0u;
        }
    }

    const uint8_t value = state->status[index];
    if (index == 0u) {
        state->status[0] &= 0x5Fu;
    } else if (index == 1u) {
        state->status[1] &= 0xFEu;
    } else if (index == 7u && coleco_vdp_is_msx2(state)) {
        state->status[7] = state->regs[44] = coleco_vdp_command_read(state);
    }
    return value;
}

void coleco_vdp_out_data(ColecoVdpState* state, uint8_t value)
{
    if (!state || !state->vram) {
        return;
    }

    coleco_vdp_write_vram(state, state->address, value);
    coleco_vdp_advance_address(state);
    state->readBuffer = value;
    state->controlPending = false;
}

void coleco_vdp_out_control(ColecoVdpState* state, uint8_t value)
{
    if (!state) {
        return;
    }

    if (!state->controlPending) {
        state->latchedControl = value;
        state->controlPending = true;
        return;
    }

    const uint8_t command = static_cast<uint8_t>(value >> 6);
    const uint16_t low14 = static_cast<uint16_t>(((value & 0x3Fu) << 8) | state->latchedControl);
    state->controlPending = false;

    if (command == 0u) {
        state->address = coleco_vdp_compose_address(state, low14);
        state->readBuffer = coleco_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
        coleco_vdp_advance_address(state);
        return;
    }

    if (command == 1u) {
        state->address = coleco_vdp_compose_address(state, low14);
        return;
    }

    const uint8_t reg = coleco_vdp_is_msx2(state) ? static_cast<uint8_t>(value & 0x3Fu) : static_cast<uint8_t>(value & 0x07u);
    coleco_vdp_write_register(state, reg, state->latchedControl);
}

void coleco_vdp_out_palette(ColecoVdpState* state, uint8_t value)
{
    if (!state || !coleco_vdp_is_msx2(state)) {
        return;
    }

    const uint8_t index = static_cast<uint8_t>(state->regs[16] & 0x0Fu);
    if (!state->palettePending) {
        state->paletteLatch = value;
        state->palettePending = true;
        return;
    }

    state->paletteRaw[index][0] = state->paletteLatch;
    state->paletteRaw[index][1] = value;
    coleco_vdp_apply_palette_entry(state, index);
    state->regs[16] = static_cast<uint8_t>((index + 1u) & 0x0Fu);
    state->palettePending = false;
    state->dirty = true;
}

void coleco_vdp_out_indirect(ColecoVdpState* state, uint8_t value)
{
    if (!state || !coleco_vdp_is_msx2(state)) {
        return;
    }

    uint8_t reg = static_cast<uint8_t>(state->regs[17] & 0x3Fu);
    if (reg != 17u && reg < sizeof(state->regs)) {
        coleco_vdp_write_register(state, reg, value);
    }

    if ((state->regs[17] & 0x80u) == 0u) {
        const uint8_t nextReg = static_cast<uint8_t>((reg + 1u) & 0x3Fu);
        state->regs[17] = static_cast<uint8_t>((state->regs[17] & 0x80u) | nextReg);
    }
}

void coleco_vdp_get_display_frame(const ColecoVdpState* state, ColecoDisplayFrame* frame)
{
    if (!frame) {
        return;
    }

    std::memset(frame, 0, sizeof(*frame));
    if (!state || !state->frameBuffer || !state->frameReady) {
        return;
    }

    frame->indexed8 = state->frameBuffer;
    frame->palette565 = ((state->mode == ColecoVdpMode::Bitmap8) || coleco_vdp_mode_yjk(state))
        ? state->screen8Palette565
        : state->palette565;
    frame->paletteEntryCount = ((state->mode == ColecoVdpMode::Bitmap8) || coleco_vdp_mode_yjk(state)) ? 256u : 16u;
    frame->width = state->activeWidth;
    frame->height = state->activeHeight;
    frame->pitchBytes = kColecoFrameWidth;
}

const char* coleco_vdp_mode_label(ColecoVdpMode mode)
{
    switch (mode) {
        case ColecoVdpMode::Graphics1:
            return "G1";
        case ColecoVdpMode::Text40:
            return "TEXT";
        case ColecoVdpMode::Graphics2:
            return "G2";
        case ColecoVdpMode::Multicolor:
            return "MC";
        case ColecoVdpMode::Graphics3:
            return "G3";
        case ColecoVdpMode::Bitmap4:
            return "G4";
        case ColecoVdpMode::Bitmap6:
            return "G6";
        case ColecoVdpMode::Bitmap7:
            return "G7";
        case ColecoVdpMode::Bitmap8:
            return "G8";
        case ColecoVdpMode::Text80:
            return "TXT80";
        case ColecoVdpMode::Unsupported:
        default:
            return "UNSUP";
    }
}

bool coleco_vdp_display_enabled(const ColecoVdpState* state)
{
    return state && (state->regs[1] & 0x40u) != 0;
}
