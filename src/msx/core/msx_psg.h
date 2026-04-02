#pragma once

#include <stddef.h>
#include <stdint.h>

struct MsxPsgState {
    uint8_t regs[16];
    uint8_t selectedReg;
    uint32_t sampleRate;
    uint32_t cpuClockHz;
    uint32_t psgClockHz;
    uint64_t sampleAccumulator;
    uint32_t tonePhase[3];
    uint32_t toneStep[3];
    uint32_t noisePhase;
    uint32_t noiseStep;
    uint32_t envelopePhase;
    uint32_t envelopeStep;
    uint32_t lfsr;
    uint8_t envelopeShape;
    uint8_t envelopeVolume;
    int8_t envelopeDirection;
    uint8_t ioPortA;
    uint8_t ioPortB;
    uint16_t ringReadIndex;
    uint16_t ringWriteIndex;
    uint16_t ringCount;
    uint32_t generatedSamples;
    uint32_t droppedSamples;
    bool envelopeHolding;
    bool ready;
    int16_t* ring;
};

bool msx_psg_init(MsxPsgState* state, uint32_t sampleRate);
void msx_psg_reset(MsxPsgState* state);
void msx_psg_shutdown(MsxPsgState* state);
void msx_psg_select_register(MsxPsgState* state, uint8_t value);
void msx_psg_write_data(MsxPsgState* state, uint8_t value);
uint8_t msx_psg_read_data(const MsxPsgState* state);
void msx_psg_run_cycles(MsxPsgState* state, uint32_t cpuCycles);
size_t msx_psg_read_samples(MsxPsgState* state, int16_t* dst, size_t maxSamples);
void msx_psg_discard_samples(MsxPsgState* state, size_t sampleCount);
size_t msx_psg_available_samples(const MsxPsgState* state);
