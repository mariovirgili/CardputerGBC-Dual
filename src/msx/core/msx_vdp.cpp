#include "msx_vdp.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../msx_display.h"

namespace {

constexpr size_t kMsx1VramSize = 0x4000;
constexpr size_t kMsx2VramSize = 0x10000;
constexpr unsigned kMsxFrameWidth = 256;
constexpr unsigned kMsxFrameHeightMsx1 = 192;
constexpr unsigned kMsxFrameHeightMsx2 = 212;
constexpr size_t kMsxFramePixels = static_cast<size_t>(kMsxFrameWidth) * kMsxFrameHeightMsx2;

// The Cardputer runs only one MSX core instance at a time, so a single
// shared indexed frame buffer avoids large heap allocations and fragmentation
// during VDP startup.
static uint8_t s_msxFrameBuffer[kMsxFramePixels];
static uint8_t s_msx1Vram[kMsx1VramSize];

constexpr uint16_t msx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

bool msx_vdp_is_msx2(const MsxVdpState* state)
{
    return state && state->machineMode == MsxMachineMode::MSX2;
}

inline uint8_t msx_vdp_expand3(uint8_t value)
{
    value &= 0x07u;
    return static_cast<uint8_t>((value << 5) | (value << 2) | (value >> 1));
}

inline uint8_t msx_vdp_read_vram_fast(const uint8_t* vram, uint32_t mask, uint32_t address)
{
    return vram[address & mask];
}

void msx_vdp_apply_palette_entry(MsxVdpState* state, uint8_t index)
{
    if (!state || index >= 16u) {
        return;
    }

    const uint8_t raw1 = state->paletteRaw[index][0];
    const uint8_t raw2 = state->paletteRaw[index][1];
    const uint8_t red = msx_vdp_expand3(static_cast<uint8_t>((raw1 >> 4) & 0x07u));
    const uint8_t blue = msx_vdp_expand3(static_cast<uint8_t>(raw1 & 0x07u));
    const uint8_t green = msx_vdp_expand3(static_cast<uint8_t>(raw2 & 0x07u));
    state->palette565[index] = msx_rgb565(red, green, blue);
}

void msx_vdp_init_palette(MsxVdpState* state)
{
    static constexpr uint8_t kMsx1Palette[16][2] = {
        {0x00, 0x00}, {0x00, 0x00}, {0x11, 0x05}, {0x33, 0x06},
        {0x26, 0x02}, {0x37, 0x03}, {0x52, 0x02}, {0x27, 0x06},
        {0x62, 0x02}, {0x63, 0x03}, {0x52, 0x05}, {0x63, 0x06},
        {0x11, 0x04}, {0x55, 0x02}, {0x55, 0x05}, {0x77, 0x07},
    };

    for (uint8_t i = 0; i < 16u; ++i) {
        state->paletteRaw[i][0] = kMsx1Palette[i][0];
        state->paletteRaw[i][1] = kMsx1Palette[i][1];
        msx_vdp_apply_palette_entry(state, i);
    }
}

inline uint8_t msx_vdp_reg_backdrop(const MsxVdpState* state)
{
    return state ? static_cast<uint8_t>(state->regs[7] & 0x0Fu) : 1u;
}

inline uint8_t msx_vdp_resolve_color(const MsxVdpState* state, uint8_t color)
{
    const uint8_t backdrop = msx_vdp_reg_backdrop(state);
    if ((color & 0x0Fu) == 0u) {
        return backdrop == 0u ? 1u : backdrop;
    }
    return static_cast<uint8_t>(color & 0x0Fu);
}

inline void msx_vdp_write_vram(MsxVdpState* state, uint32_t address, uint8_t value)
{
    const uint32_t wrapped = address & state->vramMask;
    if (state->vram[wrapped] != value) {
        state->vram[wrapped] = value;
        state->dirty = true;
    }
}

uint32_t msx_vdp_name_base(const MsxVdpState* state)
{
    return static_cast<uint32_t>(state->regs[2] & 0x7Fu) << 10;
}

uint32_t msx_vdp_color_base(const MsxVdpState* state)
{
    const uint32_t high = msx_vdp_is_msx2(state) ? static_cast<uint32_t>(state->regs[10] & 0x07u) << 14 : 0u;
    const uint32_t low = static_cast<uint32_t>(state->regs[3] & 0x80u) << 6;
    return high | low;
}

uint32_t msx_vdp_pattern_base(const MsxVdpState* state)
{
    if (msx_vdp_is_msx2(state)) {
        return static_cast<uint32_t>(state->regs[4] & 0x3Cu) << 11;
    }
    return static_cast<uint32_t>(state->regs[4] & 0x04u) << 11;
}

uint32_t msx_vdp_bitmap4_base(const MsxVdpState* state)
{
    return static_cast<uint32_t>((state->regs[2] >> 5) & 0x03u) << 15;
}

MsxVdpMode msx_vdp_decode_mode(const MsxVdpState* state)
{
    if (!state) {
        return MsxVdpMode::Unsupported;
    }

    const uint8_t m1 = static_cast<uint8_t>((state->regs[1] >> 4) & 0x01u);
    const uint8_t m2 = static_cast<uint8_t>((state->regs[1] >> 3) & 0x01u);
    const uint8_t m3 = static_cast<uint8_t>((state->regs[0] >> 1) & 0x01u);
    const uint8_t m4 = static_cast<uint8_t>((state->regs[0] >> 2) & 0x01u);
    const uint8_t m5 = static_cast<uint8_t>((state->regs[0] >> 3) & 0x01u);

    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Graphics1;
    }
    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 1u) {
        return MsxVdpMode::Text40;
    }
    if (m5 == 0u && m4 == 0u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Graphics2;
    }
    if (m5 == 0u && m4 == 0u && m3 == 0u && m2 == 1u && m1 == 0u) {
        return MsxVdpMode::Multicolor;
    }
    if (msx_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Graphics3;
    }
    if (msx_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Bitmap4;
    }
    return MsxVdpMode::Unsupported;
}

