#pragma once
#include <cstdint>
#include <cstddef>

enum SnesInterlaceMode : uint8_t {
    SNES_INTERLACE_OFF = 0,
    SNES_INTERLACE_ON,
    SNES_INTERLACE_AUTO,
};

void run_snes(const uint8_t* rom, size_t romSize, const char* romName, SnesInterlaceMode interlaceMode);
