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

extern "C" {
    #include "snes9x/snes9x.h"
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

static bool     g_altHardHalfSkip = true;
static uint32_t g_dstY            = 0;

bool     interlace_enabled           = false;
uint32_t fieldParity                 = 0;
bool     snes_interlace_lock_parity  = false;

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
    return (sizeof(uint16_t) * SNES_WIDTH * 2u * 2u) +
           (sizeof(uint8_t) * SNES_WIDTH * 2u * 2u);
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
    SNES_LOG("[SNES] Core/Video only, no audio, %s, no tilecache, %d FPS target\n",
             snes_save_has_sram() ? "with SRAM" : "no SRAM",
             targetFps);
}
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

    frameCount = 0;
    lastFpsMs  = nowMs;
}

static inline uint32_t mapDstToSrc(uint32_t dstY)
{
    const float srcYf = g_lineMap.srcStart + (dstY + 0.5f) * g_lineMap.scaleY;
    int32_t srcY = (int32_t)srcYf;

    if (srcY < 0)
        srcY = 0;
    if (srcY >= (int32_t)g_lineMap.srcH)
        srcY = (int32_t)g_lineMap.srcH - 1;

    return (uint32_t)srcY;
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

/* ---------------------------------------------------- */
/* Video callbacks                                      */
/* ---------------------------------------------------- */

static void S9XLineRender(uint32_t /*y*/,
                          const uint16_t* pixels,
                          uint32_t width)
{
    snes_display_submit_line(g_dstY, pixels, width);
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
            snes_display_submit_line(dstY, pixels, width);
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

    if (!S9xInitMemory())
    {
        snes_log_init_failure("memory", snes_estimated_memory_alloc_bytes());
        return false;
    }
    snes_log_heap_step("memory");

    if (!S9xInitMap())
    {
        snes_log_init_failure("map", snes_estimated_map_alloc_bytes());
        return false;
    }
    snes_log_heap_step("map");

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
    snes_save_load();

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

    const float scale    = (float)PPU.ScreenHeight / (float)LCD_H;
    const float srcStart = 0.5f * (PPU.ScreenHeight - LCD_H * scale);

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

        S9xMainLoop();
        snes_save_tick();
        if (!firstFrameLogged)
            snes_log_heap_runtime("post-mainloop", frameCount);

        if (IPPU.RenderThisFrame)
        {
            const uint32_t startY = interlace_enabled ? fieldParity : 0;
            const uint32_t stepY  = interlace_enabled ? 2 : 1;

            for (uint32_t dstY = startY; dstY < LCD_H; dstY += stepY)
            {
                float   srcYf = srcStart + (dstY + 0.5f) * scale;
                int32_t srcY  = (int32_t)srcYf;

                if (srcY < 0)
                    srcY = 0;
                if (srcY >= (int32_t)PPU.ScreenHeight)
                    srcY = PPU.ScreenHeight - 1;

                g_dstY = dstY;
                S9xRenderLine_NoFramebuffer((uint32_t)srcY, S9XLineRender);
            }

            if (interlace_enabled && !snes_interlace_lock_parity)
                fieldParity ^= 1;
        }
        if (!firstFrameLogged)
        {
            snes_log_heap_runtime("post-render", frameCount);
            firstFrameLogged = true;
        }

        const int64_t frame_end_us = esp_timer_get_time();
        last_frame_exec_us = (uint32_t)(frame_end_us - frame_start_us);
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
}

/* ---------------------------------------------------- */
/* RUN SNES ALT                                         */
/* ---------------------------------------------------- */

void run_snes_alt(const uint8_t* rom, size_t romSize, const char* romName)
{
    SNES_LOG("[SNES] ROM: %p (size %zu bytes)\n", rom, romSize);

    Memory.ROM           = (uint8_t*)rom;
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
    snes_save_load();

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

        S9xMainLoop();
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
        last_frame_exec_us = (uint32_t)(frame_end_us - frame_start_us);
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
}

/* ---------------------------------------------------- */
/* Entry point                                          */
/* ---------------------------------------------------- */

void run_snes(const uint8_t* rom, size_t romSize, const char* romName)
{
    SNES_LOG("[SNES][BOOT] run_snes entered rom=%s ptr=%p size=%zu\n",
            romName ? romName : "(null)", rom, romSize);

    const bool alt = isAltGame(rom, romSize);

    char title[32];
    if (snes_read_title(title, sizeof(title), rom, romSize))
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
