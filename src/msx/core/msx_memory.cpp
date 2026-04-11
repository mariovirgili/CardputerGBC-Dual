#include "msx_memory.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../../share/emu_static_pool.h"
#include "msx_disk.h"
#include "msx_psg.h"
#include "msx_vdp.h"

#ifndef MSX_MEMORY_TRACE_ENABLED
#define MSX_MEMORY_TRACE_ENABLED 0
#endif

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;
constexpr size_t kMsxPageSize16K = 0x4000;
constexpr size_t kMsxRamSizeMsx1 = 0x10000;
constexpr size_t kMsxRamSizeMsx2 = 0x20000;
constexpr uint8_t kMsxMaxRamSegments = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize16K);
constexpr uint8_t kMsxMaxRamBanks = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize8K);
constexpr uint8_t kMsxDefaultSlotRegister = 0x00;  // all pages → slot0 (BIOS); BIOS probe sets final value
constexpr uint8_t kMsxStaticBankCountMsx1 = 4u;
static uint8_t s_openBusPage[kMsxPageSize8K];
static bool s_openBusInitialized = false;
static uint8_t* const s_msx1StaticRamBanks[kMsxStaticBankCountMsx1] = {
    g_emu_static_pool + (0u * kMsxPageSize8K),
    g_emu_static_pool + (1u * kMsxPageSize8K),
    g_emu_static_pool + (2u * kMsxPageSize8K),
    g_emu_static_pool + (3u * kMsxPageSize8K),
};

constexpr uint8_t kMsxNoramByte = 0xFFu;
constexpr uint16_t kMsxAddrSlttbl = 0xFCC5u;
constexpr uint16_t kMsxAddrPage3RuntimeRamBegin = 0xF000u;
constexpr uint16_t kMsxAddrPage3ProbeWindowBegin = 0xFE00u;
constexpr uint16_t kMsxAddrPage3ProbeWindowEnd = 0xFE30u;
constexpr uint16_t kMsxAddrCartInitLo = 0xF7C5u;
constexpr uint16_t kMsxAddrCartInitHi = 0xF7C6u;
// Cart lives in primary slot 1 (standard MSX topology).
// The earlier slot-2 experiment was a misread of the BIOS trace: the CALLF
// extension-ROM scan switched page3 into slot 2 to test it, not to access
// the cart body.  Slot 2 = open bus (0xFF).
constexpr uint8_t kMsxPrimarySlotCartridge = 1u;
constexpr uint16_t kMsxCartHeaderMirrorSize = 0x0010u;
constexpr uint8_t kMsxPrimarySlotExpanded = 3u;

uint8_t msx_slot_for_page(uint8_t slotRegister, uint8_t pageIndex)
{
    return static_cast<uint8_t>((slotRegister >> (pageIndex * 2u)) & 0x03u);
}

uint8_t msx_secondary_slot_for_page(uint8_t secondarySlotReg, uint8_t pageIndex)
{
    return static_cast<uint8_t>((secondarySlotReg >> (pageIndex * 2u)) & 0x03u);
}

uint8_t msx_memory_effective_secondary_slot_reg(const MsxMemoryState* state, uint8_t primarySlot)
{
    if (!state || primarySlot >= 4u) {
        return 0u;
    }

    // In the current MSX1 model only primary slot 3 is expanded.
    if (primarySlot != kMsxPrimarySlotExpanded) {
        return 0u;
    }

    return state->secondarySlotRegs[primarySlot];
}

bool msx_memory_has_mapper(const MsxMemoryState* state)
{
    return state && state->mapperEnabled && state->ramSegmentCount > 4u;
}

uint8_t msx_memory_ram_segment(const MsxMemoryState* state, uint8_t pageIndex)
{
    if (!state || state->ramSegmentCount == 0u) {
        return 0u;
    }

    if (msx_memory_has_mapper(state)) {
        return static_cast<uint8_t>(state->mapperRegisters[pageIndex & 0x03u] % state->ramSegmentCount);
    }

    return static_cast<uint8_t>(pageIndex % state->ramSegmentCount);
}

