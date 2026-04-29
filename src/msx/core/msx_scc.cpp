#include "msx_scc.h"

#include <esp_heap_caps.h>

#include <cstring>
#include <cstdio>

#include "../msx_config.h"

#ifndef MSX_SCC_LOG_ENABLED
#define MSX_SCC_LOG_ENABLED 0
#endif

#ifndef MSX_SCC_NOTICE_ENABLED
#define MSX_SCC_NOTICE_ENABLED 1
#endif

#ifndef MSX_SCC_AUDIO_NOTICE_ENABLED
#define MSX_SCC_AUDIO_NOTICE_ENABLED 0
#endif

namespace {

constexpr uint32_t kMsxCpuClockHz = 3579545u;
constexpr size_t kMsxSccRingSamplesDefault = 1024u;
constexpr uint8_t kMsxSccWaveBaseByMode[2][5] = {
    {0u, 32u, 64u, 96u, 96u},
    {0u, 32u, 64u, 96u, 128u},
};
static int16_t* s_sccRing = nullptr;
static size_t s_sccRingSamples = 0u;
static uint16_t s_sccRingMask = static_cast<uint16_t>(kMsxSccRingSamplesDefault - 1u);
static uint16_t s_sccOutputGainPercent = 150u;

size_t msx_scc_ring_samples()
{
    return s_sccRingSamples != 0u ? s_sccRingSamples : kMsxSccRingSamplesDefault;
}

size_t msx_scc_ring_bytes()
{
    return msx_scc_ring_samples() * sizeof(int16_t);
}

bool msx_scc_ensure_ring()
{
    if (s_sccRing) {
        return true;
    }

    static constexpr size_t kRingCandidates[] = {1024u, 512u};
    for (size_t i = 0; i < (sizeof(kRingCandidates) / sizeof(kRingCandidates[0])); ++i) {
        const size_t samples = kRingCandidates[i];
        const size_t bytes = samples * sizeof(int16_t);
        s_sccRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!s_sccRing) {
            s_sccRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
        }
        if (!s_sccRing) {
            s_sccRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (!s_sccRing) {
            s_sccRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT));
        }
        if (s_sccRing) {
            s_sccRingSamples = samples;
            s_sccRingMask = static_cast<uint16_t>(samples - 1u);
            break;
        }
    }

    return s_sccRing != nullptr;
}

uint32_t msx_scc_compute_step_scale(uint32_t cpuClockHz, uint32_t sampleRate)
{
    if (sampleRate == 0u) {
        return 0u;
    }

    const uint64_t scale = (static_cast<uint64_t>(cpuClockHz) << 16) / sampleRate;
    return scale > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(scale);
}

void msx_scc_update_step_scale(MsxSccState* state)
{
    if (!state) {
        return;
    }
    state->stepScale = msx_scc_compute_step_scale(state->cpuClockHz, state->sampleRate);
}

void msx_scc_update_step(MsxSccState* state, uint8_t channel)
{
    if (!state || channel >= 5u || state->sampleRate == 0u) {
        return;
    }

    const uint8_t regBase = static_cast<uint8_t>(0xA0u + channel * 2u);
    const uint32_t freqReg =
        static_cast<uint32_t>(state->regs[regBase]) |
        (static_cast<uint32_t>(state->regs[regBase + 1u] & 0x0Fu) << 8);
    if (freqReg == 0u) {
        state->step[channel] = 0u;
        return;
    }

    const uint32_t effectivePeriod = freqReg + 1u;
    if (state->stepScale == 0u) {
        msx_scc_update_step_scale(state);
    }
    state->step[channel] = state->stepScale / effectivePeriod;
}

