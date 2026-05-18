#pragma once

#include <M5Cardputer.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/boot_log.h"

namespace cardputer_audio {

static constexpr int kSpeakerBck = 41;
static constexpr int kSpeakerWs = 43;
static constexpr int kSpeakerData = 42;
static constexpr size_t kRuntimeAudioBufferCount = 4;
static constexpr size_t kRuntimeAudioQueueDepth = 3;
static constexpr uint32_t kAudioDiagLogPeriodMs = 2000;

#if EMU_AUDIO_LOGS_ENABLED
struct AudioDiagStats {
    const char* reason = "?";
    uint32_t sampleRate = 0;
    bool stereo = false;
    uint64_t samplesTotal = 0;
    uint32_t attempts = 0;
    uint32_t queued = 0;
    uint32_t queueFull = 0;
    uint32_t queueEmpty = 0;
    uint32_t playFail = 0;
    uint32_t invalid = 0;
    uint32_t minSamples = UINT32_MAX;
    uint32_t maxSamples = 0;
    uint32_t maxDepth = 0;
    int channel = 0;
    uint64_t lastLogMs = 0;
};

inline AudioDiagStats& diagStats()
{
    static AudioDiagStats stats;
    return stats;
}

inline uint64_t nowMs()
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

inline void resetDiagStats(const char* reason, uint32_t sampleRate, bool stereo, int channel)
{
    AudioDiagStats& s = diagStats();
    s.reason = reason ? reason : "?";
    s.sampleRate = sampleRate;
    s.stereo = stereo;
    s.samplesTotal = 0;
    s.attempts = 0;
    s.queued = 0;
    s.queueFull = 0;
    s.queueEmpty = 0;
    s.playFail = 0;
    s.invalid = 0;
    s.minSamples = UINT32_MAX;
    s.maxSamples = 0;
    s.maxDepth = 0;
    s.channel = channel;
    s.lastLogMs = nowMs();
}

inline void maybeLogDiagStats(bool force = false)
{
    AudioDiagStats& s = diagStats();
    const uint64_t now = nowMs();
    if (!force && (now - s.lastLogMs) < kAudioDiagLogPeriodMs) {
        return;
    }
    if (s.attempts == 0 && s.queueFull == 0 && s.invalid == 0) {
        s.lastLogMs = now;
        return;
    }

    const uint32_t minSamples = (s.minSamples == UINT32_MAX) ? 0 : s.minSamples;
    const uint32_t avgSamples = s.queued ? (uint32_t)(s.samplesTotal / s.queued) : 0;
    AUDIO_LOG("AUDIO",
              "stream reason=%s ch=%d rate=%lu stereo=%d queued=%lu attempts=%lu full=%lu empty=%lu fail=%lu invalid=%lu depthMax=%lu samples min/avg/max=%lu/%lu/%lu",
              s.reason,
              s.channel,
              (unsigned long)s.sampleRate,
              s.stereo ? 1 : 0,
              (unsigned long)s.queued,
              (unsigned long)s.attempts,
              (unsigned long)s.queueFull,
              (unsigned long)s.queueEmpty,
              (unsigned long)s.playFail,
              (unsigned long)s.invalid,
              (unsigned long)s.maxDepth,
              (unsigned long)minSamples,
              (unsigned long)avgSamples,
              (unsigned long)s.maxSamples);

    s.samplesTotal = 0;
    s.attempts = 0;
    s.queued = 0;
    s.queueFull = 0;
    s.queueEmpty = 0;
    s.playFail = 0;
    s.invalid = 0;
    s.minSamples = UINT32_MAX;
    s.maxSamples = 0;
    s.maxDepth = 0;
    s.lastLogMs = now;
}

inline void recordQueueDiag(size_t samples,
                            uint32_t sampleRate,
                            bool stereo,
                            int channel,
                            size_t depth,
                            bool ok,
                            bool invalid,
                            bool full)
{
    AudioDiagStats& s = diagStats();
    s.sampleRate = sampleRate;
    s.stereo = stereo;
    s.channel = channel;
    if (depth > s.maxDepth) {
        s.maxDepth = (uint32_t)depth;
    }

    if (invalid) {
        ++s.invalid;
        maybeLogDiagStats();
        return;
    }
    if (full) {
        ++s.queueFull;
        maybeLogDiagStats();
        return;
    }

    ++s.attempts;
    if (depth == 0) {
        ++s.queueEmpty;
    }
    if (ok) {
        ++s.queued;
        s.samplesTotal += samples;
        if (samples < s.minSamples) {
            s.minSamples = (uint32_t)samples;
        }
        if (samples > s.maxSamples) {
            s.maxSamples = (uint32_t)samples;
        }
    } else {
        ++s.playFail;
    }
    maybeLogDiagStats();
}
#else
inline void resetDiagStats(const char*, uint32_t, bool, int) {}
inline void maybeLogDiagStats(bool = false) {}
inline void recordQueueDiag(size_t,
                            uint32_t,
                            bool,
                            int,
                            size_t,
                            bool,
                            bool,
                            bool) {}
#endif

inline bool isCardputerSpeakerBoard()
{
    const auto board = M5.getBoard();
    return board == m5::board_t::board_M5Cardputer ||
           board == m5::board_t::board_M5CardputerADV;
}

inline void applySpeakerPins(m5::speaker_config_t& cfg)
{
    if (!isCardputerSpeakerBoard()) {
        return;
    }

    cfg.pin_bck = kSpeakerBck;
    cfg.pin_ws = kSpeakerWs;
    cfg.pin_data_out = kSpeakerData;
    cfg.pin_mck = I2S_PIN_NO_CHANGE;
    cfg.i2s_port = I2S_NUM_1;
    cfg.buzzer = false;
    cfg.use_dac = false;
    cfg.magnification = 16;
}

inline void logSpeakerConfig(const char* reason, const m5::speaker_config_t& cfg)
{
#if EMU_AUDIO_LOGS_ENABLED
    AUDIO_LOG("AUDIO",
              "speaker cfg reason=%s board=%d data=%d bck=%d ws=%d mck=%d port=%d rate=%lu stereo=%d dma=%u/%u task=%u/%u vol=%u enabled=%d running=%d",
              reason ? reason : "?",
              (int)M5.getBoard(),
              cfg.pin_data_out,
              cfg.pin_bck,
              cfg.pin_ws,
              cfg.pin_mck,
              (int)cfg.i2s_port,
              (unsigned long)cfg.sample_rate,
              cfg.stereo ? 1 : 0,
              (unsigned)cfg.dma_buf_len,
              (unsigned)cfg.dma_buf_count,
              (unsigned)cfg.task_priority,
              (unsigned)cfg.task_pinned_core,
              (unsigned)M5Cardputer.Speaker.getVolume(),
              M5Cardputer.Speaker.isEnabled() ? 1 : 0,
              M5Cardputer.Speaker.isRunning() ? 1 : 0);
#else
    (void)reason;
    (void)cfg;
#endif
}

inline bool beginSpeaker(uint32_t sampleRate,
                         bool stereo,
                         size_t dmaLen,
                         size_t dmaCount,
                         uint8_t volume,
                         const char* reason,
                         uint8_t taskPriority = 4,
                         uint8_t taskCore = 0,
                         bool restart = true)
{
    if (M5Cardputer.Speaker.isRunning() && restart) {
        AUDIO_LOG("AUDIO", "speaker restart reason=%s", reason ? reason : "?");
        M5Cardputer.Speaker.end();
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    auto cfg = M5Cardputer.Speaker.config();
    applySpeakerPins(cfg);
    cfg.sample_rate = sampleRate;
    cfg.stereo = stereo;
    cfg.dma_buf_len = dmaLen;
    cfg.dma_buf_count = dmaCount;
    cfg.task_priority = taskPriority < 8 ? 8 : taskPriority;
    cfg.task_pinned_core = taskCore;
    M5Cardputer.Speaker.config(cfg);
    logSpeakerConfig(reason, cfg);

    bool ok = M5Cardputer.Speaker.begin();

    M5Cardputer.Speaker.setVolume(volume);
    M5Cardputer.Speaker.setAllChannelVolume(255);
    resetDiagStats(reason, sampleRate, stereo, 0);

    AUDIO_LOG("AUDIO", "speaker begin reason=%s ok=%d enabled=%d running=%d volume=%u",
              reason ? reason : "?",
              ok ? 1 : 0,
              M5Cardputer.Speaker.isEnabled() ? 1 : 0,
              M5Cardputer.Speaker.isRunning() ? 1 : 0,
              (unsigned)M5Cardputer.Speaker.getVolume());
    return ok;
}

inline void configureBootSpeaker()
{
    logSpeakerConfig("boot", M5Cardputer.Speaker.config());
}

inline bool allocRuntimeAudioBuffers(int16_t* buffers[kRuntimeAudioBufferCount],
                                     size_t samples_per_buffer,
                                     const char* reason)
{
    if (samples_per_buffer == 0) {
        return false;
    }

    const size_t bytes = samples_per_buffer * sizeof(int16_t);
    for (size_t i = 0; i < kRuntimeAudioBufferCount; ++i) {
        if (buffers[i]) {
            continue;
        }
        buffers[i] = static_cast<int16_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (!buffers[i]) {
            AUDIO_LOG("AUDIO", "buffer alloc failed reason=%s slot=%u bytes=%lu",
                      reason ? reason : "?",
                      (unsigned)i,
                      (unsigned long)bytes);
            return false;
        }
        memset(buffers[i], 0, bytes);
    }
    return true;
}

inline void freeRuntimeAudioBuffers(int16_t* buffers[kRuntimeAudioBufferCount])
{
    for (size_t i = 0; i < kRuntimeAudioBufferCount; ++i) {
        if (buffers[i]) {
            heap_caps_free(buffers[i]);
            buffers[i] = nullptr;
        }
    }
}

inline bool queueRuntimeAudioBuffer(int16_t* buffers[kRuntimeAudioBufferCount],
                                    uint8_t& slot,
                                    size_t samples,
                                    uint32_t sampleRate,
                                    bool stereo,
                                    int channel)
{
    const size_t depth = M5Cardputer.Speaker.isPlaying(channel);
    if (samples == 0) {
        recordQueueDiag(samples, sampleRate, stereo, channel, depth, false, true, false);
        return false;
    }

    int16_t* pcm = buffers[slot];
    if (!pcm) {
        recordQueueDiag(samples, sampleRate, stereo, channel, depth, false, true, false);
        return false;
    }

    if (depth >= kRuntimeAudioQueueDepth) {
        recordQueueDiag(samples, sampleRate, stereo, channel, depth, false, false, true);
        return false;
    }

    const bool ok = M5Cardputer.Speaker.playRaw(
        pcm,
        samples,
        sampleRate,
        stereo,
        1,
        channel,
        false);

    recordQueueDiag(samples, sampleRate, stereo, channel, depth, ok, false, false);
    if (ok) {
        slot = (uint8_t)((slot + 1) % kRuntimeAudioBufferCount);
    }
    return ok;
}

}  // namespace cardputer_audio
