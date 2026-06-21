#include "compat/arduino_compat.h"
extern "C" {
  #include "gnuboy/gnuboy.h"
}

#include "gbc_display.h"
#include "gbc_sound.h"
#include "gbc_input.h"
#include "gbc_save.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "share/utils.h"
#include "share/emu_log_cpp.h"

static uint16_t* s_gbFramebuf = nullptr;
static int16_t* s_audioBuf = nullptr;
extern int gbc_sampleRate;

#if GB_BENCHMARK_LOGS_ENABLED
static uint32_t s_benchAudioBlocks = 0;
static uint32_t s_benchAudioSamples = 0;
#endif

// callback video GNUBOY
static void gbc_video_callback(void *buffer)
{
    // buffer == s_gbFramebuf
    (void)buffer;
    gbc_display_submit_frame(s_gbFramebuf, GB_WIDTH, GB_WIDTH, GB_HEIGHT);
}

// callback audio GNUBOY
void gbc_audio_callback(void *buffer, size_t length)
{
    if (!buffer || length == 0) return;

#if GB_BENCHMARK_LOGS_ENABLED
    ++s_benchAudioBlocks;
    s_benchAudioSamples += (uint32_t)length;
#endif
    gbc_sound_submit((const int16_t*)buffer, length);
}

void gbc_allocate_buffers() {
    // framebuffer GNUBOY
    size_t fbSize = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
    s_gbFramebuf = (uint16_t*)heap_caps_malloc(
        fbSize,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_gbFramebuf) {
        EMU_LOG("[GBC] FATAL: framebuffer alloc failed\n");
        abort();
    }
    memset(s_gbFramebuf, 0, fbSize);

    // buffer audio GNUBOY
    s_audioBuf = (int16_t*)heap_caps_malloc(
        2048 * sizeof(int16_t),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_audioBuf) {
        EMU_LOG("[GBC] FATAL: audio buffer alloc failed\n");
        abort();
    }
    memset(s_audioBuf, 0, 2048 * sizeof(int16_t));

}

bool isGbcGame(const char* filename) {
    if (!filename) return false;

    size_t len = strlen(filename);
    if (len < 4) return false;

    const char* ext = filename + (len - 4);

    return (strcasecmp(ext, ".gbc") == 0);
}

