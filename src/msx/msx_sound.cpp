#include "msx_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include <cstring>

#if MSX_AUDIO_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {

#if MSX_AUDIO_ENABLED
constexpr int kChannel = 3;
constexpr size_t kMaxFrameSamples = 1024;
constexpr size_t kQueueBlocks = 4;
constexpr TickType_t kIdleDelayTicks = pdMS_TO_TICKS(2);

struct MsxAudioQueue {
    int16_t blocks[kQueueBlocks][kMaxFrameSamples];
    uint16_t sizes[kQueueBlocks];
    uint8_t readIndex;
    uint8_t writeIndex;
    uint8_t count;
};

static MsxAudioHookState s_audioState = {};
static MsxAudioQueue s_audioQueue = {};
static int16_t s_mixBuffer[kMaxFrameSamples] = {};
static TaskHandle_t s_audioTask = nullptr;
static portMUX_TYPE s_audioMux = portMUX_INITIALIZER_UNLOCKED;

static void msx_audio_task(void* arg)
{
    (void)arg;

    int16_t local[kMaxFrameSamples];

    while (s_audioState.running) {
        uint16_t sampleCount = 0;

        portENTER_CRITICAL(&s_audioMux);
        if (s_audioQueue.count > 0) {
            const uint8_t index = s_audioQueue.readIndex;
            sampleCount = s_audioQueue.sizes[index];
            if (sampleCount > kMaxFrameSamples) {
                sampleCount = static_cast<uint16_t>(kMaxFrameSamples);
            }
            if (sampleCount > 0) {
                std::memcpy(local, s_audioQueue.blocks[index], static_cast<size_t>(sampleCount) * sizeof(int16_t));
            }
            s_audioQueue.readIndex = static_cast<uint8_t>((s_audioQueue.readIndex + 1u) % kQueueBlocks);
            s_audioQueue.count--;
            s_audioState.queuedBlocks = s_audioQueue.count;
        }
        portEXIT_CRITICAL(&s_audioMux);

        if (sampleCount == 0) {
            vTaskDelay(kIdleDelayTicks);
            continue;
        }

        while (s_audioState.running && M5Cardputer.Speaker.isPlaying(kChannel) >= 2) {
            vTaskDelay(kIdleDelayTicks);
        }

        if (!s_audioState.running) {
            break;
        }

        (void)M5Cardputer.Speaker.playRaw(
            local,
            static_cast<size_t>(sampleCount),
            s_audioState.sampleRate,
            false,
            1,
            kChannel,
            false
        );
        s_audioState.streamSeen = true;
    }

    s_audioTask = nullptr;
    vTaskDelete(nullptr);
}

static void msx_sound_reset_state(bool compiledIn)
{
    std::memset(&s_audioState, 0, sizeof(s_audioState));
    s_audioState.compiledIn = compiledIn;
}
#endif

} // namespace

bool msx_sound_init(uint32_t sampleRate, uint8_t channels)
{
#if !MSX_AUDIO_ENABLED
    (void)sampleRate;
    (void)channels;
    return false;
#else
    msx_sound_shutdown();
    msx_sound_reset_state(true);

    if (sampleRate == 0 || channels == 0) {
        return false;
    }

    s_audioState.sampleRate = sampleRate;
    s_audioState.channels = channels;
    s_audioState.frameSamples = static_cast<uint16_t>((sampleRate + 30u) / 60u);
    if (s_audioState.frameSamples == 0 || s_audioState.frameSamples > kMaxFrameSamples) {
        s_audioState.frameSamples = static_cast<uint16_t>(kMaxFrameSamples);
    }

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate = sampleRate;
    cfg.stereo = false;
    cfg.dma_buf_len = 256;
    cfg.dma_buf_count = 6;
    cfg.task_priority = 4;
    cfg.task_pinned_core = 0;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.stop(kChannel);
    std::memset(&s_audioQueue, 0, sizeof(s_audioQueue));
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));

    s_audioState.enabled = true;
    s_audioState.running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        msx_audio_task,
        "msx_audio",
        3072,
        nullptr,
        5,
        &s_audioTask,
        0
    );

    if (ok != pdPASS) {
        s_audioState.enabled = false;
        s_audioState.running = false;
        s_audioTask = nullptr;
        return false;
    }

    return true;
#endif
}

void msx_sound_shutdown(void)
{
#if !MSX_AUDIO_ENABLED
    return;
#else
    if (!s_audioState.compiledIn && !s_audioState.enabled && !s_audioState.running) {
        return;
    }

    s_audioState.running = false;
    if (s_audioTask != nullptr) {
        vTaskDelay(kIdleDelayTicks);
    }

    M5Cardputer.Speaker.stop(kChannel);
    std::memset(&s_audioQueue, 0, sizeof(s_audioQueue));
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));
    msx_sound_reset_state(true);
#endif
}

int16_t* msx_sound_begin_mix(size_t* capacity)
{
#if !MSX_AUDIO_ENABLED
    if (capacity) {
        *capacity = 0;
    }
    return nullptr;
#else
    if (capacity) {
        *capacity = s_audioState.enabled ? s_audioState.frameSamples : 0u;
    }
    if (!s_audioState.enabled) {
        return nullptr;
    }

    const size_t clearCount = s_audioState.frameSamples > 0 ? s_audioState.frameSamples : 0u;
    if (clearCount > 0) {
        std::memset(s_mixBuffer, 0, clearCount * sizeof(int16_t));
    }
    return s_mixBuffer;
#endif
}

void msx_sound_end_mix(size_t sampleCount)
{
#if !MSX_AUDIO_ENABLED
    (void)sampleCount;
#else
    if (!s_audioState.enabled || sampleCount == 0) {
        return;
    }

    if (sampleCount > s_audioState.frameSamples) {
        sampleCount = s_audioState.frameSamples;
    }
    msx_sound_submit(s_mixBuffer, sampleCount);
#endif
}

void msx_sound_submit(const int16_t* samples, size_t sampleCount)
{
#if !MSX_AUDIO_ENABLED
    (void)samples;
    (void)sampleCount;
#else
    if (!s_audioState.enabled || !samples || sampleCount == 0) {
        return;
    }

    if (sampleCount > kMaxFrameSamples) {
        sampleCount = kMaxFrameSamples;
    }

    portENTER_CRITICAL(&s_audioMux);
    if (s_audioQueue.count >= kQueueBlocks) {
        s_audioState.droppedFrames++;
        portEXIT_CRITICAL(&s_audioMux);
        return;
    }

    const uint8_t index = s_audioQueue.writeIndex;
    std::memcpy(s_audioQueue.blocks[index], samples, sampleCount * sizeof(int16_t));
    s_audioQueue.sizes[index] = static_cast<uint16_t>(sampleCount);
    s_audioQueue.writeIndex = static_cast<uint8_t>((s_audioQueue.writeIndex + 1u) % kQueueBlocks);
    s_audioQueue.count++;
    s_audioState.queuedBlocks = s_audioQueue.count;
    s_audioState.submittedFrames++;
    portEXIT_CRITICAL(&s_audioMux);
#endif
}

const MsxAudioHookState& msx_sound_get_state(void)
{
#if !MSX_AUDIO_ENABLED
    static MsxAudioHookState s_disabledState = {0, 0, 0, 0, 0, 0, false, false, false, false};
    return s_disabledState;
#else
    return s_audioState;
#endif
}


