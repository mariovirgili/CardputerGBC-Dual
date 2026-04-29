#include "msx_scc.h"

#include <esp_heap_caps.h>

#include <cstring>
#include <cstdio>

#ifndef MSX_SCC_LOG_ENABLED
#define MSX_SCC_LOG_ENABLED 0
#endif

#ifndef MSX_SCC_NOTICE_ENABLED
#define MSX_SCC_NOTICE_ENABLED 1
#endif

namespace {

constexpr uint32_t kMsxCpuClockHz = 3579545u;
constexpr size_t kMsxSccRingSamplesDefault = 1024u;
static int16_t* s_sccRing = nullptr;
static size_t s_sccRingSamples = 0u;
static uint16_t s_sccRingMask = static_cast<uint16_t>(kMsxSccRingSamplesDefault - 1u);

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

    const uint64_t numerator = static_cast<uint64_t>(state->cpuClockHz) << 16;
    const uint64_t denominator =
        static_cast<uint64_t>(state->sampleRate) *
        static_cast<uint64_t>(freqReg) *
        32u;
    uint64_t step = denominator != 0u ? (numerator / denominator) : 0u;
    if (step > 0xFFFFFFFFull) {
        step = 0xFFFFFFFFull;
    }
    state->step[channel] = static_cast<uint32_t>(step);
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
    if (!state || !state->enabled) {
        return 0;
    }

    const uint8_t enableMask = static_cast<uint8_t>(state->regs[0xAFu] & 0x1Fu);
    if (enableMask == 0u) {
        return 0;
    }

    int32_t mix = 0;
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
        const uint8_t waveChannel =
            state->sccPlusMode ? ch : static_cast<uint8_t>((ch >= 4u) ? 3u : ch);
        const uint8_t waveBase = static_cast<uint8_t>(waveChannel * 32u);
        const int8_t waveSample = static_cast<int8_t>(state->regs[waveBase + waveIndex]);
        mix += static_cast<int32_t>(waveSample) * static_cast<int32_t>(volume);
    }

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
        if (reg >= 0x60u) {
            state->regs[reg + 0x20u] = value;
        }
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

    state->sampleAccumulator += static_cast<uint64_t>(cpuCycles) * static_cast<uint64_t>(state->sampleRate);
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
