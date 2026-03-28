#pragma once

#include <stddef.h>
#include <stdint.h>

void a7800_audio_init(unsigned sampleRate);
void a7800_audio_shutdown(void);
void a7800_audio_submit_batch(const int16_t* samples, size_t frames);
