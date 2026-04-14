#pragma once

#include <stddef.h>
#include <stdint.h>

struct ColecoRomImage {
    const uint8_t* data;
    size_t size;
};

struct ColecoBiosImage {
    uint8_t* data;
    size_t size;
    bool loaded;
    char path[128];
};

bool coleco_media_load_bios(ColecoBiosImage* bios, const char* biosPath);
void coleco_media_release_bios(ColecoBiosImage* bios);
