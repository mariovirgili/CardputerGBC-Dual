#pragma once

#include <stddef.h>
#include <stdint.h>

#include "msx_bios.h"
#include "msx_cart.h"
#include "msx_disk.h"
#include "msx_keyboard.h"
#include "../msx_config.h"

struct MsxCpuState;
struct MsxPsgState;
struct MsxSccState;
struct MsxVdpState;

// Callback invoked when the CPU executes the ED FE opcode (BIOS software trap).
// patchAddress is the address of the ED instruction (= the patched DISK ROM entry point).
typedef void (*MsxDiskPatchFn)(struct MsxCpuState* cpu,
                               struct MsxMemoryState* memory,
                               uint16_t patchAddress);

struct MsxCasState;

struct MsxMemoryState {
    MsxMachineMode machineMode;
    MsxBiosState bios;
    MsxCartState cart;
    MsxKeyboardState keyboard;
    const MsxCpuState* cpu;
    MsxVdpState* vdp;
    MsxPsgState* psg;
    MsxDiskState* disk;
    MsxCasState* cas;
    MsxDiskPatchFn diskPatch;
    const uint8_t* diskRom;
    size_t diskRomSize;
    uint8_t* ramBanks[16];
    uint8_t* ramBanksDynamicBase;
    size_t ramBanksDynamicSize;
    size_t ramSize;
    uint8_t ramSegmentCount;
    uint8_t ramSegmentMask;
    uint8_t ramBankCount;
    uint8_t mapperRegisters[4];
    const uint8_t* readMap[8];
    uint8_t* writeMap[8];
    uint32_t mapEpoch;
    uint8_t slotRegister;
    uint8_t secondarySlotRegs[4];  // SSLReg[primary slot], read/written via 0xFFFF using page3 slot
    uint8_t rtcRegisterSelect;
    uint8_t rtcModeReg;
    uint8_t rtcNvram[26];
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
    bool ramSegmentCountPowerOfTwo;
    bool slot3Expanded;
    bool cartBootWorkareaFallbackArmed;
    bool cartBootMappingRestoreArmed;
    bool ready;
};

inline uint8_t msx_memory_wrap_ram_segment(const MsxMemoryState* state, uint8_t segment)
{
    if (!state || state->ramSegmentCount == 0u) {
        return 0u;
    }

    if (state->ramSegmentCountPowerOfTwo) {
        return static_cast<uint8_t>(segment & state->ramSegmentMask);
    }

    return static_cast<uint8_t>(segment % state->ramSegmentCount);
}

bool msx_memory_init(MsxMemoryState* state,
                     MsxMachineMode machineMode,
                     const MsxBiosState* bios,
                     const MsxCartState* cart,
                     size_t requestedRamSize);
void msx_memory_attach_vdp(MsxMemoryState* state, MsxVdpState* vdp);
void msx_memory_attach_psg(MsxMemoryState* state, MsxPsgState* psg);
void msx_memory_attach_scc(MsxMemoryState* state, MsxSccState* scc);
MsxSccState* msx_memory_get_scc(MsxMemoryState* state);
void msx_memory_set_virtual_scc_mode(MsxMemoryState* state, MsxVirtualSccMode mode);
void msx_memory_set_keyboard_matrix(MsxMemoryState* state, const MsxKeyboardMatrix* matrix);
void msx_memory_shutdown(MsxMemoryState* state);
void msx_memory_reset(MsxMemoryState* state);
void msx_memory_refresh_maps(MsxMemoryState* state);
bool msx_memory_use_raw_page3_window(uint16_t address);
bool msx_memory_is_open_bus_fetch(const MsxMemoryState* state, uint16_t address);
bool msx_memory_try_cart_header_mirror_read(const MsxMemoryState* state,
                                            uint16_t address,
                                            uint8_t* value);
uint8_t msx_memory_read8(const MsxMemoryState* state, uint16_t address);
uint16_t msx_memory_read16(const MsxMemoryState* state, uint16_t address);
void msx_memory_write8(MsxMemoryState* state, uint16_t address, uint8_t value);
void msx_memory_write16(MsxMemoryState* state, uint16_t address, uint16_t value);
uint8_t msx_memory_in(MsxMemoryState* state, uint8_t port);
void msx_memory_out(MsxMemoryState* state, uint8_t port, uint8_t value);

