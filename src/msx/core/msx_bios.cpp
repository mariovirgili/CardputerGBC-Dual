#include "msx_bios.h"

#include <cstring>

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;
constexpr uint16_t kMsxBiosPatchAddresses[] = {
    0x00E1u, 0x00E4u, 0x00E7u, 0x00EAu, 0x00EDu, 0x00F0u, 0x00F3u,
};

const uint8_t* msx_wrap_page_ptr(const uint8_t* base, size_t size, size_t offset)
{
    if (!base || size == 0) {
        return nullptr;
    }

    const size_t wrapped = offset % size;
    return base + wrapped;
}

void msx_bios_apply_rom_patches(uint8_t* rom, size_t size)
{
    if (!rom) {
        return;
    }

    for (uint16_t address : kMsxBiosPatchAddresses) {
        if (static_cast<size_t>(address + 3u) > size) {
            continue;
        }
        rom[address + 0u] = 0xEDu;
        rom[address + 1u] = 0xFEu;
        rom[address + 2u] = 0xC9u;
    }
}

} // namespace

bool msx_bios_init(MsxBiosState* state, const MsxBiosBundle* bundle)
{
    if (!state || !bundle || !bundle->compatible || !bundle->mainRom.data || bundle->mainRom.size == 0) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    msx_bios_apply_rom_patches(bundle->mainRom.data, bundle->mainRom.size);
    state->target = bundle->target;
    state->machineMode = msx_media_target_to_machine_mode(bundle->target);
    state->mainRom = bundle->mainRom.data;
    state->mainSize = bundle->mainRom.size;
    state->subRom = bundle->subRom.data;
    state->subSize = bundle->subRom.size;
    state->hasSubRom = bundle->subRom.data && bundle->subRom.size > 0;
    state->ready = true;
    return true;
}

void msx_bios_shutdown(MsxBiosState* state)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
}

const uint8_t* msx_bios_page_ptr(const MsxBiosState* state, uint8_t pageIndex, uint8_t subPage)
{
    if (!state || !state->ready) {
        return nullptr;
    }

    if (pageIndex < 2) {
        const size_t offset = (static_cast<size_t>(pageIndex) * 2u + static_cast<size_t>(subPage)) * kMsxPageSize8K;
        return msx_wrap_page_ptr(state->mainRom, state->mainSize, offset);
    }

    if (pageIndex == 2 && state->hasSubRom) {
        const size_t offset = static_cast<size_t>(subPage) * kMsxPageSize8K;
        return msx_wrap_page_ptr(state->subRom, state->subSize, offset);
    }

    return nullptr;
}

bool msx_bios_is_msx2(const MsxBiosState* state)
{
    return state && state->ready && state->machineMode == MsxMachineMode::MSX2;
}