bool msx_memory_is_static_bank(const uint8_t* ptr)
{
    if (!ptr) {
        return false;
    }

    for (uint8_t i = 0; i < kMsxStaticBankCountMsx1; ++i) {
        if (ptr == s_msx1StaticRamBanks[i]) {
            return true;
        }
    }

    return false;
}

void msx_memory_bind_open_bus(MsxMemoryState* state, uint8_t bank)
{
    state->readMap[bank] = s_openBusPage;
    state->writeMap[bank] = nullptr;
}

void msx_memory_bind_bios(MsxMemoryState* state, uint8_t pageIndex, uint8_t bank)
{
    const uint8_t subPage = static_cast<uint8_t>(bank & 0x01u);
    const uint8_t* ptr = msx_bios_page_ptr(&state->bios, pageIndex, subPage);
    if (!ptr) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    state->readMap[bank] = ptr;
    state->writeMap[bank] = nullptr;
}

void msx_memory_bind_cart(MsxMemoryState* state, uint8_t pageIndex, uint8_t bank)
{
    // Standard MSX cartridge topology only exposes cart ROM at 4000-BFFF.
    // Page0 and page3 stay open bus even when the cartridge is selected in
    // primary slot 1, which is required for CALLF/CALLSL behavior.
    if ((pageIndex == 0u) || (pageIndex == 3u)) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    uint8_t windowIndex = 0;
    if (pageIndex == 1u) {
        windowIndex = static_cast<uint8_t>(bank & 0x01u);
    } else {
        windowIndex = static_cast<uint8_t>(2u + (bank & 0x01u));
    }

    const uint8_t* ptr = msx_cart_window_ptr(&state->cart, windowIndex);
    if (!ptr) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    state->readMap[bank] = ptr;
    state->writeMap[bank] = nullptr;
}

void msx_memory_bind_ram(MsxMemoryState* state, uint8_t pageIndex, uint8_t bank)
{
    const uint8_t segment = msx_memory_ram_segment(state, pageIndex);
    const uint8_t subPage = static_cast<uint8_t>(bank & 0x01u);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if (segment >= state->ramSegmentCount || bankIndex >= state->ramBankCount || bankIndex >= kMsxMaxRamBanks) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    uint8_t* bankPtr = state->ramBanks[bankIndex];
    if (!bankPtr) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    state->readMap[bank] = bankPtr;
    state->writeMap[bank] = bankPtr;
}

void msx_memory_bind_disk_rom(MsxMemoryState* state, uint8_t bank)
{
    if (!state->diskRom || state->diskRomSize == 0u) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    const uint8_t subPage = static_cast<uint8_t>(bank & 0x01u);
    const size_t offset = static_cast<size_t>(subPage) * kMsxPageSize8K;
    if (offset + kMsxPageSize8K > state->diskRomSize) {
        msx_memory_bind_open_bus(state, bank);
        return;
    }

    state->readMap[bank] = state->diskRom + offset;
    state->writeMap[bank] = nullptr;
}

// Dispatch a single 8K bank within primary slot 3 using the secondary slot register.
// Sub-slot layout mirrors fMSX: subslot1 = DiskROM (page1 only), subslot2 = RAM.
void msx_memory_bind_slot3(MsxMemoryState* state, uint8_t pageIndex, uint8_t bank)
{
    const uint8_t subSlot = msx_secondary_slot_for_page(
        msx_memory_effective_secondary_slot_reg(state, 3u),
        pageIndex
    );
    switch (subSlot) {
        case 1u:
            // DiskROM lives at page1 (0x4000-0x7FFF) only
            if (pageIndex == 1u) {
                msx_memory_bind_disk_rom(state, bank);
            } else {
                msx_memory_bind_open_bus(state, bank);
            }
            break;
        case 2u:
            msx_memory_bind_ram(state, pageIndex, bank);
            break;
        default:
            msx_memory_bind_open_bus(state, bank);
            break;
    }
}

