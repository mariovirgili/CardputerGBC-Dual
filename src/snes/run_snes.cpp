#pragma GCC optimize ("O3")

#include "run_snes.h"
#include "compat/arduino_compat.h"
#include "share/utils.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "snes_display.h"
#include "snes_stubs.h"
#include "snes_input.h"
#include "snes_save.h"
#include "snes_rom.h"
#include "share/input.h"
#include "share/sd_control.h"
#include "share/sd_gameplay_guard.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
    #include "snes9x/snes9x.h"
    #include "snes9x/src/gfx.h"
    #include "snes9x/src/memmap.h"
    #include "snes9x/src/ppu.h"
#include "share/emu_log_cpp.h"
}

struct SnesLineMap
{
    uint32_t srcH;
    float    scaleY;
    float    srcStart;
    uint32_t stepY;
};

/* Globals */
static uint16_t   *g_dstFirstForSrc = nullptr;
static uint16_t   *g_dstLastForSrc  = nullptr;
static uint8_t    *g_srcLineNeeded  = nullptr;
static uint16_t   *g_srcForDst      = nullptr;
static SnesLineMap g_lineMap        = {};
static uint16_t    g_defaultSrcForDst[LCD_H];
static uint32_t    g_defaultSrcMapH     = 0;
static uint32_t    g_defaultSrcMapStart = 0xFFFFFFFFu;
static uint32_t    g_defaultSrcMapStep  = 0;

static bool     g_altHardHalfSkip = true;
static uint32_t g_dstY            = 0;

bool     interlace_enabled           = false;
uint32_t fieldParity                 = 0;
bool     snes_interlace_lock_parity  = false;
static SnesInterlaceMode s_interlace_mode = SNES_INTERLACE_OFF;

#ifdef SNES_DEEP_BENCH
extern "C" {
bool g_snes_animaniacs_probe_enabled = false;
void snes_profile_ppu_get_and_reset(uint32_t* out, uint32_t count);
}
#endif

/* ---------------------------------------------------- */
/* Helpers                                              */
/* ---------------------------------------------------- */

static void snes_apply_common_settings()
{
    Settings.CyclesPercentage = 100;
    Settings.H_Max            = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL     = 20000;
    Settings.FrameTimeNTSC    = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart      = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;

    /* Audio OFF (not enough RAM) */
    Settings.SoundPlaybackRate = 0;
    Settings.SoundBufferSize   = 0;
    Settings.ThreadSound       = false;
    Settings.Mute              = true;
    Settings.APUEnabled        = false;
    Settings.DisableSoundEcho  = false;
}

#ifdef SNES_LOGS
#define SNES_LOG(...) EMU_LOG(__VA_ARGS__)

struct SnesHeapState
{
    uint32_t free8;
    uint32_t freeInternal;
    uint32_t largest8;
    uint32_t largestInternal;
};

static inline SnesHeapState snes_heap_state()
{
    return {
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    };
}

static size_t snes_estimated_memory_alloc_bytes()
{
    size_t bytes = 0;
    bytes += RAM_SIZE;
    bytes += VRAM_SIZE;
    bytes += FILLRAM_SIZE;
#ifndef SNES_NO_BYTE2000
    bytes += 0x2000u;
#endif
    return bytes;
}

static size_t snes_estimated_map_alloc_bytes()
{
    size_t bytes = 0;
    bytes += MEMMAP_NUM_BLOCKS * sizeof(uint8_t*);
    bytes += MEMMAP_NUM_BLOCKS * sizeof(SMapInfo);
    return bytes;
}

static size_t snes_estimated_ppu_alloc_bytes()
{
    size_t bytes = 0;
    bytes += 256u * 9u * sizeof(uint16_t);
    bytes += MAX_2BIT_TILES;
    bytes += 3u * 256u;
    bytes += 256u * sizeof(uint16_t);
    bytes += 128u * sizeof(SOBJ);
    bytes += 512u + 32u;
    return bytes;
}

static size_t snes_estimated_core_alloc_bytes()
{
    return snes_estimated_memory_alloc_bytes() +
           snes_estimated_map_alloc_bytes() +
           snes_estimated_ppu_alloc_bytes();
}

static size_t snes_estimated_linebuf_bytes()
{
#ifdef SNES_LAZY_LINE_BUFFERS
    return (sizeof(uint16_t) * SNES_WIDTH * 2u) +
           (sizeof(uint8_t) * SNES_WIDTH * 2u);
#else
    return (sizeof(uint16_t) * SNES_WIDTH * 2u * 2u) +
           (sizeof(uint8_t) * SNES_WIDTH * 2u * 2u);
#endif
}

static size_t snes_estimated_alt_alloc_bytes()
{
    return sizeof(uint16_t) * SNES_HEIGHT_EXTENDED * 2u +
           sizeof(uint8_t) * SNES_HEIGHT_EXTENDED +
           sizeof(uint16_t) * LCD_H;
}

static void snes_log_heap_step(const char* step)
{
    const SnesHeapState h = snes_heap_state();
    SNES_LOG("[SNES][HEAP] %-14s free8=%lu freeInternal=%lu largest8=%lu largestInternal=%lu coreReq=%lu lineReq=%lu altReq=%lu\n",
             step,
             (unsigned long)h.free8,
             (unsigned long)h.freeInternal,
             (unsigned long)h.largest8,
             (unsigned long)h.largestInternal,
             (unsigned long)snes_estimated_core_alloc_bytes(),
             (unsigned long)snes_estimated_linebuf_bytes(),
             (unsigned long)snes_estimated_alt_alloc_bytes());
}

static void snes_log_heap_runtime(const char* step, uint32_t frame)
{
    const SnesHeapState h = snes_heap_state();
    SNES_LOG("[SNES][RUNTIME] %-14s frame=%lu free8=%lu freeInternal=%lu largest8=%lu largestInternal=%lu\n",
             step,
             (unsigned long)frame,
             (unsigned long)h.free8,
             (unsigned long)h.freeInternal,
             (unsigned long)h.largest8,
             (unsigned long)h.largestInternal);
}