void run_gbc(const uint8_t* romData, size_t romLen, const char* romPathOrName) {
    // Audio Video
    gbc_display_init();
    gbc_display_start();
    gbc_sound_init(gbc_sampleRate);
    gbc_allocate_buffers();
    
    // init core GNUBOY
    int ret = gnuboy_init(
        gbc_sampleRate,
        GB_AUDIO_MONO_S16,
        GB_PIXEL_565_LE,
        &gbc_video_callback,
        &gbc_audio_callback
    );
    EMU_LOG("[GBC] gnuboy_init => %d\n", ret);
    if (ret < 0) return;

    // buffers
    gnuboy_set_framebuffer(s_gbFramebuf);
    gnuboy_set_soundbuffer(s_audioBuf, 2048);

    // load ROM
    ret = gnuboy_load_rom(romData, romLen);
    EMU_LOG("[GBC] gnuboy_load_rom => %d\n", ret);
    if (ret < 0) return;

    gnuboy_reset(true);
    gnuboy_set_pad(0);
    gbc_save_init(romPathOrName);
    gbc_save_load();
    
    uint32_t frameIndex = 0;
#if EMU_LOG_MASTER_ENABLED && !GB_BENCHMARK_LOGS_ENABLED
    uint32_t fpsFrameCount = 0;
    uint32_t lastFpsMs  = millis();
#endif
#if GB_BENCHMARK_LOGS_ENABLED
    uint32_t benchWindowFrames = 0;
    uint32_t benchDrawFrames = 0;
    uint32_t benchNoDrawFrames = 0;
    uint32_t benchLateFrames = 0;
    uint32_t benchMaxLateUs = 0;
    uint64_t benchCoreUsTotal = 0;
    uint32_t benchCoreUsMax = 0;
    uint32_t benchLastMs = millis();
#endif

    // Pacing 60 Hz
    const int targetFps       = 60;
    const uint32_t frame_us   = 1000000u / (uint32_t)targetFps; // 16.6 ms
    uint64_t next_frame_us    = esp_timer_get_time();
    bool drawFrame            = true; 
    bool gbcGame              = isGbcGame(romPathOrName);
    gbPalette                 = gbcGame ? -1 : gbPalette;
    int lastPalette           = -1;

    EMU_LOG("Palette initiale: %d\n", gbPalette);

    EMU_LOG("[GBC] starting main loop @ %d FPS\n", targetFps);

    while (true) {
        if (!gbcGame && lastPalette != gbPalette) {
            lastPalette = gbPalette;
            gnuboy_set_palette((gb_palette_t)gbPalette);
        }

        // 1/2 frame skip
        bool doDraw = drawFrame && ((frameIndex & 1) == 0);
#if GB_BENCHMARK_LOGS_ENABLED
        const int64_t coreStartUs = esp_timer_get_time();
#endif
        gnuboy_run(doDraw);   // trigger callbacks video/audio
#if GB_BENCHMARK_LOGS_ENABLED
        const uint32_t coreUs = (uint32_t)(esp_timer_get_time() - coreStartUs);
        benchCoreUsTotal += coreUs;
        if (coreUs > benchCoreUsMax) {
            benchCoreUsMax = coreUs;
        }
        ++benchWindowFrames;
        if (doDraw) {
            ++benchDrawFrames;
        } else {
            ++benchNoDrawFrames;
        }
#endif

        int pad = gbc_input_poll();
        if (pad >= 0) {
            gnuboy_set_pad(pad);
        }

        gbc_save_tick();

        // FPS log
        frameIndex++;
#if EMU_LOG_MASTER_ENABLED && !GB_BENCHMARK_LOGS_ENABLED
        fpsFrameCount++;
        uint32_t nowMs = millis();
        if (nowMs - lastFpsMs >= 1000) {
            float fps = (fpsFrameCount * 1000.0f) / (nowMs - lastFpsMs);
            EMU_LOG("[GBC] FPS: %.2f | HEAP %lu\n", fps, (unsigned long)esp_get_free_heap_size());
            fpsFrameCount = 0;
            lastFpsMs  = nowMs;
        }
#endif

        // Pacing
        next_frame_us += frame_us;
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t lateness = now - (int64_t)next_frame_us;
#if GB_BENCHMARK_LOGS_ENABLED
        if (lateness > 0) {
            ++benchLateFrames;
            if ((uint32_t)lateness > benchMaxLateUs) {
                benchMaxLateUs = (uint32_t)lateness;
            }
        }

        const uint32_t benchNowMs = millis();
        if (benchNowMs - benchLastMs >= 2000) {
            uint32_t displaySubmitted = 0;
            uint32_t displayRendered = 0;
            uint32_t displayDropped = 0;
            uint32_t displayAvgUs = 0;
            uint32_t displayMaxUs = 0;
            gbc_display_get_and_reset_bench(&displaySubmitted,
                                            &displayRendered,
                                            &displayDropped,
                                            &displayAvgUs,
                                            &displayMaxUs);
            const uint32_t windowMs = benchNowMs - benchLastMs;
            const float coreFps = windowMs ? (benchWindowFrames * 1000.0f) / windowMs : 0.0f;
            const float videoFps = windowMs ? (displayRendered * 1000.0f) / windowMs : 0.0f;
            const uint32_t coreAvgUs = benchWindowFrames
                ? (uint32_t)(benchCoreUsTotal / benchWindowFrames)
                : 0;
            GB_BENCH_LOG("coreFps=%.2f videoFps=%.2f frames=%lu draw/nodraw=%lu/%lu coreUs avg/max=%lu/%lu displayUs avg/max=%lu/%lu submitted/rendered/dropped=%lu/%lu/%lu late=%lu maxLateUs=%lu audio blocks/samples=%lu/%lu heap free/largest/min=%lu/%lu/%lu",
                         coreFps,
                         videoFps,
                         (unsigned long)benchWindowFrames,
                         (unsigned long)benchDrawFrames,
                         (unsigned long)benchNoDrawFrames,
                         (unsigned long)coreAvgUs,
                         (unsigned long)benchCoreUsMax,
                         (unsigned long)displayAvgUs,
                         (unsigned long)displayMaxUs,
                         (unsigned long)displaySubmitted,
                         (unsigned long)displayRendered,
                         (unsigned long)displayDropped,
                         (unsigned long)benchLateFrames,
                         (unsigned long)benchMaxLateUs,
                         (unsigned long)s_benchAudioBlocks,
                         (unsigned long)s_benchAudioSamples,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

            benchWindowFrames = 0;
            benchDrawFrames = 0;
            benchNoDrawFrames = 0;
            benchLateFrames = 0;
            benchMaxLateUs = 0;
            benchCoreUsTotal = 0;
            benchCoreUsMax = 0;
            s_benchAudioBlocks = 0;
            s_benchAudioSamples = 0;
            benchLastMs = benchNowMs;
        }
#endif

        if (lateness > 0) {
            // We are late, skip drawing the next frame
            drawFrame = false;

            // realign the timeline to avoid infinite accumulation
            if (lateness > (int64_t)frame_us) {
                next_frame_us = (uint64_t)now;
            }

            continue;

        } else {
            drawFrame = true;
            share::sleep_until_us(next_frame_us);
        }
    }
}
