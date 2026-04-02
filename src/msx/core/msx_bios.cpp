#include "msx_bios.h"

#include <cstring>

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;

const uint8_t* msx_wrap_page_ptr(const uint8_t* base, size_t size, size_t offset)
{
    if (!base || size == 0) {
        return nullptr;
    }

    const size_t wrapped = offset % size;
    return base + wrapped;
}

} // namespace

bool msx_bios_init(MsxBiosState* state, const MsxBiosBundle* bundle)
{
    if (!state || !bundle || !bundle->compatible || !bundle->mainRom.data || bundle->mainRom.size == 0) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
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