static void snes_log_init_failure(const char* step, size_t requestedBytes)
{
    const SnesHeapState h = snes_heap_state();
    const uint32_t missingLargest = requestedBytes > h.largestInternal
        ? (uint32_t)(requestedBytes - h.largestInternal)
        : 0;
    const uint32_t missingTotal = requestedBytes > h.freeInternal
        ? (uint32_t)(requestedBytes - h.freeInternal)
        : 0;

    SNES_LOG("[SNES][INIT][FAIL] %s requested=%lu freeInternal=%lu largestInternal=%lu free8=%lu largest8=%lu missingLargest=%lu missingTotal=%lu\n",
             step,
             (unsigned long)requestedBytes,
             (unsigned long)h.freeInternal,
             (unsigned long)h.largestInternal,
             (unsigned long)h.free8,
             (unsigned long)h.largest8,
             (unsigned long)missingLargest,
             (unsigned long)missingTotal);
}
#else
#define SNES_LOG(...) ((void)0)
#define snes_log_heap_step(step) ((void)0)
#define snes_log_heap_runtime(step, frame) ((void)0)
#define snes_log_init_failure(step, requestedBytes) ((void)0)
#define snes_estimated_memory_alloc_bytes() ((size_t)0)
#define snes_estimated_map_alloc_bytes() ((size_t)0)
#define snes_estimated_ppu_alloc_bytes() ((size_t)0)
#define snes_estimated_core_alloc_bytes() ((size_t)0)
#define snes_estimated_linebuf_bytes() ((size_t)0)
#define snes_estimated_alt_alloc_bytes() ((size_t)0)
#define snes_log_runtime_config(targetFps) ((void)0)
#endif

#ifdef SNES_LOGS
static void snes_log_runtime_config(int targetFps)
{
    const bool tileCache = S9xSmallTileCacheEnabled();
    SNES_LOG("[SNES] Core/Video only, no audio, %s, %s, mode7=%s, %d FPS target\n",
             snes_save_has_sram() ? "with SRAM" : "no SRAM",
             tileCache ? "small tilecache" : "no tilecache",
#if SNES_MODE7_INTERPOLATED
             "interp",
#else
             "fast",
#endif
             targetFps);
}
#endif

static void snes_init_dynamic_tilecache()
{
    constexpr uint32_t kEntries = 128;
    if (S9xInitSmallTileCache(kEntries))
    {
        SNES_LOG("[SNES][TILECACHE] dynamic entries=%u bytes=%u\n",
                 (unsigned) S9xSmallTileCacheEntries(),
                 (unsigned) S9xSmallTileCacheBytes());
    }
    else
    {
        SNES_LOG("[SNES][TILECACHE] disabled: allocation failed entries=%u\n",
                 (unsigned) kEntries);
    }
    snes_log_heap_step("tilecache");
}

static void snes_enter_sd_off_gameplay()
{
    share::clearRestartRequest();
    share::setRestartRequestMode(true);
    share::clearBeforeRestartCallback();
    snes_save_suspend_background();
#ifdef SNES_SD_OFF_DURING_GAMEPLAY
    snes_log_heap_step("pre SD close");
    share_sd_gameplay_close("SNES");
    snes_log_heap_step("after SD close");
#else
    snes_log_heap_step("SD kept mounted");
#endif
}

static void snes_alt_buffers_free()
{
    if (g_dstFirstForSrc) { free(g_dstFirstForSrc); g_dstFirstForSrc = nullptr; }
    if (g_dstLastForSrc)  { free(g_dstLastForSrc);  g_dstLastForSrc  = nullptr; }
    if (g_srcLineNeeded)  { free(g_srcLineNeeded);  g_srcLineNeeded  = nullptr; }
    if (g_srcForDst)      { free(g_srcForDst);      g_srcForDst      = nullptr; }
    g_lineMap = {};
    g_defaultSrcMapH = 0;
    g_defaultSrcMapStart = 0xFFFFFFFFu;
    g_defaultSrcMapStep = 0;
}

static void snes_release_core_allocations(bool releaseSram)
{
    S9xDeinitGFX();
    S9xDeinitMemory();
    if (releaseSram)
        snes_save_release_sram();
    snes_alt_buffers_free();
}

static void snes_reset_quit_controls()
{
    share::setRestartRequestMode(false);
    share::clearRestartRequest();
    share::clearBeforeRestartCallback();
}

static void snes_clean_teardown_and_save()
{
    SNES_LOG("[SNES][QUIT] clean teardown start\n");
    snes_log_heap_step("quit start");

    snes_save_suspend_background();

    uint8_t* sramSnapshot = nullptr;
    size_t sramSnapshotSize = 0;
    const bool hadSram = snes_save_has_sram();
    const bool haveSnapshot = hadSram &&
        snes_save_snapshot_sram(&sramSnapshot, &sramSnapshotSize);

    if (hadSram && !haveSnapshot)
        SNES_LOG("[SNES][SAVE] SRAM snapshot failed, using live-buffer fallback\n");

    snes_input_stop();
    snes_display_stop();
    snes_log_heap_step("tasks stopped");

    if (haveSnapshot)
    {
        snes_release_core_allocations(true);
        snes_log_heap_step("core freed");

        snes_log_heap_step("pre SD begin");
        if (share_sd_gameplay_mount("SNES", "save"))
        {
            snes_log_heap_step("after SD begin");
            snes_save_force_flush_buffer(sramSnapshot, sramSnapshotSize);
            snes_log_heap_step("after save");
        }
        else
        {
            SNES_LOG("[SNES][SAVE] SD remount failed, SRAM snapshot not saved\n");
        }
    }
    else if (hadSram)
    {
        snes_log_heap_step("pre SD begin");
        if (share_sd_gameplay_mount("SNES", "save"))
        {
            snes_log_heap_step("after SD begin");
            snes_save_force_flush();
            snes_log_heap_step("after save");
        }
        else
        {
            SNES_LOG("[SNES][SAVE] SD remount failed, SRAM not saved\n");
        }

        snes_release_core_allocations(true);
        snes_log_heap_step("core freed");
    }
    else
    {
        snes_release_core_allocations(true);
        snes_log_heap_step("core freed");
        SNES_LOG("[SNES][SD] no SRAM, skip SD remount/save\n");
    }

    if (sramSnapshot)
        free(sramSnapshot);

#ifdef SNES_SD_OFF_DURING_GAMEPLAY
    share_sd_gameplay_close_if_mounted("SNES", "save");
    snes_log_heap_step("SD reclosed");
#endif

    snes_save_shutdown();
    snes_reset_quit_controls();
    snes_log_heap_step("quit done");
    SNES_LOG("[SNES][QUIT] clean teardown done\n");
}

