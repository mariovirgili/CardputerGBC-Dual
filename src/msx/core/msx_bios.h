#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../msx_media.h"

struct MsxBiosState {
    MsxBiosTarget target;
    MsxMachineMode machineMode;
    const uint8_t* mainRom;
    size_t mainSize;
    uint8_t* patchedMainPage0;
    const uint8_t* subRom;
    size_t subSize;
    bool hasSubRom;
    bool ready;
};

bool msx_bios_init(MsxBiosState* state, const MsxBiosBundle* bundle);
void msx_bios_shutdown(MsxBiosState* state);
const uint8_t* msx_bios_page_ptr(const MsxBiosState* state, uint8_t pageIndex, uint8_t subPage);
bool msx_bios_is_msx2(const MsxBiosState* state);