void msx_vdp_update_mode_geometry(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    state->mode = msx_vdp_decode_mode(state);
    state->activeWidth = kMsxFrameWidth;
    state->activeHeight = kMsxFrameHeightMsx1;
    if (msx_vdp_is_msx2(state) &&
        state->mode == MsxVdpMode::Bitmap4 &&
        (state->regs[9] & 0x80u) != 0u) {
        state->activeHeight = kMsxFrameHeightMsx2;
    }
}

void msx_vdp_clear_active_frame(MsxVdpState* state, uint8_t color)
{
    if (!state || !state->frameBuffer) {
        return;
    }

    const size_t activePixels = static_cast<size_t>(state->activeHeight) * kMsxFrameWidth;
    std::memset(state->frameBuffer, color, activePixels);
}

void msx_vdp_render_graphics1(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = static_cast<uint32_t>(state->regs[3]) << 6;
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const unsigned row = y >> 3;
        const unsigned line = y & 0x07u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 32u + tileX);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (name >> 3));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            for (unsigned bit = 0; bit < 8; ++bit) {
                dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }
    }
}

void msx_vdp_render_text40(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    msx_vdp_clear_active_frame(state, bg);

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const unsigned row = y >> 3;
        const unsigned line = y & 0x07u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth + 8u;

        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 40u + col);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const unsigned pixelBase = col * 6u;

            for (unsigned bit = 0; bit < 6; ++bit) {
                dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }
    }
}

void msx_vdp_render_graphics2_like(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const unsigned section = y >> 6;
        const unsigned row = y >> 3;
        const unsigned line = y & 0x07u;
        const uint32_t sectionOffset = static_cast<uint32_t>(section) * 0x800u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 32u + tileX);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + sectionOffset + name * 8u + line);
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + sectionOffset + name * 8u + line);
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            for (unsigned bit = 0; bit < 8; ++bit) {
                dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }
    }
}

void msx_vdp_render_multicolor(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const unsigned row = y >> 3;
        const unsigned lineBlock = (y & 0x04u) != 0 ? 4u : 0u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 32u + tileX);
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + lineBlock);
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            for (unsigned bit = 0; bit < 4; ++bit) {
                dst[pixelBase + bit] = fg;
                dst[pixelBase + 4u + bit] = bg;
            }
        }
    }
}