static void snes_load_sram_with_sd()
{
    if (!snes_save_has_sram())
    {
        snes_save_load();
        return;
    }

    const bool was_mounted = share_sd_is_mounted();
    if (!was_mounted)
    {
        snes_log_heap_step("pre SD load");
        if (!share_sd_gameplay_mount("SNES", "load"))
        {
            SNES_LOG("[SNES][SAVE] SD remount failed, SRAM load skipped\n");
            return;
        }
        snes_log_heap_step("after SD load");
    }

    snes_save_load();

#ifdef SNES_SD_OFF_DURING_GAMEPLAY
    if (!was_mounted)
    {
        snes_log_heap_step("pre SD close");
        share_sd_gameplay_close_if_mounted("SNES", "load");
        snes_log_heap_step("after SD close");
    }
#endif
}

#ifdef SNES_DEEP_BENCH
enum
{
    SNES_PPU_PROF_COLOR_MATH = 0,
    SNES_PPU_PROF_SIMPLE,
    SNES_PPU_PROF_SUB_RENDER_CALLS,
    SNES_PPU_PROF_SUB_RENDER_US,
    SNES_PPU_PROF_MAIN_RENDER_CALLS,
    SNES_PPU_PROF_MAIN_RENDER_US,
    SNES_PPU_PROF_SUBCLIP_US,
    SNES_PPU_PROF_BACKDROP_CALLS,
    SNES_PPU_PROF_BACKDROP_US,
    SNES_PPU_PROF_SIMPLE_FILL_US,
    SNES_PPU_PROF_BD_SUB_HALF,
    SNES_PPU_PROF_BD_SUB,
    SNES_PPU_PROF_BD_ADD_HALF,
    SNES_PPU_PROF_BD_ADD,
    SNES_PPU_PROF_BD_COPY_SUB,
    SNES_PPU_PROF_RS_OBJ_CALLS,
    SNES_PPU_PROF_RS_OBJ_US,
    SNES_PPU_PROF_RS_BG0_CALLS,
    SNES_PPU_PROF_RS_BG0_US,
    SNES_PPU_PROF_RS_BG1_CALLS,
    SNES_PPU_PROF_RS_BG1_US,
    SNES_PPU_PROF_RS_BG2_CALLS,
    SNES_PPU_PROF_RS_BG2_US,
    SNES_PPU_PROF_RS_BG3_CALLS,
    SNES_PPU_PROF_RS_BG3_US,
    SNES_PPU_PROF_RS_MODE7_CALLS,
    SNES_PPU_PROF_RS_MODE7_US,
    SNES_PPU_PROF_COUNT
};

struct SnesDeepBenchState
{
    uint32_t frames;
    uint32_t rendered;
    uint32_t skipped;
    uint32_t renderCalls;
    uint32_t displayCalls;
    uint32_t frameUs;
    uint32_t mainloopUs;
    uint32_t cpuUs;
    uint32_t renderUs;
    uint32_t mainloopRenderUs;
    uint32_t postRenderUs;
    uint32_t displayUs;
    uint32_t mainloopDisplayUs;
    uint32_t postDisplayUs;
    uint32_t maxFrameUs;
    uint32_t maxMainloopUs;
    uint32_t maxCpuUs;
    uint32_t maxRenderUs;
    uint32_t maxPostRenderUs;
    uint32_t maxDisplayUs;
    uint32_t maxMainloopDisplayUs;
    uint32_t maxPostDisplayUs;
};

enum
{
    SNES_PROFILE_PHASE_IDLE = 0,
    SNES_PROFILE_PHASE_MAINLOOP,
    SNES_PROFILE_PHASE_POST_RENDER,
};

static volatile uint32_t s_snesFrameRenderUs         = 0;
static volatile uint32_t s_snesFrameMainloopRenderUs = 0;
static volatile uint32_t s_snesFramePostRenderUs     = 0;
static volatile uint32_t s_snesFrameDisplayUs        = 0;
static volatile uint32_t s_snesFrameMainloopDisplayUs = 0;
static volatile uint32_t s_snesFramePostDisplayUs     = 0;
static volatile uint32_t s_snesFrameRenderCalls      = 0;
static volatile uint32_t s_snesFrameDisplayCalls     = 0;
static volatile uint32_t s_snesProfilePhase          = SNES_PROFILE_PHASE_IDLE;
static SnesDeepBenchState s_snesDeepBench            = {};

extern "C" void snes_profile_render_add(uint32_t elapsedUs)
{
    s_snesFrameRenderUs += elapsedUs;
    s_snesFrameRenderCalls = s_snesFrameRenderCalls + 1;

    if (s_snesProfilePhase == SNES_PROFILE_PHASE_MAINLOOP)
        s_snesFrameMainloopRenderUs += elapsedUs;
    else if (s_snesProfilePhase == SNES_PROFILE_PHASE_POST_RENDER)
        s_snesFramePostRenderUs += elapsedUs;
}

static inline void snes_profile_display_add(uint32_t elapsedUs)
{
    s_snesFrameDisplayUs += elapsedUs;
    s_snesFrameDisplayCalls = s_snesFrameDisplayCalls + 1;

    if (s_snesProfilePhase == SNES_PROFILE_PHASE_MAINLOOP)
        s_snesFrameMainloopDisplayUs += elapsedUs;
    else if (s_snesProfilePhase == SNES_PROFILE_PHASE_POST_RENDER)
        s_snesFramePostDisplayUs += elapsedUs;
}

static inline void snes_profile_set_phase(uint32_t phase)
{
    s_snesProfilePhase = phase;
}