uint8_t msx_memory_page3_primary_slot(const MsxMemoryState* state)
{
    return state ? msx_slot_for_page(state->slotRegister, 3u) : 0u;
}

bool msx_memory_use_raw_page3_window_impl(uint16_t address)
{
    if ((address >> 14) != 3u) {
        return false;
    }

    // Only the upper BIOS work area/stack/hook region needs to stay anchored
    // to RAM when CALLF temporarily remaps page 3.
    if (address < kMsxAddrPage3RuntimeRamBegin) {
        return false;
    }

    // Leave the FE00h probe window mapped to the selected slot so the RAM/slot
    // test still sees the real target hardware.
    if ((address >= kMsxAddrPage3ProbeWindowBegin) && (address < kMsxAddrPage3ProbeWindowEnd)) {
        return false;
    }

    return address != 0xFFFFu;
}

// Reads a byte from the slot3:subslot2 RAM that backs page3, ignoring the
// current slot-register mapping. Used so BIOS work-area accesses still hit RAM
// even after CALLF or slot probing has switched page3 somewhere else.
static uint8_t msx_memory_raw_page3_read8(const MsxMemoryState* state, uint16_t address)
{
    if (!state || !state->ready || state->ramSegmentCount == 0u) {
        return 0xFFu;
    }
    const uint8_t segment = msx_memory_ram_segment(state, 3u);
    const uint8_t subPage = static_cast<uint8_t>((address >> 13) & 0x01u);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if (segment >= state->ramSegmentCount ||
        bankIndex >= state->ramBankCount ||
        bankIndex >= kMsxMaxRamBanks ||
        !state->ramBanks[bankIndex]) {
        return 0xFFu;
    }
    return state->ramBanks[bankIndex][address & 0x1FFFu];
}

void msx_memory_raw_page3_write8(MsxMemoryState* state, uint16_t address, uint8_t value)
{
    if (!state || !state->ready || ((address >> 14) != 3u) || (state->ramSegmentCount == 0u)) {
        return;
    }

    const uint8_t segment = msx_memory_ram_segment(state, 3u);
    const uint8_t subPage = static_cast<uint8_t>((address >> 13) & 0x01u);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if ((segment >= state->ramSegmentCount) ||
        (bankIndex >= state->ramBankCount) ||
        (bankIndex >= kMsxMaxRamBanks) ||
        !state->ramBanks[bankIndex]) {
        return;
    }

    state->ramBanks[bankIndex][address & 0x1FFFu] = value;
}

bool msx_memory_try_cart_workarea_fallback_read(const MsxMemoryState* state,
                                                uint16_t address,
                                                uint8_t* value)
{
    if (!state || !state->ready || !value) {
        return false;
    }

    if ((address != kMsxAddrCartInitLo) && (address != kMsxAddrCartInitHi)) {
        return false;
    }

    const MsxCartState* const cart = &state->cart;
    if (!cart->ready || !cart->directBootCandidate || !state->cartBootWorkareaFallbackArmed) {
        return false;
    }

    if ((cart->initAddress < 0x4000u) || (cart->initAddress >= 0xC000u)) {
        return false;
    }

    const uint8_t rawLo = msx_memory_raw_page3_read8(state, kMsxAddrCartInitLo);
    const uint8_t rawHi = msx_memory_raw_page3_read8(state, kMsxAddrCartInitHi);
    const uint16_t rawInit =
        static_cast<uint16_t>(rawLo | (static_cast<uint16_t>(rawHi) << 8));
    if ((rawInit >= 0x4000u) && (rawInit < 0xC000u)) {
        return false;
    }

    *value = (address == kMsxAddrCartInitLo)
                 ? static_cast<uint8_t>(cart->initAddress & 0x00FFu)
                 : static_cast<uint8_t>(cart->initAddress >> 8);

    return true;
}

