#pragma once

// Include the emulator core shared header first.
// This defines INPUT_UP, INPUT_DOWN, etc.
// We do NOT redefine them here to avoid conflicts and mapping errors.
extern "C" {
    #include "sms/smsplus/shared.h"
}

#include <M5Cardputer.h>
#include "display.h"  // fullscreen/scanline bool

extern bool fullscreen;
extern bool scanline;

void cardputer_input_init();
void cardputer_read_input(bool isGG);