static inline void snes_profile_frame_begin()
{
    s_snesFrameRenderUs         = 0;
    s_snesFrameMainloopRenderUs = 0;
    s_snesFramePostRenderUs     = 0;
    s_snesFrameDisplayUs        = 0;
    s_snesFrameMainloopDisplayUs = 0;
    s_snesFramePostDisplayUs     = 0;
    s_snesFrameRenderCalls      = 0;
    s_snesFrameDisplayCalls     = 0;
    s_snesProfilePhase          = SNES_PROFILE_PHASE_IDLE;
}

static inline void snes_profile_frame_end(uint32_t mainloopUs,
                                          uint32_t frameUs,
                                          bool rendered)
{
    const uint32_t renderUs         = s_snesFrameRenderUs;
    const uint32_t mainloopRenderUs = s_snesFrameMainloopRenderUs;
    const uint32_t postRenderUs     = s_snesFramePostRenderUs;
    const uint32_t displayUs        = s_snesFrameDisplayUs;
    const uint32_t mainloopDisplayUs = s_snesFrameMainloopDisplayUs;
    const uint32_t postDisplayUs     = s_snesFramePostDisplayUs;
    const uint32_t mainloopOverheadUs = mainloopRenderUs + mainloopDisplayUs;
    const uint32_t cpuUs            = mainloopUs > mainloopOverheadUs
        ? mainloopUs - mainloopOverheadUs
        : 0;

    ++s_snesDeepBench.frames;
    if (rendered)
        ++s_snesDeepBench.rendered;
    else
        ++s_snesDeepBench.skipped;

    s_snesDeepBench.renderCalls += s_snesFrameRenderCalls;
    s_snesDeepBench.displayCalls += s_snesFrameDisplayCalls;
    s_snesDeepBench.frameUs      += frameUs;
    s_snesDeepBench.mainloopUs  += mainloopUs;
    s_snesDeepBench.cpuUs       += cpuUs;
    s_snesDeepBench.renderUs    += renderUs;
    s_snesDeepBench.mainloopRenderUs += mainloopRenderUs;
    s_snesDeepBench.postRenderUs += postRenderUs;
    s_snesDeepBench.displayUs    += displayUs;
    s_snesDeepBench.mainloopDisplayUs += mainloopDisplayUs;
    s_snesDeepBench.postDisplayUs += postDisplayUs;

    if (frameUs > s_snesDeepBench.maxFrameUs)
        s_snesDeepBench.maxFrameUs = frameUs;
    if (mainloopUs > s_snesDeepBench.maxMainloopUs)
        s_snesDeepBench.maxMainloopUs = mainloopUs;
    if (cpuUs > s_snesDeepBench.maxCpuUs)
        s_snesDeepBench.maxCpuUs = cpuUs;
    if (renderUs > s_snesDeepBench.maxRenderUs)
        s_snesDeepBench.maxRenderUs = renderUs;
    if (postRenderUs > s_snesDeepBench.maxPostRenderUs)
        s_snesDeepBench.maxPostRenderUs = postRenderUs;
    if (displayUs > s_snesDeepBench.maxDisplayUs)
        s_snesDeepBench.maxDisplayUs = displayUs;
    if (mainloopDisplayUs > s_snesDeepBench.maxMainloopDisplayUs)
        s_snesDeepBench.maxMainloopDisplayUs = mainloopDisplayUs;
    if (postDisplayUs > s_snesDeepBench.maxPostDisplayUs)
        s_snesDeepBench.maxPostDisplayUs = postDisplayUs;
}

static inline void snes_deep_bench_log_window(float fps)
{
    if (s_snesDeepBench.frames == 0)
        return;

    const uint32_t frames = s_snesDeepBench.frames;
    uint32_t ppu[SNES_PPU_PROF_COUNT] = {};
    snes_profile_ppu_get_and_reset(ppu, SNES_PPU_PROF_COUNT);

    EMU_LOG("[SNES][DEEP] fps=%.2f frames=%lu drawn=%lu skip=%lu calls=%lu dispCalls=%lu avgFrame=%luus avgMain=%luus avgCpu=%luus avgRender=%luus avgMainRender=%luus avgPostRender=%luus avgDisplay=%luus avgMainDisplay=%luus avgPostDisplay=%luus maxFrame=%luus maxMain=%luus maxCpu=%luus maxRender=%luus maxPostRender=%luus maxDisplay=%luus maxMainDisplay=%luus maxPostDisplay=%luus mode=%u h=%u interlace=%u forced=%u regs=2100:%02X 212C:%02X 212D:%02X 2130:%02X 2131:%02X 2133:%02X ppu=color:%lu simple:%lu sub:%lu/%luus main:%lu/%luus subclip:%luus backdrop:%lu/%luus fill:%luus bd:%lu,%lu,%lu,%lu,%lu rs=obj:%lu/%luus bg0:%lu/%luus bg1:%lu/%luus bg2:%lu/%luus bg3:%lu/%luus m7:%lu/%luus\n",
            fps,
            (unsigned long)frames,
            (unsigned long)s_snesDeepBench.rendered,
            (unsigned long)s_snesDeepBench.skipped,
            (unsigned long)s_snesDeepBench.renderCalls,
            (unsigned long)s_snesDeepBench.displayCalls,
            (unsigned long)(s_snesDeepBench.frameUs / frames),
            (unsigned long)(s_snesDeepBench.mainloopUs / frames),
            (unsigned long)(s_snesDeepBench.cpuUs / frames),
            (unsigned long)(s_snesDeepBench.renderUs / frames),
            (unsigned long)(s_snesDeepBench.mainloopRenderUs / frames),
            (unsigned long)(s_snesDeepBench.postRenderUs / frames),
            (unsigned long)(s_snesDeepBench.displayUs / frames),
            (unsigned long)(s_snesDeepBench.mainloopDisplayUs / frames),
            (unsigned long)(s_snesDeepBench.postDisplayUs / frames),
            (unsigned long)s_snesDeepBench.maxFrameUs,
            (unsigned long)s_snesDeepBench.maxMainloopUs,
            (unsigned long)s_snesDeepBench.maxCpuUs,
            (unsigned long)s_snesDeepBench.maxRenderUs,
            (unsigned long)s_snesDeepBench.maxPostRenderUs,
            (unsigned long)s_snesDeepBench.maxDisplayUs,
            (unsigned long)s_snesDeepBench.maxMainloopDisplayUs,
            (unsigned long)s_snesDeepBench.maxPostDisplayUs,
            (unsigned)PPU.BGMode,
            (unsigned)PPU.ScreenHeight,
            (unsigned)IPPU.Interlace,
            (unsigned)PPU.ForcedBlanking,
            (unsigned)Memory.FillRAM[0x2100],
            (unsigned)Memory.FillRAM[0x212c],
            (unsigned)Memory.FillRAM[0x212d],
            (unsigned)Memory.FillRAM[0x2130],
            (unsigned)Memory.FillRAM[0x2131],
            (unsigned)Memory.FillRAM[0x2133],
            (unsigned long)ppu[SNES_PPU_PROF_COLOR_MATH],
            (unsigned long)ppu[SNES_PPU_PROF_SIMPLE],
            (unsigned long)ppu[SNES_PPU_PROF_SUB_RENDER_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_SUB_RENDER_US],
            (unsigned long)ppu[SNES_PPU_PROF_MAIN_RENDER_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_MAIN_RENDER_US],
            (unsigned long)ppu[SNES_PPU_PROF_SUBCLIP_US],
            (unsigned long)ppu[SNES_PPU_PROF_BACKDROP_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_BACKDROP_US],
            (unsigned long)ppu[SNES_PPU_PROF_SIMPLE_FILL_US],
            (unsigned long)ppu[SNES_PPU_PROF_BD_SUB_HALF],
            (unsigned long)ppu[SNES_PPU_PROF_BD_SUB],
            (unsigned long)ppu[SNES_PPU_PROF_BD_ADD_HALF],
            (unsigned long)ppu[SNES_PPU_PROF_BD_ADD],
            (unsigned long)ppu[SNES_PPU_PROF_BD_COPY_SUB],
            (unsigned long)ppu[SNES_PPU_PROF_RS_OBJ_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_OBJ_US],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG0_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG0_US],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG1_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG1_US],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG2_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG2_US],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG3_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_BG3_US],
            (unsigned long)ppu[SNES_PPU_PROF_RS_MODE7_CALLS],
            (unsigned long)ppu[SNES_PPU_PROF_RS_MODE7_US]);

    memset(&s_snesDeepBench, 0, sizeof(s_snesDeepBench));
}

