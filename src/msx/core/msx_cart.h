#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../msx_media.h"

struct MsxCartState {
    const uint8_t* rom;
    size_t size;
    size_t headerOffset;
    uint8_t* sram;
    size_t sramSize;
    MsxCartridgeType type;
    uint8_t bankCount8K;
    uint8_t quirks;
    uint8_t windowBanks[4];
    uint16_t fmpacKey;
    bool sramDirty;
    bool ownsSram;
    bool ready;
    bool bankSwitching;
    bool directBootCandidate;
    uint16_t entryPoint;
    uint16_t initAddress;
};

bool msx_cart_init(MsxCartState* state, const MsxRomImage* image);
void msx_cart_shutdown(MsxCartState* state);
void msx_cart_reset(MsxCartState* state);
const uint8_t* msx_cart_window_ptr(const MsxCartState* state, uint8_t windowIndex);
void msx_cart_write(MsxCartState* state, uint16_t address, uint8_t value);
bool msx_cart_read_sram(const MsxCartState* state, uint16_t address, uint8_t* value);
bool msx_cart_prepare_sram(MsxCartState* state);
bool msx_cart_load_sram(MsxCartState* state, const uint8_t* data, size_t size);
const uint8_t* msx_cart_sram_data(const MsxCartState* state);
size_t msx_cart_sram_size(const MsxCartState* state);
bool msx_cart_sram_dirty(const MsxCartState* state);
void msx_cart_clear_sram_dirty(MsxCartState* state);
