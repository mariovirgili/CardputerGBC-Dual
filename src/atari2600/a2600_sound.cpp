#include "a2600_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int kChannel = 0;
static constexpr int kChunkSamples = 512;
static constexpr int kRingSamples = 4096;

static bool s_inited = false;
static volatile bool s_running = false;
static int s_sampleRate = 31400;

static TaskHandle_t s_audioTask = nullptr;
static int16_t* s_ring = nullptr;
static int s_ringSize = 0;
static volatile int s_ringRead = 0;
static volatile int s_ringWrite = 0;
static volatile int s_ringCount = 0;

static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

static void a2600_audio_task(void* arg)
{
    (void)arg;

    int16_t local[kChunkSamples];

    while (s_running) {
        if (s_ringCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }

        int toRead = 0;

        portENTER_CRITICAL(&s_ringMux);
        if (s_ringCount > 0) {
            toRead = (s_ringCount < kChunkSamples) ? s_ringCount : kChunkSamples;
            for (int i = 0; i < toRead; ++i) {
                local[i] = s_ring[s_ringRead];
                s_ringRead++;
                if (s_ringRead >= s_ringSize) {
                    s_ringRead = 0;
                }
            }
            s_ringCount -= toRead;
        }
        portEXIT_CRITICAL(&s_ringMux);

        if (toRead <= 0) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }

        M5Cardputer.Speaker.playRaw(
            local,
            (size_t)toRead,
            (uint32_t)s_sampleRate,
            false,
            1,
            kChannel,
            false
        );

        if (M5Cardputer.Speaker.isPlaying(kChannel) >= 2) {
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    }

    vTaskDelete(nullptr);
}

void a2600_sound_init(int sampleRate)
{
    if (s_inited) {
        return;
    }

    if (sampleRate > 0) {
        s_sampleRate = sampleRate;
    }

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate = s_sampleRate;
    cfg.stereo = false;
    cfg.dma_buf_len = 512;
    cfg.dma_buf_count = 8;
    cfg.task_priority = 4;
    cfg.task_pinned_core = 0;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.setVolume(80);
    M5Cardputer.Speaker.stop(kChannel);

    s_ringSize = kRingSamples;
    s_ring = (int16_t*)malloc((size_t)s_ringSize * sizeof(int16_t));
    if (!s_ring) {
        printf("[A2600][AUDIO] ring alloc failed\n");
        return;
    }
    memset(s_ring, 0, (size_t)s_ringSize * sizeof(int16_t));
    s_ringRead = 0;
    s_ringWrite = 0;
    s_ringCount = 0;

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        a2600_audio_task,
        "a2600_audio",
        4096,
        nullptr,
        6,
        &s_audioTask,
        0
    );

    if (ok != pdPASS) {
        printf("[A2600][AUDIO] task create failed\n");
        s_audioTask = nullptr;
        s_running = false;
        free(s_ring);
        s_ring = nullptr;
        return;
    }

    s_inited = true;
    printf("[A2600][AUDIO] init ok, rate=%d\n", s_sampleRate);
}

void a2600_sound_shutdown(void)
{
    if (!s_inited) {
        return;
    }

    s_running = false;
    s_audioTask = nullptr;

    M5Cardputer.Speaker.stop(kChannel);

    free(s_ring);
    s_ring = nullptr;
    s_ringSize = 0;
    s_ringRead = 0;
    s_ringWrite = 0;
    s_ringCount = 0;
    s_inited = false;
}

void a2600_sound_submit_stereo(const int16_t* samples, size_t frames)
{
    if (!s_inited || !samples || frames == 0 || !s_ring) {
        return;
    }

    portENTER_CRITICAL(&s_ringMux);

    const int freeSpace = s_ringSize - s_ringCount;
    const int toWrite = (frames < (size_t)freeSpace) ? (int)frames : freeSpace;

    for (int i = 0; i < toWrite; ++i) {
        const int32_t left = samples[i * 2 + 0];
        const int32_t right = samples[i * 2 + 1];
        int32_t mono = (left + right) / 2;

        if (mono > 32767) {
            mono = 32767;
        } else if (mono < -32768) {
            mono = -32768;
        }

        s_ring[s_ringWrite] = (int16_t)mono;
        s_ringWrite++;
        if (s_ringWrite >= s_ringSize) {
            s_ringWrite = 0;
        }
    }

    s_ringCount += toWrite;

    portEXIT_CRITICAL(&s_ringMux);
}