static bool snes_title_contains_nocase(const char* title, const char* needle)
{
    if (!title || !needle || !*needle)
        return false;

    for (const char* p = title; *p; ++p)
    {
        const char* a = p;
        const char* b = needle;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b))
        {
            ++a;
            ++b;
        }
        if (*b == '\0')
            return true;
    }

    return false;
}
#else
#define snes_profile_frame_begin() ((void)0)
#define snes_profile_frame_end(mainloopUs, frameUs, rendered) ((void)0)
#define snes_profile_set_phase(phase) ((void)0)
#define snes_profile_display_add(elapsedUs) ((void)0)
#define snes_deep_bench_log_window(fps) ((void)0)
#endif

static inline bool snes_should_render_frame(uint32_t last_frame_exec_us,
                                            uint32_t budget55_us,
                                            bool skipped_last_render)
{
    const bool want_skip = (last_frame_exec_us > budget55_us);
    return !(want_skip && !skipped_last_render);
}

static inline void snes_update_interlace_from_fps(float fps)
{
    if (s_interlace_mode != SNES_INTERLACE_AUTO)
        return;

    if (!interlace_enabled && fps < 48.0f)
        interlace_enabled = true;
    else if (interlace_enabled && fps > 60.0f)
        interlace_enabled = false;
}

static inline void snes_log_fps_and_heap(uint32_t &frameCount, uint32_t &lastFpsMs)
{
    const uint32_t nowMs = millis();
    if (nowMs - lastFpsMs < 1000)
        return;

    const float fps = (frameCount * 1000.0f) / (nowMs - lastFpsMs);

    snes_update_interlace_from_fps(fps);

    SNES_LOG("[SNES] FPS: %.2f | HEAP: %lu | INTERLACE: %s\n",
             fps,
             (unsigned long)esp_get_free_heap_size(),
             interlace_enabled ? "ON" : "OFF");

    snes_deep_bench_log_window(fps);

    frameCount = 0;
    lastFpsMs  = nowMs;
}

static inline uint32_t snes_map_dst_to_src(uint32_t dstY, uint32_t srcH)
{
    if (srcH == 0)
        srcH = SNES_HEIGHT;

    uint32_t srcY = (((dstY << 1) + 1u) * srcH) / (LCD_H * 2u);
    if (srcY >= srcH)
        srcY = srcH - 1;

    return srcY;
}

static inline uint32_t mapDstToSrc(uint32_t dstY)
{
    return snes_map_dst_to_src(dstY, g_lineMap.srcH);
}

extern "C" bool S9xIsSourceLineNeeded(uint32_t srcY)
{
    if (!g_srcLineNeeded)
        return false;

    if (srcY >= SNES_HEIGHT_EXTENDED)
        return false;

    return g_srcLineNeeded[srcY] != 0;
}

static inline uint32_t quantizeSrcYHalfSkip(uint32_t srcY)
{
    const uint32_t parity = interlace_enabled ? fieldParity : 0u;

    if ((srcY & 1u) != parity)
    {
        if (srcY + 1u < g_lineMap.srcH)
            srcY++;
        else if (srcY > 0)
            srcY--;
    }

    return srcY;
}

