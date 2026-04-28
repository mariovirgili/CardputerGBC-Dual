#include "msx_psg.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace {

constexpr uint32_t kMsxCpuClockHz = 3579545u;
constexpr uint32_t kMsxPsgClockHz = 1789772u;
constexpr size_t kMsxPsgRingSamplesDefault = 4096u;
static int16_t* s_psgRing = nullptr;
static size_t s_psgRingSamples = 0u;
constexpr uint16_t kMsxVausMin = 164u;
constexpr uint16_t kMsxVausMax = 309u;
constexpr uint16_t kMsxVausCenter = 236u;
constexpr uint8_t kMsxVausBits = 9u;
constexpr uint16_t kMsxVausStep = 4u;
constexpr int16_t kMsxPsgVolumeTable[16] = {
    0, 64, 90, 128, 181, 256, 362, 512,
    724, 1024, 1448, 2048, 2896, 4096, 5792, 8192,
};

static int32_t s_dcFilterX = 0;
static int32_t s_dcFilterY = 0;

size_t IRAM_ATTR msx_psg_ring_samples()
{
    return s_psgRingSamples != 0u ? s_psgRingSamples : kMsxPsgRingSamplesDefault;
}

size_t msx_psg_ring_bytes()
{
    return msx_psg_ring_samples() * sizeof(int16_t);
}

bool msx_psg_ensure_ring()
{
    if (s_psgRing) {
        return true;
    }

    static constexpr size_t kRingCandidates[] = {4096u, 2048u, 1024u};
    for (size_t i = 0; i < (sizeof(kRingCandidates) / sizeof(kRingCandidates[0])); ++i) {
        const size_t samples = kRingCandidates[i];
        const size_t bytes = samples * sizeof(int16_t);
        s_psgRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
        if (!s_psgRing) {
            s_psgRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!s_psgRing) {
            s_psgRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (!s_psgRing) {
            s_psgRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT));
        }
        if (s_psgRing) {
            s_psgRingSamples = samples;
            break;
        }
    }
    return s_psgRing != nullptr;
}

uint16_t msx_psg_clamp_vaus_position(int value)
{
    if (value < static_cast<int>(kMsxVausMin)) {
        return kMsxVausMin;
    }
    if (value > static_cast<int>(kMsxVausMax)) {
        return kMsxVausMax;
    }
    return static_cast<uint16_t>(value);
}

void msx_psg_latch_vaus(MsxPsgState* state)
{
    if (!state) {
        return;
    }

    state->vausShiftRegister = static_cast<uint16_t>(state->vausPosition & 0x01FFu);
    state->vausBitIndex = static_cast<uint8_t>(kMsxVausBits - 1u);
    state->vausDataBit = static_cast<uint8_t>((state->vausShiftRegister >> state->vausBitIndex) & 0x01u);
}

void msx_psg_step_vaus_serial(MsxPsgState* state)
{
    if (!state) {
        return;
    }

    if (state->vausBitIndex > 0u && state->vausBitIndex < kMsxVausBits) {
        state->vausBitIndex--;
        state->vausDataBit = static_cast<uint8_t>((state->vausShiftRegister >> state->vausBitIndex) & 0x01u);
    } else {
        state->vausBitIndex = 0xFFu;
        state->vausDataBit = 0u;
    }
}

void msx_psg_update_vaus_control(MsxPsgState* state, uint8_t value)
{
    if (!state) {
        return;
    }

    const bool newClockHigh = (value & 0x01u) != 0u;
    const bool newResetHigh = (value & 0x10u) != 0u;

    if (!state->vausResetHigh && newResetHigh) {
        msx_psg_latch_vaus(state);
    } else if (!state->vausClockHigh && newClockHigh) {
        msx_psg_step_vaus_serial(state);
    }

    state->vausClockHigh = newClockHigh;
    state->vausResetHigh = newResetHigh;
}

uint8_t msx_psg_read_vaus_port(const MsxPsgState* state)
{
    if (!state) {
        return 0xFFu;
    }

    uint8_t value = 0xFCu;
    value |= static_cast<uint8_t>(state->vausDataBit & 0x01u);
    if (!state->vausButtonPressed) {
        value |= 0x02u;
    }
    return value;
}

uint16_t msx_psg_tone_period(const MsxPsgState* state, uint8_t channel)
{
    const uint8_t base = static_cast<uint8_t>(channel * 2u);
    uint16_t period = static_cast<uint16_t>(state->regs[base] | ((state->regs[base + 1u] & 0x0Fu) << 8));
    return period == 0u ? 1u : period;
}

uint16_t msx_psg_noise_period(const MsxPsgState* state)
{
    const uint16_t period = static_cast<uint16_t>(state->regs[6] & 0x1Fu);
    return period == 0u ? 1u : period;
}

