#pragma once

#include <stddef.h>
#include <stdint.h>

#include "SN76489.h"

struct ColecoCpuState;
struct ColecoVdpState;

struct ColecoMemoryState {
    const uint8_t* biosRom;
    size_t biosRomSize;
    const uint8_t* cartRom;
    size_t cartRomSize;
    uint8_t ram[1024]; // 1KB System RAM

    ColecoVdpState* vdp;
    SN76489* psg;
    
    uint16_t joyState[2];
    bool joyMode; // 0 = keypad, 1 = joystick
    
    bool ready;
};

bool coleco_memory_init(ColecoMemoryState* state, const uint8_t* biosRom, size_t biosSize, const uint8_t* cartRom, size_t cartSize);
void coleco_memory_attach_vdp(ColecoMemoryState* state, ColecoVdpState* vdp);
void coleco_memory_attach_psg(ColecoMemoryState* state, SN76489* psg);
void coleco_memory_shutdown(ColecoMemoryState* state);
void coleco_memory_reset(ColecoMemoryState* state);

uint8_t coleco_memory_read8(const ColecoMemoryState* state, uint16_t address);
uint16_t coleco_memory_read16(const ColecoMemoryState* state, uint16_t address);
void coleco_memory_write8(ColecoMemoryState* state, uint16_t address, uint8_t value);
void coleco_memory_write16(ColecoMemoryState* state, uint16_t address, uint16_t value);

uint8_t coleco_memory_in(ColecoMemoryState* state, uint8_t port);
void coleco_memory_out(ColecoMemoryState* state, uint8_t port, uint8_t value);
