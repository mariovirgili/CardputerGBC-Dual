#pragma once

#include <stdint.h>

#include "coleco_memory.h"

enum class ColecoCpuRunState : uint8_t {
    Running = 0,
    Halted,
    Unsupported,
    Faulted,
};

struct ColecoCpuState {
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
    uint8_t eiDelay; // 0=off, 1=enable after current step, 2=skip one instruction after EI
    bool halted;
    bool irqPending;
    bool nmiPending;
    ColecoCpuRunState runState;
};

void coleco_cpu_init(ColecoCpuState* state);
void coleco_cpu_reset(ColecoCpuState* state, uint16_t resetPc, uint16_t resetSp);
void coleco_cpu_request_irq(ColecoCpuState* state);
int coleco_cpu_run_cycles(ColecoCpuState* state, ColecoMemoryState* memory, int cycleBudget);
const char* coleco_cpu_run_state_label(ColecoCpuRunState state);