uint16_t msx_psg_envelope_period(const MsxPsgState* state)
{
    const uint16_t period = static_cast<uint16_t>(state->regs[11] | (static_cast<uint16_t>(state->regs[12]) << 8));
    return period == 0u ? 1u : period;
}

uint32_t msx_psg_compute_step(uint32_t clockHz, uint32_t divider, uint16_t period, uint32_t sampleRate)
{
    if (sampleRate == 0u) {
        return 0u;
    }

    const uint64_t denominator = static_cast<uint64_t>(divider) * static_cast<uint64_t>(period) * static_cast<uint64_t>(sampleRate);
    if (denominator == 0u) {
        return 0u;
    }

    const uint64_t numerator = static_cast<uint64_t>(clockHz) << 32;
    uint64_t step = numerator / denominator;
    if (step > 0xFFFFFFFFull) {
        step = 0xFFFFFFFFull;
    }
    return static_cast<uint32_t>(step);
}

void msx_psg_update_cached_steps(MsxPsgState* state)
{
    if (!state || state->sampleRate == 0u) {
        return;
    }

    for (uint8_t channel = 0; channel < 3u; ++channel) {
        state->toneStep[channel] = msx_psg_compute_step(
            state->psgClockHz,
            16u,
            msx_psg_tone_period(state, channel),
            state->sampleRate
        );
    }

    state->noiseStep = msx_psg_compute_step(
        state->psgClockHz,
        16u,
        msx_psg_noise_period(state),
        state->sampleRate
    );

    state->envelopeStep = msx_psg_compute_step(
        state->psgClockHz,
        256u,
        msx_psg_envelope_period(state),
        state->sampleRate
    );
}

void msx_psg_restart_envelope(MsxPsgState* state)
{
    if (!state) {
        return;
    }

    state->envelopeShape = static_cast<uint8_t>(state->regs[13] & 0x0Fu);
    state->envelopeHolding = false;
    state->envelopePhase = 0u;
    state->envelopeDirection = (state->envelopeShape & 0x04u) != 0u ? 1 : -1;
    state->envelopeVolume = static_cast<uint8_t>((state->envelopeDirection > 0) ? 0u : 15u);
}

void IRAM_ATTR msx_psg_step_envelope(MsxPsgState* state)
{
    if (!state || state->envelopeHolding) {
        return;
    }

    const uint8_t shape = state->envelopeShape;
    const bool continueShape = (shape & 0x08u) != 0u;
    const bool alternate = (shape & 0x02u) != 0u;
    const bool hold = (shape & 0x01u) != 0u;

    const int nextVolume = static_cast<int>(state->envelopeVolume) + static_cast<int>(state->envelopeDirection);
    if (nextVolume >= 0 && nextVolume <= 15) {
        state->envelopeVolume = static_cast<uint8_t>(nextVolume);
        return;
    }

    if (!continueShape) {
        state->envelopeVolume = static_cast<uint8_t>(state->envelopeDirection > 0 ? 15 : 0);
        state->envelopeHolding = true;
        return;
    }

    if (alternate) {
        state->envelopeDirection = static_cast<int8_t>(-state->envelopeDirection);
    }

    state->envelopeVolume = static_cast<uint8_t>(state->envelopeDirection > 0 ? 0 : 15);
    if (hold) {
        state->envelopeVolume = static_cast<uint8_t>(state->envelopeDirection > 0 ? 15 : 0);
        state->envelopeHolding = true;
    }
}

void IRAM_ATTR msx_psg_push_sample(MsxPsgState* state, int16_t sample)
{
    if (!state || !state->ready || !state->ring) {
        return;
    }

    const size_t ringSamples = msx_psg_ring_samples();
    const uint16_t ringMask = static_cast<uint16_t>(ringSamples - 1u);
    if (state->ringCount >= ringSamples) {
        state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + 1u) & ringMask);
        state->ringCount--;
        state->droppedSamples++;
    }

    state->ring[state->ringWriteIndex] = sample;
    state->ringWriteIndex = static_cast<uint16_t>((state->ringWriteIndex + 1u) & ringMask);
    state->ringCount++;
    state->generatedSamples++;
}

