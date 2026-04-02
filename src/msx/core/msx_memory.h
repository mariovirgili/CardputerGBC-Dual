#pragma once

#include <stddef.h>
#include <stdint.h>

#include "msx_bios.h"
#include "msx_cart.h"
#include "msx_disk.h"
#include "msx_keyboard.h"

struct MsxPsgState;
struct MsxVdpState;

struct MsxMemoryState {
    MsxMachineMode machineMode;
    MsxBiosState bios;
    MsxCartState cart;
    MsxKeyboardState keyboard;
    MsxVdpState* vdp;
    MsxPsgState* psg;
    MsxDiskState* disk;
    const uint8_t* diskRom;
    size_t diskRomSize;
    uint8_t* ramBanks[16];
    size_t ramSize;
    uint8_t ramSegmentCount;
    uint8_t ramBankCount;
    uint8_t mapperRegisters[4];
    const uint8_t* readMap[8];
    uint8_t* writeMap[8];
    uint8_t slotRegister;
    uint8_t ppiPortC;
    uint8_t lastPort98;
    uint8_t lastPort99;
    uint8_t lastPort9A;
    uint8_t lastPort9B;
    uint8_t lastPortA0;
    uint8_t lastPortA1;
    uint8_t lastPortA8;
    uint8_t lastPortAA;
    uint32_t ioWriteCount;
    bool mapperEnabled;
    bool ready;
};

bool msx_memory_init(MsxMemoryState* state,
                     MsxMachineMode machineMode,
                     const MsxBiosState* bios,
                     const MsxCartState* cart,
                     size_t requestedRamSize);
void msx_memory_attach_vdp(MsxMemoryState* state, MsxVdpState* vdp);
void msx_memory_attach_psg(MsxMemoryState* state, MsxPsgState* psg);
void msx_memory_set_keyboard_matrix(MsxMemoryState* state, const MsxKeyboardMatrix* matrix);
void msx_memory_shutdown(MsxMemoryState* state);
void msx_memory_reset(MsxMemoryState* state);
void msx_memory_refresh_maps(MsxMemoryState* state);
uint8_t msx_memory_read8(const MsxMemoryState* state, uint16_t address);
uint16_t msx_memory_read16(const MsxMemoryState* state, uint16_t address);
void msx_memory_write8(MsxMemoryState* state, uint16_t address, uint8_t value);
void msx_memory_write16(MsxMemoryState* state, uint16_t address, uint16_t value);
uint8_t msx_memory_in(MsxMemoryState* state, uint8_t port);
void msx_memory_out(MsxMemoryState* state, uint8_t port, uint8_t value);

