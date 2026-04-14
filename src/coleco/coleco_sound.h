#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef COLECO_AUDIO_ENABLED
#define COLECO_AUDIO_ENABLED 1
#endif

struct ColecoAudioHookState {
    uint32_t sampleRate;
    uint16_t frameSamples;
    uint8_t channels;
    uint8_t queuedBlocks;
    uint32_t submittedFrames;
    uint32_t droppedFrames;
    bool compiledIn;
    bool enabled;
    bool running;
    bool paused;
    bool streamSeen;
};

bool coleco_sound_init(uint32_t sampleRate, uint8_t channels);
void coleco_sound_shutdown(void);
int16_t* coleco_sound_begin_mix(size_t* capacity);
void coleco_sound_end_mix(size_t sampleCount);
void coleco_sound_submit(const int16_t* samples, size_t sampleCount);
void coleco_sound_set_paused(bool paused);
const ColecoAudioHookState& coleco_sound_get_state(void);
