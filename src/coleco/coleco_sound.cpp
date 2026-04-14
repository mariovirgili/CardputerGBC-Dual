#include "coleco_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp_heap_caps.h>

#include <cstring>

#ifndef COLECO_AUDIO_TRACE_ENABLED
#define COLECO_AUDIO_TRACE_ENABLED 0
#endif

namespace {

#if COLECO_AUDIO_ENABLED
constexpr int kChannel = 0;
constexpr size_t kMaxFrameSamples = 1024;
constexpr int kOutputGain = 2;
static ColecoAudioHookState s_audioState = {};
static int16_t s_mixBuffer[kMaxFrameSamples] = {};
static int16_t* s_playBuffers[2] = {nullptr, nullptr};
static uint8_t s_playFlip = 0u;

static bool coleco_sound_prepare_buffers(void)
{
    for (size_t i = 0; i < 2u; ++i) {
        if (s_playBuffers[i]) {
            continue;
        }

        s_playBuffers[i] = static_cast<int16_t*>(heap_caps_malloc(
            kMaxFrameSamples * sizeof(int16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        ));
        if (!s_playBuffers[i]) {
            s_playBuffers[i] = static_cast<int16_t*>(heap_caps_malloc(
                kMaxFrameSamples * sizeof(int16_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            ));
        }
        if (!s_playBuffers[i]) {
            return false;
        }
    }

    return true;
}

static void coleco_sound_release_buffers(void)
{
    for (size_t i = 0; i < 2u; ++i) {
        if (s_playBuffers[i]) {
            heap_caps_free(s_playBuffers[i]);
            s_playBuffers[i] = nullptr;
        }
    }
}

static void coleco_sound_reset_state(bool compiledIn)
{
    std::memset(&s_audioState, 0, sizeof(s_audioState));
    s_audioState.compiledIn = compiledIn;
}

static void coleco_sound_update_queue_depth(void)
{
    s_audioState.queuedBlocks = s_audioState.enabled
        ? static_cast<uint8_t>(M5Cardputer.Speaker.isPlaying(kChannel))
        : 0u;
}

static void coleco_sound_queue_block(const int16_t* samples, size_t sampleCount)
{
    if (!samples || sampleCount == 0u || !s_audioState.enabled || s_audioState.paused) {
        return;
    }

    (void)M5Cardputer.Speaker.playRaw(
        samples,
        sampleCount,
        s_audioState.sampleRate,
        false,
        1,
        kChannel,
        false
    );
    s_audioState.streamSeen = true;
}

static uint16_t coleco_sound_copy_with_gain(int16_t* dst, const int16_t* src, size_t count)
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

bool coleco_sound_init(uint32_t sampleRate, uint8_t channels)
{
#if !COLECO_AUDIO_ENABLED
    (void)sampleRate;
    (void)channels;
    return false;
#else
    coleco_sound_shutdown();
    coleco_sound_reset_state(true);

    if (sampleRate == 0 || channels == 0) {
        return false;
    }

    if (!coleco_sound_prepare_buffers()) {
        coleco_sound_shutdown();
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

    M5Cardputer.Speaker.setVolume(80);
    M5Cardputer.Speaker.stop(kChannel);
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));
    for (size_t i = 0; i < 2u; ++i) {
        std::memset(s_playBuffers[i], 0, kMaxFrameSamples * sizeof(int16_t));
    }
    s_playFlip = 0u;

    s_audioState.enabled = true;
    s_audioState.running = true;
    s_audioState.paused = false;
    s_audioState.queuedBlocks = 0u;
    return true;
#endif
}

void coleco_sound_shutdown(void)
{
#if !COLECO_AUDIO_ENABLED
    return;
#else
    if (!s_audioState.compiledIn && !s_audioState.enabled && !s_audioState.running) {
        return;
    }

    s_audioState.running = false;
    M5Cardputer.Speaker.stop(kChannel);
    std::memset(s_mixBuffer, 0, sizeof(s_mixBuffer));
    coleco_sound_release_buffers();
    s_playFlip = 0u;
    coleco_sound_reset_state(true);
#endif
}

int16_t* coleco_sound_begin_mix(size_t* capacity)
{
#if !COLECO_AUDIO_ENABLED
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

void coleco_sound_end_mix(size_t sampleCount)
{
#if !COLECO_AUDIO_ENABLED
    (void)sampleCount;
#else
    if (!s_audioState.enabled || sampleCount == 0) {
        return;
    }

    if (sampleCount > s_audioState.frameSamples) {
        sampleCount = s_audioState.frameSamples;
    }
    coleco_sound_submit(s_mixBuffer, sampleCount);
#endif
}

void coleco_sound_submit(const int16_t* samples, size_t sampleCount)
{
#if !COLECO_AUDIO_ENABLED
    (void)samples;
    (void)sampleCount;
#else
    if (!s_audioState.enabled || s_audioState.paused || !samples || sampleCount == 0) {
        return;
    }

    if (sampleCount > kMaxFrameSamples) {
        sampleCount = kMaxFrameSamples;
    }

    const size_t queued = M5Cardputer.Speaker.isPlaying(kChannel);
    if (queued >= 2u) {
        s_audioState.droppedFrames++;
        s_audioState.queuedBlocks = static_cast<uint8_t>(queued);

        static uint32_t s_lastDropLog = 0;
        if (millis() - s_lastDropLog > 1000) {
            std::printf("[MSX][AUDIO] WARNING: I2S buffer overrun! Frame dropped. (Total drops: %lu)\n", 
                        static_cast<unsigned long>(s_audioState.droppedFrames));
            s_lastDropLog = millis();
        }
        return;
    }

    auto copy_and_queue = [&](size_t count, size_t queuedBefore) {
        if (!s_playBuffers[s_playFlip]) {
            return;
        }

        const uint16_t peak = coleco_sound_copy_with_gain(s_playBuffers[s_playFlip], samples, count);
#if COLECO_AUDIO_TRACE_ENABLED
        static uint16_t s_audioPeakLogCount = 0u;
        static uint16_t s_lastLoggedPeak = 0xFFFFu;
        if (s_audioPeakLogCount < 64u &&
            (peak != s_lastLoggedPeak || peak == 0u || queuedBefore == 0u)) {
            std::printf("[MSX][AUDIO] submit n=%u peak=%u queued=%u gain=%u #%u\n",
                        static_cast<unsigned>(count),
                        static_cast<unsigned>(peak),
                        static_cast<unsigned>(queuedBefore),
                        static_cast<unsigned>(kOutputGain),
                        static_cast<unsigned>(s_audioPeakLogCount));
            s_lastLoggedPeak = peak;
            ++s_audioPeakLogCount;
        }
#else
        (void)peak;
        (void)queuedBefore;
#endif

        coleco_sound_queue_block(s_playBuffers[s_playFlip], count);
        s_playFlip ^= 0x01u;
        s_audioState.submittedFrames++;
    };

    // Prime the hardware queue only on a fresh stream start. Repeating the same
    // block on every underrun makes audio sound artificially slowed down.
    const bool needInitialPrime = (queued == 0u) && !s_audioState.streamSeen;
    copy_and_queue(sampleCount, queued);
    if (needInitialPrime) {
        copy_and_queue(sampleCount, 1u);
    }

    coleco_sound_update_queue_depth();
#endif
}

void coleco_sound_set_paused(bool paused)
{
#if !COLECO_AUDIO_ENABLED
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
    s_audioState.queuedBlocks = 0u;
    s_audioState.streamSeen = false;
#endif
}

const ColecoAudioHookState& coleco_sound_get_state(void)
{
#if !COLECO_AUDIO_ENABLED
    static ColecoAudioHookState s_disabledState = {0, 0, 0, 0, 0, 0, false, false, false, false};
    return s_disabledState;
#else
    return s_audioState;
#endif
}
