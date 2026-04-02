#pragma once

#include <stddef.h>
#include <stdint.h>

struct MsxDisplayFrame {
    const uint8_t* indexed8;
    const uint16_t* palette565;
    unsigned width;
    unsigned height;
    size_t pitchBytes;
};

struct MsxDisplayStatus {
    const char* romName;
    const char* coreLine;
    const char* cartLine;
    const char* machineLine;
    const char* biosLine;
    const char* audioLine;
    uint32_t frameCounter;
};

void msx_display_init(void);
void msx_display_shutdown(void);
void msx_display_submit_frame(const MsxDisplayFrame* frame, const MsxDisplayStatus* status);
