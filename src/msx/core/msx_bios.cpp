#include "msx_bios.h"

#include <esp_heap_caps.h>

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

uint8_t* msx_bios_alloc_patch_page()
{
    uint8_t* page = static_cast<uint8_t*>(heap_caps_malloc(kMsxPageSize8K,
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!page) {
        page = static_cast<uint8_t*>(heap_caps_malloc(kMsxPageSize8K, MALLOC_CAP_8BIT));
    }
    if (!page) {
        page = static_cast<uint8_t*>(heap_caps_malloc(kMsxPageSize8K, MALLOC_CAP_DEFAULT));
    }
    return page;
}

} // namespace

bool msx_bios_init(MsxBiosState* state, const MsxBiosBundle* bundle)
{
    if (!state || !bundle || !bundle->compatible || !bundle->mainRom.data || bundle->mainRom.size == 0) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));

    const bool mainRomIsEmbedded = (bundle->mainRom.path[0] == '[');
    if (mainRomIsEmbedded) {
        state->patchedMainPage0 = msx_bios_alloc_patch_page();
        if (!state->patchedMainPage0) {
            return false;
        }

        const size_t copySize = (bundle->mainRom.size < kMsxPageSize8K)
                                    ? bundle->mainRom.size
                                    : kMsxPageSize8K;
        std::memcpy(state->patchedMainPage0, bundle->mainRom.data, copySize);
        if (copySize < kMsxPageSize8K) {
            std::memset(state->patchedMainPage0 + copySize, 0xFFu, kMsxPageSize8K - copySize);
        }
        msx_bios_apply_rom_patches(state->patchedMainPage0, copySize);
    } else {
        msx_bios_apply_rom_patches(bundle->mainRom.data, bundle->mainRom.size);
    }

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

    if (state->patchedMainPage0) {
        heap_caps_free(state->patchedMainPage0);
        state->patchedMainPage0 = nullptr;
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
        if (state->patchedMainPage0 && offset < kMsxPageSize8K) {
            return state->patchedMainPage0 + offset;
        }
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