void msx_memory_release_ram_banks(MsxMemoryState* state)
{
    if (!state) {
        return;
    }

    for (uint8_t i = 0; i < kMsxMaxRamBanks; ++i) {
        if (state->ramBanks[i] && !msx_memory_is_static_bank(state->ramBanks[i])) {
            heap_caps_free(state->ramBanks[i]);
        }
        state->ramBanks[i] = nullptr;
    }
}

bool msx_memory_allocate_ram_banks(MsxMemoryState* state)
{
    if (!state) {
        return false;
    }

    uint8_t firstDynamicIndex = 0u;
    if (state->machineMode != MsxMachineMode::MSX2) {
        const uint8_t staticBankCount = (state->ramBankCount >= kMsxStaticBankCountMsx1)
                                            ? kMsxStaticBankCountMsx1
                                            : state->ramBankCount;
        for (uint8_t i = 0; i < staticBankCount; ++i) {
            state->ramBanks[i] = s_msx1StaticRamBanks[i];
            std::memset(state->ramBanks[i], kMsxNoramByte, kMsxPageSize8K);
        }
        firstDynamicIndex = staticBankCount;
    }

    for (uint8_t i = firstDynamicIndex; i < state->ramBankCount; ++i) {
        state->ramBanks[i] = static_cast<uint8_t*>(heap_caps_malloc(
            kMsxPageSize8K,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        ));
        if (!state->ramBanks[i]) {
            state->ramBanks[i] = static_cast<uint8_t*>(heap_caps_malloc(
                kMsxPageSize8K,
                MALLOC_CAP_8BIT
            ));
        }

        if (!state->ramBanks[i]) {
            std::printf("[MSX] memory init: ram bank alloc failed index=%u size=%u free8=%u freeInternal=%u largest8=%u largestInternal=%u\n",
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(kMsxPageSize8K),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            msx_memory_release_ram_banks(state);
            return false;
        }

        std::memset(state->ramBanks[i], kMsxNoramByte, kMsxPageSize8K);
    }

    return true;
}

void msx_memory_clear_ram_banks(MsxMemoryState* state)
{
    if (!state) {
        return;
    }

    for (uint8_t i = 0; i < state->ramBankCount; ++i) {
        if (state->ramBanks[i]) {
            std::memset(state->ramBanks[i], kMsxNoramByte, kMsxPageSize8K);
        }
    }
}

} // namespace

bool msx_memory_use_raw_page3_window(uint16_t address)
{
    return msx_memory_use_raw_page3_window_impl(address);
}

