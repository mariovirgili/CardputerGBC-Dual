#include "msx_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <freertos/queue.h>
#include <freertos/task.h>

#ifndef MSX_AUDIO_TRACE_ENABLED
#define MSX_AUDIO_TRACE_ENABLED 0
#endif

namespace {

#if MSX_AUDIO_ENABLED
constexpr int kChannel = 0;
constexpr size_t kMaxFrameSamples = 512;
constexpr int kOutputGain = 2;
constexpr size_t kNumPlayBuffers = 4;
constexpr TickType_t kAudioWorkerPollTicks = pdMS_TO_TICKS(20);
static MsxAudioHookState s_audioState = {};
static int16_t s_mixBuffer[kMaxFrameSamples] = {};
static int16_t* s_playBuffers[kNumPlayBuffers] = {nullptr, nullptr, nullptr, nullptr};
static QueueHandle_t s_freeBufferQueue = nullptr;
static QueueHandle_t s_readyBlockQueue = nullptr;
static TaskHandle_t s_audioTask = nullptr;
static volatile bool s_audioTaskRunning = false;

struct MsxAudioBlock {
    uint8_t bufferIndex;
    uint16_t sampleCount;
};

static bool msx_sound_prepare_buffers(void)
{
    for (size_t i = 0; i < kNumPlayBuffers; ++i) {
        if (s_playBuffers[i]) {
            continue;
        }

        s_playBuffers[i] = static_cast<int16_t*>(heap_caps_malloc(
            kMaxFrameSamples * sizeof(int16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        ));
        if (!s_playBuffers[i]) {
            s_playBuffers[i] = static_cast<int16_t*>(heap_caps_malloc(
                kMaxFrameSamples * sizeof(int16_t),
                MALLOC_CAP_8BIT
            ));
        }
        if (!s_playBuffers[i]) {
            s_playBuffers[i] = static_cast<int16_t*>(heap_caps_malloc(
                kMaxFrameSamples * sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            ));
        }
        if (!s_playBuffers[i]) {
            return false;
        }
    }

    return true;
}

static void msx_sound_release_buffers(void)
{
    for (size_t i = 0; i < kNumPlayBuffers; ++i) {
        if (s_playBuffers[i]) {
            heap_caps_free(s_playBuffers[i]);
            s_playBuffers[i] = nullptr;
        }
    }
}

static void msx_sound_release_queues(void)
{
    if (s_freeBufferQueue) {
        vQueueDelete(s_freeBufferQueue);
        s_freeBufferQueue = nullptr;
    }
    if (s_readyBlockQueue) {
        vQueueDelete(s_readyBlockQueue);
        s_readyBlockQueue = nullptr;
    }
}

static bool msx_sound_prepare_queues(void)
{
    if (!s_freeBufferQueue) {
        s_freeBufferQueue = xQueueCreate(kNumPlayBuffers, sizeof(uint8_t));
    }
    if (!s_readyBlockQueue) {
        s_readyBlockQueue = xQueueCreate(kNumPlayBuffers, sizeof(MsxAudioBlock));
    }
    if (!s_freeBufferQueue || !s_readyBlockQueue) {
        return false;
    }

    xQueueReset(s_freeBufferQueue);
    xQueueReset(s_readyBlockQueue);
    for (uint8_t index = 0u; index < static_cast<uint8_t>(kNumPlayBuffers); ++index) {
        (void)xQueueSend(s_freeBufferQueue, &index, 0);
    }

    return true;
}

static void msx_sound_reset_state(bool compiledIn)
{
    std::memset(&s_audioState, 0, sizeof(s_audioState));
    s_audioState.compiledIn = compiledIn;
}

static void msx_sound_update_queue_depth(void)
{
    if (!s_audioState.enabled) {
        s_audioState.queuedBlocks = 0u;
        return;
    }

    const size_t hardwareQueued = M5Cardputer.Speaker.isPlaying(kChannel);
    const UBaseType_t pendingQueued = s_readyBlockQueue ? uxQueueMessagesWaiting(s_readyBlockQueue) : 0u;
    size_t totalQueued = hardwareQueued + static_cast<size_t>(pendingQueued);
    if (totalQueued > 255u) {
        totalQueued = 255u;
    }
    s_audioState.queuedBlocks = static_cast<uint8_t>(totalQueued);
}

static void msx_sound_release_block(const MsxAudioBlock& block)
{
    if (!s_freeBufferQueue || block.bufferIndex >= kNumPlayBuffers) {
        return;
    }

    uint8_t bufferIndex = block.bufferIndex;
    (void)xQueueSend(s_freeBufferQueue, &bufferIndex, 0);
}

static void msx_sound_flush_pending_blocks(void)
{
    if (!s_readyBlockQueue) {
        return;
    }

    MsxAudioBlock block = {};
    while (xQueueReceive(s_readyBlockQueue, &block, 0) == pdTRUE) {
        msx_sound_release_block(block);
    }
    msx_sound_update_queue_depth();
}

static void msx_sound_queue_to_speaker(const MsxAudioBlock& block)
{
    if (block.bufferIndex >= kNumPlayBuffers ||
        !s_playBuffers[block.bufferIndex] ||
        block.sampleCount == 0u ||
        !s_audioState.enabled ||
        s_audioState.paused) {
        return;
    }

    (void)M5Cardputer.Speaker.playRaw(
        s_playBuffers[block.bufferIndex],
        block.sampleCount,
        s_audioState.sampleRate,
        false,
        1,
        kChannel,
        false
    );
    s_audioState.streamSeen = true;
}

static void msx_sound_audio_task(void* arg)
{
    (void)arg;

    while (s_audioTaskRunning) {
        MsxAudioBlock block = {};
        if (!s_readyBlockQueue ||
            xQueueReceive(s_readyBlockQueue, &block, kAudioWorkerPollTicks) != pdTRUE) {
            msx_sound_update_queue_depth();
            continue;
        }

        if (block.bufferIndex >= kNumPlayBuffers || block.sampleCount == 0u) {
            continue;
        }

        size_t queuedBefore = 0u;
        while (s_audioTaskRunning && s_audioState.enabled && !s_audioState.paused) {
            queuedBefore = M5Cardputer.Speaker.isPlaying(kChannel);
            if (queuedBefore < 2u) {
                break;
            }
            msx_sound_update_queue_depth();
            vTaskDelay(1);
        }

        if (!s_audioTaskRunning || !s_audioState.enabled || s_audioState.paused) {
            msx_sound_release_block(block);
            continue;
        }

        const bool needInitialPrime = (queuedBefore == 0u) && !s_audioState.streamSeen;
        msx_sound_queue_to_speaker(block);
        s_audioState.submittedFrames++;

        if (needInitialPrime) {
            MsxAudioBlock primeBlock = {};
            if (s_readyBlockQueue && xQueueReceive(s_readyBlockQueue, &primeBlock, 0) == pdTRUE) {
                if (!s_audioState.paused && s_audioState.enabled && s_audioTaskRunning) {
                    msx_sound_queue_to_speaker(primeBlock);
                    s_audioState.submittedFrames++;
                }
                msx_sound_release_block(primeBlock);
            } else {
                msx_sound_queue_to_speaker(block);
                s_audioState.submittedFrames++;
            }
        }

        msx_sound_release_block(block);
        msx_sound_update_queue_depth();
    }

    s_audioTask = nullptr;
    vTaskDelete(nullptr);
}

static bool msx_sound_start_worker(void)
{
    if (s_audioTask) {
        return true;
    }

    s_audioTaskRunning = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(
        msx_sound_audio_task,
        "msx_audio",
        4096,
        nullptr,
        6,
        &s_audioTask,
        0
    );
    if (ok == pdPASS) {
        return true;
    }

    s_audioTaskRunning = false;
    s_audioTask = nullptr;
    return false;
}

static void msx_sound_stop_worker(void)
{
    s_audioTaskRunning = false;

    if (s_readyBlockQueue) {
        const MsxAudioBlock wakeBlock = {0xFFu, 0u};
        (void)xQueueSend(s_readyBlockQueue, &wakeBlock, 0);
    }

    if (!s_audioTask) {
        return;
    }

    for (int i = 0; i < 25 && s_audioTask; ++i) {
        vTaskDelay(1);
    }

    if (s_audioTask) {
        vTaskDelete(s_audioTask);
        s_audioTask = nullptr;
    }
}

static uint16_t msx_sound_copy_with_gain(int16_t* dst, const int16_t* src, size_t count)
{
    if (!dst || !src || count == 0u) {
        return 0u;
    }

    uint16_t peak = 0u;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = static_cast<int32_t>(src[i]) * kOutputGain;
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        dst[i] = static_cast<int16_t>(sample);
        const uint16_t magnitude = static_cast<uint16_t>(sample < 0 ? -sample : sample);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    return peak;
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

    if (!msx_sound_prepare_buffers()) {
        msx_sound_shutdown();
        return false;
    }
    if (!msx_sound_prepare_queues()) {
        msx_sound_shutdown();
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
    cfg.dma_buf_len = 128;
    cfg.dma_buf_count = 4;
    cfg.task_priority = 4;
    cfg.task_pinned_core = 0;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }
    if (!msx_sound_start_worker()) {
        msx_sound_shutdown();
        return false;
    }

    M5Cardputer.Speaker.setVolume(80);
    M5Cardputer.Speaker.stop(kChannel);
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));
    for (size_t i = 0; i < kNumPlayBuffers; ++i) {
        std::memset(s_playBuffers[i], 0, kMaxFrameSamples * sizeof(int16_t));
    }