static void buildInterlaceSourceMaskExact()
{
    memset(g_srcLineNeeded, 0, SNES_HEIGHT_EXTENDED);

    for (uint32_t i = 0; i < SNES_HEIGHT_EXTENDED; ++i)
    {
        g_dstFirstForSrc[i] = 0xFFFF;
        g_dstLastForSrc[i]  = 0xFFFF;
    }

    g_lineMap.srcH     = PPU.ScreenHeight ? PPU.ScreenHeight : 224;
    g_lineMap.scaleY   = (float)g_lineMap.srcH / (float)LCD_H;
    g_lineMap.srcStart = 0.5f * (g_lineMap.srcH - LCD_H * g_lineMap.scaleY);
    g_lineMap.stepY    = interlace_enabled ? 2u : 1u;

    const uint32_t nextDstY = interlace_enabled ? fieldParity : 0u;

    for (uint32_t dstY = 0; dstY < LCD_H; ++dstY)
        g_srcForDst[dstY] = 0xFFFF;

    for (uint32_t dstY = nextDstY; dstY < LCD_H; dstY += g_lineMap.stepY)
    {
        uint32_t srcY = mapDstToSrc(dstY);

        if (g_altHardHalfSkip)
            srcY = quantizeSrcYHalfSkip(srcY);

        g_srcForDst[dstY]    = (uint16_t)srcY;
        g_srcLineNeeded[srcY] = 1;

        if (g_dstFirstForSrc[srcY] == 0xFFFF)
            g_dstFirstForSrc[srcY] = (uint16_t)dstY;

        g_dstLastForSrc[srcY] = (uint16_t)dstY;
    }
}

static void buildDefaultSourceMap(uint32_t srcH, uint32_t startY, uint32_t stepY)
{
    if (g_defaultSrcMapH == srcH &&
        g_defaultSrcMapStart == startY &&
        g_defaultSrcMapStep == stepY)
    {
        return;
    }

    g_defaultSrcMapH     = srcH;
    g_defaultSrcMapStart = startY;
    g_defaultSrcMapStep  = stepY;

    for (uint32_t dstY = startY; dstY < LCD_H; dstY += stepY)
        g_defaultSrcForDst[dstY] = (uint16_t)snes_map_dst_to_src(dstY, srcH);
}

/* ---------------------------------------------------- */
/* Video callbacks                                      */
/* ---------------------------------------------------- */

static void S9XLineRender(uint32_t /*y*/,
                          const uint16_t* pixels,
                          uint32_t width)
{
#ifdef SNES_DEEP_BENCH
    const int64_t displayStartUs = esp_timer_get_time();
#endif
    snes_display_submit_line(g_dstY, pixels, width);
#ifdef SNES_DEEP_BENCH
    snes_profile_display_add((uint32_t)(esp_timer_get_time() - displayStartUs));
#endif
}

static void S9XLineRenderAlt(uint32_t srcY,
                             const uint16_t* pixels,
                             uint32_t width)
{
    if (srcY >= SNES_HEIGHT_EXTENDED)
        return;

    const uint16_t first = g_dstFirstForSrc[srcY];
    if (first == 0xFFFF)
        return;

    const uint16_t last = g_dstLastForSrc[srcY];

    for (uint32_t dstY = first; dstY <= last; dstY += g_lineMap.stepY)
    {
        if (g_srcForDst[dstY] == srcY)
        {
#ifdef SNES_DEEP_BENCH
            const int64_t displayStartUs = esp_timer_get_time();
#endif
            snes_display_submit_line(dstY, pixels, width);
#ifdef SNES_DEEP_BENCH
            snes_profile_display_add((uint32_t)(esp_timer_get_time() - displayStartUs));
#endif
        }
    }
}

extern "C" void S9xSetLineCallback(S9xLineCallback cb);

/* ---------------------------------------------------- */
/* Input hook                                           */
/* ---------------------------------------------------- */

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    return snes_input_poll();
}

/* ---------------------------------------------------- */
/* Display init hook                                    */
/* ---------------------------------------------------- */

bool S9xInitDisplay(void)
{
    GFX.Pitch     = SNES_WIDTH * sizeof(uint16_t);
    GFX.Pitch2    = GFX.Pitch;
    GFX.RealPitch = GFX.Pitch;
    GFX.ZPitch    = SNES_WIDTH;

    GFX.PPL       = SNES_WIDTH;
    GFX.PPLx2     = SNES_WIDTH * 2;

    /* we use line rendering / instant push to the screen, no framebuffer needed */
    GFX.Screen         = NULL;
    GFX.SubScreen      = NULL;
    GFX.ZBuffer        = NULL;
    GFX.SubZBuffer     = NULL;
    GFX.LineRenderMode = true;
    GFX.LinePPL        = SNES_WIDTH;
    GFX.LinePitch      = SNES_WIDTH * sizeof(uint16_t);

    return true;
}

/* ---------------------------------------------------- */
/* Core init                                            */
/* ---------------------------------------------------- */

bool snes_init()
{
    snes_log_heap_step("start");

    if (!S9xInitDisplay())
    {
        snes_log_init_failure("display", 0);
        return false;
    }
    snes_log_heap_step("display");

    if (!S9xInitGFX())
    {
        snes_log_init_failure("gfx", 0);
        return false;
    }
    snes_log_heap_step("gfx");

    if (!S9xInitMap())
    {
        snes_log_init_failure("map", snes_estimated_map_alloc_bytes());
        return false;
    }
    snes_log_heap_step("map");

    if (!S9xInitMemory())
    {
        snes_log_init_failure("memory", snes_estimated_memory_alloc_bytes());
        return false;
    }
    snes_log_heap_step("memory");

    if (!S9xInitPpu())
    {
        snes_log_init_failure("ppu", snes_estimated_ppu_alloc_bytes());
        return false;
    }
    snes_log_heap_step("ppu");

    if (!S9xInitLineBuffers())
    {
        snes_log_init_failure("linebuf", snes_estimated_linebuf_bytes());
        return false;
    }
    snes_log_heap_step("linebuf");

    /* NULL means use already mapped ROM */
    if (!LoadROM(NULL))
    {
        snes_log_init_failure("loadrom", 0);
        return false;
    }

    S9xFixColourBrightness();
    return true;
}

/* ---------------------------------------------------- */
/* ALT buffers                                          */
/* ---------------------------------------------------- */

