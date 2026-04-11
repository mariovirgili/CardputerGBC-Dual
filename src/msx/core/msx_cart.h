#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../msx_media.h"

struct MsxCartState {
    const uint8_t* rom;
    size_t size;
    size_t headerOffset;
    MsxCartridgeType type;
    uint8_t bankCount8K;
    uint8_t windowBanks[4];
    bool ready;
    bool bankSwitching;
    bool directBootCandidate;
    uint16_t entryPoint;
    uint16_t initAddress;
};

bool msx_cart_init(MsxCartState* state, const MsxRomImage* image);
void msx_cart_reset(MsxCartState* state);
const uint8_t* msx_cart_window_ptr(const MsxCartState* state, uint8_t windowIndex);
void msx_cart_write(MsxCartState* state, uint16_t address, uint8_t value);