    s_audioState.enabled = true;
    s_audioState.running = true;
    s_audioState.paused = false;
    s_audioState.queuedBlocks = 0u;
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
    s_audioTaskRunning = false;
    M5Cardputer.Speaker.stop(kChannel);
    msx_sound_stop_worker();
    msx_sound_flush_pending_blocks();
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));
    msx_sound_release_queues();
    msx_sound_release_buffers();
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
    if (!s_audioState.enabled || s_audioState.paused) {
        if (capacity) {
            *capacity = 0u;
        }
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
    if (!s_audioState.enabled || s_audioState.paused || !samples || sampleCount == 0 ||
        !s_freeBufferQueue || !s_readyBlockQueue) {
        return;
    }

    if (sampleCount > kMaxFrameSamples) {
        sampleCount = kMaxFrameSamples;
    }

    uint8_t bufferIndex = 0xFFu;
    if (xQueueReceive(s_freeBufferQueue, &bufferIndex, 0) != pdTRUE ||
        bufferIndex >= kNumPlayBuffers ||
        !s_playBuffers[bufferIndex]) {
        s_audioState.droppedFrames++;
        msx_sound_update_queue_depth();

        static uint32_t s_lastDropLog = 0;
        if (millis() - s_lastDropLog > 5000) {
            std::printf("[MSX][AUDIO] WARNING: worker queue full, frame dropped. (Total drops: %lu)\n",
                        static_cast<unsigned long>(s_audioState.droppedFrames));
            s_lastDropLog = millis();
        }
        return;
    }

    const uint16_t peak = msx_sound_copy_with_gain(s_playBuffers[bufferIndex], samples, sampleCount);
#if MSX_AUDIO_TRACE_ENABLED
    static uint16_t s_audioPeakLogCount = 0u;
    static uint16_t s_lastLoggedPeak = 0xFFFFu;
    const uint8_t queuedBefore = s_audioState.queuedBlocks;
    if (s_audioPeakLogCount < 64u &&
        (peak != s_lastLoggedPeak || peak == 0u || queuedBefore == 0u)) {
        std::printf("[MSX][AUDIO] submit n=%u peak=%u queued=%u gain=%u #%u\n",
                    static_cast<unsigned>(sampleCount),
                    static_cast<unsigned>(peak),
                    static_cast<unsigned>(queuedBefore),
                    static_cast<unsigned>(kOutputGain),
                    static_cast<unsigned>(s_audioPeakLogCount));
        s_lastLoggedPeak = peak;
        ++s_audioPeakLogCount;
    }
#else
    (void)peak;
#endif

    const MsxAudioBlock block = {bufferIndex, static_cast<uint16_t>(sampleCount)};
    if (xQueueSend(s_readyBlockQueue, &block, 0) != pdTRUE) {
        msx_sound_release_block(block);
        s_audioState.droppedFrames++;
        msx_sound_update_queue_depth();
        return;
    }

    msx_sound_update_queue_depth();
#endif
}

void msx_sound_set_paused(bool paused)
{
#if !MSX_AUDIO_ENABLED
    (void)paused;
#else
    if (!s_audioState.compiledIn || !s_audioState.enabled) {
        return;
    }

    if (s_audioState.paused == paused) {
        return;
    }

    s_audioState.paused = paused;
    if (!paused) {
        s_audioState.queuedBlocks = 0u;
        s_audioState.streamSeen = false;
        return;
    }

    M5Cardputer.Speaker.stop(kChannel);
    msx_sound_flush_pending_blocks();
    s_audioState.queuedBlocks = 0u;
    s_audioState.streamSeen = false;
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