static bool snes_alt_buffers_alloc()
{
    if (g_dstFirstForSrc && g_dstLastForSrc && g_srcLineNeeded && g_srcForDst)
        return true;

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    g_dstFirstForSrc = (uint16_t*)heap_caps_malloc(
        sizeof(uint16_t) * SNES_HEIGHT_EXTENDED, caps);

    g_dstLastForSrc = (uint16_t*)heap_caps_malloc(
        sizeof(uint16_t) * SNES_HEIGHT_EXTENDED, caps);

    g_srcLineNeeded = (uint8_t*)heap_caps_malloc(
        sizeof(uint8_t) * SNES_HEIGHT_EXTENDED, caps);

    g_srcForDst = (uint16_t*)heap_caps_malloc(
        sizeof(uint16_t) * LCD_H, caps);

    if (!g_dstFirstForSrc || !g_dstLastForSrc || !g_srcLineNeeded || !g_srcForDst)
    {
        if (g_dstFirstForSrc) { free(g_dstFirstForSrc); g_dstFirstForSrc = nullptr; }
        if (g_dstLastForSrc)  { free(g_dstLastForSrc);  g_dstLastForSrc  = nullptr; }
        if (g_srcLineNeeded)  { free(g_srcLineNeeded);  g_srcLineNeeded  = nullptr; }
        if (g_srcForDst)      { free(g_srcForDst);      g_srcForDst      = nullptr; }
        return false;
    }

    return true;
}

/* ---------------------------------------------------- */
/* RUN SNES DEFAULT                                     */
/* ---------------------------------------------------- */

void run_snes_default(const uint8_t* rom, size_t romSize, const char* romName)
{
    SNES_LOG("[SNES] ROM: %p (size %zu bytes)\n", rom, romSize);

    Memory.ROM           = (uint8_t*)rom;
    Memory.ROM_ReadOnly  = true;
    Memory.ROM_Offset    = 0;
    Memory.ROM_AllocSize = romSize;

    snes_apply_common_settings();

    if (!snes_init())
    {
        snes_log_init_failure("snes_init", snes_estimated_core_alloc_bytes());
        return;
    }

    snes_save_prepare_sram();
    snes_log_heap_step("sram");
    snes_save_init(romName);
    snes_load_sram_with_sd();
    snes_enter_sd_off_gameplay();
    snes_init_dynamic_tilecache();

    S9xReset();

    const int      targetFps        = 60;
    const uint32_t frame_us         = 1000000u / (uint32_t)targetFps;
    const uint32_t budget55_us      = 1000000u / 55u;
    uint64_t       next_frame_us    = esp_timer_get_time();
    uint32_t       frameCount       = 0;
    uint32_t       lastFpsMs        = millis();
    uint32_t       last_frame_exec_us = 0;
    bool           skipped_last_render = false;
    int64_t        now;
    int64_t        lateness;

    snes_log_heap_runtime("pre-display", 0);
    snes_display_init();
    snes_log_heap_runtime("display-init", 0);
    snes_display_start();
    snes_log_heap_runtime("display-start", 0);
    snes_input_start();
    snes_log_heap_runtime("input-start", 0);

    snes_log_runtime_config(targetFps);

    heap_caps_check_integrity_all(true);
    bool firstFrameLogged = false;

    while (true)
    {
        IPPU.RenderThisFrame = snes_should_render_frame(
            last_frame_exec_us, budget55_us, skipped_last_render);

        const int64_t frame_start_us = esp_timer_get_time();
        if (!firstFrameLogged)
            snes_log_heap_runtime("pre-mainloop", frameCount);

        snes_input_tick();
        if (share::restartRequested())
            break;
#ifdef SNES_DEEP_BENCH
        snes_profile_frame_begin();
        snes_profile_set_phase(SNES_PROFILE_PHASE_MAINLOOP);
        const int64_t mainloop_start_us = esp_timer_get_time();
#endif
        S9xMainLoop();
#ifdef SNES_DEEP_BENCH
        const uint32_t mainloop_us = (uint32_t)(esp_timer_get_time() - mainloop_start_us);
        snes_profile_set_phase(SNES_PROFILE_PHASE_IDLE);
#endif
        snes_save_tick();
        if (!firstFrameLogged)
            snes_log_heap_runtime("post-mainloop", frameCount);

        if (IPPU.RenderThisFrame)
        {
            const uint32_t startY = interlace_enabled ? fieldParity : 0;
            const uint32_t stepY  = interlace_enabled ? 2 : 1;
            const uint32_t srcH   = PPU.ScreenHeight ? PPU.ScreenHeight : SNES_HEIGHT;

            buildDefaultSourceMap(srcH, startY, stepY);

#ifdef SNES_DEEP_BENCH
            snes_profile_set_phase(SNES_PROFILE_PHASE_POST_RENDER);
#endif
            for (uint32_t dstY = startY; dstY < LCD_H; dstY += stepY)
            {
                g_dstY = dstY;
                S9xRenderLine_NoFramebuffer((uint32_t)g_defaultSrcForDst[dstY], S9XLineRender);
            }
#ifdef SNES_DEEP_BENCH
            snes_profile_set_phase(SNES_PROFILE_PHASE_IDLE);
#endif

            if (interlace_enabled && !snes_interlace_lock_parity)
                fieldParity ^= 1;
        }
        if (!firstFrameLogged)
        {
            snes_log_heap_runtime("post-render", frameCount);
            firstFrameLogged = true;
        }

        const int64_t frame_end_us = esp_timer_get_time();
        const uint32_t frame_exec_us = (uint32_t)(frame_end_us - frame_start_us);
#ifdef SNES_DEEP_BENCH
        snes_profile_frame_end(mainloop_us, frame_exec_us, IPPU.RenderThisFrame);
#endif
        last_frame_exec_us = frame_exec_us;
        skipped_last_render = !IPPU.RenderThisFrame;

        ++frameCount;
        snes_log_fps_and_heap(frameCount, lastFpsMs);

        next_frame_us += frame_us;
        now      = (int64_t)esp_timer_get_time();
        lateness = now - (int64_t)next_frame_us;

        if (lateness > 0)
        {
            if (lateness > (int64_t)frame_us)
                next_frame_us = (uint64_t)now;
            continue;
        }

        share::sleep_until_us(next_frame_us);
    }

    snes_clean_teardown_and_save();
}

/* ---------------------------------------------------- */
/* RUN SNES ALT                                         */
/* ---------------------------------------------------- */

