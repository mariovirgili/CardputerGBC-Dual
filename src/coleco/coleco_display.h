#pragma once

#include <stddef.h>
#include <stdint.h>

struct ColecoDisplayFrame {
    const uint8_t* indexed8;
    const uint16_t* palette565;
    uint16_t paletteEntryCount;
    unsigned width;
    unsigned height;
    size_t pitchBytes;
};

struct ColecoDisplayStatus {
    const char* romName;
    const char* coreLine;
    const char* cartLine;
    const char* machineLine;
    const char* biosLine;
    const char* audioLine;
    uint32_t frameCounter;
};

void coleco_display_init(void);
void coleco_display_shutdown(void);
void coleco_display_submit_frame(const ColecoDisplayFrame* frame, const ColecoDisplayStatus* status);
void coleco_display_show_external_info(const char* romTitle);
