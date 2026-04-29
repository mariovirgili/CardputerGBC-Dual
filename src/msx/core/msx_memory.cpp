#include "msx_memory.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <ctime>
#include <cstring>

#include "../../share/emu_static_pool.h"
#include "msx_cpu.h"
#include "msx_disk.h"
#include "msx_psg.h"
#include "msx_scc.h"
#include "msx_vdp.h"

#ifndef MSX_MEMORY_TRACE_ENABLED
#define MSX_MEMORY_TRACE_ENABLED 0
#endif

#ifndef MSX_SCC_LOG_ENABLED
#define MSX_SCC_LOG_ENABLED 0
#endif

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;
constexpr size_t kMsxPageSize16K = 0x4000;
constexpr size_t kMsxRamSizeMsx1 = 0x10000;
constexpr size_t kMsxRamSizeMsx2 = 0x20000;
constexpr uint8_t kMsxMaxRamSegments = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize16K);
constexpr uint8_t kMsxMaxRamBanks = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize8K);
constexpr uint8_t kMsxDefaultSlotRegister = 0x00;  // all pages → slot0 (BIOS); BIOS probe sets final value
static uint8_t s_openBusPage[kMsxPageSize8K];
static bool s_openBusInitialized = false;
static MsxSccState* s_attachedScc = nullptr;

constexpr uint8_t kMsxNoramByte = 0xFFu;
constexpr uint16_t kMsxAddrSlttbl = 0xFCC5u;
constexpr uint16_t kMsxAddrPage3RuntimeRamBegin = 0xF000u;
constexpr uint16_t kMsxAddrPage3ProbeWindowBegin = 0xFE00u;
constexpr uint16_t kMsxAddrPage3ProbeWindowEnd = 0xFE30u;
// Cart lives in primary slot 1 (standard MSX topology).
// The earlier slot-2 experiment was a misread of the BIOS trace: the CALLF
// extension-ROM scan switched page3 into slot 2 to test it, not to access
// the cart body.  Slot 2 = open bus (0xFF).
constexpr uint8_t kMsxPrimarySlotCartridge = 1u;
constexpr uint16_t kMsxCartHeaderMirrorSize = 0x0010u;
constexpr uint8_t kMsxPrimarySlotExpanded = 3u;
constexpr uint8_t kMsxSecondarySlotSubRom = 0u;
constexpr uint8_t kMsxSecondarySlotDiskRom = 1u;
constexpr uint8_t kMsxSecondarySlotRam = 2u;
constexpr uint8_t kMsxMsx2SecondarySlotDefault = 0xA4u;

constexpr uint8_t kMsxRtcModeClock = 0u;
constexpr uint8_t kMsxRtcModeAlarm = 1u;
constexpr uint8_t kMsxRtcModeNvramLo = 2u;
constexpr uint8_t kMsxRtcModeNvramHi = 3u;

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

    if (primarySlot != kMsxPrimarySlotExpanded || !state->slot3Expanded) {
        return 0u;
    }

    return state->secondarySlotRegs[primarySlot];
}

bool msx_memory_primary_slot_has_secondary_register(const MsxMemoryState* state, uint8_t primarySlot)
{
    return state &&
           primarySlot == kMsxPrimarySlotExpanded &&
           state->slot3Expanded;
}

bool msx_memory_has_mapper(const MsxMemoryState* state)
{
    return state && state->mapperEnabled && state->ramSegmentCount > 4u;
}

void msx_memory_update_ram_segment_wrap(MsxMemoryState* state)
{
    if (!state || state->ramSegmentCount == 0u) {
        return;
    }

    state->ramSegmentMask = static_cast<uint8_t>(state->ramSegmentCount - 1u);
    state->ramSegmentCountPowerOfTwo =
        (state->ramSegmentCount & state->ramSegmentMask) == 0u;
}

uint8_t msx_memory_ram_segment(const MsxMemoryState* state, uint8_t pageIndex)
{
    if (!state || state->ramSegmentCount == 0u) {
        return 0u;
    }

    if (msx_memory_has_mapper(state)) {
        if (state->machineMode == MsxMachineMode::MSX2) {
            return state->mapperRegisters[pageIndex & 0x03u];
        }
        return msx_memory_wrap_ram_segment(state, state->mapperRegisters[pageIndex & 0x03u]);
    }

    return msx_memory_wrap_ram_segment(state, pageIndex);
}

