#include <Arduino.h>
extern "C" {
  #include "gnuboy/gnuboy.h"
}

#include "gbc_display.h"
#include "gbc_sound.h"
#include "gbc_input.h"
#include "gbc_save.h"
#include "share/utils.h"

static uint16_t* s_gbFramebuf = nullptr;
static int16_t* s_audioBuf = nullptr;
extern int gbc_sampleRate;

// GNUBOY video callback
static void gbc_video_callback(void *buffer)
{
    // buffer == s_gbFramebuf
    (void)buffer;
    gbc_display_submit_frame(s_gbFramebuf, GB_WIDTH, GB_WIDTH, GB_HEIGHT);
}

// GNUBOY audio callback
void gbc_audio_callback(void *buffer, size_t length)
{
    if (!buffer || length == 0) return;

    gbc_sound_submit((const int16_t*)buffer, length);
}

void gbc_allocate_buffers() {
    // GNUBOY framebuffer
    size_t fbSize = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
    s_gbFramebuf = (uint16_t*)heap_caps_malloc(
        fbSize,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_gbFramebuf) {
        printf("[GBC] FATAL: framebuffer alloc failed\n");
        abort();
    }
    memset(s_gbFramebuf, 0, fbSize);

    // GNUBOY audio buffer
    s_audioBuf = (int16_t*)heap_caps_malloc(
        2048 * sizeof(int16_t),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_audioBuf) {
        printf("[GBC] FATAL: audio buffer alloc failed\n");
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
    
    // init GNUBOY core
    int ret = gnuboy_init(
        gbc_sampleRate,
        GB_AUDIO_MONO_S16,
        GB_PIXEL_565_LE,
        &gbc_video_callback,
        &gbc_audio_callback
    );
    printf("[GBC] gnuboy_init => %d\n", ret);
    if (ret < 0) return;

    // buffers
    gnuboy_set_framebuffer(s_gbFramebuf);
    gnuboy_set_soundbuffer(s_audioBuf, 2048);

    // load ROM
    ret = gnuboy_load_rom(romData, romLen);
    printf("[GBC] gnuboy_load_rom => %d\n", ret);
    if (ret < 0) return;

    gnuboy_reset(true);
    gnuboy_set_pad(0);
    gbc_save_init(romPathOrName);
    gbc_save_load();
    
    // FPS counter
    uint32_t frameCount = 0;
    uint32_t lastFpsMs  = millis();

    // Pacing 60 Hz
    const int targetFps       = 60;
    const uint32_t frame_us   = 1000000u / (uint32_t)targetFps; // 16.6 ms
    uint64_t next_frame_us    = esp_timer_get_time();
    bool drawFrame            = true; 
    bool gbcGame              = isGbcGame(romPathOrName);
    gbPalette                 = gbcGame ? -1 : gbPalette;
    int lastPalette           = -1;

    printf("Initial palette: %d\n", gbPalette);

    printf("[GBC] starting main loop @ %d FPS\n", targetFps);

    while (true) {
        if (!gbcGame && lastPalette != gbPalette) {
            lastPalette = gbPalette;
            gnuboy_set_palette((gb_palette_t)gbPalette);
        }

        // 1/2 frame skip
        bool doDraw = drawFrame && ((frameCount & 1) == 0);
        gnuboy_run(doDraw);   // trigger callbacks video/audio

        int pad = gbc_input_poll();
        if (pad >= 0) {
            gnuboy_set_pad(pad);
        }

        gbc_save_tick();

        // FPS log
        frameCount++;
        uint32_t nowMs = millis();
        if (nowMs - lastFpsMs >= 1000) {
            float fps = (frameCount * 1000.0f) / (nowMs - lastFpsMs);
            printf("[GBC] FPS: %.2f | HEAP %u\n", fps, esp_get_free_heap_size());
            frameCount = 0;
            lastFpsMs  = nowMs;
        }

        // Pacing
        next_frame_us += frame_us;
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t lateness = now - (int64_t)next_frame_us;

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