int16_t IRAM_ATTR msx_psg_render_sample(MsxPsgState* state)
{
    if (!state) {
        return 0;
    }

    int32_t mix = 0;
    const uint8_t mixer = state->regs[7];
    const bool noiseHigh = (state->lfsr & 0x0001u) != 0u;
    for (uint8_t channel = 0; channel < 3u; ++channel) {
        state->tonePhase[channel] += state->toneStep[channel];
        const bool toneHigh = (state->tonePhase[channel] & 0x80000000u) != 0u;

        const bool toneDisabled = (mixer & (1u << channel)) != 0u;
        const bool noiseDisabled = (mixer & (1u << (channel + 3u))) != 0u;
        const bool gate = (toneDisabled || toneHigh) && (noiseDisabled || noiseHigh);

        const uint8_t volumeReg = static_cast<uint8_t>(state->regs[8u + channel] & 0x1Fu);
        const bool useEnvelope = (volumeReg & 0x10u) != 0u;
        const uint8_t level = useEnvelope ? state->envelopeVolume : static_cast<uint8_t>(volumeReg & 0x0Fu);
        const int32_t amplitude = kMsxPsgVolumeTable[level];
        if (amplitude == 0) {
            continue;
        }
        mix += gate ? amplitude : -amplitude;
    }

    const uint32_t oldNoisePhase = state->noisePhase;
    state->noisePhase += state->noiseStep;
    if (state->noisePhase < oldNoisePhase) {
        const uint32_t feedback = (state->lfsr ^ (state->lfsr >> 3)) & 0x0001u;
        state->lfsr = (state->lfsr >> 1) | (feedback << 16);
        if (state->lfsr == 0u) {
            state->lfsr = 0x1FFFFu;
        }
    }

    const uint32_t oldEnvelopePhase = state->envelopePhase;
    state->envelopePhase += state->envelopeStep;
    if (state->envelopePhase < oldEnvelopePhase) {
        msx_psg_step_envelope(state);
    }

    mix /= 2;

    // Applica un filtro DSP Passa-Alto (DC Blocker) per eliminare i crepitii statici
    // Formula: y[n] = x[n] - x[n-1] + R * y[n-1] (con R =~ 0.99)
    int32_t dcFiltered = mix - s_dcFilterX + ((s_dcFilterY * 8110) >> 13);
    s_dcFilterX = mix;
    s_dcFilterY = dcFiltered;
    mix = dcFiltered;

    if (mix > 32767) {
        mix = 32767;
    } else if (mix < -32768) {
        mix = -32768;
    }

    return static_cast<int16_t>(mix);
}

} // namespace

bool msx_psg_init(MsxPsgState* state, uint32_t sampleRate)
{
    if (!state || sampleRate == 0u || !msx_psg_ensure_ring()) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->sampleRate = sampleRate;
    state->cpuClockHz = kMsxCpuClockHz;
    state->psgClockHz = kMsxPsgClockHz;
    state->ring = s_psgRing;
    state->ready = true;
    msx_psg_reset(state);
    return true;
}

void msx_psg_reset(MsxPsgState* state)
{
    if (!state) {
        return;
    }

    const uint32_t sampleRate = state->sampleRate;
    const uint32_t cpuClockHz = state->cpuClockHz == 0u ? kMsxCpuClockHz : state->cpuClockHz;
    const uint32_t psgClockHz = state->psgClockHz == 0u ? kMsxPsgClockHz : state->psgClockHz;

    s_dcFilterX = 0;
    s_dcFilterY = 0;

    std::memset(state, 0, sizeof(*state));
    state->sampleRate = sampleRate;
    state->cpuClockHz = cpuClockHz;
    state->psgClockHz = psgClockHz;
    state->ring = s_psgRing;
    if (!state->ring) {
        state->ready = false;
        return;
    }
    std::memset(state->ring, 0, msx_psg_ring_bytes());
    state->selectedReg = 0u;
    state->joystickPortA = 0xFFu; // active-low: 0xFF = no buttons pressed
    state->joystickPortB = 0xFFu; // second GP port defaults to idle/high
    state->vausPosition = kMsxVausCenter;
    state->vausShiftRegister = kMsxVausCenter;
    state->vausBitIndex = static_cast<uint8_t>(kMsxVausBits - 1u);
    state->vausDataBit = static_cast<uint8_t>((kMsxVausCenter >> (kMsxVausBits - 1u)) & 0x01u);
    state->lfsr = 0x1FFFFu;
    state->envelopeDirection = -1;
    state->envelopeVolume = 15u;
    state->ready = true;
    state->regs[7] = 0x3Fu;
    state->regs[8] = 0x00u;
    state->regs[9] = 0x00u;
    state->regs[10] = 0x00u;
    msx_psg_update_cached_steps(state);
}

void msx_psg_shutdown(MsxPsgState* state)
{
    if (!state) {
        return;
    }

    if (s_psgRing) {
        heap_caps_free(s_psgRing);
        s_psgRing = nullptr;
        s_psgRingSamples = 0u;
    }
    std::memset(state, 0, sizeof(*state));
}

