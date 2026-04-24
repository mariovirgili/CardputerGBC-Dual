#include "msx_vdp.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

#include "../msx_config.h"
#include "../msx_display.h"
#include "../msx_video.h"
#include "../../bench/v9938_bench_trace.hpp"

#ifndef MSX_VDP_TRACE_ENABLED
#define MSX_VDP_TRACE_ENABLED 0
#endif

#ifndef MSX_VDP_G4_DISP_LOG_ENABLED
#define MSX_VDP_G4_DISP_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_SPRITE_LOG_ENABLED
#define MSX_VDP_SPRITE_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_MODE_LOG_ENABLED
#define MSX_VDP_MODE_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_DUALCORE_STATS_LOG_ENABLED
#define MSX_VDP_DUALCORE_STATS_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_BOOT_DIAG_ENABLED
#define MSX_VDP_BOOT_DIAG_ENABLED 0
#endif

#ifndef MSX_VDP_VERBOSE_DIAG_ENABLED
#define MSX_VDP_VERBOSE_DIAG_ENABLED 0
#endif

#ifndef MSX_VDP_CMD_LOG_ENABLED
#define MSX_VDP_CMD_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_FIN_LOG_ENABLED
#define MSX_VDP_FIN_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_XFER_LOG_ENABLED
#define MSX_VDP_XFER_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_G4_ADDR_LOG_ENABLED
#define MSX_VDP_G4_ADDR_LOG_ENABLED 0
#endif

#ifndef MSX_VDP_INIT_LOG_ENABLED
#define MSX_VDP_INIT_LOG_ENABLED 0
#endif

// Cardputer target without PSRAM: keep the VDP on the single-core path.
// The dual-core renderer needs a full shadow VRAM copy to be safe, which
// costs both heap and per-frame memcpy bandwidth.
#ifndef MSX_VDP_DUALCORE_ENABLED
#define MSX_VDP_DUALCORE_ENABLED 0
#endif

bool msx_vdp_render_internal(MsxVdpState* state);

namespace {

bool msx_vdp_is_msx2(const MsxVdpState* state);
bool msx_vdp_mode_yjk(const MsxVdpState* state);
void msx_vdp_update_mode_geometry(MsxVdpState* state);
uint32_t msx_vdp_sprite_attr_base(const MsxVdpState* state);
uint32_t msx_vdp_sprite_pattern_base(const MsxVdpState* state);
uint8_t msx_vdp_vscroll(const MsxVdpState* state);
MsxVdpMode msx_vdp_decode_mode(const MsxVdpState* state);
uint8_t msx_vdp_vram_page_mask(const MsxVdpState* state);
bool msx_vdp_cpu_port_carries_into_r14(const MsxVdpState* state);
uint32_t msx_vdp_compose_address(const MsxVdpState* state, uint16_t low14);
void msx_vdp_advance_address(MsxVdpState* state);

constexpr size_t kMsx1VramSize = 0x4000;
constexpr size_t kMsx2VramSize = 0x20000;
constexpr unsigned kMsxFrameWidth = 256;
constexpr unsigned kMsxWideFrameWidth = 512;
constexpr unsigned kMsxFrameHeightMsx1 = 192;
constexpr unsigned kMsxFrameHeightMsx2 = 212;
constexpr unsigned kMsxSpriteColorLineWidth = kMsxFrameWidth + 64u;
constexpr size_t kMsxFramePixels = static_cast<size_t>(kMsxFrameWidth) * kMsxFrameHeightMsx2;
constexpr uint8_t kMsxMaxSpritesLineMsx1 = 4u;
constexpr uint8_t kMsxMaxSpritesLineMsx2 = 8u;
constexpr uint8_t kMsxVdpRegsInit[64] = {
    0x00, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kMsxVdpStatusInit[10] = {
    0x9F, 0x00, 0x6C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint32_t kMsxVdpMinVramSize = 0x4000u;

// The Cardputer runs only one MSX core instance at a time, so a single
// shared indexed frame buffer avoids large heap allocations and fragmentation
// during VDP startup.
static uint8_t* s_msxFrameBuffer = nullptr;
static uint8_t* s_msx1Vram = nullptr;
static uint8_t* s_msx1VramB = nullptr;
static uint8_t* s_msx2VramB = nullptr;
static uint8_t s_msxSpriteOccupancy[kMsxFrameWidth];
static uint8_t s_msxColorSpriteLine[kMsxSpriteColorLineWidth];
struct MsxMonoSpriteCachedAttr {
    int16_t y;
    int16_t x;
    uint8_t patternId;
    uint8_t attrColor;
};
struct MsxMonoSpriteLineCache {
    uint8_t selected[kMsxMaxSpritesLineMsx1];
    uint8_t count;
    uint8_t lastIndex;
    uint8_t overflowIndex;
    bool hasOverflow;
};
static MsxMonoSpriteCachedAttr s_msxMonoSpriteAttrs[32u];
static MsxMonoSpriteLineCache s_msxMonoSpriteLines[kMsxFrameHeightMsx1];
static bool s_msxMonoSpriteCacheValid = false;
static uint8_t s_msxMonoSpriteInputHeight = 8u;
static uint8_t s_msxMonoSpriteScale = 1u;
static uint8_t s_msxColorSpriteAttrSnapshot[32u * 4u];
static uint8_t s_msxColorSpriteColorSnapshot[32u * 16u];
static uint8_t s_msxColorSpritePatternSnapshot[256u * 8u];
struct __attribute__((packed)) MsxColorSpriteWriteEvent {
    uint16_t offset;
    uint16_t cycle;
    uint8_t table;
    uint8_t value;
};
static constexpr uint8_t kMsxColorSpriteWriteAttr = 0u;
static constexpr uint8_t kMsxColorSpriteWriteColor = 1u;
static constexpr uint8_t kMsxColorSpriteWritePattern = 2u;
static constexpr uint16_t kMsxColorSpriteWriteLogMax = 256u;
static MsxColorSpriteWriteEvent s_msxColorSpriteWriteLog[kMsxColorSpriteWriteLogMax];
static uint16_t s_msxColorSpriteWriteLogCount = 0u;
static uint16_t s_msxColorSpriteWriteLogApplyIndex = 0u;
static bool s_msxColorSpriteSnapshotValid = false;
static uint32_t s_msxColorSpriteSnapshotFrameTag = 0u;
static uint8_t s_msxColorSpriteSnapshotReg1 = 0u;
static uint8_t s_msxColorSpriteSnapshotReg23 = 0u;

// Ottimizzazione memoria: impacchetta la struttura (rimuove il padding, 5 byte invece di 8)
struct __attribute__((packed)) MsxVdpRegTimelineEvent {
    uint32_t cycle;
    uint8_t value;
};
// Ridotto da 16 a 8. Difficilmente un gioco modifica lo STESSO registro più di 8 volte a frame.
static constexpr uint8_t kMsxVdpRegTimelineMax = 8u;
struct MsxVdpRenderAuxState {
    MsxVdpRegTimelineEvent r5Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r6Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r8Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r11Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r2Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r23Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r25Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r26Timeline[kMsxVdpRegTimelineMax];
    MsxVdpRegTimelineEvent r27Timeline[kMsxVdpRegTimelineMax];
    uint8_t r5TimelineCount;
    uint8_t r6TimelineCount;
    uint8_t r8TimelineCount;
    uint8_t r11TimelineCount;
    uint8_t r2TimelineCount;
    uint8_t r23TimelineCount;
    uint8_t r25TimelineCount;
    uint8_t r26TimelineCount;
    uint8_t r27TimelineCount;
};
static MsxVdpRegTimelineEvent s_msxVdpR5Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR6Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR8Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR11Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR2Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR23Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR25Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR26Timeline[kMsxVdpRegTimelineMax];
static MsxVdpRegTimelineEvent s_msxVdpR27Timeline[kMsxVdpRegTimelineMax];
static uint8_t s_msxVdpR5TimelineCount = 0u;
static uint8_t s_msxVdpR6TimelineCount = 0u;
static uint8_t s_msxVdpR8TimelineCount = 0u;
static uint8_t s_msxVdpR11TimelineCount = 0u;
static uint8_t s_msxVdpR2TimelineCount = 0u;
static uint8_t s_msxVdpR23TimelineCount = 0u;
static uint8_t s_msxVdpR25TimelineCount = 0u;
static uint8_t s_msxVdpR26TimelineCount = 0u;
static uint8_t s_msxVdpR27TimelineCount = 0u;
static MsxVdpRenderAuxState s_vdpRenderAuxSnapshot = {};
static uint8_t s_msx2LineBuffer[kMsxWideFrameWidth];
static uint32_t s_msxSpriteActiveLines = 0;
static uint32_t s_msxSpriteSelectedCount = 0;
static uint32_t s_msxSpriteColoredEntries = 0;
static uint32_t s_msxSpriteTransparentEntries = 0;
static uint32_t s_msxSpritePixelsDrawn = 0;
static MsxDisplayFrame s_msx2StreamVideoFrame = {};
static bool s_msx2StreamActive = false;
#if MSX_VDP_BOOT_DIAG_ENABLED
static uint32_t s_msxBootDiagLastFrame = 0xFFFFFFFFu;
static uint16_t s_msxBootDiagCount = 0u;
#endif

static bool msx_vdp_color_sprite_snapshot_is_empty(const uint8_t* spriteAttr)
{
    return spriteAttr &&
           spriteAttr[0] == 0xD8u &&
           spriteAttr[4] == 0xD8u &&
           spriteAttr[8] == 0xD8u &&
           spriteAttr[12] == 0xD8u;
}

static bool msx_vdp_color_sprite_snapshot_is_garbage(const uint8_t* spriteAttr,
                                                     const uint8_t* spriteColor)
{
    return spriteAttr && spriteColor &&
           spriteAttr[0] == 0xEEu &&
           spriteAttr[4] == 0xFFu &&
           spriteColor[0] == 0xEEu;
}

static uint32_t msx_vdp_cycle_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || state->frameCycleBudget == 0u) {
        return 0u;
    }

    const unsigned visibleHeight = state->activeHeight ? state->activeHeight : kMsxFrameHeightMsx2;
    if (visibleHeight == 0u) {
        return 0u;
    }

    // The VDP timing model advances CPU time across the full 262-line frame,
    // even when we only render the visible subset. Mapping line events onto the
    // visible height shifts R#2/R#23 changes and command progress too late and
    // can make page-based scrolling jump to the wrong bitmap page.
    static constexpr uint32_t kFrameScanlines = 262u;
    const unsigned clampedY = (y < visibleHeight) ? y : (visibleHeight - 1u);
    return static_cast<uint32_t>((static_cast<uint64_t>(clampedY) * state->frameCycleBudget) /
                                 static_cast<uint64_t>(kFrameScanlines));
}

#if MSX_VDP_G4_DISP_LOG_ENABLED
static uint32_t msx_vdp_cycle_for_line_visible_legacy(const MsxVdpState* state, unsigned y)
{
    if (!state || state->frameCycleBudget == 0u) {
        return 0u;
    }

    const unsigned visibleHeight = state->activeHeight ? state->activeHeight : kMsxFrameHeightMsx2;
    if (visibleHeight == 0u) {
        return 0u;
    }

    const unsigned clampedY = (y < visibleHeight) ? y : (visibleHeight - 1u);
    return static_cast<uint32_t>((static_cast<uint64_t>(clampedY) * state->frameCycleBudget) /
                                 static_cast<uint64_t>(visibleHeight));
}
#endif

static uint32_t msx_vdp_line_index_for_cycle(const MsxVdpState* state, uint32_t cycle)
{
    if (!state || state->frameCycleBudget == 0u) {
        return 0u;
    }

    static constexpr uint32_t kFrameScanlines = 262u;
    if (state->activeHeight == 0u) {
        return 0u;
    }

    if (cycle >= state->frameCycleBudget) {
        return kFrameScanlines;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(cycle) * static_cast<uint64_t>(kFrameScanlines)) /
                                 static_cast<uint64_t>(state->frameCycleBudget));
}

static void msx_vdp_log_color_sprite_write(MsxVdpState* state,
                                           uint8_t table,
                                           uint16_t offset,
                                           uint8_t value)
{
    if (!state || s_msxColorSpriteWriteLogCount >= kMsxColorSpriteWriteLogMax) {
        return;
    }

    MsxColorSpriteWriteEvent& event = s_msxColorSpriteWriteLog[s_msxColorSpriteWriteLogCount++];
    event.offset = offset;
    event.cycle = static_cast<uint16_t>((state->currentFrameCpuCycles <= 0xFFFFu)
                                            ? state->currentFrameCpuCycles
                                            : 0xFFFFu);
    event.table = table;
    event.value = value;
}

static void msx_vdp_apply_color_sprite_writes_until(uint32_t targetCycle)
{
    while (s_msxColorSpriteWriteLogApplyIndex < s_msxColorSpriteWriteLogCount) {
        const MsxColorSpriteWriteEvent& event = s_msxColorSpriteWriteLog[s_msxColorSpriteWriteLogApplyIndex];
        if (event.cycle > targetCycle) {
            break;
        }

        switch (event.table) {
            case kMsxColorSpriteWriteAttr:
                if (event.offset < sizeof(s_msxColorSpriteAttrSnapshot)) {
                    s_msxColorSpriteAttrSnapshot[event.offset] = event.value;
                }
                break;
            case kMsxColorSpriteWriteColor:
                if (event.offset < sizeof(s_msxColorSpriteColorSnapshot)) {
                    s_msxColorSpriteColorSnapshot[event.offset] = event.value;
                }
                break;
            case kMsxColorSpriteWritePattern:
                if (event.offset < sizeof(s_msxColorSpritePatternSnapshot)) {
                    s_msxColorSpritePatternSnapshot[event.offset] = event.value;
                }
                break;
            default:
                break;
        }

        ++s_msxColorSpriteWriteLogApplyIndex;
    }
}

static void msx_vdp_timeline_reset(MsxVdpRegTimelineEvent* timeline,
                                   uint8_t* count,
                                   uint8_t value)
{
    if (!timeline || !count) {
        return;
    }
    timeline[0].cycle = 0u;
    timeline[0].value = value;
    *count = 1u;
}

static void msx_vdp_timeline_append(MsxVdpRegTimelineEvent* timeline,
                                    uint8_t* count,
                                    uint32_t cycle,
                                    uint8_t value)
{
    if (!timeline || !count) {
        return;
    }
    if (*count != 0u && timeline[*count - 1u].value == value) {
        return;
    }

    if (*count < kMsxVdpRegTimelineMax) {
        timeline[*count].cycle = cycle;
        timeline[*count].value = value;
        (*count)++;
        return;
    }

    for (uint8_t i = 1u; i < kMsxVdpRegTimelineMax; ++i) {
        timeline[i - 1u] = timeline[i];
    }
    timeline[kMsxVdpRegTimelineMax - 1u].cycle = cycle;
    timeline[kMsxVdpRegTimelineMax - 1u].value = value;
}

static uint8_t msx_vdp_timeline_value_for_line(const MsxVdpState* state,
                                               const MsxVdpRegTimelineEvent* timeline,
                                               uint8_t count,
                                               unsigned y)
{
    if (!state || !timeline || count == 0u || state->activeHeight == 0u) {
        return 0u;
    }

    const uint32_t targetCycle = msx_vdp_cycle_for_line(state, y);

    uint8_t value = timeline[0].value;
    for (uint8_t i = 1u; i < count; ++i) {
        if (timeline[i].cycle > targetCycle) {
            break;
        }
        value = timeline[i].value;
    }
    return value;
}

#if MSX_VDP_G4_DISP_LOG_ENABLED
static uint8_t msx_vdp_timeline_value_for_cycle(const MsxVdpRegTimelineEvent* timeline,
                                                uint8_t count,
                                                uint32_t targetCycle)
{
    if (!timeline || count == 0u) {
        return 0u;
    }

    uint8_t value = timeline[0].value;
    for (uint8_t i = 1u; i < count; ++i) {
        if (timeline[i].cycle > targetCycle) {
            break;
        }
        value = timeline[i].value;
    }
    return value;
}
#endif

static const MsxVdpRenderAuxState* msx_vdp_render_aux(const MsxVdpState* state)
{
    return state ? static_cast<const MsxVdpRenderAuxState*>(state->renderContext) : nullptr;
}

static uint8_t msx_vdp_reg8_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[8] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r8Timeline : s_msxVdpR8Timeline,
                                           aux ? aux->r8TimelineCount : s_msxVdpR8TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg5_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[5] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r5Timeline : s_msxVdpR5Timeline,
                                           aux ? aux->r5TimelineCount : s_msxVdpR5TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg6_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[6] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r6Timeline : s_msxVdpR6Timeline,
                                           aux ? aux->r6TimelineCount : s_msxVdpR6TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg11_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[11] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r11Timeline : s_msxVdpR11Timeline,
                                           aux ? aux->r11TimelineCount : s_msxVdpR11TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg2_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[2] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r2Timeline : s_msxVdpR2Timeline,
                                           aux ? aux->r2TimelineCount : s_msxVdpR2TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg23_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[23] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r23Timeline : s_msxVdpR23Timeline,
                                           aux ? aux->r23TimelineCount : s_msxVdpR23TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg25_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[25] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r25Timeline : s_msxVdpR25Timeline,
                                           aux ? aux->r25TimelineCount : s_msxVdpR25TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg26_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[26] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r26Timeline : s_msxVdpR26Timeline,
                                           aux ? aux->r26TimelineCount : s_msxVdpR26TimelineCount,
                                           y);
}

static uint8_t msx_vdp_reg27_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return state ? state->regs[27] : 0u;
    }
    const MsxVdpRenderAuxState* aux = msx_vdp_render_aux(state);
    return msx_vdp_timeline_value_for_line(state,
                                           aux ? aux->r27Timeline : s_msxVdpR27Timeline,
                                           aux ? aux->r27TimelineCount : s_msxVdpR27TimelineCount,
                                           y);
}

static uint32_t msx_vdp_color_sprite_attr_base_for_regs(uint8_t r5, uint8_t r11, uint32_t vramMask)
{
    return ((((static_cast<uint32_t>(r11) & 0x03u) << 15) |
             ((static_cast<uint32_t>(r5) & 0xFCu) << 7))) & vramMask;
}

static uint32_t msx_vdp_color_sprite_pattern_base_for_reg6(uint8_t r6, uint32_t vramMask)
{
    return ((static_cast<uint32_t>(r6) << 11) & vramMask);
}

struct MsxVdpTableMasks {
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

bool msx_vdp_register_affects_output(uint8_t reg);

static TaskHandle_t s_vdpRenderTask = nullptr;
static SemaphoreHandle_t s_vdpRenderSem = nullptr;
static MsxVdpState s_vdpStateSnapshot;
static MsxVdpState* s_vdpOriginalState = nullptr;
static bool s_vdpTaskRunning = false;
static volatile bool s_vdpTaskBusy = false;

static uint32_t s_vdpStatFrames = 0;
static uint32_t s_vdpStatRenderUs = 0;
static uint32_t s_vdpStatCopyUs = 0;
static uint32_t s_vdpStatDrops = 0;

static uint8_t* msx_vdp_alloc_buffer(size_t size)
{
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return buffer;
}

static bool msx_vdp_init_vram(MsxVdpState* state, uint32_t vramSize)
{
    if (!state) {
        return false;
    }

    state->vram = msx_vdp_alloc_buffer(vramSize);
    if (!state->vram) {
        return false;
    }
    state->vramSize = vramSize;
    state->vramMask = vramSize - 1u;
    return true;
}

static void msx_vdp_free_msx1_buffers(void)
{
    if (s_msxFrameBuffer) {
        heap_caps_free(s_msxFrameBuffer);
        s_msxFrameBuffer = nullptr;
    }
    if (s_msx1Vram) {
        heap_caps_free(s_msx1Vram);
        s_msx1Vram = nullptr;
    }
    if (s_msx1VramB) {
        heap_caps_free(s_msx1VramB);
        s_msx1VramB = nullptr;
    }
    if (s_msx2VramB) {
        heap_caps_free(s_msx2VramB);
        s_msx2VramB = nullptr;
    }
}

static bool msx_vdp_ensure_frame_buffer(void)
{
    if (!s_msxFrameBuffer) {
        s_msxFrameBuffer = msx_vdp_alloc_buffer(kMsxFramePixels);
    }
    return s_msxFrameBuffer != nullptr;
}

static bool msx_vdp_ensure_msx1_buffers(void)
{
    if (!msx_vdp_ensure_frame_buffer()) {
        return false;
    }
    if (!s_msx1Vram) {
        s_msx1Vram = msx_vdp_alloc_buffer(kMsx1VramSize);
    }
    if (!s_msxFrameBuffer || !s_msx1Vram) {
        msx_vdp_free_msx1_buffers();
        return false;
    }
    return true;
}

static bool msx_vdp_ensure_msx1_shadow_buffer(void)
{
    if (!s_msx1VramB) {
        s_msx1VramB = msx_vdp_alloc_buffer(kMsx1VramSize);
    }
    return s_msx1VramB != nullptr;
}

static bool msx_vdp_ensure_msx2_shadow_buffer(void)
{
    if (!s_msx2VramB) {
        s_msx2VramB = msx_vdp_alloc_buffer(kMsx2VramSize);
    }
    return s_msx2VramB != nullptr;
}

static void msx_vdp_render_task(void* arg) {
    while (s_vdpTaskRunning) {
        if (xSemaphoreTake(s_vdpRenderSem, portMAX_DELAY) == pdTRUE) {
            if (!s_vdpTaskRunning) break;
            int64_t t0 = esp_timer_get_time();
            const bool rendered = msx_vdp_render_internal(&s_vdpStateSnapshot);
            int64_t t1 = esp_timer_get_time();
            s_vdpStatRenderUs += static_cast<uint32_t>(t1 - t0);
            s_vdpStatFrames++;
            if (s_vdpStatFrames >= 60) {
#if MSX_VDP_DUALCORE_STATS_LOG_ENABLED
                std::printf("[MSX][VDP-CORE0] 60fps | RenderAvg: %u us | CopyAvg: %u us | Drops: %u\n",
                            static_cast<unsigned>(s_vdpStatRenderUs / 60u),
                            static_cast<unsigned>(s_vdpStatCopyUs / 60u),
                            static_cast<unsigned>(s_vdpStatDrops));
#endif
                s_vdpStatFrames = 0;
                s_vdpStatRenderUs = 0;
                s_vdpStatCopyUs = 0;
                s_vdpStatDrops = 0;
            }
            
            MsxDisplayFrame frame = {};
            if (rendered) {
                msx_vdp_get_display_frame(&s_vdpStateSnapshot, &frame);
            }
            if (frame.indexed8) {
                msx_video_present_frame(&frame);
            }
            s_vdpTaskBusy = false;
        }
    }
    vTaskDelete(nullptr);
}

constexpr uint16_t msx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>((r & 0xF8u) | (g >> 5) | ((g & 0x1Cu) << 11) | ((b & 0xF8u) << 5));
}