void msx_vdp_render_bitmap4(MsxVdpState* state)
{
    const unsigned height = state->activeHeight;
    const uint32_t base = msx_vdp_bitmap4_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < height; ++y) {
        const uint32_t lineBase = base + static_cast<uint32_t>(y) * 128u;
        uint8_t* dst = state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth;
        for (unsigned x = 0; x < kMsxFrameWidth; x += 2u) {
            const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + (x >> 1));
            dst[x] = static_cast<uint8_t>((packed >> 4) & 0x0Fu);
            dst[x + 1u] = static_cast<uint8_t>(packed & 0x0Fu);
        }
    }
}

bool msx_vdp_register_affects_output(uint8_t reg)
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
            return true;
        default:
            return false;
    }
}

uint32_t msx_vdp_compose_address(const MsxVdpState* state, uint16_t low14)
{
    uint32_t address = static_cast<uint32_t>(low14 & 0x3FFFu);
    if (msx_vdp_is_msx2(state)) {
        address |= static_cast<uint32_t>(state->regs[14] & 0x07u) << 14;
    }
    return address & state->vramMask;
}

void msx_vdp_advance_address(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    state->address = (state->address + 1u) & state->vramMask;
    if (msx_vdp_is_msx2(state) && state->vramSize > 0x4000u) {
        state->regs[14] = static_cast<uint8_t>((state->address >> 14) & 0x07u);
    }
}

} // namespace

bool msx_vdp_init(MsxVdpState* state, MsxMachineMode machineMode)
{
    if (!state) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->machineMode = machineMode;
    state->vramSize = (machineMode == MsxMachineMode::MSX2) ? kMsx2VramSize : kMsx1VramSize;
    state->vramMask = static_cast<uint32_t>(state->vramSize - 1u);
    state->ownsVram = false;

    if (machineMode == MsxMachineMode::MSX2) {
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
    }

    state->frameBuffer = s_msxFrameBuffer;

    msx_vdp_reset(state);
    return true;
}

void msx_vdp_shutdown(MsxVdpState* state)
{
    if (!state) {
        return;
    }


    if (state->ownsVram && state->vram) {
        heap_caps_free(state->vram);
    }

    std::memset(state, 0, sizeof(*state));
}

void msx_vdp_reset(MsxVdpState* state)
{
    if (!state || !state->vram || !state->frameBuffer) {
        return;
    }

    std::memset(state->vram, 0x00, state->vramSize);
    std::memset(state->regs, 0x00, sizeof(state->regs));
    std::memset(state->status, 0x00, sizeof(state->status));
    std::memset(state->frameBuffer, 0x01, kMsxFramePixels);
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
    state->regs[1] = 0x40u;
    if (msx_vdp_is_msx2(state)) {
        state->regs[8] = 0x08u;
        state->regs[16] = 0u;
        state->regs[17] = 0u;
    }
    state->status[2] = 0x1Cu;
    msx_vdp_init_palette(state);
    msx_vdp_update_mode_geometry(state);
}

bool msx_vdp_begin_frame(MsxVdpState* state)
{
    if (!state) {
        return false;
    }

    state->status[0] |= 0x80u;
    state->status[2] = 0x5Cu;
    state->frameCounter++;
    return (state->regs[1] & 0x20u) != 0u;
}

void msx_vdp_render(MsxVdpState* state)
{
    if (!state || !state->frameBuffer) {
        return;
    }

    if (!state->dirty && state->frameReady) {
        return;
    }

    msx_vdp_update_mode_geometry(state);

    if (!msx_vdp_display_enabled(state)) {
        msx_vdp_clear_active_frame(state, msx_vdp_resolve_color(state, 0));
        state->dirty = false;
        state->frameReady = true;
        return;
    }

    switch (state->mode) {
        case MsxVdpMode::Graphics1:
            msx_vdp_render_graphics1(state);
            break;
        case MsxVdpMode::Text40:
            msx_vdp_render_text40(state);
            break;
        case MsxVdpMode::Graphics2:
        case MsxVdpMode::Graphics3:
            msx_vdp_render_graphics2_like(state);
            break;
        case MsxVdpMode::Multicolor:
            msx_vdp_render_multicolor(state);
            break;
        case MsxVdpMode::Bitmap4:
            msx_vdp_render_bitmap4(state);
            break;
        case MsxVdpMode::Unsupported:
        default:
            msx_vdp_clear_active_frame(state, msx_vdp_resolve_color(state, 0));
            break;
    }

    state->dirty = false;
    state->frameReady = true;
}