void run_snes_alt(const uint8_t* rom, size_t romSize, const char* romName)
{
    SNES_LOG("[SNES] ROM: %p (size %zu bytes)\n", rom, romSize);

    Memory.ROM           = (uint8_t*)rom;
    Memory.ROM_ReadOnly  = true;
    Memory.ROM_Offset    = 0;
    Memory.ROM_AllocSize = romSize;

    snes_apply_common_settings();
    S9xSetLineCallback(S9XLineRenderAlt);

    if (!snes_init())
    {
        snes_log_init_failure("snes_init", snes_estimated_core_alloc_bytes());
        return;
    }

    snes_save_prepare_sram();
    snes_log_heap_step("sram");
    snes_save_init(romName);
    snes_load_sram_with_sd();
    snes_enter_sd_off_gameplay();
    snes_init_dynamic_tilecache();

    S9xReset();

    const int      targetFps          = 60;
    const uint32_t frame_us           = 1000000u / (uint32_t)targetFps;
    const uint32_t budget55_us        = 1000000u / 55u;
    uint64_t       next_frame_us      = esp_timer_get_time();
    uint32_t       frameCount         = 0;
    uint32_t       lastFpsMs          = millis();
    uint32_t       last_frame_exec_us = 0;
    bool           skipped_last_render = false;
    int64_t        now;
    int64_t        lateness;

    snes_log_heap_runtime("pre-display", 0);
    snes_display_init();
    snes_log_heap_runtime("display-init", 0);
    snes_display_start();
    snes_log_heap_runtime("display-start", 0);
    snes_input_start();
    snes_log_heap_runtime("input-start", 0);

    snes_log_runtime_config(targetFps);

    heap_caps_check_integrity_all(true);
    bool firstFrameLogged = false;

    while (true)
    {
        IPPU.RenderThisFrame = snes_should_render_frame(
            last_frame_exec_us, budget55_us, skipped_last_render);

        if (IPPU.RenderThisFrame)
            buildInterlaceSourceMaskExact();

        const int64_t frame_start_us = esp_timer_get_time();
        if (!firstFrameLogged)
            snes_log_heap_runtime("pre-mainloop", frameCount);

        snes_input_tick();
        if (share::restartRequested())
            break;
#ifdef SNES_DEEP_BENCH
        snes_profile_frame_begin();
        snes_profile_set_phase(SNES_PROFILE_PHASE_MAINLOOP);
        const int64_t mainloop_start_us = esp_timer_get_time();
#endif
        S9xMainLoop();
#ifdef SNES_DEEP_BENCH
        const uint32_t mainloop_us = (uint32_t)(esp_timer_get_time() - mainloop_start_us);
        snes_profile_set_phase(SNES_PROFILE_PHASE_IDLE);
#endif
        snes_save_tick();
        if (!firstFrameLogged)
            snes_log_heap_runtime("post-mainloop", frameCount);

        if (IPPU.RenderThisFrame && interlace_enabled && !snes_interlace_lock_parity)
            fieldParity ^= 1;
        if (!firstFrameLogged)
        {
            snes_log_heap_runtime("post-render", frameCount);
            firstFrameLogged = true;
        }

        const int64_t frame_end_us = esp_timer_get_time();
        const uint32_t frame_exec_us = (uint32_t)(frame_end_us - frame_start_us);
#ifdef SNES_DEEP_BENCH
        snes_profile_frame_end(mainloop_us, frame_exec_us, IPPU.RenderThisFrame);
#endif
        last_frame_exec_us = frame_exec_us;
        skipped_last_render = !IPPU.RenderThisFrame;

        ++frameCount;
        snes_log_fps_and_heap(frameCount, lastFpsMs);

        next_frame_us += frame_us;
        now      = (int64_t)esp_timer_get_time();
        lateness = now - (int64_t)next_frame_us;

        if (lateness > 0)
        {
            if (lateness > (int64_t)frame_us)
                next_frame_us = (uint64_t)now;
            continue;
        }

        share::sleep_until_us(next_frame_us);
    }

    snes_clean_teardown_and_save();
}

/* ---------------------------------------------------- */
/* Entry point                                          */
/* ---------------------------------------------------- */

static const char* snes_interlace_mode_name(SnesInterlaceMode mode)
{
    switch (mode)
    {
        case SNES_INTERLACE_ON:   return "ON";
        case SNES_INTERLACE_AUTO: return "AUTO";
        case SNES_INTERLACE_OFF:
        default:                  return "OFF";
    }
}

void run_snes(const uint8_t* rom, size_t romSize, const char* romName, SnesInterlaceMode interlaceMode)
{
    s_interlace_mode = interlaceMode;
    interlace_enabled = (interlaceMode == SNES_INTERLACE_ON);
    fieldParity = 0;
    snes_interlace_lock_parity = false;

    SNES_LOG("[SNES][BOOT] run_snes entered rom=%s ptr=%p size=%zu interlace=%s\n",
            romName ? romName : "(null)", rom, romSize, snes_interlace_mode_name(interlaceMode));

    const bool alt = isAltGame(rom, romSize);

    char title[32];
    const bool haveTitle = snes_read_title(title, sizeof(title), rom, romSize);
#ifdef SNES_DEEP_BENCH
    g_snes_animaniacs_probe_enabled = haveTitle &&
        snes_title_contains_nocase(title, "ANIMANIACS");
    if (g_snes_animaniacs_probe_enabled)
        EMU_LOG("[SNES][DEEP][ANIMANIACS] title probe enabled for '%s'\n", title);
#endif
    if (haveTitle)
        SNES_LOG("[SNES] Internal title: %s, ALT: %s\n", title, alt ? "YES" : "NO");

    // Some game can't render line by line properly, for those we use an alternate rendering method 
    if (alt)
    {
        if (!snes_alt_buffers_alloc()) {
            snes_log_init_failure("altbuf", snes_estimated_alt_alloc_bytes());
            return;
        }
        run_snes_alt(rom, romSize, romName);
    }
    // Default rendering method, should work for most games and is more efficient
    else
    {
        S9xSetLineCallback(NULL);
        run_snes_default(rom, romSize, romName);
    }
}
