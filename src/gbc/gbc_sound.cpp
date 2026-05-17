// gbc_sound.cpp
#include "gbc_sound.h"

#include "compat/arduino_compat.h"
#include <M5Cardputer.h>
#include <string.h>
#include "cardputer/CardputerAudio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/emu_log_cpp.h"

static constexpr int kChannel = 0;

// ================== ETAT AUDIO ==================

static bool   s_inited     = false;
int    gbc_sampleRate = 44100;

static TaskHandle_t s_audioTask  = nullptr;
static volatile bool s_running   = false;

static constexpr int kRingSamples = 2048;
static constexpr int kPlayChunk = 512;
static int16_t* s_ring            = nullptr;
static int16_t* s_playBuf[cardputer_audio::kRuntimeAudioBufferCount] = { nullptr, nullptr, nullptr };
static uint8_t  s_playSlot        = 0;
static int      s_ringSize        = 0;
static int      s_ringRead        = 0;
static int      s_ringWrite       = 0;
static int      s_ringCount       = 0;

// protection ring
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// ================== TASK AUDIO ==================

static void gbc_audio_task(void* arg)
{
    while (s_running) {
        if (s_ringCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (M5Cardputer.Speaker.isPlaying(kChannel) >= 2) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (!s_playBuf[0] || !s_playBuf[1] || !s_playBuf[2]) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int16_t* local = s_playBuf[s_playSlot];

        int toRead = 0;
        portENTER_CRITICAL(&s_ringMux);
        if (s_ringCount > 0) {
            toRead = (s_ringCount < kPlayChunk) ? s_ringCount : kPlayChunk;

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

        if (cardputer_audio::queueRuntimeAudioBuffer(
                s_playBuf, s_playSlot, (size_t)toRead, (uint32_t)gbc_sampleRate, false, kChannel)) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
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

    cardputer_audio::beginSpeaker(gbc_sampleRate, false, 512, 8, 60, "gbc", 4, 1);
    M5Cardputer.Speaker.stop(kChannel);
    cardputer_audio::allocRuntimeAudioBuffers(s_playBuf, kPlayChunk, "gbc");
    s_playSlot = 0;

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
            EMU_LOG("[GBC][AUDIO] task create failed\n");
            s_audioTask = nullptr;
            s_running   = false;
        }
    }

    s_inited = (s_ring != nullptr);
    EMU_LOG("[GBC][AUDIO] init: rate=%d mono, ring=%d\n",
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
    cardputer_audio::freeRuntimeAudioBuffers(s_playBuf);

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