static uint8_t* msx_vdp_get_line_buffer(MsxVdpState* state, unsigned y)
{
    if (!state) {
        return nullptr;
    }
    if (!state->frameBuffer) {
        if (!msx_vdp_is_msx2(state) || y >= state->activeHeight) {
            return nullptr;
        }
        return s_msx2LineBuffer;
    }
    return state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth;
}

static unsigned msx_vdp_stream_frame_width(const MsxVdpState* state)
{
    if (!state) {
        return kMsxFrameWidth;
    }

    // SCREEN 6/7 are 512-dot modes. When we are on the MSX2 line-stream path
    // we keep the full horizontal source and let msx_video scale it, instead
    // of discarding every other dot up front.
    if (!state->frameBuffer &&
        (state->mode == MsxVdpMode::Bitmap6 || state->mode == MsxVdpMode::Bitmap7)) {
        return kMsxWideFrameWidth;
    }

    return state->activeWidth;
}

static void msx_vdp_stream_line_if_needed(MsxVdpState* state, uint8_t* srcLine, unsigned srcLineIndex)
{
    if (!s_msx2StreamActive || !state || !srcLine) {
        return;
    }
    msx_video_stream_line(&s_msx2StreamVideoFrame, srcLine, srcLineIndex);
}

static void msx_vdp_end_msx2_stream_frame(void)
{
    if (!s_msx2StreamActive) {
        return;
    }

    msx_video_end_line_stream();
    s_msx2StreamActive = false;
}

static bool msx_vdp_begin_msx2_stream_frame(MsxVdpState* state)
{
    if (!state || state->frameBuffer || !msx_vdp_is_msx2(state)) {
        s_msx2StreamActive = false;
        return false;
    }

    const uint16_t* palette = ((state->mode == MsxVdpMode::Bitmap8) || msx_vdp_mode_yjk(state))
        ? state->screen8Palette565
        : state->palette565;
    const uint16_t entries = ((state->mode == MsxVdpMode::Bitmap8) || msx_vdp_mode_yjk(state))
        ? 256u
        : 16u;

    s_msx2StreamVideoFrame = {};
    s_msx2StreamVideoFrame.indexed8 = s_msx2LineBuffer;
    s_msx2StreamVideoFrame.palette565 = palette;
    s_msx2StreamVideoFrame.paletteEntryCount = entries;
    s_msx2StreamVideoFrame.width = msx_vdp_stream_frame_width(state);
    s_msx2StreamVideoFrame.height = state->activeHeight;
    s_msx2StreamVideoFrame.pitchBytes = s_msx2StreamVideoFrame.width;

    if (!msx_video_begin_line_stream(&s_msx2StreamVideoFrame)) {
        s_msx2StreamActive = false;
        return false;
    }

    s_msx2StreamActive = true;
    return true;
}

static void msx_vdp_render_fill_stream_frame(MsxVdpState* state, uint8_t color)
{
    if (!state || !s_msx2StreamActive || !msx_vdp_is_msx2(state)) {
        return;
    }

    const unsigned lineWidth = msx_vdp_stream_frame_width(state);
    for (unsigned y = 0u; y < state->activeHeight; ++y) {
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            break;
        }
        std::memset(dst, color, lineWidth);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

bool msx_vdp_is_msx2(const MsxVdpState* state)
{
    return state && state->machineMode == MsxMachineMode::MSX2;
}

const MsxVdpTableMasks* msx_vdp_table_masks(MsxVdpMode mode)
{
    static constexpr MsxVdpTableMasks kText40   = {0x7F, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr MsxVdpTableMasks kGraphics1 = {0x7F, 0xFF, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr MsxVdpTableMasks kGraphics2 = {0x7F, 0x80, 0x3C, 0xFF, 0x00, 0x7F, 0x03, 0x00, 10};
    static constexpr MsxVdpTableMasks kMulticolor = {0x7F, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 10};
    static constexpr MsxVdpTableMasks kGraphics3 = {0x7F, 0x80, 0x3C, 0xFC, 0x00, 0x7F, 0x03, 0x03, 10};
    static constexpr MsxVdpTableMasks kBitmap4  = {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 10};
    static constexpr MsxVdpTableMasks kBitmap6  = {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 10};
    static constexpr MsxVdpTableMasks kBitmap7  = {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 11};
    static constexpr MsxVdpTableMasks kBitmap8  = {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03, 11};
    static constexpr MsxVdpTableMasks kText80   = {0x7C, 0xF8, 0x3F, 0x00, 0x03, 0x07, 0x00, 0x00, 10};

    switch (mode) {
        case MsxVdpMode::Text40:
            return &kText40;
        case MsxVdpMode::Graphics1:
            return &kGraphics1;
        case MsxVdpMode::Graphics2:
            return &kGraphics2;
        case MsxVdpMode::Multicolor:
            return &kMulticolor;
        case MsxVdpMode::Graphics3:
            return &kGraphics3;
        case MsxVdpMode::Bitmap4:
            return &kBitmap4;
        case MsxVdpMode::Bitmap6:
            return &kBitmap6;
        case MsxVdpMode::Bitmap7:
            return &kBitmap7;
        case MsxVdpMode::Bitmap8:
            return &kBitmap8;
        case MsxVdpMode::Text80:
            return &kText80;
        case MsxVdpMode::Unsupported:
        default:
            return nullptr;
    }
}

inline uint8_t msx_vdp_expand3(uint8_t value)
{
    value &= 0x07u;
    return static_cast<uint8_t>((value << 5) | (value << 2) | (value >> 1));
}

inline uint8_t msx_vdp_read_vram_fast(const uint8_t* vram, uint32_t mask, uint32_t address)
{
    const uint32_t wrapped = address & mask;
    V9938_BENCH_VRAM_READ(wrapped, 1u);
    return vram[wrapped];
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

    for (unsigned i = 0; i < 256u; ++i) {
        const uint8_t r = static_cast<uint8_t>(((i >> 2) & 0x07u) * 255u / 7u);
        const uint8_t g = static_cast<uint8_t>(((i >> 5) & 0x07u) * 255u / 7u);
        uint8_t b = static_cast<uint8_t>(i & 0x03u);
        b = (b == 0u) ? 0u : (b == 1u ? 73u : (b == 2u ? 146u : 255u));
        state->screen8Palette565[i] = msx_rgb565(r, g, b);
    }
}

inline void msx_vdp_write_pattern_pixels(uint8_t* dst, uint8_t pattern, uint8_t fg, uint8_t bg)
{
    dst[0] = (pattern & 0x80u) != 0u ? fg : bg;
    dst[1] = (pattern & 0x40u) != 0u ? fg : bg;
    dst[2] = (pattern & 0x20u) != 0u ? fg : bg;
    dst[3] = (pattern & 0x10u) != 0u ? fg : bg;
    dst[4] = (pattern & 0x08u) != 0u ? fg : bg;
    dst[5] = (pattern & 0x04u) != 0u ? fg : bg;
    dst[6] = (pattern & 0x02u) != 0u ? fg : bg;
    dst[7] = (pattern & 0x01u) != 0u ? fg : bg;
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

inline void msx_vdp_write_vram_fast(MsxVdpState* state, uint32_t address, uint8_t value)
{
    const uint32_t wrapped = address & state->vramMask;
    V9938_BENCH_VRAM_WRITE(wrapped, 1u);
    if (state->vram[wrapped] != value) {
        state->vram[wrapped] = value;
        state->dirty = true;
    }
}

uint32_t msx_vdp_name_base(const MsxVdpState* state)
{
    if (!state) {
        return 0u;
    }
    const MsxVdpTableMasks* masks = msx_vdp_table_masks(state->mode);
    if (!masks) {
        return 0u;
    }
    return static_cast<uint32_t>(state->regs[2] & masks->r2) << masks->nameShift;
}

static uint32_t msx_vdp_name_base_for_reg2(const MsxVdpState* state, uint8_t reg2)
{
    if (!state) {
        return 0u;
    }
    const MsxVdpTableMasks* masks = msx_vdp_table_masks(state->mode);
    if (!masks) {
        return 0u;
    }
    return static_cast<uint32_t>(reg2 & masks->r2) << masks->nameShift;
}

uint32_t msx_vdp_color_base(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }
    const uint32_t high = msx_vdp_is_msx2(state) ? static_cast<uint32_t>(state->regs[10] & 0x07u) << 14 : 0u;
    const uint32_t low = static_cast<uint32_t>(state->regs[3] & masks->r3) << 6;
    return high | low;
}

uint32_t msx_vdp_pattern_base(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }
    return static_cast<uint32_t>(state->regs[4] & masks->r4) << 11;
}

static void msx_vdp_log_boot_text_probe(const MsxVdpState* state)
{
#if MSX_VDP_BOOT_DIAG_ENABLED
    if (!state || !msx_vdp_is_msx2(state)) {
        return;
    }

    if (state->mode != MsxVdpMode::Graphics1 && state->mode != MsxVdpMode::Text40) {
        return;
    }

    if (state->frameCounter > 96u || s_msxBootDiagCount >= 24u) {
        return;
    }

    if (s_msxBootDiagLastFrame == state->frameCounter) {
        return;
    }

    s_msxBootDiagLastFrame = state->frameCounter;
    ++s_msxBootDiagCount;

    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint32_t colorBaseRaw = static_cast<uint32_t>(state->regs[3]) << 6;
    const uint32_t patternBaseRaw = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;

    uint8_t names[8] = {0};
    for (unsigned i = 0; i < 8u; ++i) {
        names[i] = msx_vdp_read_vram_fast(vram, mask, nameBase + i);
    }

    const uint8_t name0 = names[0];
    const uint8_t patt0Raw = msx_vdp_read_vram_fast(vram, mask, patternBaseRaw + static_cast<uint32_t>(name0) * 8u);
    const uint8_t patt0 = msx_vdp_read_vram_fast(vram, mask, patternBase + static_cast<uint32_t>(name0) * 8u);
    const uint8_t color0Raw = msx_vdp_read_vram_fast(vram, mask, colorBaseRaw + static_cast<uint32_t>(name0 >> 3));
    const uint8_t color0 = msx_vdp_read_vram_fast(vram, mask, colorBase + static_cast<uint32_t>(name0 >> 3));

    std::printf("[MSX][BOOTTXT] #%u frame=%lu mode=%s disp=%u irq=%u R2=%02X R3=%02X R4=%02X R7=%02X R10=%02X NB=%05lX CBraw=%05lX CB=%05lX PBraw=%05lX PB=%05lX names=%02X %02X %02X %02X %02X %02X %02X %02X patt0raw=%02X patt0=%02X color0raw=%02X color0=%02X\n",
                static_cast<unsigned>(s_msxBootDiagCount),
                static_cast<unsigned long>(state->frameCounter),
                msx_vdp_mode_label(state->mode),
                static_cast<unsigned>((state->regs[1] & 0x40u) != 0u),
                static_cast<unsigned>((state->regs[1] & 0x20u) != 0u),
                static_cast<unsigned>(state->regs[2]),
                static_cast<unsigned>(state->regs[3]),
                static_cast<unsigned>(state->regs[4]),
                static_cast<unsigned>(state->regs[7]),
                static_cast<unsigned>(state->regs[10]),
                static_cast<unsigned long>(nameBase),
                static_cast<unsigned long>(colorBaseRaw),
                static_cast<unsigned long>(colorBase),
                static_cast<unsigned long>(patternBaseRaw),
                static_cast<unsigned long>(patternBase),
                static_cast<unsigned>(names[0]),
                static_cast<unsigned>(names[1]),
                static_cast<unsigned>(names[2]),
                static_cast<unsigned>(names[3]),
                static_cast<unsigned>(names[4]),
                static_cast<unsigned>(names[5]),
                static_cast<unsigned>(names[6]),
                static_cast<unsigned>(names[7]),
                static_cast<unsigned>(patt0Raw),
                static_cast<unsigned>(patt0),
                static_cast<unsigned>(color0Raw),
                static_cast<unsigned>(color0));
#else
    (void)state;
#endif
}

uint32_t msx_vdp_graphics2_pattern_base(const MsxVdpState* state)
{
    if (!state) {
        return 0u;
    }

    // SCREEN 2 style modes use the wider pattern-table selection used by fMSX.
    // Restricting MSX1 to bit 2 only causes visible tile corruption in games
    // such as Arkanoid because the BIOS/game can relocate the 6KB pattern area.
    return static_cast<uint32_t>(state->regs[4] & 0x3Cu) << 11;
}

uint32_t msx_vdp_name_mask(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[2]) | ~static_cast<uint32_t>(masks->m2)) << masks->nameShift)
          | ((1u << masks->nameShift) - 1u));
}

uint32_t msx_vdp_color_mask(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[3]) | ~static_cast<uint32_t>(masks->m3)) << 6) | 0x1C03Fu);
}

uint32_t msx_vdp_pattern_mask(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0xFFFFFFFFu;
    }

    return (((static_cast<uint32_t>(state->regs[4]) | ~static_cast<uint32_t>(masks->m4)) << 11) | 0x007FFu);
}

uint32_t msx_vdp_sprite_attr_base(const MsxVdpState* state)
{
    const MsxVdpTableMasks* masks = state ? msx_vdp_table_masks(state->mode) : nullptr;
    if (!state || !masks) {
        return 0u;
    }

    const uint32_t high = msx_vdp_is_msx2(state) ? static_cast<uint32_t>(state->regs[11] & 0x03u) << 15 : 0u;
    return high | (static_cast<uint32_t>(state->regs[5] & masks->r5) << 7);
}

uint32_t msx_vdp_sprite_pattern_base(const MsxVdpState* state)
{
    return state ? static_cast<uint32_t>(state->regs[6]) << 11 : 0u;
}

inline uint8_t msx_vdp_vscroll(const MsxVdpState* state)
{
    return (state && msx_vdp_is_msx2(state)) ? state->regs[23] : 0u;
}

inline void msx_vdp_refresh_timing_flags(MsxVdpState* state)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return;
    }

    const uint32_t totalLines = 262u;
    const uint32_t visibleLines = state->activeHeight != 0u ? state->activeHeight : 212u;
    const uint32_t vblankLine = visibleLines > 192u ? 230u : 220u;
    const uint32_t lineIrqActiveEnd = visibleLines > 192u ? 245u : 235u;
    const uint32_t lineCycles =
        state->frameCycleBudget != 0u ? (state->frameCycleBudget + totalLines - 1u) / totalLines : 1u;
    const uint32_t scanline = (state->currentFrameCpuCycles / lineCycles) % totalLines;
    const uint32_t linePhase = state->currentFrameCpuCycles % lineCycles;
    const uint32_t hblankStart = (lineCycles * 5u) / 6u;
    const bool fieldSelectEnabled = (state->regs[9] & 0x0Cu) != 0u;
    const uint8_t eoBit =
        fieldSelectEnabled ? static_cast<uint8_t>(((state->frameCounter - 1u) & 0x01u) << 1) : 0u;

    uint8_t status2 = static_cast<uint8_t>((state->status[2] & 0x91u) | 0x0Cu | eoBit);
    if (scanline >= vblankLine) {
        status2 |= 0x40u;
    }
    if (linePhase >= hblankStart) {
        status2 |= 0x20u;
    }
    state->status[2] = status2;

    if (scanline == lineIrqActiveEnd) {
        state->status[1] &= static_cast<uint8_t>(~0x01u);
    }

    if (scanline < lineIrqActiveEnd) {
        // Il contatore delle linee per gli interrupt viene inizializzato con R#23 
        // solo all'inizio del frame (linea 0). I cambi mid-frame di R#23 non lo influenzano!
        const uint8_t vscroll = msx_vdp_reg23_for_line(state, 0);
        const uint8_t lineDelta =
            static_cast<uint8_t>(((scanline + static_cast<uint32_t>(vscroll)) - state->regs[19]) & 0xFFu);
        const uint8_t scanlineTag = static_cast<uint8_t>(scanline & 0xFFu);
        
        const bool inIrqWindow = (lineDelta >= 2u && lineDelta <= 10u);
        if (inIrqWindow &&
            ((state->lineInterruptFrameTag != state->frameCounter) ||
             (static_cast<uint8_t>(scanlineTag - state->lineInterruptLineTag) > 10u))) {
            state->status[1] |= 0x01u;
            state->lineInterruptFrameTag = state->frameCounter;
            state->lineInterruptLineTag = scanlineTag;
        } else if (!inIrqWindow && ((state->regs[0] & 0x10u) == 0u)) {
            state->status[1] &= static_cast<uint8_t>(~0x01u);
        }
    }
}

inline bool msx_vdp_sprites_disabled_for_line(const MsxVdpState* state, unsigned y)
{
    if (!state) {
        return false;
    }
    if (msx_vdp_is_msx2(state)) {
        return (msx_vdp_reg8_for_line(state, y) & 0x02u) != 0u;
    }
    return (state->regs[8] & 0x02u) != 0u;
}

inline bool msx_vdp_mode_yjk(const MsxVdpState* state)
{
    return state && msx_vdp_is_msx2(state) && (state->regs[25] & 0x08u) != 0u;
}

inline bool msx_vdp_mode_yae(const MsxVdpState* state)
{
    return msx_vdp_mode_yjk(state) && (state->regs[25] & 0x10u) != 0u;
}

inline bool msx_vdp_hscroll512_for_regs(uint8_t reg25)
{
    return (reg25 & 0x01u) != 0u;
}

inline bool msx_vdp_hscroll512(const MsxVdpState* state)
{
    return state && msx_vdp_is_msx2(state) && msx_vdp_hscroll512_for_regs(state->regs[25]);
}

inline uint16_t msx_vdp_hscroll_for_regs(uint8_t reg26, uint8_t reg27)
{
    return static_cast<uint16_t>((reg27 & 0x07u) | ((reg26 & 0x3Fu) << 3));
}

inline uint16_t msx_vdp_hscroll(const MsxVdpState* state)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return 0u;
    }

    return msx_vdp_hscroll_for_regs(state->regs[26], state->regs[27]);
}

struct MsxBitmapFetchAddress {
    uint32_t lineBase;
    uint16_t pixelX;
};

inline MsxBitmapFetchAddress msx_vdp_bitmap_fetch_address(uint32_t base,
                                                          uint32_t rowOffset,
                                                          uint16_t scroll,
                                                          unsigned visibleX,
                                                          unsigned sourceWidth,
                                                          bool dualPage,
                                                          uint32_t pageSpan)
{
    const unsigned totalWidth = dualPage ? (sourceWidth * 2u) : sourceWidth;
    unsigned absX = visibleX + static_cast<unsigned>(scroll);
    if (totalWidth != 0u) {
        absX %= totalWidth;
    }

    const bool secondPage = dualPage && absX >= sourceWidth;
    if (secondPage) {
        absX -= sourceWidth;
    }

    MsxBitmapFetchAddress address = {};
    address.lineBase = rowOffset + (secondPage ? (base ^ pageSpan) : base);
    address.pixelX = static_cast<uint16_t>(absX);
    return address;
}

inline uint8_t msx_vdp_bitmap4_read_pixel(const uint8_t* vram, uint32_t mask, uint32_t lineBase, uint16_t pixelX)
{
    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + (pixelX >> 1));
    return static_cast<uint8_t>((pixelX & 0x01u) != 0u ? (packed & 0x0Fu) : (packed >> 4));
}

inline uint8_t msx_vdp_bitmap6_read_pixel(const uint8_t* vram, uint32_t mask, uint32_t lineBase, uint16_t pixelX)
{
    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + (pixelX >> 2));
    const uint8_t shift = static_cast<uint8_t>(6u - ((pixelX & 0x03u) << 1u));
    return static_cast<uint8_t>((packed >> shift) & 0x03u);
}

inline uint8_t msx_vdp_bitmap7_read_pixel(const uint8_t* vram, uint32_t mask, uint32_t lineBase, uint16_t pixelX)
{
    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + (pixelX >> 1));
    return static_cast<uint8_t>((pixelX & 0x01u) != 0u ? (packed & 0x0Fu) : (packed >> 4));
}

inline uint8_t msx_vdp_bitmap8_read_pixel(const uint8_t* vram, uint32_t mask, uint32_t lineBase, uint16_t pixelX)
{
    return msx_vdp_read_vram_fast(vram, mask, lineBase + pixelX);
}

inline void msx_vdp_render_bitmap4_byte(uint8_t packed, uint8_t* dst)
{
    dst[0] = static_cast<uint8_t>(packed >> 4);
    dst[1] = static_cast<uint8_t>(packed & 0x0Fu);
}

inline void msx_vdp_render_bitmap4_byte_with_sprites(uint8_t packed, const uint8_t* sprite, uint8_t* dst)
{
    const uint8_t p0 = static_cast<uint8_t>(packed >> 4);
    const uint8_t p1 = static_cast<uint8_t>(packed & 0x0Fu);
    dst[0] = sprite[0] != 0u ? sprite[0] : p0;
    dst[1] = sprite[1] != 0u ? sprite[1] : p1;
}

inline void msx_vdp_render_bitmap6_byte_wide(uint8_t packed, uint8_t* dst)
{
    dst[0] = static_cast<uint8_t>((packed >> 6) & 0x03u);
    dst[1] = static_cast<uint8_t>((packed >> 4) & 0x03u);
    dst[2] = static_cast<uint8_t>((packed >> 2) & 0x03u);
    dst[3] = static_cast<uint8_t>(packed & 0x03u);
}