bool msx_memory_is_static_bank(const uint8_t* ptr)
{
    uint8_t* pool = emu_static_pool_get();
    return ptr &&
           pool &&
           ptr >= pool &&
           ptr < (pool + EMU_STATIC_POOL_SIZE);
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

const uint8_t* msx_memory_sub_rom_page_ptr(const MsxMemoryState* state, uint8_t subPage)
{
    if (!state ||
        state->machineMode != MsxMachineMode::MSX2 ||
        !state->bios.ready ||
        !state->bios.hasSubRom ||
        !state->bios.subRom ||
        state->bios.subSize == 0u) {
        return nullptr;
    }

    const size_t offset = (static_cast<size_t>(subPage) * kMsxPageSize8K) % state->bios.subSize;
    return state->bios.subRom + offset;
}

void msx_memory_bind_sub_rom(MsxMemoryState* state, uint8_t bank)
{
    const uint8_t subPage = static_cast<uint8_t>(bank & 0x01u);
    const uint8_t* ptr = msx_memory_sub_rom_page_ptr(state, subPage);
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

// Dispatch a single 8K bank within primary slot 3. Plain MSX1 cartridge
// setups expose RAM as a non-expanded primary slot, while MSX2/DiskROM setups
// use subslot0 = Sub-ROM (page0 only), subslot1 = DiskROM (page1 only),
// subslot2 = RAM.
void msx_memory_bind_slot3(MsxMemoryState* state, uint8_t pageIndex, uint8_t bank)
{
    if (!state->slot3Expanded) {
        if (pageIndex >= 2u) {
            msx_memory_bind_ram(state, pageIndex, bank);
        } else {
            msx_memory_bind_open_bus(state, bank);
        }
        return;
    }

    const uint8_t subSlot = msx_secondary_slot_for_page(
        msx_memory_effective_secondary_slot_reg(state, 3u),
        pageIndex
    );
    switch (subSlot) {
        case kMsxSecondarySlotSubRom:
            if ((state->machineMode == MsxMachineMode::MSX2) && (pageIndex == 0u)) {
                msx_memory_bind_sub_rom(state, bank);
            } else {
                msx_memory_bind_open_bus(state, bank);
            }
            break;
        case kMsxSecondarySlotDiskRom:
            // DiskROM lives at page1 (0x4000-0x7FFF) only
            if (pageIndex == 1u) {
                msx_memory_bind_disk_rom(state, bank);
            } else {
                msx_memory_bind_open_bus(state, bank);
            }
            break;
        case kMsxSecondarySlotRam:
            msx_memory_bind_ram(state, pageIndex, bank);
            break;
        default:
            msx_memory_bind_open_bus(state, bank);
            break;
    }
}

void msx_memory_rtc_local_time(std::tm* outTime)
{
    if (!outTime) {
        return;
    }

    std::memset(outTime, 0, sizeof(*outTime));
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        return;
    }

    localtime_r(&now, outTime);
}

uint8_t msx_memory_rtc_clock_nibble(uint8_t reg)
{
    std::tm localTime;
    msx_memory_rtc_local_time(&localTime);
    const unsigned month = static_cast<unsigned>(localTime.tm_mon + 1);
    const unsigned year = static_cast<unsigned>((localTime.tm_year + 1900) % 100);
    const unsigned weekday = static_cast<unsigned>(localTime.tm_wday);

    switch (reg & 0x0Fu) {
        case 0x0: return static_cast<uint8_t>(localTime.tm_sec % 10);
        case 0x1: return static_cast<uint8_t>((localTime.tm_sec / 10) % 10);
        case 0x2: return static_cast<uint8_t>(localTime.tm_min % 10);
        case 0x3: return static_cast<uint8_t>((localTime.tm_min / 10) % 10);
        case 0x4: return static_cast<uint8_t>(localTime.tm_hour % 10);
        case 0x5: return static_cast<uint8_t>((localTime.tm_hour / 10) % 10);
        case 0x6: return static_cast<uint8_t>(localTime.tm_mday % 10);
        case 0x7: return static_cast<uint8_t>((localTime.tm_mday / 10) % 10);
        case 0x8: return static_cast<uint8_t>(month % 10);
        case 0x9: return static_cast<uint8_t>((month / 10) % 10);
        case 0xA: return static_cast<uint8_t>(year % 10);
        case 0xB: return static_cast<uint8_t>((year / 10) % 10);
        case 0xC: return static_cast<uint8_t>(weekday & 0x0Fu);
        default:
            return 0x00u;
    }
}

uint8_t msx_memory_rtc_read(const MsxMemoryState* state)
{
    if (!state || state->machineMode != MsxMachineMode::MSX2) {
        return 0xFFu;
    }

    const uint8_t reg = static_cast<uint8_t>(state->rtcRegisterSelect & 0x0Fu);
    if (reg == 0x0Du) {
        return static_cast<uint8_t>(state->rtcModeReg & 0x0Fu);
    }
    if (reg >= 0x0Eu) {
        return 0x00u;
    }

    switch (state->rtcModeReg & 0x03u) {
        case kMsxRtcModeClock:
            return msx_memory_rtc_clock_nibble(reg);
        case kMsxRtcModeAlarm:
            return 0x00u;
        case kMsxRtcModeNvramLo:
            return static_cast<uint8_t>(state->rtcNvram[reg] & 0x0Fu);
        case kMsxRtcModeNvramHi: {
            const size_t index = static_cast<size_t>(13u + reg);
            return index < sizeof(state->rtcNvram)
                       ? static_cast<uint8_t>(state->rtcNvram[index] & 0x0Fu)
                       : 0x00u;
        }
        default:
            return 0x00u;
    }
}

void msx_memory_rtc_write(MsxMemoryState* state, uint8_t value)
{
    if (!state || state->machineMode != MsxMachineMode::MSX2) {
        return;
    }

    const uint8_t reg = static_cast<uint8_t>(state->rtcRegisterSelect & 0x0Fu);
    const uint8_t nibble = static_cast<uint8_t>(value & 0x0Fu);
    if (reg == 0x0Du) {
        state->rtcModeReg = nibble;
        return;
    }
    if (reg >= 0x0Eu) {
        return;
    }

    switch (state->rtcModeReg & 0x03u) {
        case kMsxRtcModeNvramLo:
            state->rtcNvram[reg] = nibble;
            break;
        case kMsxRtcModeNvramHi: {
            const size_t index = static_cast<size_t>(13u + reg);
            if (index < sizeof(state->rtcNvram)) {
                state->rtcNvram[index] = nibble;
            }
            break;
        }
        default:
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

void msx_memory_release_ram_banks(MsxMemoryState* state)
{
    if (!state) {
        return;
    }

    const uint8_t* dynamicBase = state->ramBanksDynamicBase;
    const size_t dynamicSize = state->ramBanksDynamicSize;
    if (dynamicBase && dynamicSize != 0u) {
        heap_caps_free(state->ramBanksDynamicBase);
        state->ramBanksDynamicBase = nullptr;
        state->ramBanksDynamicSize = 0u;
    }

    for (uint8_t i = 0; i < kMsxMaxRamBanks; ++i) {
        if (!state->ramBanks[i] || msx_memory_is_static_bank(state->ramBanks[i])) {
            state->ramBanks[i] = nullptr;
            continue;
        }

        if (dynamicBase &&
            (state->ramBanks[i] >= dynamicBase) &&
            (state->ramBanks[i] < (dynamicBase + dynamicSize))) {
            state->ramBanks[i] = nullptr;
            continue;
        }

        heap_caps_free(state->ramBanks[i]);
        state->ramBanks[i] = nullptr;
    }
}

bool msx_memory_allocate_ram_banks(MsxMemoryState* state)
{
    if (!state) {
        return false;
    }

    const uint8_t biosAwareStaticBankCount =
        msx_media_static_ram_bank_count_for_main_bios(state->bios.mainRom);
    const uint8_t staticBankCount = (state->ramBankCount >= biosAwareStaticBankCount)
                                        ? biosAwareStaticBankCount
                                        : state->ramBankCount;

    for (uint8_t i = 0; i < staticBankCount; ++i) {
        state->ramBanks[i] = msx_media_static_ram_bank_ptr_for_main_bios(state->bios.mainRom, i);
        if (!state->ramBanks[i]) {
            msx_memory_release_ram_banks(state);
            return false;
        }

        std::memset(state->ramBanks[i], kMsxNoramByte, kMsxPageSize8K);
    }

    const uint8_t firstDynamicIndex = staticBankCount;
    const uint8_t dynamicBankCount = static_cast<uint8_t>(state->ramBankCount - firstDynamicIndex);
    if (dynamicBankCount > 0u) {
        const size_t dynamicBytes = static_cast<size_t>(dynamicBankCount) * kMsxPageSize8K;
        uint8_t* dynamicBase = static_cast<uint8_t*>(heap_caps_malloc(
            dynamicBytes,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        ));
        if (!dynamicBase) {
            dynamicBase = static_cast<uint8_t*>(heap_caps_malloc(dynamicBytes, MALLOC_CAP_8BIT));
        }
        if (!dynamicBase) {
            dynamicBase = static_cast<uint8_t*>(heap_caps_malloc(
                dynamicBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            ));
        }

        if (dynamicBase) {
            state->ramBanksDynamicBase = dynamicBase;
            state->ramBanksDynamicSize = dynamicBytes;
            for (uint8_t i = 0u; i < dynamicBankCount; ++i) {
                const uint8_t bankIndex = static_cast<uint8_t>(firstDynamicIndex + i);
                state->ramBanks[bankIndex] = dynamicBase +
                                             (static_cast<size_t>(i) * kMsxPageSize8K);
                std::memset(state->ramBanks[bankIndex], kMsxNoramByte, kMsxPageSize8K);
            }
        } else {
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
                    state->ramBanks[i] = static_cast<uint8_t*>(heap_caps_malloc(
                        kMsxPageSize8K,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
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
        }
    }

    std::printf("[MSX] memory init: static pool banks=%u dynamic banks=%u\n",
                static_cast<unsigned>(staticBankCount),
                static_cast<unsigned>(dynamicBankCount));
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

    const size_t msx2MinRamSize = (requestedRamSize != 0u) ? requestedRamSize
                                                          : kMsxRamSizeMsx1;

    do {
        state->ramSize = ramSize;
        state->ramSegmentCount = static_cast<uint8_t>(state->ramSize / kMsxPageSize16K);
        msx_memory_update_ram_segment_wrap(state);
        state->ramBankCount = static_cast<uint8_t>(state->ramSize / kMsxPageSize8K);
        state->mapperEnabled = (machineMode == MsxMachineMode::MSX2) && state->ramSegmentCount > 4u;

        if (msx_memory_allocate_ram_banks(state)) {
            break;
        }

        if (machineMode != MsxMachineMode::MSX2 || requestedRamSize != 0u || ramSize <= msx2MinRamSize) {
            std::memset(state, 0, sizeof(*state));
            return false;
        }

        const size_t nextRamSize = ramSize - kMsxPageSize16K;
        if (nextRamSize < msx2MinRamSize) {
            std::printf("[MSX] memory init: no alloc strategy available, fallback below min %u\n",
                        static_cast<unsigned>(msx2MinRamSize));
            std::memset(state, 0, sizeof(*state));
            return false;
        }

        ramSize = nextRamSize;
        std::printf("[MSX] memory init: MSX2 RAM fallback to %u bytes\n",
                    static_cast<unsigned>(ramSize));
        msx_memory_release_ram_banks(state);
    } while (true);

    if (!state->ramSize || state->ramBankCount == 0u) {
        std::memset(state, 0, sizeof(*state));
        return false;
    }

    msx_keyboard_init(&state->keyboard);
    state->slotRegister = kMsxDefaultSlotRegister;
    std::memset(state->secondarySlotRegs, 0, sizeof(state->secondarySlotRegs));
    state->slot3Expanded = (state->machineMode == MsxMachineMode::MSX2);
    if (state->machineMode == MsxMachineMode::MSX2) {
        state->secondarySlotRegs[3] = kMsxMsx2SecondarySlotDefault;
    }
    state->rtcRegisterSelect = 0u;
    state->rtcModeReg = 0u;
    std::memset(state->rtcNvram, 0, sizeof(state->rtcNvram));
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

void msx_memory_log_scc_bank_write(uint8_t window,
                                   uint8_t rawValue,
                                   uint8_t normalizedValue,
                                   bool classicWindow,
                                   bool plusWindow)
{
#if MSX_SCC_LOG_ENABLED
    std::printf("[MSX][SCC] bank window=%u raw=%02X mapped=%02X classic=%u plus=%u\n",
                static_cast<unsigned>(window),
                static_cast<unsigned>(rawValue),
                static_cast<unsigned>(normalizedValue),
                classicWindow ? 1u : 0u,
                plusWindow ? 1u : 0u);
#else
    (void)window;
    (void)rawValue;
    (void)normalizedValue;
    (void)classicWindow;
    (void)plusWindow;
#endif
}

bool msx_memory_is_scc_trace_address(uint16_t address)
{
    return (address >= 0x5000u && address < 0x5800u) ||
           (address >= 0x7000u && address < 0x7800u) ||
           (address >= 0x9000u && address < 0xA000u) ||
           (address >= 0xB000u && address < 0xC000u);
}

void msx_memory_log_scc_write_route(const MsxMemoryState* state,
                                    uint16_t address,
                                    uint8_t value,
                                    uint8_t page,
                                    uint8_t slot,
                                    bool writeMapHit)
{
#if MSX_SCC_LOG_ENABLED
    static uint16_t s_sccCartRouteLogCount = 0u;
    static uint16_t s_sccIgnoredRouteLogCount = 0u;
    const bool cartRoute = (slot == kMsxPrimarySlotCartridge) && !writeMapHit;
    if (cartRoute) {
        if (s_sccCartRouteLogCount >= 192u) {
            return;
        }
    } else {
        if (s_sccIgnoredRouteLogCount >= 12u) {
            return;
        }
    }

    const uint16_t logIndex = cartRoute ? s_sccCartRouteLogCount : s_sccIgnoredRouteLogCount;
    const MsxSccState* const scc = s_attachedScc;
    std::printf("[MSX][SCC] %s WR %04X <- %02X page=%u slot=%u A8=%02X writeMap=%u classic=%u plus=%u banks=%02X/%02X/%02X/%02X #%u\n",
                cartRoute ? "cart-route" : "ignored-route",
                static_cast<unsigned>(address),
                static_cast<unsigned>(value),
                static_cast<unsigned>(page),
                static_cast<unsigned>(slot),
                static_cast<unsigned>(state ? state->slotRegister : 0xFFu),
                writeMapHit ? 1u : 0u,
                (scc && scc->classicWindow) ? 1u : 0u,
                (scc && scc->plusWindow) ? 1u : 0u,
                state ? static_cast<unsigned>(state->cart.windowBanks[0]) : 0xFFu,
                state ? static_cast<unsigned>(state->cart.windowBanks[1]) : 0xFFu,
                state ? static_cast<unsigned>(state->cart.windowBanks[2]) : 0xFFu,
                state ? static_cast<unsigned>(state->cart.windowBanks[3]) : 0xFFu,
                static_cast<unsigned>(logIndex));
    if (cartRoute) {
        ++s_sccCartRouteLogCount;
    } else {
        ++s_sccIgnoredRouteLogCount;
    }
#else
    (void)state;
    (void)address;
    (void)value;
    (void)page;
    (void)slot;
    (void)writeMapHit;
#endif
}

uint8_t msx_memory_normalize_cart_bank(const MsxMemoryState* state, uint8_t bank)
{
    if (!state || state->cart.bankCount8K == 0u) {
        return 0u;
    }
    return static_cast<uint8_t>(bank % state->cart.bankCount8K);
}

void msx_memory_attach_scc(MsxMemoryState* state, MsxSccState* scc)
{
    (void)state;
    s_attachedScc = scc;
}

MsxSccState* msx_memory_get_scc(MsxMemoryState* state)
{
    (void)state;
    return s_attachedScc;
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
    s_attachedScc = nullptr;
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
    msx_cpu_clear_pending_psg();
    if (state->psg) {
        msx_psg_reset(state->psg);
    }
    if (s_attachedScc) {
        msx_scc_reset(s_attachedScc);
    }

    state->slotRegister = kMsxDefaultSlotRegister;
    std::memset(state->secondarySlotRegs, 0, sizeof(state->secondarySlotRegs));
    state->slot3Expanded = (state->machineMode == MsxMachineMode::MSX2) ||
                           ((state->diskRom != nullptr) && (state->diskRomSize != 0u));
    if (state->slot3Expanded && state->machineMode == MsxMachineMode::MSX2) {
        state->secondarySlotRegs[3] = kMsxMsx2SecondarySlotDefault;
    }
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
    state->mapEpoch = 0u;
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

    state->mapEpoch = (state->mapEpoch == 0xFFFFFFFFu) ? 1u : (state->mapEpoch + 1u);
    if (state->mapEpoch == 0u) {
        state->mapEpoch = 1u;
    }
}

uint8_t IRAM_ATTR msx_memory_read8(const MsxMemoryState* state, uint16_t address)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }

    uint8_t mirroredValue = 0xFFu;
    if (address < 0x0010u &&
        msx_memory_try_cart_header_mirror_read(state, address, &mirroredValue)) {
        return mirroredValue;
    }

    // Expanded slots expose their secondary slot register at FFFFh.
    if (address == 0xFFFFu) {
        const uint8_t page3Slot = msx_memory_page3_primary_slot(state);
        if (msx_memory_primary_slot_has_secondary_register(state, page3Slot)) {
            const uint8_t value = static_cast<uint8_t>(~msx_memory_effective_secondary_slot_reg(state, page3Slot));
            return value;
        }
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);

    // During CALLF, page3 can be switched to an extension slot while the BIOS
    // still needs the upper work area and stack to live in RAM. Keep that
    // runtime RAM slice raw, but leave the FE00 probe window mapped so the
    // BIOS slot test still sees the real target slot.
    if (msx_memory_use_raw_page3_window_impl(address)) {
        return msx_memory_raw_page3_read8(state, address);
    }

    if (s_attachedScc &&
        s_attachedScc->ready &&
        state->cart.type == MsxCartridgeType::KonamiScc) {
        const uint8_t readSlot = msx_slot_for_page(state->slotRegister, static_cast<uint8_t>(address >> 14));
        if (readSlot == kMsxPrimarySlotCartridge) {
            if (address >= 0x9800u &&
                address < 0x9880u &&
                s_attachedScc->classicWindow) {
                return msx_scc_read(s_attachedScc, static_cast<uint8_t>(address & 0xFFu));
            }
            if (address >= 0xB800u &&
                address < 0xB8A0u &&
                s_attachedScc->plusWindow) {
                return msx_scc_read_plus(s_attachedScc, static_cast<uint8_t>(address & 0xFFu));
            }
        }
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

    // Expanded slots expose their secondary slot register at FFFFh.
    if (address == 0xFFFFu) {
        const uint8_t page3Slot = msx_memory_page3_primary_slot(state);
        if (msx_memory_primary_slot_has_secondary_register(state, page3Slot)) {
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
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    if (msx_memory_use_raw_page3_window_impl(address)) {
        msx_memory_raw_page3_write8(state, address, value);
        return;
    }

    const uint8_t page = static_cast<uint8_t>(address >> 14);
    const uint8_t slot = msx_slot_for_page(state->slotRegister, page);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);
    const bool writeMapHit = state->writeMap[bank] != nullptr;

#if MSX_SCC_LOG_ENABLED
    if (state->cart.type == MsxCartridgeType::KonamiScc &&
        msx_memory_is_scc_trace_address(address)) {
        msx_memory_log_scc_write_route(state, address, value, page, slot, writeMapHit);
    }
#else
    (void)msx_memory_is_scc_trace_address;
    (void)msx_memory_log_scc_write_route;
#endif

    if (writeMapHit) {
        state->writeMap[bank][offset] = value;
        return;
    }

    if (slot == kMsxPrimarySlotCartridge) {
        if (s_attachedScc &&
            s_attachedScc->ready &&
            state->cart.type == MsxCartridgeType::KonamiScc) {
            if (address == 0xBFFEu) {
                msx_cpu_flush_pending_psg(state);
                state->cart.windowBanks[3] =
                    msx_memory_normalize_cart_bank(state, static_cast<uint8_t>(value & 0x1Fu));
                msx_scc_set_windows(s_attachedScc,
                                    s_attachedScc->classicWindow,
                                    (value & 0xA0u) != 0u);
                msx_memory_refresh_maps(state);
                return;
            }

            if (address >= 0x9800u &&
                address < 0xA000u &&
                s_attachedScc->classicWindow) {
                msx_cpu_flush_pending_psg(state);
                msx_scc_write(s_attachedScc, static_cast<uint8_t>(address & 0xFFu), value);
                return;
            }

            if (address >= 0xB800u &&
                address < 0xC000u &&
                s_attachedScc->plusWindow) {
                msx_cpu_flush_pending_psg(state);
                msx_scc_write_plus(s_attachedScc, static_cast<uint8_t>(address & 0xFFu), value);
                return;
            }

            const bool bank2Write = address >= 0x9000u && address < 0x9800u;
            const bool bank3Write = address >= 0xB000u && address < 0xB800u;
            if (bank2Write || bank3Write) {
                msx_cpu_flush_pending_psg(state);
                msx_cart_write(&state->cart, address, value);
                const bool classicWindow = bank2Write ? (value == 0x3Fu) : s_attachedScc->classicWindow;
                const bool plusWindow = bank3Write ? ((value & 0x80u) != 0u) : s_attachedScc->plusWindow;
                msx_scc_set_windows(s_attachedScc, classicWindow, plusWindow);
#if MSX_SCC_LOG_ENABLED
                msx_memory_log_scc_bank_write(bank2Write ? 2u : 3u,
                                              value,
                                              bank2Write ? state->cart.windowBanks[2] : state->cart.windowBanks[3],
                                              classicWindow,
                                              plusWindow);
#else
                (void)msx_memory_log_scc_bank_write;
#endif
                msx_memory_refresh_maps(state);
                return;
            }
        }

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
    if (state && state->vdp && state->vdp->machineMode == MsxMachineMode::MSX2) {
        uint32_t frameCycles = 0u;
        if (state->cpu) {
            const uint32_t totalCycles = state->cpu->totalCycles;
            if (totalCycles >= state->vdp->frameStartCpuCycles) {
                frameCycles = totalCycles - state->vdp->frameStartCpuCycles;
            }
        }
        state->vdp->currentFrameCpuCycles = frameCycles;
    }

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
        case 0xB4:
            return state ? static_cast<uint8_t>(state->rtcRegisterSelect & 0x0Fu) : 0xFFu;
        case 0xB5:
            return msx_memory_rtc_read(state);
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
    if (state->vdp && state->vdp->machineMode == MsxMachineMode::MSX2) {
        uint32_t frameCycles = 0u;
        if (state->cpu) {
            const uint32_t totalCycles = state->cpu->totalCycles;
            if (totalCycles >= state->vdp->frameStartCpuCycles) {
                frameCycles = totalCycles - state->vdp->frameStartCpuCycles;
            }
        }
        state->vdp->currentFrameCpuCycles = frameCycles;
    }
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
                msx_cpu_flush_pending_psg(state);
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
#if MSX_SCC_LOG_ENABLED
            if (state->cart.type == MsxCartridgeType::KonamiScc) {
                static uint16_t s_sccA8LogCount = 0u;
                const uint8_t oldPage1 = msx_slot_for_page(state->slotRegister, 1u);
                const uint8_t oldPage2 = msx_slot_for_page(state->slotRegister, 2u);
                const uint8_t newPage1 = msx_slot_for_page(value, 1u);
                const uint8_t newPage2 = msx_slot_for_page(value, 2u);
                const bool oldCartVisible = oldPage1 == kMsxPrimarySlotCartridge ||
                                            oldPage2 == kMsxPrimarySlotCartridge;
                const bool newCartVisible = newPage1 == kMsxPrimarySlotCartridge ||
                                            newPage2 == kMsxPrimarySlotCartridge;
                if (s_sccA8LogCount < 64u &&
                    (oldCartVisible || newCartVisible || state->slotRegister != value)) {
                    std::printf("[MSX][SCC] slot A8 %02X -> %02X p1=%u p2=%u cartVisible=%u #%u\n",
                                static_cast<unsigned>(state->slotRegister),
                                static_cast<unsigned>(value),
                                static_cast<unsigned>(newPage1),
                                static_cast<unsigned>(newPage2),
                                newCartVisible ? 1u : 0u,
                                static_cast<unsigned>(s_sccA8LogCount));
                    ++s_sccA8LogCount;
                }
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
        case 0xB4:
            if (state->machineMode == MsxMachineMode::MSX2) {
                state->rtcRegisterSelect = static_cast<uint8_t>(value & 0x0Fu);
            }
            break;
        case 0xB5:
            msx_memory_rtc_write(state, value);
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
                state->mapperRegisters[port - 0xFCu] = value;
                msx_memory_refresh_maps(state);
            }
            break;
        default:
            break;
    }
}

