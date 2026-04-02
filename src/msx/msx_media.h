#pragma once

#include <stddef.h>
#include <stdint.h>

#include "msx_config.h"

enum class MsxCartridgeType : uint8_t {
    Unknown = 0,
    Plain16K,
    Plain32K,
    Ascii8,
    Ascii16,
    Konami,
    KonamiScc,
};

enum class MsxBiosTarget : uint8_t {
    None = 0,
    MSX1,
    MSX2,
};

enum class MsxImageLoadStatus : uint8_t {
    Missing = 0,
    Loaded,
    Incompatible,
};

struct MsxRomImage {
    const uint8_t* data;
    size_t size;
    size_t headerOffset;
    uint16_t entryPoint;
    uint16_t initAddress;
    MsxCartridgeType cartridgeType;
    uint8_t bankCount8K;
    bool hasAbHeader;
    bool sizeSupported;
};

struct MsxBiosImage {
    uint8_t* data;
    size_t size;
    MsxImageLoadStatus status;
    char path[96];
    char expectedName[24];
    char expectedMd5[33];
    char foundMd5[33];
};

struct MsxBiosBundle {
    MsxBiosTarget target;
    MsxBiosImage mainRom;
    MsxBiosImage subRom;
    bool compatible;
    bool subRomRequired;
    char message[128];
};

struct MsxBiosSearchConfig {
    MsxMachineMode requestedMode;
    const char* genericBiosPath;
    const char* msx1BiosPath;
    const char* msx2BiosPath;
    const char* msx2SubRomPath;
};

bool msx_media_analyze_rom(MsxRomImage* image, const uint8_t* romData, size_t romLen);
bool msx_media_load_bios_bundle(MsxBiosBundle* bundle, const MsxBiosSearchConfig* config);
void msx_media_release_bios_bundle(MsxBiosBundle* bundle);

const char* msx_media_cartridge_type_label(MsxCartridgeType type);
const char* msx_media_bios_target_label(MsxBiosTarget target);
MsxMachineMode msx_media_target_to_machine_mode(MsxBiosTarget target);