void msx_scc_log_window_change(bool classicWindow,
                               bool plusWindow,
                               bool sccPlusMode,
                               bool enabled)
{
#if MSX_SCC_LOG_ENABLED
    std::printf("[MSX][SCC] windows classic=%u plus=%u mode=%s enabled=%u\n",
                classicWindow ? 1u : 0u,
                plusWindow ? 1u : 0u,
                sccPlusMode ? "SCC-I" : "SCC",
                enabled ? 1u : 0u);
#else
    (void)classicWindow;
    (void)plusWindow;
    (void)sccPlusMode;
    (void)enabled;
#endif
}

void msx_scc_log_write(const char* kind, uint8_t reg, uint8_t value)
{
#if MSX_SCC_LOG_ENABLED
    std::printf("[MSX][SCC] %s reg=%02X value=%02X\n",
                kind ? kind : "write",
                static_cast<unsigned>(reg),
                static_cast<unsigned>(value));
#else
    (void)kind;
    (void)reg;
    (void)value;
#endif
}

uint16_t msx_scc_abs16(int16_t sample)
{
    return sample == INT16_MIN
               ? 32768u
               : static_cast<uint16_t>(sample < 0 ? -sample : sample);
}

void msx_scc_log_audio_if_needed(MsxSccState* state, const int16_t* samples, size_t count)
{
#if MSX_SCC_AUDIO_NOTICE_ENABLED
    if (!state || !state->outputEnabled || !samples || count == 0u) {
        return;
    }

    uint32_t nonZero = 0u;
    uint16_t peak = 0u;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t mag = msx_scc_abs16(samples[i]);
        if (mag != 0u) {
            ++nonZero;
            if (mag > peak) {
                peak = mag;
            }
        }
    }

    if (nonZero == 0u) {
        return;
    }

    state->audibleSamples += nonZero;
    if (peak > state->audiblePeak) {
        state->audiblePeak = peak;
    }

    const uint32_t minLogGap = state->sampleRate != 0u ? (state->sampleRate / 2u) : 11025u;
    const bool firstLogs = state->audibleLogCount < 8u;
    const bool periodicLog =
        state->generatedSamples - state->lastAudibleLogSample >= minLogGap;
    if (!firstLogs && !periodicLog) {
        return;
    }

    state->lastAudibleLogSample = state->generatedSamples;
    if (state->audibleLogCount < 255u) {
        ++state->audibleLogCount;
    }

    std::printf("[MSX][SCC] audio mode=%s count=%u nz=%u peak=%u totalNz=%u en=%02X vol=%X/%X/%X/%X/%X step=%u/%u/%u/%u/%u\n",
                state->sccPlusMode ? "SCC-I" : "SCC",
                static_cast<unsigned>(count),
                static_cast<unsigned>(nonZero),
                static_cast<unsigned>(state->audiblePeak),
                static_cast<unsigned>(state->audibleSamples),
                static_cast<unsigned>(state->regs[0xAFu] & 0x1Fu),
                static_cast<unsigned>(state->regs[0xAAu] & 0x0Fu),
                static_cast<unsigned>(state->regs[0xABu] & 0x0Fu),
                static_cast<unsigned>(state->regs[0xACu] & 0x0Fu),
                static_cast<unsigned>(state->regs[0xADu] & 0x0Fu),
                static_cast<unsigned>(state->regs[0xAEu] & 0x0Fu),
                static_cast<unsigned>(state->step[0]),
                static_cast<unsigned>(state->step[1]),
                static_cast<unsigned>(state->step[2]),
                static_cast<unsigned>(state->step[3]),
                static_cast<unsigned>(state->step[4]));
    state->audiblePeak = 0u;
#else
    (void)state;
    (void)samples;
    (void)count;
#endif
}

void msx_scc_push_sample(MsxSccState* state, int16_t sample)
{
    if (!state || !state->ready || !state->ring) {
        return;
    }

    if (state->ringCount >= msx_scc_ring_samples()) {
        state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + 1u) & s_sccRingMask);
        state->ringCount--;
        state->droppedSamples++;
    }

    state->ring[state->ringWriteIndex] = sample;
    state->ringWriteIndex = static_cast<uint16_t>((state->ringWriteIndex + 1u) & s_sccRingMask);
    state->ringCount++;
    state->generatedSamples++;
}

