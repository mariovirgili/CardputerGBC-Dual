#include "coleco_media.h"
#include <SD.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <esp_heap_caps.h>

bool coleco_media_load_bios(ColecoBiosImage* bios, const char* biosPath) {
    if (!bios || !biosPath) return false;

    printf("[COLECO] Loading BIOS: %s\n", biosPath);

    bios->loaded = false;
    // bios->data non viene sovrascritto se già pre-allocato
    bios->size = 0;
    strncpy(bios->path, biosPath, sizeof(bios->path) - 1);
    bios->path[sizeof(bios->path) - 1] = '\0';

    if (!bios->data) {
        bios->data = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!bios->data) {
        printf("[COLECO] BIOS ERROR: malloc failed\n");
        return false;
    }

    // SD.open() expects a path relative to the SD mount root (no /sd prefix)
    const char* sdPath = biosPath;
    if (strncmp(biosPath, "/sd/", 4) == 0) sdPath = biosPath + 3;
    else if (strcmp(biosPath, "/sd") == 0) sdPath = "/";

    File file = SD.open(sdPath, FILE_READ);
    if (!file) {
        printf("[COLECO] BIOS ERROR: file not found\n");
        return false;
    }

    size_t size = file.size();
    if (size != 8192) {
        printf("[COLECO] BIOS ERROR: wrong size %zu (expected 8192)\n", size);
        file.close();
        return false;
    }

    file.read(bios->data, size);
    file.close();

    bios->size = size;
    bios->loaded = true;

    printf("[COLECO] BIOS loaded OK (%zu bytes)\n", size);
    return true;
}

void coleco_media_release_bios(ColecoBiosImage* bios) {
    if (bios && bios->data) {
        heap_caps_free(bios->data);
        bios->data = nullptr;
        bios->size = 0;
        bios->loaded = false;
    }
}