bool msx_memory_is_open_bus_fetch(const MsxMemoryState* state, uint16_t address)
{
    if (!state || !state->ready) {
        return false;
    }

    // Page3 runtime RAM can intentionally override the visible slot mapping
    // during CALLF; treat it as executable RAM, not open bus.
    if (msx_memory_use_raw_page3_window_impl(address)) {
        return false;
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    return state->readMap[bank] == s_openBusPage;
}

bool msx_memory_try_cart_header_mirror_read(const MsxMemoryState* state,
                                            uint16_t address,
                                            uint8_t* value)
{
    if (!state || !state->ready || !value) {
        return false;
    }

    if (address >= kMsxCartHeaderMirrorSize) {
        return false;
    }

    if (msx_slot_for_page(state->slotRegister, 0u) != kMsxPrimarySlotCartridge) {
        return false;
    }

    const MsxCartState* const cart = &state->cart;
    if (!cart->ready || !cart->rom) {
        return false;
    }

    if ((cart->type != MsxCartridgeType::Plain16K) &&
        (cart->type != MsxCartridgeType::Plain32K)) {
        return false;
    }

    if (cart->headerOffset >= cart->size) {
        return false;
    }

    const size_t sourceOffset = cart->headerOffset + static_cast<size_t>(address);
    if (sourceOffset >= cart->size) {
        return false;
    }

    *value = cart->rom[sourceOffset];
    return true;
}

bool msx_memory_init(MsxMemoryState* state,
                     MsxMachineMode machineMode,
                     const MsxBiosState* bios,
                     const MsxCartState* cart,
                     size_t requestedRamSize)
{
    if (!state || !bios || !cart || !bios->ready) {
        return false;
    }

    if (!s_openBusInitialized) {
        std::memset(s_openBusPage, 0xFF, sizeof(s_openBusPage));
        s_openBusInitialized = true;
    }

    std::memset(state, 0, sizeof(*state));
    state->machineMode = machineMode;
    state->bios = *bios;
    state->cart = *cart;

    const size_t defaultRamSize = (machineMode == MsxMachineMode::MSX2) ? kMsxRamSizeMsx2 : kMsxRamSizeMsx1;
    size_t ramSize = (requestedRamSize != 0u) ? requestedRamSize : defaultRamSize;
    if (ramSize > defaultRamSize) {
        ramSize = defaultRamSize;
    }
    ramSize &= ~(kMsxPageSize16K - 1u);
    if (ramSize < kMsxPageSize16K) {
        ramSize = kMsxPageSize16K;
    }

    state->ramSize = ramSize;
    state->ramSegmentCount = static_cast<uint8_t>(state->ramSize / kMsxPageSize16K);
    state->ramBankCount = static_cast<uint8_t>(state->ramSize / kMsxPageSize8K);
    state->mapperEnabled = (machineMode == MsxMachineMode::MSX2) && state->ramSegmentCount > 4u;

    if (!msx_memory_allocate_ram_banks(state)) {
        std::memset(state, 0, sizeof(*state));
        return false;
    }

    msx_keyboard_init(&state->keyboard);
    state->slotRegister = kMsxDefaultSlotRegister;
    std::memset(state->secondarySlotRegs, 0, sizeof(state->secondarySlotRegs));
    state->ppiPortC = 0u;
    state->lastPortA8 = state->slotRegister;
    state->lastPortAA = state->ppiPortC;
    state->ready = true;
    msx_memory_reset(state);
    return true;
}

void msx_memory_attach_vdp(MsxMemoryState* state, MsxVdpState* vdp)
{
    if (!state) {
        return;
    }

    state->vdp = vdp;
}

void msx_memory_attach_psg(MsxMemoryState* state, MsxPsgState* psg)
{
    if (!state) {
        return;
    }

    state->psg = psg;
}

void msx_memory_set_keyboard_matrix(MsxMemoryState* state, const MsxKeyboardMatrix* matrix)
{
    if (!state || !state->ready || !matrix) {
        return;
    }

    msx_keyboard_set_matrix(&state->keyboard, matrix);
}

void msx_memory_shutdown(MsxMemoryState* state)
{
    if (!state) {
        return;
    }

    msx_memory_release_ram_banks(state);
    std::memset(state, 0, sizeof(*state));
}

void msx_memory_reset(MsxMemoryState* state)
{
    if (!state || !state->ready) {
        return;
    }

    msx_memory_clear_ram_banks(state);
    msx_cart_reset(&state->cart);
    msx_keyboard_reset(&state->keyboard);
    if (state->psg) {
        msx_psg_reset(state->psg);
    }

    state->slotRegister = kMsxDefaultSlotRegister;
    std::memset(state->secondarySlotRegs, 0, sizeof(state->secondarySlotRegs));
    state->ppiPortC = 0u;
    state->lastPort98 = 0u;
    state->lastPort99 = 0u;
    state->lastPort9A = 0u;
    state->lastPort9B = 0u;
    state->lastPortA0 = 0u;
    state->lastPortA1 = 0u;
    state->lastPortA8 = state->slotRegister;
    state->lastPortAA = state->ppiPortC;
    state->ioWriteCount = 0u;
    state->cartBootWorkareaFallbackArmed = false;
    state->cartBootMappingRestoreArmed = false;

    if (msx_memory_has_mapper(state)) {
        state->mapperRegisters[0] = 0u;
        state->mapperRegisters[1] = 1u;
        state->mapperRegisters[2] = 2u;
        state->mapperRegisters[3] = 3u;
    } else {
        std::memset(state->mapperRegisters, 0, sizeof(state->mapperRegisters));
    }

    msx_memory_refresh_maps(state);
}

void msx_memory_refresh_maps(MsxMemoryState* state)
{
    if (!state || !state->ready) {
        return;
    }

    for (uint8_t page = 0; page < 4u; ++page) {
        const uint8_t slot = msx_slot_for_page(state->slotRegister, page);
        for (uint8_t subPage = 0; subPage < 2u; ++subPage) {
            const uint8_t bank = static_cast<uint8_t>(page * 2u + subPage);
            switch (slot) {
                case 0:
                    msx_memory_bind_bios(state, page, bank);
                    break;
                case 1:
                    msx_memory_bind_cart(state, page, bank);
                    break;
                case 2:
                    msx_memory_bind_open_bus(state, bank);
                    break;
                case 3:
                    // Primary slot 3 is expanded: sub-slots dispatched via secondarySlotReg
                    msx_memory_bind_slot3(state, page, bank);
                    break;
                default:
                    msx_memory_bind_open_bus(state, bank);
                    break;
            }
        }
    }
}

uint8_t msx_memory_read8(const MsxMemoryState* state, uint16_t address)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }

    uint8_t mirroredValue = 0xFFu;
    if (msx_memory_try_cart_header_mirror_read(state, address, &mirroredValue)) {
        return mirroredValue;
    }

    // Secondary slot register: 0xFFFF always reads ~SSLReg[slot in page3] (fMSX RdZ80)
    if (address == 0xFFFFu) {
        const uint8_t page3Slot = msx_memory_page3_primary_slot(state);
        const uint8_t value = static_cast<uint8_t>(~msx_memory_effective_secondary_slot_reg(state, page3Slot));
        return value;
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);

    uint8_t workareaValue = 0xFFu;
    if (msx_memory_try_cart_workarea_fallback_read(state, address, &workareaValue)) {
        return workareaValue;
    }

    // During CALLF, page3 can be switched to an extension slot while the BIOS
    // still needs the upper work area and stack to live in RAM. Keep that
    // runtime RAM slice raw, but leave the FE00 probe window mapped so the
    // BIOS slot test still sees the real target slot.
    if (msx_memory_use_raw_page3_window_impl(address)) {
        return msx_memory_raw_page3_read8(state, address);
    }

    return state->readMap[bank][offset];
}

