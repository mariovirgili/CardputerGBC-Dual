#pragma once

#include <stddef.h>
#include <stdint.h>

struct MsxSccState {
    uint8_t regs[256];
    uint32_t phase[5];
    uint32_t step[5];
    uint32_t sampleRate;
    uint32_t cpuClockHz;
    uint64_t sampleAccumulator;
    uint16_t ringReadIndex;
    uint16_t ringWriteIndex;
    uint16_t ringCount;
    uint32_t generatedSamples;
    uint32_t droppedSamples;
    int16_t* ring;
    bool classicWindow;
    bool plusWindow;
    bool sccPlusMode;
    bool enabled;
    bool ready;
};

bool msx_scc_init(MsxSccState* state, uint32_t sampleRate);
void msx_scc_reset(MsxSccState* state);
void msx_scc_shutdown(MsxSccState* state);
void msx_scc_set_windows(MsxSccState* state, bool classicWindow, bool plusWindow);
void msx_scc_write(MsxSccState* state, uint8_t reg, uint8_t value);
void msx_scc_write_plus(MsxSccState* state, uint8_t reg, uint8_t value);
uint8_t msx_scc_read(const MsxSccState* state, uint8_t reg);
uint8_t msx_scc_read_plus(const MsxSccState* state, uint8_t reg);
void msx_scc_run_cycles(MsxSccState* state, uint32_t cpuCycles);
size_t msx_scc_read_samples(MsxSccState* state, int16_t* dst, size_t maxSamples);
void msx_scc_discard_samples(MsxSccState* state, size_t sampleCount);
size_t msx_scc_available_samples(const MsxSccState* state);