inline void msx_vdp_render_bitmap6_byte_wide_with_sprites(uint8_t packed, const uint8_t* sprite, uint8_t* dst)
{
    const uint8_t p0 = static_cast<uint8_t>((packed >> 6) & 0x03u);
    const uint8_t p1 = static_cast<uint8_t>((packed >> 4) & 0x03u);
    const uint8_t p2 = static_cast<uint8_t>((packed >> 2) & 0x03u);
    const uint8_t p3 = static_cast<uint8_t>(packed & 0x03u);
    dst[0] = sprite[0] != 0u ? sprite[0] : p0;
    dst[1] = sprite[0] != 0u ? sprite[0] : p1;
    dst[2] = sprite[1] != 0u ? sprite[1] : p2;
    dst[3] = sprite[1] != 0u ? sprite[1] : p3;
}

inline void msx_vdp_render_bitmap7_byte(uint8_t packed, uint8_t* dst)
{
    dst[0] = static_cast<uint8_t>(packed >> 4);
    dst[1] = static_cast<uint8_t>(packed & 0x0Fu);
}

inline void msx_vdp_render_bitmap7_byte_with_sprites(uint8_t packed, const uint8_t* sprite, uint8_t* dst)
{
    const uint8_t p0 = static_cast<uint8_t>(packed >> 4);
    const uint8_t p1 = static_cast<uint8_t>(packed & 0x0Fu);
    dst[0] = sprite[0] != 0u ? sprite[0] : p0;
    dst[1] = sprite[0] != 0u ? sprite[0] : p1;
}

inline uint8_t msx_vdp_screen8_mapped_color(uint8_t color)
{
    static constexpr uint8_t kSpriteToScreen8[16] = {
        0x00, 0x02, 0x10, 0x12, 0x80, 0x82, 0x90, 0x92,
        0x49, 0x4B, 0x59, 0x5B, 0xC9, 0xCB, 0xD9, 0xDB,
    };

    return kSpriteToScreen8[color & 0x0Fu];
}

uint8_t msx_vdp_yjk_color_index(int y, int j, int k)
{
    int r = y + j;
    int g = y + k;
    int b = (5 * y - 2 * j - k) / 4;

    r = r < 0 ? 0 : (r > 31 ? 31 : r);
    g = g < 0 ? 0 : (g > 31 ? 31 : g);
    b = b < 0 ? 0 : (b > 31 ? 31 : b);

    return static_cast<uint8_t>((r & 0x1Cu) | ((g & 0x1Cu) << 3) | (b >> 3));
}

const char* msx_vdp_command_label(uint8_t command)
{
    switch (command & 0x0Fu) {
        case 0x0: return "ABRT";
        case 0x4: return "POINT";
        case 0x5: return "PSET";
        case 0x6: return "SRCH";
        case 0x7: return "LINE";
        case 0x8: return "LMMV";
        case 0x9: return "LMMM";
        case 0xA: return "LMCM";
        case 0xB: return "LMMC";
        case 0xC: return "HMMV";
        case 0xD: return "HMMM";
        case 0xE: return "YMMM";
        case 0xF: return "HMMC";
        default:  return "UNK";
    }
}

uint32_t msx_vdp_command_estimated_bytes(uint8_t command, uint16_t nxRaw, uint16_t nyRaw, uint16_t ppb)
{
    const uint32_t width = nxRaw == 0u ? 1024u : static_cast<uint32_t>(nxRaw);
    const uint32_t height = nyRaw == 0u ? 1024u : static_cast<uint32_t>(nyRaw);
    const uint32_t bytesPerPixelRow = ppb != 0u ? ((width + ppb - 1u) / ppb) : width;

    switch (command & 0x0Fu) {
        case 0x4: // POINT
        case 0x5: // PSET
            return 1u;
        case 0x6: // SRCH
        case 0x7: // LINE
            return width;
        case 0x8: // LMMV
        case 0x9: // LMMM
        case 0xA: // LMCM
        case 0xB: // LMMC
            return width * height;
        case 0xC: // HMMV
        case 0xD: // HMMM
        case 0xE: // YMMM
        case 0xF: // HMMC
            return bytesPerPixelRow * height;
        case 0x0:
        default:
            return 0u;
    }
}

bool msx_vdp_is_traced_register(uint8_t reg)
{
    return reg == 17u || reg == 23u || reg == 25u || reg == 26u || reg == 27u ||
           (reg >= 32u && reg <= 46u);
}

int msx_vdp_command_mode_index(const MsxVdpState* state)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return -1;
    }

    switch (state->mode) {
        case MsxVdpMode::Bitmap4:
            return 0;
        case MsxVdpMode::Bitmap6:
            return 1;
        case MsxVdpMode::Bitmap7:
            return 2;
        case MsxVdpMode::Bitmap8:
            return 3;
        default:
            return -1;
    }
}

uint8_t msx_vdp_command_mask(uint8_t screenMode)
{
    static constexpr uint8_t kMasks[4] = {0x0Fu, 0x03u, 0x0Fu, 0xFFu};
    return kMasks[screenMode & 0x03u];
}

uint16_t msx_vdp_command_ppb(uint8_t screenMode)
{
    static constexpr uint16_t kPixelsPerByte[4] = {2u, 4u, 2u, 1u};
    return kPixelsPerByte[screenMode & 0x03u];
}

uint16_t msx_vdp_command_ppl(uint8_t screenMode)
{
    static constexpr uint16_t kPixelsPerLine[4] = {256u, 512u, 512u, 256u};
    return kPixelsPerLine[screenMode & 0x03u];
}

uint32_t msx_vdp_command_addr(uint8_t screenMode, int x, int y)
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

uint8_t msx_vdp_command_point(const MsxVdpState* state, uint8_t screenMode, int x, int y)
{
    const uint32_t addr = msx_vdp_command_addr(screenMode, x, y) & state->vramMask;
    V9938_BENCH_VRAM_READ(addr, 1u);
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

void msx_vdp_command_psetlowlevel(uint8_t* dst, uint8_t color, uint8_t preserveMask, uint8_t op)
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

void msx_vdp_command_pset(MsxVdpState* state, uint8_t screenMode, int x, int y, uint8_t color, uint8_t op)
{
    const uint32_t addr = msx_vdp_command_addr(screenMode, x, y) & state->vramMask;
    uint8_t* const dst = state->vram + addr;
    V9938_BENCH_VRAM_WRITE(addr, 1u);

    switch (screenMode & 0x03u) {
        case 0: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 1) << 2);
            msx_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x0Fu) << shift),
                                         static_cast<uint8_t>(~(0x0Fu << shift)), op);
            break;
        }
        case 1: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 3) << 1);
            msx_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x03u) << shift),
                                         static_cast<uint8_t>(~(0x03u << shift)), op);
            break;
        }
        case 2: {
            const uint8_t shift = static_cast<uint8_t>(((~x) & 1) << 2);
            msx_vdp_command_psetlowlevel(dst, static_cast<uint8_t>((color & 0x0Fu) << shift),
                                         static_cast<uint8_t>(~(0x0Fu << shift)), op);
            break;
        }
        case 3:
        default:
            msx_vdp_command_psetlowlevel(dst, color, 0x00u, op);
            break;
    }

    state->dirty = true;
}

uint8_t msx_vdp_read_reg16(const MsxVdpState* state, uint8_t lowReg)
{
    return static_cast<uint8_t>(state->regs[lowReg] | ((state->regs[lowReg + 1u] & 0x03u) << 8));
}

uint16_t msx_vdp_read_reg10(const MsxVdpState* state, uint8_t lowReg)
{
    return static_cast<uint16_t>(state->regs[lowReg] | ((state->regs[lowReg + 1u] & 0x03u) << 8));
}

void msx_vdp_write_reg10(MsxVdpState* state, uint8_t lowReg, uint16_t value)
{
    value &= 0x03FFu;
    state->regs[lowReg] = static_cast<uint8_t>(value & 0xFFu);
    state->regs[lowReg + 1u] = static_cast<uint8_t>((state->regs[lowReg + 1u] & ~0x03u) | ((value >> 8) & 0x03u));
}

void msx_vdp_command_finish(MsxVdpState* state)
{
    state->command.transfer = MsxVdpTransferCommand::None;
    state->status[2] &= static_cast<uint8_t>(~0x01u);
}

void msx_vdp_command_prepare_engine(MsxVdpState* state,
                                    MsxVdpTransferCommand transfer,
                                    uint8_t screenMode,
                                    uint8_t logicOp,
                                    bool byteTransfer)
{
    MsxVdpCommandState& command = state->command;
    const uint16_t nxRaw = msx_vdp_read_reg10(state, 40u);
    const uint16_t nyRaw = msx_vdp_read_reg10(state, 42u);
    const uint16_t ppl = msx_vdp_command_ppl(screenMode);
    const uint16_t ppb = msx_vdp_command_ppb(screenMode);

    command.transfer = transfer;
    command.screenMode = screenMode;
    command.logicOp = logicOp;
    command.sx = msx_vdp_read_reg10(state, 32u);
    command.sy = msx_vdp_read_reg10(state, 34u);
    command.dx = msx_vdp_read_reg10(state, 36u);
    command.dy = msx_vdp_read_reg10(state, 38u);
    command.ny = nyRaw == 0u ? 1024u : nyRaw;
    command.ty = (state->regs[45] & 0x08u) != 0u ? -1 : 1;
    command.mx = ppl;
    command.lineYMajor = false;
    if (byteTransfer) {
        const uint16_t nxBytes = nxRaw;
        command.tx = (state->regs[45] & 0x04u) != 0u ? -static_cast<int16_t>(ppb) : static_cast<int16_t>(ppb);
        command.nx = static_cast<uint16_t>(nxBytes / ppb);
    } else {
        command.tx = (state->regs[45] & 0x04u) != 0u ? -1 : 1;
        command.nx = nxRaw == 0u ? 1024u : nxRaw;
    }
    command.asx = command.sx;
    command.adx = command.dx;
    command.anx = command.nx;
    command.cycleStamp = state->currentFrameCpuCycles;
    state->status[2] |= 0x01u;
}

void msx_vdp_command_prepare_transfer(MsxVdpState* state, MsxVdpTransferCommand transfer, uint8_t screenMode, uint8_t opcode)
{
    const bool byteTransfer = ((opcode >> 4) & 0x0Cu) == 0x0Cu;
    msx_vdp_command_prepare_engine(state,
                                   transfer,
                                   screenMode,
                                   static_cast<uint8_t>(opcode & 0x0Fu),
                                   byteTransfer);
}

static bool msx_vdp_command_is_async_bulk(MsxVdpTransferCommand transfer)
{
    switch (transfer) {
        case MsxVdpTransferCommand::Line:
        case MsxVdpTransferCommand::Hmmv:
        case MsxVdpTransferCommand::Lmmv:
        case MsxVdpTransferCommand::Lmmm:
        case MsxVdpTransferCommand::Hmmm:
        case MsxVdpTransferCommand::Ymmm:
            return true;
        case MsxVdpTransferCommand::None:
        case MsxVdpTransferCommand::Lmcm:
        case MsxVdpTransferCommand::Lmmc:
        case MsxVdpTransferCommand::Hmmc:
        default:
            return false;
    }
}

#if MSX_VDP_VERBOSE_DIAG_ENABLED
const char* msx_vdp_transfer_label(MsxVdpTransferCommand transfer)
{
    switch (transfer) {
        case MsxVdpTransferCommand::Line: return "LINE";
        case MsxVdpTransferCommand::Lmcm: return "LMCM";
        case MsxVdpTransferCommand::Lmmc: return "LMMC";
        case MsxVdpTransferCommand::Hmmc: return "HMMC";
        case MsxVdpTransferCommand::Hmmv: return "HMMV";
        case MsxVdpTransferCommand::Lmmv: return "LMMV";
        case MsxVdpTransferCommand::Lmmm: return "LMMM";
        case MsxVdpTransferCommand::Hmmm: return "HMMM";
        case MsxVdpTransferCommand::Ymmm: return "YMMM";
        case MsxVdpTransferCommand::None:
        default:
            return "NONE";
    }
}

bool msx_vdp_diag_take(uint32_t* counter, uint32_t limit)
{
    if (!counter || *counter >= limit) {
        return false;
    }
    ++(*counter);
    return true;
}

void msx_vdp_diag_log_command_start(const MsxVdpState* state,
                                    uint8_t opcode,
                                    uint16_t sx,
                                    uint16_t sy,
                                    uint16_t dx,
                                    uint16_t dy,
                                    uint16_t nx,
                                    uint16_t ny)
{
#if !MSX_VDP_CMD_LOG_ENABLED
    (void)state;
    (void)opcode;
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    (void)nx;
    (void)ny;
    return;
#else
    static uint32_t s_cmdStartLogCount = 0u;
    if (!state || !msx_vdp_diag_take(&s_cmdStartLogCount, 192u)) {
        return;
    }

    std::printf("[MSX][VDP-CMD] #%lu op=%02X %s mode=%s sx=%u sy=%u dx=%u dy=%u nx=%u ny=%u r44=%02X r45=%02X r46=%02X s2=%02X r15=%02X frame=%lu cyc=%lu\n",
                static_cast<unsigned long>(s_cmdStartLogCount),
                static_cast<unsigned>(opcode),
                msx_vdp_command_label(static_cast<uint8_t>(opcode >> 4)),
                msx_vdp_mode_label(state->mode),
                static_cast<unsigned>(sx),
                static_cast<unsigned>(sy),
                static_cast<unsigned>(dx),
                static_cast<unsigned>(dy),
                static_cast<unsigned>(nx),
                static_cast<unsigned>(ny),
                static_cast<unsigned>(state->regs[44]),
                static_cast<unsigned>(state->regs[45]),
                static_cast<unsigned>(state->regs[46]),
                static_cast<unsigned>(state->status[2]),
                static_cast<unsigned>(state->regs[15]),
                static_cast<unsigned long>(state->frameCounter),
                static_cast<unsigned long>(state->currentFrameCpuCycles));
#endif
}

void msx_vdp_diag_log_transfer(const MsxVdpState* state,
                               const char* phase,
                               uint8_t payload)
{
#if !MSX_VDP_XFER_LOG_ENABLED
    (void)state;
    (void)phase;
    (void)payload;
    return;
#else
    static uint32_t s_transferLogCount = 0u;
    if (!state || !phase) {
        return;
    }

    const MsxVdpCommandState& command = state->command;
    if (command.transfer == MsxVdpTransferCommand::None) {
        return;
    }
    if (!msx_vdp_diag_take(&s_transferLogCount, 320u)) {
        return;
    }
    std::printf("[MSX][VDP-XFER] #%lu %s cmd=%s mode=%s val=%02X sx=%u sy=%u dx=%u dy=%u anx=%u nx=%u ny=%u tx=%d ty=%d s2=%02X frame=%lu cyc=%lu\n",
                static_cast<unsigned long>(s_transferLogCount),
                phase,
                msx_vdp_transfer_label(command.transfer),
                msx_vdp_mode_label(state->mode),
                static_cast<unsigned>(payload),
                static_cast<unsigned>(command.asx),
                static_cast<unsigned>(command.sy),
                static_cast<unsigned>(command.adx),
                static_cast<unsigned>(command.dy),
                static_cast<unsigned>(command.anx),
                static_cast<unsigned>(command.nx),
                static_cast<unsigned>(command.ny),
                static_cast<int>(command.tx),
                static_cast<int>(command.ty),
                static_cast<unsigned>(state->status[2]),
                static_cast<unsigned long>(state->frameCounter),
                static_cast<unsigned long>(state->currentFrameCpuCycles));
#endif
}

void msx_vdp_diag_log_status_read(const MsxVdpState* state, uint8_t index, uint8_t value)
{
    static uint32_t s_statusLogCount = 0u;
    static bool s_statusSeen[2] = { false, false };
    static uint8_t s_lastStatusValue[2] = { 0u, 0u };
    static uint8_t s_lastStatusCommand[2] = { 0u, 0u };
    if (!state) {
        return;
    }
    if (index != 2u && index != 7u) {
        return;
    }

    const MsxVdpCommandState& command = state->command;
    if (command.transfer == MsxVdpTransferCommand::None) {
        return;
    }

    const uint8_t slot = index == 7u ? 1u : 0u;
    const uint8_t commandTag = static_cast<uint8_t>(command.transfer);
    if (s_statusSeen[slot] &&
        s_lastStatusValue[slot] == value &&
        s_lastStatusCommand[slot] == commandTag) {
        return;
    }
    if (!msx_vdp_diag_take(&s_statusLogCount, 256u)) {
        return;
    }

    s_statusSeen[slot] = true;
    s_lastStatusValue[slot] = value;
    s_lastStatusCommand[slot] = commandTag;

    std::printf("[MSX][VDP-STS] #%lu s%u=%02X cmd=%s r15=%02X r44=%02X sx=%u sy=%u dx=%u dy=%u anx=%u ny=%u s2=%02X frame=%lu cyc=%lu\n",
                static_cast<unsigned long>(s_statusLogCount),
                static_cast<unsigned>(index),
                static_cast<unsigned>(value),
                msx_vdp_transfer_label(command.transfer),
                static_cast<unsigned>(state->regs[15]),
                static_cast<unsigned>(state->regs[44]),
                static_cast<unsigned>(command.asx),
                static_cast<unsigned>(command.sy),
                static_cast<unsigned>(command.adx),
                static_cast<unsigned>(command.dy),
                static_cast<unsigned>(command.anx),
                static_cast<unsigned>(command.ny),
                static_cast<unsigned>(state->status[2]),
                static_cast<unsigned long>(state->frameCounter),
                static_cast<unsigned long>(state->currentFrameCpuCycles));
}

void msx_vdp_diag_log_command_finish(const MsxVdpState* state,
                                     const char* phase,
                                     const MsxVdpCommandState& command)
{
#if !MSX_VDP_FIN_LOG_ENABLED
    (void)state;
    (void)phase;
    (void)command;
    return;
#else
    static uint32_t s_finishLogCount = 0u;
    if (!state || !phase || !msx_vdp_diag_take(&s_finishLogCount, 128u)) {
        return;
    }

    std::printf("[MSX][VDP-FIN] #%lu %s cmd=%s mode=%s sx=%u sy=%u dx=%u dy=%u nx=%u ny=%u asx=%u adx=%u anx=%u s2=%02X r44=%02X frame=%lu cyc=%lu\n",
                static_cast<unsigned long>(s_finishLogCount),
                phase,
                msx_vdp_transfer_label(command.transfer),
                msx_vdp_mode_label(state->mode),
                static_cast<unsigned>(command.sx),
                static_cast<unsigned>(command.sy),
                static_cast<unsigned>(command.dx),
                static_cast<unsigned>(command.dy),
                static_cast<unsigned>(command.nx),
                static_cast<unsigned>(command.ny),
                static_cast<unsigned>(command.asx),
                static_cast<unsigned>(command.adx),
                static_cast<unsigned>(command.anx),
                static_cast<unsigned>(state->status[2]),
                static_cast<unsigned>(state->regs[44]),
                static_cast<unsigned long>(state->frameCounter),
                static_cast<unsigned long>(state->currentFrameCpuCycles));
#endif
}
#else
void msx_vdp_diag_log_command_start(const MsxVdpState*,
                                    uint8_t,
                                    uint16_t,
                                    uint16_t,
                                    uint16_t,
                                    uint16_t,
                                    uint16_t,
                                    uint16_t)
{
}

void msx_vdp_diag_log_transfer(const MsxVdpState*, const char*, uint8_t)
{
}

void msx_vdp_diag_log_status_read(const MsxVdpState*, uint8_t, uint8_t)
{
}

void msx_vdp_diag_log_command_finish(const MsxVdpState*, const char*, const MsxVdpCommandState&)
{
}
#endif

static void msx_vdp_command_finish_line(MsxVdpState* state, MsxVdpCommandState& command)
{
    msx_vdp_diag_log_command_finish(state, "line-done", command);
    msx_vdp_write_reg10(state, 38u, command.dy);
    msx_vdp_command_finish(state);
}

static void msx_vdp_command_finish_hmmv(MsxVdpState* state, MsxVdpCommandState& command)
{
    msx_vdp_diag_log_command_finish(state, "hmmv-done", command);
    const uint16_t finalDy =
        command.ny == 0u ? static_cast<uint16_t>(command.dy + command.ty) : command.dy;
    msx_vdp_write_reg10(state, 42u, command.ny);
    msx_vdp_write_reg10(state, 38u, finalDy);
    msx_vdp_command_finish(state);
}

static void msx_vdp_command_finish_lmmv(MsxVdpState* state, MsxVdpCommandState& command)
{
    msx_vdp_diag_log_command_finish(state, "lmmv-done", command);
    const uint16_t finalDy =
        command.ny == 0u ? static_cast<uint16_t>(command.dy + command.ty) : command.dy;
    msx_vdp_write_reg10(state, 42u, command.ny);
    msx_vdp_write_reg10(state, 38u, finalDy);
    msx_vdp_command_finish(state);
}

static void msx_vdp_command_finish_lmmm(MsxVdpState* state, MsxVdpCommandState& command)
{
    msx_vdp_diag_log_command_finish(state, "copy-done", command);
    const bool sourceWrapped = command.sy == 0xFFFFu;
    const uint16_t finalSy =
        command.ny == 0u ? static_cast<uint16_t>(command.sy + command.ty) : command.sy;
    uint16_t finalDy = command.dy;
    if (command.ny == 0u || sourceWrapped) {
        finalDy = static_cast<uint16_t>(command.dy + command.ty);
    }
    msx_vdp_write_reg10(state, 42u, command.ny);
    msx_vdp_write_reg10(state, 34u, finalSy);
    msx_vdp_write_reg10(state, 38u, finalDy);
    msx_vdp_command_finish(state);
}