uint16_t msx_memory_read16(const MsxMemoryState* state, uint16_t address)
{
    const uint8_t lo = msx_memory_read8(state, address);
    const uint8_t hi = msx_memory_read8(state, static_cast<uint16_t>(address + 1u));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

void msx_memory_write8(MsxMemoryState* state, uint16_t address, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    // Secondary slot register: 0xFFFF write always goes to SSLReg[slot in page3] (fMSX WrZ80)
    if (address == 0xFFFFu) {
        const uint8_t page3Slot = msx_memory_page3_primary_slot(state);
        uint8_t coercedValue = value;
        if (page3Slot != 3u) {
            coercedValue = 0u;
        }

        state->secondarySlotRegs[page3Slot] = coercedValue;
        msx_memory_raw_page3_write8(state,
                                    static_cast<uint16_t>(kMsxAddrSlttbl + page3Slot),
                                    coercedValue);

        if (page3Slot == 3u) {
            msx_memory_refresh_maps(state);
        }
        return;
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    if (msx_memory_use_raw_page3_window_impl(address)) {
        msx_memory_raw_page3_write8(state, address, value);
        return;
    }

    const uint8_t page = static_cast<uint8_t>(address >> 14);
    const uint8_t slot = msx_slot_for_page(state->slotRegister, page);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);

    if (state->writeMap[bank]) {
        state->writeMap[bank][offset] = value;
        return;
    }

    if (slot == kMsxPrimarySlotCartridge) {
        msx_cart_write(&state->cart, address, value);
        msx_memory_refresh_maps(state);
    }
}

void msx_memory_write16(MsxMemoryState* state, uint16_t address, uint16_t value)
{
    msx_memory_write8(state, address, static_cast<uint8_t>(value & 0x00FFu));
    msx_memory_write8(state, static_cast<uint16_t>(address + 1u), static_cast<uint8_t>(value >> 8));
}

uint8_t msx_memory_in(MsxMemoryState* state, uint8_t port)
{
    switch (port) {
        case 0x98: {
            const uint8_t value = (state && state->vdp) ? msx_vdp_in_data(state->vdp) : 0xFFu;
            return value;
        }
        case 0x99: {
            const uint8_t value = (state && state->vdp) ? msx_vdp_in_status(state->vdp) : 0xFFu;
            return value;
        }
        case 0xA2: {
            const uint8_t value = (state && state->psg) ? msx_psg_read_data(state->psg) : 0xFFu;
#if MSX_MEMORY_TRACE_ENABLED
            static uint16_t s_psgReadLogCount = 0u;
            static bool s_psgReadLastValid = false;
            static uint32_t s_psgReadLastSignature = 0u;
            const unsigned reg = (state && state->psg) ? static_cast<unsigned>(state->psg->selectedReg & 0x0Fu) : 0xFFu;
            if (reg == 14u) {
                const uint8_t reg15 = (state && state->psg) ? state->psg->regs[15] : 0xFFu;
                const uint8_t portA = (state && state->psg) ? state->psg->joystickPortA : 0xFFu;
                const uint8_t portB = (state && state->psg) ? state->psg->joystickPortB : 0xFFu;
                const bool selectPortB = (reg15 & 0x40u) != 0u;
                const bool interesting = (value != 0xFFu) || (portA != 0xFFu) || (portB != 0xFFu);
                const uint32_t signature =
                    static_cast<uint32_t>(value) |
                    (static_cast<uint32_t>(reg15) << 8) |
                    (static_cast<uint32_t>(portA) << 16) |
                    (static_cast<uint32_t>(portB) << 24);
                if (!interesting) {
                    s_psgReadLastValid = false;
                } else if (s_psgReadLogCount < 128u &&
                           (!s_psgReadLastValid || s_psgReadLastSignature != signature)) {
                    std::printf("[MSX][PSG-IO] RD r14 -> %02X sel=%c portA=%02X portB=%02X r15=%02X A8=%02X SSL3=%02X #%u\n",
                                static_cast<unsigned>(value),
                                selectPortB ? 'B' : 'A',
                                static_cast<unsigned>(portA),
                                static_cast<unsigned>(portB),
                                static_cast<unsigned>(reg15),
                                static_cast<unsigned>(state ? state->slotRegister : 0xFFu),
                                static_cast<unsigned>(state ? state->secondarySlotRegs[3] : 0xFFu),
                                static_cast<unsigned>(s_psgReadLogCount));
                    s_psgReadLastSignature = signature;
                    s_psgReadLastValid = true;
                    ++s_psgReadLogCount;
                }
            }
#endif
            return value;
        }
        case 0xA8:
            return state ? state->slotRegister : 0xFFu;
        case 0xA9: {
            const uint8_t row = state ? state->keyboard.selectedRow : 0xFFu;
            const uint8_t value = state ? msx_keyboard_read_row(&state->keyboard) : 0xFFu;
#if MSX_MEMORY_TRACE_ENABLED
            static uint16_t s_kbdReadLogCount = 0u;
            if (s_kbdReadLogCount < 128u && (value != 0xFFu)) {
                std::printf("[MSX][KBD-IO] IN A9 row=%u -> %02X A8=%02X SSL3=%02X #%u\n",
                            static_cast<unsigned>(row),
                            static_cast<unsigned>(value),
                            static_cast<unsigned>(state ? state->slotRegister : 0xFFu),
                            static_cast<unsigned>(state ? state->secondarySlotRegs[3] : 0xFFu),
                            static_cast<unsigned>(s_kbdReadLogCount));
                ++s_kbdReadLogCount;
            }
#endif
            return value;
        }
        case 0xAA:
            return state ? state->ppiPortC : 0xFFu;
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xD4:
            return (state && state->disk) ? msx_disk_in(state->disk, port) : 0xFFu;
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            if (state && msx_memory_has_mapper(state)) {
                return state->mapperRegisters[port - 0xFCu];
            }
            return 0xFFu;
        default:
            return 0xFFu;
    }
}

void msx_memory_out(MsxMemoryState* state, uint8_t port, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    state->ioWriteCount++;
    switch (port) {
        case 0x98:
            state->lastPort98 = value;
            if (state->vdp) {
                msx_vdp_out_data(state->vdp, value);
            }
            break;
        case 0x99:
            state->lastPort99 = value;
            if (state->vdp) {
                msx_vdp_out_control(state->vdp, value);
            }
            break;
        case 0x9A:
            state->lastPort9A = value;
            if (state->vdp) {
                msx_vdp_out_palette(state->vdp, value);
            }
            break;
        case 0x9B:
            state->lastPort9B = value;
            if (state->vdp) {
                msx_vdp_out_indirect(state->vdp, value);
            }
            break;
        case 0xA0:
            state->lastPortA0 = value;
            if (state->psg) {
                msx_psg_select_register(state->psg, value);
            }
            break;
        case 0xA1:
            state->lastPortA1 = value;
            if (state->psg) {
                const uint8_t reg = static_cast<uint8_t>(state->psg->selectedReg & 0x0Fu);
                const uint8_t previous = state->psg->regs[reg];
                msx_psg_write_data(state->psg, value);
#if MSX_MEMORY_TRACE_ENABLED
                static uint16_t s_psgWriteLogCount = 0u;
                if (s_psgWriteLogCount < 128u &&
                    reg <= 13u &&
                    previous != value) {
                    std::printf("[MSX][PSG-IO] WR r%u <- %02X (prev=%02X) A8=%02X SSL3=%02X #%u\n",
                                static_cast<unsigned>(reg),
                                static_cast<unsigned>(value),
                                static_cast<unsigned>(previous),
                                static_cast<unsigned>(state->slotRegister),
                                static_cast<unsigned>(state->secondarySlotRegs[3]),
                                static_cast<unsigned>(s_psgWriteLogCount));
                    ++s_psgWriteLogCount;
                }
#endif
            }
            break;
        case 0xA8:
#if MSX_MEMORY_TRACE_ENABLED
            static uint16_t s_a8LogCount = 0u;
            if (s_a8LogCount < 64u) {
                std::printf("[MSX][SLOT] OUT A8 <- %02X p0=%u p1=%u p2=%u p3=%u ssl3=%02X #%u\n",
                            static_cast<unsigned>(value),
                            static_cast<unsigned>(value & 0x03u),
                            static_cast<unsigned>((value >> 2) & 0x03u),
                            static_cast<unsigned>((value >> 4) & 0x03u),
                            static_cast<unsigned>((value >> 6) & 0x03u),
                            static_cast<unsigned>(state->secondarySlotRegs[3]),
                            static_cast<unsigned>(s_a8LogCount));
                ++s_a8LogCount;
            }
#endif
            state->slotRegister = value;
            state->lastPortA8 = value;
            msx_memory_refresh_maps(state);
            break;
        case 0xAA:
            state->ppiPortC = value;
            state->lastPortAA = value;
            msx_keyboard_select_row(&state->keyboard, static_cast<uint8_t>(value & 0x0Fu));
            break;
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xD4:
            if (state->disk) {
                msx_disk_out(state->disk, port, value);
            }
            break;
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            if (msx_memory_has_mapper(state)) {
                state->mapperRegisters[port - 0xFCu] = static_cast<uint8_t>(value % state->ramSegmentCount);
                msx_memory_refresh_maps(state);
            }
            break;
        default:
            break;
    }
}

