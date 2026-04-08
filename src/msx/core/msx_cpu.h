#pragma once

#include <stdint.h>

#include "msx_memory.h"

enum class MsxCpuRunState : uint8_t {
    Running = 0,
    Halted,
    Unsupported,
    Faulted,
};

struct MsxCpuState {
    uint16_t af;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;
    uint16_t af2;
    uint16_t bc2;
    uint16_t de2;
    uint16_t hl2;
    uint16_t ix;
    uint16_t iy;
    uint16_t sp;
    uint16_t pc;
    uint8_t i;
    uint8_t r;
    uint8_t im;
    uint8_t lastOpcode;
    uint8_t unsupportedOpcode;
    uint16_t lastPc;
    uint16_t unsupportedPc;
    uint32_t totalCycles;
    bool iff1;
    bool iff2;
    bool eiDelay;   // true for one instruction after EI before IFF1 becomes active
    bool halted;
    bool irqPending;
    MsxCpuRunState runState;
};

void msx_cpu_init(MsxCpuState* state);
void msx_cpu_reset(MsxCpuState* state, uint16_t resetPc, uint16_t resetSp);
void msx_cpu_request_irq(MsxCpuState* state);
int msx_cpu_run_cycles(MsxCpuState* state, MsxMemoryState* memory, int cycleBudget);
const char* msx_cpu_run_state_label(MsxCpuRunState state);
