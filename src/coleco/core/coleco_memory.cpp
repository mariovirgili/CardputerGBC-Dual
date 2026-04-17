#include "coleco_memory.h"
#include "coleco_vdp.h"
#include "SN76489.h"

#include <cstring>
#include <cstdio>

bool coleco_memory_init(ColecoMemoryState* state, const uint8_t* biosRom, size_t biosSize, const uint8_t* cartRom, size_t cartSize)
{
    if (!state) return false;

    std::memset(state, 0, sizeof(*state));
    state->biosRom = biosRom;
    state->biosRomSize = biosSize;
    state->cartRom = cartRom;
    state->cartRomSize = cartSize;
    state->joyMode = false;
    state->joyState[0] = 0xFFFFu;
    state->joyState[1] = 0xFFFFu;
    state->ready = true;

    return true;
}

void coleco_memory_attach_vdp(ColecoMemoryState* state, ColecoVdpState* vdp)
{
    if (state) state->vdp = vdp;
}

void coleco_memory_attach_psg(ColecoMemoryState* state, SN76489* psg)
{
    if (state) state->psg = psg;
}

void coleco_memory_shutdown(ColecoMemoryState* state)
{
    if (state) {
        state->ready = false;
    }
}

void coleco_memory_reset(ColecoMemoryState* state)
{
    if (!state) return;
    
    std::memset(state->ram, 0, sizeof(state->ram));
    state->joyMode = false;
    state->joyState[0] = 0xFFFFu;
    state->joyState[1] = 0xFFFFu;
}

uint8_t coleco_memory_read8(const ColecoMemoryState* state, uint16_t address)
{
    if (address < 0x2000) {
        if (state->biosRom && address < state->biosRomSize) {
            return state->biosRom[address];
        }
        return 0xFF;
    }
    else if (address >= 0x6000 && address < 0x8000) {
        return state->ram[address & 0x3FF]; // 1KB mirrored
    }
    else if (address >= 0x8000) {
        if (state->cartRom) {
            return state->cartRom[(address - 0x8000) % state->cartRomSize];
        }
        return 0xFF;
    }
    
    return 0xFF; // Expansion space 0x2000-0x5FFF usually returns 0xFF
}

uint16_t coleco_memory_read16(const ColecoMemoryState* state, uint16_t address)
{
    uint16_t value = coleco_memory_read8(state, address);
    value |= (uint16_t)coleco_memory_read8(state, address + 1) << 8;
    return value;
}

void coleco_memory_write8(ColecoMemoryState* state, uint16_t address, uint8_t value)
{
    if (address >= 0x6000 && address < 0x8000) {
        state->ram[address & 0x3FF] = value;
    }
    // ROM is read-only
}

void coleco_memory_write16(ColecoMemoryState* state, uint16_t address, uint16_t value)
{
    coleco_memory_write8(state, address, value & 0xFF);
    coleco_memory_write8(state, address + 1, value >> 8);
}

uint8_t coleco_memory_in(ColecoMemoryState* state, uint8_t port)
{
    static const uint8_t KeyCodes[16] = {
        0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05,
        0x01, 0x0B, 0x06, 0x09, 0x08, 0x04, 0x0F, 0x0F
    };

    switch (port & 0xE0) {
        case 0xE0: { // Joysticks Data
            int joyIndex = (port >> 1) & 0x01;
            uint16_t jstate = state->joyState[joyIndex];
            uint8_t result = state->joyMode ? (jstate >> 8) : ((jstate & 0xF0) | KeyCodes[jstate & 0x0F]);
            return (result | 0xB0) & 0x7F;
        }
        case 0xA0: // VDP Status/Data
            if (state->vdp) {
                if (port & 0x01) {
                    return coleco_vdp_in_status(state->vdp);
                } else {
                    return coleco_vdp_in_data(state->vdp);
                }
            }
            return 0xFF;
    }
    
    return 0xFF;
}

void coleco_memory_out(ColecoMemoryState* state, uint8_t port, uint8_t value)
{
    switch (port & 0xE0) {
        case 0x80:
            state->joyMode = false;
            break;
        case 0xC0:
            state->joyMode = true;
            break;
        case 0xE0:
            if (state->psg) {
                Write76489(state->psg, value);
            }
            break;
        case 0xA0: // VDP
            if (state->vdp) {
                if (port & 0x01) {
                    coleco_vdp_out_control(state->vdp, value);
                } else {
                    coleco_vdp_out_data(state->vdp, value);
                }
            }
            break;
    }
}
