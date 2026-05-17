#pragma GCC optimize ("Os")

#include "a2600_sound.h"

#include <M5Cardputer.h>
#include <string.h>

#include "cardputer/CardputerAudio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/emu_log_cpp.h"

static constexpr int kChannel = 0;
static constexpr int kChunkSamples = 512;
static constexpr int kRingSamples = 4096;

// ================== ETAT AUDIO ==================

static bool s_inited = false;
static volatile bool s_running = false;
static int s_sampleRate = 31400;

static TaskHandle_t s_audioTask = nullptr;
static int16_t* s_ring = nullptr;
static int16_t* s_playBuf[cardputer_audio::kRuntimeAudioBufferCount] = { nullptr, nullptr, nullptr };
static uint8_t s_playSlot = 0;
static int s_ringSize = 0;
static volatile int s_ringRead = 0;
static volatile int s_ringWrite = 0;
static volatile int s_ringCount = 0;

static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// ================== HELPERS ==================

static int a2600_ring_pop(int16_t* dst, int maxSamples)
{
    if (!dst || maxSamples <= 0 || !s_ring) {
        return 0;
    }

    int toRead = 0;

    portENTER_CRITICAL(&s_ringMux);
    if (s_ringCount > 0) {
        toRead = (s_ringCount < maxSamples) ? s_ringCount : maxSamples;

        for (int i = 0; i < toRead; ++i) {
            dst[i] = s_ring[s_ringRead];
            s_ringRead = s_ringRead + 1;
            if (s_ringRead >= s_ringSize) {
                s_ringRead = 0;
            }
        }

        s_ringCount -= toRead;
    }
    portEXIT_CRITICAL(&s_ringMux);

    return toRead;
}

// ================== TASK AUDIO ==================

static void a2600_audio_task(void* arg)
{
    (void)arg;

    while (s_running) {
        if (s_ringCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (M5Cardputer.Speaker.isPlaying(kChannel) >= 2 ||
            !s_playBuf[0] || !s_playBuf[1] || !s_playBuf[2]) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int16_t* local = s_playBuf[s_playSlot];
        const int n = a2600_ring_pop(local, kChunkSamples);
        if (n > 0) {
            cardputer_audio::queueRuntimeAudioBuffer(
                s_playBuf, s_playSlot, (size_t)n, (uint32_t)s_sampleRate, false, kChannel);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    s_audioTask = nullptr;
    vTaskDelete(nullptr);
}

// ================== API ==================

void a2600_sound_init(int sampleRate)
{
    if (s_inited) {
        return;
    }

    if (sampleRate > 0) {
        s_sampleRate = sampleRate;
    }

    cardputer_audio::beginSpeaker(s_sampleRate, false, 512, 8, 80, "a2600", 4, 0);
    M5Cardputer.Speaker.stop(kChannel);
    cardputer_audio::allocRuntimeAudioBuffers(s_playBuf, kChunkSamples, "a2600");
    s_playSlot = 0;

    s_ringSize = kRingSamples;
    s_ring = (int16_t*)malloc((size_t)s_ringSize * sizeof(int16_t));
    if (!s_ring) {
        EMU_LOG("[A2600][AUDIO] ring alloc failed\n");
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
        EMU_LOG("[A2600][AUDIO] task create failed\n");
        s_audioTask = nullptr;
        s_running = false;
        free(s_ring);
        s_ring = nullptr;
        s_ringSize = 0;
        return;
    }

    s_inited = true;
    EMU_LOG("[A2600][AUDIO] init ok, rate=%d, ring=%d\n", s_sampleRate, s_ringSize);
}

void a2600_sound_shutdown(void)
{
    if (!s_inited) {
        return;
    }

    s_running = false;

    for (int i = 0; i < 20 && s_audioTask != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    M5Cardputer.Speaker.stop(kChannel);
    cardputer_audio::freeRuntimeAudioBuffers(s_playBuf);

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
    const int toWrite = ((int)frames < freeSpace) ? (int)frames : freeSpace;

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
        s_ringWrite = s_ringWrite + 1;
        if (s_ringWrite >= s_ringSize) {
            s_ringWrite = 0;
        }
    }

    s_ringCount += toWrite;

    portEXIT_CRITICAL(&s_ringMux);
}
