#include "run_lynx.h"

#include "compat/arduino_compat.h"
#include "handy/handy.h"
#include "lynx_display.h"
#include "lynx_input.h"
#include "lynx_sound.h"
#include "share/utils.h"
#include "share/emu_log_cpp.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

static CSystem*  s_lynx     = nullptr;
static uint16_t* s_fb       = nullptr;
static SWORD*    s_audioBuf = nullptr;

// ───────────────────────────────────────────────
// Initialization of the Lynx core
// ───────────────────────────────────────────────
static bool lynx_init_core(const uint8_t* romData, size_t romLen, int sampleRate)
{
    if (!romData || romLen == 0) {
        EMU_LOG("[LYNX] invalid ROM buffer\n");
        return false;
    }

    // ── Handy core ───────────────────────────────
    s_lynx = new CSystem(
        (UBYTE*)romData,
        (ULONG)romLen,
        MIKIE_PIXEL_FORMAT_16BPP_565_BE,
        (ULONG)sampleRate
    );

    if (!s_lynx) {
        EMU_LOG("[LYNX] CSystem allocation failed\n");
        return false;
    }

    if (s_lynx->mFileType == HANDY_FILETYPE_ILLEGAL) {
        EMU_LOG("[LYNX] ROM loading failed (illegal file type)\n");
        delete s_lynx;
        s_lynx = nullptr;
        return false;
    }

    // ── Framebuffer ──────────────────────────
    const size_t fbSize = HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT * sizeof(uint16_t);
    s_fb = (uint16_t*)heap_caps_malloc(
        fbSize,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_fb) {
        EMU_LOG("[LYNX] framebuffer alloc failed\n");
        delete s_lynx;
        s_lynx = nullptr;
        return false;
    }
    memset(s_fb, 0, fbSize);

    // Handy framebuffer pointer
    gPrimaryFrameBuffer = (UBYTE*)s_fb;

    // ── Buffer audio  ────────────────────────────
    const size_t audioSamples = 4096 * 2;
    s_audioBuf = (SWORD*)heap_caps_malloc(
        audioSamples * sizeof(SWORD),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_audioBuf) {
        EMU_LOG("[LYNX] audio buffer alloc failed\n");
        delete s_lynx;
        s_lynx = nullptr;
        free(s_fb);
        s_fb = nullptr;
        return false;
    }
    memset(s_audioBuf, 0, audioSamples * sizeof(SWORD));

    gAudioBuffer        = s_audioBuf;
    gAudioBufferPointer = 0;
    gAudioEnabled       = 0; 

    s_lynx->mMikie->SetRotation(MIKIE_NO_ROTATE);

    EMU_LOG("[LYNX] core initialized OK\n");
    return true;
}

// ───────────────────────────────────────────────
// Main loop that runs the Lynx emulation
// ───────────────────────────────────────────────
void run_lynx(const uint8_t* romData, size_t romLen, const char* romName)
{
    const int sampleRate    = lynx_sampleRate;
    const int targetFps     = 60;
    const uint32_t frame_us = 1000000u / (uint32_t)targetFps;
    uint64_t next_frame_us = esp_timer_get_time();
    uint32_t frameCount    = 0;
    uint32_t lastLogMs     = millis();
    bool skipNextDraw = false;
    int  skippedInARow = 0;

    // ── Display ─────────────────────────────────────
    lynx_display_init();
    lynx_display_start();

    // ── Core Handy ───────────────────────────────
    if (!lynx_init_core(romData, romLen, sampleRate)) {
        EMU_LOG("[LYNX] init failed, aborting\n");
        return;
    }

    // ── Audio Cardputer ─────
    lynx_sound_init(sampleRate);
    gAudioBufferPointer = 0;
    gAudioLastUpdateCycle = 0;
    gAudioEnabled = 1;
    bool drawFrame = true;

    EMU_LOG("[LYNX] starting loop @ %d FPS\n", targetFps);

    // ── Main loop ───────────
    while (true) {
        // ── Input Lynx ───────────────────────────────
        int pad = lynx_input_poll();
        if (pad >= 0) {
            s_lynx->SetButtonData((ULONG)pad);
        }

        // ── Draw ───────────────────────────────────
        // Render at least one frame if too many were skipped
        if (!drawFrame && skippedInARow >= 2) {
            drawFrame = true;
            skipNextDraw = false;
        }
        s_lynx->UpdateFrame(drawFrame);

        // ── Video ───────────────────────────────────
        if (drawFrame) {
            lynx_display_submit_frame(
                (const uint16_t*)gPrimaryFrameBuffer,
                HANDY_SCREEN_WIDTH,
                HANDY_SCREEN_HEIGHT
            );
            skippedInARow = 0;
        } else {
            skippedInARow++;
        }

        skipNextDraw = !skipNextDraw;

        // ── Audio  ───────────────────────────────────
        if (gAudioBufferPointer > 0 && gAudioBuffer) {
            size_t frames = gAudioBufferPointer / 2;  // 2 samples per frame (L,R)
            lynx_sound_submit_frames_stereo((const int16_t*)gAudioBuffer, frames);
            gAudioBufferPointer = 0;
        }

        // ── Logs FPS  ──────────────────────────────
        frameCount++;
        uint32_t nowMs = millis();
        if (nowMs - lastLogMs >= 1000) {
            float fps = (frameCount * 1000.0f) / (nowMs - lastLogMs);
            EMU_LOG("[LYNX] FPS: %.2f | HEAP %lu\n",
                   fps, (unsigned long)esp_get_free_heap_size());
            frameCount = 0;
            lastLogMs  = nowMs;
        }

        // ── Pacing 60 Hz ────────────────────────────
        next_frame_us += frame_us;
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t lateness = now - (int64_t)next_frame_us;

        if (lateness > 0) {
            if (lateness > (int64_t)frame_us) {
                next_frame_us = (uint64_t)now;
            }
            if (lateness > 500) skipNextDraw = true; // 58 fps
            taskYIELD();
            continue;
        } else {
            share::sleep_until_us(next_frame_us);
        }
    }
}
