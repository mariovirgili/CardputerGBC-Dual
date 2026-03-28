// lynx_sound.cpp
#include "lynx_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int kChannel = 0;

// ================== STATE AUDIO ==================

static bool   s_inited        = false;
int           lynx_sampleRate = 32000;

static TaskHandle_t   s_audioTask  = nullptr;
static volatile bool  s_running    = false;

static constexpr int  kRingSamples = 4096;
static int16_t*       s_ring       = nullptr;
static int            s_ringSize   = 0;
static volatile int   s_ringRead   = 0;
static volatile int   s_ringWrite  = 0;
static volatile int   s_ringCount  = 0;

// protection ring
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// ================== TASK AUDIO ==================

static void lynx_audio_task(void* arg)
{
    (void)arg;

    static const int kChunk = 512; 
    int16_t local[kChunk];

    while (s_running) {
        if (s_ringCount == 0) {
            // nothing to read
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        int toRead = 0;

        // Read a chunk from the ring buffer
        portENTER_CRITICAL(&s_ringMux);
        if (s_ringCount > 0) {
            toRead = (s_ringCount < kChunk) ? s_ringCount : kChunk;

            for (int i = 0; i < toRead; ++i) {
                local[i] = s_ring[s_ringRead];
                s_ringRead++;
                if (s_ringRead >= s_ringSize) s_ringRead = 0;
            }

            s_ringCount -= toRead;
        }
        portEXIT_CRITICAL(&s_ringMux);

        if (toRead <= 0) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        int queued = M5Cardputer.Speaker.isPlaying(kChannel);

        if (queued == 0) {
            // Startup: send 2 chunks to fill the pipe
            M5Cardputer.Speaker.playRaw(
                local,
                (size_t)toRead,
                (uint32_t)lynx_sampleRate,
                false,   // stereo
                1,       // repeat
                kChannel,
                false    // dont stop current sound
            );

            // Send another chunk if available
            int16_t local2[kChunk];
            int toRead2 = 0;

            portENTER_CRITICAL(&s_ringMux);
            if (s_ringCount > 0) {
                toRead2 = (s_ringCount < kChunk) ? s_ringCount : kChunk;
                for (int i = 0; i < toRead2; ++i) {
                    local2[i] = s_ring[s_ringRead];
                    s_ringRead++;
                    if (s_ringRead >= s_ringSize) s_ringRead = 0;
                }
                s_ringCount -= toRead2;
            }
            portEXIT_CRITICAL(&s_ringMux);

            if (toRead2 > 0) {
                M5Cardputer.Speaker.playRaw(
                    local2,
                    (size_t)toRead2,
                    (uint32_t)lynx_sampleRate,
                    false, 1, kChannel, false
                );
            }
        } else {
            // There is already at least 1 buffer in the queue: we still feed it
            M5Cardputer.Speaker.playRaw(
                local,
                (size_t)toRead,
                (uint32_t)lynx_sampleRate,
                false, 1, kChannel, false
            );
        }
    }

    vTaskDelete(nullptr);
}

// ================== API ==================

extern "C" void lynx_sound_init(int sample_rate)
{
    if (s_inited) return;

    if (sample_rate > 0) {
        lynx_sampleRate = sample_rate;
    }

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate       = lynx_sampleRate;
    cfg.stereo            = false;      // downmix to mono
    cfg.dma_buf_len       = 512;
    cfg.dma_buf_count     = 8;
    cfg.task_priority     = 4;
    cfg.task_pinned_core  = 0;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.setVolume(80);
    M5Cardputer.Speaker.stop(kChannel);

    // Ring buffer
    if (!s_ring) {
        s_ringSize  = kRingSamples;
        s_ring      = (int16_t*)malloc(s_ringSize * sizeof(int16_t));
        s_ringRead  = 0;
        s_ringWrite = 0;
        s_ringCount = 0;
    }

    s_running = true;

    if (!s_audioTask && s_ring) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            lynx_audio_task,
            "lynx_audio",
            4096,
            nullptr,
            6,     //  high priority
            &s_audioTask,
            0      // core
        );
        if (ok != pdPASS) {
            printf("[LYNX][AUDIO] task create failed\n");
            s_audioTask = nullptr;
            s_running   = false;
        }
    }

    s_inited = (s_ring != nullptr);
    printf("[LYNX][AUDIO] init: rate=%d mono, ring=%d\n",
           lynx_sampleRate, s_ringSize);
}

extern "C" void lynx_sound_set_volume(uint8_t vol)
{
    M5Cardputer.Speaker.setVolume(vol);
}

extern "C" void lynx_sound_shutdown(void)
{
    if (!s_inited) return;

    s_running = false;

    if (s_audioTask) {
        vTaskDelay(pdMS_TO_TICKS(10));
        s_audioTask = nullptr;
    }

    M5Cardputer.Speaker.stop(kChannel);

    if (s_ring) {
        free(s_ring);
        s_ring = nullptr;
    }
    s_ringSize  = 0;
    s_ringRead  = 0;
    s_ringWrite = 0;
    s_ringCount = 0;

    s_inited = false;
}

/**
 * Submit interleaved stereo (L,R,L,R,...).
 * Downmix to mono on the fly, with slight clamping to avoid saturation.
 */
extern "C" void lynx_sound_submit_frames_stereo(const int16_t* samples, size_t frames)
{
    if (!s_inited || !samples || frames == 0 || !s_ring) {
        return;
    }

    portENTER_CRITICAL(&s_ringMux);

    int freeSpace = s_ringSize - s_ringCount;
    int maxWrite  = freeSpace;
    int toWrite   = (frames <= (size_t)maxWrite) ? (int)frames : maxWrite;

    for (int i = 0; i < toWrite; ++i) {
        int32_t L = samples[i * 2 + 0];
        int32_t R = samples[i * 2 + 1];

        // mix simple L+R / 2
        int32_t mono = (L + R) / 2;

        if (mono > 32767)  mono = 32767;
        if (mono < -32768) mono = -32768;

        s_ring[s_ringWrite] = (int16_t)mono;
        s_ringWrite++;
        if (s_ringWrite >= s_ringSize) s_ringWrite = 0;
    }

    s_ringCount += toWrite;

    portEXIT_CRITICAL(&s_ringMux);
}