int16_t msx_scc_render_sample(MsxSccState* state)
{
    if (!state || !state->enabled || !state->outputEnabled) {
        return 0;
    }

    const uint8_t enableMask = static_cast<uint8_t>(state->regs[0xAFu] & 0x1Fu);
    if (enableMask == 0u) {
        return 0;
    }

    int32_t mix = 0;
    const uint8_t* waveBaseByChannel = kMsxSccWaveBaseByMode[state->sccPlusMode ? 1u : 0u];
    for (uint8_t ch = 0u; ch < 5u; ++ch) {
        if ((enableMask & (1u << ch)) == 0u || state->step[ch] == 0u) {
            continue;
        }

        const uint8_t volume = static_cast<uint8_t>(state->regs[0xAAu + ch] & 0x0Fu);
        if (volume == 0u) {
            continue;
        }

        state->phase[ch] += state->step[ch];
        const uint8_t waveIndex = static_cast<uint8_t>((state->phase[ch] >> 16) & 0x1Fu);
        const uint8_t nextWaveIndex = static_cast<uint8_t>((waveIndex + 1u) & 0x1Fu);
        const uint16_t waveFrac = static_cast<uint16_t>(state->phase[ch] & 0xFFFFu);
        const uint16_t waveBase = waveBaseByChannel[ch];
        const int32_t waveSample0 = static_cast<int8_t>(state->regs[waveBase + waveIndex]);
        const int32_t waveSample1 = static_cast<int8_t>(state->regs[waveBase + nextWaveIndex]);
        const int32_t waveSample =
            waveSample0 + (((waveSample1 - waveSample0) * static_cast<int32_t>(waveFrac)) >> 16);
        mix += waveSample * static_cast<int32_t>(volume);
    }

    mix = (mix * static_cast<int32_t>(s_sccOutputGainPercent)) / 100;
    const int32_t dcFiltered = mix - state->dcFilterX + ((state->dcFilterY * 8110) >> 13);
    state->dcFilterX = mix;
    state->dcFilterY = dcFiltered;
    mix = dcFiltered;

    if (mix > 32767) {
        mix = 32767;
    } else if (mix < -32768) {
        mix = -32768;
    }
    return static_cast<int16_t>(mix);
}

} // namespace

bool msx_scc_init(MsxSccState* state, uint32_t sampleRate)
{
    if (!state || sampleRate == 0u || !msx_scc_ensure_ring()) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->sampleRate = sampleRate;
    state->cpuClockHz = kMsxCpuClockHz;
    state->ring = s_sccRing;
    state->ready = true;
    msx_scc_reset(state);
#if MSX_SCC_LOG_ENABLED
    std::printf("[MSX][SCC] init sampleRate=%u ring=%u\n",
                static_cast<unsigned>(sampleRate),
                static_cast<unsigned>(msx_scc_ring_samples()));
#endif
    return true;
}

void msx_scc_reset(MsxSccState* state)
{
    if (!state) {
        return;
    }

    const uint32_t sampleRate = state->sampleRate;
    const uint32_t cpuClockHz = state->cpuClockHz == 0u ? kMsxCpuClockHz : state->cpuClockHz;
    std::memset(state, 0, sizeof(*state));
    state->sampleRate = sampleRate;
    state->cpuClockHz = cpuClockHz;
    msx_scc_update_step_scale(state);
    state->ring = s_sccRing;
    if (!state->ring) {
        state->ready = false;
        return;
    }
    std::memset(state->ring, 0, msx_scc_ring_bytes());
    state->ready = true;
}

