#include "msx_memory.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../../share/emu_static_pool.h"
#include "msx_disk.h"
#include "msx_psg.h"
#include "msx_vdp.h"

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;
constexpr size_t kMsxPageSize16K = 0x4000;
constexpr size_t kMsxRamSizeMsx1 = 0x10000;
constexpr size_t kMsxRamSizeMsx2 = 0x20000;
constexpr uint8_t kMsxMaxRamSegments = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize16K);
constexpr uint8_t kMsxMaxRamBanks = static_cast<uint8_t>(kMsxRamSizeMsx2 / kMsxPageSize8K);
constexpr uint8_t kMsxDefaultSlotRegister = 0xD0;
constexpr uint8_t kMsxStaticBankCountMsx1 = 4u;
static uint8_t s_openBusPage[kMsxPageSize8K];
static bool s_openBusInitialized = false;
static uint8_t* const s_msx1StaticRamBanks[kMsxStaticBankCountMsx1] = {
    g_emu_static_pool + (0u * kMsxPageSize8K),
    g_emu_static_pool + (1u * kMsxPageSize8K),
    g_emu_static_pool + (2u * kMsxPageSize8K),
    g_emu_static_pool + (3u * kMsxPageSize8K),
};

uint8_t msx_slot_for_page(uint8_t slotRegister, uint8_t pageIndex)
{
    return static_cast<uint8_t>((slotRegister >> (pageIndex * 2u)) & 0x03u);
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
    uint8_t windowIndex = 0;
    if (pageIndex <= 1u) {
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
            std::memset(state->ramBanks[i], 0x00, kMsxPageSize8K);
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

        std::memset(state->ramBanks[i], 0x00, kMsxPageSize8K);
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
            std::memset(state->ramBanks[i], 0x00, kMsxPageSize8K);
        }
    }
}

} // namespace

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
                    msx_memory_bind_disk_rom(state, bank);
                    break;
                case 3:
                    msx_memory_bind_ram(state, page, bank);
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

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);
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

    const uint8_t page = static_cast<uint8_t>(address >> 14);
    const uint8_t slot = msx_slot_for_page(state->slotRegister, page);
    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);

    if (state->writeMap[bank]) {
        state->writeMap[bank][offset] = value;
        return;
    }

    if (slot == 1u) {
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
        case 0x98:
            return (state && state->vdp) ? msx_vdp_in_data(state->vdp) : 0xFFu;
        case 0x99:
            return (state && state->vdp) ? msx_vdp_in_status(state->vdp) : 0xFFu;
        case 0xA2:
            return (state && state->psg) ? msx_psg_read_data(state->psg) : 0xFFu;
        case 0xA8:
            return state ? state->slotRegister : 0xFFu;
        case 0xA9:
            return state ? msx_keyboard_read_row(&state->keyboard) : 0xFFu;
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
                msx_psg_write_data(state->psg, value);
            }
            break;
        case 0xA8:
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

