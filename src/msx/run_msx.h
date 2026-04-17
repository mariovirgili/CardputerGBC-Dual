#pragma once

#include <stddef.h>
#include <stdint.h>

class SdService;

// Launch MSX cartridge ROM (existing entry point).
void run_msx(const uint8_t* romData, size_t romLen, const char* romName, SdService& sd);

// Launch MSX-DOS from a DSK disk image.
// The DSK data is XIP-mapped (may be nullptr to boot diskless).
// DISK.ROM is loaded automatically from /sd/bios/msx/DISK.ROM if available.
void run_msx_disk(const uint8_t* dskData, size_t dskLen, const char* dskName, SdService& sd);

// Launch MSX BASIC (no cartridge, no disk).
void run_msx_basic(const char* name, SdService& sd);

// Launch MSX BASIC with a CAS cassette tape image.
// casData/casLen point to the raw CAS file (XIP-mapped from ROM partition).
void run_msx_cas(const uint8_t* casData, size_t casLen, const char* casName, SdService& sd);