void msx_scc_shutdown(MsxSccState* state)
{
    if (!state) {
        return;
    }

    if (s_sccRing) {
        heap_caps_free(s_sccRing);
        s_sccRing = nullptr;
        s_sccRingSamples = 0u;
        s_sccRingMask = static_cast<uint16_t>(kMsxSccRingSamplesDefault - 1u);
    }
    std::memset(state, 0, sizeof(*state));
}

void msx_scc_set_windows(MsxSccState* state, bool classicWindow, bool plusWindow)
{
    if (!state || !state->ready) {
        return;
    }

    const bool oldClassicWindow = state->classicWindow;
    const bool oldPlusWindow = state->plusWindow;
    const bool oldPlusMode = state->sccPlusMode;
    const bool oldEnabled = state->enabled;

    state->classicWindow = classicWindow;
    state->plusWindow = plusWindow;
    state->enabled = classicWindow || plusWindow;
    if (plusWindow) {
        state->sccPlusMode = true;
    } else if (classicWindow) {
        state->sccPlusMode = false;
    }

#if MSX_SCC_NOTICE_ENABLED
    static bool s_classicNoticeShown = false;
    static bool s_plusNoticeShown = false;
    if (state->enabled) {
        if (state->sccPlusMode) {
            if (!s_plusNoticeShown) {
                std::printf("[MSX][SCC] active mode=SCC-I\n");
                s_plusNoticeShown = true;
            }
        } else if (!s_classicNoticeShown) {
            std::printf("[MSX][SCC] active mode=SCC\n");
            s_classicNoticeShown = true;
        }
    }
#endif

    if (oldClassicWindow != state->classicWindow ||
        oldPlusWindow != state->plusWindow ||
        oldPlusMode != state->sccPlusMode ||
        oldEnabled != state->enabled) {
        msx_scc_log_window_change(state->classicWindow,
                                  state->plusWindow,
                                  state->sccPlusMode,
                                  state->enabled);
    }
}

void msx_scc_set_output_enabled(MsxSccState* state, bool enabled)
{
    if (!state || !state->ready) {
        return;
    }
    state->outputEnabled = enabled;
}

void msx_scc_set_output_gain_percent(uint16_t gainPercent)
{
    if (gainPercent > 300u) {
        gainPercent = 300u;
    }
    s_sccOutputGainPercent = gainPercent;
}

uint16_t msx_scc_get_output_gain_percent(void)
{
    return s_sccOutputGainPercent;
}

void msx_scc_recompute_steps(MsxSccState* state)
{
    if (!state) {
        return;
    }
    state->dcFilterX = 0;
    state->dcFilterY = 0;
    msx_scc_update_step_scale(state);
    for (uint8_t ch = 0u; ch < 5u; ++ch) {
        msx_scc_update_step(state, ch);
    }
}

void msx_scc_write(MsxSccState* state, uint8_t reg, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    if (reg >= 0xE0u) {
        return;
    }

    if (reg < 0x80u) {
#if MSX_SCC_LOG_ENABLED
        static uint8_t s_waveLogCount = 0u;
        if (s_waveLogCount < 8u) {
            msx_scc_log_write("wave", reg, value);
            ++s_waveLogCount;
        }
#endif
        state->regs[reg] = value;
        return;
    }

    msx_scc_write_plus(state, static_cast<uint8_t>(reg + 0x20u), value);
}