uint8_t msx_vdp_in_data(MsxVdpState* state)
{
    if (!state || !state->vram) {
        return 0xFFu;
    }

    const uint8_t value = state->readBuffer;
    state->readBuffer = msx_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
    msx_vdp_advance_address(state);
    state->controlPending = false;
    return value;
}

uint8_t msx_vdp_in_status(MsxVdpState* state)
{
    if (!state) {
        return 0xFFu;
    }

    uint8_t index = 0u;
    if (msx_vdp_is_msx2(state)) {
        index = static_cast<uint8_t>(state->regs[15] & 0x0Fu);
        if (index >= sizeof(state->status)) {
            index = 0u;
        }
    }

    const uint8_t value = state->status[index];
    if (index == 0u) {
        state->status[0] &= 0x1Fu;
    } else if (index == 1u) {
        state->status[1] &= 0x1Fu;
    }
    state->controlPending = false;
    return value;
}

void msx_vdp_out_data(MsxVdpState* state, uint8_t value)
{
    if (!state || !state->vram) {
        return;
    }

    msx_vdp_write_vram(state, state->address, value);
    msx_vdp_advance_address(state);
    state->readBuffer = value;
}

void msx_vdp_out_control(MsxVdpState* state, uint8_t value)
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
        state->address = msx_vdp_compose_address(state, low14);
        state->readBuffer = msx_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
        msx_vdp_advance_address(state);
        return;
    }

    if (command == 1u) {
        state->address = msx_vdp_compose_address(state, low14);
        return;
    }

    const uint8_t reg = msx_vdp_is_msx2(state) ? static_cast<uint8_t>(value & 0x3Fu) : static_cast<uint8_t>(value & 0x07u);
    if (reg >= sizeof(state->regs)) {
        return;
    }

    if (state->regs[reg] != state->latchedControl) {
        state->regs[reg] = state->latchedControl;
        if (msx_vdp_register_affects_output(reg)) {
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
}

void msx_vdp_out_palette(MsxVdpState* state, uint8_t value)
{
    if (!state || !msx_vdp_is_msx2(state)) {
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
    msx_vdp_apply_palette_entry(state, index);
    state->regs[16] = static_cast<uint8_t>((index + 1u) & 0x0Fu);
    state->palettePending = false;
    state->dirty = true;
}

void msx_vdp_out_indirect(MsxVdpState* state, uint8_t value)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return;
    }

    uint8_t reg = static_cast<uint8_t>(state->regs[17] & 0x3Fu);
    if (reg != 17u && reg < sizeof(state->regs)) {
        if (state->regs[reg] != value) {
            state->regs[reg] = value;
            if (msx_vdp_register_affects_output(reg)) {
                state->dirty = true;
            }
        }
    }

    if ((state->regs[17] & 0x80u) == 0u) {
        const uint8_t nextReg = static_cast<uint8_t>((reg + 1u) & 0x3Fu);
        state->regs[17] = static_cast<uint8_t>((state->regs[17] & 0x80u) | nextReg);
    }
}

void msx_vdp_get_display_frame(const MsxVdpState* state, MsxDisplayFrame* frame)
{
    if (!frame) {
        return;
    }

    std::memset(frame, 0, sizeof(*frame));
    if (!state || !state->frameBuffer || !state->frameReady) {
        return;
    }

    frame->indexed8 = state->frameBuffer;
    frame->palette565 = state->palette565;
    frame->width = state->activeWidth;
    frame->height = state->activeHeight;
    frame->pitchBytes = kMsxFrameWidth;
}

const char* msx_vdp_mode_label(MsxVdpMode mode)
{
    switch (mode) {
        case MsxVdpMode::Graphics1:
            return "G1";
        case MsxVdpMode::Text40:
            return "TEXT";
        case MsxVdpMode::Graphics2:
            return "G2";
        case MsxVdpMode::Multicolor:
            return "MC";
        case MsxVdpMode::Graphics3:
            return "G3";
        case MsxVdpMode::Bitmap4:
            return "G4";
        case MsxVdpMode::Unsupported:
        default:
            return "UNSUP";
    }
}

bool msx_vdp_display_enabled(const MsxVdpState* state)
{
    return state && (state->regs[1] & 0x40u) != 0;
}


