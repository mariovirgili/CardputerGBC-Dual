// gbc_sound.cpp
#include "gbc_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int kChannel = 0;

// ================== ETAT AUDIO ==================

static bool   s_inited     = false;
int    gbc_sampleRate = 44100;

static TaskHandle_t s_audioTask  = nullptr;
static volatile bool s_running   = false;

static constexpr int kRingSamples = 2048;
static int16_t* s_ring            = nullptr;
static int      s_ringSize        = 0;
static int      s_ringRead        = 0;
static int      s_ringWrite       = 0;
static int      s_ringCount       = 0;

// protection ring
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// ================== TASK AUDIO ==================

static void gbc_audio_task(void* arg)
{
    static const int kChunk = 512;
    int16_t local[kChunk];

    while (s_running) {
        if (s_ringCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Lit un chunk
        int toRead = 0;
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
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int queued = M5Cardputer.Speaker.isPlaying(kChannel);

        if (queued == 0) {
            // Primer chunk

            // Envoi premier
            M5Cardputer.Speaker.playRaw(
                local,
                (size_t)toRead,
                (uint32_t)gbc_sampleRate,
                false, 1, kChannel, false
            );

            // Envoi deuxieme
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
                    (uint32_t)gbc_sampleRate,
                    false, 1, kChannel, false
                );
            }
        }
        else if (queued == 1) {
            // un seul chunk
            M5Cardputer.Speaker.playRaw(
                local,
                (size_t)toRead,
                (uint32_t)gbc_sampleRate,
                false, 1, kChannel, false
            );
        }
        else {
            // queued >= 2
        }
    }

    vTaskDelete(nullptr);
}


// ================== API ==================

extern "C" void gbc_sound_init(int sample_rate)
{
    if (s_inited) return;

    if (sample_rate > 0) {
        gbc_sampleRate = sample_rate;
    }

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate       = gbc_sampleRate;
    cfg.stereo            = false;
    cfg.dma_buf_len       = 512;
    cfg.dma_buf_count     = 8;
    cfg.task_priority     = 4;
    cfg.task_pinned_core  = 1;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.setVolume(60);
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
            gbc_audio_task,
            "gbc_audio",
            4096,
            nullptr,
            6,     // priority
            &s_audioTask,
            0      // core 
        );
        if (ok != pdPASS) {
            printf("[GBC][AUDIO] task create failed\n");
            s_audioTask = nullptr;
            s_running   = false;
        }
    }

    s_inited = (s_ring != nullptr);
    printf("[GBC][AUDIO] init: rate=%d mono, ring=%d\n",
           gbc_sampleRate, s_ringSize);
}

extern "C" void gbc_sound_set_volume(uint8_t vol)
{
    M5Cardputer.Speaker.setVolume(vol);
}

extern "C" void gbc_sound_shutdown(void)
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

// for the callback GNUBOY
extern "C" void gbc_sound_submit(const int16_t* samples, size_t sample_count)
{
    if (!s_inited || !samples || sample_count == 0 || !s_ring) {
        return;
    }

    portENTER_CRITICAL(&s_ringMux);

    int freeSpace = s_ringSize - s_ringCount;
    int toWrite   = (sample_count <= (size_t)freeSpace)
                    ? (int)sample_count
                    : freeSpace;

    for (int i = 0; i < toWrite; ++i) {
        s_ring[s_ringWrite] = samples[i];
        s_ringWrite++;
        if (s_ringWrite >= s_ringSize) s_ringWrite = 0;
    }
    s_ringCount += toWrite;

    portEXIT_CRITICAL(&s_ringMux);
}