void msx_scc_write_plus(MsxSccState* state, uint8_t reg, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    if (reg >= 0xB0u && reg < 0xC0u) {
        msx_scc_write_plus(state, static_cast<uint8_t>(reg - 0x10u), value);
        return;
    }

    if (reg < 0xA0u) {
        if (msx_config_get_scc_hardware_detect_enabled() &&
            reg >= 0x80u &&
            (state->regs[0xAFu] & 0x20u) == 0u) {
#if MSX_SCC_LOG_ENABLED
            static uint8_t s_plusWave5GateLogCount = 0u;
            if (s_plusWave5GateLogCount < 8u) {
                std::printf("[MSX][SCC] plus-wave5 blocked reg=%02X af=%02X\n",
                            static_cast<unsigned>(reg),
                            static_cast<unsigned>(state->regs[0xAFu]));
                ++s_plusWave5GateLogCount;
            }
#endif
            return;
        }
#if MSX_SCC_LOG_ENABLED
        static uint8_t s_plusWaveLogCount = 0u;
        if (s_plusWaveLogCount < 8u) {
            msx_scc_log_write("plus-wave", reg, value);
            ++s_plusWaveLogCount;
        }
#endif
        state->regs[reg] = value;
        return;
    }

    if (reg < 0xB0u) {
        const uint8_t previous = state->regs[reg];
        state->regs[reg] = value;
        state->regs[reg + 0x10u] = value;
        const uint8_t subReg = static_cast<uint8_t>(reg & 0x0Fu);
        if (subReg <= 9u) {
#if MSX_SCC_LOG_ENABLED
            static uint8_t s_freqLogCount = 0u;
            if (s_freqLogCount < 12u) {
                msx_scc_log_write("freq", reg, value);
                ++s_freqLogCount;
            }
#endif
            msx_scc_update_step(state, static_cast<uint8_t>(subReg / 2u));
        } else if (subReg <= 14u) {
#if MSX_SCC_LOG_ENABLED
            static uint8_t s_volumeLogCount = 0u;
            if (s_volumeLogCount < 10u) {
                msx_scc_log_write("volume", reg, value);
                ++s_volumeLogCount;
            }
#endif
        } else if (previous != value) {
            msx_scc_log_write("enable", reg, value);
        }
    }
}

uint8_t msx_scc_read(const MsxSccState* state, uint8_t reg)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }
    return reg < 0x80u ? state->regs[reg] : 0xFFu;
}

uint8_t msx_scc_read_plus(const MsxSccState* state, uint8_t reg)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }
    return reg < 0xA0u ? state->regs[reg] : 0xFFu;
}

void msx_scc_run_cycles(MsxSccState* state, uint32_t cpuCycles)
{
    if (!state || !state->ready || state->sampleRate == 0u || cpuCycles == 0u) {
        return;
    }

    state->sampleAccumulator += cpuCycles * state->sampleRate;
    while (state->sampleAccumulator >= state->cpuClockHz) {
        state->sampleAccumulator -= state->cpuClockHz;
        msx_scc_push_sample(state, msx_scc_render_sample(state));
    }
}

size_t msx_scc_read_samples(MsxSccState* state, int16_t* dst, size_t maxSamples)
{
    if (!state || !state->ready || !dst || maxSamples == 0u) {
        return 0u;
    }

    const size_t count = state->ringCount < maxSamples ? state->ringCount : maxSamples;
    if (count == 0u) {
        return 0u;
    }

    const size_t ringSamples = msx_scc_ring_samples();
    const size_t contiguous = ringSamples - state->ringReadIndex;
    const size_t first = count < contiguous ? count : contiguous;
    std::memcpy(dst, state->ring + state->ringReadIndex, first * sizeof(int16_t));

    const size_t second = count - first;
    if (second != 0u) {
        std::memcpy(dst + first, state->ring, second * sizeof(int16_t));
    }

    msx_scc_log_audio_if_needed(state, dst, count);

    state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + count) & s_sccRingMask);
    state->ringCount = static_cast<uint16_t>(state->ringCount - count);
    return count;
}

void msx_scc_discard_samples(MsxSccState* state, size_t sampleCount)
{
    if (!state || !state->ready || sampleCount == 0u) {
        return;
    }

    if (sampleCount > state->ringCount) {
        sampleCount = state->ringCount;
    }

    state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + sampleCount) & s_sccRingMask);
    state->ringCount = static_cast<uint16_t>(state->ringCount - sampleCount);
}

size_t msx_scc_available_samples(const MsxSccState* state)
{
    if (!state || !state->ready) {
        return 0u;
    }
    return state->ringCount;
}
