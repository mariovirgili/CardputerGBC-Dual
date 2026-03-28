#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a7800_input.h"
#include "core/libretro-common/include/libretro.h"

struct A7800HostState {
    retro_system_av_info avInfo;
    retro_pixel_format pixelFormat;
    const uint8_t* romData;
    size_t romLen;
    const char* romName;
    const char* systemDirectory;
    A7800InputState input;
    uint32_t joypadMask[2];
    unsigned frameWidth;
    unsigned frameHeight;
    size_t framePitch;
    uint64_t videoFrameCount;
    uint64_t audioFrameCount;
    unsigned sampleRate;
    double fps;
    float aspectRatio;
    bool isPal;
    bool initialized;
    bool loaded;
};

bool a7800_host_init(A7800HostState* state);
bool a7800_host_load_game(A7800HostState* state,
                          const uint8_t* romData,
                          size_t romLen,
                          const char* romName);
void a7800_host_run_frame(A7800HostState* state, const A7800InputState* input);
const retro_system_av_info* a7800_host_get_av_info(const A7800HostState* state);
bool a7800_host_is_pal(const A7800HostState* state);
double a7800_host_get_fps(const A7800HostState* state);
unsigned a7800_host_get_sample_rate(const A7800HostState* state);
void a7800_host_unload_game(A7800HostState* state);
void a7800_host_shutdown(A7800HostState* state);