void msx_psg_select_register(MsxPsgState* state, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    state->selectedReg = static_cast<uint8_t>(value & 0x0Fu);
}

void msx_psg_write_data(MsxPsgState* state, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    const uint8_t reg = state->selectedReg & 0x0Fu;
    const uint8_t previous = state->regs[reg];
    state->regs[reg] = value;

    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 11:
        case 12:
            msx_psg_update_cached_steps(state);
            break;
        case 13:
            msx_psg_restart_envelope(state);
            break;
        case 14:
            state->ioPortA = value;
            break;
        case 15:
            state->ioPortB = value;
            if (state->vausEnabled && (previous != value)) {
                msx_psg_update_vaus_control(state, value);
            }
            break;
        default:
            break;
    }
}

uint8_t msx_psg_read_data(const MsxPsgState* state)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }

    // Register 14 is the joystick/input port A — always returns hardware state,
    // Bits 6-7 stay high here to represent keyboard-type/cassette input as idle.
    const uint8_t reg = static_cast<uint8_t>(state->selectedReg & 0x0Fu);
    if (reg == 14u) {
        const bool selectPortB = (state->regs[15] & 0x40u) != 0u;
        uint8_t value = 0xFFu;
        if (state->vausEnabled && !selectPortB) {
            value = msx_psg_read_vaus_port(state);
        } else {
            value = selectPortB ? state->joystickPortB : state->joystickPortA;
        }
        value |= 0xC0u;
        return value;
    }

    return state->regs[reg];
}

void msx_psg_set_joystick(MsxPsgState* state, uint8_t portA)
{
    msx_psg_set_joysticks(state, portA, 0xFFu);
}

void msx_psg_set_joysticks(MsxPsgState* state, uint8_t portA, uint8_t portB)
{
    if (!state) {
        return;
    }

    state->joystickPortA = portA;
    state->joystickPortB = portB;
}

void msx_psg_set_vaus_enabled(MsxPsgState* state, bool enabled)
{
    if (!state) {
        return;
    }

    if (state->vausEnabled != enabled) {
        state->vausEnabled = enabled;
        state->vausClockHigh = (state->regs[15] & 0x01u) != 0u;
        state->vausResetHigh = (state->regs[15] & 0x10u) != 0u;
        msx_psg_latch_vaus(state);
    }
}

void msx_psg_set_vaus_input(MsxPsgState* state, bool moveLeft, bool moveRight, bool buttonPressed)
{
    if (!state) {
        return;
    }

    if (moveLeft != moveRight) {
        const int delta = moveRight ? static_cast<int>(kMsxVausStep) : -static_cast<int>(kMsxVausStep);
        state->vausPosition = msx_psg_clamp_vaus_position(static_cast<int>(state->vausPosition) + delta);
    }

    state->vausButtonPressed = buttonPressed;
}

void IRAM_ATTR msx_psg_run_cycles(MsxPsgState* state, uint32_t cpuCycles)
{
    if (!state || !state->ready || state->sampleRate == 0u || cpuCycles == 0u) {
        return;
    }

    state->sampleAccumulator += static_cast<uint64_t>(cpuCycles) * static_cast<uint64_t>(state->sampleRate);
    while (state->sampleAccumulator >= state->cpuClockHz) {
        state->sampleAccumulator -= state->cpuClockHz;
        msx_psg_push_sample(state, msx_psg_render_sample(state));
    }
}

size_t msx_psg_read_samples(MsxPsgState* state, int16_t* dst, size_t maxSamples)
{
    if (!state || !state->ready || !dst || maxSamples == 0u) {
        return 0u;
    }

    size_t count = 0u;
    const size_t ringSamples = msx_psg_ring_samples();
    const uint16_t ringMask = static_cast<uint16_t>(ringSamples - 1u);
    while (count < maxSamples && state->ringCount > 0u) {
        dst[count++] = state->ring[state->ringReadIndex];
        state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + 1u) & ringMask);
        state->ringCount--;
    }
    return count;
}

void msx_psg_discard_samples(MsxPsgState* state, size_t sampleCount)
{
    if (!state || !state->ready || sampleCount == 0u) {
        return;
    }

    if (sampleCount > state->ringCount) {
        sampleCount = state->ringCount;
    }

    const uint16_t ringMask = static_cast<uint16_t>(msx_psg_ring_samples() - 1u);
    state->ringReadIndex = static_cast<uint16_t>((state->ringReadIndex + sampleCount) & ringMask);
    state->ringCount = static_cast<uint16_t>(state->ringCount - sampleCount);
}

size_t msx_psg_available_samples(const MsxPsgState* state)
{
    if (!state || !state->ready) {
        return 0u;
    }

    return state->ringCount;
}
