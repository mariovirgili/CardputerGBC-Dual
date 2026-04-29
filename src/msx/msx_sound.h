#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef MSX_AUDIO_ENABLED
#define MSX_AUDIO_ENABLED 1
#endif

struct MsxAudioHookState {
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

bool msx_sound_init(uint32_t sampleRate, uint8_t channels);
bool msx_sound_prestart_speaker(uint32_t sampleRate, uint8_t channels);
void msx_sound_shutdown(void);
int16_t* msx_sound_begin_mix(size_t* capacity);
void msx_sound_end_mix(size_t sampleCount);
void msx_sound_submit(const int16_t* samples, size_t sampleCount);
void msx_sound_set_paused(bool paused);
const MsxAudioHookState& msx_sound_get_state(void);