static bool msx_vdp_command_try_bulk_hmmm(MsxVdpState* state, uint8_t liveScreenMode)
{
    if (!state || state->command.transfer != MsxVdpTransferCommand::Hmmm) {
        return false;
    }

    MsxVdpCommandState& command = state->command;
    const uint16_t ppb = msx_vdp_command_ppb(liveScreenMode);
    const uint16_t ppl = msx_vdp_command_ppl(liveScreenMode);
    const uint16_t rowBytes = static_cast<uint16_t>(ppl / ppb);
    const uint16_t len = command.nx;
    if (len == 0u || command.ny == 0u || command.anx != len ||
        command.asx != command.sx || command.adx != command.dx) {
        return false;
    }
    if (command.tx != static_cast<int16_t>(ppb) &&
        command.tx != -static_cast<int16_t>(ppb)) {
        return false;
    }

    const bool forward = command.tx > 0;
    const uint16_t srcXByte = static_cast<uint16_t>(((command.asx & (ppl - 1u)) / ppb) & 0x03FFu);
    const uint16_t dstXByte = static_cast<uint16_t>(((command.adx & (ppl - 1u)) / ppb) & 0x03FFu);
    if (forward) {
        if (srcXByte + len > rowBytes || dstXByte + len > rowBytes) {
            return false;
        }
    } else if (srcXByte + 1u < len || dstXByte + 1u < len) {
        return false;
    }

    uint8_t lastValue = state->status[7];
    while (command.ny != 0u) {
        const uint32_t srcAddr = msx_vdp_command_addr(liveScreenMode, command.asx, command.sy) & state->vramMask;
        const uint32_t dstAddr = msx_vdp_command_addr(liveScreenMode, command.adx, command.dy) & state->vramMask;
        const uint32_t srcStart = forward ? srcAddr : ((srcAddr - (len - 1u)) & state->vramMask);
        const uint32_t dstStart = forward ? dstAddr : ((dstAddr - (len - 1u)) & state->vramMask);

        if (srcStart + len > state->vramSize || dstStart + len > state->vramSize) {
            return false;
        }

        lastValue = state->vram[forward ? (srcStart + len - 1u) : srcStart];
        V9938_BENCH_VRAM_READ(srcStart, len);
        V9938_BENCH_VRAM_WRITE(dstStart, len);
        std::memmove(state->vram + dstStart, state->vram + srcStart, len);

        if (--command.ny == 0u) {
            state->status[7] = lastValue;
            state->dirty = true;
            msx_vdp_command_finish_lmmm(state, command);
            return true;
        }

        command.sy = static_cast<uint16_t>(command.sy + command.ty);
        command.dy = static_cast<uint16_t>(command.dy + command.ty);
        if (command.sy == 0xFFFFu || command.dy == 0xFFFFu) {
            state->status[7] = lastValue;
            state->dirty = true;
            msx_vdp_command_finish_lmmm(state, command);
            return true;
        }

        command.asx = command.sx;
        command.adx = command.dx;
        command.anx = command.nx;
    }

    state->status[7] = lastValue;
    state->dirty = true;
    return true;
}

