#pragma once

#include <stddef.h>
#include <stdint.h>

// Launch MSX cartridge ROM (existing entry point).
void run_msx(const uint8_t* romData, size_t romLen, const char* romName);

// Launch MSX-DOS from a DSK disk image.
// The DSK data is XIP-mapped (may be nullptr to boot diskless).
// DISK.ROM is loaded automatically from /sd/bios/msx/DISK.ROM if available.
void run_msx_disk(const uint8_t* dskData, size_t dskLen, const char* dskName);

// Launch MSX BASIC (no cartridge, no disk).
void run_msx_basic(const char* name);