static bool msx_vdp_command_advance_step(MsxVdpState* state, uint8_t liveScreenMode)
{
    MsxVdpCommandState& command = state->command;
    switch (command.transfer) {
        case MsxVdpTransferCommand::Line: {
            msx_vdp_command_pset(state,
                                 liveScreenMode,
                                 command.dx,
                                 command.dy,
                                 state->regs[44],
                                 command.logicOp);
            int accum = command.asx == 0xFFFFu ? -1 : static_cast<int>(command.asx);
            accum -= static_cast<int>(command.ny);
            if (!command.lineYMajor) {
                command.dx = static_cast<uint16_t>(command.dx + command.tx);
                if (accum < 0) {
                    accum += static_cast<int>(command.nx);
                    command.dy = static_cast<uint16_t>(command.dy + command.ty);
                }
            } else {
                command.dy = static_cast<uint16_t>(command.dy + command.ty);
                if (accum < 0) {
                    accum += static_cast<int>(command.nx);
                    command.dx = static_cast<uint16_t>(command.dx + command.tx);
                }
            }
            command.asx = static_cast<uint16_t>(accum & 1023);
            if (command.adx++ == command.nx || (command.dx & command.mx) != 0u) {
                msx_vdp_command_finish_line(state, command);
            }
            return true;
        }
        case MsxVdpTransferCommand::Hmmv: {
            const uint32_t addr =
                msx_vdp_command_addr(liveScreenMode, command.adx, command.dy) & state->vramMask;
            const uint8_t value = state->regs[44];
            V9938_BENCH_VRAM_WRITE(addr, 1u);
            state->vram[addr] = value;
            state->status[7] = value;
            state->dirty = true;
            if (--command.anx == 0u ||
                ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u ||
                    (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    msx_vdp_command_finish_hmmv(state, command);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            return true;
        }
        case MsxVdpTransferCommand::Lmmv: {
            const uint8_t value = static_cast<uint8_t>(state->regs[44] &
                                                       msx_vdp_command_mask(liveScreenMode));
            msx_vdp_command_pset(state,
                                 liveScreenMode,
                                 command.adx,
                                 command.dy,
                                 value,
                                 command.logicOp);
            state->status[7] = value;
            if (--command.anx == 0u ||
                ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u ||
                    (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    msx_vdp_command_finish_lmmv(state, command);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            return true;
        }
        case MsxVdpTransferCommand::Lmmm: {
            const uint8_t pixel =
                msx_vdp_command_point(state, liveScreenMode, command.asx, command.sy);
            msx_vdp_command_pset(state,
                                 liveScreenMode,
                                 command.adx,
                                 command.dy,
                                 pixel,
                                 command.logicOp);
            state->status[7] = pixel;
            if (--command.anx == 0u ||
                ((command.asx = static_cast<uint16_t>(command.asx + command.tx)) & command.mx) != 0u ||
                ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u ||
                    (command.sy = static_cast<uint16_t>(command.sy + command.ty)) == 0xFFFFu ||
                    (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    msx_vdp_command_finish_lmmm(state, command);
                } else {
                    command.asx = command.sx;
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            return true;
        }
        case MsxVdpTransferCommand::Hmmm: {
            const uint32_t srcAddr =
                msx_vdp_command_addr(liveScreenMode, command.asx, command.sy) & state->vramMask;
            const uint32_t dstAddr =
                msx_vdp_command_addr(liveScreenMode, command.adx, command.dy) & state->vramMask;
            V9938_BENCH_VRAM_READ(srcAddr, 1u);
            const uint8_t value = state->vram[srcAddr];
            V9938_BENCH_VRAM_WRITE(dstAddr, 1u);
            state->vram[dstAddr] = value;
            state->status[7] = value;
            state->dirty = true;
            if (--command.anx == 0u ||
                ((command.asx = static_cast<uint16_t>(command.asx + command.tx)) & command.mx) != 0u ||
                ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u ||
                    (command.sy = static_cast<uint16_t>(command.sy + command.ty)) == 0xFFFFu ||
                    (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    msx_vdp_command_finish_lmmm(state, command);
                } else {
                    command.asx = command.sx;
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            return true;
        }
        case MsxVdpTransferCommand::Ymmm: {
            const uint32_t srcAddr =
                msx_vdp_command_addr(liveScreenMode, command.adx, command.sy) & state->vramMask;
            const uint32_t dstAddr =
                msx_vdp_command_addr(liveScreenMode, command.adx, command.dy) & state->vramMask;
            V9938_BENCH_VRAM_READ(srcAddr, 1u);
            const uint8_t value = state->vram[srcAddr];
            V9938_BENCH_VRAM_WRITE(dstAddr, 1u);
            state->vram[dstAddr] = value;
            state->status[7] = value;
            state->dirty = true;
            if (--command.anx == 0u ||
                ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u ||
                    (command.sy = static_cast<uint16_t>(command.sy + command.ty)) == 0xFFFFu ||
                    (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    msx_vdp_command_finish_lmmm(state, command);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            return true;
        }
        case MsxVdpTransferCommand::None:
        case MsxVdpTransferCommand::Lmcm:
        case MsxVdpTransferCommand::Lmmc:
        case MsxVdpTransferCommand::Hmmc:
        default:
            return false;
    }
}

static void msx_vdp_command_complete_async_bulk(MsxVdpState* state, uint8_t liveScreenMode)
{
    if (!state) {
        return;
    }

    if (msx_vdp_command_try_bulk_hmmm(state, liveScreenMode)) {
        state->command.cycleStamp = state->currentFrameCpuCycles;
        return;
    }

    while (msx_vdp_command_is_async_bulk(state->command.transfer)) {
        if (!msx_vdp_command_advance_step(state, liveScreenMode)) {
            break;
        }
    }
    state->command.cycleStamp = state->currentFrameCpuCycles;
}

static uint32_t msx_vdp_command_steps_per_line(uint8_t liveScreenMode)
{
    switch (liveScreenMode & 0x03u) {
        case 0u:
        case 1u:
            return 16u;
        case 2u:
        case 3u:
        default:
            return 8u;
    }
}

void msx_vdp_command_continue(MsxVdpState* state)
{
    MsxVdpCommandState& command = state->command;
    if (!state || command.transfer == MsxVdpTransferCommand::None || (state->status[2] & 0x80u) != 0u) {
        return;
    }

    msx_vdp_update_mode_geometry(state);
    const int liveModeIndex = msx_vdp_command_mode_index(state);
    const uint8_t liveScreenMode = liveModeIndex >= 0
        ? static_cast<uint8_t>(liveModeIndex)
        : command.screenMode;

    switch (command.transfer) {
        case MsxVdpTransferCommand::Lmcm: {
            const uint8_t value = msx_vdp_command_point(state, liveScreenMode, command.asx, command.sy);
            state->regs[44] = value;
            state->status[7] = value;
            msx_vdp_diag_log_transfer(state, "lmcm-fetch", value);
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.asx = static_cast<uint16_t>(command.asx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.sy = static_cast<uint16_t>(command.sy + command.ty)) == 0xFFFFu) {
                    const uint16_t finalSy =
                        command.ny == 0u ? static_cast<uint16_t>(command.sy + command.ty) : command.sy;
                    msx_vdp_diag_log_command_finish(state, "lmcm-done", command);
                    msx_vdp_write_reg10(state, 42u, command.ny);
                    msx_vdp_write_reg10(state, 34u, finalSy);
                    msx_vdp_command_finish(state);
                } else {
                    command.asx = command.sx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case MsxVdpTransferCommand::Lmmc: {
            const uint8_t value = static_cast<uint8_t>(state->regs[44] & msx_vdp_command_mask(liveScreenMode));
            state->regs[44] = value;
            state->status[7] = value;
            msx_vdp_diag_log_transfer(state, "lmmc-store", value);
            msx_vdp_command_pset(state, liveScreenMode, command.adx, command.dy, value, command.logicOp);
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    const uint16_t finalDy =
                        command.ny == 0u ? static_cast<uint16_t>(command.dy + command.ty) : command.dy;
                    msx_vdp_diag_log_command_finish(state, "lmmc-done", command);
                    msx_vdp_write_reg10(state, 42u, command.ny);
                    msx_vdp_write_reg10(state, 38u, finalDy);
                    msx_vdp_command_finish(state);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case MsxVdpTransferCommand::Hmmc: {
            const uint32_t addr = msx_vdp_command_addr(liveScreenMode, command.adx, command.dy) & state->vramMask;
            const uint8_t value = state->regs[44];
            msx_vdp_diag_log_transfer(state, "hmmc-store", value);
            V9938_BENCH_VRAM_WRITE(addr, 1u);
            state->vram[addr] = value;
            state->status[7] = value;
            state->dirty = true;
            state->status[2] |= 0x80u;
            if (--command.anx == 0u || ((command.adx = static_cast<uint16_t>(command.adx + command.tx)) & command.mx) != 0u) {
                if (--command.ny == 0u || (command.dy = static_cast<uint16_t>(command.dy + command.ty)) == 0xFFFFu) {
                    const uint16_t finalDy =
                        command.ny == 0u ? static_cast<uint16_t>(command.dy + command.ty) : command.dy;
                    msx_vdp_diag_log_command_finish(state, "hmmc-done", command);
                    msx_vdp_write_reg10(state, 42u, command.ny);
                    msx_vdp_write_reg10(state, 38u, finalDy);
                    msx_vdp_command_finish(state);
                } else {
                    command.adx = command.dx;
                    command.anx = command.nx;
                }
            }
            break;
        }
        case MsxVdpTransferCommand::None:
        default:
            break;
    }
}

void msx_vdp_advance_command_engine_internal(MsxVdpState* state, uint32_t targetFrameCycles)
{
    if (!state) {
        return;
    }

    MsxVdpCommandState& command = state->command;
    if (targetFrameCycles < command.cycleStamp) {
        command.cycleStamp = targetFrameCycles;
    }
    if (!msx_vdp_is_msx2(state) || !msx_vdp_command_is_async_bulk(command.transfer)) {
        command.cycleStamp = targetFrameCycles;
        return;
    }

    if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
        msx_vdp_update_mode_geometry(state);
        const int liveModeIndex = msx_vdp_command_mode_index(state);
        const uint8_t liveScreenMode =
            liveModeIndex >= 0 ? static_cast<uint8_t>(liveModeIndex) : command.screenMode;
        msx_vdp_command_complete_async_bulk(state, liveScreenMode);
        command.cycleStamp = targetFrameCycles;
        return;
    }

    const uint32_t fromLine = msx_vdp_line_index_for_cycle(state, command.cycleStamp);
    const uint32_t toLine = msx_vdp_line_index_for_cycle(state, targetFrameCycles);
    if (toLine <= fromLine) {
        command.cycleStamp = targetFrameCycles;
        return;
    }

    msx_vdp_update_mode_geometry(state);
    const int liveModeIndex = msx_vdp_command_mode_index(state);
    const uint8_t liveScreenMode =
        liveModeIndex >= 0 ? static_cast<uint8_t>(liveModeIndex) : command.screenMode;
    uint32_t steps = (toLine - fromLine) * msx_vdp_command_steps_per_line(liveScreenMode);

    while (steps-- != 0u && msx_vdp_command_is_async_bulk(command.transfer)) {
        if (!msx_vdp_command_advance_step(state, liveScreenMode)) {
            break;
        }
    }

    command.cycleStamp = targetFrameCycles;
}

uint8_t msx_vdp_command_read(MsxVdpState* state)
{
    if (!state) {
        return 0xFFu;
    }

    msx_vdp_diag_log_transfer(state, "cpu-read-before", state->regs[44]);
    state->status[2] &= static_cast<uint8_t>(~0x80u);
    msx_vdp_command_continue(state);
    msx_vdp_diag_log_transfer(state, "cpu-read-after", state->regs[44]);
    return state->regs[44];
}

void msx_vdp_command_write(MsxVdpState* state, uint8_t value)
{
    if (!state) {
        return;
    }

    msx_vdp_diag_log_transfer(state, "cpu-write", value);
    state->status[2] &= static_cast<uint8_t>(~0x80u);
    state->regs[44] = value;
    state->status[7] = value;
    msx_vdp_command_continue(state);
}

void msx_vdp_command_execute(MsxVdpState* state, uint8_t opcode)
{
    if (!state || !msx_vdp_is_msx2(state)) {
        return;
    }

    msx_vdp_update_mode_geometry(state);
    const int sm = msx_vdp_command_mode_index(state);
    state->command.transfer = MsxVdpTransferCommand::None;
    state->status[2] &= static_cast<uint8_t>(~0x81u);
    if (sm < 0) {
        return;
    }

    const uint8_t screenMode = static_cast<uint8_t>(sm);
    const uint8_t command = static_cast<uint8_t>(opcode >> 4);
    const uint8_t logicOp = static_cast<uint8_t>(opcode & 0x0Fu);
    const uint8_t colorMask = msx_vdp_command_mask(screenMode);
    uint16_t sx = msx_vdp_read_reg10(state, 32u);
    uint16_t sy = msx_vdp_read_reg10(state, 34u);
    uint16_t dx = msx_vdp_read_reg10(state, 36u);
    uint16_t dy = msx_vdp_read_reg10(state, 38u);
    uint16_t nx = msx_vdp_read_reg10(state, 40u);
    uint16_t ny = msx_vdp_read_reg10(state, 42u);
    const int txDot = (state->regs[45] & 0x04u) != 0u ? -1 : 1;
    const int ty = (state->regs[45] & 0x08u) != 0u ? -1 : 1;
    const bool searchNotEqual = (state->regs[45] & 0x02u) != 0u;
    const bool lineYMajor = (state->regs[45] & 0x01u) != 0u;
    const uint16_t ppl = msx_vdp_command_ppl(screenMode);
    const uint16_t ppb = msx_vdp_command_ppb(screenMode);
#if MSX_VDP_G4_ADDR_LOG_ENABLED
    static uint32_t s_g4AddrLogCount = 0u;
    static uint8_t s_g4LastR2 = 0xFFu;
    const auto g4_visible_band = [&](uint16_t coord) {
        const uint16_t height = static_cast<uint16_t>(state->activeHeight);
        return (coord < height) ||
               (coord >= 256u && coord < static_cast<uint16_t>(256u + height)) ||
               (coord >= 512u && coord < static_cast<uint16_t>(512u + height)) ||
               (coord >= 768u && coord < static_cast<uint16_t>(768u + height));
    };

    if (screenMode == 0u && state->regs[2] != s_g4LastR2) {
        s_g4LastR2 = state->regs[2];
        s_g4CmdLogCount = 0u;
        s_g4AddrLogCount = 0u;
    }

    const bool g4TouchesVisible = screenMode == 0u &&
                                  (g4_visible_band(sy) ||
                                   g4_visible_band(dy));
    const bool g4InterestingCmd = screenMode == 0u &&
                                  g4TouchesVisible &&
                                  command != 0x0Bu;

    const auto log_g4_addrs = [&](const char* tag,
                                  uint16_t srcX,
                                  uint16_t srcY,
                                  uint16_t dstX,
                                  uint16_t dstY,
                                  uint16_t width,
                                  uint16_t height,
                                  bool hasSource,
                                  bool hasDest) {
        if (!g4InterestingCmd || s_g4AddrLogCount >= 48u) {
            return;
        }

        ++s_g4AddrLogCount;
        const uint32_t srcAddr = hasSource ? (msx_vdp_command_addr(screenMode, srcX, srcY) & state->vramMask) : 0u;
        const uint32_t dstAddr = hasDest ? (msx_vdp_command_addr(screenMode, dstX, dstY) & state->vramMask) : 0u;
        const uint8_t s0 = hasSource ? state->vram[(srcAddr + 0u) & state->vramMask] : 0u;
        const uint8_t s1 = hasSource ? state->vram[(srcAddr + 1u) & state->vramMask] : 0u;
        const uint8_t s2 = hasSource ? state->vram[(srcAddr + 2u) & state->vramMask] : 0u;
        const uint8_t s3 = hasSource ? state->vram[(srcAddr + 3u) & state->vramMask] : 0u;
        const uint8_t d0 = hasDest ? state->vram[(dstAddr + 0u) & state->vramMask] : 0u;
        const uint8_t d1 = hasDest ? state->vram[(dstAddr + 1u) & state->vramMask] : 0u;
        const uint8_t d2 = hasDest ? state->vram[(dstAddr + 2u) & state->vramMask] : 0u;
        const uint8_t d3 = hasDest ? state->vram[(dstAddr + 3u) & state->vramMask] : 0u;
        const unsigned srcPage = static_cast<unsigned>((srcAddr >> 15) & 0x03u);
        const unsigned dstPage = static_cast<unsigned>((dstAddr >> 15) & 0x03u);
        std::printf("[MSX][G4-ADDR] #%lu %s sx=%u sy=%u src=%05lX sp=%u s=%02X%02X%02X%02X dx=%u dy=%u dst=%05lX dp=%u d=%02X%02X%02X%02X nx=%u ny=%u r2=%02X frame=%lu\n",
                    static_cast<unsigned long>(s_g4AddrLogCount),
                    tag,
                    static_cast<unsigned>(srcX),
                    static_cast<unsigned>(srcY),
                    static_cast<unsigned long>(srcAddr),
                    srcPage,
                    static_cast<unsigned>(s0),
                    static_cast<unsigned>(s1),
                    static_cast<unsigned>(s2),
                    static_cast<unsigned>(s3),
                    static_cast<unsigned>(dstX),
                    static_cast<unsigned>(dstY),
                    static_cast<unsigned long>(dstAddr),
                    dstPage,
                    static_cast<unsigned>(d0),
                    static_cast<unsigned>(d1),
                    static_cast<unsigned>(d2),
                    static_cast<unsigned>(d3),
                    static_cast<unsigned>(width),
                    static_cast<unsigned>(height),
                    static_cast<unsigned>(state->regs[2]),
                    static_cast<unsigned long>(state->frameCounter));
    };
#else
    const auto log_g4_addrs = [](const char*,
                                 uint16_t,
                                 uint16_t,
                                 uint16_t,
                                 uint16_t,
                                 uint16_t,
                                 uint16_t,
                                 bool,
                                 bool) {};
#endif

    if ((command & 0x0Cu) != 0x0Cu && command != 0u) {
        state->regs[44] = static_cast<uint8_t>(state->regs[44] & colorMask);
        state->status[7] = state->regs[44];
    }

    msx_vdp_diag_log_command_start(state, opcode, sx, sy, dx, dy, nx, ny);
    if (command != 0u) {
        const bool hasSource = command == 0x4u || command == 0x6u || command == 0x9u ||
                               command == 0xAu || command == 0xDu || command == 0xEu;
        const bool hasDest = command == 0x5u || command == 0x7u || command == 0x8u ||
                             command == 0x9u || command == 0xBu || command == 0xCu ||
                             command == 0xDu || command == 0xEu || command == 0xFu;
        const uint32_t srcAddr = hasSource ? (msx_vdp_command_addr(screenMode, sx, sy) & state->vramMask) : 0u;
        const uint32_t dstAddr = hasDest ? (msx_vdp_command_addr(screenMode, dx, dy) & state->vramMask) : 0u;
        V9938_BENCH_VDP_COMMAND(msx_vdp_command_label(command),
                                srcAddr,
                                dstAddr,
                                msx_vdp_command_estimated_bytes(command, nx, ny, ppb));
    }

    switch (command) {
        case 0x0:
            return;
        case 0x4: {
            const uint8_t value = msx_vdp_command_point(state, screenMode, sx, sy);
            state->regs[44] = value;
            state->status[7] = value;
            return;
        }
        case 0x5:
            msx_vdp_command_pset(state, screenMode, dx, dy, state->regs[44], logicOp);
            return;
        case 0x6: {
            const uint8_t color = static_cast<uint8_t>(state->regs[44] & colorMask);
            int x = sx;
            state->status[2] &= static_cast<uint8_t>(~0x10u);
            for (;;) {
                if (((msx_vdp_command_point(state, screenMode, x, sy) == color) ? 1 : 0) ^ (searchNotEqual ? 1 : 0)) {
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
            MsxVdpCommandState& line = state->command;
            line.transfer = MsxVdpTransferCommand::Line;
            line.screenMode = screenMode;
            line.logicOp = logicOp;
            line.sx = sx;
            line.sy = sy;
            line.dx = dx;
            line.dy = dy;
            line.nx = nx;
            line.ny = ny;
            line.asx = nx == 0u ? 0xFFFFu : static_cast<uint16_t>((nx - 1u) >> 1);
            line.adx = 0u;
            line.anx = nx;
            line.tx = static_cast<int16_t>(txDot);
            line.ty = static_cast<int16_t>(ty);
            line.mx = ppl;
            line.lineYMajor = lineYMajor;
            line.cycleStamp = state->currentFrameCpuCycles;
            state->status[2] |= 0x01u;
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0x8: {
            msx_vdp_command_prepare_engine(state,
                                           MsxVdpTransferCommand::Lmmv,
                                           screenMode,
                                           logicOp,
                                           false);
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0x9: {
            log_g4_addrs("LMMM", sx, sy, dx, dy, nx, ny, true, true);
            msx_vdp_command_prepare_engine(state,
                                           MsxVdpTransferCommand::Lmmm,
                                           screenMode,
                                           logicOp,
                                           false);
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0xA:
            msx_vdp_command_prepare_transfer(state, MsxVdpTransferCommand::Lmcm, screenMode, opcode);
            msx_vdp_command_continue(state);
            return;
        case 0xB:
            msx_vdp_command_prepare_transfer(state, MsxVdpTransferCommand::Lmmc, screenMode, opcode);
            msx_vdp_command_continue(state);
            return;
        case 0xC: {
            log_g4_addrs("HMMV", 0u, 0u, dx, dy, nx, ny, false, true);
            msx_vdp_command_prepare_engine(state,
                                           MsxVdpTransferCommand::Hmmv,
                                           screenMode,
                                           logicOp,
                                           true);
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0xD: {
            log_g4_addrs("HMMM", sx, sy, dx, dy, nx, ny, true, true);
            msx_vdp_command_prepare_engine(state,
                                           MsxVdpTransferCommand::Hmmm,
                                           screenMode,
                                           logicOp,
                                           true);
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0xE: {
            log_g4_addrs("YMMM", dx, sy, dx, dy, nx, ny, true, true);
            msx_vdp_command_prepare_engine(state,
                                           MsxVdpTransferCommand::Ymmm,
                                           screenMode,
                                           logicOp,
                                           true);
            if (msx_config_get_performance_flag(MsxPerformanceFlag::InstantVdpCommands)) {
                msx_vdp_command_complete_async_bulk(state, screenMode);
            }
            return;
        }
        case 0xF:
            msx_vdp_command_prepare_transfer(state, MsxVdpTransferCommand::Hmmc, screenMode, opcode);
            msx_vdp_command_continue(state);
            return;
        default:
            return;
    }
}

void msx_vdp_write_register(MsxVdpState* state, uint8_t reg, uint8_t value)
{
    if (!state || reg >= sizeof(state->regs)) {
        return;
    }

    if (reg == 14u && msx_vdp_is_msx2(state)) {
        value &= msx_vdp_vram_page_mask(state);
    }

    if (reg == 44u && msx_vdp_is_msx2(state)) {
        msx_vdp_command_write(state, value);
        return;
    }

#if MSX_VDP_TRACE_ENABLED
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
 #if MSX_VDP_VERBOSE_DIAG_ENABLED
        static uint32_t s_msx2KeyRegLogCount = 0u;
        if (msx_vdp_is_msx2(state) && msx_vdp_is_traced_register(reg) && s_msx2KeyRegLogCount < 192u) {
            ++s_msx2KeyRegLogCount;
            std::printf("[MSX][REG] #%lu R%02u=%02X prev=%02X mode=%s frame=%lu\n",
                        static_cast<unsigned long>(s_msx2KeyRegLogCount),
                        static_cast<unsigned>(reg),
                        static_cast<unsigned>(value),
                        static_cast<unsigned>(state->regs[reg]),
                        msx_vdp_mode_label(state->mode),
                        static_cast<unsigned long>(state->frameCounter));
        }
#endif
        state->regs[reg] = value;
        if (msx_vdp_is_msx2(state)) {
            if (reg == 5u) {
                msx_vdp_timeline_append(s_msxVdpR5Timeline,
                                        &s_msxVdpR5TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 6u) {
                msx_vdp_timeline_append(s_msxVdpR6Timeline,
                                        &s_msxVdpR6TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 8u) {
                msx_vdp_timeline_append(s_msxVdpR8Timeline,
                                        &s_msxVdpR8TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 11u) {
                msx_vdp_timeline_append(s_msxVdpR11Timeline,
                                        &s_msxVdpR11TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 2u) {
                msx_vdp_timeline_append(s_msxVdpR2Timeline,
                                        &s_msxVdpR2TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 23u) {
                msx_vdp_timeline_append(s_msxVdpR23Timeline,
                                        &s_msxVdpR23TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 25u) {
                msx_vdp_timeline_append(s_msxVdpR25Timeline,
                                        &s_msxVdpR25TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 26u) {
                msx_vdp_timeline_append(s_msxVdpR26Timeline,
                                        &s_msxVdpR26TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            } else if (reg == 27u) {
                msx_vdp_timeline_append(s_msxVdpR27Timeline,
                                        &s_msxVdpR27TimelineCount,
                                        state->currentFrameCpuCycles,
                                        value);
            }
        }
        if (msx_vdp_register_affects_output(reg)) {
            state->dirty = true;
        }
    }

    if (reg == 16u) {
        state->palettePending = false;
    }
    if (reg == 14u) {
        state->address = msx_vdp_compose_address(state, static_cast<uint16_t>(state->address & 0x3FFFu));
    }
    if (reg == 46u && msx_vdp_is_msx2(state)) {
        msx_vdp_command_execute(state, value);
    }
}

void msx_vdp_reset_sprite_status(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    // Keep VBLANK flag (bit 7); clear overflow (bit 6) and collision (bit 5);
    // reset 5th-sprite number to 0x1F (31 = "no 5th sprite").
    state->status[0] = static_cast<uint8_t>((state->status[0] & 0x80u) | 0x1Fu);
}

static void msx_vdp_invalidate_mono_sprite_cache(void)
{
    s_msxMonoSpriteCacheValid = false;
    s_msxMonoSpriteInputHeight = 8u;
    s_msxMonoSpriteScale = 1u;
}

static void msx_vdp_prepare_mono_sprite_cache(MsxVdpState* state)
{
    msx_vdp_invalidate_mono_sprite_cache();
    if (!state || msx_vdp_is_msx2(state)) {
        return;
    }

    static constexpr uint8_t kSpriteHeights[4] = {8u, 16u, 16u, 32u};
    const uint8_t outputHeight = kSpriteHeights[state->regs[1] & 0x03u];
    s_msxMonoSpriteInputHeight = kSpriteHeights[state->regs[1] & 0x02u];
    s_msxMonoSpriteScale = (outputHeight > s_msxMonoSpriteInputHeight) ? 2u : 1u;

    const uint32_t attrBase = msx_vdp_sprite_attr_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    uint8_t scanEndIndex = 31u;

    for (unsigned line = 0u; line < kMsxFrameHeightMsx1; ++line) {
        MsxMonoSpriteLineCache& cache = s_msxMonoSpriteLines[line];
        cache.count = 0u;
        cache.lastIndex = 31u;
        cache.overflowIndex = 31u;
        cache.hasOverflow = false;
    }

    for (uint8_t index = 0u; index < 32u; ++index) {
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        const uint8_t rawY = msx_vdp_read_vram_fast(vram, mask, attr);
        if (rawY == 208u) {
            scanEndIndex = index;
            break;
        }

        MsxMonoSpriteCachedAttr& sprite = s_msxMonoSpriteAttrs[index];
        sprite.y = static_cast<int16_t>(rawY);
        if (sprite.y > static_cast<int16_t>(256 - s_msxMonoSpriteInputHeight)) {
            sprite.y = static_cast<int16_t>(sprite.y - 256);
        }
        sprite.x = static_cast<int16_t>(msx_vdp_read_vram_fast(vram, mask, attr + 1u));
        sprite.patternId = msx_vdp_read_vram_fast(vram, mask, attr + 2u);
        sprite.attrColor = msx_vdp_read_vram_fast(vram, mask, attr + 3u);
        if ((sprite.attrColor & 0x80u) != 0u) {
            sprite.x = static_cast<int16_t>(sprite.x - 32);
        }

        int startY = static_cast<int>(sprite.y) + 1;
        int endY = static_cast<int>(sprite.y) + outputHeight;
        if (endY < 0 || startY >= static_cast<int>(kMsxFrameHeightMsx1)) {
            continue;
        }

        if (startY < 0) {
            startY = 0;
        }
        if (endY >= static_cast<int>(kMsxFrameHeightMsx1)) {
            endY = static_cast<int>(kMsxFrameHeightMsx1) - 1;
        }

        for (int line = startY; line <= endY; ++line) {
            MsxMonoSpriteLineCache& cache = s_msxMonoSpriteLines[line];
            if (cache.count < kMsxMaxSpritesLineMsx1) {
                cache.selected[cache.count++] = index;
            } else if (!cache.hasOverflow) {
                cache.hasOverflow = true;
                cache.overflowIndex = index;
            }
        }
    }

    for (unsigned line = 0u; line < kMsxFrameHeightMsx1; ++line) {
        MsxMonoSpriteLineCache& cache = s_msxMonoSpriteLines[line];
        cache.lastIndex = cache.hasOverflow ? cache.overflowIndex : scanEndIndex;
    }

    s_msxMonoSpriteCacheValid = true;
}

void msx_vdp_record_sprite_index(MsxVdpState* state, uint8_t index)
{
    if (!state) {
        return;
    }

    state->status[0] = static_cast<uint8_t>((state->status[0] & ~0x1Fu) | (index & 0x1Fu));
    if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
        s_vdpOriginalState->status[0] = static_cast<uint8_t>((s_vdpOriginalState->status[0] & ~0x1Fu) | (index & 0x1Fu));
    }
}

void msx_vdp_record_sprite_overflow(MsxVdpState* state, uint8_t index)
{
    if (!state || (state->status[0] & 0x40u) != 0u) {
        return;
    }

    msx_vdp_record_sprite_index(state, index);
    state->status[0] |= 0x40u;
    if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
        s_vdpOriginalState->status[0] |= 0x40u;
    }
}

inline void msx_vdp_record_sprite_collision(MsxVdpState* state)
{
    if (state) {
        state->status[0] |= 0x20u;
        if (state == &s_vdpStateSnapshot && s_vdpOriginalState) {
            s_vdpOriginalState->status[0] |= 0x20u;
        }
    }
}

void msx_vdp_plot_sprite_bits(MsxVdpState* state,
                              uint8_t* dst,
                              uint8_t* occupancy,
                              int x,
                              uint8_t pattern,
                              uint8_t color,
                              unsigned scale,
                              bool detectCollision)
{
    for (unsigned bit = 0; bit < 8u; ++bit) {
        if ((pattern & static_cast<uint8_t>(0x80u >> bit)) == 0u) {
            continue;
        }

        const int pixelBase = x + static_cast<int>(bit * scale);
        for (unsigned repeat = 0; repeat < scale; ++repeat) {
            const int px = pixelBase + static_cast<int>(repeat);
            if (px < 0 || px >= static_cast<int>(kMsxFrameWidth)) {
                continue;
            }
            if (detectCollision) {
                if (occupancy[px] != 0u) {
                    msx_vdp_record_sprite_collision(state);
                }
                occupancy[px] = 1u;
            }
            dst[px] = color;
        }
    }
}

void msx_vdp_render_mono_sprites_line(MsxVdpState* state, unsigned y, uint8_t* dst)
{
    if (!state || !dst || msx_vdp_sprites_disabled_for_line(state, y)) {
        return;
    }

    if (!msx_vdp_is_msx2(state) && y < kMsxFrameHeightMsx1) {
        if (!s_msxMonoSpriteCacheValid) {
            msx_vdp_prepare_mono_sprite_cache(state);
        }

        if (s_msxMonoSpriteCacheValid) {
            const MsxMonoSpriteLineCache& cache = s_msxMonoSpriteLines[y];
            const uint32_t patternBase = msx_vdp_sprite_pattern_base(state);
            const bool accurateSpriteOverflow =
                !msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow);
            const bool detectSpriteCollision =
                !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision);

            if (cache.hasOverflow && accurateSpriteOverflow) {
                msx_vdp_record_sprite_overflow(state, cache.overflowIndex);
            } else {
                msx_vdp_record_sprite_index(state, cache.lastIndex);
            }

            if (detectSpriteCollision) {
                std::memset(s_msxSpriteOccupancy, 0, sizeof(s_msxSpriteOccupancy));
            }

            const bool largeSprite = s_msxMonoSpriteInputHeight > 8u;
            for (int selectedIndex = static_cast<int>(cache.count) - 1; selectedIndex >= 0; --selectedIndex) {
                const uint8_t index = cache.selected[static_cast<size_t>(selectedIndex)];
                const MsxMonoSpriteCachedAttr& sprite = s_msxMonoSpriteAttrs[index];
                const uint8_t color = static_cast<uint8_t>(sprite.attrColor & 0x0Fu);
                if (color == 0u) {
                    continue;
                }

                int line = static_cast<int>(y) - static_cast<int>(sprite.y) - 1;
                if (s_msxMonoSpriteScale == 2u) {
                    line >>= 1;
                }
                if (line < 0) {
                    continue;
                }

                const uint8_t spriteColor = msx_vdp_resolve_color(state, color);
                const uint8_t basePattern =
                    largeSprite ? static_cast<uint8_t>(sprite.patternId & 0xFCu) : sprite.patternId;
                const uint32_t patternRow =
                    patternBase + static_cast<uint32_t>(basePattern) * 8u + static_cast<uint32_t>(line);
                const uint8_t leftBits =
                    msx_vdp_read_vram_fast(state->vram, state->vramMask, patternRow);
                msx_vdp_plot_sprite_bits(state,
                                         dst,
                                         s_msxSpriteOccupancy,
                                         static_cast<int>(sprite.x),
                                         leftBits,
                                         spriteColor,
                                         s_msxMonoSpriteScale,
                                         detectSpriteCollision);

                if (largeSprite) {
                    const uint8_t rightBits =
                        msx_vdp_read_vram_fast(state->vram, state->vramMask, patternRow + 16u);
                    msx_vdp_plot_sprite_bits(state,
                                             dst,
                                             s_msxSpriteOccupancy,
                                             static_cast<int>(sprite.x)
                                                 + static_cast<int>(8u * s_msxMonoSpriteScale),
                                             rightBits,
                                             spriteColor,
                                             s_msxMonoSpriteScale,
                                             detectSpriteCollision);
                }
            }
            return;
        }
    }

    static constexpr uint8_t kSpriteHeights[4] = {8u, 16u, 16u, 32u};
    const uint8_t outputHeight = kSpriteHeights[state->regs[1] & 0x03u];
    const uint8_t inputHeight = kSpriteHeights[state->regs[1] & 0x02u];
    const uint32_t attrBase = msx_vdp_sprite_attr_base(state);
    const uint32_t patternBase = msx_vdp_sprite_pattern_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    uint8_t selected[32];
    uint8_t count = 0u;
    uint8_t lastIndex = 31u;
    const unsigned spriteLimit = kMsxMaxSpritesLineMsx1;
    const bool accurateSpriteOverflow =
        !msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow);
    const bool detectSpriteCollision =
        !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision);

    for (uint8_t index = 0; index < 32u; ++index) {
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(msx_vdp_read_vram_fast(vram, mask, attr));
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
                if (accurateSpriteOverflow) {
                    msx_vdp_record_sprite_overflow(state, index);
                }
                lastIndex = index;
                break;
            }
            selected[count++] = index;
        }

        lastIndex = index;
    }

    msx_vdp_record_sprite_index(state, lastIndex);
    if (detectSpriteCollision) {
        std::memset(s_msxSpriteOccupancy, 0, sizeof(s_msxSpriteOccupancy));
    }

    const unsigned scale = (outputHeight > inputHeight) ? 2u : 1u;
    for (int selectedIndex = static_cast<int>(count) - 1; selectedIndex >= 0; --selectedIndex) {
        const uint8_t index = selected[static_cast<size_t>(selectedIndex)];
        const uint32_t attr = attrBase + static_cast<uint32_t>(index) * 4u;
        int spriteY = static_cast<int>(msx_vdp_read_vram_fast(vram, mask, attr));
        if (spriteY > (256 - inputHeight)) {
            spriteY -= 256;
        }

        const uint8_t xRaw = msx_vdp_read_vram_fast(vram, mask, attr + 1u);
        const uint8_t patternId = msx_vdp_read_vram_fast(vram, mask, attr + 2u);
        const uint8_t attrColor = msx_vdp_read_vram_fast(vram, mask, attr + 3u);
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

        const uint8_t spriteColor = msx_vdp_resolve_color(state, color);
        const bool largeSprite = inputHeight > 8u;
        const uint8_t basePattern = largeSprite ? static_cast<uint8_t>(patternId & 0xFCu) : patternId;
        const uint32_t patternRow = !msx_vdp_is_msx2(state)
            ? (patternBase + static_cast<uint32_t>(basePattern) * 8u + static_cast<uint32_t>(line))
            : (patternBase
               + static_cast<uint32_t>(largeSprite
                                            ? static_cast<uint8_t>(basePattern + ((line & 0x08u) ? 2u : 0u))
                                            : basePattern) * 8u
               + static_cast<uint32_t>(line & 0x07u));
        const uint8_t leftBits = msx_vdp_read_vram_fast(vram, mask, patternRow);
        msx_vdp_plot_sprite_bits(state,
                                 dst,
                                 s_msxSpriteOccupancy,
                                 x,
                                 leftBits,
                                 spriteColor,
                                 scale,
                                 detectSpriteCollision);

        if (largeSprite) {
            const uint8_t rightBits = msx_vdp_read_vram_fast(vram, mask, patternRow + (msx_vdp_is_msx2(state) ? 8u : 16u));
            msx_vdp_plot_sprite_bits(state,
                                     dst,
                                     s_msxSpriteOccupancy,
                                     x + static_cast<int>(8u * scale),
                                     rightBits,
                                     spriteColor,
                                     scale,
                                     detectSpriteCollision);
        }
    }
}

void msx_vdp_plot_color_sprite_bits(MsxVdpState* state,
                                    uint8_t* line,
                                    int x,
                                    uint8_t pattern,
                                    uint8_t color,
                                    unsigned scale,
                                    bool mergeColors,
                                    bool detectCollision)
{
    for (unsigned bit = 0; bit < 8u; ++bit) {
        if ((pattern & static_cast<uint8_t>(0x80u >> bit)) == 0u) {
            continue;
        }

        const int pixelBase = x + static_cast<int>(bit * scale);
        for (unsigned repeat = 0; repeat < scale; ++repeat) {
            const int px = pixelBase + static_cast<int>(repeat);
            if (px < -32 || px >= static_cast<int>(kMsxFrameWidth + 32u)) {
                continue;
            }
            uint8_t& dst = line[static_cast<size_t>(px + 32)];
            if (detectCollision && dst != 0u) {
                msx_vdp_record_sprite_collision(state);
            }
            dst = mergeColors ? static_cast<uint8_t>(dst | color) : color;
#if MSX_VDP_SPRITE_LOG_ENABLED
            s_msxSpritePixelsDrawn++;
#endif
        }
    }
}

bool msx_vdp_render_color_sprites_line(MsxVdpState* state, unsigned y, uint8_t* line)
{
    if (!state || !line) {
        return false;
    }

    std::memset(line, 0, kMsxSpriteColorLineWidth);
    static constexpr uint8_t kSpriteHeights[4] = {8u, 16u, 16u, 32u};
    const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
    const uint8_t scrolledScanY =
        static_cast<uint8_t>(static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll));

    if (msx_vdp_sprites_disabled_for_line(state, y)) {
        return false;
    }

    const uint8_t spriteReg1 = state->regs[1];
    const uint8_t spriteReg5 = msx_vdp_reg5_for_line(state, y);
    const uint8_t spriteReg6 = msx_vdp_reg6_for_line(state, y);
    const uint8_t spriteReg11 = msx_vdp_reg11_for_line(state, y);
    const uint32_t attrBase = msx_vdp_color_sprite_attr_base_for_regs(spriteReg5, spriteReg11, state->vramMask);
    const uint32_t colorBase = (attrBase - 0x200u) & state->vramMask;
    const uint32_t patternBase = msx_vdp_color_sprite_pattern_base_for_reg6(spriteReg6, state->vramMask);
    const uint8_t outputHeight = kSpriteHeights[spriteReg1 & 0x03u];
    const uint8_t inputHeight = kSpriteHeights[spriteReg1 & 0x02u];
    const int outputHeightInt = static_cast<int>(outputHeight);
    const int inputHeightInt = static_cast<int>(inputHeight);
    uint8_t selected[32];
    uint8_t count = 0u;
    uint8_t lastIndex = 31u;
    uint8_t orMask = 0u;
    const bool accurateSpriteOverflow =
        !msx_config_get_performance_flag(MsxPerformanceFlag::SimplifySpriteOverflow);
    const bool detectSpriteCollision =
        !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSpriteCollision);

    for (uint8_t index = 0; index < 32u; ++index) {
        const uint32_t attr = static_cast<uint32_t>(index) * 4u;
        const uint8_t rawSpriteY = msx_vdp_read_vram_fast(state->vram, state->vramMask, attrBase + attr);
        if (rawSpriteY == 216u) {
            lastIndex = index;
            break;
        }
        const uint8_t spriteLine = static_cast<uint8_t>(scrolledScanY - rawSpriteY - 1u);
        if (spriteLine < outputHeight) {
            if (count >= kMsxMaxSpritesLineMsx2) {
                if (accurateSpriteOverflow) {
                    msx_vdp_record_sprite_overflow(state, index);
                }
                lastIndex = index;
                break;
            }
            selected[count++] = index;
        }

        lastIndex = index;
    }

    msx_vdp_record_sprite_index(state, lastIndex);
    if (count == 0u) {
        return false;
    }
#if MSX_VDP_SPRITE_LOG_ENABLED
    s_msxSpriteActiveLines++;
    s_msxSpriteSelectedCount += count;
#endif

    const unsigned scale = (outputHeight > inputHeight) ? 2u : 1u;
    for (int selectedIndex = static_cast<int>(count) - 1; selectedIndex >= 0; --selectedIndex) {
        const uint8_t index = selected[static_cast<size_t>(selectedIndex)];
        const uint32_t attr = static_cast<uint32_t>(index) * 4u;
        const uint8_t spriteY =
            msx_vdp_read_vram_fast(state->vram, state->vramMask, attrBase + attr);
        const uint8_t xRaw = msx_vdp_read_vram_fast(state->vram, state->vramMask, attrBase + attr + 1u);
        const uint8_t patternId = msx_vdp_read_vram_fast(state->vram, state->vramMask, attrBase + attr + 2u);
        int lineIndex = static_cast<int>(static_cast<uint8_t>(scrolledScanY - spriteY - 1u));
        if (lineIndex >= outputHeightInt) {
            continue;
        }
        if (scale == 2u) {
            lineIndex >>= 1;
        }
        if (lineIndex < 0) {
            continue;
        }

        const uint32_t colorOffset = static_cast<uint32_t>(index) * 16u + static_cast<uint32_t>(lineIndex);
        const uint8_t colorAttr = msx_vdp_read_vram_fast(state->vram, state->vramMask, colorBase + colorOffset);
        const uint8_t color = static_cast<uint8_t>(colorAttr & 0x0Fu);
        if (color == 0u) {
#if MSX_VDP_SPRITE_LOG_ENABLED
            s_msxSpriteTransparentEntries++;
#endif
            orMask = static_cast<uint8_t>((orMask | (colorAttr & 0x40u)) >> 1);
            continue;
        }
#if MSX_VDP_SPRITE_LOG_ENABLED
        s_msxSpriteColoredEntries++;
#endif

        const bool mergeColors = (orMask & 0x20u) != 0u;
        orMask = static_cast<uint8_t>((orMask | (colorAttr & 0x40u)) >> 1);
        const int x = static_cast<int>(xRaw) - (((colorAttr & 0x80u) != 0u) ? 32 : 0);
        const bool largeSprite = inputHeight > 8u;
        const uint8_t basePattern = largeSprite ? static_cast<uint8_t>(patternId & 0xFCu) : patternId;
        const uint32_t patternRow = static_cast<uint32_t>(basePattern) * 8u + static_cast<uint32_t>(lineIndex);
        msx_vdp_plot_color_sprite_bits(state,
                                      line,
                                      x,
                                      msx_vdp_read_vram_fast(state->vram, state->vramMask, patternBase + patternRow),
                                      color,
                                      scale,
                                      mergeColors,
                                      detectSpriteCollision);

        if (largeSprite) {
            msx_vdp_plot_color_sprite_bits(state,
                                           line,
                                           x + static_cast<int>(8u * scale),
                                      msx_vdp_read_vram_fast(state->vram, state->vramMask, patternBase + patternRow + 16u),
                                           color,
                                           scale,
                                           mergeColors,
                                           detectSpriteCollision);
        }
    }

    return true;
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
    if (msx_vdp_is_msx2(state) && m5 == 1u && m4 == 0u && m3 == 0u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Bitmap6;
    }
    if (msx_vdp_is_msx2(state) && m5 == 1u && m4 == 0u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Bitmap7;
    }
    if (msx_vdp_is_msx2(state) && m5 == 1u && m4 == 1u && m3 == 1u && m2 == 0u && m1 == 0u) {
        return MsxVdpMode::Bitmap8;
    }
    if (msx_vdp_is_msx2(state) && m5 == 0u && m4 == 1u && m3 == 0u && m2 == 0u && m1 == 1u) {
        return MsxVdpMode::Text80;
    }
    return MsxVdpMode::Unsupported;
}

void msx_vdp_update_mode_geometry(MsxVdpState* state)
{
#if MSX_VDP_MODE_LOG_ENABLED
    static MsxVdpMode s_lastLoggedMode = MsxVdpMode::Unsupported;
    static unsigned s_lastLoggedHeight = 0u;
    static uint8_t s_lastLoggedR2 = 0xFFu;
    static uint8_t s_lastLoggedR9 = 0xFFu;
    static uint8_t s_lastLoggedR25 = 0xFFu;
    static uint8_t s_lastLoggedR26 = 0xFFu;
    static uint8_t s_lastLoggedR27 = 0xFFu;
#endif

    if (!state) {
        return;
    }

    state->mode = msx_vdp_decode_mode(state);
    state->activeWidth = kMsxFrameWidth;
    state->activeHeight = kMsxFrameHeightMsx1;
    if (state->mode == MsxVdpMode::Text80) {
        state->activeWidth = 240u;
    }
    if (msx_vdp_is_msx2(state) &&
        state->mode != MsxVdpMode::Text40 &&
        state->mode != MsxVdpMode::Text80 &&
        state->mode != MsxVdpMode::Unsupported &&
        (state->regs[9] & 0x80u) != 0u) {
        state->activeHeight = kMsxFrameHeightMsx2;
    }

#if MSX_VDP_MODE_LOG_ENABLED
    if (msx_vdp_is_msx2(state) &&
        (state->mode != s_lastLoggedMode ||
         state->activeHeight != s_lastLoggedHeight ||
         state->regs[2] != s_lastLoggedR2 ||
         state->regs[9] != s_lastLoggedR9 ||
         state->regs[25] != s_lastLoggedR25 ||
         state->regs[26] != s_lastLoggedR26 ||
         state->regs[27] != s_lastLoggedR27)) {
        std::printf("[MSX][VDP] mode=%s yjk=%d yae=%d h=%u r2=%02X r9=%02X r25=%02X r26=%02X r27=%02X hs=%u hs512=%d\n",
                    msx_vdp_mode_label(state->mode),
                    static_cast<int>(msx_vdp_mode_yjk(state)),
                    static_cast<int>(msx_vdp_mode_yae(state)),
                    static_cast<unsigned>(state->activeHeight),
                    static_cast<unsigned>(state->regs[2]),
                    static_cast<unsigned>(state->regs[9]),
                    static_cast<unsigned>(state->regs[25]),
                    static_cast<unsigned>(state->regs[26]),
                    static_cast<unsigned>(state->regs[27]),
                    static_cast<unsigned>(msx_vdp_hscroll(state)),
                    static_cast<int>(msx_vdp_hscroll512(state)));
        s_lastLoggedMode = state->mode;
        s_lastLoggedHeight = state->activeHeight;
        s_lastLoggedR2 = state->regs[2];
        s_lastLoggedR9 = state->regs[9];
        s_lastLoggedR25 = state->regs[25];
        s_lastLoggedR26 = state->regs[26];
        s_lastLoggedR27 = state->regs[27];
    }
#endif
}

void msx_vdp_clear_active_frame(MsxVdpState* state, uint8_t color)
{
    if (!state) {
        return;
    }

    const unsigned height = state->activeHeight;
    for (unsigned y = 0u; y < height; ++y) {
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            return;
        }
        std::memset(dst, color, kMsxFrameWidth);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static bool msx_vdp_slice_bounds(unsigned height, unsigned yStart, unsigned yEnd,
                                 unsigned* startLine, unsigned* endLine)
{
    if (!startLine || !endLine || height == 0u) {
        return false;
    }

    *startLine = yStart < height ? yStart : height;
    *endLine = yEnd < height ? yEnd : height;
    return *endLine > *startLine;
}

void msx_vdp_render_graphics1(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = msx_vdp_vscroll(state);

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(vscroll);
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        const uint32_t nameRowBase = nameBase + row * 32u;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameRowBase + tileX);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (name >> 3));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            msx_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        msx_vdp_render_mono_sprites_line(state, y, dst);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

void msx_vdp_render_text40(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    msx_vdp_clear_active_frame(state, bg);

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        uint8_t* lineDst = dst + 8u;

        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 40u + col);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const unsigned pixelBase = col * 6u;

            for (unsigned bit = 0; bit < 6; ++bit) {
                lineDst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }

        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

void msx_vdp_render_graphics2_like(MsxVdpState* state)
{
    const bool isMsx2 = msx_vdp_is_msx2(state);
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_graphics2_pattern_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternMask = msx_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = msx_vdp_vscroll(state);

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + vscroll);
        // Follow the fMSX SCREEN 2 addressing scheme directly:
        // T = ChrTab + ((Y & 0xF8) << 2)
        // I = ((Y & 0xC0) << 5) + (Y & 0x07)
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = isMsx2
            ? msx_vdp_get_line_buffer(state, y)
            : (state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth);
        if (!dst) {
            continue;
        }
        const uint32_t nameLineBase = nameBase + nameIndex;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameLineBase + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            msx_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        msx_vdp_render_mono_sprites_line(state, y, dst);
        if (isMsx2) {
            msx_vdp_stream_line_if_needed(state, dst, y);
        }
    }
}

void msx_vdp_render_text80(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t nameMask = msx_vdp_name_mask(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t altFg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] >> 4));
    const uint8_t altBg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    msx_vdp_clear_active_frame(state, bg);

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const uint32_t patternLine = static_cast<uint32_t>(scrolledY & 0x07u);
        const uint32_t nameOffset = (static_cast<uint32_t>(80u) * (scrolledY >> 3)) & nameMask;
        const uint32_t colorOffset = (static_cast<uint32_t>(10u) * (scrolledY >> 3)) & colorMask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        uint8_t* lineDst = dst + 8u;

        for (unsigned col = 0; col < 80u; ++col) {
            const uint8_t glyph = msx_vdp_read_vram_fast(vram, mask, nameBase + nameOffset + col);
            const uint8_t attrMask = msx_vdp_read_vram_fast(vram, mask, colorBase + colorOffset + (col >> 3));
            const bool useAlt = (attrMask & static_cast<uint8_t>(0x80u >> (col & 0x07u))) != 0u;
            const uint8_t pixel = msx_vdp_read_vram_fast(vram, mask, patternBase + static_cast<uint32_t>(glyph) * 8u + patternLine);
            const uint8_t drawFg = useAlt ? altFg : fg;
            const uint8_t drawBg = useAlt ? altBg : bg;
            const unsigned pixelBase = col * 3u;
            lineDst[pixelBase + 0u] = (pixel & 0xC0u) != 0u ? drawFg : drawBg;
            lineDst[pixelBase + 1u] = (pixel & 0x30u) != 0u ? drawFg : drawBg;
            lineDst[pixelBase + 2u] = (pixel & 0x0Cu) != 0u ? drawFg : drawBg;
        }

        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

void msx_vdp_render_multicolor(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < kMsxFrameHeightMsx1; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned lineBlock = (scrolledY & 0x04u) != 0 ? 4u : 0u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

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

        msx_vdp_render_mono_sprites_line(state, y, dst);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

void msx_vdp_render_graphics3(MsxVdpState* state)
{
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternMask = msx_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = 0; y < state->activeHeight; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + nameIndex + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            if (hasSprites) {
                for (unsigned bit = 0; bit < 8u; ++bit) {
                    const unsigned px = pixelBase + bit;
                    const uint8_t spriteColor = spriteLine[px];
                    dst[px] = spriteColor != 0u
                        ? msx_vdp_resolve_color(state, spriteColor)
                        : (((pattern << bit) & 0x80u) != 0 ? fg : bg);
                }
            } else {
                for (unsigned bit = 0; bit < 8u; ++bit) {
                    dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
                }
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_graphics1_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    if (!state || !msx_vdp_slice_bounds(kMsxFrameHeightMsx1, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = msx_vdp_vscroll(state);

    for (unsigned y = startLine; y < endLine; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(vscroll);
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        const uint32_t nameRowBase = nameBase + row * 32u;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameRowBase + tileX);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (name >> 3));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            msx_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        msx_vdp_render_mono_sprites_line(state, y, dst);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_text40_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    if (!state || !msx_vdp_slice_bounds(kMsxFrameHeightMsx1, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    if (startLine == 0u && endLine == kMsxFrameHeightMsx1) {
        msx_vdp_clear_active_frame(state, bg);
    }

    for (unsigned y = startLine; y < endLine; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned line = scrolledY & 0x07u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        uint8_t* lineDst = dst + 8u;

        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + row * 40u + col);
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + name * 8u + line);
            const unsigned pixelBase = col * 6u;

            for (unsigned bit = 0; bit < 6; ++bit) {
                lineDst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
            }
        }

        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_graphics2_like_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    if (!state || !msx_vdp_slice_bounds(kMsxFrameHeightMsx1, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const bool isMsx2 = msx_vdp_is_msx2(state);
    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_graphics2_pattern_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternMask = msx_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t vscroll = msx_vdp_vscroll(state);

    for (unsigned y = startLine; y < endLine; ++y) {
        const uint8_t scrolledY = static_cast<uint8_t>(y + vscroll);
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = isMsx2
            ? msx_vdp_get_line_buffer(state, y)
            : (state->frameBuffer + static_cast<size_t>(y) * kMsxFrameWidth);
        if (!dst) {
            continue;
        }
        const uint32_t nameLineBase = nameBase + nameIndex;

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameLineBase + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            msx_vdp_write_pattern_pixels(dst + tileX * 8u, pattern, fg, bg);
        }

        msx_vdp_render_mono_sprites_line(state, y, dst);
        if (isMsx2) {
            msx_vdp_stream_line_if_needed(state, dst, y);
        }
    }
}

static void msx_vdp_render_text80_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    if (!state || !msx_vdp_slice_bounds(kMsxFrameHeightMsx1, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t nameMask = msx_vdp_name_mask(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] >> 4));
    const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[7] & 0x0Fu));
    const uint8_t altFg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] >> 4));
    const uint8_t altBg = msx_vdp_resolve_color(state, static_cast<uint8_t>(state->regs[12] & 0x0Fu));
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    if (startLine == 0u && endLine == kMsxFrameHeightMsx1) {
        msx_vdp_clear_active_frame(state, bg);
    }

    for (unsigned y = startLine; y < endLine; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const uint32_t patternLine = static_cast<uint32_t>(scrolledY & 0x07u);
        const uint32_t nameOffset = (static_cast<uint32_t>(80u) * (scrolledY >> 3)) & nameMask;
        const uint32_t colorOffset = (static_cast<uint32_t>(10u) * (scrolledY >> 3)) & colorMask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }
        uint8_t* lineDst = dst + 8u;

        for (unsigned col = 0; col < 80u; ++col) {
            const uint8_t glyph = msx_vdp_read_vram_fast(vram, mask, nameBase + nameOffset + col);
            const uint8_t attrMask = msx_vdp_read_vram_fast(vram, mask, colorBase + colorOffset + (col >> 3));
            const bool useAlt = (attrMask & static_cast<uint8_t>(0x80u >> (col & 0x07u))) != 0u;
            const uint8_t pixel = msx_vdp_read_vram_fast(vram, mask, patternBase + static_cast<uint32_t>(glyph) * 8u + patternLine);
            const uint8_t drawFg = useAlt ? altFg : fg;
            const uint8_t drawBg = useAlt ? altBg : bg;
            const unsigned pixelBase = col * 3u;
            lineDst[pixelBase + 0u] = (pixel & 0xC0u) != 0u ? drawFg : drawBg;
            lineDst[pixelBase + 1u] = (pixel & 0x30u) != 0u ? drawFg : drawBg;
            lineDst[pixelBase + 2u] = (pixel & 0x0Cu) != 0u ? drawFg : drawBg;
        }

        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_multicolor_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    if (!state || !msx_vdp_slice_bounds(kMsxFrameHeightMsx1, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t patternBase = static_cast<uint32_t>(state->regs[4] & 0x07u) << 11;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = startLine; y < endLine; ++y) {
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const unsigned row = scrolledY >> 3;
        const unsigned lineBlock = (scrolledY & 0x04u) != 0 ? 4u : 0u;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

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

        msx_vdp_render_mono_sprites_line(state, y, dst);
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_graphics3_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    const unsigned height = state ? state->activeHeight : 0u;
    if (!state || !msx_vdp_slice_bounds(height, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t nameBase = msx_vdp_name_base(state);
    const uint32_t colorBase = msx_vdp_color_base(state);
    const uint32_t patternBase = msx_vdp_pattern_base(state);
    const uint32_t colorMask = msx_vdp_color_mask(state);
    const uint32_t patternMask = msx_vdp_pattern_mask(state);
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;

    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(msx_vdp_vscroll(state));
        const uint32_t nameIndex = static_cast<uint32_t>(scrolledY & 0xF8u) << 2;
        const uint32_t patternIndex = (static_cast<uint32_t>(scrolledY & 0xC0u) << 5)
                                    | static_cast<uint32_t>(scrolledY & 0x07u);
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        for (unsigned tileX = 0; tileX < 32; ++tileX) {
            const uint8_t name = msx_vdp_read_vram_fast(vram, mask, nameBase + nameIndex + tileX);
            const uint32_t tileIndex = patternIndex + static_cast<uint32_t>(name) * 8u;
            const uint8_t pattern = msx_vdp_read_vram_fast(vram, mask, patternBase + (tileIndex & patternMask));
            const uint8_t color = msx_vdp_read_vram_fast(vram, mask, colorBase + (tileIndex & colorMask));
            const uint8_t fg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color >> 4));
            const uint8_t bg = msx_vdp_resolve_color(state, static_cast<uint8_t>(color & 0x0Fu));
            const unsigned pixelBase = tileX * 8u;

            if (hasSprites) {
                for (unsigned bit = 0; bit < 8u; ++bit) {
                    const unsigned px = pixelBase + bit;
                    const uint8_t spriteColor = spriteLine[px];
                    dst[px] = spriteColor != 0u
                        ? msx_vdp_resolve_color(state, spriteColor)
                        : (((pattern << bit) & 0x80u) != 0 ? fg : bg);
                }
            } else {
                for (unsigned bit = 0; bit < 8u; ++bit) {
                    dst[pixelBase + bit] = ((pattern << bit) & 0x80u) != 0 ? fg : bg;
                }
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_bitmap4_range(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame)
{
    if (!state) {
        return;
    }

    const unsigned height = state->activeHeight;
    if (height == 0u) {
        return;
    }

    const unsigned startLine = yStart < height ? yStart : height;
    const unsigned endLine = yEnd < height ? yEnd : height;
    if (endLine <= startLine) {
        return;
    }

    // SCREEN 5 display fetch wraps within the selected 0x8000-byte page.
    // R#2 bits 6:5 select the display page; R#23 selects the first displayed line
    // within that same page.
    // Do not infer alternate pages from write patterns: the VDP fetches only
    // from the page selected by the live registers.
    const uint32_t lineMask = 0x7FFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
#if MSX_VDP_G4_DISP_LOG_ENABLED
    const unsigned probeY = height > 160u ? 160u : (height > 0u ? (height - 1u) : 0u);
    static uint32_t s_g4DispLogCount = 0u;
    static uint32_t s_g4DispLastFrame = 0xFFFFFFFFu;
    static uint8_t s_g4LastDispR2 = 0xFFu;
    static uint8_t s_g4LastDispR23 = 0xFFu;
    static uint8_t s_g4LastDispR25 = 0xFFu;
    static uint8_t s_g4LastDispR26 = 0xFFu;
    static uint8_t s_g4LastDispR27 = 0xFFu;
    static uint8_t s_g4DispBudget = 0u;

    if (state->regs[2] != s_g4LastDispR2 ||
        state->regs[23] != s_g4LastDispR23 ||
        state->regs[25] != s_g4LastDispR25 ||
        state->regs[26] != s_g4LastDispR26 ||
        state->regs[27] != s_g4LastDispR27) {
        s_g4LastDispR2 = state->regs[2];
        s_g4LastDispR23 = state->regs[23];
        s_g4LastDispR25 = state->regs[25];
        s_g4LastDispR26 = state->regs[26];
        s_g4LastDispR27 = state->regs[27];
        s_g4DispBudget = 1u;
    }
#endif

    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool useHScroll = lineHScroll != 0u;
        const bool dualPage = msx_vdp_hscroll512_for_regs(lineReg25);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        const uint32_t pageBase = (static_cast<uint32_t>(lineReg2 & 0x60u) << 10) & mask;
        const uint32_t rowOffset = (scrolledY << 7) & lineMask;
        const uint32_t lineBase = (pageBase + rowOffset) & mask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

#if MSX_VDP_G4_DISP_LOG_ENABLED
        if (y == probeY &&
            s_g4DispBudget != 0u &&
            s_g4DispLastFrame != state->frameCounter) {
            ++s_g4DispLogCount;
            s_g4DispLastFrame = state->frameCounter;
            s_g4DispBudget = 0u;

            const MsxVdpRegTimelineEvent* const r2Timeline = s_msxVdpR2Timeline;
            const uint8_t r2TimelineCount = s_msxVdpR2TimelineCount;
            const MsxVdpRegTimelineEvent* const r23Timeline = s_msxVdpR23Timeline;
            const uint8_t r23TimelineCount = s_msxVdpR23TimelineCount;
            const uint32_t probeCycle262 = msx_vdp_cycle_for_line(state, y);
            const uint32_t probeCycleVis = msx_vdp_cycle_for_line_visible_legacy(state, y);
            const uint8_t probeReg2Legacy =
                msx_vdp_timeline_value_for_cycle(r2Timeline, r2TimelineCount, probeCycleVis);
            const uint8_t probeReg23Legacy =
                msx_vdp_timeline_value_for_cycle(r23Timeline, r23TimelineCount, probeCycleVis);
            const uint8_t pageIndex = static_cast<uint8_t>((pageBase >> 15) & 0x03u);
            const uint32_t altBase = (pageBase ^ 0x08000u) & state->vramMask;
            const uint8_t altPageIndex = static_cast<uint8_t>((altBase >> 15) & 0x03u);
            const uint32_t altAddr = (altBase + rowOffset) & mask;
            const MsxBitmapFetchAddress fetch0 = msx_vdp_bitmap_fetch_address(pageBase,
                                                                              rowOffset,
                                                                              lineHScroll,
                                                                              0u,
                                                                              256u,
                                                                              dualPage,
                                                                              0x8000u);
            const MsxBitmapFetchAddress fetch128 = msx_vdp_bitmap_fetch_address(pageBase,
                                                                                rowOffset,
                                                                                lineHScroll,
                                                                                128u,
                                                                                256u,
                                                                                dualPage,
                                                                                0x8000u);
            std::printf("[MSX][G4-DISP] #%lu frame=%lu line=%u now=%lu r2=%02X r23=%02X r25=%02X r26=%02X r27=%02X r9=%02X legacy=%02X/%02X cyc=%lu/%lu live=%02X/%02X/%02X/%02X/%02X hs=%u hs512=%u sy=%u page=%u base=%05lX cur=%05lX %02X%02X%02X%02X altp=%u alt=%05lX %02X%02X%02X%02X fx0=%05lX/%u fx128=%05lX/%u\n",
                        static_cast<unsigned long>(s_g4DispLogCount),
                        static_cast<unsigned long>(state->frameCounter),
                        static_cast<unsigned>(y),
                        static_cast<unsigned long>(state->currentFrameCpuCycles),
                        static_cast<unsigned>(lineReg2),
                        static_cast<unsigned>(lineVScroll),
                        static_cast<unsigned>(lineReg25),
                        static_cast<unsigned>(lineReg26),
                        static_cast<unsigned>(lineReg27),
                        static_cast<unsigned>(state->regs[9]),
                        static_cast<unsigned>(probeReg2Legacy),
                        static_cast<unsigned>(probeReg23Legacy),
                        static_cast<unsigned long>(probeCycle262),
                        static_cast<unsigned long>(probeCycleVis),
                        static_cast<unsigned>(state->regs[2]),
                        static_cast<unsigned>(state->regs[23]),
                        static_cast<unsigned>(state->regs[25]),
                        static_cast<unsigned>(state->regs[26]),
                        static_cast<unsigned>(state->regs[27]),
                        static_cast<unsigned>(lineHScroll),
                        static_cast<unsigned>(dualPage ? 1u : 0u),
                        static_cast<unsigned>(scrolledY),
                        static_cast<unsigned>(pageIndex),
                        static_cast<unsigned long>(pageBase & state->vramMask),
                        static_cast<unsigned long>(lineBase),
                        static_cast<unsigned>(vram[(lineBase + 0u) & mask]),
                        static_cast<unsigned>(vram[(lineBase + 1u) & mask]),
                        static_cast<unsigned>(vram[(lineBase + 2u) & mask]),
                        static_cast<unsigned>(vram[(lineBase + 3u) & mask]),
                        static_cast<unsigned>(altPageIndex),
                        static_cast<unsigned long>(altAddr),
                        static_cast<unsigned>(vram[(altAddr + 0u) & mask]),
                        static_cast<unsigned>(vram[(altAddr + 1u) & mask]),
                        static_cast<unsigned>(vram[(altAddr + 2u) & mask]),
                        static_cast<unsigned>(vram[(altAddr + 3u) & mask]),
                        static_cast<unsigned long>(fetch0.lineBase & mask),
                        static_cast<unsigned>(fetch0.pixelX),
                        static_cast<unsigned long>(fetch128.lineBase & mask),
                        static_cast<unsigned>(fetch128.pixelX));
        }
#endif

        if (!useHScroll) {
            for (unsigned byteX = 0; byteX < (kMsxFrameWidth / 2u); ++byteX) {
                const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + byteX);
                if (hasSprites) {
                    msx_vdp_render_bitmap4_byte_with_sprites(packed, spriteLine + byteX * 2u, dst + byteX * 2u);
                } else {
                    msx_vdp_render_bitmap4_byte(packed, dst + byteX * 2u);
                }
            }
        } else {
            for (unsigned x = 0; x < kMsxFrameWidth; ++x) {
                const MsxBitmapFetchAddress fetch = msx_vdp_bitmap_fetch_address(pageBase,
                                                                                 rowOffset,
                                                                                 lineHScroll,
                                                                                 x,
                                                                                 256u,
                                                                                 dualPage,
                                                                                 0x8000u);
                const uint8_t pixel = msx_vdp_bitmap4_read_pixel(vram,
                                                                 mask,
                                                                 fetch.lineBase & mask,
                                                                 fetch.pixelX);
                dst[x] = hasSprites && spriteLine[x] != 0u ? spriteLine[x] : pixel;
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }

    if (finalizeFrame) {
#if MSX_VDP_SPRITE_LOG_ENABLED
        static uint32_t s_g4SpriteStatsFrames = 0u;
        s_g4SpriteStatsFrames++;
        if (s_g4SpriteStatsFrames >= 60u) {
            const uint32_t attrBase = msx_vdp_sprite_attr_base(state) & state->vramMask;
            const uint32_t colorBase = (attrBase - 0x200u) & state->vramMask;
            const uint32_t patternBase = msx_vdp_sprite_pattern_base(state) & state->vramMask;
            std::printf("[MSX][SPR-STATS] 60f lines=%lu sel=%lu colored=%lu transparent=%lu px=%lu sat=%05lX col=%05lX pat=%05lX r1=%02X r5=%02X r6=%02X r8=%02X r11=%02X\n",
                        static_cast<unsigned long>(s_msxSpriteActiveLines),
                        static_cast<unsigned long>(s_msxSpriteSelectedCount),
                        static_cast<unsigned long>(s_msxSpriteColoredEntries),
                        static_cast<unsigned long>(s_msxSpriteTransparentEntries),
                        static_cast<unsigned long>(s_msxSpritePixelsDrawn),
                        static_cast<unsigned long>(attrBase),
                        static_cast<unsigned long>(colorBase),
                        static_cast<unsigned long>(patternBase),
                        static_cast<unsigned>(state->regs[1]),
                        static_cast<unsigned>(state->regs[5]),
                        static_cast<unsigned>(state->regs[6]),
                        static_cast<unsigned>(state->regs[8]),
                        static_cast<unsigned>(state->regs[11]));
            if (s_msxSpriteSelectedCount == 0u) {
                const uint32_t sat0 = attrBase;
                const uint32_t sat1 = (attrBase + 4u) & state->vramMask;
                const uint32_t sat2 = (attrBase + 8u) & state->vramMask;
                const uint32_t sat3 = (attrBase + 12u) & state->vramMask;
                std::printf("[MSX][SPR-DUMP] s0=%02X,%02X,%02X,%02X s1=%02X,%02X,%02X,%02X s2=%02X,%02X,%02X,%02X s3=%02X,%02X,%02X,%02X c0=%02X c1=%02X c2=%02X c3=%02X\n",
                            static_cast<unsigned>(vram[(sat0 + 0u) & mask]),
                            static_cast<unsigned>(vram[(sat0 + 1u) & mask]),
                            static_cast<unsigned>(vram[(sat0 + 2u) & mask]),
                            static_cast<unsigned>(vram[(sat0 + 3u) & mask]),
                            static_cast<unsigned>(vram[(sat1 + 0u) & mask]),
                            static_cast<unsigned>(vram[(sat1 + 1u) & mask]),
                            static_cast<unsigned>(vram[(sat1 + 2u) & mask]),
                            static_cast<unsigned>(vram[(sat1 + 3u) & mask]),
                            static_cast<unsigned>(vram[(sat2 + 0u) & mask]),
                            static_cast<unsigned>(vram[(sat2 + 1u) & mask]),
                            static_cast<unsigned>(vram[(sat2 + 2u) & mask]),
                            static_cast<unsigned>(vram[(sat2 + 3u) & mask]),
                            static_cast<unsigned>(vram[(sat3 + 0u) & mask]),
                            static_cast<unsigned>(vram[(sat3 + 1u) & mask]),
                            static_cast<unsigned>(vram[(sat3 + 2u) & mask]),
                            static_cast<unsigned>(vram[(sat3 + 3u) & mask]),
                            static_cast<unsigned>(vram[(colorBase + 0u) & mask]),
                            static_cast<unsigned>(vram[(colorBase + 16u) & mask]),
                            static_cast<unsigned>(vram[(colorBase + 32u) & mask]),
                            static_cast<unsigned>(vram[(colorBase + 48u) & mask]));
            }
            s_g4SpriteStatsFrames = 0;
            s_msxSpriteActiveLines = 0;
            s_msxSpriteSelectedCount = 0;
            s_msxSpriteColoredEntries = 0;
            s_msxSpriteTransparentEntries = 0;
            s_msxSpritePixelsDrawn = 0;
        }
#endif
    }
}

static void msx_vdp_render_bitmap4_slice_internal(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame)
{
    msx_vdp_render_bitmap4_range(state, yStart, yEnd, finalizeFrame);
}

void msx_vdp_render_bitmap4(MsxVdpState* state)
{
    msx_vdp_render_bitmap4_range(state, 0u, state ? state->activeHeight : 0u, true);
}

static void msx_vdp_render_bitmap6_range(MsxVdpState* state, unsigned yStart, unsigned yEnd);
static void msx_vdp_render_bitmap7_range(MsxVdpState* state, unsigned yStart, unsigned yEnd);
static void msx_vdp_render_bitmap8_range(MsxVdpState* state, unsigned yStart, unsigned yEnd);

void msx_vdp_render_bitmap6(MsxVdpState* state)
{
    msx_vdp_render_bitmap6_range(state, 0u, state ? state->activeHeight : 0u);
}

void msx_vdp_render_bitmap7(MsxVdpState* state)
{
    msx_vdp_render_bitmap7_range(state, 0u, state ? state->activeHeight : 0u);
}

void msx_vdp_render_bitmap8(MsxVdpState* state)
{
    msx_vdp_render_bitmap8_range(state, 0u, state ? state->activeHeight : 0u);
}

void msx_vdp_render_yjk(MsxVdpState* state, bool yae)
{
    const unsigned height = state->activeHeight;
    const uint32_t lineMask = msx_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t backdrop = msx_vdp_reg_backdrop(state);
    for (unsigned y = 0; y < height; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = yae ? 0u : msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool hscroll512 =
            !yae && msx_vdp_hscroll512_for_regs(lineReg25) && (lineHScroll > 255u);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        uint32_t lineBase = (msx_vdp_name_base_for_reg2(state, lineReg2) +
                             ((scrolledY << 8) & lineMask)) & mask;
        if (hscroll512) {
            lineBase += 0x10000u;
        }
        lineBase += static_cast<uint32_t>(lineHScroll & 0xFCu);
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        if (hasSprites) {
            for (unsigned i = 0; i < 4u; ++i) {
                const uint8_t sprite = spriteLine[i];
                dst[i] = sprite != 0u ? sprite : backdrop;
            }
        } else {
            for (unsigned i = 0; i < 4u; ++i) {
                dst[i] = backdrop;
            }
        }

        for (unsigned group = 0; group < 63u; ++group) {
            const uint32_t groupBase = lineBase + group * 4u;
            const uint8_t t0 = msx_vdp_read_vram_fast(vram, mask, groupBase + 0u);
            const uint8_t t1 = msx_vdp_read_vram_fast(vram, mask, groupBase + 1u);
            const uint8_t t2 = msx_vdp_read_vram_fast(vram, mask, groupBase + 2u);
            const uint8_t t3 = msx_vdp_read_vram_fast(vram, mask, groupBase + 3u);
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
                const uint8_t sprite = hasSprites ? spriteLine[pixelBase + i] : 0u;
                if (sprite != 0u) {
                    dst[pixelBase + i] = sprite;
                    continue;
                }

                const uint8_t yv = static_cast<uint8_t>(pixels[i] >> 3);
                if (yae && (yv & 0x01u) != 0u) {
                    dst[pixelBase + i] = static_cast<uint8_t>(yv >> 1);
                } else {
                    dst[pixelBase + i] = msx_vdp_yjk_color_index(static_cast<int>(yv), j, k);
                }
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_bitmap6_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    const unsigned height = state ? state->activeHeight : 0u;
    if (!state || !msx_vdp_slice_bounds(height, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    // SCREEN 6 fetch wraps within the selected 0x8000-byte page, like fMSX.
    const bool wideStream = state && !state->frameBuffer;
    const unsigned renderWidth = wideStream ? kMsxWideFrameWidth : kMsxFrameWidth;
    const uint32_t lineMask = 0x7FFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool useHScroll = lineHScroll != 0u;
        const bool dualPage = msx_vdp_hscroll512_for_regs(lineReg25);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        const uint32_t pageBase = msx_vdp_name_base_for_reg2(state, lineReg2) & mask;
        const uint32_t rowOffset = (scrolledY << 7) & lineMask;
        const uint32_t lineBase = (pageBase + rowOffset) & mask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        if (!useHScroll) {
            if (wideStream) {
                for (unsigned byteX = 0; byteX < (kMsxWideFrameWidth / 4u); ++byteX) {
                    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + byteX);
                    if (hasSprites) {
                        msx_vdp_render_bitmap6_byte_wide_with_sprites(packed,
                                                                       spriteLine + byteX * 2u,
                                                                       dst + byteX * 4u);
                    } else {
                        msx_vdp_render_bitmap6_byte_wide(packed, dst + byteX * 4u);
                    }
                }
            } else {
                for (unsigned byteX = 0; byteX < (kMsxFrameWidth / 2u); ++byteX) {
                    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + byteX);
                    const uint8_t p0 = static_cast<uint8_t>((packed >> 6) & 0x03u);
                    const uint8_t p1 = static_cast<uint8_t>((packed >> 2) & 0x03u);
                    const unsigned dstX = byteX * 2u;
                    if (hasSprites) {
                        const uint8_t s0 = spriteLine[dstX + 0u];
                        const uint8_t s1 = spriteLine[dstX + 1u];
                        dst[dstX + 0u] = s0 != 0u ? s0 : p0;
                        dst[dstX + 1u] = s1 != 0u ? s1 : p1;
                    } else {
                        dst[dstX + 0u] = p0;
                        dst[dstX + 1u] = p1;
                    }
                }
            }
        } else {
            for (unsigned x = 0; x < renderWidth; ++x) {
                const unsigned sourceX = wideStream ? x : (x * 2u);
                const MsxBitmapFetchAddress fetch = msx_vdp_bitmap_fetch_address(pageBase,
                                                                                 rowOffset,
                                                                                 lineHScroll,
                                                                                 sourceX,
                                                                                 512u,
                                                                                 dualPage,
                                                                                 0x8000u);
                const uint8_t pixel = msx_vdp_bitmap6_read_pixel(vram,
                                                                 mask,
                                                                 fetch.lineBase & mask,
                                                                 fetch.pixelX);
                const uint8_t sprite = spriteLine[wideStream ? (x >> 1) : x];
                dst[x] = hasSprites && sprite != 0u ? sprite : pixel;
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_bitmap7_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    const unsigned height = state ? state->activeHeight : 0u;
    if (!state || !msx_vdp_slice_bounds(height, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const bool wideStream = state && !state->frameBuffer;
    const unsigned renderWidth = wideStream ? kMsxWideFrameWidth : kMsxFrameWidth;
    const uint32_t lineMask = msx_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool useHScroll = lineHScroll != 0u;
        const bool dualPage = msx_vdp_hscroll512_for_regs(lineReg25);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        const uint32_t pageBase = msx_vdp_name_base_for_reg2(state, lineReg2) & mask;
        const uint32_t rowOffset = (scrolledY << 8) & lineMask;
        const uint32_t lineBase = (pageBase + rowOffset) & mask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        if (!useHScroll) {
            if (wideStream) {
                for (unsigned byteX = 0; byteX < (kMsxWideFrameWidth / 2u); ++byteX) {
                    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + byteX);
                    if (hasSprites) {
                        msx_vdp_render_bitmap7_byte_with_sprites(packed, spriteLine + byteX, dst + byteX * 2u);
                    } else {
                        msx_vdp_render_bitmap7_byte(packed, dst + byteX * 2u);
                    }
                }
            } else {
                for (unsigned x = 0; x < renderWidth; ++x) {
                    const uint8_t packed = msx_vdp_read_vram_fast(vram, mask, lineBase + x);
                    const uint8_t pixel = static_cast<uint8_t>(packed >> 4);
                    const uint8_t sprite = spriteLine[x];
                    dst[x] = hasSprites && sprite != 0u ? sprite : pixel;
                }
            }
        } else {
            for (unsigned x = 0; x < renderWidth; ++x) {
                const unsigned sourceX = wideStream ? x : (x * 2u);
                const MsxBitmapFetchAddress fetch = msx_vdp_bitmap_fetch_address(pageBase,
                                                                                 rowOffset,
                                                                                 lineHScroll,
                                                                                 sourceX,
                                                                                 512u,
                                                                                 dualPage,
                                                                                 0x10000u);
                const uint8_t pixel = msx_vdp_bitmap7_read_pixel(vram,
                                                                 mask,
                                                                 fetch.lineBase & mask,
                                                                 fetch.pixelX);
                const uint8_t sprite = spriteLine[wideStream ? (x >> 1) : x];
                dst[x] = hasSprites && sprite != 0u ? sprite : pixel;
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_bitmap8_range(MsxVdpState* state, unsigned yStart, unsigned yEnd)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    const unsigned height = state ? state->activeHeight : 0u;
    if (!state || !msx_vdp_slice_bounds(height, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t lineMask = msx_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool useHScroll = lineHScroll != 0u;
        const bool dualPage = msx_vdp_hscroll512_for_regs(lineReg25);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        const uint32_t pageBase = msx_vdp_name_base_for_reg2(state, lineReg2) & mask;
        const uint32_t rowOffset = (scrolledY << 8) & lineMask;
        const uint32_t lineBase = (pageBase + rowOffset) & mask;
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        if (!useHScroll) {
            if (!hasSprites && lineBase + kMsxFrameWidth - 1u <= mask) {
                V9938_BENCH_VRAM_READ(lineBase, kMsxFrameWidth);
                std::memcpy(dst, vram + lineBase, kMsxFrameWidth);
            } else {
                for (unsigned x = 0; x < kMsxFrameWidth; ++x) {
                    const uint8_t sprite = spriteLine[x];
                    const uint8_t pixel = msx_vdp_bitmap8_read_pixel(vram, mask, lineBase, x);
                    dst[x] = hasSprites && sprite != 0u
                        ? msx_vdp_screen8_mapped_color(sprite)
                        : pixel;
                }
            }
        } else {
            for (unsigned x = 0; x < kMsxFrameWidth; ++x) {
                const MsxBitmapFetchAddress fetch = msx_vdp_bitmap_fetch_address(pageBase,
                                                                                 rowOffset,
                                                                                 lineHScroll,
                                                                                 x,
                                                                                 256u,
                                                                                 dualPage,
                                                                                 0x10000u);
                const uint8_t sprite = spriteLine[x];
                const uint8_t pixel = msx_vdp_bitmap8_read_pixel(vram,
                                                                 mask,
                                                                 fetch.lineBase & mask,
                                                                 fetch.pixelX);
                dst[x] = hasSprites && sprite != 0u
                    ? msx_vdp_screen8_mapped_color(sprite)
                    : pixel;
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
    }
}

static void msx_vdp_render_yjk_range(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool yae)
{
    unsigned startLine = 0u;
    unsigned endLine = 0u;
    const unsigned height = state ? state->activeHeight : 0u;
    if (!state || !msx_vdp_slice_bounds(height, yStart, yEnd, &startLine, &endLine)) {
        return;
    }

    const uint32_t lineMask = msx_vdp_name_mask(state) & 0xFFFFu;
    const uint8_t* const vram = state->vram;
    const uint32_t mask = state->vramMask;
    const uint8_t backdrop = msx_vdp_reg_backdrop(state);
    for (unsigned y = startLine; y < endLine; ++y) {
        const bool hasSprites = msx_vdp_render_color_sprites_line(state, y, s_msxColorSpriteLine);
        const uint8_t* const spriteLine = s_msxColorSpriteLine + 32u;
        const uint8_t lineReg2 = msx_vdp_reg2_for_line(state, y);
        const uint8_t lineReg25 = msx_vdp_reg25_for_line(state, y);
        const uint8_t lineReg26 = msx_vdp_reg26_for_line(state, y);
        const uint8_t lineReg27 = msx_vdp_reg27_for_line(state, y);
        const uint8_t lineVScroll = msx_vdp_reg23_for_line(state, y);
        const uint16_t lineHScroll = yae ? 0u : msx_vdp_hscroll_for_regs(lineReg26, lineReg27);
        const bool hscroll512 =
            !yae && msx_vdp_hscroll512_for_regs(lineReg25) && (lineHScroll > 255u);
        const uint32_t scrolledY = static_cast<uint32_t>(y) + static_cast<uint32_t>(lineVScroll);
        uint32_t lineBase = (msx_vdp_name_base_for_reg2(state, lineReg2) +
                             ((scrolledY << 8) & lineMask)) & mask;
        if (hscroll512) {
            lineBase += 0x10000u;
        }
        lineBase += static_cast<uint32_t>(lineHScroll & 0xFCu);
        uint8_t* dst = msx_vdp_get_line_buffer(state, y);
        if (!dst) {
            continue;
        }

        if (hasSprites) {
            for (unsigned i = 0; i < 4u; ++i) {
                const uint8_t sprite = spriteLine[i];
                dst[i] = sprite != 0u ? sprite : backdrop;
            }
        } else {
            for (unsigned i = 0; i < 4u; ++i) {
                dst[i] = backdrop;
            }
        }

        for (unsigned group = 0; group < 63u; ++group) {
            const uint32_t groupBase = lineBase + group * 4u;
            const uint8_t t0 = msx_vdp_read_vram_fast(vram, mask, groupBase + 0u);
            const uint8_t t1 = msx_vdp_read_vram_fast(vram, mask, groupBase + 1u);
            const uint8_t t2 = msx_vdp_read_vram_fast(vram, mask, groupBase + 2u);
            const uint8_t t3 = msx_vdp_read_vram_fast(vram, mask, groupBase + 3u);
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
                const uint8_t sprite = hasSprites ? spriteLine[pixelBase + i] : 0u;
                if (sprite != 0u) {
                    dst[pixelBase + i] = sprite;
                    continue;
                }

                const uint8_t yv = static_cast<uint8_t>(pixels[i] >> 3);
                if (yae && (yv & 0x01u) != 0u) {
                    dst[pixelBase + i] = static_cast<uint8_t>(yv >> 1);
                } else {
                    dst[pixelBase + i] = msx_vdp_yjk_color_index(static_cast<int>(yv), j, k);
                }
            }
        }
        msx_vdp_stream_line_if_needed(state, dst, y);
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

uint8_t msx_vdp_vram_page_mask(const MsxVdpState* state)
{
    if (!state || !msx_vdp_is_msx2(state) || state->vramSize <= 0x4000u) {
        return 0u;
    }

    const uint32_t pageCount = static_cast<uint32_t>(state->vramSize >> 14);
    return static_cast<uint8_t>((pageCount - 1u) & 0x07u);
}

bool msx_vdp_cpu_port_carries_into_r14(const MsxVdpState* state)
{
    if (!state || !msx_vdp_is_msx2(state) || state->vramSize <= 0x4000u) {
        return false;
    }

    switch (msx_vdp_decode_mode(state)) {
        case MsxVdpMode::Graphics1:
        case MsxVdpMode::Text40:
        case MsxVdpMode::Graphics2:
        case MsxVdpMode::Multicolor:
            return false;
        default:
            return true;
    }
}

uint32_t msx_vdp_compose_address(const MsxVdpState* state, uint16_t low14)
{
    uint32_t address = static_cast<uint32_t>(low14 & 0x3FFFu);
    if (msx_vdp_is_msx2(state)) {
        address |= static_cast<uint32_t>(state->regs[14] & msx_vdp_vram_page_mask(state)) << 14;
    }
    return address & state->vramMask;
}

void msx_vdp_advance_address(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    const uint16_t low14 = static_cast<uint16_t>((state->address + 1u) & 0x3FFFu);
    if (!msx_vdp_is_msx2(state)) {
        state->address = static_cast<uint32_t>(low14) & state->vramMask;
        return;
    }

    uint8_t page = static_cast<uint8_t>(state->regs[14] & msx_vdp_vram_page_mask(state));
    // On V9938, R14 always selects the CPU-side VRAM page, but the carry from
    // A13 only increments that page outside of the four MSX1-compatible modes.
    if ((state->address & 0x3FFFu) == 0x3FFFu && msx_vdp_cpu_port_carries_into_r14(state)) {
        page = static_cast<uint8_t>((page + 1u) & msx_vdp_vram_page_mask(state));
        state->regs[14] = page;
    }

    state->address = msx_vdp_compose_address(state, low14);
}

} // namespace

void msx_vdp_advance_command_engine(MsxVdpState* state, uint32_t targetFrameCycles)
{
    msx_vdp_advance_command_engine_internal(state, targetFrameCycles);
}

void msx_vdp_refresh_timing(MsxVdpState* state)
{
    msx_vdp_refresh_timing_flags(state);
}

void msx_vdp_render_bitmap4_slice(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame)
{
    msx_vdp_render_bitmap4_slice_internal(state, yStart, yEnd, finalizeFrame);
}

void msx_vdp_render_slice(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame)
{
    if (!state) {
        return;
    }

    switch (state->mode) {
        case MsxVdpMode::Graphics1:
            msx_vdp_render_graphics1_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Text40:
            msx_vdp_render_text40_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Graphics2:
            msx_vdp_render_graphics2_like_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Multicolor:
            msx_vdp_render_multicolor_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Graphics3:
            msx_vdp_render_graphics3_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Bitmap4:
            msx_vdp_render_bitmap4_range(state, yStart, yEnd, finalizeFrame);
            break;
        case MsxVdpMode::Bitmap6:
            msx_vdp_render_bitmap6_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Bitmap7:
            if (msx_vdp_is_msx2(state) && msx_vdp_mode_yjk(state)) {
                msx_vdp_render_yjk_range(state, yStart, yEnd, msx_vdp_mode_yae(state));
            } else {
                msx_vdp_render_bitmap7_range(state, yStart, yEnd);
            }
            break;
        case MsxVdpMode::Bitmap8:
            if (msx_vdp_is_msx2(state) && msx_vdp_mode_yjk(state)) {
                msx_vdp_render_yjk_range(state, yStart, yEnd, msx_vdp_mode_yae(state));
            } else {
                msx_vdp_render_bitmap8_range(state, yStart, yEnd);
            }
            break;
        case MsxVdpMode::Text80:
            msx_vdp_render_text80_range(state, yStart, yEnd);
            break;
        case MsxVdpMode::Unsupported:
        default:
            break;
    }
}

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
        // MSX2 uses heap-backed VRAM so we can request the full V9938 space when
        // available and gracefully fall back on tighter memory budgets.
        uint32_t requestedVram = static_cast<uint32_t>(state->vramSize);
        uint32_t selectedVram = requestedVram;
        while (selectedVram >= kMsxVdpMinVramSize && !msx_vdp_init_vram(state, selectedVram)) {
            if (selectedVram == kMsxVdpMinVramSize) {
                break;
            }

            const uint32_t nextVram = selectedVram >> 1;
#if MSX_VDP_INIT_LOG_ENABLED
            std::printf("[MSX] vdp init: vram size fallback from %u to %u\n",
                        static_cast<unsigned>(selectedVram),
                        static_cast<unsigned>(nextVram));
#endif
            selectedVram = nextVram;
        }
        if (!state->vram) {
#if MSX_VDP_INIT_LOG_ENABLED
            const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            const uint32_t freeSpiRam = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            std::printf("[MSX] vdp init: vram alloc failed size=%u free8=%u freeInternal=%u freeSPIRAM=%u largestInternal=%u largestSPIRAM=%u\n",
                        static_cast<unsigned>(requestedVram),
                        static_cast<unsigned>(free8),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(freeSpiRam),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
            std::memset(state, 0, sizeof(*state));
            return false;
        }
        if (selectedVram != requestedVram) {
#if MSX_VDP_INIT_LOG_ENABLED
            std::printf("[MSX] vdp init: vram downgraded to %u bytes for MSX2\n",
                        static_cast<unsigned>(selectedVram));
#endif
        }
        state->ownsVram = true;
        if (!msx_vdp_ensure_frame_buffer()) {
#if MSX_VDP_INIT_LOG_ENABLED
            std::printf("[MSX] vdp init: frame buffer alloc failed, keeping MSX2 line stream path\n");
#endif
        }
    } else {
        if (!msx_vdp_ensure_msx1_buffers()) {
#if MSX_VDP_INIT_LOG_ENABLED
            std::printf("[MSX] vdp init: MSX1 work buffer alloc failed frame=%u vram=%u snapshot=%u\n",
                        static_cast<unsigned>(kMsxFramePixels),
                        static_cast<unsigned>(kMsx1VramSize),
                        static_cast<unsigned>(kMsx1VramSize));
#endif
            std::memset(state, 0, sizeof(*state));
            return false;
        }
        state->vram = s_msx1Vram;
        state->frameBuffer = s_msxFrameBuffer;
        msx_vdp_reset(state);
        return true;
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
    if (state->machineMode == MsxMachineMode::MSX1 || state->machineMode == MsxMachineMode::MSX2) {
        msx_vdp_free_msx1_buffers();
    }

    std::memset(state, 0, sizeof(*state));
}

void msx_vdp_reset(MsxVdpState* state)
{
    if (!state || !state->vram) {
        return;
    }

    if (!msx_vdp_is_msx2(state)) {
        if (!state->frameBuffer) {
            return;
        }

        std::memset(state->vram, 0x00, state->vramSize);
        std::memcpy(state->regs, kMsxVdpRegsInit, sizeof(state->regs));
        std::memcpy(state->status, kMsxVdpStatusInit, sizeof(state->status));
        std::memset(state->frameBuffer, 0x01, kMsxFramePixels);
        std::memset(state->paletteRaw, 0x00, sizeof(state->paletteRaw));

        state->readBuffer = 0;
        state->address = 0;
        state->latchedControl = 0;
        state->paletteLatch = 0;
        state->vramWriteMode = false;
        state->palettePending = false;
        state->controlPending = false;
        state->renderContext = nullptr;
        state->dirty = true;
        state->frameReady = false;
        state->frameCounter = 0;
        msx_vdp_invalidate_mono_sprite_cache();
        msx_vdp_init_palette(state);
        msx_vdp_update_mode_geometry(state);
        return;
    }

    std::memset(state->vram, 0x00, state->vramSize);
    std::memcpy(state->regs, kMsxVdpRegsInit, sizeof(state->regs));
    std::memcpy(state->status, kMsxVdpStatusInit, sizeof(state->status));
    if (state->frameBuffer) {
        std::memset(state->frameBuffer, 0x01, kMsxFramePixels);
    }
    std::memset(state->paletteRaw, 0x00, sizeof(state->paletteRaw));

    state->readBuffer = 0;
    state->address = 0;
    state->latchedControl = 0;
    state->paletteLatch = 0;
    state->vramWriteMode = false;
    state->palettePending = false;
    state->controlPending = false;
    state->renderContext = nullptr;
    state->dirty = true;
    state->frameReady = false;
    s_msxVdpR5TimelineCount = 0u;
    s_msxVdpR6TimelineCount = 0u;
    s_msxVdpR8TimelineCount = 0u;
    s_msxVdpR11TimelineCount = 0u;
    s_msxVdpR2TimelineCount = 0u;
    s_msxVdpR23TimelineCount = 0u;
    s_msxVdpR25TimelineCount = 0u;
    s_msxVdpR26TimelineCount = 0u;
    s_msxVdpR27TimelineCount = 0u;
#if MSX_VDP_BOOT_DIAG_ENABLED
    s_msxBootDiagLastFrame = 0xFFFFFFFFu;
    s_msxBootDiagCount = 0u;
#endif
    state->frameCounter = 0;
    state->lineInterruptFrameTag = 0xFFFFFFFFu;
    state->lineInterruptLineTag = 0xFFu;
    msx_vdp_invalidate_mono_sprite_cache();
    if (msx_vdp_is_msx2(state)) {
        state->regs[8] = 0x08u;
        state->regs[16] = 0u;
        state->regs[17] = 0u;
    }
    msx_vdp_init_palette(state);
    msx_vdp_update_mode_geometry(state);
}

bool msx_vdp_begin_frame(MsxVdpState* state)
{
    if (!state) {
        return false;
    }

    if (!msx_vdp_is_msx2(state)) {
        state->status[0] |= 0x80u;
        // Match the upstream V9938 reset state: FH/VR high, BD low.
        // Using 0x5C leaves SRCH's border-detect bit set from the start and
        // produces spurious 1C/3C/5C/7C status patterns during Aleste's polls.
        state->status[2] = 0x6Cu;
        state->frameCounter++;
        return (state->regs[1] & 0x20u) != 0u;
    }

    msx_vdp_timeline_reset(s_msxVdpR5Timeline, &s_msxVdpR5TimelineCount, state->regs[5]);
    msx_vdp_timeline_reset(s_msxVdpR6Timeline, &s_msxVdpR6TimelineCount, state->regs[6]);
    msx_vdp_timeline_reset(s_msxVdpR8Timeline, &s_msxVdpR8TimelineCount, state->regs[8]);
    msx_vdp_timeline_reset(s_msxVdpR11Timeline, &s_msxVdpR11TimelineCount, state->regs[11]);
    msx_vdp_timeline_reset(s_msxVdpR2Timeline, &s_msxVdpR2TimelineCount, state->regs[2]);
    msx_vdp_timeline_reset(s_msxVdpR23Timeline, &s_msxVdpR23TimelineCount, state->regs[23]);
    msx_vdp_timeline_reset(s_msxVdpR25Timeline, &s_msxVdpR25TimelineCount, state->regs[25]);
    msx_vdp_timeline_reset(s_msxVdpR26Timeline, &s_msxVdpR26TimelineCount, state->regs[26]);
    msx_vdp_timeline_reset(s_msxVdpR27Timeline, &s_msxVdpR27TimelineCount, state->regs[27]);
    if (msx_vdp_is_msx2(state)) {
    }
    state->status[0] |= 0x80u;
    state->status[1] &= 0xFEu;
    // Keep S#2 aligned with the upstream V9938 baseline at frame start.
    state->status[2] = 0x6Cu;
    state->frameCounter++;
    state->lineInterruptFrameTag = 0xFFFFFFFFu;
    state->lineInterruptLineTag = 0xFFu;
    return (state->regs[1] & 0x20u) != 0u;
}

void msx_vdp_prepare_frame_render(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    msx_vdp_update_mode_geometry(state);
    msx_vdp_log_boot_text_probe(state);
    msx_vdp_reset_sprite_status(state);
    if (msx_vdp_is_msx2(state)) {
        msx_vdp_invalidate_mono_sprite_cache();
    } else {
        msx_vdp_prepare_mono_sprite_cache(state);
    }
}

bool msx_vdp_render_internal(MsxVdpState* state)
{
    if (!state) {
        return false;
    }

    const bool msx2LineStream = msx_vdp_is_msx2(state) && !state->frameBuffer &&
                                msx_vdp_begin_msx2_stream_frame(state);
    if (msx_vdp_is_msx2(state) && !msx2LineStream && !state->frameBuffer) {
        return false;
    }

    if (!msx_vdp_display_enabled(state)) {
        msx_vdp_clear_active_frame(state, msx_vdp_resolve_color(state, 0));
        if (msx2LineStream) {
            msx_vdp_end_msx2_stream_frame();
        }
        return true;
    }

    switch (state->mode) {
        case MsxVdpMode::Graphics1:
            msx_vdp_render_graphics1(state);
            break;
        case MsxVdpMode::Text40:
            msx_vdp_render_text40(state);
            break;
        case MsxVdpMode::Graphics2:
            msx_vdp_render_graphics2_like(state);
            break;
        case MsxVdpMode::Graphics3:
            msx_vdp_render_graphics3(state);
            break;
        case MsxVdpMode::Multicolor:
            msx_vdp_render_multicolor(state);
            break;
        case MsxVdpMode::Bitmap4:
            msx_vdp_render_bitmap4(state);
            break;
        case MsxVdpMode::Bitmap6:
            msx_vdp_render_bitmap6(state);
            break;
        case MsxVdpMode::Bitmap7:
            if (msx_vdp_mode_yjk(state)) {
                msx_vdp_render_yjk(state, msx_vdp_mode_yae(state));
            } else {
                msx_vdp_render_bitmap7(state);
            }
            break;
        case MsxVdpMode::Bitmap8:
            if (msx_vdp_mode_yjk(state)) {
                msx_vdp_render_yjk(state, msx_vdp_mode_yae(state));
            } else {
                msx_vdp_render_bitmap8(state);
            }
            break;
        case MsxVdpMode::Text80:
            msx_vdp_render_text80(state);
            break;
        case MsxVdpMode::Unsupported:
        default:
            msx_vdp_clear_active_frame(state, msx_vdp_resolve_color(state, 0));
            break;
    }

    if (msx2LineStream) {
        msx_vdp_end_msx2_stream_frame();
    }

    return true;
}

void msx_vdp_render(MsxVdpState* state)
{
    if (!state) {
        return;
    }

    if (!state->dirty && state->frameReady) {
        return;
    }

    msx_vdp_prepare_frame_render(state);

        if (!msx_vdp_render_internal(state)) {
            return;
        }
        state->dirty = false;
        state->frameReady = true;

        MsxDisplayFrame frame = {};
        msx_vdp_get_display_frame(state, &frame);
        if (frame.indexed8) {
            msx_video_present_frame(&frame);
        }
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
        msx_vdp_advance_command_engine_internal(state, state->currentFrameCpuCycles);
        msx_vdp_refresh_timing_flags(state);
        index = static_cast<uint8_t>(state->regs[15] & 0x0Fu);
        if (index >= sizeof(state->status)) {
            index = 0u;
        }
    }

    uint8_t value = state->status[index];
    msx_vdp_diag_log_status_read(state, index, value);
    if (index == 0u) {
        state->status[0] &= 0x5Fu;
    } else if (index == 1u) {
        state->status[1] &= 0xFEu;
    } else if (index == 7u && msx_vdp_is_msx2(state)) {
        state->status[7] = state->regs[44] = msx_vdp_command_read(state);
    }
    return value;
}

void msx_vdp_out_data(MsxVdpState* state, uint8_t value)
{
    if (!state || !state->vram) {
        return;
    }

    if (!state->vramWriteMode) {
        // Upstream fMSX keeps a distinct "write key" state: a data-port write
        // issued while the VDP is still in read mode first performs the
        // prefetched read/advance, then stores to the advanced address.
        state->readBuffer = msx_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
        msx_vdp_advance_address(state);
        msx_vdp_write_vram_fast(state, state->address, value);
        state->controlPending = false;
        return;
    }

    msx_vdp_write_vram_fast(state, state->address, value);
    msx_vdp_advance_address(state);
    state->readBuffer = value;
    state->controlPending = false;
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
        state->vramWriteMode = false;
        state->readBuffer = msx_vdp_read_vram_fast(state->vram, state->vramMask, state->address);
        msx_vdp_advance_address(state);
        return;
    }

    if (command == 1u) {
        state->address = msx_vdp_compose_address(state, low14);
        state->vramWriteMode = true;
        return;
    }

    const uint8_t reg = msx_vdp_is_msx2(state) ? static_cast<uint8_t>(value & 0x3Fu) : static_cast<uint8_t>(value & 0x07u);
    msx_vdp_write_register(state, reg, state->latchedControl);
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

#if MSX_VDP_VERBOSE_DIAG_ENABLED
    static constexpr uint32_t kMsx2IndirectG4LogLimit = 12u;
    static uint32_t s_msx2IndirectLogCount = 0u;
    static uint32_t s_msx2IndirectG4LogCount = 0u;
    static uint32_t s_msx2IndirectG4FrameTag = 0xFFFFFFFFu;
    uint8_t reg = static_cast<uint8_t>(state->regs[17] & 0x3Fu);
    if (state->mode == MsxVdpMode::Bitmap4 && s_msx2IndirectG4FrameTag != state->frameCounter) {
        s_msx2IndirectG4FrameTag = state->frameCounter;
        s_msx2IndirectG4LogCount = 0u;
    }
    if (msx_vdp_is_traced_register(reg)) {
        const bool g4CommandReg = state->mode == MsxVdpMode::Bitmap4 && reg >= 32u && reg <= 46u;
        if (g4CommandReg) {
            if (s_msx2IndirectG4LogCount < kMsx2IndirectG4LogLimit) {
                ++s_msx2IndirectG4LogCount;
                std::printf("[MSX][IND-G4] #%lu R%02u=%02X ptr=%02X r2=%02X r23=%02X mode=%s frame=%lu cyc=%lu\n",
                            static_cast<unsigned long>(s_msx2IndirectG4LogCount),
                            static_cast<unsigned>(reg),
                            static_cast<unsigned>(value),
                            static_cast<unsigned>(state->regs[17]),
                            static_cast<unsigned>(state->regs[2]),
                            static_cast<unsigned>(state->regs[23]),
                            msx_vdp_mode_label(state->mode),
                            static_cast<unsigned long>(state->frameCounter),
                            static_cast<unsigned long>(state->currentFrameCpuCycles));
            } else if (s_msx2IndirectG4LogCount == kMsx2IndirectG4LogLimit) {
                ++s_msx2IndirectG4LogCount;
                std::printf("[MSX][IND-G4] frame=%lu further indirect command writes suppressed after %lu entries\n",
                            static_cast<unsigned long>(state->frameCounter),
                            static_cast<unsigned long>(kMsx2IndirectG4LogLimit));
            }
        } else if (s_msx2IndirectLogCount < 192u) {
            ++s_msx2IndirectLogCount;
            std::printf("[MSX][IND] #%lu R%02u=%02X ptr=%02X mode=%s frame=%lu\n",
                        static_cast<unsigned long>(s_msx2IndirectLogCount),
                        static_cast<unsigned>(reg),
                        static_cast<unsigned>(value),
                        static_cast<unsigned>(state->regs[17]),
                        msx_vdp_mode_label(state->mode),
                        static_cast<unsigned long>(state->frameCounter));
        }
    }
#else
    uint8_t reg = static_cast<uint8_t>(state->regs[17] & 0x3Fu);
#endif
    if (reg != 17u && reg < sizeof(state->regs)) {
        msx_vdp_write_register(state, reg, value);
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
    if (!state || !state->frameReady) {
        return;
    }

    frame->indexed8 = state->frameBuffer;
    frame->pitchBytes = state->frameBuffer ? kMsxFrameWidth : 0u;
    frame->palette565 = ((state->mode == MsxVdpMode::Bitmap8) || msx_vdp_mode_yjk(state))
        ? state->screen8Palette565
        : state->palette565;
    frame->paletteEntryCount = ((state->mode == MsxVdpMode::Bitmap8) || msx_vdp_mode_yjk(state)) ? 256u : 16u;
    frame->width = state->activeWidth;
    frame->height = state->activeHeight;
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
        case MsxVdpMode::Bitmap6:
            return "G6";
        case MsxVdpMode::Bitmap7:
            return "G7";
        case MsxVdpMode::Bitmap8:
            return "G8";
        case MsxVdpMode::Text80:
            return "TXT80";
        case MsxVdpMode::Unsupported:
        default:
            return "UNSUP";
    }
}

bool msx_vdp_display_enabled(const MsxVdpState* state)
{
    return state && (state->regs[1] & 0x40u) != 0;
}
